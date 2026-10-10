/*
 * tc2_audio.c -- the C352's output (src/tc2_h8.c generates it in step with the emulated time, 88200 Hz, four channels FL FR RL RR): front and
 * rear summed to stereo (MAME's speaker routes 0+2 left, 1+3 right) and queued to SDL in a window; $TC2_WAV=path also writes it to a WAV file
 * (headless runs: the way to listen to, or measure, what the sub-CPU's sound program played).
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <SDL2/SDL.h>

static SDL_AudioDeviceID dev;
static FILE *wav; static uint32_t wav_frames;
static int volume = 100;

static void wav_header(void)
{
    const uint32_t rate = 88200, bytes = wav_frames * 4;
    uint8_t h[44] = { 'R','I','F','F', 0,0,0,0, 'W','A','V','E', 'f','m','t',' ', 16,0,0,0, 1,0, 2,0 };
    #define PUT32(o, v) do { h[o] = (uint8_t)(v); h[o+1] = (uint8_t)((v) >> 8); h[o+2] = (uint8_t)((v) >> 16); h[o+3] = (uint8_t)((v) >> 24); } while (0)
    PUT32(4, 36 + bytes); PUT32(24, rate); PUT32(28, rate * 4); h[32] = 4; h[34] = 16;
    memcpy(h + 36, "data", 4); PUT32(40, bytes);
    fseek(wav, 0, SEEK_SET); fwrite(h, 1, 44, wav); fseek(wav, 0, SEEK_END);
}
static void wav_close(void) { if (wav) { wav_header(); fclose(wav); wav = NULL; } }

void tc2_audio_open(void)
{
    if (dev) return;
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) { fprintf(stderr, "[TC2] no audio: %s\n", SDL_GetError()); return; }
    SDL_AudioSpec want = { 0 }, have;
    want.freq = 88200; want.format = AUDIO_S16SYS; want.channels = 2; want.samples = 1024;
    dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);           /* SDL converts to the card's own rate */
    if (!dev) { fprintf(stderr, "[TC2] no audio device: %s\n", SDL_GetError()); return; }
    SDL_PauseAudioDevice(dev, 0);
}
void tc2_audio_volume(int percent) { volume = percent; }

void tc2_audio_push(const int16_t *s4, int n)
{
    static int tried;
    if (!tried) { tried = 1;
        extern int g_c352_gain_music, g_c352_gain_fx; extern unsigned g_c352_music_mask;           /* headless tests: the Audio page's gains without a window */
        if (getenv("TC2_MUSIC_VOL")) g_c352_gain_music = atoi(getenv("TC2_MUSIC_VOL"));
        if (getenv("TC2_FX_VOL")) g_c352_gain_fx = atoi(getenv("TC2_FX_VOL"));
        if (getenv("TC2_MUSIC_VOICES")) g_c352_music_mask = (unsigned)strtoul(getenv("TC2_MUSIC_VOICES"), NULL, 16);
        const char *p = getenv("TC2_WAV"); if (p && (wav = fopen(p, "wb"))) { fseek(wav, 44, SEEK_SET); atexit(wav_close); } }
    if (!dev && !wav) return;
    static int16_t st[2 * 256];
    for (int i = 0; i < n && i < 256; i++) {
        int l = (s4[4 * i] + s4[4 * i + 2]) * volume / 100, r = (s4[4 * i + 1] + s4[4 * i + 3]) * volume / 100;
        st[2 * i] = (int16_t)(l > 32767 ? 32767 : l < -32768 ? -32768 : l);
        st[2 * i + 1] = (int16_t)(r > 32767 ? 32767 : r < -32768 ? -32768 : r);
    }
    if (wav) { fwrite(st, 4, (size_t)n, wav); wav_frames += (uint32_t)n; }
    if (dev) {
        if (SDL_GetQueuedAudioSize(dev) > 88200u * 4 / 4) return;     /* more than 0.25 s queued: the emulation runs ahead -- drop */
        SDL_QueueAudio(dev, st, (uint32_t)n * 4);
    }
}
