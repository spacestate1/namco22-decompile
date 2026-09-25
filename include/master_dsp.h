/*
 * master_dsp.h -- the live game's MASTER DSP: the C71 (TMS320C25) port in
 * engine/c25 running the TRANSLATED program (gen/pc_c25.c), booted from the game's own ROM and run once per frame on the
 * real CPU->master doorbell, over g_sys.dspram itself.
 *
 * This is the stage the hardware uses to turn the CPU's short display list
 * into the scene: every object's matrices, lighting, the viewport records and
 * the primitive list at polygon-RAM word 0x300 that the renderer draws. The
 * port is gated exact against MAME (tools/master_seq_gate.c: 60/60 attract
 * frames and 120/120 steered gameplay frames, word for word; a cold boot
 * converges after one frame).
 *
 * Off unless PROPCYCL_MASTER=1 while the live path is being validated.
 */
#ifndef MASTER_DSP_H
#define MASTER_DSP_H
#include <stdint.h>
#include <stdbool.h>

/* Load the BIOS (c71.bin) from rom_dir and the master program from the game
 * ROM (upload block 0x4374A), reset, and run the master's own init to IDLE.
 * Returns false (and stays inactive) when disabled or the BIOS is missing. */
bool master_dsp_init(const char *rom_dir);

/* True once the master is booted and driving the scene. */
bool master_dsp_active(void);

/* Call right after the CPU's vblank handler: if it rang the doorbell
 * (polygon-RAM word 1), run the master's frame to IDLE. */
void master_dsp_vblank(void);

/* The master's output as the renderer's record walker wants it: all 0x8000
 * polygon-RAM words masked to 24 bits. NULL until the master has run a frame. */
const uint32_t *master_dsp_output(void);

#endif
