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
/* THE CUSHION: the ring aims to hold TARGET of sound. It starts at 60 ms; every time the card runs dry it grows by 20 ms, up to 140 ms, and
 * the card waits (silent, already faded out) until the ring holds the new amount. A machine whose frames are sometimes slower than the
 * cushion -- a weak GPU in Dirt Dash's jungle -- used to pop on EVERY such frame; now it pops a few times and then has room. A machine that
 * never runs dry keeps 60 ms. ENG_AUDIO_MAXMS caps it (60 = the old fixed cushion). */
#define TARGET_START (OUT_HZ * 60 / 1000)
#define TARGET_STEP  (OUT_HZ * 20 / 1000)
static atomic_int g_target = TARGET_START;
static int target_max = OUT_HZ * 140 / 1000;
#define TARGET atomic_load_explicit(&g_target, memory_order_relaxed)
static int16_t ring[RING * 2];
static atomic_uint r_head, r_tail;
static SDL_AudioDeviceID dev;
static double rs_pos;                         /* resampler position in input samples */
static int16_t rs_prev[4];
static float g_mix[2][4] = { { 1, 0, 0, 0 }, { 0, 1, 0, 0 } };   /* the chip's four outputs into the stereo pair: the front pair alone */
static float g_volume = 1.0f;
static double g_out_gain = 6.0;
static uint32_t underruns, per_sec_ur, cb_samples, latency_resets;
static atomic_uint ur_total, reset_total;      /* the audio thread's counts, read by the emulation thread for the log */
#define FADE 240                               /* samples (5 ms): the soft landing when the card is starved, and the fade back in */
static int16_t last_out[2], hold[2];          /* the last sample handed to the card / the one a starvation fades out from */
static int fade_phase, fade_step;              /* 0 data flowing, 1 fading the held sample out, 2 silent / fading the data back in */
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
    if ((int)(h - t) > 4 * TARGET) { t = h - TARGET; latency_resets++; atomic_fetch_add(&reset_total, 1); }   /* far behind: drop to the target once */
    if (!atomic_load(&primed) && fade_phase != 1) {       /* waiting for the cushion (start-up, or after a dry spell: the fade-out first) */
        if ((int)(h - t) < TARGET) {
            for (int i = 0; i < n; i++) { o[2*i] = o[2*i+1] = 0; }
            last_out[0] = last_out[1] = 0;
            if (fade_phase == 0) { fade_phase = 2; fade_step = 0; }         /* the sound fades back in when it resumes */
            return;
        }
        atomic_store(&primed, 1);
    }
    unsigned ur_now = 0;
    /* A starved card would be handed zeros: a step from the last sample to silence is a click. Instead the last sample is HELD and faded to zero over
     * FADE samples, always to the end (even if the data comes back sooner: it waits in the ring), and only then is the data faded back in -- so every step
     * of the output stays small whatever the length of the gap. */
    for (int i = 0; i < n; i++) {
        int16_t l = 0, r = 0;
        if (fade_phase == 0 && t == h) {
            hold[0] = last_out[0]; hold[1] = last_out[1]; fade_phase = 1; fade_step = 0;
            const int tg = TARGET;                                    /* ran dry: a bigger cushion, refilled before the sound comes back */
            if (tg < target_max) { atomic_store(&g_target, tg + TARGET_STEP > target_max ? target_max : tg + TARGET_STEP); atomic_store(&primed, 0); }
        }
        if (t == h) { underruns++; per_sec_ur++; ur_now++; }
        if (fade_phase == 1) {
            const float k = 1.0f - (float)(fade_step + 1) / FADE;
            l = (int16_t)(hold[0] * k); r = (int16_t)(hold[1] * k);
            if (++fade_step >= FADE) { fade_phase = 2; fade_step = 0; }
        } else if (t != h) {
            l = ring[(t & (RING - 1)) * 2]; r = ring[(t & (RING - 1)) * 2 + 1];
            t++;
            if (fade_phase == 2) {
                const float k = (float)fade_step / FADE; l = (int16_t)(l * k); r = (int16_t)(r * k);
                if (++fade_step >= FADE) { fade_phase = 0; fade_step = 0; }
            }
        }
        o[2*i] = l; o[2*i+1] = r; last_out[0] = l; last_out[1] = r;
    }
    if (ur_now) atomic_fetch_add(&ur_total, ur_now);
    atomic_store_explicit(&r_tail, t, memory_order_release);
    cb_samples += (uint32_t)n;
    if (cb_samples >= OUT_HZ) {
        static int logon = -1; if (logon < 0) logon = getenv("ENG_AUDIOLOG") != NULL;
        if (logon) fprintf(stderr, "[AUDIO] second: %u underrun samples, ring %d, cushion %d ms, latency resets %u\n", per_sec_ur, (int)(h - t), TARGET * 1000 / OUT_HZ, latency_resets);
        cb_samples -= OUT_HZ; per_sec_ur = 0;
    }
}

bool eng_audio_open(void)
{
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) { fprintf(stderr, "[AUDIO] SDL audio: %s\n", SDL_GetError()); return false; }
    SDL_AudioSpec want, have;
    memset(&want, 0, sizeof want);
    want.freq = OUT_HZ; want.format = AUDIO_S16SYS; want.channels = 2; want.callback = audio_cb;
#ifdef _WIN32
    want.samples = 1024;                         /* WASAPI's callback thread is scheduled less tightly than ALSA's: a 10 ms buffer crackles when it is late (the 60 ms ring sets the latency, not this) */
#else
    want.samples = 512;                          /* ENG_AUDIO_SAMPLES: 480 = exactly 10 ms, for SDL's disk driver (it sleeps whole ms) */
#endif
    if (getenv("ENG_AUDIO_SAMPLES")) want.samples = (Uint16)atoi(getenv("ENG_AUDIO_SAMPLES"));
    dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!dev) { fprintf(stderr, "[AUDIO] no audio device: %s\n", SDL_GetError()); return false; }
    if (getenv("ENG_AUDIO_MAXMS")) { int ms = atoi(getenv("ENG_AUDIO_MAXMS")); if (ms < 60) ms = 60; if (ms > 300) ms = 300; target_max = OUT_HZ * ms / 1000; }
    if (getenv("ENG_OUTPUT_GAIN")) { g_out_gain = atof(getenv("ENG_OUTPUT_GAIN")); if (g_out_gain < 0) g_out_gain = 0; }
    SDL_PauseAudioDevice(dev, 0);
    fprintf(stderr, "[AUDIO] output %d Hz stereo, %d-sample buffer, gain x%.2f\n", have.freq, have.samples, g_out_gain);
    return true;
}

void eng_audio_set_gain(double gain) { if (gain > 0) g_out_gain = gain; }
void eng_audio_set_mix(const float m[2][4]) { memcpy(g_mix, m, sizeof g_mix); }
void eng_audio_set_volume(int percent) { g_volume = (percent < 0 ? 0 : percent > 100 ? 100 : percent) / 100.0f; }

void eng_audio_push(const int16_t *in4, int n)
{
    if (!dev || n <= 0) return;
    {   /* a starved sound card is heard as pops: say so in the log, at most every 3 s */
        static uint32_t last_ms, seen_ur, seen_rs;
        const uint32_t now = SDL_GetTicks();
        if (now - last_ms >= 3000) {
            const uint32_t ur = atomic_load(&ur_total), rs = atomic_load(&reset_total);
            if (last_ms && (ur != seen_ur || rs != seen_rs))
                fprintf(stderr, "[AUDIO] the sound card ran dry: %u samples of silence (%.0f ms) and %u latency resets in the last %.1f s -- the game did not deliver "
                        "sound fast enough (heard as pops); a slow frame, or the window busy\n", ur - seen_ur, (ur - seen_ur) * 1000.0 / OUT_HZ, rs - seen_rs, (now - last_ms) / 1000.0);
            last_ms = now; seen_ur = ur; seen_rs = rs;
        }
    }
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
        double s[4];
        for (int c = 0; c < 4; c++) {
            const double a = i == 0 ? rs_prev[c] : in4[(i - 1) * 4 + c], b = in4[i * 4 + c];
            s[c] = a + (b - a) * f;
        }
        for (int ch = 0; ch < 2; ch++) {
            const double v = soft_limit((s[0] * g_mix[ch][0] + s[1] * g_mix[ch][1] + s[2] * g_mix[ch][2] + s[3] * g_mix[ch][3]) * g_volume * g_out_gain);
            if (h - t < RING) ring[(h & (RING - 1)) * 2 + ch] = (int16_t)lrint(v);
        }
        if (h - t < RING) h++;
        rs_pos += step;
    }
    rs_pos -= n;
    memcpy(rs_prev, &in4[(n - 1) * 4], sizeof rs_prev);
    atomic_store_explicit(&r_head, h, memory_order_release);
}

void eng_audio_close(void)
{
    if (dev) { SDL_CloseAudioDevice(dev); dev = 0; if (underruns) fprintf(stderr, "[AUDIO] %u output underrun samples\n", underruns); }
}
