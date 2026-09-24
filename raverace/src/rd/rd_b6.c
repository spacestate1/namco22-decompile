/*
 * rd_b6.c -- Phase B batch 6 (0x29C68..0x318BE): readable replacements for
 * the test-mode pages (menu rows, the coin/play counters, the sound and
 * option pages, the switch/lamp tests), the boot-time RAM tests, the attract
 * display-list builders and the two animation-curve samplers.
 * Same rules as rd_b3.c (include/rd.h): written from the 68K INSTRUCTIONS,
 * every register a caller could read set exactly as the 68K leaves it,
 * instructions charged in execution order before each poll point (taken
 * backward branch, bsr/jsr, computed jmp). Every entry uses cost 0 and
 * charges its own rts. After a call, registers are re-read from R[]: a
 * callee may change any register its kill mask names.
 * Proven by RR_RD=check (+ fuzz) against the lifted twin.
 */
#include "rd.h"
#include "rr_lifted.h"
#include <stdio.h>

/* (d16,A6): A6 always holds WRAM + 0x8000 */
#define W(off) (0x10008000u + (uint32_t)(int32_t)(off))
static inline uint16_t w16(int32_t off)             { return (uint16_t)vrd16(W(off)); }
static inline uint32_t w32(int32_t off)             { return vrd32(W(off)); }
static inline uint8_t  w8(int32_t off)              { return (uint8_t)vrd8(W(off)); }
static inline void     w16_set(int32_t off, uint32_t v) { vwr16(W(off), v & 0xFFFFu); }
static inline void     w32_set(int32_t off, uint32_t v) { vwr32(W(off), v); }
static inline void     w8_set(int32_t off, uint32_t v)  { vwr8(W(off), v & 0xFFu); }
static inline uint16_t dw(int n) { return (uint16_t)d_reg(n); }
static inline uint32_t sx16(uint32_t v) { return (uint32_t)(int32_t)(int16_t)v; }
static inline uint32_t rol32(uint32_t v, unsigned n) { n &= 31; return n ? (v << n) | (v >> (32 - n)) : v; }
/* rol.l Dn,Dm with a REGISTER count: the lifted build (Ghidra's p-code) takes
 * the count mod 64 and yields 0 for 33..63 (32 gives the value unchanged), where
 * a 68020 would rotate by the
 * count mod 32. Only a fuzzed register reaches that range; the lifted result
 * is the one kept. */
static inline uint32_t rol32_reg(uint32_t v, uint32_t cnt) { cnt &= 63; return cnt == 32 ? v : cnt > 32 ? 0 : rol32(v, cnt); }

/* ---- WRAM (A6-relative) ---- */
#define TEXT_ATTR        0x0C5E   /* word: attribute (palette) added to printed characters     */
#define DL_CURSOR        0x0C46   /* long: display-list write pointer                          */
#define DL_PRIO          0x0C40   /* word: display-list priority word                          */
#define MENU_VALUE       0x0DAE   /* word: value being edited (low byte = the setting)          */
#define MENU_ROW         0x5002   /* word: cursor row of the test-mode page                     */
#define MENU_DEFAULT     0x5008   /* word: the factory setting of the row being printed         */
#define ROW_ATTR_SEL     0x0DC0   /* word: attribute of the selected row                        */
#define ROW_ATTR_NORMAL  0x0DBE   /* word: attribute of an unselected row                       */

/* external routines */
#define PRINT_TEXT       0x005A0Au   /* FUN_00005a0a: print the text command string at A1 */

/* ======================================================================== */
/* row attributes and small menu helpers                                    */
/* ======================================================================== */

/* FUN_00029c68: the attribute for printing row D4 whose value is D5: the
 * cursor row gets ROW_ATTR_SEL/ROW_ATTR_NORMAL, any other row 0xE000/0xD000;
 * a value equal to the factory setting (MENU_DEFAULT) uses the first of the
 * pair. Stored in TEXT_ATTR; D0w/D1w hold the pair (D1w = the choice). */
static uint32_t rd_29c68(void)
{
    uint16_t a, b;
    if (dw(4) == w16(MENU_ROW)) { charge(5); a = w16(ROW_ATTR_SEL); b = w16(ROW_ATTR_NORMAL); }
    else                        { charge(4); a = 0xE000; b = 0xD000; }
    set_d16(0, a); set_d16(1, b);
    charge(2);
    if (dw(5) == w16(MENU_DEFAULT)) { charge(1); b = a; set_d16(1, b); }
    charge(2);
    w16_set(TEXT_ATTR, b);
    return RD_RTS;
}

/* FUN_0002a454: edit the value at 0x57BC with the limits table 0x2A46A
 * (FUN_00029bcc) */
static uint32_t rd_2a454(void)
{
    uint32_t r;
    charge(3);
    set_a(0, 0x02A46A);
    w16_set(MENU_VALUE, w16(0x57BC));
    if ((r = rd_call(L_29BCC, 0x02A462))) return r;
    charge(2);
    w16_set(0x57BC, w16(MENU_VALUE));
    return RD_RTS;
}

/* FUN_0002a46c: print row 1 (value 0x57BC, factory setting 1): text 0x2A56E
 * when the value is even, 0x2A575 when odd (tail jump to the printer) */
static uint32_t rd_2a46c(void)
{
    uint32_t r;
    charge(4);
    set_d(4, 1);
    set_d16(5, w16(0x57BC));
    w16_set(MENU_DEFAULT, 1);
    if ((r = rd_call(L_29C68, 0x02A47C))) return r;
    charge(3);
    set_a(1, 0x02A56E);
    set_d16(5, dw(5) & 1);
    if (dw(5)) { charge(1); set_a(1, 0x02A575); }
    charge(1);
    return RD_JMP(PRINT_TEXT);
}

/* FUN_0002a5c8: print the text at 0x2A5E0, then the one at 0x7AFA8 */
static uint32_t rd_2a5c8(void)
{
    uint32_t r;
    charge(2);
    set_a(1, 0x02A5E0);
    if ((r = rd_call(L_5A0A, 0x02A5D2))) return r;
    charge(2);
    set_a(1, 0x07AFA8);
    return RD_JMP(PRINT_TEXT);
}

/* print a fixed text (lea (d,PC),A1 ; jmp printer) */
static uint32_t print_fixed(uint32_t a1) { charge(2); set_a(1, a1); return RD_JMP(PRINT_TEXT); }
static uint32_t rd_2b354(void) { return print_fixed(0x02B35E); }   /* FUN_0002b354 */
static uint32_t rd_2b378(void) { return print_fixed(0x02B35E); }   /* FUN_0002b378: same text */
static uint32_t rd_2b974(void) { return print_fixed(0x02B97E); }   /* FUN_0002b974 */
static uint32_t rd_2c170(void) { return print_fixed(0x02C17A); }   /* FUN_0002c170 */

/* FUN_0002c088: clear the word 0x5000, continue at FUN_00026e24 */
static uint32_t rd_2c088(void)
{
    charge(2);
    w16_set(0x5000, 0);
    return RD_JMP(0x026E24);
}

/* ======================================================================== */
/* number formatting                                                         */
/* ======================================================================== */

/* FUN_0002cbca: D0 = D1 in decimal digits, one per nibble (8 digits, the top
 * one dropped): each digit is D1 / 10^k from the table at 0x2CBF0 (divul.l),
 * the remainder carried on. A1 ends past the table, D3 = 0, D2w = 0xFFFF. */
static uint32_t rd_2cbca(void)
{
    uint32_t a1 = 0x02CBF0, d0 = 0, d1 = d_reg(1);
    charge(3);
    for (int n = 7; ; n--) {
        uint32_t div = vrd32(a1); a1 += 4;
        uint32_t q = d1 / div, rem = d1 % div;
        d0 = rol32(d0, 4) | (q & 0xF);
        d1 = rem;
        charge(7);
        set_a(1, a1); set_d(0, d0); set_d(1, d1); set_d(2, (uint32_t)(n - 1) & 0xFFFFu); set_d(3, 0);
        if (n == 0) break;
        poll();
    }
    charge(2);
    set_a(1, a1);
    set_d(0, d0 & 0x0FFFFFFFu);
    set_d(1, d1);
    set_d(2, 0x0000FFFFu);
    set_d(3, 0);
    return RD_RTS;
}

/* FUN_0002cc10: D1 = D0 (32-bit) in packed BCD, low six digits (the
 * shift-and-decimal-add loop); D2b/D3b keep the middle/low digit pairs */
static uint32_t rd_2cc10(void)
{
    uint32_t v = d_reg(0), b1 = 0, b2 = 0, b3 = 0;
    charge(4);
    for (int k = 31; ; k--) {
        R[RR_XF] = (uint8_t)(v >> 31); v <<= 1;              /* add.l D0,D0: X = carry */
        b3 = rr_abcd(b3, b3); b2 = rr_abcd(b2, b2); b1 = rr_abcd(b1, b1);
        charge(5);
        set_d(0, v); set_d(1, (d_reg(1) & 0xFFFF0000u) | b1); set_d(2, (d_reg(2) & 0xFFFF0000u) | b2);
        set_d(3, (d_reg(3) & 0xFFFF0000u) | b3); set_d(7, (uint32_t)(k - 1) & 0xFFFFu);
        if (k == 0) break;
        poll();
    }
    charge(5);
    set_d(0, 0);
    set_d(1, b1 << 16 | b2 << 8 | b3);
    set_d(2, (d_reg(2) & 0xFFFF0000u) | b2);
    set_d(3, (d_reg(3) & 0xFFFF0000u) | b3);
    set_d(7, 0x0000FFFFu);
    return RD_RTS;
}

/* FUN_0002cc2e: D1 = D0 (32-bit) in packed BCD, low eight digits */
static uint32_t rd_2cc2e(void)
{
    uint32_t v = d_reg(0), b1 = 0, b2 = 0, b3 = 0, b4 = 0;
    charge(5);
    for (int k = 31; ; k--) {
        R[RR_XF] = (uint8_t)(v >> 31); v <<= 1;
        b4 = rr_abcd(b4, b4); b3 = rr_abcd(b3, b3); b2 = rr_abcd(b2, b2); b1 = rr_abcd(b1, b1);
        charge(6);
        set_d(0, v); set_d(1, (d_reg(1) & 0xFFFF0000u) | b1); set_d(2, (d_reg(2) & 0xFFFF0000u) | b2);
        set_d(3, (d_reg(3) & 0xFFFF0000u) | b3); set_d(4, (d_reg(4) & 0xFFFF0000u) | b4);
        set_d(7, (uint32_t)(k - 1) & 0xFFFFu);
        if (k == 0) break;
        poll();
    }
    charge(7);
    set_d(0, 0);
    set_d(1, b1 << 24 | b2 << 16 | b3 << 8 | b4);
    set_d(2, (d_reg(2) & 0xFFFF0000u) | b2);
    set_d(3, (d_reg(3) & 0xFFFF0000u) | b3);
    set_d(4, (d_reg(4) & 0xFFFF0000u) | b4);
    set_d(7, 0x0000FFFFu);
    return RD_RTS;
}

/* the digit writer shared by FUN_0002c8d8 / FUN_0002c900: D1 rotated so its
 * digit (8 - D6) is at the top, then D6w+1 digits written to (A0)+ as
 * characters 0xC030+digit, then the terminator 0xC020 */
static void write_digits(uint32_t first_rot)
{
    uint32_t d2 = (first_rot - d_reg(6)) << 2;
    uint32_t d1 = rol32_reg(d_reg(1), d2);
    uint32_t a0 = a_reg(0), d0 = d_reg(0);
    uint16_t n = dw(6);
    charge(4);
    for (;;) {
        d0 = (d1 & 0xF);
        d0 = (d0 & 0xFFFF0000u) | (uint16_t)(d0 + 0xC030);
        vwr16(a0, (uint16_t)d0); a0 += 2;
        d1 = rol32(d1, 4);
        charge(6);
        if (n-- == 0) break;
        set_d(0, d0); set_d(1, d1); set_d(2, d2); set_d16(6, n); set_a(0, a0);
        poll();
    }
    charge(2);
    vwr16(a0, 0xC020); a0 += 2;
    set_d(0, d0); set_d(1, d1); set_d(2, d2);
    set_d16(6, 0xFFFF);
    set_a(0, a0);
}

/* FUN_0002c8d8: print D0 as D6w+1 decimal digits (FUN_0002cc10) at A0 */
static uint32_t rd_2c8d8(void)
{
    uint32_t r;
    charge(1);
    if ((r = rd_call(L_2CC10, 0x02C8DC))) return r;
    charge(1);                                          /* nop */
    write_digits(8);
    return RD_RTS;
}

/* FUN_0002c900: the same with eight digits available (FUN_0002cc2e) */
static uint32_t rd_2c900(void)
{
    uint32_t r;
    charge(1);
    if ((r = rd_call(L_2CC2E, 0x02C904))) return r;
    charge(1);
    write_digits(8);
    return RD_RTS;
}

/* the digit writers with separators, FUN_0002c928 / FUN_0002c968: D6w+1
 * digits of D1 from digit (top - D6); after the digit that makes the count
 * D2w reach sep1 / sep2 a separator character is written and one cell
 * skipped; then the closing character(s). */
static void write_digits_sep(uint32_t top, uint16_t sep1, uint16_t c1, uint16_t sep2, uint16_t c2)
{
    uint32_t d0 = (top - d_reg(6)) << 2;
    uint32_t d1 = rol32_reg(d_reg(1), d0);
    uint32_t d2 = d_reg(2), a0 = a_reg(0);
    uint16_t n = dw(6);
    charge(4);
    for (;;) {
        d2 = (d2 & 0xFFFF0000u) | (uint16_t)(d2 + 1);
        d0 = d1 & 0xF;
        d0 = (uint16_t)(d0 + 0xC030);
        vwr16(a0, (uint16_t)d0); a0 += 2;
        d1 = rol32(d1, 4);
        charge(8);
        if ((uint16_t)d2 == sep1) { charge(2); vwr16(a0, c1); a0 += 4; }
        charge(2);
        if ((uint16_t)d2 == sep2) { charge(2); vwr16(a0, c2); a0 += 4; }
        charge(1);
        if (n-- == 0) break;
        set_d(0, d0); set_d(1, d1); set_d(2, d2); set_d16(6, n); set_a(0, a0);
        poll();
    }
    set_d(0, d0); set_d(1, d1); set_d(2, d2);
    set_d16(6, 0xFFFF);
    set_a(0, a0);
}

/* FUN_0002c928: seven-digit money style: "ddd,dd.d" style separators after
 * digits 3 and 5, closed by 0xC053 and the terminator */
static uint32_t rd_2c928(void)
{
    write_digits_sep(8, 3, 0xC048, 5, 0xC04D);
    charge(3);
    uint32_t a0 = a_reg(0);
    vwr16(a0, 0xC053); vwr16(a0 + 2, 0xC020);
    set_a(0, a0 + 4);
    return RD_RTS;
}

/* FUN_0002c968: time style, separators 0xC04D / 0xC053 after digits 2 and 4 */
static uint32_t rd_2c968(void)
{
    write_digits_sep(7, 2, 0xC04D, 4, 0xC053);
    charge(2);
    uint32_t a0 = a_reg(0);
    vwr16(a0, 0xC020);
    set_a(0, a0 + 2);
    return RD_RTS;
}

/* ======================================================================== */
/* the counter pages                                                         */
/* ======================================================================== */

/* the tail shared by the money counters: D1 = D0, D6 = 6, D2 = 0, A0 = cell,
 * continue at FUN_0002c928 */
static uint32_t money_tail(uint32_t cell)
{
    charge(6);                                          /* nop, move, moveq, clr, lea, bra */
    set_d(1, d_reg(0));
    set_d(6, 6);
    set_d(2, 0);
    set_a(0, cell);
    return 0x02C928;
}

/* FUN_0002c572: the sum of the six counters 0x520A..0x521E, as digits
 * (FUN_0002cbca), at 0x9009E31E */
static uint32_t rd_2c572(void)
{
    uint32_t r, s = 0;
    for (int i = 0; i < 6; i++) s += w32(0x520A + 4 * i);
    set_d(1, s);
    charge(7);
    if ((r = rd_call(L_2CBCA, 0x02C58E))) return r;
    return money_tail(0x9009E31Eu);
}

/* sum of two counters -> digits -> cell */
static uint32_t money_pair(int32_t off, uint32_t ret, uint32_t cell)
{
    uint32_t r;
    set_d(1, w32(off) + w32(off + 4));
    charge(3);
    if ((r = rd_call(L_2CBCA, ret))) return r;
    return money_tail(cell);
}
static uint32_t rd_2c5a0(void) { return money_pair(0x520A, 0x02C5AC, 0x9009E39Eu); }  /* FUN_0002c5a0 */
static uint32_t rd_2c5be(void) { return money_pair(0x521A, 0x02C5CA, 0x9009E41Eu); }  /* FUN_0002c5be */
static uint32_t rd_2c5dc(void) { return money_pair(0x5212, 0x02C5E8, 0x9009E49Eu); }  /* FUN_0002c5dc */

/* FUN_0002c5fa: the six-counter sum divided by 0x5242 (when non-zero) as
 * digits, printed with three decimals at 0x9009E6A8 */
static uint32_t rd_2c5fa(void)
{
    uint32_t r, s = 0;
    set_d(0, 0);
    for (int i = 0; i < 6; i++) s += w32(0x520A + 4 * i);
    set_d(1, s);
    uint32_t d5 = w32(0x5242);
    set_d(5, d5);
    charge(9);
    if (d5) {
        set_d(1, s / d5);
        charge(2);
        if ((r = rd_call(L_2CBCA, 0x02C622))) return r;
        charge(1);                                      /* nop */
    }
    charge(5);
    set_d(1, d_reg(0));
    set_d(6, 3);
    set_d(2, 3);
    set_a(0, 0x9009E6A8u);
    return 0x02C928;
}

/* one play counter (a long at 0x100011xx) as six/seven digits at a cell */
static uint32_t counter_one(uint32_t src, uint32_t cell)
{
    charge(4);
    set_d(0, vrd32(src));
    set_a(0, cell);
    set_d(6, 5);
    return 0x02C8D8;
}
static uint32_t counter_two(uint32_t src, uint32_t cell)
{
    charge(6);
    set_d(0, vrd32(src) + vrd32(src + 4));
    set_a(0, cell);
    set_d(6, 6);
    return 0x02C900;
}
static uint32_t rd_2c634(void) { return counter_one(0x100011E0u, 0x9009E81Cu); }   /* FUN_0002c634 */
static uint32_t rd_2c646(void) { return counter_one(0x100011E4u, 0x9009E82Cu); }   /* FUN_0002c646 */
static uint32_t rd_2c658(void) { return counter_two(0x100011E0u, 0x9009E83Cu); }   /* FUN_0002c658 */
static uint32_t rd_2c66e(void) { return counter_one(0x100011F0u, 0x9009E89Cu); }   /* FUN_0002c66e */
static uint32_t rd_2c680(void) { return counter_one(0x100011F4u, 0x9009E8ACu); }   /* FUN_0002c680 */
static uint32_t rd_2c692(void) { return counter_two(0x100011F0u, 0x9009E8BCu); }   /* FUN_0002c692 */
static uint32_t rd_2c6a8(void) { return counter_one(0x100011E8u, 0x9009E91Cu); }   /* FUN_0002c6a8 */
static uint32_t rd_2c6ba(void) { return counter_one(0x100011ECu, 0x9009E92Cu); }   /* FUN_0002c6ba */
static uint32_t rd_2c6cc(void) { return counter_two(0x100011E8u, 0x9009E93Cu); }   /* FUN_0002c6cc */

/* FUN_0002c6e2: the totals of the three even and three odd counters */
static uint32_t rd_2c6e2(void)
{
    uint32_t r;
    set_a(0, 0x100011E0u);
    set_d(0, vrd32(0x100011E0u) + vrd32(0x100011E8u) + vrd32(0x100011F0u));
    set_a(0, 0x9009E99Au);
    set_d(6, 6);
    charge(7);
    if ((r = rd_call(L_2C900, 0x02C6FE))) return r;
    charge(7);
    set_d(0, vrd32(0x100011E4u) + vrd32(0x100011ECu) + vrd32(0x100011F4u));
    set_a(0, 0x9009E9AAu);
    set_d(6, 6);
    return 0x02C900;
}

/* FUN_0002c86a: the total of the six counters at 0x100011E0, printed at
 * 0x9009E5A8 and kept in 0x5242 */
static uint32_t rd_2c86a(void)
{
    uint32_t r, a0 = 0x100011E0u, d5 = 0;
    charge(3);
    for (int n = 5; ; n--) {
        d5 += vrd32(a0); a0 += 4;
        charge(2);
        set_a(0, a0); set_d(5, d5); set_d(1, (uint32_t)(n - 1) & 0xFFFFu);
        if (n == 0) break;
        poll();
    }
    set_a(0, a0);
    set_d(5, d5);
    set_d(1, 0x0000FFFFu);
    set_d(0, d5);
    set_d(6, 6);
    set_a(0, 0x9009E5A8u);
    charge(4);
    if ((r = rd_call(L_2C900, 0x02C888))) return r;
    charge(2);
    w32_set(0x5242, d_reg(5));
    return RD_RTS;
}

/* FUN_0002cb52: page D0 of the records: eight times, the long from
 * 0x50C6 + D0*32 converted (FUN_0000a8c8) and printed in time style
 * (FUN_0002c968) at the cell from the pointer table at 0x2CB82 */
static uint32_t rd_2cb52(void)
{
    uint32_t r;
    uint16_t d0 = dw(0);
    uint32_t a3 = vrd32(0x02CB82 + (uint32_t)((int32_t)(int16_t)d0 * 4));
    uint32_t a2 = W(0x50C6);
    d0 = (uint16_t)(d0 << 5);
    a2 += sx16(d0);
    set_d16(0, d0);
    set_a(3, a3); set_a(2, a2);
    set_d(7, 0);
    charge(6);
    for (;;) {
        a2 = a_reg(2);
        set_d(1, vrd32(a2)); set_a(2, a2 + 4);
        charge(2);
        if ((r = rd_call(L_A8C8, 0x02CB6C))) return r;
        set_d(1, d_reg(0));
        set_d(6, 6);
        set_d(2, 0);
        a3 = a_reg(3);
        set_a(0, vrd32(a3)); set_a(3, a3 + 4);
        charge(5);
        if ((r = rd_call(L_2C968, 0x02CB78))) return r;
        set_d16(7, (uint16_t)(dw(7) + 1));
        charge(3);
        if (!((int16_t)dw(7) < 8)) break;
        poll();
    }
    charge(1);
    return RD_RTS;
}

/* FUN_0002c4fc: the coin page's header rows: row 0 (value 0x5202, factory
 * setting 1, text 0x2CD64 / 0x2CD6B), row 1 (the byte at 0x10001080 against
 * the factory byte at 0x2B832, text 0x2CD72 / 0x2CD79), then the counter
 * 0x5222 as money at 0x9009E21E */
static uint32_t rd_2c4fc(void)
{
    uint32_t r;
    set_a(2, 0x10001080u);
    set_a(3, 0x02B832);
    set_d(4, 0);
    set_d16(5, w16(0x5202));
    w16_set(MENU_DEFAULT, 1);
    charge(6);
    if ((r = rd_call(L_29C68, 0x02C518))) return r;
    set_a(1, 0x02CD64);
    set_d16(5, dw(5) & 1);
    charge(3);
    if (dw(5)) { charge(1); set_a(1, 0x02CD6B); }
    charge(1);
    if ((r = rd_call(L_5A0A, 0x02C52C))) return r;
    set_d(4, 1);
    uint32_t a2 = a_reg(2);
    set_d(5, vrd8(a2)); set_a(2, a2 + 1);
    set_d(0, vrd8(a_reg(3)));
    w16_set(MENU_DEFAULT, dw(0));
    charge(7);
    if ((r = rd_call(L_29C68, 0x02C53E))) return r;
    set_a(1, 0x02CD72);
    set_d16(5, dw(5) & 1);
    charge(3);
    if (dw(5)) { charge(1); set_a(1, 0x02CD79); }
    charge(1);
    if ((r = rd_call(L_5A0A, 0x02C552))) return r;
    w16_set(TEXT_ATTR, 0xC000);
    set_d(1, w32(0x5222));
    charge(3);
    if ((r = rd_call(L_2CBCA, 0x02C560))) return r;
    return money_tail(0x9009E21Eu);
}

/* ======================================================================== */
/* the option / sound page values                                            */
/* ======================================================================== */

/* edit the byte at A1 with the limits table A0 (FUN_00029ba4 or _29bcc);
 * the pattern of FUN_0002d688 / 2e23e / 2e646 / 2da5e / 2f5a8 */
static uint32_t edit_byte(void (*edit)(void), uint32_t ret)
{
    uint32_t r;
    uint32_t a1 = a_reg(1);
    set_d(0, vrd8(a1));
    w16_set(MENU_VALUE, dw(0));
    charge(3);                                          /* moveq, move.b, move.w */
    charge(1);                                          /* bsr */
    if ((r = rd_call(edit, ret))) return r;
    charge(2);
    vwr8(a_reg(1), w8(MENU_VALUE + 1));
    return RD_RTS;
}

/* FUN_0002d688: edit option byte 0x10001060 + row (table 0x2D6AC + row*2) */
static uint32_t rd_2d688(void)
{
    uint16_t d0 = w16(MENU_ROW);
    set_d16(0, d0);
    set_a(0, 0x02D6AC + (uint32_t)((int32_t)(int16_t)d0 * 2));
    set_a(1, 0x10001060u + sx16(d0));
    charge(5);                                          /* move, lea, nop, lea, adda */
    return edit_byte(L_29BA4, 0x02D6A6);
}

/* FUN_0002e23e: edit byte 0x10001040 + row (table 0x2E262 + row*2) */
static uint32_t rd_2e23e(void)
{
    uint16_t d0 = w16(MENU_ROW);
    set_d16(0, d0);
    set_a(0, 0x02E262 + (uint32_t)((int32_t)(int16_t)d0 * 2));
    set_a(1, 0x10001040u + sx16(d0));
    charge(5);
    return edit_byte(L_29BA4, 0x02E25C);
}

/* FUN_0002e646: edit byte 0x1000104B + row - 2 (table 0x2E6E0 + row*4) */
static uint32_t rd_2e646(void)
{
    uint16_t d0 = w16(MENU_ROW);
    set_a(0, 0x02E6E0 + (uint32_t)((int32_t)(int16_t)d0 * 4));
    d0 = (uint16_t)(d0 - 2);
    set_d16(0, d0);
    set_a(1, 0x1000104Bu + sx16(d0));
    charge(5);                                          /* move, lea, lea, subq, adda */
    return edit_byte(L_29BA4, 0x02E666);
}

/* FUN_0002da52: A1 = 0x1000106D + row, then FUN_0002da5e (falls through) */
static uint32_t rd_2da52(void)
{
    uint16_t d0 = w16(MENU_ROW);
    set_d16(0, d0);
    set_a(1, 0x1000106Du + sx16(d0));
    charge(3);
    return 0x02DA5E;
}

/* FUN_0002da5e: edit the byte at A1 with the table 0x2DA76 + D0w*2 */
static uint32_t rd_2da5e(void)
{
    set_a(0, 0x02DA76 + (uint32_t)((int32_t)(int16_t)dw(0) * 2));
    charge(2);                                          /* lea, nop */
    return edit_byte(L_29BCC, 0x02DA70);
}

/* FUN_0002f5a8: edit the byte 0x10001071 with the table 0x2F5C6 */
static uint32_t rd_2f5a8(void)
{
    set_a(0, 0x02F5C6);
    set_a(1, 0x10001071u);
    charge(2);
    return edit_byte(L_29BCC, 0x02F5C0);
}

/* FUN_0002d9da: factory option bytes: 0x10001062..64 = 9, 0x10001065..6C = 4 */
static uint32_t rd_2d9da(void)
{
    uint32_t a1 = 0x10001062u;
    charge(3);
    for (int n = 2; ; n--) { vwr8(a1++, 9); charge(2); set_a(1, a1); set_d(0, (uint32_t)(n - 1) & 0xFFFFu); if (n == 0) break; poll(); }
    charge(1);
    for (int n = 7; ; n--) { vwr8(a1++, 4); charge(2); set_a(1, a1); set_d(0, (uint32_t)(n - 1) & 0xFFFFu); if (n == 0) break; poll(); }
    charge(1);
    set_a(1, a1);
    set_d(0, 0x0000FFFFu);
    return RD_RTS;
}

/* FUN_0002d9f8: copy the 11 factory bytes at 0x2B816 to 0x10001062 */
static uint32_t rd_2d9f8(void)
{
    uint32_t a1 = 0x10001062u, a3 = 0x02B816;
    charge(5);
    for (int n = 10; ; n--) { vwr8(a1++, vrd8(a3++)); charge(2); set_a(1, a1); set_a(3, a3); set_d(0, (uint32_t)(n - 1) & 0xFFFFu); if (n == 0) break; poll(); }
    charge(1);
    set_a(1, a1); set_a(3, a3);
    set_d(0, 0x0000FFFFu);
    return RD_RTS;
}

/* FUN_0002e492: 0x57BA = 1, 0x57BB = 5 */
static uint32_t rd_2e492(void)
{
    charge(3);
    w8_set(0x57BA, 1); w8_set(0x57BB, 5);
    return RD_RTS;
}

/* FUN_0002f3dc: 0x57BA = 0, 0x57BB = 1, or 2 when the byte 0x10001071 is 0 */
static uint32_t rd_2f3dc(void)
{
    charge(4);
    w8_set(0x57BA, 0); w8_set(0x57BB, 1);
    if (vrd8(0x10001071u) == 0) { charge(1); w8_set(0x57BB, 2); }
    charge(1);
    return RD_RTS;
}

/* FUN_0002e9e6: 0x57BA = 1, 0x57BB = 2; when 0xDE0 is 2, 0x57BA = 0 and the
 * text 0x2F17B; when the byte 0x10001071 is 0, 0x57BB = 3 and text 0x2F164 */
static uint32_t rd_2e9e6(void)
{
    uint32_t r;
    charge(4);
    w8_set(0x57BA, 1); w8_set(0x57BB, 2);
    if (w16(0x0DE0) == 2) {
        charge(3);
        w8_set(0x57BA, 0);
        set_a(1, 0x02F17B);
        if ((r = rd_call(L_5A0A, 0x02EA0A))) return r;
    }
    charge(2);
    if (vrd8(0x10001071u) == 0) {
        charge(3);
        w8_set(0x57BB, 3);
        set_a(1, 0x02F164);
        if ((r = rd_call(L_5A0A, 0x02EA22))) return r;
    }
    charge(1);
    return RD_RTS;
}

/* ======================================================================== */
/* the records page and the text/palette set-up                              */
/* ======================================================================== */

/* FUN_0002da12: flag records 1, 6 and 7 as changed (0x500A) and not written
 * (0x5060), reload the record tables (FUN_0002bf66) and the names
 * (FUN_0002bfc0) */
static uint32_t rd_2da12(void)
{
    uint32_t r;
    uint32_t d0 = w32(0x500A) | 0x02 | 0x40 | 0x80;
    w32_set(0x500A, d0);
    d0 = w32(0x5060) & ~(uint32_t)(0x02 | 0x40 | 0x80);
    w32_set(0x5060, d0);
    set_d(0, d0);
    charge(11);
    if ((r = rd_call(L_2BF66, 0x02DA3E))) return r;
    charge(1);
    return RD_JMP(0x02BFC0);
}

/* FUN_0002bf66: factory records: for the two transmissions, the four course
 * best laps (longs 0x283CE..) to 0x50C6.. and 0x5106 + (t*4+c)*4, then the
 * four best totals picked by (byte 0x10001069+c) mod 5 from the table at
 * 0x283EE + c*20 to 0x50C6.. and 0x5126 + (t*4+c)*4 */
static uint32_t rd_2bf66(void)
{
    uint32_t a0 = 0x0283CE, a1 = 0x0283EE, a3 = W(0x50C6), a2 = a_reg(2), d0 = d_reg(0), d1 = 0;
    uint16_t d7 = 0;
    charge(4);
    for (;;) {
        d1 = (uint32_t)d7;
        d1 = (d1 & 0xFFFF0000u) | (uint16_t)(d1 << 2);
        charge(3);
        for (int n = 3; ; n--) {
            d0 = vrd32(a0); a0 += 4;
            vwr32(a3, d0); a3 += 4;
            vwr32(W(0x5106) + (uint32_t)((int32_t)(int16_t)d1 * 4), d0);
            d1 = (d1 & 0xFFFF0000u) | (uint16_t)(d1 + 1);
            charge(5);
            set_d(0, d0); set_d(1, d1); set_d(6, (uint32_t)(n - 1) & 0xFFFFu); set_d(7, d7);
            set_a(0, a0); set_a(1, a1); set_a(3, a3);
            if (n == 0) break;
            poll();
        }
        a2 = 0x10001069u;
        d1 = (uint32_t)d7;
        d1 = (d1 & 0xFFFF0000u) | (uint16_t)(d1 << 2);
        charge(4);
        for (int n = 3; ; n--) {
            uint32_t b = vrd8(a2++);
            uint32_t rem = b % 5;
            d0 = vrd32(a1 + rem * 4);
            vwr32(a3, d0); a3 += 4;
            vwr32(W(0x5126) + (uint32_t)((int32_t)(int16_t)d1 * 4), d0);
            d1 = (d1 & 0xFFFF0000u) | (uint16_t)(d1 + 1);
            a1 += 0x14;
            charge(10);
            set_d(0, d0); set_d(1, d1); set_d(6, (uint32_t)(n - 1) & 0xFFFFu); set_d(7, d7);
            set_a(0, a0); set_a(1, a1); set_a(2, a2); set_a(3, a3);
            if (n == 0) break;
            poll();
        }
        d7++;
        charge(3);
        set_d(7, d7);
        if (!((int16_t)d7 < 2)) break;
        poll();
    }
    charge(1);
    set_d(0, d0); set_d(1, d1);
    set_d(6, 0x0000FFFFu);
    set_d(7, d7);
    set_a(0, a0); set_a(1, a1); set_a(2, a2); set_a(3, a3);
    return RD_RTS;
}

/* FUN_0002bfc0: factory record names: for the four courses, the long picked
 * by (byte 0x10001069+c) mod 5 from 0x2843E + c*20 to 0x593E + c*4, and three
 * bytes from 0x2848E (widened to words) to 0x594E + c*8 */
static uint32_t rd_2bfc0(void)
{
    uint32_t a0 = 0x02843E, a1 = 0x02848E, a2 = 0x10001069u, a3 = W(0x593E), a4 = W(0x594E);
    uint32_t d0 = 0, d1 = 0;
    charge(7);
    for (int n = 3; ; n--) {
        uint32_t b = vrd8(a2++);
        d0 = (b / 5) << 16 | (b % 5);                  /* divu + swap: quotient high, remainder low */
        vwr32(a3, vrd32(a0 + (uint32_t)((int32_t)(int16_t)d0 * 4))); a3 += 4;
        for (int k = 0; k < 3; k++) {
            d1 = (d1 & 0xFFFFFF00u) | vrd8(a1++);
            vwr16(a4, (uint16_t)d1); a4 += 2;
        }
        a0 += 0x14;
        a4 += 2;
        charge(14);
        set_d(0, d0); set_d(1, d1); set_d(7, (uint32_t)(n - 1) & 0xFFFFu);
        set_a(0, a0); set_a(1, a1); set_a(2, a2); set_a(3, a3); set_a(4, a4);
        if (n == 0) break;
        poll();
    }
    charge(1);
    set_d(0, d0); set_d(1, d1);
    set_d(7, 0x0000FFFFu);
    set_a(0, a0); set_a(1, a1); set_a(2, a2); set_a(3, a3); set_a(4, a4);
    return RD_RTS;
}

/* FUN_0002d15e: the last palette entries of the three colour banks = 0xFF */
static uint32_t rd_2d15e(void)
{
    charge(4);
    vwr8(0x9002FFE8u, 0xFF); vwr8(0x90037FE8u, 0xFF); vwr8(0x9003FFE8u, 0xFF);
    return RD_RTS;
}

/* FUN_0002d178: those entries = 0, and a 6 x 8 block of tile 0xE00C at
 * 0x9009E620 (rows 0x80 bytes apart) */
static uint32_t rd_2d178(void)
{
    vwr8(0x9002FFE8u, 0); vwr8(0x90037FE8u, 0); vwr8(0x9003FFE8u, 0);
    uint32_t a1 = 0x9009E620u;
    set_d16(2, 0xE00C);
    charge(6);
    for (int row = 5; ; row--) {
        charge(1);
        for (int n = 7; ; n--) { vwr16(a1, 0xE00C); a1 += 2; charge(2); set_d16(0, (uint16_t)(n - 1)); set_d16(1, (uint16_t)row); set_a(1, a1); if (n == 0) break; poll(); }
        a1 += 0x70;
        charge(2);
        set_d16(1, (uint16_t)(row - 1)); set_a(1, a1);
        if (row == 0) break;
        poll();
    }
    charge(1);
    set_a(1, a1);
    set_d16(0, 0xFFFF);
    set_d16(1, 0xFFFF);
    return RD_RTS;
}

/* FUN_0002cef0: test-mode colour ramps: 16 steps of red/green/blue/white
 * (component 0xFF..0x00 by 0x11) in the three banks at 0x9002FF40 /
 * 0x90037F40 / 0x9003FF40, then eight fixed colours at +0xA0 */
static uint32_t rd_2cef0(void)
{
    uint32_t a0 = 0x9002FF40u, a1 = 0x90037F40u, a2 = 0x9003FF40u;
    uint32_t d1 = 0xFFFFFFFFu;
    set_d16(0, 0xF);
    charge(6);
    for (int n = 15; ; n--) {
        uint8_t v = (uint8_t)d1;
        vwr8(a0 + 0x10, v); vwr8(a1 + 0x10, 0); vwr8(a2 + 0x10, 0);
        vwr8(a0 + 0x20, 0); vwr8(a1 + 0x20, 0); vwr8(a2 + 0x20, v);
        vwr8(a0 + 0x30, v); vwr8(a1 + 0x30, v); vwr8(a2 + 0x30, v);
        vwr8(a0++, 0); vwr8(a1++, v); vwr8(a2++, 0);
        d1 = (d1 & 0xFFFFFF00u) | (uint8_t)(v - 0x11);
        charge(14);
        set_d16(0, (uint16_t)(n - 1)); set_d(1, d1); set_d(2, 0); set_a(0, a0); set_a(1, a1); set_a(2, a2);
        if (n == 0) break;
        poll();
    }
    static const uint8_t fixed[8][3] = {       /* per colour: bank 0, 1, 2 (1 = 0x11) */
        { 0, 1, 0 }, { 0, 0, 0 }, { 1, 0, 0 }, { 0, 0, 0 },
        { 0, 0, 1 }, { 0, 0, 0 }, { 1, 1, 1 }, { 0, 0, 0 } };
    a0 = 0x9002FFE0u; a1 = 0x90037FE0u; a2 = 0x9003FFE0u;
    for (int i = 0; i < 8; i++) {
        vwr8(a0++, fixed[i][0] ? 0x11 : 0);
        vwr8(a1++, fixed[i][1] ? 0x11 : 0);
        vwr8(a2++, fixed[i][2] ? 0x11 : 0);
    }
    charge(30);
    set_d16(0, 0xFFFF);
    set_d(1, 0x11); set_d(2, 0);
    set_a(0, a0); set_a(1, a1); set_a(2, a2);
    return RD_RTS;
}

/* FUN_0002cf84: clear the tilemap to tile 0xE0D1, the four corner tiles, and
 * 4 x 7 rows of 35 tiles (the words at 0x2D004 + attribute 0x40C0, +0x1000
 * per group) each followed by five words from the table at 0x2D04A */
static uint32_t rd_2cf84(void)
{
    uint32_t a1 = 0x9009E000u;
    set_d16(1, 0xE0D1);
    charge(3);
    for (int n = 0xFFF; ; n--) { vwr16(a1, 0xE0D1); a1 += 2; charge(2); set_d16(0, (uint16_t)(n - 1)); set_a(1, a1); if (n == 0) break; poll(); }
    vwr16(0x9009E000u, 0xE0C0); vwr16(0x9009E04Eu, 0xE0C1);
    vwr16(0x9009EE80u, 0xE0C2); vwr16(0x9009EECEu, 0xE0C3);
    a1 = 0x9009E080u;
    uint16_t d3 = 0x40C0, d4 = dw(4);
    uint32_t a4 = 0x02D04A, a3 = 0, a0 = 0;
#define SYNC_2CF84() (set_d16(3, d3), set_d16(4, d4), set_a(0, a0), set_a(1, a1), set_a(3, a3), set_a(4, a4))
    charge(8);
    for (int g = 3; ; g--) {
        charge(1);
        for (int row = 6; ; row--) {
            a3 = a4; a0 = 0x02D004;
            charge(4);
            for (int n = 0x22; ; n--) {
                d4 = (uint16_t)(vrd16(a0) + d3); a0 += 2;
                vwr16(a1, d4); a1 += 2;
                charge(4);
                set_d16(0, (uint16_t)(n - 1)); set_d16(1, (uint16_t)row); set_d16(2, (uint16_t)g); SYNC_2CF84();
                if (n == 0) break;
                poll();
            }
            charge(1);
            for (int n = 4; ; n--) { vwr16(a1, vrd16(a3)); a1 += 2; a3 += 2; charge(2); set_d16(0, (uint16_t)(n - 1)); SYNC_2CF84(); if (n == 0) break; poll(); }
            a1 += 0x30;
            charge(2);
            set_d16(1, (uint16_t)(row - 1)); SYNC_2CF84();
            if (row == 0) break;
            poll();
        }
        d3 = (uint16_t)(d3 + 0x1000);
        a4 += 0xA;
        charge(3);
        set_d16(2, (uint16_t)(g - 1)); SYNC_2CF84();
        if (g == 0) break;
        poll();
    }
    charge(1);
    set_d16(0, 0xFFFF); set_d16(1, 0xFFFF); set_d16(2, 0xFFFF);
    set_d16(3, d3); set_d16(4, d4);
    set_a(0, a0); set_a(1, a1); set_a(3, a3); set_a(4, a4);
#undef SYNC_2CF84
    return RD_RTS;
}

/* FUN_0002d072: the frame of the test screen: 15 pairs of rows of alternating
 * tiles, bottom row, two side columns, a divider row and two single tiles */
static uint32_t rd_2d072(void)
{
    uint32_t a1 = 0x9009E000u;
    uint16_t d0 = dw(0), d2 = 0, d3 = 0;
    charge(1);
    /* 15 x (row of E0C0/E0C4 ending E0C1, row of E0C6/E0DE ending E0C7) */
    set_d16(1, 0xE);
    charge(1);
    for (int k = 14; ; k--) {
        d3 = 0xE0C0; d2 = 0xE0C4;
        charge(3);
        for (int n = 0x13; ; n--) { vwr16(a1, d3); vwr16(a1 + 2, d2); a1 += 4; charge(3); set_d16(0, (uint16_t)(n - 1)); set_d16(2, d2); set_d16(3, d3); set_a(1, a1); if (n == 0) break; poll(); }
        vwr16(a1 - 2, 0xE0C1);
        a1 += 0x30;
        d3 = 0xE0C6; d2 = 0xE0DE;
        charge(5);
        for (int n = 0x13; ; n--) { vwr16(a1, d3); vwr16(a1 + 2, d2); a1 += 4; charge(3); set_d16(0, (uint16_t)(n - 1)); set_d16(2, d2); set_d16(3, d3); set_a(1, a1); if (n == 0) break; poll(); }
        vwr16(a1 - 2, 0xE0C7);
        a1 += 0x30;
        charge(3);
        set_d16(1, (uint16_t)(k - 1)); set_a(1, a1);
        if (k == 0) break;
        poll();
    }
    set_d16(1, 0xFFFF);
    a1 = 0x9009EE80u;
    d3 = 0xE0C2; d2 = 0xE0C5;
    charge(4);
    for (int n = 0x13; ; n--) { vwr16(a1, d3); vwr16(a1 + 2, d2); a1 += 4; charge(3); set_d16(0, (uint16_t)(n - 1)); set_d16(2, d2); set_d16(3, d3); set_a(1, a1); if (n == 0) break; poll(); }
    vwr16(a1 - 2, 0xE0C3);
    for (int col = 0; col < 2; col++) {
        a1 = col ? 0x9009E02Au : 0x9009E026u;
        d3 = 0xE0C0; d2 = 0xE0C6;
        charge(5);
        for (int n = 0xE; ; n--) {
            vwr16(a1, d3); a1 += 0x80;
            vwr16(a1, d2); a1 += 0x80;
            charge(5);
            set_d16(0, (uint16_t)(n - 1)); set_d16(2, d2); set_d16(3, d3); set_a(1, a1);
            if (n == 0) break;
            poll();
        }
        vwr16(a1 - 0x80, 0xE0C2);
    }
    a1 = 0x9009E780u;
    d3 = 0xE0C0; d2 = 0xE0C4;
    charge(5);
    for (int n = 0x13; ; n--) { vwr16(a1, d3); vwr16(a1 + 2, d2); a1 += 4; charge(3); set_d16(0, (uint16_t)(n - 1)); set_d16(2, d2); set_d16(3, d3); set_a(1, a1); if (n == 0) break; poll(); }
    vwr16(a1 - 2, 0xE0C1);
    vwr16(0x9009E7A6u, 0xE0C0); vwr16(0x9009E7AAu, 0xE0C0);
    charge(4);
    (void)d0;
    set_d16(0, 0xFFFF); set_d16(1, 0xFFFF);
    set_d16(2, d2); set_d16(3, d3);
    set_a(1, a1);
    return RD_RTS;
}


/* ======================================================================== */
/* test-mode page rows                                                       */
/* ======================================================================== */

/* the print-one-of-two idiom: lea text_a,A1 ; andi.w #1,D5w ; beq ;
 * lea text_b,A1 ; jsr printer -- text_b when bit 0 of D5 is set */
static uint32_t print_bit0(uint32_t even, uint32_t odd, uint32_t ret)
{
    set_a(1, even);
    set_d16(5, dw(5) & 1);
    charge(3);
    if (dw(5)) { charge(1); set_a(1, odd); }
    charge(1);
    return rd_call(L_5A0A, ret);
}

/* FUN_0002c71c: the test-mode BOOKKEEPING page, a three-state machine on
 * 0xDAC (table 0x2C72E): 0 = draw the page (texts from 0x2CD88 until an
 * empty one, both record pages FUN_0002cb52), select row 0, go to state 1;
 * 1 = edit the row (FUN_00029ba4 with limits 0x2C798; bit 7 of 0x812 moves
 * to state 2), then redraw it (FUN_00029bf4, text table 0x2CD80);
 * 2 = act on the row: row 0 leaves the page (0x5000 = 1), row 1 jumps to
 * 0x2C4C8 */
static uint32_t rd_2c71c(void)
{
    uint32_t r;
    uint16_t st = (uint16_t)(w16(0x0DAC) & 3);
    set_d16(0, (uint16_t)vrd16(0x02C72E + 2u * st));
    charge(5);                                          /* move, andi, move, nop, jmp */
    poll();
    if (st == 1) {
        charge(2);
        if (w8(-0x77EE) & 0x80) { charge(1); w16_set(0x0DAC, 2); }
        charge(2);
        set_a(0, 0x02C798);
        if ((r = rd_call(L_29BA4, 0x02C786))) return r;
        charge(4);
        w16_set(MENU_ROW, w16(MENU_VALUE));
        set_a(1, 0x02CD80);
        w32_set(0x0DBA, 0x02CD80);
        return RD_JMP(0x029BF4);                                /* bra.w FUN_00029bf4 */
    }
    if (st == 2) {
        uint16_t row = (uint16_t)(w16(MENU_ROW) & 1);
        set_d16(0, (uint16_t)vrd16(0x02C7AE + 2u * row));
        charge(5);
        if (row) return RD_JMP(0x02C4C8);
        poll();
        charge(4);
        w16_set(0x0DAC, 0); w16_set(MENU_VALUE, 0); w16_set(0x5000, 1);
        return RD_RTS;
    }
    /* states 0 and 3 */
    charge(1);
    if ((r = rd_call(L_575A, 0x02C73C))) return r;
    charge(2);
    w16_set(TEXT_ATTR, 0xC000);
    set_a(1, 0x02CD88);
    for (;;) {
        charge(1);
        if ((r = rd_call(L_5A0A, 0x02C74C))) return r;
        charge(2);
        if (vrd8(a_reg(1)) == 0) break;
        poll();
    }
    set_d(0, 0);
    charge(2);
    if ((r = rd_call(L_2CB52, 0x02C756))) return r;
    set_d(0, 1);
    charge(2);
    if ((r = rd_call(L_2CB52, 0x02C75C))) return r;
    charge(4);
    w16_set(MENU_VALUE, 0); w16_set(MENU_ROW, 0); w16_set(0x0DAC, 1);
    return RD_RTS;
}

/* FUN_0002d6c8: the option page: player car (0x10001060, changing it resets
 * the car tables 0x11A0/0x1158/0x115A/0x115C), its colour and the car model
 * preview (FUN_0002db9c), three level rows (texts from 0x2DF4C, value +0x40),
 * eight rows of two-digit numbers from the pair table 0x2D7BE, and the
 * "free play"/"coins" line chosen by any bit set in 0x1104 */
static uint32_t rd_2d6c8(void)
{
    uint32_t r;
    set_a(2, 0x10001060u);
    set_a(3, 0x02B814);
    set_d(4, 0);
    set_d8(5, (uint8_t)vrd8(0x10001060u)); set_a(2, 0x10001061u);
    charge(5);
    if ((r = rd_call(L_29C42, 0x02D6DC))) return r;
    charge(2);
    if ((uint8_t)d_reg(5) != w8(0x11A1)) {
        uint16_t v = dw(5);
        set_d16(1, (uint16_t)(v + v));
        w16_set(0x11A0, v);
        uint16_t d0 = (uint16_t)(v + v);
        w16_set(0x1158, d0);
        d0 = (uint16_t)(d0 << 3); w16_set(0x115A, d0);
        d0 = (uint16_t)(d0 << 2); w16_set(0x115C, d0);
        set_d16(0, d0);
        charge(10);
    }
    set_d8(5, (uint8_t)(d_reg(5) + 1));
    set_a(1, 0x02DEAC);
    charge(3);
    if ((r = rd_call(L_5A0A, 0x02D70A))) return r;
    set_d(4, 1);
    { uint32_t a2 = a_reg(2); set_d8(5, (uint8_t)vrd8(a2)); set_a(2, a2 + 1); }
    charge(3);
    if ((r = rd_call(L_29C42, 0x02D712))) return r;
    charge(1);
    if ((r = rd_call(L_2DB9C, 0x02D716))) return r;
    set_d8(1, (uint8_t)d_reg(5));
    set_d16(1, (uint16_t)((dw(1) & 7) << 4));
    set_a(1, 0x02DEC9 + sx16(dw(1)));
    charge(6);                                          /* move.b, andi, lsl, lea, nop, lea */
    charge(1);
    if ((r = rd_call(L_5A0A, 0x02D72E))) return r;
    set_d8(5, (uint8_t)(d_reg(5) + 1));
    set_a(1, 0x02DEC3);
    charge(3);
    if ((r = rd_call(L_5A0A, 0x02D73A))) return r;
    set_a(3, 0x02B814);
    set_a(4, 0x02DF4C);
    set_d(4, 2);
    charge(3);
    for (;;) {
        { uint32_t a2 = a_reg(2); set_d8(5, (uint8_t)vrd8(a2)); set_a(2, a2 + 1); }
        charge(2);
        if ((r = rd_call(L_29C42, 0x02D74C))) return r;
        { uint32_t a4 = a_reg(4); set_a(1, vrd32(a4)); set_a(4, a4 + 4); }
        set_d8(5, (uint8_t)(d_reg(5) + 0x40));
        charge(3);
        if ((r = rd_call(L_5A0A, 0x02D758))) return r;
        set_d16(4, (uint16_t)(dw(4) + 1));
        charge(3);
        if (!((int16_t)dw(4) <= 4)) break;
        poll();
    }
    set_a(4, 0x02DF58);
    set_d(4, 5);
    charge(2);
    for (;;) {
        { uint32_t a2 = a_reg(2); set_d8(5, (uint8_t)vrd8(a2)); set_a(2, a2 + 1); }
        charge(2);
        if ((r = rd_call(L_29C42, 0x02D76C))) return r;
        uint32_t a4 = a_reg(4), a1 = vrd32(a4);
        set_a(4, a4 + 4);
        uint32_t idx = (uint32_t)((int32_t)(int16_t)dw(5) * 2);
        uint16_t attr = w16(TEXT_ATTR);
        uint32_t d0 = vrd8(0x02D7BE + idx);
        vwr16(a1, (uint16_t)(d0 + attr));
        d0 = vrd8(0x02D7BF + idx);
        d0 = (uint16_t)(d0 + attr);
        vwr16(a1 + 2, d0);
        set_a(1, a1 + 4);
        set_d(0, d0);
        set_d16(4, (uint16_t)(dw(4) + 1));
        charge(14);
        if (!((int16_t)dw(4) <= 0xC)) break;
        poll();
    }
    w16_set(TEXT_ATTR, 0xC000);
    set_d(1, 0);
    uint16_t d0 = w16(0x1104);
    uint16_t d5 = 0;
    charge(5);
    for (int n = 7; ; n--) {
        d0 = (uint16_t)((d0 >> 1) | (d0 << 15));
        charge(3);
        if (d0 & 0x8000) { charge(1); d5++; }
        set_d16(0, d0); set_d(5, d5); set_d(6, (uint32_t)(n - 1) & 0xFFFFu);
        if (n == 0) break;
        poll();
    }
    set_d16(0, d0);
    set_d(6, 0x0000FFFFu);
    set_d(5, d5);
    set_a(1, 0x02DEB2);
    charge(3);
    if (d5 == 0) { charge(1); set_a(1, 0x02DEBB); }
    charge(1);
    return RD_JMP(PRINT_TEXT);
}


/* the sin/cos pair the display lists use: A5 is the sine table (a word per
 * 1/65536 turn step of 2); D1w = -sin(a), D2w = cos(a) (sin a quarter turn on),
 * D0w ends a + 0x4000 */
static void sincos_a5(uint32_t *d0, uint32_t *d1, uint32_t *d2)
{
    uint32_t a5 = a_reg(5);
    *d0 = (*d0 & 0xFFFF0000u) | (uint16_t)(*d0 & 0xFFFE);
    *d1 = (*d1 & 0xFFFF0000u) | (uint16_t)vrd16(a5 + sx16(*d0));
    *d0 = (*d0 & 0xFFFF0000u) | (uint16_t)(*d0 + 0x4000);
    *d2 = (*d2 & 0xFFFF0000u) | (uint16_t)vrd16(a5 + sx16(*d0));
    *d1 = (*d1 & 0xFFFF0000u) | (uint16_t)(-(int32_t)(uint16_t)*d1);
}

/* FUN_0002db9c: the option page's car preview: a display list (header
 * 0x8001) of the car's two model halves, drawn twice at the two positions of
 * the table at 0x2DCA8 (angle, x, y, z per entry), the model ids from the
 * per-car row of 0x2DCC8 (D5 & 15); each copy also gets a third part offset
 * by the scaled sine/cosine of its angle */
static uint32_t rd_2db9c(void)
{
    uint32_t a4 = 0x02DCC8 + (uint32_t)((int32_t)(int16_t)(dw(5) & 0xF) * 8);
    set_d16(5, dw(5) & 0xF);
    uint32_t a3 = w32(DL_CURSOR), a1 = 0x02DCA8;
    uint32_t d0 = d_reg(0), d1 = d_reg(1), d2 = d_reg(2), d3 = d_reg(3), d7 = d_reg(7);
    vwr32(a3, 0x8001); vwr32(a3 + 4, 0); a3 += 8;
    charge(10);
    for (int k = 1; ; k--) {
        d0 = vrd32(a1);
        sincos_a5(&d0, &d1, &d2);
        d7 = 1;
        charge(7);
        for (int j = 1; ; j--) {
            d3 = vrd16(a4 + 2u * (uint32_t)j);
            vwr32(a3, d3); vwr32(a3 + 4, vrd32(a1 + 4)); vwr32(a3 + 8, vrd32(a1 + 8)); vwr32(a3 + 12, vrd32(a1 + 12));
            vwr32(a3 + 16, 0); vwr32(a3 + 20, 0x7FFF); vwr32(a3 + 24, d1); vwr32(a3 + 28, d2);
            vwr32(a3 + 32, 0); vwr32(a3 + 36, 0x7FFF); vwr32(a3 + 40, 0);
            a3 += 44;
            charge(14);
            set_d(0, d0); set_d(1, d1); set_d(2, d2); set_d(3, d3); set_d(6, (uint32_t)k);
            set_d(7, (uint32_t)(j - 1) & 0xFFFFu); set_a(1, a1); set_a(3, a3); set_a(4, a4);
            if (j == 0) break;
            poll();
        }
        d7 = 0x0000FFFFu;
        d3 = vrd16(a4 + 2);
        vwr32(a3, d3); a3 += 4;
        d0 = vrd32(a1);
        sincos_a5(&d0, &d1, &d2);
        d3 = vrd16(a4 + 4);
        d1 = (uint32_t)((int32_t)(int16_t)d1 * (int16_t)d3);
        d1 = d1 * 2; d1 = (d1 << 16) | (d1 >> 16);
        d2 = (uint32_t)((int32_t)(int16_t)d2 * (int16_t)d3);
        d2 = d2 * 2; d2 = (d2 << 16) | (d2 >> 16);
        d1 = sx16(d1); d2 = sx16(d2);
        vwr32(a3, vrd32(a1 + 4) + d1);
        vwr32(a3 + 8, vrd32(a1 + 12) + d2);
        vwr32(a3 + 4, vrd32(a1 + 8));
        a3 += 12;
        vwr32(a3, 0); vwr32(a3 + 4, 0x7FFF); a3 += 8;
        d0 = vrd32(a1);
        sincos_a5(&d0, &d1, &d2);
        vwr32(a3, d1); vwr32(a3 + 4, d2); vwr32(a3 + 8, 0); vwr32(a3 + 12, 0x7FFF); vwr32(a3 + 16, 0);
        a3 += 20;
        a1 += 0x10;
        charge(40);
        set_d(0, d0); set_d(1, d1); set_d(2, d2); set_d(3, d3); set_d(6, (uint32_t)(k - 1) & 0xFFFFu);
        set_d(7, d7); set_a(1, a1); set_a(3, a3); set_a(4, a4);
        if (k == 0) break;
        poll();
    }
    charge(3);
    vwr32(a3, 0xFFFFFFFFu);
    w32_set(DL_CURSOR, a3);
    set_d(0, d0); set_d(1, d1); set_d(2, d2); set_d(3, d3);
    set_d(6, 0x0000FFFFu); set_d(7, d7);
    set_a(1, a1); set_a(3, a3); set_a(4, a4);
    return RD_RTS;
}

/* FUN_0002da84: the second option page: four on/off rows from 0x1000106D
 * (texts 0x2E0A4.. by bit 0), the row for 0x56EC, and two rows whose
 * factory setting is the byte 0x2B821 + row - 1 */
static uint32_t rd_2da84(void)
{
    uint32_t r;
    static const uint32_t txt[4][3] = {
        { 0x02E0A4, 0x02E0AC, 0x02DAAC }, { 0x02E0B4, 0x02E0BB, 0x02DAC8 },
        { 0x02E0C2, 0x02E0C9, 0x02DAE4 }, { 0x02E0D0, 0x02E0D7, 0x02DB00 } };
    static const uint32_t cret[4] = { 0x02DA98, 0x02DAB4, 0x02DAD0, 0x02DAEC };
    set_a(2, 0x1000106Du);
    set_a(3, 0x02B821);
    charge(2);
    for (int row = 0; row < 4; row++) {
        set_d(4, (uint32_t)row);
        uint32_t a2 = a_reg(2); set_d8(5, (uint8_t)vrd8(a2)); set_a(2, a2 + 1);
        charge(3);
        if ((r = rd_call(L_29C42, cret[row]))) return r;
        if ((r = print_bit0(txt[row][0], txt[row][1], txt[row][2]))) return r;
    }
    set_d(4, 4);
    set_d16(5, w16(0x56EC));
    w16_set(MENU_DEFAULT, 1);
    charge(4);
    if ((r = rd_call(L_29C68, 0x02DB10))) return r;
    if ((r = print_bit0(0x02E0DE, 0x02E0E5, 0x02DB24))) return r;
    set_d(4, 5);
    set_d(5, 0);
    { uint32_t a2 = a_reg(2); set_d8(5, (uint8_t)vrd8(a2)); set_a(2, a2 + 1); }
    set_d(0, vrd8(a_reg(3) + sx16(dw(4)) - 1));
    w16_set(MENU_DEFAULT, dw(0));
    charge(7);
    if ((r = rd_call(L_29C68, 0x02DB38))) return r;
    if ((r = print_bit0(0x02E0EC, 0x02E0F3, 0x02DB4C))) return r;
    set_d(4, 6);
    set_d(5, 0);
    { uint32_t a2 = a_reg(2); set_d8(5, (uint8_t)vrd8(a2)); set_a(2, a2 + 1); }
    set_d(0, vrd8(a_reg(3) + sx16(dw(4)) - 1));
    w16_set(MENU_DEFAULT, dw(0));
    charge(7);
    if ((r = rd_call(L_29C68, 0x02DB60))) return r;
    set_a(1, 0x02E0FA);
    set_d16(5, dw(5) & 1);
    charge(3);
    if (dw(5)) { charge(1); set_a(1, 0x02E101); }
    charge(1);
    return RD_JMP(PRINT_TEXT);
}

/* FUN_0002db74: copy the 55 default words at ROM 0x5204 to the start of
 * WRAM (A6), then continue at 0x5092 with D1 = 0 */
static uint32_t rd_2db74(void)
{
    uint32_t a0 = 0x5204, a1 = a_reg(6);
    charge(7);
    for (int n = 0x36; ; n--) {
        vwr16(a1, vrd16(a0)); a0 += 2; a1 += 2;
        charge(2);
        set_a(0, a0); set_a(1, a1); set_d(7, (uint32_t)(n - 1) & 0xFFFFu);
        if (n == 0) break;
        poll();
    }
    set_a(0, a0); set_a(1, a1);
    set_d(7, 0x0000FFFFu);
    set_d(1, 0);
    charge(2);
    return RD_JMP(0x005092);
}

/* FUN_0002e26c: the coin option page: three rows from 0x10001040 (label,
 * then "1 coin"/"n coins" text by value == 1) and the last row by bit 0;
 * factory settings from 0x2B7F6 (0x2B8C8 when 0xDDE is set) */
static uint32_t rd_2e26c(void)
{
    uint32_t r;
    static const uint32_t lbl[3] = { 0x02E3D4, 0x02E3DA, 0x02E3E0 };
    static const uint32_t r1[3] = { 0x02E28C, 0x02E2B8, 0x02E2E4 };
    static const uint32_t r2[3] = { 0x02E296, 0x02E2C2, 0x02E2EE };
    static const uint32_t r3[3] = { 0x02E2B0, 0x02E2DC, 0x02E308 };
    set_a(2, 0x10001040u);
    set_a(3, 0x02B7F6);
    charge(4);
    if (w16(0x0DDE)) { charge(1); set_a(3, 0x02B8C8); }
    for (int row = 0; row < 3; row++) {
        set_d(4, (uint32_t)row);
        uint32_t a2 = a_reg(2); set_d8(5, (uint8_t)vrd8(a2)); set_a(2, a2 + 1);
        charge(3);
        if ((r = rd_call(L_29C42, r1[row]))) return r;
        set_a(1, lbl[row]);
        charge(2);
        if ((r = rd_call(L_5A0A, r2[row]))) return r;
        set_a(1, 0x02E3E6);
        charge(3);
        if ((uint8_t)d_reg(5) != 1) { charge(1); set_a(1, 0x02E3EC); }
        w16_set(TEXT_ATTR, 0xC000);
        charge(2);
        if ((r = rd_call(L_5A0A, r3[row]))) return r;
    }
    set_d(4, 3);
    { uint32_t a2 = a_reg(2); set_d8(5, (uint8_t)vrd8(a2)); set_a(2, a2 + 1); }
    charge(3);
    if ((r = rd_call(L_29C42, 0x02E310))) return r;
    set_a(1, 0x02E3F2);
    set_d8(5, (uint8_t)(d_reg(5) & 1));
    charge(3);
    if (!(d_reg(5) & 1)) { charge(1); set_a(1, 0x02E3F9); }
    charge(1);
    return RD_JMP(PRINT_TEXT);
}

/* FUN_0002e6fc: the sound page: two volume rows (0x1000104B.., labels from
 * 0x2E914), the row for 0x577E, the sound-name line (FUN_0002e7e6), then the
 * two volumes sent to the sound CPU (words 0x60004022/26 and 0x60004024/28,
 * each byte also copied two on); when 0xDD8 runs out and 0x56EE is clear the
 * sound command 0x60005000 is cleared */
static uint32_t rd_2e6fc(void)
{
    uint32_t r;
    set_a(2, 0x1000104Bu);
    set_a(3, 0x02B801);
    charge(4);
    if (w16(0x0DDE)) { charge(1); set_a(3, 0x02B8D3); }
    set_a(4, 0x02E914);
    set_d(4, 2);
    charge(2);
    for (;;) {
        uint32_t a2 = a_reg(2); set_d8(5, (uint8_t)vrd8(a2)); set_a(2, a2 + 1);
        charge(2);
        if ((r = rd_call(L_29C42, 0x02E720))) return r;
        uint32_t a4 = a_reg(4); set_a(1, vrd32(a4)); set_a(4, a4 + 4);
        charge(2);
        if ((r = rd_call(L_5A0A, 0x02E728))) return r;
        set_d16(4, (uint16_t)(dw(4) + 1));
        charge(3);
        if (!((int16_t)dw(4) <= 3)) break;
        poll();
    }
    set_a(2, a_reg(2) + 2);
    set_a(3, a_reg(3) + 2);
    set_d(4, 5);
    set_d(0, 0);
    w16_set(MENU_DEFAULT, 0);
    set_d16(0, w16(0x577E));
    set_d16(5, dw(0));
    charge(8);
    if ((r = rd_call(L_29C68, 0x02E746))) return r;
    set_a(1, 0x02E932);
    charge(2);
    if ((r = rd_call(L_5A0A, 0x02E750))) return r;
    charge(1);
    if ((r = rd_call(L_2E7E6, 0x02E754))) return r;
    uint32_t d0 = d_reg(0) & 0xFFFF0000u;
    uint8_t b = (uint8_t)vrd8(0x1000104Bu);
    d0 |= b;
    vwr8(0x1000104Du, b);
    vwr16(0x60004022u, (uint16_t)d0); vwr16(0x60004026u, (uint16_t)d0);
    b = (uint8_t)vrd8(0x1000104Cu);
    d0 = (d0 & 0xFFFFFF00u) | b;
    vwr8(0x1000104Eu, b);
    vwr16(0x60004024u, (uint16_t)d0); vwr16(0x60004028u, (uint16_t)d0);
    set_d(0, d0);
    set_a(1, 0x1000104Du);
    uint16_t t = (uint16_t)(w16(0x0DD8) - 1);
    w16_set(0x0DD8, t);
    charge(12);
    if ((int16_t)t < 0) {
        charge(2);
        if (w16(0x56EE) == 0) {
            charge(2);
            vwr16(0x60005000u, 0);
            w16_set(0x0DD8, 0);
        }
    }
    charge(1);
    return RD_RTS;
}

/* FUN_0002e814: copy the sound name (NUL-terminated, 24 bytes at most) from
 * the sound CPU's words at 0x60005300 to 0x578B, high byte first */
static uint32_t rd_2e814(void)
{
    uint32_t a0 = 0x60005300u, a1 = W(0x578B), d0 = d_reg(0);
    charge(3);
    int n = 0xB;
    for (;;) {
        d0 = (d0 & 0xFFFF0000u) | (uint16_t)vrd16(a0);
        vwr8(a1++, (uint8_t)d0);
        charge(3);
        if ((uint8_t)d0 == 0) break;
        d0 = (d0 & 0xFFFF0000u) | (uint16_t)(((uint16_t)d0 << 8) | ((uint16_t)d0 >> 8));
        vwr8(a1++, (uint8_t)d0);
        charge(3);
        if ((uint8_t)d0 == 0) break;
        a0 += 2;
        charge(2);
        if (n-- == 0) { set_d(7, 0x0000FFFFu); goto done; }
        set_d(0, d0); set_d(7, (uint32_t)n); set_a(0, a0); set_a(1, a1);
        poll();
    }
    set_d(7, (uint32_t)n);
done:
    charge(1);
    set_d(0, d0);
    set_a(0, a0); set_a(1, a1);
    return RD_RTS;
}

/* FUN_0002e836: make the 24-byte name at 0x578B printable: lower case to
 * upper (a byte >= 0x61, signed), the rest after the terminator blanks */
static uint32_t rd_2e836(void)
{
    uint32_t a0 = W(0x578B);
    uint16_t d7 = 0x17;
    charge(2);
    for (;;) {
        charge(2);
        uint8_t b = (uint8_t)vrd8(a0);
        if (b == 0) break;
        charge(2);
        if ((int8_t)b >= 0x61) { charge(1); vwr8(a0, (uint8_t)(b - 0x20)); }
        a0++;
        charge(2);
        if (d7-- == 0) goto out;
        set_d(7, d7); set_a(0, a0);
        poll();
    }
    for (;;) {
        vwr8(a0++, 0x20);
        charge(2);
        if (d7-- == 0) break;
        set_d(7, d7); set_a(0, a0);
        poll();
    }
out:
    charge(1);
    set_d(7, 0x0000FFFFu);
    set_a(0, a0);
    return RD_RTS;
}

/* FUN_0002e7e6: the sound name line: fetch it (FUN_0002e814), make it
 * printable (FUN_0002e836), put the print header (at 0x5788: 1, row 13,
 * column 17, and the terminator after 24 characters) and print it */
static uint32_t rd_2e7e6(void)
{
    uint32_t r;
    charge(1);
    if ((r = rd_call(L_2E814, 0x02E7EA))) return r;
    charge(1);
    if ((r = rd_call(L_2E836, 0x02E7EE))) return r;
    uint32_t a1 = W(0x5788);
    vwr8(a1, 1); vwr8(a1 + 1, 0xD); vwr8(a1 + 2, 0x11); vwr8(a1 + 0x1B, 0);
    w16_set(TEXT_ATTR, 0xC000);
    set_a(1, a1);
    charge(7);
    return RD_JMP(PRINT_TEXT);
}

/* FUN_0002ef64: print D0w as a signed four-digit number at A0 (sign
 * character 0xC02B / 0xC02D, digits from FUN_0000a870) and the label
 * 0x2F308 / 0x2F30D by D5 (attribute 0xE000 for the label) */
static uint32_t rd_2ef64(void)
{
    uint32_t r;
    uint16_t sign = 0x2B;
    charge(3);
    if ((int16_t)dw(0) < 0) { charge(2); set_d16(0, (uint16_t)-(int16_t)dw(0)); sign = 0x2D; }
    uint16_t d2 = (uint16_t)(sign + 0xC000);
    set_d16(2, d2);
    uint32_t a0 = a_reg(0);
    vwr16(a0, d2); set_a(0, a0 + 2);
    charge(3);
    if ((r = rd_call(L_A870, 0x02EF7E))) return r;
    a0 = a_reg(0);
    uint16_t d1 = dw(1);
    charge(1);
    for (int n = 3; ; n--) {
        d1 = (uint16_t)((d1 << 4) | (d1 >> 12));
        d2 = (uint16_t)(((d1 & 0xF) + 0x30) + 0xC000);
        vwr16(a0, d2); a0 += 2;
        charge(7);
        set_d16(1, d1); set_d16(2, d2); set_d(0, (uint32_t)(n - 1) & 0xFFFFu); set_a(0, a0);
        if (n == 0) break;
        poll();
    }
    set_d16(1, d1); set_d16(2, d2);
    set_d(0, 0x0000FFFFu);
    set_a(0, a0);
    w16_set(TEXT_ATTR, 0xE000);
    set_a(1, 0x02F308);
    charge(4);
    if (dw(5)) { charge(1); set_a(1, 0x02F30D); }
    charge(1);
    if ((r = rd_call(L_5A0A, 0x02EFAE))) return r;
    charge(2);
    w16_set(TEXT_ATTR, 0xC000);
    return RD_RTS;
}


/* the switch rows of the switch-test page: eight digits 1..8 at attribute
 * 0xC000 (bit set) or 0xD000 (clear) of the byte in D2, text 0x2F19E */
static uint32_t switch_row(uint32_t ret)
{
    uint32_t r;
    set_d(6, 0);
    charge(1);
    for (;;) {
        w16_set(TEXT_ATTR, 0xC000);
        charge(3);
        if (!((d_reg(2) >> (d_reg(6) & 31)) & 1)) { charge(1); w16_set(TEXT_ATTR, 0xD000); }
        set_d16(5, dw(6));
        set_d16(5, (uint16_t)(dw(5) + 1));
        set_a(1, 0x02F19E);
        charge(4);
        if ((r = rd_call(L_5A0A, ret))) return r;
        set_d16(6, (uint16_t)(dw(6) + 1));
        charge(3);
        if (!((int16_t)dw(6) < 8)) break;
        poll();
    }
    return 0;
}

/* FUN_0002ebb8: the switch test page: when 0xDE0 is 2 the row for 0x57B6,
 * when the byte 0x10001071 is 0 the row for 0x57B8; the two DIP switch
 * banks (0x50000001, 0x50000000) as eight lamps each; and the "free
 * play"/"coins" line by any bit of 0x1104 */
static uint32_t rd_2ebb8(void)
{
    uint32_t r;
    charge(2);
    if (w16(0x0DE0) == 2) {
        set_d(4, 0);
        set_d16(5, w16(0x57B6));
        w16_set(MENU_DEFAULT, 1);
        charge(4);
        if ((r = rd_call(L_29C68, 0x02EBD0))) return r;
        if ((r = print_bit0(0x02F1D1, 0x02F1D8, 0x02EBE4))) return r;
    }
    charge(2);
    if (vrd8(0x10001071u) == 0) {
        set_d(4, 3);
        set_d16(5, w16(0x57B8));
        w16_set(MENU_DEFAULT, 1);
        charge(4);
        if ((r = rd_call(L_29C68, 0x02EBFC))) return r;
        if ((r = print_bit0(0x02F1B2, 0x02F1B9, 0x02EC10))) return r;
    }
    set_a(1, 0x02F188);
    charge(2);
    if ((r = rd_call(L_5A0A, 0x02EC1A))) return r;
    set_d8(2, (uint8_t)vrd8(0x50000001u));
    charge(1);
    if ((r = switch_row(0x02EC40))) return r;
    set_a(1, 0x02F193);
    charge(2);
    if ((r = rd_call(L_5A0A, 0x02EC52))) return r;
    set_d8(2, (uint8_t)vrd8(0x50000000u));
    charge(1);
    if ((r = switch_row(0x02EC78))) return r;
    w16_set(TEXT_ATTR, 0xC000);
    set_d(1, 0);
    uint16_t d0 = w16(0x1104), d5 = 0;
    charge(5);
    for (int n = 7; ; n--) {
        d0 = (uint16_t)((d0 >> 1) | (d0 << 15));
        charge(3);
        if (d0 & 0x8000) { charge(1); d5++; }
        set_d16(0, d0); set_d(5, d5); set_d(6, (uint32_t)(n - 1) & 0xFFFFu);
        if (n == 0) break;
        poll();
    }
    set_d16(0, d0);
    set_d(6, 0x0000FFFFu);
    set_d(5, d5);
    set_a(1, 0x02F1A1);
    charge(3);
    if (d5 == 0) { charge(1); set_a(1, 0x02F1AA); }
    charge(1);
    return RD_JMP(PRINT_TEXT);
}

/* the lamp word both lamp pages build: 0x57B0 = ~(the six bits of 0x57AB,
 * bit n weighted 0x20 >> n) << 2 | the word at `base` (0x57AE or 0x57AC)
 * (bit 1 of the low byte toggled
 * when 0xDDE is set), copied to 0x4344, and bit 3 of the sound CPU's word
 * 0x60004020 set. D0 and D7 end as the loop leaves them. */
static void lamp_word(int32_t base)
{
    uint16_t d0 = 0, d7 = 0x20;
    w16_set(0x57B0, 0);
    charge(3);
    for (;;) {
        charge(2);
        if ((w8(0x57AB) >> (d0 & 7)) & 1) { charge(1); w16_set(0x57B0, (uint16_t)(w16(0x57B0) + d7)); }
        d7 = (uint16_t)(d7 >> 1);
        d0++;
        charge(4);
        set_d(0, d0); set_d(7, d7);
        if (!((int16_t)d0 < 6)) break;
        poll();
    }
    d0 = w16(0x57B0);
    d0 = (uint16_t)~d0;
    d0 = (uint16_t)(d0 << 2);
    d0 = (uint16_t)(d0 | w16(base));
    w16_set(0x57B0, d0);
    charge(7);
    if (w16(0x0DDE)) { charge(1); w8_set(0x57B1, w8(0x57B1) ^ 2); }
    w16_set(0x4344, w16(0x57B0));
    uint32_t v = (uint16_t)vrd16(0x60004020u) | 8u;    /* D0 was cleared by the moveq */
    vwr16(0x60004020u, (uint16_t)v);
    charge(4);
    set_d(0, v);
    set_d(7, d7);
}

/* FUN_00029d9c: the lamp-test page's lamp word. When 0x5006 is set: all
 * lamps on (0x4344 = 0xFFFF), bit 3 of 0x60004020 cleared, 0x57B4 = -1.
 * Otherwise the lamp pattern follows the handle: |0xDD2| (<= 0x3C0) / 64 * 4
 * lamps lit, a quarter less when 0x57B4 is set (half again with 0xDDE); a
 * negative 0x57B4 waits for the handle to come within 0x80 of centre */
static uint32_t rd_29d9c(void)
{
    charge(2);
    if (w16(0x5006)) {
        w16_set(0x4344, 0xFFFF);
        uint32_t v = (d_reg(0) & 0xFFFF0000u) | (uint16_t)vrd16(0x60004020u);
        v &= ~8u;
        vwr16(0x60004020u, (uint16_t)v);
        set_d(0, v);
        w16_set(0x57B4, 0xFFFF);
        charge(6);
        return RD_RTS;
    }
    uint32_t d0 = 0;
    w16_set(0x57AE, 0);
    w8_set(0x57AF, w8(0x57AF) | 2);
    d0 = w16(0x0DD2);
    charge(5);
    if ((int16_t)d0 < 0) { charge(2); w8_set(0x57AF, w8(0x57AF) ^ 2); d0 = (uint16_t)-(int16_t)d0; }
    charge(2);
    if (!((int16_t)w16(0x57B4) < 0)) {
        charge(2);
        if ((int16_t)d0 >= 0x3C0) { charge(1); d0 = 0x3C0; }
        d0 &= 0xFFFF;
        d0 = ((d0 % 0x40) << 16) | (d0 / 0x40);
        d0 = (d0 & 0xFFFF0000u) | (uint16_t)(d0 << 2);
        charge(5);                                      /* andi, divu, lsl, tst, bne */
        if (w16(0x57B4) == 0) {
            charge(2);
            d0 = (d0 & 0xFFFF0000u) | (uint16_t)((uint16_t)d0 >> 2);
        } else {
            uint16_t d7 = (uint16_t)((uint16_t)d0 >> 2);
            set_d16(7, d7);
            d0 = (d0 & 0xFFFF0000u) | (uint16_t)(d0 - d7);
            charge(5);
            if (w16(0x0DDE)) { charge(1); d0 = (d0 & 0xFFFF0000u) | (uint16_t)((uint16_t)d0 >> 1); }
        }
        w16_set(0x57AA, d0);
        charge(2);
    } else {
        charge(2);
        if (!((int16_t)d0 > 0x80)) {
            charge(3);
            w16_set(0x57B4, 1);
            set_d(0, d0);
            return RD_RTS;
        }
        charge(1);
        w16_set(0x57AA, 0x10);
    }
    set_d(0, 0);
    lamp_word(0x57AE);
    charge(1);
    return RD_RTS;
}

/* FUN_0002f7a8: the lamp test's slow sweep: every 600 frames the step 0x57AA
 * restarts and bit 1 of 0x57AD flips; bit 8 of the frame count 0x57A8
 * advances the step on each rising edge (0x57B2), up to 12. Builds the lamp
 * word, prints the header (0x2FA18, 0x2FA28 / 0x2FA2D by the flipped bit),
 * the step * 25 / 16 as two digits at 0x9009E3C8, and the handle 0xDD2 as a
 * signed four-digit number at 0x9009E344 */
static uint32_t rd_2f7a8(void)
{
    uint32_t r;
    uint16_t t = (uint16_t)(w16(0x57A8) + 1);
    w16_set(0x57A8, t);
    charge(3);
    if (!((int16_t)t < 0x258)) {
        charge(3);
        w16_set(0x57AA, 0); w16_set(0x57A8, 0);
        w8_set(0x57AD, w8(0x57AD) ^ 2);
    }
    charge(2);
    if (w16(0x57AA) != 0xC) {
        uint16_t d0 = (uint16_t)((w16(0x57A8) & 0x100) >> 8);
        uint16_t d7 = w16(0x57B2);
        d7 = (uint16_t)((d7 ^ d0) & d0);
        w16_set(0x57B2, d7);
        w16_set(0x57AA, (uint16_t)(w16(0x57AA) + d7));
        charge(8);
    }
    lamp_word(0x57AC);
    set_a(1, 0x02FA18);
    charge(2);
    if ((r = rd_call(L_5A0A, 0x02F83C))) return r;
    set_a(1, 0x02FA28);
    charge(3);
    if (w8(0x57AD) & 2) { charge(1); set_a(1, 0x02FA2D); }
    charge(1);
    if ((r = rd_call(L_5A0A, 0x02F852))) return r;
    uint32_t d0 = (uint32_t)(uint16_t)w16(0x57AA) * 0x19;
    d0 = (d0 & 0xFFFF0000u) | (uint16_t)((uint16_t)d0 >> 4);
    set_d(0, d0);
    charge(4);
    if ((r = rd_call(L_A870, 0x02F862))) return r;
    uint16_t d1 = dw(1);
    d1 = (uint16_t)((d1 >> 4) | (d1 << 12));
    uint32_t a0 = 0x9009E3C8u, d0l = d_reg(0);
    uint16_t attr;
    charge(3);
    for (int n = 1; ; n--) {
        attr = w16(TEXT_ATTR);
        d0l = (d0l & 0xFFFF0000u) | (uint16_t)(((d1 & 0xF) + 0x30) + attr);
        vwr16(a0, (uint16_t)d0l); a0 += 2;
        d1 = (uint16_t)((d1 << 4) | (d1 >> 12));
        charge(7);
        set_d16(1, d1); set_d(0, d0l); set_d(7, (uint32_t)(n - 1) & 0xFFFFu); set_a(0, a0);
        if (n == 0) break;
        poll();
    }
    set_d16(1, d1);
    set_d(0, d0l);
    set_d(7, 0x0000FFFFu);
    set_a(0, a0);
    set_d16(5, w16(0x0DD2));
    set_a(1, 0x02FA32);
    charge(4);
    if ((int16_t)w16(0x0DD2) < 0) { charge(2); set_a(1, 0x02FA3D); set_d16(5, (uint16_t)-(int16_t)dw(5)); }
    charge(1);
    if ((r = rd_call(L_5A0A, 0x02F89C))) return r;
    set_d16(0, dw(5));
    charge(2);
    if ((r = rd_call(L_A870, 0x02F8A4))) return r;
    a0 = 0x9009E344u;
    d1 = dw(1);
    d0l = d_reg(0);
    charge(2);
    for (int n = 3; ; n--) {
        d1 = (uint16_t)((d1 << 4) | (d1 >> 12));
        attr = w16(TEXT_ATTR);
        d0l = (d0l & 0xFFFF0000u) | (uint16_t)(((d1 & 0xF) + 0x30) + attr);
        vwr16(a0, (uint16_t)d0l); a0 += 2;
        charge(7);
        set_d16(1, d1); set_d(0, d0l); set_d(7, (uint32_t)(n - 1) & 0xFFFFu); set_a(0, a0);
        if (n == 0) break;
        poll();
    }
    charge(1);
    set_d16(1, d1);
    set_d(0, d0l);
    set_d(7, 0x0000FFFFu);
    set_a(0, a0);
    return RD_RTS;
}


/* ======================================================================== */
/* attract display lists                                                     */
/* ======================================================================== */

/* FUN_0003088c: display-list objects from the word table at A0: a count-1
 * word, then per object: 0x8004, 0, a model word, 0, 0, a word, then four
 * vertex records (word, word, signed word, signed word, 0, word); ends the
 * list with -1 and stores the cursor. The words are stored as longs over
 * whatever D0's high half holds (move.w into D0w, move.l D0). */
static void obj_list(uint32_t a0, uint32_t a4, uint16_t d2, uint32_t d0)
{
    for (;;) {
        vwr32(a4, 0x8004); vwr32(a4 + 4, 0);
        d0 = (d0 & 0xFFFF0000u) | (uint16_t)vrd16(a0); a0 += 2;
        vwr32(a4 + 8, d0); vwr32(a4 + 12, 0); vwr32(a4 + 16, 0);
        d0 = (d0 & 0xFFFF0000u) | (uint16_t)vrd16(a0); a0 += 2;
        vwr32(a4 + 20, d0);
        a4 += 24;
        charge(9);
        for (int n = 3; ; n--) {
            d0 = (d0 & 0xFFFF0000u) | (uint16_t)vrd16(a0); a0 += 2;
            vwr32(a4, d0);
            d0 = (d0 & 0xFFFF0000u) | (uint16_t)vrd16(a0); a0 += 2;
            vwr32(a4 + 4, d0);
            d0 = sx16(vrd16(a0)); a0 += 2;
            vwr32(a4 + 8, d0);
            d0 = sx16(vrd16(a0)); a0 += 2;
            vwr32(a4 + 12, d0);
            vwr32(a4 + 16, 0);
            d0 = (d0 & 0xFFFF0000u) | (uint16_t)vrd16(a0); a0 += 2;
            vwr32(a4 + 20, d0);
            a4 += 24;
            charge(14);
            set_d(0, d0); set_d16(1, (uint16_t)(n - 1)); set_d16(2, d2); set_a(0, a0); set_a(4, a4);
            if (n == 0) break;
            poll();
        }
        charge(1);
        if (d2-- == 0) break;
        set_d16(1, 0xFFFF); set_d16(2, d2);
        poll();
    }
    charge(3);
    vwr32(a4, 0xFFFFFFFFu);
    w32_set(DL_CURSOR, a4);
    set_d(0, d0);
    set_d16(1, 0xFFFF);
    set_d16(2, 0xFFFF);
    set_a(0, a0); set_a(4, a4);
}

static uint32_t rd_3088c(void)                      /* FUN_0003088c */
{
    uint32_t a0 = a_reg(0);
    uint16_t d2 = (uint16_t)vrd16(a0);
    set_d16(2, d2);
    charge(1);
    obj_list(a0 + 2, a_reg(4), d2, d_reg(0));
    return RD_RTS;
}

/* FUN_000306cc: the object table at 0x30A34 into the display list */
static uint32_t rd_306cc(void)
{
    charge(3);
    set_a(0, 0x030A34);
    set_a(4, w32(DL_CURSOR));
    return 0x03088C;
}

/* one object from a table (count word 0), continuing into FUN_0003088c's loop */
static uint32_t obj_one(uint32_t a0)
{
    charge(4);
    set_a(4, w32(DL_CURSOR));
    set_a(0, a0);
    set_d16(2, 0);
    return 0x03088E;
}
static uint32_t rd_3084c(void) { return obj_one(0x030984); }   /* FUN_0003084c */
static uint32_t rd_3085c(void) { return obj_one(0x0309B0); }   /* FUN_0003085c */
static uint32_t rd_3086c(void) { return obj_one(0x0309DC); }   /* FUN_0003086c */
static uint32_t rd_3087c(void) { return obj_one(0x030A08); }   /* FUN_0003087c */

/* FUN_000307b6: a camera record for the attract list: header 0x8001 with the
 * priority word 0xC40, the object-shift marker 0x8010 (3, -0x8000, -1), the
 * position (0x6B7, 0, 0x75C, 0x15A0), the rotation pairs of angles 0x280 and
 * 0 (twice), then 0 and the list end 0x8010, -1, -1 */
static uint32_t rd_307b6(void)
{
    uint32_t a4 = w32(DL_CURSOR);
    uint32_t d0 = (d_reg(0) & 0xFFFF0000u) | w16(DL_PRIO);
    uint32_t d1 = d_reg(1), d2 = d_reg(2);
    static const uint32_t fixed[8] = { 0x8010, 3, 0xFFFF8000u, 0xFFFFFFFFu, 0x6B7, 0, 0x75C, 0x15A0 };
    vwr32(a4, 0x8001); vwr32(a4 + 4, d0); a4 += 8;
    for (int i = 0; i < 8; i++) { vwr32(a4, fixed[i]); a4 += 4; }
    d0 = (d0 & 0xFFFF0000u) | 0x280;
    sincos_a5(&d0, &d1, &d2);
    vwr32(a4, d1); vwr32(a4 + 4, d2); a4 += 8;
    d0 &= 0xFFFF0000u;
    sincos_a5(&d0, &d1, &d2);
    vwr32(a4, d1); vwr32(a4 + 4, d2); vwr32(a4 + 8, d1); vwr32(a4 + 12, d2);
    vwr32(a4 + 16, 0); vwr32(a4 + 20, 0x8010); vwr32(a4 + 24, 0xFFFFFFFFu);
    a4 += 28;
    vwr32(a4, 0xFFFFFFFFu);
    w32_set(DL_CURSOR, a4);
    set_d(0, d0); set_d(1, d1); set_d(2, d2);
    set_a(4, a4);
    charge(36);
    return RD_RTS;
}

/* ======================================================================== */
/* animation curves                                                          */
/* ======================================================================== */

/* the curve walk shared by FUN_00031168 / FUN_000311c6: A0 is a chain of
 * segments (word type, word length, words...); walk to the segment holding
 * frame 0x305C, then a type-1 segment is a table (value = its word at the
 * frame), frame 0 of any other segment is its first word, and otherwise the
 * interpolator for the type (table at 0x31224) is called with D1/D2 = the
 * two end values and D3/D4 = length/position. Returns 1 when D0 holds the
 * value directly, 0 after the interpolator (whose return path is `ret`). */
static uint32_t curve_walk(uint32_t a0, uint32_t ret, uint32_t at, int *direct)
{
    uint32_t r;
    uint16_t d2 = w16(0x305C);
    set_d16(2, d2);
    for (;;) {
        uint32_t a1 = a0 + 6;
        uint16_t d1 = (uint16_t)vrd16(a0 + 2), d0 = (uint16_t)vrd16(a0);
        set_d16(1, d1); set_d16(0, d0);
        charge(5);
        if (d0 == 1) { charge(1); a1 = a0 + 4 + (uint32_t)((int32_t)(int16_t)d1 * 2); }
        set_a(1, a1);
        charge(2);
        if ((int16_t)d2 < (int16_t)d1) break;
        d2 = (uint16_t)(d2 - d1);
        set_d16(2, d2);
        a0 = a1;
        charge(3);
        set_a(0, a0);
        poll();
    }
    set_a(0, a0);
    uint16_t d0 = (uint16_t)(dw(0) & 7);
    set_d16(0, d0);
    charge(3);
    if (d0 == 1) {
        set_d(0, sx16(vrd16(a0 + 4 + (uint32_t)((int32_t)(int16_t)d2 * 2))));
        charge(2);
        *direct = 1;
        return 0;
    }
    charge(2);
    if (d2 == 0) {
        set_d(0, sx16(vrd16(a0 + 4)));
        charge(2);
        *direct = 1;
        return 0;
    }
    set_d16(3, dw(1));
    set_d16(4, d2);
    set_d16(1, (uint16_t)vrd16(a0 + 4));
    set_d16(2, (uint16_t)vrd16(a_reg(1) + 4));
    uint32_t fn = vrd32(0x031224 + (uint32_t)((int32_t)(int16_t)d0 * 4));
    set_a(0, fn);
    charge(7);
    *direct = 0;
    if ((r = rd_call_ind(fn, ret, at))) return r;
    return 0;
}

/* FUN_00031168: value of curve D0 (1-based index into the pointer table A0)
 * at frame 0x305C, halved after interpolation */
static uint32_t rd_31168(void)
{
    uint32_t r;
    int direct;
    uint16_t d0 = (uint16_t)(dw(0) - 1);
    set_d16(0, d0);
    uint32_t a0 = vrd32(a_reg(0) + (uint32_t)((int32_t)(int16_t)d0 * 4));
    set_a(0, a0);
    charge(3);
    if ((r = curve_walk(a0, 0x0311C2, 0x0311C0, &direct))) return r;
    if (!direct) {
        set_d(0, (uint32_t)((int32_t)d_reg(0) >> 1));
        charge(2);
    } else charge(1);
    return RD_RTS;
}

/* FUN_000311c6: value of the curve at byte offset D0 of the pointer table
 * A0, doubled when read directly (the interpolators return twice the value) */
static uint32_t rd_311c6(void)
{
    uint32_t r;
    int direct;
    uint32_t a0 = vrd32(a_reg(0) + sx16(dw(0)));
    set_a(0, a0);
    charge(2);
    if ((r = curve_walk(a0, 0x031222, 0x031220, &direct))) return r;
    if (direct) {
        set_d(0, d_reg(0) << 1);
        charge(2);
    } else charge(1);
    return RD_RTS;
}

/* ======================================================================== */
/* EEPROM record staging (the writes themselves are FUN_0002b188)           */
/* ======================================================================== */

/* FUN_0002b940: stage record 0 -- the three words at 0x81A..0x81E -- in the
 * buffer at 0x50A0 (cleared by FUN_0002b172) and at 0x10001000; unless bit 0
 * of 0x5060 is set, continue into FUN_0002b188 with D7 = 0 */
static uint32_t rd_2b940(void)
{
    uint32_t r;
    charge(1);
    if ((r = rd_call(L_2B172, 0x02B944))) return r;
    for (int i = 0; i < 3; i++) vwr16(W(0x50A0) + 2u * i, w16(-0x77E6 + 2 * i));
    for (int i = 0; i < 3; i++) vwr16(0x10001000u + 2u * i, w16(0x50A0 + 2 * i));
    set_a(0, W(0x50A6)); set_a(1, 0x10001006u);
    uint32_t d0 = w32(0x5060);
    set_d(0, d0);
    set_d(7, 0);
    charge(13);
    if (d0 & 1) { charge(1); return RD_RTS; }
    charge(1);
    return RD_JMP(0x02B188);
}

/* stage 13 words from `src` into the buffer at 0x50A0 */
static void stage13(uint32_t src)
{
    uint32_t a0 = W(0x50A0), a1 = src;
    charge(1);
    for (int n = 12; ; n--) { vwr16(a0, vrd16(a1)); a0 += 2; a1 += 2; charge(2); set_a(0, a0); set_a(1, a1); set_d(0, (uint32_t)(n - 1) & 0xFFFFu); if (n == 0) break; poll(); }
    set_a(0, a0); set_a(1, a1);
    set_d(0, 0x0000FFFFu);
}

/* FUN_0002bae6: stage record 2 from 0x10001040 when its first byte is set;
 * unless bit 2 of 0x5060 is set, continue into FUN_0002b188 with D7 = 2 */
static uint32_t rd_2bae6(void)
{
    uint32_t r;
    charge(1);
    if ((r = rd_call(L_2B172, 0x02BAEA))) return r;
    set_a(0, W(0x50A0)); set_a(1, 0x10001040u);
    charge(4);
    if (vrd8(0x10001040u) == 0) { charge(1); return RD_RTS; }
    stage13(0x10001040u);
    uint32_t d0 = w32(0x5060);
    set_d(0, d0);
    set_d(7, 2);
    charge(4);
    if (d0 & 4) { charge(1); return RD_RTS; }
    charge(1);
    return RD_JMP(0x02B188);
}

/* FUN_0002bb10: stage record 3 from 0x10001060; unless bit 3 of 0x5060 is
 * set, continue into FUN_0002b188 with D7 = 3 */
static uint32_t rd_2bb10(void)
{
    uint32_t r;
    charge(1);
    if ((r = rd_call(L_2B172, 0x02BB14))) return r;
    charge(2);
    stage13(0x10001060u);
    uint32_t d0 = w32(0x5060);
    set_d(0, d0);
    set_d(7, 3);
    charge(4);
    if (d0 & 8) { charge(1); return RD_RTS; }
    charge(1);
    return RD_JMP(0x02B188);
}

/* FUN_0002bb5e: pack the four record names (a long at 0x593E + c*4, three
 * name words at 0x594E + c*8) as six bytes each into 0x100010E0; unless bit 7
 * of 0x5060 is set, stage those 13 words and continue into FUN_0002b188 with
 * D7 = 7 */
static uint32_t rd_2bb5e(void)
{
    uint32_t r;
    charge(1);
    if ((r = rd_call(L_2B172, 0x02BB62))) return r;
    uint32_t a0 = W(0x593E), a1 = W(0x594E), a2 = 0x100010E0u, d0 = 0;
    charge(4);
    for (int n = 3; ; n--) {
        d0 = vrd32(a0); a0 += 4;
        d0 = (d0 << 16) | (d0 >> 16);
        vwr8(a2++, (uint8_t)d0);
        d0 = rol32(d0, 8);
        vwr8(a2++, (uint8_t)d0);
        d0 = rol32(d0, 8);
        vwr8(a2++, (uint8_t)d0);
        vwr8(a2++, vrd8(a1 + 1)); vwr8(a2++, vrd8(a1 + 3)); vwr8(a2++, vrd8(a1 + 5));
        a1 += 8;
        charge(12);
        set_a(0, a0); set_a(1, a1); set_a(2, a2); set_d(0, d0); set_d(7, (uint32_t)(n - 1) & 0xFFFFu);
        if (n == 0) break;
        poll();
    }
    set_a(0, a0); set_a(1, a1); set_a(2, a2);
    d0 = w32(0x5060);
    set_d(0, d0);
    set_d(7, 7);
    charge(4);
    if (d0 & 0x80) { charge(1); return RD_RTS; }
    a0 = 0x100010E0u; a1 = W(0x50A0);
    charge(3);
    for (int n = 12; ; n--) { vwr16(a1, vrd16(a0)); a0 += 2; a1 += 2; charge(2); set_a(0, a0); set_a(1, a1); set_d(7, (uint32_t)(n - 1) & 0xFFFFu); if (n == 0) break; poll(); }
    set_a(0, a0); set_a(1, a1);
    set_d(7, 7);
    charge(2);
    return RD_JMP(0x02B188);
}


/* ======================================================================== */
/* EEPROM page helpers                                                       */
/* ======================================================================== */

/* FUN_0002ad14: the 31 default words at ROM 0x2B3DC to the EEPROM record
 * buffer at 0x500E */
static uint32_t rd_2ad14(void)
{
    uint32_t a0 = 0x02B3DC, a1 = W(0x500E);
    charge(3);
    for (int n = 0x1E; ; n--) {
        vwr16(a1, vrd16(a0)); a0 += 2; a1 += 2;
        charge(2);
        set_a(0, a0); set_a(1, a1); set_d(6, (uint32_t)(n - 1) & 0xFFFFu);
        if (n == 0) break;
        poll();
    }
    charge(1);
    set_a(0, a0); set_a(1, a1);
    set_d(6, 0x0000FFFFu);
    return RD_RTS;
}

/* FUN_0002af76: clear the word 0x50C0 and fall into FUN_0002af7a */
static uint32_t rd_2af76(void)
{
    charge(1);
    w16_set(0x50C0, 0);
    return 0x02AF7A;
}

/* NOT converted (left lifted), with the reason:
 *  - the EEPROM writers FUN_0002abcc 2ad50 2aeb4 2af7a 2afc2 2b00c 2b066 2b188
 *    2b39c 2bf44 2c000 2c092 2c19c 2c234: each reaches FUN_0002b056, which spins
 *    until an interrupt changes a word -- a check holds interrupts off
 *    (rd_funcs.c batch 5 note). 2b056 itself likewise.
 *  - FUN_0002f5cc: its cmp2.w at 0x2F672 is lifted wrongly (Ghidra's p-code
 *    reads the bounds through a double dereference and compares the whole
 *    register); reported in work/INFRA_REQUEST.md.
 *  - the power-on RAM test FUN_0002fa68 / 2fe16 / 2fee8: every checked call is
 *    "unverifiable" -- a watchdog I/O write per tested word overflows the
 *    check's 256-entry I/O log.
 *  - FUN_000318be (master DSP upload): polls the DSP's handshake word in
 *    polygon RAM while its DSP-control writes are deferred to the end of a
 *    checked call, so a checked run would time out and lose the DSP program. */

static const rd_entry rd_table_b6[] = {
    { 0x029C68, rd_29c68, 0x0003, 0, 0, "FUN_00029c68" },
    { 0x02A454, rd_2a454, 0x0101, 0, 0, "FUN_0002a454" },
    { 0x02A46C, rd_2a46c, 0x03FF, 0, 0, "FUN_0002a46c" },
    { 0x02A5C8, rd_2a5c8, 0x0FDF, 0, 0, "FUN_0002a5c8" },
    { 0x02AD14, rd_2ad14, 0x034F, 0, 0, "FUN_0002ad14" },
    { 0x02AF76, rd_2af76, 0x0000, 0, 0, "FUN_0002af76" },
    { 0x02B354, rd_2b354, 0x0200, 0, 0, "FUN_0002b354" },
    { 0x02B378, rd_2b378, 0x0200, 0, 0, "FUN_0002b378" },
    { 0x02B974, rd_2b974, 0x0200, 0, 0, "FUN_0002b974" },
    { 0x02BF66, rd_2bf66, 0x0FDF, 0, 0, "FUN_0002bf66" },
    { 0x02BFC0, rd_2bfc0, 0x1F87, 0, 0, "FUN_0002bfc0" },
    { 0x02C088, rd_2c088, 0x0000, 0, 0, "FUN_0002c088" },
    { 0x02C170, rd_2c170, 0x0200, 0, 0, "FUN_0002c170" },
    { 0x02C4FC, rd_2c4fc, 0x0FFF, 0, 0, "FUN_0002c4fc" },
    { 0x02C572, rd_2c572, 0x034F, 0, 0, "FUN_0002c572" },
    { 0x02C5A0, rd_2c5a0, 0x034F, 0, 0, "FUN_0002c5a0" },
    { 0x02C5BE, rd_2c5be, 0x034F, 0, 0, "FUN_0002c5be" },
    { 0x02C5DC, rd_2c5dc, 0x034F, 0, 0, "FUN_0002c5dc" },
    { 0x02C5FA, rd_2c5fa, 0x03FF, 0, 0, "FUN_0002c5fa" },
    { 0x02C634, rd_2c634, 0x0141, 0, 0, "FUN_0002c634" },
    { 0x02C646, rd_2c646, 0x0141, 0, 0, "FUN_0002c646" },
    { 0x02C658, rd_2c658, 0x0141, 0, 0, "FUN_0002c658" },
    { 0x02C66E, rd_2c66e, 0x0141, 0, 0, "FUN_0002c66e" },
    { 0x02C680, rd_2c680, 0x0141, 0, 0, "FUN_0002c680" },
    { 0x02C692, rd_2c692, 0x0141, 0, 0, "FUN_0002c692" },
    { 0x02C6A8, rd_2c6a8, 0x0141, 0, 0, "FUN_0002c6a8" },
    { 0x02C6BA, rd_2c6ba, 0x0141, 0, 0, "FUN_0002c6ba" },
    { 0x02C6CC, rd_2c6cc, 0x0141, 0, 0, "FUN_0002c6cc" },
    { 0x02C6E2, rd_2c6e2, 0x03FF, 0, 0, "FUN_0002c6e2" },
    { 0x02C86A, rd_2c86a, 0x03FF, 0, 0, "FUN_0002c86a" },
    { 0x02C8D8, rd_2c8d8, 0x03CF, 0, 0, "FUN_0002c8d8" },
    { 0x02C900, rd_2c900, 0x03FF, 0, 0, "FUN_0002c900" },
    { 0x02C928, rd_2c928, 0x034F, 0, 0, "FUN_0002c928" },
    { 0x02C968, rd_2c968, 0x034F, 0, 0, "FUN_0002c968" },
    { 0x02CB52, rd_2cb52, 0x0FDF, 0, 0, "FUN_0002cb52" },
    { 0x02CBCA, rd_2cbca, 0x020F, 0, 0, "FUN_0002cbca" },
    { 0x02CC10, rd_2cc10, 0x008F, 0, 0, "FUN_0002cc10" },
    { 0x02CC2E, rd_2cc2e, 0x08FF, 0, 0, "FUN_0002cc2e" },
    { 0x02CEF0, rd_2cef0, 0x0F1F, 0, 0, "FUN_0002cef0" },
    { 0x02CF84, rd_2cf84, 0x1BFF, 0, 0, "FUN_0002cf84" },
    { 0x02D072, rd_2d072, 0x020F, 0, 0, "FUN_0002d072" },
    { 0x02D15E, rd_2d15e, 0x0000, 0, 0, "FUN_0002d15e" },
    { 0x02D178, rd_2d178, 0x0207, 0, 0, "FUN_0002d178" },
    { 0x02D688, rd_2d688, 0x0301, 0, 0, "FUN_0002d688" },
    { 0x02D9DA, rd_2d9da, 0x0201, 0, 0, "FUN_0002d9da" },
    { 0x02D9F8, rd_2d9f8, 0x0F01, 0, 0, "FUN_0002d9f8" },
    { 0x02DA12, rd_2da12, 0x0FDF, 0, 0, "FUN_0002da12" },
    { 0x02DA52, rd_2da52, 0x0201, 0, 0, "FUN_0002da52" },
    { 0x02DA5E, rd_2da5e, 0x0101, 0, 0, "FUN_0002da5e" },
    { 0x02E23E, rd_2e23e, 0x0301, 0, 0, "FUN_0002e23e" },
    { 0x02E492, rd_2e492, 0x0000, 0, 0, "FUN_0002e492" },
    { 0x02E646, rd_2e646, 0x0301, 0, 0, "FUN_0002e646" },
    { 0x02E9E6, rd_2e9e6, 0x0FDF, 0, 0, "FUN_0002e9e6" },
    { 0x02F3DC, rd_2f3dc, 0x0000, 0, 0, "FUN_0002f3dc" },
    { 0x02F5A8, rd_2f5a8, 0x0301, 0, 0, "FUN_0002f5a8" },
    { 0x029D9C, rd_29d9c, 0x0081, 0, 0, "FUN_00029d9c" },
    { 0x02B940, rd_2b940, 0x0381, 0, 0, "FUN_0002b940" },
    { 0x02BAE6, rd_2bae6, 0x0381, 0, 0, "FUN_0002bae6" },
    { 0x02BB10, rd_2bb10, 0x0381, 0, 0, "FUN_0002bb10" },
    { 0x02BB5E, rd_2bb5e, 0x37C1, 0, 0, "FUN_0002bb5e" },
    { 0x02C71C, rd_2c71c, 0x0FDF, 0, 0, "FUN_0002c71c", 1 },
    { 0x02D6C8, rd_2d6c8, 0x1FFF, 0, 0, "FUN_0002d6c8" },
    { 0x02DA84, rd_2da84, 0x0FFF, 0, 0, "FUN_0002da84" },
    { 0x02DB74, rd_2db74, 0x0383, 0, 0, "FUN_0002db74" },
    { 0x02DB9C, rd_2db9c, 0x1BFF, 0, 0, "FUN_0002db9c" },
    { 0x02E26C, rd_2e26c, 0x0FFF, 0, 0, "FUN_0002e26c" },
    { 0x02E6FC, rd_2e6fc, 0x1FFF, 0, 0, "FUN_0002e6fc" },
    { 0x02E7E6, rd_2e7e6, 0x0381, 0, 0, "FUN_0002e7e6" },
    { 0x02E814, rd_2e814, 0x0381, 0, 0, "FUN_0002e814" },
    { 0x02E836, rd_2e836, 0x0180, 0, 0, "FUN_0002e836" },
    { 0x02EBB8, rd_2ebb8, 0x0FFF, 0, 0, "FUN_0002ebb8" },
    { 0x02EF64, rd_2ef64, 0x0FDF, 0, 0, "FUN_0002ef64" },
    { 0x02F7A8, rd_2f7a8, 0x0FFF, 0, 0, "FUN_0002f7a8" },
    { 0x0306CC, rd_306cc, 0x1100, 0, 0, "FUN_000306cc" },
    { 0x0307B6, rd_307b6, 0x1007, 0, 0, "FUN_000307b6" },
    { 0x03084C, rd_3084c, 0x1104, 0, 0, "FUN_0003084c" },
    { 0x03085C, rd_3085c, 0x1104, 0, 0, "FUN_0003085c" },
    { 0x03086C, rd_3086c, 0x1104, 0, 0, "FUN_0003086c" },
    { 0x03087C, rd_3087c, 0x1104, 0, 0, "FUN_0003087c" },
    { 0x03088C, rd_3088c, 0x1F87, 0, 0, "FUN_0003088c" },
    { 0x031168, rd_31168, 0x3FFF, 0, 0, "FUN_00031168" },
    { 0x0311C6, rd_311c6, 0x3FFF, 0, 0, "FUN_000311c6" },
};
RD_REGISTER(rd_table_b6)
