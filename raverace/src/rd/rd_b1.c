/*
 * rd_b1.c -- Phase B batch b1: readable replacements for functions in
 * 0x4000..0x113F0 (include/rd.h). Written from the 68K instructions
 * (tools/rd_show.py), Ghidra's C used only as a naming aid; every entry is
 * proven against its lifted twin by RR_RD=check.
 *
 * Conventions (see rd_funcs.c): A6 is always 0x10008000, so (d16,A6) operands
 * are fixed WRAM addresses. Every register a caller could read is left exactly
 * as the 68K leaves it (move.w changes only the low word, etc.). Instructions
 * are charge()d in execution order before every poll point (a taken backward
 * branch, each bsr/jsr); entries here charge everything themselves, so the
 * table's `cost` is 0 unless noted.
 */
#include "rd.h"
#include "rr_lifted.h"

/* ---- small helpers for 68K register arithmetic ---- */
static inline uint32_t sx16(uint32_t v)                { return (uint32_t)(int32_t)(int16_t)v; }
static inline uint32_t sx8(uint32_t v)                 { return (uint32_t)(int32_t)(int8_t)v; }
static inline uint32_t w_lo(uint32_t reg, uint32_t v)  { return (reg & 0xFFFF0000u) | (v & 0xFFFFu); }
static inline uint32_t b_lo(uint32_t reg, uint32_t v)  { return (reg & 0xFFFFFF00u) | (v & 0xFFu); }
static inline uint32_t swap32(uint32_t x)              { return x << 16 | x >> 16; }
static inline uint32_t muls_w(uint32_t a, uint32_t b)  { return (uint32_t)((int32_t)(int16_t)a * (int32_t)(int16_t)b); }
/* byte / word stores with the value masked to the width (an I/O write is
 * logged with the value as passed) */
static inline void wr8(uint32_t a, uint32_t v)  { vwr8(a, v & 0xFFu); }
static inline void wr16(uint32_t a, uint32_t v) { vwr16(a, v & 0xFFFFu); }
/* an rts exactly as the 68K performs it, for a routine that is both CALLED
 * (bsr: the rts returns) and BRANCHED to after a `pea` (the rts is then a
 * computed jump to the pushed address): pop, then a matched return unwinds
 * to its call site, anything else is a jump (which polls first). Charge the
 * rts before calling this. */
static uint32_t rts_as_68k(void)
{
    uint32_t sp = a_reg(7), t = vrd32(sp);
    set_a(7, sp + 4);
    RS4(0x50, t);
    if (rr_return(t)) return RD_UNWIND;
    return RD_JMP(t);
}
/* movem.l <regs>,-(SP) / (SP)+ for D3..D6, as 5092 uses around its call */
static void push_longs(const uint32_t *v, int n)
{
    uint32_t sp = a_reg(7) - 4u * (uint32_t)n;
    for (int i = 0; i < n; i++) vwr32(sp + 4u * (uint32_t)i, v[i]);
    set_a(7, sp);
}

/* ---- WRAM (A6 = 0x10008000) ---- */
#define W(o) (0x10008000u + (uint32_t)(int32_t)(o))     /* (o,A6) */
#define G_DLIST_BASE     W(0xC42)   /* long: display-list base in polygon RAM (this buffer) */
#define G_DLIST_PTR      W(0xC46)   /* long: display-list write pointer                     */
#define A5_SINE_TABLE    0x6C400u   /* ROM: the sine table every angle lookup uses (A5)     */

/* ---- hardware ---- */
#define SHARED(o)        (0x60004000u + (uint32_t)(o))   /* shared RAM with the C74 sound/IO CPU */
#define POLY(o)          (0x70000000u + (uint32_t)(o))   /* polygon RAM (master DSP)             */
#define SERIAL_90000000  0x90000000u                     /* byte port bit-banged by 5924 (purpose not yet identified) */
#define MIXER(o)         (0x90020000u + (uint32_t)(o))
#define CZRAM(o)         (0x90010000u + (uint32_t)(o))

/* =====================================================================
 * Boot-time setup
 * ===================================================================== */

/* FUN_00004f22: master-DSP start-up: A5 = the sine table; the control words
 * at polygon RAM 0x80..0x94 and 0x00..0x50; clear the 32 KB list area at
 * 0x70010000; FUN_00005038; then build the two display-list buffers
 * (0x70010000 and 0x70018000, each written from +0x400) with FUN_00005064;
 * FUN_00004fe6 last. */
static uint32_t rd_dsp_startup(void)
{
    uint32_t r;
    set_a(5, A5_SINE_TABLE);
    vwr32(POLY(0x80), 1);     vwr32(POLY(0x84), 0x780);
    vwr32(POLY(0x88), 0x200); vwr32(POLY(0x8C), 0x100);
    vwr32(POLY(0x90), 0);     vwr32(POLY(0x94), 0);
    static const uint32_t head[21] = { 0, 0, 1, 0, 0, 1, 0x22, 0x140 };   /* the rest 0 */
    for (int i = 0; i < 21; i++) vwr32(POLY(4 * i), head[i]);
    set_a(0, POLY(4 * 21));
    set_a(2, 0x70010000u);
    set_d(0, 0);
    charge(33);                                   /* up to the clear loop */
    set_d16(1, 0x1FFF);
    for (int n = 0x1FFF; n >= 0; n--) {
        vwr32(0x70010000u + (uint32_t)(0x1FFF - n) * 4, 0);
        set_a(2, 0x70010000u + (uint32_t)(0x2000 - n) * 4);
        set_d16(1, (uint16_t)(n - 1));
        charge(2);                                /* move.l, dbf */
        if (n) poll();
    }
    charge(1);
    if ((r = rd_call(L_5038, 0x004FB0))) return r;
    vwr32(G_DLIST_BASE, 0x70010000u);
    vwr32(G_DLIST_PTR, 0x70010400u);
    charge(3);
    if ((r = rd_call(L_5064, 0x004FC4))) return r;
    vwr32(G_DLIST_BASE, 0x70018000u);
    vwr32(G_DLIST_PTR, 0x70018400u);
    charge(3);
    if ((r = rd_call(L_5064, 0x004FD8))) return r;
    charge(1);
    if ((r = rd_call(L_4FE6, 0x004FDC))) return r;
    charge(1);                                    /* rts */
    return RD_RTS;
}

/* FUN_00005064: fill one display-list buffer: the eight 0x80-byte object
 * blocks (FUN_00005092, block n at offset n*0x80, n = 7..0), then eight
 * headers 0x8002, n, -1 (n = 7..0) at the write pointer 0xC46, which is left
 * pointing at the last -1. D4w ends 0xFFFF, D0 = -1, D1w = 0xFFFF. */
static uint32_t rd_fill_display_buffer(void)
{
    uint32_t r;
    set_d16(4, 7);
    charge(1);
    for (;;) {
        uint16_t n = (uint16_t)d_reg(4);
        set_d16(1, (uint16_t)(n << 7));
        charge(3);                                /* move, lsl, bsr */
        if ((r = rd_call(L_5092, 0x005070))) return r;
        uint16_t d4 = (uint16_t)(d_reg(4) - 1);
        set_d16(4, d4);
        charge(1);                                /* dbf */
        if (d4 == 0xFFFF) break;
        poll();
    }
    uint32_t p = vrd32(G_DLIST_PTR);
    set_d(0, 0xFFFFFFFFu);
    charge(3);                                    /* movea, moveq, moveq */
    for (int n = 7; n >= 0; n--) {
        vwr32(p, 0x8002); vwr32(p + 4, (uint32_t)n); vwr32(p + 8, 0xFFFFFFFFu);
        p += 12;
        set_a(2, p); set_d(1, (uint32_t)(n - 1) & 0xFFFF);
        charge(4);
        if (n) poll();
    }
    p -= 4;
    vwr32(G_DLIST_PTR, p);
    set_a(2, p);
    set_d(1, 0x0000FFFFu);
    charge(3);                                    /* subq, move, rts */
    return RD_RTS;
}

/* One sine-table lookup as the 68K writes it: D0w = angle & ~1,
 * D1w = -table[D0w], D0w += 0x4000, D2w = table[D0w] (A5 = the table) --
 * i.e. D1w = -sin, D2w = cos. 6 instructions. */
static void a5_angle(uint32_t angle_addr, uint32_t *d0, uint32_t *d1, uint32_t *d2)
{
    uint32_t a5 = a_reg(5);
    uint16_t a = (uint16_t)(vrd16(angle_addr) & 0xFFFEu);
    uint16_t s = (uint16_t)vrd16(a5 + sx16(a));
    a = (uint16_t)(a + 0x4000);
    uint16_t c = (uint16_t)vrd16(a5 + sx16(a));
    *d0 = w_lo(*d0, a);
    *d1 = w_lo(*d1, (uint16_t)-s);
    *d2 = w_lo(*d2, c);
}

/* FUN_00005092: one object block of the display list. The block is the WRAM
 * record at 0x10008000 + D1w; its flag word (kept in D7w) selects work:
 *   bit 0: -sin/cos of the three angles at +0x5A/+0x5C/+0x5E into +2..+0xC
 *   bit 1: FUN_00005182 (a composed rotation into +0x14..+0x18), D3-D6 saved
 *   bit 3: -sin/cos of the angles at +0x66/+0x68/+0x6A into +0x2E..+0x38
 * then the first 32 words are copied, each widened to a long, to the display
 * list base (0xC42) + D1w. A0/A1 end past the copied data. */
static uint32_t rd_object_block(void)
{
    uint32_t r;
    int16_t  off = (int16_t)d_reg(1);
    uint32_t a1 = vrd32(G_DLIST_BASE) + (uint32_t)off;
    uint32_t a0 = W(off);
    uint32_t d0 = d_reg(0), d1 = d_reg(1), d2 = d_reg(2), d7 = d_reg(7);
    uint16_t flags = (uint16_t)vrd16(a0);
    d0 = w_lo(d0, flags);
    d7 = w_lo(d7, flags);
    charge(7);
    if (flags & 1) {
        static const uint8_t in[3] = { 0x5A, 0x5C, 0x5E };
        for (int k = 0; k < 3; k++) {
            a5_angle(a0 + in[k], &d0, &d1, &d2);
            wr16(a0 + 2 + 4u * (uint32_t)k, d1);
            wr16(a0 + 4 + 4u * (uint32_t)k, d2);
        }
        charge(24);
    }
    charge(2);                                    /* btst, beq */
    if (flags & 2) {
        uint32_t save[4] = { d_reg(3), d_reg(4), d_reg(5), d_reg(6) };
        push_longs(save, 4);
        set_d(0, d0); set_d(1, d1); set_d(2, d2); set_d(7, d7);
        set_a(0, a0); set_a(1, a1);
        charge(2);                                /* movem, bsr */
        if ((r = rd_call(L_5182, 0x00510E))) return r;
        uint32_t sp = a_reg(7);
        for (int i = 0; i < 4; i++) set_d(3 + i, vrd32(sp + 4u * (uint32_t)i));
        set_a(7, sp + 16);
        d0 = d_reg(0); d1 = d_reg(1); d2 = d_reg(2); d7 = d_reg(7);
        a0 = a_reg(0); a1 = a_reg(1);
        charge(1);                                /* movem */
    }
    charge(2);
    if (flags & 8) {
        static const uint8_t in[3] = { 0x66, 0x68, 0x6A };
        for (int k = 0; k < 3; k++) {
            a5_angle(a0 + in[k], &d0, &d1, &d2);
            wr16(a0 + 0x2E + 4u * (uint32_t)k, d1);
            wr16(a0 + 0x30 + 4u * (uint32_t)k, d2);
        }
        charge(24);
    }
    d1 = 0;
    charge(2);                                    /* moveq, move */
    set_d(2, d2); set_d(7, d7);
    for (int n = 0x1F; n >= 0; n--) {
        d1 = vrd16(a0); a0 += 2;
        vwr32(a1, d1); a1 += 4;
        set_d(0, w_lo(d0, (uint32_t)(n - 1))); set_d(1, d1); set_a(0, a0); set_a(1, a1);
        charge(3);
        if (n) poll();
    }
    charge(1);                                    /* rts */
    return RD_RTS;
}

static void compose_rotation(uint32_t a0, uint32_t d1, uint32_t *d2, uint32_t *d3, uint32_t *d4, uint32_t *d5, uint32_t *d6);

/* FUN_00005182: the composed rotation of the object record at A0 (see
 * compose_rotation) from the -sin/cos of its angles +0x62 (D3/D4), +0x64
 * (D5/D6) and +0x60 (D1/D2); registers end as the 68K leaves them. */
static uint32_t rd_compose_rotation(void)
{
    uint32_t a0 = a_reg(0);
    uint32_t d0 = d_reg(0), d1 = d_reg(1), d2 = d_reg(2), d3 = d_reg(3);
    uint32_t d4 = d_reg(4), d5 = d_reg(5), d6 = d_reg(6);
    a5_angle(a0 + 0x62, &d0, &d1, &d2);
    d3 = w_lo(d3, d1); d4 = w_lo(d4, d2);
    a5_angle(a0 + 0x64, &d0, &d1, &d2);
    d5 = w_lo(d5, d1); d6 = w_lo(d6, d2);
    a5_angle(a0 + 0x60, &d0, &d1, &d2);
    compose_rotation(a0, d1, &d2, &d3, &d4, &d5, &d6);
    set_d(0, d0); set_d(1, d1); set_d(2, d2); set_d(3, d3);
    set_d(4, d4); set_d(5, d5); set_d(6, d6);
    charge(47);
    return RD_RTS;
}

/* FUN_000055ae: three 4-byte mixer entries at 0x90020100/0x180/0x200:
 * (FF, F3, 50, 00), (FF, 71, 37, 00), (FF, 10, 00, 00), written interleaved.
 * A0-A2 end past them. */
static uint32_t rd_mixer_entries(void)
{
    static const uint8_t v[4][3] = { { 0xFF, 0xFF, 0xFF }, { 0xF3, 0x71, 0x10 }, { 0x50, 0x37, 0x00 }, { 0, 0, 0 } };
    for (uint32_t i = 0; i < 4; i++) {
        wr8(MIXER(0x100) + i, v[i][0]);
        wr8(MIXER(0x180) + i, v[i][1]);
        wr8(MIXER(0x200) + i, v[i][2]);
    }
    set_a(0, MIXER(0x104)); set_a(1, MIXER(0x184)); set_a(2, MIXER(0x204));
    charge(16);
    return RD_RTS;
}

/* FUN_000055f2: clear the 32 KB depth-cue RAM at 0x90010000, then build its
 * two ramps (FUN_00005650) and fill 0x90012000 (FUN_00005742). */
static uint32_t rd_czram_init(void)
{
    uint32_t r;
    charge(3);                                    /* lea, move, moveq */
    set_d(1, 0);
    set_d16(0, 0x1FFF);
    for (int n = 0x1FFF; n >= 0; n--) {
        vwr32(CZRAM((uint32_t)(0x1FFF - n) * 4), 0);
        set_a(0, CZRAM((uint32_t)(0x2000 - n) * 4));
        set_d16(0, (uint16_t)(n - 1));
        charge(2);
        if (n) poll();
    }
    charge(1);
    if ((r = rd_call(L_5650, 0x005608))) return r;
    charge(1);
    if ((r = rd_call(L_5742, 0x00560C))) return r;
    charge(1);
    return RD_RTS;
}

/* FUN_00005650: two 0x1100-byte ramps in depth-cue RAM, at 0x90010000
 * (step 0x1500/0x10000 per byte) and 0x90014000 (step 0x9500/0x10000),
 * each rising from 0 and clamped at 0xFF. D0 holds the 16.16 value swapped
 * (integer part in the low word), as the 68K keeps it. */
static uint32_t rd_czram_ramps(void)
{
    static const uint32_t base[2] = { CZRAM(0), CZRAM(0x4000) };
    static const uint32_t step[2] = { 0x1500, 0x9500 };
    uint32_t d0 = 0, a1 = 0;
    charge(1);                                    /* lea A5 */
    set_a(5, A5_SINE_TABLE);
    for (int k = 0; k < 2; k++) {
        a1 = base[k]; d0 = 0;
        set_d(6, step[k]);
        charge(4);                                /* lea, moveq, move.l, move.w */
        for (int n = 0x10FF; n >= 0; n--) {
            wr8(a1++, d0);
            d0 = swap32(swap32(d0) + step[k]);
            charge(6);
            if ((d0 & 0xFFFF) >= 0xFF) { d0 = w_lo(d0, 0xFF); charge(1); }
            set_a(1, a1); set_d(0, d0); set_d16(7, (uint16_t)(n - 1));
            charge(1);                            /* dbf */
            if (n) poll();
        }
    }
    charge(1);                                    /* rts */
    return RD_RTS;
}

/* FUN_000057a4: video start-up: depth-cue RAM (FUN_000055f2), the mixer
 * registers 0x90020000..0x16, FUN_00005564, FUN_000055ae, then it runs on
 * into FUN_00005868 (no rts). */
static uint32_t rd_video_startup(void)
{
    static const uint8_t mix[0x17] = {
        0x4F, 0x03, 0x00, 0x00, 0x7F, 0x00, 0x00, 0x7F, 0x4D, 0x4D, 0x4D, 0x42,
        0x0C, 0x00, 0xC0, 0xC0, 0xC0, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00 };
    uint32_t r;
    charge(1);
    if ((r = rd_call(L_55F2, 0x0057A8))) return r;
    for (uint32_t i = 0; i < sizeof mix; i++) wr8(MIXER(i), mix[i]);
    charge(24);
    if ((r = rd_call(L_5564, 0x005864))) return r;
    charge(1);
    if ((r = rd_call(L_55AE, 0x005868))) return r;
    return 0x005868u;                             /* falls through */
}

/* FUN_00005868: load palette block 0x50000 (A3), count 0x7E (D5w) with
 * FUN_0000589e, then run on into FUN_00005876 (no rts). */
static uint32_t rd_palette_block_50000(void)
{
    uint32_t r;
    set_a(3, 0x50000u);
    set_d16(5, 0x7E);
    charge(3);
    if ((r = rd_call(L_589E, 0x005876))) return r;
    return 0x005876u;
}

/* FUN_00005700: A3 = 0x5732 and continue at 0x570A (inside FUN_00005706) */
static uint32_t rd_5700(void)
{
    set_a(3, 0x5732u);
    charge(2);
    return 0x00570Au;
}

/* FUN_00005924: bit-bang two ROM command lists into the byte port 0x90000000
 * (purpose not yet identified). Port bit 6 is held high. Each list starts with
 * a count-1; each entry is (value, nbits-1): for every bit, LSB first, write
 * 0x40|bit then 0x40|bit + strobe (+4 in the first list, +2 in the second). */
static uint32_t rd_serial_lists(void)
{
    static const uint32_t list[2] = { 0x5988u, 0x59CAu };
    static const uint8_t strobe[2] = { 4, 2 };
    uint32_t d0 = d_reg(0), d1 = d_reg(1), d2 = d_reg(2), d3 = d_reg(3);
    uint32_t a0 = 0;
    set_d8(4, 0x40);
    wr8(SERIAL_90000000, 0x40);
    charge(2);
    for (int k = 0; k < 2; k++) {
        a0 = list[k];
        d0 = w_lo(d0, vrd16(a0)); a0 += 2;
        charge(3);                                /* lea, nop, move */
        for (;;) {
            d1 = w_lo(d1, vrd16(a0)); a0 += 2;
            d2 = w_lo(d2, vrd16(a0)); a0 += 2;
            charge(2);
            for (;;) {
                d3 = b_lo(d3, (d1 & 1) | 0x40);
                wr8(SERIAL_90000000, d3);
                d3 = b_lo(d3, d3 + strobe[k]);
                wr8(SERIAL_90000000, d3);
                d1 = w_lo(d1, (uint32_t)((int16_t)d1 >> 1));
                d2 = w_lo(d2, d2 - 1);
                charge(8);
                if ((d2 & 0xFFFF) == 0xFFFF) break;
                set_a(0, a0); set_d(0, d0); set_d(1, d1); set_d(2, d2); set_d(3, d3);
                poll();
            }
            d0 = w_lo(d0, d0 - 1);
            charge(1);
            if ((d0 & 0xFFFF) == 0xFFFF) break;
            set_a(0, a0); set_d(0, d0); set_d(1, d1); set_d(2, d2); set_d(3, d3);
            poll();
        }
    }
    charge(1);                                    /* rts */
    set_a(0, a0);
    set_d(0, d0); set_d(1, d1); set_d(2, d2); set_d(3, d3);
    return RD_RTS;
}

/* =====================================================================
 * Per-frame input
 * ===================================================================== */

#define G_STEER_RANGE_2X W(0xDDE)       /* word: nonzero doubles the steering threshold */
#define G_STEER_CENTRE   W(-0x77E6)     /* word: centre values subtracted from the three */
#define G_GAS_ZERO       W(-0x77E4)     /*       analogue inputs (0x1000081A..0x81E)      */
#define G_BRAKE_ZERO     W(-0x77E2)
#define G_STEER          W(0xDD2)       /* word: steering relative to centre              */
#define G_GAS            W(0xDD4)
#define G_BRAKE          W(0xDD6)
#define G_MIRROR_2394    W(0x2394)      /* word: with 0xDA0 == 3, the stored steering is negated */
#define G_MODE_DA0       W(0xDA0)
#define G_BUTTONS        W(-0x77EA)     /* byte: this frame's digital-input bits          */
#define G_BUTTONS_PREV   W(-0x77E9)     /* byte: last frame's                             */
#define G_BUTTONS_EDGE   W(-0x77E8)     /* byte: newly pressed this frame                 */

/* FUN_000047d0: read the inputs the sound/IO CPU leaves in shared RAM:
 * steering, gas and brake relative to their zero points (stored at 0xDD2..0xDD6;
 * steering negated in the mirrored mode), and one byte of bits:
 *   0 steer >= +limit, 1 steer < -limit (limit 0x100, or 0x200 if 0xDDE is set),
 *   7 gas >= 0x200, 6 brake >= 0x200, 5/4/3/2 = switches 0x60004031 bits
 *   4/6/0/1 pressed (active low).
 * Then prev = current, current = the bits, edge = current & ~prev. */
static uint32_t rd_read_inputs(void)
{
    uint32_t d0 = d_reg(0), d1 = d_reg(1), d2 = d_reg(2);
    d2 = w_lo(d2, 0x100);
    charge(3);
    if (vrd16(G_STEER_RANGE_2X)) { d2 = w_lo(d2, 0x200); charge(1); }
    d1 = w_lo(d1, 0);
    d0 = w_lo(d0, vrd16(SHARED(0x32)) - vrd16(G_STEER_CENTRE));
    wr16(G_STEER, d0);
    charge(6);
    if (vrd16(G_MIRROR_2394)) {
        charge(2);
        if (vrd16(G_MODE_DA0) == 3) { wr16(G_STEER, (uint32_t)-(int32_t)vrd16(G_STEER)); charge(1); }
    }
    charge(2);
    if ((int16_t)d0 >= (int16_t)d2) { d1 |= 1; charge(1); }
    d2 = w_lo(d2, (uint32_t)-(int32_t)d2);
    charge(3);
    if ((int16_t)d0 < (int16_t)d2) { d1 |= 2; charge(1); }
    d0 = w_lo(d0, vrd16(SHARED(0x34)) - vrd16(G_GAS_ZERO));
    wr16(G_GAS, d0);
    charge(5);
    if ((int16_t)d0 >= 0x200) { d1 |= 0x80; charge(1); }
    d0 = w_lo(d0, vrd16(SHARED(0x36)) - vrd16(G_BRAKE_ZERO));
    wr16(G_BRAKE, d0);
    charge(5);
    if ((int16_t)d0 >= 0x200) { d1 |= 0x40; charge(1); }
    static const uint8_t sw_bit[4] = { 4, 6, 0, 1 }, out_bit[4] = { 5, 4, 3, 2 };
    for (int k = 0; k < 4; k++) {
        charge(2);
        if (!(vrd8(SHARED(0x31)) & (1u << sw_bit[k]))) { d1 |= 1u << out_bit[k]; charge(1); }
    }
    wr8(G_BUTTONS_PREV, vrd8(G_BUTTONS));
    wr8(G_BUTTONS, d1);
    d0 = b_lo(d0, vrd8(G_BUTTONS));
    d1 = b_lo(d1, vrd8(G_BUTTONS_PREV));
    d0 = b_lo(d0, d0 ^ d1);
    d0 = b_lo(d0, d0 & vrd8(G_BUTTONS));
    wr8(G_BUTTONS_EDGE, d0);
    charge(8);
    set_d(0, d0); set_d(1, d1); set_d(2, d2);
    return RD_RTS;
}


/* =====================================================================
 * Text, number formatting, angles
 * ===================================================================== */

#define G_TEXT_REMAP     W(0xC60)   /* word: nonzero = remap punctuation into the font's 0x40.. range */
#define G_TEXT_ATTR      W(0xC5E)   /* word: added to every character (palette/attribute bits)        */
#define G_TEXT_COUNT     W(0xC56)   /* word: characters written                                       */

/* FUN_00005a2a: store character D0w at A0 (post-increment) as a text-layer
 * word. With remapping on (0xC60), nine punctuation codes are moved to their
 * glyphs and 0x40 added; then the attribute word 0xC5E is added. Counts the
 * character in 0xC56. */
static uint32_t rd_put_char(void)
{
    static const uint8_t from[9] = { 0x22, 0x27, 0x2E, 0x2D, 0x2F, 0x28, 0x29, 0x21, 0x2A };
    static const uint8_t to[9]   = { 0x3B, 0x3A, 0x3E, 0x3C, 0x3D, 0x5B, 0x5C, 0x5D, 0x5E };
    uint32_t d0 = d_reg(0);
    charge(2);
    if (vrd16(G_TEXT_REMAP)) {
        for (int k = 0; k < 9; k++) {
            charge(2);
            if ((d0 & 0xFFFF) == from[k]) { d0 = w_lo(d0, to[k]); charge(1); }
        }
        d0 = w_lo(d0, d0 + 0x40);
        charge(1);
    }
    d0 = w_lo(d0, d0 + vrd16(G_TEXT_ATTR));
    uint32_t a0 = a_reg(0);
    wr16(a0, d0);
    set_a(0, a0 + 2);
    wr16(G_TEXT_COUNT, vrd16(G_TEXT_COUNT) + 1);
    set_d(0, d0);
    charge(4);
    return rts_as_68k();          /* reached by bsr, and by bra from the string
                                   * interpreter FUN_00005a0a, whose pea makes
                                   * this rts a jump back to 0x5A0A */
}

/* FUN_0000a98c: the angle of the vector (D1w, D2w), 0x10000 = a full turn,
 * from the arctangent table at ROM 0x74400 (indexed by min/max * 0x1000):
 * octant folding by the signs (D3w = 0x8000 when D1 < 0, D4w = the sign
 * product mask) and by which component is larger (then D0 = -1 selects
 * 0x4000 - atan). Equal magnitudes give 0x2000. Result in D0w.
 * divu.w as a real 68K: on overflow D2 is left unchanged. */
static uint32_t rd_vector_angle(void)
{
    uint32_t d0 = d_reg(0), d1 = d_reg(1), d2 = d_reg(2);
    uint32_t d3 = 0, d4 = 0;
    d0 = w_lo(d0, 0x2000);
    charge(5);
    if (d1 & 0x8000) { d1 = w_lo(d1, (uint32_t)-(int32_t)d1); d3 = 0x8000; d4 = w_lo(d4, ~d4); charge(3); }
    charge(2);
    if (d2 & 0x8000) { d2 = w_lo(d2, (uint32_t)-(int32_t)d2); d4 = w_lo(d4, ~d4); charge(2); }
    charge(2);                                    /* cmp, beq */
    if ((uint16_t)d1 != (uint16_t)d2) {
        charge(1);                                /* bgt */
        if (!((int16_t)d1 > (int16_t)d2)) { uint32_t t = d1; d1 = d2; d2 = t; d0 = 0xFFFFFFFFu; charge(2); }
        d2 = (d2 & 0xFFFF) << 16;
        charge(4);                                /* swap, clr, tst, bne */
        if (!(d1 & 0xFFFF)) { d1 = 1; charge(1); }
        uint32_t q = d2 / (d1 & 0xFFFF), rem = d2 % (d1 & 0xFFFF);
        if (q <= 0xFFFF) d2 = rem << 16 | q;       /* divu.w overflow: D2 unchanged */
        d2 = w_lo(d2, ((d2 & 0xFFFF) >> 4) * 2);
        set_a(1, 0x74400u);
        d1 = w_lo(d1, vrd16(0x74400u + sx16(d2)));
        charge(7);
        if (d0 & 0x8000) { d1 = w_lo(d1, (uint32_t)-(int32_t)d1 + 0x4000); charge(2); }
        d0 = w_lo(d0, d1);
        charge(1);
    }
    d0 = w_lo(d0, d0 ^ d4);
    d0 = w_lo(d0, d0 - d4);
    d0 = w_lo(d0, d0 + d3);
    charge(4);
    set_d(0, d0); set_d(1, d1); set_d(2, d2); set_d(3, d3); set_d(4, d4);
    return RD_RTS;
}

/* FUN_0000ac4e: clear 0xC62/0xC64 and fill the three 16-byte descriptors at
 * 0xD68: (WRAM 0xC68, 0xC88, ROM 0xAADE, -), (0xCC8, 0xCD8, ROM 0xAAF4, -),
 * (0xD18, 0xD28, ROM 0xAB0A, -) -- the fourth long of each left as it is
 * (purpose not yet identified). */
static uint32_t rd_init_descriptors_d68(void)
{
    static const uint32_t v[3][3] = { { W(0xC68), W(0xC88), 0xAADEu },
                                      { W(0xCC8), W(0xCD8), 0xAAF4u },
                                      { W(0xD18), W(0xD28), 0xAB0Au } };
    wr16(W(0xC62), 0);
    wr16(W(0xC64), 0);
    for (uint32_t k = 0; k < 3; k++)
        for (uint32_t j = 0; j < 3; j++) vwr32(W(0xD68) + k * 16 + j * 4, v[k][j]);
    set_a(0, 0xAB0Au);
    set_a(1, W(0xD68) + 48);
    charge(25);
    return RD_RTS;
}

/* FUN_0000b4e4: copy a packed object description at A0 into the current
 * object block (0x10008000 + (0xC40) << 7): the flag word, words to
 * +0xE..+0x2C and +0x3A..+0x3E, longs to +0x42..+0x4A, words to +0x5A..+0x6A.
 * A0 ends past the source, A1 = the block, D0w = the block offset. */
static uint32_t rd_load_object_block(void)
{
    uint16_t off = (uint16_t)(vrd16(W(0xC40)) << 7);
    uint32_t a1 = W((int16_t)off), a0 = a_reg(0);
    wr16(a1, vrd16(a0)); a0 += 2;
    for (uint32_t d = 0xE; d <= 0x2C; d += 2) { wr16(a1 + d, vrd16(a0)); a0 += 2; }
    for (uint32_t d = 0x3A; d <= 0x3E; d += 2) { wr16(a1 + d, vrd16(a0)); a0 += 2; }
    for (uint32_t d = 0x42; d <= 0x4A; d += 4) { vwr32(a1 + d, vrd32(a0)); a0 += 4; }
    for (uint32_t d = 0x5A; d <= 0x6A; d += 2) { wr16(a1 + d, vrd16(a0)); a0 += 2; }
    set_a(0, a0); set_a(1, a1);
    set_d16(0, off);
    charge(36);
    return RD_RTS;
}

/* The composed rotation both 5182 and b56e compute: from (-sin, cos) of three
 * angles -- (s1, c1) in D1/D2, (s3, c3) in D3/D4, (s5, c5) in D5/D6 --
 * products in 1.15 (muls.w, doubled, swapped), stored at +0x14/+0x16/+0x18.
 * 24 instructions. */
static void compose_rotation(uint32_t a0, uint32_t d1, uint32_t *d2, uint32_t *d3, uint32_t *d4, uint32_t *d5, uint32_t *d6)
{
    *d3 = swap32(muls_w(*d3, *d2) * 2);
    *d4 = swap32(muls_w(*d4, *d2) * 2);
    *d2 = w_lo(*d2, *d3);
    *d2 = swap32(muls_w(*d2, *d5) * 2);
    *d3 = swap32(muls_w(*d3, *d6) * 2);
    *d5 = swap32(muls_w(*d5, d1) * 2);
    *d6 = swap32(muls_w(*d6, d1) * 2);
    *d3 = w_lo(*d3, *d3 + *d5);
    *d2 = w_lo(*d2, *d2 - *d6);
    wr16(a0 + 0x14, *d3);
    wr16(a0 + 0x16, *d2);
    wr16(a0 + 0x18, *d4);
}

/* FUN_0000b56e: the current object block (0xC40) into the display list --
 * the same work as FUN_00005092 with the rotation inline and bit 4 (not 3)
 * selecting the second angle set; the words are copied sign-extended. */
static uint32_t rd_current_object_block(void)
{
    uint32_t d0 = d_reg(0), d1 = d_reg(1), d2 = d_reg(2), d3 = d_reg(3), d4 = d_reg(4);
    uint32_t d5 = d_reg(5), d6 = d_reg(6), d7 = d_reg(7);
    d1 = w_lo(d1, vrd16(W(0xC40)) << 7);
    uint32_t a0 = W((int16_t)d1);
    uint32_t a1 = vrd32(G_DLIST_BASE) + sx16(d1);
    uint16_t flags = (uint16_t)vrd16(a0);
    d7 = w_lo(d7, flags);
    charge(8);
    if (flags & 1) {
        static const uint8_t in[3] = { 0x5A, 0x5C, 0x5E };
        for (int k = 0; k < 3; k++) {
            a5_angle(a0 + in[k], &d0, &d1, &d2);
            wr16(a0 + 2 + 4u * (uint32_t)k, d1);
            wr16(a0 + 4 + 4u * (uint32_t)k, d2);
        }
        charge(24);
    }
    charge(2);
    if (flags & 2) {
        uint32_t t0 = d0, t1 = d1, t2 = d2;
        a5_angle(a0 + 0x60, &d0, &d1, &d2);
        a5_angle(a0 + 0x62, &t0, &t1, &t2); d3 = w_lo(d3, t1); d4 = w_lo(d4, t2);
        a5_angle(a0 + 0x64, &t0, &t1, &t2); d5 = w_lo(d5, t1); d6 = w_lo(d6, t2);
        d0 = t0;
        compose_rotation(a0, d1, &d2, &d3, &d4, &d5, &d6);
        charge(42);
    }
    charge(2);
    if (flags & 0x10) {
        static const uint8_t in[3] = { 0x66, 0x68, 0x6A };
        for (int k = 0; k < 3; k++) {
            a5_angle(a0 + in[k], &d0, &d1, &d2);
            wr16(a0 + 0x2E + 4u * (uint32_t)k, d1);
            wr16(a0 + 0x30 + 4u * (uint32_t)k, d2);
        }
        charge(24);
    }
    charge(1);
    set_d(2, d2); set_d(3, d3); set_d(4, d4); set_d(5, d5); set_d(6, d6); set_d(7, d7);
    for (int n = 0x1F; n >= 0; n--) {
        d1 = sx16(vrd16(a0)); a0 += 2;
        vwr32(a1, d1); a1 += 4;
        set_d(0, w_lo(d0, (uint32_t)(n - 1))); set_d(1, d1); set_a(0, a0); set_a(1, a1);
        charge(4);
        if (n) poll();
    }
    charge(1);
    return RD_RTS;
}

/* FUN_0000b788 / FUN_0000b7cc: step the three mixer colour words at
 * 0x90020011/13/15 (odd addresses: byte-lane words) by -2 toward 0 or +2
 * toward 0x100; 0x233E is 1 while a fade is in progress and cleared when all
 * three have arrived (sum 0, or 0x300). */
#define G_FADE_BUSY W(0x233E)
static uint32_t fade_step(int up)
{
    uint32_t d[3] = { d_reg(0), d_reg(1), d_reg(2) };
    wr16(G_FADE_BUSY, 1);
    charge(2);
    for (int k = 0; k < 3; k++) {
        uint32_t a = MIXER(0x11 + 2 * k);
        uint16_t v = (uint16_t)vrd16(a);
        if (up) {
            v = (uint16_t)(v + 2);
            charge(4);
            if ((int16_t)v > 0x100) { v = 0x100; charge(1); }
        } else {
            v = (uint16_t)(v - 2);
            charge(3);
            if ((int16_t)v < 0) { v = 0; charge(1); }
        }
        wr16(a, v);
        d[k] = w_lo(d[k], v);
        charge(1);
    }
    uint16_t sum = (uint16_t)(d[0] + d[1] + d[2]);
    d[0] = w_lo(d[0], sum);
    charge(up ? 4 : 3);
    if (sum == (up ? 0x300 : 0)) { wr16(G_FADE_BUSY, 0); charge(1); }
    charge(1);
    set_a(0, MIXER(0));
    set_d(0, d[0]); set_d(1, d[1]); set_d(2, d[2]);
    return RD_RTS;
}
static uint32_t rd_fade_down(void) { return fade_step(0); }
static uint32_t rd_fade_up(void)   { return fade_step(1); }

/* =====================================================================
 * Coins / service counters, tilemap and CGRAM set-up
 * ===================================================================== */

#define G_SEL_INDEX_1160  W(0x1160)     /* word: index into the byte table at 0x11B5 (purpose not yet identified) */
#define G_MODE_1102       W(0x1102)     /* word: when 0 the byte at 0xC4D is used instead */
#define G_SW_SNAPSHOT     W(0x2034)     /* word: the selected byte (and the high byte of 0x1160) */
#define G_SW_BIT5         W(0x2032)     /* word: bit 5 of it */
#define G_SYS_EDGE        W(0x2030)     /* byte: newly pressed system switches (active low at 0x60004030) */
#define G_SYS_LEVEL       W(0x2031)     /* byte: system switches, inverted */
#define G_SERVICE_COUNT   W(0x2036)     /* byte: counts presses of switch bit 3 */
#define G_SERVICE_SEEN    W(0x2037)     /* byte: the count last credited */
#define G_COIN_ENABLE     W(0x2028)     /* word: coins are counted only when >= 2 */
#define G_CREDITS         W(0x202A)     /* word: credits, clamped at 0xFF */
#define G_COIN1_SEEN      W(0x202E)     /* byte: last coin-1 counter value seen */
#define G_COIN2_SEEN      W(0x202F)     /* byte */
#define G_COIN1_PENDING   W(0x203A)     /* word: coin-meter pulses still to send (coin 1) */
#define G_COIN2_PENDING   W(0x203C)     /* word: (coin 2) */
#define G_METER_PHASE     W(0x2038)     /* word: 0..7, a meter pulse every 8 frames */
#define COIN1_VALUE       0x10001041u   /* byte: credits per coin 1 (EEPROM settings) */
#define COIN2_VALUE       0x10001042u   /* byte: credits per coin 2 */
#define SND_CMD_5010      SHARED(0x1010) /* word: 0x40CB written on every coin (the coin sound) */

/* FUN_0000c150: per-frame switches and coins. Latches a byte from a table (or
 * 0xC4D) into 0x2034/0x2032, edge-detects the system switches at 0x60004030
 * (a press of bit 3 counts in 0x2036), and -- when coins are enabled -- turns
 * the coin counters at 0x6000403A/B and the service count into credits
 * (0x202A, clamped at 0xFF; coin values from the settings at 0x10001041/2),
 * with the coin sound 0x40CB for each; every 8th frame one pending coin-meter
 * pulse is sent as bit 1 of the word 0x60004020. */
static uint32_t rd_coins_and_switches(void)
{
    uint32_t d0 = d_reg(0), d1 = d_reg(1), d5 = d_reg(5);
    d0 = w_lo(d0, vrd16(G_SEL_INDEX_1160));
    d0 = b_lo(d0, vrd8(W(0x11B5) + sx16(d0)));
    charge(4);
    if (!vrd16(G_MODE_1102)) { d0 = b_lo(d0, vrd8(W(0xC4D))); charge(1); }
    wr16(G_SW_SNAPSHOT, d0);
    d0 = b_lo(d0, (d0 & 0xFF) >> 5);
    d0 = w_lo(d0, d0 & 1);
    wr16(G_SW_BIT5, d0);
    d0 = b_lo(d0, ~vrd8(SHARED(0x30)));
    d1 = b_lo(d1, vrd8(G_SYS_LEVEL) ^ d0);
    d1 = b_lo(d1, d1 & d0);
    wr8(G_SYS_EDGE, d1);
    wr8(G_SYS_LEVEL, d0);
    charge(13);
    if (d1 & 8) { wr8(G_SERVICE_COUNT, vrd8(G_SERVICE_COUNT) + 1); charge(1); }
    charge(2);                                    /* cmpi, bge */
    if ((int16_t)vrd16(G_COIN_ENABLE) < 2) {
        charge(1);
        set_d(0, d0); set_d(1, d1);
        return RD_RTS;
    }
    d0 = 0;
    d5 = w_lo(d5, vrd16(G_CREDITS));
    static const uint32_t ctr[2] = { SHARED(0x3A), SHARED(0x3B) }, seen[2] = { G_COIN1_SEEN, G_COIN2_SEEN };
    static const uint32_t pend[2] = { G_COIN1_PENDING, G_COIN2_PENDING }, val[2] = { COIN1_VALUE, COIN2_VALUE };
    charge(2);                                    /* moveq, move */
    for (int k = 0; k < 2; k++) {
        d0 = b_lo(d0, vrd8(ctr[k]) - vrd8(seen[k]));
        charge(3);                                /* move.b, sub.b, beq */
        if (d0 & 0xFF) {
            wr16(SND_CMD_5010, 0x40CB);
            wr16(pend[k], vrd16(pend[k]) + (d0 & 0xFFFF));
            d1 = vrd8(val[k]);
            d1 = muls_w(d1, d0);
            d5 = w_lo(d5, d5 + d1);
            wr8(seen[k], vrd8(seen[k]) + d0);
            charge(7);
        }
    }
    d0 = b_lo(d0, vrd8(G_SERVICE_COUNT) - vrd8(G_SERVICE_SEEN));
    charge(3);
    if (d0 & 0xFF) {
        wr16(SND_CMD_5010, 0x40CB);
        d5 = w_lo(d5, d5 + d0);
        wr8(G_SERVICE_SEEN, vrd8(G_SERVICE_COUNT));
        charge(3);
    }
    charge(2);
    if ((int16_t)d5 > 0xFF) { d5 = w_lo(d5, 0xFF); charge(1); }
    wr16(G_CREDITS, d5);
    d0 = w_lo(d0, vrd16(SHARED(0x20)));
    wr16(G_METER_PHASE, (vrd16(G_METER_PHASE) + 1) & 7);
    charge(5);
    if (!(vrd16(G_METER_PHASE) & 0xFFFF)) {
        d0 &= ~2u;
        charge(3);                                /* bclr, tst, beq */
        if (vrd16(G_COIN1_PENDING)) {
            wr16(G_COIN1_PENDING, vrd16(G_COIN1_PENDING) - 1);
            d0 |= 2;
            charge(3);
        } else {
            charge(2);
            if (vrd16(G_COIN2_PENDING)) {
                wr16(G_COIN2_PENDING, vrd16(G_COIN2_PENDING) - 1);
                d0 |= 2;
                charge(2);
            }
        }
    }
    wr16(SHARED(0x20), d0);
    charge(2);
    set_d(0, d0); set_d(1, d1); set_d(5, d5);
    return RD_RTS;
}

#define TEXT_LAYER   0x9009E000u   /* tilemap (text layer), 0x1000 words */
#define TILEMAP_ATTR 0x900A0000u
#define G_CG_BANK    W(0xDE2)      /* word: selects one of four CGRAM/palette sets (& 3) */

/* FUN_0000c606: text-layer start-up: tilemap attributes (0x35C, 0, 0x6E, then
 * FUN_0000c656, then 0, 0x1FF), fill the text layer with spaces (0x20), load
 * the fixed CGRAM (FUN_0000c6fe) and CGRAM set 0 (FUN_0000c69a). */
static uint32_t rd_text_layer_init(void)
{
    uint32_t r;
    wr16(TILEMAP_ATTR + 0, 0x35C);
    wr16(TILEMAP_ATTR + 2, 0);
    wr16(TILEMAP_ATTR + 4, 0x6E);
    charge(4);
    if ((r = rd_call(L_C656, 0x00C622))) return r;
    wr16(TILEMAP_ATTR + 6, 0);
    wr16(TILEMAP_ATTR + 8, 0x1FF);
    set_d(1, 0x20);
    charge(5);
    for (int n = 0xFFF; n >= 0; n--) {
        wr16(TEXT_LAYER + (uint32_t)(0xFFF - n) * 2, 0x20);
        set_a(0, TEXT_LAYER + (uint32_t)(0x1000 - n) * 2);
        set_d(0, (uint32_t)(n - 1) & 0xFFFF);
        charge(2);
        if (n) poll();
    }
    charge(1);
    if ((r = rd_call(L_C6FE, 0x00C64A))) return r;
    wr16(G_CG_BANK, 0);
    charge(2);
    if ((r = rd_call(L_C69A, 0x00C654))) return r;
    charge(1);
    return RD_RTS;
}

/* The copy loops below keep the 68K registers themselves up to date at every
 * poll point: an interrupt taken there pushes them on the stack (WRAM). */

/* copy n*16 8-byte cells from ROM A0 to CGRAM A1 (each long written twice, as
 * the 68K does): D0w counts the characters, D1w the cells, D2/D3 the pair */
static void cgram_cells(uint32_t a0, uint32_t a1, int n)
{
    set_d16(0, (uint16_t)(n - 1));
    for (int o = n - 1; o >= 0; o--) {
        charge(1);                                /* move.w #$f,D1w */
        for (int i = 15; i >= 0; i--) {
            uint32_t d2 = vrd32(a0), d3 = vrd32(a0 + 4);
            a0 += 8;
            vwr32(a1, d2); vwr32(a1, d2); a1 += 4;
            vwr32(a1, d3); vwr32(a1, d3); a1 += 4;
            set_d(2, d2); set_d(3, d3); set_a(0, a0); set_a(1, a1);
            set_d16(1, (uint16_t)(i - 1));
            charge(7);
            if (i) poll();
        }
        set_d16(0, (uint16_t)(o - 1));
        charge(1);                                /* outer dbf */
        if (o) poll();
    }
}
/* copy 128 RGB triples from ROM A0 to the palette planes A1/A2/A3 (D0w counts) */
static void palette_triples(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3)
{
    for (int n = 0x7F; n >= 0; n--) {
        wr8(a1++, vrd8(a0++));
        wr8(a2++, vrd8(a0++));
        wr8(a3++, vrd8(a0++));
        set_a(0, a0); set_a(1, a1); set_a(2, a2); set_a(3, a3);
        set_d16(0, (uint16_t)(n - 1));
        charge(4);
        if (n) poll();
    }
}
#define PAL_R_TEXT 0x9002FF00u      /* the text layer's 128 colours in the three planes */
#define PAL_G_TEXT 0x90037F00u
#define PAL_B_TEXT 0x9003FF00u

/* FUN_0000c69a: load CGRAM set (0xDE2 & 3): 0x300 characters from ROM
 * 0x1A0000 + set*0x18000 to CGRAM 0x90086000, then its 128 colours from
 * ROM 0x199A00 + set*0x180 into the text palette. */
static uint32_t rd_load_cgram_set(void)
{
    uint32_t set = vrd16(G_CG_BANK) & 3;
    uint32_t d4 = set * 0x180, d0 = d4 << 8;
    set_d(4, d4); set_d(0, d0);
    charge(9);
    cgram_cells(0x1A0000u + d0, 0x90086000u, 0x300);
    charge(6);
    palette_triples(0x199A00u + d4, PAL_R_TEXT, PAL_G_TEXT, PAL_B_TEXT);
    charge(1);
    return RD_RTS;
}

/* FUN_0000c6fe: load the fixed CGRAM: 0xC0 characters from ROM 0x19A000 to
 * CGRAM 0x90080000 and 128 colours from ROM 0x199880 into the text palette
 * (at +0x80). */
static uint32_t rd_load_cgram_fixed(void)
{
    charge(3);
    cgram_cells(0x19A000u, 0x90080000u, 0xC0);
    charge(5);
    palette_triples(0x199880u, PAL_R_TEXT + 0x80, PAL_G_TEXT + 0x80, PAL_B_TEXT + 0x80);
    charge(1);
    return RD_RTS;
}

/* FUN_0000c74e: reload the text palette only: set (0xDE2 & 3)'s 128 colours
 * from ROM 0x199A00 + set*0x180, then the fixed 128 from ROM 0x199880. */
static uint32_t rd_load_text_palette(void)
{
    uint32_t d0 = (vrd16(G_CG_BANK) & 3) * 0x180;
    set_d(0, d0);
    charge(9);
    palette_triples(0x199A00u + d0, PAL_R_TEXT, PAL_G_TEXT, PAL_B_TEXT);
    charge(2);
    palette_triples(0x199880u, PAL_R_TEXT + 0x80, PAL_G_TEXT + 0x80, PAL_B_TEXT + 0x80);
    charge(1);
    return RD_RTS;
}

/* =====================================================================
 * Text-layer blocks (the HUD and menu text)
 * ===================================================================== */

/* FUN_0000dbc8: draw text-layer block D0w. A block is 16 bytes in a ROM
 * table -- one of four tables by the CGRAM set (0xDE2 & 3), or the table at
 * 0xC7A8 when D0w is negative (block -D0w):
 *   +0 type (& 3), +2 first character, +4 width-1, +6 height-1,
 *   +8 row skip (bytes), +0xA position (byte offset in the text layer),
 *   +0xC digit-table index.
 * Type 0 draws nothing; type 1 fills the rectangle with consecutive
 * characters; type 2 first adds entry (D1w & 0xF) of the ROM digit table
 * (0xDC9E + index*4) to the character -- a digit; type 3 the same but at
 * the position D3w & 0x1FFF instead of +0xA. The type dispatches through a
 * jump table in the code (0xDBF6). */
static uint32_t rd_draw_text_block(void)
{
    uint32_t d0 = d_reg(0), d1 = d_reg(1), d2 = d_reg(2), d3 = d_reg(3), a0, a1 = a_reg(1);
    charge(2);                                    /* tst, bpl */
    if (d0 & 0x8000) {
        a0 = 0xC7A8u;
        d0 = w_lo(d0, (uint32_t)-(int32_t)d0);
        charge(3);
    } else {
        d2 = w_lo(d2, vrd16(G_CG_BANK) & 3);
        a0 = vrd32(0xC798u + sx16(d2) * 4);
        charge(3);
    }
    d0 = w_lo(d0, d0 << 4);
    a0 += sx16(d0);
    d0 = w_lo(d0, vrd16(a0) & 3);
    d0 = w_lo(d0, vrd16(0xDBF6u + sx16(d0) * 2));
    charge(7);                                    /* lsl, adda, move, andi, move, nop, jmp */
    set_d(0, d0); set_d(1, d1); set_d(2, d2); set_d(3, d3); set_a(0, a0); set_a(1, a1); poll();                                       /* the jmp polls */
    uint32_t type = 0xDBF6u + sx16(d0);
    if (type == 0xDBFEu) {                        /* type 0 */
        charge(1);
        set_d(0, d0); set_d(2, d2); set_a(0, a0);
        return RD_RTS;
    }
    if (type == 0xDC00u) {                        /* type 1 */
        a1 = TEXT_LAYER + sx16(vrd16(a0 + 0xA));
        d2 = w_lo(d2, vrd16(a0 + 2));
        d1 = w_lo(d1, vrd16(a0 + 6));
        charge(4);
    } else {                                      /* types 2 and 3 */
        d2 = w_lo(d2, vrd16(a0 + 2));
        d0 = w_lo(d0, vrd16(a0 + 0xC));
        a1 = vrd32(0xDC9Eu + sx16(d0) * 4);
        d1 = w_lo(d1, d1 & 0xF);
        d2 = w_lo(d2, d2 + vrd16(a1 + sx16(d1) * 2));
        d1 = w_lo(d1, vrd16(a0 + 6));
        if (type == 0xDC28u) {
            a1 = TEXT_LAYER + sx16(vrd16(a0 + 0xA));
            charge(9);
        } else {
            d3 = w_lo(d3, d3 & 0x1FFF);
            a1 = TEXT_LAYER + sx16(d3);
            charge(10);
        }
    }
    for (;;) {                                    /* rows */
        d0 = w_lo(d0, vrd16(a0 + 4));
        charge(1);
        for (;;) {                                /* characters */
            wr16(a1, d2); a1 += 2;
            d2 = w_lo(d2, d2 + 1);
            d0 = w_lo(d0, d0 - 1);
            charge(3);
            if ((d0 & 0xFFFF) == 0xFFFF) break;
            set_d(0, d0); set_d(1, d1); set_d(2, d2); set_d(3, d3); set_a(0, a0); set_a(1, a1); poll();
        }
        a1 += sx16(vrd16(a0 + 8));
        d1 = w_lo(d1, d1 - 1);
        charge(2);
        if ((d1 & 0xFFFF) == 0xFFFF) break;
        set_d(0, d0); set_d(1, d1); set_d(2, d2); set_d(3, d3); set_a(0, a0); set_a(1, a1); poll();
    }
    charge(1);
    set_d(0, d0); set_d(1, d1); set_d(2, d2); set_d(3, d3);
    set_a(0, a0); set_a(1, a1);
    return RD_RTS;
}

#define G_TICKER_SRC   W(0x2004)   /* long: the string being revealed        */
#define G_TICKER_LEN   W(0x2008)   /* word: its length - 1                    */
#define G_TICKER_POS   W(0x200A)   /* word: characters shown so far           */
#define G_TICKER_TIMER W(0x200C)   /* word: frame divider                     */
#define G_TICKER_STEP  W(0x200E)   /* word: slide-in offset (mode 1)          */
#define G_TICKER_ATTR  W(0x2010)   /* word: or'ed into every character        */
#define G_TICKER_DEST  W(0x2012)   /* long: where in the text layer           */
#define G_TICKER_MODE  W(0x2022)   /* word: reveal style 0..3, negative = done */

/* FUN_0000defe: start a text reveal: string A0, D1w+1 characters, at text
 * layer cell D0w (A0 = its address). */
static uint32_t rd_ticker_start(void)
{
    vwr32(G_TICKER_SRC, a_reg(0));
    uint32_t a0 = TEXT_LAYER + sx16(d_reg(0)) * 2;
    set_a(0, a0);
    vwr32(G_TICKER_DEST, a0);
    wr16(G_TICKER_LEN, d_reg(1));
    wr16(G_TICKER_POS, 0);
    wr16(G_TICKER_TIMER, 0);
    wr16(G_TICKER_STEP, 0);
    charge(8);
    return RD_RTS;
}

/* FUN_0000e082: map ASCII D0b to its text-layer glyph: a space stays 0x20
 * (unchanged, and no offset); punctuation " ' . - / ( ) ! * and the four
 * lower-case codes o m k i are moved to their glyphs, then 0x40 is added. */
static uint32_t rd_glyph_code(void)
{
    static const uint8_t from[13] = { 0x22, 0x27, 0x2E, 0x2D, 0x2F, 0x28, 0x29, 0x21, 0x2A, 0x6F, 0x6D, 0x6B, 0x69 };
    static const uint8_t to[13]   = { 0x3B, 0x3A, 0x3E, 0x3C, 0x3D, 0x5B, 0x5C, 0x5D, 0x5E, 0x2F, 0x2D, 0x2B, 0x29 };
    uint32_t d0 = d_reg(0);
    charge(2);
    if ((d0 & 0xFF) != 0x20) {
        for (int k = 0; k < 13; k++) {
            charge(2);
            if ((d0 & 0xFF) == from[k]) { d0 = b_lo(d0, to[k]); charge(1); }
        }
        d0 = w_lo(d0, d0 + 0x40);
        charge(1);
    }
    charge(1);
    set_d(0, d0);
    return RD_RTS;
}

/* FUN_0000e262: draw text blocks D6 + D7w, D6 + D7w - 1, ..., D6 (FUN_0000dbc8) */
static uint32_t rd_draw_block_run(void)
{
    uint32_t r;
    for (;;) {
        set_d(0, w_lo(d_reg(6), d_reg(6) + d_reg(7)));
        charge(3);                                /* move.l, add.w, bsr */
        if ((r = rd_call(L_DBC8, 0x00E26A))) return r;
        uint16_t d7 = (uint16_t)(d_reg(7) - 1);
        set_d16(7, d7);
        charge(1);
        if (d7 == 0xFFFF) break;
        poll();
    }
    charge(1);
    return RD_RTS;
}

/* draw one text block: D0 = block (moveq), then bsr FUN_0000dbc8 */
#define DRAW_BLOCK(n, ret) do { set_d(0, (uint32_t)(int32_t)(n)); charge(2); \
    if ((r = rd_call(L_DBC8, (ret)))) return r; } while (0)

#define G_GEAR_SHOWN   W(0x221C)   /* word: gear indicator, & 7 (purpose partly identified) */
#define G_GEAR_PREV    W(0x221E)   /* word */

/* FUN_0000e3f0: draw the seven blocks of the ROM list at 0xE462 (entries 6..0),
 * then 0x221E = 0, 0x221C = 1. */
static uint32_t rd_draw_gear_frame(void)
{
    uint32_t r;
    set_d(7, 6);
    charge(1);
    for (;;) {
        set_d(0, vrd16(0xE462u + sx16(d_reg(7)) * 2));
        charge(4);                                /* moveq, move, nop, bsr */
        if ((r = rd_call(L_DBC8, 0x00E3FE))) return r;
        uint16_t d7 = (uint16_t)(d_reg(7) - 1);
        set_d16(7, d7);
        charge(1);
        if (d7 == 0xFFFF) break;
        poll();
    }
    wr16(G_GEAR_PREV, 0);
    wr16(G_GEAR_SHOWN, 1);
    charge(3);
    return RD_RTS;
}

/* FUN_0000e424: draw block 0xE452[0x221C & 7] and then 0xE462[0x221E & 7],
 * each only when its index is non-zero */
static uint32_t rd_draw_gear_pair(void)
{
    uint32_t r;
    set_d16(1, vrd16(G_GEAR_SHOWN) & 7);
    charge(3);
    if (d_reg(1) & 0xFFFF) {
        set_d(0, vrd16(0xE452u + sx16(d_reg(1)) * 2));
        charge(4);
        if ((r = rd_call(L_DBC8, 0x00E43A))) return r;
        set_d16(1, vrd16(G_GEAR_PREV) & 7);
        charge(3);
        if (d_reg(1) & 0xFFFF) {
            set_d(0, vrd16(0xE462u + sx16(d_reg(1)) * 2));
            charge(4);
            if ((r = rd_call(L_DBC8, 0x00E450))) return r;
        }
    }
    charge(1);
    return RD_RTS;
}

/* FUN_0000e4bc: if the word at 0x204E is non-zero, show it as two BCD digits
 * (FUN_0000a870) in block 6 at positions 0x144 (units) and 0x13E (tens) */
static uint32_t rd_draw_two_digits_204e(void)
{
    uint32_t r;
    set_d(5, 6);
    set_d16(0, vrd16(W(0x204E)));
    charge(3);
    if (!(d_reg(0) & 0xFFFF)) { charge(1); return RD_RTS; }
    set_d(0, d_reg(0) & 0xFFFF);
    charge(2);
    if ((r = rd_call(L_A870, 0x00E4D0))) return r;
    uint32_t d4 = d_reg(1);
    set_d(4, d4); set_d(0, d_reg(5)); set_d(1, d4 & 0xF); set_d16(3, 0x144);
    charge(6);
    if ((r = rd_call(L_DBC8, 0x00E4E4))) return r;
    d4 = d_reg(4) >> 4;
    set_d(0, d_reg(5)); set_d(4, d4); set_d(1, d4 & 0xF); set_d16(3, 0x13E);
    charge(6);
    if ((r = rd_call(L_DBC8, 0x00E4F8))) return r;
    charge(1);
    return RD_RTS;
}

/* FUN_0000e61e: block 0x45 if 0x2344 is set and 0x2342 > 3; block 0x47 if
 * 0x2394 is set */
static uint32_t rd_draw_flags_e61e(void)
{
    uint32_t r;
    charge(2);
    if (vrd16(W(0x2344))) {
        charge(2);
        if ((int16_t)vrd16(W(0x2342)) > 3) DRAW_BLOCK(0x45, 0x00E632);
    }
    charge(2);
    if (vrd16(W(0x2394))) DRAW_BLOCK(0x47, 0x00E63E);
    charge(1);
    return RD_RTS;
}

/* FUN_0000e654 / e65c / e66c / e71c / e9d8: load a block and continue in a
 * shared routine (tail branches back into it -- they poll) */
static uint32_t rd_e654(void) { set_d(7, 2); set_d(6, 0x19); charge(3); return RD_JMP(0xE262u); }
static uint32_t rd_e65c(void) { set_d(0, 0x32); charge(2); return RD_JMP(0xDBC8u); }
static uint32_t rd_e66c(void) { set_d(0, 0x33); charge(2); return RD_JMP(0xDBC8u); }
static uint32_t rd_e71c(void) { set_d(0, 0x19); charge(2); return RD_JMP(0xDBC8u); }
static uint32_t rd_e9d8(void) { set_d(0, 0x49); charge(2); return RD_JMP(0xDBC8u); }

/* FUN_0000e6e0: by (0x232E >> 2) & 3 -- a jump table at 0xE6F4:
 * 0: E71C, E722, then E850; 1: E71C, E758, then E850; 2/3: E71C */
static uint32_t rd_e6e0(void)
{
    uint32_t r;
    uint32_t sel = (vrd16(W(0x232E)) >> 2) & 3;
    uint32_t off = vrd16(0xE6F4u + sel * 2);
    set_d16(0, (uint16_t)off);
    charge(6);
    poll();                                       /* jmp */
    uint32_t t = 0xE6F4u + sx16(off);
    if (t == 0xE714u || t == 0xE718u) { charge(1); return 0xE71Cu; }
    charge(1);
    if ((r = rd_call(L_E71C, t + 4))) return r;
    charge(1);
    if (t == 0xE6FCu) { if ((r = rd_call(L_E722, 0x00E704))) return r; }
    else              { if ((r = rd_call(L_E758, 0x00E710))) return r; }
    charge(1);
    return 0xE850u;
}

/* FUN_0000e722: block 0x31 and blocks 0x2F..0x30, 0x1D..0x1E when the low
 * nibble of 0x1102 is above 1; otherwise blocks 0x88..0x89 */
static uint32_t rd_e722(void)
{
    uint32_t r;
    set_d(0, 0x31);
    set_d16(1, vrd16(G_MODE_1102) & 0xF);
    charge(5);
    if ((int16_t)d_reg(1) > 1) {
        charge(1);
        if ((r = rd_call(L_DBC8, 0x00E736))) return r;
        set_d16(7, 1); set_d(6, 0x2F);
        charge(3);
        if ((r = rd_call(L_E262, 0x00E740))) return r;
        set_d16(7, 1); set_d(6, 0x1D);
        charge(3);
        return RD_JMP(0xE262u);
    }
    set_d16(7, 1); set_d(6, 0x88);
    charge(3);
    return RD_JMP(0xE262u);
}

/* FUN_0000e850: unless 0x10001070 is set, count the three bytes at
 * 0x10001062.. that are 1..3; if any, draw blocks 0x34..0x35 */
static uint32_t rd_e850(void)
{
    uint32_t r;
    charge(2);
    if (vrd8(0x10001070u)) { charge(1); return RD_RTS; }
    uint32_t a0 = 0x10001062u, d0 = d_reg(0), d1 = 0, d7 = 0;
    charge(3);
    do {
        d0 = b_lo(d0, vrd8(a0++) - 1);
        charge(3);                                /* move.b, subq, cmpi */
        charge(1);                                /* bcc */
        if ((d0 & 0xFF) < 3) { d1 = w_lo(d1, d1 + 1); charge(1); }
        d7 = w_lo(d7, d7 + 1);
        charge(3);                                /* addq, cmpi, blt */
        if ((int16_t)d7 < 3) { set_a(0, a0); set_d(0, d0); set_d(1, d1); set_d(7, d7); poll(); }
    } while ((int16_t)d7 < 3);
    set_a(0, a0); set_d(0, d0); set_d(1, d1); set_d(7, d7);
    charge(2);
    if (d1 & 0xFFFF) {
        set_d(7, 1); set_d(6, 0x34);
        charge(3);
        if ((r = rd_call(L_E262, 0x00E882))) return r;
    }
    charge(1);
    return RD_RTS;
}

/* FUN_0000e88e: show D1w as three BCD digits (FUN_0000a88e) -- the hundreds
 * and tens in blocks D5 at positions 0xCC8/0xCC2, D5 = 2 when D1w <= 0xF0
 * (else 0); the last draw is a tail branch */
static uint32_t rd_e88e(void)
{
    uint32_t r;
    set_d(5, 0);
    charge(3);
    if ((int16_t)d_reg(1) <= 0xF0) { set_d(5, 2); charge(1); }
    set_d(1, d_reg(1) & 0xFFFF);
    charge(2);
    if ((r = rd_call(L_A88E, 0x00E8A4))) return r;
    uint32_t d4 = d_reg(0) >> 8;
    set_d(4, d4); set_d(0, d_reg(5)); set_d(1, d4 & 0xF); set_d16(3, 0xCC8);
    charge(7);
    if ((r = rd_call(L_DBC8, 0x00E8BA))) return r;
    d4 = d_reg(4) >> 4;
    set_d(0, d_reg(5)); set_d(4, d4); set_d(1, d4 & 0xF); set_d16(3, 0xCC2);
    charge(6);
    return RD_JMP(0xDBC8u);
}

/* FUN_0000e8fa / e92a: blocks 5 and 6, then blocks D6..D6+1 (0xB or 7) */
static uint32_t draw_56_then(uint32_t a, uint32_t d6)
{
    uint32_t r;
    DRAW_BLOCK(5, a + 6);
    DRAW_BLOCK(6, a + 0xC);
    set_d16(7, 1); set_d(6, d6);
    charge(3);
    return RD_JMP(0xE262u);
}
static uint32_t rd_e8fa(void) { return draw_56_then(0xE8FAu, 0xB); }
static uint32_t rd_e92a(void) { return draw_56_then(0xE92Au, 7); }

/* FUN_0000e940: if 0x2342 >= 4, blocks 0x13..0x1E; otherwise block 4 */
static uint32_t rd_e940(void)
{
    uint32_t r;
    uint16_t v = (uint16_t)(vrd16(W(0x2342)) - 4);
    set_d16(0, v);
    charge(3);
    if (v & 0x8000) {
        DRAW_BLOCK(4, 0x00E94E);
        charge(1);                                /* bra */
    } else {
        set_d(7, 0xB); set_d(6, 0x13);
        charge(3);
        if ((r = rd_call(L_E262, 0x00E958))) return r;
    }
    charge(1);
    return RD_RTS;
}

/* FUN_0000e9e0: show D6w (0..19) as two digits: when >= 10, block 0x4B with
 * the units digit D6w - 10; then block 0x4A with D6w (tail branch) */
static uint32_t rd_e9e0(void)
{
    uint32_t r;
    charge(2);
    if ((uint16_t)d_reg(6) >= 10) {
        set_d16(6, (uint16_t)(d_reg(6) - 10));
        set_d16(1, (uint16_t)d_reg(6));
        set_d(0, 0x4B);
        charge(4);
        if ((r = rd_call(L_DBC8, 0x00E9F2))) return r;
    }
    set_d16(1, (uint16_t)d_reg(6));
    set_d(0, 0x4A);
    charge(3);
    return RD_JMP(0xDBC8u);
}

/* FUN_0000ea6a: print the string at 0xEA8E (FUN_00005a0a), then five 0x235A
 * and three 0x2360 characters after it */
static uint32_t rd_ea6a(void)
{
    uint32_t r;
    set_a(1, 0xEA8Eu);
    charge(2);
    if ((r = rd_call(L_5A0A, 0x00EA74))) return r;
    uint32_t a0 = a_reg(0);
    charge(1);
    for (int n = 4; n >= 0; n--) {
        wr16(a0, 0x235A); a0 += 2;
        set_a(0, a0); set_d16(7, (uint16_t)(n - 1));
        charge(2); if (n) poll();
    }
    charge(1);
    for (int n = 2; n >= 0; n--) {
        wr16(a0, 0x2360); a0 += 2;
        set_a(0, a0); set_d16(7, (uint16_t)(n - 1));
        charge(2); if (n) poll();
    }
    charge(1);
    set_a(0, a0); set_d16(7, 0xFFFF);
    return RD_RTS;
}

/* FUN_0000ea92: print the string at 0xEA8E, then one bar character for the
 * value at 0x412A: at (0x412A / 20) words on, 0x235A (or 0x2360 when that is
 * >= 10) plus the step table 0xEACA[remainder] */
static uint32_t rd_ea92(void)
{
    uint32_t r;
    set_a(1, 0xEA8Eu);
    charge(2);
    if ((r = rd_call(L_5A0A, 0x00EA9C))) return r;
    uint32_t v = vrd16(W(0x412A));
    uint32_t d0 = ((v % 20) << 16) | (v / 20);
    uint32_t d1 = d0;
    d0 = w_lo(d0, d0 * 2);
    uint32_t a0 = a_reg(0) + sx16(d0);
    uint32_t d2 = w_lo(d_reg(2), 0x235A);
    charge(9);
    if ((uint16_t)d0 >= 10) { d2 = w_lo(d2, 0x2360); charge(1); }
    d1 = swap32(d1);
    d1 = w_lo(d1, vrd16(0xEACAu + sx16(d1) * 2));
    d2 = w_lo(d2, d2 + d1);
    wr16(a0, d2);
    charge(6);
    set_a(0, a0); set_a(1, 0xEACAu);
    set_d(0, d0); set_d(1, d1); set_d(2, d2);
    return RD_RTS;
}

/* FUN_0000eafa: advance the counter 0x2432 and draw block -6 with the
 * digit (count >> 4) & 0xF, capped at 10, at position 0x304 */
static uint32_t rd_eafa(void)
{
    uint32_t r;
    uint32_t d1 = vrd16(W(0x2432)) + 1;
    d1 &= 0xFFFF;
    wr16(W(0x2432), d1);
    d1 = (d1 >> 4) & 0xF;
    charge(8);
    if ((int16_t)d1 > 10) { d1 = 10; charge(1); }
    set_d(1, d1); set_d16(3, 0x304); set_d(0, 0xFFFFFFFAu);
    charge(3);
    if ((r = rd_call(L_DBC8, 0x00EB20))) return r;
    charge(1);
    return RD_RTS;
}

/* =====================================================================
 * Game-state handlers around the attract / start sequence
 * ===================================================================== */

#define G_STATE_TIMER   W(0x2002)   /* word: frames in this state (purpose partly identified) */
#define G_SUBSTATE      W(0x232E)   /* word: sub-state of the current state (index of 0xF51E)  */
#define G_STATE_DA0     W(0xDA0)    /* word: the game's main state number                      */

/* FUN_0000f464: a state step. FUN_0000c258 every call; on the first frame
 * (0x2002 == 0) FUN_00005868, FUN_0000575a, FUN_000058ee, clear a set of
 * state words and, if 0x7028 is 0, FUN_0001371a; from frame 0x3C on,
 * FUN_0000575a, CGRAM set 1 (FUN_0000c69a), FUN_0000efee and the next main
 * state. Counts 0x2002. */
static uint32_t rd_f464(void)
{
    uint32_t r;
    charge(1);
    if ((r = rd_call(L_C258, 0x00F468))) return r;
    charge(2);
    if (!vrd16(G_STATE_TIMER)) {
        charge(1); if ((r = rd_call(L_5868, 0x00F474))) return r;
        charge(1); if ((r = rd_call(L_575A, 0x00F47A))) return r;
        charge(1); if ((r = rd_call(L_58EE, 0x00F480))) return r;
        wr16(W(0xC64), 0); wr16(W(0xC62), 0); wr16(W(0x232A), 0);
        wr16(G_SUBSTATE, 0); wr16(W(0x2332), 0);
        wr8(W(0x11AA), 0); wr8(W(0x11AB), 0); wr8(W(0x11A8), 0);
        wr16(W(0x11A2), 0);
        charge(11);                               /* nine clears, cmpi, bne */
        if (!vrd16(W(0x7028))) {
            charge(1);
            if ((r = rd_call(L_1371A, 0x00F4B0))) return r;
        }
    }
    charge(2);
    if ((int16_t)vrd16(G_STATE_TIMER) >= 0x3C) {
        charge(1); if ((r = rd_call(L_575A, 0x00F4BE))) return r;
        wr16(G_CG_BANK, 1);
        charge(2); if ((r = rd_call(L_C69A, 0x00F4CA))) return r;
        charge(1); if ((r = rd_call(L_EFEE, 0x00F4D0))) return r;
        wr16(G_STATE_DA0, vrd16(G_STATE_DA0) + 1);
        charge(1);
    }
    wr16(G_STATE_TIMER, vrd16(G_STATE_TIMER) + 1);
    charge(2);
    return RD_RTS;
}

/* FUN_0000f4da: a state step: FUN_0000f61e, FUN_0000c258, then call the
 * sub-state handler from the long offset table at 0xF51E (sub-states
 * 0..0x1F; a negative sub-state -1..-3 selects the entries just before it),
 * and continue in FUN_0000f67e. */
static uint32_t rd_f4da(void)
{
    uint32_t r;
    charge(1);
    if ((r = rd_call(L_F61E, 0x00F4DE))) return r;
    charge(1);
    if ((r = rd_call(L_C258, 0x00F4E2))) return r;
    uint32_t d0 = w_lo(d_reg(0), vrd16(G_SUBSTATE));
    charge(2);
    if (d0 & 0x8000) {
        d0 = w_lo(d0, (uint32_t)-(int32_t)d0);
        d0 = w_lo(d0, d0 & 3);
        d0 = w_lo(d0, (uint32_t)-(int32_t)d0);
        charge(4);
    } else {
        d0 = w_lo(d0, d0 & 0x1F);
        charge(3);
        if ((int16_t)d0 > 0x23) { d0 = w_lo(d0, 0); charge(1); }
    }
    wr16(G_SUBSTATE, d0);
    uint32_t off = vrd32(0xF51Eu + sx16(d0) * 4);
    set_d(0, off);
    charge(4);                                    /* move, move.l, nop, jsr */
    if ((r = rd_call_ind(0xF51Eu + off, 0x00F50E, 0x00F50A))) return r;
    charge(1);                                    /* bra.w */
    return 0xF67Eu;
}

#define G_SLOT_FLAGS_11B4 W(0x11B4)  /* 8 records of 16 bytes: per-player cabinet flags (purpose partly identified) */

/* FUN_0000f61e: unless the sub-state is negative: reset the words/bytes
 * 0x1100 (-1), 0x11A2, 0x11AA, 0x11AB, 0x11A8 (bit 0 set); then -- unless
 * the main state is 5 with no credits -- if any of the 8 records at 0x11B4
 * has flag bits 6 and 5 set, go to main state 1 with sub-state -1. */
static uint32_t rd_f61e(void)
{
    charge(2);
    if (vrd16(G_SUBSTATE) & 0x8000) { charge(1); return RD_RTS; }
    wr16(W(0x1100), 0xFFFF); wr16(W(0x11A2), 0);
    wr8(W(0x11AA), 0); wr8(W(0x11AB), 0); wr8(W(0x11A8), 0);
    wr8(W(0x11A8), vrd8(W(0x11A8)) | 1);
    charge(8);                                    /* six stores, cmpi, bne */
    if (vrd16(G_STATE_DA0) == 5) {
        charge(2);
        if (!vrd16(G_CREDITS)) { charge(1); return RD_RTS; }
    }
    uint32_t d0 = 0, d6 = 7;
    charge(2);
    for (;;) {
        uint32_t f = vrd8(G_SLOT_FLAGS_11B4 + sx16(d0));
        charge(2);
        if (f & 0x40) {
            charge(2);
            if (vrd8(G_SLOT_FLAGS_11B4 + sx16(d0)) & 0x20) {
                wr16(G_STATE_DA0, 1);
                wr16(G_SUBSTATE, 0xFFFF);
                charge(4);                        /* move, move, bra, rts */
                set_d(0, d0); set_d(6, d6);
                return RD_RTS;
            }
        }
        d0 = w_lo(d0, d0 + 0x10);
        d6 = w_lo(d6, d6 - 1);
        charge(2);
        if ((d6 & 0xFFFF) == 0xFFFF) break;
        set_d(0, d0); set_d(6, d6); poll();
    }
    charge(1);
    set_d(0, d0); set_d(6, d6);
    return RD_RTS;
}

#define G_COST_PER_GAME  0x10001040u /* byte: credits a game costs (settings) */
#define G_FREE_PLAY      0x10001043u /* byte: free play (settings)            */

/* FUN_0000f67e: start a game when possible: coins enabled (> 2), enough
 * credits (0x202A >= the game cost; at exactly the cost only in free play or
 * with 0x203E clear), and the start button (0x10000812 bit 7) -- except that
 * the exact-cost, not-free, 0x203E-clear case starts without the button.
 * Then take the credits, main state 2, and FUN_000286fc. */
static uint32_t rd_f67e(void)
{
    uint32_t r;
    charge(2);
    if ((int16_t)vrd16(G_COIN_ENABLE) <= 2) { charge(1); return RD_RTS; }
    uint32_t d0 = vrd8(G_COST_PER_GAME);
    set_d(0, d0);
    int16_t cred = (int16_t)vrd16(G_CREDITS);
    charge(4);                                    /* moveq, move.b, cmp, bgt */
    if ((int16_t)d0 > cred) { charge(1); return RD_RTS; }
    charge(1);                                    /* blt */
    int need_button = 1;
    if ((int16_t)d0 == cred) {
        charge(2);                                /* tst.b, bne */
        if (!vrd8(G_FREE_PLAY)) {
            charge(2);                            /* tst.w, beq */
            if (!vrd16(W(0x203E))) need_button = 0;
        }
    }
    if (need_button) {
        charge(2);
        if (!(vrd8(W(-0x77EE)) & 0x80)) { charge(1); return RD_RTS; }
    }
    wr16(G_CREDITS, vrd16(G_CREDITS) - d0);
    wr16(G_COIN_ENABLE, 2);
    wr16(G_SUBSTATE, 0);
    wr16(W(0x2332), 0);
    wr16(G_STATE_DA0, 2);
    wr8(W(0x11A8), vrd8(W(0x11A8)) & ~1u);
    wr8(W(0x11A8), vrd8(W(0x11A8)) | 2);
    charge(8);
    if ((r = rd_call(L_286FC, 0x00F6D6))) return r;
    vwr32(W(0x5206), 0);
    charge(2);
    return RD_RTS;
}

/* FUN_0000f6dc: sub-state step: clear 0x4740/0x4346/0x434A, FUN_0000575a,
 * CGRAM set 1 if not already, FUN_0000b89a, FUN_00016fc4, 0x2330 = 1200,
 * FUN_0000f9ca, next sub-state */
static uint32_t rd_f6dc(void)
{
    uint32_t r;
    wr16(W(0x4740), 0); vwr32(W(0x4346), 0); wr16(W(0x434A), 0);
    charge(4);
    if ((r = rd_call(L_575A, 0x00F6F6))) return r;
    charge(2);
    if (vrd16(G_CG_BANK) != 1) {
        wr16(G_CG_BANK, 1);
        charge(2);
        if ((r = rd_call(L_C69A, 0x00F70A))) return r;
    }
    charge(1); if ((r = rd_call(L_B89A, 0x00F710))) return r;
    charge(1); if ((r = rd_call(L_16FC4, 0x00F716))) return r;
    wr16(W(0x2330), 0x4B0);
    charge(2); if ((r = rd_call(L_F9CA, 0x00F720))) return r;
    wr16(G_SUBSTATE, vrd16(G_SUBSTATE) + 1);
    charge(3);                                    /* nop, addq, rts */
    return RD_RTS;
}

/* FUN_0000f728: FUN_00028b98, then print the strings at 0xF748 one after
 * another (FUN_00005a0a, D5 = 0x5B) until an empty one, then blocks 0x32
 * (FUN_0000e65c) and 0x33 (tail branch into FUN_0000e66c) */
static uint32_t rd_f728(void)
{
    uint32_t r;
    charge(1);
    if ((r = rd_call(L_28B98, 0x00F72E))) return r;
    set_a(1, 0xF748u);
    set_d(5, 0x5B);
    charge(2);
    for (;;) {
        charge(1);
        if ((r = rd_call(L_5A0A, 0x00F73A))) return r;
        charge(2);                                /* tst.b, bne */
        if (!vrd8(a_reg(1))) break;
        poll();
    }
    charge(1);
    if ((r = rd_call(L_E65C, 0x00F744))) return r;
    charge(1);
    return RD_JMP(0xE66Cu);
}

/* FUN_0000f798: FUN_0001704e, then count down 0x2330; when it runs out: with
 * at most one player (0x1102 <= 1) just the next sub-state; otherwise flag
 * 0x11A4 bit 0 and pick the next sub-state from the largest byte at
 * 0x11B1 + 16n among the 8 records whose 0x11B4 bit 0 is set -- stopping
 * at once if such a record's 0x11B0 bit 0 is clear. */
static uint32_t rd_f798(void)
{
    uint32_t r;
    charge(1);
    if ((r = rd_call(L_1704E, 0x00F79E))) return r;
    uint32_t d0 = w_lo(d_reg(0), vrd16(W(0x2330)));
    set_d(0, d0);
    charge(2);
    if (d0 & 0xFFFF) {
        d0 = w_lo(d0, d0 - 1);
        wr16(W(0x2330), d0);
        set_d(0, d0);
        charge(4);                                /* subq, move, bra, rts */
        return RD_RTS;
    }
    charge(2);
    if ((int16_t)vrd16(G_MODE_1102) <= 1) {
        wr16(G_SUBSTATE, vrd16(G_SUBSTATE) + 1);
        charge(3);
        return RD_RTS;
    }
    wr8(W(0x11A4), vrd8(W(0x11A4)) | 1);
    wr8(W(0x11A5), vrd16(G_SUBSTATE));
    uint32_t d6 = 7, d1 = 0;
    d0 = 0;
    charge(6);
    for (;;) {
        charge(2);
        if (vrd8(W(0x11B4) + sx16(d0)) & 1) {
            charge(2);
            if (!(vrd8(W(0x11B0) + sx16(d0)) & 1)) {
                charge(1);
                set_d(0, d0); set_d(1, d1); set_d(6, d6);
                return RD_RTS;
            }
            uint32_t v = vrd8(W(0x11B1) + sx16(d0));
            charge(2);
            if ((d1 & 0xFF) < v) { d1 = b_lo(d1, v); charge(1); }
        }
        d0 = w_lo(d0, d0 + 0x10);
        d6 = w_lo(d6, d6 - 1);
        charge(2);
        if ((d6 & 0xFFFF) == 0xFFFF) break;
        set_d(0, d0); set_d(1, d1); set_d(6, d6); poll();
    }
    wr8(W(0x11A5), d1);
    wr16(G_SUBSTATE, d1);
    wr16(G_SUBSTATE, vrd16(G_SUBSTATE) + 1);
    charge(4);
    set_d(0, d0); set_d(1, d1); set_d(6, d6);
    return RD_RTS;
}

/* FUN_0000f9ca: object block 0: 0x1C = 0x3C, 0x68 = 0, current block 0,
 * flags |= 0x1D, then emit it (FUN_0000b56e, FUN_0000b6d0) */
static uint32_t rd_f9ca(void)
{
    uint32_t r;
    wr16(W(0x1C), 0x3C); wr16(W(0x68), 0); wr16(W(0xC40), 0);
    wr16(W(0), vrd16(W(0)) | 0x1D);
    charge(5);
    if ((r = rd_call(L_B56E, 0x00F9E6))) return r;
    charge(1);
    if ((r = rd_call(L_B6D0, 0x00F9EC))) return r;
    charge(1);
    return RD_RTS;
}

/* FUN_0000f9ee: object block 0 for the player count shown (FUN_0000faf4 ->
 * 0x46F4): model word 0x1C from ROM 0xFAE2[n]; 0x66 = 0x6A = 0; 0x68 = 0 for
 * n = 0, else the word from the per-n table 0xFA4C[n] at index
 * 0xFAD0[n] + 0x11A0; then emit it (FUN_0000b56e, FUN_0000b6d0) */
static uint32_t rd_f9ee(void)
{
    uint32_t r;
    charge(1);
    if ((r = rd_call(L_FAF4, 0x00F9F2))) return r;
    uint32_t d0 = w_lo(d_reg(0), vrd16(W(0x46F4)));
    d0 = w_lo(d0, vrd16(0xFAE2u + sx16(d0) * 2));
    wr16(W(0x1C), d0);
    wr16(W(0x66), 0); wr16(W(0x6A), 0);
    uint32_t a0 = 0xFAD0u, d1 = d_reg(1), d2 = w_lo(d_reg(2), 0);
    d0 = w_lo(d0, vrd16(W(0x46F4)));
    charge(10);
    if (d0 & 0xFFFF) {
        d1 = w_lo(d1, vrd16(0xFAD0u + sx16(d0) * 2) + vrd16(W(0x11A0)));
        a0 = vrd32(0xFA4Cu + sx16(d0) * 4);
        d2 = w_lo(d2, vrd16(a0 + sx16(d1) * 2));
        charge(5);
    }
    wr16(W(0x68), d2);
    wr16(W(0xC40), 0);
    wr16(W(0), vrd16(W(0)) | 0x1D);
    set_d(0, d0); set_d(1, d1); set_d(2, d2); set_a(0, a0);
    charge(4);
    if ((r = rd_call(L_B56E, 0x00FA44))) return r;
    charge(1);
    if ((r = rd_call(L_B6D0, 0x00FA4A))) return r;
    charge(1);
    return RD_RTS;
}

/* FUN_0000faf4: 0x46F4 = 1 + the highest set bit (7..0) of 0x1104 when both
 * 0x1104 and 0x1102 are non-zero, else 0 (unchanged when 0x1104's low byte
 * has no bit set) */
static uint32_t rd_faf4(void)
{
    uint32_t d7 = w_lo(d_reg(7), 7);
    uint32_t d0 = w_lo(d_reg(0), vrd16(W(0x1104)));
    set_d(0, d0);
    charge(3);
    if ((d0 & 0xFFFF) && (charge(2), vrd16(G_MODE_1102))) {
        for (;;) {
            charge(2);                            /* btst, bne */
            if (d0 & (1u << (d7 & 31))) {
                d7 += 1;
                wr16(W(0x46F4), d7);
                charge(3);
                set_d(7, d7);
                return RD_RTS;
            }
            d7 = w_lo(d7, d7 - 1);
            charge(1);
            if ((d7 & 0xFFFF) == 0xFFFF) break;
            set_d(7, d7); poll();
        }
        charge(1);
        set_d(7, d7);
        return RD_RTS;
    }
    wr16(W(0x46F4), 0);
    charge(2);
    set_d(7, d7);
    return RD_RTS;
}

/* FUN_0000fdee: FUN_0001dc20, 0x221E = 0x221C, FUN_0000fe22 with D0 = 0,
 * then 0x2220 = 0x221C (+8 when 0x1000080E bit 6 is set) */
static uint32_t rd_fdee(void)
{
    uint32_t r;
    charge(1);
    if ((r = rd_call(L_1DC20, 0x00FDF4))) return r;
    wr16(G_GEAR_PREV, vrd16(G_GEAR_SHOWN));
    set_d(0, 0);
    charge(3);
    if ((r = rd_call(L_FE22, 0x00FE00))) return r;
    uint32_t d0 = w_lo(d_reg(0), vrd16(G_GEAR_SHOWN));
    charge(3);
    if (vrd8(W(-0x77F2)) & 0x40) { d0 = w_lo(d0, d0 + 8); charge(1); }
    wr16(W(0x2220), d0);
    set_d(0, d0);
    charge(2);
    return RD_RTS;
}

/* FUN_0000fe44: FUN_0000fdee, then 0x2224 = 0x2220 unless 0x2224 is 7 and
 * 0x2222 already equals 0x2220 */
static uint32_t rd_fe44(void)
{
    uint32_t r;
    charge(1);
    if ((r = rd_call(L_FDEE, 0x00FE46))) return r;
    charge(2);
    if (vrd16(W(0x2224)) == 7) {
        uint16_t v = (uint16_t)vrd16(W(0x2222));
        set_d16(0, v);
        charge(3);
        if (v == (uint16_t)vrd16(W(0x2220))) { charge(1); return RD_RTS; }
    }
    wr16(W(0x2224), vrd16(W(0x2220)));
    charge(2);
    return RD_RTS;
}

/* print the string at `str` (FUN_00005a0a); the ret address is after the jsr */
#define PRINT(str, ret) do { set_a(1, (str)); if ((r = rd_call(L_5A0A, (ret)))) return r; } while (0)

/* FUN_0000fe80: with remapping on, print the string at 0x10232, then the
 * strings selected by (0x2034 >> 3) & 3 (table 0xFFB4) and by 0x2224 & 0xF
 * (tables 0xFF34 and 0xFF74) */
static uint32_t rd_fe80(void)
{
    uint32_t r;
    wr16(G_TEXT_REMAP, 1);
    charge(3);
    PRINT(0x10232u, 0x00FE90);
    uint32_t d0 = w_lo(d_reg(0), vrd16(G_SW_SNAPSHOT));
    d0 = b_lo(d0, (d0 & 0xFF) >> 3);
    d0 = w_lo(d0, d0 & 3);
    set_d(0, d0);
    charge(5);
    PRINT(vrd32(0xFFB4u + sx16(d0) * 4), 0x00FEA6);
    d0 = w_lo(d_reg(0), vrd16(W(0x2224)) & 0xF);
    set_d(0, d0);
    charge(4);
    PRINT(vrd32(0xFF34u + sx16(d0) * 4), 0x00FEBA);
    d0 = w_lo(d_reg(0), vrd16(W(0x2224)) & 0xF);
    set_d(0, d0);
    charge(4);
    PRINT(vrd32(0xFF74u + sx16(d0) * 4), 0x00FECE);
    wr16(G_TEXT_REMAP, 0);
    charge(2);
    return RD_RTS;
}

/* FUN_0000fed6: with remapping on, print the string at 0x10238, the one
 * selected by (0x2034 >> 2) & 3 (table 0xFFB4) and by 0x2224 & 0xF (table
 * 0xFF74), then FUN_0000eafa */
static uint32_t rd_fed6(void)
{
    uint32_t r;
    wr16(G_TEXT_REMAP, 1);
    charge(3);
    PRINT(0x10238u, 0x00FEE6);
    uint32_t d0 = w_lo(d_reg(0), vrd16(G_SW_SNAPSHOT));
    d0 = b_lo(d0, (d0 & 0xFF) >> 2);
    d0 = w_lo(d0, d0 & 3);
    set_d(0, d0);
    charge(5);
    PRINT(vrd32(0xFFB4u + sx16(d0) * 4), 0x00FEFC);
    d0 = w_lo(d_reg(0), vrd16(W(0x2224)) & 0xF);
    set_d(0, d0);
    charge(5);
    PRINT(vrd32(0xFF74u + sx16(d0) * 4), 0x00FF10);
    wr16(G_TEXT_REMAP, 0);
    charge(2);
    if ((r = rd_call(L_EAFA, 0x00FF1C))) return r;
    charge(1);
    return RD_RTS;
}

/* FUN_0000ff1e: print the strings at 0x10238 and 0x1020E */
static uint32_t rd_ff1e(void)
{
    uint32_t r;
    charge(2);
    PRINT(0x10238u, 0x00FF28);
    charge(2);
    PRINT(0x1020Eu, 0x00FF32);
    charge(1);
    return RD_RTS;
}

/* FUN_0001024e: copy 33 bytes from A0 to A1 (both post-incremented) */
static uint32_t rd_copy33(void)
{
    uint32_t a0 = a_reg(0), a1 = a_reg(1);
    charge(1);
    for (int n = 0x20; n >= 0; n--) {
        wr8(a1++, vrd8(a0++));
        charge(2);
        set_a(0, a0); set_a(1, a1); set_d16(0, (uint16_t)(n - 1));
        if (n) poll();
    }
    charge(1);
    set_a(0, a0); set_a(1, a1); set_d16(0, 0xFFFF);
    return RD_RTS;
}

/* FUN_0001025a: build "o" + the 33 bytes at ROM 0x10054 at 0x2440 and reveal
 * it (FUN_0000defe: cell 0x45, 0x20 + 1 characters) with attribute 0xE000,
 * reveal mode 0 */
static uint32_t rd_1025a(void)
{
    uint32_t r;
    set_a(0, 0x10054u);
    set_a(1, W(0x2441));
    wr8(W(0x2440), 0x6F);
    charge(4);
    if ((r = rd_call(L_1024E, 0x010268))) return r;
    set_a(0, W(0x2440));
    set_d16(0, 0x45);
    set_d16(1, 0x20);
    wr16(G_TICKER_ATTR, 0xE000);
    wr16(G_TICKER_MODE, 0);
    charge(6);
    return RD_JMP(0xDEFEu);
}

/* FUN_000103c4: advance the text reveal (FUN_0000df22) unless 0x1000106E is set */
static uint32_t rd_103c4(void)
{
    uint32_t r;
    charge(2);
    if (!vrd8(0x1000106Eu)) {
        charge(1);
        if ((r = rd_call(L_DF22, 0x0103D2))) return r;
    }
    charge(1);
    return RD_RTS;
}

/* FUN_000103d4: FUN_0000fe44, then continue in FUN_0000fe80 (backward: polls) */
static uint32_t rd_103d4(void)
{
    uint32_t r;
    charge(1);
    if ((r = rd_call(L_FE44, 0x0103D8))) return r;
    charge(1);
    return RD_JMP(0xFE80u);
}

/* thunk_FUN_0000fe60: continue in 0xFE60 (backward: polls) */
static uint32_t rd_103dc(void) { charge(1); return RD_JMP(0xFE60u); }

/* FUN_00010462 / 10474: step a menu value through FUN_00010502 / 10514 and,
 * if it changed, FUN_00028706 (FUN_00010498) */
static uint32_t rd_10462(void)
{
    uint32_t r;
    uint16_t v = (uint16_t)vrd16(W(0x2340));
    set_d16(0, v); set_d16(7, v);
    charge(3);
    if ((r = rd_call(L_10502, 0x01046C))) return r;
    wr16(W(0x2340), d_reg(0));
    charge(2);
    return 0x10498u;
}
static uint32_t rd_10474(void)
{
    uint32_t r;
    uint16_t v = (uint16_t)vrd16(W(0x2342));
    set_d16(0, v); set_d16(7, v);
    charge(3);
    if ((r = rd_call(L_10514, 0x01047E))) return r;
    uint32_t d0 = d_reg(0);
    charge(2);
    if (vrd8(W(-0x77F2)) & 0x10) { d0 = w_lo(d0, d0 + 4); charge(1); }
    wr16(W(0x2342), d0);
    d0 = w_lo(d0, d0 & 3);
    set_d(0, d0);
    set_d16(7, d_reg(7) & 3);
    charge(4);
    return 0x10498u;
}

/* FUN_00010498: if D0w != D7w (the value changed), FUN_00028706 */
static uint32_t rd_10498(void)
{
    uint32_t r;
    charge(2);
    if ((uint16_t)d_reg(0) != (uint16_t)d_reg(7)) {
        charge(1);
        if ((r = rd_call(L_28706, 0x0104A2))) return r;
    }
    charge(1);
    return RD_RTS;
}

/* FUN_00010502 / 10514: FUN_000104dc, then D0w = the word at index 0x2226 of
 * the table selected by D0w (ROM 0x1052A; 0x10532 with D0w & 3) */
static uint32_t table_pick(uint32_t tbl, uint32_t ret)
{
    uint32_t r;
    if ((r = rd_call(L_104DC, ret))) return r;
    uint32_t d1 = w_lo(d_reg(1), vrd16(W(0x2226)));
    uint32_t a0 = vrd32(tbl + sx16(d_reg(0)) * 4);
    set_d(1, d1); set_a(0, a0);
    set_d16(0, (uint16_t)vrd16(a0 + sx16(d1) * 2));
    charge(5);
    return RD_RTS;
}
static uint32_t rd_10502(void) { charge(1); return table_pick(0x1052Au, 0x010504); }
static uint32_t rd_10514(void) { set_d16(0, d_reg(0) & 3); charge(2); return table_pick(0x10532u, 0x01051A); }

/* FUN_00010f26: start-up of a state: FUN_00005868, FUN_0000575a,
 * FUN_000058ee, clear 0x2216/18/19, load object block 6 from ROM 0x10F9E
 * (FUN_0000b4e4) and emit it, clear its bit in 0x2216, copy four longs from
 * ROM 0x10F8E to 0x2356.. and set 0x2366.. to 0x15C0, clear 0x238E,
 * FUN_0001429e, FUN_0000b8b0 */
static uint32_t rd_10f26(void)
{
    uint32_t r;
    charge(1); if ((r = rd_call(L_5868, 0x010F2C))) return r;
    charge(1); if ((r = rd_call(L_575A, 0x010F32))) return r;
    charge(1); if ((r = rd_call(L_58EE, 0x010F38))) return r;
    wr8(W(0x2216), 0); wr8(W(0x2218), 0); wr8(W(0x2219), 0);
    wr16(W(0xC40), 6);
    set_a(0, 0x10F9Eu);
    charge(6);
    if ((r = rd_call(L_B4E4, 0x010F52))) return r;
    charge(1);
    if ((r = rd_call(L_B56E, 0x010F56))) return r;
    charge(1);
    if ((r = rd_call(L_B6D0, 0x010F5A))) return r;
    uint32_t d0 = w_lo(d_reg(0), vrd16(W(0xC40)));
    wr8(W(0x2216), vrd8(W(0x2216)) & ~(1u << (d0 & 7)));
    set_d(0, d0);
    set_d(7, 3);
    charge(3);                                    /* move, bclr, moveq */
    for (int i = 3; i >= 0; i--) {
        vwr32(W(0x2356) + (uint32_t)i * 4, vrd32(0x10F8Eu + (uint32_t)i * 4));
        vwr32(W(0x2366) + (uint32_t)i * 4, 0x15C0);
        charge(4);                                /* move.l, nop, move.l, dbf */
        set_d16(7, (uint16_t)(i - 1));
        if (i) poll();
    }
    set_d(7, 0x0000FFFFu);
    wr16(W(0x238E), 0);
    charge(2);
    if ((r = rd_call(L_1429E, 0x010F86))) return r;
    charge(1);
    if ((r = rd_call(L_B8B0, 0x010F8C))) return r;
    charge(1);
    return RD_RTS;
}

/* =====================================================================
 * Credits display, text reveal, HUD
 * ===================================================================== */

/* divu.w exactly as the lifted build computes it (a zero divisor traps and
 * gives 0; an overflowing quotient leaves the register unchanged) */
static uint32_t divu_w(uint32_t dividend, uint32_t divisor)
{
    uint64_t q = UDIVREM(dividend, divisor & 0xFFFF, '/');
    uint64_t r = UDIVREM(dividend, divisor & 0xFFFF, '%');
    if (q > 0xFFFF) return dividend;
    return (uint32_t)(r << 16) | (uint32_t)(q & 0xFFFF);
}

#define COIN_MODE_1045  0x10001045u   /* byte: 1 = both coin slots worth the same (settings) */

/* FUN_0000c258: the coin/credit state machine, by 0x2028 & 3 (a jump table
 * at 0xC26A):
 *   0: wait for the I/O board (0x60004032 non-zero); then latch the coin
 *      counters and go to 1 -- printing the message at 0xC2BA, else 0xC29E
 *      (a tail jump into the string printer FUN_00005a0a)
 *   1: clear the service count, credits, meter and pending pulses; go to 2
 *   2: 0x2040 = 0, 0x203E = 0x202C = credits; go to 3
 *   3: outside states 0 and 5, print the credit line: "CREDIT(S)" with the
 *      count (capped at 9), the coins-per-credit legend when a game costs
 *      more than one credit, and -- unless 0x2032 -- the "press start" /
 *      "insert coin(s)" prompt, choosing the words by the settings; in free
 *      play the free-play text. Then 0x2040 decides the start prompt blocks
 *      (FUN_0000e6d6, and FUN_0000e6cc / FUN_0000e6b2). */
static uint32_t rd_credit_display(void)
{
    uint32_t r;
    uint32_t d0 = w_lo(d_reg(0), vrd16(G_COIN_ENABLE) & 3);
    d0 = w_lo(d0, vrd16(0xC26Au + sx16(d0) * 2));
    set_d(0, d0);
    charge(5);
    poll();                                       /* jmp */
    uint32_t t = 0xC26Au + sx16(d0);
    if (t == 0xC272u) {
        charge(2);
        if (vrd16(SHARED(0x32))) {
            wr8(G_COIN1_SEEN, vrd8(SHARED(0x3A)));
            wr8(G_COIN2_SEEN, vrd8(SHARED(0x3B)));
            wr16(G_COIN_ENABLE, vrd16(G_COIN_ENABLE) + 1);
            set_a(1, 0xC2BAu);
            charge(5);                            /* move.b x2, addq, lea, bra */
        } else {
            set_a(1, 0xC29Eu);
            charge(1);
        }
        charge(1);                                /* jmp */
        return RD_JMP(0x5A0Au);
    }
    if (t == 0xC2D6u) {
        wr16(G_SERVICE_COUNT, 0); wr16(G_CREDITS, 0); wr16(G_METER_PHASE, 0);
        wr16(G_COIN1_PENDING, 0); wr16(G_COIN2_PENDING, 0);
        wr16(G_COIN_ENABLE, vrd16(G_COIN_ENABLE) + 1);
        charge(7);
        return RD_RTS;
    }
    if (t == 0xC2F0u) {
        wr16(W(0x2040), 0);
        wr16(W(0x203E), vrd16(G_CREDITS));
        wr16(W(0x202C), vrd16(G_CREDITS));
        wr16(G_COIN_ENABLE, vrd16(G_COIN_ENABLE) + 1);
        charge(5);
        return RD_RTS;
    }
    /* state 3: the credit line */
    d0 = w_lo(d0, vrd16(G_STATE_DA0));
    set_d(0, d0);
    charge(2);
    if (!(d0 & 0xFFFF)) { charge(1); return RD_RTS; }
    charge(2);
    if ((d0 & 0xFFFF) == 5) { charge(1); return RD_RTS; }
    charge(2);
    PRINT(0xC5E7u, 0x00C320);                     /* "CREDIT" */
    uint32_t d6 = w_lo(d_reg(6), vrd16(G_SUBSTATE));
    charge(2);
    if (d6 & 0x8000) { d6 = w_lo(d6, (uint32_t)-(int32_t)d6); charge(1); }
    d6 = w_lo(d6, (d6 & 0xFFFF) >> 1);
    d6 = w_lo(d6, d6 & 1);                        /* which of two text sets (alternates) */
    set_d(6, d6);
    uint32_t i6 = sx16(d6) * 4;
    charge(4);                                    /* lsr, andi, tst.b, bne */
    if (vrd8(G_FREE_PLAY)) {
        uint32_t v = vrd8(G_COST_PER_GAME);
        set_d(0, v);
        wr16(G_CREDITS, v);
        charge(6);                                /* moveq, move.b, move, movea, nop, jsr */
        PRINT(vrd32(0xC4C8u + i6), 0x00C48C);     /* "FREE PLAY" */
        charge(2);
        if (!vrd16(G_SW_BIT5)) {
            charge(2);
            PRINT(0xC5CBu, 0x00C49C);             /* the start prompt */
            wr16(W(0x2040), 0xFFFF);
            charge(1);
        }
        goto prompt;
    }
    /* the count, capped at 9, and "S" for plural */
    uint32_t a1 = vrd32(0xC4D0u + i6);
    set_d16(5, (uint16_t)vrd16(G_CREDITS));
    charge(4);
    if ((int16_t)d_reg(5) > 1) { a1 = vrd32(0xC4D8u + i6); charge(1); }
    charge(1);
    PRINT(a1, 0x00C354);
    charge(2);
    if ((int16_t)d_reg(5) > 9) { set_d(5, 9); charge(1); }
    charge(2);
    PRINT(vrd32(0xC4E0u + i6), 0x00C368);
    set_d(5, vrd8(G_COST_PER_GAME));
    charge(4);
    if ((int16_t)d_reg(5) > 1) {                  /* "n CREDITS TO START" */
        charge(2);
        PRINT(vrd32(0xC4E8u + i6), 0x00C382);
        charge(2);
        PRINT(vrd32(0xC4F0u + i6), 0x00C38E);
    }
    charge(2);
    if (vrd16(G_SW_BIT5)) goto prompt;
    wr16(W(0x2040), 0);
    set_d(0, vrd8(G_COST_PER_GAME));
    charge(5);
    if (!((int16_t)d_reg(0) > (int16_t)vrd16(G_CREDITS))) {    /* enough credits */
        charge(2);
        PRINT(0xC5CBu, 0x00C3B2);
        wr16(W(0x2040), 0xFFFF);
        charge(2);
        goto prompt;
    }
    /* not enough: say how many more coins, when both slots are worth the
     * same and the arithmetic comes out even */
    charge(2);
    if (vrd8(COIN_MODE_1045) != 1) goto insert_coin;
    set_d(0, vrd8(COIN1_VALUE));
    charge(4);
    if ((d_reg(0) & 0xFF) != vrd8(COIN2_VALUE)) goto insert_coin;
    set_d(1, swap32(divu_w(vrd8(G_COST_PER_GAME), d_reg(0))));
    charge(6);
    if (d_reg(1) & 0xFFFF) goto insert_coin;      /* cost not a whole number of coins */
    set_d(1, swap32(d_reg(1)));
    charge(3);
    if (!(d_reg(1) & 0xFFFF)) goto insert_coin;
    {
        uint32_t d1 = w_lo(vrd8(G_COST_PER_GAME), vrd8(G_COST_PER_GAME) - vrd16(G_CREDITS));
        set_d(1, swap32(divu_w(d1, d_reg(0))));
        charge(7);
        if (d_reg(1) & 0xFFFF) goto insert_coin;
    }
    set_d(1, swap32(d_reg(1)));
    set_d16(5, (uint16_t)d_reg(1));               /* coins still needed */
    charge(4);
    if (vrd16(W(0x7028)) == 1) {
        charge(2);
        if (vrd16(G_CREDITS)) goto need_more;
    }
    charge(2);
    PRINT(0xC582u, 0x00C420);
    charge(2);
    PRINT(0xC58Cu, 0x00C42A);
    charge(2);
    if (vrd16(G_CREDITS)) goto need_more;
    set_a(1, 0xC593u);
    charge(3);
    if ((int16_t)d_reg(5) > 1) { set_a(1, 0xC5A1u); charge(1); }
    charge(1);
    if ((r = rd_call(L_5A0A, 0x00C444))) return r;
    charge(1);
    goto prompt;
need_more:
    wr16(W(0x2040), d_reg(5));
    set_a(1, 0xC5AFu);
    charge(4);
    if ((int16_t)d_reg(5) > 1) { set_a(1, 0xC5BDu); charge(1); }
    charge(2);
    if (vrd16(W(0x7028)) != 1) {
        charge(1);
        if ((r = rd_call(L_5A0A, 0x00C466))) return r;
        charge(1);
    }
    goto prompt;
insert_coin:
    charge(2);
    PRINT(0xC56Fu, 0x00C472);                     /* "INSERT COIN(S)" */
    charge(1);
prompt:
    charge(2);                                    /* tst, beq */
    if (!vrd16(W(0x2040))) { charge(1); return RD_RTS; }
    charge(1);
    if ((r = rd_call(L_E6D6, 0x00C4AE))) return r;
    charge(2);
    if (vrd16(G_SW_BIT5)) { charge(1); return RD_RTS; }
    set_d16(1, (uint16_t)vrd16(W(0x2040)));
    charge(2);
    if (d_reg(1) & 0x8000) { charge(1); return 0xE6CCu; }     /* jmp to an entry: no poll */
    charge(1);
    if ((r = rd_call(L_E6B2, 0x00C4C6))) return r;
    charge(1);
    return RD_RTS;
}

/* FUN_0000df22: advance the text reveal started by FUN_0000defe, in the
 * style 0x2022 & 3 (a jump table at 0xDF38; negative = finished):
 *   0: every 16 frames re-draw the whole string, one more character each
 *      time, right-aligned (from the end), until all are shown
 *   1: every 4 frames drop the next character in from the right, sliding
 *      along 0x200E, until the field is full
 *   2: every 16 frames add the next character on the left end
 *   3: every 16 frames add the next character counting from the right end
 * Characters go through FUN_0000e082 and get the attribute 0x2010. */
static uint32_t ticker_step_body(void)
{
    uint32_t r;
    uint32_t d0 = w_lo(d_reg(0), vrd16(G_TICKER_MODE));
    set_d(0, d0);
    charge(2);
    if (d0 & 0x8000) { charge(1); return RD_RTS; }
    d0 = w_lo(d0, d0 & 3);
    d0 = w_lo(d0, vrd16(0xDF38u + sx16(d0) * 2));
    set_d(0, d0);
    charge(4);
    poll();                                       /* jmp */
    uint32_t t = 0xDF38u + sx16(d0);
    uint32_t mask = (t == 0xDF92u) ? 3 : 0xF;
    wr16(G_TICKER_TIMER, (vrd16(G_TICKER_TIMER) + 1) & mask);
    charge(3);
    if (vrd16(G_TICKER_TIMER)) { charge(1); return RD_RTS; }
    if (t == 0xDF42u) {                           /* mode 0 */
        uint32_t a0 = vrd32(G_TICKER_DEST);
        uint16_t n = (uint16_t)vrd16(G_TICKER_LEN);
        charge(2);
        for (;;) {
            wr16(a0, 0x20); a0 += 2;
            charge(2);
            if (n-- == 0) break;
            set_a(0, a0); set_d16(0, n); poll();
        }
        set_d(0, 0);
        uint32_t d1 = w_lo(d_reg(1), vrd16(G_TICKER_POS));
        uint32_t a1 = vrd32(G_TICKER_SRC) + sx16(d1) + 1;
        charge(4);
        for (;;) {
            a1 -= 1;
            set_d(0, b_lo(d_reg(0), vrd8(a1)));
            set_d(1, d1); set_a(0, a0); set_a(1, a1);
            charge(2);
            if ((r = rd_call(L_E082, 0x00DF72))) return r;
            uint32_t v = w_lo(d_reg(0), d_reg(0) | vrd16(G_TICKER_ATTR));
            set_d(0, v);
            a0 -= 2;
            wr16(a0, v);
            d1 = w_lo(d1, d1 - 1);
            charge(3);
            if ((d1 & 0xFFFF) == 0xFFFF) break;
            set_d(1, d1); set_a(0, a0); set_a(1, a1); poll();
        }
        set_d(1, d1); set_a(0, a0); set_a(1, a1);
        wr16(G_TICKER_POS, vrd16(G_TICKER_POS) + 1);
        uint32_t len = vrd16(G_TICKER_LEN);
        set_d16(0, (uint16_t)len);
        charge(4);
        if (!((int16_t)len >= (int16_t)vrd16(G_TICKER_POS))) { wr16(G_TICKER_MODE, 0xFFFF); charge(1); }
        charge(1);
        return RD_RTS;
    }
    if (t == 0xDF92u) {                           /* mode 1 */
        uint32_t d1 = w_lo(d_reg(1), vrd16(G_TICKER_POS));
        uint32_t a0 = vrd32(G_TICKER_DEST) + sx16(d1) * 2;
        uint16_t n = (uint16_t)vrd16(G_TICKER_LEN);
        charge(4);
        for (;;) {
            wr16(a0, 0x20); a0 += 2;
            charge(2);
            if (n-- == 0) break;
            set_a(0, a0); set_d(1, d1); set_d16(0, n); poll();
        }
        set_d16(0, 0xFFFF);
        uint32_t a1 = vrd32(G_TICKER_SRC);
        set_d(0, b_lo(d_reg(0), vrd8(a1 + sx16(d1))));
        set_d(0, w_lo(d_reg(0), d_reg(0) & 0xFF));
        set_d(1, d1); set_a(0, a0); set_a(1, a1);
        charge(4);
        if ((r = rd_call(L_E082, 0x00DFC6))) return r;
        uint32_t v = w_lo(d_reg(0), d_reg(0) | vrd16(G_TICKER_ATTR));
        set_d(0, v);
        d1 = w_lo(d1, vrd16(G_TICKER_STEP));
        wr16(a0 - 2 + sx16(d1) * 2, v);
        d1 = w_lo(d1, d1 - 1);
        wr16(G_TICKER_STEP, d1);
        d1 = w_lo(d1, (uint32_t)-(int32_t)d1);
        set_d(1, d1);
        charge(8);
        if ((int16_t)d1 <= (int16_t)vrd16(G_TICKER_LEN)) { charge(1); return RD_RTS; }
        wr16(G_TICKER_STEP, 0);
        wr16(G_TICKER_POS, vrd16(G_TICKER_POS) + 1);
        uint16_t len = (uint16_t)(vrd16(G_TICKER_LEN) - 1);
        wr16(G_TICKER_LEN, len);
        charge(4);
        if (!(len & 0x8000)) { charge(1); return RD_RTS; }
        wr16(G_TICKER_MODE, 0xFFFF);
        charge(2);
        return RD_RTS;
    }
    /* modes 2 and 3: one character at index i */
    uint32_t d1;
    if (t == 0xDFF6u) { d1 = w_lo(d_reg(1), vrd16(G_TICKER_POS)); charge(1); }
    else              { d1 = w_lo(d_reg(1), vrd16(G_TICKER_LEN) - vrd16(G_TICKER_POS)); charge(2); }
    uint32_t a0 = vrd32(G_TICKER_DEST) + sx16(d1) * 2;
    uint32_t a1 = vrd32(G_TICKER_SRC) + sx16(d1);
    set_d(0, vrd8(a1));
    set_d(1, d1); set_a(0, a0); set_a(1, a1);
    charge(7);                                    /* movea, lea, movea, lea, moveq, move.b, bsr */
    if ((r = rd_call(L_E082, t == 0xDFF6u ? 0x00E01E : 0x00E064))) return r;
    uint32_t v = w_lo(d_reg(0), d_reg(0) | vrd16(G_TICKER_ATTR));
    set_d(0, v);
    wr16(a0, v);
    charge(2);
    if (t != 0xDFF6u) { d1 = w_lo(d1, vrd16(G_TICKER_POS)); charge(1); }
    d1 = w_lo(d1, d1 + 1);
    set_d(1, d1);
    charge(3);
    if ((int16_t)d1 > (int16_t)vrd16(G_TICKER_LEN)) { wr16(G_TICKER_MODE, 0xFFFF); charge(1); }
    wr16(G_TICKER_POS, d1);
    charge(2);
    return RD_RTS;
}

/* The rts goes through rts_as_68k: FUN_0000df22 writes through the text
 * destination pointer 0x2012, and when that points at the stack (seen under
 * fuzz) the rts pops what was written -- the 68K then jumps there. */
static uint32_t rd_ticker_step(void)
{
    uint32_t r = ticker_step_body();
    return r == RD_RTS ? rts_as_68k() : r;
}

/* FUN_0000e2b0: the race HUD numbers. The lap time 0x232C (capped at 0x1734)
 * as mm:ss:ff (FUN_0000a88e): when its frames digit pair is 0, the seconds in
 * blocks D5 (6, or 8 below 0x259) at 0x10C / 0x106 -- the tens digit shifted
 * by 6 when there are minutes -- and the page handler from 0xE332 by
 * 0x2344 & 1; the speed 0x2088 in BCD (FUN_0000a870) as blocks 0xC/0xD/0xE;
 * FUN_0000fe14; the gear block 0xE3C0[0x221C & 7]; and when the gear changed,
 * the handler from 0xE3D0 by (0xDDE & 1) * 2 + (0x2340 & 1). */
static uint32_t rd_hud_numbers(void)
{
    uint32_t r;
    uint32_t d0 = w_lo(d_reg(0), vrd16(W(0x232C)));
    uint32_t d5 = 6;
    charge(4);
    if (!((int16_t)d0 > 0x258)) { d5 = 8; charge(1); }
    charge(2);
    if ((int16_t)d0 > 0x1734) { d0 = w_lo(d0, 0x1734); charge(1); }
    d0 &= 0xFFFF;
    set_d(0, d0); set_d(1, d0); set_d(5, d5);
    charge(3);
    if ((r = rd_call(L_A88E, 0x00E2D6))) return r;
    uint32_t t0 = d_reg(0);
    set_d(4, t0 & 0xFF);
    charge(3);
    if (!(t0 & 0xFF)) {
        uint32_t d4 = t0 >> 8;
        set_d(4, d4); set_d(0, d_reg(5)); set_d(1, d4 & 0xF); set_d16(3, 0x10C);
        charge(7);
        if ((r = rd_call(L_DBC8, 0x00E2F8))) return r;
        uint32_t d6 = 0;
        d4 = d_reg(4);
        uint32_t d0b = (d4 >> 8) & 0xF;
        set_d(0, d0b);
        charge(5);
        if (d0b) { d6 += 6; charge(1); }
        d4 >>= 4;
        set_d(6, d6); set_d(4, d4); set_d(0, d_reg(5));
        set_d(1, (d4 + d6) & 0xF); set_d16(3, 0x106);
        charge(7);
        if ((r = rd_call(L_DBC8, 0x00E31E))) return r;
        d0 = w_lo(d_reg(0), vrd16(W(0x2344)) & 1);
        uint32_t off = vrd32(0xE332u + sx16(d0) * 4);
        set_d(0, off);
        charge(5);
        if ((r = rd_call_ind(0xE332u + off, 0x00E330, 0x00E32C))) return r;
        charge(1);                                /* bra */
    }
    set_d(0, vrd16(W(0x2088)));
    charge(3);
    if ((r = rd_call(L_A870, 0x00E34A))) return r;
    uint32_t d4 = d_reg(1);
    set_d(4, d4); set_d(0, 0xC); set_d(1, w_lo(d_reg(1), d4) & 0xF);
    charge(5);
    if ((r = rd_call(L_DBC8, 0x00E35A))) return r;
    set_d(0, 0xD); set_d(1, w_lo(d_reg(1), (d_reg(4) & 0xFFFF) >> 4) & 0xF);
    charge(5);
    if ((r = rd_call(L_DBC8, 0x00E36A))) return r;
    set_d(0, 0xE); set_d(1, w_lo(d_reg(1), (d_reg(4) & 0xFFFF) >> 8) & 0xF);
    charge(5);
    if ((r = rd_call(L_DBC8, 0x00E37A))) return r;
    charge(1);
    if ((r = rd_call(L_FE14, 0x00E37E))) return r;
    set_d(0, vrd16(0xE3C0u + (vrd16(G_GEAR_SHOWN) & 7) * 2));
    charge(6);
    if ((r = rd_call(L_DBC8, 0x00E392))) return r;
    uint16_t g = (uint16_t)vrd16(G_GEAR_SHOWN);
    set_d16(0, g);
    charge(3);
    if (g == (uint16_t)vrd16(G_GEAR_PREV)) { charge(1); return RD_RTS; }
    uint32_t sel = (((vrd16(G_STEER_RANGE_2X) & 1) << 1) + (vrd16(W(0x2340)) & 1)) & 3;
    set_d16(1, vrd16(W(0x2340)) & 1);
    uint32_t off = vrd32(0xE3D0u + sel * 4);
    set_d(0, off);
    charge(10);
    if ((r = rd_call_ind(0xE3D0u + off, 0x00E3BE, 0x00E3BA))) return r;
    charge(1);
    return RD_RTS;
}

/* FUN_0000e54c: draw the race HUD frame: block 9 (0x48 when 0x1000106D is
 * set), blocks 0, 0xA, 0xB, block 0x37 with 0x2340, the view/transmission
 * block from the per-page table 0xE606[0x2344 & 1][0x2342 & 3] (plus 0x44 on
 * page 0 with value 3), FUN_0000e4fa on page 0, FUN_0001dc20, FUN_0000fe14,
 * then the handlers from 0xE3E0 and 0xE3D0 by (0xDDE & 1) * 2 + (0x2340 & 1). */
static uint32_t rd_hud_frame(void)
{
    uint32_t r;
    uint32_t d0 = 9;
    charge(3);
    if (vrd8(0x1000106Du)) { d0 = 0x48; charge(1); }
    set_d(0, d0);
    charge(1);
    if ((r = rd_call(L_DBC8, 0x00E55C))) return r;
    DRAW_BLOCK(0, 0x00E562);
    DRAW_BLOCK(0xA, 0x00E568);
    DRAW_BLOCK(0xB, 0x00E56E);
    set_d(0, 0x37); set_d16(1, (uint16_t)vrd16(W(0x2340)));
    charge(3);
    if ((r = rd_call(L_DBC8, 0x00E578))) return r;
    uint32_t d7 = vrd16(W(0x2344)) & 1;
    uint32_t a0 = vrd32(0xE606u + d7 * 4);
    uint32_t d6 = w_lo(d_reg(6), vrd16(W(0x2342)) & 3);
    set_d(7, d7); set_a(0, a0); set_d(6, d6);
    set_d16(0, (uint16_t)vrd16(a0 + sx16(d6) * 2));
    charge(8);
    if ((r = rd_call(L_DBC8, 0x00E598))) return r;
    charge(2);
    if (!(d_reg(7) & 0xFFFF)) {
        charge(2);
        if ((uint16_t)d_reg(6) == 3) DRAW_BLOCK(0x44, 0x00E5A8);
    }
    charge(2);
    if (!vrd16(W(0x2344))) {
        charge(1);
        if ((r = rd_call(L_E4FA, 0x00E5B2))) return r;
    }
    charge(1);
    if ((r = rd_call(L_1DC20, 0x00E5B8))) return r;
    charge(1);
    if ((r = rd_call(L_FE14, 0x00E5BC))) return r;
    static const uint32_t tbl[2] = { 0xE3E0u, 0xE3D0u }, ret[2] = { 0x00E5E0, 0x00E604 }, at[2] = { 0x00E5DA, 0x00E5FE };
    for (int k = 0; k < 2; k++) {
        uint32_t sel = (((vrd16(G_STEER_RANGE_2X) & 1) << 1) + (vrd16(W(0x2340)) & 1)) & 3;
        set_d16(1, vrd16(W(0x2340)) & 1);
        uint32_t off = vrd32(tbl[k] + sel * 4);
        set_d(0, off);
        charge(9);
        if ((r = rd_call_ind(tbl[k] + off, ret[k], at[k]))) return r;
    }
    charge(1);
    return RD_RTS;
}

/* =====================================================================
 * The frame: vblank work
 * ===================================================================== */

#define DSW_HI          0x50000000u   /* byte: DIP switches (bit 0 in 440A)   */
#define DSW_LO          0x50000001u   /* byte: bit 7 skips the sound-CPU handshake */
#define G_SND_TOGGLE    W(0x1156)     /* byte: counts sound-CPU status changes, bit 0 used */
#define G_SND_STATUS    W(0x1157)     /* byte: last sound-CPU status byte seen */
#define G_SND_WAIT      W(0x1154)     /* word: -1 while the frame waits for the sound CPU */
#define G_FLAGS_080D    W(-0x77F3)    /* byte: bit 6 = display buffer 1 in use */

/* FUN_0000440a: the per-frame video work. Polygon RAM word 0 = 1 (busy);
 * unless DSW bit 7, handshake with the sound CPU through its status word
 * 0x60004030 (0x1156/0x1157/0x11AA bit 2/0x1198 bit 2 -> 0x1154 = -1 to
 * skip this frame). Otherwise pick the display-list buffer from polygon RAM
 * 0x70000010 bit 0 (0x70010000 or 0x70018000; flag 0x80D bit 6), set the
 * list pointers 0xC42/0xC46/0xDEA, then the random step, the list header
 * reset (FUN_000046a6), the game frame FUN_0000c052 and FUN_0002b29e; then
 * FUN_0000ac98 unless DSW bit 0, and polygon RAM word 0 = 0. */
static uint32_t rd_frame_video(void)
{
    uint32_t r;
    vwr32(POLY(0), 1);
    charge(3);
    int skip_list = 0;
    if (!(vrd8(DSW_LO) & 0x80)) {
        uint32_t d0 = d_reg(0), d1 = w_lo(d_reg(1), 0);
        d0 = w_lo(d0, vrd16(SHARED(0x30)));
        charge(4);
        int toggle_test = 1;
        if ((d0 & 0xFF) != vrd8(G_SND_STATUS)) {
            d0 = w_lo(d0, d0 & 3);
            charge(3);
            if ((d0 & 0xFFFF) != 1) toggle_test = 0;
            else { wr8(G_SND_TOGGLE, vrd8(G_SND_TOGGLE) + 1); charge(1); }
        }
        int to_446e = 0;
        if (toggle_test) {
            wr8(G_SND_TOGGLE, vrd8(G_SND_TOGGLE) & 1);
            charge(2);
            if (!(vrd8(G_SND_TOGGLE))) {
                wr8(W(0x11AA), vrd8(W(0x11AA)) & ~4u);
                charge(1);
                to_446e = 1;
            } else {
                d1 = w_lo(d1, 0xFFFF);
                wr8(W(0x11AA), vrd8(W(0x11AA)) | 4);
                charge(2);
            }
        }
        if (!to_446e) {
            d0 = w_lo(d0, vrd16(SHARED(0x30)));
            charge(3);
            if ((d0 & 0xFF) == vrd8(G_SND_STATUS)) to_446e = 1;
            else {
                d1 = w_lo(d1, 0);
                d0 = w_lo(d0, d0 & 3);
                charge(4);
                if ((d0 & 0xFFFF) != 2) { charge(1); to_446e = 1; }
            }
        }
        if (to_446e) {
            charge(2);
            if (vrd8(W(0x1198)) & 4) { d1 = w_lo(d1, 0xFFFF); charge(1); }
        }
        wr8(G_SND_STATUS, vrd8(SHARED(0x31)));
        wr16(G_SND_WAIT, d1);
        charge(3);
        set_d(0, d0); set_d(1, d1);
        if (d1 & 0xFFFF) skip_list = 1;
    }
    if (!skip_list) {
        wr8(G_FLAGS_080D, vrd8(G_FLAGS_080D) | 0x40);
        uint32_t d0 = vrd32(POLY(0x10)) & 1;
        charge(5);
        if (!d0) { wr8(G_FLAGS_080D, vrd8(G_FLAGS_080D) & ~0x40u); charge(1); }
        d0 = ((d0 >> 2) | (d0 << 14)) & 0xFFFF;   /* ror.w #2 */
        d0 |= 0x8000;
        uint32_t a0 = POLY(0) + d0 + d0;
        vwr32(G_DLIST_BASE, a0);
        a0 += 0x400;
        vwr32(G_DLIST_PTR, a0);
        vwr32(W(0xDEA), a0);
        set_d(0, d0); set_a(0, a0); set_a(5, A5_SINE_TABLE);
        charge(11);
        if ((r = rd_call(L_F02E, 0x0044D2))) return r;
        charge(1); if ((r = rd_call(L_46A6, 0x0044D6))) return r;
        charge(1); if ((r = rd_call(L_C052, 0x0044DC))) return r;
        charge(1); if ((r = rd_call(L_2B29E, 0x0044E2))) return r;
    }
    charge(2);
    if (!(vrd8(DSW_HI) & 1)) {
        charge(1);
        if ((r = rd_call(L_AC98, 0x0044F2))) return r;
    }
    vwr32(POLY(0), 0);
    charge(2);
    return RD_RTS;
}

#define G_DSP_FRAMES    W(0xC4A)      /* word: frames since the master DSP took a list */

/* FUN_000045aa: per-frame I/O. Unless the frame is skipped (0x1154): when the
 * master DSP has taken the list (polygon RAM 0x70000004 low word 0), flip
 * the list buffer (0x70000010 bit 0), latch 0x7000005C into 0xDF6 and set
 * 0x70000004 = 1; count 0xC4A and after 32 frames without, restart the
 * DSPs (FUN_00004684). Then the tilemap scroll 0x4740 & 0x3FE, the LEDs,
 * inputs, input-sequence check, the 0x816 block, switches and coins, the
 * sound flag -- and count 0x808. */
static uint32_t rd_frame_io(void)
{
    uint32_t r;
    uint32_t d0 = d_reg(0);
    charge(2);
    if (!vrd16(G_SND_WAIT)) {
        d0 = vrd32(POLY(4));
        charge(3);
        if (!(d0 & 0xFFFF)) {
            wr16(G_DSP_FRAMES, 0);
            d0 = vrd32(POLY(0x10));
            d0 = w_lo(d0, d0 & 1);
            d0 = w_lo(d0, d0 ^ 1);
            vwr32(POLY(0x10), d0);
            vwr32(W(0xDF6), vrd32(POLY(0x5C)));
            vwr32(POLY(4), 1);
            charge(7);
        }
        wr16(G_DSP_FRAMES, vrd16(G_DSP_FRAMES) + 1);
        charge(3);
        set_d(0, d0);
        if ((uint16_t)vrd16(G_DSP_FRAMES) >= 0x20) {
            charge(1);
            if ((r = rd_call(L_4684, 0x0045F4))) return r;
        }
    }
    d0 = w_lo(d_reg(0), vrd16(W(0x4740)) & 0x3FE);
    wr16(0x900A0002u, d0);
    set_d(0, d0);
    charge(4);
    if ((r = rd_call(L_4640, 0x004606))) return r;
    charge(1); if ((r = rd_call(L_47D0, 0x00460A))) return r;
    charge(1); if ((r = rd_call(L_4988, 0x00460E))) return r;
    charge(1); if ((r = rd_call(L_494C, 0x004612))) return r;
    charge(1); if ((r = rd_call(L_C150, 0x004618))) return r;
    charge(1); if ((r = rd_call(L_4CFA, 0x00461C))) return r;
    wr16(W(-0x77F8), vrd16(W(-0x77F8)) + 1);
    charge(2);
    return RD_RTS;
}

/* =====================================================================
 * Display-list entries for the start screens
 * ===================================================================== */

/* a display-list writer: A3 and the three work registers of the pattern */
typedef struct { uint32_t a3, d0, d1, d2; } dl_t;
static void dl_put(dl_t *s, uint32_t v) { vwr32(s->a3, v); s->a3 += 4; }
/* the angle block every entry carries (8 instructions for a move.w #imm
 * start, and the three pairs):  D0w = angle & ~1, D1w = -table[D0w],
 * D2w = table[D0w + 0x4000], written as (D1, D2) `pairs` times */
static void dl_angle(dl_t *s, uint32_t angle, int pairs)
{
    uint32_t a5 = a_reg(5);
    s->d0 = w_lo(s->d0, angle & 0xFFFE);
    s->d1 = w_lo(s->d1, vrd16(a5 + sx16(s->d0)));
    s->d0 = w_lo(s->d0, s->d0 + 0x4000);
    s->d2 = w_lo(s->d2, vrd16(a5 + sx16(s->d0)));
    s->d1 = w_lo(s->d1, (uint32_t)-(int32_t)s->d1);
    for (int k = 0; k < pairs; k++) { dl_put(s, s->d1); dl_put(s, s->d2); }
}
/* moveq #0,D0 ; (angle 0) ; three pairs ; move.l #0: 13 instructions */
static void dl_rot0(dl_t *s)
{
    s->d0 = 0;
    dl_angle(s, 0, 3);
    dl_put(s, 0);
    charge(13);
}

#define G_CAR_X(i)  (W(0x2356) + (uint32_t)(i) * 4)   /* long: x of start-screen car i (0..3) */
#define G_CAR_Z(i)  (W(0x2366) + (uint32_t)(i) * 4)   /* long: z */

/* FUN_0001121a: object block 6 (a transmission/view selector display, the
 * block that FUN_00011d2a sets up): a 0x8002 header with block number
 * 0xC40, the fixed entry (0x6B7, 0, 0x75C, 0x15A0, angles 0x280/0/0, 0),
 * two entries left (x -0x5C0) and two right (x 0x5C0) with models from the
 * ROM pairs 0x113CC / 0x113D4 chosen by 0x2340 & 1, and one entry whose
 * model is 0x113DC[(0x2034 >> 2) & 1] and position the ROM triple 0x113E0
 * + (0x2340 & 1) * 8; three empty 0x8001 records and -1; then
 * FUN_0000b56e. 0x238E = 1. */
static uint32_t rd_select_display(void)
{
    uint32_t r;
    wr16(W(0xC40), 6);
    wr16(W(0x238E), 1);
    charge(3);
    if ((r = rd_call(L_11D2A, 0x01122A))) return r;
    dl_t s = { vrd32(G_DLIST_PTR), 0, 0, 0 };
    dl_put(&s, 0x8002);
    s.d1 = w_lo(s.d1, vrd16(W(0xC40)));
    dl_put(&s, s.d1);
    dl_put(&s, 0x6B7); dl_put(&s, 0); dl_put(&s, 0x75C); dl_put(&s, 0x15A0);
    charge(11);
    dl_angle(&s, 0x280, 1); dl_angle(&s, 0, 1); dl_angle(&s, 0, 1);
    dl_put(&s, 0);
    charge(25);
    static const uint32_t tbl[2] = { 0x113CCu, 0x113D4u };
    static const int32_t xs[2] = { -0x5C0, 0x5C0 };
    uint32_t a0 = 0;
    for (int side = 0; side < 2; side++) {
        uint32_t d7 = 1;
        uint32_t sel = side == 0 ? (vrd16(W(0x2340)) & 1) : ((vrd16(W(0x2340)) + 1) & 1);
        s.d0 = w_lo(s.d0, sel);
        a0 = tbl[side] + sel * 4;
        charge(side == 0 ? 4 : 5);
        for (;;) {
            s.d0 = vrd16(a0 + d7 * 2);
            dl_put(&s, s.d0);
            dl_put(&s, (uint32_t)xs[side]); dl_put(&s, 0x160); dl_put(&s, 0x15C0);
            s.d0 = 0;
            dl_angle(&s, 0, 3);
            dl_put(&s, 0);
            d7 = w_lo(d7, d7 - 1);
            charge(20);
            if ((d7 & 0xFFFF) == 0xFFFF) break;
            set_d(0, s.d0); set_d(1, s.d1); set_d(2, s.d2); set_a(3, s.a3); set_a(0, a0); set_d(7, d7); poll();
        }
        set_d(7, d7);
    }
    s.d0 = 0; s.d1 = 0;
    s.d0 = w_lo(s.d0, (vrd16(G_SW_SNAPSHOT) >> 2) & 1);
    s.d1 = w_lo(s.d1, vrd16(0x113DCu + s.d0 * 2));
    dl_put(&s, s.d1);
    s.d0 = w_lo(s.d0, vrd16(W(0x2340)) & 1);
    for (uint32_t k = 0; k < 3; k++) {
        s.d1 = sx16(vrd16(0x113E0u + k * 2 + s.d0 * 8));
        dl_put(&s, s.d1);
    }
    dl_put(&s, 0);
    for (int k = 0; k < 3; k++) { dl_put(&s, 0x8001); dl_put(&s, 0); }
    vwr32(s.a3, 0xFFFFFFFFu);
    vwr32(G_DLIST_PTR, s.a3);
    set_d(0, s.d0); set_d(1, s.d1); set_d(2, s.d2);
    set_a(0, a0); set_a(3, s.a3);
    charge(8 + 14 + 8 + 2);
    if ((r = rd_call(L_B56E, 0x0113CA))) return r;
    charge(1);
    return RD_RTS;
}

/* one car-screen entry: model word, x from 0x2356[i], y, z from 0x2366[i],
 * and the zero angles (the move.l D0 .. move.l #0 run: 4 + 13 instructions) */
static void car_entry(dl_t *s, uint32_t model_d0, uint32_t y, uint32_t i)
{
    dl_put(s, model_d0);
    dl_put(s, vrd32(G_CAR_X(i)));
    dl_put(s, y);
    dl_put(s, vrd32(G_CAR_Z(i)));
    charge(4);
    dl_rot0(s);
}

/* FUN_000113f0: the car-select screen's display list (object block 6): for
 * each of the four cars i = 3..0, at (0x2356[i], y, 0x2366[i]), with the
 * model set from ROM table 0x116B6 by page (0x2344 & 1) and the variant
 * D5 (0 for the selected car 0x2342 & 3, else 1):
 *   the body parts at +0, +0x10, +0x20; on page 1 a part from +0x40 (or
 *   +0x50 when 0x7028 is set; +0x60 for gears >= 4) at y -0x4D0; with
 *   0x1000106D set a part from +0x30; a colour part by the car's byte in
 *   0x10001065.. (0x1176E table); and three wheel parts from 0x11782 by the
 *   per-car nibbles of 0x117BA's table, offset in x by 0x117AA[k].
 * Terminated with -1; then FUN_0000b56e. */
static uint32_t rd_car_select_list(void)
{
    uint32_t r;
    wr16(W(0xC40), 6);
    dl_t s = { vrd32(G_DLIST_PTR), 0, 0, 0 };
    dl_put(&s, 0x8002);
    s.d1 = w_lo(s.d1, vrd16(W(0xC40)));
    dl_put(&s, s.d1);
    s.d0 = w_lo(s.d0, vrd16(W(0x2344)) & 1);
    uint32_t a0 = vrd32(0x116B6u + s.d0 * 4);
    s.d0 = 0; s.d1 = 0;
    uint32_t d7 = 3, d5 = d_reg(5), d4 = d_reg(4), d3 = d_reg(3), a1 = 0, a2 = 0, a4 = a_reg(4);
    charge(14);
    for (;;) {
        a1 = a0 + sx16(d7) * 4;
        d5 = w_lo(d5, vrd16(W(0x2342)) & 3);
        d5 = w_lo(d5, d5 - d7);
        charge(5);
        if (d5 & 0xFFFF) { d5 = 1; charge(1); }
        for (uint32_t part = 0; part < 3; part++) {
            s.d0 = vrd16(a1 + part * 0x10 + sx16(d5) * 2);
            charge(4);                            /* moveq, move, nop, nop */
            car_entry(&s, s.d0, 0x200, d7);
        }
        charge(2);
        if (vrd16(W(0x2344))) {
            s.d0 = vrd16(a1 + 0x40 + sx16(d5) * 2);
            charge(6);
            if (vrd16(W(0x7028))) { s.d0 = w_lo(s.d0, vrd16(a1 + 0x50 + sx16(d5) * 2)); charge(3); }
            d4 = w_lo(d4, vrd16(W(0x2342)) - 4);
            charge(3);
            if (!(d4 & 0x8000)) { s.d0 = vrd16(a1 + 0x60 + sx16(d5) * 2); charge(4); }
            car_entry(&s, s.d0, 0xFFFFFB30u, d7);
        }
        charge(2);
        if (vrd8(0x1000106Du)) {
            s.d0 = vrd16(a1 + 0x30 + sx16(d5) * 2);
            charge(4);
            car_entry(&s, s.d0, 0x200, d7);
        }
        a2 = 0x10001060u;
        s.d0 = w_lo(s.d0, ((((vrd16(W(0x2344)) & 1) << 2) + d7 + 5) & 0xFFFF));
        d4 = b_lo(d4, vrd8(a2 + sx16(s.d0)));
        d4 = w_lo(d4, d4 & 7);
        s.d0 = 0;
        a2 = 0x1176Eu + sx16(d4) * 4;
        s.d0 = vrd16(a2 + sx16(d5) * 2);
        charge(11);
        car_entry(&s, s.d0, 0x200, d7);
        s.d0 = b_lo(s.d0, vrd8(0x1000106Du));
        s.d0 = w_lo(s.d0, s.d0 & 1);
        s.d0 = w_lo(s.d0, ((s.d0 << 1) + vrd16(W(0x2344))) & 3);
        a2 = vrd32(0x117BAu + sx16(s.d0) * 4);
        d3 = 0x10;
        s.d0 = w_lo(s.d0, vrd16(W(0x2342)) - 4);
        charge(10);
        if (s.d0 & 0x8000) { d3 = 0; charge(1); }
        d4 = 2;
        charge(1);
        for (;;) {
            s.d0 = w_lo(s.d0, ((d7 << 2) + d4 + d3) & 0xFFFF);
            s.d0 = b_lo(s.d0, vrd8(a2 + sx16(s.d0)));
            s.d0 = w_lo(s.d0, s.d0 & 0xF);
            a4 = 0x11782u + sx16(s.d0) * 4;
            s.d0 = vrd16(a4 + sx16(d5) * 2);
            dl_put(&s, s.d0);
            s.d0 = vrd32(G_CAR_X(sx16(d7))) + vrd32(0x117AAu + sx16(d4) * 4);
            dl_put(&s, s.d0);
            dl_put(&s, 0x200);
            dl_put(&s, vrd32(G_CAR_Z(sx16(d7))));
            charge(15);
            dl_rot0(&s);
            d4 = w_lo(d4, d4 - 1);
            charge(1);
            if ((d4 & 0xFFFF) == 0xFFFF) break;
            set_d(0, s.d0); set_d(1, s.d1); set_d(2, s.d2); set_d(3, d3); set_d(4, d4); set_d(5, d5); set_d(7, d7); set_a(0, a0); set_a(1, a1); set_a(2, a2); set_a(3, s.a3); set_a(4, a4); poll();
        }
        d7 = w_lo(d7, d7 - 1);
        charge(1);
        if ((d7 & 0xFFFF) == 0xFFFF) break;
        set_d(0, s.d0); set_d(1, s.d1); set_d(2, s.d2); set_d(3, d3); set_d(4, d4); set_d(5, d5); set_d(7, d7); set_a(0, a0); set_a(1, a1); set_a(2, a2); set_a(3, s.a3); set_a(4, a4); poll();
    }
    vwr32(s.a3, 0xFFFFFFFFu);
    vwr32(G_DLIST_PTR, s.a3);
    set_d(0, s.d0); set_d(1, s.d1); set_d(2, s.d2); set_d(3, d3); set_d(4, d4);
    set_d(5, d5); set_d(7, d7);
    set_a(0, a0); set_a(1, a1); set_a(2, a2); set_a(3, s.a3); set_a(4, a4);
    charge(3);
    if ((r = rd_call(L_B56E, 0x0116B4))) return r;
    charge(1);
    return RD_RTS;
}

/* =====================================================================
 * FUN_0000fb1c: the attract / title / selection STATE MACHINE. Each frame:
 * count the frame (long 0x5206), run FUN_00022088 unless the byte at
 * 0x10001071 is set, then dispatch on the state word 0x2332 & 0x3F through
 * the 64-entry offset table at 0xFB40 (a computed jmp: it polls, and D0 is
 * left holding the table entry). Most states run a few screen helpers,
 * count the timer word 0x2346 down (or until the START edge, bit 7 of
 * 0x10000812) and step 0x2332. The eight 16-byte player records at
 * 0x111B4 + n*0x10 (flag byte +0, word +0x11AE-0x11B4, word +0x11B2-...)
 * are scanned by several states.
 * ===================================================================== */
#define FB_STATE   W(0x2332)    /* word: the state (dispatch index & 0x3F)          */
#define FB_COUNT   W(0x234E)    /* word: a per-state step counter                   */
#define FB_TIMER   W(0x2346)    /* word: frames left in the current screen          */
#define FB_START   W(-0x77EE)   /* byte: bit 7 = START pressed this frame           */
#define FB_REC(o, d) (W(o) + sx16(d))     /* (o,A6,D0w): a player record field     */
#define FB_CALL(fn, ret) do { if ((r = rd_call(fn, ret))) return r; } while (0)
static inline void fb_addw(uint32_t a, int n) { wr16(a, vrd16(a) + (uint32_t)n); }
static inline void fb_bset(uint32_t a, int b) { wr8(a, vrd8(a) | (1u << b)); }
static inline void fb_bclr(uint32_t a, int b) { wr8(a, vrd8(a) & ~(1u << b)); }
static inline int  fb_btst(uint32_t a, int b) { return (int)((vrd8(a) >> b) & 1u); }
static inline int16_t d16s(int n)             { return (int16_t)d_reg(n); }
static inline uint16_t d16u(int n)            { return (uint16_t)d_reg(n); }
/* dbf Dn at the foot of a loop (charged with the instruction before it):
 * returns 1 when the loop ends, else polls (the taken backward branch) */
static int fb_dbf(int n, int ch)
{
    uint16_t v = (uint16_t)(d_reg(n) - 1);
    set_d16(n, v);
    charge(ch);
    if (v == 0xFFFF) return 1;
    poll();
    return 0;
}
/* "subq.w #1,timer; beq go; btst #7,START; beq skip": 1 = the screen ends */
static int fb_timer_or_start(void)
{
    uint16_t t = (uint16_t)(vrd16(FB_TIMER) - 1);
    wr16(FB_TIMER, t);
    charge(2);
    if (t == 0) return 1;
    charge(2);
    return fb_btst(FB_START, 7);
}
/* 0x1076C..0x107B2 and 0x10862..0x108A8 (identical): D1w = 1 + the largest
 * word +0x11AE among the records with any of flag bits 1-3 set, skipping the
 * record at offset 0x115A; 0x11A2 is raised to it. */
static void fb_scan_max(void)
{
    set_d(6, 7); set_d(0, 0); set_d(1, 0); set_d(2, 0); set_d(3, 0xE);
    charge(8);
    do {
        uint32_t d0 = d_reg(0);
        set_d8(2, vrd8(FB_REC(0x11B4, d0)) & d_reg(3));
        charge(3);
        if (d_reg(2) & 0xFF) {
            charge(2);
            if ((uint16_t)d0 != vrd16(W(0x115A))) {
                uint16_t m = (uint16_t)vrd16(FB_REC(0x11AE, d0));
                charge(2);
                if (d16u(1) < m) { set_d16(1, m); charge(1); }
            }
        }
        set_d16(0, (uint16_t)(d0 + 0x10));
    } while (!fb_dbf(6, 2));
    for (;;) {                                   /* addq.w #1,D1; beq back */
        set_d16(1, (uint16_t)(d_reg(1) + 1));
        charge(2);
        if (d16u(1)) break;
        poll();
    }
    charge(2);
    if (!(d16u(1) < (uint16_t)vrd16(W(0x11A2)))) { wr16(W(0x11A2), d_reg(1)); charge(1); }
}

/* state 0 */
static uint32_t fb_fc42(void)
{
    uint32_t r;
    if (vrd8(0x10001071u)) charge(2);
    else if (vrd8(0x10001071u)) charge(4);
    else {
        vwr32(a_reg(7) - 4, d_reg(7));           /* move.l D7,-(SP) ... (SP)+,D7 */
        wr16(SHARED(0x20), vrd16(SHARED(0x20)) | 8u);
        wr16(W(0x48BA), 0x3C);
        charge(10);
    }
    wr16(FB_COUNT, 0);
    fb_addw(FB_STATE, 1);
    charge(3); FB_CALL(L_F9CA, 0x00FC78);
    charge(1); FB_CALL(L_2872E, 0x00FC7E);
    charge(1);
    return RD_RTS;
}
/* state 2: set up, then wait until some record has flag bits 0/1 */
static uint32_t fb_fc80(void)
{
    uint32_t r;
    charge(1); FB_CALL(L_ECCE, 0x00FC86);
    wr16(W(0x4740), 0);
    charge(2); FB_CALL(L_10F26, 0x00FC90);
    wr16(W(0xDE2), 2);
    charge(2); FB_CALL(L_C69A, 0x00FC9C);
    set_d(0, 0); set_d(1, 1);
    wr16(W(0x2340), 0); wr16(W(0x2344), 0); wr16(W(0x2342), 1);
    wr8(W(0x11AB), 1);
    set_d8(0, (uint8_t)vrd8(0x10001061u));
    set_d16(0, (uint16_t)(d_reg(0) & 7));
    wr8(W(0x11A5), d_reg(0));
    charge(9);
    for (;;) {                                   /* FCBE */
        set_d(6, 7); set_d(0, 0); set_d(1, 0); set_d(4, 0); set_d(5, 0); set_d(7, 0);
        set_d(2, 3);
        charge(9);
        do {                                     /* FCD4 */
            uint32_t d0 = d_reg(0);
            charge(2);
            if (fb_btst(FB_REC(0x11B4, d0), 6)) {
                set_d(4, 1);
                set_d16(5, (uint16_t)vrd16(FB_REC(0x11AE, d0)));
                set_d16(7, (uint16_t)vrd16(FB_REC(0x11B2, d0)));
                charge(3);
            }
            set_d8(3, vrd8(FB_REC(0x11B4, d0)) & d_reg(2));
            charge(3);
            if (d_reg(3) & 0xFF) { set_d16(1, (uint16_t)(d_reg(1) + 1)); charge(1); }
            set_d16(0, (uint16_t)(d0 + 0x10));
        } while (!fb_dbf(6, 2));
        charge(2);
        if (d16u(1)) break;
        poll();
    }
    set_d16(1, (uint16_t)(d_reg(1) - 1));
    charge(2);
    int pick;
    if (!d16u(1)) { wr16(W(0x2344), 1); charge(2); pick = 1; }
    else { fb_addw(FB_STATE, 1); charge(3); pick = d16u(4) != 0; }
    if (pick) {
        set_d16(0, (uint16_t)vrd16(W(0x2344)));
        uint32_t ix = sx16(d_reg(0)) * 2;
        wr16(W(0x1100), vrd16(0xFDA4u + ix));
        wr16(FB_STATE, vrd16(0xFDA0u + ix));
        charge(4);
    }
    wr16(W(0x11A2), d_reg(5)); wr16(W(0x2348), d_reg(7)); wr16(W(0x11A6), d_reg(7));
    charge(4);
    return RD_RTS;
}
/* state 3 */
static uint32_t fb_fd3a(void)
{
    uint32_t r;
    charge(1); FB_CALL(L_575A, 0x00FD40);
    charge(1); FB_CALL(L_E8E4, 0x00FD46);
    charge(1); FB_CALL(L_104DC, 0x00FD4A);
    wr16(W(0x2228), vrd16(W(0x2226)));
    wr16(FB_TIMER, 0x1A4);
    fb_addw(FB_STATE, 1);
    charge(4);
    return RD_RTS;
}
/* state 4 */
static uint32_t fb_fd5c(void)
{
    uint32_t r;
    charge(1); FB_CALL(L_10FE4, 0x00FD60);
    charge(1); FB_CALL(L_10450, 0x00FD64);
    if (fb_timer_or_start()) {
        set_d16(0, (uint16_t)(vrd16(W(0x2344)) & 1));
        uint32_t ix = sx16(d_reg(0)) * 2;
        wr16(W(0x1100), vrd16(0xFDA4u + ix));
        wr16(FB_STATE, vrd16(0xFDA0u + ix));
        charge(7); FB_CALL(L_575A, 0x00FD90);
        charge(1); FB_CALL(L_286FC, 0x00FD96);
    }
    set_d16(1, (uint16_t)vrd16(FB_TIMER));
    charge(2);
    return RD_JMP(0xE88Eu);
}
/* state 60 */
static uint32_t fb_fda8(void)
{
    wr16(W(0x11A4), 0);
    wr8(W(0x11AB), 0);
    fb_bclr(W(0x11A8), 7);
    fb_bclr(W(0x11A8), 2);
    fb_bset(W(0x11A8), 3);
    wr16(W(0xDA0), 3);
    wr16(FB_STATE, 0);
    charge(8);
    return RD_RTS;
}
/* states 16, 19, 48 */
static uint32_t fb_fdce(void)
{
    fb_bclr(W(0x11A8), 1);
    fb_bset(W(0x11A8), 2);
    fb_addw(FB_STATE, 1);
    charge(4);
    return RD_RTS;
}
/* states 26, 58 */
static uint32_t fb_fde0(void)
{
    uint32_t r;
    charge(1); FB_CALL(L_EFEE, 0x00FDE6);
    wr16(FB_STATE, 0x3C);
    charge(2);
    return RD_RTS;
}
/* state 17 */
static uint32_t fb_105f6(void)
{
    uint32_t r;
    charge(1); FB_CALL(L_575A, 0x0105FC);
    charge(1); FB_CALL(L_E8FA, 0x010602);
    charge(1); FB_CALL(L_FDEE, 0x010606);
    wr16(W(0x2222), vrd16(W(0x2220)));
    wr16(W(0x2224), 7);
    charge(3); FB_CALL(L_104DC, 0x010616);
    wr16(W(0x2228), vrd16(W(0x2226)));
    wr16(FB_TIMER, 0x1A4);
    fb_addw(FB_STATE, 1);
    charge(4);
    return RD_RTS;
}
/* state 18 */
static uint32_t fb_10628(void)
{
    uint32_t r;
    charge(1); FB_CALL(L_28786, 0x01062E);
    charge(1); FB_CALL(L_1121A, 0x010632);
    charge(1); FB_CALL(L_103D4, 0x010636);
    charge(1); FB_CALL(L_10462, 0x01063A);
    if (fb_timer_or_start()) {
        charge(1); FB_CALL(L_103DC, 0x01064C);
        charge(1); FB_CALL(L_286FC, 0x010652);
        wr16(FB_STATE, 0x16);
        charge(1);
    }
    set_d16(1, (uint16_t)vrd16(FB_TIMER));
    charge(2);
    return RD_JMP(0xE88Eu);
}
/* state 20 */
static uint32_t fb_10662(void)
{
    uint32_t r;
    charge(1); FB_CALL(L_1765E, 0x010668);
    charge(1); FB_CALL(L_575A, 0x01066E);
    charge(1); FB_CALL(L_E9C4, 0x010674);
    wr16(FB_TIMER, 0xF0);
    fb_addw(FB_STATE, 1);
    charge(3);
    return RD_RTS;
}
/* state 21 */
static uint32_t fb_10680(void)
{
    uint32_t r;
    charge(1); FB_CALL(L_1182E, 0x010684);
    charge(2); FB_CALL(L_176B2, 0x01068C);
    uint16_t t = (uint16_t)(vrd16(FB_TIMER) - 1);
    wr16(FB_TIMER, t);
    charge(2);
    if (t == 0) { fb_addw(FB_STATE, 1); charge(1); }
    charge(1);
    return RD_RTS;
}
/* state 22 */
static uint32_t fb_10698(void)
{
    uint32_t r;
    charge(1); FB_CALL(L_575A, 0x01069E);
    charge(1); FB_CALL(L_E92A, 0x0106A4);
    charge(1); FB_CALL(L_FE80, 0x0106A8);
    charge(1); FB_CALL(L_104DC, 0x0106AC);
    wr16(W(0x2228), vrd16(W(0x2226)));
    wr16(FB_TIMER, 0x258);
    fb_addw(FB_STATE, 1);
    charge(4);
    return RD_RTS;
}
/* state 23 */
static uint32_t fb_106be(void)
{
    uint32_t r;
    charge(1); FB_CALL(L_E940, 0x0106C4);
    charge(1); FB_CALL(L_113F0, 0x0106C8);
    wr16(W(0x238E), 2);
    charge(2); FB_CALL(L_11D2A, 0x0106D2);
    charge(1); FB_CALL(L_11E12, 0x0106D6);
    charge(1); FB_CALL(L_10474, 0x0106DA);
    charge(1); FB_CALL(L_FE80, 0x0106DE);
    if (fb_timer_or_start()) {
        set_d16(0, (uint16_t)(vrd16(W(0x2342)) & 7));
        wr16(W(0x2046), vrd16(0x10720u + sx16(d_reg(0)) * 2));
        wr16(FB_TIMER, 0x78);
        charge(6); FB_CALL(L_575A, 0x010708);
        charge(1); FB_CALL(L_286FC, 0x01070E);
        fb_addw(FB_STATE, 1);
        charge(2);
    } else {
        set_d16(1, (uint16_t)vrd16(FB_TIMER));
        charge(2); FB_CALL(L_E88E, 0x01071E);
    }
    charge(1);
    return RD_RTS;
}
/* state 24 */
static uint32_t fb_10730(void)
{
    uint32_t r;
    charge(1); FB_CALL(L_113F0, 0x010734);
    wr16(W(0x238E), 3);
    charge(2); FB_CALL(L_11D2A, 0x01073E);
    uint16_t t = (uint16_t)(vrd16(FB_TIMER) - 1);
    wr16(FB_TIMER, t);
    charge(2);
    if (t == 0) { fb_addw(FB_STATE, 1); wr16(FB_COUNT, 0); charge(2); }
    charge(1);
    return 0x11CF2u;                             /* bra.w: forward, into FUN_00011cf2 */
}
/* state 25 */
static uint32_t fb_10750(void)
{
    charge(2);
    if ((int16_t)vrd16(FB_COUNT) >= 8) {
        fb_addw(FB_STATE, 1);
        wr16(FB_COUNT, 0xFFFF);
        charge(4);
        if (vrd16(W(0x11A2)) != 0) goto done;
        fb_addw(FB_STATE, -1);
        charge(1);
    }
    fb_scan_max();
done:
    fb_addw(FB_COUNT, 1);
    charge(2);
    return RD_RTS;
}
/* state 32 */
static uint32_t fb_107bc(void)
{
    set_d(6, 7); set_d(0, 0);
    charge(2);
    int found = 0;
    do {
        uint32_t d0 = d_reg(0);
        charge(2);
        if (fb_btst(FB_REC(0x11B4, d0), 6)) { found = 1; break; }
        set_d16(0, (uint16_t)(d0 + 0x10));
    } while (!fb_dbf(6, 2));
    if (!found) { fb_bset(W(0x11A8), 6); charge(1); }
    wr16(FB_COUNT, 0);
    fb_addw(FB_STATE, 1);
    charge(3);
    return RD_RTS;
}
/* states 1, 33, 38 */
static uint32_t fb_107e2(void)
{
    charge(2);
    if ((int16_t)vrd16(FB_COUNT) >= 8) { fb_addw(FB_STATE, 1); wr16(FB_COUNT, 0xFFFF); charge(2); }
    fb_addw(FB_COUNT, 1);
    charge(2);
    return RD_RTS;
}
/* state 34 */
static uint32_t fb_107fa(void)
{
    charge(2);
    if (fb_btst(W(0x11A8), 6)) {
        set_d(6, 7); set_d(0, 0); set_d16(1, (uint16_t)vrd16(W(0x115A)));
        charge(3);
        do {
            uint32_t d0 = d_reg(0);
            charge(2);
            if (d16u(1) != (uint16_t)d0) {
                charge(2);
                if (fb_btst(FB_REC(0x11B4, d0), 6)) {
                    charge(2);
                    if (!(d16s(1) < (int16_t)d0)) { fb_bclr(W(0x11A8), 6); charge(1); }
                }
            }
            set_d16(0, (uint16_t)(d0 + 0x10));
        } while (!fb_dbf(6, 2));
    }
    wr16(FB_COUNT, 0);
    fb_addw(FB_STATE, 1);
    charge(3);
    return RD_RTS;
}
/* state 35 */
static uint32_t fb_10834(void)
{
    charge(2);
    if ((int16_t)vrd16(FB_COUNT) >= 8) {
        fb_addw(FB_STATE, 1);
        wr16(FB_COUNT, 0xFFFF);
        charge(4);
        if (vrd16(W(0x11A2)) != 0) goto done;
        charge(2);
        if (!fb_btst(W(0x11A8), 6)) goto done;
        wr16(FB_STATE, 0x20);
        charge(1);
    }
    charge(2);
    if (!fb_btst(W(0x11A8), 6)) goto done;
    fb_scan_max();
done:
    fb_addw(FB_COUNT, 1);
    charge(2);
    return RD_RTS;
}
/* state 36 */
static uint32_t fb_108b2(void)
{
    charge(2);
    if ((int16_t)vrd16(FB_COUNT) >= 8) {
        fb_addw(FB_STATE, 1);
        wr16(FB_COUNT, 0xFFFF);
        charge(4);
        if (vrd16(W(0x11A2)) != 0) goto done;
        wr16(FB_STATE, 0x20);
        charge(1);
    }
    charge(2);
    if (fb_btst(W(0x11A8), 6)) goto done;
    set_d(6, 7); set_d(0, 0);
    charge(2);
    do {
        uint32_t d0 = d_reg(0);
        charge(2);
        if (fb_btst(FB_REC(0x11B4, d0), 6)) {
            set_d16(1, (uint16_t)vrd16(FB_REC(0x11AE, d0)));
            charge(3);
            if (!(d16u(1) < (uint16_t)vrd16(W(0x11A2)))) {
                wr16(W(0x11A2), d_reg(1));
                charge(2);
                if (d16u(1)) goto done;
            }
        }
        set_d16(0, (uint16_t)(d0 + 0x10));
    } while (!fb_dbf(6, 2));
done:
    fb_addw(FB_COUNT, 1);
    charge(2);
    return RD_RTS;
}
/* state 37 */
static uint32_t fb_10906(void)
{
    charge(2);
    if (fb_btst(W(0x11A8), 6)) {
        fb_bset(W(0x11A8), 5);
        wr16(W(0x2348), 0x2D0);
        wr16(W(0x11A6), vrd16(W(0x2348)));
        wr16(FB_TIMER, 0x1A4);
        charge(4);
    }
    wr16(FB_COUNT, 0);
    fb_addw(FB_STATE, 1);
    charge(3);
    return RD_RTS;
}
/* state 39 */
static uint32_t fb_10930(void)
{
    charge(2);
    if (!fb_btst(W(0x11A8), 6)) {
        set_d(6, 7); set_d(0, 0);
        charge(2);
        int found = 0;
        do {
            uint32_t d0 = d_reg(0);
            charge(2);
            if (fb_btst(FB_REC(0x11B4, d0), 6)) {
                set_d16(1, (uint16_t)vrd16(FB_REC(0x11B2, d0)));
                wr16(W(0x11A6), d_reg(1));
                wr16(W(0x2348), d_reg(1));
                charge(4);
                found = 1;
                break;
            }
            set_d16(0, (uint16_t)(d0 + 0x10));
        } while (!fb_dbf(6, 2));
        if (!found) {
            fb_addw(FB_STATE, -1);
            fb_addw(FB_COUNT, 1);
            charge(4);
            if ((int16_t)vrd16(FB_COUNT) >= 8) { wr16(FB_COUNT, 0); fb_addw(FB_STATE, 1); charge(2); }
        }
    }
    fb_addw(FB_STATE, 1);
    charge(2);
    return RD_RTS;
}
/* state 40 */
static uint32_t fb_1097c(void)
{
    uint32_t r;
    charge(1); FB_CALL(L_575A, 0x010982);
    charge(1); FB_CALL(L_E8FA, 0x010988);
    charge(1); FB_CALL(L_FDEE, 0x01098C);
    wr16(W(0x2222), vrd16(W(0x2220)));
    wr16(W(0x2224), 7);
    charge(3); FB_CALL(L_104DC, 0x01099C);
    wr16(W(0x2228), vrd16(W(0x2226)));
    charge(3);
    if (!fb_btst(W(0x11A8), 6)) {
        set_d16(1, (uint16_t)vrd16(W(0x2348)));
        charge(3);
        if (d16s(1) < 0xF0) { set_d16(1, 0xF0); charge(1); }
        charge(2);
        if (d16s(1) > 0x1A4) { set_d16(1, 0x1A4); charge(1); }
        wr16(FB_TIMER, d_reg(1));
        charge(1);
    }
    fb_addw(FB_STATE, 1);
    charge(2);
    return RD_RTS;
}
/* state 41 */
static uint32_t fb_109cc(void)
{
    uint32_t r;
    charge(1); FB_CALL(L_28786, 0x0109D2);
    charge(1); FB_CALL(L_1121A, 0x0109D6);
    charge(1); FB_CALL(L_103E0, 0x0109DA);
    charge(1); FB_CALL(L_10E68, 0x0109DE);
    charge(1); FB_CALL(L_10462, 0x0109E2);
    if (fb_timer_or_start()) {
        charge(1); FB_CALL(L_103FE, 0x0109F4);
        charge(1); FB_CALL(L_286FC, 0x0109FA);
        fb_addw(FB_STATE, 1);
        charge(1);
    }
    set_d16(1, (uint16_t)vrd16(FB_TIMER));
    charge(2);
    return RD_JMP(0xE88Eu);
}
/* state 42 */
static uint32_t fb_10a08(void)
{
    uint32_t r;
    charge(1); FB_CALL(L_16EE8, 0x010A0E);
    charge(1); FB_CALL(L_575A, 0x010A14);
    charge(1); FB_CALL(L_E910, 0x010A1A);
    charge(1); FB_CALL(L_10E68, 0x010A1E);
    wr16(W(0x234A), 0);
    wr16(W(0x234C), 0);
    fb_addw(FB_STATE, 1);
    charge(4);
    return RD_RTS;
}
/* state 43 */
static uint32_t fb_10a2c(void)
{
    uint32_t r;
    charge(1); FB_CALL(L_16F54, 0x010A32);
    charge(1); FB_CALL(L_10E68, 0x010A36);
    charge(1); FB_CALL(L_10ED8, 0x010A3A);
    charge(2);
    if ((int16_t)vrd16(W(0x234A)) >= 0) {
        set_d16(7, 0);
        charge(3);
        if (vrd16(W(0x234A)) != 0) { set_d16(7, 0x12C); charge(1); }
        charge(2);
        if (!(d16s(7) >= (int16_t)vrd16(W(0x2348)))) {
            set_d16(1, (uint16_t)vrd16(W(0x234A)));
            charge(3);
            if (d16s(1) < (int16_t)vrd16(W(0x234C))) goto out;
        }
        wr16(FB_COUNT, 0);
        charge(3);
        if (d16u(7)) { wr16(FB_COUNT, 0x3C); charge(1); }
        fb_addw(FB_STATE, 1);
        charge(1);
    }
out:
    set_d16(1, (uint16_t)vrd16(W(0x2348)));
    charge(2);
    return RD_JMP(0xE88Eu);
}
/* state 44 */
static uint32_t fb_10a7a(void)
{
    uint32_t r;
    charge(1); FB_CALL(L_16F54, 0x010A80);
    charge(2);
    if ((int16_t)vrd16(FB_COUNT) >= 0x3C) { wr16(FB_COUNT, 0xFFFF); fb_addw(FB_STATE, 1); charge(2); }
    fb_addw(FB_COUNT, 1);
    charge(2);
    return RD_RTS;
}
/* state 45 */
static uint32_t fb_10a98(void)
{
    uint32_t r;
    charge(1); FB_CALL(L_16F54, 0x010A9E);
    charge(1); FB_CALL(L_10E68, 0x010AA2);
    charge(1); FB_CALL(L_10ED8, 0x010AA6);
    int16_t v = (int16_t)vrd16(W(0x234A));
    charge(2);
    if (v >= 0) {
        charge(1);
        if (v != 0) { fb_addw(FB_STATE, 1); charge(2); }
        else {
            fb_bclr(W(0x11A8), 6);
            fb_bclr(W(0x11A8), 5);
            wr16(W(0x11A2), 0);
            wr16(W(0x2344), 1);
            set_d16(0, (uint16_t)vrd16(W(0x2344)));
            wr16(W(0x1100), vrd16(0xFDA4u + sx16(d_reg(0)) * 2));
            wr16(FB_STATE, 0x13);
            charge(7);
        }
    }
    charge(1);
    return RD_RTS;
}
/* state 46 */
static uint32_t fb_10ade(void)
{
    uint32_t r;
    charge(1); FB_CALL(L_575A, 0x010AE4);
    charge(1); FB_CALL(L_E95A, 0x010AEA);
    charge(1); FB_CALL(L_10414, 0x010AEE);
    charge(1); FB_CALL(L_FE80, 0x010AF2);
    charge(1); FB_CALL(L_104DC, 0x010AF6);
    wr16(W(0x2228), vrd16(W(0x2226)));
    charge(2); FB_CALL(L_10E68, 0x010B00);
    set_d16(1, (uint16_t)vrd16(W(0x2348)));
    set_d16(2, (uint16_t)(0x12C + d_reg(1)));
    charge(5);
    if (d16s(2) > 0x258) { set_d16(2, 0x258); charge(1); }
    charge(2);
    if (d16s(2) < 0xB4) { set_d16(2, 0xB4); charge(1); }
    wr16(FB_TIMER, d_reg(2));
    fb_addw(FB_STATE, 1);
    charge(3);
    return RD_RTS;
}
/* state 47 */
static uint32_t fb_10b28(void)
{
    uint32_t r;
    charge(1); FB_CALL(L_113F0, 0x010B2C);
    wr16(W(0x238E), 2);
    charge(2); FB_CALL(L_11D2A, 0x010B36);
    charge(1); FB_CALL(L_11E12, 0x010B3A);
    charge(1); FB_CALL(L_10E68, 0x010B3E);
    charge(1); FB_CALL(L_10414, 0x010B42);
    charge(1); FB_CALL(L_FE80, 0x010B46);
    charge(1); FB_CALL(L_10474, 0x010B4A);
    set_d16(0, (uint16_t)(vrd16(W(0x2342)) & 3));
    wr16(W(0x2342), d_reg(0));
    wr8(W(0x11AB), d_reg(0));
    charge(4);
    if (fb_timer_or_start()) {
        charge(1); FB_CALL(L_286FC, 0x010B6E);
        fb_addw(FB_STATE, 1);
        charge(1);
    }
    set_d16(1, (uint16_t)vrd16(FB_TIMER));
    charge(2);
    return RD_JMP(0xE88Eu);
}
/* state 49 */
static uint32_t fb_10b84(void)
{
    uint32_t r;
    charge(1); FB_CALL(L_575A, 0x010B8A);
    charge(1); FB_CALL(L_E984, 0x010B90);
    charge(1); FB_CALL(L_113F0, 0x010B94);
    wr16(W(0x238E), 3);
    charge(2); FB_CALL(L_11D2A, 0x010B9E);
    charge(1); FB_CALL(L_10E68, 0x010BA2);
    wr16(FB_TIMER, 0x78);
    fb_addw(FB_STATE, 1);
    charge(3);
    return RD_RTS;
}
/* state 50: count the records by flag bits and 0x11A2 */
static uint32_t fb_10bae(void)
{
    uint32_t r;
    charge(1); FB_CALL(L_E984, 0x010BB4);
    charge(1); FB_CALL(L_113F0, 0x010BB8);
    wr16(W(0x238E), 3);
    charge(2); FB_CALL(L_11D2A, 0x010BC2);
    charge(1); FB_CALL(L_10E68, 0x010BC6);
    charge(1); FB_CALL(L_10414, 0x010BCA);
    charge(1); FB_CALL(L_FE80, 0x010BCE);
    set_d(6, 7); set_d(0, 0); set_d(1, 0); set_d(2, 0); set_d(3, 0); set_d(4, 0); set_d(5, 7);
    charge(10);
    do {
        uint32_t d0 = d_reg(0), rec = FB_REC(0x11B4, d0);
        set_d8(7, vrd8(rec) & d_reg(5));
        charge(3);
        if (d_reg(7) & 0xFF) { set_d16(3, (uint16_t)(d_reg(3) + 1)); charge(1); }
        set_d16(7, (uint16_t)vrd16(W(0x11A2)));
        charge(3);
        if (d16u(7) == (uint16_t)vrd16(FB_REC(0x11AE, d0))) {
            set_d16(1, (uint16_t)(d_reg(1) + 1));
            charge(3);
            if (fb_btst(rec, 6)) { set_d(4, 1); charge(1); }
            charge(2);
            if (fb_btst(rec, 2)) { set_d16(2, (uint16_t)(d_reg(2) + 1)); charge(1); }
            charge(2);
            if (fb_btst(rec, 3)) { set_d16(2, (uint16_t)(d_reg(2) + 1)); charge(1); }
        }
        set_d16(0, (uint16_t)(d0 + 0x10));
    } while (!fb_dbf(6, 2));
    set_d16(3, (uint16_t)(d_reg(3) - d_reg(1)));
    charge(2);
    if (!d16u(3)) { wr16(W(0x2348), 0); charge(1); }
    charge(2);
    if (!d16u(4)) {
        charge(2);
        if (!(d16s(2) < d16s(1))) { fb_addw(FB_STATE, 1); wr16(FB_COUNT, 0); charge(2); }
    }
    charge(1);
    return 0x10EA4u;                             /* bra.w: forward, into FUN_00010ea4 */
}
/* states 51, 54 */
static uint32_t fb_10c4a(void)
{
    uint32_t r;
    charge(1); FB_CALL(L_E984, 0x010C50);
    charge(1); FB_CALL(L_113F0, 0x010C54);
    wr16(W(0x238E), 3);
    charge(2); FB_CALL(L_11D2A, 0x010C5E);
    charge(1); FB_CALL(L_E99C, 0x010C64);
    charge(1); FB_CALL(L_10414, 0x010C68);
    charge(1); FB_CALL(L_FE80, 0x010C6C);
    charge(2);
    if ((int16_t)vrd16(FB_COUNT) >= 8) {
        charge(1); FB_CALL(L_575A, 0x010C7A);
        fb_addw(FB_STATE, 1);
        charge(1);
    }
    fb_addw(FB_COUNT, 1);
    charge(2);
    return RD_RTS;
}
/* state 52 */
static uint32_t fb_10c84(void)
{
    uint32_t r;
    charge(1); FB_CALL(L_575A, 0x010C8A);
    charge(1); FB_CALL(L_E984, 0x010C90);
    charge(1); FB_CALL(L_E99C, 0x010C96);
    charge(1); FB_CALL(L_113F0, 0x010C9A);
    wr16(W(0x238E), 3);
    charge(2); FB_CALL(L_11D2A, 0x010CA4);
    wr16(FB_COUNT, 0);
    fb_addw(FB_STATE, 1);
    charge(3);
    return RD_RTS;
}
/* state 53: the majority vote on the records' byte +3 (& 3) among those
 * with word +0x11AE == 0x11A2; a tie is broken by stepping from word 0xF000 */
static uint32_t fb_10cae(void)
{
    uint32_t r;
    charge(1); FB_CALL(L_E984, 0x010CB4);
    charge(1); FB_CALL(L_E99C, 0x010CBA);
    charge(1); FB_CALL(L_113F0, 0x010CBE);
    wr16(W(0x238E), 3);
    charge(2); FB_CALL(L_11D2A, 0x010CC8);
    charge(2);
    if (fb_btst(W(0x11A8), 7)) {
        set_d(6, 7); set_d(0, 0); set_d(1, 0);
        set_d16(2, (uint16_t)vrd16(W(0x11A2)));
        set_d(5, 0);
        vwr32(W(0x2352), 0);
        charge(6);
        do {
            uint32_t d0 = d_reg(0);
            charge(2);
            if (d16u(2) == (uint16_t)vrd16(FB_REC(0x11AE, d0))) {
                set_d8(1, (uint8_t)vrd8(FB_REC(0x11B7, d0)));
                set_d16(1, (uint16_t)(d_reg(1) & 3));
                uint32_t a = W(0x2352) + sx16(d_reg(1));
                wr8(a, vrd8(a) + 1);
                set_d16(5, (uint16_t)(d_reg(5) + 1));
                charge(4);
            }
            set_d16(0, (uint16_t)(d0 + 0x10));
        } while (!fb_dbf(6, 2));
        set_d(6, 3); set_d(0, 0); set_d(1, 0); set_d(2, 0); set_d(3, 0); set_d(4, 0);
        charge(6);
        do {
            uint32_t d0 = d_reg(0);
            set_d8(3, (uint8_t)vrd8(W(0x2352) + sx16(d0)));
            charge(2);
            if (d_reg(3) & 0xFF) {
                charge(2);
                if (!((int8_t)d_reg(3) < (int8_t)d_reg(1))) {
                    charge(1);
                    if ((uint8_t)d_reg(3) == (uint8_t)d_reg(1)) {
                        set_d(4, d_reg(4) | 1u << (d0 & 31));
                        set_d(4, d_reg(4) | 1u << (d_reg(2) & 31));
                        charge(2);
                    } else {
                        set_d8(4, 0);
                        charge(2);
                    }
                    set_d8(1, (uint8_t)vrd8(W(0x2352) + sx16(d0)));
                    set_d16(2, (uint16_t)d0);
                    charge(2);
                }
            }
            set_d16(0, (uint16_t)(d0 + 1));
        } while (!fb_dbf(6, 2));
        charge(2);
        if (d_reg(4) & 0xFF) {
            /* move.w (0x7000,A6),D2w; tst.b D4b; bne 0x10D50 -- always taken
             * here (D4b != 0), so 0x10D40..0x10D4C never runs */
            set_d16(2, (uint16_t)vrd16(W(0x7000)));
            charge(3);
            for (;;) {
                set_d16(2, (uint16_t)((d_reg(2) + 1) & 3));
                charge(4);
                if ((d_reg(4) >> (d_reg(2) & 31)) & 1) break;
                poll();
            }
        }
        wr16(W(0x2342), d_reg(2));
        wr8(W(0x11AB), d_reg(2));
        charge(2);
    }
    charge(2);
    if ((int16_t)vrd16(FB_COUNT) >= 8) { wr16(FB_COUNT, 0); fb_addw(FB_STATE, 1); charge(3); }
    else { fb_addw(FB_COUNT, 1); charge(1); }
    charge(1);
    return RD_RTS;
}
/* state 55 */
static uint32_t fb_10d7a(void)
{
    uint32_t r;
    charge(1); FB_CALL(L_113F0, 0x010D7E);
    wr16(W(0x238E), 3);
    charge(2); FB_CALL(L_11D2A, 0x010D88);
    uint16_t t = (uint16_t)(vrd16(FB_TIMER) - 1);
    wr16(FB_TIMER, t);
    charge(2);
    if (t == 0) {
        set_d16(0, (uint16_t)(vrd16(W(0x2342)) & 3));
        wr16(W(0x2046), vrd16(0x10B7Cu + sx16(d_reg(0)) * 2));
        fb_addw(FB_STATE, 1);
        charge(4);
    }
    charge(2);
    if (!fb_btst(W(0x11A8), 7)) {
        set_d(6, 7); set_d(0, 0); set_d(1, 1);
        charge(3);
        do {
            uint32_t d0 = d_reg(0);
            charge(2);
            if (fb_btst(FB_REC(0x11B4, d0), 7)) {
                set_d8(1, (uint8_t)vrd8(FB_REC(0x11B7, d0)));
                set_d16(1, (uint16_t)(d_reg(1) & 3));
                charge(3);
                break;
            }
            set_d16(0, (uint16_t)(d0 + 0x10));
        } while (!fb_dbf(6, 2));
        wr16(W(0x2342), d_reg(1));
        charge(1);
    }
    charge(1);
    return 0x11CF2u;                             /* bra.w: forward, into FUN_00011cf2 */
}
/* state 56: 0x208A = 2 + the records ranking ahead (same word +0x11AE as
 * 0x11A2, no flag bit 7, (+0x11B2 << 16 | offset) >= (0x11A6 << 16 | 0x115A)) */
static uint32_t fb_10dd6(void)
{
    set_d(5, 1);
    charge(3);
    if (!fb_btst(W(0x11A8), 7)) {
        set_d(6, 7);
        set_d16(5, (uint16_t)(d_reg(5) + 1));
        set_d16(4, (uint16_t)vrd16(W(0x11A2)));
        set_d16(3, (uint16_t)vrd16(W(0x11A6)));
        set_d(3, swap32(d_reg(3)));
        set_d16(3, (uint16_t)vrd16(W(0x115A)));
        set_d(0, 0); set_d(1, 0);
        charge(8);
        do {
            uint32_t d0 = d_reg(0);
            charge(2);
            if ((uint16_t)d0 != (uint16_t)vrd16(W(0x115A))) {
                charge(2);
                if (d16u(4) == (uint16_t)vrd16(FB_REC(0x11AE, d0))) {
                    charge(2);
                    if (!fb_btst(FB_REC(0x11B4, d0), 7)) {
                        set_d(7, 0);
                        set_d16(7, (uint16_t)vrd16(FB_REC(0x11B2, d0)));
                        set_d(7, swap32(d_reg(7)));
                        set_d16(7, (uint16_t)d0);
                        charge(6);
                        if (!((int32_t)d_reg(3) > (int32_t)d_reg(7))) { set_d16(5, (uint16_t)(d_reg(5) + 1)); charge(1); }
                    }
                }
            }
            set_d16(0, (uint16_t)(d0 + 0x10));
        } while (!fb_dbf(6, 2));
    }
    wr16(W(0x208A), d_reg(5));
    fb_addw(FB_STATE, 1);
    charge(3);
    return RD_RTS;
}
/* state 57 */
static uint32_t fb_10e32(void)
{
    charge(2);
    if (fb_btst(W(0x11A8), 7)) {
        set_d(6, 7); set_d(0, 0); set_d16(1, (uint16_t)vrd16(W(0x11A2)));
        charge(3);
        int hit = 0;
        do {
            uint32_t d0 = d_reg(0);
            charge(2);
            if (d16u(1) == (uint16_t)vrd16(FB_REC(0x11AE, d0))) {
                charge(2);
                if (fb_btst(FB_REC(0x11B6, d0), 0)) { hit = 1; break; }
            }
            set_d16(0, (uint16_t)(d0 + 0x10));
        } while (!fb_dbf(6, 2));
        if (!hit) { fb_bset(W(0x11AA), 0); charge(1); }
    }
    fb_addw(FB_STATE, 1);
    charge(2);
    return RD_RTS;
}
static uint32_t rd_fb1c(void)
{
    uint32_t r;
    vwr32(W(0x5206), vrd32(W(0x5206)) + 1);
    if (vrd8(0x10001071u)) charge(3);
    else { charge(4); FB_CALL(L_22088, 0x00FB2E); }
    set_d16(0, (uint16_t)(vrd16(FB_STATE) & 0x3F));
    set_d(0, vrd32(0xFB40u + (d_reg(0) & 0xFFFF) * 4));
    charge(5);                                   /* move, andi, move.l, nop, jmp */
    poll();                                      /* the computed jmp */
    uint32_t t = 0xFB40u + d_reg(0);
    switch (t) {
    case 0xFC40: charge(1); return RD_RTS;
    case 0xFC42: return fb_fc42();
    case 0xFC80: return fb_fc80();
    case 0xFD3A: return fb_fd3a();
    case 0xFD5C: return fb_fd5c();
    case 0xFDA8: return fb_fda8();
    case 0xFDCE: return fb_fdce();
    case 0xFDE0: return fb_fde0();
    case 0x105F6: return fb_105f6();
    case 0x10628: return fb_10628();
    case 0x10662: return fb_10662();
    case 0x10680: return fb_10680();
    case 0x10698: return fb_10698();
    case 0x106BE: return fb_106be();
    case 0x10730: return fb_10730();
    case 0x10750: return fb_10750();
    case 0x107BC: return fb_107bc();
    case 0x107E2: return fb_107e2();
    case 0x107FA: return fb_107fa();
    case 0x10834: return fb_10834();
    case 0x108B2: return fb_108b2();
    case 0x10906: return fb_10906();
    case 0x10930: return fb_10930();
    case 0x1097C: return fb_1097c();
    case 0x109CC: return fb_109cc();
    case 0x10A08: return fb_10a08();
    case 0x10A2C: return fb_10a2c();
    case 0x10A7A: return fb_10a7a();
    case 0x10A98: return fb_10a98();
    case 0x10ADE: return fb_10ade();
    case 0x10B28: return fb_10b28();
    case 0x10B84: return fb_10b84();
    case 0x10BAE: return fb_10bae();
    case 0x10C4A: return fb_10c4a();
    case 0x10C84: return fb_10c84();
    case 0x10CAE: return fb_10cae();
    case 0x10D7A: return fb_10d7a();
    case 0x10DD6: return fb_10dd6();
    case 0x10E32: return fb_10e32();
    default: return t;                           /* not a table target: resume the lifted code there */
    }
}

static const rd_entry rd_table_b1[] = {
    { 0x00440A, rd_frame_video,            0x3FFF, 0, 0, "FUN_0000440a" },
    { 0x0045AA, rd_frame_io,               0x003F, 0, 0, "FUN_000045aa" },
    { 0x0047D0, rd_read_inputs,            0x0007, 0, 0, "FUN_000047d0" },
    { 0x004F22, rd_dsp_startup,            0x2FFF, 0, 0, "FUN_00004f22" },
    { 0x005064, rd_fill_display_buffer,    0x07FF, 0, 0, "FUN_00005064" },
    { 0x005092, rd_object_block,           0x03FF, 0, 0, "FUN_00005092" },
    { 0x005182, rd_compose_rotation,       0x08FF, 0, 0, "FUN_00005182" },
    { 0x0055AE, rd_mixer_entries,          0x0F01, 0, 0, "FUN_000055ae" },
    { 0x0055F2, rd_czram_init,             0x2FE7, 0, 0, "FUN_000055f2" },
    { 0x005650, rd_czram_ramps,            0x37C1, 0, 0, "FUN_00005650" },
    { 0x005700, rd_5700,                   0x0800, 0, 0, "FUN_00005700" },
    { 0x0057A4, rd_video_startup,          0x2FE7, 0, 0, "FUN_000057a4" },
    { 0x005868, rd_palette_block_50000,    0x0FFF, 0, 0, "FUN_00005868" },
    { 0x005924, rd_serial_lists,           0x031F, 0, 0, "FUN_00005924" },
    { 0x005A2A, rd_put_char,               0x0101, 0, 0, "FUN_00005a2a" },
    { 0x00A98C, rd_vector_angle,           0x021F, 0, 0, "FUN_0000a98c" },
    { 0x00AC4E, rd_init_descriptors_d68,   0x0300, 0, 0, "FUN_0000ac4e" },
    { 0x00B4E4, rd_load_object_block,      0x0301, 0, 0, "FUN_0000b4e4" },
    { 0x00B56E, rd_current_object_block,   0x03FF, 0, 0, "FUN_0000b56e" },
    { 0x00B788, rd_fade_down,              0x0107, 0, 0, "FUN_0000b788" },
    { 0x00B7CC, rd_fade_up,                0x0107, 0, 0, "FUN_0000b7cc" },
    { 0x00C150, rd_coins_and_switches,     0x003F, 0, 0, "FUN_0000c150" },
    { 0x00C258, rd_credit_display,         0x0FFF, 0, 0, "FUN_0000c258" },
    { 0x00C606, rd_text_layer_init,        0x0F1F, 0, 0, "FUN_0000c606" },
    { 0x00C69A, rd_load_cgram_set,         0x0F1F, 0, 0, "FUN_0000c69a" },
    { 0x00C6FE, rd_load_cgram_fixed,       0x0F1F, 0, 0, "FUN_0000c6fe" },
    { 0x00C74E, rd_load_text_palette,      0x0F01, 0, 0, "FUN_0000c74e" },
    { 0x00DBC8, rd_draw_text_block,        0x030F, 0, 0, "FUN_0000dbc8", 1 },   /* nofuzz: a mutated block number reads a ROM "descriptor" with a 64K x 64K rectangle */
    { 0x00DEFE, rd_ticker_start,           0x0100, 0, 0, "FUN_0000defe" },
    { 0x00DF22, rd_ticker_step,            0x0303, 0, 0, "FUN_0000df22" },
    { 0x00E082, rd_glyph_code,             0x0001, 0, 0, "FUN_0000e082" },
    { 0x00E262, rd_draw_block_run,         0x038F, 0, 0, "FUN_0000e262" },
    { 0x00E3F0, rd_draw_gear_frame,        0x038F, 0, 0, "FUN_0000e3f0" },
    { 0x00E424, rd_draw_gear_pair,         0x030F, 0, 0, "FUN_0000e424" },
    { 0x00E4BC, rd_draw_two_digits_204e,   0x03FF, 0, 0, "FUN_0000e4bc" },
    { 0x00E61E, rd_draw_flags_e61e,        0x030F, 0, 0, "FUN_0000e61e" },
    { 0x00E654, rd_e654,                   0x00C0, 0, 0, "FUN_0000e654" },
    { 0x00E65C, rd_e65c,                   0x0001, 0, 0, "FUN_0000e65c" },
    { 0x00E66C, rd_e66c,                   0x0001, 0, 0, "FUN_0000e66c" },
    { 0x00E6E0, rd_e6e0,                   0x03CF, 0, 0, "FUN_0000e6e0" },
    { 0x00E71C, rd_e71c,                   0x0001, 0, 0, "FUN_0000e71c" },
    { 0x00E722, rd_e722,                   0x03CF, 0, 0, "FUN_0000e722" },
    { 0x00E850, rd_e850,                   0x03CF, 0, 0, "FUN_0000e850" },
    { 0x00E88E, rd_e88e,                   0x03FF, 0, 0, "FUN_0000e88e" },
    { 0x00E8FA, rd_e8fa,                   0x03CF, 0, 0, "FUN_0000e8fa" },
    { 0x00E92A, rd_e92a,                   0x03CF, 0, 0, "FUN_0000e92a" },
    { 0x00E940, rd_e940,                   0x03CF, 0, 0, "FUN_0000e940" },
    { 0x00E9D8, rd_e9d8,                   0x0001, 0, 0, "FUN_0000e9d8" },
    { 0x00E9E0, rd_e9e0,                   0x034F, 0, 0, "FUN_0000e9e0" },
    { 0x00EA6A, rd_ea6a,                   0x0FDF, 0, 0, "FUN_0000ea6a" },
    { 0x00EA92, rd_ea92,                   0x0FDF, 0, 0, "FUN_0000ea92" },
    { 0x00EAFA, rd_eafa,                   0x030F, 0, 0, "FUN_0000eafa" },
    { 0x00F464, rd_f464,                   0x0FFF, 0, 0, "FUN_0000f464" },
    { 0x00F61E, rd_f61e,                   0x0041, 0, 0, "FUN_0000f61e" },
    { 0x00F67E, rd_f67e,                   0x0001, 0, 0, "FUN_0000f67e" },
    { 0x00F6DC, rd_f6dc,                   0x0FFF, 0, 0, "FUN_0000f6dc" },
    { 0x00F728, rd_f728,                   0x0FFF, 0, 0, "FUN_0000f728" },
    { 0x00F798, rd_f798,                   0x3FFF, 0, 0, "FUN_0000f798" },
    { 0x00F9CA, rd_f9ca,                   0x0BFF, 0, 0, "FUN_0000f9ca" },
    { 0x00F9EE, rd_f9ee,                   0x0BFF, 0, 0, "FUN_0000f9ee" },
    { 0x00FAF4, rd_faf4,                   0x0081, 0, 0, "FUN_0000faf4" },
    { 0x00FB1C, rd_fb1c,                   0x3FFF, 0, 0, "FUN_0000fb1c" },
    { 0x00FDEE, rd_fdee,                   0x020F, 0, 0, "FUN_0000fdee" },
    { 0x00FE44, rd_fe44,                   0x020F, 0, 0, "FUN_0000fe44" },
    { 0x00FE80, rd_fe80,                   0x0FDF, 0, 0, "FUN_0000fe80" },
    { 0x00FED6, rd_fed6,                   0x0FDF, 0, 0, "FUN_0000fed6" },
    { 0x00FF1E, rd_ff1e,                   0x0FDF, 0, 0, "FUN_0000ff1e" },
    { 0x01024E, rd_copy33,                 0x0301, 0, 0, "FUN_0001024e" },
    { 0x01025A, rd_1025a,                  0x0303, 0, 0, "FUN_0001025a" },
    { 0x0103C4, rd_103c4,                  0x0303, 0, 0, "FUN_000103c4" },
    { 0x0103D4, rd_103d4,                  0x020F, 0, 0, "FUN_000103d4" },
    { 0x0103DC, rd_103dc,                  0x0003, 0, 0, "thunk_FUN_0000fe60" },
    { 0x010462, rd_10462,                  0x0183, 0, 0, "FUN_00010462" },
    { 0x010474, rd_10474,                  0x0183, 0, 0, "FUN_00010474" },
    { 0x010498, rd_10498,                  0x0000, 0, 0, "FUN_00010498" },
    { 0x010502, rd_10502,                  0x0103, 0, 0, "FUN_00010502" },
    { 0x010514, rd_10514,                  0x0103, 0, 0, "FUN_00010514" },
    { 0x010F26, rd_10f26,                  0x0FFF, 0, 0, "FUN_00010f26" },
    { 0x01121A, rd_select_display,         0x0BFF, 0, 0, "FUN_0001121a" },
    { 0x0113F0, rd_car_select_list,        0x1FFF, 0, 0, "FUN_000113f0" },
    { 0x00E2B0, rd_hud_numbers,            0x3FFF, 0, 0, "FUN_0000e2b0" },
    { 0x00E54C, rd_hud_frame,              0x3FFF, 0, 0, "FUN_0000e54c" },
    { 0x00F4DA, rd_f4da,                   0x0FFF, 0, 0, "FUN_0000f4da" },
};
RD_REGISTER(rd_table_b1)
