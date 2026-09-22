/* Namco C352 32-voice PCM chip -- audio phase 2 of AUDIO_PLAN.md.
 *
 * Ported from MAME's src/devices/sound/c352.cpp (BSD-3-Clause, R. Belmont /
 * superctr). Register semantics, the mulaw table and the mix formula are
 * copied field for field so this can be gated against MAME's own output --
 * see tools/overnight/c352_gate.py. The register map (8 u16 registers per
 * voice: vol_f, vol_r, freq, flags, wave_bank, wave_start, wave_end,
 * wave_loop) is MAME's `reg_map` order, not a struct-layout accident, so
 * it does not depend on this struct's field order.
 *
 * On Prop Cycle (Super System 22), the chip sits at 0x002000-0x002fff in
 * the M37710 sound MCU's program space (offset 0-0xff = 32 voices x 8
 * regs, 0x200 = control, 0x202 = keyon/keyoff exec), clocked at
 * 49.152MHz/2 = 24.576MHz with divider 288 -> 85333.33Hz output.
 * Sample ROM is a 16MB space: pr1wavea.2l at 0x000000, pr1waveb.1l at
 * 0x800000 (both 4MB; the gaps are unmapped).
 */
#ifndef PROPCYCL_C352_H
#define PROPCYCL_C352_H

#include <stdint.h>

#define C352_NUM_VOICES 32
#define C352_CLOCK       24576000u
#define C352_DIVIDER      288
/* clock/divider = 85333.33.. Hz; kept as a rational pair so callers can
 * do exact frame-accumulator math instead of drifting on a rounded float. */

enum {
    C352_FLG_BUSY       = 0x8000,
    C352_FLG_KEYON      = 0x4000,
    C352_FLG_KEYOFF     = 0x2000,
    C352_FLG_LOOPTRG    = 0x1000,
    C352_FLG_LOOPHIST   = 0x0800,
    C352_FLG_FM         = 0x0400,
    C352_FLG_PHASERL    = 0x0200,
    C352_FLG_PHASEFL    = 0x0100,
    C352_FLG_PHASEFR    = 0x0080,
    C352_FLG_LDIR       = 0x0040,
    C352_FLG_LINK       = 0x0020,
    C352_FLG_NOISE      = 0x0010,
    C352_FLG_MULAW      = 0x0008,
    C352_FLG_FILTER     = 0x0004,
    C352_FLG_REVLOOP    = 0x0003,
    C352_FLG_LOOP       = 0x0002,
    C352_FLG_REVERSE    = 0x0001
};

typedef struct {
    uint32_t pos;
    uint32_t counter;

    int16_t sample;
    int16_t last_sample;

    uint16_t vol_f;
    uint16_t vol_r;
    uint8_t  curr_vol[4];

    uint16_t freq;
    uint16_t flags;

    uint16_t wave_bank;
    uint16_t wave_start;
    uint16_t wave_end;
    uint16_t wave_loop;
} c352_voice_t;

typedef struct {
    c352_voice_t voice[C352_NUM_VOICES];
    int16_t  mulawtab[256];
    uint16_t random_state;
    uint16_t control;

    const uint8_t *rom;      /* sample ROM image, byte-addressed */
    uint32_t       rom_size; /* e.g. 0x1000000; reads outside return 0 */
} c352_t;

void c352_init(c352_t *c, const uint8_t *rom, uint32_t rom_size);
void c352_reset(c352_t *c);

uint16_t c352_read(const c352_t *c, uint32_t offset);
void     c352_write(c352_t *c, uint32_t offset, uint16_t data, uint16_t mem_mask);

/* Generates `n` output frames at the chip's native 85333.33Hz rate into
 * `out`, interleaved [FL, FR, RL, RR] s16 per frame (out must hold n*4
 * int16_t). This is MAME's sound_stream_update, unrolled to a plain call
 * so an offline replay can drive it sample-by-sample between register
 * writes exactly like the real audio stream update does between MCU
 * writes. */
void c352_generate(c352_t *c, int16_t *out, int n);

#endif /* PROPCYCL_C352_H */
