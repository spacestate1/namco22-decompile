/*
 * ending_rd.h -- 68K-address readers for code ported straight from the ROM.
 *
 * The ending and the final-stage scenery pass 68K ADDRESSES around: a scene
 * origin record may live in ROM (0x37AA4), in work RAM (0xE17294), or in the
 * caller's stack frame (-$18(a6)). The transpiled code dereferenced those as
 * host pointers. These helpers read and write them the way the 68K does:
 *
 *   ROM    (< 0x400000)        big-endian, vrd32s / vrd16s
 *   WRAM   (0xE00000..)        the _W[] model: whole 4-aligned slots for a
 *                              long, the right HALF of the slot for a word
 *
 * A 68K stack local is modelled as work RAM just below the machine's initial
 * stack pointer 0xE20000 (ROM vector 0). Nothing in the ROM addresses
 * 0xE18000..0xE1FFFF directly and nothing in the C tree touches it, so a
 * frame there is as private as the stack was on the machine. EFRAME(n) names
 * the n-th 0x40-byte frame; each ported function that needs a local record
 * other code reads through a pointer uses its own frame number.
 */
#ifndef ENDING_RD_H
#define ENDING_RD_H

#include "propcycl.h"

#define EWRAM_BASE   0xE00000u
#define EFRAME(n)    (0xE1FC00u + (unsigned)(n) * 0x40u)

static inline int e_is_wram(uint32_t a) { return a >= 0xE00000u && a < 0xE40000u; }

static inline int32_t e_rd32(uint32_t a)
{
    if (a < 0x400000u) return vrd32s(a);
    if (e_is_wram(a)) return (int32_t)_W[a - EWRAM_BASE];
    return 0;
}

static inline void e_wr32(uint32_t a, int32_t v)
{
    if (e_is_wram(a)) _W[a - EWRAM_BASE] = (intptr_t)v;
}

static inline int16_t e_rd16(uint32_t a)
{
    if (a < 0x400000u) return vrd16s(a);
    if (e_is_wram(a)) {
        uint32_t o = a - EWRAM_BASE;
        return (o & 2) ? W_LO16(o & ~3u) : W_HI16(o & ~3u);
    }
    return 0;
}

static inline void e_wr16(uint32_t a, int v)
{
    if (e_is_wram(a)) {
        uint32_t o = a - EWRAM_BASE, s = o & ~3u;
        if (o & 2) _W[s] = (intptr_t)(int32_t)(((uint32_t)_W[s] & 0xffff0000u) | (uint16_t)v);
        else       _W[s] = (intptr_t)(int32_t)(((uint32_t)(uint16_t)v << 16) | ((uint32_t)_W[s] & 0xffffu));
    }
}

/* The DSP command cursor 0xE00CA4 holds a HOST pointer into g_sys.dspram. */
static inline int32_t *e_cur(void)          { return (int32_t *)_W[0x0CA4]; }
static inline void     e_setcur(int32_t *p) { _W[0x0CA4] = (intptr_t)p; }

/* The trig table: interleaved [sin, cos] on a 4-byte stride (CLAUDE.md
 * "Trig table"). A `move.l (a),(dst)+` off 0x20B002+x carries sin(x) in its
 * low half and `move.l $2(a)` carries cos(x); the master uses the low 16. */
static inline int32_t e_sin(uint32_t a) { return vrd16s(0x20B004u + (a & 0xfffcu)); }
static inline int32_t e_cos(uint32_t a) { return vrd16s(0x20B006u + (a & 0xfffcu)); }

/* ROM 0x0146B2 (fixed_point_mul_32x32 in the C, which returns >> 16 -- row
 * 160): `muls.l` 32x32 -> 64, doubled with add/addx, bits 47..16 kept, i.e.
 * the product >> 15. */
static inline int32_t e_mul15(int32_t a, int32_t b)
{
    return (int32_t)(((int64_t)a * (int64_t)b) >> 15);
}

#endif /* ENDING_RD_H */
