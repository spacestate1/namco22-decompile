/* audio_out.c -- see audio_out.h. The producer is the emulation thread (per slice), the consumer SDL's audio thread; a
 * lock-free single-producer / single-consumer ring joins them. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdatomic.h>
#include <SDL2/SDL.h>
#include "audio_out.h"

#define OUT_HZ 48000
#define RING 16384                            /* stereo frames, power of two */
#define TARGET (OUT_HZ * 60 / 1000)           /* 60 ms */
static int16_t ring[RING * 2];
static atomic_uint r_head, r_tail;
static SDL_AudioDeviceID dev;
static double rs_pos;                         /* resampler position in input samples */
static int16_t rs_prev[2];
static float g_volume = 1.0f;
static double g_out_gain = 6.0;
static uint32_t underruns, per_sec_ur, cb_samples, latency_resets;
static atomic_int primed;                     /* 0 until the ring first reaches TARGET: start-up silence is not an underrun */

static inline double soft_limit(double v)
{
    const double knee = 16384.0, room = 32767.0 - knee;
    double a = v < 0 ? -v : v;
    if (a <= knee) return v;
    a = knee + room * (1.0 - 1.0 / (1.0 + (a - knee) / room));   /* approaches 32767, never passes */
    return v < 0 ? -a : a;
}

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
        static int logon = -1; if (logon < 0) logon = getenv("ENG_AUDIOLOG") != NULL;
        if (logon) fprintf(stderr, "[AUDIO] second: %u underrun samples, ring %d, latency resets %u\n", per_sec_ur, (int)(h - t), latency_resets);
        cb_samples -= OUT_HZ; per_sec_ur = 0;
    }
}

bool eng_audio_open(void)
{
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) { fprintf(stderr, "[AUDIO] SDL audio: %s\n", SDL_GetError()); return false; }
    SDL_AudioSpec want, have;
    memset(&want, 0, sizeof want);
    want.freq = OUT_HZ; want.format = AUDIO_S16SYS; want.channels = 2; want.callback = audio_cb;
    want.samples = 512;                          /* ENG_AUDIO_SAMPLES: 480 = exactly 10 ms, for SDL's disk driver (it sleeps whole ms) */
    if (getenv("ENG_AUDIO_SAMPLES")) want.samples = (Uint16)atoi(getenv("ENG_AUDIO_SAMPLES"));
    dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!dev) { fprintf(stderr, "[AUDIO] no audio device: %s\n", SDL_GetError()); return false; }
    if (getenv("ENG_OUTPUT_GAIN")) { g_out_gain = atof(getenv("ENG_OUTPUT_GAIN")); if (g_out_gain < 0) g_out_gain = 0; }
    SDL_PauseAudioDevice(dev, 0);
    fprintf(stderr, "[AUDIO] output %d Hz stereo, %d-sample buffer, gain x%.2f\n", have.freq, have.samples, g_out_gain);
    return true;
}

void eng_audio_set_volume(int percent) { g_volume = (percent < 0 ? 0 : percent > 100 ? 100 : percent) / 100.0f; }

void eng_audio_push(const int16_t *in4, int n)
{
    if (!dev || n <= 0) return;
    unsigned h = atomic_load_explicit(&r_head, memory_order_relaxed);
    unsigned t = atomic_load_explicit(&r_tail, memory_order_acquire);
    const int fill = (int)(h - t);
    /* +-0.5% (inaudible) near the target; up to +-2% when far off, so a device or display whose clock is well away from
     * the board's still converges. */
    const double err = (double)(TARGET - fill) / TARGET;
    const double lim = (err > 1.0 || err < -1.0) ? 0.02 : 0.005;
    double adj = 1.0 + err * 0.005 * ((err > 1.0 || err < -1.0) ? 4 : 1);
    if (adj > 1.0 + lim) adj = 1.0 + lim;
    if (adj < 1.0 - lim) adj = 1.0 - lim;
    const double step = 85333.333 / OUT_HZ / adj;       /* input samples per output sample */
    while (rs_pos < n) {
        const int i = (int)rs_pos; const double f = rs_pos - i;
        for (int ch = 0; ch < 2; ch++) {
            const double a = i == 0 ? rs_prev[ch] : in4[(i - 1) * 4 + ch], b = in4[i * 4 + ch];
            const double v = soft_limit((a + (b - a) * f) * g_volume * g_out_gain);
            if (h - t < RING) ring[(h & (RING - 1)) * 2 + ch] = (int16_t)lrint(v);
        }
        if (h - t < RING) h++;
        rs_pos += step;
    }
    rs_pos -= n;
    rs_prev[0] = in4[(n - 1) * 4]; rs_prev[1] = in4[(n - 1) * 4 + 1];
    atomic_store_explicit(&r_head, h, memory_order_release);
}

void eng_audio_close(void)
{
    if (dev) { SDL_CloseAudioDevice(dev); dev = 0; if (underruns) fprintf(stderr, "[AUDIO] %u output underrun samples\n", underruns); }
}
