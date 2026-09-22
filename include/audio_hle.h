/* HLE of the sound COMMAND layer -- AUDIO_PLAN.md phase 3's documented
 * fallback, taken deliberately: the M37710 sound MCU core does not exist
 * (a real CPU core is multi-session work), so one-shot SFX are triggered
 * directly from the exact decompiled call site that would have sent the
 * real MCU command. Music stays silent -- it needs the MCU's actual
 * sequencer, which this does not attempt to fake.
 *
 * Unlike src/c352.c's offline gate path (which drives the full chip
 * emulation off the 16MB wave ROM, for measuring against MAME), the LIVE
 * game plays small pre-decoded assets that tools/extract_sound_assets.py
 * extracts from that same ROM once, at build time -- see that tool's
 * header. The running game never touches the wave ROM.
 */
#ifndef PROPCYCL_AUDIO_HLE_H
#define PROPCYCL_AUDIO_HLE_H

#include <stdint.h>
#include <stdbool.h>

/* Loads sounds_dir/sfx_*.wav (tools/extract_sound_assets.py's output;
 * NULL defaults to "sounds") and installs the live one-shot mixer as the
 * audio_set_source. Missing/unextracted assets log a warning and just
 * play silent -- never fatal, matching audio_init's own "no sound card is
 * not a reason to refuse to run the game" rule. */
void audio_hle_init(const char *sounds_dir, const char *rom_dir);

typedef enum {
    AUDIO_SFX_COIN,   /* coin_credit_update(), game_misc.c: fires on the
                        * same coin edge that already writes
                        * commsram[0x7D26]=1 (the real, currently-unconsumed
                        * MCU command). Voice params measured against MAME
                        * -- see tools/overnight/dump_sound_trigger.lua and
                        * AUDIO_PLAN.md phase 3. */

    AUDIO_SFX_COUNT
} audio_sfx_t;


/* Starts sfx playing (one-shot, mixed with whatever else is already
 * playing). Call from the exact decompiled call site named in the enum
 * comment above, not from a new decision point -- the trigger CONDITION
 * is the decompiled code's, this only supplies the sound. */
void audio_hle_trigger(audio_sfx_t sfx);

/* Looping ambient/music tracks -- see tools/extract_music_assets.py. There
 * is no discrete "background music" on this hardware in the usual sense:
 * the M37710 firmware drives several C352 voices continuously and we have
 * not decompiled that firmware, so each track here is MAME's own captured
 * mix for one game STATE, looped, rather than a synthesized composition.
 * Selected every frame in audio_hle_tick() directly from the decompiled
 * state variables (W[0x0CBC]/W[0x0CC0], and W[0x0E0C] for the course
 * number during gameplay) -- not a new decision point, the same state
 * machine game_gameplay.c/game_title.c already drive. */
typedef enum {
    AUDIO_MUSIC_NONE = -1,
    AUDIO_MUSIC_ATTRACT_DEMO,   /* top state 0/1 (attract) */
    AUDIO_MUSIC_MENU,           /* state 3, sub in {20,21,12,13,0,1}
                                  * (controls tutorial / mode select / stage
                                  * select -- the "menu cluster" CLAUDE.md
                                  * documents under state_gameplay_init) */
    AUDIO_MUSIC_GAMEPLAY_COURSE0, /* state 3, sub in {2,3,5}, W[0x0E0C]==0
                                    * (gameplay_sub_init / intro orbit /
                                    * actual riding) */
    AUDIO_MUSIC_GAMEPLAY_COURSE1, /* same, W[0x0E0C]==1 */
    AUDIO_MUSIC_GAMEPLAY_COURSE2, /* same, W[0x0E0C]==2 */
    /* Course 3 is not reached via the ordinary STAGE SELECT cursor (it
     * clamps at course 2 -- measured against real MAME with
     * tools/overnight/probe_course_select.lua) and needs whatever
     * `stage_transition_init` reaches it with -- see CLAUDE.md's
     * "Multi-Course Status" -- so it has no captured track and falls
     * through to AUDIO_MUSIC_NONE (silence) below, same treatment as
     * every other unmeasured state. */
    AUDIO_MUSIC_COUNT
} audio_music_t;

/* Call once per game frame, after game_frame(). Drives both the state-based
 * music selection above and (headless/deviceless) PROPCYCL_AUDIODUMP
 * verification -- see src/audio_hle.c. PROPCYCL_NO_MUSIC=1 (read once at
 * audio_hle_init) mixes SFX only, for isolating one layer from the other. */
void audio_hle_tick(void);

/* Master output gain, 0.0 (silent) .. 1.0 (unattenuated). Applied at the end
 * of the mix, so it scales the chip and the one-shot voices together and
 * never alters the balance the sound driver chose. The Escape menu's Audio
 * tab drives it and ui_controls_save() persists it. */
float audio_hle_volume(void);
void  audio_hle_set_volume(float v);

#endif /* PROPCYCL_AUDIO_HLE_H */
