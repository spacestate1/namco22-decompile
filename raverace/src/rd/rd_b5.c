/*
 * rd_b5.c -- Phase B batch 5: readable-C replacements, each proven against its
 * lifted twin by RR_RD=check (include/rd.h). Written from the 68K instructions
 * (gen/rr_lifted.c); Ghidra's C is a naming aid only.
 *
 * A6 is always WRAM + 0x8000 (0x10008000): (d16,A6) is a fixed address.
 * Registers are kept in R[] across every call (a callee may change any of them).
 */
#include "rd.h"
#include "rr_lifted.h"

#define G_DL_CURSOR  0x10008C46u   /* long: display-list write pointer */
#define TEXT_BASE    0x9009E000u   /* the text layer: 64 words a row */
#define A6W(off) (0x10008000u + (uint32_t)(off))    /* (d16,A6), d16 < 0x8000 */

static inline uint16_t w16(uint32_t off)                 { return (uint16_t)vrd16(A6W(off)); }
static inline void     w16_set(uint32_t off, uint16_t v) { vwr16(A6W(off), v); }
static inline uint32_t sx16(uint32_t v)                  { return (uint32_t)(int32_t)(int16_t)v; }
static inline uint16_t dw(int n)                         { return (uint16_t)d_reg(n); }

#define RTS() do { charge(1); return RD_RTS; } while (0)
/* a bsr/jsr: charge it, call, leave if the callee did not come back here */
#define CALL(fn, ret) do { charge(1); if ((r = rd_call(fn, ret))) return r; } while (0)

/* dbf Dn: decrement the low word; 1 = branch taken (the caller polls) */
static int dbf(int n)
{
    uint16_t v = (uint16_t)(dw(n) - 1);
    set_d16(n, v);
    charge(1);
    return v != 0xFFFF;
}

/* ============================== 0x5770 .. 0xA870 ============================== */

/* FUN_00005770: clear three 32 KB blocks at 0x90028000, 0x90030000 and
 * 0x90038000 (0x2000 longs each). Leaves D1 = 0, D0w = 0xFFFF, A0 past the end. */
static uint32_t rd_clear_90028000(void)
{
    static const uint32_t base[3] = { 0x90028000u, 0x90030000u, 0x90038000u };
    set_d(1, 0);
    charge(1);                                          /* moveq */
    for (int b = 0; b < 3; b++) {
        uint32_t a = base[b];
        set_d16(0, 0x1FFF);
        charge(2);                                      /* lea, move.w */
        do {
            vwr32(a, 0); a += 4;
            set_a(0, a);
            charge(1);                                  /* move.l */
            if (!dbf(0)) break;
            poll();
        } while (1);
    }
    RTS();
}

/* FUN_00005bb6: D7 = the next byte at (A1)+ (zero-extended to a word), then
 * fall into FUN_00005bbc */
static uint32_t rd_count_from_a1(void)
{
    uint32_t a1 = a_reg(1);
    set_d8(7, vrd8(a1));
    set_a(1, a1 + 1);
    set_d16(7, (uint16_t)(dw(7) & 0xFF));
    charge(2);
    return 0x5BBCu;                                     /* falls through (no poll) */
}

/* FUN_00005bbc: print the low D7w+1 hex digits of D1 (most significant first)
 * through FUN_00005a2a, one character in D0b each (digits from ROM 0x5BD8).
 * D1 is rotated so that it ends where it started. */
static uint32_t rd_print_hex(void)
{
    uint32_t r;
    set_d16(0, (uint16_t)(dw(7) << 2));
    unsigned n = dw(0) & 63;
    uint32_t d1 = d_reg(1);
    n &= 31;
    set_d(1, n ? (d1 >> n) | (d1 << (32 - n)) : d1);
    charge(3);                                          /* move.w, lsl.w, ror.l */
    do {
        set_d16(0, (uint16_t)(dw(1) & 0xF));
        set_d8(0, vrd8(0x5BD8u + sx16(dw(0))));
        d1 = d_reg(1);
        set_d(1, (d1 << 4) | (d1 >> 28));
        charge(4);                                      /* move.w, andi.w, move.b, rol.l */
        CALL(L_5A2A, 0x5BD2);
        if (!dbf(7)) break;
        poll();
    } while (1);
    RTS();
}



/* ============================== 0x5A0A: text ============================== */

/* The text cursor: A0 = the text-layer address of (column 0xC56, row 0xC58),
 * 0xC5E = the attribute word ORed into each character. */
#define G_COL  0xC56
#define G_ROW  0xC58

/* 0x5AB4: A0 += (column*2 + row*128) as a signed word; then rts */
static void text_cursor_a0(void)
{
    set_d16(0, (uint16_t)(dw(0) + dw(0)));
    set_d16(1, (uint16_t)(dw(1) << 7));
    set_d16(0, (uint16_t)(dw(0) + dw(1)));
    set_a(0, a_reg(0) + sx16(dw(0)));
    charge(5);                                          /* ... adda, rts */
}

/* the numeric print of control codes 5 / 12 (0x5B5C): D0 -> five BCD digits
 * (FUN_00005cee), then the low (count byte at (A1)+) + 1 digits of D1,
 * leading zeros as spaces (the last digit always printed) */
static uint32_t text_decimal(void)
{
    uint32_t r;
    CALL(L_5CEE, 0x5B60);
    set_d16(2, 0);
    set_d8(7, vrd8(a_reg(1)));
    set_a(1, a_reg(1) + 1);
    set_d16(7, (uint16_t)(dw(7) & 0xFF));
    set_d16(0, (uint16_t)(dw(7) << 2));
    unsigned n = dw(0) & 31;
    uint32_t d1 = d_reg(1);
    set_d(1, n ? (d1 >> n) | (d1 << (32 - n)) : d1);
    charge(6);
    do {
        set_d16(0, (uint16_t)(dw(1) & 0xF));
        charge(3);
        if (dw(0) != 0) { set_d16(2, 0xFFFF); charge(1); }
        set_d16(0, (uint16_t)(dw(0) + 0x30));
        d1 = d_reg(1);
        set_d(1, (d1 << 4) | (d1 >> 28));
        charge(4);
        int space = 0;
        if (dw(7) != 0) { charge(2); if (dw(2) == 0) space = 1; }
        if (space) {
            uint32_t a0 = a_reg(0);
            vwr16(a0, 0x20);
            set_a(0, a0 + 2);
            w16_set(G_COL, (uint16_t)(w16(G_COL) + 1));
            charge(3);
        } else {
            CALL(L_5A2A, 0x5B96);
        }
        if (!dbf(7)) break;
        poll();
    } while (1);
    return 0;
}

/* one hex digit of D6 (rolled into the low nibble) printed through FUN_00005a2a */
static uint32_t text_hex_digit(uint32_t ret)
{
    uint32_t r;
    uint32_t d6 = d_reg(6);
    set_d(6, (d6 << 4) | (d6 >> 28));
    set_d16(0, (uint16_t)(dw(6) & 0xF));
    set_d8(0, vrd8(0x5BD8u + sx16(dw(0))));
    charge(4);
    CALL(L_5A2A, ret);
    return 0;
}

/* FUN_00005a0a: the TEXT INTERPRETER, one step per call. It pushes its own
 * address as the return address (so every handler's rts comes back here),
 * then reads the next byte at (A1)+ into D0:
 *   > 0x20  printable: continue in FUN_00005a2a (prints it, returns here)
 *   = 0x20  space: continue in 0x5A8E
 *   < 0x20  a control code, through the ROM word table 0x59E8 (jmp base 0x5A9A):
 *     0 end (drop the loop address, return)    1 set column, row (two bytes)
 *     2 set attribute (byte << 8) 3 print chars, one long stack arg per
 *                                    embedded control byte, as hex (FUN_00005bb6)
 *     4 hex of D5                 5 decimal of the long at the 32-bit address in
 *                                    the next four bytes; 12 decimal of D5
 *     6 / 7 hex of the signed byte / word at such an address
 *     8 hex words from the list 0xC4E (count byte; 0 = the word 0xC50)
 *     9 tab to a multiple of 8    10 continue at the word-aligned 16-bit address
 *     11 word-align A1            13 newline
 *     14 D6 as a time m'ss"fff    15 move column, row by signed bytes
 *     16 print the character D5b
 * The handlers' rts land back at the top of this loop (the lifted code resumes
 * internally, so the loop is part of this function); only the exits above
 * leave it. Codes above 16 index past the table into this code's own bytes;
 * no text uses them (this entry is never fuzzed). */
static uint32_t rd_text_step(void)
{
    uint32_t r;
    for (;;) {
        uint32_t sp = a_reg(7) - 4;
        set_a(7, sp);
        vwr32(sp, 0x5A0A);
        set_d(0, 0);
        uint32_t a1 = a_reg(1);
        uint8_t c = vrd8(a1);
        set_a(1, a1 + 1);
        set_d8(0, c);
        charge(5);                                      /* pea, moveq, move.b, cmpi, bcs */
        if (c >= 0x20) {
            charge(1);                                  /* beq */
            if (c == 0x20) return 0x5A8Eu;
            charge(1);                                  /* bra: forward, no poll */
            return 0x5A2Au;
        }
        set_d16(0, (uint16_t)((c & 0x1F) * 2));
        set_d16(0, (uint16_t)vrd16(0x59E8u + sx16(dw(0))));
        uint32_t t = 0x5A9Au + sx16(dw(0));
        charge(4);                                      /* andi, add, move, jmp */
        poll();
        switch (t) {
        case 0x5A9E:                                    /* 1: set column, row */
            set_a(0, TEXT_BASE);
            set_d16(0, 0); set_d16(1, 0);
            set_d8(0, vrd8(a_reg(1))); set_a(1, a_reg(1) + 1);
            w16_set(G_COL, dw(0));
            set_d8(1, vrd8(a_reg(1))); set_a(1, a_reg(1) + 1);
            w16_set(G_ROW, dw(1));
            charge(7);
            text_cursor_a0();
            break;
        case 0x5ABE:                                    /* 15: move by signed bytes */
            set_a(0, TEXT_BASE);
            set_d16(0, 0); set_d16(1, 0);
            set_d8(0, vrd8(a_reg(1))); set_a(1, a_reg(1) + 1);
            set_d16(0, (uint16_t)(int16_t)(int8_t)d_reg(0));
            w16_set(G_COL, (uint16_t)(w16(G_COL) + dw(0)));
            set_d8(1, vrd8(a_reg(1))); set_a(1, a_reg(1) + 1);
            set_d16(1, (uint16_t)(int16_t)(int8_t)d_reg(1));
            w16_set(G_ROW, (uint16_t)(w16(G_ROW) + dw(1)));
            set_d16(0, w16(G_COL)); set_d16(1, w16(G_ROW));
            charge(12);
            poll();
            text_cursor_a0();
            break;
        case 0x5AE2:                                    /* 2: attribute */
            set_d8(0, vrd8(a_reg(1))); set_a(1, a_reg(1) + 1);
            set_d16(0, (uint16_t)(dw(0) << 8));
            w16_set(0xC5E, dw(0));
            charge(4);
            break;
        case 0x5AEC:                                    /* 9: tab */
            set_d16(0, (uint16_t)((w16(G_COL) + 8) & 0xFFF8));
            w16_set(G_COL, dw(0));
            set_d16(1, w16(G_ROW));
            charge(6);
            poll();
            text_cursor_a0();
            break;
        case 0x5B02:                                    /* 13: newline */
            w16_set(G_COL, 0);
            w16_set(G_ROW, (uint16_t)(w16(G_ROW) + 1));
            set_a(0, TEXT_BASE);
            set_d16(0, w16(G_COL)); set_d16(1, w16(G_ROW));
            charge(6);
            poll();
            text_cursor_a0();
            break;
        case 0x5B1E:                                    /* 11: align A1 */
            set_d(0, (a_reg(1) + 1) & ~1u);
            set_a(1, d_reg(0));
            charge(5);
            break;
        case 0x5B2A:                                    /* 10: jump to the 16-bit address */
            set_d(0, (a_reg(1) + 1) & ~1u);
            set_a(1, d_reg(0));
            set_d16(0, (uint16_t)vrd16(a_reg(1)));
            set_a(1, d_reg(0));
            charge(7);
            break;
        case 0x5B3A:                                    /* 16: print D5b */
            set_d16(0, 0);
            set_d8(0, (uint8_t)d_reg(5));
            charge(3);
            poll();
            return 0x5A2Au;                             /* bra.w backward (polled) */
        case 0x5B54:                                    /* 5: decimal at an address */
            CALL(L_5B4A, 0x5B56);
            set_a(2, d_reg(0));
            set_d(0, vrd32(a_reg(2)));
            charge(3);
            if ((r = text_decimal())) return r;
            charge(1);                                  /* rts */
            break;
        case 0x5B9C:                                    /* 12: decimal of D5 */
            set_d(0, d_reg(5));
            charge(2);
            poll();
            if ((r = text_decimal())) return r;
            charge(1);
            break;
        case 0x5BA0:                                    /* 4: hex of D5 */
            set_d(1, d_reg(5));
            charge(2);
            return 0x5BB6u;                             /* forward bra */
        case 0x5BA4:                                    /* 6: hex of a signed byte */
            CALL(L_5B4A, 0x5BA6);
            set_a(2, d_reg(0));
            set_d8(1, vrd8(a_reg(2)));
            set_d16(1, (uint16_t)(int16_t)(int8_t)d_reg(1));
            charge(4);
            return 0x5BB6u;
        case 0x5BAE:                                    /* 7: hex of a signed word */
            CALL(L_5B4A, 0x5BB0);
            set_a(2, d_reg(0));
            set_d16(1, (uint16_t)vrd16(a_reg(2)));
            set_d(1, sx16(dw(1)));
            charge(3);
            return 0x5BB6u;                             /* falls through into 0x5BB6 (see report) */
        case 0x5BE8:                                    /* 8: hex word list */
            set_d(4, 0);
            set_d8(4, vrd8(a_reg(1))); set_a(1, a_reg(1) + 1);
            charge(3);
            if ((uint8_t)d_reg(4) == 0) {
                set_d16(1, w16(0xC50));
                set_d(7, 3);
                charge(3);
                poll();
                return 0x5BBCu;
            }
            set_d16(4, (uint16_t)(dw(4) - 1));
            set_a(2, vrd32(A6W(0xC4E)));
            charge(2);
            do {
                uint32_t a2 = a_reg(2);
                set_d16(1, (uint16_t)vrd16(a2));
                set_a(2, a2 + 2);
                set_d(7, 3);
                charge(2);
                CALL(L_5BBC, 0x5BFA);
                set_a(0, a_reg(0) + 2);
                charge(1);
                if (!dbf(4)) break;
                poll();
            } while (1);
            vwr32(A6W(0xC4E), a_reg(2));
            charge(2);
            break;
        case 0x5C0E:                                    /* 3: chars + stack args */
            set_d16(2, 0);
            set_d8(2, vrd8(a_reg(1))); set_a(1, a_reg(1) + 1);
            set_a(2, a_reg(7) + 8);
            set_d16(2, (uint16_t)(dw(2) - 1));
            charge(5);
            for (;;) {
                charge(2);                              /* cmpi.b, bcs */
                if (vrd8(a_reg(1)) >= 0x20) {
                    set_d16(0, 0);
                    set_d8(0, vrd8(a_reg(1))); set_a(1, a_reg(1) + 1);
                    charge(2);
                    CALL(L_5A2A, 0x5C26);
                    charge(1);                          /* bra */
                    poll();
                    continue;
                }
                uint32_t a2 = a_reg(2);
                set_d(1, vrd32(a2));
                set_a(2, a2 + 4);
                charge(1);
                CALL(L_5BB6, 0x5C2C);
                if (!dbf(2)) break;
                poll();
            }
            charge(1);                                  /* rts */
            break;
        case 0x5C60:                                    /* 14: time m'ss"fff from D6 */
        {
            uint32_t d6 = d_reg(6);
            set_d(6, (d6 << 4) | (d6 >> 28));
            set_d16(0, (uint16_t)(dw(6) & 0xF));
            charge(4);
            if (dw(0) == 0) { set_d16(0, 0x20); charge(1); CALL(L_5A2A, 0x5C72); charge(1); }
            else { set_d8(0, vrd8(0x5BD8u + sx16(dw(0)))); charge(1); CALL(L_5A2A, 0x5C7E); }
            if ((r = text_hex_digit(0x5C90))) return r;
            set_d16(0, 0x27);
            charge(1);
            CALL(L_5A2A, 0x5C98);
            set_d(2, 1);
            charge(1);
            do {
                if ((r = text_hex_digit(0x5CAC))) return r;
                if (!dbf(2)) break;
                poll();
            } while (1);
            set_d16(0, 0x22);
            charge(1);
            CALL(L_5A2A, 0x5CB8);
            set_d(2, 2);
            charge(1);
            do {
                if ((r = text_hex_digit(0x5CCC))) return r;
                if (!dbf(2)) break;
                poll();
            } while (1);
            w16_set(G_COL, (uint16_t)(w16(G_COL) - 9));
            w16_set(G_ROW, (uint16_t)(w16(G_ROW) + 1));
            set_d16(0, w16(G_COL)); set_d16(1, w16(G_ROW));
            set_a(0, TEXT_BASE);
            charge(6);
            poll();
            text_cursor_a0();
            break;
        }
        case 0x5A9A:                                    /* 0: end -- addq.l #4,SP ; rts (to our caller) */
            set_a(7, a_reg(7) + 4);
            charge(2);
            return RD_RTS;
        default:                                        /* codes past the table (never used) */
            return t;                                   /* the jmp already polled */
        }
        /* the handler's rts: back to 0x5A0A (not a call site, so the lifted
         * code resumes here, after a poll) */
        set_a(7, a_reg(7) + 4);
        poll();
    }
}


/* ============================ 0xC052: test mode ============================ */

/* WRAM bytes/words of the test-mode menus (A6 offsets unless noted) */
#define T_MODE    0xD9E    /* word: 1 while test mode runs                        */
#define T_PAGE    0xDAA    /* word: the page (0 = menu)                            */
#define T_STEP    0xDAC    /* word: the page's step                                */
#define T_SEL     0xDAE    /* word: the menu selection (FUN_00029b1c etc.)         */
#define T_SUB     0x5000   /* word: a page's sub-state                             */
#define T_ITEM    0x5002   /* word: the item being edited                          */
#define T_EDIT    0x5004   /* word: the edit state of that item                    */

static inline int key_start(void) { return (vrd8(0x10000812u) & 0x80) != 0; }   /* the input byte at 0x812, bit 7 */
static inline int key_exit(void)  { return (vrd8(0x10000812u) & 0x40) != 0; }   /* bit 6 */

/* print the text list at `a1` through FUN_00005a0a until its terminating 0 byte
 * (the jsr is at jsr_at; lea charged by the caller) */
static uint32_t print_list(uint32_t a1, uint32_t jsr_at)
{
    uint32_t r;
    set_a(1, a1);
    for (;;) {
        CALL(L_5A0A, jsr_at + 6);
        charge(2);                                      /* tst.b (A1), bne */
        if (vrd8(a_reg(1)) == 0) return 0;
        poll();
    }
}

/* the long 0x500A gets bit n set and the long 0x5060 bit n cleared (6 instructions) */
static void mark_changed(unsigned n)
{
    set_d(0, vrd32(A6W(0x500A)) | (1u << n));
    vwr32(A6W(0x500A), d_reg(0));
    set_d(0, vrd32(A6W(0x5060)) & ~(1u << n));
    vwr32(A6W(0x5060), d_reg(0));
    charge(6);
}

/* `move.w (x,A6),D0w ; andi.w #mask,D0w ; move.w (T,PC,D0w*2),D0w ; nop ;
 * jmp (T,PC,D0w)` -- a page's jump table; returns the target (polled) */
static uint32_t table_jump(uint32_t var, uint16_t mask, uint32_t tbl)
{
    set_d16(0, (uint16_t)(w16(var) & mask));
    set_d16(0, (uint16_t)vrd16(tbl + sx16(dw(0)) * 2u));
    charge(5);
    poll();
    return tbl + sx16(dw(0));
}

/* a step that ends by clearing the step, the page and the sub-state (4 instructions) */
static uint32_t leave_page(void)
{
    w16_set(T_STEP, 0); w16_set(T_PAGE, 0); w16_set(T_SUB, 0);
    charge(3);
    RTS();
}

/* the common "edit an item" finish: when START is pressed, the selection
 * returns to the item and the step to 1 (btst, beq; move, move) */
static uint32_t start_back_to_item(void)
{
    charge(2);
    if (key_start()) { w16_set(T_SEL, w16(T_ITEM)); w16_set(T_STEP, 1); charge(2); }
    RTS();
}

/* a "menu page" step 1: on START mark the setting group changed and go to step
 * 2; then move the selection (FUN_00029ba4, limits at a0), remember it as the
 * item and redraw (FUN_00029bf4, strings at a1); continue in `redraw` */
static uint32_t menu_select_step(unsigned bit, int clear_edit, int clear_dd8, uint32_t a0, uint32_t a1,
                                 uint32_t ret_ba4, uint32_t ret_bf4, uint32_t redraw)
{
    uint32_t r;
    charge(2);                                          /* btst, beq */
    if (key_start()) {
        mark_changed(bit);
        if (clear_dd8) { w16_set(0xDD8, 0); charge(1); }
        if (clear_edit) { w16_set(T_EDIT, 0); charge(1); }
        w16_set(T_STEP, 2);
        charge(1);
    }
    set_a(0, a0);
    charge(1);
    CALL(L_29BA4, ret_ba4);
    w16_set(T_ITEM, w16(T_SEL));
    set_a(1, a1);
    vwr32(A6W(0xDBA), a1);
    charge(3);
    CALL(L_29BF4, ret_bf4);
    charge(1);                                          /* bra.w */
    return redraw;                                      /* forward: no poll */
}


/* A jump-table target inside the lifted function's address range but owned by
 * another function: the lifted code resumes there through its switch default,
 * `rr_jump(pc_, pc_)`, i.e. with the TARGET as the jumping address -- which the
 * checker does not treat as leaving the function, so the other function's code
 * runs as part of this call. Do exactly that (the jmp has already polled). */
static uint32_t jump_as_lifted(uint32_t t)
{
    rr_jump(t, t);
    return RD_UNWIND;
}

/* page 0: the test-mode menu (0x29CD8) */
static uint32_t tm_menu(void)
{
    uint32_t r;
    uint32_t t = table_jump(T_STEP, 1, 0x29CEAu);
    if (t == 0x29CEEu) {                                /* draw the menu */
        CALL(L_28632, 0x29CF4);
        CALL(L_575A, 0x29CFA);
        CALL(L_57A4, 0x29D00);
        CALL(L_B8B0, 0x29D06);
        CALL(L_2DB74, 0x29D0A);
        w16_set(0x57B4, 0xFFFF);
        charge(3);                                      /* move, cmpi, bne */
        if (w16(0xDE0) == 2) CALL(L_2F0FC, 0x29D1C);
        charge(1);                                      /* lea */
        if ((r = print_list(0x29EB0u, 0x29D20u))) return r;
        w16_set(0xDCA, 7);
        set_a(1, 0x29E90u);
        vwr32(A6W(0xDCE), 0x29E90u);
        w16_set(T_SUB, 0);
        w16_set(T_SEL, 0);
        w16_set(T_STEP, (uint16_t)(w16(T_STEP) + 1));
        charge(6);
        RTS();
    }
    /* 0x29D48: choose a page */
    CALL(L_29D9C, 0x29D4C);
    charge(1);                                          /* nop */
    CALL(L_46A6, 0x29D54);
    CALL(L_29B1C, 0x29D5A);
    t = table_jump(0xDCC, 3, 0x29D6Cu);
    if (t == 0x29D76u) { w16_set(T_STEP, 0); w16_set(T_PAGE, 0); charge(2); }
    else if (t == 0x29D84u) {
        set_d16(0, (uint16_t)(w16(T_SEL) + 1));
        w16_set(T_PAGE, dw(0));
        w16_set(T_STEP, 0); w16_set(T_SEL, 0);
        charge(5);
    }
    RTS();
}

/* page 7 (0x2C2CC) */
static uint32_t tm_page7(void)
{
    uint32_t r;
    w16_set(0x57B4, 1);
    charge(1);
    CALL(L_29D9C, 0x2C2D6);
    uint32_t t = table_jump(T_SUB, 3, 0x2C2E8u);
    if (t == 0x2C2F0u) { w16_set(T_SUB, 0); charge(1); RTS(); }
    if (t == 0x2C2F6u) {
        CALL(L_5770, 0x2C2FC);
        w16_set(0xDE2, 3);
        charge(1);
        CALL(L_5868, 0x2C308);
        CALL(L_575A, 0x2C30E);
        w16_set(T_STEP, 0); w16_set(T_SUB, 1);
        charge(2);
        RTS();
    }
    if (t != 0x2C31Au) return jump_as_lifted(t);        /* 0x2C71C: another function */
    t = table_jump(T_STEP, 3, 0x2C32Cu);
    if (t == 0x2C334u) {
        CALL(L_575A, 0x2C33A);
        w16_set(0xC5E, 0xC000);
        charge(2);                                      /* move.w, lea */
        if ((r = print_list(0x2CC64u, 0x2C344u))) return r;
        w16_set(T_STEP, 1); w16_set(T_SEL, 2); w16_set(T_ITEM, 2); w16_set(0x5202, 1);
        charge(4);
        static const struct { void (*f)(void); uint32_t ret; } rows[15] = {
            { L_2C572, 0x2C36A }, { L_2C5A0, 0x2C36E }, { L_2C5BE, 0x2C372 }, { L_2C5DC, 0x2C376 },
            { L_2C86A, 0x2C37A }, { L_2C5FA, 0x2C37E }, { L_2C634, 0x2C382 }, { L_2C646, 0x2C386 },
            { L_2C658, 0x2C38A }, { L_2C6A8, 0x2C38E }, { L_2C6BA, 0x2C392 }, { L_2C6CC, 0x2C396 },
            { L_2C66E, 0x2C39A }, { L_2C680, 0x2C39E }, { L_2C692, 0x2C3A2 } };
        for (int i = 0; i < 15; i++) CALL(rows[i].f, rows[i].ret);
        charge(1);
        return 0x2C6E2u;
    }
    if (t == 0x2C3A6u) {
        charge(2);
        if (key_start()) { w16_set(T_EDIT, 0); w16_set(T_STEP, 2); charge(2); }
        set_a(0, 0x2C3D6u);
        charge(1);
        CALL(L_29BA4, 0x2C3C0);
        w16_set(T_ITEM, w16(T_SEL));
        set_a(1, 0x2CC54u);
        vwr32(A6W(0xDBA), 0x2CC54u);
        charge(3);
        CALL(L_29BF4, 0x2C3D2);
        charge(1);
        return 0x2C4FCu;
    }
    /* 0x2C3D8: edit */
    t = table_jump(T_ITEM, 3, 0x2C3EAu);
    if (t == 0x2C3F2u) {
        t = table_jump(T_EDIT, 3, 0x2C404u);
        if (t == 0x2C40Cu) {
            set_d16(0, w16(T_ITEM));
            set_a(0, 0x2C4F8u + sx16(dw(0)) * 2u);
            w16_set(T_SEL, w16(0x5202));
            charge(3);
            CALL(L_29BCC, 0x2C420);
            w16_set(0x5202, w16(T_SEL));
            charge(1);
            CALL(L_2C4FC, 0x2C42A);
            charge(2);
            if (key_start()) { w16_set(T_EDIT, 1); charge(1); }
            RTS();
        }
        if (t == 0x2C43Au) {
            CALL(L_2C4FC, 0x2C43E);
            charge(2);
            if (w16(0x5202) == 0) { w16_set(T_EDIT, 2); charge(1); RTS(); }
            w16_set(0x5202, 1); w16_set(T_SEL, w16(T_ITEM)); w16_set(T_EDIT, 0); w16_set(T_STEP, 1);
            charge(4);
            RTS();
        }
        /* 0x2C464 */
        CALL(L_2C7C2, 0x2C468);
        w16_set(0x5202, 1); w16_set(T_SEL, w16(T_ITEM)); w16_set(T_EDIT, 0); w16_set(T_STEP, 0);
        charge(5);
        return 0x2C4FCu;
    }
    if (t == 0x2C482u) {
        CALL(L_2C4D6, 0x2C486);
        CALL(L_2C4FC, 0x2C48A);
        charge(2);
        if (key_start()) {
            mark_changed(4);
            w16_set(T_SEL, w16(T_ITEM)); w16_set(T_STEP, 1);
            charge(2);
        }
        RTS();
    }
    if (t == 0x2C4B8u) { w16_set(T_STEP, 0); w16_set(T_SEL, 0); w16_set(T_SUB, 2); charge(3); RTS(); }
    return leave_page();                                /* 0x2C4C8 */
}

/* page 5 (0x2CE20): a sequence of steps, each waiting for a key */
static uint32_t tm_page5(void)
{
    uint32_t r;
    charge(2);
    if (key_exit()) { w16_set(T_PAGE, 0); w16_set(T_STEP, 0); charge(3); return RD_RTS; }
    w16_set(0x57B4, 1);
    charge(1);
    CALL(L_29D9C, 0x2CE40);
    uint32_t t = table_jump(T_STEP, 0xF, 0x2CE52u);
    switch (t) {
    case 0x2CE72: w16_set(T_STEP, 0); charge(1); RTS();
    case 0x2CE78:
        w16_set(0xDE2, 3);
        charge(1);
        CALL(L_C69A, 0x2CE84);
        break;
    case 0x2CE8A:
        CALL(L_2CF84, 0x2CE8E);
        CALL(L_2CEF0, 0x2CE92);
        break;
    case 0x2CE98:
        CALL(L_2D072, 0x2CE9C);
        vwr8(0x9002FFEEu, 0); vwr8(0x90037FEEu, 0); vwr8(0x9003FFEEu, 0);
        vwr8(0x9002FFE8u, 0); vwr8(0x90037FE8u, 0xB4); vwr8(0x9003FFE8u, 0);
        vwr8(0x9002FFE0u, 0xFF); vwr8(0x90037FE0u, 0xFF); vwr8(0x9003FFE0u, 0xFF);
        charge(9);
        break;
    default: {
        /* the key-wait steps: continue when any key but the one at bit 6 is down */
        set_d8(0, vrd8(0x10000812u));
        set_d8(0, (uint8_t)(d_reg(0) & 0xBF));
        charge(3);
        if ((uint8_t)d_reg(0) == 0) RTS();
        switch (t) {
        case 0x2D396: break;
        case 0x2D3A6: CALL(L_2D15E, 0x2D3B4); break;
        case 0x2D3BA: CALL(L_2D178, 0x2D3C8); break;
        case 0x2D3CE:
            vwr8(0x9002FFE0u, 0xB8); vwr8(0x90037FE0u, 0xB8); vwr8(0x9003FFE0u, 0xB8);
            charge(3);
            break;
        case 0x2D3F6:
            vwr8(0x9002FFE0u, 0x1E); vwr8(0x90037FE0u, 0x1E); vwr8(0x9003FFE0u, 0x1E);
            charge(3);
            break;
        case 0x2D41E:
            vwr8(0x9002FFE0u, 0); vwr8(0x90037FE0u, 0); vwr8(0x9003FFE0u, 0);
            vwr8(0x9002FFECu, 0x33); vwr8(0x90037FECu, 0x33); vwr8(0x9003FFECu, 0x33);
            charge(6);
            CALL(L_2D1B2, 0x2D45C);
            break;
        case 0x2D462:
            CALL(L_2D1DA, 0x2D470);
            set_a(1, 0x9009E620u);
            charge(1);
            break;
        case 0x2D47C: CALL(L_2D32A, 0x2D48A); break;
        case 0x2D490: CALL(L_2D35C, 0x2D49E); break;
        case 0x2D4A4: w16_set(T_STEP, 1); charge(1); RTS();
        }
    }
    }
    w16_set(T_STEP, (uint16_t)(w16(T_STEP) + 1));
    charge(1);
    RTS();
}

/* page 3 (0x2D4CC) */
static uint32_t tm_page3_items(void);
static uint32_t tm_page3_sound(void);
static uint32_t tm_page3(void)
{
    uint32_t r;
    CALL(L_29D9C, 0x2D4D0);
    uint32_t t = table_jump(T_SUB, 3, 0x2D4E2u);
    if (t == 0x2D4EAu) { w16_set(T_SUB, 0); charge(1); RTS(); }
    if (t == 0x2D4F0u) {
        CALL(L_5770, 0x2D4F6);
        w16_set(0xDE2, 3);
        charge(1);
        CALL(L_5868, 0x2D502);
        CALL(L_575A, 0x2D508);
        CALL(L_2DB74, 0x2D50C);
        w16_set(T_STEP, 0); w16_set(T_SUB, 1);
        charge(2);
        RTS();
    }
    if (t == 0x2D518u) return tm_page3_items();
    return tm_page3_sound();                            /* 0x2D7C8 */
}

static uint32_t tm_page3_items(void)
{
    uint32_t r;
    uint32_t t = table_jump(T_STEP, 3, 0x2D52Au);
    if (t == 0x2D532u) { w16_set(T_STEP, 0); charge(1); RTS(); }
    if (t == 0x2D538u) {
        CALL(L_2DB74, 0x2D53C);
        CALL(L_575A, 0x2D542);
        charge(1);
        if ((r = print_list(0x2DD84u, 0x2D546u))) return r;
        w16_set(T_STEP, 1); w16_set(T_SEL, 0xD); w16_set(T_ITEM, 0xD);
        charge(3);
        RTS();
    }
    if (t == 0x2D564u) {
        CALL(L_46A6, 0x2D56A);
        return menu_select_step(3, 0, 0, 0x2D5AEu, 0x2DD48u, 0x2D598u, 0x2D5AAu, 0x2D6C8u);
    }
    /* 0x2D5B0 */
    t = table_jump(T_ITEM, 0xF, 0x2D5C2u);
    if (t == 0x2D66Cu) {
        w16_set(T_STEP, 0); w16_set(T_SEL, 0); w16_set(T_SUB, (uint16_t)(w16(T_SUB) + 1));
        charge(3);
        RTS();
    }
    if (t == 0x2D67Au) return leave_page();
    uint32_t base = 0;
    if (t != 0x2D5E2u) {                                /* 0x2D600 / 0x2D634: only with the byte 0x1072 set */
        charge(2);
        if (vrd8(0x10001072u) == 0) { w16_set(T_SEL, w16(T_ITEM)); w16_set(T_STEP, 1); charge(2); RTS(); }
        base = t == 0x2D600u ? 0x2D616u : 0x2D64Au;
    } else base = 0x2D5E2u;
    CALL(L_2D688, base + 4);
    CALL(L_2D6C8, base + 8);
    if (base == 0x2D64Au) CALL(L_2DA12, 0x2D656);
    return start_back_to_item();
}

static uint32_t tm_page3_sound(void)
{
    uint32_t r;
    uint32_t t = table_jump(T_STEP, 3, 0x2D7DAu);
    if (t == 0x2D7E2u) { w16_set(T_STEP, 0); charge(1); RTS(); }
    if (t == 0x2D7E8u) {
        CALL(L_46A6, 0x2D7EE);
        CALL(L_575A, 0x2D7F4);
        charge(1);
        if ((r = print_list(0x2DFDCu, 0x2D7F8u))) return r;
        w16_set(T_STEP, 1); w16_set(T_SEL, 7); w16_set(T_ITEM, 7); w16_set(0x56EC, 1);
        charge(4);
        RTS();
    }
    if (t == 0x2D81Cu) return menu_select_step(3, 1, 0, 0x2D864u, 0x2DFB8u, 0x2D84Eu, 0x2D860u, 0x2DA84u);
    /* 0x2D868 */
    t = table_jump(T_ITEM, 0xF, 0x2D87Au);
    if (t == 0x2D89Au) {
        CALL(L_2DA52, 0x2D89E);
        CALL(L_2DA84, 0x2D8A2);
        return start_back_to_item();
    }
    if (t == 0x2D8B8u) {
        t = table_jump(T_EDIT, 3, 0x2D8CAu);
        if (t == 0x2D8D2u) {
            set_d16(0, w16(T_ITEM));
            set_a(0, 0x2DA76u + sx16(dw(0)) * 2u);
            w16_set(T_SEL, w16(0x56EC));
            charge(3);
            CALL(L_29BCC, 0x2D8E6);
            w16_set(0x56EC, w16(T_SEL));
            charge(1);
            CALL(L_2DA84, 0x2D8F0);
            charge(2);
            if (key_start()) { w16_set(T_EDIT, 1); charge(1); }
            RTS();
        }
        if (t == 0x2D900u) {
            CALL(L_2DA84, 0x2D904);
            charge(2);
            if (w16(0x56EC) == 0) { w16_set(T_EDIT, 2); charge(1); RTS(); }
            w16_set(0x56EC, 1); w16_set(T_SEL, w16(T_ITEM)); w16_set(T_EDIT, 0); w16_set(T_STEP, 1);
            charge(4);
            RTS();
        }
        /* 0x2D92A */
        CALL(L_2DA12, 0x2D92E);
        w16_set(0x56EC, 1); w16_set(T_SEL, w16(T_ITEM)); w16_set(T_EDIT, 0); w16_set(T_STEP, 1);
        charge(5);
        return 0x2DA84u;
    }
    if (t == 0x2D94Au || t == 0x2D982u) {
        uint32_t b = t;
        set_d16(0, w16(T_ITEM));
        set_a(1, 0x1000106Du + sx16(dw(0)) - 1u);
        charge(4);
        CALL(L_2DA5E, b + 0x12);
        CALL(L_2DA84, b + 0x16);
        if (b == 0x2D94Au) {
            charge(2);
            if (key_start()) {
                w16_set(T_SEL, w16(T_ITEM));
                w16_set(0x5006, 0);
                vwr8(A6W(0x5007), vrd8(0x10001071u));
                w16_set(T_STEP, 1);
                charge(4);
            }
            RTS();
        }
        set_a(1, 0x10001072u);
        charge(3);                                      /* lea, tst.w, bne */
        if (vrd16(0x10001072u) == 0) { CALL(L_2D9DA, 0x2D9A6); charge(1); }
        else CALL(L_2D9F8, 0x2D9AC);
        CALL(L_2DA12, 0x2D9B0);
        return start_back_to_item();
    }
    if (t == 0x2DA42u) { w16_set(T_STEP, 0); w16_set(T_SEL, 0); w16_set(T_SUB, 1); charge(3); RTS(); }
    return leave_page();                                /* 0x2D67A */
}

/* page 2 (0x2E108): the game-option bytes at 0x1040 */
static uint32_t tm_page2(void)
{
    uint32_t r;
    w16_set(0x57B4, 1);
    charge(1);
    CALL(L_29D9C, 0x2E112);
    uint32_t t = table_jump(T_STEP, 3, 0x2E124u);
    if (t == 0x2E12Cu) { w16_set(T_STEP, 0); charge(1); RTS(); }
    if (t == 0x2E132u) {
        CALL(L_5770, 0x2E138);
        w16_set(0xDE2, 3);
        charge(1);
        CALL(L_5868, 0x2E144);
        CALL(L_575A, 0x2E14A);
        charge(1);
        if ((r = print_list(0x2E338u, 0x2E14Eu))) return r;
        w16_set(T_STEP, 1); w16_set(T_SEL, 0); w16_set(T_ITEM, 0);
        charge(3);
        RTS();
    }
    if (t == 0x2E16Cu) return menu_select_step(2, 0, 0, 0x2E1B0u, 0x2E324u, 0x2E19Au, 0x2E1ACu, 0x2E26Cu);
    /* 0x2E1B4 */
    t = table_jump(T_ITEM, 7, 0x2E1C6u);
    if (t == 0x2E1D6u) {
        CALL(L_2E23E, 0x2E1DA);
        CALL(L_2E26C, 0x2E1DE);
        return start_back_to_item();
    }
    if (t == 0x2E1F4u) {
        set_d16(0, w16(T_ITEM));
        set_a(0, 0x2E262u + sx16(dw(0)) * 2u);
        uint32_t a1 = 0x10001040u + sx16(dw(0));
        set_a(1, a1);
        set_d(0, vrd8(a1));
        w16_set(T_SEL, dw(0));
        charge(8);                                      /* move, lea, nop, lea, adda, moveq, move.b, move.w */
        CALL(L_29BCC, 0x2E212);
        vwr8(a_reg(1), vrd8(A6W(0xDAF)));
        charge(1);
        CALL(L_2E26C, 0x2E21A);
        return start_back_to_item();
    }
    return leave_page();                                /* 0x2E230 */
}

/* the sound-test slot: 0x5774 = the sound CPU word 0x60005100[0x5780 & 0xFF] */
static void sound_slot_read(void)
{
    set_a(0, 0x60005100u);
    set_a(1, A6W(0x5774));
    set_d16(0, (uint16_t)((vrd16(A6W(0x5780)) << 1) & 0x1FE));
    set_a(0, 0x60005100u + sx16(dw(0)));
    vwr16(A6W(0x5774), (uint16_t)vrd16(a_reg(0)));
    charge(7);
}

/* page 6 (0x2E400): the sound test */
static uint32_t tm_page6(void)
{
    uint32_t r;
    w16_set(0x57B4, 1);
    charge(1);
    CALL(L_29D9C, 0x2E40A);
    uint32_t t = table_jump(T_STEP, 3, 0x2E41Cu);
    if (t == 0x2E424u) { w16_set(T_STEP, 0); charge(1); RTS(); }
    if (t == 0x2E42Au) {
        CALL(L_5770, 0x2E430);
        w16_set(0xDE2, 3);
        charge(1);
        CALL(L_5876, 0x2E43C);
        CALL(L_575A, 0x2E442);
        charge(1);
        if ((r = print_list(0x2E878u, 0x2E446u))) return r;
        uint32_t a1 = A6W(0x5774);
        set_d(7, 7);
        charge(2);
        do {
            vwr16(a1, 0); a1 += 2;
            set_a(1, a1);
            charge(1);
            if (!dbf(7)) break;
            poll();
        } while (1);
        sound_slot_read();
        CALL(L_2E492, 0x2E478);
        w16_set(0xDD8, 0); w16_set(T_SEL, 2); w16_set(T_ITEM, 2); w16_set(T_STEP, 1);
        charge(4);
        RTS();
    }
    if (t == 0x2E4A0u) return menu_select_step(2, 1, 1, A6W(0x57BA), 0x2E85Cu, 0x2E4D8u, 0x2E4EAu, 0x2E6FCu);
    /* 0x2E4F0 */
    t = table_jump(T_ITEM, 7, 0x2E502u);
    if (t == 0x2E510u) {
        t = table_jump(T_EDIT, 1, 0x2E522u);
        CALL(L_2E646, t + 4);
        CALL(L_2E6FC, t + 8);
        if (t == 0x2E526u) {
            vwr16(0x60005000u, 0x405D);
            w16_set(0x56EE, 1); w16_set(T_EDIT, 1);
            charge(3);
            RTS();
        }
        charge(2);
        if (key_start()) {
            w16_set(T_SEL, w16(T_ITEM));
            w16_set(0x56EE, 0);
            vwr16(0x60005000u, 0);
            w16_set(T_EDIT, 0); w16_set(T_STEP, 1);
            charge(5);
        }
        RTS();
    }
    if (t == 0x2E570u) {
        w16_set(T_STEP, 1);
        vwr16(0x60005000u, 0x4001);
        w16_set(0xDD8, 0x3C0);
        charge(4);
        return 0x2E6FCu;
    }
    if (t == 0x2E588u) {
        CALL(L_2E6A2, 0x2E58C);
        CALL(L_2E6FC, 0x2E590);
        charge(2);
        if (key_exit()) {
            vwr16(0x60005000u, 0);
            w16_set(T_SEL, w16(T_ITEM)); w16_set(T_STEP, 1);
            charge(3);
        }
        charge(2);
        if (key_start()) {
            set_d(1, 0);
            set_d16(1, (uint16_t)(w16(0x577E) | 0x4000));
            vwr16(0x60005000u, dw(1));
            w16_set(0xDD8, 0xF0);
            charge(5);
        }
        RTS();
    }
    if (t == 0x2E5CCu) {
        CALL(L_2E66C, 0x2E5D0);
        CALL(L_2E6FC, 0x2E5D4);
        sound_slot_read();
        return start_back_to_item();
    }
    if (t == 0x2E602u) {
        CALL(L_2E66C, 0x2E606);
        CALL(L_2E6FC, 0x2E60A);
        set_a(0, A6W(0x5774));
        set_a(1, 0x60005100u);
        set_d16(0, (uint16_t)((vrd16(A6W(0x5780)) << 1) & 0x1FE));
        set_a(1, 0x60005100u + sx16(dw(0)));
        vwr16(a_reg(1), (uint16_t)vrd16(A6W(0x5774)));
        charge(7);
        return start_back_to_item();
    }
    if (t == 0x2E638u) return leave_page();
    return t;                                           /* 0x31530: past the table (never) */
}

/* page 1 (0x2F364) */
static uint32_t tm_page1(void)
{
    uint32_t r;
    uint32_t t = table_jump(T_STEP, 3, 0x2F376u);
    if (t == 0x2F37Eu) { w16_set(T_STEP, 0); charge(1); RTS(); }
    if (t == 0x2F384u) {
        CALL(L_5770, 0x2F38A);
        CALL(L_5876, 0x2F390);
        CALL(L_575A, 0x2F396);
        charge(1);
        if ((r = print_list(0x2F8D0u, 0x2F39Au))) return r;
        charge(2);
        if (vrd8(0x10001071u) == 0) {
            w16_set(0xC5E, 0xC000);
            set_a(1, 0x2F930u);
            charge(2);
            CALL(L_5A0A, 0x2F3BC);
        }
        w16_set(T_STEP, 1); w16_set(T_SEL, 1); w16_set(T_ITEM, 1); w16_set(0x57B8, 1); w16_set(0x57B4, 0xFFFF);
        charge(5);
        RTS();
    }
    if (t == 0x2F3F8u) {
        CALL(L_29D9C, 0x2F3FE);
        charge(2);
        if (key_start()) { mark_changed(3); w16_set(T_EDIT, 0); w16_set(T_STEP, 2); charge(2); }
        CALL(L_2F3DC, 0x2F42A);
        set_a(0, A6W(0x57BA));
        charge(1);
        CALL(L_29BA4, 0x2F434);
        w16_set(T_ITEM, w16(T_SEL));
        set_a(1, 0x2F8C4u);
        vwr32(A6W(0xDBA), 0x2F8C4u);
        charge(3);
        CALL(L_29BF4, 0x2F448);
        charge(1);
        return 0x2F5CCu;
    }
    /* 0x2F450 */
    t = table_jump(T_ITEM, 3, 0x2F462u);
    if (t == 0x2F59Au) return leave_page();
    if (t == 0x2F46Au) {
        CALL(L_29D9C, 0x2F470);
        CALL(L_2F5A8, 0x2F474);
        charge(2);
        if (w16(T_SEL) == 0) {
            w16_set(0xC5E, 0xC000);
            set_a(1, 0x2F930u);
            charge(2);
            CALL(L_5A0A, 0x2F48A);
            charge(1);
        } else {
            set_a(1, 0x2F947u);
            charge(1);
            CALL(L_5A0A, 0x2F496);
        }
        CALL(L_2F5CC, 0x2F49A);
        charge(2);
        if (key_start()) {
            w16_set(T_SEL, w16(T_ITEM));
            w16_set(0x5006, 0);
            vwr8(A6W(0x5007), vrd8(0x10001071u));
            w16_set(T_STEP, 1);
            charge(4);
        }
        RTS();
    }
    /* 0x2F4BC */
    t = table_jump(T_EDIT, 3, 0x2F4CEu);
    if (t == 0x2F4D6u) {
        CALL(L_29D9C, 0x2F4DC);
        set_a(0, 0x2F5C8u);
        w16_set(T_SEL, w16(0x57B8));
        charge(2);
        CALL(L_29BCC, 0x2F4EC);
        w16_set(0x57B8, w16(T_SEL));
        charge(1);
        CALL(L_2F5CC, 0x2F4F6);
        charge(2);
        if (key_start()) { w16_set(T_EDIT, 1); charge(1); }
        RTS();
    }
    if (t == 0x2F506u) {
        CALL(L_2F5CC, 0x2F50A);
        charge(2);
        if (w16(0x57B8) == 0) {
            w16_set(0x57A8, 0); w16_set(0x57AA, 0); w16_set(0x57AC, 0); w16_set(0x57B2, 0);
            w16_set(T_EDIT, 2);
            charge(5);
            RTS();
        }
        w16_set(0x57B8, 1); w16_set(T_SEL, w16(T_ITEM)); w16_set(0x57B4, 0xFFFF); w16_set(T_EDIT, 0); w16_set(T_STEP, 1);
        charge(5);
        RTS();
    }
    if (t == 0x2F546u) {
        charge(2);
        if (key_start()) { w16_set(T_EDIT, 3); charge(1); }
        CALL(L_2F7A8, 0x2F558);
        charge(1);
        return 0x2F5CCu;
    }
    /* 0x2F55C */
    w16_set(0x57B4, 0xFFFF);
    charge(1);
    CALL(L_29D9C, 0x2F568);
    charge(2);
    if (w16(0x57B4) == 1) {
        set_a(1, 0x2FA48u);
        charge(1);
        CALL(L_5A0A, 0x2F57A);
        w16_set(0x57B8, 1); w16_set(T_SEL, w16(T_ITEM)); w16_set(0x57B4, 0xFFFF); w16_set(T_EDIT, 0); w16_set(T_STEP, 1);
        charge(5);
    }
    charge(1);
    return 0x2F5CCu;
}

/* FUN_0000c052: the per-frame test-mode check and the TEST MODE itself.
 * The test switch (word 0x804) entering: 0xD9E = 1, the page state reset (page
 * 9 unless shared-RAM 0x60004030 bit 3); leaving: the state cleared, the
 * machine reset through 0x4000 when the boot state 0x800 is set. Then dispatch
 * on 0xD9E & 1 through the ROM long table 0xC148: 0 -> 0xAE04 (the game), 1 ->
 * the test mode: menu input (FUN_00029a54, FUN_00029a8a) and the page 0xDAA
 * through the table 0x29CB2 (0 menu, 1..7 the pages above; 4, 8, 9 are other
 * functions). */
static uint32_t rd_test_mode(void)
{
    uint32_t r;
    set_d16(0, w16(T_MODE));
    charge(3);                                          /* move, tst, beq */
    if (vrd16(0x10000804u) != 0) {
        charge(2);
        if (dw(0) != 1) {
            w16_set(T_MODE, 1);
            vwr8(A6W(0x11A8), 0); vwr8(A6W(0x11AA), 0); vwr8(A6W(0x11AB), 0);
            w16_set(0x11A2, 0); w16_set(0x11A4, 0); w16_set(0x11A6, 0);
            w16_set(0xC60, 0); w16_set(0x2394, 0); w16_set(0x2028, 0);
            charge(12);
            if (vrd8(0x60004030u) & 8) { w16_set(T_STEP, 0); w16_set(T_PAGE, 0); charge(2); }
            else { w16_set(T_PAGE, 9); w16_set(T_STEP, 0); w16_set(0x202A, 0); charge(4); }
        }
    } else {
        charge(2);
        if (dw(0) != 0) {
            w16_set(T_MODE, 0); w16_set(0xDA0, 0); w16_set(0xDA2, 0); w16_set(0x2002, 0);
            w16_set(0x2028, 0); w16_set(0x202A, 0); w16_set(0x2040, 0); w16_set(0xC60, 0);
            charge(8);
            CALL(L_575A, 0xC0DA);
            w16_set(0x4344, 0xFFFF);
            set_d16(0, (uint16_t)vrd16(0x60004020u));
            set_d(0, d_reg(0) & ~8u);
            vwr16(0x60004020u, dw(0));
            charge(4);
            CALL(L_28632, 0xC0F6);
            charge(2);
            if (w16(0xDE0) == 2) CALL(L_2F0FC, 0xC104);
            CALL(L_2C092, 0xC10A);
            CALL(L_2C000, 0xC110);
            CALL(L_2C088, 0xC116);
            charge(2);
            if (vrd16(0x10000800u) != 0) {
                vwr16(0x10000802u, 0xFFFF);
                vwr16(0x10000800u, 0);
                charge(3);
                return RD_JMP(0x4000u);                 /* restart */
            }
            w16_set(T_STEP, 0); w16_set(T_PAGE, 0);
            charge(2);
        }
    }
    set_d16(0, (uint16_t)(w16(T_MODE) & 1));
    set_d(0, vrd32(0xC148u + sx16(dw(0)) * 4u));
    charge(5);
    poll();
    uint32_t t = 0xC148u + d_reg(0);
    if (t != 0x29C90u) return t;                        /* 0xAE04: the game */
    CALL(L_29A54, 0x29C94);
    CALL(L_29A8A, 0x29C98);
    w16_set(0x4740, 0);
    set_d(0, 0);
    set_d16(0, (uint16_t)(w16(T_PAGE) & 0xF));
    set_d16(0, (uint16_t)vrd16(0x29CB2u + sx16(dw(0)) * 2u));
    charge(7);
    poll();
    t = 0x29CB2u + d_reg(0);
    switch (t) {
    case 0x29CD8: return tm_menu();
    case 0x2F364: return tm_page1();
    case 0x2E108: return tm_page2();
    case 0x2D4CC: return tm_page3();
    case 0x2CE20: return tm_page5();
    case 0x2E400: return tm_page6();
    case 0x2C2CC: return tm_page7();
    case 0x29CD2: w16_set(T_PAGE, 0); charge(1); RTS();
    default:      return jump_as_lifted(t);             /* pages 4, 8, 9: other functions */
    }
}

/* ============================== 0xE640 .. 0xE81E ============================== */

/* FUN_0000e640: D0 = 0x13, continue in 0xDBC8 (a backward branch: polls) */
static uint32_t rd_dbc8_with_13(void) { set_d(0, 0x13); charge(2); return RD_JMP(0xDBC8u); }

/* FUN_0000e646: D7 = 5, D6 = 0x38, continue in 0xE262 */
static uint32_t rd_e262_5_38(void) { set_d(7, 5); set_d(6, 0x38); charge(3); return RD_JMP(0xE262u); }

/* FUN_0000e6b2: FUN_0000dbc8 with D0 = 2, D1 = D1 & 0xF, D3w = 0x7A2; then
 * D7w = 1, D6 = 3 and continue in 0xE262 */
static uint32_t rd_e6b2(void)
{
    uint32_t r;
    set_d(0, 2);
    set_d(1, d_reg(1) & 0xF);
    set_d16(3, 0x7A2);
    charge(3);
    CALL(L_DBC8, 0xE6C2);
    set_d16(7, 1);
    set_d(6, 3);
    charge(3);
    return RD_JMP(0xE262u);
}

/* FUN_0000e758: unless the byte at 0x1071 is set, FUN_0000e262 with D7w = 1,
 * D6 = 0x2D; then continue in 0xE262 with D7w = 1, D6 = 0x1F */
static uint32_t rd_e758(void)
{
    uint32_t r;
    charge(2);                                          /* tst.b, bne */
    if (vrd8(0x10001071u) == 0) {
        set_d16(7, 1);
        set_d(6, 0x2D);
        charge(2);
        CALL(L_E262, 0xE76A);
    }
    set_d16(7, 1);
    set_d(6, 0x1F);
    charge(3);
    return RD_JMP(0xE262u);
}

/* FUN_0000e774: for each of the eight bytes at 0x1065 (their low three bits
 * mapped through the ROM word table 0xE820), FUN_0000dbc8 with D0w from ROM
 * 0xE830[n] (and first 0xE840[n] when the mapped value is 2; value 3 is not
 * counted in D6); then FUN_0000e262 with D7 = 0x13, D6 = 0x42, and with D7 = 1,
 * D6 = 0x6E when the byte at 0x106F is clear and D6b is non-zero; finally fill
 * a block of the text layer at 0x9009E588 with the word 0xF0BF (five rows,
 * with the tiles 0xF0A6..0xF0A9 at fixed places). */
static uint32_t rd_e774(void)
{
    uint32_t r;
    set_a(2, 0x10001065u);
    set_d(6, 0);
    set_d(7, 0);
    charge(3);
    do {
        set_d(1, 0);
        uint32_t a2 = a_reg(2);
        set_d8(1, vrd8(a2));
        set_a(2, a2 + 1);
        set_d16(1, (uint16_t)(dw(1) & 7));
        set_d16(1, (uint16_t)vrd16(0xE820u + sx16(dw(1)) * 2u));
        charge(6);                                      /* moveq, move.b, andi, move.w, cmpi, beq */
        if (dw(1) != 3) { set_d16(6, (uint16_t)(dw(6) + 1)); charge(1); }
        set_a(0, 0xE830u);
        charge(3);                                      /* lea, cmpi, bne */
        if (dw(1) == 2) {
            set_a(0, 0xE840u);
            set_d16(0, (uint16_t)vrd16(a_reg(0) + sx16(dw(7)) * 2u));
            charge(2);                                  /* lea, move.w */
            CALL(L_DBC8, 0xE7AA);
            set_a(0, 0xE830u);
            set_d16(1, 2);
            charge(2);
        }
        set_d16(0, (uint16_t)vrd16(a_reg(0) + sx16(dw(7)) * 2u));
        charge(1);
        CALL(L_DBC8, 0xE7BA);
        set_d16(7, (uint16_t)(dw(7) + 1));
        charge(3);                                      /* addq, cmpi, blt */
        if ((int16_t)dw(7) >= 8) break;
        poll();
    } while (1);
    set_d(7, 0x13);
    set_d(6, 0x42);
    charge(2);
    CALL(L_E262, 0xE7CA);
    charge(2);                                          /* tst.b, bne */
    if (vrd8(0x1000106Fu) == 0) {
        charge(2);                                      /* tst.b, beq */
        if ((uint8_t)d_reg(6) != 0) {
            set_d(7, 1);
            set_d(6, 0x6E);
            charge(2);
            CALL(L_E262, 0xE7DE);
        }
    }
    uint16_t fill = 0xF0BF;
    uint32_t a0 = 0x9009E588u;
    set_d16(0, fill);
    set_d(7, 4);
    charge(3);                                          /* move.w, lea, moveq */
    do {
        set_d(6, 0xE);
        charge(1);
        do {
            vwr16(a0, fill); a0 += 2;
            charge(1);
            if (!dbf(6)) break;
            set_a(0, a0);
            poll();
        } while (1);
        vwr16(a0, 0xF0A6); a0 += 2;
        vwr16(a0, 0xF0A7); a0 += 2;
        a0 += 0x7C;
        vwr16(a0, 0xF0A8); a0 += 2;
        vwr16(a0, 0xF0A9); a0 += 2;
        charge(7);                                      /* move x2, adda, move x2, tst, bne */
        if (dw(7) != 0) { set_d(6, 0xE); charge(1); }
        else            { set_d(6, 6);   charge(2); }   /* moveq, bra */
        do {
            vwr16(a0, fill); a0 += 2;
            charge(1);
            if (!dbf(6)) break;
            set_a(0, a0);
            poll();
        } while (1);
        a0 += 0x140;
        charge(1);                                      /* adda */
        set_a(0, a0);
        if (!dbf(7)) break;
        poll();
    } while (1);
    set_a(0, a0);
    RTS();
}

/* ============================= 0x10286 .. 0x102DE ============================= */

/* FUN_00010286 / 102b2 / 102de: build a string at 0x2440 (a lead byte, then
 * FUN_0001024e copies the ROM string at A0 after it), then continue in 0xDEFE
 * with A0 = 0x2440, D0w = the text position, D1w = 0x20, 0x2010 = 0xE000 and
 * 0x2022 = the message number */
static uint32_t string_2440(uint32_t src, uint8_t lead, uint32_t ret, uint16_t pos, uint16_t msg)
{
    uint32_t r;
    set_a(0, src);
    set_a(1, A6W(0x2440));
    vwr8(A6W(0x2440), lead);
    set_a(1, A6W(0x2441));
    charge(3);
    CALL(L_1024E, ret);
    set_a(0, A6W(0x2440));
    set_d16(0, pos);
    set_d16(1, 0x20);
    w16_set(0x2010, 0xE000);
    w16_set(0x2022, msg);
    charge(6);
    return RD_JMP(0xDEFEu);
}
static uint32_t rd_string_6d(void) { return string_2440(0x10076u, 0x6D, 0x10294u, 0x45,  1); }
static uint32_t rd_string_6b(void) { return string_2440(0x100DCu, 0x6B, 0x102C0u, 0x45,  2); }
static uint32_t rd_string_69(void) { return string_2440(0x10032u, 0x69, 0x102EEu, 0x685, 3); }

/* ============================= 0x15838 .. 0x16210 ============================= */

/* FUN_00015838: FUN_000160c8, then continue in FUN_00015f20 */
static uint32_t rd_15838(void)
{
    uint32_t r;
    CALL(L_160C8, 0x1583C);
    charge(1);
    return 0x15F20u;                                    /* forward bra: no poll */
}

/* FUN_00015852: FUN_00016210, FUN_00015c0c, FUN_00015b3c, then continue in
 * FUN_00015d6c */
static uint32_t rd_15852(void)
{
    uint32_t r;
    CALL(L_16210, 0x15856);
    CALL(L_15C0C, 0x1585A);
    CALL(L_15B3C, 0x1585E);
    charge(1);
    return 0x15D6Cu;
}

/* a display-list header (FUN_00015862), then n+1 objects from the ROM table at
 * `tbl` through `obj` (D0..D2 = 0, D6w = the priority word 0xC40), then close
 * the list (0x15876, a backward branch) */
static uint32_t object_block(uint32_t hdr_ret, uint32_t tbl, uint32_t tbl2, uint16_t n,
                             void (*obj)(void), uint32_t obj_ret)
{
    uint32_t r;
    CALL(L_15862, hdr_ret);
    set_d(0, 0); set_d(1, 0); set_d(2, 0);
    set_a(0, tbl);
    charge(4);
    if (tbl2) { set_a(2, tbl2); charge(1); }
    set_d(7, n);
    set_d16(6, w16(0xC40));
    charge(2);
    do {
        CALL(obj, obj_ret);
        if (!dbf(7)) break;
        poll();
    } while (1);
    charge(1);
    return RD_JMP(0x15876u);
}
/* FUN_00015b3c / 15c0c / 15f20 / 160c8 / 16210: roadside object blocks
 * (FUN_0001595c) from the ROM tables following each function */
static uint32_t rd_objs_15b3c(void) { return object_block(0x15B40u, 0x15B5Cu, 0, 0x14, L_1595C, 0x15B54u); }
static uint32_t rd_objs_15c0c(void) { return object_block(0x15C10u, 0x15C2Cu, 0, 0x26, L_1595C, 0x15C24u); }
static uint32_t rd_objs_15f20(void) { return object_block(0x15F24u, 0x15F40u, 0, 0x2F, L_1595C, 0x15F38u); }
static uint32_t rd_objs_160c8(void) { return object_block(0x160CCu, 0x160E8u, 0, 0x23, L_1595C, 0x160E0u); }
static uint32_t rd_objs_16210(void) { return object_block(0x16214u, 0x16230u, 0, 0x0E, L_1595C, 0x16228u); }
/* FUN_00015d6c: eight objects through FUN_00015a36 (tables 0x15D90 / 0x15DD8) */
static uint32_t rd_objs_15d6c(void) { return object_block(0x15D70u, 0x15D90u, 0x15DD8u, 7, L_15A36, 0x15D88u); }

/* ============================= 0x1E9B0 .. 0x1EA30 ============================= */

/* FUN_0001e9b0: sound CPU level words: D0w = 0 writes 0xFF to 0x60005116;
 * otherwise 0x4100 to 0x60005118 and 0xFF - min(D0w >> 4, 0xFF) to 0x60005116 */
static uint32_t rd_snd_level(void)
{
    charge(2);                                          /* tst.w, beq */
    if (dw(0) == 0) { vwr16(0x60005116u, 0xFF); charge(2); return RD_RTS; }
    set_d16(1, 0x4100);
    vwr16(0x60005118u, 0x4100);
    set_d16(0, (uint16_t)(dw(0) >> 4));
    charge(5);                                          /* move.w, move.w, lsr, cmpi, ble */
    if ((int16_t)dw(0) > 0xFF) { set_d16(0, 0xFF); charge(1); }
    set_d16(1, (uint16_t)(0xFF - dw(0)));
    vwr16(0x60005116u, dw(1));
    charge(4);                                          /* move.w, sub.w, move.w, rts */
    return RD_RTS;
}

/* FUN_0001ea30: unless bit 15 of the sound CPU word 0x60005018 is set, write
 * 0x40C0 to it. D0w = that word & 0x8000. */
static uint32_t rd_snd_5018_40c0(void)
{
    set_d16(0, (uint16_t)(vrd16(0x60005018u) & 0x8000));
    charge(3);
    if (dw(0) == 0) { vwr16(0x60005018u, 0x40C0); charge(1); }
    RTS();
}


/* ============================= 0x21446 .. 0x22088 ============================= */

/* The object block at 0x44C8: model word, three position words, three angle
 * words and a flags word, consumed by FUN_00021702 (purpose of the object not
 * yet identified). */
#define G_OBJ_44C8   0x44C8

/* FUN_00021446: object 0x686 at the origin, flags 4; continue in 0x21702 */
static uint32_t rd_obj_686(void)
{
    static const uint16_t v[8] = { 0x686, 0, 0, 0, 0, 0, 0, 4 };
    for (int i = 0; i < 8; i++) w16_set(G_OBJ_44C8 + 2u * (uint32_t)i, v[i]);
    charge(9);
    return 0x21702u;                                    /* forward bra: no poll */
}

/* The common tail of FUN_000215b8 / FUN_000215de: nothing when the word 0x204C
 * lies in 0x675..0x89A (return through the rts at 0x217D6); otherwise pick a
 * model from D1w and the frame counter 0xC4C -- base `early` + (counter / 10)
 * & 15 while the long 0x4846 - 0xFF00 is above the long 0x204A, else 0x6A7 +
 * ROM 0x2163E[(counter & 0x7C) >> 2] -- and place it at (-0x5D5E, 0x3BE,
 * -0x4E20) with flags 4, continuing in 0x21702. */
static uint32_t pick_timer_model(uint16_t early, int from_215de)
{
    set_d16(0, w16(0x204C));
    set_d16(0, (uint16_t)(dw(0) - 0x675));
    charge(4);
    if (dw(0) < 0x226) return 0x217D6u;                 /* bcs to an rts elsewhere */
    set_d(0, vrd32(A6W(0x4846)) - 0xFF00u);
    charge(4);
    if ((int32_t)d_reg(0) <= (int32_t)vrd32(A6W(0x204A))) {
        if (!from_215de) return 0x2161Eu;
        set_d16(1, 0x6A7);
        set_d16(0, w16(0xC4C));
        set_d16(0, (uint16_t)((dw(0) & 0x7C) >> 2));
        set_a(0, 0x2163Eu);
        set_d16(0, (uint16_t)vrd16(0x2163Eu + sx16(dw(0)) * 2u));
        set_d16(0, (uint16_t)(dw(0) + dw(1)));
        w16_set(G_OBJ_44C8, dw(0));
        charge(9);
    } else {
        set_d16(1, early);
        if (!from_215de) { charge(2); return 0x21602u; } /* move.w, bra into 215DE */
        uint32_t d0 = w16(0xC4C);
        d0 = ((d0 % 10u) << 16) | (d0 / 10u);           /* divu.w #10 */
        set_d(0, d0);
        set_d16(0, (uint16_t)((dw(0) & 0xF) + dw(1)));
        w16_set(G_OBJ_44C8, dw(0));
        charge(8);                                      /* move.w #, then 7 up to the bra */
    }
    static const uint16_t v[7] = { 0xA2A2, 0x3BE, 0xB1E0, 0, 0, 0, 4 };
    for (int i = 0; i < 7; i++) w16_set(G_OBJ_44C8 + 2u + 2u * (uint32_t)i, v[i]);
    charge(8);
    return 0x21702u;
}
/* FUN_000215b8: as FUN_000215de with base 0x687 (it branches into 0x215DE's code) */
static uint32_t rd_timer_model_687(void) { return pick_timer_model(0x687, 0); }
static uint32_t rd_timer_model_697(void) { return pick_timer_model(0x697, 1); }

/* FUN_000217d8: append display-list command 0x8017 (1, 0x6FFF x3, 0x66E, three
 * zero words, an identity rotation, flags 4) and terminate the list */
static uint32_t rd_dl_8017(void)
{
    uint32_t p = vrd32(G_DL_CURSOR);
    static const uint32_t v[18] = { 0x8017, 1, 0x6FFF, 0x6FFF, 0x6FFF, 0x66E, 0, 0, 0,
                                    0, 0x7FFF, 0, 0x7FFF, 0, 0x7FFF, 4 };
    for (int i = 0; i < 16; i++) { vwr32(p, v[i]); p += 4; }
    vwr32(p, 0xFFFFFFFFu);
    vwr32(G_DL_CURSOR, p);
    set_a(3, p);
    set_d(0, 0);
    charge(21);
    return RD_RTS;
}

/* FUN_0002184a: place each object of the ROM list at 0x21880 (model, three
 * position words; ends at a negative model) through FUN_00021702, flags 4 */
static uint32_t rd_obj_list_21880(void)
{
    uint32_t r;
    set_a(0, 0x21880u);
    w16_set(0x44D0, 0); w16_set(0x44D2, 0); w16_set(0x44D4, 0);
    charge(4);
    for (;;) {
        uint32_t a0 = a_reg(0);
        uint16_t m = (uint16_t)vrd16(a0);
        w16_set(G_OBJ_44C8, m);
        set_a(0, a0 + 2);
        charge(2);
        if ((int16_t)m < 0) RTS();
        a0 += 2;
        for (int i = 1; i <= 3; i++) { w16_set(G_OBJ_44C8 + 2u * (uint32_t)i, (uint16_t)vrd16(a0)); a0 += 2; }
        set_a(0, a0);
        w16_set(0x44D6, 4);
        charge(4);
        CALL(L_21702, 0x2187C);
        charge(1);
        poll();
    }
}

/* FUN_00021902: for the twelve entries n = 11..0 add ROM 0x219CC[n] to the
 * word 0x4406[n] and 0x40 to the word 0x438E[n] (two phase counters) */
static uint32_t rd_phase_step_12(void)
{
    set_d16(7, 0xB);
    charge(1);
    do {
        uint16_t n = dw(7);
        set_a(0, 0x219CCu);
        set_d16(0, (uint16_t)vrd16(0x219CCu + sx16(n) * 2u));
        uint32_t a = A6W(0x4406) + sx16(n) * 2u;
        vwr16(a, (uint16_t)(vrd16(a) + dw(0)));
        a = A6W(0x438E) + sx16(n) * 2u;
        vwr16(a, (uint16_t)(vrd16(a) + 0x40));
        charge(4);
        if (!dbf(7)) break;
        poll();
    } while (1);
    RTS();
}

/* 68K `muls.w Dm,Dn ; add.l Dn,Dn ; swap Dn`: Q15 multiply, result in the low word */
static uint32_t q15_swap(uint16_t a, uint16_t b)
{
    uint32_t p = (uint32_t)((int32_t)(int16_t)a * (int32_t)(int16_t)b);
    p += p;
    return (p << 16) | (p >> 16);
}

/* FUN_00021922: for entries n = D7w..0: scale the three words at A1 + n*8 by
 * the phase 0x438E[n] & 0x7FFF (Q15), add the words at A0 + n*8 and store them
 * in 0x43A6/0x43BE/0x43D6[n]; copy A0 + n*8 + 6 to 0x43EE[n] */
static uint32_t rd_phase_positions(void)
{
    do {
        uint32_t n = sx16(dw(7));
        uint32_t a0 = a_reg(0), a1 = a_reg(1);
        set_d16(0, (uint16_t)(vrd16(A6W(0x438E) + n * 2u) & 0x7FFF));
        uint16_t d0 = dw(0);
        uint32_t d1 = q15_swap((uint16_t)vrd16(a1 + n * 8u), d0);
        uint32_t d2 = q15_swap((uint16_t)vrd16(a1 + n * 8u + 2), d0);
        uint32_t d3 = q15_swap((uint16_t)vrd16(a1 + n * 8u + 4), d0);
        d1 = (d1 & 0xFFFF0000u) | (uint16_t)(d1 + vrd16(a0 + n * 8u));
        d2 = (d2 & 0xFFFF0000u) | (uint16_t)(d2 + vrd16(a0 + n * 8u + 2));
        d3 = (d3 & 0xFFFF0000u) | (uint16_t)(d3 + vrd16(a0 + n * 8u + 4));
        set_d(1, d1); set_d(2, d2); set_d(3, d3);
        vwr16(A6W(0x43A6) + n * 2u, (uint16_t)d1);
        vwr16(A6W(0x43BE) + n * 2u, (uint16_t)d2);
        vwr16(A6W(0x43D6) + n * 2u, (uint16_t)d3);
        vwr16(A6W(0x43EE) + n * 2u, (uint16_t)vrd16(a0 + n * 8u + 6));
        charge(21);
        if (!dbf(7)) break;
        poll();
    } while (1);
    RTS();
}

/* FUN_00021976: for n = the word 0x49D4 down to 0, place model 0x441E +
 * (0x4406[n] >> 12) at (0x43A6[n], 0x43BE[n], 0x43D6[n]) with angle word
 * 0x43EE[n] through FUN_00021702 (flags 0) */
static uint32_t rd_phase_objects(void)
{
    uint32_t r;
    for (;;) {
        set_d16(7, w16(0x49D4));
        uint32_t n = sx16(dw(7));
        uint16_t d0 = (uint16_t)vrd16(A6W(0x4406) + n * 2u);
        d0 = (uint16_t)((d0 << 4) | (d0 >> 12));
        d0 = (uint16_t)((d0 & 0xF) + w16(0x441E));
        set_d16(0, d0);
        w16_set(G_OBJ_44C8, d0);
        w16_set(0x44CA, (uint16_t)vrd16(A6W(0x43A6) + n * 2u));
        w16_set(0x44CC, (uint16_t)vrd16(A6W(0x43BE) + n * 2u));
        w16_set(0x44CE, (uint16_t)vrd16(A6W(0x43D6) + n * 2u));
        w16_set(0x44D2, (uint16_t)vrd16(A6W(0x43EE) + n * 2u));
        w16_set(0x44D0, 0); w16_set(0x44D4, 0); w16_set(0x44D6, 0);
        charge(13);
        CALL(L_21702, 0x219C4);
        uint16_t c = (uint16_t)(w16(0x49D4) - 1);
        w16_set(0x49D4, c);
        charge(2);
        if ((int16_t)c < 0) RTS();
        poll();
    }
}

/* FUN_000218de: step the phases (FUN_00021902), compute the positions of the
 * eight entries 7..0 from the ROM tables 0x219EC / 0x21A2C (FUN_00021922) and
 * place them as models 0x647.. (FUN_00021976) */
static uint32_t rd_phase_group(void)
{
    uint32_t r;
    CALL(L_21902, 0x218E2);
    set_a(0, 0x219ECu);
    set_a(1, 0x21A2Cu);
    set_d16(7, 7);
    w16_set(0x49D4, 7);
    w16_set(0x441E, 0x647);
    charge(5);
    CALL(L_21922, 0x218FC);
    CALL(L_21976, 0x21900);
    RTS();
}

/* FUN_00022088: from the word 0xDD2: D0w = |-(v >> 5)| - 2 (floored at 0),
 * D1w = 2 if -(v >> 5) is negative else 0; continue in 0x222E0 */
static uint32_t rd_22088(void)
{
    uint16_t d0 = (uint16_t)(0u - (uint16_t)((int16_t)w16(0xDD2) >> 5));
    set_d16(1, (uint16_t)(((uint16_t)((d0 << 2) | (d0 >> 14))) & 2));
    charge(8);
    if ((int16_t)d0 < 0) { d0 = (uint16_t)(0u - d0); charge(1); }
    d0 = (uint16_t)(d0 - 2);
    charge(2);
    if ((int16_t)d0 < 0) { d0 = 0; charge(1); }
    set_d16(0, d0);
    charge(1);
    return 0x222E0u;                                    /* forward bra */
}

/* ============================= 0x298DE .. 0x29C42 ============================= */

/* FUN_000298de: sound CPU word 0x6000501E = 0x4093 */
static uint32_t rd_snd_501e_4093(void) { vwr16(0x6000501Eu, 0x4093); charge(2); return RD_RTS; }

/* FUN_000298e8: turn a four-bit direction in D0 (bit 0 / 1: +D2 / -D2, bit 2 /
 * 3: -D1 / +D1; opposite pairs cancel) into the step (D1w, D2w), through the
 * ROM word table 0x298F6 (a computed jmp, which polls). D0w = the table word. */
static uint32_t rd_dir_step(void)
{
    uint16_t i = (uint16_t)((dw(0) & 0xF) * 2);
    uint16_t off = (uint16_t)vrd16(0x298F6u + i);
    set_d16(0, off);
    charge(4);
    poll();
    uint16_t d1 = dw(1), d2 = dw(2);
    switch (0x298F6u + sx16(off)) {
    case 0x29916: set_d16(1, 0); set_d16(2, 0); charge(3); break;
    case 0x2991C: set_d16(1, 0); charge(2); break;
    case 0x29920: set_d16(2, (uint16_t)(0u - d2)); set_d16(1, 0); charge(3); break;
    case 0x29926: set_d16(2, 0); charge(2); break;
    case 0x2992A: set_d16(1, (uint16_t)(0u - d1)); set_d16(2, 0); charge(3); break;
    case 0x29930: charge(1); break;
    case 0x29932: set_d16(2, (uint16_t)(0u - d2)); charge(2); break;
    case 0x29936: set_d16(1, (uint16_t)(0u - d1)); charge(2); break;
    case 0x2993A: set_d16(1, (uint16_t)(0u - d1)); set_d16(2, (uint16_t)(0u - d2)); charge(3); break;
    }
    return RD_RTS;
}

/* One of FUN_000299ca's two identical halves: a held-input repeat timer. With
 * the byte `hold` set, the word `w` becomes 1. Otherwise w &= ~1 and, while
 * `inp` & `mask` is non-zero, the byte `cnt` advances by 8 and on each wrap w
 * grows by 4 (saturating at 0x4000); w then decays by 2 (floored at 0, which
 * also clears `cnt`). D0 = the new w, D7w = the last input byte read. */
static void repeat_timer(uint32_t hold, uint32_t inp, uint16_t mask, uint32_t cnt, uint32_t w)
{
    set_d(0, 1);
    set_d8(7, vrd8(A6W(0) - 0x8000u + hold));
    set_d16(7, (uint16_t)(dw(7) & 0xFF));
    charge(4);
    if (dw(7) == 0) {
        set_d16(0, (uint16_t)(vrd16(A6W(w)) & 0xFFFE));
        set_d8(7, vrd8(A6W(0) - 0x8000u + inp));
        set_d16(7, (uint16_t)(dw(7) & mask));
        charge(5);
        int decay = 1;
        if (dw(7) != 0) {
            uint8_t c = (uint8_t)(vrd8(A6W(cnt)) + 8);
            vwr8(A6W(cnt), c);
            charge(2);
            if (c != 0) decay = 0;
            else {
                set_d16(0, (uint16_t)(dw(0) + 4));
                charge(3);
                if (dw(0) >= 0x4000) { set_d16(0, 0x4002); charge(1); }
            }
        }
        if (decay) {
            set_d16(0, (uint16_t)(dw(0) - 2));
            charge(2);
            if ((int16_t)dw(0) < 0) { set_d16(0, 0); vwr8(A6W(cnt), 0); charge(2); }
        }
    }
    vwr16(A6W(w), dw(0));
    charge(1);
}
/* FUN_000299ca: the two repeat timers 0xDC4 (inputs 0x812 / 0x80E & 0xF3,
 * counter 0xDC8) and 0xDC6 (inputs 0x814 / 0x810, counter 0xDC9) */
static uint32_t rd_repeat_timers(void)
{
    repeat_timer(0x0812, 0x080E, 0xF3, 0xDC8, 0xDC4);
    repeat_timer(0x0814, 0x0810, 0xFF, 0xDC9, 0xDC6);
    RTS();
}

/* FUN_00029a54: the three menu colour words 0xDBE/0xDC0/0xDC2 = 0xD000 /
 * 0xE000 / 0xC000, or all 0xB000 in the second half of every 16 frames (0xC4C) */
static uint32_t rd_menu_colours(void)
{
    uint16_t d1 = 0xD000, d2 = 0xE000, d3 = 0xC000;
    uint32_t d0 = (d_reg(0) & 0xFFFF0000u) | w16(0xC4C);
    uint32_t q = d0 / 16u;
    if (q <= 0xFFFF) d0 = ((d0 % 16u) << 16) | q;      /* divu.w; on overflow Dn is unchanged */
    d0 = (d0 << 16) | (d0 >> 16);
    set_d(0, d0);
    charge(8);
    if ((int16_t)d0 >= 8) { d1 = d2 = d3 = 0xB000; charge(3); }
    set_d16(1, d1); set_d16(2, d2); set_d16(3, d3);
    w16_set(0xDBE, d1); w16_set(0xDC0, d2); w16_set(0xDC2, d3);
    charge(4);
    return RD_RTS;
}

/* FUN_00029a8a: menu cursor input: the repeat timers (FUN_000299ca), then the
 * step from the direction bytes 0x812 (unit step -> 0xDB0) and 0x80E (step of
 * the repeat bit of 0xDC4, which restarts that timer); 0xDB2 = the first step,
 * 0xDB0 += the second, 0xDB4 = the sum; and while the 0x80E direction is held
 * the counter 0xDB6 grows to 0x1A4, and past 0xB4 frames 0xDB4 becomes the
 * counter byte 0xDC8 >> 7 (>> 3 once 0x1A4 is reached), negated unless the
 * direction is 1. */
static uint32_t rd_menu_cursor(void)
{
    uint32_t r;
    charge(1);
    if ((r = rd_call(L_299CA, 0x29A8Eu))) return r;
    set_d8(0, vrd8(0x10000812u));
    set_d(2, 1);
    charge(3);
    if ((r = rd_call(L_298E8, 0x29A98u))) return r;
    w16_set(0xDB0, dw(2));
    set_d16(2, (uint16_t)((w16(0xDC4) >> 1) & 1));
    charge(5);
    if (dw(2) != 0) { w16_set(0xDC4, 0); w16_set(0xDC8, 0x8000); charge(2); }
    set_d8(0, vrd8(0x1000080Eu));
    charge(2);
    if ((r = rd_call(L_298E8, 0x29ABCu))) return r;
    w16_set(0xDB2, w16(0xDB0));
    w16_set(0xDB0, (uint16_t)(w16(0xDB0) + dw(2)));
    w16_set(0xDB4, w16(0xDB0));
    set_d8(0, vrd8(0x1000080Eu));
    set_d16(0, (uint16_t)(dw(0) & 3));
    charge(6);
    if (dw(0) == 0) { w16_set(0xDB6, 0); charge(2); RTS(); }
    charge(2);                                          /* cmpi.b, bcc */
    if (vrd8(A6W(0xDC8)) < 0xF0) {
        charge(2);                                      /* tst.w, beq */
        if (w16(0xDB6) == 0) RTS();
    }
    set_d(1, vrd8(A6W(0xDC8)));
    w16_set(0xDB6, (uint16_t)(w16(0xDB6) + 1));
    charge(5);                                          /* moveq, move.b, addq, cmpi, bge */
    if ((int16_t)w16(0xDB6) >= 0x1A4) {
        w16_set(0xDB6, 0x1A4);
        set_d16(1, (uint16_t)(dw(1) >> 3));
        charge(2);
    } else {
        charge(2);                                      /* cmpi, blt */
        if ((int16_t)w16(0xDB6) < 0xB4) RTS();
        set_d16(1, (uint16_t)(dw(1) >> 7));
        charge(2);                                      /* lsr, bra */
    }
    set_d16(0, (uint16_t)(dw(0) - 1));
    charge(2);
    if (dw(0) != 0) { set_d16(1, (uint16_t)(0u - dw(1))); charge(1); }
    w16_set(0xDB4, dw(1));
    charge(1);
    RTS();
}

/* FUN_00029b1c: menu selection 0xDAE from the step 0xDB0 (wrapping between 0
 * and 0xDCA); 0xDCC = 1 when the input byte 0x812 has bit 6 set (and nothing
 * else happens), 3 when the selection moved, 2 when bit 7 is set, else 0. The
 * old item (when it moved) is redrawn in colour 0xC000 and the new one in
 * 0xDBE through FUN_00005a0a (strings from the pointer table at 0xDCE). */
static uint32_t rd_menu_select(void)
{
    uint32_t r;
    w16_set(0xDCC, 0);
    charge(3);
    if (vrd8(0x10000812u) & 0x40) { w16_set(0xDCC, 1); charge(2); return RD_RTS; }
    set_d16(1, w16(0xDAE));
    set_d16(2, dw(1));
    set_d16(0, w16(0xDB0));
    charge(4);
    if ((int16_t)dw(0) >= 0) {
        set_d16(1, (uint16_t)(dw(1) + dw(0)));
        charge(3);
        if ((int16_t)dw(1) > (int16_t)w16(0xDCA)) { set_d(1, 0); set_d(0, 0); charge(3); }
    } else {
        set_d16(1, (uint16_t)(dw(1) + dw(0)));
        charge(2);
        if ((int16_t)dw(1) < 0) { set_d16(1, w16(0xDCA)); charge(1); }
    }
    w16_set(0xDAE, dw(1));
    charge(3);
    if (dw(1) != dw(2)) {
        w16_set(0xC5E, 0xC000);
        set_a(1, vrd32(vrd32(A6W(0xDCE)) + sx16(dw(2)) * 4u));
        charge(3);
        CALL(L_5A0A, 0x29B70);
        w16_set(0xDCC, 3);
        charge(1);
    }
    w16_set(0xC5E, w16(0xDBE));
    set_d16(2, w16(0xDAE));
    set_a(1, vrd32(vrd32(A6W(0xDCE)) + sx16(dw(2)) * 4u));
    charge(4);
    CALL(L_5A0A, 0x29B8E);
    w16_set(0xC5E, 0xC000);
    charge(3);
    if (vrd8(0x10000812u) & 0x80) { w16_set(0xDCC, 2); charge(1); }
    RTS();
}

/* FUN_00029ba4 / FUN_00029bcc: 0xDB8 = the value 0xDAE; 0xDAE += the step
 * 0xDB0 / 0xDB2, wrapping (signed bytes) between the limits (A0) and 1(A0) */
static uint32_t step_wrap(uint32_t step)
{
    set_d16(0, w16(0xDAE));
    w16_set(0xDB8, dw(0));
    set_d16(0, (uint16_t)(dw(0) + w16(step)));
    uint32_t a0 = a_reg(0);
    charge(5);
    if ((int8_t)d_reg(0) > (int8_t)vrd8(a0 + 1)) {
        set_d(0, vrd8(a0));
        charge(3);
    } else {
        charge(2);
        if ((int8_t)d_reg(0) < (int8_t)vrd8(a0)) { set_d(0, vrd8(a0 + 1)); charge(2); }
    }
    w16_set(0xDAE, dw(0));
    charge(2);
    return RD_RTS;
}
static uint32_t rd_step_wrap_db0(void) { return step_wrap(0xDB0); }
static uint32_t rd_step_wrap_db2(void) { return step_wrap(0xDB2); }

/* FUN_00029bf4: redraw the menu item 0xDB8 (the previous one, if it changed)
 * in colour 0xC000 and the item 0xDAE in 0xDBE (0xD000 while 0xDAC is 2),
 * strings from the pointer table at 0xDBA, through FUN_00005a0a */
static uint32_t rd_menu_redraw(void)
{
    uint32_t r;
    set_d16(0, w16(0xDB8));
    charge(3);
    if (dw(0) != w16(0xDAE)) {
        w16_set(0xC5E, 0xC000);
        set_a(1, vrd32(vrd32(A6W(0xDBA)) + sx16(dw(0)) * 4u));
        charge(3);
        CALL(L_5A0A, 0x29C12);
    }
    charge(2);
    if (w16(0xDAC) == 2) { w16_set(0xC5E, 0xD000); charge(1); }
    else                 { w16_set(0xC5E, w16(0xDBE)); charge(2); }
    set_d16(0, w16(0xDAE));
    set_a(1, vrd32(vrd32(A6W(0xDBA)) + sx16(dw(0)) * 4u));
    charge(3);
    CALL(L_5A0A, 0x29C3A);
    w16_set(0xC5E, 0xC000);
    charge(1);
    RTS();
}

/* FUN_00029c42: the text colour 0xC5E for item D4w: 0xDBE (0xDC0 when the byte
 * A3[D4w] equals D5b) if D4w is the word 0x5002, else 0xD000 (0xE000) */
static uint32_t rd_item_colour(void)
{
    charge(2);
    if (dw(4) == w16(0x5002)) { set_d16(0, w16(0xDC0)); set_d16(1, w16(0xDBE)); charge(3); }
    else                      { set_d16(0, 0xE000);     set_d16(1, 0xD000);     charge(2); }
    charge(2);
    if (vrd8(a_reg(3) + sx16(dw(4))) == (uint8_t)d_reg(5)) { set_d16(1, dw(0)); charge(1); }
    w16_set(0xC5E, dw(1));
    charge(2);
    return RD_RTS;
}


/* ============================= 0x15A36 / 0x1A6A2 ============================= */

/* movem.l Dn,-(SP) / movem.l (SP)+,Dn for one register */
static inline void push32(uint32_t v) { uint32_t sp = a_reg(7) - 4; set_a(7, sp); vwr32(sp, v); }
static inline uint32_t pop32(void)    { uint32_t sp = a_reg(7), v = vrd32(sp); set_a(7, sp + 4); return v; }
/* append one long at A3 */
static inline void put_a3(uint32_t v) { uint32_t a3 = a_reg(3); vwr32(a3, v); set_a(3, a3 + 4); }
/* (-sin, cos) of angle word `a` from the sine table at A5: D1 = -A5[a & ~1],
 * D2 = A5[(a & ~1) + 0x4000], D0w = the cosine index (high halves untouched) */
static void sin_cos_a5(uint16_t a)
{
    uint32_t a5 = a_reg(5);
    a = (uint16_t)(a & 0xFFFE);
    set_d16(1, (uint16_t)vrd16(a5 + sx16(a)));
    a = (uint16_t)(a + 0x4000);
    set_d16(0, a);
    set_d16(2, (uint16_t)vrd16(a5 + sx16(a)));
    set_d16(1, (uint16_t)(0u - dw(1)));
}

/* FUN_00015a36: one 0x8002-style display-list entry at A3 for roadside object
 * D7w of the table at A0 (8-byte records at A0 + 8.. : x, y, z, model; A0 holds
 * the base height) relative to view D6w (0x2128/0x2148/0x2168), skipped when
 * farther than 0x2000 (in 1/32 units, city-block). The Y angle faces the viewer
 * (FUN_0000a98c bearing) when the angle record at A2 has its +6 flag set,
 * limited to +-0x3800 from the record's own angle; X/Z angles from the record,
 * sine table at A5. */
static uint32_t rd_facing_object(void)
{
    uint32_t r;
    uint32_t a0 = a_reg(0), n = sx16(dw(7)) * 8u, v = sx16(dw(6)) * 2u;
    uint32_t d1 = (sx16((uint16_t)vrd16(a0 + 8 + n)) << 4) - vrd32(A6W(0x2128) + v);
    uint32_t d2 = (sx16((uint16_t)vrd16(a0 + 0xC + n)) << 4) - vrd32(A6W(0x2168) + v);
    set_a(4, d1);
    set_d(5, d2);
    d1 = (uint32_t)((int32_t)d1 >> 5) & 0xFFFF;
    d2 = (uint32_t)((int32_t)d2 >> 5) & 0xFFFF;
    set_d(1, d1); set_d(2, d2);
    uint16_t d3 = (uint16_t)d1, d4 = (uint16_t)d2;
    charge(18);
    if ((int16_t)d3 < 0) { d3 = (uint16_t)(0u - d3); charge(1); }
    charge(2);
    if ((int16_t)d4 < 0) { d4 = (uint16_t)(0u - d4); charge(1); }
    set_d(4, d4);
    set_d(3, (uint32_t)d3 + d4);
    charge(3);
    if (d_reg(3) > 0x2000) RTS();
    charge(2);
    if ((uint16_t)d1 == 0) {
        charge(2);
        if ((uint16_t)d2 == 0) { set_d(0, 0); charge(2); goto have_angle; }
    }
    CALL(L_A98C, 0x15A94);
    set_d16(0, (uint16_t)(0x4000 - dw(0)));
    charge(2);
have_angle:
    set_d(1, a_reg(4));
    set_d(2, d_reg(5));
    set_d16(5, dw(0));
    set_d(0, (uint16_t)vrd16(a0 + 0xE + n));
    put_a3(d_reg(0));                                              /* model */
    put_a3(d_reg(1));                                              /* x */
    set_d(0, sx16((uint16_t)vrd16(a0 + 0xA + n)) + vrd32(a0) - vrd32(A6W(0x2148) + v));
    put_a3(d_reg(0));                                              /* y */
    put_a3(d_reg(2));                                              /* z */
    set_d(0, 0); set_d(1, 0); set_d(2, 0);
    uint32_t a2 = a_reg(2);
    sin_cos_a5((uint16_t)vrd16(a2 + n));
    put_a3(d_reg(1)); put_a3(d_reg(2));
    set_d16(0, (uint16_t)vrd16(a2 + 2 + n));
    charge(27);
    if (vrd16(a2 + 6 + n) != 0) {
        set_d(1, 0);
        set_d16(0, dw(5));
        set_d16(5, (uint16_t)(dw(5) - vrd16(a2 + 2 + n)));
        charge(4);
        if ((int16_t)dw(5) < 0) { set_d16(5, (uint16_t)(0u - dw(5))); set_d(1, 1); charge(2); }
        charge(2);
        if ((int16_t)dw(5) >= 0x3800) {
            set_d16(0, 0x3800);
            charge(3);
            if (dw(1) != 0) { set_d16(0, (uint16_t)(0u - dw(0))); charge(1); }
        }
    }
    sin_cos_a5(dw(0));
    put_a3(d_reg(1)); put_a3(d_reg(2));
    sin_cos_a5((uint16_t)vrd16(a2 + 4 + n));
    put_a3(d_reg(1)); put_a3(d_reg(2));
    put_a3(0);
    charge(16);
    RTS();
}

/* FUN_0001a6a2: drone D4 (byte offset) steering: |D2w| clamped to 3..13 into
 * 0x41A8; the bearing (FUN_0000a98c, 0 for a zero vector) from the drone's
 * position 0x16B0/0x16B4 to its next course point (course table 0xECDC via
 * 0x4AAA, point 0x16AE + 4); the push 0x16D0/0x16D2 = (-sin, -cos) of it
 * scaled by 0x800 and by the 0x41A8 speed (sine table at A5). */
static uint32_t rd_drone_push(void)
{
    uint32_t r;
    charge(2);
    if ((int16_t)dw(2) < 0) { set_d16(2, (uint16_t)(0u - dw(2))); charge(1); }
    charge(2);
    if ((int16_t)dw(2) < 3) { set_d(2, 3); charge(2); }
    else {
        charge(2);
        if ((int16_t)dw(2) > 0xD) { set_d16(2, 0xD); charge(1); }
    }
    w16_set(0x41A8, dw(2));
    set_a(4, 0xECDCu);
    uint32_t d4 = sx16(dw(4));
    set_d16(0, (uint16_t)(vrd16(A6W(0x4AAA) + d4) << 6));
    set_a(1, vrd32(0xECDCu + 0xC + sx16(dw(0))));
    set_d16(0, (uint16_t)(vrd16(A6W(0x16AE) + d4) + 4));
    uint32_t a1 = a_reg(1), i = sx16(dw(0)) * 4u;
    set_d16(1, (uint16_t)(vrd16(a1 + i) - vrd16(A6W(0x16B0) + d4)));
    set_d16(2, (uint16_t)(vrd16(a1 + 2 + i) - vrd16(A6W(0x16B4) + d4)));
    push32(d_reg(4));
    charge(12);
    charge(2);
    int zero = 0;
    if (dw(1) == 0) { charge(2); if (dw(2) == 0) zero = 1; }
    if (zero) { set_d(0, 0); charge(2); }
    else {
        CALL(L_A98C, 0x1A704);
        set_d16(0, (uint16_t)(0x4000 - dw(0)));
        charge(2);
    }
    set_d(4, pop32());
    d4 = sx16(dw(4));
    uint16_t a = dw(0);
    set_d16(6, a);
    set_d16(3, 0x800);
    uint32_t a5 = a_reg(5);
    uint16_t w = w16(0x41A8);
    uint16_t s = (uint16_t)(0u - (uint16_t)vrd16(a5 + sx16((uint16_t)(a & 0xFFFE))));
    uint32_t d1 = (uint32_t)((int32_t)(int16_t)s * 0x800);
    d1 = (d1 << 16) | (d1 >> 16);
    d1 = (uint32_t)((int32_t)(int16_t)d1 * (int32_t)(int16_t)w);
    uint16_t ci = (uint16_t)((a & 0xFFFE) + 0x4000);
    uint16_t c = (uint16_t)vrd16(a5 + sx16(ci));
    set_d16(0, c);
    uint32_t d2 = (uint32_t)((int32_t)(int16_t)c * 0x800);
    d2 = (d2 << 16) | (d2 >> 16);
    d2 = (uint32_t)((int32_t)(int16_t)d2 * (int32_t)(int16_t)w);
    d2 = (d2 & 0xFFFF0000u) | (uint16_t)(0u - (uint16_t)d2);
    set_d(1, d1); set_d(2, d2);
    vwr16(A6W(0x16D0) + d4, (uint16_t)d1);
    vwr16(A6W(0x16D2) + d4, (uint16_t)d2);
    charge(23);
    return RD_RTS;
}

/* ============================= 0x228B2 .. 0x2291E ============================= */

/* FUN_000228b2 / 228e8 / 2291e: set up one of three race-start variants
 * (0x46DE = n, 0x1100 = -1, 0x2342 / 0x2046 / 0x2048 / 0x478A per variant,
 * 0x204A = 0x70000), draw its message (FUN_00010286 / 102b2 / 102de) and
 * continue in 0x22A2C */
static uint32_t race_variant(uint16_t n, uint16_t sel, uint16_t v2046, uint16_t v2048,
                             uint16_t v478a, void (*msg)(void), uint32_t ret)
{
    uint32_t r;
    w16_set(0x46DE, n);
    w16_set(0x1100, 0xFFFF);
    w16_set(0x2342, sel);
    w16_set(0x2046, v2046);
    w16_set(0x2048, v2048);
    vwr32(A6W(0x204A), 0x70000);
    w16_set(0x478A, v478a);
    charge(7);
    CALL(msg, ret);
    charge(1);
    return 0x22A2Cu;                                    /* forward bra */
}
static uint32_t rd_race_variant_1(void) { return race_variant(1, 3, 4, 1, 0x6000, L_10286, 0x228E4u); }
static uint32_t rd_race_variant_2(void) { return race_variant(2, 0, 5, 2, 0x7000, L_102B2, 0x2291Au); }
static uint32_t rd_race_variant_3(void) { return race_variant(3, 2, 6, 3, 0x7000, L_102DE, 0x22950u); }

/* ============================= 0x27F44 .. 0x28706 ============================= */


/* the 2x2 tile block drawn by FUN_00028002 / FUN_00028044 at (D0w, D1w):
 * tiles D3w, D3w+1 / D3w+2, D3w+3 */
static void tile_block(void)
{
    set_a(0, TEXT_BASE);
    set_d16(4, dw(0));
    set_d16(5, (uint16_t)(dw(1) << 6));
    set_d16(4, (uint16_t)(dw(4) + dw(5)));
    set_d16(4, (uint16_t)(dw(4) << 1));
    uint32_t a0 = TEXT_BASE + sx16(dw(4));
    set_a(0, a0);
    uint16_t t = dw(3);
    vwr16(a0, t);        t++;
    vwr16(a0 + 2, t);    t++;
    vwr16(a0 + 0x80, t); t++;
    vwr16(a0 + 0x82, t);
    set_d16(3, t);
    set_d16(0, (uint16_t)(dw(0) + 2));
}

/* FUN_00028002: a row of 2x2 text-layer tile blocks from the list at A1:
 * column, row, count, then count tile codes (each + the two bases 0x5930 and
 * 0x5932), two columns apart */
static uint32_t rd_tile_row(void)
{
    uint32_t a2 = a_reg(1);
    set_d16(0, (uint16_t)vrd16(a2));
    set_d16(1, (uint16_t)vrd16(a2 + 2));
    set_d16(2, (uint16_t)(vrd16(a2 + 4) - 1));
    a2 += 6;
    set_a(2, a2);
    charge(5);
    do {
        set_d16(3, (uint16_t)(vrd16(a2) + w16(0x5930) + w16(0x5932)));
        a2 += 2;
        set_a(2, a2);
        tile_block();
        charge(18);
        if (!dbf(2)) break;
        poll();
    } while (1);
    RTS();
}

/* FUN_00028044: as FUN_00028002, but the first nine blocks (counter 0x5898)
 * use base 0x2000 instead of 0x5930 */
static uint32_t rd_tile_row_counted(void)
{
    w16_set(0x5898, 0);
    uint32_t a2 = a_reg(1);
    set_d16(0, (uint16_t)vrd16(a2));
    set_d16(1, (uint16_t)vrd16(a2 + 2));
    set_d16(2, (uint16_t)(vrd16(a2 + 4) - 1));
    a2 += 6;
    set_a(2, a2);
    charge(6);
    do {
        set_d16(3, (uint16_t)vrd16(a2));
        a2 += 2;
        set_a(2, a2);
        charge(3);
        if ((int16_t)w16(0x5898) > 8) { set_d16(3, (uint16_t)(dw(3) + w16(0x5930))); charge(1); }
        else                          { set_d16(3, (uint16_t)(dw(3) + 0x2000));     charge(2); }
        set_d16(3, (uint16_t)(dw(3) + w16(0x5932)));
        tile_block();
        w16_set(0x5898, (uint16_t)(w16(0x5898) + 1));
        charge(17);
        if (!dbf(2)) break;
        poll();
    } while (1);
    RTS();
}

/* FUN_00027f44: fill the tile-code buffer at 0x590E for a ranking line: the
 * eight digits of the long 0x593E[0x5906] (FUN_0000a8c8, packed decimal) as
 * codes digit*4, the separators 0xA2/0xA6 inserted (the last six codes shift
 * up), a 0x32, then the three characters of the name 0x594E[0x5906] ('.' ->
 * 0x9E, below '.' -> 0x32, digits -> 4*(c-'0'), letters -> 4*(c-'A')+0x36)
 * and two more 0x32 */
static uint32_t rd_rank_line(void)
{
    uint32_t r;
    set_a(0, A6W(0x593E));
    set_a(2, A6W(0x590E));
    set_d16(0, (uint16_t)(w16(0x5906) << 2));
    set_a(0, A6W(0x593E) + sx16(dw(0)));
    set_d(1, vrd32(a_reg(0)));
    charge(6);
    CALL(L_A8C8, 0x27F5C);
    set_d(6, 3);
    charge(1);
    do {
        set_d(5, 1);
        charge(1);
        do {
            uint32_t d0 = d_reg(0);
            d0 = (d0 << 4) | (d0 >> 28);
            set_d(0, d0);
            set_d8(1, (uint8_t)d0);
            set_d16(1, (uint16_t)((dw(1) & 0xF) << 2));
            uint32_t a2 = a_reg(2);
            vwr16(a2, dw(1));
            set_a(2, a2 + 2);
            charge(6);
            if (!dbf(5)) break;
            poll();
        } while (1);
        if (!dbf(6)) break;
        poll();
    } while (1);
    uint32_t a3 = A6W(0x591A);
    set_a(3, a3);
    vwr16(a3 + 4, (uint16_t)vrd16(a3));
    vwr16(a3 + 2, (uint16_t)vrd16(a3 - 2));
    vwr16(a3, (uint16_t)vrd16(a3 - 4));
    vwr16(a3 - 4, (uint16_t)vrd16(a3 - 6));
    vwr16(a3 - 6, (uint16_t)vrd16(a3 - 8));
    vwr16(a3 - 8, 0xA2);
    vwr16(a3 - 2, 0xA6);
    uint32_t a2 = a_reg(2) + 2;
    vwr16(a2, 0x32); a2 += 2;
    set_d16(0, (uint16_t)(w16(0x5906) << 3));
    uint32_t a0 = A6W(0x594E) + sx16(dw(0));
    set_d(0, 0);
    set_d(6, 2);
    charge(16);
    do {
        uint16_t c = (uint16_t)(vrd16(a0) & 0xFF);
        a0 += 2;
        uint8_t b = (uint8_t)c;
        charge(4);
        if (b == 0x2E) { c = 0x9E; charge(1); }
        else {
            charge(1);
            if ((uint8_t)(b - 0x2E) & 0x80) { c = 0x32; charge(2); }
            else {
                charge(2);
                if ((int8_t)b >= 0x41) c = (uint16_t)(((uint16_t)(c - 0x41) << 2) + 0x36);
                else                   c = (uint16_t)((uint16_t)(c - 0x30) << 2);
                charge(4);
            }
        }
        c &= 0xFF;
        set_d16(0, c);
        vwr16(a2, c); a2 += 2;
        set_a(0, a0); set_a(2, a2);
        charge(2);
        if (!dbf(6)) break;
        poll();
    } while (1);
    vwr16(a2, 0x32); a2 += 2;
    vwr16(a2, 0x32); a2 += 2;
    set_a(2, a2);
    charge(2);
    RTS();
}

/* FUN_00028116: reset the ranking-screen state (0x5904 = 0x5906 = 0, 0x5938 =
 * 0xDE2 = 3), FUN_00005876, then continue in 0xC69A */
static uint32_t rd_rank_reset(void)
{
    uint32_t r;
    w16_set(0x5904, 0); w16_set(0x5906, 0);
    w16_set(0x5938, 3); w16_set(0xDE2, 3);
    charge(4);
    charge(1);
    if ((r = rd_call(L_5876, 0x28130u))) return r;
    charge(1);
    return RD_JMP(0xC69Au);
}

/* run the handler list at A4 (ROM 0x28298, or 0x282AC when 0x7028 is set):
 * each entry is called with 0xC40 = 2, followed by FUN_0000b56e */
static uint32_t rank_handler(uint32_t *a4p, uint32_t jsr_at)
{
    uint32_t r;
    w16_set(0xC40, 2);
    uint32_t a4 = a_reg(4);
    set_a(3, vrd32(a4));
    set_a(4, a4 + 4);
    vwr32(A6W(0x5894), a4 + 4);
    charge(4);
    if ((r = rd_call_ind(a_reg(3), jsr_at + 2, jsr_at))) return r;
    charge(1);
    if ((r = rd_call(L_B56E, jsr_at + 8))) return r;
    set_a(4, vrd32(A6W(0x5894)));
    (void)a4p;
    return 0;
}

/* FUN_00028136: the ranking-screen step, dispatched on 0x5904 & 15 through
 * the ROM word table 0x28148 (a computed jmp, which polls):
 *   even < 8: draw the title rows (FUN_00028002 on the list 0x28334, bases
 *     0x6000/0x1B2), clear a text row at 0x9009E486 + (0x5906 & 3) * 0x200,
 *     set the line state 0x5936/0x5938/0x593A/0x593C and advance 0x5904
 *   odd < 8: run 1..4 handlers (by 0x5904), build the rank line (FUN_00027f44,
 *     colour ROM 0x282C0[0x5906 & 3]) and draw it (FUN_00028044) while the line
 *     slides in; after 60 frames at rest advance 0x5906 and 0x5904
 *   8..15: run the five handlers */
static uint32_t rd_rank_step(void)
{
    uint32_t r;
    set_d16(0, (uint16_t)(w16(0x5904) & 0xF));
    set_d16(0, (uint16_t)vrd16(0x28148u + sx16(dw(0)) * 2u));
    charge(5);
    poll();
    uint32_t t = 0x28148u + sx16(dw(0));
    if (t == 0x28168u) {
        set_a(1, 0x28334u);
        w16_set(0x5930, 0x6000);
        w16_set(0x5932, 0x1B2);
        charge(3);
        CALL(L_28002, 0x2817C);
        set_d16(0, 0xF0BF);
        set_d16(1, (uint16_t)(w16(0x5906) & 3));
        set_d(1, (uint32_t)dw(1) * 0x200u);
        uint32_t a0 = 0x9009E486u + sx16(dw(1));
        set_d(7, 0x21);
        charge(7);
        do {
            vwr16(a0, 0xF0BF); a0 += 2;
            set_a(0, a0);
            charge(1);
            if (!dbf(7)) break;
            poll();
        } while (1);
        w16_set(0x5936, 0x26);
        w16_set(0x5938, (uint16_t)(w16(0x5938) + 4));
        w16_set(0x593A, 1);
        w16_set(0x593C, 0);
        w16_set(0x5904, (uint16_t)(w16(0x5904) + 1));
        charge(5);
        RTS();
    }
    if (t == 0x281B6u) {
        w16_set(0x593C, (uint16_t)(w16(0x593C) + 1));
        set_a(4, 0x28298u);
        charge(4);
        if (w16(0x7028) != 0) { set_a(4, 0x282ACu); charge(1); }
        w16_set(0x5892, 0);
        charge(1);
        for (;;) {
            if ((r = rank_handler(0, 0x281DAu))) return r;
            set_d16(0, w16(0x5892));
            set_d16(1, (uint16_t)(((w16(0x5904) >> 1) & 3) + 1));
            w16_set(0x5892, (uint16_t)(w16(0x5892) + 1));
            charge(9);
            if (!((int16_t)dw(0) < (int16_t)dw(1))) break;
            poll();
        }
        set_d16(0, (uint16_t)(w16(0x5906) & 3));
        set_d16(0, (uint16_t)vrd16(0x282C0u + sx16(dw(0)) * 2u));
        w16_set(0x5930, dw(0));
        w16_set(0x5932, 0x1B2);
        charge(5);
        CALL(L_27F44, 0x2821A);
        set_a(1, A6W(0x5908));
        vwr16(A6W(0x5908), w16(0x5936));
        vwr16(A6W(0x590A), w16(0x5938));
        vwr16(A6W(0x590C), w16(0x593A));
        charge(4);
        CALL(L_28044, 0x28232);
        charge(2);
        if ((int16_t)w16(0x5936) > 0xB) {
            w16_set(0x5936, (uint16_t)(w16(0x5936) - 1));
            charge(3);
            if ((int16_t)w16(0x593A) < 0x10) { w16_set(0x593A, (uint16_t)(w16(0x593A) + 1)); charge(1); }
            RTS();
        }
        charge(2);
        if ((int16_t)w16(0x593C) >= 0x3C) {
            w16_set(0x5906, (uint16_t)(w16(0x5906) + 1));
            w16_set(0x5904, (uint16_t)(w16(0x5904) + 1));
            charge(2);
        }
        RTS();
    }
    /* 0x2825E */
    set_a(4, 0x28298u);
    charge(3);
    if (w16(0x7028) != 0) { set_a(4, 0x282ACu); charge(1); }
    w16_set(0x5892, 0);
    charge(1);
    for (;;) {
        if ((r = rank_handler(0, 0x2827Eu))) return r;
        w16_set(0x5892, (uint16_t)(w16(0x5892) + 1));
        charge(4);
        if (!((int16_t)w16(0x5892) < 5)) break;
        poll();
    }
    RTS();
}

/* FUN_00028706: sound CPU word 0x60005010 = 0x40CC */
static uint32_t rd_snd_5010_40cc(void) { vwr16(0x60005010u, 0x40CC); charge(2); return RD_RTS; }

static const rd_entry rd_table_b5[] = {
    /* ep       fn                   kill    scratch cost name */
    { 0x005770, rd_clear_90028000,   0x0103, 0,      0, "FUN_00005770" },
    { 0x005A0A, rd_text_step,        0x0FDF, 0,      0, "FUN_00005a0a", 1 },
    { 0x005BB6, rd_count_from_a1,    0x0381, 0,      0, "FUN_00005bb6" },
    { 0x005BBC, rd_print_hex,        0x0183, 0,      0, "FUN_00005bbc" },
    { 0x00C052, rd_test_mode,        0x3FFF, 0,      0, "FUN_0000c052", 1 },
    { 0x00E640, rd_dbc8_with_13,     0x0001, 0,      0, "FUN_0000e640" },
    { 0x00E646, rd_e262_5_38,        0x00C0, 0,      0, "FUN_0000e646" },
    { 0x00E6B2, rd_e6b2,             0x03CF, 0,      0, "FUN_0000e6b2" },
    { 0x00E758, rd_e758,             0x03CF, 0,      0, "FUN_0000e758" },
    { 0x00E774, rd_e774,             0x07FF, 0,      0, "FUN_0000e774" },
    { 0x010286, rd_string_6d,        0x0303, 0,      0, "FUN_00010286" },
    { 0x0102B2, rd_string_6b,        0x0303, 0,      0, "FUN_000102b2" },
    { 0x0102DE, rd_string_69,        0x0303, 0,      0, "FUN_000102de" },
    { 0x015838, rd_15838,            0x0FFF, 0,      0, "FUN_00015838" },
    { 0x015852, rd_15852,            0x0FFF, 0,      0, "FUN_00015852" },
    { 0x015A36, rd_facing_object,    0x1BFF, 0,      0, "FUN_00015a36" },
    { 0x015B3C, rd_objs_15b3c,       0x0FFF, 0,      0, "FUN_00015b3c" },
    { 0x015C0C, rd_objs_15c0c,       0x0FFF, 0,      0, "FUN_00015c0c" },
    { 0x015D6C, rd_objs_15d6c,       0x1FFF, 0,      0, "FUN_00015d6c" },
    { 0x015F20, rd_objs_15f20,       0x0FFF, 0,      0, "FUN_00015f20" },
    { 0x0160C8, rd_objs_160c8,       0x0FFF, 0,      0, "FUN_000160c8" },
    { 0x016210, rd_objs_16210,       0x0FFF, 0,      0, "FUN_00016210" },
    { 0x01A6A2, rd_drone_push,       0x1BFF, 0,      0, "FUN_0001a6a2" },
    { 0x01E9B0, rd_snd_level,        0x0003, 0,      0, "FUN_0001e9b0" },
    { 0x01EA30, rd_snd_5018_40c0,    0x0001, 0,      0, "FUN_0001ea30" },
    { 0x021446, rd_obj_686,          0x0000, 0,      0, "FUN_00021446" },
    { 0x0215B8, rd_timer_model_687,  0x0003, 0,      0, "FUN_000215b8" },
    { 0x0215DE, rd_timer_model_697,  0x0103, 0,      0, "FUN_000215de" },
    { 0x0217D8, rd_dl_8017,          0x0801, 0,      0, "FUN_000217d8" },
    { 0x02184A, rd_obj_list_21880,   0x0BFF, 0,      0, "FUN_0002184a" },
    { 0x0218DE, rd_phase_group,      0x0BFF, 0,      0, "FUN_000218de" },
    { 0x021902, rd_phase_step_12,    0x0181, 0,      0, "FUN_00021902" },
    { 0x021922, rd_phase_positions,  0x008F, 0,      0, "FUN_00021922" },
    { 0x021976, rd_phase_objects,    0x08FF, 0,      0, "FUN_00021976" },
    { 0x022088, rd_22088,            0x0003, 0,      0, "FUN_00022088" },
    { 0x0228B2, rd_race_variant_1,   0x0303, 0,      0, "FUN_000228b2" },
    { 0x0228E8, rd_race_variant_2,   0x0303, 0,      0, "FUN_000228e8" },
    { 0x02291E, rd_race_variant_3,   0x0303, 0,      0, "FUN_0002291e" },
    { 0x027F44, rd_rank_line,        0x0FFF, 0,      0, "FUN_00027f44" },
    { 0x028002, rd_tile_row,         0x07FF, 0,      0, "FUN_00028002" },
    { 0x028044, rd_tile_row_counted, 0x07FF, 0,      0, "FUN_00028044" },
    { 0x028116, rd_rank_reset,       0x0F03, 0,      0, "FUN_00028116" },
    { 0x028136, rd_rank_step,        0x1FFF, 0,      0, "FUN_00028136", 1 },
    { 0x028706, rd_snd_5010_40cc,    0x0000, 0,      0, "FUN_00028706" },
    { 0x0298DE, rd_snd_501e_4093,    0x0000, 0,      0, "FUN_000298de" },
    { 0x0298E8, rd_dir_step,         0x0007, 0,      0, "FUN_000298e8" },
    { 0x0299CA, rd_repeat_timers,    0x0081, 0,      0, "FUN_000299ca" },
    { 0x029A54, rd_menu_colours,     0x000F, 0,      0, "FUN_00029a54" },
    { 0x029A8A, rd_menu_cursor,      0x008F, 0,      0, "FUN_00029a8a" },
    { 0x029B1C, rd_menu_select,      0x0FDF, 0,      0, "FUN_00029b1c" },
    { 0x029BA4, rd_step_wrap_db0,    0x0001, 0,      0, "FUN_00029ba4" },
    { 0x029BCC, rd_step_wrap_db2,    0x0001, 0,      0, "FUN_00029bcc" },
    { 0x029BF4, rd_menu_redraw,      0x0FDF, 0,      0, "FUN_00029bf4" },
    { 0x029C42, rd_item_colour,      0x0003, 0,      0, "FUN_00029c42" },
};
RD_REGISTER(rd_table_b5)
