/* Audio output plumbing.
 *
 * Phase 1 of AUDIO_PLAN.md: a device, a ring buffer, and nothing else. The
 * sample SOURCE is a callback the mixer installs later (the C352, phase 2);
 * until then this feeds silence.
 *
 * TWO RULES, both from things this project has already been burned by:
 *
 *  1. AUDIO MUST NOT BECOME A SECOND CLOCK. Every gate in this repo depends
 *     on the frame loop being deterministic and headless-runnable. Nothing
 *     here ever blocks the game, and the device is not opened at all when
 *     headless.
 *  2. NO getenv() IN THE FRAME LOOP (register row 40 -- it segfaults). All
 *     configuration is read once, at init.
 */
#ifndef PROPCYCL_AUDIO_H
#define PROPCYCL_AUDIO_H

#include <stdint.h>
#include <stdbool.h>

/* Fills `frames` stereo 16-bit sample pairs. Called from the SDL audio
 * thread, so it must not touch game state that the frame loop is writing.
 * Phase 2 points this at the C352. */
typedef void (*audio_source_fn)(int16_t *dst, int frames, void *user);

/* Open the device. `enable` false (headless, or --noaudio) makes every other
 * call here a no-op, so callers need no conditionals. Returns true if a
 * device is actually running. */
bool audio_init(bool enable);
void audio_shutdown(void);

/* Install the sample source. NULL restores silence. */
void audio_set_source(audio_source_fn fn, void *user);

/* The device's sample rate, or 0 when no device is open. */
int  audio_sample_rate(void);

/* Diagnostics for PROPCYCL_AUDIOLOG: how many times the callback ran dry.
 * An underrun count that climbs is the symptom of the mixer being too slow
 * or the ring being too small; it is the one number worth watching. */
void audio_stats(unsigned *callbacks, unsigned *underruns);

#endif /* PROPCYCL_AUDIO_H */
