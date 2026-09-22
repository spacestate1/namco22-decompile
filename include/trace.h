/*
 * L3 differential trace support (GUARDRAILS.md §5 L3 / §7 step 1).
 *
 * Per-frame dump of work RAM + DSP RAM, anchored at the game frame counter
 * increment (WRAM 0x0C98) so both sides — this reimplementation and MAME —
 * capture the same logical instant: post-irq_vblank, pre-state-dispatch,
 * with the previous frame's completed DSP command list in the buffer.
 *
 * MAME side counterpart: tools/l3/mame_trace.lua (write tap on 0xE00C98).
 * Differ: tools/l3/diff_trace.py.
 */
#ifndef TRACE_H
#define TRACE_H

#include <stdint.h>
#include <stdbool.h>

/* Start tracing into dir (created if missing). max_fc = stop dumping after
 * this game-frame-counter value (0 = no limit). */
bool trace_init(const char* dir, uint32_t max_fc);
bool trace_active(void);

/* Call exactly once per game frame, immediately after the frame counter
 * increment in game_frame() — the L3 anchor point (D4). */
void trace_on_frame(uint32_t fc);

void trace_finish(void);

/* ---- KEYCUS determinism (D2) ----
 * Replays the read-value sequence recorded from MAME (keycus.log written
 * by mame_trace.lua: one "fc addr data mask" line per bus read of
 * 0x400000-0x40001F). While a recording is loaded, memory.c routes keycus
 * reads here instead of the LCG. */
bool trace_keycus_load(const char* path);
bool trace_keycus_active(void);
uint32_t trace_keycus_read(uint32_t addr);

#endif
