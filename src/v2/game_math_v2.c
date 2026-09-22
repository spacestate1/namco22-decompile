/*
 * Transpilation v2 pilot — math functions (GUARDRAILS.md §7 step 3).
 *
 * Translated from the M68K machine code (MAME debugger disassembly of
 * ROM 0xA870-0xA9E8), NOT from the pass-one transpile, because pass one
 * mangled all three: lost table base addresses, moved the address scale
 * (*2) onto the table value, used native-LE ROM reads, and stubbed the
 * quadrant jump tables ("dispatch stub - needs ROM table" returned 0 for
 * three of four quadrants).
 *
 * Rules in force (GUARDRAILS §2-§3):
 *   R1/R4  every memory access is a vrd* call at the original M68K
 *          address, big-endian, no rom_nr* transitional layer
 *   R3     no address-carrying short/int parameters (these functions
 *          take plain values, not addresses)
 *   R5     widths from the machine code: move.w (...) -> vrd16
 *   G3     division-by-zero (hardware would take the zero-divide trap)
 *          fails loudly, never silently
 *   G4     per-function provenance comments + execution counters
 *
 * Verified by L2 differential traces against MAME — see
 * tools/l3/mame_l2_capture.lua + tools/l3/l2_verify.c.
 *
 * Jump-table decode (Ghidra could not recover these):
 *   0xA8E2/0xA966: quadrant dispatch tables, 4 x u16 code offsets
 *   {0x0008, 0x0010, 0x001A, 0x0024}, applied to the base angle:
 *     quad 0 (y>=0,x>=0): base + 0xFFFFC000   (addi.l #-0x4000)
 *     quad 1 (y< 0,x>=0): 0x4000 - base       (lea/suba)
 *     quad 2 (y>=0,x< 0): 0xFFFFC000 - base   (lea (0xC000).w sign-ext)
 *     quad 3 (y< 0,x< 0): base + 0x4000       (addi.l #0x4000)
 *   then andi.l #0xFFFF.
 *
 * ROM tables:
 *   0x200000  fine atan: 1025 x u16, index (min<<10)/max in [0,1024]
 *   0x200802  coarse atan: 32x32 x u16, index |x|*0x40 + |y|*2
 *   0x201002  slope angle: u16[], index (a<<9)/(b>>3)
 */
#include "propcycl.h"
#include <stdio.h>

/* G4: execution counters, readable from the debugger / future coverage
 * reports (§5.5). */
uint32_t v2_cov_math_atan2;
uint32_t v2_cov_math_atan2_coarse;
uint32_t v2_cov_math_slope_angle;

static void v2_div0(const char* fn) {
    /* G3: the 68020 would take the zero-divide exception here. Reaching
     * this means upstream state is already wrong (e.g. attract demo
     * calling with zeroed player state) — say so, loudly. */
    /* The first one is real information; the rest are the same degenerate
     * scene state repeating 60x a second and drown the console.
     * PROPCYCL_VERBOSE=1 shows them all. */
    static int warned;
    if (warned++ == 0 || propcycl_verbose())
        fprintf(stderr, "[V2-TRAP] %s: division by zero (hardware would "
                "take the zero-divide trap)\n", fn);
}

/* Quadrant transform shared by both atan2 variants (jump tables at
 * 0xA8E2 / 0xA966 are byte-identical). */
static uint32_t quadrant_adjust(uint32_t base, uint32_t quad) {
    switch (quad) {
    case 0:  base = base + 0xFFFFC000u; break;  /* base - 0x4000 */
    case 1:  base = 0x4000u - base;     break;
    case 2:  base = 0xFFFFC000u - base; break;
    default: base = base + 0x4000u;     break;  /* quad 3 */
    }
    return base & 0xFFFF;
}

/* ---- math_atan2 @ 0x00A870 (Ghidra: math_atan2) ----
 * Full-precision atan2 via 1025-entry ROM table at 0x200000.
 * Returns 16-bit angle (0x10000 = 360 deg). */
uint32_t math_atan2(int32_t param_1, int32_t param_2) {
    v2_cov_math_atan2++;

    uint32_t quad = 0;
    uint32_t ay = (uint32_t)param_1;           /* D4 */
    uint32_t ax = (uint32_t)param_2;           /* D3 */
    if (param_1 < 0) { ay = -(uint32_t)param_1; quad += 1; }
    if (param_2 < 0) { ax = -(uint32_t)param_2; quad += 2; }

    uint32_t base;
    if (ay >= ax) {                            /* cmp.l D3,D4 / bcs */
        if (ay == 0) { v2_div0("math_atan2"); return 0; }
        base = vrd16(0x200000 + ((ax << 10) / ay) * 2);   /* divu.l */
    }
    else {
        base = 0x4000u - vrd16(0x200000 + ((ay << 10) / ax) * 2);
    }
    return quadrant_adjust(base, quad);
}

/* ---- math_atan2_coarse @ 0x00A91A (Ghidra: math_atan2_coarse) ----
 * Coarse atan2 via 32x32 u16 ROM table at 0x200802. Callers must keep
 * |y|,|x| < 32 (the hardware does not clamp; neither do we, but we log
 * out-of-table indices once). */
uint32_t math_atan2_coarse(int32_t param_1, int32_t param_2) {
    v2_cov_math_atan2_coarse++;

    uint32_t quad = 0;
    uint32_t ay = (uint32_t)param_1;           /* D5 */
    uint32_t ax = (uint32_t)param_2;           /* D4 */
    if (param_1 < 0) { ay = -(uint32_t)param_1; quad += 1; }
    if (param_2 < 0) { ax = -(uint32_t)param_2; quad += 2; }

    if (ax >= 32 || ay >= 32) {                /* G3: visible, not fatal */
        static int warned;
        if (warned++ < 8)
            fprintf(stderr, "[V2-WARN] math_atan2_coarse(%d,%d): index "
                    "outside 32x32 table (faithful read proceeds)\n",
                    param_1, param_2);
    }
    uint32_t base = vrd16(0x200802 + ax * 0x40 + ay * 2);
    return quadrant_adjust(base, quad);
}

/* ---- math_slope_angle @ 0x00A99E (Ghidra: math_slope_angle) ----
 * Slope-to-angle via u16 ROM table at 0x201002, index (a<<9)/(b>>3).
 * Note: lsl.l #9 then divs.l — the quotient is the table INDEX
 * (move.w (0x201002,D3.l*2)); pass one multiplied the table VALUE by 2
 * instead and dropped the address scale. */
uint32_t math_slope_angle(int32_t param_1, int32_t param_2) {
    v2_cov_math_slope_angle++;

    int32_t num = (int32_t)((uint32_t)param_1 << 9);   /* lsl.l #9 */
    if (param_2 < 0)
        param_2 += 7;
    param_2 >>= 3;                                     /* asr.l #3 */

    if (param_2 == 0) {                                /* divs.l #0 */
        v2_div0("math_slope_angle");
        return 0;
    }
    if ((num ^ param_2) < 0) {                         /* signs differ */
        int32_t q = num / -param_2;                    /* divs.l */
        return 0x8000u - (uint32_t)vrd16(0x201002 + (uint32_t)q * 2);
    }
    int32_t q = num / param_2;
    return (uint32_t)vrd16(0x201002 + (uint32_t)q * 2);
}
