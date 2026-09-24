/*
 * rd_b3.c -- Phase B batch 3 (0x1B0C2..0x2077C): readable replacements for
 * the lap-time display, the sound-CPU mixer updates and the car/scene code in
 * that range. Same rules as rd_funcs.c / rd_calls.c (include/rd.h): written
 * from the 68K INSTRUCTIONS, every register a caller could read set exactly
 * as the 68K leaves it, instructions charged in execution order before each
 * poll point (taken backward branch, bsr/jsr, computed jmp). Every entry here
 * uses cost 0 and charges its own rts, so each path's count is visible in
 * the body. Proven by RR_RD=check (+ fuzz) against the lifted twin.
 */
#include "rd.h"
#include "rr_lifted.h"
#include <stdio.h>

/* low word of Dn */
static inline uint16_t dw(int n) { return (uint16_t)d_reg(n); }
/* 68K muls.w: Dn = (int16)Dn.w * (int16)src, 32-bit result */
static inline void muls_w(int n, uint16_t src) { set_d(n, (uint32_t)((int32_t)(int16_t)dw(n) * (int16_t)src)); }
/* the fixed-point idiom `add.l Dn,Dn ; swap Dn` */
static inline void dbl_swap(int n) { uint32_t v = d_reg(n) * 2; set_d(n, (v << 16) | (v >> 16)); }

/* tst.w (An): the lifted code reads the operand TWICE (Ghidra's p-code), which
 * matters when An points at I/O (every read is logged and replayed) */
static inline uint16_t tst_w(uint32_t a) { (void)vrd16(a); return (uint16_t)vrd16(a); }

/* (d16,A6): A6 always holds WRAM + 0x8000 */
#define W(off) (0x10008000u + (uint32_t)(off))

/* ---- WRAM (A6-relative) ---- */
#define RACE_CLOCK       W(0x204A)   /* long: race time in frames (compared with the lap limits)   */
#define RACE_TIME_LIMIT  W(0x4846)   /* long: the clock runs until this (purpose of the name inferred) */
#define LAP_LIMIT_LO     W(0x4848)   /* word: low half of RACE_TIME_LIMIT, read on its own           */
#define LAP_NEXT_MARK    W(0x48EA)   /* long: clock value at which the next lap begins (+0x10000/lap) */
#define LAP_ROW_OFFSET   W(0x48EE)   /* word: lap index * 4, capped at 28 -- the text row offset     */
#define LAP_INDEX        W(0x48F2)   /* word: current lap                                             */
#define LAP_TIMES        W(0x48F4)   /* 16 longs: time per lap; entry 15 = the sum of the first 15    */
#define LAP_ROWS_LEFT    W(0x49D4)   /* word: loop counter of the lap-list printers                   */
#define LAP_NEW_BITS     W(0x49BE)   /* word: bit n = lap n is highlighted (a new record)             */
#define LAP_DONE_BITS    W(0x48D6)   /* word: laps finished (bits), copied to LAP_NEW_BITS            */
#define LAP_DELAY        W(0x48D8)   /* word: countdown before the finished lap is recorded           */
#define COURSE_INDEX     W(0x2342)   /* word: selects the per-course record tables                    */
#define BEST_LAP         W(0x5106)   /* longs[course]: best lap time                                  */
#define BEST_TOTAL       W(0x5126)   /* longs[course]: best total time                                */
#define ATTRACT_FLAG     W(0x1100)   /* word: nonzero outside a real race (purpose not yet fully identified) */
#define PLAYER_CAR       W(0x11A0)   /* word: index of the player's car                               */
#define FRAME_PARITY     W(0x0C4C)   /* word: frame counter, only bit 0 used here                     */
#define RANDOM_WORD      W(0x7000)   /* word: random source (low 7 bits added to a lap time)          */
#define DLIST_PTR        W(0x0C46)   /* long: display-list write pointer                              */

/* ---- the sound CPU's command block (shared RAM) ---- */
#define SND_VOICE_16     0x60005016u
#define SND_VOICE_18     0x60005018u
#define SND_VOICE_1A     0x6000501Au
#define SND_VOICE_1C     0x6000501Cu

/* external routines */
#define PRINT_TEXT       0x005A0Au   /* FUN_00005a0a: print the text command string at A1 */

/* ======================================================================== */
/* small sound-CPU command routines                                          */
/* ======================================================================== */

/* FUN_0001e6ce: silence the four voice slots (FUN_0001e760) */
static uint32_t rd_1e6ce(void)
{
    uint32_t r;
    charge(1);
    if ((r = rd_call(L_1E760, 0x01E6D2))) return r;
    charge(1);                                          /* rts */
    return RD_RTS;
}

/* FUN_0001ea1c: start voice 0x40D4 in slot 0x18 */
static uint32_t rd_1ea1c(void)
{
    charge(2);
    vwr16(SND_VOICE_18, 0x40D4);
    return RD_RTS;
}

/* FUN_0001e6f4: send the four mixer levels (bytes 0x104B..0x104E) and set
 * eight channel volumes to 0xFF. D0w ends 0 (high word kept), D1w = 0xFF. */
static uint32_t rd_1e6f4(void)
{
    static const uint32_t vol[8] = { 0x60005100u, 0x60005104u, 0x60005108u, 0x6000510Cu,
                                     0x60005110u, 0x60005116u, 0x60005140u, 0x6000514Cu };
    charge(20);
    for (int i = 0; i < 4; i++)
        vwr16(0x60004022u + 2u * (uint32_t)i, vrd8(0x1000104Bu + (uint32_t)i));
    set_d16(0, 0);
    set_d16(1, 0xFF);
    for (int i = 0; i < 8; i++) vwr16(vol[i], 0xFF);
    return RD_RTS;
}

/* FUN_0001e6d4: start voices 0x40B0 / 0x40B1 in slots 0x1C / 0x1A, then set
 * the mixer (FUN_0001e6f4) */
static uint32_t rd_1e6d4(void)
{
    uint32_t r;
    charge(3);
    vwr16(SND_VOICE_1C, 0x40B0);
    vwr16(SND_VOICE_1A, 0x40B1);
    if ((r = rd_call(L_1E6F4, 0x01E6E8))) return r;
    charge(1);
    return RD_RTS;
}

/* FUN_0001e6aa: engine sounds on -- FUN_0001e6d4, FUN_0001e99c, FUN_0001e6ea */
static uint32_t rd_1e6aa(void)
{
    uint32_t r;
    charge(1);
    if ((r = rd_call(L_1E6D4, 0x01E6AE))) return r;
    charge(1);
    if ((r = rd_call(L_1E99C, 0x01E6B2))) return r;
    charge(1);
    if ((r = rd_call(L_1E6EA, 0x01E6B6))) return r;
    charge(1);
    return RD_RTS;
}

/* FUN_0001e6b8: per-frame sound update -- the five mixer routines in order */
static uint32_t rd_1e6b8(void)
{
    uint32_t r;
    charge(1);
    if ((r = rd_call(L_1E78A, 0x01E6BC))) return r;
    charge(1);
    if ((r = rd_call(L_1EB66, 0x01E6C0))) return r;
    charge(1);
    if ((r = rd_call(L_1E930, 0x01E6C4))) return r;
    charge(1);
    if ((r = rd_call(L_1EAA0, 0x01E6C8))) return r;
    charge(1);
    if ((r = rd_call(L_1EC32, 0x01E6CC))) return r;
    charge(1);
    return RD_RTS;
}

/* ======================================================================== */
/* lap times                                                                 */
/* ======================================================================== */

/* FUN_0001e292: clear the 16 lap times (A0 ends past them, D0 = 0,
 * D7w = 0xFFFF), print the course records outside a race (FUN_0001e49a),
 * and start at lap 0 */
static uint32_t rd_1e292(void)
{
    uint32_t r;
    charge(3);                                          /* lea, moveq, move.w */
    set_d(0, 0);
    uint32_t a0 = LAP_TIMES;
    for (int n = 15; ; n--) {
        charge(2);                                      /* move.l, dbf */
        vwr32(a0, 0); a0 += 4;
        if (n == 0) break;
        poll();
    }
    set_a(0, a0);
    set_d16(7, 0xFFFF);
    charge(2);                                          /* tst.w, beq */
    if (vrd16(ATTRACT_FLAG) != 0) {
        charge(1);
        if ((r = rd_call(L_1E49A, 0x01E2AC))) return r;
    }
    charge(2);                                          /* move.w, rts */
    vwr16(LAP_INDEX, 0);
    return RD_RTS;
}

/* FUN_0001e49a: print the current course's best lap and best total --
 * each converted to digits (FUN_0000a8c8) and printed (FUN_00005a0a) with the
 * text commands at 0x1E4D2 / 0x1E4E4; ends in a tail jump to the printer */
static uint32_t rd_1e49a(void)
{
    uint32_t r;
    charge(3);
    set_d16(0, (uint16_t)vrd16(COURSE_INDEX));
    set_d(1, vrd32(BEST_LAP + (uint32_t)((int16_t)d_reg(0) * 4)));
    if ((r = rd_call(L_A8C8, 0x01E4AA))) return r;
    charge(3);
    set_d(6, d_reg(0));
    set_a(1, 0x01E4D2);
    if ((r = rd_call(L_5A0A, 0x01E4B6))) return r;
    charge(3);
    set_d16(0, (uint16_t)vrd16(COURSE_INDEX));
    set_d(1, vrd32(BEST_TOTAL + (uint32_t)((int16_t)d_reg(0) * 4)));
    if ((r = rd_call(L_A8C8, 0x01E4C6))) return r;
    charge(3);                                          /* move.l, lea, jmp */
    set_d(6, d_reg(0));
    set_a(1, 0x01E4E4);
    return RD_JMP(PRINT_TEXT);
}

/* FUN_0001e3cc: print the "LAP n" label (text 0x1E45A + LAP_ROW_OFFSET), then,
 * if the current lap has a time, that time (text 0x1E48A) */
static uint32_t rd_1e3cc(void)
{
    uint32_t r;
    charge(4);
    set_d16(0, (uint16_t)vrd16(LAP_ROW_OFFSET));
    set_a(1, 0x01E45Au + (uint32_t)(int16_t)d_reg(0));
    if ((r = rd_call(L_5A0A, 0x01E3DC))) return r;
    charge(3);
    set_d16(0, (uint16_t)vrd16(LAP_INDEX));
    uint32_t t = vrd32(LAP_TIMES + (uint32_t)((int16_t)d_reg(0) * 4));
    set_d(1, t);
    if (t == 0) { charge(1); return RD_RTS; }
    charge(1);
    if ((r = rd_call(L_A8C8, 0x01E3F0))) return r;
    charge(3);
    set_d(6, d_reg(0));
    set_a(1, 0x01E48A);
    return RD_JMP(PRINT_TEXT);
}

/* FUN_0001e2b4: per-frame lap bookkeeping. While the clock is inside the
 * current lap window it adds a little randomness to the running lap time;
 * then totals the laps (FUN_0001e3b8), records a new best lap outside a race,
 * redraws (FUN_0001e5a4), and when the clock passes the lap mark advances to
 * the next lap. Finally counts the current lap's time up by 0xA6 or 0xA7
 * (frame parity) and prints it. */
static uint32_t rd_1e2b4(void)
{
    uint32_t r;
    uint32_t clock = vrd32(RACE_CLOCK);
    set_d(0, clock);
    charge(3);                                          /* move.l, cmp.l, bge */
    if ((int32_t)clock < (int32_t)vrd32(RACE_TIME_LIMIT)) {
        charge(2);                                      /* cmp.l, blt.w */
        if ((int32_t)clock < (int32_t)vrd32(LAP_NEXT_MARK)) goto count_up;
        charge(5);
        uint32_t d1 = vrd16(RANDOM_WORD) & 0x7F;
        set_d16(0, (uint16_t)vrd16(LAP_INDEX));
        uint32_t slot = LAP_TIMES + (uint32_t)((int16_t)d_reg(0) * 4);
        d1 += vrd32(slot);
        set_d(1, d1);
        vwr32(slot, d1);
    }
    charge(1);
    if ((r = rd_call(L_1E3B8, 0x01E2E4))) return r;
    charge(3);                                          /* clr.w, tst.w, beq */
    vwr16(W(0x48B8), 0);
    if (vrd16(ATTRACT_FLAG) != 0) {
        charge(6);
        set_d16(0, (uint16_t)vrd16(LAP_INDEX));
        uint32_t lap = vrd32(LAP_TIMES + (uint32_t)((int16_t)d_reg(0) * 4));
        set_d(0, lap);
        set_d16(1, (uint16_t)vrd16(COURSE_INDEX));
        uint32_t best = BEST_LAP + (uint32_t)((int16_t)d_reg(1) * 4);
        set_d(2, vrd32(best));
        if (lap < d_reg(2)) {                           /* a new best lap */
            charge(9);
            vwr32(best, lap);
            vwr16(W(0x4988), 0xFFFF);
            vwr16(W(0x57E0), 1);
            uint32_t bit = vrd16(LAP_INDEX) & 0xF;
            set_d(0, bit);
            set_d(1, 1u << bit);
            vwr16(LAP_NEW_BITS, (uint16_t)(1u << bit));
            if ((r = rd_call(L_1E49A, 0x01E32E))) return r;
        }
    }
    charge(1);
    if ((r = rd_call(L_1E5A4, 0x01E334))) return r;
    charge(3);                                          /* move.l, cmp.l, bge.w */
    set_d(0, vrd32(RACE_CLOCK));
    if ((int32_t)d_reg(0) >= (int32_t)vrd32(RACE_TIME_LIMIT)) { charge(1); return RD_RTS; }
    charge(4);                                          /* addq, move, cmpi, bcs */
    vwr16(LAP_INDEX, (uint16_t)(vrd16(LAP_INDEX) + 1));
    uint16_t lapi = (uint16_t)vrd16(LAP_INDEX);
    if (lapi >= 7) { charge(1); lapi = 7; }
    charge(4);                                          /* lsl, move, addi.l, bsr */
    set_d16(0, (uint16_t)(lapi << 2));
    vwr16(LAP_ROW_OFFSET, (uint16_t)(lapi << 2));
    vwr32(LAP_NEXT_MARK, vrd32(LAP_NEXT_MARK) + 0x10000);
    if ((r = rd_call(L_1E3FC, 0x01E364))) return r;
    charge(1);
    if ((r = rd_call(L_1E62C, 0x01E368))) return r;

count_up:
    charge(2);                                          /* tst.w, beq */
    if (vrd16(ATTRACT_FLAG) != 0) {
        charge(3);                                      /* move.l, cmp.l, blt */
        set_d(0, vrd32(RACE_CLOCK));
        if ((int32_t)d_reg(0) >= (int32_t)vrd32(RACE_TIME_LIMIT)) { charge(1); return RD_RTS; }
        charge(4);                                      /* moveq, move.w, cmp.l, bgt */
        set_d(0, vrd16(LAP_LIMIT_LO));
        if ((int32_t)d_reg(0) > (int32_t)vrd32(RACE_CLOCK)) goto print;
    }
    {
        charge(9);
        set_d16(0, (uint16_t)(vrd16(LAP_INDEX) << 2));
        uint32_t a1 = LAP_TIMES + (uint32_t)(int16_t)d_reg(0);
        set_a(1, a1);
        uint32_t inc = (vrd16(FRAME_PARITY) & 1) + 0xA6;
        set_d(0, inc);
        vwr32(a1, vrd32(a1) + inc);
    }
print:
    charge(2);
    set_a(1, 0x01E48C);
    if ((r = rd_call(L_5A0A, 0x01E3B0))) return r;
    charge(1);
    if ((r = rd_call(L_1E3CC, 0x01E3B6))) return r;
    charge(1);
    return RD_RTS;
}

/* FUN_0001e5c4: after the LAP_DELAY countdown runs out: unless another car
 * (same word at +0x11AE, a greater long at +0x11B0) is ahead of the player,
 * mark the previous lap done, highlight it and reprint the lap list
 * (FUN_0001e3fc, FUN_0001e62c). Leaves early by branching to 0x1E69E /
 * 0x1E6A8 (other functions' code). */
static uint32_t rd_1e5c4(void)
{
    uint32_t r;
    charge(2);                                          /* tst.w, beq.w */
    if (vrd16(LAP_DELAY) == 0) return 0x01E69E;
    charge(2);                                          /* subq.w, bne.w */
    uint16_t left = (uint16_t)(vrd16(LAP_DELAY) - 1);
    vwr16(LAP_DELAY, left);
    if (left != 0) return 0x01E6A8;
    charge(3);                                          /* move.w, move.l, move.w */
    uint16_t car = 7;
    uint32_t d1 = vrd32(W(0x11A4));
    uint16_t d2 = (uint16_t)vrd16(W(0x11A2));
    set_d(1, d1);
    set_d16(2, d2);
    for (;;) {
        charge(2);                                      /* cmp.w, beq */
        if (car != (uint16_t)vrd16(PLAYER_CAR)) {
            charge(4);                                  /* move.w, lsl.w, cmp.w, bne */
            uint16_t off = (uint16_t)(car << 4);
            set_d16(0, off);
            if (d2 == (uint16_t)vrd16(W(0x11AE) + (uint32_t)(int16_t)off)) {
                charge(2);                              /* cmp.l, bgt.w */
                if ((int32_t)d1 > (int32_t)vrd32(W(0x11B0) + (uint32_t)(int16_t)off)) {
                    set_d16(6, car);
                    return 0x01E6A8;
                }
            }
        }
        charge(1);                                      /* dbf */
        if (car-- == 0) break;
        set_d16(6, car);
        poll();
    }
    set_d16(6, 0xFFFF);
    charge(9);
    vwr16(W(0x57EA), 1);
    set_d16(0, (uint16_t)vrd16(LAP_INDEX));
    uint32_t bit = (d_reg(0) - 1) & 0xF;
    set_d(0, bit);
    uint16_t bits = (uint16_t)(vrd16(LAP_DONE_BITS) | (1u << bit));
    set_d(1, d1 | (1u << bit));
    set_d16(1, bits);
    vwr16(LAP_DONE_BITS, bits);
    vwr16(LAP_NEW_BITS, bits);
    if ((r = rd_call(L_1E3FC, 0x01E626))) return r;
    charge(1);
    if ((r = rd_call(L_1E62C, 0x01E62A))) return r;
    charge(1);
    return RD_RTS;
}


/* ======================================================================== */
/* per-frame sound mixer (called from FUN_0001e6b8)                          */
/* ======================================================================== */

#define SND_CH06   0x60005106u
#define SND_CH0A   0x6000510Au
#define SND_CH12   0x60005112u
#define SND_CH04   0x60005104u
#define SND_CH08   0x60005108u
#define SND_CH10   0x60005110u
#define SND_CH14   0x60005114u
#define SND_CH1C   0x6000511Cu
#define SND_CH22   0x60005122u

#define ENGINE_RPM       W(0x4014)   /* word: engine speed (the pitch source) */
#define ENGINE_RPM_PREV  W(0x40DA)   /* word: last frame's ENGINE_RPM          */
#define ENGINE_RPM_DELTA W(0x40DC)   /* word: ENGINE_RPM - ENGINE_RPM_PREV     */
#define GEAR_SHIFT_FLAG  W(0x40B4)   /* word: selects which of two engine channels plays (purpose not yet fully identified) */
#define SKID_LEVEL       W(0x46D4)   /* word: per-car table value at 0x466C, 0 = none */

/* 68K divs.w <n>,Dn: quotient:remainder into Dn unless the quotient
 * overflows 16 bits (then Dn is unchanged). Division by zero follows the
 * lifted code (SDIVREM: a counted trap, quotient and remainder 0). */
static uint32_t divs_w(uint32_t dn, int16_t n)
{
    int64_t q = SDIVREM((int32_t)dn, n, '/'), rm = SDIVREM((int32_t)dn, n, '%');
    if ((uint32_t)((int32_t)q + 0x8000) > 0xFFFFu) return dn;
    return ((uint32_t)rm << 16) | ((uint32_t)q & 0xFFFF);
}

/* FUN_0001e78a: engine and road-noise channels. Pitch from ENGINE_RPM (plus
 * the throttle word 0x41E2/16 unless 0x4002), to one of two engine channels
 * chosen by GEAR_SHIFT_FLAG; the tyre channel from SKID_LEVEL (0x466C[car])
 * or, without skidding, from the lateral words 0x21C4 / 0x20A4. */
static uint32_t rd_1e78a(void)
{
    charge(3);                                          /* move.w, cmp.w, beq */
    uint16_t d0 = (uint16_t)vrd16(W(0x46D2));
    set_d16(0, d0);
    if (d0 != (uint16_t)vrd16(W(0x4006))) { charge(1); vwr16(SND_VOICE_1C, 0x40B0); }
    charge(5);                                          /* move.w, move.w, lsr, tst, beq */
    vwr16(W(0x46D2), vrd16(W(0x4006)));
    set_d16(1, (uint16_t)((uint16_t)vrd16(W(0x41E2)) >> 4));
    if (vrd16(W(0x4002)) != 0) { charge(1); set_d(1, 0); }
    charge(11);
    d0 = (uint16_t)(vrd16(ENGINE_RPM) - 0xBB8 + 0x2000 - vrd16(W(0x41E8)) + d_reg(1));
    set_d16(1, d0);
    set_d(1, divs_w(d_reg(1), 4));
    d0 = (uint16_t)(d0 + d_reg(1) + 0x1000);
    if (d0 >= 0x6B00) { charge(1); d0 = 0x6B00; }
    charge(2);                                          /* tst.w, bne */
    if (vrd16(GEAR_SHIFT_FLAG) == 0) {
        charge(3);
        vwr16(SND_CH06, 0); vwr16(SND_CH0A, d0);
    } else {
        charge(2);
        vwr16(SND_CH06, d0); vwr16(SND_CH0A, 0);
    }
    charge(2);                                          /* subi.w, bpl */
    d0 = (uint16_t)(d0 - 0x50);
    if (d0 & 0x8000) { charge(1); d0 = 0; }
    charge(8);
    vwr16(SND_CH12, d0);
    vwr16(SND_CH04, 0);
    vwr16(SND_CH08, 0);
    d0 = (uint16_t)(vrd16(ENGINE_RPM) - vrd16(ENGINE_RPM_PREV));
    vwr16(ENGINE_RPM_DELTA, d0);
    vwr16(ENGINE_RPM_PREV, vrd16(ENGINE_RPM));
    set_d16(1, 0x80);
    charge(2);                                          /* cmpi.w, bcs */
    if ((uint16_t)vrd16(GEAR_SHIFT_FLAG) >= 0x8000) {
        /* computed and then overwritten below -- kept for the registers */
        charge(11);
        d0 = (uint16_t)(((uint16_t)vrd16(W(0x4022)) << 2) & 0xFFFE);
        int16_t s = (int16_t)(0 - (uint16_t)vrd16(a_reg(5) + (uint32_t)(int16_t)d0));
        uint32_t d1 = (uint32_t)((int32_t)s * 0x80) * 2;
        d1 = (d1 << 16) | (d1 >> 16);                   /* swap */
        uint16_t d2 = (uint16_t)(0x80 - (uint16_t)d1);
        set_d16(2, d2);
        set_d(1, (d1 & 0xFFFF0000u) | d2);
    }
    charge(5);                                          /* move.w, lea, move.w, tst.w, bne */
    set_d16(1, 0xFF);
    set_a(1, W(0x466C));
    d0 = 0;
    if (vrd16(ATTRACT_FLAG) == 0) { charge(1); d0 = (uint16_t)vrd16(PLAYER_CAR); }
    charge(2);                                          /* move.w, beq */
    uint16_t skid = (uint16_t)vrd16(W(0x466C) + (uint32_t)((int16_t)d0 * 2));
    vwr16(SKID_LEVEL, skid);
    if (skid != 0) {
        charge(3);                                      /* move.w, cmpi, bne */
        uint16_t d1 = 0;
        if (skid == 3) { charge(1); d1 = 0xE; }
        charge(3);
        set_d16(1, d1);
        vwr16(SND_CH10, d1);
        vwr16(SND_CH14, 0);
    } else {
        charge(6);                                      /* move.w x3, bpl (always: N from #2), and.w, beq */
        uint16_t d1 = (uint16_t)vrd16(W(0x21C4));
        d0 = (uint16_t)vrd16(W(0x20A4));
        uint16_t d2 = (uint16_t)(2 & vrd16(W(0x46CE)));
        set_d16(2, d2);
        if (d2 == 0) {
            charge(1);
            d1 = 0xFF;
        } else {
            charge(2);                                  /* tst.w, bpl */
            if (d0 & 0x8000) { charge(1); d0 = (uint16_t)(0 - d0); }
            charge(3);                                  /* subi.w, sub.w, bpl */
            d1 = (uint16_t)(d1 - 0x60 - d0);
            if (d1 & 0x8000) { charge(1); d1 = 0; }
            charge(2);                                  /* cmpi.w, ble */
            if ((int16_t)d1 > 0xFF) { charge(1); d1 = 0xFF; }
        }
        charge(4);                                      /* move.w, lsr.w, tst.w, bpl */
        vwr16(SND_CH10, d1);
        d1 = (uint16_t)(d1 >> 2);
        if (vrd16(W(0x20A4)) & 0x8000) { charge(1); d1 = (uint16_t)(0 - d1); }
        charge(1);
        set_d16(1, d1);
        vwr16(SND_CH14, d1);
    }
    set_d16(0, d0);
    charge(3);                                          /* two moves, rts */
    vwr16(SND_CH1C, 0);
    vwr16(SND_CH22, 0);
    return RD_RTS;
}

/* FUN_0001e930: the wind/road voice in slot 0x1A: off (FUN_0001e926) unless
 * the car is moving fast enough (0x4022 >= 0xE00, 0x41E2/4 != 0, no 0x47BC);
 * 0x40EE set leaves through FUN_0001e926 at once, 0x405E through
 * FUN_0001e910 / 0x1E9F0. */
static uint32_t rd_1e930(void)
{
    uint32_t r;
    charge(2);                                          /* move.w, beq */
    uint16_t d0 = (uint16_t)vrd16(W(0x40EE));
    set_d16(0, d0);
    if (d0 != 0) { charge(1); return RD_JMP(0x01E926); }
    charge(2);                                          /* tst.w, bne.w */
    if (vrd16(W(0x405E)) != 0) {
        charge(3);
        set_d16(0, (uint16_t)((uint16_t)vrd16(W(0x4022)) >> 1));
        if ((r = rd_call(L_1E910, 0x01E998))) return r;
        charge(1);
        return 0x01E9F0;
    }
    charge(2);                                          /* cmpi.w, bcs.w */
    if ((uint16_t)vrd16(W(0x4022)) >= 0xE00) {
        charge(2);                                      /* tst.w, bne.w */
        if (vrd16(W(0x47BC)) == 0) {
            charge(3);                                  /* move.w, lsr.w, beq.w */
            d0 = (uint16_t)((uint16_t)vrd16(W(0x41E2)) >> 2);
            set_d16(0, d0);
            if (d0 != 0) {
                charge(2);                              /* cmpi.w, bcs */
                if (d0 >= 0x2C00) { charge(1); set_d16(0, 0x2C00); }
                charge(1);
                if ((r = rd_call(L_1E99C, 0x01E982))) return r;
                charge(1);
                return 0x01E9B0;
            }
        }
    }
    charge(1);
    if ((r = rd_call(L_1E926, 0x01EA00))) return r;
    charge(4);                                          /* three moves, rts */
    vwr16(0x60005118u, 0);
    vwr16(0x6000511Eu, 0);
    vwr16(0x60005124u, 0);
    return RD_RTS;
}

/* FUN_0001eaa0: the nearest of the eight trackside sound sources (x at
 * 0x4448[i], z at 0x4470[i], Manhattan distance from the player at
 * 0x208C/0x2090) sets the ambient channel's volume and pitch (a Doppler term
 * from the distance change). D3w = its index, D6 = the distance; D0w ends
 * as the distance change / 4. */
static uint32_t rd_1eaa0(void)
{
    charge(2);                                          /* cmpi.w, beq */
    if ((uint16_t)vrd16(SND_VOICE_16) != 0x80C4) { charge(1); vwr16(SND_VOICE_16, 0x40C4); }
    charge(2);                                          /* move.w, move.l */
    uint32_t best = 0x7FFFFFFF;
    set_d(6, best);
    uint32_t d4 = 0, d5 = 0;
    for (int i = 7; ; i--) {
        charge(6);                                      /* moveq, moveq, move.w, move.w, sub.w, bpl */
        d4 = (uint16_t)(vrd16(W(0x208C)) - vrd16(W(0x4448) + 2u * (uint32_t)i));
        if (d4 & 0x8000) { charge(1); d4 = (uint16_t)(0 - d4); }
        charge(2);                                      /* sub.w, bpl */
        d5 = (uint16_t)(vrd16(W(0x2090)) - vrd16(W(0x4470) + 2u * (uint32_t)i));
        if (d5 & 0x8000) { charge(1); d5 = (uint16_t)(0 - d5); }
        charge(3);                                      /* add.l, cmp.l, blt */
        d5 += d4;
        if (!((int32_t)best < (int32_t)d5)) { charge(2); best = d5; set_d16(3, (uint16_t)i); }
        set_d(4, d4); set_d(5, d5); set_d(6, best);
        charge(1);                                      /* dbf */
        set_d16(7, (uint16_t)(i - 1));
        if (i == 0) break;
        poll();
    }
    charge(4);
    uint16_t delta = (uint16_t)(best - vrd16(W(0x44C0)));
    vwr16(W(0x44C2), delta);
    vwr16(W(0x44C0), (uint16_t)best);
    charge(3);                                          /* move.w, cmpi.l, bge */
    uint16_t d0 = 0xFF;
    if ((int32_t)best < 0x1000) {
        charge(5);                                      /* move.w, lsr, subi, cmpi, bge */
        d0 = (uint16_t)(((uint16_t)best >> 4) - 0x10);
        if ((int16_t)d0 < 0x10) { charge(1); d0 = 0x10; }
    }
    charge(2);                                          /* tst.w, beq */
    if (vrd16(SKID_LEVEL) != 0) { charge(1); d0 = 0xFF; }
    charge(11);
    vwr16(0x6000514Cu, d0 & 0xFF);
    vwr16(0x6000514Eu, (uint16_t)(((uint16_t)(0 - vrd16(W(0x44C2))) << 4) + 0x2500));
    vwr16(0x60005150u, 0);
    set_d16(0, (uint16_t)((int16_t)vrd16(W(0x44C2)) >> 2));
    return RD_RTS;
}

/* FUN_0001eb66: the nearest other car (bit 7 of its flag byte at +0x16C4,
 * position at +0x16B0/+0x16B4, 64-byte records) within 0xE00 sets the
 * passing-car channel: volume from the distance, pitch from its speed word
 * (+0x16C0) plus a Doppler term, pan from +0x16CE. Otherwise the channel is
 * silenced (0x1EB4C). */
__attribute__((unused)) static uint32_t rd_1eb66(void)
{
    charge(3);                                          /* move.w, move.l, moveq */
    uint32_t best = 0x7FFFFFFF;
    uint16_t near = 0xFFFF;
    set_d(6, best); set_d(5, 0xFFFFFFFFu);
    for (int car = 15; ; car--) {
        charge(3);                                      /* move.w, tst.w, beq */
        set_d16(0, (uint16_t)vrd16(PLAYER_CAR));
        if (vrd16(ATTRACT_FLAG) != 0) { charge(1); set_d(0, 0); }
        charge(2);                                      /* cmp.w, beq */
        if ((uint16_t)d_reg(0) != (uint16_t)car) {
            charge(4);                                  /* move.w, lsl.w, btst, beq */
            uint16_t off = (uint16_t)(car << 6);
            set_d16(0, off);
            if (vrd8(W(0x16C4) + off) & 0x80) {
                charge(6);                              /* moveq x2, move.w x2, sub.w, bpl */
                uint32_t d1 = (uint16_t)(vrd16(W(0x208C)) - vrd16(W(0x16B0) + off));
                if (d1 & 0x8000) { charge(1); d1 = (uint16_t)(0 - d1); }
                charge(2);                              /* sub.w, bpl */
                uint32_t d2 = (uint16_t)(vrd16(W(0x2090)) - vrd16(W(0x16B4) + off));
                if (d2 & 0x8000) { charge(1); d2 = (uint16_t)(0 - d2); }
                charge(3);                              /* add.l, cmp.l, bcc.w */
                d1 += d2;
                set_d(1, d1); set_d(2, d2);
                if (d1 < best) { charge(2); best = d1; near = (uint16_t)car; set_d(6, best); set_d16(5, near); }
            }
        }
        charge(1);                                      /* dbf */
        set_d16(7, (uint16_t)(car - 1));
        if (car == 0) break;
        poll();
    }
    charge(4);
    uint16_t delta = (uint16_t)(best - vrd16(W(0x44C4)));
    vwr16(W(0x44C6), delta);
    vwr16(W(0x44C4), (uint16_t)best);
    charge(3);                                          /* move.w, tst.w, bmi.w */
    set_d16(0, 0xFF);
    if (!(near & 0x8000)) {
        charge(2);                                      /* cmpi.l, bcc.w */
        if (best < 0xE00) {
            charge(2);                                  /* lsr.w, bpl (always: N clear after lsr) */
            uint16_t vol = (uint16_t)((uint16_t)best >> 3);
            charge(2);                                  /* cmpi.w, bcs */
            if (vol >= 0xFF) { charge(1); vol = 0xFF; }
            set_d16(6, vol);
            charge(11);
            vwr16(0x60005140u, vol);
            uint16_t off = (uint16_t)(near << 6);
            set_d16(5, off);
            uint16_t d1 = (uint16_t)((uint16_t)(0 - vrd16(W(0x44C6))) << 4);
            set_d16(1, d1);
            uint16_t d0 = (uint16_t)(vrd16(W(0x16C0) + (uint32_t)(int16_t)off) + d1 + 0x2800);
            if (d0 >= 0x6B00) { charge(1); d0 = 0x6B00; }
            charge(6);
            vwr16(0x60005142u, d0);
            d0 = (uint16_t)((int16_t)vrd16(W(0x16CE) + (uint32_t)(int16_t)off) >> 10);
            set_d16(0, d0);
            vwr16(0x60005144u, d0);
            return RD_RTS;
        }
    }
    charge(4);                                          /* 0x1EB4C: three moves, rts */
    vwr16(0x60005140u, 0xFF);
    vwr16(0x60005142u, 0);
    vwr16(0x60005144u, 0);
    return RD_RTS;
}

/* FUN_0001ec32: look up the track section the player is in -- a list per
 * course (0x2046) of (start, length, value) 8-byte entries at 0x1EC64,
 * ended by a negative start -- and store its value (3 if none) at 0x46CE */
static uint32_t rd_1ec32(void)
{
    charge(3);                                          /* move.w, lea, movea.l */
    set_d16(0, (uint16_t)vrd16(W(0x2046)));
    uint32_t a0 = vrd32(0x01EC64u + (uint32_t)((int16_t)d_reg(0) * 4));
    set_a(0, a0);
    for (;;) {
        charge(2);                                      /* tst.w, bmi */
        if (tst_w(a0) & 0x8000) { charge(2); vwr16(W(0x46CE), 3); return RD_RTS; }
        charge(4);                                      /* move.w, sub.w, cmp.w, bcc */
        uint16_t d1 = (uint16_t)(vrd16(W(0x204C)) - vrd16(a0));
        set_d16(1, d1);
        if (d1 < (uint16_t)vrd16(a0 + 2)) { charge(2); vwr16(W(0x46CE), vrd16(a0 + 4)); return RD_RTS; }
        charge(2);                                      /* adda.w, bra.b */
        a0 += 8;
        set_a(0, a0);
        poll();
    }
}

/* ======================================================================== */
/* display-list records                                                     */
/* ======================================================================== */

/* FUN_0001ed66: append the player-camera record at A3: header 0x725, an
 * offset (D0,D1,D2 -- mirrored when 0x2394), identity-ish words, then a
 * smoothed engine-shake angle (0x486E eases toward (RPM-5000)*5) looked up
 * in the sine table at A5 as (-sin, cos). A3 advances 13 longs. */
static uint32_t rd_1ed66(void)
{
    uint32_t a3 = a_reg(3);
    charge(6);                                          /* move.l, move.l x3, cmpi.w, bne */
    vwr32(a3, 0x725); a3 += 4;
    uint32_t d0 = 0xF9B, d1 = 0xF6A8, d2 = 0x2400;
    if (vrd16(W(0x4332)) == 1) { charge(3); d0 = 0xF33; d1 = 0xF6DA; d2 = 0x1810; }
    charge(2);                                          /* tst.w, beq */
    if (vrd16(W(0x2394)) != 0) { charge(1); d0 = 0u - d0; }
    charge(10);                                         /* ext.l x3, seven move.l */
    d0 = (uint32_t)(int16_t)d0; d1 = (uint32_t)(int16_t)d1; d2 = (uint32_t)(int16_t)d2;
    vwr32(a3, d0); vwr32(a3 + 4, d1); vwr32(a3 + 8, d2);
    vwr32(a3 + 12, 0); vwr32(a3 + 16, 0x7FFF); vwr32(a3 + 20, 0); vwr32(a3 + 24, 0x7FFF);
    a3 += 28;
    charge(4);                                          /* move.w, lsr.w, tst.w, beq */
    d1 = (d1 & 0xFFFF0000u) | ((uint16_t)vrd16(W(0x41E2)) >> 4);
    if (vrd16(W(0x4002)) != 0) { charge(1); d1 = 0; }
    charge(10);
    uint16_t w = (uint16_t)(vrd16(ENGINE_RPM) - 0x1388 + d1);
    d0 = ((uint32_t)((int32_t)(int16_t)w * 5)) & 0xFFFF;
    d1 = (uint16_t)vrd16(W(0x486E));
    d0 = (uint32_t)((int32_t)(d0 - d1) >> 4);
    if (!(d0 & 0x80000000u)) { charge(1); d0 += 1; }
    charge(4);                                          /* add.w, move.w, tst.w, beq */
    vwr16(W(0x486E), (uint16_t)(vrd16(W(0x486E)) + d0));
    uint16_t ang = (uint16_t)vrd16(W(0x486E));
    if (vrd16(W(0x2394)) != 0) { charge(1); ang = (uint16_t)(0 - ang); }
    charge(12);
    ang = (uint16_t)((ang - 0x7400) & 0xFFFE);
    uint16_t s = (uint16_t)vrd16(a_reg(5) + (uint32_t)(int16_t)ang);
    ang = (uint16_t)(ang + 0x4000);
    uint16_t c = (uint16_t)vrd16(a_reg(5) + (uint32_t)(int16_t)ang);
    d1 = (d1 & 0xFFFF0000u) | (uint16_t)(0 - s);
    d1 = (uint32_t)(int16_t)d1;
    d2 = (uint32_t)(int16_t)c;
    vwr32(a3, d1); vwr32(a3 + 4, d2); vwr32(a3 + 8, 0);
    a3 += 12;
    set_d(0, (d0 & 0xFFFF0000u) | ang);
    set_d(1, d1); set_d(2, d2);
    set_a(3, a3);
    return RD_RTS;
}

/* FUN_0001ece4: a display-list block 0x8001 holding the player-camera record
 * (FUN_0001ed66), terminated by -1; DLIST_PTR is left at the terminator */
static uint32_t rd_1ece4(void)
{
    uint32_t r;
    charge(4);
    uint32_t a3 = vrd32(DLIST_PTR);
    vwr32(a3, 0x8001); vwr32(a3 + 4, 0);
    set_a(3, a3 + 8);
    if ((r = rd_call(L_1ED66, 0x01ECF8))) return r;
    charge(3);
    vwr32(a_reg(3), 0xFFFFFFFFu);
    vwr32(DLIST_PTR, a_reg(3));
    return RD_RTS;
}

/* FUN_0001ed04: a display-list block 0x8001 holding the fixed 11-long record
 * at 0x1ED3A, terminated by -1 */
static uint32_t rd_1ed04(void)
{
    charge(18);
    uint32_t a1 = 0x01ED3A, a3 = vrd32(DLIST_PTR);
    vwr32(a3, 0x8001); vwr32(a3 + 4, 0);
    a3 += 8;
    for (int i = 0; i < 11; i++) { vwr32(a3, vrd32(a1)); a3 += 4; a1 += 4; }
    vwr32(a3, 0xFFFFFFFFu);
    vwr32(DLIST_PTR, a3);
    set_a(1, a1);
    set_a(3, a3);
    return RD_RTS;
}

/* ======================================================================== */
/* player car: engine and speed bookkeeping                                 */
/* ======================================================================== */

#define CAR_SPEED        W(0x4022)   /* word: player car speed                         */

/* 68K divs.w <src>,Dn with a 16-bit register divisor */
static uint32_t divs_wr(uint32_t dn, uint16_t d) { return divs_w(dn, (int16_t)d); }

/* FUN_0001bd04: 0x4038 = 0x67AE * (0x4084) / max(speed, 0x340) / 16 + (0x4086)
 * - (0x4004) (negated when 0x4798 < 0); 0 when the car is standing */
static uint32_t rd_1bd04(void)
{
    charge(3);                                          /* moveq, move.w, beq.w */
    set_d(0, 0);
    uint16_t v = (uint16_t)vrd16(CAR_SPEED);
    set_d16(1, v);
    if (v != 0) {
        charge(2);                                      /* cmpi.w, bcc */
        if (v < 0x340) { charge(1); v = 0x340; set_d16(1, v); }
        charge(8);
        uint32_t d0 = (uint32_t)(0x67AE * (int32_t)(int16_t)vrd16(W(0x4084)));
        d0 = divs_wr(d0, v);
        uint16_t w = (uint16_t)(((int16_t)d0 >> 4) + vrd16(W(0x4086)));
        uint16_t d1 = (uint16_t)vrd16(W(0x4004));
        if (vrd16(W(0x4798)) & 0x8000) { charge(1); d1 = (uint16_t)(0 - d1); }
        charge(1);                                      /* sub.w */
        set_d16(1, d1);
        set_d(0, (d0 & 0xFFFF0000u) | (uint16_t)(w - d1));
    }
    charge(2);                                          /* move.w, rts */
    vwr16(W(0x4038), (uint16_t)d_reg(0));
    return RD_RTS;
}

/* FUN_0001bd86: two scaled products (0x4072, 0x4074) from the words at
 * 0x4064 / 0x4066 against a common correction term; the fixed-point chain is
 * kept exactly (purpose not yet identified) */
static uint32_t rd_1bd86(void)
{
    charge(23);
    uint32_t d0 = (uint32_t)((int32_t)(int16_t)vrd16(W(0x4024)) * (int16_t)vrd16(W(0x419A)));
    d0 = (d0 << 16) | (d0 >> 16);
    d0 = (uint32_t)((int32_t)(int16_t)d0 * (int16_t)vrd16(W(0x4244)));
    uint32_t k1 = vrd16(W(0x424E)), k2 = vrd16(W(0x4252));
    uint32_t d1 = vrd16(W(0x4064)) * k1 - d0;
    d1 <<= 5; d1 = (d1 << 16) | (d1 >> 16);
    d1 = (d1 & 0xFFFF) * k2; d1 = (d1 << 16) | (d1 >> 16);
    d1 = (d1 & 0xFFFF0000u) | (uint16_t)((uint16_t)d1 >> 3);
    vwr16(W(0x4072), (uint16_t)d1);
    d1 = vrd16(W(0x4066)) * k1 + d0;
    d1 <<= 5; d1 = (d1 << 16) | (d1 >> 16);
    d1 = (d1 & 0xFFFF) * k2; d1 = (d1 << 16) | (d1 >> 16);
    d1 = (d1 & 0xFFFF0000u) | (uint16_t)((uint16_t)d1 >> 3);
    vwr16(W(0x4074), (uint16_t)d1);
    set_d(0, d0); set_d(1, d1);
    return RD_RTS;
}

/* FUN_0001cb34: engine RPM toward its target 0x4018 = 5000 + 0x40B4 scaled
 * (rise rate 0x1800, fall 0xC00), step to 0x409C, clamped to 5000..0x36B0 */
static uint32_t rd_1cb34(void)
{
    charge(9);
    uint32_t d0 = (uint32_t)vrd16(GEAR_SHIFT_FLAG) * 0x36B0;
    d0 = (d0 << 16) | (d0 >> 16);
    uint16_t target = (uint16_t)(d0 + 0x1388);
    vwr16(W(0x4018), target);
    uint16_t rate = 0x1800;
    uint16_t rpm = (uint16_t)vrd16(ENGINE_RPM);
    uint16_t diff = (uint16_t)(target - rpm);
    if (target < rpm) { charge(1); rate = 0xC00; }
    charge(7);
    d0 = (uint32_t)((int32_t)(int16_t)diff * (int16_t)rate);
    d0 = (d0 << 16) | (d0 >> 16);
    vwr16(W(0x409C), (uint16_t)d0);
    uint16_t nrpm = (uint16_t)(d0 + rpm);
    d0 = (d0 & 0xFFFF0000u) | nrpm;
    vwr16(ENGINE_RPM, nrpm);
    if ((uint16_t)vrd16(ENGINE_RPM) < 0x1388) { charge(1); vwr16(ENGINE_RPM, 0x1388); }
    charge(4);                                          /* move.w, move.w, cmp.w, bls */
    uint16_t d1 = (uint16_t)vrd16(ENGINE_RPM);
    set_d(0, d0); set_d16(1, d1); set_d16(2, 0x36B0);
    if (d1 > 0x36B0) { charge(1); vwr16(ENGINE_RPM, 0x36B0); }
    charge(1);
    return RD_RTS;
}

/* FUN_0001d3c6: while |0x4284| < 6 and the car is moving (speed >= 0xE00),
 * a growing count 0x4142 is subtracted from 0x4052 (floored at 0);
 * otherwise the count is reset */
static uint32_t rd_1d3c6(void)
{
    charge(2);                                          /* move.w, bpl */
    uint16_t d0 = (uint16_t)vrd16(W(0x4284));
    if (d0 & 0x8000) { charge(1); d0 = (uint16_t)(0 - d0); }
    set_d16(0, d0);
    charge(2);                                          /* cmpi.w, bcc */
    if (d0 < 6) {
        charge(2);                                      /* cmpi.w, bcs */
        if ((uint16_t)vrd16(CAR_SPEED) >= 0xE00) {
            charge(4);                                  /* addq, move, sub, bpl */
            vwr16(W(0x4142), (uint16_t)(vrd16(W(0x4142)) + 1));
            d0 = (uint16_t)vrd16(W(0x4142));
            set_d16(0, d0);
            uint16_t v = (uint16_t)(vrd16(W(0x4052)) - d0);
            vwr16(W(0x4052), v);
            if (v & 0x8000) { charge(1); vwr16(W(0x4052), 0); }
            charge(1);
            return RD_RTS;
        }
    }
    charge(2);                                          /* clr.w, rts */
    vwr16(W(0x4142), 0);
    return RD_RTS;
}

/* FUN_0001da8c: the sector (0..2, times 2) of the angle 0x41AC in thirds of a
 * turn, passed in D1w to FUN_0001ea60 */
static uint32_t rd_1da8c(void)
{
    uint32_t r;
    charge(3);                                          /* move.w, addi.w, move.w */
    uint16_t d0 = (uint16_t)(vrd16(W(0x41AC)) + 0x2AAA);
    uint32_t d1 = (d_reg(1) & 0xFFFF0000u) | 0xFFFF;
    for (;;) {
        charge(3);                                      /* addq.l, subi.w, bpl */
        d1 += 1;
        d0 = (uint16_t)(d0 - 0x5555);
        set_d16(0, d0); set_d(1, d1);
        if (d0 & 0x8000) break;
        poll();
    }
    charge(2);                                          /* add.w, jsr */
    set_d16(1, (uint16_t)(d1 * 2));
    if ((r = rd_call(L_1EA60, 0x01DAA8))) return r;
    charge(1);
    return RD_RTS;
}

/* FUN_0001daaa: a counter at 0x405E that rises by 0x60 (capped below 0x800)
 * while the car is fast (>= 0x1000) and the lateral word |0x20A6| exceeds the
 * side's limit (0x21C8 / 0x21CA), and is cleared otherwise */
static uint32_t rd_1daaa(void)
{
    charge(3);                                          /* move.w, move.w, bmi */
    uint16_t d2 = (uint16_t)vrd16(W(0x21C8));
    uint16_t d1 = (uint16_t)vrd16(W(0x20A6));
    if (!(d1 & 0x8000)) { charge(1); d2 = (uint16_t)vrd16(W(0x21CA)); }
    set_d16(1, d1); set_d16(2, d2);
    charge(2);                                          /* cmpi.w, bcs */
    if ((uint16_t)vrd16(CAR_SPEED) >= 0x1000) {
        charge(2);                                      /* tst.w, bpl */
        if (d1 & 0x8000) { charge(1); d1 = (uint16_t)(0 - d1); }
        charge(2);                                      /* sub.w, bcs.w */
        set_d16(1, d1);
        set_d16(2, (uint16_t)(d2 - d1));
        if (d2 < d1) {
            charge(4);                                  /* move.w, addi.w, cmpi.w, bcc */
            uint16_t d0 = (uint16_t)(vrd16(W(0x405E)) + 0x60);
            set_d16(0, d0);
            if (d0 < 0x800) { charge(1); vwr16(W(0x405E), d0); }
            charge(1);
            return RD_RTS;
        }
    }
    charge(2);
    vwr16(W(0x405E), 0);
    return RD_RTS;
}

/* ======================================================================== */
/* gears                                                                     */
/* ======================================================================== */

#define GEAR             W(0x4006)   /* word: current gear, 0..5 (-1 = neutral/reverse) */
#define SHIFT_MANUAL     W(0x2340)   /* word: nonzero = manual transmission            */

/* the automatic gearbox's per-gear speed thresholds (the code at 0x1DB82..):
 * shift up at `up` or more, down below `down`; 0 = no such test */
static const struct { uint16_t up, down; } auto_shift[6] = {
    { 0x480, 0 }, { 0x850, 0x484 }, { 0xB40, 0x854 },
    { 0xE80, 0xB44 }, { 0x1170, 0xE84 }, { 0, 0x1174 },
};

/* FUN_0001db4e: automatic gearbox -- one gear up or down from the speed,
 * dispatched through the table at 0x1DC04 on the gear; with a manual
 * transmission it continues in FUN_0001dc20 (the shift lever) */
static uint32_t rd_1db4e(void)
{
    charge(3);                                          /* move.w, tst.w, bne.w */
    vwr16(W(0x4796), 0);
    if (vrd16(SHIFT_MANUAL) != 0) return 0x01DC20;
    charge(9);                                          /* ... movea.l, move.w, jmp (A3) */
    vwr16(W(0x4790), 0);
    uint16_t idx = (uint16_t)(((vrd16(GEAR) + 1) & 0xF) << 2);
    uint32_t target = vrd32(0x01DC04u + (uint32_t)(int16_t)idx);
    set_a(3, target);
    uint16_t gear = (uint16_t)vrd16(GEAR);
    set_d16(4, gear);
    int g = -1;
    static const uint32_t cases[7] = { 0x1DB7E, 0x1DB82, 0x1DB8E, 0x1DBA2, 0x1DBB6, 0x1DBCA, 0x1DBDE };
    for (int i = 0; i < 7; i++) if (target == cases[i]) g = i - 1;
    if (g < -1 || (g == -1 && target != cases[0])) return RD_JMP(target);   /* past the table */
    poll();                                             /* jmp (A3) */
    if (g >= 0) {
        uint16_t speed = (uint16_t)vrd16(CAR_SPEED);
        if (auto_shift[g].up) {
            charge(2);                                  /* cmpi.w, bcc */
            if (speed >= auto_shift[g].up) {
                charge(2);                              /* cmpi.w, bcc */
                if ((uint16_t)vrd16(GEAR) >= 5) { charge(1); return RD_RTS; }
                charge(3);                              /* addq.w, bra.b, rts */
                vwr16(GEAR, (uint16_t)(vrd16(GEAR) + 1));
                return RD_RTS;
            }
        }
        if (auto_shift[g].down) {
            charge(2);                                  /* cmpi.w, bcs */
            if (speed < auto_shift[g].down) {
                charge(2);                              /* cmpi.w, bcs */
                if ((uint16_t)vrd16(GEAR) < 1) { charge(1); return RD_RTS; }
                charge(2);                              /* subq.w, rts */
                vwr16(GEAR, (uint16_t)(vrd16(GEAR) - 1));
                return RD_RTS;
            }
        }
    }
    charge(2);                                          /* bra, rts */
    return RD_RTS;
}

#define SHIFT_LEVER      0x60004030u /* word: the gear lever position bits (I/O board) */

/* FUN_0001dc20: manual gearbox. The H-pattern lever (0x60004030) is decoded
 * through the table at 0x1DC74 into a gear (bit 4 clear = clutch in, D2 = 1);
 * an unknown position sets 0x4796. A gear change with the clutch out sets
 * 0x405A = 0x80. With 0xDDE set it is a sequential shifter instead: bit 0 /
 * bit 1 falling edges shift down / up. */
static uint32_t rd_1dc20(void)
{
    charge(2);                                          /* tst.w, bne.w */
    if (vrd16(W(0x0DDE)) == 0) {
        charge(9);
        set_a(1, SHIFT_LEVER);
        uint16_t raw = (uint16_t)vrd16(SHIFT_LEVER);
        uint32_t d2 = 1, d3 = 0;
        uint16_t d1 = raw;
        uint16_t off = (uint16_t)(~(uint16_t)(raw + raw) & 0x1E);
        uint16_t d0 = (uint16_t)vrd16(0x01DC74u + (uint32_t)(int16_t)off);
        charge(1);                                      /* bmi */
        if (d0 & 0x8000) {
            charge(1);
            vwr16(W(0x4796), 0xFFFF);
        } else {
            charge(5);                                  /* subq, move, moveq, andi, bne */
            d0 = (uint16_t)(d0 - 1);
            d3 = d0;
            d2 = 0;
            d1 &= 0x10;
            if (d1 == 0) { charge(2); d2 = 1; }
        }
        set_d16(0, d0); set_d16(1, d1); set_d(2, d2); set_d(3, d3);
        charge(2);                                      /* cmp.w, beq */
        if ((uint16_t)d3 != (uint16_t)vrd16(GEAR)) {
            charge(2);                                  /* tst.w, bne */
            if (d2 == 0) { charge(1); vwr16(W(0x405A), 0x80); }
        }
        charge(3);
        vwr16(GEAR, (uint16_t)d3);
        vwr16(W(0x4790), (uint16_t)d2);
        return RD_RTS;
    }
    charge(6);                                          /* move.w x2, andi.w x2, cmp.w, beq */
    uint16_t d0 = (uint16_t)(vrd16(SHIFT_LEVER) & 3);
    uint16_t d1 = (uint16_t)(vrd16(W(0x4342)) & 3);
    if (d0 != d1) {
        charge(3);                                      /* move.w, andi.w, bne */
        d1 = d0;
        d0 &= 1;
        if (d0 == 0) {
            charge(2);                                  /* move.w, ble */
            d0 = (uint16_t)vrd16(GEAR);
            if ((int16_t)d0 > 0) { charge(1); vwr16(GEAR, (uint16_t)(vrd16(GEAR) - 1)); }
        }
        charge(2);                                      /* andi.w, bne */
        d1 &= 2;
        if (d1 == 0) {
            charge(2);                                  /* cmpi.w, bge.w */
            if ((int16_t)vrd16(GEAR) < 5) { charge(1); vwr16(GEAR, (uint16_t)(vrd16(GEAR) + 1)); }
        }
    }
    set_d16(0, d0); set_d16(1, d1);
    charge(3);
    vwr16(W(0x4342), vrd16(SHIFT_LEVER));
    vwr16(W(0x4790), 0);
    return RD_RTS;
}

/* ======================================================================== */
/* jumps (the car leaving the ground)                                       */
/* ======================================================================== */

#define JUMP_VEL         W(0x47BC)   /* long: upward velocity while airborne (its high word is used as a step) */
#define JUMP_GRAVITY     W(0x47C0)   /* long: accumulated fall term, capped at 0x10000 */
#define CAR_Y            W(0x208E)   /* word: player car height (purpose inferred)   */
#define GROUND_Y         W(0x20AC)   /* word: road height under the car              */

/* 68K divu.w <n>,Dn: quotient:remainder unless the quotient overflows */
static uint32_t divu_w(uint32_t dn, uint16_t n)
{
    uint64_t q = UDIVREM(dn, n, '/'), rm = UDIVREM(dn, n, '%');
    if (q > 0xFFFF) return dn;
    return ((uint32_t)rm << 16) | (uint32_t)q;
}

/* FUN_0001dd82: jump physics. The course's list of jump points (per course
 * 0x2046, table at 0x1DDB2, words ended by a negative one) launches the car
 * when it passes within 0x10 of one at speed >= 0xE00 while above the road;
 * in the air the car rises by JUMP_VEL's high word and falls back; on landing
 * (FUN_0001ea30) the speed goes to 0x41E4 and the state is cleared. */
static uint32_t rd_1dd82(void)
{
    uint32_t r;
    charge(6);
    vwr16(W(0x47D4), 0);
    uint32_t a1 = vrd32(0x01DDB2u + (uint32_t)((vrd16(W(0x2046)) & 0xF) * 4));
    set_a(0, 0x01DDB2); set_a(1, a1);
    set_d(0, vrd16(W(0x2046)) & 0xF);
    int launch = 0;
    for (;;) {
        charge(2);                                      /* tst.w, bmi.w */
        if (tst_w(a1) & 0x8000) break;
        charge(4);                                      /* move.w, sub.w, cmpi.w, bcs */
        uint16_t d0 = (uint16_t)(vrd16(W(0x204C)) - vrd16(a1));
        set_d16(0, d0);
        if (d0 < 0x10) { launch = 1; break; }
        charge(2);                                      /* adda.w, bra.b */
        a1 += 2; set_a(1, a1);
        poll();
    }
    if (launch) {
        charge(2);                                      /* cmpi.w, bcs.w */
        if ((uint16_t)vrd16(CAR_SPEED) < 0xE00) goto landed;
        charge(3);                                      /* move.w, sub.w, bge.w */
        uint16_t d0 = (uint16_t)(vrd16(GROUND_Y) - vrd16(CAR_Y));
        set_d16(0, d0);
        if ((int16_t)vrd16(GROUND_Y) >= (int16_t)vrd16(CAR_Y)) goto touchdown;
        charge(2);                                      /* move.l, bra.b */
        vwr32(JUMP_VEL, 0x6800);
    } else {
        charge(2);                                      /* tst.l, bne.w */
        if (vrd32(JUMP_VEL) == 0) { charge(1); goto landed; }
        charge(3);                                      /* move.w, sub.w, bge.w */
        uint16_t d0 = (uint16_t)(vrd16(GROUND_Y) - vrd16(CAR_Y));
        set_d16(0, d0);
        if ((int16_t)vrd16(GROUND_Y) >= (int16_t)vrd16(CAR_Y)) goto touchdown;
        charge(1);
        vwr32(JUMP_VEL, vrd32(JUMP_VEL) + 0xC000);
    }
    {   /* 0x1DE4C: airborne step */
        charge(4);                                      /* move.l, addi.l, cmpi.l, ble */
        uint32_t d0 = vrd32(JUMP_GRAVITY) + 0x400;
        if ((int32_t)d0 > 0x10000) { charge(1); d0 = 0x10000; }
        charge(12);
        vwr32(JUMP_GRAVITY, d0);
        uint16_t d1 = (uint16_t)(vrd16(CAR_Y) - vrd16(JUMP_VEL));
        vwr16(CAR_Y, d1);
        d0 = (uint32_t)(uint16_t)vrd16(JUMP_VEL) * 0x10;
        vwr16(W(0x47B6), (uint16_t)(vrd16(W(0x47B6)) - d0));
        uint16_t d2 = (uint16_t)vrd16(W(0x47B6));
        vwr16(W(0x209E), d2);
        set_d16(1, d1); set_d16(2, d2);
        set_d(0, (d0 & 0xFFFF0000u) | (uint16_t)(vrd16(GROUND_Y) - vrd16(CAR_Y)));
        if ((int16_t)vrd16(GROUND_Y) < (int16_t)vrd16(CAR_Y)) { charge(1); return RD_RTS; }
    }
touchdown:
    charge(6);
    set_d(0, divu_w(vrd32(JUMP_VEL), 0x430));
    vwr16(W(0x41E4), (uint16_t)d_reg(0));
    vwr16(W(0x41FC), 0);
    vwr16(W(0x47D4), 0xFFFF);
    if ((r = rd_call(L_1EA30, 0x01DEB2))) return r;
landed:
    charge(9);
    set_d16(0, (uint16_t)(vrd16(GROUND_Y) - vrd16(CAR_Y)));
    vwr16(W(0x47CE), (uint16_t)d_reg(0));
    vwr16(CAR_Y, vrd16(GROUND_Y));
    vwr16(W(0x209E), vrd16(W(0x20AE)));
    vwr16(W(0x47B6), vrd16(W(0x209E)));
    vwr32(JUMP_VEL, 0);
    vwr32(JUMP_GRAVITY, 0);
    return RD_RTS;
}

/* FUN_0001deda: a boost bump -- when airborne-capable (JUMP_VEL set, speed >=
 * 0x1300) and 0x412A & 0x4200, or when 0x47C4 requests it, start a 4-unit
 * sine bounce (0x47C8 phase from 0x1000), add 0x412A + 0x300 to the speed
 * (capped at 0x2000); then advance the bounce, offsetting CAR_Y by
 * -sin(phase) * 0x47CA until the phase passes 0x8000 */
static uint32_t rd_1deda(void)
{
    int start = 0;
    charge(2);                                          /* tst.w, beq */
    if (vrd16(JUMP_VEL) != 0) {
        charge(2);                                      /* cmpi.w, bcs.w */
        if ((uint16_t)vrd16(CAR_SPEED) >= 0x1300) {
            charge(2);                                  /* move.w, beq */
            uint16_t d0 = (uint16_t)vrd16(W(0x412A));
            set_d16(0, d0);
            if (d0 != 0) {
                charge(3);                              /* move.w, and.w, bne */
                uint16_t d1 = (uint16_t)vrd16(W(0x4200));
                set_d16(1, d1);
                d0 &= d1;
                set_d16(0, d0);
                if (d0 != 0) start = 1;
            }
        }
    }
    if (!start) {
        charge(2);                                      /* tst.w, beq.w */
        if (vrd16(W(0x47C4)) != 0) start = 1;
    }
    if (start) {
        charge(10);
        vwr16(W(0x47C4), 0);
        vwr16(W(0x47CA), 4);
        vwr16(W(0x47C8), 0x1000);
        vwr16(W(0x47C6), 0);
        vwr32(JUMP_VEL, 0x4800);
        uint16_t d0 = (uint16_t)(vrd16(W(0x412A)) + 0x300);
        set_d16(0, d0);
        vwr16(CAR_SPEED, (uint16_t)(vrd16(CAR_SPEED) + d0));
        if ((uint16_t)vrd16(CAR_SPEED) >= 0x2000) { charge(1); vwr16(CAR_SPEED, 0x2000); }
        charge(1);
        vwr16(W(0x47B6), vrd16(W(0x209E)));
    }
    charge(4);                                          /* move.w, addi.w, cmpi.w, bcc */
    uint16_t ph = (uint16_t)(vrd16(W(0x47C8)) + 0x400);
    set_d16(0, ph);
    if (ph >= 0x8000) { charge(1); return RD_RTS; }
    charge(10);
    vwr16(W(0x47C8), ph);
    ph &= 0xFFFE;
    set_d16(0, ph);
    int16_t s = (int16_t)(0 - (uint16_t)vrd16(a_reg(5) + (uint32_t)(int16_t)ph));
    uint32_t d1 = (uint32_t)((int32_t)s * (int16_t)vrd16(W(0x47CA))) * 2;
    d1 = (d1 << 16) | (d1 >> 16);
    set_d(1, d1);
    vwr16(W(0x47C6), (uint16_t)d1);
    vwr16(CAR_Y, (uint16_t)(vrd16(CAR_Y) + d1));
    return RD_RTS;
}

/* step a signed word toward 0 by `step`, clearing it and its partner when it
 * would cross (the shape at 0x1DF76 and 0x1DF9C) */
static uint16_t decay_toward_zero(uint32_t word, uint32_t partner, uint16_t step)
{
    charge(2);                                          /* move.w, bmi */
    uint16_t d0 = (uint16_t)vrd16(word);
    if (!(d0 & 0x8000)) {
        charge(2);                                      /* subi.w, ble */
        d0 = (uint16_t)(d0 - step);
        if ((int16_t)d0 > 0) { charge(2); vwr16(word, d0); return d0; }
    } else {
        charge(2);                                      /* addi.w, bge */
        d0 = (uint16_t)(d0 + step);
        if ((int16_t)d0 < 0) { charge(2); vwr16(word, d0); return d0; }
    }
    charge(2);                                          /* clr.w x2 */
    vwr16(word, 0);
    vwr16(partner, 0);
    return d0;
}

/* FUN_0001df6a: advance two spin phases (0x41FC / 0x41FE) by 0x1444 and let
 * their amplitudes 0x41E4 / 0x41E6 decay toward zero by 0x28 / 0xF0 */
static uint32_t rd_1df6a(void)
{
    charge(3);
    vwr16(W(0x41FC), (uint16_t)(vrd16(W(0x41FC)) + 0x1444));
    vwr16(W(0x41FE), (uint16_t)(vrd16(W(0x41FE)) + 0x1444));
    decay_toward_zero(W(0x41E4), W(0x41FC), 0x28);
    uint16_t d0 = decay_toward_zero(W(0x41E6), W(0x41FE), 0xF0);
    set_d16(0, d0);
    charge(1);
    return RD_RTS;
}

/* ======================================================================== */
/* camera / view                                                            */
/* ======================================================================== */

#define VIEW_MODE        W(0x4332)   /* word: player view (bit 0: cockpit / behind)  */

/* v = v - target; if nonzero: v >>= 4 (arithmetic), +1 unless negative --
 * one easing step of the view offsets (the shape at 0x1E07E..0x1E0AE).
 * `add` is added before the zero test (used for the Y offset). */
static uint32_t ease_step(uint32_t dn, uint16_t target, uint16_t add, int with_add)
{
    charge(with_add ? 3 : 2);                           /* sub.w, [add.w], beq */
    uint16_t v = (uint16_t)((uint16_t)dn - target);
    if (with_add) v = (uint16_t)(v + add);
    dn = (dn & 0xFFFF0000u) | v;
    if (v == 0) return dn;
    charge(2);                                          /* asr.w, bmi */
    v = (uint16_t)((int16_t)v >> 4);
    dn = (dn & 0xFFFF0000u) | v;
    if (v & 0x8000) return dn;
    charge(1);                                          /* addq.l */
    return dn + 1;
}

/* FUN_0001e006: the view. On the VIEW button (bit 4 of 0x10000812) toggle
 * VIEW_MODE, set the player car's display word from the table at 0x1DFF6 and
 * rebuild it (FUN_0000b56e; FUN_0001dfc6 for the rear view). Then ease the
 * four camera offsets 0x4334..0x433A toward the mode's targets at 0x1E1D2
 * (+ a speed-bob term on Y), publish them (0x20BC..0x20C2, rotated by the
 * heading 0x2198), and every 8th mode value run the free-look from the shift
 * buttons (0x433C..0x4340, 0x2108). */
static uint32_t rd_1e006(void)
{
    uint32_t r;
    charge(2);                                          /* btst.b, beq */
    if (vrd8(0x10000812u) & 0x10) {
        charge(13);
        uint16_t mode = (uint16_t)((vrd16(VIEW_MODE) + 1) & 1);
        set_d16(0, mode);
        vwr16(VIEW_MODE, mode);
        vwr16(W(0x20B0), mode);
        set_a(0, 0x01DFF6);
        uint16_t dw = (uint16_t)vrd16(0x01DFF6u + 2u * mode);
        set_d16(1, dw);
        uint16_t off = (uint16_t)(vrd16(W(0x0C40)) << 7);
        set_d16(0, off);
        uint32_t a4 = 0x10008000u + (uint32_t)(int16_t)off;
        set_a(4, a4);
        vwr16(a4 + 0x1C, dw);
        vwr16(a4, (uint16_t)(vrd16(a4) | 4));
        if ((r = rd_call(L_B56E, 0x01E040))) return r;
        charge(2);                                      /* tst.w, bne */
        if (vrd16(W(0x20B0)) == 0) {
            charge(1);
            if ((r = rd_call(L_1DFC6, 0x01E04A))) return r;
        }
    }
    charge(15);
    uint32_t a1 = 0x01E1D2;
    set_a(1, a1);
    uint16_t mi = (uint16_t)(vrd16(VIEW_MODE) & 0xF);
    set_d16(4, mi);
    uint16_t ang = (uint16_t)(vrd16(W(0x209E)) & 0xFFFE);
    uint16_t sn = (uint16_t)(0 - (uint16_t)vrd16(a_reg(5) + (uint32_t)(int16_t)ang));
    uint16_t cy = (uint16_t)vrd16(W(0x4334));
    uint32_t d5 = (uint32_t)((int32_t)(int16_t)sn * (int16_t)cy) * 2;
    d5 = (d5 << 16) | (d5 >> 16);
    set_d(5, d5);
    uint32_t d0 = (d_reg(0) & 0xFFFF0000u) | (uint16_t)vrd16(W(0x4334));
    uint32_t d1 = (d_reg(1) & 0xFFFF0000u) | (uint16_t)vrd16(W(0x4336));
    uint32_t d2 = (d_reg(2) & 0xFFFF0000u) | (uint16_t)vrd16(W(0x4338));
    uint32_t d3 = (d_reg(3) & 0xFFFF0000u) | (uint16_t)vrd16(W(0x433A));
    uint32_t t = a1 + (uint32_t)((int16_t)mi * 8);
    d0 = ease_step(d0, (uint16_t)vrd16(t + 0), 0, 0);
    d1 = ease_step(d1, (uint16_t)vrd16(t + 2), 0, 0);
    d2 = ease_step(d2, (uint16_t)vrd16(t + 4), (uint16_t)d5, 1);
    d3 = ease_step(d3, (uint16_t)vrd16(t + 6), 0, 0);
    charge(4);
    vwr16(W(0x4334), (uint16_t)(vrd16(W(0x4334)) - d0));
    vwr16(W(0x4336), (uint16_t)(vrd16(W(0x4336)) - d1));
    vwr16(W(0x4338), (uint16_t)(vrd16(W(0x4338)) - d2));
    vwr16(W(0x433A), (uint16_t)(vrd16(W(0x433A)) - d3));
    charge(9);
    d1 = (d1 & 0xFFFF0000u) | (uint16_t)vrd16(W(0x4336));
    d2 = (d2 & 0xFFFF0000u) | (uint16_t)vrd16(W(0x4338));
    d3 = (d3 & 0xFFFF0000u) | (uint16_t)vrd16(W(0x4334));
    set_d16(4, (uint16_t)vrd16(W(0x433A)));
    uint16_t y = (uint16_t)((uint16_t)d1 + vrd16(W(0x41E8)));
    vwr16(W(0x20C2), y);
    vwr16(W(0x20BE), (uint16_t)d2);
    charge(16);                                         /* ... bra.w */
    uint16_t h = (uint16_t)((vrd16(W(0x2198)) - 0x4000) & 0xFFFE);
    uint16_t s1 = (uint16_t)vrd16(a_reg(5) + (uint32_t)(int16_t)h);
    h = (uint16_t)(h + 0x4000);
    uint16_t c1 = (uint16_t)vrd16(a_reg(5) + (uint32_t)(int16_t)h);
    d0 = (d0 & 0xFFFF0000u) | h;
    s1 = (uint16_t)(0 - s1);
    d1 = (uint32_t)((int32_t)(int16_t)s1 * (int16_t)d3) * 2;
    d1 = (d1 << 16) | (d1 >> 16);
    vwr16(W(0x20C0), (uint16_t)d1);
    d2 = (uint32_t)((int32_t)(int16_t)c1 * (int16_t)d3) * 2;
    d2 = (d2 << 16) | (d2 >> 16);
    vwr16(W(0x20BC), (uint16_t)d2);
    set_d(1, d1); set_d(2, d2); set_d(3, d3);
    charge(4);                                          /* move.w, andi.w, cmpi.w, bne */
    uint16_t m7 = (uint16_t)(vrd16(VIEW_MODE) & 7);
    d0 = (d0 & 0xFFFF0000u) | m7;
    if (m7 != 7) {
        charge(3);
        set_d(0, d0);
        vwr16(W(0x433C), 0);
        vwr16(W(0x4340), vrd16(W(0x2108)));
        return RD_RTS;
    }
    charge(5);                                          /* move.w, move.w, andi.w, cmpi.w, bne */
    vwr16(W(0x433C), 0xFFFF);
    uint16_t btn = (uint16_t)(vrd16(SHIFT_LEVER) & 3);
    if (btn == 3) { charge(1); vwr16(W(0x433E), 0x100); }
    charge(6);                                          /* move.w, addq.w, move.w, lsr.w, andi.w, bne */
    vwr16(W(0x433E), (uint16_t)(vrd16(W(0x433E)) + 8));
    uint16_t step = (uint16_t)((uint16_t)vrd16(W(0x433E)) >> 8);
    set_d16(2, step);
    set_d16(1, btn);
    d0 = (d0 & 0xFFFF0000u) | (btn & 1);
    if ((btn & 1) == 0) { charge(1); vwr16(W(0x4340), (uint16_t)(vrd16(W(0x4340)) + step)); }
    charge(2);                                          /* andi.w, bne */
    set_d16(1, btn & 2);
    if ((btn & 2) == 0) { charge(1); vwr16(W(0x4340), (uint16_t)(vrd16(W(0x4340)) - step)); }
    charge(2);
    set_d(0, d0);
    vwr16(W(0x2108), vrd16(W(0x4340)));
    return RD_RTS;
}

/* FUN_0001d300: bit n of 0x412E = car n (not the player; not car 0 outside a
 * race) is active (flag bit 7), roughly facing us (|0x16CE + 0x2000| < 0x4000)
 * and within Manhattan distance 0x384. With any such car and speed >= 0xE00
 * the slipstream counter 0x412A rises to 0xA0; otherwise it falls and 0x4052
 * is halved (or reduced by 10 outside a race). */
static uint32_t rd_1d300(void)
{
    charge(2);                                          /* moveq, moveq */
    uint32_t d6 = 0;
    set_d(7, 0xF); set_d(6, 0);
    for (int car = 15; ; car--) {
        int skip = 0;
        charge(2);                                      /* tst.w, beq */
        if (vrd16(ATTRACT_FLAG) != 0) {
            charge(2);                                  /* tst.w, beq.w */
            if (car == 0) skip = 1; else charge(1);     /* bra.w */
        } else {
            charge(2);                                  /* cmp.w, beq.w */
            if ((uint16_t)vrd16(PLAYER_CAR) == (uint16_t)car) skip = 1;
        }
        if (!skip) {
            charge(6);                                  /* move x2, lsl x2, btst, beq.w */
            set_d16(4, (uint16_t)(car << 4));
            uint16_t off = (uint16_t)(car << 6);
            set_d16(5, off);
            if (!(vrd8(W(0x16C4) + off) & 0x80)) skip = 1;
        }
        if (!skip) {
            charge(4);                                  /* move.w, addi.w, cmpi.w, bcc.w */
            uint16_t off = (uint16_t)(car << 6);
            uint16_t d1 = (uint16_t)(vrd16(W(0x16CE) + off) + 0x2000);
            set_d16(1, d1);
            if (d1 >= 0x4000) skip = 1;
            else {
                charge(8);                              /* clr.l x2, move.w x4, sub.w, bpl */
                uint16_t x = (uint16_t)vrd16(W(0x16B0) + off), z = (uint16_t)vrd16(W(0x16B4) + off);
                set_d16(2, x); set_d16(3, z);
                uint32_t d0 = (uint16_t)(vrd16(W(0x208C)) - x);
                if (d0 & 0x8000) { charge(1); d0 = (uint16_t)(0 - d0); }
                charge(2);                              /* sub.w, bpl */
                uint32_t e1 = (uint16_t)(vrd16(W(0x2090)) - z);
                if (e1 & 0x8000) { charge(1); e1 = (uint16_t)(0 - e1); }
                charge(3);                              /* add.l, cmpi.l, bcc */
                d0 += e1;
                set_d(0, d0); set_d(1, e1);
                if (d0 < 0x384) { charge(1); d6 |= 1u << car; set_d(6, d6); }
            }
        }
        charge(1);                                      /* dbf */
        set_d16(7, (uint16_t)(car - 1));
        if (car == 0) break;
        poll();
    }
    charge(2);                                          /* move.w, beq */
    vwr16(W(0x412E), (uint16_t)d6);
    if ((uint16_t)d6 != 0) {
        charge(2);                                      /* cmpi.w, bcs */
        if ((uint16_t)vrd16(CAR_SPEED) >= 0xE00) {
            charge(5);                                  /* move.w, move.w, addq.w, cmpi.w, bcc.w */
            vwr16(W(0x4130), 0);
            uint16_t d0 = (uint16_t)(vrd16(W(0x412A)) + 1);
            set_d16(0, d0);
            if (d0 >= 0xA0) { charge(1); return RD_RTS; }
            charge(2);
            vwr16(W(0x412A), d0);
            return RD_RTS;
        }
    }
    charge(3);                                          /* move.w, move.w, beq */
    vwr16(W(0x4130), 0xFFFF);
    uint16_t d0 = (uint16_t)vrd16(W(0x412A));
    set_d16(0, d0);
    if (d0 == 0) { charge(1); return RD_RTS; }
    charge(4);                                          /* subq.w, move.w, tst.w, bne */
    d0 = (uint16_t)(d0 - 1);
    set_d16(0, d0);
    vwr16(W(0x412A), d0);
    if (vrd16(ATTRACT_FLAG) != 0) {
        charge(2);
        vwr16(W(0x4052), (uint16_t)(vrd16(W(0x4052)) - 10));
    } else {
        charge(2);
        vwr16(W(0x4052), (uint16_t)((int16_t)vrd16(W(0x4052)) >> 1));
    }
    return RD_RTS;
}

/* ======================================================================== */
/* collisions with other cars                                               */
/* ======================================================================== */

#define CAR_REC(off, f)  (W(f) + (uint32_t)(int16_t)(off))   /* 64-byte car records at W(0x16B0..) */

/* (-sin(a), cos(a)) of the heading at 0x20A0, each times k (Q15 with the
 * add.l/swap idiom), in D1w / D2w -- the probe offset ahead of the car */
static void heading_offset(uint16_t k)
{
    set_d16(0, (uint16_t)((0 - vrd16(W(0x20A0))) & 0xFFFE));
    set_d16(1, (uint16_t)vrd16(a_reg(5) + (uint32_t)(int16_t)dw(0)));
    set_d16(0, (uint16_t)(dw(0) + 0x4000));
    set_d16(2, (uint16_t)vrd16(a_reg(5) + (uint32_t)(int16_t)dw(0)));
    set_d16(1, (uint16_t)(0 - dw(1)));
    muls_w(1, k); dbl_swap(1);
    muls_w(2, k); dbl_swap(2);
}

/* FUN_0001d3f8: bump into another car. The probe point 0x78 ahead of the
 * player (0x49DE.., only when VIEW_MODE is set; otherwise the car itself) is
 * tested against each active car (flag bit 7) within radius^2 0x2F44 -- also
 * 0x64 ahead -- and 0x40 in height. On the first hit: halve the other car's
 * word +0x16C2 unless protected, mark it in 0x4200, count 0x419E, and push the
 * player along the bearing to it (0x41AC, 0x41D8/0x41DA), slowing a fast car;
 * then FUN_0001d87e. No hit clears 0x4200. */
static uint32_t rd_1d3f8(void)
{
    uint32_t r;
    charge(2);
    if (vrd16(W(0x46E0)) != 0) { charge(1); return RD_RTS; }
    charge(2);
    if (vrd16(W(0x433C)) != 0) { charge(1); return RD_RTS; }
    charge(4);                                          /* move.w x2, tst.w, beq */
    set_d16(1, 0); set_d16(2, 0);
    if (vrd16(VIEW_MODE) != 0) { charge(13); heading_offset(0x78); }
    charge(10);
    vwr16(W(0x4202), dw(1));
    vwr16(W(0x420A), dw(2));
    set_d16(1, (uint16_t)vrd16(W(0x4202)));
    set_d16(2, (uint16_t)vrd16(W(0x420A)));
    vwr16(W(0x49DE), (uint16_t)(vrd16(W(0x208C)) + dw(1)));
    vwr16(W(0x49E0), (uint16_t)(vrd16(W(0x208E)) - 10));
    vwr16(W(0x49E2), (uint16_t)(vrd16(W(0x2090)) + dw(2)));
    charge(2);                                          /* moveq, move.w */
    set_d(6, 0); set_d16(7, 0xF);
    for (;;) {
        uint16_t off = dw(6);
        int hit = 0;
        charge(3);                                      /* move.w, tst.w, bne */
        set_d16(0, 0);
        if (vrd16(ATTRACT_FLAG) == 0) { charge(1); set_d16(0, (uint16_t)vrd16(W(0x115C))); }
        charge(2);                                      /* cmp.w, beq.w */
        if (dw(0) != off) {
            charge(2);                                  /* btst.b, beq.w */
            if (vrd8(CAR_REC(off, 0x16C4)) & 0x80) {
                charge(13);
                set_d(0, 0); set_d(1, 0);
                set_d16(0, (uint16_t)(vrd16(W(0x208C)) - vrd16(W(0x4202)) - vrd16(CAR_REC(off, 0x16B0))));
                set_d16(1, (uint16_t)(vrd16(W(0x2090)) - vrd16(W(0x420A)) - vrd16(CAR_REC(off, 0x16B4))));
                muls_w(0, dw(0)); muls_w(1, dw(1));
                set_d(0, d_reg(0) + d_reg(1));
                if ((int32_t)d_reg(0) < 0x2F44) hit = 1;
                else {
                    charge(24);
                    set_d(1, 0); set_d(2, 0);
                    heading_offset(0x64);
                    set_d16(1, (uint16_t)(dw(1) + vrd16(W(0x208C)) - vrd16(CAR_REC(off, 0x16B0))));
                    set_d16(2, (uint16_t)(dw(2) + vrd16(W(0x2090)) - vrd16(CAR_REC(off, 0x16B4))));
                    muls_w(1, dw(1)); muls_w(2, dw(2));
                    set_d(1, d_reg(1) + d_reg(2));
                    if (!((int32_t)d_reg(1) > 0x2F44)) hit = 1;
                }
            }
        }
        if (hit) {
            charge(3);                                  /* move.w, sub.w, bpl */
            set_d16(0, (uint16_t)(vrd16(W(0x208E)) - vrd16(CAR_REC(off, 0x16B2))));
            if (dw(0) & 0x8000) { charge(1); set_d16(0, (uint16_t)(0 - dw(0))); }
            charge(2);                                  /* cmpi.w, bgt.w */
            if ((int16_t)dw(0) <= 0x40) break;          /* a real hit */
        }
        charge(2);                                      /* addi.w, dbf */
        set_d16(6, (uint16_t)(off + 0x40));
        uint16_t n = dw(7);
        set_d16(7, (uint16_t)(n - 1));
        if (n == 0) { charge(2); vwr16(W(0x4200), 0); return RD_RTS; }
        poll();
    }
    uint16_t off = dw(6);
    charge(2);                                          /* btst.b, bne.w */
    if (!(vrd8(CAR_REC(off, 0x16C4)) & 1)) {
        charge(2);                                      /* cmp.w, bcs.w */
        if (off >= (uint16_t)vrd16(W(0x4A94))) {
            charge(1);                                  /* lsr (0x16C2,A6,D6w) */
            vwr16(CAR_REC(off, 0x16C2), (uint16_t)((uint16_t)vrd16(CAR_REC(off, 0x16C2)) >> 1));
        }
    }
    charge(13);
    set_d(1, 0);
    set_d16(1, (uint16_t)vrd16(W(0x4200)));
    set_d16(0, (uint16_t)(off >> 6));
    set_d(1, d_reg(1) | (1u << (dw(0) & 31)));
    vwr16(W(0x4200), dw(1));
    vwr16(W(0x419E), (uint16_t)(vrd16(W(0x419E)) + 1));
    set_d16(1, (uint16_t)(vrd16(W(0x208C)) - vrd16(CAR_REC(off, 0x16B0))));
    set_d16(2, (uint16_t)(vrd16(W(0x2090)) - vrd16(CAR_REC(off, 0x16B4))));
    int both_zero = 0;
    if (dw(1) == 0) {
        charge(2);                                      /* tst.w, bne */
        if (dw(2) == 0) { charge(2); set_d(0, 0); both_zero = 1; }
    }
    if (!both_zero) {
        charge(1);
        if ((r = rd_call(L_A98C, 0x01D57E))) return r;
        charge(2);                                      /* neg.w, addi.w */
        set_d16(0, (uint16_t)(0x4000 - dw(0)));
    }
    charge(2);                                          /* move.w, bra.w */
    vwr16(W(0x41AC), dw(0));
    charge(19);                                         /* 0x1D834 */
    set_d16(3, 0x955);
    set_d16(0, (uint16_t)(dw(0) & 0xFFFE));
    set_d16(1, (uint16_t)vrd16(a_reg(5) + (uint32_t)(int16_t)dw(0)));
    set_d16(0, (uint16_t)(dw(0) + 0x4000));
    set_d16(2, (uint16_t)vrd16(a_reg(5) + (uint32_t)(int16_t)dw(0)));
    set_d16(1, (uint16_t)(0 - dw(1)));
    set_d16(0, (uint16_t)(((uint16_t)vrd16(CAR_SPEED) >> 1) + 0x955));
    muls_w(1, dw(0)); dbl_swap(1);
    muls_w(2, dw(0)); dbl_swap(2);
    vwr16(W(0x41D8), dw(1));
    vwr16(W(0x41DA), dw(2));
    if ((uint16_t)vrd16(CAR_SPEED) >= 0x1000) {
        charge(3);
        set_d16(0, (uint16_t)((uint16_t)vrd16(CAR_SPEED) >> 6));
        vwr16(CAR_SPEED, (uint16_t)(vrd16(CAR_SPEED) - dw(0)));
    }
    charge(1);
    if ((r = rd_call(L_1D87E, 0x01D87C))) return r;
    charge(1);
    return RD_RTS;
}

/* ======================================================================== */
/* per-car update loop                                                      */
/* ======================================================================== */

/* the six per-car routines FUN_0001f1da runs for the car at offset D6w */
static uint32_t per_car_routines(uint32_t first_ret)
{
    static const void (*fn[6])(void) = { L_1F2C0, L_1F4C0, L_1F7BE, L_1F574, L_1F61E, L_1F674 };
    uint32_t r;
    for (int i = 0; i < 6; i++) {
        charge(1);                                      /* bsr */
        if ((r = rd_call(fn[i], first_ret + 4u * (uint32_t)i))) return r;
    }
    return 0;
}

/* FUN_0001f1da: update every car. Refresh the per-car section values
 * (FUN_0001f72c); in a race the player car (0x115C) is tilted by 0x4086/4
 * for view 1 and skipped for views 0 and 7, and all 16 cars run the six
 * per-car routines. Outside a race the player car (offset 0) runs them first
 * -- with its own tilt smoothing through 0x46C4 -- then the 0x4A96+1 extra
 * cars from offset 0x4A94. */
static uint32_t rd_1f1da(void)
{
    uint32_t r;
    charge(1);
    if ((r = rd_call(L_1F72C, 0x01F1DE))) return r;
    charge(2);                                          /* tst.w, beq.w */
    if (vrd16(ATTRACT_FLAG) != 0) {
        int run_player = 0, tilt = 0;
        charge(2);                                      /* tst.w, bne.b */
        if (vrd16(W(0x46E0)) != 0) run_player = 1;
        else {
            charge(2);                                  /* move.w, beq.w */
            uint16_t v = (uint16_t)vrd16(W(0x20B0));
            set_d16(0, v);
            if (v != 0) {
                charge(2);                              /* cmpi.w, beq.w */
                if (v != 7) {
                    charge(2);                          /* cmpi.w, bne.b */
                    run_player = 1;
                    if (v == 1) {
                        charge(2);                      /* tst.w, beq.b */
                        if (vrd16(CAR_SPEED) != 0) tilt = 1;
                    }
                }
            }
        }
        if (tilt) {
            charge(8);
            uint16_t d0 = (uint16_t)(((vrd16(W(0x0DD2)) & 0xFFFE) << 1) - vrd16(W(0x46C4)));
            d0 = (uint16_t)((int16_t)d0 >> 4);
            vwr16(W(0x46C4), (uint16_t)(vrd16(W(0x46C4)) + d0));
            d0 = (uint16_t)vrd16(W(0x46C4));
            set_d16(0, d0);
            vwr16(W(0x16B8), (uint16_t)(vrd16(W(0x16B8)) - d0));
        }
        if (run_player) {
            charge(1);                                  /* move.w #0,D6w */
            set_d16(6, 0);
            if ((r = per_car_routines(0x01F22C))) return r;
        }
        charge(2);                                      /* move.w, beq.w */
        uint16_t off = (uint16_t)vrd16(W(0x4A94));
        set_d16(6, off);
        if (off == 0) return 0x01FE0A;
        charge(1);
        set_d16(7, (uint16_t)vrd16(W(0x4A96)));
        for (;;) {
            if ((r = per_car_routines(0x01F250))) return r;
            charge(2);                                  /* addi.w, dbf */
            set_d16(6, (uint16_t)(dw(6) + 0x40));
            uint16_t n = dw(7);
            set_d16(7, (uint16_t)(n - 1));
            if (n == 0) break;
            poll();
        }
        charge(1);
        return RD_RTS;
    }
    charge(2);                                          /* move.w x2 */
    set_d16(6, 0); set_d16(7, 0xF);
    for (;;) {
        int run = 1;
        charge(2);                                      /* cmp.w, bne.b */
        if (dw(6) == (uint16_t)vrd16(W(0x115C))) {
            charge(2);                                  /* move.w, beq.w */
            uint16_t v = (uint16_t)vrd16(W(0x20B0));
            set_d16(0, v);
            if (v == 0) run = 0;
            else {
                charge(2);                              /* cmpi.w, beq.w */
                if (v == 7) run = 0;
                else {
                    charge(2);                          /* cmpi.w, bne.b */
                    if (v == 1) {
                        charge(3);
                        set_d16(0, (uint16_t)((int16_t)vrd16(W(0x4086)) >> 2));
                        uint32_t a = W(0x16B8) + (uint32_t)(int16_t)dw(6);
                        vwr16(a, (uint16_t)(vrd16(a) + dw(0)));
                    }
                }
            }
        }
        if (run && (r = per_car_routines(0x01F2A2))) return r;
        charge(2);                                      /* addi.w, dbf */
        set_d16(6, (uint16_t)(dw(6) + 0x40));
        uint16_t n = dw(7);
        set_d16(7, (uint16_t)(n - 1));
        if (n == 0) break;
        poll();
    }
    charge(1);
    return RD_RTS;
}

/* FUN_0001f4c0: fill the render block at 0x45B4.. for the car at D6w: its
 * model (0x468C[car] + the section table 0x1EEB4[0x464C[car]].+4), position
 * (0x4562, (Y - the view's 0x2108 - 0x20BE) * 25, 0x456A), the sin/cos of
 * its yaw 0x16D8 and roll 0x16BA. Skipped (0x1FE0A) when 0x4AA6 == 2 or
 * 0x4560 < 0. */
static uint32_t rd_1f4c0(void)
{
    charge(2);
    if (vrd16(W(0x4AA6)) == 2) return 0x01FE0A;
    charge(2);
    if (vrd16(W(0x4560)) & 0x8000) return 0x01FE0A;
    charge(39);
    uint16_t off = dw(6);
    uint16_t car = (uint16_t)(off >> 6);
    uint16_t d5 = (uint16_t)vrd16(W(0x468C) + 2u * car);
    uint16_t sec = (uint16_t)vrd16(W(0x464C) + 2u * car);
    set_a(0, 0x01EEB4);
    d5 = (uint16_t)(d5 + vrd16(0x01EEB4u + 4 + (uint32_t)((int16_t)sec * 8)));
    set_d16(5, d5);
    vwr16(W(0x45B4), d5);
    vwr32(W(0x45B6), vrd32(W(0x4562)));
    uint16_t vi = (uint16_t)(vrd16(W(0x0C40)) & 7);
    uint32_t d0 = (uint16_t)(vrd16(W(0x16B2) + (uint32_t)(int16_t)off) - vrd16(W(0x2108) + 2u * vi) - vrd16(W(0x20BE)));
    d0 = (uint32_t)((int32_t)(int16_t)d0 * 0x19);
    vwr32(W(0x45BA), d0);
    vwr32(W(0x45BE), vrd32(W(0x456A)));
    uint16_t a = (uint16_t)((0 - vrd16(W(0x16D8) + (uint32_t)(int16_t)off)) & 0xFFFE);
    vwr16(W(0x45C2), (uint16_t)(0 - vrd16(a_reg(5) + (uint32_t)(int16_t)a)));
    vwr16(W(0x45C4), (uint16_t)vrd16(a_reg(5) + (uint32_t)(int16_t)(uint16_t)(a + 0x4000)));
    vwr16(W(0x45C6), vrd16(W(0x4572)));
    vwr16(W(0x45C8), vrd16(W(0x4574)));
    a = (uint16_t)((0 - vrd16(W(0x16BA) + (uint32_t)(int16_t)off)) & 0xFFFE);
    uint16_t s1 = (uint16_t)(0 - vrd16(a_reg(5) + (uint32_t)(int16_t)a));
    uint16_t c1 = (uint16_t)vrd16(a_reg(5) + (uint32_t)(int16_t)(uint16_t)(a + 0x4000));
    vwr16(W(0x45CA), s1);
    vwr16(W(0x45CC), c1);
    vwr16(W(0x45CE), 1);
    set_d(0, (d0 & 0xFFFF0000u) | (uint16_t)(a + 0x4000));
    set_d16(1, s1); set_d16(2, c1);
    return RD_RTS;
}

/* FUN_0001f6d4: the per-car section index table 0x464C[16]: in attract, car
 * 0 from the EEPROM setting 0x10001061 and cars 1..15 = 0..14; in a race each
 * car's own low nibble at +0x16BE. Continues into FUN_0001f72c. */
static uint32_t rd_1f6d4(void)
{
    charge(2);                                          /* tst.w, beq.w */
    if (vrd16(ATTRACT_FLAG) != 0) {
        charge(6);
        uint16_t d6 = (uint16_t)(vrd8(0x10001061u) & 0xF);
        vwr16(W(0x464C), d6);
        set_d16(5, 0); set_d16(6, 1); set_d16(7, 0xE);
        for (;;) {
            charge(4);                                  /* move.w, addq.l x2, dbf */
            vwr16(W(0x464C) + 2u * dw(6), dw(5));
            set_d(5, d_reg(5) + 1);
            set_d(6, d_reg(6) + 1);
            uint16_t n = dw(7);
            set_d16(7, (uint16_t)(n - 1));
            if (n == 0) break;
            poll();
        }
        charge(1);                                      /* bra.w */
        return 0x01F72C;
    }
    charge(3);                                          /* lea, move.w x2 */
    set_a(2, 0x01EE2E);
    set_d16(6, 0x3C0); set_d16(7, 0xF);
    for (;;) {
        charge(5);                                      /* move.w, andi.w, move.w, subi.w, dbf */
        uint16_t v = (uint16_t)(vrd16(W(0x16BE) + (uint32_t)(int16_t)dw(6)) & 0xF);
        set_d16(0, v);
        vwr16(W(0x464C) + 2u * dw(7), v);
        set_d16(6, (uint16_t)(dw(6) - 0x40));
        uint16_t n = dw(7);
        set_d16(7, (uint16_t)(n - 1));
        if (n == 0) break;
        poll();
    }
    return 0x01F72C;                                    /* falls into FUN_0001f72c */
}

/* walk one (start, length) word-pair list at A0 (ended by a negative start);
 * if the car's track position is inside any entry, D1w/D2w = (hit, 1) */
static void section_scan(uint16_t off, uint16_t hit)
{
    uint32_t a0 = a_reg(0);
    for (;;) {
        charge(2);                                      /* tst.w, bmi */
        if (tst_w(a0) & 0x8000) return;
        charge(4);                                      /* move.w, sub.w, cmp.w, bcc */
        uint16_t d0 = (uint16_t)(vrd16(W(0x16AE) + (uint32_t)(int16_t)off) - vrd16(a0));
        set_d16(0, d0);
        if (d0 < (uint16_t)vrd16(a0 + 2)) { charge(2); set_d16(1, hit); set_d16(2, 1); }
        charge(2);                                      /* adda.w, bra.b */
        a0 += 4; set_a(0, a0);
        poll();
    }
}

/* FUN_0001f72c: for each active car, SKID_LEVEL-style values 0x466C[car]
 * (3 or 6, 0 = none) and 0x468C[car] (1 = inside) from the two per-course
 * lists of track sections at 0x1EF34 (course * 8, +0 / +4) */
static uint32_t rd_1f72c(void)
{
    charge(4);
    set_a(4, W(0x466C)); set_a(3, W(0x468C));
    set_d16(6, 0); set_d16(7, 0xF);
    for (;;) {
        uint16_t off = dw(6);
        charge(2);                                      /* btst.b, beq.w */
        if (vrd8(W(0x16C4) + (uint32_t)(int16_t)off) & 0x80) {
            charge(5);
            set_d16(1, 0); set_d16(2, 0);
            set_d16(0, (uint16_t)vrd16(W(0x2046)));
            set_a(0, vrd32(0x01EF34u + (uint32_t)((int16_t)dw(0) * 8)));
            section_scan(off, 3);
            charge(3);
            set_d16(0, (uint16_t)vrd16(W(0x2046)));
            set_a(0, vrd32(0x01EF34u + 4 + (uint32_t)((int16_t)dw(0) * 8)));
            section_scan(off, 6);
            charge(4);
            uint16_t car = (uint16_t)(off >> 6);
            set_d16(0, car);
            vwr16(W(0x466C) + 2u * car, dw(1));
            vwr16(W(0x468C) + 2u * car, dw(2));
        }
        charge(2);                                      /* addi.w, dbf */
        set_d16(6, (uint16_t)(off + 0x40));
        uint16_t n = dw(7);
        set_d16(7, (uint16_t)(n - 1));
        if (n == 0) break;
        poll();
    }
    charge(1);
    return RD_RTS;
}

/* FUN_0001ff4e: a thunk into FUN_00020238 */
static uint32_t rd_1ff4e(void)
{
    charge(1);
    return 0x020238;
}

/* ======================================================================== */
/* trackside objects                                                        */
/* ======================================================================== */

/* D1..D3 = the three words at a1 scaled by D0w (muls + add.l/swap each) */
static void scale3(uint32_t a1)
{
    set_d16(1, (uint16_t)vrd16(a1 + 0));
    set_d16(2, (uint16_t)vrd16(a1 + 2));
    set_d16(3, (uint16_t)vrd16(a1 + 4));
    muls_w(1, dw(0)); dbl_swap(1);
    muls_w(2, dw(0)); dbl_swap(2);
    muls_w(3, dw(0)); dbl_swap(3);
}

/* FUN_00020238: the eight swinging trackside objects of the course (0x2048,
 * table at 0x20342: A0 = base positions, A1 = swing vectors, 8 bytes each).
 * Each swings along its vector by (cos(phase) + 0x8000)/2, the phase 0x4434[i]
 * advancing 0x50 a frame; positions go to 0x4448/0x445C/0x4470. Then each is
 * drawn as model 0x822 facing the player (FUN_0000a98c bearing) and 0x823
 * spinning with the frame counter (FUN_00021702). */
static uint32_t rd_20238(void)
{
    uint32_t r;
    charge(6);
    set_a(2, 0x020342);
    set_d16(0, (uint16_t)(vrd16(W(0x2048)) & 3));
    uint32_t a0 = vrd32(0x020342u + (uint32_t)((int16_t)dw(0) * 8));
    uint32_t a1 = vrd32(0x020342u + 4 + (uint32_t)((int16_t)dw(0) * 8));
    set_a(0, a0); set_a(1, a1);
    set_d16(7, 7);
    for (;;) {
        charge(26);
        uint16_t i = dw(7);
        uint32_t ph = W(0x4434) + (uint32_t)((int16_t)i * 2);
        uint16_t a = (uint16_t)((vrd16(ph) & 0xFFFE) + 0x4000);
        uint16_t c = (uint16_t)vrd16(a_reg(5) + (uint32_t)(int16_t)a);
        set_d16(0, (uint16_t)((uint16_t)(c + 0x8000) >> 1));
        uint32_t e = (uint32_t)((int16_t)i * 8);
        scale3(a1 + e);
        set_d16(1, (uint16_t)(dw(1) + vrd16(a0 + e + 0)));
        set_d16(2, (uint16_t)(dw(2) + vrd16(a0 + e + 2)));
        set_d16(3, (uint16_t)(dw(3) + vrd16(a0 + e + 4)));
        vwr16(W(0x4448) + (uint32_t)((int16_t)i * 2), dw(1));
        vwr16(W(0x445C) + (uint32_t)((int16_t)i * 2), dw(2));
        vwr16(W(0x4470) + (uint32_t)((int16_t)i * 2), dw(3));
        vwr16(ph, (uint16_t)(vrd16(ph) + 0x50));
        set_d16(7, (uint16_t)(i - 1));
        if (i == 0) break;
        poll();
    }
    charge(1);
    vwr16(LAP_ROWS_LEFT, 7);
    for (;;) {
        charge(13);
        set_d16(7, (uint16_t)vrd16(LAP_ROWS_LEFT));
        uint32_t k = (uint32_t)((int16_t)dw(7) * 2);
        vwr16(W(0x44C8), 0x822);
        vwr16(W(0x44CA), vrd16(W(0x4448) + k));
        vwr16(W(0x44CC), vrd16(W(0x445C) + k));
        vwr16(W(0x44CE), vrd16(W(0x4470) + k));
        vwr16(W(0x44D0), 0x4000);
        vwr16(W(0x44D4), 0);
        set_d16(1, (uint16_t)(vrd16(W(0x44CA)) - vrd16(W(0x208C))));
        set_d16(2, (uint16_t)(vrd16(W(0x44CE)) - vrd16(W(0x2090))));
        int zero = 0;
        if (dw(1) == 0) {
            charge(2);                                  /* tst.w, bne.b */
            if (dw(2) == 0) { charge(2); set_d(0, 0); zero = 1; }
        }
        if (!zero) {
            charge(1);
            if ((r = rd_call(L_A98C, 0x020306))) return r;
            charge(2);
            set_d16(0, (uint16_t)(0x4000 - dw(0)));
        }
        charge(4);                                      /* neg.w, move.w x2, bsr */
        set_d16(0, (uint16_t)(0 - dw(0)));
        vwr16(W(0x44D2), dw(0));
        vwr16(W(0x44D6), 0);
        if ((r = rd_call(L_21702, 0x02031C))) return r;
        charge(7);
        vwr16(W(0x44C8), 0x823);
        set_d16(0, (uint16_t)(vrd16(FRAME_PARITY) << 12));
        vwr16(W(0x44D4), dw(0));
        vwr16(W(0x44D6), 4);
        if ((r = rd_call(L_21702, 0x020338))) return r;
        charge(2);                                      /* subq.w, bpl.w */
        uint16_t n = (uint16_t)(vrd16(LAP_ROWS_LEFT) - 1);
        vwr16(LAP_ROWS_LEFT, n);
        if (n & 0x8000) break;
        poll();
    }
    charge(1);
    return RD_RTS;
}

/* FUN_000204e2: the course's (0x2048, table at 0x205A6) list of animated
 * objects: count-1 at (A0)+, then per object a base position + angle at A0
 * and a motion vector at A1 (8 bytes each), scaled by the phase 0x4420[i]
 * (advancing 0x20 a frame); each is drawn as model 0x824 (FUN_00021702). */
static uint32_t rd_204e2(void)
{
    uint32_t r;
    charge(5);
    set_a(2, 0x0205A6);
    set_d16(0, (uint16_t)(vrd16(W(0x2048)) & 3));
    uint32_t a0 = vrd32(0x0205A6u + (uint32_t)((int16_t)dw(0) * 8));
    uint32_t a1 = vrd32(0x0205A6u + 4 + (uint32_t)((int16_t)dw(0) * 8));
    set_a(1, a1);
    charge(2);                                          /* move.w (A0)+, bmi.w */
    set_d16(7, (uint16_t)vrd16(a0));
    a0 += 2; set_a(0, a0);
    if (dw(7) & 0x8000) { charge(1); return RD_RTS; }
    charge(1);
    vwr16(LAP_ROWS_LEFT, dw(7));
    for (;;) {
        charge(24);
        uint16_t i = dw(7);
        uint32_t k = (uint32_t)((int16_t)i * 2), e = (uint32_t)((int16_t)i * 8);
        set_d16(0, (uint16_t)(vrd16(W(0x4420) + k) & 0x7FFF));
        scale3(a1 + e);
        set_d16(5, dw(1));
        set_d16(1, (uint16_t)(dw(1) + vrd16(a0 + e + 0)));
        set_d16(2, (uint16_t)(dw(2) + vrd16(a0 + e + 2)));
        set_d16(3, (uint16_t)(dw(3) + vrd16(a0 + e + 4)));
        vwr16(W(0x4424) + k, dw(1));
        vwr16(W(0x4428) + k, dw(2));
        vwr16(W(0x442C) + k, dw(3));
        vwr16(W(0x4430) + k, vrd16(a0 + e + 6));
        vwr16(W(0x4420) + k, (uint16_t)(vrd16(W(0x4420) + k) + 0x20));
        set_d16(7, (uint16_t)(i - 1));
        if (i == 0) break;
        poll();
    }
    for (;;) {
        charge(10);
        set_d16(7, (uint16_t)vrd16(LAP_ROWS_LEFT));
        uint32_t k = (uint32_t)((int16_t)dw(7) * 2);
        vwr16(W(0x44C8), 0x824);
        vwr16(W(0x44CA), vrd16(W(0x4424) + k));
        vwr16(W(0x44CC), vrd16(W(0x4428) + k));
        vwr16(W(0x44CE), vrd16(W(0x442C) + k));
        vwr16(W(0x44D2), vrd16(W(0x4430) + k));
        vwr16(W(0x44D0), 0xF800);
        vwr16(W(0x44D4), 0);
        vwr16(W(0x44D6), 0);
        if ((r = rd_call(L_21702, 0x02059C))) return r;
        charge(2);                                      /* subq.w, bpl.b */
        uint16_t n = (uint16_t)(vrd16(LAP_ROWS_LEFT) - 1);
        vwr16(LAP_ROWS_LEFT, n);
        if (n & 0x8000) break;
        poll();
    }
    charge(1);
    return RD_RTS;
}

/* camera-relative offset of a world word: w - cam(0x20F8/0x2108/0x2118 + i*0x10)
 * - the view offset (0x20BC/0x20BE/0x20C0) */
#define REL_X(w) ((uint16_t)((w) - vrd16(W(0x20F8)) - vrd16(W(0x20BC))))
#define REL_Y(w) ((uint16_t)((w) - vrd16(W(0x2108)) - vrd16(W(0x20BE))))
#define REL_Z(w) ((uint16_t)((w) - vrd16(W(0x2118)) - vrd16(W(0x20C0))))

/* the start of an object entry: D6w/D2w = x, D7w/D3w = z (D2/D3 = |x|, |z|),
 * D3 = |x| + |z|; returns it. D2..D7 cleared first as the 68K does. */
static uint32_t near_test(uint16_t x, uint16_t z)
{
    charge(11);                                         /* moveq x5, move.w, sub.w x2, move.w x2, bpl */
    set_d(2, 0); set_d(3, 0); set_d(5, 0); set_d(6, 0); set_d(7, 0);
    set_d16(0, x); set_d16(6, x); set_d16(2, x);
    if (x & 0x8000) { charge(1); set_d16(2, (uint16_t)(0 - x)); }
    charge(6);                                          /* move.w, sub.w x2, move.w x2, bpl */
    set_d16(0, z); set_d16(7, z); set_d16(3, z);
    if (z & 0x8000) { charge(1); set_d16(3, (uint16_t)(0 - z)); }
    charge(3);                                          /* add.l, cmpi.l, bcc.w */
    set_d(3, d_reg(3) + d_reg(2));
    return d_reg(3);
}

/* append (-sin(a), cos(a)) as two longs at A3 (D1 keeps its high word) */
static uint32_t put_sincos(uint32_t a3, uint16_t a)
{
    set_d16(0, (uint16_t)(a & 0xFFFE));
    set_d16(1, (uint16_t)vrd16(a_reg(5) + (uint32_t)(int16_t)dw(0)));
    set_d16(0, (uint16_t)(dw(0) + 0x4000));
    set_d16(2, (uint16_t)vrd16(a_reg(5) + (uint32_t)(int16_t)dw(0)));
    set_d16(1, (uint16_t)(0 - dw(1)));
    vwr32(a3, d_reg(1)); vwr32(a3 + 4, d_reg(2));
    return a3 + 8;
}

/* FUN_0001fe38: course 0 only -- two drifting objects (x 0x4A7C.. wrapping
 * 0x52D0 -> 0x2ED0, z 0x4A7E.. wrapping 0x28A0 -> -0x2590) drawn as model
 * 0x827 with three rotations (0x4A88/0x4A8C/0x4A90) when within 10000 of the
 * camera, in a display-list block ended by -1 */
static uint32_t rd_1fe38(void)
{
    charge(2);
    if (vrd16(W(0x2046)) != 0) { charge(1); return RD_RTS; }
    charge(3);
    set_d16(0, (uint16_t)vrd16(W(0x4A7C)));
    if (dw(0) >= 0x52D0) { charge(1); vwr16(W(0x4A7C), 0x2ED0); }
    charge(4);
    vwr16(W(0x4A7C), (uint16_t)(vrd16(W(0x4A7C)) + 0x10));
    set_d16(0, (uint16_t)vrd16(W(0x4A7E)));
    if ((int16_t)dw(0) >= 0x28A0) { charge(1); vwr16(W(0x4A7E), 0xDA70); }
    charge(3);
    vwr16(W(0x4A7E), (uint16_t)(vrd16(W(0x4A7E)) + 0x10));
    set_d(4, 0);
    uint32_t a3 = vrd32(DLIST_PTR);
    set_a(3, a3);
    for (;;) {
        uint16_t i = dw(4);
        uint32_t d3 = near_test(REL_X(vrd16(W(0x4A7C) + i)), REL_Z(vrd16(W(0x4A84) + i)));
        if (d3 < 0x2710) {
            charge(37);
            set_d16(5, REL_Y(vrd16(W(0x4A80) + i)));
            set_d16(0, dw(5));
            set_d(0, 0x827);
            vwr32(a3, 0x827);
            set_d(6, (uint32_t)((int32_t)(int16_t)dw(6) * 0x19)); vwr32(a3 + 4, d_reg(6));
            set_d(5, (uint32_t)((int32_t)(int16_t)dw(5) * 0x19)); vwr32(a3 + 8, d_reg(5));
            set_d(7, (uint32_t)((int32_t)(int16_t)dw(7) * 0x19)); vwr32(a3 + 12, d_reg(7));
            a3 += 16;
            a3 = put_sincos(a3, (uint16_t)vrd16(W(0x4A88) + i));
            a3 = put_sincos(a3, (uint16_t)vrd16(W(0x4A8C) + i));
            a3 = put_sincos(a3, (uint16_t)vrd16(W(0x4A90) + i));
            vwr32(a3, 4); a3 += 4;
            set_a(3, a3);
        }
        charge(3);                                      /* addq.w, cmpi.w, bne.w */
        set_d16(4, (uint16_t)(i + 2));
        if (dw(4) == 4) break;
        poll();
    }
    charge(3);
    vwr32(a3, 0xFFFFFFFFu);
    vwr32(DLIST_PTR, a3);
    return RD_RTS;
}

/* FUN_0002077c: course 0 only -- fifteen animated trackside objects. Each
 * steps its frame counter 0x49EA[i] up to the per-object length in the table
 * at 0x2068C (16 bytes per object, indexed by its animation 0x4A0E[i], which
 * cycles 0..5); objects within 20000 of the camera are drawn (positions and
 * yaw from 0x20614, 8 bytes each; model = 0x205F6[i] + animation) in a
 * display-list block ended by -1. */
static uint32_t rd_2077c(void)
{
    charge(2);
    if (vrd16(W(0x2046)) != 0) { charge(1); return RD_RTS; }
    charge(2);
    set_a(0, 0x02068C);
    set_d16(7, 0xE);
    for (;;) {
        charge(10);
        uint16_t i = dw(7);
        uint32_t k = (uint32_t)((int16_t)i * 2);
        set_d16(6, (uint16_t)(i << 4));
        uint16_t off = (uint16_t)(vrd16(W(0x4A0E) + k) * 2 + dw(6));
        set_d16(0, (uint16_t)vrd16(0x02068Cu + (uint32_t)(int16_t)off));
        set_d16(2, (uint16_t)(vrd16(W(0x49EA) + k) + 1));
        if (dw(2) == dw(0)) {
            charge(5);                                  /* clr.w, move.w, addq.w, cmpi.w, bne */
            set_d16(2, 0);
            set_d16(3, (uint16_t)(vrd16(W(0x4A0E) + k) + 1));
            if (dw(3) == 6) { charge(1); set_d16(3, 0); }
            charge(1);
            vwr16(W(0x4A0E) + k, dw(3));
        }
        charge(2);                                      /* move.w, dbf */
        vwr16(W(0x49EA) + k, dw(2));
        set_d16(7, (uint16_t)(i - 1));
        if (i == 0) break;
        poll();
    }
    charge(4);
    set_a(0, 0x020614); set_a(1, 0x0205F6);
    set_d(4, 0);
    uint32_t a3 = vrd32(DLIST_PTR);
    set_a(3, a3);
    for (;;) {
        uint16_t i = dw(4);
        uint32_t e = 0x020614u + (uint32_t)((int16_t)i * 8);
        uint32_t d3 = near_test(REL_X(vrd16(e + 0)), REL_Z(vrd16(e + 4)));
        if (d3 < 0x4E20) {
            charge(29);
            set_d16(5, REL_Y(vrd16(e + 2)));
            set_d16(0, (uint16_t)(vrd16(0x0205F6u + (uint32_t)((int16_t)i * 2)) + vrd16(W(0x4A0E) + (uint32_t)((int16_t)i * 2))));
            set_d(0, dw(0));
            vwr32(a3, d_reg(0));
            set_d16(0, 0x19);
            set_d(6, (uint32_t)((int32_t)(int16_t)dw(6) * 0x19)); vwr32(a3 + 4, d_reg(6));
            set_d(5, (uint32_t)((int32_t)(int16_t)dw(5) * 0x19)); vwr32(a3 + 8, d_reg(5));
            set_d(7, (uint32_t)((int32_t)(int16_t)dw(7) * 0x19)); vwr32(a3 + 12, d_reg(7));
            vwr32(a3 + 16, 0); vwr32(a3 + 20, 0x7FFF);
            a3 = put_sincos(a3 + 24, (uint16_t)(vrd16(e + 6) + 0x8000));
            vwr32(a3, 0); vwr32(a3 + 4, 0x7FFF); vwr32(a3 + 8, 4);
            a3 += 12;
            set_a(3, a3);
        }
        charge(3);                                      /* addq.w, cmpi.w, bne.w */
        set_d16(4, (uint16_t)(i + 1));
        if (dw(4) == 0xF) break;
        poll();
    }
    charge(3);
    vwr32(a3, 0xFFFFFFFFu);
    vwr32(DLIST_PTR, a3);
    return RD_RTS;
}

/* ======================================================================== */
/* car render blocks                                                        */
/* ======================================================================== */

/* the four render-block ids FUN_0001f2c0 clears (and others fill) */
static const uint16_t car_block_ids[8] = { 0x4560, 0x457C, 0x4598, 0x45B4, 0x45D0, 0x45EC, 0x4608, 0x4624 };

/* D1w = -sin(a), D2w = cos(a) from the table at A5 (D0w ends at a' + 0x4000) */
static void sincos_to_d1d2(uint16_t a)
{
    set_d16(0, (uint16_t)(a & 0xFFFE));
    set_d16(1, (uint16_t)vrd16(a_reg(5) + (uint32_t)(int16_t)dw(0)));
    set_d16(0, (uint16_t)(dw(0) + 0x4000));
    set_d16(2, (uint16_t)vrd16(a_reg(5) + (uint32_t)(int16_t)dw(0)));
    set_d16(1, (uint16_t)(0 - dw(1)));
}

/* FUN_0001f2c0: the main render block (0x4560..0x457A) for the car at D6w.
 * All eight block ids are first set to -1 (none). An active car within
 * Manhattan distance 9000 of the camera gets its camera-relative position *25,
 * its bearing (FUN_0000a98c) stored at +0x16CE, and -- unless it is seen from
 * behind the camera -- a level of detail 0x4AA6 (0/1/2 by distance; 1/2 in
 * the rear view), a model from the section table 0x1EE2E and 0x466C, bits in
 * the drawn-car mask 0x4640 and counter 0x4644[lod], flag bit 0 at +0x16C4,
 * and the sin/cos of its three angles. Inactive cars leave via 0x1FE0A. */
static uint32_t rd_1f2c0(void)
{
    uint32_t r;
    charge(9);
    set_d16(5, 0xFFFF);
    for (int i = 0; i < 8; i++) vwr16(W(car_block_ids[i]), 0xFFFF);
    uint16_t off = dw(6);
    charge(2);                                          /* btst.b, beq.w */
    if (!(vrd8(CAR_REC(off, 0x16C4)) & 0x80)) return 0x01FE0A;
    charge(29);
    set_d(3, 0); set_d(4, 0);
    uint16_t vi = (uint16_t)(vrd16(W(0x0C40)) & 7);
    set_d16(1, vi);
    set_d16(0, (uint16_t)(vrd16(CAR_REC(off, 0x16B0)) + vrd16(CAR_REC(off, 0x16DE)) - vrd16(W(0x20F8) + 2u * vi) - vrd16(W(0x20BC))));
    set_d16(3, dw(0));
    muls_w(0, 0x19);
    vwr32(W(0x4562), d_reg(0));
    set_d16(0, (uint16_t)(vrd16(CAR_REC(off, 0x16B4)) + vrd16(CAR_REC(off, 0x16E0)) - vrd16(W(0x2118) + 2u * vi) - vrd16(W(0x20C0))));
    set_d16(4, dw(0));
    muls_w(0, 0x19);
    vwr32(W(0x456A), d_reg(0));
    set_d(0, 0);
    set_d16(0, (uint16_t)(vrd16(CAR_REC(off, 0x16B2)) - vrd16(W(0x2108) + 2u * vi) - vrd16(W(0x20BE))));
    muls_w(0, 0x19);
    vwr32(W(0x4566), d_reg(0));
    set_d16(1, dw(3));
    set_d16(2, dw(4));
    set_d16(5, 0xFFFF);
    if (dw(3) & 0x8000) { charge(1); set_d16(3, (uint16_t)(0 - dw(3))); }
    charge(2);                                          /* tst.w, bpl */
    if (dw(4) & 0x8000) { charge(1); set_d16(4, (uint16_t)(0 - dw(4))); }
    charge(3);                                          /* add.l, cmpi.l, bgt.w */
    set_d(4, d_reg(4) + d_reg(3));
    if ((int32_t)d_reg(4) > 0x2328) goto none;
    {
        charge(1);                                      /* movem.l {D3,D4},-(SP) */
        uint32_t sp = a_reg(7) - 8;
        vwr32(sp, d_reg(3)); vwr32(sp + 4, d_reg(4));
        set_a(7, sp);
        int zero = 0;
        charge(2);                                      /* tst.w, bne */
        if (dw(1) == 0) {
            charge(2);                                  /* tst.w, bne */
            if (dw(2) == 0) { charge(2); set_d(0, 0); zero = 1; }
        }
        if (!zero) {
            charge(1);
            if ((r = rd_call(L_A98C, 0x01F38C))) return r;
            charge(2);
            set_d16(0, (uint16_t)(0x4000 - dw(0)));
        }
        charge(5);                                      /* movem.l, sub.w, move.w, cmpi.w, bne */
        sp = a_reg(7);
        set_d(3, vrd32(sp)); set_d(4, vrd32(sp + 4));
        set_a(7, sp + 8);
    }
    set_d16(0, (uint16_t)(dw(0) - vrd16(W(0x2198))));
    vwr16(CAR_REC(off, 0x16CE), dw(0));
    if (vrd16(W(0x0C40)) == 1) { charge(1); set_d16(0, (uint16_t)(dw(0) + 0x8000)); }
    charge(3);                                          /* subi.w, cmpi.w, bcs.w */
    set_d16(0, (uint16_t)(dw(0) - 0x5800));
    if (dw(0) < 0x5000) goto none;
    charge(2);                                          /* cmpi.w, bne */
    if (vrd16(W(0x0C40)) == 1) {
        charge(3);                                      /* move.w, subi.l, bmi */
        vwr16(W(0x4AA6), 1);
        set_d(4, d_reg(4) - 0x3E8);
        if (!(d_reg(4) & 0x80000000u)) { charge(2); vwr16(W(0x4AA6), 2); }
    } else {
        charge(3);
        vwr16(W(0x4AA6), 0);
        set_d(4, d_reg(4) - 0x5DC);
        if (!(d_reg(4) & 0x80000000u)) {
            charge(3);
            vwr16(W(0x4AA6), 1);
            set_d(4, d_reg(4) - 0xBB8);
            if (!(d_reg(4) & 0x80000000u)) { charge(1); vwr16(W(0x4AA6), 2); }
        }
    }
    charge(9);                                          /* ... cmpi.w, bne */
    set_d16(1, (uint16_t)(off >> 6));
    set_d16(0, (uint16_t)vrd16(W(0x464C) + 2u * dw(1)));
    set_d16(5, (uint16_t)(vrd16(0x01EE2Eu + (uint32_t)((int16_t)dw(0) * 8)) + vrd16(W(0x466C) + 2u * dw(1)) + vrd16(W(0x4AA6))));
    set_d(0, vrd32(W(0x4640)));
    if (vrd16(W(0x0C40)) == 1) { charge(1); set_d16(1, (uint16_t)(dw(1) + 0x10)); }
    charge(7);
    set_d(0, d_reg(0) | (1u << (dw(1) & 31)));
    vwr32(W(0x4640), d_reg(0));
    set_d16(0, (uint16_t)vrd16(W(0x4AA6)));
    {
        uint32_t c = W(0x4644) + (uint32_t)((int16_t)dw(0) * 2);
        vwr16(c, (uint16_t)(vrd16(c) + 1));
    }
    vwr8(CAR_REC(off, 0x16C4), vrd8(CAR_REC(off, 0x16C4)) | 1);
    vwr16(W(0x4560), dw(5));
    if (dw(5) & 0x8000) goto none;
    charge(30);
    sincos_to_d1d2((uint16_t)(0 - vrd16(CAR_REC(off, 0x16B6))));
    vwr16(W(0x456E), dw(1)); vwr16(W(0x4570), dw(2));
    sincos_to_d1d2((uint16_t)(vrd16(CAR_REC(off, 0x16B8)) + 0x8000));
    vwr16(W(0x4572), dw(1)); vwr16(W(0x4574), dw(2));
    sincos_to_d1d2((uint16_t)(0 - (uint16_t)(vrd16(CAR_REC(off, 0x16BA)) + vrd16(CAR_REC(off, 0x4AD8)))));
    vwr16(W(0x4576), dw(1)); vwr16(W(0x4578), dw(2));
    vwr16(W(0x457A), 1);
    return RD_RTS;
none:
    charge(2);
    vwr16(W(0x4560), 0xFFFF);
    return RD_RTS;
}

/* ======================================================================== */
/* leaving the road                                                         */
/* ======================================================================== */

#define TRACK_DATA       0x00ECDCu   /* 64-byte per-course blocks of track table pointers */
#define TRACK_POS        W(0x204C)   /* word: player position along the track (index) */

/* FUN_0001d8d0: off-road handling. When the lateral offset |0x20A6| exceeds
 * the road half-width (0x21C4 / 0x21C6 by side, less 0x28, less 0x14 more on
 * course 7), 0x41D6 flags the side and the car is pushed back toward the road
 * centre line (the track point 2 -- or 10 with 0x405E -- ahead; bearing via
 * FUN_0000a98c) with a strength 0x41A8 from the overshoot (5..15, +4/+5 on
 * course 7); speed is clamped (to 0xE00, and by a per-surface factor at
 * 0x1DD5E) unless on course 7; then FUN_0001da8c. Beyond 0x800 the car is
 * put back on the track (0x1DAE8). */
static uint32_t rd_1d8d0(void)
{
    uint32_t r;
    charge(2);
    if (vrd16(W(0x433C)) != 0) { charge(1); return RD_RTS; }
    charge(9);
    set_a(0, TRACK_DATA);
    set_d16(0, (uint16_t)(vrd16(W(0x2046)) << 6));
    set_a(1, vrd32(TRACK_DATA + 0xC + (uint32_t)(int16_t)dw(0)));
    set_a(3, vrd32(TRACK_DATA + 0x20 + (uint32_t)(int16_t)dw(0)));
    set_d16(0, (uint16_t)vrd16(TRACK_POS));
    set_d16(2, (uint16_t)vrd16(W(0x21C4)));
    set_d16(1, (uint16_t)vrd16(W(0x20A6)));
    if (!(dw(1) & 0x8000)) { charge(1); set_d16(2, (uint16_t)vrd16(W(0x21C6))); }
    charge(2);                                          /* tst.w, bpl */
    if (dw(1) & 0x8000) { charge(1); set_d16(1, (uint16_t)(0 - dw(1))); }
    charge(3);                                          /* subi.w, cmpi.w, bne */
    set_d16(2, (uint16_t)(dw(2) - 0x28));
    if (vrd16(W(0x2046)) == 7) { charge(1); set_d16(2, (uint16_t)(dw(2) - 0x14)); }
    charge(2);                                          /* sub.w, bcs.w */
    uint16_t lim = dw(2);
    set_d16(2, (uint16_t)(lim - dw(1)));
    if (!(lim < dw(1))) { charge(2); vwr16(W(0x41D6), 0); return RD_RTS; }
    charge(2);                                          /* tst.w, bpl */
    if (dw(2) & 0x8000) { charge(1); set_d16(2, (uint16_t)(0 - dw(2))); }
    charge(2);                                          /* cmpi.w, bcc.w */
    if (dw(2) >= 0x800) goto back_on_track;
    charge(3);                                          /* move.w, cmpi.w, bge */
    set_d16(0, dw(2));
    if ((int16_t)dw(2) < 5) { charge(2); set_d(2, 5); }
    else {
        charge(2);                                      /* cmpi.w, ble */
        if ((int16_t)dw(2) > 0xF) { charge(1); set_d16(2, 0xF); }
    }
    charge(2);                                          /* cmpi.w, bne */
    if (vrd16(W(0x2046)) == 7) {
        charge(3);                                      /* addq.w, tst.l, bne */
        set_d16(2, (uint16_t)(dw(2) + 4));
        if (vrd32(JUMP_VEL) == 0) { charge(1); set_d16(2, (uint16_t)(dw(2) + 5)); }
    }
    charge(4);                                          /* move.w x2, cmpi.w, beq */
    vwr16(W(0x41A8), dw(2));
    vwr16(W(0x41AA), dw(0));
    if (vrd16(W(0x2046)) != 7) {
        charge(2);                                      /* cmpi.w, bcs */
        if ((uint16_t)vrd16(CAR_SPEED) >= 0x1200) {
            charge(4);                                  /* move.w x2, sub.w, bpl */
            set_d16(1, (uint16_t)vrd16(CAR_SPEED));
            set_d16(0, (uint16_t)(vrd16(W(0x408A)) - vrd16(W(0x21D6))));
            if (dw(0) & 0x8000) { charge(1); set_d16(0, (uint16_t)(0 - dw(0))); }
            charge(10);
            set_d16(0, (uint16_t)(((dw(0) & 0xFFFE) + 0x4000)));
            set_d16(0, (uint16_t)vrd16(a_reg(5) + (uint32_t)(int16_t)dw(0)));
            set_d(1, (uint32_t)dw(1) * dw(0));
            dbl_swap(1);
            set_d16(0, dw(1));
            set_d(2, 0);
            if (vrd16(ATTRACT_FLAG) == 0) { charge(1); set_d16(2, (uint16_t)vrd16(W(0x204E))); }
            charge(6);                                  /* lea, move.w, mulu.w, lsr.l, cmp.w, bcs */
            set_a(1, 0x01DD5E);
            set_d16(2, (uint16_t)vrd16(0x01DD5Eu + (uint32_t)((int16_t)dw(2) * 2)));
            set_d(1, ((uint32_t)dw(1) * dw(2)) >> 5);
            if (!(dw(1) < (uint16_t)vrd16(CAR_SPEED))) { charge(1); set_d16(1, (uint16_t)vrd16(CAR_SPEED)); }
            charge(2);                                  /* cmpi.w, bcs */
            if ((uint16_t)vrd16(CAR_SPEED) >= 0xE00) { charge(1); set_d16(1, 0xE00); }
            charge(1);
            vwr16(CAR_SPEED, dw(1));
        }
    }
    /* 0x1D9C8: which side, then steer back toward the track */
    charge(4);                                          /* move.w, clr.w, tst.w, bmi */
    set_d16(0, 1); set_d16(1, 0);
    if (!(vrd16(W(0x20A6)) & 0x8000)) { charge(1); set_d16(1, 2); }
    charge(10);
    set_d16(0, (uint16_t)(dw(0) | dw(1)));
    vwr16(W(0x41D6), dw(0));
    set_a(0, TRACK_DATA);
    set_d16(0, (uint16_t)(vrd16(W(0x2046)) << 6));
    set_a(1, vrd32(TRACK_DATA + 0xC + (uint32_t)(int16_t)dw(0)));
    set_d16(0, (uint16_t)(vrd16(TRACK_POS) + 2));
    if (vrd16(W(0x405E)) != 0) { charge(1); set_d16(0, (uint16_t)(dw(0) + 8)); }
    charge(6);                                          /* move.w x2, sub.w x2, tst.w, bne */
    uint32_t pt = a_reg(1) + (uint32_t)((int16_t)dw(0) * 4);
    set_d16(1, (uint16_t)(vrd16(pt) - vrd16(W(0x208C))));
    set_d16(2, (uint16_t)(vrd16(pt + 2) - vrd16(W(0x2090))));
    int zero = 0;
    if (dw(1) == 0) {
        charge(2);
        if (dw(2) == 0) { charge(2); set_d(0, 0); zero = 1; }
    }
    if (!zero) {
        charge(1);
        if ((r = rd_call(L_A98C, 0x01DA1E))) return r;
        charge(2);
        set_d16(0, (uint16_t)(0x4000 - dw(0)));
    }
    charge(23);
    vwr16(W(0x41AC), dw(0));
    set_d16(6, dw(0));
    set_d16(3, 0xA00);
    set_d16(0, (uint16_t)(dw(6) & 0xFFFE));
    set_d16(0, (uint16_t)(0 - vrd16(a_reg(5) + (uint32_t)(int16_t)dw(0))));
    set_d16(1, dw(0));
    muls_w(1, dw(3));
    set_d(1, (d_reg(1) << 16) | (d_reg(1) >> 16));
    muls_w(1, (uint16_t)vrd16(W(0x41A8)));
    set_d16(0, (uint16_t)((dw(6) & 0xFFFE) + 0x4000));
    set_d16(0, (uint16_t)vrd16(a_reg(5) + (uint32_t)(int16_t)dw(0)));
    set_d16(2, dw(0));
    muls_w(2, dw(3));
    set_d(2, (d_reg(2) << 16) | (d_reg(2) >> 16));
    muls_w(2, (uint16_t)vrd16(W(0x41A8)));
    vwr16(W(0x41D8), dw(1));
    vwr16(W(0x41DA), dw(2));
    if ((uint16_t)vrd16(CAR_SPEED) < 0xF00) {
        charge(1);
        if ((r = rd_call(L_1DA8C, 0x01DA70))) return r;
        charge(1);
        return RD_RTS;
    }
    charge(2);                                          /* cmpi.w, beq */
    if (vrd16(W(0x2046)) != 7) {
        charge(2);
        vwr16(W(0x40EA), 0x7200);
        vwr16(W(0x40EC), 0x7200);
    }
    charge(1);
    if ((r = rd_call(L_1DA8C, 0x01DA8A))) return r;
    charge(1);
    return RD_RTS;

back_on_track:                                          /* 0x1DAE8 */
    charge(24);
    set_a(0, TRACK_DATA);
    set_d16(0, (uint16_t)(vrd16(W(0x2046)) << 6));
    {
        uint32_t base = TRACK_DATA + (uint32_t)(int16_t)dw(0);
        set_a(1, vrd32(base + 0xC));
        set_a(2, vrd32(base + 0x14));
        set_a(3, vrd32(base + 0x18));
        set_a(4, vrd32(base + 0x10));
    }
    set_d16(6, (uint16_t)vrd16(TRACK_POS));
    uint32_t k2 = (uint32_t)((int16_t)dw(6) * 2), k4 = (uint32_t)((int16_t)dw(6) * 4);
    set_d16(0, (uint16_t)vrd16(a_reg(3) + k2));
    vwr16(W(0x400A), dw(0));
    vwr16(W(0x209E), dw(0));
    set_d16(0, (uint16_t)(0 - vrd16(a_reg(2) + k2)));
    vwr16(W(0x4088), dw(0));
    vwr16(W(0x408A), dw(0));
    vwr16(W(0x20A0), dw(0));
    vwr16(W(0x400C), 0);
    vwr16(W(0x20A2), 0);
    set_d16(0, (uint16_t)vrd16(a_reg(4) + k2));
    vwr16(W(0x208E), dw(0));
    vwr16(W(0x2108), dw(0));
    vwr16(W(0x208C), vrd16(a_reg(1) + k4));
    vwr16(W(0x2090), vrd16(a_reg(1) + k4 + 2));
    return RD_RTS;
}

/* ======================================================================== */
/* race start                                                               */
/* ======================================================================== */

/* the per-course track tables: A1 = (x, z) points, A2 = pitch, A3 = yaw,
 * A4 = height, all indexed by the track position (A0 = the block itself) */
static void load_track_tables(void)
{
    set_a(0, TRACK_DATA);
    set_d16(0, (uint16_t)(vrd16(W(0x2046)) << 6));
    uint32_t base = TRACK_DATA + (uint32_t)(int16_t)dw(0);
    set_a(1, vrd32(base + 0xC));
    set_a(2, vrd32(base + 0x14));
    set_a(3, vrd32(base + 0x18));
    set_a(4, vrd32(base + 0x10));
}

/* the grid-slot offset: D1w/D2w = (-sin, cos)(-pitch[pos]) * the slot's
 * lateral offset D3w, added to the track point (x, z) -> D2w = x, D1w = z */
static void grid_slot_xz(uint16_t pos, uint16_t lateral)
{
    set_d16(0, (uint16_t)((0 - vrd16(a_reg(2) + (uint32_t)((int16_t)pos * 2))) & 0xFFFE));
    set_d16(1, (uint16_t)vrd16(a_reg(5) + (uint32_t)(int16_t)dw(0)));
    set_d16(0, (uint16_t)(dw(0) + 0x4000));
    set_d16(2, (uint16_t)vrd16(a_reg(5) + (uint32_t)(int16_t)dw(0)));
    set_d16(1, (uint16_t)(0 - dw(1)));
    set_d16(3, lateral);
    muls_w(2, lateral); dbl_swap(2);
    muls_w(1, lateral); dbl_swap(1);
    uint32_t pt = a_reg(1) + (uint32_t)((int16_t)pos * 4);
    set_d16(2, (uint16_t)(dw(2) + vrd16(pt)));
    set_d16(1, (uint16_t)(dw(1) + vrd16(pt + 2)));
}

/* FUN_0001b308: put the player car on its starting-grid slot (0x1B4AA: 8
 * slots of (track position, lateral offset); slot 7 outside a race, else
 * 0x208A-1): track position, yaw, pitch, height and (x, z) from the course's
 * track tables. The race clock's high word starts at -1 on courses 0..8. */
static uint32_t rd_1b308(void)
{
    charge(11);
    load_track_tables();
    set_a(0, 0x01B4AA);
    set_d16(5, 7);
    if (vrd16(ATTRACT_FLAG) == 0) { charge(3); set_d16(5, (uint16_t)((vrd16(W(0x208A)) - 1) & 7)); }
    charge(3);                                          /* move.w, cmpi.w, bcc */
    set_d16(0, 0);
    if ((uint16_t)vrd16(W(0x2046)) < 9) { charge(1); set_d16(0, 0xFFFF); }
    charge(36);
    vwr16(RACE_CLOCK, dw(0));
    uint32_t slot = 0x01B4AAu + (uint32_t)((int16_t)dw(5) * 4);
    uint16_t pos = (uint16_t)vrd16(slot);
    set_d16(6, pos);
    vwr16(TRACK_POS, pos); vwr16(W(0x20C8), pos); vwr16(W(0x20D8), pos);
    uint32_t k2 = (uint32_t)((int16_t)pos * 2);
    set_d16(0, (uint16_t)vrd16(a_reg(3) + k2));
    vwr16(W(0x209E), dw(0));
    set_d16(0, (uint16_t)vrd16(a_reg(2) + k2));
    vwr16(W(0x20A0), dw(0)); vwr16(W(0x408A), dw(0));
    vwr16(W(0x4088), (uint16_t)(0 - dw(0)));
    vwr16(W(0x20A2), 0);
    set_d16(0, (uint16_t)vrd16(a_reg(4) + k2));
    vwr16(CAR_Y, dw(0)); vwr16(GROUND_Y, dw(0));
    grid_slot_xz(pos, (uint16_t)vrd16(slot + 2));
    vwr16(W(0x208C), dw(2));
    vwr16(W(0x2090), dw(1));
    vwr16(W(0x21EC), 0);
    return RD_RTS;
}

/* FUN_0001b3ca: put the 0x4A96+1 (at most 5) extra cars from record offset
 * 0x4A94 on the grid slots after the player's (0x1B4B2): active flag, clock,
 * course, track position, yaw (+ 0x8000 for the stored copies), height and
 * (x, z) as for the player */
static uint32_t rd_1b3ca(void)
{
    charge(12);
    load_track_tables();
    set_a(0, 0x01B4B2);
    set_d16(5, (uint16_t)vrd16(W(0x4A96)));
    if (dw(5) >= 4) { charge(1); set_d16(5, 4); }
    charge(1);
    set_d16(4, (uint16_t)vrd16(W(0x4A94)));
    for (;;) {
        charge(38);
        uint16_t off = dw(4);
        vwr8(CAR_REC(off, 0x16C4), vrd8(CAR_REC(off, 0x16C4)) | 0x80);
        vwr16(CAR_REC(off, 0x16AC), vrd16(RACE_CLOCK));
        vwr16(CAR_REC(off, 0x4AAA), vrd16(W(0x2046)));
        uint32_t slot = 0x01B4B2u + (uint32_t)((int16_t)dw(5) * 4);
        uint16_t pos = (uint16_t)vrd16(slot);
        set_d16(6, pos);
        vwr16(CAR_REC(off, 0x16AE), pos);
        uint32_t k2 = (uint32_t)((int16_t)pos * 2);
        set_d16(0, (uint16_t)vrd16(a_reg(3) + k2));
        vwr16(CAR_REC(off, 0x16B6), dw(0));
        set_d16(0, (uint16_t)(0x8000 - vrd16(a_reg(2) + k2)));
        vwr16(CAR_REC(off, 0x16B8), dw(0));
        vwr16(CAR_REC(off, 0x4AAC), dw(0));
        vwr16(CAR_REC(off, 0x16CA), dw(0));
        vwr16(CAR_REC(off, 0x16DC), dw(0));
        vwr16(CAR_REC(off, 0x16BA), 0);
        set_d16(0, (uint16_t)vrd16(a_reg(4) + k2));
        vwr16(CAR_REC(off, 0x16B2), dw(0));
        vwr16(CAR_REC(off, 0x16BC), dw(0));
        grid_slot_xz(pos, (uint16_t)vrd16(slot + 2));
        vwr16(CAR_REC(off, 0x16B0), dw(2));
        vwr16(CAR_REC(off, 0x16B4), dw(1));
        set_d16(4, (uint16_t)(off + 0x40));
        uint16_t n = dw(5);
        set_d16(5, (uint16_t)(n - 1));
        if (n == 0) break;
        poll();
    }
    charge(1);
    return RD_RTS;
}

/* word offsets in the A6 area FUN_0001b0c2 clears to 0 at a race start */
static const uint16_t race_clear_words[57] = {
    0x46C8, 0x4740, 0x4876, 0x478C, 0x4874, 0x47B0, 0x48E4, 0x47B6, 0x41E4, 0x41E6,
    0x40A4, 0x47B2, 0x41DE, 0x41D8, 0x41DA, 0x4086, 0x4014, 0x4018, 0x4010, 0x4022,
    0x4006, 0x406C, 0x400E, 0x401A, 0x4052, 0x4054, 0x4020, 0x48D6, 0x48D8, 0x49BE,
    0x4084, 0x409A, 0x4984, 0x48EE, 0x49BA, 0x486A, 0x40F0, 0x40CC, 0x487E, 0x49CC,
    0x4842, 0x44F2, 0x478E, 0x412A, 0x412E, 0x41A6, 0x49BC, 0x47AC, 0x41E8, 0x42B8,
    0x42BA, 0x2088, 0x486E, 0x48B8, 0x47A2, 0x48CC, 0x48D0,
};

/* FUN_0001b0c2: race start. Place the player (FUN_0001b308, FUN_0001b2e2),
 * set the lap mark (course table 0x1BA0A) and the time limit (0x1BA20 by the
 * EEPROM difficulty byte 0x10001065 + course/attract), clear the race state,
 * ask the sound CPU for the engine start (unless 0x10001071), and load the
 * course's car parameters (0x1B4CA, 16 bytes per course/mode) and the 48-word
 * handling table (0x1B58A, or 0x1B5EA for the manual gearbox) into 0x4144. */
static uint32_t rd_1b0c2(void)
{
    uint32_t r;
    charge(1);
    if ((r = rd_call(L_1B308, 0x01B0C6))) return r;
    charge(1);
    if ((r = rd_call(L_1B2E2, 0x01B0CA))) return r;
    charge(20);
    vwr16(W(0x46E0), 0);
    vwr16(W(0x46E4), 0);
    vwr16(W(0x49B6), 0x3840);
    vwr16(W(0x44EC), 0xF0);
    set_a(0, 0x01BA0A);
    uint16_t mark = (uint16_t)vrd16(0x01BA0Au + (uint32_t)((int16_t)vrd16(W(0x2046)) * 2));
    set_d(0, 0x10000u | mark);
    vwr32(LAP_NEXT_MARK, d_reg(0));
    vwr16(LAP_LIMIT_LO, mark);
    set_a(0, 0x10001000);
    set_d16(1, (uint16_t)(0x65 + (vrd16(COURSE_INDEX) & 3)));
    set_d16(2, (uint16_t)(vrd16(COURSE_INDEX) & 3));
    if (vrd16(ATTRACT_FLAG) != 0) { charge(1); set_d16(1, (uint16_t)(dw(1) + 4)); }
    charge(6);
    vwr16(W(0x49C0), 0);
    set_d8(1, (uint8_t)vrd8(0x10001000u + (uint32_t)(int16_t)dw(1)));
    set_d16(1, (uint16_t)(dw(1) & 7));
    vwr16(W(0x484A), dw(1));
    if (dw(1) == 4) { charge(1); vwr16(W(0x49C0), 0xFFFF); }
    charge(63);
    set_a(1, 0x01BA20);
    vwr16(RACE_TIME_LIMIT, vrd16(0x01BA20u + (uint32_t)((int16_t)dw(1) * 2)));
    set_d(0, 0);
    for (int i = 0; i < 57; i++) vwr16(W(race_clear_words[i]), 0);
    vwr16(W(0x4344), 0xFFFF);
    if (vrd8(0x10001071u) == 0) {
        charge(6);
        uint32_t sp = a_reg(7) - 4;
        vwr32(sp, d_reg(7));
        set_a(7, sp);
        uint16_t v = (uint16_t)(vrd16(0x60004020u) | 8);
        vwr16(0x60004020u, v);
        vwr16(W(0x48BA), 0x3C);
        set_d(7, vrd32(sp));
        set_a(7, sp + 4);
    }
    charge(3);                                          /* move.w, tst.w, beq */
    vwr16(W(0x48C8), 0x26);
    if (vrd16(W(0x0DDE)) != 0) { charge(1); vwr16(W(0x48C8), 0x1B); }
    charge(6);
    vwr16(W(0x4794), 1);
    vwr8(W(0x11AA), vrd8(W(0x11AA)) | 8);
    set_a(1, 0x01B4CA);
    set_d16(0, (uint16_t)vrd16(COURSE_INDEX));
    if (vrd16(ATTRACT_FLAG) == 0) { charge(1); set_d16(0, (uint16_t)(dw(0) + 8)); }
    charge(11);
    set_d16(0, (uint16_t)(dw(0) * 2));
    {
        static const uint16_t dst[8] = { 0x478A, 0x411C, 0x411E, 0x4120, 0x4122, 0x4124, 0x4126, 0x4128 };
        uint32_t src = 0x01B4CAu + (uint32_t)((int16_t)dw(0) * 8);
        for (int i = 0; i < 8; i++) vwr16(W(dst[i]), vrd16(src + 2u * (uint32_t)i));
    }
    if (vrd16(SHIFT_MANUAL) != 0) { charge(1); vwr16(W(0x478A), (uint16_t)(vrd16(W(0x478A)) + 0xD0)); }
    charge(4);                                          /* lea x2, tst.w, bne */
    uint32_t src = 0x01B58A;
    if (vrd16(W(0x0DDE)) == 0) {
        charge(2);                                      /* tst.w, beq */
        if (vrd16(SHIFT_MANUAL) != 0) { charge(1); src = 0x01B5EA; }
    }
    charge(1);
    uint32_t dsta = W(0x4144);
    set_d16(6, 0x2F);
    for (;;) {
        charge(2);                                      /* move.w (A1)+,(A0)+ ; dbf */
        vwr16(dsta, vrd16(src));
        dsta += 2; src += 2;
        set_a(0, dsta); set_a(1, src);
        uint16_t n = dw(6);
        set_d16(6, (uint16_t)(n - 1));
        if (n == 0) break;
        poll();
    }
    charge(1);
    return RD_RTS;
}

/* ======================================================================== */
/* driving controls                                                         */
/* ======================================================================== */

#define IN_GAS           W(0x0DD4)   /* word: gas pedal (from the I/O board)   */
#define IN_BRAKE         W(0x0DD6)   /* word: brake pedal                      */
#define IN_STEER         W(0x0DD2)   /* word: steering wheel                   */

/* FUN_0001ba32: read the driving controls. Gas (dead zone 0x64, max 0x600,
 * *0x37) -> GEAR_SHIFT_FLAG (0x40B4, the throttle); brake (dead zone 0x96,
 * max 0x400, *4) -> 0x4002; steering -> the lock 0x4284 through the curve at
 * 0x1B64A (0x1B82A for the other cabinet, 0xDDE) and its index 0x4286; speed
 * * 13 -> 0x40B6 (and its change 0x40DE); 0x40F2 = heading + 4*steer. With
 * 0x49BA set the car drives itself (demo): full throttle, no brake, and the
 * wheel follows the track 0x20 ahead, offset by the per-car lane 0x1BCF0. */
static uint32_t rd_1ba32(void)
{
    uint32_t r;
    charge(4);
    set_d(0, 0); set_d(6, 0);
    if (vrd16(W(0x49BA)) == 0) {
        charge(3);                                      /* move.w, subi.w, bpl */
        set_d16(0, (uint16_t)(vrd16(IN_GAS) - 0x64));
        if (dw(0) & 0x8000) { charge(1); set_d16(0, 0); }
        charge(2);
        if (dw(0) >= 0x600) { charge(1); set_d16(0, 0x600); }
        charge(3);                                      /* mulu.w, cmpi.l, bcs.w */
        set_d(0, (uint32_t)dw(0) * 0x37);
        if (d_reg(0) >= 0x10000) { charge(1); set_d(0, 0xFFFFFFFFu); }
        charge(5);                                      /* move.w, moveq, move.w, subi.w, bpl */
        vwr16(GEAR_SHIFT_FLAG, dw(0));
        set_d(0, 0);
        set_d16(0, (uint16_t)(vrd16(IN_BRAKE) - 0x96));
        if (dw(0) & 0x8000) { charge(1); set_d16(0, 0); }
        charge(2);
        if (dw(0) >= 0x400) { charge(1); set_d16(0, 0x400); }
        charge(8);                                      /* lsl, move, moveq, move, neg, asr, subq, bpl */
        set_d16(0, (uint16_t)(dw(0) << 2));
        vwr16(W(0x4002), dw(0));
        set_d(0, 0);
        set_d16(0, (uint16_t)(((int16_t)(uint16_t)(0 - vrd16(IN_STEER)) >> 4) - 1));
        if (dw(0) & 0x8000) {
            charge(2);                                  /* addq.w, bcc */
            uint32_t sum = (uint32_t)dw(0) + 2;
            set_d16(0, (uint16_t)sum);
            if (sum > 0xFFFF) { charge(1); set_d16(0, 0); }
        }
        charge(3);                                      /* lea, tst.w, beq */
        uint32_t curve = 0x01B64A;
        if (vrd16(W(0x0DDE)) != 0) { charge(1); curve = 0x01B82A; }
        set_a(0, curve);
        charge(2);                                      /* move.w, beq.w */
        set_d16(1, dw(0));
        if (dw(1) != 0) {
            charge(1);                                  /* bpl.w */
            if (!(dw(1) & 0x8000)) {
                charge(2);
                set_d16(1, (uint16_t)(dw(1) & 0xFF));
                set_d16(0, (uint16_t)vrd16(curve + 2u * dw(1)));
            } else {
                charge(6);
                set_d16(1, (uint16_t)((0 - dw(1)) & 0xFF));
                set_d16(0, (uint16_t)(0 - vrd16(curve + 2u * dw(1))));
                set_d16(1, (uint16_t)(0 - dw(1)));
            }
        }
        charge(7);                                      /* move x3, move, muls, cmpi.l, bcs */
        vwr16(W(0x4284), dw(0));
        vwr16(W(0x4286), dw(1));
        vwr16(W(0x40E2), vrd16(W(0x40B6)));
        set_d16(0, (uint16_t)vrd16(CAR_SPEED));
        muls_w(0, 0xD);
        if (d_reg(0) >= 0x10000) { charge(1); set_d(0, 0xFFFFFFFFu); }
        charge(10);
        vwr16(W(0x40B6), dw(0));
        set_d16(0, (uint16_t)(dw(0) - vrd16(W(0x40E2))));
        vwr16(W(0x40DE), dw(0));
        set_d16(0, (uint16_t)(vrd16(IN_STEER) * 4 + vrd16(W(0x4088))));
        vwr16(W(0x40F2), dw(0));
        if (vrd16(W(0x49CC)) != 0) { charge(1); vwr16(GEAR_SHIFT_FLAG, 0); }
        charge(5);
        set_d16(0, (uint16_t)(vrd16(W(0x40F6)) - vrd16(IN_STEER)));
        vwr16(W(0x40F8), dw(0));
        vwr16(W(0x40F6), vrd16(IN_STEER));
        return RD_RTS;
    }
    /* 0x1BB2E: self-driving */
    charge(15);
    set_d16(0, (uint16_t)(vrd16(PLAYER_CAR) & 7));
    set_a(0, 0x01BCF0);
    vwr16(W(0x49B8), vrd16(0x01BCF0u + 2u * dw(0)));
    vwr16(GEAR_SHIFT_FLAG, 0x8000);
    vwr16(W(0x4002), 0);
    set_a(0, TRACK_DATA);
    set_d16(0, (uint16_t)(vrd16(W(0x2046)) << 6));
    set_a(1, vrd32(TRACK_DATA + 0xC + (uint32_t)(int16_t)dw(0)));
    set_a(2, vrd32(TRACK_DATA + 0x14 + (uint32_t)(int16_t)dw(0)));
    set_d16(3, (uint16_t)vrd16(TRACK_POS));
    set_d16(0, 0x20);
    if (vrd16(W(0x4866)) != 0) { charge(1); set_d16(0, 0xFFE0); }
    charge(21);
    set_d16(3, (uint16_t)(dw(3) + dw(0)));
    set_d16(0, (uint16_t)(((0 - vrd16(a_reg(2) + (uint32_t)((int16_t)dw(3) * 2))) + 0x4000) & 0xFFFE));
    set_d16(1, (uint16_t)vrd16(a_reg(5) + (uint32_t)(int16_t)dw(0)));
    set_d16(0, (uint16_t)(dw(0) + 0x4000));
    set_d16(2, (uint16_t)vrd16(a_reg(5) + (uint32_t)(int16_t)dw(0)));
    set_d16(1, (uint16_t)(0 - dw(1)));
    muls_w(1, (uint16_t)vrd16(W(0x49B8))); dbl_swap(1);
    muls_w(2, (uint16_t)vrd16(W(0x49B8))); dbl_swap(2);
    uint32_t pt = a_reg(1) + (uint32_t)((int16_t)dw(3) * 4);
    set_d16(1, (uint16_t)(dw(1) + vrd16(pt) - vrd16(W(0x208C))));
    set_d16(2, (uint16_t)(dw(2) + vrd16(pt + 2) - vrd16(W(0x2090))));
    int zero = 0;
    if (dw(1) == 0) {
        charge(2);
        if (dw(2) == 0) { charge(2); set_d(0, 0); zero = 1; }
    }
    if (!zero) {
        charge(1);
        if ((r = rd_call(L_A98C, 0x01BBC0))) return r;
        charge(2);
        set_d16(0, (uint16_t)(0x4000 - dw(0)));
    }
    charge(7);                                          /* neg, move, sub, asr, move, tst, bpl */
    set_d16(0, (uint16_t)(0 - dw(0)));
    vwr16(W(0x40F2), dw(0));
    set_d16(0, (uint16_t)((int16_t)(uint16_t)(dw(0) - vrd16(W(0x4088))) >> 6));
    vwr16(W(0x4284), dw(0));
    if (dw(0) & 0x8000) { charge(1); set_d16(0, (uint16_t)(0 - dw(0))); }
    charge(2);                                          /* cmpi.w, bcs */
    if (dw(0) >= 0x2D) { charge(2); vwr16(W(0x40EA), 0); vwr16(W(0x40EC), 0); }
    charge(1);
    return RD_RTS;
}

/* ======================================================================== */
/* car wheels / shadow render blocks                                        */
/* ======================================================================== */

#define CAR_DIMS         0x01EE2Eu   /* 8 bytes per car section: +2 half-length, +4 half-width (inferred) */
#define CAR_MODELS       0x01EEB4u   /* 8 bytes per car section: model ids for the extra blocks */

/* trig table word at A5 for the angle a (bit 0 dropped) */
static inline uint16_t trig(uint16_t a) { return (uint16_t)vrd16(a_reg(5) + (uint32_t)(int16_t)(uint16_t)(a & 0xFFFE)); }
/* the 68K `muls.w Dx,Dn ; add.l Dn,Dn ; swap Dn` */
static inline void qmul(int n, uint16_t k) { muls_w(n, k); dbl_swap(n); }

/* D0w = a & ~1, D1w = -T(a), D0w += 0x4000, D2w = T(a + 0x4000): (-sin, cos) */
static void sincos_pair(uint16_t a)
{
    set_d16(0, (uint16_t)(a & 0xFFFE));
    set_d16(1, trig(dw(0)));
    set_d16(0, (uint16_t)(dw(0) + 0x4000));
    set_d16(2, trig(dw(0)));
    set_d16(1, (uint16_t)(0 - dw(1)));
}

/* one extra render block (the wheel/axle pair) from the offset pair (ox, oz),
 * the height term `yadd` and the block's model: 0x...+0x00 model, +2 x,
 * +6 y, +0xA z (longs), then (-sin,cos) of yaw 0x16D6, pitch `pitch_f`,
 * roll 0x16BA, and a 1 (the shape at 0x1F90A / 0x1F9D4 / 0x1FB6E) */
static void car_sub_block(uint32_t blk, uint16_t off, uint16_t ox, uint16_t oz, uint16_t yadd, uint16_t pitch_f)
{
    set_d16(1, ox); set_d16(2, oz);
    set_d(1, (uint32_t)(int16_t)dw(1)); set_d(2, (uint32_t)(int16_t)dw(2));
    set_d(1, d_reg(1) + vrd32(W(0x4562)));
    vwr32(blk + 2, d_reg(1));
    uint16_t vi = (uint16_t)(vrd16(W(0x0C40)) & 7);
    set_d16(1, vi);
    set_d(0, 0);
    set_d16(0, (uint16_t)(vrd16(CAR_REC(off, 0x16B2)) - vrd16(W(0x2108) + 2u * vi) - vrd16(W(0x20BE))));
    muls_w(0, 0x19);
    set_d16(0, (uint16_t)(dw(0) + yadd));
    set_d(0, (uint32_t)(int16_t)dw(0));
    vwr32(blk + 6, d_reg(0));
    set_d(2, d_reg(2) + vrd32(W(0x456A)));
    vwr32(blk + 10, d_reg(2));
    sincos_pair((uint16_t)vrd16(CAR_REC(off, 0x16D6)));
    vwr16(blk + 14, dw(1)); vwr16(blk + 16, dw(2));
    sincos_pair((uint16_t)vrd16(CAR_REC(off, pitch_f)));
    vwr16(blk + 18, dw(1)); vwr16(blk + 20, dw(2));
    sincos_pair((uint16_t)vrd16(CAR_REC(off, 0x16BA)));
    vwr16(blk + 22, dw(1)); vwr16(blk + 24, dw(2));
    vwr16(blk + 26, 1);
}

/* the block's model: 1 - 0x468C[car] + the section's entry at CAR_MODELS+k */
static void car_sub_model(uint32_t blk, uint16_t off, uint32_t k)
{
    set_d16(1, (uint16_t)(off >> 6));
    set_d16(5, (uint16_t)(0 - (uint16_t)(vrd16(W(0x468C) + 2u * dw(1)) - 1)));
    set_d16(1, (uint16_t)vrd16(W(0x464C) + 2u * dw(1)));
    set_a(0, CAR_MODELS);
    set_d16(5, (uint16_t)(dw(5) + vrd16(CAR_MODELS + k + (uint32_t)((int16_t)dw(1) * 8))));
    vwr16(blk, dw(5));
}

/* FUN_0001f7be: the two extra render blocks of the car at D6w (0x457C,
 * 0x4598 -- its wheel pairs, offset from the body by the section dimensions
 * rotated by yaw 0x16B6, pitch 0x16B8, roll 0x16BA). For the car being
 * followed (0x46C8) they are replaced by shadow models 0x7F5/0x7F7 (copies at
 * 0x4608/0x4624), and a flashing brightness from 0x46CA is written to 32
 * bytes of video RAM at 0x90016000 with three mixer bytes. */
static uint32_t rd_1f7be(void)
{
    charge(2);                                          /* 0x1FE0A is this function's own rts */
    if (vrd16(W(0x4AA6)) == 2) { charge(1); return RD_RTS; }
    charge(2);
    if (vrd16(W(0x4560)) & 0x8000) { charge(1); return RD_RTS; }
    charge(98);
    uint16_t off = dw(6), car = (uint16_t)(off >> 6);
    set_a(2, CAR_DIMS);
    /* 0x46BA / 0x46C0: the two height offsets (front/back) */
    set_d16(5, car);
    set_d16(5, (uint16_t)vrd16(W(0x464C) + 2u * car));
    set_d16(5, (uint16_t)vrd16(CAR_DIMS + 4 + (uint32_t)((int16_t)dw(5) * 8)));
    set_d16(0, (uint16_t)((0 - vrd16(CAR_REC(off, 0x16B6))) & 0xFFFE));
    set_d16(1, (uint16_t)(0 - trig(dw(0))));
    qmul(1, dw(5));
    vwr16(W(0x46BA), dw(1)); vwr16(W(0x46C0), dw(1));
    set_d16(5, (uint16_t)vrd16(CAR_DIMS + 2 + (uint32_t)((int16_t)car * 8)));
    set_d16(0, (uint16_t)((0 - vrd16(CAR_REC(off, 0x16BA))) & 0xFFFE));
    set_d16(1, (uint16_t)(0 - trig(dw(0))));
    qmul(1, dw(5));
    vwr16(W(0x46BA), (uint16_t)(vrd16(W(0x46BA)) + dw(1)));
    vwr16(W(0x46C0), (uint16_t)(vrd16(W(0x46C0)) - dw(1)));
    /* 0x46B8/0x46BC and 0x46BE/0x46C2: the (x, z) offsets */
    set_d16(5, (uint16_t)vrd16(CAR_DIMS + 4 + (uint32_t)((int16_t)car * 8)));
    set_d16(0, (uint16_t)((((uint16_t)(vrd16(CAR_REC(off, 0x16B6)) + 0x8000)) & 0xFFFE) + 0x4000));
    set_d16(1, trig(dw(0)));
    qmul(1, dw(5));
    set_d16(5, dw(1));
    sincos_pair((uint16_t)vrd16(CAR_REC(off, 0x16B8)));
    qmul(1, dw(5)); qmul(2, dw(5));
    vwr16(W(0x46B8), dw(1)); vwr16(W(0x46BC), dw(2));
    vwr16(W(0x46BE), dw(1)); vwr16(W(0x46C2), dw(2));
    set_d16(5, car);
    set_d16(5, (uint16_t)vrd16(CAR_DIMS + 2 + (uint32_t)((int16_t)car * 8)));
    set_d16(0, (uint16_t)((((uint16_t)(vrd16(CAR_REC(off, 0x16BA)) + 0x8000)) & 0xFFFE) + 0x4000));
    set_d16(1, trig(dw(0)));
    qmul(1, dw(5));
    set_d16(5, dw(1));
    sincos_pair((uint16_t)(vrd16(CAR_REC(off, 0x16B8)) - 0x4000));
    qmul(1, dw(5)); qmul(2, dw(5));
    vwr16(W(0x46B8), (uint16_t)(vrd16(W(0x46B8)) + dw(1)));
    vwr16(W(0x46BC), (uint16_t)(vrd16(W(0x46BC)) + dw(2)));
    sincos_pair((uint16_t)(vrd16(CAR_REC(off, 0x16B8)) + 0x4000));
    qmul(1, dw(5)); qmul(2, dw(5));
    vwr16(W(0x46BE), (uint16_t)(vrd16(W(0x46BE)) + dw(1)));
    vwr16(W(0x46C2), (uint16_t)(vrd16(W(0x46C2)) + dw(2)));
    /* the two blocks */
    charge(52);
    car_sub_model(W(0x457C), off, 2);
    car_sub_block(W(0x457C), off, (uint16_t)vrd16(W(0x46B8)), (uint16_t)vrd16(W(0x46BC)), (uint16_t)vrd16(W(0x46BA)), 0x16DC);
    charge(52);
    car_sub_model(W(0x4598), off, 0);
    car_sub_block(W(0x4598), off, (uint16_t)vrd16(W(0x46BE)), (uint16_t)vrd16(W(0x46C2)), (uint16_t)vrd16(W(0x46C0)), 0x16DC);
    charge(2);                                          /* tst.w, beq */
    if (vrd16(W(0x46C8)) == 0) { charge(1); return RD_RTS; }
    charge(2);                                          /* cmp.w, bne */
    if (off != (uint16_t)vrd16(W(0x46C8))) { charge(1); return RD_RTS; }
    charge(1 + 106);                                    /* bra.b, then to the bgt */
    vwr16(W(0x4598), 0x7F5);
    vwr16(W(0x4608), 0x7F7);
    for (int i = 0; i < 6; i++) vwr32(W(0x460A) + 4u * (uint32_t)i, vrd32(W(0x459A) + 4u * (uint32_t)i));
    vwr16(W(0x4622), vrd16(W(0x45B2)));
    set_a(2, CAR_DIMS);
    set_d16(6, (uint16_t)vrd16(W(0x46C8)));
    off = dw(6); car = (uint16_t)(off >> 6);
    set_d16(5, (uint16_t)vrd16(CAR_DIMS + 2 + (uint32_t)((int16_t)car * 8)));
    set_d16(0, (uint16_t)((0 - vrd16(CAR_REC(off, 0x16BA))) & 0xFFFE));
    set_d16(1, (uint16_t)(0 - trig(dw(0))));
    qmul(1, dw(5));
    set_d16(1, (uint16_t)(0 - dw(1)));
    vwr16(W(0x46C0), dw(1));
    set_d16(5, (uint16_t)vrd16(CAR_DIMS + 2 + (uint32_t)((int16_t)car * 8)));
    set_d16(0, (uint16_t)((((uint16_t)(vrd16(CAR_REC(off, 0x16BA)) + 0x8000)) & 0xFFFE) + 0x4000));
    set_d16(1, trig(dw(0)));
    qmul(1, dw(5));
    set_d16(5, dw(1));
    sincos_pair((uint16_t)(vrd16(CAR_REC(off, 0x16B8)) + 0x4000));
    qmul(1, dw(5)); qmul(2, dw(5));
    vwr16(W(0x46BE), dw(1)); vwr16(W(0x46C2), dw(2));
    vwr16(W(0x45B4), 0x7F5);
    car_sub_block(W(0x45B4), off, (uint16_t)vrd16(W(0x46BE)), (uint16_t)vrd16(W(0x46C2)), (uint16_t)vrd16(W(0x46C0)), 0x16B8);
    vwr16(W(0x4624), 0x7F7);
    for (int i = 0; i < 6; i++) vwr32(W(0x4626) + 4u * (uint32_t)i, vrd32(W(0x45B6) + 4u * (uint32_t)i));
    vwr16(W(0x463E), vrd16(W(0x45CE)));
    set_d16(6, (uint16_t)vrd16(W(0x46C8)));
    off = dw(6);
    if ((int16_t)vrd16(CAR_REC(off, 0x4AB2)) > 7) {
        charge(1);                                      /* lsr (0x46CA,A6) */
        vwr16(W(0x46CA), (uint16_t)((uint16_t)vrd16(W(0x46CA)) >> 1));
    } else {
        charge(4);                                      /* move.w, addi.w, cmpi.w, bcs */
        set_d16(0, (uint16_t)(vrd16(W(0x46CA)) + 0x180));
        if (dw(0) >= 0x8000) { charge(1); set_d16(0, 0x8000); }
        charge(2);                                      /* move.w, bra.b */
        vwr16(W(0x46CA), dw(0));
    }
    charge(47);
    uint32_t a0 = 0x90016000u;
    set_d16(0, trig((uint16_t)(vrd16(W(0x46CA)) - 0x4000)));
    set_d16(0, (uint16_t)(0 - dw(0)));
    qmul(0, 0x30);
    set_d16(0, (uint16_t)((dw(0) + 0x30) & 0xFF));
    for (int i = 0; i < 32; i++) vwr8(a0++, (uint8_t)dw(0));
    set_a(0, a0);
    vwr8(0x90020102u, 0xFF);
    vwr8(0x90020182u, 0);
    vwr8(0x90020202u, 0);
    return RD_RTS;
}

/* ======================================================================== */
/* the player car's driving physics                                         */
/* ======================================================================== */

#define ENGINE_FORCE     W(0x40EA)   /* word: drive force, raised by the throttle, cut by the brake */
#define ENGINE_FORCE_S   W(0x40EC)   /* word: its smoothed copy                                    */
#define YAW_RATE         W(0x41DE)   /* word: the car's yaw (slide) rate                           */
#define CAR_HEADING      W(0x4088)   /* word: where the car points                                 */
#define CAR_COURSE       W(0x408A)   /* word: where the car goes (velocity heading)                */

/* 68K `divs.w Dn` after `ext.l D0`: D0 = sign-extended D0w / D5w */
static void ext_divs(int n, uint16_t d)
{
    set_d(n, (uint32_t)(int16_t)dw(n));
    set_d(n, divs_w(d_reg(n), (int16_t)d));
}

/* FUN_0001cb8a: one frame of the player car's physics. The throttle (0x40B4
 * high byte, off while airborne 0x47BC or with the clutch 0x4790) builds the
 * drive force 0x40EA (smoothed into 0x40EC, which also kicks the yaw rate);
 * the brake 0x4002 cuts it; a missed shift (0x4780) costs force by gear
 * (0x1D2F2). The gear/speed tables (0x1CFC2 / 0x1D052 / 0x1D0E2, by the car
 * 0x56E8) give the steering response 0x40E4 / 0x40E6; the heading 0x4088
 * follows the wheel, the course 0x408A follows the heading (the slide
 * 0x4086), a spin-out halves the speed twice; the yaw rate is limited and
 * damped by the grip values 0x411C..0x4124; finally the car is moved along
 * its course by speed * the gear ratio (0x478A/0x478C), in 8.8 fixed point
 * (fractions at 0x4266 / 0x4268), and 0x40B0 = 0x400A. */
static uint32_t rd_1cb8a(void)
{
    uint32_t r;
    /* --- throttle -> 0x47A2 (full-throttle-at-speed flag) --- */
    charge(3);                                          /* move.w, tst.w, bne */
    set_d16(0, (uint16_t)vrd16(GEAR_SHIFT_FLAG));
    if (vrd16(JUMP_VEL) != 0) { charge(1); set_d(0, 0); }
    else {
        charge(2);                                      /* tst.w, beq */
        if (vrd16(W(0x4790)) != 0) { charge(1); set_d(0, 0); }
    }
    charge(4);                                          /* rol.w, andi.w, subi.w, bmi */
    set_d16(0, (uint16_t)(((dw(0) >> 8) & 0xFF) - 0x80));
    if (dw(0) & 0x8000) {
        charge(2);                                      /* tst.w, beq */
        if (vrd16(W(0x47A2)) != 0) {
            charge(2);                                  /* cmpi.w, bcc */
            if ((uint16_t)vrd16(GEAR_SHIFT_FLAG) < 0x800) { charge(1); vwr16(W(0x47A2), 0); }
        }
        charge(1);                                      /* bra.b */
    } else {
        charge(3);                                      /* bra.b, cmpi.w, bcs */
        if ((uint16_t)vrd16(CAR_SPEED) >= 0x1000) { charge(1); vwr16(W(0x47A2), 0xFFFF); }
    }
    /* --- drive force --- */
    charge(3);                                          /* move.w, add.w, bmi */
    set_d16(1, (uint16_t)(vrd16(ENGINE_FORCE) + dw(0)));
    if (!(dw(1) & 0x8000)) { charge(1); vwr16(ENGINE_FORCE, dw(1)); }
    charge(7);
    set_d16(1, (uint16_t)vrd16(ENGINE_FORCE));
    set_d16(0, (uint16_t)vrd16(ENGINE_FORCE_S));
    set_d16(1, (uint16_t)((int16_t)(uint16_t)(dw(1) - dw(0)) >> 1));
    vwr16(ENGINE_FORCE_S, (uint16_t)(vrd16(ENGINE_FORCE_S) + dw(1)));
    int brake_part = 1;
    if (!(dw(1) & 0x8000)) {
        charge(2);                                      /* cmpi.w, bcs.w */
        if ((uint16_t)vrd16(CAR_SPEED) < 0x1000) brake_part = 0;
        else {
            charge(7);
            set_d16(0, (uint16_t)((int16_t)vrd16(W(0x40A4)) >> 3));
            muls_w(0, dw(1));
            set_d(0, (uint32_t)(int16_t)dw(0));
            set_d(2, (uint32_t)(int16_t)dw(2));
            set_d16(0, (uint16_t)((int16_t)dw(0) >> 1));
            vwr16(YAW_RATE, (uint16_t)(vrd16(YAW_RATE) + dw(0)));
        }
    }
    if (brake_part) {
        charge(6);                                      /* move, lsr, andi, move, sub, bmi */
        set_d16(0, (uint16_t)(((uint16_t)vrd16(W(0x4002)) >> 3) & 0xFFF));
        set_d16(1, (uint16_t)(vrd16(ENGINE_FORCE) - dw(0)));
        if (!(dw(1) & 0x8000)) { charge(1); vwr16(ENGINE_FORCE, dw(1)); }
        charge(3);                                      /* move.w, cmpi.w, ble */
        set_d16(0, (uint16_t)vrd16(W(0x4002)));
        if ((int16_t)dw(0) > 0xC00) {
            charge(5);
            set_d16(0, (uint16_t)(dw(0) >> 7));
            set_d16(1, (uint16_t)((int16_t)vrd16(W(0x40A4)) >> 2));
            muls_w(0, dw(1));
            vwr16(YAW_RATE, (uint16_t)(vrd16(YAW_RATE) + dw(0)));
        }
    }
    /* --- a missed shift --- */
    charge(2);                                          /* tst.w, beq */
    if (vrd16(W(0x4780)) != 0) {
        charge(3);                                      /* move.w, tst.w, bne */
        set_d16(0, (uint16_t)vrd16(W(0x4008)));
        if (vrd16(W(0x4790)) == 0) {
            charge(2);                                  /* sub.w, ble */
            int16_t a = (int16_t)dw(0), b = (int16_t)vrd16(GEAR);
            set_d16(0, (uint16_t)(a - b));
            if (!(a <= b)) {
                charge(5);                              /* lea, move, move, sub, bpl */
                set_a(0, 0x01D2F2);
                set_d16(2, (uint16_t)vrd16(0x01D2F2u + (uint32_t)((int16_t)dw(0) * 2)));
                set_d16(1, (uint16_t)(vrd16(ENGINE_FORCE) - dw(2)));
                if (dw(1) & 0x8000) { charge(1); set_d(1, 0); }
                charge(1);
                vwr16(ENGINE_FORCE, dw(1));
            }
            charge(1);
            vwr16(W(0x4008), vrd16(GEAR));
        }
    }
    /* --- steering response from the tables --- */
    charge(15);
    uint16_t cm = (uint16_t)vrd16(W(0x56E8));
    set_d16(0, cm);
    set_a(0, vrd32(0x01CFC2u + (uint32_t)((int16_t)cm * 4)));
    set_a(1, vrd32(0x01D052u + (uint32_t)((int16_t)cm * 4)));
    set_a(2, vrd32(0x01D0E2u + (uint32_t)((int16_t)cm * 4)));
    set_d16(2, (uint16_t)((vrd16(W(0x40B6)) >> 12) & 0xF));
    set_d16(0, (uint16_t)vrd16(a_reg(0) + (uint32_t)((int16_t)dw(2) * 4)));
    set_d16(1, (uint16_t)vrd16(a_reg(0) + 2 + (uint32_t)((int16_t)dw(2) * 4)));
    set_d16(2, (uint16_t)vrd16(GEAR_SHIFT_FLAG));
    if (vrd16(W(0x4790)) != 0) { charge(1); set_d(2, 0xC000); }
    charge(5);                                          /* rol.w, andi.w, add.w, move.w, bpl */
    set_d16(2, (uint16_t)((dw(2) >> 12) & 0xF));
    set_d16(0, (uint16_t)(dw(0) + vrd16(a_reg(1) + (uint32_t)((int16_t)dw(2) * 2))));
    set_d16(2, (uint16_t)vrd16(W(0x4284)));
    if (dw(2) & 0x8000) { charge(1); set_d16(2, (uint16_t)(0 - dw(2))); }
    charge(8);
    set_d16(2, (uint16_t)vrd16(a_reg(2) + (uint32_t)((int16_t)(dw(2) & 0x3F) * 2)));
    set_d(2, (uint32_t)dw(2) * (uint16_t)vrd16(W(0x40B6)));
    set_d(2, (d_reg(2) << 16) | (d_reg(2) >> 16));
    set_d16(0, (uint16_t)(dw(0) + dw(2)));
    set_d16(1, (uint16_t)(dw(1) + dw(2)));
    if (vrd16(W(0x47AE)) != 0) { charge(2); set_d16(0, 9); set_d16(1, 0xE); }
    charge(8);
    set_d16(2, (uint16_t)((int16_t)vrd16(W(0x40F8)) >> 4));
    set_d16(1, (uint16_t)(dw(1) + dw(2)));
    vwr16(W(0x40E4), dw(0));
    vwr16(W(0x40E6), dw(1));
    set_d16(0, (uint16_t)vrd16(W(0x4284)));
    if (vrd16(W(0x4798)) != 0) { charge(1); set_d16(0, (uint16_t)(0 - dw(0))); }
    charge(2);                                          /* tst.l, beq */
    if (vrd32(JUMP_VEL) != 0) { charge(1); set_d16(0, (uint16_t)((int16_t)dw(0) >> 1)); }
    charge(5);                                          /* asl.w, add.w, move.w, tst.w, ble */
    set_d16(0, (uint16_t)((dw(0) << 6) + vrd16(CAR_HEADING)));
    vwr16(W(0x40CA), dw(0));
    if ((int16_t)vrd16(CAR_SPEED) <= 0) {
        charge(1);
        vwr16(W(0x40A4), 0);
    } else {
        charge(5);                                      /* move.w x2, sub.w, move.w, bgt */
        set_d16(0, (uint16_t)vrd16(W(0x40CA)));
        set_d16(1, (uint16_t)vrd16(CAR_HEADING));
        set_d16(0, (uint16_t)(dw(0) - dw(1)));
        set_d16(5, (uint16_t)vrd16(W(0x40E4)));
        if ((int16_t)dw(5) <= 0) { charge(1); set_d16(5, 1); }
        charge(6);                                      /* ext.l, divs.w, move, add, move, bra */
        ext_divs(0, dw(5));
        vwr16(W(0x40A4), dw(0));
        set_d16(0, (uint16_t)(dw(0) + vrd16(CAR_HEADING)));
        vwr16(CAR_HEADING, dw(0));
    }
    /* --- the course follows the heading; spin-outs --- */
    charge(4);                                          /* tst.w, move.w, cmpi.w, bcs.w */
    set_d16(0, (uint16_t)vrd16(W(0x40CA)));
    if ((uint16_t)vrd16(W(0x41E2)) >= 0x2800) {
        charge(2);                                      /* tst.w, bne.w */
        if (vrd16(W(0x405E)) == 0) {
            int track_dir = 0;
            charge(2);                                  /* cmpi.w, beq */
            if (vrd16(W(0x2046)) != 3) {
                charge(2);                              /* cmpi.w, beq */
                if (vrd16(W(0x0DA0)) == 3) track_dir = 1;
            }
            if (!track_dir) {
                charge(2);                              /* move.w, bra.w */
                set_d16(0, (uint16_t)vrd16(CAR_COURSE));
            } else {
                charge(29);
                vwr16(W(0x40E6), vrd16(W(0x4126)));
                vwr16(W(0x49B4), (uint16_t)(vrd16(W(0x49B4)) + 1));
                set_d16(4, (uint16_t)(0xA + vrd16(TRACK_POS)));
                set_a(0, TRACK_DATA);
                set_d16(0, (uint16_t)(vrd16(W(0x2046)) << 6));
                set_a(1, vrd32(TRACK_DATA + 0xC + (uint32_t)(int16_t)dw(0)));
                set_d16(3, (uint16_t)vrd16(W(0x20A6)));
                sincos_pair((uint16_t)((0 - vrd16(W(0x21D6))) + 0x4000));
                qmul(1, dw(3)); qmul(2, dw(3));
                uint32_t pt = a_reg(1) + (uint32_t)((int16_t)dw(4) * 4);
                set_d16(1, (uint16_t)(dw(1) + vrd16(pt) - vrd16(W(0x208C))));
                set_d16(2, (uint16_t)(dw(2) + vrd16(pt + 2) - vrd16(W(0x2090))));
                int zero = 0;
                if (dw(1) == 0) {
                    charge(2);
                    if (dw(2) == 0) { charge(2); set_d(0, 0); zero = 1; }
                }
                if (!zero) {
                    charge(1);
                    if ((r = rd_call(L_A98C, 0x01CDD8))) return r;
                    charge(2);
                    set_d16(0, (uint16_t)(0x4000 - dw(0)));
                }
                charge(3);                              /* neg.w, tst.w, beq */
                set_d16(0, (uint16_t)(0 - dw(0)));
                if (vrd16(W(0x4866)) != 0) { charge(1); set_d16(0, (uint16_t)(dw(0) + 0x8000)); }
            }
        }
    }
    charge(3);                                          /* sub.w, move.w, bpl */
    set_d16(0, (uint16_t)(dw(0) - vrd16(CAR_COURSE)));
    set_d16(1, dw(0));
    if (dw(1) & 0x8000) { charge(1); set_d16(1, (uint16_t)(0 - dw(1))); }
    charge(2);                                          /* cmpi.w, beq */
    if (vrd16(W(0x2046)) != 7) {
        charge(2);                                      /* cmpi.w, blt */
        if (!((int16_t)dw(1) < 0x5000)) {
            charge(3);                                  /* move.w, lsr, lsr */
            vwr16(CAR_COURSE, vrd16(CAR_HEADING));
            vwr16(CAR_SPEED, (uint16_t)((uint16_t)vrd16(CAR_SPEED) >> 1));
            vwr16(CAR_SPEED, (uint16_t)((uint16_t)vrd16(CAR_SPEED) >> 1));
        }
    }
    charge(5);                                          /* move.w x2, lsl.w, cmp.w, bge */
    set_d16(5, (uint16_t)vrd16(W(0x40E6)));
    set_d16(2, (uint16_t)(dw(5) << 2));
    if ((int16_t)dw(1) < (int16_t)dw(2)) {
        charge(2);                                      /* lsr.w, sub.w */
        set_d16(1, (uint16_t)(dw(1) >> 2));
        set_d16(5, (uint16_t)(dw(5) - dw(1)));
    }
    charge(2);                                          /* tst.w, beq */
    if (dw(5) != 0) {
        charge(3);                                      /* ext.l, divs.w, add.w */
        ext_divs(0, dw(5));
        vwr16(CAR_COURSE, (uint16_t)(vrd16(CAR_COURSE) + dw(0)));
    }
    charge(5);                                          /* move, sub, move, move, bpl */
    set_d16(0, (uint16_t)(vrd16(CAR_HEADING) - vrd16(CAR_COURSE)));
    vwr16(W(0x4086), dw(0));
    set_d16(0, (uint16_t)vrd16(YAW_RATE));
    if (dw(0) & 0x8000) { charge(1); set_d16(0, (uint16_t)(0 - dw(0))); }
    charge(2);                                          /* cmpi.w, bcs */
    if (dw(0) >= 0x7800) {
        charge(3);                                      /* move.w, tst.w, bpl */
        set_d16(0, 0x7800);
        if (vrd16(YAW_RATE) & 0x8000) { charge(1); set_d16(0, (uint16_t)(0 - dw(0))); }
        charge(1);
        vwr16(YAW_RATE, dw(0));
    }
    charge(2);                                          /* move.w, bpl */
    set_d16(0, (uint16_t)vrd16(YAW_RATE));
    if (dw(0) & 0x8000) { charge(1); set_d16(0, (uint16_t)(0 - dw(0))); }
    charge(4);                                          /* move.w, moveq, cmpi.w, bcs */
    vwr16(W(0x41E2), dw(0));
    set_d(2, 6);
    if (dw(0) >= 0x600) { charge(1); set_d(2, 7); }
    charge(9);
    set_d16(0, (uint16_t)vrd16(YAW_RATE));
    set_d(0, (uint32_t)(int16_t)dw(0));
    set_d16(0, (uint16_t)((int16_t)dw(0) >> dw(2)));
    set_d16(0, (uint16_t)(dw(0) + vrd16(CAR_HEADING)));
    vwr16(CAR_HEADING, dw(0));
    set_d16(2, (uint16_t)vrd16(W(0x411C)));
    set_d16(0, (uint16_t)vrd16(W(0x41E2)));
    if (dw(0) < (uint16_t)vrd16(W(0x4124))) {
        charge(4);                                      /* move.w x2, muls.w, bpl */
        set_d16(2, (uint16_t)vrd16(W(0x411E)));
        set_d16(0, (uint16_t)vrd16(W(0x4284)));
        muls_w(0, (uint16_t)vrd16(YAW_RATE));
        if (d_reg(0) & 0x80000000u) {
            charge(4);                                  /* move.w x2, muls.w, bpl */
            set_d16(3, (uint16_t)vrd16(W(0x4286)));
            set_d16(1, (uint16_t)vrd16(W(0x4120)));
            muls_w(1, dw(3));
            if (d_reg(1) & 0x80000000u) { charge(1); set_d16(1, (uint16_t)(0 - dw(1))); }
            charge(2);                                  /* sub.w, bpl */
            uint16_t f = (uint16_t)(vrd16(ENGINE_FORCE) - dw(1));
            vwr16(ENGINE_FORCE, f);
            if (f & 0x8000) { charge(1); vwr16(ENGINE_FORCE, 0); }
            charge(3);                                  /* move.w, muls.w, bpl */
            set_d16(1, (uint16_t)vrd16(W(0x4122)));
            muls_w(1, dw(3));
            if (d_reg(1) & 0x80000000u) { charge(1); set_d16(1, (uint16_t)(0 - dw(1))); }
            charge(4);                                  /* add.w, move.w, sub.w, bpl */
            set_d16(2, (uint16_t)(dw(2) + dw(1)));
            set_d16(3, (uint16_t)(vrd16(W(0x21D6)) - vrd16(W(0x20A0))));
            if (dw(3) & 0x8000) { charge(1); set_d16(3, (uint16_t)(0 - dw(3))); }
            charge(2);                                  /* cmpi.w, bcs */
            if (dw(3) >= 0x100) { charge(1); set_d16(2, (uint16_t)(dw(2) + 0x200)); }
        }
    }
    /* --- yaw-rate decay by the grip D2w --- */
    charge(2);                                          /* move.w, bmi */
    set_d16(0, (uint16_t)vrd16(YAW_RATE));
    int16_t yr = (int16_t)dw(0), g = (int16_t)dw(2);
    int zero_yaw;
    if (yr >= 0) {
        charge(2);                                      /* sub.w, ble */
        set_d16(0, (uint16_t)(yr - g));
        zero_yaw = ((int32_t)yr - g <= 0);
    } else {
        charge(2);                                      /* add.w, bge */
        set_d16(0, (uint16_t)(yr + g));
        zero_yaw = ((int32_t)yr + g >= 0);
    }
    if (zero_yaw) { charge(1); vwr16(YAW_RATE, 0); }
    else { charge(2); vwr16(YAW_RATE, dw(0)); }
    charge(2);                                          /* tst.w, beq */
    if (vrd16(CAR_SPEED) != 0) {
        charge(2);                                      /* move.w, bpl */
        set_d16(0, (uint16_t)vrd16(W(0x4086)));
        if (dw(0) & 0x8000) { charge(1); set_d16(0, (uint16_t)(0 - dw(0))); }
        charge(2);                                      /* cmpi.w #0, bcc (always taken) */
    } else {
        charge(4);
        vwr16(CAR_COURSE, vrd16(CAR_HEADING));
        vwr16(YAW_RATE, 0);
        vwr16(ENGINE_FORCE, 0x7FFF);
        vwr16(ENGINE_FORCE_S, 0x7FFF);
    }
    /* --- move the car --- */
    charge(5);                                          /* move.w x2, add.w, cmpi.w, bne */
    set_d16(1, (uint16_t)vrd16(CAR_SPEED));
    set_d16(0, (uint16_t)(vrd16(W(0x478A)) + vrd16(W(0x478C))));
    if (vrd16(W(0x2046)) == 7) { charge(1); set_d16(0, (uint16_t)(dw(0) + 0x1800)); }
    charge(9);
    set_d(1, (uint32_t)dw(1) * dw(0));
    set_d(1, d_reg(1) << 2);
    set_d(1, (d_reg(1) << 16) | (d_reg(1) >> 16));
    vwr16(W(0x478E), dw(1));
    set_d(1, (uint32_t)dw(1) * 0xD555u);
    dbl_swap(1);
    if (vrd16(W(0x4798)) & 0x8000) { charge(1); set_d16(1, (uint16_t)(0 - dw(1))); }
    charge(11);
    set_d16(0, (uint16_t)(0 - trig((uint16_t)vrd16(CAR_COURSE))));
    qmul(0, dw(1));
    set_d16(0, (uint16_t)(dw(0) - vrd16(W(0x41D8))));
    uint16_t sum = (uint16_t)(dw(0) + vrd16(W(0x4266)));
    set_d16(0, sum);
    vwr8(W(0x4266), (sum & 0x8000) ? 0xFF : 0);
    if (sum & 0x8000) { charge(1); set_d16(0, (uint16_t)(dw(0) + 0x100)); }
    charge(16);
    vwr8(W(0x4267), (uint8_t)dw(0));
    set_d16(0, (uint16_t)((int16_t)dw(0) >> 8));
    vwr16(W(0x40A6), dw(0));
    vwr16(W(0x208C), (uint16_t)(vrd16(W(0x208C)) - dw(0)));
    set_d16(0, (uint16_t)(0 - trig((uint16_t)((vrd16(CAR_COURSE) & 0xFFFE) + 0x4000))));
    qmul(0, dw(1));
    set_d16(0, (uint16_t)(dw(0) - vrd16(W(0x41DA))));
    sum = (uint16_t)(dw(0) + vrd16(W(0x4268)));
    set_d16(0, sum);
    vwr8(W(0x4268), (sum & 0x8000) ? 0xFF : 0);
    if (sum & 0x8000) { charge(1); set_d16(0, (uint16_t)(dw(0) + 0x100)); }
    charge(6);
    vwr8(W(0x4269), (uint8_t)dw(0));
    set_d16(0, (uint16_t)((int16_t)dw(0) >> 8));
    vwr16(W(0x40A8), dw(0));
    vwr16(W(0x2090), (uint16_t)(vrd16(W(0x2090)) + dw(0)));
    vwr16(W(0x40B0), vrd16(W(0x400A)));
    return RD_RTS;
}

/* ======================================================================== */
static uint32_t rd_1be5c_tail(void);

/* FUN_0001be5c: the player car's per-frame dynamics                        */
/* ======================================================================== */

static inline uint32_t rol32(uint32_t v, int k) { return (v << k) | (v >> (32 - k)); }
static inline uint32_t ror32(uint32_t v, int k) { return (v >> k) | (v << (32 - k)); }
/* `bpl.b +2 ; neg.w Dn` -- make Dn's word non-negative, charging the neg */
static inline void abs_w(int n) { if (dw(n) & 0x8000) { charge(1); set_d16(n, (uint16_t)(0 - dw(n))); } }
/* read / write the A6 area */
static inline uint16_t g16(uint16_t off) { return (uint16_t)vrd16(W(off)); }
static inline void s16(uint16_t off, uint16_t v) { vwr16(W(off), v); }

/* -T(angle) (optionally +0x4000) stored signed at `dst` and absolute at `dabs` */
static void attitude_term(uint16_t src, int cosine, uint16_t dst, uint16_t dabs)
{
    charge(cosine ? 7 : 6);
    uint16_t a = (uint16_t)(g16(src) & 0xFFFE);
    if (cosine) a = (uint16_t)(a + 0x4000);
    set_d16(0, (uint16_t)(0 - vrd16(a_reg(5) + (uint32_t)(int16_t)a)));
    s16(dst, dw(0));
    abs_w(0);
    charge(1);
    s16(dabs, dw(0));
}

/* v -= v/8 (rounded toward zero, at least 1) -- the decay at 0x1BEFC/0x1BF12;
 * the result is left in D1w */
static void decay8(uint16_t off)
{
    charge(3);                                          /* moveq, move.w, beq */
    set_d(1, 0);
    set_d16(0, g16(off));
    if (dw(0) != 0) {
        charge(3);                                      /* move.w, asr.w, bmi */
        set_d16(1, dw(0));
        set_d16(0, (uint16_t)((int16_t)dw(0) >> 3));
        if (!(dw(0) & 0x8000)) { charge(1); set_d16(0, (uint16_t)(dw(0) + 1)); }
        charge(1);
        set_d16(1, (uint16_t)(dw(1) - dw(0)));
    }
}

/* the "unit circle clamp" at 0x1C38C / 0x1C47C: D2w = limit, or when |D1w|
 * exceeds `thr`, ((|D1w| - thr) + k) << 16 >> 6 / |D1w| */
static void grip_limit(uint16_t thr, uint16_t k)
{
    charge(3);                                          /* move.w, subi.w, bls */
    set_d16(3, (uint16_t)(dw(1) - thr));
    if (dw(1) > thr) {
        charge(6);                                      /* mulu, asr.l, addi, swap, asr.l, beq */
        set_d(3, (uint32_t)dw(3) * 0x100);
        set_d(3, (uint32_t)((int32_t)d_reg(3) >> 8));
        set_d16(3, (uint16_t)(dw(3) + k));
        set_d(3, (d_reg(3) << 16) | (d_reg(3) >> 16));
        set_d(3, (uint32_t)((int32_t)d_reg(3) >> 6));
        if (d_reg(3) != 0) { charge(1); set_d(3, divu_w(d_reg(3), dw(1))); }
        charge(1);
        set_d16(2, dw(3));
    }
}

/* the lateral force of one axle (0x1C412.. / 0x1C524..): see rd_1be5c */
static void axle_force(uint16_t slip, uint16_t out_c, int swap_first, uint16_t out_f, uint16_t side_src, uint16_t side_dst, uint16_t out_l)
{
    charge(swap_first ? 6 : 5);
    if (swap_first) s16(0x4034, dw(2));                 /* the front's scaled force */
    set_d16(0, g16(slip));
    muls_w(2, dw(0));
    set_d16(0, g16(0x4086));
    muls_w(0, g16(0x4190));
    if (d_reg(0) & 0x80000000u) { charge(1); set_d(0, 0u - d_reg(0)); }
    charge(4);                                          /* swap, rol.l, cmpi.w, bcs */
    set_d(0, rol32((d_reg(0) << 16) | (d_reg(0) >> 16), 6));
    if (dw(0) >= 0x4000) { charge(1); set_d16(0, 0x4000); }
    charge(14);
    s16(out_c, dw(0));
    if (swap_first) set_d(2, rol32((d_reg(2) << 16) | (d_reg(2) >> 16), 4));
    else { set_d(2, rol32(d_reg(2), 4)); set_d(2, (d_reg(2) << 16) | (d_reg(2) >> 16)); }
    s16(out_f, dw(2));
    s16(side_dst, g16(side_src));
    set_d16(0, (uint16_t)(0 - vrd16(a_reg(5) + (uint32_t)(int16_t)(uint16_t)(g16(slip) & 0xFFFE))));
    muls_w(0, g16(0x40BA));
    set_d(0, rol32((d_reg(0) << 16) | (d_reg(0) >> 16), 1));
    abs_w(0);
    charge(4);                                          /* mulu.w, swap, lsr.w, move.w */
    set_d(0, (uint32_t)dw(0) * g16(0x418E));
    set_d(0, (d_reg(0) << 16) | (d_reg(0) >> 16));
    set_d16(0, (uint16_t)(dw(0) >> 1));
    s16(out_l, dw(0));
}

/* D1w * D7w >> 16, doubled / quadrupled, into 0x4020 -- the drive torque */
static void torque(int times4)
{
    muls_w(7, dw(1));                                   /* muls.w D1w,D7 */
    set_d(7, (d_reg(7) << 16) | (d_reg(7) >> 16));
    set_d16(7, (uint16_t)(dw(7) * 2));
    if (times4) set_d16(7, (uint16_t)(dw(7) * 2));
    s16(0x4020, dw(7));
}

/* FUN_0001be5c: the player car's dynamics for one frame: gearbox and controls
 * (FUN_0001db4e, FUN_0001ba32), the attitude sin/cos terms (0x4244..0x4252),
 * slide decay, engine torque from the rpm curve (0x4186/0x4188/0x4172), grip
 * of the front and rear axles (0x4068/0x406A...) with their slip limits,
 * braking and drive torque into the wheel speed 0x409A and rpm 0x4014,
 * clutch/neutral revving, the speed 0x4022 update, the yaw dynamics
 * (0x4080..0x4086) and the lateral terms 0x408C/0x408E; then it continues in
 * FUN_0001cb8a. (purpose of most intermediate words inferred from use) */
static uint32_t rd_1be5c(void)
{
    uint32_t r;
    charge(1);
    if ((r = rd_call(L_1DB4E, 0x01BE62))) return r;
    charge(1);
    if ((r = rd_call(L_1BA32, 0x01BE68))) return r;
    charge(2);                                          /* tst.w, beq */
    if (vrd16(JUMP_VEL) != 0) { charge(1); s16(0x4790, 0xFFFF); }
    /* --- attitude terms --- */
    charge(8);
    set_a(5, 0x06C400);
    set_d16(0, (uint16_t)(0 - vrd16(0x06C400u + (uint32_t)(int16_t)(uint16_t)(g16(0x209E) & 0xFFFE))));
    s16(0x4254, g16(0x4244));
    s16(0x4244, dw(0));
    abs_w(0);
    charge(1);
    s16(0x424C, dw(0));
    attitude_term(0x209E, 1, 0x4246, 0x424E);
    attitude_term(0x20A2, 0, 0x4248, 0x4250);
    attitude_term(0x20A2, 1, 0x424A, 0x4252);
    /* --- speed squared, slide decay --- */
    charge(3);                                          /* move.w, muls.w, move.l */
    set_d16(0, g16(0x4022));
    set_d(0, (uint32_t)((int32_t)(int16_t)dw(0) * (int16_t)dw(0)));
    vwr32(W(0x426E), d_reg(0));
    decay8(0x41D8);
    charge(1);
    s16(0x41D8, dw(1));
    decay8(0x41DA);
    charge(2);                                          /* move.w, bra.w */
    s16(0x41DA, dw(1));
    /* --- pitch rate -> 0x4062, 0x4024 --- */
    charge(10);
    set_d16(0, (uint16_t)(g16(0x4244) - g16(0x4254)));
    muls_w(0, 0x4444);
    set_d(0, (d_reg(0) << 16) | (d_reg(0) >> 16));
    muls_w(0, g16(0x4022));
    set_d(0, (d_reg(0) << 16) | (d_reg(0) >> 16));
    s16(0x4062, dw(0));
    set_d16(0, (uint16_t)(g16(0x4062) + 0x273));
    if (!(dw(0) & 0x8000)) {
        charge(3);                                      /* mulu.w, asr.w, bra.b */
        set_d(0, (uint32_t)dw(0) * 0x21);
        set_d16(0, (uint16_t)((int16_t)dw(0) >> 3));
    } else { charge(1); set_d16(0, 0); }
    charge(2);
    s16(0x4024, dw(0));
    if ((r = rd_call(L_1BD68, 0x01BFA4))) return r;
    charge(1);
    if ((r = rd_call(L_1BD86, 0x01BFAA))) return r;
    /* --- aerodynamic / rolling terms 0x406C..0x4070 --- */
    charge(2);                                          /* move.w, bpl */
    set_d16(1, g16(0x400C));
    abs_w(1);
    charge(13);
    set_d16(7, g16(0x4022));
    muls_w(7, dw(7));
    set_d(7, rol32((d_reg(7) << 16) | (d_reg(7) >> 16), 2));
    s16(0x426C, dw(7));
    set_d(1, (uint32_t)dw(1) * dw(7));
    set_d(1, (d_reg(1) << 16) | (d_reg(1) >> 16));
    set_d(1, (uint32_t)dw(1) * 0x68);
    set_d(1, (uint32_t)((int32_t)d_reg(1) >> 1));
    s16(0x406C, dw(1));
    set_d16(1, g16(0x406C));
    set_d16(0, g16(0x4248));
    abs_w(0);
    charge(13);
    muls_w(1, dw(0));
    set_d(1, (d_reg(1) << 16) | (d_reg(1) >> 16));
    set_d(1, d_reg(1) * 8);
    set_d16(0, dw(1));
    set_d(1, (uint32_t)dw(1) * 0x6666);
    set_d(1, (d_reg(1) << 16) | (d_reg(1) >> 16));
    s16(0x406E, dw(1));
    set_d(0, (uint32_t)dw(0) * 0x9999);
    set_d(0, (d_reg(0) << 16) | (d_reg(0) >> 16));
    s16(0x4070, dw(0));
    if ((r = rd_call(L_1BDCA, 0x01C000))) return r;
    charge(1);
    if ((r = rd_call(L_1BDEC, 0x01C006))) return r;
    /* --- wheel loads 0x4068 / 0x406A --- */
    charge(6);
    s16(0x4068, 0);
    set_d16(1, (uint16_t)(g16(0x407A) + g16(0x4072) + g16(0x406E) + g16(0x4076)));
    if (!(dw(1) & 0x8000)) { charge(1); s16(0x4068, dw(1)); }
    charge(6);
    s16(0x406A, 0);
    set_d16(1, (uint16_t)(g16(0x407C) + g16(0x4074) + g16(0x4070) + g16(0x4078)));
    if (!(dw(1) & 0x8000)) { charge(1); s16(0x406A, dw(1)); }
    /* --- gear constants --- */
    charge(4);                                          /* moveq, move.w, cmpi.w, bne */
    set_d(2, 0);
    set_d16(1, g16(0x4006));
    if (dw(1) == 0xFFFF) { charge(1); set_d16(2, 0xFFFF); }
    charge(2);                                          /* tst.w, bne */
    if (g16(0x4022) == 0) { charge(1); s16(0x4798, dw(2)); }
    charge(9);
    set_d16(1, (uint16_t)(dw(1) << 1));
    set_a(0, W(0x4146) + (uint32_t)(int16_t)dw(1));
    s16(0x4012, (uint16_t)vrd16(a_reg(0)));
    s16(0x4272, (uint16_t)vrd16(a_reg(0) + 0xE));
    s16(0x40C2, (uint16_t)vrd16(a_reg(0) + 0x1C));
    set_d16(1, g16(0x4014));
    /* --- engine torque curve -> 0x4010, D2w --- */
    if (dw(1) <= 0x2710) {
        charge(7);
        set_d(1, (uint32_t)(int16_t)dw(1));
        set_d(1, (uint32_t)dw(1) * g16(0x4186));
        set_d(1, d_reg(1) << 3);
        set_d(1, (d_reg(1) << 16) | (d_reg(1) >> 16));
        s16(0x4010, dw(1));
        set_d16(2, dw(1));
    } else {
        charge(2);                                      /* cmpi.w, bcc */
        if (dw(1) < 0x2CEC) {
            charge(2);
            set_d16(2, g16(0x4172));
        } else {
            charge(9);
            set_d16(1, (uint16_t)(dw(1) - 0x2CEC));
            set_d(1, (uint32_t)(int16_t)dw(1));
            set_d(1, (uint32_t)dw(1) * g16(0x4188));
            set_d(1, rol32((d_reg(1) << 16) | (d_reg(1) >> 16), 6));
            set_d16(2, g16(0x4172));
            set_d16(2, (uint16_t)(((uint16_t)(dw(2) >> 2) - dw(1)) << 2));
        }
        charge(2);                                      /* tst.w, beq */
        if (g16(0x4790) != 0) { charge(1); set_d(2, 0); }
        charge(1);
        s16(0x4010, dw(2));
    }
    /* --- drive torque 0x4026 / 0x400E and the engine-braking 0x401A --- */
    charge(13);
    set_d16(1, (uint16_t)(g16(0x40B4) >> 1));
    muls_w(2, dw(1));
    set_d(2, (d_reg(2) << 16) | (d_reg(2) >> 16));
    muls_w(2, g16(0x4012));
    set_d(2, rol32((d_reg(2) << 16) | (d_reg(2) >> 16), 3));
    muls_w(2, 0x730A);
    set_d(2, (d_reg(2) << 16) | (d_reg(2) >> 16));
    set_d16(2, (uint16_t)(dw(2) * 2));
    s16(0x4026, dw(2));
    if (g16(0x4790) != 0) { charge(1); set_d16(2, 0); }
    charge(24);
    s16(0x400E, dw(2));
    set_d16(1, 0x36B0);
    set_d(1, (uint32_t)dw(1) * g16(0x40B4));
    set_d(1, (d_reg(1) << 16) | (d_reg(1) >> 16));
    set_d16(1, (uint16_t)(dw(1) + 0x1388));
    s16(0x4018, dw(1));
    set_d16(2, (uint16_t)((int16_t)g16(0x4022) >> 4));
    set_d(2, (uint32_t)dw(2) * 0x19);
    set_d(2, ror32(d_reg(2), 1));
    muls_w(2, g16(0x4012));
    set_d(2, d_reg(2) * 8);
    set_d(2, (d_reg(2) << 16) | (d_reg(2) >> 16));
    muls_w(2, 0x57DC);
    set_d(2, d_reg(2) * 16);
    set_d(2, (d_reg(2) << 16) | (d_reg(2) >> 16));
    s16(0x401A, 0);
    {
        uint16_t a = dw(2), b = dw(1);
        set_d16(2, (uint16_t)(a - b));
        if (!(a <= b)) {                                /* bls not taken: over-revving */
            charge(9);
            s16(0x4026, 0);
            set_d(2, (uint32_t)dw(2) * g16(0x4174));
            set_d(2, (d_reg(2) << 16) | (d_reg(2) >> 16));
            muls_w(2, g16(0x4012));
            set_d(2, (d_reg(2) << 16) | (d_reg(2) >> 16));
            set_d(2, d_reg(2) * 4);
            if (g16(0x4790) != 0) { charge(1); set_d16(2, 0); }
            charge(2);
            set_d16(2, 0);
            s16(0x401A, dw(2));
        }
    }
    /* --- rolling resistance 0x4044, drag 0x4046/0x4050, slip 0x4056 --- */
    charge(4);
    vwr32(W(0x401C), 0);
    s16(0x4044, 0);
    set_d16(1, g16(0x40BE));
    if (dw(1) != 0) {
        charge(6);
        set_d16(1, g16(0x426C));
        set_d(1, (uint32_t)dw(1) * g16(0x4068));
        set_d(1, (d_reg(1) << 16) | (d_reg(1) >> 16));
        set_d(1, (uint32_t)dw(1));
        set_d(1, rol32(d_reg(1), 2));
        s16(0x4044, dw(1));
    }
    charge(15);
    set_d16(2, g16(0x409A));
    muls_w(2, 0x51EB);
    set_d(2, (d_reg(2) << 16) | (d_reg(2) >> 16));
    muls_w(2, dw(2));
    set_d(2, (uint32_t)((int32_t)d_reg(2) >> 6));
    set_d(2, (uint32_t)dw(2) * g16(0x406A));
    set_d(2, (d_reg(2) << 16) | (d_reg(2) >> 16));
    set_d(2, (uint32_t)dw(2));
    set_d(2, (uint32_t)((int32_t)d_reg(2) >> 2));
    s16(0x4046, dw(2));
    set_d(2, (uint32_t)dw(2) * g16(0x4288));
    set_d16(2, (uint16_t)(dw(2) + dw(1)));
    s16(0x4050, dw(2));
    set_d16(1, g16(0x4086));
    abs_w(1);
    charge(3);                                          /* lsr.w, tst.w, bne */
    set_d16(1, (uint16_t)(dw(1) >> 6));
    if (g16(0x1100) == 0) {
        charge(5);                                      /* move.w, subq.w, lea, move.w, beq */
        set_d16(0, (uint16_t)(g16(0x204E) - 1));
        set_a(1, 0x01DD70);
        set_d16(0, (uint16_t)vrd16(0x01DD70u + (uint32_t)((int16_t)dw(0) * 2)));
        if (dw(0) != 0) { charge(2); set_d(1, divu_w(d_reg(1), dw(0))); }
    } else {
        charge(2);                                      /* tst.w, beq */
        if (g16(0x4780) != 0) {
            charge(2);                                  /* cmpi.w, beq */
            if (g16(0x4006) != 5) { charge(1); set_d16(1, (uint16_t)(dw(1) >> 2)); }
        }
    }
    charge(3);                                          /* move.w, move.w, bpl */
    s16(0x4056, dw(1));
    set_d16(0, g16(0x48D0));
    abs_w(0);
    charge(4);                                          /* lsl.w, move.w, move.w, beq.w */
    set_d16(0, (uint16_t)(dw(0) << 5));
    s16(0x405C, dw(0));
    set_d16(1, g16(0x428A));
    /* --- off-road drag 0x4274 --- */
    if (dw(1) == 0) {
        charge(2);                                      /* cmpi.w, bcs.w */
        if (g16(0x4022) < 0x400) goto clr_4274;
        charge(2);                                      /* subi.w, bpl.w */
        s16(0x4274, (uint16_t)(g16(0x4274) - 0x20));
        if (!(g16(0x4274) & 0x8000)) goto drag_done;
        goto clr_4274;
    }
    charge(4);                                          /* move.w, mulu.w, cmpi.w, bcs.w */
    set_d16(2, g16(0x4046));
    set_d(2, (uint32_t)dw(2) * dw(1));
    if (dw(2) >= 0x2A00) { charge(1); set_d16(2, 0x2A00); }
    charge(2);                                          /* cmpi.w, bcs.w */
    if (g16(0x4022) < 0x400) goto clr_4274;
    charge(3);                                          /* move.w, sub.w, bcs.w */
    {
        uint16_t a = g16(0x4274);
        set_d16(1, (uint16_t)(a - dw(2)));
        if (a >= dw(2)) {
            charge(2);                                  /* cmpi.w, bcs.w */
            if (dw(1) >= 0x80) {
                charge(2);
                s16(0x4274, (uint16_t)(g16(0x4274) - 0x80));
                goto drag_done;
            }
        }
    }
    charge(2);
    s16(0x4274, dw(2));
    goto drag_done;
clr_4274:
    charge(1);
    s16(0x4274, 0);
drag_done:
    charge(6);
    set_d16(1, (uint16_t)(g16(0x4180) - g16(0x428C)));
    set_d(1, (uint32_t)dw(1) * g16(0x426C));
    set_d(1, (d_reg(1) << 16) | (d_reg(1) >> 16));
    s16(0x4052, dw(1));
    if ((r = rd_call(L_1D300, 0x01C256))) return r;
    charge(1);
    if ((r = rd_call(L_1D3C6, 0x01C25C))) return r;
    charge(6);
    set_d16(1, g16(0x4244));
    set_d16(0, g16(0x4024));
    muls_w(1, dw(0));
    dbl_swap(1);
    if (!(d_reg(1) & 0x80000000u)) { charge(1); set_d16(1, 0); }   /* N of the swapped long */
    charge(2);
    s16(0x4054, dw(1));
    if ((r = rd_call(L_1BD04, 0x01C278))) return r;
    charge(1);
    if ((r = rd_call(L_1BD3C, 0x01C27E))) return r;
    /* --- front axle grip 0x4048, 0x40BA, 0x402C --- */
    charge(2);
    set_d16(1, g16(0x403C));
    abs_w(1);
    charge(14);
    set_d(1, (uint32_t)dw(1) * g16(0x418A));
    set_d(1, (d_reg(1) << 16) | (d_reg(1) >> 16));
    set_d16(1, (uint16_t)(dw(1) + g16(0x4044) + g16(0x401C) + g16(0x4040)));
    s16(0x4048, dw(1));
    set_d16(2, g16(0x402C));
    set_d(2, (uint32_t)dw(2) * g16(0x4068));
    set_d(2, rol32(d_reg(2), 5));
    set_d(2, (d_reg(2) << 16) | (d_reg(2) >> 16));
    s16(0x40BA, dw(2));
    s16(0x402C, g16(0x4176));
    if (!(dw(2) > dw(1))) { charge(1); s16(0x402C, g16(0x4178)); }
    /* --- rear axle grip 0x423C/0x4230, 0x404A, 0x40BC, 0x428E/0x4290 --- */
    charge(6);
    set_d16(1, (uint16_t)(((int16_t)g16(0x400E) >> 1) - g16(0x401A) - g16(0x401E)));
    s16(0x423C, dw(1));
    abs_w(1);
    charge(5);
    s16(0x4230, dw(1));
    set_d16(1, (uint16_t)(dw(1) + g16(0x4046) + g16(0x4042)));
    set_d16(2, g16(0x403E));
    abs_w(2);
    charge(16);
    set_d(2, (uint32_t)dw(2) * g16(0x418A));
    set_d(2, (d_reg(2) << 16) | (d_reg(2) >> 16));
    set_d16(1, (uint16_t)(dw(1) + dw(2)));
    s16(0x404A, dw(1));
    set_d16(2, g16(0x402E));
    set_d(2, (uint32_t)dw(2) * g16(0x406A));
    set_d(2, rol32(d_reg(2), 5));
    set_d(2, (d_reg(2) << 16) | (d_reg(2) >> 16));
    s16(0x40BC, dw(2));
    set_d16(2, (uint16_t)((int16_t)g16(0x4022) >> 4));
    set_d(2, (uint32_t)dw(2) * 0x19);
    set_d16(2, (uint16_t)((int16_t)dw(2) >> 1));
    s16(0x428E, dw(2));
    set_d16(2, (uint16_t)(dw(2) - g16(0x409A)));
    abs_w(2);
    charge(3);
    s16(0x4290, dw(2));
    if (dw(2) < 0x800) {
        charge(5);
        set_d16(1, g16(0x417A));
        set_d16(2, (uint16_t)((int16_t)dw(2) >> 1));
        set_d16(1, (uint16_t)(dw(1) - dw(2)));
        s16(0x402E, dw(1));
    } else {
        charge(1);
        s16(0x402E, g16(0x417C));
    }
    /* --- remaining grip 0x404C / 0x404E --- */
    for (int axle = 0; axle < 2; axle++) {
        uint16_t src = axle ? 0x403E : 0x403C;
        charge(2);                                      /* move.w, bmi */
        set_d16(1, g16(src));
        if (!(dw(1) & 0x8000)) { charge(1); set_d16(1, (uint16_t)(0 - dw(1))); }
        charge(7);
        muls_w(1, g16(0x418C));
        set_d(1, (d_reg(1) << 16) | (d_reg(1) >> 16));
        set_d16(1, (uint16_t)(dw(1) + g16(axle ? 0x40BC : 0x40BA)));
        s16(axle ? 0x404E : 0x404C, 0);
        set_d16(1, (uint16_t)(dw(1) - g16(axle ? 0x4042 : 0x4040) - g16(axle ? 0x4046 : 0x4044)));
        if (!(dw(1) & 0x8000)) { charge(1); s16(axle ? 0x404E : 0x404C, dw(1)); }
    }
    /* --- front slip limit and cornering force --- */
    charge(3);
    set_d16(2, g16(0x416E));
    set_d16(1, g16(0x4038));
    abs_w(1);
    grip_limit(0x2480, 0x2D00);
    charge(3);                                          /* move.w, mulu.w, beq */
    s16(0x4030, dw(2));
    set_d(2, (uint32_t)dw(2) * g16(0x4068));
    if (d_reg(2) != 0) { charge(1); set_d(2, divu_w(d_reg(2), 0x2059)); }
    charge(2);                                          /* move.w, bpl */
    set_d16(3, g16(0x46CC));
    int front_scale = 0;
    if (dw(3) & 0x8000) {
        charge(3);                                      /* move.w, cmp.w, bls */
        set_d16(3, g16(0x4048));
        if (dw(3) > g16(0x40BA)) front_scale = 1;
    }
    if (front_scale) {
        charge(2);
        set_d16(3, g16(0x403C));
        abs_w(3);
        charge(2);                                      /* cmpi.w, bcs */
        if (dw(3) >= 0xC000) { charge(1); set_d16(3, 0xBFD8); }
        charge(3);                                      /* move.w, subi.w, bne */
        s16(0x4292, dw(3));
        set_d16(3, (uint16_t)(dw(3) + 0x4000));
        if (dw(3) == 0) { charge(1); set_d(3, 1); }
        charge(4);                                      /* move.w, ext.l, asl.l, beq */
        set_d16(4, g16(0x401C));
        set_d(4, (uint32_t)(int16_t)dw(4));
        set_d(4, d_reg(4) << 5);
        if (d_reg(4) != 0) { charge(1); set_d(4, divs_w(d_reg(4), (int16_t)dw(3))); }
        charge(4);                                      /* addi.w, move.w, cmpi.w, blt */
        set_d16(4, (uint16_t)(dw(4) + 0x40));
        s16(0x4296, dw(4));
        charge(3);
        if (!((int16_t)dw(4) < 0x28)) {
            muls_w(2, dw(4));
            set_d(2, ror32(d_reg(2), 3));
        } else {
            muls_w(2, 0x14);
            set_d(2, (uint32_t)((int32_t)d_reg(2) >> 2));
        }
    } else {
        charge(1);
        set_d16(2, (uint16_t)(dw(2) << 3));
    }
    axle_force(0x4038, 0x40C4, 1, 0x429E, 0x403C, 0x42B4, 0x4040);
    /* --- rear slip limit and cornering force --- */
    charge(3);
    set_d16(2, g16(0x4170));
    set_d16(1, g16(0x403A));
    abs_w(1);
    grip_limit(0x2000, 0x5A00);
    charge(2);                                          /* move.w, bra.w */
    s16(0x4032, dw(2));
    axle_force(0x403A, 0x40C6, 0, 0x42A0, 0x403E, 0x42B6, 0x4042);
    /* --- total resistance 0x4060 --- */
    charge(3);                                          /* move.w, subq.w, bge */
    set_d16(0, (uint16_t)(g16(0x405A) - 8));
    if ((int16_t)g16(0x405A) < 8) { charge(1); set_d(0, 0); }
    charge(12);
    s16(0x405A, dw(0));
    set_d16(0, (uint16_t)(g16(0x4042) + g16(0x4052) + g16(0x4050) + g16(0x4056) + g16(0x405A) + g16(0x405C) + g16(0x405E)));
    s16(0x4060, dw(0));
    s16(0x4276, 0);
    return rd_1be5c_tail();
}

/* 0x1C7A0 / 0x1C80E: wheel speed D2w/D1w -> rpm 0x4014 through the gear
 * ratio 0x4012 (two slightly different fixed-point chains) */

/* FUN_0001be5c, from 0x1C5BC: traction. Picks, from the drive / brake
 * torques and the axle grips, whether the driven wheels grip or spin (0x40BE,
 * the 0x40EE/0x40F0 launch assist), the drive torque 0x4020, then the wheel
 * speed 0x409A and rpm 0x4014 (free-revving with the clutch 0x4790), the new
 * car speed 0x4022 (8.8 fraction at 0x429A), the yaw dynamics and the
 * lateral terms; continues in FUN_0001cb8a. Written with labels: the 68K
 * code jumps back and forth between its cases (the backward jumps poll). */
static uint32_t rd_1be5c_tail(void)
{
    int flag_n;
    charge(3);                                          /* move.w, cmpi.w, bcc */
    set_d16(7, g16(0x40C2));
    if (g16(0x4022) < 0x100) { charge(1); set_d16(7, 0x500); }
    charge(7);
    set_d16(7, (uint16_t)(dw(7) << 4));
    set_d16(6, g16(0x423C));
    set_d16(5, (uint16_t)(g16(0x401C) - g16(0x404C)));
    set_d16(1, g16(0x404A));
    if (dw(1) <= g16(0x40BC)) goto L606;
    charge(4);                                          /* move.w, addq.w, cmpi.w, bcc */
    set_d16(0, (uint16_t)(g16(0x40F0) + 1));
    if (dw(0) >= 0x3C) goto L606;
    charge(5);
    s16(0x40F0, dw(0));
    set_d16(0, 0x1E00);
    s16(0x40EE, dw(0));
    s16(0x4028, 0x6000);
    goto L6AE;
L606:
    charge(3);                                          /* move.w, move.w, bpl */
    s16(0x40EE, 0);
    set_d16(1, dw(5));
    if (!(dw(1) & 0x8000)) goto L636;
L610:
    charge(7);
    set_d16(1, (uint16_t)((uint16_t)(g16(0x4002) + g16(0x401A) + g16(0x4060)) << 1));
    set_d16(2, (uint16_t)(g16(0x400E) - dw(1)));
    vwr8(W(0x40BE), 0xFF);
L628:
    charge(5);                                          /* muls, swap, add.w, move.w, bra.w */
    muls_w(7, dw(2));
    set_d(7, (d_reg(7) << 16) | (d_reg(7) >> 16));
    set_d16(7, (uint16_t)(dw(7) * 2));
    s16(0x4020, dw(7));
    goto L730;
L636:
    charge(7);
    set_d16(2, (uint16_t)((uint16_t)(g16(0x423C) - g16(0x4274) - g16(0x4276) - g16(0x404C)) * 2));
    vwr8(W(0x40BE), 0);
    poll();                                             /* bra.b back */
    goto L628;
L64E:
    charge(3);                                          /* move.w, sub.w, bmi */
    set_d16(6, (uint16_t)(g16(0x4230) - g16(0x404E)));
    if (dw(6) & 0x8000) { poll(); goto L606; }
    charge(3);                                          /* move.w, cmp.w, bls.w */
    set_d16(1, g16(0x401C));
    if (dw(1) <= g16(0x404C)) goto L6D8;
    charge(5);
    set_d16(1, (uint16_t)(g16(0x4060) + g16(0x404C)));
    vwr8(W(0x40BE), 0);
    set_d16(6, g16(0x423C));
    if (!(dw(6) & 0x8000)) goto L696;
    charge(7);
    set_d16(1, (uint16_t)(0 - (uint16_t)(dw(1) + g16(0x404E))));
    torque(1);
L688:
    charge(2);                                          /* cmpi.w, bmi.w */
    if ((uint16_t)(g16(0x401E) - 0x28) & 0x8000) goto L730;
    charge(1);
    goto L824;
L696:
    charge(8);
    set_d16(1, (uint16_t)(0 - (uint16_t)(dw(1) - g16(0x404E) - g16(0x4046))));
    muls_w(7, dw(1));
    set_d(7, (d_reg(7) << 16) | (d_reg(7) >> 16));
    set_d(7, d_reg(7) * 2);
    s16(0x4020, dw(7));
    goto L7C6;
L6AE:
    charge(8);
    set_d16(7, (uint16_t)(g16(0x40C2) << 4));
    set_d16(6, g16(0x423C));
    set_d16(5, (uint16_t)(g16(0x401C) - g16(0x404C)));
    set_d16(1, g16(0x4048));
    if (dw(1) > g16(0x40BA)) { poll(); goto L64E; }
    charge(2);                                          /* move.w, bpl */
    set_d16(1, dw(6));
    if (dw(1) & 0x8000) { charge(1); set_d16(6, (uint16_t)(0 - dw(6))); }
    charge(2);                                          /* sub.w, bmi.w */
    set_d16(6, (uint16_t)(dw(6) - g16(0x404E)));
    if (dw(6) & 0x8000) { poll(); goto L610; }
L6D8:
    charge(3);                                          /* st, move.w, bpl */
    vwr8(W(0x40BE), 0xFF);
    set_d16(1, g16(0x423C));
    if (!(dw(1) & 0x8000)) goto L706;
    charge(12);
    set_d16(1, (uint16_t)(0 - (uint16_t)(g16(0x4060) + g16(0x4274) + g16(0x4276) + g16(0x401C) + g16(0x404E))));
    torque(1);
    poll();                                             /* bra.b back */
    goto L688;
L706:
    charge(13);
    set_d16(1, (uint16_t)(0 - (uint16_t)(g16(0x4060) + g16(0x4274) + g16(0x4276) + g16(0x401C) - g16(0x404E) - g16(0x4046))));
    torque(1);
    goto L7C6;
L730:
    charge(2);                                          /* tst.w, bne.w */
    if (g16(0x4790) != 0) goto L84E;
    charge(5);                                          /* move.w x2, clr.l, eor.w, bmi */
    set_d16(3, g16(0x4012));
    set_d16(2, (uint16_t)(g16(0x4236) ^ dw(3)));
    set_d(1, 0);
    if (!(dw(2) & 0x8000)) {
        charge(3);                                      /* subq.l, cmpi.w, bcs */
        set_d(1, 0xFFFFFFFFu);
        if (g16(0x4290) < 0x40) goto L78C;
    }
    charge(9);
    set_d16(2, (uint16_t)(0 - g16(0x401A) - g16(0x401E) - g16(0x404E) - g16(0x4042) - g16(0x4046)));
    set_d16(1, g16(0x409A));
    if (d_reg(1) & 0x80000000u) {
        charge(2);                                      /* cmp.w, bpl */
        if ((uint16_t)(dw(1) - g16(0x428E)) & 0x8000) { charge(1); set_d16(2, (uint16_t)(0 - dw(2))); }
    }
    charge(8);
    set_d16(3, (uint16_t)((int16_t)g16(0x400E) >> 1));
    set_d16(2, (uint16_t)(dw(2) + dw(3)));
    muls_w(2, 0xAAB);
    set_d(2, (d_reg(2) << 16) | (d_reg(2) >> 16));
    s16(0x40C0, dw(2));
    set_d16(2, (uint16_t)(dw(2) + dw(1)));
    goto L79C;
L78C:
    charge(5);
    set_d16(1, g16(0x4236));
    set_d16(2, (uint16_t)((int16_t)g16(0x4022) >> 4));
    muls_w(2, 0x19);
    set_d16(2, (uint16_t)((int16_t)dw(2) >> 1));
L79C:
    charge(1);                                          /* bpl */
    if (dw(2) & 0x8000) { charge(1); set_d16(2, 0); }
    charge(14);
    s16(0x409A, dw(2));
    muls_w(2, g16(0x4012));
    set_d(2, d_reg(2) * 8);
    set_d(2, (d_reg(2) << 16) | (d_reg(2) >> 16));
    muls_w(2, 0x57DC);
    set_d(2, d_reg(2) * 16);
    set_d(2, (d_reg(2) << 16) | (d_reg(2) >> 16));
    s16(0x4014, dw(2));
    goto L84E;
L7C6:
    charge(2);
    if (g16(0x4790) != 0) goto L84E;
    charge(12);
    set_d16(1, (uint16_t)(((int16_t)g16(0x400E) >> 1) - g16(0x4046) - g16(0x4042) - g16(0x401E) - g16(0x404E)));
    muls_w(1, 0xAAB);
    set_d(1, (d_reg(1) << 16) | (d_reg(1) >> 16));
    s16(0x40C0, dw(1));
    set_d16(1, (uint16_t)(dw(1) + g16(0x409A)));
    if (!((int16_t)dw(1) <= (int16_t)g16(0x4272))) { charge(1); set_d16(1, g16(0x4272)); }
    charge(3);                                          /* move.w, cmp.w, bpl */
    set_d16(2, g16(0x428E));
    if ((uint16_t)(dw(1) - dw(2)) & 0x8000) {
        charge(2);                                      /* move.w, bpl */
        set_d16(1, dw(2));
        if (dw(1) & 0x8000) { charge(1); set_d16(1, 0); }
    }
    charge(1);
    s16(0x409A, dw(1));
L80E:
    charge(7);
    muls_w(1, 0x57DC);
    set_d(1, (d_reg(1) << 16) | (d_reg(1) >> 16));
    muls_w(1, g16(0x4012));
    set_d(1, d_reg(1) << 7);
    set_d(1, (d_reg(1) << 16) | (d_reg(1) >> 16));
    s16(0x4014, dw(1));
    goto L84E;
L824:
    charge(2);                                          /* tst.w, beq.w */
    if (g16(0x4790) == 0) goto L84E;
    charge(8);
    set_d16(1, (uint16_t)(0 - (uint16_t)(g16(0x401A) + g16(0x401E))));
    muls_w(1, 0xAAB);
    set_d(1, (d_reg(1) << 16) | (d_reg(1) >> 16));
    s16(0x40C0, dw(1));
    set_d16(1, (uint16_t)(dw(1) + g16(0x409A)));
    if (dw(1) & 0x8000) { charge(1); set_d16(1, 0); }
    charge(2);                                          /* move.w, bra.b */
    s16(0x409A, dw(1));
    poll();
    goto L80E;
L84E:
    /* --- clutch / neutral revving --- */
    charge(2);
    set_d16(1, g16(0x409A));
    if (dw(1) & 0x8000) { charge(1); s16(0x409A, 0); }
    charge(2);                                          /* cmpi.w, bgt */
    if (!((int16_t)g16(0x4020) > -0x700)) { charge(1); s16(0x4020, 0xF900); }
    charge(2);                                          /* tst.w, bne */
    int neutral_rev = 0;
    if (g16(0x4006) == 0) {
        charge(5);
        set_d16(0, g16(0x4794));
        set_d16(1, (uint16_t)(g16(0x4790) ^ dw(0)));
        set_d16(0, (uint16_t)(dw(0) & dw(1)));
        if (dw(0) != 0) neutral_rev = 1;
    }
    if (neutral_rev) {
        charge(12);
        set_d16(0, g16(0x40B4));
        set_d16(1, 0xBC0);
        set_d(1, (uint32_t)dw(1) * dw(0));
        set_d(1, (d_reg(1) << 16) | (d_reg(1) >> 16));
        s16(0x409A, dw(1));
        set_d(0, (uint32_t)dw(0) * 0x36B0);
        set_d(0, (d_reg(0) << 16) | (d_reg(0) >> 16));
        set_d16(0, (uint16_t)(dw(0) + 0x1388));
        s16(0x4018, dw(0));
        set_d16(1, 0x1800);
        {
            uint16_t a = dw(0), b = g16(0x4014);
            set_d16(0, (uint16_t)(a - b));
            if (a < b) { charge(1); set_d16(1, 0xC00); }
        }
        charge(6);
        muls_w(0, dw(1));
        set_d(0, (d_reg(0) << 16) | (d_reg(0) >> 16));
        s16(0x409C, dw(0));
        set_d16(0, (uint16_t)(dw(0) + g16(0x4014)));
        if (dw(0) >= 0x36B0) { charge(1); set_d16(0, 0x36B0); }
        charge(3);
        s16(0x4014, dw(0));
        s16(0x4794, g16(0x4790));
        goto L91E;
    }
    charge(3);                                          /* move.w, tst.w, beq */
    s16(0x4794, g16(0x4790));
    if (g16(0x4790) == 0) goto L91E;
    charge(4);                                          /* move.w x2, sub.w, bcc */
    set_d16(1, 0x1800);
    {
        uint16_t a = g16(0x4018), b = g16(0x4014);
        set_d16(0, (uint16_t)(a - b));
        if (a < b) { charge(1); set_d16(1, 0xC00); }
    }
    charge(5);
    muls_w(0, dw(1));
    set_d(0, (d_reg(0) << 16) | (d_reg(0) >> 16));
    s16(0x409C, dw(0));
    if (vrd16(JUMP_VEL) != 0) { charge(1); set_d16(0, (uint16_t)((int16_t)dw(0) >> 4)); }
    charge(5);
    set_d16(0, (uint16_t)(dw(0) + g16(0x4014)));
    s16(0x4014, dw(0));
    set_d16(1, g16(0x404A));
    if (dw(1) >= g16(0x40BC)) goto L92C;
    charge(6);
    set_d16(2, (uint16_t)((int16_t)g16(0x4022) >> 4));
    muls_w(2, 0x19);
    set_d16(2, (uint16_t)((int16_t)dw(2) >> 1));
    s16(0x409A, dw(2));
    goto L92C;
L91E:
    charge(2);
    if (g16(0x4014) < 0x1388) { charge(1); s16(0x4014, 0x1388); }
L92C:
    charge(4);
    set_d16(1, g16(0x4014));
    set_d16(2, 0x36B0);
    if (dw(1) > 0x36B0) { charge(1); s16(0x4014, dw(2)); }
    /* --- the speed --- */
    charge(2);                                          /* move.w, bpl */
    set_d16(1, g16(0x4020));
    if (dw(1) & 0x8000) charge(1);                      /* bra.b */
    else {
        charge(4);
        set_d16(3, g16(0x4798));
        set_d16(6, (uint16_t)(g16(0x4006) ^ dw(3)));
        if (dw(6) & 0x8000) { charge(1); set_d16(1, 0xD8F0); }
    }
    charge(6);
    muls_w(1, 0x222);
    set_d(1, d_reg(1) + vrd32(W(0x429A)));
    set_d(1, (d_reg(1) << 16) | (d_reg(1) >> 16));
    s16(0x409E, dw(1));
    int clear_41a2 = 1;                                 /* cmpi.w/bcs.w charged above */
    if (g16(0x4022) >= 0x200) {
        charge(2);                                      /* tst.b, beq.w */
        if (vrd8(W(0x41A2)) == 0) clear_41a2 = 0;
    }
    if (clear_41a2) { charge(1); vwr8(W(0x41A2), 0); }
    charge(2);                                          /* tst.w, beq */
    int dec = 0;
    if (g16(0x41A6) != 0) {
        charge(2);                                      /* cmpi.w, bcs */
        if (g16(0x4022) >= 0x200) dec = 1;
    }
    if (dec) {
        charge(2);
        s16(0x4022, (uint16_t)(g16(0x4022) - 0x80));
    } else {
        charge(2);
        set_d16(1, g16(0x409E));
        s16(0x4022, (uint16_t)(g16(0x4022) + dw(1)));
    }
    flag_n = (g16(0x4022) & 0x8000) != 0;
    charge(1);                                          /* bpl */
    if (flag_n) { charge(1); s16(0x4022, 0); }
    charge(2);                                          /* bpl (always taken), clr.w */
    set_d16(1, 0);
    /* --- yaw dynamics --- */
    charge(46);
    set_d(1, (d_reg(1) << 16) | (d_reg(1) >> 16));
    vwr32(W(0x429A), d_reg(1));
    set_d(1, (d_reg(1) << 16) | (d_reg(1) >> 16));
    set_d16(1, g16(0x403E));
    muls_w(1, 0x451E);
    set_d16(2, g16(0x403C));
    muls_w(2, 0x67AE);
    set_d(1, (d_reg(1) - d_reg(2)) * 4);
    set_d(1, (d_reg(1) << 16) | (d_reg(1) >> 16));
    s16(0x4080, dw(1));
    set_d16(1, g16(0x4080));
    muls_w(1, 0x11D3);
    set_d(1, (d_reg(1) << 16) | (d_reg(1) >> 16));
    s16(0x40A0, dw(1));
    set_d16(1, g16(0x40A0));
    s16(0x4084, (uint16_t)(g16(0x4084) + dw(1)));
    set_d16(2, g16(0x4022));
    muls_w(2, 0x5FE0);
    set_d(2, (d_reg(2) << 16) | (d_reg(2) >> 16));
    muls_w(2, g16(0x4084));
    set_d(2, (uint32_t)((int32_t)d_reg(2) >> 2));
    set_d(2, (uint32_t)((int32_t)d_reg(2) >> 7));
    set_d16(6, g16(0x403C));
    set_d16(0, g16(0x403E));
    muls_w(0, 0x4CCC);
    set_d(0, d_reg(0) << 2);
    set_d(0, (d_reg(0) << 16) | (d_reg(0) >> 16));
    set_d16(6, (uint16_t)(0 - (uint16_t)(((int16_t)(uint16_t)(dw(6) + dw(0)) >> 2) + dw(2))));
    set_d16(0, dw(6));
    s16(0x4082, dw(6));
    set_d16(0, g16(0x4082));
    set_d16(3, g16(0x4022));
    muls_w(0, 0xA5);
    s16(0x40A2, 0);
    set_d16(3, (uint16_t)(dw(3) + 0x40));
    set_d(0, divs_w(d_reg(0), (int16_t)dw(3)));
    s16(0x40A2, dw(0));
    set_d16(0, g16(0x40A2));
    s16(0x4086, (uint16_t)(g16(0x4086) + dw(0)));
    set_d16(0, g16(0x4084));
    abs_w(0);
    for (int k = 0; k < 2; k++) {                       /* clamp 0x4084 to +-0x800, 0x4086 to +-0x1800 */
        uint16_t off = k ? 0x4086 : 0x4084, lim = k ? 0x1800 : 0x800;
        if (k) { charge(2); set_d16(0, g16(0x4086)); abs_w(0); }
        charge(2);                                      /* cmpi.w, bcs */
        if (dw(0) >= lim) {
            charge(3);                                  /* move.w, tst.w, bpl */
            set_d16(0, lim);
            if (g16(off) & 0x8000) { charge(1); set_d16(0, (uint16_t)(0 - dw(0))); }
            charge(1);
            s16(off, dw(0));
        }
    }
    charge(2);
    if (g16(0x4022) < 0x10) { charge(4); s16(0x40A2, 0); s16(0x4086, 0); s16(0x40A0, 0); s16(0x4084, 0); }
    charge(2);
    if (g16(0x4022) < 8) { charge(1); s16(0x4022, 0); }
    /* --- 0x4234/0x4236 and the lateral terms 0x408C / 0x408E --- */
    charge(13);
    set_d16(0, (uint16_t)(0 - vrd16(a_reg(5) + (uint32_t)(int16_t)(uint16_t)(g16(0x4086) & 0xFFFE))));
    s16(0x4234, dw(0));
    set_d16(0, (uint16_t)(0 - vrd16(a_reg(5) + (uint32_t)(int16_t)(uint16_t)((g16(0x4086) & 0xFFFE) + 0x4000))));
    s16(0x4236, dw(0));
    set_d16(0, g16(0x400A));
    abs_w(0);
    charge(3);                                          /* move.w, move.w, bpl */
    set_d16(1, g16(0x400C));
    set_d16(2, dw(1));
    abs_w(1);
    charge(6);
    set_d16(0, (uint16_t)((dw(0) - dw(1)) & 0xFFFE));
    set_d16(0, (uint16_t)(0 - vrd16(a_reg(5) + (uint32_t)(int16_t)dw(0))));
    set_d16(1, g16(0x4084));
    if (dw(1) & 0x8000) { charge(1); set_d16(0, (uint16_t)(0 - dw(0))); }
    charge(6);
    set_d16(0, (uint16_t)(dw(0) + 0x1000));
    muls_w(1, dw(0));
    set_d(1, rol32(d_reg(1), 4));
    set_d(1, (d_reg(1) << 16) | (d_reg(1) >> 16));
    if (g16(0x4022) == 0) { charge(1); set_d16(1, 0); }
    charge(2);                                          /* cmpi.w, bcc */
    if (g16(0x4022) < 0x280) {
        charge(2);                                      /* and.w, bpl */
        if (dw(1) & 0x8000) {
            charge(2);                                  /* cmpi.w, bcc */
            if (dw(1) < 0xFA00) { charge(2); set_d16(1, 0xFA00); }
        } else {
            charge(2);                                  /* cmpi.w, bcs */
            if (dw(1) >= 0x600) { charge(1); set_d16(1, 0x600); }
        }
    }
    charge(11);
    s16(0x408C, dw(1));
    muls_w(2, 0x7525);
    set_d(2, (d_reg(2) << 16) | (d_reg(2) >> 16));
    muls_w(2, g16(0x4022));
    set_d(2, rol32((d_reg(2) << 16) | (d_reg(2) >> 16), 2));
    s16(0x408E, dw(2));
    set_d16(3, (uint16_t)((int16_t)dw(1) >> 3));
    set_d16(1, (uint16_t)(dw(1) - dw(3)));
    return 0x01CB8A;                                    /* bra.w into FUN_0001cb8a */
}

static const rd_entry rd_table_b3[] = {
    { 0x01B0C2, rd_1b0c2, 0x1FFF, 0, 0, "FUN_0001b0c2" },
    { 0x01B308, rd_1b308, 0x1FFF, 0, 0, "FUN_0001b308" },
    { 0x01B3CA, rd_1b3ca, 0x1FFF, 0, 0, "FUN_0001b3ca" },
    { 0x01BA32, rd_1ba32, 0x07FF, 0, 0, "FUN_0001ba32" },
    { 0x01BD04, rd_1bd04, 0x0003, 0, 0, "FUN_0001bd04" },
    { 0x01BD86, rd_1bd86, 0x0003, 0, 0, "FUN_0001bd86" },
    { 0x01BE5C, rd_1be5c, 0x2FFF, 0, 0, "FUN_0001be5c" },
    { 0x01CB34, rd_1cb34, 0x0007, 0, 0, "FUN_0001cb34" },
    { 0x01CB8A, rd_1cb8a, 0x07FF, 0, 0, "FUN_0001cb8a" },
    { 0x01D300, rd_1d300, 0x08FF, 0, 0, "FUN_0001d300" },
    { 0x01D3C6, rd_1d3c6, 0x0001, 0, 0, "FUN_0001d3c6" },
    { 0x01D3F8, rd_1d3f8, 0x03FF, 0, 0, "FUN_0001d3f8" },
    { 0x01D8D0, rd_1d8d0, 0x1FFF, 0, 0, "FUN_0001d8d0" },
    { 0x01DA8C, rd_1da8c, 0x0003, 0, 0, "FUN_0001da8c" },
    { 0x01DAAA, rd_1daaa, 0x0007, 0, 0, "FUN_0001daaa" },
    { 0x01DB4E, rd_1db4e, 0x08FF, 0, 0, "FUN_0001db4e" },
    { 0x01DC20, rd_1dc20, 0x020F, 0, 0, "FUN_0001dc20" },
    { 0x01DD82, rd_1dd82, 0x0307, 0, 0, "FUN_0001dd82" },
    { 0x01DEDA, rd_1deda, 0x0003, 0, 0, "FUN_0001deda" },
    { 0x01DF6A, rd_1df6a, 0x0001, 0, 0, "FUN_0001df6a" },
    { 0x01E006, rd_1e006, 0x1BFF, 0, 0, "FUN_0001e006" },
    { 0x01E292, rd_1e292, 0x0FDF, 0, 0, "FUN_0001e292" },
    { 0x01E2B4, rd_1e2b4, 0x0FDF, 0, 0, "FUN_0001e2b4" },
    { 0x01E3CC, rd_1e3cc, 0x0FDF, 0, 0, "FUN_0001e3cc" },
    { 0x01E49A, rd_1e49a, 0x0FDF, 0, 0, "FUN_0001e49a" },
    { 0x01E5C4, rd_1e5c4, 0x0FDF, 0, 0, "FUN_0001e5c4" },
    { 0x01E6AA, rd_1e6aa, 0x0003, 0, 0, "FUN_0001e6aa" },
    { 0x01E6B8, rd_1e6b8, 0x03FF, 0, 0, "FUN_0001e6b8" },
    { 0x01E6CE, rd_1e6ce, 0x0000, 0, 0, "FUN_0001e6ce" },
    { 0x01E6D4, rd_1e6d4, 0x0003, 0, 0, "FUN_0001e6d4" },
    { 0x01E6F4, rd_1e6f4, 0x0003, 0, 0, "FUN_0001e6f4" },
    { 0x01E78A, rd_1e78a, 0x0207, 0, 0, "FUN_0001e78a" },
    { 0x01E930, rd_1e930, 0x0001, 0, 0, "FUN_0001e930" },
    { 0x01EA1C, rd_1ea1c, 0x0000, 0, 0, "FUN_0001ea1c" },
    { 0x01EAA0, rd_1eaa0, 0x00F9, 0, 0, "FUN_0001eaa0" },
    /* 0x01EB66 rd_1eb66: passes every check (556 calls + 2880 fuzz) but RR_RD=1
     * changes the frame timing once in the race (idle counter 0x2390 at
     * f2220) -- left lifted until that is understood */
    { 0x01EC32, rd_1ec32, 0x0103, 0, 0, "FUN_0001ec32" },
    { 0x01ECE4, rd_1ece4, 0x0807, 0, 0, "FUN_0001ece4" },
    { 0x01ED04, rd_1ed04, 0x0F01, 0, 0, "FUN_0001ed04" },
    { 0x01ED66, rd_1ed66, 0x0807, 0, 0, "FUN_0001ed66" },
    { 0x01F1DA, rd_1f1da, 0x1FFF, 0, 0, "FUN_0001f1da" },
    { 0x01F2C0, rd_1f2c0, 0x03FF, 0, 0, "FUN_0001f2c0" },
    { 0x01F4C0, rd_1f4c0, 0x03FF, 0, 0, "FUN_0001f4c0" },
    { 0x01F6D4, rd_1f6d4, 0x07FF, 0, 0, "FUN_0001f6d4" },
    { 0x01F72C, rd_1f72c, 0x1FE7, 0, 0, "FUN_0001f72c" },
    { 0x01F7BE, rd_1f7be, 0x07FF, 0, 0, "FUN_0001f7be" },
    { 0x01FE38, rd_1fe38, 0x08FF, 0, 0, "FUN_0001fe38" },
    { 0x01FF4E, rd_1ff4e, 0x0FFF, 0, 0, "thunk_FUN_00020238" },
    { 0x020238, rd_20238, 0x0FFF, 0, 0, "FUN_00020238" },
    { 0x0204E2, rd_204e2, 0x0FFF, 0, 0, "FUN_000204e2" },
    { 0x02077C, rd_2077c, 0x0BFF, 0, 0, "FUN_0002077c" },
};
RD_REGISTER(rd_table_b3)
