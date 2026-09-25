/*
 * rd_b7.c -- Phase B batch 7: readable replacements for the functions that
 * use cmp2 (blocked until the lifter's cmp2 was fixed, e8c5293b9) and for
 * functions newly reached by the batch-7 scenarios.
 * Same rules as rd_b3.c / rd_b6.c (include/rd.h): written from the 68K
 * INSTRUCTIONS, every register a caller could read set exactly as the 68K
 * leaves it, instructions charged in execution order before each poll point
 * (taken backward branch, bsr/jsr, computed jmp). Every entry uses cost 0 and
 * charges its own rts. After a call, registers are re-read from R[]: a callee
 * may change any register its kill mask names.
 * Proven by RR_RD=check (+ fuzz) against the lifted twin.
 */
#include "rd.h"
#include "rr_lifted.h"
#include <stdio.h>

/* (d16,A6): A6 always holds WRAM + 0x8000 */
#define W(off) (0x10008000u + (uint32_t)(int32_t)(off))
static inline uint16_t w16(int32_t off)             { return (uint16_t)vrd16(W(off)); }
static inline uint8_t  w8(int32_t off)              { return (uint8_t)vrd8(W(off)); }
static inline void     w16_set(int32_t off, uint32_t v) { vwr16(W(off), v & 0xFFFFu); }
static inline void     w8_set(int32_t off, uint32_t v)  { vwr8(W(off), v & 0xFFu); }
static inline uint16_t dw(int n) { return (uint16_t)d_reg(n); }

/* cmp2.w <bounds>,Dn: C set when the signed word Dn lies outside
 * [lower, upper] (the two signed words at `bounds`), Z when it equals one */
static int cmp2w_outside(uint32_t bounds, uint16_t v)
{
    int32_t lo = (int16_t)vrd16(bounds), hi = (int16_t)vrd16(bounds + 2), x = (int16_t)v;
    RS1(0x45, x == lo || x == hi);
    int out = !(lo <= x && x <= hi);
    RS1(0x47, out);
    return out;
}

/* ---- WRAM (A6-relative) ---- */
#define TEXT_ATTR        0x0C5E   /* word: attribute (palette) added to printed characters     */
#define MENU_DEFAULT     0x5008   /* word: the factory setting of the row being printed         */

/* sound CPU words */
#define SND_CMD_12       0x60005012u
#define SND_LEVEL_160    0x60005160u
/* the switch inputs as the I/O MCU leaves them in shared RAM (active low) */
#define SWITCHES         0x60004031u

/* ======================================================================== */
/* engine-sound sequencers (states in WRAM 0x57CA / 0x57DC)                   */
/* ======================================================================== */

/* the level written to SND_LEVEL_160 from the value at 0x204C against a
 * per-course base (table `tbl`, indexed by 0x2048 & 3): below the base -> 0,
 * 0xFF above base + 0xFF, else the distance above the base. Shared tail of
 * FUN_000288ac (0x2894C). */
static void level_above(uint32_t tbl)
{
    uint16_t d0 = w16(0x2048) & 3;
    d0 = (uint16_t)vrd16(tbl + (uint32_t)d0 * 2);
    uint16_t d1 = w16(0x204C);
    charge(6);                                          /* move andi move nop move cmp */
    if (!(d1 > d0)) { charge(3); d1 = 0; }              /* bhi, clr, bra */
    else {
        d0 = (uint16_t)(d0 + 0xFF);
        charge(4);                                      /* bhi addi cmp bcs */
        if (d1 < d0) { charge(2); d0 = (uint16_t)(d0 - 0xFF); d1 = (uint16_t)(d1 - d0); }
        else         { charge(2); d1 = 0xFF; }
    }
    set_d16(0, d0); set_d16(1, d1);
    charge(2);                                          /* move.w, rts */
    vwr16(SND_LEVEL_160, d1);
}

/* the same with the distance BELOW the base (0x2898A): at or below the base ->
 * base - value, above base + 0xFF -> 0xFF, else value - base */
static void level_below(uint32_t tbl)
{
    uint16_t d0 = w16(0x2048) & 3;
    d0 = (uint16_t)vrd16(tbl + (uint32_t)d0 * 2);
    uint16_t d1 = w16(0x204C);
    charge(6);
    if (!(d1 > d0)) { charge(4); d1 = (uint16_t)(d0 - d1); }   /* bhi sub neg bra */
    else {
        d0 = (uint16_t)(d0 + 0xFF);
        charge(4);
        if (d1 < d0) { charge(2); d0 = (uint16_t)(d0 - 0xFF); d1 = (uint16_t)(d1 - d0); }
        else         { charge(2); d1 = 0xFF; }
    }
    set_d16(0, d0); set_d16(1, d1);
    charge(2);
    vwr16(SND_LEVEL_160, d1);
}

/* FUN_000288ac: a sound sequence stepped by the state word 0x57CA (0..7,
 * a jump table at 0x288BE):
 *   0  start: command 0x40CE to the sound CPU, next state
 *   1  unless 0x57D8 is set, next state; then the level above the base
 *   2  command cleared, next state
 *   3  when 0x204A + 1 == 0x57CC, 0x57D8 is clear and 0x204C lies in the
 *      per-course window (0x2892E + (0x2048 & 3) * 4): command 0x40CE, next state
 *   4  the level below the base
 *   5  command cleared
 *   6,7  nothing */
static uint32_t rd_288ac(void)
{
    uint16_t st = w16(0x57CA) & 7;
    uint16_t off = (uint16_t)vrd16(0x0288BEu + (uint32_t)st * 2);
    set_d16(0, off);
    charge(5);                                          /* move andi move nop jmp */
    poll();
    switch (0x0288BEu + (uint32_t)(int32_t)(int16_t)off) {
    case 0x0288D0:
        charge(3);
        vwr16(SND_CMD_12, 0x40CE);
        w16_set(0x57CA, w16(0x57CA) + 1);
        return RD_RTS;
    case 0x0288DE:
        charge(2);
        if (w16(0x57D8) == 0) { charge(1); w16_set(0x57CA, w16(0x57CA) + 1); }
        charge(1);                                      /* bra.w */
        level_above(0x028982u);
        return RD_RTS;
    case 0x0288EC:
        charge(3);
        vwr16(SND_CMD_12, 0);
        w16_set(0x57CA, w16(0x57CA) + 1);
        return RD_RTS;
    case 0x0288F8: {
        uint16_t d0 = (uint16_t)(w16(0x204A) + 1);
        set_d16(0, d0);
        charge(4);                                      /* move addq cmp bne */
        if (d0 != w16(0x57CC)) { charge(1); return RD_RTS; }
        charge(2);                                      /* tst bne */
        if (w16(0x57D8) != 0) { charge(1); return RD_RTS; }
        d0 = w16(0x2048) & 3;
        uint16_t d1 = w16(0x204C);
        set_d16(0, d0); set_d16(1, d1);
        charge(6);                                      /* move move andi cmp2 nop bcs */
        if (cmp2w_outside(0x02892Eu + (uint32_t)(int32_t)(int16_t)d0 * 4, d1)) { charge(1); return RD_RTS; }
        charge(3);
        vwr16(SND_CMD_12, 0x40CE);
        w16_set(0x57CA, w16(0x57CA) + 1);
        return RD_RTS;
    }
    case 0x02893E:
        charge(1);                                      /* bra.w */
        level_below(0x0289C2u);
        return RD_RTS;
    case 0x028944:
        charge(2);
        vwr16(SND_CMD_12, 0);
        return RD_RTS;
    default:                                            /* 0x288CE: rts */
        charge(1);
        return RD_RTS;
    }
}

/* FUN_000290ba: a second sequence on the state word 0x57DC (0..3, table at
 * 0x290CC):
 *   0  on course 1 (0x2048 == 1) with 0x57D8 clear and 0x204C inside the
 *      window 0x290F8 (0xC3..0x136): state 1
 *   1  if 0x57D8 is set: flag bit 6 of 0x5880, sound 0x40A7 in 0x57DE,
 *      state 2; else when 0x204C leaves the window: state 3
 *   2  when 0x204C >= 0x8B: flag bit 6 of 0x5880, sound 0x4091, state 0
 *   3  state 0 */
static uint32_t rd_290ba(void)
{
    uint16_t st = w16(0x57DC) & 3;
    uint16_t off = (uint16_t)vrd16(0x0290CCu + (uint32_t)st * 2);
    set_d16(0, off);
    charge(5);
    poll();
    switch (0x0290CCu + (uint32_t)(int32_t)(int16_t)off) {
    case 0x0290D4: {
        charge(2);                                      /* cmpi bne */
        if (w16(0x2048) != 1) { charge(1); return RD_RTS; }
        charge(2);                                      /* tst bne */
        if (w16(0x57D8) != 0) { charge(1); return RD_RTS; }
        uint16_t d0 = w16(0x204C);
        set_d16(0, d0); set_a(0, 0x0290F8u);
        charge(4);                                      /* move lea cmp2 bcs */
        if (cmp2w_outside(0x0290F8u, d0)) { charge(1); return RD_RTS; }
        charge(2);
        w16_set(0x57DC, 1);
        return RD_RTS;
    }
    case 0x0290FC:
        charge(2);                                      /* tst beq */
        if (w16(0x57D8) != 0) {
            charge(4);
            w8_set(0x5880, w8(0x5880) | 0x40);
            w16_set(0x57DE, 0x40A7);
            w16_set(0x57DC, 2);
            return RD_RTS;
        }
        set_a(0, 0x0290F8u);
        set_d16(0, w16(0x204C));
        charge(4);                                      /* lea move cmp2 bcc */
        if (!cmp2w_outside(0x0290F8u, dw(0))) { charge(1); return RD_RTS; }
        charge(2);
        w16_set(0x57DC, 3);
        return RD_RTS;
    case 0x02912C:
        charge(2);                                      /* cmpi blt */
        if ((int16_t)w16(0x204C) < 0x8B) { charge(1); return RD_RTS; }
        charge(4);
        w8_set(0x5880, w8(0x5880) | 0x40);
        w16_set(0x57DE, 0x4091);
        w16_set(0x57DC, 0);
        return RD_RTS;
    default:                                            /* 0x29146 */
        charge(2);
        w16_set(0x57DC, 0);
        return RD_RTS;
    }
}

/* ======================================================================== */
/* test mode                                                                  */
/* ======================================================================== */

/* one switch row of FUN_0002f5cc: print at display position `pos` the text
 * 0x2F9E0 (attribute 0xD000) or, when the switch bit is pressed, 0x2F9E4
 * (attribute 0xC000); `last` = tail jump to the printer */
static uint32_t switch_row(uint32_t pos, int bit, uint32_t ret)
{
    set_a(0, pos);
    w16_set(TEXT_ATTR, 0xD000);
    set_a(1, 0x02F9E0u);
    charge(5);                                          /* lea move lea btst beq */
    if (vrd8(SWITCHES) & (1u << bit)) {
        charge(2);
        w16_set(TEXT_ATTR, 0xC000);
        set_a(1, 0x02F9E4u);
    }
    charge(1);                                          /* jsr / jmp */
    if (!ret) return RD_JMP(0x005A0Au);
    return rd_call(L_5A0A, ret);
}

/* print a fixed text (lea (d,PC),A1 ; jsr printer) */
static uint32_t print_at(uint32_t a1, uint32_t ret)
{
    charge(2);
    set_a(1, a1);
    return rd_call(L_5A0A, ret);
}

/* FUN_0002f5cc: a test-mode page: two option rows (the byte at 0x10001071 with
 * its factory value from 0x2B825, and when that byte is 0 the word 0x57B8),
 * a range check of 0xDD2 (window 0x2F790, or 0x2F79C when 0xDDE is set) whose
 * result goes to FUN_0002ef64 in D5, the switch state as text, and the rows of
 * the individual switches (four, or two when 0xDDE is set). */
static uint32_t rd_2f5cc(void)
{
    uint32_t r;
    /* row 1 */
    set_a(2, 0x10001071u);
    set_a(3, 0x02B825u);
    set_d(4, 1);
    set_d8(5, (uint8_t)vrd8(0x10001071u));
    set_d(0, (uint8_t)vrd8(0x02B825u));
    w16_set(MENU_DEFAULT, dw(0));
    charge(8);
    if ((r = rd_call(L_29C68, 0x02F5EA))) return r;
    set_a(1, 0x02F9F2u);
    set_d16(5, dw(5) & 1);
    charge(3);                                          /* lea andi beq */
    if (dw(5)) { charge(1); set_a(1, 0x02F9F9u); }
    charge(1);
    if ((r = rd_call(L_5A0A, 0x02F5FE))) return r;
    charge(2);                                          /* tst.b beq */
    if (vrd8(a_reg(2))) {
        charge(2);
        set_a(1, 0x02FA0Eu);
        if ((r = rd_call(L_5A0A, 0x02F60C))) return r;
        charge(1);                                      /* bra */
    } else {
        set_d(4, 2);
        set_d16(5, w16(0x57B8));
        w16_set(MENU_DEFAULT, 1);
        charge(4);
        if ((r = rd_call(L_29C68, 0x02F620))) return r;
        set_a(1, 0x02FA00u);
        set_d16(5, dw(5) & 1);
        charge(3);
        if (dw(5)) { charge(1); set_a(1, 0x02FA07u); }
        charge(1);
        if ((r = rd_call(L_5A0A, 0x02F634))) return r;
    }
    if ((r = switch_row(0x9009E498u, 6, 0x02F65E))) return r;
    /* the range check */
    set_d16(0, w16(0x0DD2));
    set_d(5, 1);
    set_a(0, 0x02F790u);
    charge(5);                                          /* move moveq lea tst beq */
    if (w16(0x0DDE)) { charge(1); set_a(0, 0x02F79Cu); }
    charge(2);                                          /* cmp2 bcc */
    if (cmp2w_outside(a_reg(0), dw(0))) { charge(1); set_d(5, 0); }
    set_a(0, 0x9009E598u);
    charge(2);
    if ((r = rd_call(L_2EF64, 0x02F686))) return r;
    /* the switch state */
    uint8_t sw = (uint8_t)~vrd8(SWITCHES);
    set_d(0, d_reg(0) & 0xFFFF0000u);
    set_d8(0, sw);
    charge(5);                                          /* clr move not tst bne */
    if (w16(0x0DDE) == 0) {
        set_d8(0, sw & 0x0F);
        set_a(1, 0x02F994u);
        set_d8(5, (uint8_t)vrd8(0x02F994u + (sw & 0x0F)));
        set_a(1, 0x02F9A4u);
        charge(6);                                      /* andi lea move lea bra jsr */
    } else {
        set_d8(0, sw & 0x03);
        set_a(1, vrd32(0x02F9ACu + (uint32_t)(sw & 0x03) * 4));
        charge(3);
    }
    if ((r = rd_call(L_5A0A, 0x02F6B8))) return r;
    if ((r = print_at(0x02F960u, 0x02F6C2))) return r;
    if ((r = switch_row(0x9009E6B8u, 0, 0x02F6EC))) return r;
    if ((r = print_at(0x02F96Du, 0x02F6F6))) return r;
    if ((r = switch_row(0x9009E738u, 1, 0x02F720))) return r;
    charge(2);                                          /* tst bne */
    if (w16(0x0DDE)) { charge(1); return RD_RTS; }
    if ((r = print_at(0x02F97Au, 0x02F730))) return r;
    if ((r = switch_row(0x9009E7B8u, 2, 0x02F75A))) return r;
    if ((r = print_at(0x02F98Bu, 0x02F764))) return r;
    return switch_row(0x9009E838u, 3, 0);
}

/* ======================================================================== */
/* small helpers reached by the batch-7 scenarios                            */
/* ======================================================================== */

#define TEXT_RAM   0x9009E000u   /* the text layer: 0x1000 words              */
#define flags_w(v) rd_flags_nzvc(((v) & 0x8000u) != 0, ((v) & 0xFFFFu) == 0, 0, 0)
#define flags_l(v) rd_flags_nzvc(((v) & 0x80000000u) != 0, (v) == 0, 0, 0)
#define flags_b(v) rd_flags_nzvc(((v) & 0x80u) != 0, ((v) & 0xFFu) == 0, 0, 0)

/* FUN_000045a6: rts (a stub the reset path calls) */
static uint32_t rd_45a6(void) { charge(1); return RD_RTS; }

/* FUN_00028724: sound command 0x40CD */
static uint32_t rd_28724(void)
{
    charge(2);
    vwr16(SND_CMD_12, 0x40CD);
    flags_w(0x40CDu);
    return RD_RTS;
}

/* FUN_00004624: three init steps of the reset path, in order */
static uint32_t rd_4624(void)
{
    uint32_t r;
    charge(1); if ((r = rd_call(L_4640, 0x004628))) return r;
    charge(1); if ((r = rd_call(L_47D0, 0x00462C))) return r;
    charge(1); if ((r = rd_call(L_494C, 0x004630))) return r;
    charge(1);
    return RD_RTS;
}

/* FUN_00026b12: unless the byte at 0x2002000B is set, on to FUN_00026c84 */
static uint32_t rd_26b12(void)
{
    uint8_t b = (uint8_t)vrd8(0x2002000Bu);
    charge(2);
    flags_b(b);
    charge(1);
    if (b) return RD_RTS;
    return 0x026C84;                                    /* bra.w */
}

/* FUN_000043da: cache on (CACR = 3), sound word 0x5000 and the WRAM word
 * -0x77FE cleared */
static uint32_t rd_43da(void)
{
    charge(5);
    set_d(0, 3);
    RS4(0x10C, 3);                                      /* movec D0,CACR */
    vwr16(0x60005000u, 0);
    w16_set(-0x77FE, 0);
    flags_w(0u);
    return RD_RTS;
}

/* fill `n+1` words of the text layer with `v` from TEXT_RAM (a dbf loop):
 * the pointer and the counter are live at every poll */
static void text_fill(int ptr_reg, int cnt_reg, uint16_t v, uint16_t n)
{
    uint32_t a = TEXT_RAM;
    for (uint16_t k = n; ; k--) {
        vwr16(a, v); a += 2;
        charge(2);
        set_a(ptr_reg, a); set_d16(cnt_reg, (uint16_t)(k - 1));
        if (k == 0) break;
        poll();
    }
    flags_w(v);
}

/* FUN_00006524: the text layer cleared to spaces (A0, D0) */
static uint32_t rd_6524(void)
{
    charge(2);
    set_a(0, TEXT_RAM); set_d16(0, 0x0FFF);
    text_fill(0, 0, 0x20, 0x0FFF);
    charge(1);
    return RD_RTS;
}

/* FUN_00005d7c: the text layer cleared to spaces (A1, D1; D0w = 0x20) */
static uint32_t rd_5d7c(void)
{
    charge(3);
    set_d16(1, 0x0FFF); set_a(1, TEXT_RAM); set_d16(0, 0x20);
    text_fill(1, 1, 0x20, 0x0FFF);
    charge(1);
    return RD_RTS;
}

/* FUN_00029e78: WRAM 0x4344 = -1; output bit 3 (0x60004020) off */
static uint32_t rd_29e78(void)
{
    w16_set(0x4344, 0xFFFF);
    uint16_t v = (uint16_t)vrd16(0x60004020u);
    v &= (uint16_t)~8u;                                /* bclr.l #3 */
    set_d16(0, v);
    vwr16(0x60004020u, v);
    flags_w(v);
    charge(5);
    return RD_RTS;
}

/* FUN_000060ce: 0x200 longs at 0x10002800 = 0x00FF00FF; WRAM 0x110A = -1 */
static uint32_t rd_60ce(void)
{
    uint32_t a = 0x10002800u;
    charge(3);
    set_a(1, a); set_d(0, 0x00FF00FFu); set_d16(1, 0x01FF);
    for (uint16_t k = 0x1FF; ; k--) {
        vwr32(a, 0x00FF00FFu); a += 4;
        charge(2);
        set_a(1, a); set_d16(1, (uint16_t)(k - 1));
        if (k == 0) break;
        poll();
    }
    charge(2);
    w16_set(0x110A, 0xFFFF);
    flags_w(0xFFFFu);
    return RD_RTS;
}

/* FUN_0001e3b8: the lap-time table 0x48F4 (15 longs) summed into the 16th */
static uint32_t rd_1e3b8(void)
{
    uint32_t a = W(0x48F4), sum = 0;
    charge(3);
    set_a(0, a); set_d(0, 0); set_d16(7, 0x0E);
    for (uint16_t k = 0x0E; ; k--) {
        uint32_t v = vrd32(a); a += 4;
        uint64_t t = (uint64_t)sum + v;
        uint32_t n = (uint32_t)t;
        RS1(0x47, (uint8_t)(t >> 32)); R[RR_XF] = (uint8_t)(t >> 32);
        RS1(0x46, (uint8_t)((~(sum ^ v) & (sum ^ n)) >> 31));
        sum = n;
        charge(2);
        set_a(0, a); set_d(0, sum); set_d16(7, (uint16_t)(k - 1));
        if (k == 0) break;
        poll();
    }
    vwr32(a, sum);
    charge(2);
    flags_l(sum);
    return RD_RTS;
}

/* FUN_0001e5a4: at the lap line, solo: position 1 -> 0x11A4 = WRAM 0x4930,
 * 0x48D8 = 8; not first -> FUN_0001e69e; linked (0x1100 set) -> 0x1E6A8 (rts) */
static uint32_t rd_1e5a4(void)
{
    uint16_t lk = w16(0x1100);
    charge(2);
    flags_w(lk);
    if (lk) return 0x01E6A8;
    uint16_t pos = w16(0x204E);
    charge(2);
    rd_flags_cmp16(pos, 1);
    if (pos != 1) return 0x01E69E;
    vwr32(W(0x11A4), vrd32(W(0x4930)));
    w16_set(0x48D8, 8);
    charge(3);
    flags_w(8u);
    return RD_RTS;
}

/* FUN_0002d32a: the three colour banks' byte 0xFFEE = 0, byte 0xFFE8 = 0xFF */
static uint32_t rd_2d32a(void)
{
    static const uint32_t bank[3] = { 0x9002FFE0u, 0x90037FE0u, 0x9003FFE0u };
    for (int i = 0; i < 3; i++) vwr8(bank[i] + 0x0E, 0);
    for (int i = 0; i < 3; i++) vwr8(bank[i] + 0x08, 0xFF);
    charge(7);
    flags_b(0xFFu);
    return RD_RTS;
}

/* FUN_000064fc: 0x1000 bytes of ROM from 0x6518 copied, one byte per
 * bank, into the three RAMs at 0x90028000 / 0x90030000 / 0x90038000 */
static uint32_t rd_64fc(void)
{
    uint32_t a0 = 0x90028000u, a1 = 0x90030000u, a2 = 0x90038000u, a3 = 0x006518u;
    charge(6);
    set_a(0, a0); set_a(1, a1); set_a(2, a2); set_a(3, a3); set_d16(0, 0x0FFF);
    for (uint16_t k = 0x0FFF; ; k--) {
        uint8_t b;
        b = (uint8_t)vrd8(a3++); vwr8(a0++, b);
        b = (uint8_t)vrd8(a3++); vwr8(a1++, b);
        b = (uint8_t)vrd8(a3++); vwr8(a2++, b);
        flags_b(b);
        charge(4);
        set_a(0, a0); set_a(1, a1); set_a(2, a2); set_a(3, a3); set_d16(0, (uint16_t)(k - 1));
        if (k == 0) break;
        poll();
    }
    charge(1);
    return RD_RTS;
}

/* FUN_000297b6: push D1w onto the 8-entry word queue at 0x586E (count 0x586C,
 * which saturates at 8) */
static uint32_t rd_297b6(void)
{
    uint16_t n = w16(0x586C);
    charge(2);
    if ((int16_t)n >= 8) {
        charge(2);
        w16_set(0x586C, 8);
        flags_w(8u);
        charge(1);
        return RD_RTS;
    }
    set_a(0, W(0x586E));
    set_d16(0, n);
    vwr16(W(0x586E) + (uint32_t)(int16_t)n * 2, dw(1));
    uint16_t c = w16(0x586C);
    charge(5);
    rd_flags_cmp16(c, 8);
    if (c != 8) { charge(1); w16_set(0x586C, (uint16_t)(c + 1)); rd_flags_nzvc(((uint16_t)(c + 1) & 0x8000) != 0, (uint16_t)(c + 1) == 0, (c + 1) == 0x8000, 0); R[RR_XF] = 0; }
    charge(1);
    return RD_RTS;
}

/* FUN_000060ec: once after the switch test (0x110A set): WRAM 0x1038 = the
 * board ID byte + 1 (1 above 0xFD), then FUN_00026b12; 0x110A cleared */
static uint32_t rd_60ec(void)
{
    uint16_t f = w16(0x110A);
    charge(2);
    flags_w(f);
    if (!f) { charge(1); return RD_RTS; }
    uint16_t d0 = (uint16_t)((vrd16(0x10002000u) & 0xFF) + 1);
    charge(5);
    if ((int16_t)d0 > 0xFD) { charge(1); set_d(0, 1); d0 = 1; }
    else set_d16(0, d0);
    w16_set(0x1038, d0);
    charge(2);
    uint32_t r;
    if ((r = rd_call(L_26B12, 0x006110))) return r;
    charge(2);
    w16_set(0x110A, 0);
    flags_w(0u);
    return RD_RTS;
}

/* ======================================================================== */
/* lap results (RIDGE RACER SHORT, the lap scenario)                         */
/* ======================================================================== */

#define PRINT 0x005A0Au   /* the text interpreter: string at A1 */

/* FUN_0001e3fc: the lap-time list -- a heading (0x1E45A), then one line per
 * lap from the oldest shown (0x48F2 - 7 laps back, if more than seven):
 * the label 0x1E48C (0x1E48F for the best lap, bit (index/4) of 0x49BE),
 * the time from 0x48F4 formatted by FUN_0000a8c8 into D6, and the tail
 * 0x1E48A.  0x49D4 counts the lines down from 0x48EE - 4 by 4. */
static uint32_t rd_1e3fc(void)
{
    uint32_t r;
    charge(2); set_a(1, 0x01E45A);
    if ((r = rd_call(L_5A0A, 0x01E406))) return r;
    uint16_t d0 = (uint16_t)(w16(0x48EE) - 4);
    set_d16(0, d0);
    w16_set(0x49D4, d0);
    set_d(4, 0);
    d0 = (uint16_t)(w16(0x48F2) - 7);
    set_d16(0, d0);
    charge(7);                                          /* move subq move moveq move subq bmi */
    if (!(d0 & 0x8000)) {
        d0 = (uint16_t)(d0 << 2);
        set_d16(0, d0); set_d16(4, d0);
        charge(2);
    }
    for (;;) {
        uint16_t d4 = dw(4);
        uint32_t a1 = 0x01E48C;
        uint16_t i = (uint16_t)(d4 >> 2);
        uint16_t d1 = w16(0x49BE);
        set_d16(0, i); set_d16(1, d1);
        charge(6);                                      /* lea move lsr move btst beq */
        if (((d_reg(1) & 0xFFFF0000u) | d1) & (1u << (i & 31))) { charge(1); a1 = 0x01E48F; }   /* btst.l */
        set_a(1, a1);
        charge(1);
        if ((r = rd_call(L_5A0A, 0x01E438))) return r;
        d4 = dw(4);
        set_d(1, vrd32(W(0x48F4) + (uint32_t)(int16_t)d4));
        charge(2);
        if ((r = rd_call(L_A8C8, 0x01E444))) return r;
        set_d(6, d_reg(0));
        set_a(1, 0x01E48A);
        charge(3);
        if ((r = rd_call(L_5A0A, 0x01E450))) return r;
        set_d16(4, (uint16_t)(dw(4) + 4));
        uint16_t n = (uint16_t)(w16(0x49D4) - 4);
        w16_set(0x49D4, n);
        charge(3);
        if (n & 0x8000) break;
        poll();
    }
    charge(1);
    return RD_RTS;
}

/* FUN_0001e62c: solo only (0x1100 clear, else the rts at 0x1E6A8): a heading
 * (0x1E67E), then per lap a two-character mark at A0 (0xD1AA 0xD1AB for the
 * best lap, bit n of 0x49BE, else two spaces) and a print of the text at A1
 * as the interpreter left it */
static uint32_t rd_1e62c(void)
{
    uint32_t r;
    uint16_t lk = w16(0x1100);
    charge(2);
    flags_w(lk);
    if (lk) { charge(1); return RD_RTS; }             /* bne.w 0x1E6A8: rts */
    set_a(1, 0x01E67E);
    charge(2);
    if ((r = rd_call(L_5A0A, 0x01E63E))) return r;
    uint16_t d0 = (uint16_t)(w16(0x48EE) - 4);
    set_d16(0, d0);
    w16_set(0x49D4, d0);
    set_d(4, 0);
    d0 = (uint16_t)(w16(0x48F2) - 7);
    set_d16(0, d0);
    charge(7);
    if (!(d0 & 0x8000)) { charge(1); set_d16(4, d0); }
    for (;;) {
        uint16_t d4 = dw(4), d1 = w16(0x49BE);
        uint32_t a0 = a_reg(0);
        set_d16(1, d1);
        charge(3);                                      /* move btst beq */
        if (((d_reg(1) & 0xFFFF0000u) | d1) & (1u << (d4 & 31))) {      /* btst.l */
            vwr16(a0, 0xD1AA); vwr16(a0 + 2, 0xD1AB);
            charge(3);                                  /* move move bra */
        } else {
            vwr16(a0, 0x0020); vwr16(a0 + 2, 0x0020);
            charge(2);
        }
        set_a(0, a0 + 4);
        charge(1);
        if ((r = rd_call(L_5A0A, 0x01E674))) return r;
        set_d16(4, (uint16_t)(dw(4) + 1));
        uint16_t n = (uint16_t)(w16(0x49D4) - 4);
        w16_set(0x49D4, n);
        charge(3);
        if (n & 0x8000) break;
        poll();
    }
    charge(1);
    return RD_RTS;
}

/* FUN_000297e0: pop the word queue at 0x586E -- the head goes to sound word
 * 0x6000501E if it lies in the range at 0x29818 (else the head is dropped),
 * the other seven move down one, the count 0x586C drops by one if not 0 */
static uint32_t rd_297e0(void)
{
    uint32_t q = W(0x586E);
    uint16_t head = (uint16_t)vrd16(q);
    set_a(0, q); set_a(1, 0x029818); set_d16(0, head);
    charge(5);                                          /* lea lea move cmp2 bcc */
    if (cmp2w_outside(0x029818, head)) { charge(2); vwr16(q, 0); }
    else { charge(1); vwr16(0x6000501Eu, head); }
    set_d(1, 1);
    charge(1);
    for (uint16_t d1 = 1; ; ) {
        vwr16(q + (uint32_t)(d1 - 1) * 2, (uint16_t)vrd16(q + (uint32_t)d1 * 2));
        d1++;
        set_d16(1, d1);
        charge(4);
        rd_flags_cmp16(d1, 8);
        if (!((int16_t)d1 < 8)) break;
        poll();
    }
    uint16_t n = w16(0x586C);
    charge(2);
    flags_w(n);
    if (n) { charge(1); n--; w16_set(0x586C, n); rd_flags_nzvc((n & 0x8000) != 0, n == 0, n == 0x7FFF, 0); R[RR_XF] = 0; }
    charge(1);
    return RD_RTS;
}

/* ======================================================================== */
/* test mode: MONITOR TEST patterns, BOARD TEST                              */
/* ======================================================================== */

/* one word `v` written n+1 times at A1 by `move.w D1w,(A1)+ ; dbf D0w`
 * (D0w the counter), A1 and D0 live at every poll */
static uint32_t fill_d1(uint32_t a1, uint16_t v, uint16_t n)
{
    for (uint16_t k = n; ; k--) {
        vwr16(a1, v); a1 += 2;
        charge(2);
        set_a(1, a1); set_d16(0, (uint16_t)(k - 1));
        if (k == 0) break;
        poll();
    }
    return a1;
}

/* one 40-cell text row of the frame drawn by FUN_0002d1da: `first`, 18 x
 * `fill`, `mid1` `mid2`, 18 x `fill`, `last` (no increment for the very last
 * row), then A1 on to the next row */
static uint32_t frame_row(uint32_t a1, uint16_t first, uint16_t fill, uint16_t mid1,
                          uint16_t mid2, uint16_t last, int last_inc)
{
    vwr16(a1, first); a1 += 2;
    set_a(1, a1); set_d16(1, fill); set_d16(0, 0x11);
    charge(3);
    a1 = fill_d1(a1, fill, 0x11);
    vwr16(a1, mid1); a1 += 2;
    vwr16(a1, mid2); a1 += 2;
    set_a(1, a1); set_d16(0, 0x11);
    charge(3);
    a1 = fill_d1(a1, fill, 0x11);
    vwr16(a1, last); if (last_inc) a1 += 2;
    a1 += 0x30;
    set_a(1, a1);
    charge(2);
    return a1;
}

/* FUN_0002d1da: MONITOR TEST, the crosshatch/frame pattern: colour bank
 * bytes, then the text layer filled with frame cells -- a top edge, 13
 * inner rows, a middle band of two rows, 13 inner rows, a bottom edge */
static uint32_t rd_2d1da(void)
{
    vwr8(0x9002FFEEu, 0x00); vwr8(0x90037FEEu, 0x69); vwr8(0x9003FFEEu, 0x00);
    vwr8(0x9002FFE8u, 0x00); vwr8(0x90037FE8u, 0x00); vwr8(0x9003FFE8u, 0x00);
    uint32_t a1 = TEXT_RAM;
    set_a(1, a1);
    charge(7);
    a1 = frame_row(a1, 0xE003, 0xE004, 0xE007, 0xE407, 0xE403, 1);
    charge(1); set_d16(2, 0x0C);
    for (uint16_t k = 0x0C; ; k--) {
        a1 = frame_row(a1, 0xE005, 0xE00B, 0xE009, 0xE409, 0xE405, 1);
        charge(1); set_d16(2, (uint16_t)(k - 1));
        if (k == 0) break;
        poll();
    }
    a1 = frame_row(a1, 0xE006, 0xE00A, 0xE008, 0xE408, 0xE406, 1);
    a1 = frame_row(a1, 0xE806, 0xE80A, 0xE808, 0xEC08, 0xEC06, 1);
    charge(1); set_d16(2, 0x0C);
    for (uint16_t k = 0x0C; ; k--) {
        a1 = frame_row(a1, 0xE005, 0xE00B, 0xE009, 0xE409, 0xE405, 1);
        charge(1); set_d16(2, (uint16_t)(k - 1));
        if (k == 0) break;
        poll();
    }
    a1 = frame_row(a1, 0xE803, 0xE804, 0xE807, 0xEC07, 0xEC03, 0);
    flags_w(0xEC03u);
    charge(1);
    return RD_RTS;
}

/* FUN_0002d1b2: five 4x4-cell blocks (positions from the table at 0x2D4B6)
 * of 0xE001 0xE001 0xE002 0xE002 per row, rows 0x80 bytes apart */
static uint32_t rd_2d1b2(void)
{
    uint32_t a0 = 0x02D4B6;
    set_d16(1, 0xE001); set_d16(2, 0xE002); set_a(0, a0); set_d(3, 4);
    charge(4);
    for (uint16_t j = 4; ; j--) {
        uint32_t a1 = vrd32(a0); a0 += 4;
        set_a(0, a0); set_a(1, a1); set_d(0, 3);
        charge(2);
        for (uint16_t k = 3; ; k--) {
            vwr16(a1, 0xE001); vwr16(a1 + 2, 0xE001); vwr16(a1 + 4, 0xE002); vwr16(a1 + 6, 0xE002);
            a1 += 8 + 0x78;
            charge(6);
            set_a(1, a1); set_d16(0, (uint16_t)(k - 1));
            if (k == 0) break;
            poll();
        }
        charge(1);
        set_d16(3, (uint16_t)(j - 1));
        if (j == 0) break;
        poll();
    }
    flags_w(0xE002u);
    charge(1);
    return RD_RTS;
}

/* FUN_0002d35c: MONITOR TEST, a full screen of cell 0xE00B (30 rows of 40),
 * colour bank bytes 0xFFEE = 0xFF */
static uint32_t rd_2d35c(void)
{
    vwr8(0x9002FFEEu, 0xFF); vwr8(0x90037FEEu, 0xFF); vwr8(0x9003FFEEu, 0xFF);
    uint32_t a1 = TEXT_RAM;
    set_a(1, a1); set_d16(1, 0xE00B); set_d16(2, 0x1D);
    charge(6);
    for (uint16_t j = 0x1D; ; j--) {
        set_d16(0, 0x27);
        charge(1);
        a1 = fill_d1(a1, 0xE00B, 0x27);
        a1 += 0x30;
        set_a(1, a1);
        charge(2);
        set_d16(2, (uint16_t)(j - 1));
        if (j == 0) break;
        poll();
    }
    flags_w(0xE00Bu);
    charge(1);
    return RD_RTS;
}

/* FUN_00005f3c: BOARD TEST -> I/O BOARD, set-up: the board ID area filled
 * (FUN_000060ce), text cleared, two titles printed, the two DIP bytes kept
 * in WRAM 0x1000 / 0x1001, output word 0x60004020 = 1 */
static uint32_t rd_5f3c(void)
{
    uint32_t r;
    set_a(6, 0x10008000u);
    charge(2); if ((r = rd_call(L_60CE, 0x005F46))) return r;
    charge(1); if ((r = rd_call(L_5D7C, 0x005F4A))) return r;
    set_a(1, 0x005F7A);
    charge(2); if ((r = rd_call(L_5A0A, 0x005F52))) return r;
    set_a(1, 0x006083);
    charge(2); if ((r = rd_call(L_5A0A, 0x005F5A))) return r;
    uint8_t d0 = (uint8_t)(vrd8(0x60004009u) & 0x3F);
    set_d8(0, d0);
    w8_set(0x1000, d0);
    w8_set(0x1001, (uint8_t)vrd8(0x60004007u));
    vwr16(0x60004020u, 1);
    flags_w(1u);
    charge(6);
    return RD_RTS;
}

/* FUN_00026c84: the next request bit of 0x1034 from 0x1036 on (16 tries):
 * its word from the table at 0x103C (or 0x26D1A for a linked cabinet, bit
 * of 0x1162, not this one 0x11A0) to 0x103A, the bit cleared; then the
 * request is sent: 0x2002000E = 0x103A, 0x20020006 = 1, 0x2002000A =
 * 0x1038 + 2 with 0x1038 = 0x26, 0x20020002 &= 8, 0x20020006 = 0 */
static uint32_t rd_26c84(void)
{
    uint16_t d4 = w16(0x1036), d2 = dw(2);
    uint32_t d0 = (d_reg(0) & 0xFFFF0000u) | w16(0x1034);   /* btst.l / bclr.l see all 32 bits */
    set_d(6, 0xF); set_d(5, 0xF); set_d16(4, d4); set_d(0, d0);
    charge(4);
    for (uint16_t k = 0xF; ; ) {
        charge(2);                                      /* btst beq */
        if (d0 & (1u << (d4 & 31))) {
            d2 = (uint16_t)(0x3000 + d4);
            w16_set(0x103A, 0);
            charge(5);                                  /* move add move cmp beq */
            if (d4 != w16(0x11A0)) {
                d2 = w16(0x1162);
                charge(2);                              /* move beq */
                int linked = 0;
                if (d2) {
                    charge(2);                          /* btst.l: bit 16..31 is D2's own high word */
                    if (((d_reg(2) & 0xFFFF0000u) | d2) & (1u << (d4 & 31))) linked = 1;
                }
                if (linked) {
                    d2 = (uint16_t)(d4 - 8);
                    w16_set(0x103A, (uint16_t)vrd16(0x026D1Au + (uint32_t)(int16_t)d2 * 2));
                    charge(5);                          /* move subq move nop bra */
                } else {
                    w16_set(0x103A, (uint16_t)vrd16(W(0x103C) + (uint32_t)(int16_t)d4 * 2));
                    charge(1);
                }
            }
            d0 &= ~(1u << (d4 & 31));
            d4 = (uint16_t)((d4 + 1) & 0xF);
            charge(4);                                  /* bclr addq and bra */
            set_d16(2, d2); set_d(0, d0); set_d16(4, d4);
            break;
        }
        d4 = (uint16_t)((d4 + 1) & 0xF);
        charge(3);                                      /* addq and dbf */
        set_d16(4, d4); set_d16(6, (uint16_t)(k - 1));
        if (k == 0) break;
        k--;
        poll();
    }
    w16_set(0x1038, 0x26);
    w16_set(0x1036, d4);
    w16_set(0x1034, (uint16_t)d0);
    d4 = w16(0x103A);
    set_d16(4, d4);
    vwr16(0x2002000Eu, d4);
    set_d16(0, 1);
    vwr16(0x20020006u, 1);
    d2 = (uint16_t)(w16(0x1038) + 2);
    set_d16(2, d2);
    vwr16(0x2002000Au, d2);
    uint16_t c = (uint16_t)(vrd16(0x20020002u) & 8);
    vwr16(0x20020002u, c);
    vwr16(0x20020006u, 0);
    flags_w(0u);
    charge(14);
    return RD_RTS;
}

/* FUN_00006118: BOARD TEST -> I/O BOARD, one frame: the 16 switch bits of
 * 0x60004030 drawn as cells (0xF02A, 0xE02A when set) right to left before
 * 0x9009E2B4; the ADC/DIP/ID values printed (D5 each, strings 0x62B5..0x62E5);
 * the lamp cursor 0xDAE moved by 0xDB0 (0..5, wrapping), the lamp under it
 * lit (0x60004020 bit) while a start/view switch and the edge bit 7 of
 * -0x77F2 are down; the six lamp names printed, the current one in 0xC000 /
 * 0xE000 (lit) and the rest in 0xC000 */
static uint32_t rd_6118(void)
{
    uint32_t r;
    set_a(6, 0x10008000u);
    charge(2);
    if ((r = rd_call(L_60EC, 0x006120))) return r;
    uint32_t a0 = 0x9009E2B4u;
    uint16_t d0 = 0x2A, d1 = (uint16_t)vrd16(0x60004030u);
    set_a(0, a0); set_d16(0, d0); set_d16(1, d1); set_d(2, 0xF);
    charge(4);
    for (uint16_t k = 0xF; ; k--) {
        d0 |= 0xF000;
        charge(3);                                      /* ori btst beq */
        if (d1 & (1u << (k & 31))) { charge(1); d0 &= 0xE0FF; }
        a0 -= 2; vwr16(a0, d0);
        charge(2);                                      /* move dbf */
        set_a(0, a0); set_d16(0, d0); set_d16(2, (uint16_t)(k - 1));
        if (k == 0) break;
        poll();
    }
    vwr32(W(0x0C4E), 0x60004032u);
    set_a(1, 0x0062B5);
    charge(3);
    if ((r = rd_call(L_5A0A, 0x006154))) return r;
    uint16_t d5;
    d5 = (uint16_t)(vrd16(0x60004006u) & 0xFF);
    d5 = (uint16_t)((d5 & 0xFF00) | (uint8_t)((uint8_t)d5 - w8(0x1001)));
    set_d16(5, d5); set_a(1, 0x0062BB);
    charge(5);
    if ((r = rd_call(L_5A0A, 0x00616A))) return r;
    d5 = (uint16_t)(vrd16(0x60004008u) & 0x3F);
    d5 = (uint16_t)((d5 & 0xFF00) | (uint8_t)((uint8_t)d5 - w8(0x1000)));
    set_d16(5, d5); set_a(1, 0x0062C1);
    charge(5);
    if ((r = rd_call(L_5A0A, 0x006180))) return r;
    set_d16(5, w16(0x1008)); set_a(1, 0x0062C7);
    charge(3);
    if ((r = rd_call(L_5A0A, 0x00618C))) return r;
    set_d16(5, w16(0x100A)); set_a(1, 0x0062CD);
    charge(3);
    if ((r = rd_call(L_5A0A, 0x006198))) return r;
    set_d16(5, w16(0x1002)); set_a(1, 0x0062D3);
    charge(3);
    if ((r = rd_call(L_5A0A, 0x0061A4))) return r;
    set_d16(5, (uint16_t)vrd16(0x10002000u)); set_a(1, 0x0062D9);
    charge(3);
    if ((r = rd_call(L_5A0A, 0x0061B2))) return r;
    set_a(1, 0x0062DF);
    set_d8(5, (uint8_t)vrd8(0x6000403Au)); set_d16(5, dw(5) & 0xFF);
    charge(4);
    if ((r = rd_call(L_5A0A, 0x0061C4))) return r;
    set_a(1, 0x0062E5);
    set_d8(5, (uint8_t)vrd8(0x6000403Bu)); set_d16(5, dw(5) & 0xFF);
    charge(4);
    if ((r = rd_call(L_5A0A, 0x0061D6))) return r;
    /* the lamp cursor */
    d1 = w16(0x0DAE);
    set_d16(2, d1);
    d0 = w16(0x0DB0);
    charge(4);                                          /* move move move bmi */
    if (!(d0 & 0x8000)) {
        d1 = (uint16_t)(d1 + d0);
        charge(3);                                      /* add cmpi ble */
        if ((int16_t)d1 > 5) {
            set_d(1, 0); set_d(0, 0); d1 = 0; d0 = 0;
            charge(2);
            d1 = (uint16_t)(d1 + d0);
            charge(2);                                  /* add bpl (taken) */
        }
    } else {
        d1 = (uint16_t)(d1 + d0);
        charge(2);                                      /* add bpl */
        if (d1 & 0x8000) { charge(1); d1 = 5; }
    }
    set_d16(0, d0); set_d16(1, d1);
    w16_set(0x0DAE, d1);
    set_d(0, 0);
    uint32_t lamps = 0;
    uint8_t sw = (uint8_t)(~vrd8(0x60004031u) & 3);
    set_d8(1, sw);
    charge(6);                                          /* move moveq move not andi beq */
    if (sw) {
        uint8_t e = (uint8_t)(w8(-0x77F2) & 0x80);
        set_d8(1, e);
        charge(3);                                      /* move andi beq */
        if (e) {
            uint16_t c = w16(0x0DAE);
            set_d16(1, c);
            lamps = 1u << (c & 31);
            set_d(0, lamps);
            w16_set(0x0DBE, 0xD000);
            charge(3);
        }
    }
    vwr16(0x60004020u, (uint16_t)lamps);
    set_a(1, 0x0062A0);
    charge(3);                                          /* move lea bsr */
    if ((r = rd_call(L_5A0A, 0x00622E))) return r;
    set_a(4, 0x00626A);
    set_d(4, 0);
    set_d16(5, (uint16_t)vrd16(0x60004020u));
    charge(3);
    for (;;) {
        uint16_t d4 = dw(4), c = 0xC000;
        set_d16(1, c);
        charge(3);                                      /* move cmp bne */
        if (d4 == w16(0x0DAE)) {
            c = w16(0x0DBE);
            set_d16(1, c);
            charge(3);                                  /* move btst beq */
            if (dw(5) & (1u << (d4 & 31))) { charge(1); c = 0xE000; set_d16(1, c); }
        }
        w16_set(0x0C5E, c);
        uint32_t a4 = a_reg(4);
        set_a(1, vrd32(a4)); set_a(4, a4 + 4);
        charge(3);                                      /* move movea bsr */
        if ((r = rd_call(L_5A0A, 0x00625A))) return r;
        d4 = (uint16_t)(dw(4) + 1);
        set_d16(4, d4);
        charge(3);                                      /* addq cmpi bne */
        rd_flags_cmp16(d4, 6);
        if (d4 == 6) break;
        poll();
    }
    w16_set(0x0C5E, 0xC000);
    flags_w(0xC000u);
    charge(2);
    return RD_RTS;
}

/* FUN_00029f2c: TEST MODE -> OTHERS -> BOARD TEST, the page's state machine
 * (0xDAC): 0 draw the menu, 1 run the menu (0xDCC: 2 = entered, the item
 * 0xDAE picks the state 4*item + 2), then per board: I/O (2 set up, 3 run
 * until bits 7+6 of -0x77F2, 4/5 leave), MPU/DSP (6 set up, 7 the test,
 * 8/9 leave), VIDEO (10 set up, 11 the test, 12/13 leave), 14 back to the
 * OTHERS page, 15 reset */
static void board_leave(void)
{
    w16_set(-0x7800, 0); w16_set(-0x77FE, 0xFFFF); w16_set(0x0DAA, 0); w16_set(0x0DAC, 0); w16_set(0x5000, 0);
    flags_w(0u);
    charge(6);
}
static uint32_t board_setup(uint32_t ret_base, uint16_t page)
{
    uint32_t r;
    charge(1); if ((r = rd_call(L_2C000, ret_base))) return r;
    charge(1); if ((r = rd_call(L_29E78, ret_base + 6))) return r;
    charge(1); if ((r = rd_call(L_575A, ret_base + 12))) return r;
    w16_set(-0x7800, page); w16_set(-0x77FE, 0xFFFF);
    uint16_t n = (uint16_t)(w16(0x0DAC) + 1);
    w16_set(0x0DAC, n);
    rd_flags_nzvc((n & 0x8000) != 0, n == 0, n == 0x8000, 0); R[RR_XF] = 0;
    charge(4);
    return RD_RTS;
}
static uint32_t rd_29f2c(void)
{
    uint32_t r;
    w16_set(0x57B4, 1);
    charge(2);
    if ((r = rd_call(L_29D9C, 0x029F36))) return r;
    uint16_t st = w16(0x0DAC) & 0xF;
    uint16_t off = (uint16_t)vrd16(0x029F48u + (uint32_t)st * 2);
    set_d16(0, off);
    charge(5);
    poll();                                             /* jmp (0x29F48,PC,D0w) */
    switch (st) {
    case 0: {
        charge(1); if ((r = rd_call(L_5770, 0x029F74))) return r;
        charge(1); if ((r = rd_call(L_5876, 0x029F7A))) return r;
        charge(1); if ((r = rd_call(L_575A, 0x029F80))) return r;
        set_a(1, 0x02A0F0);
        charge(1);
        for (;;) {
            charge(1);
            if ((r = rd_call(L_5A0A, 0x029F8A))) return r;
            uint8_t b = (uint8_t)vrd8(a_reg(1));
            charge(2);
            flags_b(b);
            if (!b) break;
            poll();
        }
        w16_set(0x0DCA, 3);
        set_a(1, 0x02A0E0);
        vwr32(W(0x0DCE), 0x02A0E0);
        uint16_t n = (uint16_t)(w16(0x0DAC) + 1);
        w16_set(0x0DAC, n);
        rd_flags_nzvc((n & 0x8000) != 0, n == 0, n == 0x8000, 0); R[RR_XF] = 0;
        charge(5);
        return RD_RTS;
    }
    case 1: {
        charge(1);
        if ((r = rd_call(L_29B1C, 0x029FA6))) return r;
        uint16_t c = w16(0x0DCC) & 3;
        set_d16(0, (uint16_t)vrd16(0x029FB8u + (uint32_t)c * 2));
        charge(5);
        poll();
        if (c != 2) { charge(1); return RD_RTS; }
        uint16_t d0 = (uint16_t)((w16(0x0DAE) << 2) + 2);
        set_d16(0, d0);
        w16_set(0x0DAC, d0);
        w16_set(0x0DAE, 0);
        flags_w(0u);
        charge(6);
        return RD_RTS;
    }
    case 2: {
        charge(1); if ((r = rd_call(L_2C000, 0x029FDE))) return r;
        charge(1); if ((r = rd_call(L_5F3C, 0x029FE4))) return r;
        uint16_t n = (uint16_t)(w16(0x0DAC) + 1);
        w16_set(0x0DAC, n);
        rd_flags_nzvc((n & 0x8000) != 0, n == 0, n == 0x8000, 0); R[RR_XF] = 0;
        charge(2);
        return RD_RTS;
    }
    case 3: {
        charge(1); if ((r = rd_call(L_6118, 0x029FF0))) return r;
        uint8_t b = (uint8_t)(w8(-0x77F2) & 0xC0);
        set_d8(0, b);
        charge(4);
        { uint8_t t = (uint8_t)(b - 0xC0); rd_flags_nzvc((t & 0x80) != 0, t == 0, ((b ^ 0xC0) & (b ^ t) & 0x80) != 0, 0xC0 > b); }
        if (b == 0xC0) {
            uint16_t n = (uint16_t)(w16(0x0DAC) + 1);
            w16_set(0x0DAC, n);
            rd_flags_nzvc((n & 0x8000) != 0, n == 0, n == 0x8000, 0); R[RR_XF] = 0;
            charge(1);
        }
        charge(1);
        return RD_RTS;
    }
    case 4: case 5: case 8: case 9: case 12: case 13:
        board_leave();
        return RD_RTS;
    case 6:  return board_setup(0x02A028, 2);
    case 10: return board_setup(0x02A076, 3);
    case 7: {
        charge(1); if ((r = rd_call(L_62EC, 0x02A04C))) return r;
        uint16_t n = (uint16_t)(w16(0x0DAC) + 1);
        w16_set(0x0DAC, n);
        rd_flags_nzvc((n & 0x8000) != 0, n == 0, n == 0x8000, 0); R[RR_XF] = 0;
        charge(2);
        return RD_RTS;
    }
    case 11:
        charge(1); if ((r = rd_call(L_404, 0x02A09A))) return r;
        charge(1);
        return RD_RTS;
    case 14:
        charge(1); if ((r = rd_call(L_5770, 0x02A0C0))) return r;
        charge(1); if ((r = rd_call(L_5876, 0x02A0C6))) return r;
        charge(1); if ((r = rd_call(L_575A, 0x02A0CC))) return r;
        w16_set(0x0DAC, 0); w16_set(0x0DAE, 0); w16_set(0x5000, 1);
        flags_w(1u);
        charge(4);
        return 0x02A270;                                /* jmp 0x2A270.l */
    default:                                            /* 15 */
        w16_set(0x0DAC, 0);
        flags_w(0u);
        charge(2);
        return RD_RTS;
    }
}

/* FUN_00006da2: a DSP program upload for the MPU/DSP board test: the DSP
 * held (0x4000001A = 0), its control longs 0x38/0x30/0x00 = 0/-1/-1, then
 * the word list at A1 (a count, then count+1 words) written as longs to the
 * DSP RAM window 0x70000C00 (the count first), the DSP released */
static uint32_t rd_6da2(void)
{
    vwr8(0x4000001Au, 0);
    vwr32(0x70000038u, 0);
    vwr32(0x70000030u, 0xFFFFFFFFu);
    vwr32(0x70000000u, 0xFFFFFFFFu);
    uint32_t a0 = 0x70000C00u, a1 = a_reg(1);
    uint16_t n = (uint16_t)vrd16(a1); a1 += 2;
    set_d16(0, n);
    uint32_t d0 = d_reg(0);
    vwr32(a0, d0); a0 += 4;
    set_a(0, a0); set_a(1, a1);
    charge(7);
    for (uint16_t k = n; ; k--) {
        uint16_t v = (uint16_t)vrd16(a1); a1 += 2;
        set_d16(1, v);
        uint32_t d1 = d_reg(1);
        vwr32(a0, d1); a0 += 4;
        flags_l(d1);
        charge(3);
        set_a(0, a0); set_a(1, a1); set_d16(0, (uint16_t)(k - 1));
        if (k == 0) break;
        poll();
    }
    vwr8(0x4000001Au, 1);
    flags_b(1u);
    charge(2);
    return RD_RTS;
}

/* ======================================================================== */
/* BOARD TEST -> MPU/DSP BOARD: the master-DSP tests                         */
/* ======================================================================== */
/* Each test uploads a DSP program (FUN_00006da2, word list at A1), waits up
 * to 0x1E000 polls for the DSP's word 0x30 to read 0, starts the DSP with its
 * control longs, then reads its answer.  A timeout leaves D0 = -1 through
 * the rts at 0x6DE4. */
#define POLYW(o) (0x70000000u + (uint32_t)(o))

/* the upload and the ready wait; 1 = ready, 0 = timed out (D0 = -1 set,
 * the moveq and bra charged) */
static int dsp_upload_ready(uint32_t prog, uint32_t ret, uint32_t *r)
{
    set_a(1, prog);
    charge(3);                                          /* lea nop bsr */
    if ((*r = rd_call(L_6DA2, ret))) return 0;
    uint32_t d7 = 0xF000u << 1;
    set_d(7, d7);
    charge(2);
    for (;;) {
        uint32_t d1 = vrd32(POLYW(0x30)) & 0xFFFFu;
        set_d(1, d1);
        charge(3);                                      /* move andi beq */
        if (!d1) { rd_flags_nzvc(0, 1, 0, 0); return 1; }
        d7--;
        set_d(7, d7);
        charge(2);                                      /* subq bne */
        if (!d7) break;
        poll();
    }
    set_d(0, 0xFFFFFFFFu);
    rd_flags_nzvc(1, 0, 0, 0);
    charge(2);                                          /* moveq bra */
    return 0;
}

/* the DSP started: held, control longs 0x00 = 0, 0x20 = `go`, 0x28/0x30 = 0,
 * 0x50 and 0x38 = 0 (in the order `late38` says), released */
static void dsp_start(uint32_t go, int late38)
{
    vwr8(0x4000001Au, 0);
    vwr32(POLYW(0x00), 0);
    vwr32(POLYW(0x20), go);
    vwr32(POLYW(0x28), 0);
    vwr32(POLYW(0x30), 0);
    if (late38) { vwr32(POLYW(0x50), 0); vwr32(POLYW(0x38), 0); }
    else        { vwr32(POLYW(0x38), 0); vwr32(POLYW(0x50), 0); }
    vwr8(0x4000001Au, 1);
    flags_b(1u);
    charge(8);
}

/* poll word 0x20 into data register `reg` (up to 0x1E000 times) while its low
 * word reads -1; 1 = answered (the bne taken) */
static int dsp_answer(int reg)
{
    uint32_t d2 = 0xF000u << 1;
    set_d(2, d2);
    charge(2);
    for (;;) {
        uint32_t v = vrd32(POLYW(0x20));
        set_d(reg, v);
        charge(3);                                      /* move cmpi bne */
        rd_flags_cmp16((uint16_t)v, 0xFFFF);
        if ((uint16_t)v != 0xFFFF) return 1;
        d2--;
        set_d(2, d2);
        charge(2);                                      /* subi bne */
        rd_flags_nzvc((d2 & 0x80000000u) != 0, d2 == 0, d2 == 0x7FFFFFFFu, d2 == 0xFFFFFFFFu);
        if (!d2) return 0;
        poll();
    }
}

/* FUN_0000691e: test 2 -- program 0x6F3E; D0w = the DSP's answer (word 0x20)
 * unless it stays -1 */
static uint32_t rd_691e(void)
{
    uint32_t r;
    if (!dsp_upload_ready(0x006F3E, 0x006928, &r)) return r ? r : 0x006DE4;
    dsp_start(0xFFFFFFFFu, 1);
    if (dsp_answer(1)) { set_d16(0, dw(1)); flags_w(dw(1)); charge(2); }   /* move.w D1w,D0w ; rts */
    else charge(2);                                     /* bra ; rts */
    return RD_RTS;
}

/* FUN_000069bc / 00006a5e / 00006b00: tests 3, 4 and 1 -- the answer in D0
 * (still -1 if the DSP never gave one); 3 and 4 also set word 0x24 to -1 */
static uint32_t dsp_test_d0(uint32_t prog, uint32_t ret, int w24)
{
    uint32_t r;
    if (!dsp_upload_ready(prog, ret, &r)) return r ? r : 0x006DE4;
    if (w24) { vwr32(POLYW(0x24), 0xFFFFFFFFu); charge(1); }
    dsp_start(0xFFFFFFFFu, 1);
    dsp_answer(0);
    charge(1);                                          /* rts */
    return RD_RTS;
}
static uint32_t rd_69bc(void) { return dsp_test_d0(0x00707E, 0x0069C6, 1); }
static uint32_t rd_6a5e(void) { return dsp_test_d0(0x0072FE, 0x006A68, 1); }
static uint32_t rd_6b00(void) { return dsp_test_d0(0x0075EE, 0x006B0A, 0); }

/* FUN_00006d28: test 6 -- program 0x764C started with word 0x20 = 1, no
 * answer read (a timeout ends in this function's own rts at 0x6DE4) */
static uint32_t rd_6d28(void)
{
    uint32_t r;
    if (!dsp_upload_ready(0x00764C, 0x006D32, &r)) { if (r) return r; charge(1); return RD_RTS; }
    dsp_start(1, 0);
    charge(1);
    return RD_RTS;
}

/* FUN_000067c0: test 0 -- program 0x6DE6; the DSP answers 0 (next stage),
 * 0x5963 (D0 = 1, stop) or nothing (D0 = 1); stage 2, program 0x6E46: the
 * answer 0 -> D0 = 0, 1 or 2 -> D0w = answer + 1, else D0 = the last read */
static uint32_t rd_67c0(void)
{
    uint32_t r;
    if (!dsp_upload_ready(0x006DE6, 0x0067CA, &r)) return r ? r : 0x006DE4;
    dsp_start(0xFFFFFFFFu, 1);
    set_d(0, 1);
    uint32_t d2 = 0xF000u << 1;
    set_d(2, d2);
    charge(3);                                          /* moveq move lsl */
    int stage2 = 0;
    for (;;) {
        uint32_t v = vrd32(POLYW(0x20));
        set_d(1, v);
        charge(3);                                      /* move cmpi beq */
        rd_flags_cmp16((uint16_t)v, 0);
        if ((uint16_t)v == 0) { stage2 = 1; break; }
        charge(2);                                      /* cmpi beq */
        rd_flags_cmp16((uint16_t)v, 0x5963);
        if ((uint16_t)v == 0x5963) break;
        d2--;
        set_d(2, d2);
        charge(2);
        rd_flags_nzvc((d2 & 0x80000000u) != 0, d2 == 0, d2 == 0x7FFFFFFFu, d2 == 0xFFFFFFFFu);
        if (!d2) { charge(1); break; }                  /* bra.w 0x691C */
        poll();
    }
    if (!stage2) { charge(1); return RD_RTS; }
    if (!dsp_upload_ready(0x006E46, 0x00686E, &r)) return r ? r : 0x006DE4;
    dsp_start(0xFFFFFFFFu, 1);
    d2 = 0xF000u << 1;
    set_d(2, d2);
    charge(2);
    for (;;) {
        uint32_t v = vrd32(POLYW(0x20));
        uint16_t w = (uint16_t)v;
        set_d(1, v);
        charge(3);
        rd_flags_cmp16(w, 0);
        if (w == 0) { set_d(0, 0); rd_flags_nzvc(0, 1, 0, 0); charge(2); return RD_RTS; }   /* moveq 0 ; rts */
        charge(2);
        rd_flags_cmp16(w, 1);
        if (w != 1) { charge(2); rd_flags_cmp16(w, 2); }
        if (w == 1 || w == 2) {
            w = (uint16_t)(w + 1);
            set_d16(1, w); set_d16(0, w);
            flags_w(w);
            charge(4);                                  /* addi move bra rts */
            return RD_RTS;
        }
        d2--;
        set_d(2, d2);
        charge(2);
        rd_flags_nzvc((d2 & 0x80000000u) != 0, d2 == 0, d2 == 0x7FFFFFFFu, d2 == 0xFFFFFFFFu);
        if (!d2) {
            set_d(0, v);
            flags_l(v);
            charge(3);                                  /* move.l bra rts */
            return RD_RTS;
        }
        poll();
    }
}

/* the LFSR the memory test writes and checks: shift left, feedback bit 0 =
 * bit 31 (N) xor bit 4 after the shift */
static uint32_t lfsr_step(uint32_t d2, int *extra)
{
    d2 <<= 1;
    int n = (d2 >> 31) & 1, b4 = (d2 >> 4) & 1;
    /* N=1: beq(bit4 clear -> addq) / bne; N=0: beq (bit4 clear -> skip) / addq */
    if (n) { *extra = b4 ? 2 : 2; if (!b4) d2 += 1; }
    else   { *extra = b4 ? 2 : 1; if (b4) d2 += 1; }
    return d2;
}

/* FUN_00006b98: test 5 -- DSP RAM check: D1w blocks of 0x20000 words from
 * the DSP address D2, in chunks of at most 0x3C00+1 words through the window
 * at 0x70000C00: the LFSR pattern (seed 0x1234) written chunk by chunk and
 * the DSP asked to store it, then read back chunk by chunk and compared (low
 * 24 bits).  0 = all equal; else D0 = 1, D1 = the failing DSP address,
 * D3 = the value read.  A DSP that never answers the store gives D0w = -1. */
static uint32_t rd_6b98(void)
{
    vwr8(0x4000001Au, 0);
    vwr32(POLYW(0x28), 0xFFFFFFFFu);
    vwr32(POLYW(0x2C), 0);
    vwr8(0x4000001Au, 1);
    uint32_t a0 = d_reg(2);
    set_a(0, a0);
    vwr32(POLYW(0x4C), 0xFFFFFFFFu);
    vwr32(POLYW(0x44), a0);
    vwr32(POLYW(0x40), 0xFFFFFFFFu);
    set_a(1, 0x70000C00u);
    uint32_t d2 = 0x1234, d1 = d_reg(1);
    d1 = (d1 << 16 | d1 >> 16);                         /* swap */
    d1 = (d1 & 0xFFFF0000u);                            /* move.w #0,D1w */
    d1 = (d1 << 1) - 1;                                 /* lsl.l #1 ; subq.l #1 */
    uint32_t d5 = d1, d6 = 0;
    set_d(2, d2); set_d(3, 1); set_d(1, d1); set_d(5, d5); set_d(6, 0);
    charge(17);
    /* write pass */
    for (;;) {
        uint16_t d0 = (uint16_t)d1;
        charge(3);                                      /* move cmpi blt */
        if (!((int32_t)d1 < 0x3C00)) { charge(1); d0 = 0x3C00; }
        d6 = (d6 & 0xFFFF0000u) | d0;
        uint32_t a1 = 0x70000C00u;
        set_d16(0, d0); set_d(6, d6); set_a(1, a1);
        charge(2);                                      /* move lea */
        for (uint16_t k = d0; ; k--) {
            vwr32(a1, d2); a1 += 4;
            int ex; d2 = lfsr_step(d2, &ex);
            charge(4 + ex + 1);                         /* move lsl btst bpl, beq/bne/addq, dbf */
            set_a(1, a1); set_d(2, d2); set_d16(0, (uint16_t)(k - 1));
            if (k == 0) break;
            poll();
        }
        vwr32(POLYW(0x48), d6);
        vwr32(POLYW(0x2C), 0xFFFFFFFFu);
        uint32_t d0l = 0xF000u << 1;
        set_d(0, d0l);
        charge(4);
        for (;;) {
            uint32_t d4 = vrd32(POLYW(0x2C));
            set_d(4, d4);
            charge(3);                                  /* move tst beq */
            if (!(uint16_t)d4) break;
            d0l--;
            set_d(0, d0l);
            charge(2);
            if (!d0l) {
                set_d16(0, 0xFFFF);
                rd_flags_nzvc(1, 0, 0, 0);
                charge(2);                              /* move.w bra */
                return 0x006DE4;
            }
            poll();
        }
        uint32_t rest = d1 - d6 - 1;
        set_d(0, rest);
        charge(4);                                      /* move sub subq ble */
        if ((int32_t)rest <= 0) break;
        d1 = rest;
        set_d(1, d1);
        vwr32(POLYW(0x40), 0);
        charge(3);                                      /* move move bra */
        poll();
    }
    /* read-back pass */
    vwr32(POLYW(0x4C), 0);
    vwr32(POLYW(0x44), a0);
    vwr32(POLYW(0x40), 0xFFFFFFFFu);
    d2 = 0x1234; d1 = d5; d6 = 0;
    set_d(2, d2); set_d(3, 1); set_d(1, d1); set_d(6, 0);
    charge(7);
    for (;;) {
        uint16_t d0 = (uint16_t)d1;
        charge(3);
        if (!((int32_t)d1 < 0x3C00)) { charge(1); d0 = 0x3C00; }
        d6 = (d6 & 0xFFFF0000u) | d0;
        set_d16(0, d0); set_d(6, d6);
        vwr32(POLYW(0x48), d6);
        vwr32(POLYW(0x2C), 0xFFFFFFFFu);
        charge(3);
        for (;;) {
            uint32_t d4 = vrd32(POLYW(0x2C));
            set_d(4, d4);
            charge(3);                                  /* move tst bne */
            if (!(uint16_t)d4) break;
            poll();
        }
        uint32_t a1 = 0x70000C00u;
        set_a(1, a1);
        charge(1);
        for (uint16_t k = d0; ; k--) {
            uint32_t v = vrd32(a1) & 0xFFFFFFu; a1 += 4;
            uint32_t d7 = d2 & 0xFFFFFFu;
            set_d(5, v); set_d(7, d7); set_a(1, a1);
            charge(6);                                  /* move andi move andi cmp bne */
            if (v != d7) {
                d6 = (d6 & 0xFFFF0000u) | (uint16_t)((uint16_t)d6 - k);
                a0 += d6;
                set_d(6, d6); set_a(0, a0);
                set_d(0, 1); set_d(3, v); set_d(1, a0);
                flags_l(a0);
                charge(6);                              /* sub adda moveq move move rts */
                return RD_RTS;
            }
            int ex; d2 = lfsr_step(d2, &ex);
            charge(3 + ex + 1);                         /* lsl btst bpl, beq/bne/addq, dbf */
            set_d(2, d2); set_d16(0, (uint16_t)(k - 1));
            if (k == 0) break;
            poll();
        }
        uint32_t d0l = d1 - d6 - 1;
        set_d(0, d0l);
        charge(4);
        if ((int32_t)d0l <= 0) { set_d(0, 0); rd_flags_nzvc(0, 1, 0, 0); charge(2); return RD_RTS; }
        d1 = d0l;
        set_d(1, d1);
        vwr32(POLYW(0x40), 0);
        a0 += d6 + 1;
        set_a(0, a0);
        charge(5);                                      /* move move adda addq bra */
        poll();
    }
}

/* FUN_0000679e: run DSP test D0 (0..6, table 0x67B2) with D4-D7/A0-A6 saved
 * around it on the stack */
static uint32_t rd_679e(void)
{
    uint32_t sp = a_reg(7) - 44;
    for (int i = 0; i < 4; i++) vwr32(sp + 4u * (uint32_t)i, d_reg(4 + i));
    for (int i = 0; i < 7; i++) vwr32(sp + 16u + 4u * (uint32_t)i, a_reg(i));
    set_a(7, sp);
    uint16_t d0 = (uint16_t)(dw(0) + dw(0));
    uint16_t off = (uint16_t)vrd16(0x0067B2u + (uint32_t)(int16_t)d0);
    set_d16(0, off);
    flags_w(off);
    charge(4);                                          /* movem add move jsr */
    uint32_t r;
    if ((r = rd_call_ind(0x0067B2u + (uint32_t)(int16_t)off, 0x0067AC, 0x0067A8))) return r;
    sp = a_reg(7);
    for (int i = 0; i < 4; i++) set_d(4 + i, vrd32(sp + 4u * (uint32_t)i));
    for (int i = 0; i < 7; i++) set_a(i, vrd32(sp + 16u + 4u * (uint32_t)i));
    set_a(7, sp + 44);
    charge(2);                                          /* movem rts */
    return RD_RTS;
}

/* FUN_000062ec (the MPU/DSP board test page itself) stays lifted: one call
 * runs every DSP test and then waits across frames for the gas or the brake,
 * past the checker's 1M-instruction probe cap, so it cannot be proven. */

static const rd_entry rd_table_b7[] = {
    { 0x0288AC, rd_288ac, 0x0003, 0, 0, "FUN_000288ac" },
    { 0x0290BA, rd_290ba, 0x0101, 0, 0, "FUN_000290ba" },
    { 0x02F5CC, rd_2f5cc, 0x0FFF, 0, 0, "FUN_0002f5cc" },
    { 0x0045A6, rd_45a6,  0x0000, 0, 0, "FUN_000045a6" },
    { 0x028724, rd_28724, 0x0000, 0, 0, "FUN_00028724" },
    { 0x004624, rd_4624,  0x0007, 0, 0, "FUN_00004624" },
    { 0x026B12, rd_26b12, 0x0000, 0, 0, "FUN_00026b12" },
    { 0x0043DA, rd_43da,  0x0001, 0, 0, "FUN_000043da" },
    { 0x006524, rd_6524,  0x0101, 0, 0, "FUN_00006524" },
    { 0x005D7C, rd_5d7c,  0x0203, 0, 0, "FUN_00005d7c" },
    { 0x029E78, rd_29e78, 0x0001, 0, 0, "FUN_00029e78" },
    { 0x0060CE, rd_60ce,  0x0203, 0, 0, "FUN_000060ce" },
    { 0x01E3B8, rd_1e3b8, 0x0181, 0, 0, "FUN_0001e3b8" },
    { 0x01E5A4, rd_1e5a4, 0x0000, 0, 0, "FUN_0001e5a4" },
    { 0x02D32A, rd_2d32a, 0x0000, 0, 0, "FUN_0002d32a" },
    { 0x0064FC, rd_64fc,  0x0F01, 0, 0, "FUN_000064fc" },
    { 0x0297B6, rd_297b6, 0x0101, 0, 0, "FUN_000297b6" },
    { 0x0060EC, rd_60ec,  0x0001, 0, 0, "FUN_000060ec" },
    { 0x01E3FC, rd_1e3fc, 0x0FDF, 0, 0, "FUN_0001e3fc" },
    { 0x01E62C, rd_1e62c, 0x0FDF, 0, 0, "FUN_0001e62c" },
    { 0x0297E0, rd_297e0, 0x0303, 0, 0, "FUN_000297e0" },
    { 0x02D1DA, rd_2d1da, 0x0207, 0, 0, "FUN_0002d1da" },
    { 0x02D1B2, rd_2d1b2, 0x030F, 0, 0, "FUN_0002d1b2" },
    { 0x02D35C, rd_2d35c, 0x0207, 0, 0, "FUN_0002d35c" },
    { 0x005F3C, rd_5f3c,  0x0FDF, 0, 0, "FUN_00005f3c" },
    { 0x026C84, rd_26c84, 0x08FF, 0, 0, "FUN_00026c84" },
    { 0x006118, rd_6118,  0x1FFF, 0, 0, "FUN_00006118" },
    { 0x029F2C, rd_29f2c, 0x3FFF, 0, 0, "FUN_00029f2c" },
    { 0x006DA2, rd_6da2,  0x0303, 0, 0, "FUN_00006da2" },
    { 0x00679E, rd_679e,  0x3FFF, 0, 0, "FUN_0000679e" },
    { 0x0067C0, rd_67c0,  0x0387, 0, 0, "FUN_000067c0" },
    { 0x00691E, rd_691e,  0x0387, 0, 0, "FUN_0000691e" },
    { 0x0069BC, rd_69bc,  0x0387, 0, 0, "FUN_000069bc" },
    { 0x006A5E, rd_6a5e,  0x0387, 0, 0, "FUN_00006a5e" },
    { 0x006B00, rd_6b00,  0x0387, 0, 0, "FUN_00006b00" },
    { 0x006D28, rd_6d28,  0x0383, 0, 0, "FUN_00006d28" },
    { 0x006B98, rd_6b98,  0x03FF, 0, 0, "FUN_00006b98" },
};
RD_REGISTER(rd_table_b7)
