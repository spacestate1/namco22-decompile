/* snd_engine.h -- the sound driver, rewritten as readable C, one routine at a
 * time (see src/snd/snd_engine.c). While the rewrite is in progress every
 * routine is entered from the translated program (gen/snd_driver.c) at its
 * original address and returns the way the original does. Each returns 1 when
 * it ran the routine, 0 (touching nothing) to have the original run instead. */
#ifndef PC_SND_ENGINE_H
#define PC_SND_ENGINE_H
#include "m37710.h"

int  snd_atomic_ok(m37710_t *c, unsigned maxcyc);
int  snd_call(m37710_t *c, uint16_t addr);      /* run a translated routine to its RTS; nonzero = abandon (gen/snd_driver.c) */
void snd_cost_check(m37710_t *c, uint32_t pc, uint64_t t0, unsigned maxcyc);
int  snd_voice_volume_from_channel(m37710_t *c);   /* 0xE97E */
int  snd_voice_volume_from_scratch(m37710_t *c);   /* 0xE987 */
int  snd_pitch_glide(m37710_t *c);                 /* 0xEBB8 */
int  snd_pitch_envelope(m37710_t *c);              /* 0xEBFD */
int  snd_pan_to_speakers(m37710_t *c);             /* 0xE87E */
int  snd_slot_level(m37710_t *c);                  /* 0xD745 */
int  snd_tick(m37710_t *c);                        /* 0xC258 */
int  snd_tick_body(m37710_t *c);                   /* 0xD684 */
int  snd_mailbox(m37710_t *c);                     /* 0xD6E3 */
int  snd_channels(m37710_t *c);                    /* 0xE46E */
int  snd_channel_update(m37710_t *c);              /* 0xE8DB */
int  snd_hook_fx1(m37710_t *c);                    /* 0xEF45 */
int  snd_hook_fx4(m37710_t *c);                    /* 0xE9D6 */
int  snd_hook_volume(m37710_t *c);                 /* 0xECAA */
int  snd_pan_then_volume(m37710_t *c);             /* 0xECC4 */
int  snd_note_event(m37710_t *c);                  /* 0xE4E7 */
int  snd_key_on(m37710_t *c);                      /* 0xE5A7 */
int  snd_sequencer(m37710_t *c);                   /* 0xD9D0 */
int  snd_seq_goto(m37710_t *c);                    /* command 0x10, 0xE277 */
int  snd_seq_poke16(m37710_t *c);                  /* command 0x02, 0xDAAF */
int  snd_seq_06(m37710_t *c);                      /* command 0x06, 0xDB5C */
int  snd_seq_07(m37710_t *c);                      /* command 0x07, 0xDB63 */
int  snd_seq_2d(m37710_t *c);                      /* command 0x2D, 0xDB9D */
int  snd_seq_04(m37710_t *c);                      /* command 0x04, 0xDAF0 */
int  snd_seq_08(m37710_t *c);                      /* command 0x08, 0xDB6D */
int  snd_seq_28(m37710_t *c);                      /* command 0x28, 0xE38A */
int  snd_seq_29(m37710_t *c);                      /* command 0x29, 0xE3A3 */
int  snd_seq_1a(m37710_t *c);                      /* command 0x1A, 0xDC83 */
int  snd_seq_1d(m37710_t *c);                      /* command 0x1D, 0xE342 */
int  snd_seq_1b(m37710_t *c);                      /* command 0x1B, 0xDD01 */
int  snd_seq_1c(m37710_t *c);                      /* command 0x1C, 0xDC90 */
int  snd_seq_0a(m37710_t *c);                      /* command 0x0A, 0xDB8D */
int  snd_seq_0d(m37710_t *c);                      /* command 0x0D, 0xDBF0 */
int  snd_seq_gosub(m37710_t *c);                   /* command 0x11, 0xE157 */
int  snd_seq_return(m37710_t *c);                  /* command 0x14, 0xE185 */
int  snd_seq_17(m37710_t *c);                      /* command 0x17, 0xE143 */
int  snd_seq_19(m37710_t *c);                      /* command 0x19, 0xDC4A */
int  snd_seq_20(m37710_t *c);                      /* command 0x20, 0xDF59 */
int  snd_seq_22(m37710_t *c);                      /* command 0x22, 0xDF6D */
int  snd_seq_1f(m37710_t *c);                      /* command 0x1F, 0xDE77 */
int  snd_seq_1e(m37710_t *c);                      /* command 0x1E, 0xDD85 */
int  snd_seq_12(m37710_t *c);                      /* command 0x12, 0xE286 */
int  snd_idle_wait(m37710_t *c);                   /* 0xC12B */
int  snd_new_command(m37710_t *c);                 /* 0xD788 */
int  snd_track_stop(m37710_t *c);                  /* 0xE18D */
int  snd_stop_slot(m37710_t *c);                   /* 0xD982 */
int  snd_voice_free(m37710_t *c);                  /* 0xD8F9 */
int  snd_voice_best(m37710_t *c);                  /* 0xD912 */
int  snd_voice_handoff(m37710_t *c);               /* 0xD936 */
int  snd_voice_request(m37710_t *c);               /* 0xD8A0 */
int  snd_voice_claim(m37710_t *c);                 /* 0xD8CF */
int  snd_subch_reset(m37710_t *c);                 /* 0xD95B */
int  snd_env_up(m37710_t *c);                      /* 0xEF48 */
int  snd_env_down(m37710_t *c);                    /* 0xEF5C */
int  snd_env_sustain(m37710_t *c);                 /* 0xEF6D */
int  snd_env_start(m37710_t *c);                   /* 0xEF9D */
int  snd_env_off(m37710_t *c);                     /* 0xEFAE */
int  snd_sample_change(m37710_t *c);               /* 0xF013 */
int  snd_sample_reapply(m37710_t *c);              /* 0xF064 */
int  snd_sample_stream(m37710_t *c);               /* 0xEB29 */
int  snd_fixed_pan(m37710_t *c);                   /* 0xE7FE */
int  snd_pan_script_start(m37710_t *c);            /* 0xECD4 */
int  snd_pan_sweep_down(m37710_t *c);              /* 0xED7B */
int  snd_pan_sweep_up(m37710_t *c);                /* 0xED8B */

#endif
