#ifndef RR_SOUND_H
#define RR_SOUND_H
#include <stdint.h>
#include <stdbool.h>
/*
 * The sound system. Two halves:
 *   the AUDIO side (src/rr_audio.c, always the engine's): the C352 sample
 *     mixer, the wave ROM (rv1wav0-3) and the output stream;
 *   the DRIVER (the C74 sound program's logic): src/rr_sound_stub.c in the game
 *     until the translated driver replaces it; tools/sndoracle/ in rr_sndoracle,
 *     where the ORIGINAL program runs on an emulated M37702 as a test oracle only.
 * The two share the 32 KB RAM at 68K 0x60004000 (C74 0x4000), g_rr.shared.
 */
#define RR_SND_SLICES 16                      /* matches rr_main's frame slices */

/* driver (one implementation per binary) */
bool rr_sound_init(const char *rom_dir);
void rr_sound_set_run(bool run);              /* syscon 0x18: 0 holds the sound CPU in reset */
void rr_sound_slice(void);                    /* 1/16 of a video frame of sound-program time */

/* audio (engine) */
bool rr_audio_init(const char *rom_dir);
void rr_audio_c352_write(unsigned word, uint16_t v);
uint16_t rr_audio_c352_read(unsigned word);
void rr_audio_slice(void);                    /* mix this slice's samples */
void rr_audio_close(void);
bool rr_audio_output_open(void);               /* windowed runs: SDL audio at 48 kHz */
void rr_audio_set_volume(int percent);

#endif
