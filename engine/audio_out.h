/*
 * audio_out.h -- the C352's stereo output to the sound card (engine/audio_out.c).
 *
 * The chip produces 85,333 Hz frames (clock 24.576 MHz / 288); the card takes 48 kHz. The game is paced by the display,
 * never exactly by the audio clock, so a resampler trims its ratio (+-0.5%, up to +-2% when far off) to hold the ring
 * near a 60 ms target: no underrun crackle, no creeping latency. A soft limiter above -6 dBFS rounds off a loud moment
 * instead of clipping. The chip's level is authentic (it matches MAME's wavwrite) but the cabinet's amplifier supplied the
 * rest, so the speaker output carries a gain (default x6, which is Rave Racer's; a game sets its own with eng_audio_set_gain because its
 * chip runs hotter or cooler; ENG_OUTPUT_GAIN=<x> overrides both, 1 = raw). If the sound card is ever starved the output fades out and back
 * in over 5 ms instead of dropping to zero (a click), and the log says so (rate-limited).
 */
#ifndef ENG_AUDIO_OUT_H
#define ENG_AUDIO_OUT_H
#include <stdint.h>
#include <stdbool.h>

bool eng_audio_open(void);                          /* SDL audio device, stereo 48 kHz; false = silent */
void eng_audio_push(const int16_t *in4, int n);     /* n frames of the chip's four outputs (0/1 = the front pair) */
/* how the four outputs reach the stereo pair: out[s] = sum of m[s][c] * in[c]. The default is the front pair alone (1 0 0 0 / 0 1 0 0);
 * a cabinet with a speaker of its own on output 2 or 3 (Dirt Dash's road speaker) mixes it in (engine/ss22_snd.c) */
void eng_audio_set_mix(const float m[2][4]);
void eng_audio_set_volume(int percent);             /* 0..100 */
void eng_audio_set_gain(double gain);               /* this game's speaker gain (before eng_audio_open); <= 0 keeps the default; ENG_OUTPUT_GAIN wins */
void eng_audio_close(void);
#endif
