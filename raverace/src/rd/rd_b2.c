/*
 * rd_b2.c -- Phase B batch 2 (functions 0x11CF2..0x1AE7E): readable-C
 * replacements, each proven against its lifted twin by RR_RD=check
 * (include/rd.h). Written from the 68K instructions (gen/rr_lifted.c);
 * Ghidra's C is a naming aid only.
 *
 * A6 is always WRAM + 0x8000 (0x10008000): (d16,A6) is a fixed address.
 */
#include "rd.h"
#include <string.h>
#include "rr_lifted.h"

#define A6W(off) (0x10008000u + (uint32_t)(off))    /* (d16,A6), d16 < 0x8000 */

/* ---- WRAM globals ---- */
#define G_DL_CURSOR   A6W(0xC46)   /* long: display-list write pointer            */
#define G_DL_PRIO     A6W(0xC40)   /* word: current display-list priority/mode word */

/* read / write a WRAM word or long at (d16,A6) */
static inline uint16_t w16(uint32_t off)             { return (uint16_t)vrd16(A6W(off)); }
static inline uint32_t w32(uint32_t off)             { return vrd32(A6W(off)); }
static inline void     w16_set(uint32_t off, uint16_t v) { vwr16(A6W(off), v); }
static inline void     w32_set(uint32_t off, uint32_t v) { vwr32(A6W(off), v); }
static inline uint32_t sx16(uint32_t v)              { return (uint32_t)(int32_t)(int16_t)v; }
/* bset / bclr on a WRAM byte at (d16,A6) (the bit number mod 8, as the 68K does) */
static inline void bset8(uint32_t off, unsigned bit) { vwr8(A6W(off), (uint8_t)(vrd8(A6W(off)) | (1u << (bit & 7)))); }
static inline void bclr8(uint32_t off, unsigned bit) { vwr8(A6W(off), (uint8_t)(vrd8(A6W(off)) & ~(1u << (bit & 7)))); }
/* append one long to a display list and advance the pointer */
static inline void put32(uint32_t *p, uint32_t v)    { vwr32(*p, v); *p += 4; }

/* movem.l Dn,-(SP) / movem.l (SP)+,Dn for one register */
static inline void push32(uint32_t v) { uint32_t sp = a_reg(7) - 4; set_a(7, sp); vwr32(sp, v); }
static inline uint32_t pop32(void)    { uint32_t sp = a_reg(7), v = vrd32(sp); set_a(7, sp + 4); return v; }

/* a run of bsr/jsr with nothing in between: each charged 1 */
typedef struct { void (*fn)(void); uint32_t ret; } callee_t;
static uint32_t call_seq(const callee_t *c, int n)
{
    uint32_t r;
    for (int i = 0; i < n; i++) { charge(1); if ((r = rd_call(c[i].fn, c[i].ret))) return r; }
    return 0;
}
#define CALLS(tbl) do { if ((r = call_seq(tbl, (int)(sizeof tbl / sizeof tbl[0])))) return r; } while (0)

/* a bsr/jsr: charge it, call, and leave if the callee did not come back here.
 * Needs `uint32_t r;` in the caller. */
/* rts, charged here (entries with several kinds of exit use cost 0) */
#define RTS() do { charge(1); return RD_RTS; } while (0)
#define CALL(fn, ret) do { charge(1); if ((r = rd_call(fn, ret))) return r; } while (0)

/* ============================ 0x11CF2 .. 0x12264 ============================ */

#define G_SEL_2342    0x2342   /* word: a selection index; its low two bits pick one of four entries (purpose not yet identified) */
#define G_TBL_2356    0x2356   /* 4 longs, decayed toward 0 except the selected one (FUN_00011cf2) */
#define G_TBL_2366    0x2366   /* 4 longs, likewise */

/* FUN_00011cf2: for each of the four entries n = 3..0, subtract a fraction of
 * the two longs 0x2366[n] (>>6) and 0x2356[n] (>>3) from themselves -- except
 * for the selected entry (G_SEL_2342 & 3), which instead ADDS that fraction.
 * D1 = the last fraction, D6w = the selection, D7w = 0xFFFF (dbf). */
static uint32_t rd_decay_2356_2366(void)
{
    uint16_t sel = (uint16_t)(w16(G_SEL_2342) & 3);
    uint32_t d1 = d_reg(1);
    charge(3);                                          /* moveq, move.w, andi.w */
    for (int n = 3; n >= 0; n--) {
        uint32_t a = A6W(G_TBL_2366) + (uint32_t)n * 4u;
        d1 = (uint32_t)((int32_t)vrd32(a) >> 6);
        charge(4);                                      /* move.l, asr, cmp, beq */
        if (n != sel) { d1 = 0u - d1; charge(1); }
        vwr32(a, vrd32(a) - d1);
        a = A6W(G_TBL_2356) + (uint32_t)n * 4u;
        d1 = (uint32_t)((int32_t)vrd32(a) >> 3);
        charge(5);                                      /* sub.l, move.l, asr, cmp, beq */
        if (n != sel) { d1 = 0u - d1; charge(1); }
        vwr32(a, vrd32(a) - d1);
        charge(2);                                      /* sub.l, dbf */
        set_d(1, d1); set_d16(6, sel);
        set_d(7, n ? (uint32_t)(n - 1) : 0x0000FFFFu);
        if (n) poll();
    }
    return RD_RTS;
}

/* FUN_00011d2a: append a 0x8002 block of four rotated objects to the display
 * list: per entry n = 3..0 the model from ROM 0x11DDA[n], x = ROM 0x11DE2[n] +
 * a smoothed word 0x237E[n], y = ROM 0x11DF2[n] + 8 + a smoothed word 0x2386[n]
 * (each word moves 1/16 of the way toward its target in the ROM pairs at
 * 0x11E02 / 0x11E04, chosen by 0x238E & 3), z = 0x1560, then an identity
 * rotation (0, 0x8001 x3 ...). Terminates the list with -1. */
static uint32_t rd_dl_four_smoothed(void)
{
    uint32_t p = vrd32(G_DL_CURSOR);
    put32(&p, 0x8002);
    put32(&p, (uint32_t)w16(0xC40));
    uint16_t pick = (uint16_t)(w16(0x238E) & 3);
    uint32_t d0 = 0, d1 = d_reg(1);
    uint16_t d2 = (uint16_t)d_reg(2);
    charge(8);
    for (int n = 3; n >= 0; n--) {
        put32(&p, (uint32_t)(uint16_t)vrd16(0x11DDAu + (uint32_t)n * 2u));      /* model */
        d0 = vrd32(0x11DE2u + (uint32_t)n * 4u);
        uint32_t a = A6W(0x237E) + (uint32_t)n * 2u;
        uint16_t w = (uint16_t)vrd16(a);
        d2 = (uint16_t)((int16_t)(uint16_t)(vrd16(0x11E02u + pick * 4u) - w) >> 4);
        w = (uint16_t)(w + d2);
        vwr16(a, w);
        d1 = sx16(w);
        d0 += d1;
        put32(&p, d0);                                                     /* x */
        d0 = vrd32(0x11DF2u + (uint32_t)n * 4u);
        a = A6W(0x2386) + (uint32_t)n * 2u;
        w = (uint16_t)vrd16(a);
        d2 = (uint16_t)((int16_t)(uint16_t)(vrd16(0x11E04u + pick * 4u) - w) >> 4);
        w = (uint16_t)(w + d2);
        vwr16(a, w);
        d1 = sx16((uint16_t)(w + 8));
        d0 += d1;
        put32(&p, d0);                                                     /* y */
        put32(&p, 0x1560);                                                 /* z */
        put32(&p, 0);
        for (int k = 0; k < 3; k++) { put32(&p, 0x8001); put32(&p, 0); }
        charge(34);
        set_a(3, p);
        set_d(0, d0); set_d(1, d1); set_d16(2, d2);
        set_d16(6, pick); set_d(7, n ? (uint32_t)(n - 1) : 0x0000FFFFu);
        if (n) poll();
    }
    vwr32(p, 0xFFFFFFFFu);
    vwr32(G_DL_CURSOR, p);
    return RD_RTS;
}

/* FUN_00011e12: append a 0x8002 block to the display list: object 0x6B7 at
 * (0, 0x75C, 0x15A0) rotated by the sine table at A5 (angles 0x280, 0, 0);
 * when 0x2344 is set and the selection 0x2342 is below 4, object 0x718 at
 * (-0x9A8, -0x788, 0x15C0) unrotated; then the object ROM 0x1182A[(0x2034>>2)&1]
 * at (0x2356[s], 0x200, 0x2366[s]) (s = 0x2342 & 3), unrotated. Ends the list. */
static uint32_t rd_dl_select_scene(void)
{
    uint32_t p = vrd32(G_DL_CURSOR), a5 = a_reg(5);
    uint32_t d0 = 0, d1 = 0, d2 = 0;
    put32(&p, 0x8002);
    d0 = w16(0xC40);
    put32(&p, d0);
    put32(&p, 0x6B7); put32(&p, 0); put32(&p, 0x75C); put32(&p, 0x15A0);
    static const uint16_t ang[3] = { 0x280, 0, 0 };
    for (int k = 0; k < 3; k++) {                          /* (-sin, cos) for each angle */
        uint16_t a = (uint16_t)(ang[k] & 0xFFFE);
        d1 = (d1 & 0xFFFF0000u) | (uint16_t)vrd16(a5 + sx16(a));
        a = (uint16_t)(a + 0x4000);
        d0 = (d0 & 0xFFFF0000u) | a;
        d2 = (d2 & 0xFFFF0000u) | (uint16_t)vrd16(a5 + sx16(a));
        d1 = (d1 & 0xFFFF0000u) | (uint16_t)(0u - d1);
        put32(&p, d1); put32(&p, d2);
    }
    put32(&p, 0);
    charge(38);                                            /* up to the beq */
    if (w16(0x2344) != 0) {
        uint16_t d4 = (uint16_t)(w16(G_SEL_2342) - 4);
        set_d16(4, d4);
        charge(3);
        if ((int16_t)d4 < 0) {
            put32(&p, 0x718); put32(&p, 0xFFFFF658u); put32(&p, 0xFFFFF878u); put32(&p, 0x15C0);
            put32(&p, 0);
            for (int k = 0; k < 3; k++) { put32(&p, 0x8001); put32(&p, 0); }
            charge(11);
        }
    }
    d0 = (uint16_t)((w16(0x2034) >> 2) & 1);
    d1 = (uint16_t)vrd16(0x1182Au + d0 * 2u);
    put32(&p, d1);
    d0 = (uint16_t)(w16(G_SEL_2342) & 3);
    put32(&p, w32(G_TBL_2356 + d0 * 4u));
    put32(&p, 0x200);
    put32(&p, w32(G_TBL_2366 + d0 * 4u));
    put32(&p, 0);
    for (int k = 0; k < 3; k++) { put32(&p, 0x8001); put32(&p, 0); }
    vwr32(p, 0xFFFFFFFFu);
    vwr32(G_DL_CURSOR, p);
    set_a(3, p);
    set_d(0, d0); set_d(1, d1); set_d(2, d2);
    return RD_RTS;
}

#define G_STATE_232A  0x232A   /* word: this mode's step counter (FUN_00011f4a dispatches on its low two bits) */

/* FUN_00011f4a: per-frame step of a four-state sequence (state = 0x232A & 3,
 * jump table at 0x11F60); counts frames in the long 0x5206.
 *   0: set up -- sound, text layer, palette etc., the selection state, then
 *      advance to 1 and run FUN_000176d6 and FUN_0000f9ca
 *   1: FUN_0000e54c, FUN_000125ea, FUN_0000ea6a; clear 0x20B0; advance
 *   2: per-frame work; advance once 0x49C8 is clear
 *   3: the steady state (selection handling, FUN_000120c0 / FUN_0001218c) */
static uint32_t rd_state_232a(void)
{
    uint32_t r;
    w32_set(0x5206, w32(0x5206) + 1);
    uint16_t st = (uint16_t)(w16(G_STATE_232A) & 3);
    uint32_t off = vrd32(0x11F60u + (uint32_t)st * 4u);
    set_d(0, off);
    uint32_t t = 0x11F60u + off;
    charge(6);                                          /* addq, move, andi, move.l, nop, jmp */
    if (t != 0x11F70u && t != 0x11FF4u && t != 0x1203Cu && t != 0x12084u) return RD_JMP(t);
    poll();                                             /* the computed jmp */
    switch (t) {
    case 0x11F70u:
        CALL(L_286F2, 0x11F76); CALL(L_28646, 0x11F7C); CALL(L_1E6CE, 0x11F82); CALL(L_575A, 0x11F88);
        charge(1); w16_set(0xDE2, 0);
        CALL(L_C69A, 0x11F94); CALL(L_B8B0, 0x11F9A); CALL(L_EFEE, 0x11FA0);
        charge(5);
        w16_set(0x221A, 0); w16_set(0x20B0, 0); w16_set(0x20B2, 0); w16_set(0x49CE, 0); w16_set(0x4866, 0);
        CALL(L_1429E, 0x11FBA); CALL(L_272D4, 0x11FC0); CALL(L_1253A, 0x11FC4); CALL(L_1563A, 0x11FC8);
        CALL(L_287A6, 0x11FCE); CALL(L_288AC, 0x11FD4);
        charge(4);
        w16_set(0x2394, 0); w16_set(0x434C, 0);
        w16_set(G_STATE_232A, (uint16_t)(w16(G_STATE_232A) + 1));
        w16_set(0x49CE, 0);
        CALL(L_176D6, 0x11FEE); CALL(L_F9CA, 0x11FF2);
        RTS();
    case 0x11FF4u:
        charge(w16(0x1100) == 0 ? 2 : 3);               /* tst, beq (or tst, beq, bra) -- both reach 0x12020 */
        CALL(L_E54C, 0x12026); CALL(L_125EA, 0x1202A); CALL(L_EA6A, 0x12030);
        charge(2);
        w16_set(0x20B0, 0);
        w16_set(G_STATE_232A, (uint16_t)(w16(G_STATE_232A) + 1));
        RTS();
    case 0x1203Cu:
        CALL(L_272D4, 0x12042); CALL(L_15674, 0x12048);
        charge(2);                                      /* tst.w, bmi */
        if (!(w16(0x237A) & 0x8000)) { CALL(L_14626, 0x12052); CALL(L_FED6, 0x12056); }
        CALL(L_176D6, 0x1205C); CALL(L_289CA, 0x12062); CALL(L_288AC, 0x12068); CALL(L_28C32, 0x1206E);
        charge(2);                                      /* tst.w, bne */
        if (w16(0x49C8) != 0) RTS();
        CALL(L_FF1E, 0x12078); CALL(L_E61E, 0x1207E);
        charge(1);
        w16_set(G_STATE_232A, (uint16_t)(w16(G_STATE_232A) + 1));
        RTS();
    default:                                            /* 0x12084 */
        CALL(L_1229A, 0x12088); CALL(L_12774, 0x1208C); CALL(L_272D4, 0x12092); CALL(L_15674, 0x12098);
        CALL(L_120C0, 0x1209C); CALL(L_1218C, 0x120A0); CALL(L_176D6, 0x120A6); CALL(L_EA92, 0x120AC);
        CALL(L_289CA, 0x120B2); CALL(L_288AC, 0x120B8); CALL(L_28C32, 0x120BE);
        RTS();
    }
}

/* count the enabled slots matching the current one: over the eight 16-byte
 * slot records at 0x11AE, AND together the flag byte 0x11B6[slot] of every
 * slot whose word 0x11AE[slot] equals 0x11A2 and whose bit in 0x487E is clear,
 * starting from `bit` alone. Leaves D0/D1/D2/D5/D6 as the 68K loop does. */
static uint8_t and_matching_slot_flags(unsigned bit)
{
    uint16_t d0 = 0, d5 = 0, d1 = w16(0x11A2);
    uint8_t d2 = (uint8_t)(1u << bit);
    charge(6);                                          /* moveq x3, move.w, moveq, bset */
    for (int k = 7; k >= 0; k--) {
        charge(2);                                      /* cmp.w, bne */
        if (vrd16(A6W(0x11AE) + sx16(d0)) == d1) {
            charge(2);                                  /* btst, bne */
            if (!(vrd8(A6W(0x487E)) & (1u << (d5 & 7)))) { charge(1); d2 &= (uint8_t)vrd8(A6W(0x11B6) + sx16(d0)); }
        }
        d0 = (uint16_t)(d0 + 0x10); d5++;
        charge(3);                                      /* addi, addq, dbf */
        if (k) poll();
    }
    set_d(0, d0); set_d16(1, d1); set_d(2, d2); set_d(5, d5); set_d(6, 0x0000FFFFu);
    return d2;
}

/* FUN_000120c0: bits 4 and 5 of 0x11AA flag that every matching slot is ready
 * (see and_matching_slot_flags); when both hold (or 0x2344 is set), count the
 * word 0x4022 down by 0x60 and when it runs out switch to mode 4 (0xDA0),
 * reset the step counters, set 0x49CC = -1 and call FUN_0002863e /
 * FUN_00014634. Nothing happens while 0x232C is set. */
static uint32_t rd_wait_all_ready(void)
{
    uint32_t r;
    bclr8(0x11AA, 4);
    charge(3);                                          /* bclr, tst, bne */
    if (w16(0x232C) != 0) return RD_RTS;
    charge(2);                                          /* tst, bne */
    if (w16(0x2344) == 0) {
        charge(1); bset8(0x11AA, 4);
        uint8_t f = and_matching_slot_flags(4);
        charge(2);                                      /* tst.b, beq */
        if (!f) return RD_RTS;
    }
    uint16_t v = (uint16_t)(w16(0x4022) - 0x60);
    w16_set(0x4022, v);
    charge(2);                                          /* subi, bpl */
    if (!(v & 0x8000)) return RD_RTS;
    w16_set(0x4022, 0);
    charge(3);                                          /* clr, tst, bne */
    if (w16(0x2344) == 0) {
        charge(1); bset8(0x11AA, 5);
        uint8_t f = and_matching_slot_flags(5);
        charge(2);
        if (!f) return RD_RTS;
    }
    w16_set(0x49CC, 0xFFFF); w16_set(0x4022, 0); w16_set(0xDA0, 4);
    w16_set(G_STATE_232A, 0); w16_set(0x2336, 0); w16_set(0x57CA, 5);
    charge(6);
    CALL(L_2863E, 0x12180);
    charge(1); w16_set(0x2394, 0);
    CALL(L_14634, 0x1218A);
    return RD_RTS;
}

/* FUN_0001218c: once the long 0x204A reaches 0x4846, switch to mode 4 (0xDA0),
 * reset the step counters and call FUN_000286f2 / FUN_00014634.
 * D0 = the long 0x204A. */
static uint32_t rd_check_time_up(void)
{
    uint32_t r;
    uint32_t v = w32(0x204A);
    set_d(0, v);
    charge(3);
    if ((int32_t)v < (int32_t)w32(0x4846)) return RD_RTS;
    w16_set(0xDA0, 4); w16_set(G_STATE_232A, 0); w16_set(0x2336, 0); w16_set(0x57CA, 5);
    charge(4);
    CALL(L_286F2, 0x121B0);
    charge(1); w16_set(0x2394, 0);
    CALL(L_14634, 0x121BA);
    return RD_RTS;
}

/* FUN_000121bc: clear the bytes 0x2216/0x2218/0x2219, then with 0xC40 = 1
 * run FUN_0000b4e4 (A0 = ROM 0x1394E), FUN_0000b56e and FUN_0000b6d0, set
 * bit (0xC40) of 0x2216 and 0x2324 = 1 */
static uint32_t rd_init_2216(void)
{
    uint32_t r;
    vwr8(A6W(0x2216), 0); vwr8(A6W(0x2218), 0); vwr8(A6W(0x2219), 0);
    w16_set(0xC40, 0);
    uint16_t d0 = w16(0xC40);
    set_d16(0, d0);
    bclr8(0x2216, d0);
    w16_set(0xC40, 1);
    set_a(0, 0x1394Eu);
    charge(8);
    CALL(L_B4E4, 0x121E4); CALL(L_B56E, 0x121E8); CALL(L_B6D0, 0x121EC);
    d0 = w16(0xC40);
    set_d16(0, d0);
    bset8(0x2216, d0);
    w16_set(0x2324, 1);
    charge(3);
    return RD_RTS;
}

/* FUN_000121fc: copy entry 0 of the word pairs 0x20E8/0x20D8 into entry 1 when
 * 0x20E8 changed; when bit 0 of 0x20B0 differs from 0x2324, call the handler
 * from the table at 0x12266 and record the new bit. With the bit clear, build
 * the display list (FUN_00014744, b56e, 14eb6, 1226e) and continue at 0x1F1DE;
 * with it set, copy 0x20D8 entry D6 to entry D7 and return. */
static uint32_t rd_pair_update(void)
{
    uint32_t r;
    set_d16(6, 0); set_d16(7, 1);
    uint16_t d0 = w16(0x20E8);
    set_d16(0, d0);
    charge(5);                                          /* move x3, cmp, beq */
    if (d0 != w16(0x20EA)) {
        w16_set(0x20EA, d0);
        w16_set(0x20DA, w16(0x20D8));
        charge(2);
    }
    uint16_t d5 = (uint16_t)(w16(0x20B0) & 1);
    set_d16(5, d5);
    charge(4);                                          /* move, andi, cmp, beq */
    if (d5 != w16(0x2324)) {
        uint32_t off = vrd32(0x12266u + (uint32_t)d5 * 4u);
        set_d(0, off);
        charge(3);                                      /* move.l, nop, jsr */
        if ((r = rd_call_ind(0x12266u + off, 0x1223A, 0x12236))) return r;
    }
    d5 = (uint16_t)d_reg(5);
    w16_set(0x2324, d5);
    charge(2);                                          /* move.w, bne */
    if (d5 == 0) {
        charge(1); w16_set(0xC40, (uint16_t)d_reg(7));
        CALL(L_14744, 0x12248); CALL(L_B56E, 0x1224C); CALL(L_14EB6, 0x12250); CALL(L_1226E, 0x12254);
        return 0x1F1DEu;                                /* jmp: charged as the entry's cost */
    }
    vwr16(A6W(0x20D8) + sx16(d_reg(7)) * 2u, vrd16(A6W(0x20D8) + sx16(d_reg(6)) * 2u));
    charge(1);
    return RD_RTS;
}

/* low-word / low-byte updates of a 32-bit register image (move.w / move.b into Dn) */
#define LOW16(x, v) ((x) = ((x) & 0xFFFF0000u) | (uint16_t)(v))
#define LOW8(x, v)  ((x) = ((x) & 0xFFFFFF00u) | (uint8_t)(v))

/* a working copy of D0-D7 / A0-A6 for register-heavy code: load at entry and
 * after every call, store before every call and at exit */
typedef struct { uint32_t d[8], a[7]; } regs_t;
static void regs_load(regs_t *r)        { for (int i = 0; i < 8; i++) r->d[i] = d_reg(i); for (int i = 0; i < 7; i++) r->a[i] = a_reg(i); }
static void regs_store(const regs_t *r) { for (int i = 0; i < 8; i++) set_d(i, r->d[i]); for (int i = 0; i < 7; i++) set_a(i, r->a[i]); }
static inline uint32_t swap32(uint32_t x)             { return (x << 16) | (x >> 16); }
static inline uint32_t muls_w(uint32_t a, uint32_t b) { return (uint32_t)((int32_t)(int16_t)a * (int32_t)(int16_t)b); }
/* muls.w, add.l Dn,Dn, swap: a Q15 product in the low word (and the rest) */
static inline uint32_t fmul15(uint32_t a, uint32_t b) { return swap32(muls_w(a, b) * 2u); }
/* divs.w src,Dn exactly as the 68K (and the lifted build): on overflow V is
 * set and Dn is unchanged; a zero divisor traps like the lifted code */
static inline void divs_w(uint32_t *dn, uint32_t src)
{
    int64_t q = SDIVREM((int32_t)*dn, (int16_t)src, '/'), r = SDIVREM((int32_t)*dn, (int16_t)src, '%');
    if ((uint32_t)((int32_t)q + 0x8000) > 0xFFFFu) return;
    *dn = ((uint32_t)r << 16) | ((uint32_t)q & 0xFFFFu);
}

/* ============================ 0x1229A .. 0x12774 ============================ */
/* A progression through a table of stages: 0x2078 picks a table set, 0x207E a
 * row, 0x207A the position in the row, 0x207C a level; 0x232C accumulates the
 * stage values, 0x204A is compared against the row's thresholds (purpose not
 * yet identified -- a time/score table). */

/* FUN_0001229a: advance the stage when a matching slot reports a later stage
 * (flag bit 7 of 0x11B6[slot] with a higher packed position than ours) or when
 * 0x204A falls below the current row threshold; then run the countdown in
 * 0x2084 that clears bit 7 of 0x11AA, and continue in FUN_000126d0. */
static uint32_t rd_stage_advance(void)
{
    uint32_t r;
    uint32_t d0 = d_reg(0), d1 = d_reg(1), d2 = d_reg(2), d3 = d_reg(3), d5 = d_reg(5), d6 = d_reg(6), d7 = d_reg(7);
    uint32_t a0 = a_reg(0), a1 = a_reg(1);
    int advance = 0;                                    /* reached 0x1232A */
    charge(2);                                          /* tst, bne */
    if (w16(0x2344) == 0) {
        charge(2);                                      /* btst, bne */
        if (!(vrd8(A6W(0x11AA)) & 0x80)) {
            d6 = 7; d0 = 0;
            LOW16(d1, w16(0x11A2)); LOW16(d2, w16(0x207A)); LOW16(d3, w16(0x207C));
            LOW16(d2, d2 & 0xF); LOW16(d3, d3 & 0xF); LOW16(d3, d3 << 4); LOW16(d2, d2 | d3);
            charge(9);
            for (;;) {
                charge(2);                              /* cmp.w, bne */
                if ((uint16_t)vrd16(A6W(0x11AE) + sx16(d0)) == (uint16_t)d1) {
                    charge(2);                          /* btst, beq */
                    if (vrd8(A6W(0x11B6) + sx16(d0)) & 0x80) {
                        charge(2);                      /* cmp.b, bge */
                        if ((int8_t)d2 < (int8_t)vrd8(A6W(0x11B7) + sx16(d0))) {
                            w16_set(0x2086, 1);
                            charge(2);                  /* move.w, bra */
                            advance = 1;
                            break;
                        }
                    }
                }
                LOW16(d0, d0 + 0x10);
                charge(2);                              /* addi, dbf */
                uint16_t c = (uint16_t)(d6 - 1); LOW16(d6, c);
                if (c == 0xFFFF) break;
                poll();
            }
        }
    }
    if (!advance) {
        a0 = 0xECDC;
        LOW16(d0, w16(0x2046)); LOW16(d0, d0 << 6);
        LOW8(d0, vrd8(a0 + sx16(d0)) & 0xF);
        charge(6);                                      /* lea, move, lsl, move.b, andi.b, bne */
        if ((d0 & 0xFF) == 0) {
            d7 = ((uint32_t)w16(0x207C) << 16) | (d7 >> 16);
            LOW16(d0, w16(0x207E));
            a0 = 0x1246Au + sx16(d0) * 8u;
            LOW16(d1, w16(0x207A));
            LOW16(d7, vrd16(a0 + sx16(d1) * 2u));
            charge(8);                                  /* move, swap, move, lea, move, move, cmp.l, bge */
            if ((int32_t)d7 < (int32_t)w32(0x204A)) {
                w16_set(0x2086, 0);
                charge(1);
                advance = 1;
            }
        }
    }
    if (advance) {                                      /* 0x1232A: step to the next position */
        w16_set(0x48E4, 0xB4);
        LOW16(d6, w16(0x207A));
        LOW16(d0, w16(0x2078));
        a1 = vrd32(0x124EAu + sx16(d0) * 4u);
        LOW16(d0, w16(0x207E));
        a1 += sx16(d0) * 8u;
        LOW16(d0, vrd16(a1 + sx16(d6) * 2u));
        w16_set(0x232C, (uint16_t)(w16(0x232C) + d0));
        LOW16(d6, d6 + 1);
        LOW16(d0, w16(0x207E));
        a0 = 0x1246Au + sx16(d0) * 8u;
        charge(13);
        if ((uint16_t)vrd16(a0 + sx16(d6) * 2u) == 0xFFFF) {   /* end of the row: next level */
            LOW16(d6, 0);
            LOW16(d5, w16(0x207C)); LOW16(d5, d5 + 1);
            LOW16(d0, w16(0x207E));
            a1 = vrd32(0x1242Au + sx16(d0) * 4u);
            LOW8(d0, vrd8(a1)); LOW16(d0, d0 & 7);
            charge(9);
            if (!((int16_t)d5 < (int16_t)vrd16(0x1241Au + sx16(d0) * 2u))) { LOW16(d5, d5 + 1); charge(1); }
            w16_set(0x207C, (uint16_t)d5);
            charge(1);
        }
        w16_set(0x207A, (uint16_t)d6);
        charge(2);                                      /* move.w, jsr */
        set_d(0, d0); set_d(1, d1); set_d(2, d2); set_d(3, d3); set_d(5, d5); set_d(6, d6); set_d(7, d7);
        set_a(0, a0); set_a(1, a1);
        if ((r = rd_call(L_28724, 0x12392))) return r;
        d0 = d_reg(0); d1 = d_reg(1); d2 = d_reg(2); d3 = d_reg(3); d5 = d_reg(5); d6 = d_reg(6); d7 = d_reg(7);
        a0 = a_reg(0); a1 = a_reg(1);
        w16_set(0x2082, 0x3C); w16_set(0x2432, 0x168);
        charge(4);                                      /* move, move, tst, bne */
        if (w16(0x2344) != 0) goto out;
        bset8(0x11AA, 7);
        LOW16(d0, w16(0x207A)); LOW16(d1, w16(0x207C));
        LOW16(d0, d0 & 0xF); LOW16(d1, d1 & 0xF); LOW16(d1, d1 << 4); LOW16(d0, d0 | d1);
        vwr8(A6W(0x11AB), (uint8_t)d0);
        charge(8);
    }
    /* 0x123C2: the countdown in 0x2084 */
    charge(2);                                          /* tst, bne */
    if (w16(0x2344) != 0) goto out;
    LOW16(d0, w16(0x2084));
    charge(2);                                          /* move, beq */
    if ((uint16_t)d0 != 0) {
        LOW16(d0, d0 - 1);
        charge(2);                                      /* subq, bne */
        if ((uint16_t)d0 == 0) { bclr8(0x11AA, 7); w16_set(0x2084, 0); charge(2); }
        w16_set(0x2084, (uint16_t)d0);
        charge(2);                                      /* move, bra */
        goto out;
    }
    /* count the matching slots, and those with flag bit 7 */
    d6 = 7; d0 = 0; LOW16(d1, w16(0x11A2)); d2 = 0; d3 = 0;
    charge(5);
    for (int k = 7; k >= 0; k--) {
        charge(2);                                      /* cmp, bne */
        if ((uint16_t)vrd16(A6W(0x11AE) + sx16(d0)) == (uint16_t)d1) {
            LOW16(d2, d2 + 1);
            charge(3);                                  /* addq, btst, beq */
            if (vrd8(A6W(0x11B6) + sx16(d0)) & 0x80) { LOW16(d3, d3 + 1); charge(1); }
        }
        LOW16(d0, d0 + 0x10);
        charge(2);                                      /* addi, dbf */
        LOW16(d6, d6 - 1);
        if (k) poll();
    }
    charge(2);                                          /* cmp, bne */
    if ((uint16_t)d2 == (uint16_t)d3) { w16_set(0x2084, 8); charge(1); }
out:
    set_d(0, d0); set_d(1, d1); set_d(2, d2); set_d(3, d3); set_d(5, d5); set_d(6, d6); set_d(7, d7);
    set_a(0, a0); set_a(1, a1);
    charge(1);                                          /* bra.w */
    return 0x126D0u;
}

/* FUN_0001253a: start the stage progression: table set 0x2078 from the first
 * byte (minus 1, &15) of the course record ROM 0x1252A[(0x2344&1)*2 + ((0x2342>>2)&1)],
 * row 0x207E = (0x2344&1)*8 + (0x2342&7), 0x232C = that row's word +6 of the
 * set's table, position 1, level 0, and clear 0x2082/0x2084/0x2086/0x2430 */
static uint32_t rd_stage_start(void)
{
    uint32_t d0 = d_reg(0), d1 = d_reg(1);
    LOW16(d0, w16(0x2344)); LOW16(d0, d0 & 1); LOW16(d0, d0 << 1);
    LOW16(d1, w16(G_SEL_2342)); LOW16(d1, (uint16_t)d1 >> 2); LOW16(d1, d1 & 1);
    LOW16(d0, d0 + d1);
    uint32_t a0 = vrd32(0x1252Au + sx16(d0) * 4u);
    LOW8(d0, vrd8(a0)); LOW8(d0, d0 - 1); LOW16(d0, d0 & 0xF);
    w16_set(0x2078, (uint16_t)d0);
    a0 = vrd32(0x124EAu + sx16(d0) * 4u);
    LOW16(d0, w16(0x2344)); LOW16(d0, d0 & 1); LOW16(d0, d0 << 3);
    LOW16(d1, w16(G_SEL_2342)); LOW16(d1, d1 & 7);
    LOW16(d0, d0 + d1);
    w16_set(0x207E, (uint16_t)d0);
    w16_set(0x232C, (uint16_t)vrd16(a0 + 6u + sx16(d0) * 8u));
    w16_set(0x207C, 0); w16_set(0x207A, 1);
    w16_set(0x2082, 0); w16_set(0x2084, 0); w16_set(0x2086, 0); w16_set(0x2430, 0);
    set_d(0, d0); set_d(1, d1); set_a(0, a0);
    return RD_RTS;
}

/* FUN_000125a0: D0 = (row limit - 1) : 0x200 where the limit comes from the
 * level table (ROM 0x1241A[first byte of 0x1242A[row] & 7]); when 0x204A is
 * past it set 0x2430 = -1; when 0x2438 is 8 and the time's high word reaches
 * the limit, show the text at ROM 0x126C2 (FUN_00005a0a) and clear 0x2438 */
static uint32_t rd_stage_limit(void)
{
    uint32_t r;
    uint32_t d0 = d_reg(0);
    LOW16(d0, w16(0x207E));
    uint32_t a1 = vrd32(0x1242Au + sx16(d0) * 4u);
    LOW8(d0, vrd8(a1)); LOW16(d0, d0 & 7);
    LOW16(d0, vrd16(0x1241Au + sx16(d0) * 2u));
    LOW16(d0, d0 - 1);
    d0 = (d0 << 16) | (d0 >> 16);
    LOW16(d0, 0x200);
    set_d(0, d0); set_a(1, a1);
    charge(9);                                          /* move, movea, move.b, andi, move, subq, swap, move, cmp.l */
    charge(1);                                          /* bgt */
    if (!((int32_t)d0 > (int32_t)w32(0x204A))) { w16_set(0x2430, 0xFFFF); charge(1); }
    charge(2);                                          /* cmpi, bne */
    if (w16(0x2438) != 8) RTS();
    d0 = (d0 << 16) | (d0 >> 16);
    set_d(0, d0);
    charge(3);                                          /* swap, cmp.w, bgt */
    if ((int16_t)d0 > (int16_t)w16(0x204A)) RTS();
    set_a(1, 0x126C2u);
    charge(1);
    CALL(L_5A0A, 0x125E4);
    w16_set(0x2438, 0);
    charge(1);
    RTS();
}

/* FUN_000125ea: show the stage texts: count = min(limit - 1, 8) (the limit as
 * in FUN_000125a0), stored in 0x2438, then FUN_00005a0a for the text entries
 * ROM 0x12620[count] down to [0] */
static uint32_t rd_stage_texts(void)
{
    uint32_t r;
    uint32_t d0 = d_reg(0), d6 = d_reg(6);
    LOW16(d0, w16(0x207E));
    uint32_t a1 = vrd32(0x1242Au + sx16(d0) * 4u);
    LOW8(d0, vrd8(a1)); LOW16(d0, d0 & 7);
    LOW16(d6, vrd16(0x1241Au + sx16(d0) * 2u));
    LOW16(d6, d6 - 1);
    set_d(0, d0); set_a(1, a1);
    charge(8);                                          /* move, movea, move.b, andi, move, subq, cmpi, ble */
    if ((int16_t)d6 > 8) { d6 = 8; charge(1); }
    set_d(6, d6);
    w16_set(0x2438, (uint16_t)d6);
    charge(1);
    for (;;) {
        set_a(1, vrd32(0x12620u + sx16(d_reg(6)) * 4u));
        charge(3);                                      /* movea, nop, jsr */
        if ((r = rd_call(L_5A0A, 0x1261A))) return r;
        uint16_t c = (uint16_t)(d_reg(6) - 1);
        set_d16(6, c);
        charge(1);                                      /* dbf */
        if (c == 0xFFFF) break;
        poll();
    }
    RTS();
}

/* FUN_000126d0: while 0x2082 counts down (and 0x2344 is clear), show one of
 * four texts from the table pair at ROM 0x12702 (bit 0 of 0x2086 picks the
 * table, the count's low two bits the entry); then continue in FUN_000125a0 */
static uint32_t rd_stage_banner(void)
{
    uint32_t r;
    charge(2);                                          /* tst, bne */
    if (w16(0x2344) == 0) {
        uint32_t d0 = d_reg(0);
        LOW16(d0, w16(0x2082));
        set_d(0, d0);
        charge(2);                                      /* move, beq */
        if ((uint16_t)d0 != 0) {
            LOW16(d0, d0 - 1);
            w16_set(0x2082, (uint16_t)d0);
            LOW16(d0, d0 & 3);
            uint32_t d1 = d_reg(1);
            LOW16(d1, w16(0x2086)); LOW16(d1, d1 & 1);
            uint32_t a0 = vrd32(0x12702u + sx16(d1) * 4u);
            set_d(0, d0); set_d(1, d1); set_a(0, a0);
            set_a(1, vrd32(a0 + sx16(d0) * 4u));
            charge(9);                                  /* subq, move, andi, move, andi, movea, nop, movea, jsr */
            if ((r = rd_call(L_5A0A, 0x126FE))) return r;
        }
    }
    charge(1);                                          /* bra.w (backward: polls) */
    return RD_JMP(0x125A0u);
}

/* FUN_00012774: 0x222A = a two-state flag derived from 0x4866 and the course
 * (0x2046: course 2 flips it, course 3 forces 0); when it differs from 0x222C,
 * call one of the two handlers in the table at ROM 0x127B8; 0x222C = 0x222A */
static uint32_t rd_toggle_222a(void)
{
    uint32_t r;
    uint32_t d0 = d_reg(0);
    LOW16(d0, 0u - w16(0x4866));
    charge(4);                                          /* move, neg, cmpi, bne */
    if (w16(0x2046) == 2) { LOW16(d0, d0 + 1); LOW16(d0, d0 & 1); charge(2); }
    charge(2);                                          /* cmpi, bne */
    if (w16(0x2046) == 3) { d0 = 0; charge(1); }
    w16_set(0x222A, (uint16_t)d0);
    uint32_t d1 = d_reg(1);
    LOW16(d1, w16(0x222C));
    LOW16(d0, d0 ^ d1);
    set_d(0, d0); set_d(1, d1);
    charge(4);                                          /* move, move, eor, beq */
    if ((uint16_t)d0 != 0) {
        LOW16(d0, w16(0x222A)); LOW16(d0, d0 & 1);
        uint32_t off = vrd32(0x127B8u + sx16(d0) * 4u);
        set_d(0, off);
        charge(5);                                      /* move, andi, move.l, nop, jsr */
        if ((r = rd_call_ind(0x127B8u + off, 0x127B0, 0x127AC))) return r;
    }
    w16_set(0x222C, w16(0x222A));
    charge(1);
    RTS();
}

/* ============================ 0x12C40 .. 0x13136 ============================ */

/* clear bit 3 of the sound CPU word 0x60004020 unless 0x10001071 is set
 * (the 68K saves D7 around it) */
static void snd_4020_clear_bit3(void)
{
    charge(2);                                          /* tst.b, bne */
    if (vrd8(0x10001071u) != 0) return;
    uint16_t v = (uint16_t)vrd16(0x60004020u);
    vwr16(0x60004020u, (uint16_t)(v & ~8u));
    charge(5);                                          /* move.l, move.w, bclr, move.w, move.l */
}

/* select screen N at priority 0xC40: its record (A6 + 0xC40*128) gets
 * +0x1C = 60 and bit 2 of its first word; A4 = the record, D0w = the offset */
static void mark_screen_record(void)
{
    uint16_t d0 = (uint16_t)(w16(0xC40) << 7);
    set_d16(0, d0);
    uint32_t a4 = 0x10008000u + sx16(d0);
    set_a(4, a4);
    vwr16(a4 + 0x1C, 0x3C);
    vwr16(a4, (uint16_t)(vrd16(a4) | 4));
    charge(5);                                          /* move, lsl, lea, move, ori */
}

/* FUN_00012c40: after FUN_0001811a, one step of a 16-way state machine on
 * 0x2336 (+8 when 0x49CC is clear; jump table at ROM 0x12C60):
 *   0: the result screen -- texts, the per-player times (FUN_0000a8c8), set up
 *   1: count 0x2338 down, then switch to mode 5 (0xDA0) and reset
 *   8: the alternative result screen, set up
 *   9: count 0x2338 down while FUN_0000b788 runs, then FUN_00012f66/12ffa
 *   10-15: count down to mode 5, redrawing each frame
 *   2-7: nothing */
static uint32_t rd_result_screen(void)
{
    uint32_t r;
    CALL(L_1811A, 0x12C46);
    uint32_t d0 = d_reg(0);
    LOW16(d0, w16(0x2336));
    charge(3);                                          /* move, tst, bne */
    if (w16(0x49CC) == 0) { LOW16(d0, d0 + 8); charge(1); }
    LOW16(d0, d0 & 0xF);
    uint32_t off = vrd32(0x12C60u + sx16(d0) * 4u);
    set_d(0, off);
    uint32_t t = 0x12C60u + off;
    charge(4);                                          /* andi, move.l, nop, jmp */
    if (t < 0x12C40u || t > 0x13136u) return RD_JMP(t);
    poll();
    switch (t) {
    case 0x12CA0u:
        RTS();
    case 0x12CA2u:
        snd_4020_clear_bit3();
        charge(1); w16_set(0x4022, 0);
        CALL(L_28680, 0x12CCA); CALL(L_1E6CE, 0x12CD0); CALL(L_575A, 0x12CD6);
        charge(1); w16_set(0xDE2, 0);
        CALL(L_C69A, 0x12CE2); CALL(L_B8B0, 0x12CE8);
        w16_set(0x2338, 0x12C);
        w16_set(0x2338, (uint16_t)(w16(0x2338) + 0x3C));
        w16_set(0x5890, 0); w32_set(0x51F4, 0);
        w16_set(0x51F8, w16(G_SEL_2342)); w16_set(0x51FA, w16(0x1100)); w16_set(0x51FC, w16(0x2340));
        set_a(1, 0x12DD0u);
        charge(8);
        CALL(L_5A0A, 0x12D18);
        w16_set(0x49D4, w16(0x48F2));
        set_a(2, A6W(0x48F4));
        set_d(5, 1);
        charge(3);
        do {                                            /* one line per player: label and time */
            uint32_t a1 = 0x12E05u;
            uint16_t d0w = (uint16_t)(d_reg(5) - 1);
            set_d16(0, d0w);
            uint16_t d1w = w16(0x49BE);
            set_d16(1, d1w);
            charge(6);                                  /* lea, move, subq, move, btst, beq */
            if (d_reg(1) & (1u << (d0w & 31))) { a1 = 0x12E08u; charge(1); }
            set_a(1, a1);
            CALL(L_5A0A, 0x12D3E);
            uint32_t a2 = a_reg(2);
            set_d(1, vrd32(a2)); set_a(2, a2 + 4);
            charge(1);
            CALL(L_A8C8, 0x12D46);
            set_d(6, d_reg(0));
            a1 = 0x12DE1u;
            charge(4);                                  /* move.l, lea, tst, beq */
            if (w16(0x49C0) != 0) { a1 = 0x12DF3u; charge(1); }
            set_a(1, a1);
            CALL(L_5A0A, 0x12D5C);
            set_d16(5, (uint16_t)(d_reg(5) + 1));
            w16_set(0x49D4, (uint16_t)(w16(0x49D4) - 1));
            charge(3);                                  /* addq, subq, bpl */
            if (w16(0x49D4) & 0x8000) break;
            poll();
        } while (1);
        {
            uint32_t a1 = 0x12DE8u;
            charge(3);                                  /* lea, tst, beq */
            if (w16(0x49C0) != 0) { a1 = 0x12DFAu; charge(1); }
            set_a(1, a1);
        }
        CALL(L_5A0A, 0x12D78);
        set_d16(6, w16(0x204E));
        charge(1);
        CALL(L_E9E0, 0x12D82);
        {
            uint16_t d6 = w16(0x204E);
            charge(3);                                  /* move, cmpi, bcs */
            if (d6 >= 4) { d6 = 4; charge(1); }
            set_d16(6, d6);
            set_d16(1, (uint16_t)(d6 - 1));
            charge(2);
        }
        CALL(L_E9D8, 0x12D9A);
        w16_set(0xC40, 0);
        w16_set(0x49CE, (uint16_t)(w16(0x49CE) + 1));
        charge(2);
        CALL(L_176D6, 0x12DAA); CALL(L_1ED04, 0x12DB0);
        mark_screen_record();
        CALL(L_B56E, 0x12DCA);
        w16_set(0x2336, (uint16_t)(w16(0x2336) + 1));
        charge(1);
        RTS();
    case 0x12E0Cu:
        w16_set(0x2338, (uint16_t)(w16(0x2338) - 1));
        charge(2);                                      /* subq, bne */
        if (w16(0x2338) == 0) {
            w16_set(0xDA0, 5); w16_set(0x233A, 0); w16_set(0x2336, 0);
            charge(3);
            CALL(L_2C234, 0x12E26); CALL(L_2C248, 0x12E2C); CALL(L_227D2, 0x12E32);
            RTS();
        }
        charge(1); w16_set(0x57D2, 0);
        CALL(L_28A56, 0x12E40); CALL(L_28A8C, 0x12E46);
        charge(1); w16_set(0xC40, 6);
        CALL(L_131F4, 0x12E50);
        charge(1); w16_set(0xC40, 0);
        CALL(L_176D6, 0x12E5C); CALL(L_1ED04, 0x12E62); CALL(L_22802, 0x12E68);
        RTS();
    case 0x12E6Au:
        snd_4020_clear_bit3();
        CALL(L_28680, 0x12E8C); CALL(L_1E6CE, 0x12E92); CALL(L_575A, 0x12E98);
        bset8(0x11AA, 6);
        w32_set(0x11A4, w32(0x4930));
        w16_set(0x2338, 0xB4);
        w16_set(0x5890, 0); w32_set(0x51F4, 0);
        w16_set(0x51F8, w16(G_SEL_2342)); w16_set(0x51FA, w16(0x1100)); w16_set(0x51FC, w16(0x2340));
        w16_set(0xC40, 0);
        w16_set(0x49CE, (uint16_t)(w16(0x49CE) + 2));
        charge(10);
        CALL(L_176D6, 0x12ED4);
        mark_screen_record();
        CALL(L_B56E, 0x12EEE);
        CALL(L_1320A, 0x12EF2);
        w16_set(0x2336, (uint16_t)(w16(0x2336) + 1));
        charge(1);
        RTS();
    case 0x12F14u:
        w16_set(0x2338, (uint16_t)(w16(0x2338) - 1));
        charge(2);                                      /* subq, bpl */
        if (w16(0x2338) & 0x8000) {
            CALL(L_12F66, 0x12F20); CALL(L_12FFA, 0x12F24);
            w16_set(0x2338, 0x168);
            w16_set(0x2336, (uint16_t)(w16(0x2336) + 1));
            charge(2);
            RTS();
        }
        CALL(L_B788, 0x12F36); CALL(L_176D6, 0x12F3C);
        RTS();
    default:                                            /* 0x130F0 */
        w16_set(0x2338, (uint16_t)(w16(0x2338) - 1));
        charge(2);                                      /* subq, bne */
        if (w16(0x2338) == 0) {
            w16_set(0xDA0, 5); w16_set(0x233A, 0); w16_set(0x2336, 0);
            charge(3);
        }
        charge(2);                                      /* cmpi, bne */
        if (w16(0x2338) == 0xB2) CALL(L_28A8C, 0x13114);
        CALL(L_B7CC, 0x1311A); CALL(L_131F4, 0x1311E); CALL(L_1320C, 0x13122);
        CALL(L_132D6, 0x13126); CALL(L_1325E, 0x1312A); CALL(L_28B3C, 0x13130); CALL(L_28B70, 0x13136);
        RTS();
    }
}

/* FUN_000131f4: append the header (0x8001, D0 with its low word = G_DL_PRIO)
 * to the display list and advance its cursor. A4 = the new cursor. */
static uint32_t rd_dl_header_8001(void)
{
    uint32_t p = vrd32(G_DL_CURSOR);
    vwr32(p, 0x8001);
    uint32_t d0 = (d_reg(0) & 0xFFFF0000u) | (uint16_t)vrd16(G_DL_PRIO);
    set_d(0, d0);
    vwr32(p + 4, d0);
    vwr32(G_DL_CURSOR, p + 8);
    set_a(4, p + 8);
    return RD_RTS;
}

/* ============================ 0x1348A .. 0x13718 ============================ */

/* FUN_0001348a: FUN_0000f61e, FUN_0000c258, then the step for 0x233A & 7 from
 * the table at ROM 0x134AA (0x134CA, 0x1356A, 0x135E2, 0x1366E, 0x13696 x4),
 * then continue in 0xF67E */
static uint32_t rd_step_233a(void)
{
    uint32_t r;
    CALL(L_F61E, 0x1348E); CALL(L_C258, 0x13494);
    uint16_t i = (uint16_t)(w16(0x233A) & 7);
    uint32_t off = vrd32(0x134AAu + (uint32_t)i * 4u);
    set_d(0, off);
    charge(5);                                          /* move, andi, move.l, nop, jsr */
    if ((r = rd_call_ind(0x134AAu + off, 0x134A6, 0x134A2))) return r;
    charge(1);                                          /* jmp */
    return 0xF67Eu;
}

/* FUN_000134ca: set up the attract/demo sequence: texts, palette, clear the
 * 0x1162/0x4346/0x434A state, FUN_00027680; then either (0x49CC clear and
 * 0x204E == 1) a 1200-frame step 1 with FUN_00024f56, or step 3 for 330
 * frames with FUN_00025bc4 and sound word 0x4020 bit 3 cleared */
static uint32_t rd_demo_setup(void)
{
    uint32_t r;
    CALL(L_575A, 0x134D0);
    charge(1); w16_set(0xDE2, 1);
    CALL(L_C69A, 0x134DC); CALL(L_B8B0, 0x134E2); CALL(L_EFEE, 0x134E8);
    charge(3);
    w16_set(0x1162, 0); w32_set(0x4346, 0); w16_set(0x434A, 0);
    CALL(L_27680, 0x13502);
    charge(2);                                          /* tst, bne */
    if (w16(0x49CC) == 0) {
        charge(2);                                      /* cmpi, bne */
        if (w16(0x204E) == 1) {
            w16_set(0x233C, 0x4B0);
            w16_set(0x233A, (uint16_t)(w16(0x233A) + 1));
            w16_set(0x4022, 0); w16_set(0x4200, 0); w16_set(0x41D6, 0);
            charge(5);
            CALL(L_24F56, 0x13532);
            RTS();
        }
    }
    w16_set(0x233A, 3); w16_set(0x233C, 0x14A); w16_set(0x4740, 0);
    charge(3);
    CALL(L_25BC4, 0x1354C);
    snd_4020_clear_bit3();
    RTS();
}

/* FUN_0001366e: unless 0x5884 is 3, FUN_0000b7cc and FUN_00025f50; when it
 * is, FUN_0002863e and advance 0x233A with a 330-frame count in 0x233C */
static uint32_t rd_step_5884(void)
{
    uint32_t r;
    charge(2);                                          /* cmpi, beq */
    if (w16(0x5884) != 3) {
        CALL(L_B7CC, 0x1367C); CALL(L_25F50, 0x13682);
        RTS();
    }
    CALL(L_2863E, 0x1368A);
    w16_set(0x233C, 0x14A);
    w16_set(0x233A, (uint16_t)(w16(0x233A) + 1));
    charge(2);
    RTS();
}

/* FUN_00013696: count 0x233C down (on its first frame, FUN_00025bc4 and clear
 * sound bit 3); at 0 go to mode 1 (0xDA0) and FUN_000055f2; above 5 run
 * FUN_00025bf2 / FUN_00028b12 and, below 0x85, FUN_0000b788 + FUN_000286f2,
 * else FUN_0000b7cc */
static uint32_t rd_countdown_233c(void)
{
    uint32_t r;
    charge(2);                                          /* cmpi, bne */
    if (w16(0x233C) == 0x14A) {
        CALL(L_25BC4, 0x136A4);
        snd_4020_clear_bit3();
    }
    w16_set(0x233C, (uint16_t)(w16(0x233C) - 1));
    charge(2);                                          /* subq, bne */
    if (w16(0x233C) == 0) {
        w16_set(0xDA0, 1); w16_set(0x232E, 0); w16_set(0x233A, 0); w16_set(0x11A4, 0); w16_set(0x11A6, 0);
        charge(5);
        CALL(L_55F2, 0x136E2);
        RTS();
    }
    charge(2);                                          /* cmpi, ble */
    if ((int16_t)w16(0x233C) <= 5) RTS();
    CALL(L_25BF2, 0x136F4); CALL(L_28B12, 0x136FA);
    charge(2);                                          /* cmpi, bcc */
    if (w16(0x233C) < 0x85) {
        CALL(L_B788, 0x13708); CALL(L_286F2, 0x1370E);
        RTS();
    }
    CALL(L_B7CC, 0x13716);
    RTS();
}

/* ============================ 0x1429E .. 0x14870 ============================ */
/* The course record at ROM 0xECDC + course*64 (course = 0x2046) holds pointers
 * to per-course tables: +0x0C the (x, z) word pairs of the course points,
 * +0x10/+0x14/+0x18/+0x1C per-point words, +0x20/+0x32 more; +6 the number of
 * points. 0x204C is the current point, 0x208C/0x2090 a position. */
#define COURSE_RECS 0xECDCu

/* FUN_0001429e: start the course: point 0 for both players (0x20D8/0x20E8
 * entries 0 and 1 = point 0 / the course), position (0x208C, 0x2090) and
 * 0x208E, 0x209E, 0x20A0, 0x21D6, 0x20A2 from the course tables at point 0 */
static uint32_t rd_course_start(void)
{
    regs_t R_; regs_load(&R_);
    uint32_t a0 = COURSE_RECS;
    LOW16(R_.d[7], w16(0x2046));
    LOW16(R_.d[6], R_.d[7]);
    LOW16(R_.d[7], R_.d[7] << 6);
    uint32_t rec = a0 + sx16(R_.d[7]);
    uint32_t a1 = vrd32(rec + 0xC), a2 = vrd32(rec + 0x10);
    w16_set(0x204C, 0);
    LOW16(R_.d[3], w16(0x204C));
    uint32_t i = sx16(R_.d[3]);
    w16_set(0x20D8, (uint16_t)R_.d[3]); w16_set(0x20E8, (uint16_t)R_.d[6]);
    w16_set(0x20DA, (uint16_t)R_.d[3]); w16_set(0x20EA, (uint16_t)R_.d[6]);
    LOW16(R_.d[0], 1);
    w16_set(0x208C, (uint16_t)vrd16(a1 + i * 4u));
    w16_set(0x2090, (uint16_t)vrd16(a1 + 2u + i * 4u));
    w16_set(0x208E, (uint16_t)vrd16(a2 + i * 2u));
    w16_set(0x20A8, 0); w16_set(0x20AA, 0); w16_set(0x21D2, 0); w16_set(0x21D4, 0);
    a1 = vrd32(rec + 0x18); a2 = vrd32(rec + 0x14);
    uint32_t a3 = vrd32(rec + 0x1C);
    w16_set(0x209E, (uint16_t)vrd16(a1 + i * 2u));
    w16_set(0x20A0, (uint16_t)vrd16(a2 + i * 2u));
    w16_set(0x21D6, (uint16_t)vrd16(a2 + i * 2u));
    w16_set(0x20A2, (uint16_t)vrd16(a3 + i * 2u));
    R_.a[0] = a0; R_.a[1] = a1; R_.a[2] = a2; R_.a[3] = a3;
    regs_store(&R_);
    return RD_RTS;
}

/* the nearest course point to (x, z): starting 8 points behind D7w (wrapping
 * around the D6w+1 points), 16 candidates, Manhattan distance to the (x, z)
 * word pairs at A1. D2w = the best point, D5 = its distance (from 0x7FFF);
 * D0/D3/D4/D7 as the 68K loop leaves them. */
static void nearest_course_point(regs_t *R_, uint32_t a1, uint32_t xaddr, uint32_t zaddr)
{
    uint32_t *d = R_->d;
    for (int k = 15; k >= 0; k--) {
        charge(2);                                      /* cmp, ble */
        if ((int16_t)d[7] > (int16_t)d[6]) { d[7] = 0; charge(1); }
        uint32_t p = a1 + sx16(d[7]) * 4u;
        LOW16(d[0], vrd16(xaddr) - vrd16(p));
        charge(3);                                      /* move, sub, bpl */
        if ((int16_t)d[0] < 0) { LOW16(d[0], 0u - d[0]); charge(1); }
        d[0] &= 0xFFFFu;
        LOW16(d[3], vrd16(zaddr) - vrd16(p + 2));
        charge(4);                                      /* andi.l, move, sub, bpl */
        if ((int16_t)d[3] < 0) { LOW16(d[3], 0u - d[3]); charge(1); }
        d[3] &= 0xFFFFu;
        d[3] += d[0];
        charge(4);                                      /* andi.l, add, cmp, bge */
        if ((int32_t)d[3] < (int32_t)d[5]) { d[5] = d[3]; LOW16(d[2], d[7]); charge(2); }
        LOW16(d[7], d[7] + 1);
        LOW16(d[4], d[4] - 1);
        charge(2);                                      /* addq, dbf */
        if (k) { regs_store(R_); poll(); }
    }
}

/* FUN_0001432e: for player 0xC40, find the course point nearest to its
 * position (0x20F8/0x2118[player]) around its current point 0x20D8[player];
 * store it there and its segment (point >> 6) in 0x20C8[player] */
static uint32_t rd_player_nearest_point(void)
{
    regs_t R_; regs_load(&R_);
    uint32_t *d = R_.d;
    R_.a[0] = COURSE_RECS;
    LOW16(d[0], w16(0xC40));
    uint32_t pl = sx16(d[0]) * 2u;
    LOW16(d[7], w16(0x20E8 + pl));
    LOW16(d[7], d[7] << 6);
    uint32_t rec = COURSE_RECS + sx16(d[7]);
    R_.a[1] = vrd32(rec + 0xC);
    d[4] = 0xF;
    LOW16(d[6], vrd16(rec + 6));
    d[5] = 0x7FFF;
    R_.a[2] = A6W(0x20D8) + pl; R_.a[3] = A6W(0x20F8) + pl; R_.a[4] = A6W(0x2118) + pl;
    LOW16(d[2], vrd16(R_.a[2]));
    LOW16(d[7], d[2]);
    LOW16(d[7], d[7] - 8);
    charge(15);
    if ((int16_t)d[7] < 0) { LOW16(d[7], d[7] + 1); LOW16(d[7], d[7] + d[6]); charge(2); }
    nearest_course_point(&R_, R_.a[1], R_.a[3], R_.a[4]);
    vwr16(R_.a[2], (uint16_t)d[2]);
    LOW16(d[0], d[2]); LOW16(d[0], (uint16_t)d[0] >> 6);
    LOW16(d[2], w16(0xC40));
    vwr16(A6W(0x20C8) + sx16(d[2]) * 2u, (uint16_t)d[0]);
    charge(5);
    regs_store(&R_);
    return RD_RTS;
}

/* FUN_000143b6: the nearest course point to the position 0x208C/0x2090
 * around 0x204C (FUN_00014532 in between), stored in 0x204C; then on course 3
 * push the position back inside two walls, and on a course whose record byte
 * +1 is set, switch course at the ends of the point list (the ROM pair table
 * at 0x144F2: new course, new point) */
static uint32_t rd_course_nearest_point(void)
{
    uint32_t r;
    regs_t R_; regs_load(&R_);
    uint32_t *d = R_.d;
    R_.a[0] = COURSE_RECS;
    LOW16(d[7], w16(0x2046)); LOW16(d[7], d[7] << 6);
    uint32_t rec = COURSE_RECS + sx16(d[7]);
    R_.a[1] = vrd32(rec + 0xC);
    R_.a[2] = rec + 1;
    d[4] = 0xF;
    LOW16(d[6], vrd16(rec + 6));
    d[5] = 0x7FFF;
    LOW16(d[7], w16(0x204C)); LOW16(d[7], d[7] - 8);
    charge(11);
    if ((int16_t)d[7] < 0) { LOW16(d[7], d[7] + 1); LOW16(d[7], d[7] + d[6]); charge(2); }
    nearest_course_point(&R_, R_.a[1], A6W(0x208C), A6W(0x2090));
    regs_store(&R_);
    CALL(L_14532, 0x14420);
    regs_load(&R_);
    w16_set(0x204C, (uint16_t)d[2]);
    charge(3);                                          /* move, tst.b, beq */
    if (vrd8(R_.a[2]) == 0) goto done;
    charge(2);                                          /* cmpi, bne */
    if (w16(0x2046) == 3) {
        charge(2);                                      /* cmpi, blt */
        int16_t x = (int16_t)w16(0x208C);
        int inside = 0;
        if (x >= -0x4428) { charge(2); inside = (x <= 0x4128); }
        if (!inside) {                                  /* 0x14446: clamp z */
            charge(2);                                  /* cmpi, bgt */
            if (!((int16_t)w16(0x2090) > -0x4538)) {
                LOW16(d[0], w16(0x2090)); LOW16(d[0], d[0] + 0x4538); LOW16(d[0], d[0] << 1);
                w16_set(0x2090, (uint16_t)-0x4538);
                w16_set(0x2090, (uint16_t)(w16(0x2090) - d[0]));
                charge(5);
            }
            charge(2);                                  /* cmpi, blt */
            if (!((int16_t)w16(0x2090) < -0x3C78)) {
                LOW16(d[0], w16(0x2090)); LOW16(d[0], d[0] + 0x3C78); LOW16(d[0], d[0] << 1);
                w16_set(0x2090, (uint16_t)-0x3C78);
                w16_set(0x2090, (uint16_t)(w16(0x2090) - d[0]));
                charge(5);
            }
            charge(1);                                  /* bra */
            goto done;
        }
        /* 0x14480 */
        charge(2);                                      /* cmpi, blt */
        if (!((int16_t)d[2] < 0x3D)) { LOW16(d[7], w16(0x2046)); charge(2); goto sw; }
        charge(2);                                      /* cmpi, bgt */
        if (!((int16_t)d[2] > 6)) { d[7] = 0xB; charge(2); goto sw; }
    }
    /* 0x14496 */
    charge(2);                                          /* cmpi, bne */
    if (w16(0x2046) == 2) {
        charge(2);                                      /* tst, bne */
        if ((uint16_t)d[2] == 0) { d[7] = 0xC; charge(2); goto sw; }
    }
    charge(2);
    if (w16(0x2046) == 1) {
        charge(2);
        if ((uint16_t)d[2] == 0) { d[7] = 0xD; charge(2); goto sw; }
    }
    LOW16(d[6], d[6] - 4);
    charge(3);                                          /* subq, cmp, ble */
    if ((int16_t)d[2] <= (int16_t)d[6]) goto done;
    LOW16(d[7], w16(0x2046));
    charge(1);
sw: /* 0x144C0: switch course */
    LOW16(d[7], d[7] & 0xF);
    LOW16(d[0], w16(0xC40)); LOW16(d[0], d[0] & 7);
    {
        uint32_t t = 0x144F2u + sx16(d[7]) * 4u, pl = sx16(d[0]) * 2u;
        w16_set(0x2046, (uint16_t)vrd16(t));
        w16_set(0x20E8 + pl, (uint16_t)vrd16(t));
        w16_set(0x204C, (uint16_t)vrd16(t + 2));
        w16_set(0x20D8 + pl, (uint16_t)vrd16(t + 2));
    }
    charge(11);                                         /* andi, move, andi, 4 x (move, nop) */
done:
    regs_store(&R_);
    RTS();
}

/* FUN_000145c4: when 0x20B0 is set, add the six deltas 0x20BC..0x20C6 to the
 * player's (0xC40) position 0x20F8/0x2108/0x2118 and angles 0x2188/0x2198/0x21A8 */
static uint32_t rd_player_add_deltas(void)
{
    charge(2);                                          /* tst, beq */
    if (w16(0x20B0) == 0) return RD_RTS;
    uint16_t pl = w16(0xC40);
    set_d16(6, pl);
    static const uint16_t dst[6] = { 0x20F8, 0x2108, 0x2118, 0x2188, 0x2198, 0x21A8 };
    uint16_t v = 0;
    for (int k = 0; k < 6; k++) {
        v = w16(0x20BC + 2u * (uint32_t)k);
        uint32_t a = A6W(dst[k]) + sx16(pl) * 2u;
        vwr16(a, (uint16_t)(vrd16(a) + v));
    }
    set_d16(0, v);
    charge(13);
    return RD_RTS;
}

/* FUN_00014634: FUN_0001468a / FUN_0001469c for players 0 and 1 in the order
 * bit 0 of 0x2394 selects (jump table at ROM 0x14646); 0xC40 kept */
static uint32_t rd_players_2a_flags(void)
{
    uint32_t r;
    uint16_t i = (uint16_t)(w16(0x2394) & 1);
    uint32_t off = vrd32(0x14646u + (uint32_t)i * 4u);
    set_d(0, off);
    uint32_t t = 0x14646u + off;
    charge(5);                                          /* move, andi, move.l, nop, jmp */
    if (t != 0x1464Eu && t != 0x1466Cu) return RD_JMP(t);
    poll();
    set_d16(7, w16(0xC40));
    w16_set(0xC40, 0);
    charge(2);
    CALL(t == 0x1464Eu ? L_1468A : L_1469C, t == 0x1464Eu ? 0x1465Cu : 0x1467Au);
    w16_set(0xC40, 1);
    charge(1);
    CALL(t == 0x1464Eu ? L_1469C : L_1468A, t == 0x1464Eu ? 0x14666u : 0x14684u);
    w16_set(0xC40, (uint16_t)d_reg(7));
    charge(1);
    RTS();
}

/* copy the player's position/angles into its screen record at A4:
 * 0x2128/0x2148/0x2168[player] (x, y, z * 25) -> +0x42/+0x46/+0x4A,
 * the angles 0x2188/0x2198/0x21A8 -> +0x5A/+0x5C/+0x5E, and mark it (+0 = 1) */
static void player_to_record(uint32_t a4, uint32_t pl)
{
    vwr32(a4 + 0x42, w32(0x2128 + pl * 4u));
    vwr32(a4 + 0x46, w32(0x2148 + pl * 4u));
    vwr32(a4 + 0x4A, w32(0x2168 + pl * 4u));
    vwr16(a4 + 0x5A, w16(0x2188 + pl * 2u));
    vwr16(a4 + 0x5C, w16(0x2198 + pl * 2u));
    vwr16(a4 + 0x5E, w16(0x21A8 + pl * 2u));
    vwr16(a4, 1);
}

/* (int16)w * 25 -- world units to the display's */
static inline uint32_t x25(uint16_t w) { return (uint32_t)((int32_t)(int16_t)w * 0x19); }

/* FUN_000146ae: scale the position 0x208C/0x208E/0x2090 by 25 into
 * 0x2092/0x2096/0x209A and the player's 0x20F8/0x2108/0x2118 into
 * 0x2128/0x2148/0x2168, then copy them into the player's screen record */
static uint32_t rd_player_to_record(void)
{
    uint16_t pl = w16(0xC40);
    set_d16(6, pl);
    uint32_t a4 = 0x10008000u + sx16((uint16_t)(pl << 7));
    set_a(4, a4);
    w32_set(0x2092, x25(w16(0x208C)));
    w32_set(0x2096, x25(w16(0x208E)));
    w32_set(0x209A, x25(w16(0x2090)));
    uint32_t i = sx16(pl);
    w32_set(0x2128 + i * 4u, x25(w16(0x20F8 + i * 2u)));
    w32_set(0x2148 + i * 4u, x25(w16(0x2108 + i * 2u)));
    uint32_t d0 = x25(w16(0x2118 + i * 2u));
    w32_set(0x2168 + i * 4u, d0);
    set_d(0, d0);
    player_to_record(a4, i);
    return RD_RTS;
}

/* FUN_00014744: place player 0xC40 relative to player 0: same position and
 * angles with the yaw turned by 0x400 (and negated), the pitch by 0x8000 and
 * the roll negated, then moved 0x78 along the heading (sine table at A5) and
 * raised by 0x10; scaled into 0x2128.. and copied into its screen record */
static uint32_t rd_player_mirror(void)
{
    uint16_t pl = w16(0xC40);
    set_d16(6, pl);
    uint32_t a4 = 0x10008000u + sx16((uint16_t)(pl << 7)), a5 = a_reg(5);
    set_a(4, a4);
    set_d16(5, 0);
    uint32_t i = sx16(pl);
    w16_set(0x20F8 + i * 2u, w16(0x20F8));
    w16_set(0x2108 + i * 2u, w16(0x2108));
    w16_set(0x2118 + i * 2u, w16(0x2118));
    w16_set(0x2188 + i * 2u, w16(0x2188));
    w16_set(0x2188 + i * 2u, (uint16_t)(w16(0x2188 + i * 2u) - 0x400));
    w16_set(0x2188 + i * 2u, (uint16_t)(0u - w16(0x2188 + i * 2u)));
    w16_set(0x2198 + i * 2u, w16(0x2198));
    w16_set(0x2198 + i * 2u, (uint16_t)(w16(0x2198 + i * 2u) + 0x8000));
    w16_set(0x21A8 + i * 2u, w16(0x21A8));
    w16_set(0x21A8 + i * 2u, (uint16_t)(0u - w16(0x21A8 + i * 2u)));
    set_d16(3, 0x78);
    uint32_t d0 = d_reg(0), d1 = d_reg(1), d2 = d_reg(2);
    LOW16(d0, w16(0x2198 + i * 2u)); LOW16(d0, d0 + 0x4000); LOW16(d0, d0 & 0xFFFE);
    LOW16(d1, vrd16(a5 + sx16(d0)));
    LOW16(d0, d0 + 0x4000); LOW16(d0, d0 & 0xFFFE);
    LOW16(d2, vrd16(a5 + sx16(d0)));
    LOW16(d1, 0u - d1);
    d1 = (uint32_t)((int32_t)(int16_t)d1 * 0x78); d1 += d1; d1 = (d1 << 16) | (d1 >> 16);
    d2 = (uint32_t)((int32_t)(int16_t)d2 * 0x78); d2 += d2; d2 = (d2 << 16) | (d2 >> 16);
    w16_set(0x20F8 + i * 2u, (uint16_t)(w16(0x20F8 + i * 2u) + d2));
    w16_set(0x2118 + i * 2u, (uint16_t)(w16(0x2118 + i * 2u) + d1));
    LOW16(d0, w16(0x2188)); LOW16(d0, d0 & 0xFFFE);
    LOW16(d1, vrd16(a5 + sx16(d0)));
    LOW16(d1, 0u - d1);
    d1 = (uint32_t)((int32_t)(int16_t)d1 * 0x78); d1 += d1; d1 = (d1 << 16) | (d1 >> 16);
    w16_set(0x2108 + i * 2u, (uint16_t)(w16(0x2108 + i * 2u) - d1));
    w16_set(0x2108 + i * 2u, (uint16_t)(w16(0x2108 + i * 2u) + 0x10));
    w32_set(0x2128 + i * 4u, x25(w16(0x20F8 + i * 2u)));
    w32_set(0x2148 + i * 4u, x25(w16(0x2108 + i * 2u)));
    d0 = x25(w16(0x2118 + i * 2u));
    w32_set(0x2168 + i * 4u, d0);
    set_d(0, d0); set_d(1, d1); set_d(2, d2);
    player_to_record(a4, i);
    return RD_RTS;
}

/* FUN_00014872 (a thunk): continue in FUN_00014b70 */
static uint32_t rd_thunk_14b70(void) { return 0x14B70u; }

/* FUN_00014b70: the track geometry at the current course point (0x204C):
 * direction and side (bit 2 of 0x2219: running backwards), the next point in
 * 0x2050, the segment records 0x21C8.. / 0x21E0.. / 0x21F0.. from the course
 * tables +0x20/+0x32, the heading to the position (FUN_0000a98c) relative to
 * the point's heading 0x21D6 in 0x21D0, the distance (FUN_00018452), its
 * across/along components 0x21DE/0x20A6, the banked width terms 0x21C4..
 * 0x21E6, the lateral offsets 0x21D4/0x21D8, and 0x21D2 = the across
 * component scaled by the segment length (0x21DC). */
static uint32_t rd_track_geometry(void)
{
    uint32_t r;
    regs_t R_; regs_load(&R_);
    uint32_t *d = R_.d, *a = R_.a, a5 = a_reg(5);
    LOW16(d[7], w16(0x2046)); LOW16(d[7], d[7] << 6);
    a[0] = COURSE_RECS;
    uint32_t rec = COURSE_RECS + sx16(d[7]);
    a[1] = vrd32(rec + 0x14); a[2] = vrd32(rec + 0xC); a[3] = vrd32(rec + 0x20); a[4] = vrd32(rec + 0x32);
    LOW16(d[3], w16(0x204C));
    d[4] = 1;
    LOW16(d[0], vrd16(a[1] + sx16(d[3]) * 2u));
    w16_set(0x21D6, (uint16_t)d[0]);
    LOW16(d[0], d[0] - w16(0x20A0));
    charge(13);
    if ((int16_t)d[0] < 0) { LOW16(d[0], 0u - d[0]); charge(1); }
    bclr8(0x2219, 2);
    charge(3);                                          /* bclr, cmpi, bcs */
    if ((uint16_t)d[0] >= 0x4000) { LOW16(d[4], d[4] - 2); bset8(0x2219, 2); charge(2); }
    LOW16(d[4], d[4] + d[3]);
    w16_set(0x2050, (uint16_t)d[4]);
    uint32_t i3 = sx16(d[3]), i4 = sx16(d[4]);
    w32_set(0x21C8, vrd32(a[3] + i3 * 8u));
    w32_set(0x21E0, vrd32(a[3] + 4u + i3 * 8u));
    w32_set(0x21F0, vrd32(a[4] + i3 * 4u));
    w32_set(0x21E8, vrd32(a[3] + 4u + i4 * 8u));
    w32_set(0x21F4, vrd32(a[4] + i4 * 4u));
    LOW16(d[1], w16(0x208C)); LOW16(d[1], d[1] - vrd16(a[2] + i3 * 4u));
    LOW16(d[2], w16(0x2090)); LOW16(d[2], d[2] - vrd16(a[2] + 2u + i3 * 4u));
    LOW16(d[5], d[1]); LOW16(d[6], d[2]);
    d[5] = muls_w(d[5], d[5]); d[6] = muls_w(d[6], d[6]);
    d[6] += d[5];
    charge(18);
    int zero = 0;
    if ((uint16_t)d[1] == 0) {
        charge(2);                                      /* tst, bne */
        if ((uint16_t)d[2] == 0) { d[0] = 0; charge(2); zero = 1; }
    }
    if (!zero) {
        regs_store(&R_);
        CALL(L_A98C, 0x14C06);
        regs_load(&R_);
        LOW16(d[0], 0u - d[0]); LOW16(d[0], d[0] + 0x4000);
        charge(2);
    }
    LOW16(d[0], d[0] - w16(0x21D6));
    w16_set(0x21D0, (uint16_t)d[0]);
    d[3] = d[6];
    charge(3);
    regs_store(&R_);
    CALL(L_18452, 0x14C1C);
    regs_load(&R_);
    /* across / along components of the distance (D2) at the heading 0x21D0 */
    LOW16(d[0], w16(0x21D0)); LOW16(d[0], d[0] + 0x4000); LOW16(d[0], d[0] & 0xFFFE);
    LOW16(d[4], vrd16(a5 + sx16(d[0])));
    LOW16(d[0], d[0] + 0x4000); LOW16(d[0], d[0] & 0xFFFE);
    LOW16(d[5], vrd16(a5 + sx16(d[0])));
    LOW16(d[4], 0u - d[4]);
    d[4] = fmul15(d[2], d[4]);
    d[5] = fmul15(d[2], d[5]);
    w16_set(0x21DE, (uint16_t)d[4]);
    w16_set(0x20A6, (uint16_t)d[5]);
    rec = a[0] + sx16(d[7]);
    a[1] = vrd32(rec + 0x10); a[4] = vrd32(rec + 0x1C);
    LOW16(d[3], w16(0x204C)); LOW16(d[4], w16(0x2050));
    i3 = sx16(d[3]); i4 = sx16(d[4]);
    LOW16(d[0], vrd16(a[4] + i3 * 2u)); LOW16(d[0], d[0] + 0x4000); LOW16(d[0], d[0] & 0xFFFE);
    LOW16(d[1], vrd16(a5 + sx16(d[0])));
    LOW16(d[1], 0u - d[1]);
    charge(27);
    if ((uint16_t)d[1] != 0) {
        d[5] &= 0xFFFFu; d[5] = swap32(d[5]);
        divs_w(&d[5], d[1]);
        LOW16(d[5], (uint16_t)((int16_t)d[5] >> 1));
        charge(4);
    }
    w16_set(0x20A4, (uint16_t)d[5]);
    LOW16(d[0], vrd16(a[4] + i3 * 2u)); LOW16(d[0], d[0] + 0x4000); LOW16(d[0], d[0] & 0xFFFE);
    LOW16(d[0], vrd16(a5 + sx16(d[0])));
    LOW16(d[0], 0u - d[0]);
    LOW16(d[1], w16(0x21C8)); LOW16(d[2], w16(0x21CA));
    d[1] = fmul15(d[0], d[1]); d[2] = fmul15(d[0], d[2]);
    w16_set(0x21CC, (uint16_t)d[1]); w16_set(0x21CE, (uint16_t)d[2]);
    LOW16(d[0], w16(0x21F0)); LOW16(d[0], d[0] + 0x4000); LOW16(d[0], d[0] & 0xFFFE);
    LOW16(d[0], vrd16(a5 + sx16(d[0])));
    LOW16(d[0], 0u - d[0]);
    LOW16(d[1], w16(0x21E0)); LOW16(d[1], d[1] - 0x40);
    charge(24);
    if ((int16_t)d[1] < 0) { d[1] = 0; charge(1); }
    d[1] = fmul15(d[0], d[1]);
    w16_set(0x21E4, (uint16_t)d[1]);
    LOW16(d[0], w16(0x21F2)); LOW16(d[0], d[0] + 0x4000); LOW16(d[0], d[0] & 0xFFFE);
    LOW16(d[0], vrd16(a5 + sx16(d[0])));
    LOW16(d[0], 0u - d[0]);
    LOW16(d[1], w16(0x21E2)); LOW16(d[1], d[1] - 0x40);
    charge(12);
    if ((int16_t)d[1] < 0) { d[1] = 0; charge(1); }
    d[1] = fmul15(d[0], d[1]);
    w16_set(0x21E6, (uint16_t)d[1]);
    LOW16(d[1], w16(0x21CC)); LOW16(d[1], d[1] + w16(0x21E4)); w16_set(0x21C4, (uint16_t)d[1]);
    LOW16(d[1], w16(0x21CE)); LOW16(d[1], d[1] + w16(0x21E6)); w16_set(0x21C6, (uint16_t)d[1]);
    LOW16(d[0], vrd16(a[4] + i3 * 2u)); LOW16(d[0], d[0] + 0x8000); LOW16(d[0], d[0] & 0xFFFE);
    LOW16(d[1], vrd16(a5 + sx16(d[0])));
    LOW16(d[0], vrd16(a[4] + i4 * 2u)); LOW16(d[0], d[0] + 0x8000); LOW16(d[0], d[0] & 0xFFFE);
    LOW16(d[2], vrd16(a5 + sx16(d[0])));
    d[6] = 1;
    LOW16(d[5], w16(0x20A6));
    charge(21);
    if ((int16_t)d[5] < 0) { LOW16(d[5], 0u - d[5]); d[6] = 0; charge(2); }
    uint32_t i6 = sx16(d[6]) * 2u;
    LOW16(d[0], w16(0x20A4));
    charge(3);                                          /* move, cmp, ble */
    if ((int16_t)d[5] > (int16_t)w16(0x21CC + i6)) {
        LOW16(d[0], w16(0x21C8 + i6));
        charge(3);                                      /* move, tst, bne */
        if ((uint16_t)d[6] == 0) { LOW16(d[0], 0u - d[0]); charge(1); }
    }
    d[1] = fmul15(d[0], d[1]); d[2] = fmul15(d[0], d[2]);
    w16_set(0x21EC, 0);
    LOW16(d[5], d[5] - w16(0x21CC + i6));
    charge(9);
    if ((uint16_t)d[5] != 0) {
        charge(1);                                      /* bmi */
        if ((int16_t)d[5] > 0) {
            charge(2);                                  /* tst, bne */
            if ((uint16_t)d[6] == 0) { LOW16(d[5], 0u - d[5]); charge(1); }
            LOW16(d[0], w16(0x21F0 + i6)); LOW16(d[0], d[0] + 0x8000); LOW16(d[0], d[0] & 0xFFFE);
            LOW16(d[0], vrd16(a5 + sx16(d[0])));
            d[0] = fmul15(d[5], d[0]);
            LOW16(d[1], d[1] + d[0]);
            LOW16(d[0], w16(0x21F4 + i6)); LOW16(d[0], d[0] + 0x8000); LOW16(d[0], d[0] & 0xFFFE);
            LOW16(d[0], vrd16(a5 + sx16(d[0])));
            d[0] = fmul15(d[5], d[0]);
            LOW16(d[2], d[2] + d[0]);
            w16_set(0x21EC, w16(0x21F0));
            charge(17);
        }
    }
    LOW16(d[1], d[1] + vrd16(a[1] + i3 * 2u));
    LOW16(d[2], d[2] + vrd16(a[1] + i4 * 2u));
    charge(4);                                          /* add, add, btst, beq */
    if (vrd8(A6W(0x2219)) & 4) {
        LOW16(d[5], d[1]); LOW16(d[5], d[5] - vrd16(a[1] + i4 * 2u));
        w16_set(0x21D4, (uint16_t)d[5]);
        LOW16(d[1], d[1] - d[2]);
        w16_set(0x21D8, (uint16_t)d[1]);
        charge(6);
    } else {
        LOW16(d[5], d[2]); LOW16(d[5], d[5] - vrd16(a[1] + i3 * 2u));
        w16_set(0x21D4, (uint16_t)d[5]);
        LOW16(d[2], d[2] - d[1]);
        w16_set(0x21D8, (uint16_t)d[2]);
        charge(5);
    }
    /* the segment length: distance from point D3 to point D4 */
    LOW16(d[0], vrd16(a[2] + i3 * 4u)); LOW16(d[0], d[0] - vrd16(a[2] + i4 * 4u));
    LOW16(d[1], vrd16(a[2] + 2u + i3 * 4u)); LOW16(d[1], d[1] - vrd16(a[2] + 2u + i4 * 4u));
    d[0] = muls_w(d[0], d[0]); d[1] = muls_w(d[1], d[1]);
    d[0] += d[1];
    d[3] = d[0];
    charge(8);
    regs_store(&R_);
    CALL(L_18452, 0x14E08);
    regs_load(&R_);
    w16_set(0x21DC, (uint16_t)d[2]);
    LOW16(d[0], w16(0x21DE));
    LOW16(d[1], w16(0x21D8));
    d[1] = muls_w(d[0], d[1]);
    LOW16(d[0], w16(0x21DC));
    divs_w(&d[1], d[0]);
    w16_set(0x21D2, (uint16_t)d[1]);
    charge(8);
    regs_store(&R_);
    return RD_RTS;
}

/* ============================ 0x14EB6 .. 0x1514A ============================ */

/* FUN_00014eb6: build the course display list for player 0xC40: the camera
 * (0x21B8.. = the player's 0x2128/0x2148/0x2168), a 0x8017 view block with the
 * sky model (course record +0x2A, or +0x2C when 0x221A is set) rotated by
 * 0x434A, then five track pieces around the player's segment 0x20C8 (runs of
 * FUN_0001502c / FUN_00015038 / FUN_0001506c, direction bit 0 of 0x2218 from
 * the heading against the track), then FUN_000150e6; ends the list. */
/* DRAW DISTANCE (a deliberate change, OFF by default): the original draws five
 * track pieces around the player; g_rr_draw_extra adds that many more AHEAD,
 * with their trackside objects. 0 = the original, which the checker proves
 * equal to the lifted code (RR_RD=check forces 0). Menu: Display -> Draw
 * distance; rr_controls.cfg `draw_distance`; headless RR_DRAW_EXTRA=<n>. */
int g_rr_draw_extra;

static uint32_t rd_course_display_list(void)
{
    uint32_t r;
    regs_t R_;
    CALL(L_1514C, 0x14EBA); CALL(L_1432E, 0x14EBE);
    regs_load(&R_);
    uint32_t *d = R_.d, *a = R_.a, a5 = a_reg(5);
    d[0] = 0; d[1] = 0;
    a[0] = COURSE_RECS;
    LOW16(d[6], w16(0xC40));
    LOW16(d[7], w16(0x20E8 + sx16(d[6]) * 2u)); LOW16(d[7], d[7] << 6);
    LOW16(d[6], 4 + (uint32_t)g_rr_draw_extra);          /* dbf count: 5 pieces (+ extra) */
    LOW16(d[0], w16(0xC40));
    uint32_t pl = sx16(d[0]);
    LOW16(d[5], w16(0x20C8 + pl * 2u));
    w32_set(0x21B8, w32(0x2128 + pl * 4u)); w32_set(0x21BC, w32(0x2148 + pl * 4u)); w32_set(0x21C0, w32(0x2168 + pl * 4u));
    uint32_t rec = a[0] + sx16(d[7]);
    a[1] = vrd32(rec + 0x14); a[2] = vrd32(rec + 8);
    uint32_t p = vrd32(G_DL_CURSOR);
    a[4] = vrd32(rec + 0x2E);
    put32(&p, 0x8002);
    LOW16(d[0], w16(0xC40)); put32(&p, d[0]);
    LOW16(d[0], vrd16(rec + 0x2A));
    charge(22);
    if (w16(0x221A) != 0) { LOW16(d[0], vrd16(rec + 0x2C)); charge(1); }
    put32(&p, 0x8017); put32(&p, 1); put32(&p, 0x7FFF); put32(&p, 0x7FFF); put32(&p, 0x7FFF);
    put32(&p, d[0]); put32(&p, d[1]); put32(&p, w32(0x4346)); put32(&p, d[1]); put32(&p, 0); put32(&p, 0x7FFF);
    LOW16(d[0], w16(0x434A)); LOW16(d[0], d[0] & 0xFFFE);
    LOW16(d[1], vrd16(a5 + sx16(d[0])));
    LOW16(d[0], d[0] + 0x4000);
    LOW16(d[2], vrd16(a5 + sx16(d[0])));
    LOW16(d[1], 0u - d[1]);
    put32(&p, d[1]); put32(&p, d[2]); put32(&p, 0); put32(&p, 0x7FFF); put32(&p, 0); put32(&p, 0x8000);
    LOW16(d[0], w16(0xC40)); put32(&p, d[0]);
    /* direction: the track heading at the player's point against its heading */
    LOW16(d[1], w16(0xC40));
    uint32_t i1 = sx16(d[1]) * 2u;
    LOW16(d[0], w16(0x20D8 + i1));
    LOW16(d[0], vrd16(a[1] + sx16(d[0]) * 2u));
    LOW16(d[0], d[0] - w16(0x2198 + i1));
    charge(30);
    if ((int16_t)d[0] < 0) { LOW16(d[0], 0u - d[0]); charge(1); }
    bclr8(0x2218, 0);
    charge(3);                                          /* bclr, cmpi, bcs */
    if ((uint16_t)d[0] >= 0x4000) { LOW16(d[5], d[5] - 2); bset8(0x2218, 0); charge(2); }
    LOW16(d[5], d[5] - 1);
    charge(2);                                          /* subq, bpl */
    if ((int16_t)d[5] < 0) {
        LOW16(d[5], d[5] + 1);
        LOW16(d[5], d[5] + vrd16(rec + 4));
        charge(4);                                      /* addq, add, tst.b, beq */
        if (vrd8(rec + 1) != 0) { d[5] = 0; LOW16(d[6], d[6] - 1); charge(2); }
    }
    a[3] = p;
    for (;;) {                                          /* 0x14FD0 */
        rec = a[0] + sx16(d[7]);
        charge(2);                                      /* cmp, ble */
        if ((int16_t)d[5] > (int16_t)vrd16(rec + 4)) {
            charge(2);                                  /* tst.b, bne */
            if (vrd8(rec + 1) != 0) break;
            d[5] = 0;
            charge(1);
        }
        LOW16(d[4], d[5]); LOW16(d[4], d[4] << 1);
        a[1] = a[2];
        d[1] = 0; LOW16(d[1], vrd16(rec + 2));
        LOW16(d[0], w16(0xC40));
        charge(8);
        if (vrd8(A6W(0x2216)) & (1u << (d[0] & 7))) { LOW16(d[1], vrd16(rec + 0x24)); charge(1); }
        regs_store(&R_);
        CALL(L_1502C, 0x14FFC);
        regs_load(&R_);
        rec = a[0] + sx16(d[7]);
        LOW16(d[1], vrd16(rec + 0x26));
        LOW16(d[0], w16(0xC40));
        charge(4);
        if (vrd8(A6W(0x2216)) & (1u << (d[0] & 7))) { LOW16(d[1], vrd16(rec + 0x28)); charge(1); }
        regs_store(&R_);
        CALL(L_15038, 0x15012);
        CALL(L_1506C, 0x15016);
        regs_load(&R_);
        LOW16(d[5], d[5] + 1);
        charge(2);                                      /* addq, dbf */
        LOW16(d[6], d[6] - 1);
        if ((uint16_t)d[6] == 0xFFFF) break;
        regs_store(&R_); poll();
    }
    regs_store(&R_);
    CALL(L_150E6, 0x15020);
    p = a_reg(3);
    vwr32(p, 0xFFFFFFFFu);
    vwr32(G_DL_CURSOR, p);
    charge(2);
    RTS();
}

/* FUN_00015038: when D1 is not zero, two vertex records (FUN_00015048):
 * index D1w, then D1w + 1 */
static uint32_t rd_two_vertex_records_nz(void)
{
    uint32_t r;
    charge(2);                                          /* tst.l, beq */
    if (d_reg(1) == 0) RTS();
    CALL(L_15048, 0x15040);
    set_d16(1, (uint16_t)(d_reg(1) + 1));
    charge(1);
    CALL(L_15048, 0x15046);
    RTS();
}

/* FUN_0001506c: the objects standing on track piece D5 (records of 8 bytes at
 * A4, keyed by the piece's point number 2*D5 + course word +2; the search
 * starts at the player's cursor 0x2206[player]). Each object's model group
 * (+4 + +2) is drawn once per frame -- 0x2230[0..0x222E] lists those already
 * drawn -- with FUN_00015038 / FUN_0001502c; the cursor ends at the first
 * zero record (reset to 0). */
static uint32_t rd_track_objects(void)
{
    uint32_t r;
    regs_t R_; regs_load(&R_);
    uint32_t *d = R_.d, *a = R_.a;
    LOW16(d[3], d[5]); LOW16(d[3], d[3] << 1);
    LOW16(d[3], d[3] + vrd16(a[0] + sx16(d[7]) + 2u));
    LOW16(d[0], w16(0xC40));
    LOW16(d[2], w16(0x2206 + sx16(d[0]) * 2u));
    charge(5);
    for (;;) {                                          /* 0x1507E */
        uint32_t e = a[4] + sx16(d[2]) * 8u;
        LOW16(d[0], vrd16(e));
        charge(2);                                      /* move, beq */
        if ((uint16_t)d[0] == 0) break;
        charge(2);                                      /* cmp, beq */
        if ((uint16_t)d[0] == (uint16_t)d[3]) {
            LOW16(d[1], vrd16(e + 4)); LOW16(d[1], d[1] + vrd16(e + 2));
            LOW16(d[0], w16(0x222E));
            charge(4);                                  /* move, add, move, bmi */
            int seen = 0;
            if (!((int16_t)d[0] < 0)) {
                for (;;) {                              /* 0x150A8 */
                    charge(2);                          /* cmp, beq */
                    if ((uint16_t)vrd16(A6W(0x2230) + sx16(d[0]) * 2u) == (uint16_t)d[1]) { seen = 1; break; }
                    charge(1);                          /* dbf */
                    LOW16(d[0], d[0] - 1);
                    if ((uint16_t)d[0] == 0xFFFF) break;
                    regs_store(&R_); poll();
                }
            }
            if (!seen) {
                LOW16(d[0], w16(0x222E)); LOW16(d[0], d[0] + 1);
                w16_set(0x2230 + sx16(d[0]) * 2u, (uint16_t)d[1]);
                w16_set(0x222E, (uint16_t)d[0]);
                d[1] = 0;
                LOW16(d[0], vrd16(e + 6)); LOW16(d[0], d[0] << 6);
                LOW16(d[4], vrd16(e + 4));
                a[1] = vrd32(a[0] + sx16(d[0]) + 8u);
                LOW16(d[1], vrd16(a[0] + sx16(d[0]) + 0x28u));
                charge(10);
                regs_store(&R_);
                CALL(L_15038, 0x150DC);
                regs_load(&R_);
                LOW16(d[1], vrd16(a[4] + sx16(d[2]) * 8u + 2u));
                charge(1);
                regs_store(&R_);
                CALL(L_1502C, 0x150E4);
                regs_load(&R_);
            }
            charge(1);                                  /* bra 0x15088 */
            regs_store(&R_); poll();
        }
        LOW16(d[2], d[2] + 1);
        charge(2);                                      /* addq, bra */
        regs_store(&R_); poll();
    }
    d[2] = 0;
    LOW16(d[0], w16(0xC40));
    w16_set(0x2206 + sx16(d[0]) * 2u, (uint16_t)d[2]);
    charge(3);
    regs_store(&R_);
    RTS();
}

/* FUN_000150e6: on a course whose record byte +1 is clear, draw the track
 * pieces behind the start (0 or 4 of them, the side from bit 0 of 0x2218 and
 * the player's bit in 0x2216) with FUN_0001502c / FUN_00015038 */
static uint32_t rd_track_tail(void)
{
    uint32_t r;
    regs_t R_; regs_load(&R_);
    uint32_t *d = R_.d, *a = R_.a;
    uint32_t rec = a[0] + sx16(d[7]);
    charge(2);                                          /* tst.b, bne */
    if (vrd8(rec + 1) != 0) RTS();
    d[4] = 0; d[6] = 0;
    LOW16(d[0], w16(0xC40));
    charge(5);                                          /* moveq, moveq, move, btst, bne */
    if (!(vrd8(A6W(0x2216)) & (1u << (d[0] & 7)))) { LOW16(d[6], d[6] + 1); d[4] = 4; charge(2); }
    charge(2);                                          /* btst, beq */
    if (vrd8(A6W(0x2218)) & 1) {
        LOW16(d[6], 0u - d[6]); LOW16(d[6], d[6] + 5); LOW16(d[6], d[6] + d[4]);
        charge(3);
    }
    LOW16(d[5], d[5] - d[6]);
    charge(2);                                          /* sub, bpl */
    if ((int16_t)d[5] < 0) { LOW16(d[5], d[5] + 1); LOW16(d[5], d[5] + vrd16(rec + 4)); charge(2); }
    LOW16(d[6], d[4]);
    charge(2);                                          /* move, beq */
    if ((uint16_t)d[6] == 0) { regs_store(&R_); RTS(); }
    LOW16(d[6], d[6] - 1);
    charge(1);
    for (;;) {                                          /* 0x1511C */
        rec = a[0] + sx16(d[7]);
        charge(2);                                      /* cmp, ble */
        if ((int16_t)d[5] > (int16_t)vrd16(rec + 4)) { d[5] = 0; charge(1); }
        LOW16(d[4], d[5]); LOW16(d[4], d[4] << 1);
        a[1] = a[2];
        d[1] = 0; LOW16(d[1], vrd16(rec + 0x24));
        charge(5);
        regs_store(&R_);
        CALL(L_1502C, 0x15134);
        regs_load(&R_);
        rec = a[0] + sx16(d[7]);
        LOW16(d[4], d[5]); LOW16(d[4], d[4] << 1);
        a[1] = a[2];
        d[1] = 0; LOW16(d[1], vrd16(rec + 0x28));
        charge(5);
        regs_store(&R_);
        CALL(L_15038, 0x15144);
        regs_load(&R_);
        LOW16(d[5], d[5] + 1);
        charge(2);                                      /* addq, dbf */
        LOW16(d[6], d[6] - 1);
        if ((uint16_t)d[6] == 0xFFFF) break;
        regs_store(&R_); poll();
    }
    regs_store(&R_);
    RTS();
}

/* ============================ 0x1563A .. 0x15E34 ============================ */

/* FUN_0001563a: set up screen object 2 from the descriptor at ROM 0x155F4
 * (FUN_0000b4e4 / b56e / b6d0), clear its bit in 0x2216, start the 120-frame
 * count 0x237A with step 0x2378 = 1 and clear 0x2376 / 0x237C */
static uint32_t rd_init_object2(void)
{
    uint32_t r;
    w16_set(0xC40, 2);
    set_a(0, 0x155F4u);
    charge(2);
    CALL(L_B4E4, 0x1564A); CALL(L_B56E, 0x15650); CALL(L_B6D0, 0x15656);
    uint16_t d0 = w16(0xC40);
    set_d16(0, d0);
    bclr8(0x2216, d0);
    w16_set(0x237A, 0x78); w16_set(0x2378, 1); w16_set(0x2376, 0); w16_set(0x237C, 0);
    charge(6);
    RTS();
}

/* FUN_00015674: while 0x237A is not negative: 0x237C += 0x20 (at most 0x700),
 * object 2's record: +0x1E grows by 5 up to 0x64 (then state 4, 0x237C = 0),
 * FUN_0000b56e and FUN_000156c0 */
static uint32_t rd_object2_step(void)
{
    uint32_t r;
    charge(2);                                          /* tst, bmi */
    if (w16(0x237A) & 0x8000) RTS();
    uint32_t d0 = d_reg(0);
    LOW16(d0, w16(0x237C)); LOW16(d0, d0 + 0x20);
    charge(4);                                          /* move, addi, cmpi, ble */
    if ((int16_t)d0 > 0x700) { LOW16(d0, 0x700); charge(1); }
    w16_set(0x237C, (uint16_t)d0);
    w16_set(0xC40, 2);
    LOW16(d0, w16(0xC40)); LOW16(d0, d0 << 7);
    uint32_t a4 = 0x10008000u + sx16(d0);
    set_d(0, d0); set_a(4, a4);
    charge(7);                                          /* move, move, move, lsl, lea, cmpi, bgt */
    if (!((int16_t)vrd16(a4 + 0x1E) > 0x64)) {
        vwr16(a4 + 0x1E, (uint16_t)(vrd16(a4 + 0x1E) + 5));
        vwr16(a4, 4);
        w16_set(0x237C, 0);
        charge(3);
    }
    CALL(L_B56E, 0x156BA); CALL(L_156C0, 0x156BE);
    RTS();
}

/* FUN_000156c0: step the four-part object 2: 0x2376 grows by the doubling
 * step 0x2378 toward the ROM stops 0x157CC (one per 0x790), 0x237A counts
 * down at the last stop; then append a 0x8017 block of four parts (models ROM
 * 0x157C4[n], height stop - 0x2376, depth 0x237C, pitch 0xF90) and close the list */
static uint32_t rd_object2_draw(void)
{
    uint32_t r;
    regs_t R_; regs_load(&R_);
    uint32_t *d = R_.d, a5 = a_reg(5);
    LOW16(d[5], w16(0x2376)); d[5] = sx16(d[5]);
    divs_w(&d[5], 0x790);
    LOW16(d[4], d[5]);
    LOW16(d[5], d[5] - 2); LOW16(d[5], 0u - d[5]);
    charge(7);
    if ((int16_t)d[5] < 0) { LOW16(d[5], 0); charge(1); }
    charge(2);                                          /* tst, bne */
    if ((uint16_t)d[5] == 0) { w16_set(0x237A, (uint16_t)(w16(0x237A) - 1)); charge(1); }
    LOW16(d[6], w16(0x49C8));
    charge(2);                                          /* move, beq */
    if ((uint16_t)d[6] != 0) { LOW16(d[6], (uint16_t)d[6] >> 6); LOW16(d[6], d[6] + 1); charge(2); }
    charge(2);                                          /* cmp, bne */
    if ((uint16_t)d[6] == (uint16_t)d[5]) {
        LOW16(d[0], w16(0x2378)); LOW16(d[0], d[0] + d[0]);
        w16_set(0x2378, (uint16_t)d[0]);
        w16_set(0x2376, (uint16_t)(w16(0x2376) + d[0]));
        charge(6);                                      /* move, add, move, add, cmpi, ble */
        if ((int16_t)d[4] > 2) { LOW16(d[4], 2); charge(1); }
        LOW16(d[4], d[4] + 1);
        d[4] = muls_w(d[4], 0x790);
        LOW16(d[4], vrd16(0x157CCu + sx16(d[5]) * 2u));
        charge(5);                                      /* addq, muls, move, cmp, bge */
        if ((int16_t)d[4] < (int16_t)w16(0x2376)) { w16_set(0x2376, (uint16_t)d[4]); w16_set(0x2378, 1); charge(2); }
    }
    regs_store(&R_);
    CALL(L_15862, 0x15722);
    regs_load(&R_);
    uint32_t p = R_.a[3];
    d[7] = 3;
    put32(&p, 0x8017); put32(&p, d[7]); put32(&p, 0x5FFF); put32(&p, 0x4000); put32(&p, 0x4000);
    charge(6);
    for (;;) {                                          /* 0x1573E */
        uint32_t n = sx16(d[7]) * 2u;
        d[0] = (uint16_t)vrd16(0x157C4u + n);
        put32(&p, d[0]);
        LOW16(d[0], 0u - w16(0x2376)); LOW16(d[0], d[0] + vrd16(0x157CCu + n));
        d[0] = sx16(d[0]);
        put32(&p, d[0]);
        put32(&p, 0);
        d[0] = sx16(w16(0x237C));
        put32(&p, d[0]);
        static const uint16_t ang[3] = { 0xF90, 0, 0 };
        d[1] = 0; d[2] = 0;
        for (int k = 0; k < 3; k++) {                   /* (-sin, cos) for pitch, yaw, roll */
            d[0] = ang[k] & 0xFFFE;
            LOW16(d[1], vrd16(a5 + sx16(d[0])));
            LOW16(d[0], d[0] + 0x4000);
            LOW16(d[2], vrd16(a5 + sx16(d[0])));
            LOW16(d[1], 0u - d[1]);
            put32(&p, d[1]); put32(&p, d[2]);
        }
        put32(&p, 0);
        charge(42);
        LOW16(d[7], d[7] - 1);
        if ((uint16_t)d[7] == 0xFFFF) break;
        R_.a[3] = p;
        regs_store(&R_); poll();
    }
    R_.a[3] = p;
    regs_store(&R_);
    charge(1);                                          /* bra.w (forward: no poll) */
    return 0x15876u;
}

/* FUN_00015840: when 0x2430 is set, FUN_000162b0 and FUN_0001638c; then
 * continue in FUN_00015e18 */
static uint32_t rd_15840(void)
{
    uint32_t r;
    charge(2);                                          /* tst, beq */
    if (w16(0x2430) != 0) { CALL(L_162B0, 0x1584A); CALL(L_1638C, 0x1584E); }
    charge(1);                                          /* bra.w */
    return 0x15E18u;
}

/* FUN_0001595c: one roadside object, record D7 (8 bytes from A0 + 8: x, y,
 * z, model) relative to player D6's position (0x2128/0x2148/0x2168, indexed
 * D6w*2 as the 68K does): when within 0x2000 (Manhattan, >> 5) append it at
 * A3 facing the player (heading from FUN_0000a98c) */
static uint32_t rd_roadside_object(void)
{
    uint32_t r;
    regs_t R_; regs_load(&R_);
    uint32_t *d = R_.d, *a = R_.a, a5 = a_reg(5);
    uint32_t e = a[0] + sx16(d[7]) * 8u, pl = sx16(d[6]) * 2u;
    d[1] = (sx16(vrd16(e + 8)) << 4) - w32(0x2128 + pl);
    d[2] = (sx16(vrd16(e + 0xC)) << 4) - w32(0x2168 + pl);
    a[2] = d[1]; d[5] = d[2];
    d[1] = (uint32_t)((int32_t)d[1] >> 5) & 0xFFFFu;
    d[2] = (uint32_t)((int32_t)d[2] >> 5) & 0xFFFFu;
    d[3] = 0; d[4] = 0;
    LOW16(d[3], d[1]);
    charge(18);
    if ((int16_t)d[3] < 0) { LOW16(d[3], 0u - d[3]); charge(1); }
    LOW16(d[4], d[2]);
    charge(2);
    if ((int16_t)d[4] < 0) { LOW16(d[4], 0u - d[4]); charge(1); }
    d[3] += d[4];
    charge(3);                                          /* add.l, cmpi.l, bhi */
    if (d[3] > 0x2000u) { regs_store(&R_); RTS(); }
    int zero = 0;
    charge(2);                                          /* tst, bne */
    if ((uint16_t)d[1] == 0) {
        charge(2);
        if ((uint16_t)d[2] == 0) { d[0] = 0; charge(2); zero = 1; }
    }
    if (!zero) {
        regs_store(&R_);
        CALL(L_A98C, 0x159BA);
        regs_load(&R_);
        LOW16(d[0], 0u - d[0]); LOW16(d[0], d[0] + 0x4000);
        charge(2);
    }
    d[1] = a[2]; d[2] = d[5];
    LOW16(d[5], d[0]);
    e = a[0] + sx16(d[7]) * 8u; pl = sx16(d[6]) * 2u;
    uint32_t p = a[3];
    d[0] = (uint16_t)vrd16(e + 0xE);
    put32(&p, d[0]); put32(&p, d[1]);
    d[0] = sx16(vrd16(e + 0xA)) + vrd32(a[0]) - w32(0x2148 + pl);
    put32(&p, d[0]); put32(&p, d[2]);
    const uint16_t ang[3] = { 0, (uint16_t)d[5], 0 };
    d[0] = 0; d[1] = 0; d[2] = 0;
    for (int k = 0; k < 3; k++) {
        LOW16(d[0], ang[k]); if (k != 1) d[0] = 0;
        LOW16(d[0], d[0] & 0xFFFE);
        LOW16(d[1], vrd16(a5 + sx16(d[0])));
        LOW16(d[0], d[0] + 0x4000);
        LOW16(d[2], vrd16(a5 + sx16(d[0])));
        LOW16(d[1], 0u - d[1]);
        put32(&p, d[1]); put32(&p, d[2]);
    }
    put32(&p, 0);
    a[3] = p;
    charge(41);
    regs_store(&R_);
    return RD_RTS;
}

/* FUN_00015e18: the display-list header, then the 28 roadside objects of the
 * table at ROM 0x15E38 for player 0xC40 (FUN_0001595c), then close the list */
static uint32_t rd_roadside_objects(void)
{
    uint32_t r;
    CALL(L_15862, 0x15E1C);
    set_d(0, 0); set_d(1, 0); set_d(2, 0);
    set_a(0, 0x15E38u);
    set_d(7, 0x1B);
    set_d16(6, w16(0xC40));
    charge(6);
    for (;;) {
        CALL(L_1595C, 0x15E30);
        uint16_t c = (uint16_t)(d_reg(7) - 1);
        set_d16(7, c);
        charge(1);                                      /* dbf */
        if (c == 0xFFFF) break;
        poll();
    }
    charge(1);                                          /* bra.w (backward: polls) */
    return RD_JMP(0x15876u);
}

/* ============================ 0x16598 .. 0x166C6 ============================ */

/* FUN_00016598: the camera for object 0xC40 (its record A4 = A6 + (n&7)*128):
 * the offset 0x2414.. - record +0x42.. (low words) into 0x303A/0x303C/0x303E
 * and the angles 0x2420.. into 0x301C.. (pitch + 0x8000); continue in 0x2FF92 */
static uint32_t rd_camera_from_2414(void)
{
    uint16_t d1 = (uint16_t)((w16(0xC40) & 7) << 7);
    set_d16(1, d1);
    uint32_t a4 = 0x10008000u + sx16(d1), d0 = 0;
    set_a(4, a4);
    static const uint16_t dst[3] = { 0x303A, 0x303C, 0x303E };
    for (int k = 0; k < 3; k++) {
        d0 = (w32(0x2414 + 4u * (uint32_t)k) - vrd32(a4 + 0x42 + 4u * (uint32_t)k)) & 0xFFFFu;
        w16_set(dst[k], (uint16_t)d0);
    }
    set_d(0, d0);
    w16_set(0x301C, w16(0x2420));
    w16_set(0x301E, w16(0x2422));
    w16_set(0x301E, (uint16_t)(w16(0x301E) + 0x8000));
    w16_set(0x3020, w16(0x2424));
    return 0x2FF92u;                                    /* jmp: the last of the 21 */
}

/* FUN_000165fa: the object list ROM 0x169A0[0x2426 & 7]: a count word, then
 * count+1 records drawn with FUN_0001661e + FUN_0003013e (D7 kept in A2) */
static uint32_t rd_draw_object_list(void)
{
    uint32_t r;
    uint16_t i = (uint16_t)(w16(0x2426) & 7);
    set_d16(6, i);
    uint32_t a4 = vrd32(0x169A0u + (uint32_t)i * 4u);
    set_d16(7, (uint16_t)vrd16(a4));
    set_a(4, a4 + 2);
    charge(4);
    for (;;) {
        CALL(L_1661E, 0x1660E);
        set_a(2, d_reg(7));
        charge(2);                                      /* movea, jsr */
        if ((r = rd_call(L_3013E, 0x16616))) return r;
        set_d(7, a_reg(2));
        uint16_t c = (uint16_t)(d_reg(7) - 1);
        set_d16(7, c);
        charge(2);                                      /* move.l, dbf */
        if (c == 0xFFFF) break;
        poll();
    }
    RTS();
}

/* FUN_0001661e: copy the 28-word object record at A4 into 0x2398, advance its
 * spin counter 0x23D0[D7 & 31] (+1, & 0x7FF) into the four angle words, OR
 * bit 7 of 0x240E into the flags and add -0x2410 to the four height words */
static uint32_t rd_object_record(void)
{
    uint32_t a0 = A6W(0x2398), a4 = a_reg(4);
    charge(2);
    for (int k = 0x1B; k >= 0; k--) {
        vwr16(a0, (uint16_t)vrd16(a4)); a0 += 2; a4 += 2;
        charge(2);
        set_a(0, a0); set_a(4, a4); set_d16(0, (uint16_t)(k - 1));
        if (k) poll();
    }
    a0 = A6W(0x2398);
    uint16_t d1 = (uint16_t)(d_reg(7) & 0x1F);
    uint16_t d0 = (uint16_t)((w16(0x23D0 + d1 * 2u) + 1) & 0x7FF);
    w16_set(0x23D0 + d1 * 2u, d0);
    for (uint32_t o = 0xA; o <= 0x2E; o += 0xC) vwr16(a0 + o, (uint16_t)(vrd16(a0 + o) + d0));
    uint8_t b = (uint8_t)(w16(0x240E) & 1);
    b = (uint8_t)((b >> 1) | (b << 7));                 /* ror.b #1 */
    d0 = (uint16_t)((w16(0x240E) & 1 & 0xFF00) | b);
    vwr16(a0 + 2, (uint16_t)(vrd16(a0 + 2) | d0));
    d0 = (uint16_t)(0u - w16(0x2410));
    for (uint32_t o = 0x12; o <= 0x36; o += 0xC) vwr16(a0 + o, (uint16_t)(vrd16(a0 + o) + d0));
    charge(21);
    set_a(0, a0); set_a(4, a4);
    set_d16(0, d0); set_d16(1, d1);
    return RD_RTS;
}

/* FUN_0001667e: append a 0x8000 block at priority 0xC40 with the 0x8010
 * marker and object 0x829 at the origin, and end the list */
static uint32_t rd_dl_object_829(void)
{
    uint32_t p = vrd32(G_DL_CURSOR);
    put32(&p, 0x8000);
    put32(&p, (uint32_t)w16(0xC40));
    put32(&p, 0x8010); put32(&p, 0); put32(&p, 0xFFFFFFFFu);
    put32(&p, 0x829); put32(&p, 0); put32(&p, 0); put32(&p, 0);
    put32(&p, 0x8010); put32(&p, 0xFFFFFFFFu);
    vwr32(p, 0xFFFFFFFFu);
    vwr32(G_DL_CURSOR, p);
    set_a(3, p); set_d(0, 0);
    return RD_RTS;
}

/* ============================ 0x167CE .. 0x1749C ============================ */
/* The title / attract object (screen object 3): position 0x2414/0x2418/0x241C,
 * angles 0x2420/0x2422/0x2424, a spin 0x2428, the variant 0x2426 (0..7). */

/* append the three rotation pairs (-sin, cos) of the angle words a, b, c */
static void put_rotation(uint32_t *p, uint32_t *d0, uint32_t *d1, uint32_t *d2, uint32_t a5,
                         uint16_t a, uint16_t b, uint16_t c)
{
    const uint16_t ang[3] = { a, b, c };
    for (int k = 0; k < 3; k++) {
        LOW16(*d0, ang[k]); LOW16(*d0, *d0 & 0xFFFE);
        LOW16(*d1, vrd16(a5 + sx16(*d0)));
        LOW16(*d0, *d0 + 0x4000);
        LOW16(*d2, vrd16(a5 + sx16(*d0)));
        LOW16(*d1, 0u - *d1);
        put32(p, *d1); put32(p, *d2);
    }
}

/* FUN_000167ce: append the title object as a 0x8002 block: four parts
 * (models from the per-part tables ROM 0x16978/0x16990 by variant, + ROM
 * 0x16988[n]; flags ROM 0x16998[n]) at the object's position relative to the
 * record at A6 + 0xC40*128, the first spun by 0x2428; then its shadow (model
 * ROM 0x1EEB4[variant]+4, offset by the variant's radius ROM 0x1EE2E+4 along
 * the pitch 0x2422); ends the list */
static uint32_t rd_title_object(void)
{
    regs_t R_; regs_load(&R_);
    uint32_t *d = R_.d, *a = R_.a, a5 = a_reg(5);
    uint32_t p = vrd32(G_DL_CURSOR);
    put32(&p, 0x8002);
    d[1] = 0; LOW16(d[1], w16(0xC40));
    put32(&p, d[1]);
    LOW16(d[1], d[1] << 7);
    a[4] = 0x10008000u + sx16(d[1]);
    LOW16(d[6], w16(0x2426)); LOW16(d[6], d[6] & 7);
    d[7] = 3;
    LOW16(d[5], w16(0xC40)); LOW16(d[5], d[5] & 7);
    d[1] = 0; d[2] = 0;
    charge(14);
    for (;;) {                                          /* 0x167FC: parts 3..0 */
        uint32_t n = sx16(d[7]);
        d[0] = 0;
        a[1] = vrd32(0x16978u + n * 4u);
        a[1] += sx16(vrd16(0x16990u + n * 2u));
        LOW16(d[0], vrd16(a[1] + sx16(d[6]) * 8u));
        LOW16(d[0], d[0] + vrd16(0x16988u + n * 2u));
        put32(&p, d[0]);
        d[0] = w32(0x2414) - vrd32(a[4] + 0x42); put32(&p, d[0]);
        d[0] = w32(0x2418) - vrd32(a[4] + 0x46); put32(&p, d[0]);
        d[0] = w32(0x241C) - vrd32(a[4] + 0x4A); put32(&p, d[0]);
        d[0] = 0;
        uint16_t yaw = w16(0x2420);
        charge(19);
        if ((uint16_t)d[7] == 3) { yaw = (uint16_t)(yaw + w16(0x2428)); charge(1); }
        put_rotation(&p, &d[0], &d[1], &d[2], a5, yaw, w16(0x2422), w16(0x2424));
        d[0] = 0; LOW16(d[0], vrd16(0x16998u + n * 2u));
        put32(&p, d[0]);
        charge(27);
        LOW16(d[7], d[7] - 1);
        if ((uint16_t)d[7] == 0xFFFF) break;
        a[3] = p;
        regs_store(&R_); poll();
    }
    /* the shadow */
    d[0] = d[1] = d[2] = d[3] = d[4] = 0;
    LOW16(d[0], w16(0x2422)); LOW16(d[0], 0u - d[0]); LOW16(d[0], d[0] + 0x4000); LOW16(d[0], d[0] & 0xFFFE);
    LOW16(d[3], vrd16(a5 + sx16(d[0])));
    LOW16(d[0], d[0] + 0x4000); LOW16(d[0], d[0] & 0xFFFE);
    LOW16(d[4], vrd16(a5 + sx16(d[0])));
    LOW16(d[3], 0u - d[3]);
    a[1] = 0x1EE2Eu;
    LOW16(d[0], vrd16(a[1] + 4u + sx16(d[6]) * 8u));
    d[3] = fmul15(d[0], d[3]); d[4] = fmul15(d[0], d[4]);
    d[3] = sx16(d[3]); d[4] = sx16(d[4]);
    a[1] = 0x1EEB4u;
    d[0] = 0; LOW16(d[0], vrd16(a[1] + 4u + sx16(d[6]) * 8u));
    put32(&p, d[0]);
    d[0] = w32(0x2414) + d[4] - vrd32(a[4] + 0x42); put32(&p, d[0]);
    d[0] = w32(0x2418) - vrd32(a[4] + 0x46); put32(&p, d[0]);
    d[0] = sx16(w32(0x241C) + d[3]) - vrd32(a[4] + 0x4A); put32(&p, d[0]);
    d[0] = 0;
    put_rotation(&p, &d[0], &d[1], &d[2], a5, (uint16_t)(w16(0x2420) + w16(0x2428)), w16(0x2422), w16(0x2424));
    put32(&p, 1);
    vwr32(p, 0xFFFFFFFFu);
    vwr32(G_DL_CURSOR, p);
    a[3] = p;
    charge(70);
    regs_store(&R_);
    return RD_RTS;
}

/* FUN_00016fc4: set up the title screen: palette/text (FUN_00005868,
 * 0000575a, 000058ee), screen objects 5, 6 and 4 from their descriptors
 * (ROM 0x16E5C, 0x16F7E, 0x16F7E), the title object (FUN_000172f2), and
 * FUN_00030d80 / FUN_0000b8b0 */
static uint32_t rd_title_setup(void)
{
    uint32_t r;
    CALL(L_5868, 0x16FCA); CALL(L_575A, 0x16FD0); CALL(L_58EE, 0x16FD6);
    static const struct { uint16_t obj; uint32_t desc, ret; } o[3] = {
        { 5, 0x16E5C, 0x16FE0 }, { 6, 0x16F7E, 0x16FFC }, { 4, 0x16F7E, 0x17018 } };
    for (int k = 0; k < 3; k++) {
        w16_set(0xC40, o[k].obj);
        set_a(0, o[k].desc);
        charge(2);
        CALL(L_B4E4, o[k].ret + 6); CALL(L_B56E, o[k].ret + 12); CALL(L_B6D0, o[k].ret + 18);
    }
    w16_set(0x243C, 0); w16_set(0x243E, 0);
    charge(2);
    CALL(L_172F2, 0x17036);
    vwr8(A6W(0x3056), 0);
    charge(1);
    CALL(L_30D80, 0x17040);
    w16_set(0x308C, 8);
    charge(1);
    CALL(L_B8B0, 0x1704C);
    RTS();
}

/* the title's step counter and frame counter */
#define G_TITLE_STEP  0x243C
#define G_TITLE_CNT   0x243E

/* the title object's spin wobble: 0x243E += 0x22, D0w = -cos(64 * -cos(it)) */
static uint16_t title_wobble(void)
{
    uint32_t a5 = a_reg(5);
    uint16_t v = (uint16_t)(w16(G_TITLE_CNT) + 0x22);
    w16_set(G_TITLE_CNT, v);
    v = (uint16_t)((v + 0x4000) & 0xFFFE);
    v = (uint16_t)(0u - vrd16(a5 + sx16(v)));
    v = (uint16_t)(v << 6);
    v = (uint16_t)((v + 0x4000) & 0xFFFE);
    v = (uint16_t)(0u - vrd16(a5 + sx16(v)));
    set_d16(0, v);
    charge(14);
    return v;
}

/* FUN_0001704e: the title screen's steps (0x243C & 7, word jump table at
 * ROM 0x17060): scroll the logo in (FUN_000174f2) until 0x305C reaches 0x78,
 * hold 60 frames drawing the object (FUN_00017378), scroll the rest, hold
 * again, then spin in (showing the object or the logo by the wobble's sign),
 * and the final steps with FUN_000171de */
static uint32_t rd_title_steps(void)
{
    uint32_t r;
    uint16_t i = (uint16_t)(w16(G_TITLE_STEP) & 7);
    uint16_t off = (uint16_t)vrd16(0x17060u + (uint32_t)i * 2u);
    set_d16(0, off);
    uint32_t t = 0x17060u + sx16(off);
    charge(5);                                          /* move, andi, move, nop, jmp */
    if (t < 0x1704Eu || t > 0x171A8u) return RD_JMP(t);
    poll();
    switch (t) {
    case 0x17070u:
        CALL(L_174F2, 0x17074);
        charge(3);                                      /* nop, cmpi, bcs */
        if (w16(0x305C) >= 0x78) { w16_set(G_TITLE_STEP, (uint16_t)(w16(G_TITLE_STEP) + 1)); charge(1); }
        RTS();
    case 0x17084u:
    case 0x170C8u:
        CALL(L_17378, t + 4);
        w16_set(G_TITLE_CNT, (uint16_t)(w16(G_TITLE_CNT) + 1));
        charge(4);                                      /* nop, addq, cmpi, bcs */
        if (w16(G_TITLE_CNT) >= 0x3C) {
            w16_set(G_TITLE_STEP, (uint16_t)(w16(G_TITLE_STEP) + 1));
            w16_set(G_TITLE_CNT, 0);
            charge(2);
        }
        RTS();
    case 0x170A0u: {
        CALL(L_174F2, 0x170A4);
        uint16_t d1 = (uint16_t)(w16(0x305E) - 2);
        set_d16(1, d1);
        charge(5);                                      /* nop, move, subq, cmp, bhi */
        if (d1 > w16(0x305C)) RTS();
        w16_set(G_TITLE_STEP, (uint16_t)(w16(G_TITLE_STEP) + 1));
        vwr8(A6W(0x3056), 0);
        charge(2);
        CALL(L_30D24, 0x170C0);
        w16_set(0x308C, 8);
        charge(1);
        RTS();
    }
    case 0x170E4u:
    case 0x17134u: {
        uint16_t v = title_wobble();
        uint32_t base = t == 0x170E4u ? 0x17112u : 0x17162u;
        if (!(v & 0x8000)) CALL(L_171AA, base + 10);
        else { CALL(L_17378, base + 4); charge(1); }   /* + bra */
        if (t == 0x170E4u) {
            uint16_t d1 = (uint16_t)(w16(0x305E) - 7);
            set_d16(1, d1);
            charge(4);                                  /* move, subq, cmp, bhi */
            if (d1 > w16(0x305C)) RTS();
            w16_set(G_TITLE_STEP, (uint16_t)(w16(G_TITLE_STEP) + 1));
            charge(1);
            CALL(L_286F2, 0x17132);
            RTS();
        }
        CALL(L_B788, 0x17172);
        charge(2);                                      /* cmpi, bne */
        if (w16(0x233E) != 0) RTS();
        w16_set(G_TITLE_STEP, (uint16_t)(w16(G_TITLE_STEP) + 1));
        charge(1);
        CALL(L_28632, 0x17184);
        RTS();
    }
    case 0x17186u:
        CALL(L_171DE, 0x1718A); CALL(L_B7CC, 0x17190);
        charge(2);                                      /* cmpi, bne */
        if (w16(0x233E) != 0) RTS();
        CALL(L_F728, 0x1719E);
        w16_set(G_TITLE_STEP, (uint16_t)(w16(G_TITLE_STEP) + 1));
        charge(1);
        RTS();
    default:                                            /* 0x171A4 */
        CALL(L_171DE, 0x171A8);
        RTS();
    }
}

/* FUN_000171aa / FUN_000174f2 (identical code): object 5 -- clamp the scroll
 * 0x305C to 0x305E - 2 -- FUN_00030e8c, FUN_0000b56e; then object 6 with the
 * logo block (FUN_00017250) and FUN_0000b56e. `at` = the function's address. */
static uint32_t title_logo_scroll(uint32_t at)
{
    uint32_t r;
    w16_set(0xC40, 5);
    uint16_t d1 = (uint16_t)(w16(0x305E) - 2);
    set_d16(1, d1);
    charge(5);                                          /* move, move, subq, cmp, bhi */
    if (!(d1 > w16(0x305C))) { w16_set(0x305C, d1); charge(1); }
    CALL(L_30E8C, at + 0x1C); CALL(L_B56E, at + 0x22);
    w16_set(0xC40, 6);
    charge(1);
    CALL(L_17250, at + 0x2C); CALL(L_B56E, at + 0x32);
    RTS();
}
static uint32_t rd_title_logo_171aa(void) { return title_logo_scroll(0x171AAu); }
static uint32_t rd_title_logo_174f2(void) { return title_logo_scroll(0x174F2u); }

/* FUN_000171de: object 4 = the 0x730 block (FUN_000171f0), then FUN_0000b56e */
static uint32_t rd_title_obj4(void)
{
    uint32_t r;
    w16_set(0xC40, 4);
    charge(1);
    CALL(L_171F0, 0x171E8); CALL(L_B56E, 0x171EE);
    RTS();
}

/* append `model` at (0, 0, 0x15A0) unrotated as a block of type `type` at
 * priority 0xC40, and end the list; D1 = the priority, A3 = the end */
static void dl_single_model(uint32_t type, uint32_t model)
{
    uint32_t p = vrd32(G_DL_CURSOR);
    put32(&p, type);
    uint32_t d1 = (uint32_t)w16(0xC40);
    put32(&p, d1);
    put32(&p, model); put32(&p, 0); put32(&p, 0); put32(&p, 0x15A0); put32(&p, 0);
    for (int k = 0; k < 3; k++) { put32(&p, 0x8001); put32(&p, 0); }
    vwr32(p, 0xFFFFFFFFu);
    vwr32(G_DL_CURSOR, p);
    set_d(1, d1); set_a(3, p);
}

/* FUN_000171f0: object 0x730 at (0, 0, 0x15A0), as a 0x8001 block */
static uint32_t rd_dl_model_730(void) { dl_single_model(0x8001, 0x730); return RD_RTS; }

/* FUN_00017250: the logo model ROM 0x172C0[(0x2034 >> 2) & 15] (none when
 * negative) at (0, 0, 0x15A0), as a 0x8002 block */
static uint32_t rd_dl_logo(void)
{
    uint32_t d0 = (uint16_t)vrd16(0x172C0u + ((w16(0x2034) >> 2) & 0xFu) * 2u);
    set_d(0, d0);
    charge(7);                                          /* moveq, move, lsr, andi, move, nop, bmi */
    if (d0 & 0x8000) RTS();
    dl_single_model(0x8002, d0);
    charge(18);
    RTS();
}

/* FUN_000172f2: set up the title object (screen object 3, descriptor ROM
 * 0x16F7E): position (0, 0x7530, 0x1B6F), pitch 0x8000, variant = byte
 * 0x10001061 & 7, height 0x20, spin 0, wobble phases from 0x2034, and
 * FUN_0002ff64(0x2AAA, 0x280); 0x240E = 1 */
static uint32_t rd_title_object_setup(void)
{
    uint32_t r;
    w16_set(0xC40, 3);
    set_a(0, 0x16F7Eu);
    charge(2);
    CALL(L_B4E4, 0x17302); CALL(L_B56E, 0x17308); CALL(L_B6D0, 0x1730E);
    w32_set(0x2414, 0); w32_set(0x2418, 0x7530); w32_set(0x241C, 0x1B6F);
    w16_set(0x2420, 0); w16_set(0x2422, 0x8000); w16_set(0x2424, 0);
    w16_set(0x2412, 1);
    uint32_t d6 = d_reg(6);
    LOW8(d6, vrd8(0x10001061u)); LOW16(d6, d6 & 7);
    set_d(6, d6);
    w16_set(0x2426, (uint16_t)d6);
    w16_set(0x2410, 0x20);
    w16_set(0x2428, 0);
    uint16_t ph = w16(0x2034);
    w16_set(0x242A, ph); w16_set(0x242C, ph);
    set_d16(0, 0x2AAA); set_d16(1, 0x280);
    charge(17);
    CALL(L_2FF64, 0x17370);
    w16_set(0x240E, 1);
    charge(1);
    RTS();
}

/* FUN_00017378: draw the title object (screen object 3): camera
 * (FUN_00016598), its object list, the 0x829 marker, the ground strip,
 * the parts, FUN_0000b56e, then advance it (FUN_0001739e) */
static uint32_t rd_title_object_draw(void)
{
    uint32_t r;
    w16_set(0xC40, 3);
    charge(1);
    CALL(L_16598, 0x17382); CALL(L_165FA, 0x17386); CALL(L_1667E, 0x1738A);
    CALL(L_17436, 0x1738E); CALL(L_167CE, 0x17392); CALL(L_B56E, 0x17398);
    CALL(L_1739E, 0x1739C);
    RTS();
}

/* FUN_0001739e: advance the title object: its record (A1) gets y = 0x7800
 * and z moving by -0x200 (wrapping at -0x7080); position z = that + 0x1800 +
 * a wobble, x = a second wobble, the spin 0x2428 -= 0x1300 and the height
 * 0x2410 = 0x58 - cos(phase) >> 11 */
static uint32_t rd_title_object_step(void)
{
    uint32_t a5 = a_reg(5);
    uint16_t d1 = (uint16_t)(w16(0xC40) << 7);
    set_d16(1, d1);
    uint32_t a1 = 0x10008000u + sx16(d1);
    set_a(1, a1);
    vwr32(a1 + 0x46, 0x7800);
    uint32_t d0 = vrd32(a1 + 0x4A) - 0x200;
    if (!((int32_t)d0 > -0x7080)) { d0 = 0; charge(1); }
    vwr32(a1 + 0x4A, d0);
    w32_set(0x241C, d0);
    LOW16(d0, w16(0x242C)); LOW16(d0, d0 & 0xFFFE);
    LOW16(d0, vrd16(a5 + sx16(d0)));
    LOW16(d0, 0u - d0); LOW16(d0, (uint16_t)((int16_t)d0 >> 4));
    d0 = sx16(d0) + 0x1800;
    w32_set(0x241C, w32(0x241C) + d0);
    LOW16(d0, 0x80);
    w16_set(0x242C, (uint16_t)(w16(0x242C) + 0x80));
    LOW16(d0, w16(0x242A)); LOW16(d0, d0 & 0xFFFE);
    LOW16(d0, vrd16(a5 + sx16(d0)));
    LOW16(d0, 0u - d0); LOW16(d0, (uint16_t)((int16_t)d0 >> 4));
    d0 = sx16(d0);
    w32_set(0x2414, d0);
    w16_set(0x242A, (uint16_t)(w16(0x242A) + 0xC0));
    w16_set(0x2428, (uint16_t)(w16(0x2428) - 0x1300));
    LOW16(d0, w16(0x242C)); LOW16(d0, d0 & 0xFFFE); LOW16(d0, d0 + 0x4000);
    LOW16(d0, vrd16(a5 + sx16(d0)));
    LOW16(d0, (uint16_t)((int16_t)d0 >> 8)); LOW16(d0, (uint16_t)((int16_t)d0 >> 3));
    LOW16(d0, 0u - d0); LOW16(d0, d0 + 0x58);
    w16_set(0x2410, (uint16_t)d0);
    vwr16(a1, 1);
    set_d(0, d0);
    return RD_RTS;
}

/* FUN_00017436: the ground strip under the title object: eight 0x85F tiles
 * every 0x7080 along z, starting at the record's z rounded down to 0x7080,
 * as a 0x8000 block; ends the list */
static uint32_t rd_title_ground(void)
{
    uint32_t p = vrd32(G_DL_CURSOR);
    put32(&p, 0x8000);
    uint32_t d1 = (uint32_t)w16(0xC40);
    put32(&p, d1);
    LOW16(d1, d1 << 7);
    uint32_t a1 = 0x10008000u + sx16(d1);
    d1 = vrd32(a1 + 0x4A);
    d1 = (uint32_t)SDIVREM((int32_t)d1, 0x7080, '/');
    d1 &= 0xFFFFu;
    d1 = muls_w(d1, 0x7080);
    uint32_t d7 = d_reg(7), d0 = 0;
    charge(13);
    for (int k = 7; k >= 0; k--) {
        put32(&p, 0x85F);
        d0 = 0u - vrd32(a1 + 0x42); put32(&p, d0);
        d0 = 0u - vrd32(a1 + 0x46); put32(&p, d0);
        d0 = d1 - vrd32(a1 + 0x4A); put32(&p, d0);
        d1 += 0x7080;
        charge(12);
        set_a(3, p); set_a(1, a1);
        set_d(0, d0); set_d(1, d1);
        set_d(7, (d7 & 0xFFFF0000u) | (uint16_t)(k - 1));
        if (k) poll();
    }
    vwr32(p, 0xFFFFFFFFu);
    vwr32(G_DL_CURSOR, p);
    set_a(3, p); set_a(1, a1);
    set_d(0, d0); set_d(1, d1);
    set_d(7, (d7 & 0xFFFF0000u) | 0xFFFFu);
    charge(3);
    return RD_RTS;
}

/* ============================ 0x176D6: the game-mode frame ============================ */
static const callee_t seq_176d6_0a[] = {
    { L_B89A, 0x17710 }, { L_180F4, 0x17714 }, { L_5868, 0x1771A }, { L_575A, 0x17720 },
    { L_55F2, 0x17726 }, { L_58EE, 0x1772C }, { L_5706, 0x17732 }, { L_2147A, 0x17738 },
};
static const callee_t seq_176d6_0b[] = {
    { L_B4E4, 0x17748 }, { L_B56E, 0x1774E }, { L_B6D0, 0x17754 }, { L_1B0C2, 0x1775A },
    { L_1B09C, 0x17760 }, { L_1460C, 0x17766 }, { L_1DFC6, 0x1776C }, { L_184E2, 0x17772 },
    { L_1B2F0, 0x17776 }, { L_1FE0C, 0x1777C }, { L_205F4, 0x17782 }, { L_257C2, 0x17788 },
    { L_211DE, 0x1778E }, { L_2132A, 0x17794 }, { L_2089A, 0x1779A }, { L_17DEC, 0x177A0 },
    { L_1F6D4, 0x177A6 }, { L_1E292, 0x177AC }, { L_121BC, 0x177B2 }, { L_1E6AA, 0x177B8 },
};
static const callee_t seq_176d6_1[] = {
    { L_E2B0, 0x1783A }, { L_1ECE4, 0x1783E }, { L_1800E, 0x17842 }, { L_17E7C, 0x17846 },
    { L_220AA, 0x1784C }, { L_17B3C, 0x17850 }, { L_1F1DA, 0x17854 }, { L_1FF4E, 0x1785A },
    { L_2077C, 0x17860 }, { L_17B64, 0x17864 }, { L_145C4, 0x17868 }, { L_146AE, 0x1786C },
    { L_B56E, 0x17872 }, { L_14EB6, 0x17878 }, { L_121FC, 0x1787E }, { L_1E6B8, 0x17882 },
    { L_17DEA, 0x17886 }, { L_27334, 0x1788C }, { L_18046, 0x17890 }, { L_1F6D4, 0x17894 },
    { L_1BA32, 0x17898 }, { L_1CB34, 0x1789C }, { L_1DB4E, 0x178A0 },
};
static const callee_t seq_176d6_2[] = {
    { L_1228C, 0x1790C }, { L_17F4A, 0x17910 }, { L_E2B0, 0x17916 }, { L_1ECE4, 0x1791A },
    { L_1800E, 0x1791E }, { L_17DEC, 0x17922 }, { L_17E7C, 0x17926 }, { L_19DAC, 0x1792A },
    { L_18156, 0x1792E }, { L_17F80, 0x17932 }, { L_17FAE, 0x17936 }, { L_1BE5C, 0x1793C },
    { L_220AA, 0x17942 }, { L_143B6, 0x17946 }, { L_17E0E, 0x1794A }, { L_17E2C, 0x1794E },
    { L_14872, 0x17952 }, { L_17B3C, 0x17956 }, { L_17D54, 0x1795A }, { L_17B64, 0x1795E },
    { L_17E7C, 0x17962 }, { L_180DA, 0x17966 }, { L_1F1DA, 0x1796C }, { L_1FF4E, 0x17972 },
    { L_204E2, 0x17978 }, { L_1FE38, 0x1797E }, { L_2077C, 0x17984 }, { L_20AA4, 0x1798A },
    { L_20C36, 0x17990 }, { L_20E36, 0x17996 }, { L_211E0, 0x1799C }, { L_2131A, 0x179A2 },
    { L_2133E, 0x179A8 }, { L_21360, 0x179AE }, { L_157D4, 0x179B4 }, { L_1D3F8, 0x179BA },
    { L_1D8D0, 0x179C0 }, { L_1DAAA, 0x179C6 }, { L_1AE7E, 0x179CA }, { L_145C4, 0x179CE },
    { L_146AE, 0x179D2 }, { L_B56E, 0x179D8 }, { L_1E006, 0x179DC }, { L_14EB6, 0x179E2 },
    { L_121FC, 0x179E8 }, { L_1E6B8, 0x179EC }, { L_1E2B4, 0x179F0 }, { L_1E5C4, 0x179F4 },
    { L_1811A, 0x179F8 }, { L_18046, 0x179FC }, { L_1E578, 0x17A00 }, { L_180BA, 0x17A04 },
    { L_17DEA, 0x17A08 }, { L_13EA0, 0x17A0E }, { L_27334, 0x17A14 }, { L_316A2, 0x17A1A },
    { L_1818C, 0x17A1E },
};
static const callee_t seq_176d6_3[] = {
    { L_1800E, 0x17A24 }, { L_17E7C, 0x17A28 }, { L_17B64, 0x17A2C }, { L_17E7C, 0x17A32 },
    { L_1F1DA, 0x17A38 }, { L_1FE38, 0x17A3E }, { L_2077C, 0x17A44 }, { L_20AA4, 0x17A4A },
    { L_20C36, 0x17A50 }, { L_145C4, 0x17A54 }, { L_146AE, 0x17A58 }, { L_B56E, 0x17A5E },
    { L_14EB6, 0x17A62 }, { L_27334, 0x17A68 }, { L_17DEA, 0x17A6C },
};
static const callee_t seq_176d6_4[] = {
    { L_1800E, 0x17A8A }, { L_17E7C, 0x17A8E }, { L_19DAC, 0x17A92 }, { L_1BE5C, 0x17A98 },
    { L_22088, 0x17A9E }, { L_143B6, 0x17AA2 }, { L_17E0E, 0x17AA6 }, { L_17E2C, 0x17AAA },
    { L_14872, 0x17AAE }, { L_17B3C, 0x17AB2 }, { L_17D54, 0x17AB6 }, { L_17B64, 0x17ABA },
    { L_1E006, 0x17ABE }, { L_17E7C, 0x17AC2 }, { L_180DA, 0x17AC6 }, { L_1F1DA, 0x17ACA },
    { L_1FF4E, 0x17AD0 }, { L_204E2, 0x17AD6 }, { L_1FE38, 0x17ADC }, { L_2077C, 0x17AE2 },
    { L_20AA4, 0x17AE8 }, { L_20C36, 0x17AEE }, { L_2131A, 0x17AF4 }, { L_2133E, 0x17AFA },
    { L_21360, 0x17B00 }, { L_157D4, 0x17B06 }, { L_1D3F8, 0x17B0C }, { L_1D8D0, 0x17B12 },
    { L_1DAAA, 0x17B18 }, { L_145C4, 0x17B1C }, { L_146AE, 0x17B20 }, { L_B56E, 0x17B26 },
    { L_14EB6, 0x17B2C }, { L_27334, 0x17B32 }, { L_1811A, 0x17B36 }, { L_17DEA, 0x17B3A },
};


#define G_MODE_STEP 0x49CE   /* word: step of the current game mode (caseD_3 dispatches on its low 3 bits) */

/* caseD_3 (0x176D6): one frame of the race mode, by step 0x49CE & 7 (jump
 * table at ROM 0x176EA):
 *   0: set up the race -- sound, palette, text, the player's screen object
 *      (descriptor ROM 0x13908) and every subsystem's init; 240-frame count
 *      0x49C8; step 1
 *   1: the start countdown (a run of per-frame updates); 0x412A counts while
 *      0x4014 is in 0x2B40..0x2C3F; when 0x49C8 runs out, step 2 (0x4022 =
 *      0x600 if still in that range)
 *   2: the race itself (the full per-frame update list)
 *   3: the finish (a reduced list)
 *   4-7: the attract / replay run of the race */
static uint32_t rd_race_frame(void)
{
    uint32_t r;
    uint16_t i = (uint16_t)(w16(G_MODE_STEP) & 7);
    uint32_t off = vrd32(0x176EAu + (uint32_t)i * 4u);
    set_d(0, off);
    uint32_t t = 0x176EAu + off;
    charge(6);                                          /* move, andi, move.l, nop, nop, jmp */
    if (t != 0x1770Au && t != 0x17810u && t != 0x178E6u && t != 0x17A20u && t != 0x17A6Eu) return RD_JMP(t);
    poll();
    switch (t) {
    case 0x1770Au:
        CALLS(seq_176d6_0a);
        w16_set(0xC40, 0);
        set_a(0, 0x13908u);
        charge(2);
        CALLS(seq_176d6_0b);
        w16_set(0x49C8, 0xF0);
        charge(1);
        CALL(L_F9CA, 0x177C4);
        w16_set(G_MODE_STEP, (uint16_t)(w16(G_MODE_STEP) + 1));
        charge(1);
        RTS();
    case 0x17810u: {
        CALL(L_B7CC, 0x17816);
        w16_set(0xC40, 0);
        w16_set(0x20E8, w16(0x2046)); w16_set(0x20D8, w16(0x204C));
        w16_set(0x42B8, 0); w16_set(0x42BA, 0);
        charge(5);
        CALLS(seq_176d6_1);
        uint16_t d0 = (uint16_t)(w16(0x4014) - 0x2B40);
        set_d16(0, d0);
        charge(4);                                      /* move, subi, cmpi, bcc */
        if (d0 < 0x100) {
            d0 = (uint16_t)(w16(0x412A) + 1);
            set_d16(0, d0);
            charge(4);                                  /* move, addq, cmpi, bcc */
            if (d0 < 0xA0) { w16_set(0x412A, d0); charge(1); }
        }
        CALL(L_EA92, 0x178C6);
        w16_set(0x49C8, (uint16_t)(w16(0x49C8) - 1));
        charge(2);                                      /* subq, bne */
        if (w16(0x49C8) != 0) RTS();
        d0 = (uint16_t)(w16(0x4014) - 0x2B40);
        set_d16(0, d0);
        charge(4);
        if (d0 < 0x100) { w16_set(0x4022, 0x600); charge(1); }
        w16_set(G_MODE_STEP, (uint16_t)(w16(G_MODE_STEP) + 1));
        charge(1);
        RTS();
    }
    case 0x178E6u:
        w16_set(0xC40, 0);
        w16_set(0x20E8, w16(0x2046)); w16_set(0x20D8, w16(0x204C));
        w32_set(0x4640, 0); w16_set(0x4644, 0); w16_set(0x4646, 0); w16_set(0x4648, 0);
        charge(7);
        CALLS(seq_176d6_2);
        RTS();
    case 0x17A20u:
        CALLS(seq_176d6_3);
        RTS();
    default:                                            /* 0x17A6E */
        w16_set(0x49BA, 0xFFFF);
        w16_set(0xC40, 0);
        w16_set(0x20E8, w16(0x2046)); w16_set(0x20D8, w16(0x204C));
        charge(4);
        CALLS(seq_176d6_4);
        RTS();
    }
}

/* ============================ 0x17B3C .. 0x17FAC ============================ */

/* FUN_00017b3c: FUN_00017c52, the three FUN_0001dxxx updates, the pitch
 * smoothing FUN_00017c82; heading 0x20A0 = -0x4088; continue in FUN_00017cf4 */
static uint32_t rd_17b3c(void)
{
    uint32_t r;
    CALL(L_17C52, 0x17B40); CALL(L_1DEDA, 0x17B46); CALL(L_1DD82, 0x17B4C); CALL(L_1DF6A, 0x17B52);
    CALL(L_17C82, 0x17B56);
    uint16_t v = (uint16_t)(0u - w16(0x4088));
    set_d16(0, v);
    w16_set(0x20A0, v);
    charge(4);                                          /* move, neg, move, bra */
    return 0x17CF4u;
}

/* FUN_00017b64: unless 0x46E0 is set (then continue at 0x24818), player 0
 * takes the car's position/angles (0x208C.. -> 0x20F8.., 0x209E.. ->
 * 0x2188..), the yaw plus 0x42B8 and a steering term 0x41E8 (/32 when
 * 0x4332 is 1), the roll plus 0x42BA; its record (A6 + 0xC40*128) is marked */
static uint32_t rd_player0_from_car(void)
{
    uint32_t d0 = d_reg(0);
    LOW16(d0, w16(0x46E0));
    set_d(0, d0);
    charge(2);                                          /* move, beq */
    if ((uint16_t)d0 != 0) { charge(1); return 0x24818u; }
    LOW16(d0, w16(0xC40)); LOW16(d0, d0 << 7);
    uint32_t a1 = 0x10008000u + sx16(d0);
    w16_set(0x20F8, w16(0x208C)); w16_set(0x2108, w16(0x208E)); w16_set(0x2118, w16(0x2090));
    w16_set(0x2188, w16(0x209E)); w16_set(0x2198, w16(0x20A0)); w16_set(0x21A8, w16(0x20A2));
    LOW16(d0, w16(0x42B8));
    uint32_t d1 = d_reg(1);
    LOW16(d1, w16(0x41E8));
    charge(13);
    if (w16(0x4332) == 1) { LOW16(d1, (uint16_t)((int16_t)d1 >> 5)); charge(1); }
    LOW16(d0, d0 + d1);
    w16_set(0x2188, (uint16_t)(w16(0x2188) + d0));
    LOW16(d0, w16(0x42BA));
    w16_set(0x21A8, (uint16_t)(w16(0x21A8) + d0));
    vwr16(a1, 1);
    charge(6);
    set_d(0, d0); set_d(1, d1); set_a(1, a1);
    return RD_RTS;
}

/* the course's cross-section at the current point 0x204C: two words from the
 * course tables (+off1, +off2) projected along the angle `ang` (sine table
 * at A5); result = the difference to `cur`, as the 68K computes it */
static void bank_step(regs_t *R_, uint32_t off1, uint32_t off2, uint16_t add1, uint16_t ang, uint32_t curaddr)
{
    uint32_t *d = R_->d, *a = R_->a, a5 = a_reg(5);
    a[0] = COURSE_RECS;
    LOW16(d[7], w16(0x2046)); LOW16(d[7], d[7] << 6);
    uint32_t rec = COURSE_RECS + sx16(d[7]);
    a[2] = vrd32(rec + off1); a[3] = vrd32(rec + off2);
    LOW16(d[3], w16(0x204C));
    LOW16(d[5], vrd16(a[2] + sx16(d[3]) * 2u));
    LOW16(d[4], vrd16(a[3] + sx16(d[3]) * 2u));
    if (add1) LOW16(d[5], d[5] + w16(add1));
    LOW16(d[0], ang); LOW16(d[0], d[0] & 0xFFFE);
    LOW16(d[1], vrd16(a5 + sx16(d[0])));
    LOW16(d[0], d[0] + 0x4000);
    LOW16(d[2], vrd16(a5 + sx16(d[0])));
    LOW16(d[1], 0u - d[1]);
    d[2] = fmul15(d[5], d[2]);
    LOW16(d[2], 0u - d[2]);
    d[1] = fmul15(d[4], d[1]);
    LOW16(d[2], d[2] - d[1]);
    LOW16(d[2], d[2] - vrd16(curaddr));
    charge(add1 ? 26 : 25);
    if ((uint16_t)d[2] != 0) {                          /* move a quarter of the way (at least 1 up) */
        LOW16(d[2], (uint16_t)((int16_t)d[2] >> 2));
        charge(2);
        if (!((int16_t)d[2] < 0)) { LOW16(d[2], d[2] + 1); charge(1); }
        vwr16(curaddr, (uint16_t)(vrd16(curaddr) + d[2]));
        charge(1);
    }
}

/* FUN_00017c82: FUN_00017dca, then smooth the pitch 0x20AE toward the
 * course's bank at the car's heading relative to the track (tables +0x18 /
 * +0x1C); 0x47CC = 0x20AE - the car's pitch 0x209E (saved in 0x483C) */
static uint32_t rd_smooth_pitch(void)
{
    uint32_t r;
    CALL(L_17DCA, 0x17C86);
    w16_set(0x483C, w16(0x209E));
    charge(1);
    regs_t R_; regs_load(&R_);
    bank_step(&R_, 0x18, 0x1C, 0, (uint16_t)(w16(0x21D6) - w16(0x20A0)), A6W(0x20AE));
    LOW16(R_.d[0], w16(0x20AE)); LOW16(R_.d[0], R_.d[0] - w16(0x483C));
    w16_set(0x47CC, (uint16_t)R_.d[0]);
    charge(4);
    regs_store(&R_);
    return RD_RTS;
}

/* FUN_00017cf4: smooth the roll 0x20A2 the same way (tables +0x1C / +0x18,
 * the first plus 0x21EC, the angle heading - track heading) */
static uint32_t rd_smooth_roll(void)
{
    regs_t R_; regs_load(&R_);
    bank_step(&R_, 0x1C, 0x18, 0x21EC, (uint16_t)(w16(0x20A0) - w16(0x21D6)), A6W(0x20A2));
    charge(1);
    regs_store(&R_);
    return RD_RTS;
}

/* FUN_00017d54: engine sway: 0x42BA moves 1/8 toward 1.25*0x40A4 scaled by
 * the speed 0x4022 (bits 9..15); the acceleration (0x4022 - last) * 0x60,
 * limited to -0x900..0x600 (none when braking below 0x800 with 0x40B4 set),
 * smoothed by 1/16 into 0x47AC and 0x42B8; 0x40D8 = the speed */
static uint32_t rd_sway(void)
{
    uint32_t d0 = d_reg(0), d1 = d_reg(1);
    LOW16(d0, w16(0x40A4));
    LOW16(d1, d0); LOW16(d1, (uint16_t)((int16_t)d1 >> 2)); LOW16(d0, d0 + d1);
    uint16_t v = w16(0x4022);
    LOW16(d1, (uint16_t)((v << 7) | (v >> 9))); LOW16(d1, d1 & 0x7F);
    d1 = muls_w(d0, d1);
    LOW16(d1, d1 - w16(0x42BA)); LOW16(d1, (uint16_t)((int16_t)d1 >> 3));
    w16_set(0x42BA, (uint16_t)(w16(0x42BA) + d1));
    LOW16(d0, w16(0x4022)); LOW16(d0, d0 - w16(0x40D8));
    d0 = muls_w(d0, 0x60);
    charge(15);
    if (!((int32_t)d0 < 0)) {
        charge(2);                                      /* cmpi, bcs */
        if ((uint16_t)d0 >= 0x600) { LOW16(d0, 0x600); charge(2); }
    } else {
        charge(2);                                      /* cmpi, bcc */
        if ((uint16_t)d0 < 0xF700) { LOW16(d0, 0xF700); charge(1); }
    }
    charge(2);                                          /* tst, bpl */
    if ((int16_t)d0 < 0) {
        charge(2);                                      /* cmpi, bge */
        if ((int16_t)w16(0x4002) < 0x800) {
            charge(2);                                  /* cmpi, beq */
            if (w16(0x40B4) != 0) { LOW16(d0, 0); charge(1); }
        }
    }
    LOW16(d0, d0 - w16(0x47AC)); LOW16(d0, (uint16_t)((int16_t)d0 >> 4)); LOW16(d0, d0 + w16(0x47AC));
    w16_set(0x47AC, (uint16_t)d0);
    w16_set(0x42B8, (uint16_t)d0);
    w16_set(0x40D8, w16(0x4022));
    charge(7);
    set_d(0, d0); set_d(1, d1);
    return RD_RTS;
}

/* FUN_00017e2c: wrong-way detection: 0x4868 = -1 while the heading is more
 * than 90 degrees off the track; going backwards past a point starts the
 * 60-frame count 0x486A; 0x4866 = -1 while it runs; 0x48DE = the point */
static uint32_t rd_wrong_way(void)
{
    uint32_t d1 = 0, d0 = d_reg(0);
    w16_set(0x4868, 0);
    LOW16(d0, w16(0x2198)); LOW16(d0, d0 - w16(0x21D6));
    charge(5);
    if ((int16_t)d0 < 0) { LOW16(d0, 0u - d0); charge(1); }
    charge(2);                                          /* cmpi, bcs */
    if ((uint16_t)d0 >= 0x4000) {
        w16_set(0x4868, 0xFFFF);
        LOW16(d0, w16(0x204C));
        charge(4);                                      /* move, move, cmp, bcc */
        if ((uint16_t)d0 < w16(0x48DE)) { w16_set(0x486A, 0x3C); charge(1); }
    }
    charge(2);
    if (w16(0x486A) != 0) { LOW16(d1, 0xFFFF); charge(1); }
    charge(2);
    if (w16(0x486A) != 0) { w16_set(0x486A, (uint16_t)(w16(0x486A) - 1)); charge(1); }
    w16_set(0x4866, (uint16_t)d1);
    w16_set(0x48DE, w16(0x204C));
    charge(3);
    set_d(0, d0); set_d(1, d1);
    return RD_RTS;
}

/* FUN_00017e7c: record the car's state for replay/network in the 64-byte
 * slot 0x11A0 (slot 0 when 0x1100 is set): course, time, point, position,
 * angles (negated), pitch, sway, 0x47BC, the view yaw, and add the speed to
 * two odometer words */
static uint32_t rd_record_car_state(void)
{
    uint32_t d6 = d_reg(6), d0 = d_reg(0);
    LOW16(d6, 0);
    charge(3);                                          /* move, tst, bne */
    if (w16(0x1100) == 0) { LOW16(d6, w16(0x11A0)); LOW16(d6, d6 << 6); charge(2); }
    uint32_t x = sx16(d6);
    w16_set(0x4AAA + x, w16(0x2046));
    w16_set(0x16AC + x, w16(0x204A));
    w16_set(0x16AE + x, w16(0x204C));
    w16_set(0x4AB8 + x, w16(0x20A6));
    w16_set(0x16B0 + x, w16(0x208C));
    LOW16(d0, w16(0x208E) - 0x18); w16_set(0x16B2 + x, (uint16_t)d0);
    w16_set(0x16B4 + x, w16(0x2090));
    LOW16(d0, 0u - w16(0x209E)); w16_set(0x16B6 + x, (uint16_t)d0);
    LOW16(d0, 0u - w16(0x20A0)); LOW16(d0, d0 + 0x8000); w16_set(0x16B8 + x, (uint16_t)d0);
    LOW16(d0, 0u - w16(0x20A2)); w16_set(0x16BA + x, (uint16_t)d0);
    w16_set(0x16BC + x, w16(0x20AC));
    w16_set(0x16BC + x, (uint16_t)(w16(0x16BC + x) - 0x18));
    LOW16(d0, 0u - w16(0x20AE)); w16_set(0x16C6 + x, (uint16_t)d0);
    LOW16(d0, (uint16_t)((int16_t)w16(0x42BA) >> 1)); w16_set(0x4AD8 + x, (uint16_t)d0);
    w32_set(0x4ACA + x, w32(0x47BC));
    LOW16(d0, w16(0xDD2) << 3); LOW16(d0, d0 + w16(0x20A0)); LOW16(d0, 0u - d0); LOW16(d0, d0 + 0x8000);
    w16_set(0x16DC + x, (uint16_t)d0);
    LOW16(d0, w16(0x4022));
    w16_set(0x16D6 + x, (uint16_t)(w16(0x16D6 + x) + d0));
    w16_set(0x16D8 + x, (uint16_t)(w16(0x16D8 + x) + d0));
    charge(38);
    set_d(0, d0); set_d(6, d6);
    return RD_RTS;
}

/* FUN_00017f4a: 0x2088 = a speed figure from 0x4022 * 0x478A (x 0.66, then
 * x 10/16 when byte 0x1000106D is set) */
static uint32_t rd_speed_2088(void)
{
    uint32_t d0 = d_reg(0), d1 = d_reg(1);
    LOW16(d0, w16(0x4022));
    d0 = (uint32_t)(uint16_t)d0 * (uint32_t)w16(0x478A);
    d0 <<= 2; d0 = swap32(d0);
    LOW16(d1, d0);
    LOW16(d0, (uint16_t)((int16_t)d0 >> 3)); LOW16(d1, d1 - d0);
    LOW16(d0, (uint16_t)((int16_t)d0 >> 3)); LOW16(d1, d1 + d0);
    LOW16(d1, (uint16_t)((int16_t)d1 >> 4));
    LOW16(d0, d1); LOW16(d0, (uint16_t)((int16_t)d0 >> 1)); LOW16(d1, d1 - d0);
    w16_set(0x2088, (uint16_t)d1);
    charge(16);
    if (vrd8(0x1000106Du) != 0) {
        d1 = muls_w(d1, 0xA);
        LOW16(d1, (uint16_t)d1 >> 4);
        w16_set(0x2088, (uint16_t)d1);
        charge(3);
    }
    set_d(0, d0); set_d(1, d1);
    return RD_RTS;
}

/* FUN_00017f80: the view yaw 0xDD2: its change since last frame (0x48CC),
 * and how much its magnitude grew (0x48D0, 0 when it shrank) */
static uint32_t rd_view_yaw_delta(void)
{
    uint32_t d0 = d_reg(0), d1 = d_reg(1);
    LOW16(d0, w16(0xDD2)); LOW16(d1, d0);
    LOW16(d0, d0 - w16(0x48CA));
    w16_set(0x48CC, (uint16_t)d0);
    w16_set(0x48CA, (uint16_t)d1);
    LOW16(d0, w16(0xDD2));
    charge(7);
    if ((int16_t)d0 < 0) { LOW16(d0, 0u - d0); charge(1); }
    LOW16(d1, d0); LOW16(d0, d0 - w16(0x48CE));
    charge(3);
    if ((int16_t)d0 < 0) { LOW16(d0, 0); charge(1); }
    w16_set(0x48D0, (uint16_t)d0);
    w16_set(0x48CE, (uint16_t)d1);
    charge(2);
    set_d(0, d0); set_d(1, d1);
    return RD_RTS;
}

/* ============================ 0x18046 .. 0x186B2 ============================ */

/* FUN_00018046: the race position 0x204E = 1 + the number of cars ahead:
 * linked (0x1100): the 0x4A96+1 recorded slots from 0x4A94 (64 bytes each)
 * whose time word 0x16AC is later, or equal with a later point; otherwise
 * the matching enabled cabinets (0x11AE[n] == 0x11A2, bit clear in 0x487E)
 * whose long time 0x16AC[n*64] is not below ours */
static uint32_t rd_race_position(void)
{
    regs_t R_; regs_load(&R_);
    uint32_t *d = R_.d;
    charge(2);                                          /* tst, beq */
    if (w16(0x1100) != 0) {
        LOW16(d[6], w16(0x4A94)); LOW16(d[7], w16(0x4A96));
        d[5] = 1;
        charge(3);
        for (;;) {
            LOW16(d[0], w16(0x204A));
            uint32_t x = sx16(d[6]);
            int16_t t = (int16_t)w16(0x16AC + x);
            charge(3);                                  /* move, cmp, blt */
            int ahead = 0;
            if ((int16_t)d[0] < t) ahead = 1;
            else {
                charge(1);                              /* bgt */
                if ((int16_t)d[0] == t) {
                    LOW16(d[0], w16(0x204C));
                    charge(3);                          /* move, cmp, bgt */
                    if (!((int16_t)d[0] > (int16_t)w16(0x16AE + x))) ahead = 1;
                }
            }
            if (ahead) { LOW16(d[5], d[5] + 1); charge(1); }
            LOW16(d[6], d[6] + 0x40);
            charge(2);                                  /* addi, dbf */
            LOW16(d[7], d[7] - 1);
            if ((uint16_t)d[7] == 0xFFFF) break;
            regs_store(&R_); poll();
        }
    } else {
        d[5] = 0; LOW16(d[7], 7);
        charge(2);
        for (;;) {
            charge(2);                                  /* btst, bne */
            int ahead = 0;
            if (vrd8(A6W(0x487E)) & (1u << (d[7] & 7))) ahead = 1;
            else {
                LOW16(d[3], d[7]); LOW16(d[4], d[7]);
                LOW16(d[3], d[3] << 4); LOW16(d[4], d[4] << 6);
                LOW16(d[0], w16(0x11AE + sx16(d[3])));
                charge(7);                              /* move, move, lsl, lsl, move, cmp, bne */
                if ((uint16_t)d[0] == w16(0x11A2)) {
                    d[0] = w32(0x204A);
                    charge(3);                          /* move.l, cmp.l, bgt */
                    if (!((int32_t)d[0] > (int32_t)w32(0x16AC + sx16(d[4])))) ahead = 1;
                }
            }
            if (ahead) { LOW16(d[5], d[5] + 1); charge(1); }
            charge(1);                                  /* dbf */
            LOW16(d[7], d[7] - 1);
            if ((uint16_t)d[7] == 0xFFFF) break;
            regs_store(&R_); poll();
        }
    }
    w16_set(0x204E, (uint16_t)d[5]);
    charge(2);
    regs_store(&R_);
    return RD_RTS;
}

/* FUN_000180ba: add the distance sqrt(0x40A6^2 + 0x40A8^2) (FUN_00018452) to
 * the odometer long at 0x2056 + 0x48EE */
static uint32_t rd_odometer(void)
{
    uint32_t r;
    set_a(0, A6W(0x2056) + sx16(w16(0x48EE)));
    uint32_t d0 = d_reg(0), d3 = d_reg(3);
    LOW16(d0, w16(0x40A6)); LOW16(d3, w16(0x40A8));
    d0 = muls_w(d0, d0); d3 = muls_w(d3, d3);
    d3 += d0;
    set_d(0, d0); set_d(3, d3);
    charge(8);
    CALL(L_18452, 0x180D6);
    uint32_t a0 = a_reg(0);
    vwr32(a0, vrd32(a0) + d_reg(2));
    charge(1);
    RTS();
}

/* FUN_00018156: when not linked, step 0x478C by 1 toward the target for our
 * race position (ROM 0x1DD3C[0x204E - 1], a quarter on selection 3) */
static uint32_t rd_step_478c(void)
{
    charge(2);                                          /* tst, bne */
    if (w16(0x1100) != 0) RTS();
    uint32_t d3 = d_reg(3);
    LOW16(d3, w16(0x204E));
    d3 -= 1;
    set_a(1, 0x1DD3Cu);
    LOW16(d3, vrd16(0x1DD3Cu + sx16(d3) * 2u));
    charge(6);
    if (w16(G_SEL_2342) == 3) { LOW16(d3, (uint16_t)d3 >> 2); charge(1); }
    uint32_t d1 = 1;
    set_d(3, d3);
    charge(3);                                          /* moveq, cmp, beq */
    if ((uint16_t)d3 == w16(0x478C)) { set_d(1, d1); RTS(); }
    charge(1);                                          /* bgt */
    if (!((int16_t)d3 > (int16_t)w16(0x478C))) { LOW16(d1, 0u - d1); charge(1); }
    w16_set(0x478C, (uint16_t)(w16(0x478C) + d1));
    set_d(1, d1);
    charge(1);
    RTS();
}

/* FUN_0001818c: the finish fade: while the finish flag 0x466C[slot] is set,
 * 0x44EA counts up (to 0xA0 on course 0, 0x180 otherwise) and 0x44EC counts
 * frames (to 0xF0); the mixer levels at 0x90020011/13/15 move toward
 * 0x100 + 0x44EA (1/2 step, or toward 0x100 at 1/8 at first / 1/64 after 60) */
static uint32_t rd_finish_fade(void)
{
    uint32_t d0 = d_reg(0), d1 = d_reg(1), d2, d3 = d_reg(3), d6 = 0;
    charge(3);                                          /* moveq, tst, bne */
    if (w16(0x1100) == 0) { LOW16(d6, w16(0x11A0)); charge(1); }
    LOW16(d0, w16(0x466C + sx16(d6) * 2u));
    charge(2);                                          /* move, beq */
    if ((uint16_t)d0 != 0) {
        w16_set(0x44EC, 0);
        LOW16(d1, 0x180);
        charge(4);                                      /* clr, move, cmpi, bne */
        if (w16(0x2046) == 0) { LOW16(d1, 0xA0); charge(1); }
        LOW16(d0, w16(0x44EA)); LOW16(d0, d0 + 1);
        charge(4);                                      /* move, addq, cmp, bcc */
        if ((uint16_t)d0 < (uint16_t)d1) { w16_set(0x44EA, (uint16_t)d0); charge(1); }
    }
    d2 = 0x100; LOW16(d3, 3);
    LOW16(d0, w16(0x44EC));
    charge(4);                                          /* move.l, move, move, beq */
    if ((uint16_t)d0 != 0) {
        d2 = 0; LOW16(d2, w16(0x44EA)); LOW16(d2, d2 + 0x100);
        LOW16(d3, 1);
        charge(6);                                      /* moveq, move, addi, move, cmpi, blt */
        if (!((int16_t)d0 < 0x3C)) { d2 = 0x100; w16_set(0x44EA, 0); LOW16(d3, 6); charge(3); }
    }
    d1 = 0; LOW16(d1, vrd16(0x90020011u));
    d1 -= d2;
    charge(4);                                          /* moveq, move, sub, beq */
    if (d1 != 0) {
        unsigned n = d3 & 63;                           /* asr.l D3,D1 */
        d1 = (uint32_t)((int32_t)d1 >> (n > 31 ? 31 : n));
        charge(2);                                      /* asr, bmi */
        if (!((int32_t)d1 < 0)) { LOW16(d1, d1 + 1); charge(1); }
        vwr16(0x90020011u, (uint16_t)(vrd16(0x90020011u) - d1));
        vwr16(0x90020015u, (uint16_t)(vrd16(0x90020015u) - d1));
        vwr16(0x90020013u, (uint16_t)(vrd16(0x90020013u) - d1));
        charge(3);
    }
    LOW16(d0, w16(0x44EC)); LOW16(d0, d0 + 1);
    charge(4);                                          /* move, addq, cmpi, bcc */
    if ((uint16_t)d0 < 0xF0) { w16_set(0x44EC, (uint16_t)d0); charge(1); }
    set_d(0, d0); set_d(1, d1); set_d(2, d2); set_d(3, d3); set_d(6, d6);
    RTS();
}

/* FUN_00018452: D2 = the integer square root of D3 (two passes of eight
 * 2-bit digit steps, the high then the low word); D4-D6 are saved on the
 * stack and restored, D3 is left shifted as the 68K leaves it */
static uint32_t rd_isqrt(void)
{
    /* movem.l {D6 D5 D4},-(SP): the saved registers live on the 68K stack
     * (D4 at the lowest address), and the registers stay live at every poll */
    uint32_t sp = a_reg(7) - 12;
    vwr32(sp, d_reg(4)); vwr32(sp + 4, d_reg(5)); vwr32(sp + 8, d_reg(6));
    set_a(7, sp);
    uint32_t d3 = d_reg(3), d2 = 0, d5 = 0;
    uint32_t d4 = swap32((d_reg(4) & 0xFFFF0000u) | (d3 & 0xFFFFu));
    set_d(4, d4); set_d(6, 7); set_d(5, 0); set_d(2, 0);
    charge(6);                                          /* movem, move, swap, moveq x3 */
    for (int pass = 0; pass < 2; pass++) {
        uint32_t *src = pass ? &d4 : &d3;
        int borrow = 0;
        if (pass) { set_d(6, 7); charge(1); }           /* moveq #7 */
        for (int k = 7; k >= 0; k--) {
            d5 <<= 2;
            *src &= 0xFFFF0000u;
            *src = (*src << 2) | (*src >> 30);
            d5 = (d5 & 0xFFFF0000u) | (uint16_t)(d5 | *src);
            d2 += d2;
            LOW16(d2, d2 + 1);
            borrow = d5 < d2;
            d5 -= d2;
            charge(8);                                  /* lsl, clr, rol, or, add, addq, sub, bcs */
            if (borrow) { d5 += d2; d2 -= 1; charge(3); }
            else { d2 += 1; charge(2); }
            set_d(2, d2); set_d(3, d3); set_d(4, d4); set_d(5, d5);
            set_d16(6, (uint16_t)(k - 1));
            if (k) poll();
        }
        if (!pass && !borrow) charge(1);                /* bra 0x18480 */
    }
    d2 >>= 1;
    set_d(2, d2); set_d(3, d3);
    set_d(4, vrd32(sp)); set_d(5, vrd32(sp + 4)); set_d(6, vrd32(sp + 8));
    set_a(7, sp + 12);
    charge(3);                                          /* lsr, movem, rts */
    return RD_RTS;
}

/* FUN_000184e2: set up the linked-race car slots: clear the slot tables
 * (FUN_000186c6); linked (0x1100): 64-byte slots from 0x40, count from ROM
 * 0x188DC by selection and 0x484A, the drone table 0x4742 from ROM 0x1881C
 * by selection and 0xC4C bits 2-3, FUN_00018702, continue at 0x1B3CA;
 * otherwise find our cabinet among the eight 0x11AE slots (0x1164), and
 * when we are the master (== 0x11A0) and slot 8 or 12 of 0x1104 is free,
 * claim it: count the matching cabinets (ROM 0x186B4), FUN_00018802, the
 * drone table ROM 0x1889C, mark the claimed slots and continue in
 * FUN_00018702; else clear the linked state */
static uint32_t rd_link_setup(void)
{
    uint32_t r;
    regs_t R_;
    CALL(L_186C6, 0x184E6);
    regs_load(&R_);
    uint32_t *d = R_.d, *a = R_.a;
    charge(2);                                          /* tst, beq */
    if (w16(0x1100) != 0) {
        LOW16(d[0], w16(0xC4C)); LOW16(d[0], d[0] & 0xC);
        w16_set(0x474A, (uint16_t)d[0]);
        LOW16(d[0], (uint16_t)d[0] >> 2);
        a[2] = 0x1881Cu;
        LOW16(d[5], w16(G_SEL_2342)); LOW16(d[5], d[5] << 4); LOW16(d[5], d[5] + w16(0x474A));
        w32_set(0x4742, vrd32(a[2] + sx16(d[5])));
        w16_set(0x4A94, 0x40);
        a[2] = 0x188DCu;
        LOW16(d[5], w16(G_SEL_2342)); LOW16(d[5], d[5] << 3); LOW16(d[5], d[5] + w16(0x484A));
        w16_set(0x4A96, (uint16_t)vrd16(a[2] + sx16(d[5]) * 2u));
        w16_set(0x1162, 0);
        charge(16);
        regs_store(&R_);
        CALL(L_18702, 0x18532);
        charge(1);                                      /* bra.w */
        return 0x1B3CAu;
    }
    LOW16(d[7], 7); LOW16(d[6], 0); LOW16(d[5], 0);
    charge(3);
    for (;;) {                                          /* which slot is ours */
        LOW16(d[0], w16(0x11AE + sx16(d[6])));
        charge(3);                                      /* move, cmp, bne */
        if ((uint16_t)d[0] == w16(0x11A2)) { LOW16(d[5], d[6]); charge(1); }
        LOW16(d[6], d[6] + 0x10);
        charge(2);                                      /* addi, dbf */
        LOW16(d[7], d[7] - 1);
        if ((uint16_t)d[7] == 0xFFFF) break;
        regs_store(&R_); poll();
    }
    LOW16(d[5], (uint16_t)d[5] >> 4);
    w16_set(0x1164, (uint16_t)d[5]);
    charge(4);                                          /* lsr, move, cmp, bne */
    int claim = 0;
    if ((uint16_t)d[5] == w16(0x11A0)) {
        LOW16(d[1], 8);
        LOW16(d[0], w16(0x1104));
        charge(4);                                      /* move, move, btst, beq */
        if (!(d[0] & (1u << 8))) claim = 1;
        else {
            LOW16(d[1], 0xC);
            charge(3);                                  /* move, btst, bne */
            if (!(d[0] & (1u << 12))) claim = 1;
        }
    }
    if (!claim) {
        w16_set(0x1162, 0); w16_set(0x4A94, 0); w16_set(0x1168, 0); w16_set(0x4A96, 0);
        charge(4);
        regs_store(&R_);
        RTS();
    }
    w16_set(0x1168, (uint16_t)d[1]);
    LOW16(d[1], d[1] << 6);
    w16_set(0x4A94, (uint16_t)d[1]);
    LOW16(d[5], 0); LOW16(d[6], 0); LOW16(d[7], 7);
    charge(6);
    for (;;) {                                          /* how many cabinets match */
        LOW16(d[0], w16(0x11AE + sx16(d[6])));
        charge(3);
        if ((uint16_t)d[0] == w16(0x11A2)) { LOW16(d[5], d[5] + 1); charge(1); }
        LOW16(d[6], d[6] + 0x10);
        charge(2);
        LOW16(d[7], d[7] - 1);
        if ((uint16_t)d[7] == 0xFFFF) break;
        regs_store(&R_); poll();
    }
    a[0] = 0x186B4u;
    w16_set(0x4A96, (uint16_t)vrd16(a[0] + sx16(d[5]) * 2u));
    charge(2);
    regs_store(&R_);
    CALL(L_18802, 0x18660);
    regs_load(&R_);
    LOW16(d[0], w16(0xC4C)); LOW16(d[0], d[0] & 0xC);
    w16_set(0x474A, (uint16_t)d[0]);
    a[2] = 0x1889Cu;
    LOW16(d[5], w16(G_SEL_2342)); LOW16(d[5], d[5] << 4); LOW16(d[5], d[5] + w16(0x474A));
    w32_set(0x4742, vrd32(a[2] + sx16(d[5])));
    LOW16(d[6], w16(0x4A94)); LOW16(d[6], (uint16_t)d[6] >> 2);
    LOW16(d[7], w16(0x4A96));
    LOW16(d[0], w16(0x11A2));
    charge(12);
    for (;;) {                                          /* claim the slots */
        w16_set(0x11AE + sx16(d[6]), (uint16_t)d[0]);
        LOW16(d[6], d[6] + 0x10);
        charge(3);
        LOW16(d[7], d[7] - 1);
        if ((uint16_t)d[7] == 0xFFFF) break;
        regs_store(&R_); poll();
    }
    regs_store(&R_);
    charge(1);                                          /* bra.w */
    return 0x18702u;
}

/* FUN_00018558: mark finished drones: our progress key (point >> 4 | time
 * << 12 -- the leader's among the eight recorded slots when not linked);
 * every drone slot (from 0x4A94, 0x4A96+1 of them, 16-byte records at
 * 0x4742) whose finish key +0xC is not above it gets bit 7 of 0x16C4 set */
static uint32_t rd_mark_finished(void)
{
    regs_t R_; regs_load(&R_);
    uint32_t *d = R_.d;
    charge(2);                                          /* tst, bne */
    uint32_t base;
    if (w16(0x1100) == 0) {
        LOW16(d[7], 7); d[5] = 0; d[6] = 0;
        charge(3);
        for (;;) {                                      /* the leader: latest time */
            LOW16(d[0], d[7]); LOW16(d[0], d[0] << 6);
            uint32_t x = sx16(d[0]);
            charge(4);                                  /* move, lsl, btst, beq */
            if (vrd8(A6W(0x16C4) + x) & 0x80) {
                charge(2);                              /* cmp.l, bge */
                if ((int32_t)d[6] < (int32_t)w32(0x16AC + x)) { d[6] = w32(0x16AC + x); LOW16(d[5], d[7]); charge(2); }
            }
            charge(1);                                  /* dbf */
            LOW16(d[7], d[7] - 1);
            if ((uint16_t)d[7] == 0xFFFF) break;
            regs_store(&R_); poll();
        }
        LOW16(d[5], d[5] << 6);
        base = A6W(0x16AC) + sx16(d[5]);
        charge(7);
    } else {
        base = A6W(0x204A);
        charge(5);
    }
    LOW16(d[0], vrd16(base + 2)); LOW16(d[1], vrd16(base));
    LOW16(d[0], (uint16_t)d[0] >> 4);
    { uint16_t v = (uint16_t)d[1]; LOW16(d[1], (uint16_t)((v >> 4) | (v << 12))); }
    LOW16(d[0], d[0] | d[1]);
    LOW16(d[5], d[0]);
    uint32_t a0 = w32(0x4742);
    LOW16(d[6], w16(0x4A94)); LOW16(d[7], w16(0x4A96));
    charge(4);
    for (;;) {
        uint32_t f = A6W(0x16C4) + sx16(d[6]);
        charge(2);                                      /* btst, bne */
        if (!(vrd8(f) & 0x80)) {
            charge(2);                                  /* cmp, blt */
            if (!((int16_t)d[5] < (int16_t)vrd16(a0 + 0xC))) { vwr8(f, (uint8_t)(vrd8(f) | 0x80)); charge(1); }
        }
        a0 += 0x10;
        LOW16(d[6], d[6] + 0x40);
        charge(3);                                      /* adda, addi, dbf */
        LOW16(d[7], d[7] - 1);
        if ((uint16_t)d[7] == 0xFFFF) break;
        R_.a[0] = a0;
        regs_store(&R_); poll();
    }
    R_.a[0] = a0;
    regs_store(&R_);
    RTS();
}

/* FUN_000186c6: clear the slot tables at 0x4AAA and (falling into
 * FUN_000186d2) 0x16AC -- 1 KB each */
static uint32_t rd_clear_slot_tables(void)
{
    uint32_t r;
    set_a(0, A6W(0x4AAA));
    charge(1);
    CALL(L_186D2, 0x186CE);
    set_a(0, A6W(0x16AC));
    charge(1);
    return 0x186D2u;                                    /* falls into FUN_000186d2 */
}

/* FUN_000186d2: clear 1 KB at A0 (16 x 16 longs); A0 ends past it */
static uint32_t rd_clear_1k(void)
{
    uint32_t a0 = a_reg(0);
    charge(3);
    for (int k = 15; k >= 0; k--) {
        for (int i = 0; i < 16; i++) { vwr32(a0, 0); a0 += 4; }
        charge(17);
        set_a(0, a0); set_d(0, 0); set_d16(6, 0); set_d16(7, (uint16_t)(k - 1));
        if (k) poll();
    }
    set_a(0, a0); set_d(0, 0); set_d16(6, 0); set_d16(7, 0xFFFF);
    RTS();
}

/* FUN_00018702: fill the drone slots (from 0x4A94, 0x4A96+1 of them) from
 * the 16-byte records at 0x4742: parameters into 0x4AAE.., the 'done' bit
 * when +0xC is 0 (tested on slot D4 -- as the 68K does), state 0x2100; then
 * their course (ROM 0x18538[0x2046 & 15]) and start point +0xE (time in the
 * top nibble): position and heading from the course tables */
static uint32_t rd_drone_slots(void)
{
    regs_t R_; regs_load(&R_);
    uint32_t *d = R_.d, *a = R_.a;
    a[3] = w32(0x4742);
    LOW16(d[6], w16(0x4A94)); LOW16(d[7], w16(0x4A96));
    charge(3);
    for (;;) {
        uint32_t x = sx16(d[6]);
        w16_set(0x4AAE + x, (uint16_t)vrd16(a[3]));
        w16_set(0x4AB0 + x, (uint16_t)vrd16(a[3] + 2));
        w16_set(0x4ADE + x, (uint16_t)vrd16(a[3] + 4));
        w16_set(0x4ADC + x, (uint16_t)vrd16(a[3] + 6));
        w16_set(0x4AE0 + x, (uint16_t)vrd16(a[3] + 8));
        w16_set(0x4AE2 + x, (uint16_t)vrd16(a[3] + 10));
        charge(8);                                      /* 6 moves, tst, bne */
        (void)vrd16(a[3] + 0xC);                        /* the lifted tst.w reads it twice */
        if (vrd16(a[3] + 0xC) == 0) { bset8(0x16C4 + sx16(d[4]), 7); charge(1); }
        w16_set(0x16C2 + x, 0); w16_set(0x4AB4 + x, 0); w16_set(0x16C0 + x, 0x2100);
        LOW16(d[6], d[6] + 0x40);
        a[3] += 0x10;
        charge(6);
        LOW16(d[7], d[7] - 1);
        if ((uint16_t)d[7] == 0xFFFF) break;
        regs_store(&R_); poll();
    }
    a[0] = COURSE_RECS;
    a[1] = 0x18538u;
    LOW16(d[5], w16(0x2046)); LOW16(d[5], d[5] & 0xF);
    LOW16(d[5], vrd16(a[1] + sx16(d[5]) * 2u));
    LOW16(d[1], d[5]);
    LOW16(d[5], d[5] << 6);
    uint32_t rec = COURSE_RECS + sx16(d[5]);
    a[1] = vrd32(rec + 0xC); a[2] = vrd32(rec + 0x10); a[3] = vrd32(rec + 0x14);
    LOW16(d[4], w16(0x4A94));
    a[4] = w32(0x4742);
    LOW16(d[6], w16(0x4A96));
    charge(13);
    for (;;) {
        uint32_t x = sx16(d[4]);
        w16_set(0x4AAA + x, (uint16_t)d[1]);
        LOW16(d[0], vrd16(a[4] + 0xE));
        LOW16(d[2], d[0]);
        { uint16_t v = (uint16_t)d[2]; LOW16(d[2], (uint16_t)((v << 4) | (v >> 12))); }
        LOW16(d[2], d[2] & 0xF);
        w16_set(0x16AC + x, (uint16_t)d[2]);
        LOW16(d[0], d[0] & 0xFFF); LOW16(d[0], d[0] << 4);
        w16_set(0x16AE + x, (uint16_t)d[0]);
        uint32_t i = sx16(d[0]);
        w16_set(0x16B0 + x, (uint16_t)vrd16(a[1] + i * 4u));
        w16_set(0x16B4 + x, (uint16_t)vrd16(a[1] + 2u + i * 4u));
        w16_set(0x16B2 + x, (uint16_t)vrd16(a[2] + i * 2u));
        LOW16(d[0], vrd16(a[3] + i * 2u));
        LOW16(d[0], 0u - d[0]); LOW16(d[0], d[0] + 0x8000);
        w16_set(0x16CA + x, (uint16_t)d[0]);
        w16_set(0x16B8 + x, (uint16_t)d[0]);
        LOW16(d[4], d[4] + 0x40);
        a[4] += 0x10;
        charge(20);
        LOW16(d[6], d[6] - 1);
        if ((uint16_t)d[6] == 0xFFFF) break;
        regs_store(&R_); poll();
    }
    regs_store(&R_);
    RTS();
}

/* ============================ 0x19DAC: the drone cars ============================ */
static const callee_t seq_19dac[] = {
    { L_1A102, 0x19DB8 }, { L_1A49E, 0x19DBC }, { L_1A2AA, 0x19DC0 }, { L_1A26C, 0x19DC4 },
    { L_1A22E, 0x19DC8 }, { L_1A2FA, 0x19DCC }, { L_1A22C, 0x19DD0 }, { L_1A64C, 0x19DD4 },
    { L_1A752, 0x19DD8 }, { L_1A034, 0x19DDC }, { L_1A8D0, 0x19DE0 }, { L_1A3D2, 0x19DE4 },
    { L_1A42C, 0x19DE8 }, { L_19FB4, 0x19DEC }, { L_18558, 0x19DF0 }, { L_1ABDA, 0x19DF4 },
    { L_1ADCE, 0x19DF8 }, { L_1A1E0, 0x19DFC }, { L_19F80, 0x19E00 }, { L_19E0C, 0x19E04 },
    { L_19EB0, 0x19E08 },
};


/* FUN_00019dac: when there are drone cars (0x4A94), run their whole update
 * list, then continue in FUN_0001a07e */
static uint32_t rd_drones_update(void)
{
    uint32_t r;
    charge(2);                                          /* tst, beq */
    if (w16(0x4A94) == 0) RTS();
    CALLS(seq_19dac);
    charge(1);                                          /* bra.w */
    return 0x1A07Eu;
}

/* The drone cars: 64-byte slots from 0x4A94 (count 0x4A96 + 1), fields at
 * 0x16AC.. and 0x4AAA.. + slot; bit 7 of 0x16C4 = active, bit 0 = ours to
 * drive when linked (0x1100). */
#define DRONE_ACTIVE(x) (vrd8(A6W(0x16C4) + (x)) & 0x80)

/* the per-drone gate most loops share: active, and (when linked) ours */
static int drone_gate(uint32_t x)
{
    charge(2);                                          /* btst #7, beq */
    if (!DRONE_ACTIVE(x)) return 0;
    charge(2);                                          /* tst 0x1100, beq */
    if (w16(0x1100) == 0) return 1;
    charge(2);                                          /* btst #0, beq */
    return (vrd8(A6W(0x16C4) + x) & 1) != 0;
}

/* FUN_00019e0c: each drone's target heading: the course point 20 ahead,
 * offset sideways by the drone's lane 0x4ABA along the track normal; the
 * offset to it (0x4ABC/0x4ABE) and its heading (FUN_0000a98c) into 0x4AAC */
static uint32_t rd_drone_target(void)
{
    uint32_t r;
    regs_t R_; regs_load(&R_);
    uint32_t *d = R_.d, *a = R_.a, a5 = a_reg(5);
    a[0] = COURSE_RECS;
    LOW16(d[6], w16(0x4A94)); LOW16(d[7], w16(0x4A96));
    charge(3);
    for (;;) {
        uint32_t x = sx16(d[6]);
        LOW16(d[0], w16(0x4AAA + x)); LOW16(d[0], d[0] << 6);
        uint32_t rec = a[0] + sx16(d[0]);
        a[4] = vrd32(rec + 0xC); a[3] = vrd32(rec + 0x14);
        LOW16(d[5], w16(0x16AE + x)); LOW16(d[5], d[5] + 0x14);
        LOW16(d[0], vrd16(a[3] + sx16(d[5]) * 2u));
        LOW16(d[0], 0u - d[0]); LOW16(d[0], d[0] + 0x4000); LOW16(d[0], d[0] & 0xFFFE);
        LOW16(d[1], vrd16(a5 + sx16(d[0])));
        LOW16(d[0], d[0] + 0x4000);
        LOW16(d[2], vrd16(a5 + sx16(d[0])));
        LOW16(d[1], 0u - d[1]);
        LOW16(d[0], w16(0x4ABA + x));
        d[1] = fmul15(d[0], d[1]); d[2] = fmul15(d[0], d[2]);
        LOW16(d[1], d[1] + vrd16(a[4] + sx16(d[5]) * 4u)); LOW16(d[1], d[1] - w16(0x16B0 + x));
        w16_set(0x4ABC + x, (uint16_t)d[1]);
        LOW16(d[2], d[2] + vrd16(a[4] + 2u + sx16(d[5]) * 4u)); LOW16(d[2], d[2] - w16(0x16B4 + x));
        w16_set(0x4ABE + x, (uint16_t)d[2]);
        charge(29);
        int zero = 0;
        if ((uint16_t)d[1] == 0) {
            charge(2);
            if ((uint16_t)d[2] == 0) { d[0] = 0; charge(2); zero = 1; }
        }
        if (!zero) {
            regs_store(&R_);
            CALL(L_A98C, 0x19E94);
            regs_load(&R_);
            LOW16(d[0], 0u - d[0]); LOW16(d[0], d[0] + 0x4000);
            charge(2);
        }
        LOW16(d[0], 0u - d[0]); LOW16(d[0], d[0] + 0x8000);
        w16_set(0x4AAC + sx16(d[6]), (uint16_t)d[0]);
        LOW16(d[6], d[6] + 0x40);
        charge(5);
        LOW16(d[7], d[7] - 1);
        if ((uint16_t)d[7] == 0xFFFF) break;
        regs_store(&R_); poll();
    }
    regs_store(&R_);
    RTS();
}

/* FUN_00019eb0: each drone's roll and speed: the roll 0x16B8/0x16D4 eases
 * toward the course bank +0x1A ahead; the speed 0x16C2 steps by 0x4AB0 -
 * 0x4AB2 - 1 (just -1 once above 0x4AAE + 0x4AB4 (+0x180 with 0x4AB6)),
 * not below 0; the heading 0x16CA eases 1/16 toward 0x4AAC; the roll term
 * 0x16D4 loses 0xE4 and is limited to 0x1400. A0 = the course records. */
static uint32_t rd_drone_speed(void)
{
    regs_t R_; regs_load(&R_);
    uint32_t *d = R_.d, *a = R_.a;
    LOW16(d[4], w16(0x4A94)); LOW16(d[7], w16(0x4A96));
    charge(2);
    for (;;) {
        uint32_t x = sx16(d[4]);
        LOW16(d[0], w16(0x4AAA + x)); LOW16(d[0], d[0] << 6);
        a[1] = vrd32(a[0] + sx16(d[0]) + 0x14u);
        LOW16(d[0], w16(0x16AE + x)); LOW16(d[0], d[0] + 0x1A);
        LOW16(d[0], vrd16(a[1] + sx16(d[0]) * 2u));
        LOW16(d[0], 0u - d[0]); LOW16(d[0], d[0] + 0x8000);
        LOW16(d[0], d[0] + w16(0x16D4 + x)); LOW16(d[0], d[0] - w16(0x16B8 + x));
        LOW16(d[0], (uint16_t)((int16_t)d[0] >> 5));
        w16_set(0x16B8 + x, (uint16_t)(w16(0x16B8 + x) + d[0]));
        w16_set(0x16D4 + x, (uint16_t)(w16(0x16D4 + x) + d[0]));
        w16_set(0x16D4 + x, (uint16_t)(w16(0x16D4 + x) + d[0]));
        d[1] = 0xFFFFFFFFu;
        LOW16(d[3], w16(0x4AAE + x)); LOW16(d[3], d[3] + w16(0x4AB4 + x));
        charge(19);
        if (w16(0x4AB6 + x) != 0) { LOW16(d[3], d[3] + 0x180); charge(1); }
        charge(2);                                      /* cmp, bcs */
        if (!((uint16_t)d[3] < w16(0x16C2 + x))) {
            LOW16(d[1], d[1] + w16(0x4AB0 + x)); LOW16(d[1], d[1] - w16(0x4AB2 + x));
            charge(2);
        }
        uint16_t sp = (uint16_t)(w16(0x16C2 + x) + d[1]);
        w16_set(0x16C2 + x, sp);
        charge(2);                                      /* add, bpl */
        if (sp & 0x8000) { w16_set(0x16C2 + x, 0); charge(1); }
        LOW16(d[0], w16(0x4AAC + x)); LOW16(d[0], d[0] - w16(0x16CA + x));
        LOW16(d[0], (uint16_t)((int16_t)d[0] >> 4));
        w16_set(0x16CA + x, (uint16_t)(w16(0x16CA + x) + d[0]));
        LOW16(d[1], 0xE4);
        LOW16(d[0], w16(0x16D4 + x));
        charge(7);
        if ((int16_t)d[0] < 0) { LOW16(d[0], 0u - d[0]); charge(1); }
        LOW16(d[0], d[0] - d[1]);
        charge(2);
        if ((int16_t)d[0] < 0) { LOW16(d[0], 0); charge(1); }
        charge(2);                                      /* cmpi, bcs */
        if ((uint16_t)d[0] >= 0x1400) { LOW16(d[0], 0x1400); charge(1); }
        charge(2);                                      /* tst, bpl */
        if ((int16_t)w16(0x16D4 + x) < 0) { LOW16(d[0], 0u - d[0]); charge(1); }
        w16_set(0x16D4 + x, (uint16_t)d[0]);
        LOW16(d[4], d[4] + 0x40);
        charge(3);
        LOW16(d[7], d[7] - 1);
        if ((uint16_t)d[7] == 0xFFFF) break;
        regs_store(&R_); poll();
    }
    regs_store(&R_);
    RTS();
}

/* FUN_00019fb4: each active drone that is ours: 0x4AB2 = the largest change
 * of the course's heading table over the next 33 points / 11 (a braking
 * figure) once its speed 0x16C2 is 0xD00 or more, else 0 */
static uint32_t rd_drone_brake(void)
{
    regs_t R_; regs_load(&R_);
    uint32_t *d = R_.d, *a = R_.a;
    a[0] = COURSE_RECS;
    LOW16(d[0], w16(0x2046)); LOW16(d[0], d[0] << 6);
    a[1] = vrd32(COURSE_RECS + sx16(d[0]) + 0x14u);
    LOW16(d[6], w16(0x4A94)); LOW16(d[7], w16(0x4A96));
    charge(6);
    for (;;) {
        uint32_t x = sx16(d[6]);
        w16_set(0x4AB2 + x, 0);
        charge(3);                                      /* move, btst, beq */
        if (DRONE_ACTIVE(x)) {
            charge(2);                                  /* btst, beq */
            if (vrd8(A6W(0x16C4) + x) & 1) {
                LOW16(d[4], 0);
                charge(3);                              /* move, cmpi, blt */
                if (!((int16_t)w16(0x16C2 + x) < 0xD00)) {
                    d[4] = 0; LOW16(d[5], 0x20);
                    charge(2);
                    for (;;) {
                        LOW16(d[2], d[5]); LOW16(d[2], d[2] + w16(0x16AE + x));
                        uint32_t e = a[1] + sx16(d[2]) * 2u;
                        LOW16(d[3], vrd16(e)); LOW16(d[3], d[3] - vrd16(e + 2));
                        charge(5);
                        if ((int16_t)d[3] < 0) { LOW16(d[3], 0u - d[3]); charge(1); }
                        charge(2);                      /* cmp, bcc */
                        if ((uint16_t)d[4] < (uint16_t)d[3]) { LOW16(d[4], d[3]); charge(1); }
                        charge(1);                      /* dbf */
                        LOW16(d[5], d[5] - 1);
                        if ((uint16_t)d[5] == 0xFFFF) break;
                        regs_store(&R_); poll();
                    }
                    d[4] = sx16(d[4]);
                    divs_w(&d[4], 0xB);
                    charge(2);
                }
                w16_set(0x4AB2 + x, (uint16_t)d[4]);
                charge(1);
            }
        }
        LOW16(d[6], d[6] + 0x40);
        charge(2);
        LOW16(d[7], d[7] - 1);
        if ((uint16_t)d[7] == 0xFFFF) break;
        regs_store(&R_); poll();
    }
    regs_store(&R_);
    RTS();
}

/* shrink a word by 1/8 of itself (at least 1 when positive) */
static void decay8(regs_t *R_, uint32_t addr)
{
    uint32_t *d = R_->d;
    LOW16(d[0], vrd16(addr)); LOW16(d[1], d[0]);
    LOW16(d[0], (uint16_t)((int16_t)d[0] >> 3));
    charge(4);                                          /* move, move, asr, bmi */
    if (!((int16_t)d[0] < 0)) { LOW16(d[0], d[0] + 1); charge(1); }
    LOW16(d[1], d[1] - d[0]);
    vwr16(addr, (uint16_t)d[1]);
    charge(2);
}

/* FUN_0001a034: each active drone's push velocity 0x16D0/0x16D2 decays */
static uint32_t rd_drone_push_decay(void)
{
    regs_t R_; regs_load(&R_);
    uint32_t *d = R_.d;
    LOW16(d[4], w16(0x4A94)); LOW16(d[7], w16(0x4A96));
    charge(2);
    for (;;) {
        uint32_t x = sx16(d[4]);
        charge(2);                                      /* btst, beq */
        if (DRONE_ACTIVE(x)) { decay8(&R_, A6W(0x16D0) + x); decay8(&R_, A6W(0x16D2) + x); }
        LOW16(d[4], d[4] + 0x40);
        charge(2);
        LOW16(d[7], d[7] - 1);
        if ((uint16_t)d[7] == 0xFFFF) break;
        regs_store(&R_); poll();
    }
    regs_store(&R_);
    RTS();
}

/* FUN_0001a07e: move each active drone: speed 0x16C2 scaled by 0x478A (the
 * speed unit) along its heading 0x16CA, plus its push 0x16D0/0x16D2, into
 * its position 0x16B0/0x16B4; 0x16C0 = speed + 0x2100 */
static uint32_t rd_drone_move(void)
{
    regs_t R_; regs_load(&R_);
    uint32_t *d = R_.d, a5 = a_reg(5);
    LOW16(d[4], w16(0x4A94)); LOW16(d[7], w16(0x4A96));
    charge(2);
    for (;;) {
        uint32_t x = sx16(d[4]);
        charge(2);
        if (DRONE_ACTIVE(x)) {
            LOW16(d[0], w16(0x16CA + x)); LOW16(d[0], d[0] & 0xFFFE);
            LOW16(d[1], vrd16(a5 + sx16(d[0])));
            LOW16(d[0], d[0] + 0x4000);
            LOW16(d[2], vrd16(a5 + sx16(d[0])));
            LOW16(d[1], 0u - d[1]);
            LOW16(d[3], w16(0x16C2 + x));
            d[3] = (uint32_t)(uint16_t)d[3] * (uint32_t)w16(0x478A);
            d[3] <<= 2; d[3] = swap32(d[3]);
            LOW16(d[3], d[3] + d[3]);
            d[3] = (uint32_t)(uint16_t)d[3] * 0xD555u;
            d[3] = swap32(d[3]);
            d[1] = fmul15(d[3], d[1]);
            LOW16(d[1], d[1] + w16(0x16D0 + x)); LOW16(d[1], (uint16_t)((int16_t)d[1] >> 8));
            w16_set(0x16B0 + x, (uint16_t)(w16(0x16B0 + x) + d[1]));
            d[2] = fmul15(d[3], d[2]);
            LOW16(d[2], d[2] + w16(0x16D2 + x)); LOW16(d[2], (uint16_t)((int16_t)d[2] >> 8));
            w16_set(0x16B4 + x, (uint16_t)(w16(0x16B4 + x) + d[2]));
            LOW16(d[0], w16(0x16C2 + x)); LOW16(d[0], d[0] + 0x2100);
            w16_set(0x16C0 + x, (uint16_t)d[0]);
            charge(28);
        }
        LOW16(d[4], d[4] + 0x40);
        charge(2);
        LOW16(d[7], d[7] - 1);
        if ((uint16_t)d[7] == 0xFFFF) break;
        regs_store(&R_); poll();
    }
    regs_store(&R_);
    RTS();
}

/* FUN_0001a102: each drone's nearest course point among the next 16 from
 * its current one 0x16AE (Manhattan distance to 0x16B0/0x16B4, wrapping at
 * the course's point count, saved in 0x4766); a new point steps its lap
 * state (FUN_0001a1b6); at the end of a course with a successor (record
 * byte +1) switch to it from the ROM pair table at 0x144F2 */
static uint32_t rd_drone_nearest_point(void)
{
    uint32_t r;
    regs_t R_; regs_load(&R_);
    uint32_t *d = R_.d, *a = R_.a;
    a[0] = COURSE_RECS;
    LOW16(d[4], w16(0x4A94)); LOW16(d[7], w16(0x4A96));
    charge(3);
    for (;;) {
        uint32_t x = sx16(d[4]);
        LOW16(d[1], w16(0x4AAA + x)); LOW16(d[1], d[1] << 6);
        uint32_t rec = a[0] + sx16(d[1]);
        a[1] = vrd32(rec + 0xC);
        a[2] = rec + 1;
        LOW16(d[1], vrd16(rec + 6));
        w16_set(0x4766, (uint16_t)d[1]);
        LOW16(d[5], w16(0x16AE + x));
        LOW16(d[0], d[5]);
        d[3] = 0x7FFF;
        LOW16(d[6], 0xF);
        charge(10);
        for (;;) {
            charge(2);                                  /* cmp, bcs */
            if (!((uint16_t)d[5] < w16(0x4766))) { d[5] = 0; charge(1); }
            d[1] = 0; d[2] = 0;
            uint32_t p = a[1] + sx16(d[5]) * 4u;
            LOW16(d[1], w16(0x16B0 + x)); LOW16(d[1], d[1] - vrd16(p));
            charge(5);
            if ((int16_t)d[1] < 0) { LOW16(d[1], 0u - d[1]); charge(1); }
            LOW16(d[2], w16(0x16B4 + x)); LOW16(d[2], d[2] - vrd16(p + 2));
            charge(3);
            if ((int16_t)d[2] < 0) { LOW16(d[2], 0u - d[2]); charge(1); }
            d[1] += d[2];
            charge(3);                                  /* add, cmp, bcc */
            if (d[1] < d[3]) { d[3] = d[1]; LOW16(d[0], d[5]); charge(2); }
            d[5] += 1;
            charge(2);                                  /* addq, dbf */
            LOW16(d[6], d[6] - 1);
            if ((uint16_t)d[6] == 0xFFFF) break;
            regs_store(&R_); poll();
        }
        charge(2);                                      /* cmp, beq */
        if ((uint16_t)d[0] != w16(0x16AE + x)) {
            regs_store(&R_);
            CALL(L_1A1B6, 0x1A17E);
            regs_load(&R_);
            w16_set(0x16AE + sx16(d[4]), (uint16_t)d[0]);
            charge(1);
        }
        x = sx16(d[4]);
        charge(2);                                      /* tst.b, beq */
        if (vrd8(a[2]) != 0) {
            LOW16(d[0], d[0] + 0x10);
            charge(3);                                  /* addi, cmp, blt */
            if (!((int16_t)d[0] < (int16_t)w16(0x4766))) {
                LOW16(d[0], w16(0x4AAA + x));
                uint32_t t = sx16(d[0]) * 4u;
                w16_set(0x4AAA + x, (uint16_t)vrd16(0x144F2u + t));
                w16_set(0x16AE + x, (uint16_t)vrd16(0x144F4u + t));
                charge(3);
            }
        }
        LOW16(d[4], d[4] + 0x40);
        charge(2);
        LOW16(d[7], d[7] - 1);
        if ((uint16_t)d[7] == 0xFFFF) break;
        regs_store(&R_); poll();
    }
    regs_store(&R_);
    RTS();
}

/* FUN_0001a1e0: each drone we drive: add its speed to the two odometer words
 * 0x16D6 (as is) and 0x16D8 (at least 0xE00) */
static uint32_t rd_drone_odometer(void)
{
    uint32_t d0 = d_reg(0), d6 = d_reg(6), d7 = d_reg(7);
    LOW16(d6, w16(0x4A94)); LOW16(d7, w16(0x4A96));
    charge(2);
    for (;;) {
        uint32_t x = sx16(d6);
        if (drone_gate(x)) {
            LOW16(d0, w16(0x16C2 + x));
            w16_set(0x16D6 + x, (uint16_t)(w16(0x16D6 + x) + d0));
            charge(4);                                  /* move, add, cmpi, bcc */
            if ((uint16_t)d0 < 0xE00) { LOW16(d0, 0xE00); charge(1); }
            w16_set(0x16D8 + x, (uint16_t)(w16(0x16D8 + x) + d0));
            charge(1);
        }
        LOW16(d6, d6 + 0x40);
        charge(2);
        LOW16(d7, d7 - 1);
        if ((uint16_t)d7 == 0xFFFF) break;
        set_d(0, d0); set_d(6, d6); set_d(7, d7);
        poll();
    }
    set_d(0, d0); set_d(6, d6); set_d(7, d7);
    RTS();
}

/* FUN_0001a2aa: each active drone's height 0x16BC = the course height at its
 * point (table +0x10) + 0x4AC6 + 0x4AC8 + 14 */
static uint32_t rd_drone_height(void)
{
    regs_t R_; regs_load(&R_);
    uint32_t *d = R_.d, *a = R_.a;
    a[0] = COURSE_RECS;
    LOW16(d[4], w16(0x4A94)); LOW16(d[7], w16(0x4A96));
    charge(3);
    for (;;) {
        uint32_t x = sx16(d[4]);
        charge(2);
        if (DRONE_ACTIVE(x)) {
            LOW16(d[0], w16(0x4AAA + x)); LOW16(d[0], d[0] << 6);
            a[1] = vrd32(a[0] + sx16(d[0]) + 0x10u);
            LOW16(d[6], w16(0x16AE + x));
            LOW16(d[0], vrd16(a[1] + sx16(d[6]) * 2u));
            LOW16(d[0], d[0] + w16(0x4AC6 + x)); LOW16(d[0], d[0] + w16(0x4AC8 + x)); LOW16(d[0], d[0] + 0xE);
            w16_set(0x16BC + x, (uint16_t)d[0]);
            charge(9);
        }
        LOW16(d[4], d[4] + 0x40);
        charge(2);
        LOW16(d[7], d[7] - 1);
        if ((uint16_t)d[7] == 0xFFFF) break;
        regs_store(&R_); poll();
    }
    regs_store(&R_);
    RTS();
}

/* FUN_0001a2fa: each drone we drive: pitch 0x16C6 and roll 0x16BA = minus
 * the course's tables +0x18 / +0x1C at its point */
static uint32_t rd_drone_attitude(void)
{
    regs_t R_; regs_load(&R_);
    uint32_t *d = R_.d, *a = R_.a;
    a[0] = COURSE_RECS;
    LOW16(d[6], w16(0x4A94)); LOW16(d[7], w16(0x4A96));
    charge(3);
    for (;;) {
        uint32_t x = sx16(d[6]);
        if (drone_gate(x)) {
            LOW16(d[0], w16(0x4AAA + x)); LOW16(d[0], d[0] << 6);
            uint32_t rec = a[0] + sx16(d[0]);
            a[1] = vrd32(rec + 0x18); a[2] = vrd32(rec + 0x1C);
            LOW16(d[0], w16(0x16AE + x));
            LOW16(d[5], vrd16(a[1] + sx16(d[0]) * 2u));
            LOW16(d[4], vrd16(a[2] + sx16(d[0]) * 2u));
            LOW16(d[5], 0u - d[5]); LOW16(d[4], 0u - d[4]);
            w16_set(0x16C6 + x, (uint16_t)d[5]);
            w16_set(0x16BA + x, (uint16_t)d[4]);
            charge(11);
        }
        LOW16(d[6], d[6] + 0x40);
        charge(2);
        LOW16(d[7], d[7] - 1);
        if ((uint16_t)d[7] == 0xFFFF) break;
        regs_store(&R_); poll();
    }
    regs_store(&R_);
    RTS();
}

/* FUN_0001a3d2: each drone we drive: 0x4ADA eases 1/16 toward speed * 0x16B6
 * >> 17 (>> 18 when negative) */
static uint32_t rd_drone_4ada(void)
{
    uint32_t d0 = d_reg(0), d1 = d_reg(1), d6 = d_reg(6), d7 = d_reg(7);
    LOW16(d6, w16(0x4A94)); LOW16(d7, w16(0x4A96));
    charge(2);
    for (;;) {
        uint32_t x = sx16(d6);
        if (drone_gate(x)) {
            LOW16(d0, w16(0x16C2 + x)); LOW16(d1, w16(0x16B6 + x));
            d1 = muls_w(d0, d1); d1 = swap32(d1);
            LOW16(d1, (uint16_t)((int16_t)d1 >> 1));
            charge(6);                                  /* move, move, muls, swap, asr, bpl */
            if ((int16_t)d1 < 0) { LOW16(d1, (uint16_t)((int16_t)d1 >> 1)); charge(1); }
            LOW16(d1, d1 - w16(0x4ADA + x));
            charge(2);                                  /* sub, beq */
            if ((uint16_t)d1 != 0) {
                LOW16(d1, (uint16_t)((int16_t)d1 >> 4));
                charge(2);                              /* asr, bmi */
                if (!((int16_t)d1 < 0)) { LOW16(d1, d1 + 1); charge(1); }
                w16_set(0x4ADA + x, (uint16_t)(w16(0x4ADA + x) + d1));
                charge(1);
            }
        }
        LOW16(d6, d6 + 0x40);
        charge(2);
        LOW16(d7, d7 - 1);
        if ((uint16_t)d7 == 0xFFFF) break;
        set_d(0, d0); set_d(1, d1); set_d(6, d6); set_d(7, d7);
        poll();
    }
    set_d(0, d0); set_d(1, d1); set_d(6, d6); set_d(7, d7);
    RTS();
}

/* FUN_0001a42c: each drone we drive: its steering 0x4AD8 eases 1/32 toward
 * the heading error 0x4AAC - 0x16CA (0 while 0x4ACA is set), limited to
 * +-0x400 */
static uint32_t rd_drone_steer(void)
{
    uint32_t d0 = d_reg(0), d1 = d_reg(1), d6 = d_reg(6), d7 = d_reg(7);
    LOW16(d6, w16(0x4A94)); LOW16(d7, w16(0x4A96));
    charge(2);
    for (;;) {
        uint32_t x = sx16(d6);
        if (drone_gate(x)) {
            LOW16(d0, w16(0x4AAC + x)); LOW16(d0, d0 - w16(0x16CA + x));
            charge(4);                                  /* move, sub, tst, beq */
            if (w16(0x4ACA + x) != 0) { LOW16(d0, 0); charge(1); }
            LOW16(d1, d0);
            charge(2);                                  /* move, bpl */
            if ((int16_t)d1 < 0) { LOW16(d1, 0u - d1); charge(1); }
            charge(2);                                  /* cmpi, bcs */
            if ((uint16_t)d1 >= 0x400) {
                LOW16(d1, 0x400);
                charge(3);                              /* move, tst, bpl */
                if ((int16_t)d0 < 0) { LOW16(d1, 0u - d1); charge(1); }
                LOW16(d0, d1);
                charge(1);
            }
            LOW16(d0, d0 - w16(0x4AD8 + x));
            charge(2);                                  /* sub, beq */
            if ((uint16_t)d0 != 0) {
                LOW16(d0, (uint16_t)((int16_t)d0 >> 5));
                charge(2);
                if (!((int16_t)d0 < 0)) { LOW16(d0, d0 + 1); charge(1); }
                w16_set(0x4AD8 + x, (uint16_t)(w16(0x4AD8 + x) + d0));
                charge(1);
            }
        }
        LOW16(d6, d6 + 0x40);
        charge(2);
        LOW16(d7, d7 - 1);
        if ((uint16_t)d7 == 0xFFFF) break;
        set_d(0, d0); set_d(1, d1); set_d(6, d6); set_d(7, d7);
        poll();
    }
    set_d(0, d0); set_d(1, d1); set_d(6, d6); set_d(7, d7);
    RTS();
}

/* FUN_0001a49e: each drone we drive, its place across the track: the offset
 * from its point, the heading to it (FUN_0000a98c) against the point's
 * 0x4AC4 (0x4A9A), the distance (FUN_00018452) split across/along; the side
 * offsets from the course's widths (+0x10) banked by +0x1C into 0x4AC8 and
 * 0x4AA4, and 0x4AC6 = across * that / the segment length (FUN_00018452) */
static uint32_t rd_drone_lateral(void)
{
    uint32_t r;
    regs_t R_; regs_load(&R_);
    uint32_t *d = R_.d, *a = R_.a, a5 = a_reg(5);
    a[0] = COURSE_RECS;
    LOW16(d[6], w16(0x4A94)); LOW16(d[7], w16(0x4A96));
    charge(3);
    for (;;) {
        uint32_t x = sx16(d[6]);
        if (drone_gate(x)) {
            LOW16(d[0], w16(0x4AAA + x)); LOW16(d[0], d[0] << 6);
            uint32_t rec = a[0] + sx16(d[0]);
            a[1] = vrd32(rec + 0x14); a[2] = vrd32(rec + 0xC); a[3] = vrd32(rec + 0x20);
            LOW16(d[5], w16(0x16AE + x));
            w16_set(0x4A9C, 0);
            d[4] = 1; LOW16(d[4], d[4] + d[5]);
            w16_set(0x4A9E, (uint16_t)d[4]);
            LOW16(d[1], w16(0x16B0 + x)); LOW16(d[1], d[1] - vrd16(a[2] + sx16(d[5]) * 4u));
            LOW16(d[2], w16(0x16B4 + x)); LOW16(d[2], d[2] - vrd16(a[2] + 2u + sx16(d[5]) * 4u));
            LOW16(d[3], d[1]); LOW16(d[4], d[2]);
            d[3] = muls_w(d[3], d[3]); d[4] = muls_w(d[4], d[4]);
            d[3] += d[4];
            push32(d[3]);                               /* movem.l {D3},-(SP) */
            charge(22);
            int zero = 0;
            if ((uint16_t)d[1] == 0) {
                charge(2);
                if ((uint16_t)d[2] == 0) { d[0] = 0; charge(2); zero = 1; }
            }
            if (!zero) {
                regs_store(&R_);
                CALL(L_A98C, 0x1A526);
                regs_load(&R_);
                LOW16(d[0], 0u - d[0]); LOW16(d[0], d[0] + 0x4000);
                charge(2);
            }
            d[3] = pop32();                             /* movem.l (SP)+,{D3} */
            LOW16(d[0], d[0] - w16(0x4AC4 + sx16(d[6])));
            w16_set(0x4A9A, (uint16_t)d[0]);
            push32(d[6]); push32(d[5]);                 /* movem.l {D6 D5},-(SP): D5 at the lower address */
            regs_store(&R_);
            charge(5);                                  /* movem, sub, move, movem, jsr */
            if ((r = rd_call(L_18452, 0x1A544))) return r;
            regs_load(&R_);
            d[5] = pop32(); d[6] = pop32();
            x = sx16(d[6]);
            LOW16(d[0], w16(0x4A9A)); LOW16(d[0], d[0] + 0x4000); LOW16(d[0], d[0] & 0xFFFE);
            LOW16(d[3], vrd16(a5 + sx16(d[0])));
            LOW16(d[0], d[0] + 0x4000); LOW16(d[0], d[0] & 0xFFFE);
            LOW16(d[4], vrd16(a5 + sx16(d[0])));
            LOW16(d[3], 0u - d[3]);
            d[3] = fmul15(d[2], d[3]); d[4] = fmul15(d[2], d[4]);
            w16_set(0x4AA0, (uint16_t)d[3]);
            w16_set(0x16C8 + x, (uint16_t)d[4]);
            a[0] = COURSE_RECS;
            LOW16(d[0], w16(0x4AAA + x)); LOW16(d[0], d[0] << 6);
            rec = a[0] + sx16(d[0]);
            a[1] = vrd32(rec + 0x10); a[4] = vrd32(rec + 0x1C);
            LOW16(d[4], w16(0x16AE + x));
            LOW16(d[5], d[4]); LOW16(d[5], d[5] + 1);
            uint32_t i4 = sx16(d[4]), i5 = sx16(d[5]);
            LOW16(d[0], vrd16(a[4] + i4 * 2u)); LOW16(d[0], d[0] + 0x8000); LOW16(d[0], d[0] & 0xFFFE);
            LOW16(d[1], vrd16(a5 + sx16(d[0])));
            LOW16(d[0], vrd16(a[4] + i5 * 2u)); LOW16(d[0], d[0] + 0x8000); LOW16(d[0], d[0] & 0xFFFE);
            LOW16(d[2], vrd16(a5 + sx16(d[0])));
            LOW16(d[0], w16(0x16C8 + x));
            d[1] = fmul15(d[0], d[1]); d[2] = fmul15(d[0], d[2]);
            LOW16(d[1], d[1] + vrd16(a[1] + i4 * 2u));
            LOW16(d[2], d[2] + vrd16(a[1] + i5 * 2u));
            charge(44);
            if ((int16_t)w16(0x4A9C) < 0) {
                LOW16(d[0], d[1]); LOW16(d[0], d[0] - vrd16(a[1] + i5 * 2u));
                w16_set(0x4AC8 + x, (uint16_t)d[0]);
                LOW16(d[1], d[1] - d[2]);
                w16_set(0x4AA4, (uint16_t)d[1]);
                charge(6);
            } else {
                LOW16(d[0], d[2]); LOW16(d[0], d[0] - vrd16(a[1] + i4 * 2u));
                w16_set(0x4AC8 + x, (uint16_t)d[0]);
                LOW16(d[2], d[2] - d[1]);
                w16_set(0x4AA4, (uint16_t)d[2]);
                charge(5);
            }
            LOW16(d[0], vrd16(a[2] + i4 * 4u)); LOW16(d[0], d[0] - vrd16(a[2] + i5 * 4u));
            LOW16(d[1], vrd16(a[2] + 2u + i4 * 4u)); LOW16(d[1], d[1] - vrd16(a[2] + 2u + i5 * 4u));
            d[0] = muls_w(d[0], d[0]); d[1] = muls_w(d[1], d[1]);
            d[0] += d[1];
            d[3] = d[0];
            regs_store(&R_);
            push32(d[6]);
            charge(10);                                 /* ... movem, jsr */
            if ((r = rd_call(L_18452, 0x1A624))) return r;
            regs_load(&R_);
            d[6] = pop32();
            x = sx16(d[6]);
            w16_set(0x4AA2, (uint16_t)d[2]);
            LOW16(d[0], w16(0x4AA0)); LOW16(d[1], w16(0x4AA4));
            d[1] = muls_w(d[0], d[1]);
            LOW16(d[0], w16(0x4AA2));
            divs_w(&d[1], d[0]);
            w16_set(0x4AC6 + x, (uint16_t)d[1]);
            charge(8);
        }
        LOW16(d[6], d[6] + 0x40);
        charge(2);
        LOW16(d[7], d[7] - 1);
        if ((uint16_t)d[7] == 0xFFFF) break;
        regs_store(&R_); poll();
    }
    regs_store(&R_);
    RTS();
}

/* FUN_0001a64c: each drone we drive: when the track's half-width on its side
 * (0x4AC2, or 0x4AC0 when its lateral 0x16C8 is negative) less 0x14 is
 * below |0x16C8|, it has hit the edge: FUN_0001a6a2 */
static uint32_t rd_drone_edge(void)
{
    uint32_t r;
    regs_t R_; regs_load(&R_);
    uint32_t *d = R_.d;
    LOW16(d[4], w16(0x4A94)); LOW16(d[7], w16(0x4A96));
    charge(2);
    for (;;) {
        uint32_t x = sx16(d[4]);
        charge(2);                                      /* btst #7, beq */
        if (DRONE_ACTIVE(x)) {
            charge(2);                                  /* btst #0, beq */
            if (vrd8(A6W(0x16C4) + x) & 1) {
                LOW16(d[0], w16(0x16AE + x));
                LOW16(d[2], w16(0x4AC0 + x));
                LOW16(d[1], w16(0x16C8 + x));
                charge(4);
                if (!((int16_t)d[1] < 0)) { LOW16(d[2], w16(0x4AC2 + x)); charge(1); }
                charge(2);                              /* tst, bpl */
                if ((int16_t)d[1] < 0) { LOW16(d[1], 0u - d[1]); charge(1); }
                LOW16(d[2], d[2] - 0x14);
                int borrow = (uint16_t)d[2] < (uint16_t)d[1];
                LOW16(d[2], d[2] - d[1]);
                charge(3);                              /* subi, sub, bcc */
                if (borrow) {
                    regs_store(&R_);
                    CALL(L_1A6A2, 0x1A698);
                    regs_load(&R_);
                }
            }
        }
        LOW16(d[4], d[4] + 0x40);
        charge(2);
        LOW16(d[7], d[7] - 1);
        if ((uint16_t)d[7] == 0xFFFF) break;
        regs_store(&R_); poll();
    }
    regs_store(&R_);
    RTS();
}

/* FUN_0001a752: drone-to-drone contact: for each drone we drive, against
 * every other active drone within 0x40 in height: when closer than
 * sqrt(0x2F44) now (or, against one of ours, 0x78 ahead along its heading --
 * 0x4202/0x420A), push it away from the other (FUN_0000a98c heading + 0x8000)
 * by 0x400 (0x1000 and halve its speed when the other is not ours) into
 * 0x16D0/0x16D2; one push per drone per frame */
static uint32_t rd_drone_contact(void)
{
    uint32_t r;
    regs_t R_; regs_load(&R_);
    uint32_t *d = R_.d, a5 = a_reg(5);
    LOW16(d[4], w16(0x4A94)); LOW16(d[7], w16(0x4A96));
    charge(2);
    for (;;) {
        uint32_t x = sx16(d[4]);
        if (drone_gate(x)) {
            LOW16(d[3], w16(0x4A94)); LOW16(d[6], w16(0x4A96));
            LOW16(d[0], w16(0x16B8 + x)); LOW16(d[0], d[0] & 0xFFFE);
            LOW16(d[1], vrd16(a5 + sx16(d[0])));
            LOW16(d[0], d[0] + 0x4000);
            LOW16(d[2], vrd16(a5 + sx16(d[0])));
            LOW16(d[1], 0u - d[1]);
            d[1] = fmul15(d[1], 0x78); d[2] = fmul15(d[2], 0x78);
            w16_set(0x4202, (uint16_t)d[1]); w16_set(0x420A, (uint16_t)d[2]);
            charge(16);
            for (;;) {                                  /* 0x1A7B0: the others */
                x = sx16(d[4]);
                uint32_t y = sx16(d[3]);
                int hit = 0;
                charge(2);                              /* btst, beq */
                if (!DRONE_ACTIVE(y)) goto next;
                charge(2);                              /* cmp, beq */
                if ((uint16_t)d[3] == (uint16_t)d[4]) goto next;
                LOW16(d[0], w16(0x16B2 + x)); LOW16(d[0], d[0] - w16(0x16B2 + y));
                charge(3);
                if ((int16_t)d[0] < 0) { LOW16(d[0], 0u - d[0]); charge(1); }
                charge(2);                              /* cmpi, bgt */
                if ((int16_t)d[0] > 0x40) goto next;
                LOW16(d[0], w16(0x16B0 + x)); LOW16(d[1], w16(0x16B4 + x));
                LOW16(d[0], d[0] - w16(0x16B0 + y)); LOW16(d[1], d[1] - w16(0x16B4 + y));
                d[0] = muls_w(d[0], d[0]); d[1] = muls_w(d[1], d[1]);
                d[0] += d[1];
                d[2] = 0x2F44;
                charge(10);
                if ((int32_t)d[0] <= (int32_t)d[2]) hit = 1;
                else {
                    charge(2);                          /* cmp, blt */
                    if ((int16_t)d[3] < (int16_t)w16(0x4A94)) goto next;
                    LOW16(d[0], w16(0x16B0 + x)); LOW16(d[1], w16(0x16B4 + x));
                    LOW16(d[0], d[0] - w16(0x4202)); LOW16(d[1], d[1] - w16(0x420A));
                    LOW16(d[0], d[0] - w16(0x16B0 + y)); LOW16(d[1], d[1] - w16(0x16B4 + y));
                    d[0] = muls_w(d[0], d[0]); d[1] = muls_w(d[1], d[1]);
                    d[0] += d[1];
                    d[2] = 0x2F44;
                    charge(12);
                    if (!((int32_t)d[0] > (int32_t)d[2])) hit = 1;
                }
                if (hit) {
                    LOW16(d[1], w16(0x16B0 + y)); LOW16(d[2], w16(0x16B4 + y));
                    LOW16(d[1], d[1] - w16(0x16B0 + x)); LOW16(d[2], d[2] - w16(0x16B4 + x));
                    push32(d[4]); push32(d[3]);         /* movem.l {D4 D3},-(SP) */
                    charge(7);
                    int zero = 0;
                    if ((uint16_t)d[1] == 0) {
                        charge(2);
                        if ((uint16_t)d[2] == 0) { d[0] = 0; charge(2); zero = 1; }
                    }
                    if (!zero) {
                        regs_store(&R_);
                        CALL(L_A98C, 0x1A86C);
                        regs_load(&R_);
                        LOW16(d[0], 0u - d[0]); LOW16(d[0], d[0] + 0x4000);
                        charge(2);
                    }
                    d[3] = pop32(); d[4] = pop32();
                    x = sx16(d[4]);
                    LOW16(d[0], d[0] + 0x8000);
                    LOW16(d[5], 0x400);
                    charge(5);                          /* movem, addi, move, cmp, bge */
                    if ((int16_t)d[3] < (int16_t)w16(0x4A94)) {
                        LOW16(d[5], 0x1000);
                        w16_set(0x16C2 + x, (uint16_t)(w16(0x16C2 + x) >> 1));
                        charge(2);
                    }
                    LOW16(d[0], d[0] & 0xFFFE);
                    LOW16(d[1], vrd16(a5 + sx16(d[0])));
                    LOW16(d[0], d[0] + 0x4000);
                    LOW16(d[2], vrd16(a5 + sx16(d[0])));
                    LOW16(d[1], 0u - d[1]);
                    d[1] = fmul15(d[5], d[1]); d[2] = fmul15(d[5], d[2]);
                    LOW16(d[2], 0u - d[2]);
                    w16_set(0x16D0 + x, (uint16_t)(w16(0x16D0 + x) + d[1]));
                    w16_set(0x16D2 + x, (uint16_t)(w16(0x16D2 + x) + d[2]));
                    charge(15);
                    break;                              /* bra 0x1A8C6 */
                }
            next:
                LOW16(d[3], d[3] + 0x40);
                charge(2);                              /* addi, dbf */
                LOW16(d[6], d[6] - 1);
                if ((uint16_t)d[6] == 0xFFFF) break;
                regs_store(&R_); poll();
            }
        }
        LOW16(d[4], d[4] + 0x40);
        charge(2);
        LOW16(d[7], d[7] - 1);
        if ((uint16_t)d[7] == 0xFFFF) break;
        regs_store(&R_); poll();
    }
    regs_store(&R_);
    RTS();
}

/* FUN_0001a8dc: each drone we drive: its lane target 0x4AB8 from the
 * course's lane-change table (ROM 0x1AA52[course]: records of 8 bytes
 * start, length, lane, mirror; -1 ends): the lane of the first record whose
 * range holds its point, negated for odd slots (bit 6 of the slot offset)
 * when mirrored; 0 when none */
static uint32_t rd_drone_lane(void)
{
    uint32_t d0 = d_reg(0), d6 = d_reg(6), d7 = d_reg(7), a0 = a_reg(0);
    LOW16(d6, w16(0x4A94)); LOW16(d7, w16(0x4A96));
    charge(2);
    for (;;) {
        uint32_t x = sx16(d6);
        w16_set(0x4AB8 + x, 0);
        charge(3);                                      /* move, btst, beq */
        if (DRONE_ACTIVE(x)) {
            charge(2);                                  /* btst, beq */
            if (vrd8(A6W(0x16C4) + x) & 1) {
                a0 = 0x1AA52u;
                LOW16(d0, w16(0x4AAA + x));
                a0 = vrd32(a0 + sx16(d0) * 4u);
                charge(3);
                for (;;) {
                    charge(2);                          /* tst, bmi */
                    if ((int16_t)vrd16(a0) < 0) break;
                    LOW16(d0, w16(0x16AE + x)); LOW16(d0, d0 - vrd16(a0));
                    charge(4);                          /* move, sub, cmp, bcc */
                    if ((uint16_t)d0 < (uint16_t)vrd16(a0 + 2)) {
                        w16_set(0x4AB8 + x, (uint16_t)vrd16(a0 + 4));
                        charge(3);                      /* move, tst, beq */
                        if (vrd16(a0 + 6) != 0) {
                            charge(2);                  /* btst, bne */
                            if (!(d6 & 0x40)) { w16_set(0x4AB8 + x, (uint16_t)(0u - w16(0x4AB8 + x))); charge(1); }
                        }
                        charge(1);                      /* bra */
                        break;
                    }
                    a0 += 8;
                    charge(2);                          /* adda, bra */
                    set_d(0, d0); set_d(6, d6); set_d(7, d7); set_a(0, a0);
                    poll();
                }
            }
        }
        LOW16(d6, d6 + 0x40);
        charge(2);
        LOW16(d7, d7 - 1);
        if ((uint16_t)d7 == 0xFFFF) break;
        set_d(0, d0); set_d(6, d6); set_d(7, d7); set_a(0, a0);
        poll();
    }
    set_d(0, d0); set_d(6, d6); set_d(7, d7); set_a(0, a0);
    RTS();
}

/* FUN_0001a950: overtaking: each drone we drive, behind another active drone
 * of the same course by fewer than 16 points, not slower, and within 0x80
 * of its lane: steer 0x90 to the side away from the other's lane (or its
 * current offset 0x4AB6) into 0x4AB6; else 0x4AB6 = 0 */
static uint32_t rd_drone_overtake(void)
{
    regs_t R_; regs_load(&R_);
    uint32_t *d = R_.d;
    LOW16(d[6], w16(0x4A94)); LOW16(d[7], w16(0x4A96));
    charge(2);
    for (;;) {
        uint32_t x = sx16(d[6]);
        charge(2);                                      /* btst #7, beq */
        if (DRONE_ACTIVE(x)) {
            charge(2);                                  /* btst #0, beq */
            if (vrd8(A6W(0x16C4) + x) & 1) {
                LOW16(d[4], w16(0x4A94)); LOW16(d[5], w16(0x4A96));
                charge(2);
                for (;;) {
                    uint32_t y = sx16(d[4]);
                    d[3] = 0;
                    int ok = 0;
                    charge(3);                          /* moveq, cmp, beq */
                    if ((uint16_t)d[6] == (uint16_t)d[4]) goto skip;
                    charge(2);                          /* btst, beq */
                    if (!DRONE_ACTIVE(y)) goto skip;
                    LOW16(d[0], w16(0x4AAA + x));
                    charge(3);                          /* move, cmp, bne */
                    if ((uint16_t)d[0] != w16(0x4AAA + y)) goto skip;
                    LOW16(d[0], w16(0x16AE + x)); LOW16(d[0], d[0] - w16(0x16AE + y));
                    charge(3);                          /* move, sub, bpl */
                    if (!((int16_t)d[0] < 0)) goto skip;
                    LOW16(d[0], 0u - d[0]);
                    charge(4);                          /* bpl, neg, cmpi, bcc */
                    if ((uint16_t)d[0] >= 0x10) goto skip;
                    LOW16(d[0], w16(0x16C2 + x)); LOW16(d[0], d[0] - w16(0x16C2 + y));
                    charge(3);                          /* move, sub, bmi */
                    if ((int16_t)d[0] < 0) goto skip;
                    LOW16(d[0], w16(0x4AB8 + x)); LOW16(d[0], d[0] - w16(0x4AB8 + y));
                    charge(3);
                    if ((int16_t)d[0] < 0) { LOW16(d[0], 0u - d[0]); charge(1); }
                    charge(2);                          /* cmpi, bcc */
                    if ((uint16_t)d[0] >= 0x80) goto skip;
                    ok = 1;
                skip:
                    if (ok) {
                        LOW16(d[3], 0x90);
                        LOW16(d[0], w16(0x4AB6 + y));
                        charge(3);                      /* move, move, beq */
                        if ((uint16_t)d[0] != 0) { LOW16(d[0], w16(0x4AB8 + y)); charge(1); }
                        charge(1);                      /* bmi */
                        if (!((int16_t)d[0] < 0)) { LOW16(d[3], 0u - d[3]); charge(1); }
                        LOW16(d[3], d[3] + d[0]);
                        w16_set(0x4AB6 + x, (uint16_t)d[3]);
                        charge(3);                      /* add, move, bra */
                        break;
                    }
                    w16_set(0x4AB6 + x, (uint16_t)d[3]);
                    LOW16(d[4], d[4] + 0x40);
                    charge(3);                          /* move, addi, dbf */
                    LOW16(d[5], d[5] - 1);
                    if ((uint16_t)d[5] == 0xFFFF) break;
                    regs_store(&R_); poll();
                }
            }
        }
        LOW16(d[6], d[6] + 0x40);
        charge(2);
        LOW16(d[7], d[7] - 1);
        if ((uint16_t)d[7] == 0xFFFF) break;
        regs_store(&R_); poll();
    }
    regs_store(&R_);
    RTS();
}

/* FUN_0001aa10: each active drone's lane 0x4ABA steps by 1 toward its
 * overtaking offset 0x4AB6 (or its lane target 0x4AB8 when that is 0) */
static uint32_t rd_drone_lane_step(void)
{
    uint32_t d0 = d_reg(0), d1 = d_reg(1), d6 = d_reg(6), d7 = d_reg(7);
    LOW16(d6, w16(0x4A94)); LOW16(d7, w16(0x4A96));
    charge(2);
    for (;;) {
        uint32_t x = sx16(d6);
        charge(2);                                      /* btst, beq */
        if (DRONE_ACTIVE(x)) {
            LOW16(d0, w16(0x4AB6 + x));
            charge(2);                                  /* move, bne */
            if ((uint16_t)d0 == 0) { LOW16(d0, w16(0x4AB8 + x)); charge(1); }
            LOW16(d1, 1);
            uint16_t diff = (uint16_t)(d0 - w16(0x4ABA + x));
            charge(3);                                  /* move, cmp, beq */
            if (diff != 0) {
                charge(1);                              /* bpl */
                if (diff & 0x8000) { LOW16(d1, 0u - d1); charge(1); }
                w16_set(0x4ABA + x, (uint16_t)(w16(0x4ABA + x) + d1));
                charge(1);
            }
        }
        LOW16(d6, d6 + 0x40);
        charge(2);
        LOW16(d7, d7 - 1);
        if ((uint16_t)d7 == 0xFFFF) break;
        set_d(0, d0); set_d(1, d1); set_d(6, d6); set_d(7, d7);
        poll();
    }
    set_d(0, d0); set_d(1, d1); set_d(6, d6); set_d(7, d7);
    RTS();
}

/* FUN_0001abda: after FUN_0001ad64, each active drone we drive: on the
 * jump sections of its course (ROM 0x1AC3E: course -> list of points, -1
 * ends) or while airborne (0x4ACA), fly: fall speed 0x4ACA += 0x4800, the
 * height 0x16B2 drops, pitch 0x4AD2 and 0x4AB2 follow; on landing (height
 * reaches the ground 0x16BC) start the bounce 0x4AD4 (= speed >> 8) and put
 * it on the ground with the course pitch 0x16C6 plus the bounce wobble */
static uint32_t rd_drone_jump(void)
{
    uint32_t r;
    CALL(L_1AD64, 0x1ABDE);
    regs_t R_; regs_load(&R_);
    uint32_t *d = R_.d, *a = R_.a, a5 = a_reg(5);
    LOW16(d[6], w16(0x4A94)); LOW16(d[7], w16(0x4A96));
    charge(2);
    for (;;) {
        uint32_t x = sx16(d[6]);
        charge(2);                                      /* btst #7, beq */
        if (!DRONE_ACTIVE(x)) goto next;
        charge(2);                                      /* btst #0, beq */
        if (!(vrd8(A6W(0x16C4) + x) & 1)) goto ground;
        {
            d[0] = w16(0x4AAA + x);
            a[0] = 0x1AC3Eu;
            LOW16(d[5], 3);
            charge(4);
            int found = 0, fly = 0;
            for (;;) {
                charge(2);                              /* cmp.l, bne */
                if (d[0] == vrd32(a[0])) { a[1] = vrd32(a[0] + 4); charge(2); found = 1; break; }
                a[0] += 8;
                charge(2);                              /* adda, dbf */
                LOW16(d[5], d[5] - 1);
                if ((uint16_t)d[5] == 0xFFFF) break;
                regs_store(&R_); poll();
            }
            if (!found) charge(1);                      /* bra 0x1AC76 */
            else for (;;) {
                charge(2);                              /* tst, bmi */
                if ((int16_t)vrd16(a[1]) < 0) break;
                LOW16(d[0], w16(0x16AE + x)); LOW16(d[0], d[0] - vrd16(a[1]));
                charge(4);                              /* move, sub, cmpi, bcs */
                if ((uint16_t)d[0] < 0x10) { fly = 1; break; }
                a[1] += 2;
                charge(2);                              /* adda, bra */
                regs_store(&R_); poll();
            }
            if (!fly) {                                 /* 0x1AC76 */
                charge(2);                              /* tst.l, bne */
                if (w32(0x4ACA + x) == 0) { charge(1); goto ground; }
            }
            /* 0x1AC84: in the air */
            LOW16(d[0], w16(0x16BC + x)); LOW16(d[0], d[0] - w16(0x16B2 + x));
            charge(3);
            if ((int16_t)d[0] < 0) {
                w32_set(0x4ACA + x, w32(0x4ACA + x) + 0x4800);
                LOW16(d[1], w16(0x16B2 + x)); LOW16(d[1], d[1] - w16(0x4ACA + x));
                w16_set(0x16B2 + x, (uint16_t)d[1]);
                LOW16(d[0], w16(0x4ACA + x)); LOW16(d[0], d[0] << 2);
                w16_set(0x4AB2 + x, (uint16_t)d[0]);
                LOW16(d[0], w16(0x4ACA + x)); LOW16(d[0], d[0] << 4);
                w16_set(0x4AD2 + x, (uint16_t)(w16(0x4AD2 + x) + d[0]));
                charge(12);
                if (w16(0x4AD2 + x) >= 0x8000) { w16_set(0x4AD2 + x, (uint16_t)(w16(0x4AD2 + x) - d[0])); charge(1); }
                w16_set(0x16B6 + x, w16(0x4AD2 + x));
                LOW16(d[0], w16(0x16BC + x)); LOW16(d[0], d[0] - w16(0x16B2 + x));
                charge(4);
                if ((int16_t)d[0] < 0) { charge(1); goto next; }
            }
            /* 0x1ACFC: landed */
            d[0] = w32(0x4ACA + x) >> 8;
            w16_set(0x4AD4 + x, (uint16_t)d[0]);
            w16_set(0x4AD6 + x, 0);
            charge(4);
        }
    ground: /* 0x1AD10 */
        {
            uint32_t x2 = sx16(d[6]);
            w16_set(0x16B2 + x2, w16(0x16BC + x2));
            w16_set(0x16B6 + x2, w16(0x16C6 + x2));
            w16_set(0x4AD2 + x2, w16(0x16B6 + x2));
            w32_set(0x4ACA + x2, 0);
            LOW16(d[0], w16(0x4AD6 + x2)); LOW16(d[0], d[0] & 0xFFFE); LOW16(d[0], d[0] + 0x4000);
            LOW16(d[1], vrd16(a5 + sx16(d[0])));
            LOW16(d[0], w16(0x4AD4 + x2));
            d[1] = swap32(muls_w(d[0], d[1]));
            w16_set(0x16B6 + x2, (uint16_t)(w16(0x16B6 + x2) + d[1]));
            charge(13);
        }
    next:
        LOW16(d[6], d[6] + 0x40);
        charge(2);
        LOW16(d[7], d[7] - 1);
        if ((uint16_t)d[7] == 0xFFFF) break;
        regs_store(&R_); poll();
    }
    regs_store(&R_);
    RTS();
}

/* FUN_0001ad64: each drone we drive: the landing bounce -- its phase 0x4AD6
 * advances 0x1444 and its size 0x4AD4 shrinks by 0x78 toward 0 (both
 * cleared once it is done, or when not ours) */
static uint32_t rd_drone_bounce(void)
{
    uint32_t d0 = d_reg(0), d6 = d_reg(6), d7 = d_reg(7);
    LOW16(d6, w16(0x4A94)); LOW16(d7, w16(0x4A96));
    charge(2);
    for (;;) {
        uint32_t x = sx16(d6);
        int done = 1;
        if (drone_gate(x)) {
            LOW16(d0, 0x1444);
            w16_set(0x4AD6 + x, (uint16_t)(w16(0x4AD6 + x) + d0));
            LOW16(d0, w16(0x4AD4 + x));
            charge(6);                                  /* move, add, move, bmi, subi/addi, ble/bge */
            if (!((int16_t)d0 < 0)) { LOW16(d0, d0 - 0x78); if ((int16_t)d0 > 0) done = 0; }
            else { LOW16(d0, d0 + 0x78); if ((int16_t)d0 < 0) done = 0; }
            if (!done) { w16_set(0x4AD4 + x, (uint16_t)d0); charge(2); }
        }
        if (done) { w16_set(0x4AD4 + x, 0); w16_set(0x4AD6 + x, 0); charge(2); }
        LOW16(d6, d6 + 0x40);
        charge(2);
        LOW16(d7, d7 - 1);
        if ((uint16_t)d7 == 0xFFFF) break;
        set_d(0, d0); set_d(6, d6); set_d(7, d7);
        poll();
    }
    set_d(0, d0); set_d(6, d6); set_d(7, d7);
    RTS();
}

/* FUN_0001adce: rubber band: until the race time 0x204A reaches the limit
 * (0x4846 - 1 : ROM 0x1895C[selection]), each drone's speed bonus 0x4AB4 by
 * its distance to us (course points, laps x the course length): ahead ->
 * 0x4ADE when within 0x4ADC; behind by 20..300 -> 0x4AE0; beyond -> 0x4AE2;
 * after the limit all bonuses are 0 */
static uint32_t rd_drone_rubber_band(void)
{
    regs_t R_; regs_load(&R_);
    uint32_t *d = R_.d, *a = R_.a;
    a[0] = 0x1895Cu;
    LOW16(d[0], w16(G_SEL_2342));
    LOW16(d[0], vrd16(a[0] + sx16(d[0]) * 2u));
    d[0] = swap32(d[0]);
    LOW16(d[0], w16(0x4846)); LOW16(d[0], d[0] - 1);
    d[0] = swap32(d[0]);
    charge(9);
    if ((int32_t)d[0] < (int32_t)w32(0x204A)) {
        d[0] = 0;
        LOW16(d[6], w16(0x4A94)); LOW16(d[7], w16(0x4A96));
        charge(3);
        for (;;) {
            w16_set(0x4AB4 + sx16(d[6]), (uint16_t)d[0]);
            LOW16(d[6], d[6] + 0x40);
            charge(3);
            LOW16(d[7], d[7] - 1);
            if ((uint16_t)d[7] == 0xFFFF) break;
            regs_store(&R_); poll();
        }
        regs_store(&R_);
        RTS();
    }
    LOW16(d[6], w16(0x4A94)); LOW16(d[7], w16(0x4A96));
    charge(2);
    for (;;) {
        uint32_t x = sx16(d[6]);
        d[3] = 0;
        LOW16(d[0], w16(0x4AAA + x));
        a[0] = COURSE_RECS;
        LOW16(d[0], d[0] << 6);
        LOW16(d[5], vrd16(a[0] + sx16(d[0]) + 6u));
        LOW16(d[0], w16(0x16AC + x)); LOW16(d[1], w16(0x16AE + x));
        LOW16(d[0], d[0] - w16(0x204A)); LOW16(d[1], d[1] - w16(0x204C));
        d[0] = muls_w(d[5], d[0]);
        LOW16(d[0], d[0] + d[1]);
        charge(12);
        if ((int16_t)d[0] < 0) {
            charge(2);                                  /* cmpi, bge */
            if ((int16_t)d[0] < -0x14) {
                LOW16(d[3], w16(0x4AE0 + x));
                charge(3);                              /* move, cmpi, bge */
                if ((int16_t)d[0] < -0x12C) { LOW16(d[3], w16(0x4AE2 + x)); charge(2); }
            }
        } else {
            charge(2);                                  /* cmp, bcc */
            if ((uint16_t)d[0] < w16(0x4ADC + x)) { LOW16(d[3], w16(0x4ADE + x)); charge(1); }
        }
        w16_set(0x4AB4 + x, (uint16_t)d[3]);
        LOW16(d[6], d[6] + 0x40);
        charge(3);
        LOW16(d[7], d[7] - 1);
        if ((uint16_t)d[7] == 0xFFFF) break;
        regs_store(&R_); poll();
    }
    regs_store(&R_);
    RTS();
}

/* restart the car at course point `pt` (heading from point `hp`, + `turn`):
 * position, height, heading, and the per-course reset (0x1AF34 / 0x1AFC0) */
static void car_restart(regs_t *R_, uint16_t pt, uint16_t hp, uint16_t turn)
{
    uint32_t *d = R_->d, *a = R_->a;
    a[0] = COURSE_RECS;
    LOW16(d[0], w16(0x2046)); LOW16(d[0], d[0] << 6);
    uint32_t rec = COURSE_RECS + sx16(d[0]);
    a[1] = vrd32(rec + 0xC); a[2] = vrd32(rec + 0x10); a[3] = vrd32(rec + 0x14);
    LOW16(d[0], pt); LOW16(d[2], hp);
    w16_set(0x204C, pt); w16_set(0x20C8, 0); w16_set(0x20D8, 0);
    uint32_t i = sx16(pt);
    w16_set(0x208C, (uint16_t)vrd16(a[1] + i * 4u));
    w16_set(0x2090, (uint16_t)vrd16(a[1] + 2u + i * 4u));
    w16_set(0x208E, (uint16_t)vrd16(a[2] + i * 2u));
    w16_set(0x20AC, (uint16_t)vrd16(a[2] + i * 2u));
    LOW16(d[1], vrd16(a[3] + sx16(hp) * 2u)); LOW16(d[1], 0u - d[1]); LOW16(d[1], d[1] + turn);
    w16_set(0x408A, (uint16_t)d[1]); w16_set(0x4088, (uint16_t)d[1]);
    w16_set(0x47CE, 0);
}

/* FUN_0001ae7e: per-course special cases (word jump table at ROM 0x1AE94 by
 * 0x2046 & 15):
 *   0x1AEB6: the shortcut -- between points 0xD3 and 0x125 with bit 1 of
 *            0x41D7 and 0x41AA >= 5, jump to course 7 point 10 (0x47BC,
 *            0x48B2.. saved, 0x408A = 0x800)
 *   0x1AF10 / 0x1AF28: at point 0 restart at point 1 (0x4022 = 0x2200 when linked)
 *   0x1AFB4: past point 0x529 restart at 0x528 facing back, FUN_0001ea60(0)
 *   else nothing */
static uint32_t rd_course_special(void)
{
    uint32_t r;
    regs_t R_; regs_load(&R_);
    uint32_t *d = R_.d;
    LOW16(d[0], w16(0x2046)); LOW16(d[0], d[0] & 0xF);
    LOW16(d[0], vrd16(0x1AE94u + sx16(d[0]) * 2u));
    uint32_t t = 0x1AE94u + sx16(d[0]);
    regs_store(&R_);
    charge(6);                                          /* move, andi, move, nop, nop, jmp */
    if (t != 0x1B02Au && t != 0x1AEB6u && t != 0x1AF10u && t != 0x1AF28u && t != 0x1AFB4u) return RD_JMP(t);
    regs_store(&R_); poll();
    switch (t) {
    case 0x1AEB6u:
        charge(2);                                      /* tst, beq */
        if (w16(0x41D6) == 0) RTS();
        LOW16(d[0], w16(0x204C)); LOW16(d[0], d[0] - 0xD3);
        set_d(0, d[0]);
        charge(4);
        if ((uint16_t)d[0] >= 0x53) RTS();
        charge(2);
        if (!(vrd8(A6W(0x41D7)) & 2)) RTS();
        charge(2);
        if (w16(0x41AA) < 5) RTS();
        w16_set(0x2046, 7); w16_set(0x204C, 0xA);
        w16_set(0x208E, (uint16_t)(w16(0x208E) + 0x10));
        w32_set(0x47BC, 0x70000); w16_set(0x48B2, 1);
        w16_set(0x48B4, w16(0x408A)); w16_set(0x48B6, w16(0x4022));
        w16_set(0x408A, 0x800);
        charge(8);
        RTS();
    case 0x1AF10u:
        LOW16(d[0], w16(0x204C));
        set_d(0, d[0]);
        charge(3);
        if ((uint16_t)d[0] != 0) RTS();
        charge(2);
        if (w16(0x1100) != 0) { w16_set(0x4022, 0x2200); charge(1); }
        /* fall into 0x1AF28 */
    case 0x1AF28u:
        LOW16(d[0], w16(0x204C));
        set_d(0, d[0]);
        charge(3);
        if ((uint16_t)d[0] != 0) RTS();
        car_restart(&R_, 1, 0x14, 0);
        w16_set(0x41DE, 0);
        charge(21);
        regs_store(&R_);
        RTS();
    case 0x1AFB4u:
        LOW16(d[0], w16(0x204C));
        set_d(0, d[0]);
        charge(3);
        if (!((int16_t)d[0] > 0x529)) RTS();
        car_restart(&R_, 0x528, 0x514, 0x8000);
        LOW16(d[1], 0);
        charge(22);
        regs_store(&R_);
        CALL(L_1EA60, 0x1B028);
        RTS();
    default:
        RTS();
    }
}

static const rd_entry rd_table_b2[] = {
    /*  ep        fn                         kill    scratch cost  name */
    { 0x011CF2, rd_decay_2356_2366,        0x00C3, 0,      1, "FUN_00011cf2" },
    { 0x011D2A, rd_dl_four_smoothed,       0x08FF, 0,      3, "FUN_00011d2a" },
    { 0x011E12, rd_dl_select_scene,        0x08FF, 0,      22, "FUN_00011e12" },
    { 0x011F4A, rd_state_232a,             0x3FFF, 0,      0, "FUN_00011f4a", 1 },
    { 0x0120C0, rd_wait_all_ready,         0x1FE7, 0,      1, "FUN_000120c0" },
    { 0x01218C, rd_check_time_up,          0x37C1, 0,      1, "FUN_0001218c" },
    { 0x0121BC, rd_init_2216,              0x0BFF, 0,      1, "FUN_000121bc" },
    { 0x0121FC, rd_pair_update,            0x3FFF, 0,      1, "FUN_000121fc" },
    { 0x01229A, rd_stage_advance,          0x03FF, 0,      0, "FUN_0001229a" },
    { 0x01253A, rd_stage_start,            0x0103, 0,      28, "FUN_0001253a" },
    { 0x0125A0, rd_stage_limit,            0x0FDF, 0,      0, "FUN_000125a0" },
    { 0x0125EA, rd_stage_texts,            0x0FDF, 0,      0, "FUN_000125ea" },
    { 0x0126D0, rd_stage_banner,           0x0FDF, 0,      0, "FUN_000126d0" },
    { 0x012774, rd_toggle_222a,            0x3FFF, 0,      0, "FUN_00012774" },
    { 0x012C40, rd_result_screen,          0x3FFF, 0,      0, "FUN_00012c40", 1 },
    { 0x0131F4, rd_dl_header_8001,         0x1001, 0,      6, "FUN_000131f4" },
    { 0x01348A, rd_step_233a,              0x3FFF, 0,      0, "FUN_0001348a" },
    { 0x0134CA, rd_demo_setup,             0x1FFF, 0,      0, "FUN_000134ca" },
    { 0x01366E, rd_step_5884,              0x3FFF, 0,      0, "FUN_0001366e" },
    { 0x013696, rd_countdown_233c,         0x2FFF, 0,      0, "FUN_00013696" },
    { 0x01429E, rd_course_start,           0x0FDF, 0,      29, "FUN_0001429e" },
    { 0x01432E, rd_player_nearest_point,   0x1FFF, 0,      1, "FUN_0001432e" },
    { 0x0143B6, rd_course_nearest_point,   0x07FF, 0,      0, "FUN_000143b6" },
    { 0x0145C4, rd_player_add_deltas,      0x0041, 0,      1, "FUN_000145c4" },
    { 0x014634, rd_players_2a_flags,       0x37C1, 0,      0, "FUN_00014634" },
    { 0x0146AE, rd_player_to_record,       0x37C1, 0,      30, "FUN_000146ae" },
    { 0x014744, rd_player_mirror,          0x1BFF, 0,      58, "FUN_00014744" },
    { 0x014872, rd_thunk_14b70,            0x1FFF, 0,      1, "thunk_FUN_00014b70" },
    { 0x014B70, rd_track_geometry,         0x1FFF, 0,      0, "FUN_00014b70" },
    { 0x014EB6, rd_course_display_list,    0x1FFF, 0,      0, "FUN_00014eb6" },
    { 0x015038, rd_two_vertex_records_nz,  0x0803, 0,      0, "FUN_00015038" },
    { 0x01506C, rd_track_objects,          0x0F1F, 0,      0, "FUN_0001506c" },
    { 0x0150E6, rd_track_tail,             0x0BFF, 0,      0, "FUN_000150e6" },
    { 0x01563A, rd_init_object2,           0x0BFF, 0,      0, "FUN_0001563a" },
    { 0x015674, rd_object2_step,           0x1BFF, 0,      0, "FUN_00015674" },
    { 0x0156C0, rd_object2_draw,           0x08FF, 0,      0, "FUN_000156c0" },
    { 0x015840, rd_15840,                  0x1FFF, 0,      0, "FUN_00015840" },
    { 0x01595C, rd_roadside_object,        0x0FFF, 0,      0, "FUN_0001595c" },
    { 0x015E18, rd_roadside_objects,       0x0FFF, 0,      0, "FUN_00015e18" },
    { 0x016598, rd_camera_from_2414,       0x1003, 0,      21, "FUN_00016598" },
    { 0x0165FA, rd_draw_object_list,       0x1FFF, 0,      0, "FUN_000165fa" },
    { 0x01661E, rd_object_record,          0x3103, 0,      1, "FUN_0001661e" },
    { 0x01667E, rd_dl_object_829,          0x0801, 0,      18, "FUN_0001667e" },
    { 0x0167CE, rd_title_object,           0x1BFF, 0,      0, "FUN_000167ce" },
    { 0x016FC4, rd_title_setup,            0x0FFF, 0,      0, "FUN_00016fc4" },
    { 0x01704E, rd_title_steps,            0x3FFF, 0,      0, "FUN_0001704e", 1 },
    { 0x0171AA, rd_title_logo_171aa,       0x3FFF, 0,      0, "FUN_000171aa" },
    { 0x0171DE, rd_title_obj4,             0x0BFF, 0,      0, "FUN_000171de" },
    { 0x0171F0, rd_dl_model_730,           0x0803, 0,      19, "FUN_000171f0" },
    { 0x017250, rd_dl_logo,                0x0803, 0,      0, "FUN_00017250" },
    { 0x0172F2, rd_title_object_setup,     0x0BFF, 0,      0, "FUN_000172f2" },
    { 0x017378, rd_title_object_draw,      0x1FFF, 0,      0, "FUN_00017378" },
    { 0x01739E, rd_title_object_step,      0x0203, 0,      40, "FUN_0001739e" },
    { 0x017436, rd_title_ground,           0x1F87, 0,      0, "FUN_00017436" },
    { 0x0174F2, rd_title_logo_174f2,       0x3FFF, 0,      0, "FUN_000174f2" },
    { 0x0176D6, rd_race_frame,             0x3FFF, 0,      0, "caseD_3 @176D6", 1 },
    { 0x017B3C, rd_17b3c,                  0x0FFF, 0,      0, "FUN_00017b3c" },
    { 0x017B64, rd_player0_from_car,       0x0203, 0,      0, "FUN_00017b64" },
    { 0x017C82, rd_smooth_pitch,           0x0FFF, 0,      0, "FUN_00017c82" },
    { 0x017CF4, rd_smooth_roll,            0x0FFF, 0,      0, "FUN_00017cf4" },
    { 0x017D54, rd_sway,                   0x0003, 0,      0, "FUN_00017d54" },
    { 0x017E2C, rd_wrong_way,              0x0003, 0,      0, "FUN_00017e2c" },
    { 0x017E7C, rd_record_car_state,       0x0041, 0,      0, "FUN_00017e7c" },
    { 0x017F4A, rd_speed_2088,             0x0003, 0,      1, "FUN_00017f4a" },
    { 0x017F80, rd_view_yaw_delta,         0x0003, 0,      1, "FUN_00017f80" },
    { 0x018046, rd_race_position,          0x00F9, 0,      0, "FUN_00018046" },
    { 0x0180BA, rd_odometer,               0x03FF, 0,      0, "FUN_000180ba" },
    { 0x018156, rd_step_478c,              0x020F, 0,      0, "FUN_00018156" },
    { 0x01818C, rd_finish_fade,            0x034F, 0,      0, "FUN_0001818c" },
    { 0x018452, rd_isqrt,                  0x08FF, 0,      0, "FUN_00018452" },
    { 0x0184E2, rd_link_setup,             0x1FF7, 0,      0, "FUN_000184e2" },
    { 0x018558, rd_mark_finished,          0x03FF, 0,      0, "FUN_00018558" },
    { 0x0186C6, rd_clear_slot_tables,      0x01C1, 0,      0, "FUN_000186c6" },
    { 0x0186D2, rd_clear_1k,               0x01C1, 0,      0, "FUN_000186d2" },
    { 0x018702, rd_drone_slots,            0x1FF7, 0,      0, "FUN_00018702" },
    { 0x019DAC, rd_drones_update,          0x1FFF, 0,      0, "FUN_00019dac" },
    { 0x019E0C, rd_drone_target,           0x1BFF, 0,      0, "FUN_00019e0c" },
    { 0x019EB0, rd_drone_speed,            0x03FF, 0,      0, "FUN_00019eb0" },
    { 0x019FB4, rd_drone_brake,            0x03FF, 0,      0, "FUN_00019fb4" },
    { 0x01A034, rd_drone_push_decay,       0x08FF, 0,      0, "FUN_0001a034" },
    { 0x01A07E, rd_drone_move,             0x08FF, 0,      0, "FUN_0001a07e" },
    { 0x01A102, rd_drone_nearest_point,    0x07FF, 0,      0, "FUN_0001a102" },
    { 0x01A1E0, rd_drone_odometer,         0x00C1, 0,      0, "FUN_0001a1e0" },
    { 0x01A2AA, rd_drone_height,           0x03FF, 0,      0, "FUN_0001a2aa" },
    { 0x01A2FA, rd_drone_attitude,         0x07FF, 0,      0, "FUN_0001a2fa" },
    { 0x01A3D2, rd_drone_4ada,             0x00C3, 0,      0, "FUN_0001a3d2" },
    { 0x01A42C, rd_drone_steer,            0x00C3, 0,      0, "FUN_0001a42c" },
    { 0x01A49E, rd_drone_lateral,          0x1FFF, 0,      0, "FUN_0001a49e" },
    { 0x01A64C, rd_drone_edge,             0x1BFF, 0,      0, "FUN_0001a64c" },
    { 0x01A752, rd_drone_contact,          0x03FF, 0,      0, "FUN_0001a752" },
    { 0x01A8DC, rd_drone_lane,             0x01C1, 0,      0, "FUN_0001a8dc" },
    { 0x01A950, rd_drone_overtake,         0x00F9, 0,      0, "FUN_0001a950" },
    { 0x01AA10, rd_drone_lane_step,        0x00C3, 0,      0, "FUN_0001aa10" },
    { 0x01ABDA, rd_drone_jump,             0x03FF, 0,      0, "FUN_0001abda" },
    { 0x01AD64, rd_drone_bounce,           0x00C1, 0,      0, "FUN_0001ad64" },
    { 0x01ADCE, rd_drone_rubber_band,      0x03FF, 0,      0, "FUN_0001adce" },
    { 0x01AE7E, rd_course_special,         0x0F1F, 0,      0, "FUN_0001ae7e", 1 },
};
RD_REGISTER(rd_table_b2)
