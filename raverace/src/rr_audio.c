/*
 * rr_audio.c -- the engine's audio side: the C352 sample mixer (src/c352.c, a
 * port of MAME's c352.cpp), the wave ROM, and the output.
 *
 * Wave ROM layout is NOT file order (namcos22.cpp raverace):
 *   rv1wav0.10r @ 0x000000, rv1wav2.10n @ 0x100000,
 *   rv1wav1.10p @ 0x200000, rv1wav3.10l @ 0x300000.
 * Clock 24.576 MHz / 288 = 85333.3 samples/s, four outputs (front L/R, rear L/R;
 * this cabinet routes only 0/1 to the speakers). The video frame is 59.906 Hz, so
 * a frame is 1424.45 samples -- carried as a fraction so nothing drifts.
 *
 * RR_AUDIODUMP=<file.wav>: write the front pair as a 16-bit stereo WAV at the
 * chip's own rate, for comparing with MAME's -wavwrite.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "c352.h"
#include "rr_sound.h"
#include <SDL.h>
#include <stdatomic.h>
#include <math.h>

/* ---- output (windowed runs only): 48 kHz stereo through SDL's callback ----
 * A lock-free single-producer (the emulation thread, per slice) / single-
 * consumer (SDL's audio thread) ring. The game is paced by the display, never
 * exactly the audio clock, so the resampler trims its ratio by up to +-0.5% to
 * hold the ring near TARGET -- no underrun crackle, no creeping latency. */
#define OUT_HZ 48000
#define RING 16384                            /* stereo frames, power of two */
#define TARGET (OUT_HZ * 60 / 1000)           /* 60 ms */
static int16_t ring[RING * 2];
static atomic_uint r_head, r_tail;            /* written by producer / consumer */
static SDL_AudioDeviceID dev;
static double rs_pos;                         /* resampler: position in input samples */
static int16_t rs_prev[2];
static float g_volume = 1.0f;
static uint32_t underruns;
static atomic_int primed;
static uint32_t per_sec_ur, cb_samples, latency_resets;           /* RR_AUDIOLOG: underruns per second of output */                     /* 0 until the ring first reaches TARGET: start-up silence is not an underrun */

static void audio_cb(void *u, Uint8 *stream, int len)
{
    (void)u;
    int16_t *o = (int16_t *)stream;
    int n = len / 4;
    unsigned t = atomic_load_explicit(&r_tail, memory_order_relaxed);
    unsigned h = atomic_load_explicit(&r_head, memory_order_acquire);
    if ((int)(h - t) > 4 * TARGET) { t = h - TARGET; latency_resets++; }   /* far behind: drop to the target once */
    if (!atomic_load(&primed)) {
        if ((int)(h - t) < TARGET) { memset(stream, 0, (size_t)len); return; }
        atomic_store(&primed, 1);
    }
    for (int i = 0; i < n; i++) {
        if (t == h) { o[2*i] = o[2*i+1] = 0; underruns++; per_sec_ur++; continue; }
        o[2*i] = ring[(t & (RING - 1)) * 2]; o[2*i+1] = ring[(t & (RING - 1)) * 2 + 1];
        t++;
    }
    atomic_store_explicit(&r_tail, t, memory_order_release);
    cb_samples += (uint32_t)n;
    if (cb_samples >= OUT_HZ) {
        static int logon = -1; if (logon < 0) logon = getenv("RR_AUDIOLOG") != NULL;
        if (logon) fprintf(stderr, "[AUDIO] second: %u underrun samples, ring %d, latency resets %u\n", per_sec_ur, (int)(h - t), latency_resets);
        cb_samples -= OUT_HZ; per_sec_ur = 0;
    }
}

bool rr_audio_output_open(void)
{
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) { fprintf(stderr, "[AUDIO] SDL audio: %s\n", SDL_GetError()); return false; }
    SDL_AudioSpec want = { 0 }, have;
    want.freq = OUT_HZ; want.format = AUDIO_S16SYS; want.channels = 2; want.callback = audio_cb;
    want.samples = 512;                          /* RR_AUDIO_SAMPLES: 480 = exactly 10 ms, for SDL's disk driver (it sleeps whole ms) */
    if (getenv("RR_AUDIO_SAMPLES")) want.samples = (Uint16)atoi(getenv("RR_AUDIO_SAMPLES"));
    dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!dev) { fprintf(stderr, "[AUDIO] no audio device: %s\n", SDL_GetError()); return false; }
    SDL_PauseAudioDevice(dev, 0);
    fprintf(stderr, "[AUDIO] output %d Hz stereo, %d-sample buffer\n", have.freq, have.samples);
    return true;
}
void rr_audio_set_volume(int percent) { g_volume = (percent < 0 ? 0 : percent > 100 ? 100 : percent) / 100.0f; }

static void push_out(const int16_t *in4, int n)        /* in: n frames of the chip's 4 outputs, 85333 Hz */
{
    if (!dev) return;
    unsigned h = atomic_load_explicit(&r_head, memory_order_relaxed);
    unsigned t = atomic_load_explicit(&r_tail, memory_order_acquire);
    int fill = (int)(h - t);
    /* +-0.5% (inaudible) near the target; up to +-2% when far off, so a device or
     * display whose clock is well away from the board's still converges. */
    double err = (double)(TARGET - fill) / TARGET;
    double lim = (err > 1.0 || err < -1.0) ? 0.02 : 0.005;
    double adj = 1.0 + err * 0.005 * ((err > 1.0 || err < -1.0) ? 4 : 1);
    if (adj > 1.0 + lim) adj = 1.0 + lim;
    if (adj < 1.0 - lim) adj = 1.0 - lim;
    const double step = 85333.333 / OUT_HZ / adj;       /* input samples per output sample */
    while (rs_pos < n) {
        int i = (int)rs_pos; double f = rs_pos - i;
        for (int ch = 0; ch < 2; ch++) {
            double a = i == 0 ? rs_prev[ch] : in4[(i - 1) * 4 + ch], b = in4[i * 4 + ch];
            double v = (a + (b - a) * f) * g_volume;
            if (v > 32767) v = 32767;
            if (v < -32768) v = -32768;
            if (h - t < RING) ring[(h & (RING - 1)) * 2 + ch] = (int16_t)lrint(v);
        }
        if (h - t < RING) h++;
        rs_pos += step;
    }
    rs_pos -= n;
    rs_prev[0] = in4[(n - 1) * 4]; rs_prev[1] = in4[(n - 1) * 4 + 1];
    atomic_store_explicit(&r_head, h, memory_order_release);
}

static c352_t chip;
static uint8_t *wave;
static bool ready;
static FILE *dump;
static uint32_t dump_frames;
static uint64_t acc;                         /* samples owed, x (59906 * 16) */
#define RATE_NUM  85333333ull                /* samples/s x 1000 */
#define SLICE_DEN (59906ull * RR_SND_SLICES)  /* slices/s x 1000 */

static void wav_header(FILE *f, uint32_t frames)
{
    uint32_t rate = 85333, bytes = frames * 4;
    uint8_t h[44] = { 'R','I','F','F' };
    uint32_t v;
#define P32(o, x) do { v = (x); h[o] = v; h[o+1] = v >> 8; h[o+2] = v >> 16; h[o+3] = v >> 24; } while (0)
    P32(4, 36 + bytes); memcpy(h + 8, "WAVEfmt ", 8); P32(16, 16);
    h[20] = 1; h[22] = 2; P32(24, rate); P32(28, rate * 4); h[32] = 4; h[34] = 16;
    memcpy(h + 36, "data", 4); P32(40, bytes);
    fseek(f, 0, SEEK_SET); fwrite(h, 1, 44, f);
}

bool rr_audio_init(const char *dir)
{
    static const struct { const char *n; uint32_t at; } w[4] = {
        { "rv1wav0.10r", 0x000000 }, { "rv1wav2.10n", 0x100000 },
        { "rv1wav1.10p", 0x200000 }, { "rv1wav3.10l", 0x300000 } };
    wave = calloc(1, 0x400000);
    for (int i = 0; i < 4; i++) {
        char p[1024]; snprintf(p, sizeof p, "%s/%s", dir, w[i].n);
        FILE *f = fopen(p, "rb");
        if (!f || fread(wave + w[i].at, 1, 0x100000, f) != 0x100000) {
            fprintf(stderr, "[AUDIO] cannot read %s -- silent\n", p);
            if (f) fclose(f);
            return false;
        }
        fclose(f);
    }
    c352_init(&chip, wave, 0x400000);
    c352_reset(&chip);
    const char *d = getenv("RR_AUDIODUMP");
    if (d && (dump = fopen(d, "wb"))) { wav_header(dump, 0); fprintf(stderr, "[AUDIO] dumping to %s\n", d); }
    ready = true;
    return true;
}

void rr_audio_c352_write(unsigned word, uint16_t v) { if (ready) c352_write(&chip, word, v, 0xffff); }
uint16_t rr_audio_c352_read(unsigned word) { return ready ? c352_read(&chip, word) : 0; }

void rr_audio_slice(void)
{
    if (!ready) return;
    acc += RATE_NUM;
    int n = (int)(acc / SLICE_DEN);
    acc -= (uint64_t)n * SLICE_DEN;
    static int16_t buf[4 * 256];
    while (n > 0) {
        int k = n > 256 ? 256 : n;
        c352_generate(&chip, buf, k);
        push_out(buf, k);
        if (dump) {
            for (int i = 0; i < k; i++) { fwrite(&buf[i * 4], 2, 2, dump); }
            dump_frames += (uint32_t)k;
        }
        n -= k;
    }
}

void rr_audio_close(void)
{
    if (dev) { SDL_CloseAudioDevice(dev); dev = 0; if (underruns) fprintf(stderr, "[AUDIO] %u output underrun samples\n", underruns); }
    if (dump) { wav_header(dump, dump_frames); fclose(dump); dump = NULL;
                fprintf(stderr, "[AUDIO] wrote %u samples (%.1f s)\n", dump_frames, dump_frames / 85333.0); }
}
