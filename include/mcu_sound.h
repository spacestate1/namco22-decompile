/* The Prop Cycle sound MCU, actually running.
 *
 * WHY THIS AND NOT A MODEL. AUDIO_PLAN.md's "route not taken" is the
 * master-DSP lesson applied to sound: a captured register script replays
 * identically no matter what the player does (measured -- two runs at pedal
 * 40 and 127 were bit-identical, max sample difference 0). The music and the
 * effects are produced by a program in `pr1data.8k`, so the only way to get
 * them is to run it.
 *
 * The board (namcos22.cpp, namcos22s_state::mcu_program):
 *     0x002000-0x002fff  C352            -> src/c352.c, the chip audio_hle owns
 *     0x004000-0x00bfff  shared RAM      -> g_sys.commsram, BYTE LANES SWAPPED
 *     0x00c000-0x00ffff  ROM @ 0xc000
 *     0x200000-0x27ffff  ROM @ 0
 *     0x308000-0x308003  MB87078 volume
 * Clock 49.152MHz/3 = 16.384MHz.
 *
 * THE BYTE LANE SWAP is not a detail. The shared RAM is one 16-bit memory
 * wired to both CPUs; MAME models it as a u16 array that each side reads as a
 * word, so both see the same VALUE. We hold it as bytes in 68K order, and the
 * M37710 is little-endian, so its byte address A reaches the other lane,
 * A ^ 1. Without it the driver's own command test cannot pass: it polls with
 * `LDA $00,X ; ASL A ; BPL` (ROM 0xD6F6) -- "is bit 14 set?" -- and a
 * 68K-order 0x4005 reads back as 0x0540, whose bit 14 is 0. Measured: a
 * command sat unacknowledged in its slot for a whole run before this.
 */
#ifndef PROPCYCL_MCU_SOUND_H
#define PROPCYCL_MCU_SOUND_H

#include <stdbool.h>

/* Loads pr1data.8k from rom_dir and resets the core. Returns false and stays
 * inert if the ROM is absent -- a missing file silences the game, it never
 * stops it, the same rule audio_init already follows. */
bool mcu_sound_init(const char *rom_dir);

/* One 60Hz frame of MCU time (16.384MHz / 60 = 273066 cycles), with the four
 * interrupt sources the board drives. Call once per game frame, after the
 * game has written its commands into g_sys.commsram. */
void mcu_sound_run_frame(void);

bool mcu_sound_ready(void);

#endif /* PROPCYCL_MCU_SOUND_H */
