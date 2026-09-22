/* SDL2 audio output. See include/audio.h for the two rules this obeys. */
#include "audio.h"

#include <SDL2/SDL.h>
#include <stdio.h>
#include <string.h>

/* 44100 is a request, not a promise -- SDL may hand back something else and
 * audio_sample_rate() reports what we actually got. The C352 runs at its own
 * rate and phase 2 resamples; keeping that conversion in one place is why
 * the rate is exposed rather than assumed. */
#define AUDIO_REQ_RATE     44100
#define AUDIO_REQ_SAMPLES  1024   /* frames per callback; ~23 ms at 44.1 kHz */

static SDL_AudioDeviceID g_dev;
static int               g_rate;
static audio_source_fn   g_src;
static void             *g_src_user;
static SDL_SpinLock      g_src_lock;
static unsigned          g_cb_count, g_underruns;

static void audio_callback(void *user, Uint8 *stream, int len)
{
    (void)user;
    int frames = len / (int)(2 * sizeof(int16_t));   /* stereo, 16-bit */

    audio_source_fn fn; void *fu;
    SDL_AtomicLock(&g_src_lock);
    fn = g_src; fu = g_src_user;
    SDL_AtomicUnlock(&g_src_lock);

    g_cb_count++;
    if (!fn) {
        /* Silence, not stale memory: SDL does not guarantee a zeroed buffer
         * and a garbage-filled one is a loud noise, not a quiet bug. */
        memset(stream, 0, (size_t)len);
        g_underruns++;
        return;
    }
    fn((int16_t *)stream, frames, fu);
}

bool audio_init(bool enable)
{
    if (!enable) return false;
    if (g_dev) return true;

    /* SDL_INIT_VIDEO is already up by the time this is called; initialising
     * the audio subsystem separately keeps a failure here from taking the
     * video down with it. A machine with no sound card is not a reason to
     * refuse to run the game. */
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
        fprintf(stderr, "[AUDIO] no audio subsystem (%s) -- running silent\n",
                SDL_GetError());
        return false;
    }

    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq     = AUDIO_REQ_RATE;
    want.format   = AUDIO_S16SYS;
    want.channels = 2;
    want.samples  = AUDIO_REQ_SAMPLES;
    want.callback = audio_callback;

    g_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have,
                                SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
    if (!g_dev) {
        fprintf(stderr, "[AUDIO] could not open a device (%s) -- running silent\n",
                SDL_GetError());
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return false;
    }
    g_rate = have.freq;
    SDL_PauseAudioDevice(g_dev, 0);
    fprintf(stderr, "[AUDIO] %d Hz, %d channels, %d-frame buffer\n",
            have.freq, have.channels, have.samples);
    return true;
}

void audio_shutdown(void)
{
    if (!g_dev) return;
    SDL_PauseAudioDevice(g_dev, 1);
    SDL_CloseAudioDevice(g_dev);
    g_dev = 0; g_rate = 0;
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

void audio_set_source(audio_source_fn fn, void *user)
{
    SDL_AtomicLock(&g_src_lock);
    g_src = fn; g_src_user = user;
    SDL_AtomicUnlock(&g_src_lock);
}

int audio_sample_rate(void) { return g_rate; }

void audio_stats(unsigned *callbacks, unsigned *underruns)
{
    if (callbacks) *callbacks = g_cb_count;
    if (underruns) *underruns = g_underruns;
}
