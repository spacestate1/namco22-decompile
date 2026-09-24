/*
 * rd_funcs.c -- the readable-C replacements themselves (include/rd.h).
 *
 * Written from the 68K INSTRUCTIONS (the comments in gen/rr_lifted.c), with
 * Ghidra's decompiled C (decompiled/raverace_all.c) as a naming aid only:
 * its C drops register results it does not model -- for FUN_00027062 it kept
 * D4 and lost D1, D2 and D3. Every entry is verified by RR_RD=check.
 *
 * Conventions: A6 is the game's context register and always holds WRAM +
 * 0x8000 (0x10008000), so `(d16,A6)` operands are fixed addresses, named
 * here as WRAM globals. A register a caller reads back is set exactly as the
 * 68K leaves it (move.w into Dn changes only the low word: set_d16).
 *
 * rd_table MUST stay sorted by entry address.
 */
#include "rd.h"

/* ---- WRAM globals (A6 = 0x10008000) ---- */
#define G_SKIP_SND_1071    0x10001071u   /* byte: nonzero skips the sound-parameter write (purpose not yet identified) */
#define G_SND_PARAM_C344   0x1000C344u   /* word: sent to the sound CPU (low byte)    */
#define G_FLAG_0804        0x10000804u   /* word: 1 if sound status bit 2 or DSW bit 0 is clear (purpose not yet identified) */
#define G_CAM_X            0x1000A1B8u   /* long: camera position the vertices are    */
#define G_CAM_Y            0x1000A1BCu   /*       made relative to                    */
#define G_CAM_Z            0x1000A1C0u
#define G_SLOT_INDEX       0x1000915Au   /* word: byte offset of the current slot     */
#define G_SLOT_TEMPLATE    0x100091A0u   /* 12-byte record copied into the slot table  */
#define G_SLOT_TABLE       0x100091ACu
#define G_SLOT_ENABLE_MASK 0x10009105u   /* byte: bit n = slot n enabled               */

/* ---- hardware (shared RAM with the C74 sound CPU, DSW port) ---- */
#define SND_CMD_402A       0x6000402Au   /* word */
#define SND_ENGINE_PARAM   0x60004041u   /* byte */
#define SND_STATUS         0x60004030u   /* byte: bit 2 */
#define DSW_PORT_LO        0x50000001u   /* byte: bit 0 */

/* FUN_00015048: append one 16-byte vertex record at A3 --
 *   (D1 with its low word + index, x - cam_x, -cam_y, z - cam_z)
 * where index = D4w (signed) selects the (x, z) pair A1[index]. A3 advances
 * past the record; D0 ends holding the last long written, as the 68K leaves it. */
static uint32_t rd_vertex_record(void)
{
    uint32_t out = a_reg(3), d1 = d_reg(1);
    int32_t  idx = (int16_t)d_reg(4);
    uint32_t pair = a_reg(1) + (uint32_t)(idx * 8);
    uint32_t tag = (d1 & 0xFFFF0000u) | ((d1 + (uint32_t)idx) & 0xFFFFu);
    uint32_t x = vrd32(pair) - vrd32(G_CAM_X);
    uint32_t y = 0u - vrd32(G_CAM_Y);
    uint32_t z = vrd32(pair + 4) - vrd32(G_CAM_Z);
    vwr32(out, tag); vwr32(out + 4, x); vwr32(out + 8, y); vwr32(out + 12, z);
    set_a(3, out + 16);
    set_d(0, z);
    return RD_RTS;
}

/* FUN_00031246: D0 = D1 = (signed word D1) * 2 -- a word index scaled to bytes */
static uint32_t rd_word_index_x2(void)
{
    uint32_t v = (uint32_t)((int32_t)(int16_t)d_reg(1) * 2);
    set_d(1, v);
    set_d(0, v);
    return RD_RTS;
}

/* FUN_0000470e: unless G_SKIP_SND_1071 is set, send command 3 and the low byte of
 * G_SND_PARAM_C344 to the sound CPU (D0w = the parameter, as the 68K leaves it) */
static uint32_t rd_send_sound_param(void)
{
    if (vrd8(G_SKIP_SND_1071) != 0) return RD_RTS;
    charge(3);                                   /* the three moves below */
    vwr16(SND_CMD_402A, 3);
    uint16_t p = (uint16_t)vrd16(G_SND_PARAM_C344);
    set_d16(0, p);
    vwr8(SND_ENGINE_PARAM, p & 0xFF);
    return RD_RTS;
}

/* FUN_0000472a: G_FLAG_0804 (and D0) = 1 if the sound CPU's status bit 2 is
 * clear or DSW bit 0 is clear, else 0 */
static uint32_t rd_update_flag_0804(void)
{
    uint32_t v = 0;
    if (!(vrd8(SND_STATUS) & 4)) v = 1;
    if (!(vrd8(DSW_PORT_LO) & 1)) v = 1;
    set_d(0, v);
    vwr16(G_FLAG_0804, v);
    return RD_RTS;
}

/* FUN_0000494c: copy two words (0x816 -> 0x80E, 0x818 -> 0x812) and clear six
 * others -- a per-frame reset; what the words hold is not yet identified */
static uint32_t rd_reset_0816_block(void)
{
    vwr16(0x1000080Eu, vrd16(0x10000816u));
    vwr16(0x10000812u, vrd16(0x10000818u));
    vwr16(0x10000810u, 0);
    vwr16(0x10000814u, 0);
    vwr16(0x1000A306u, 0);
    vwr16(0x1000A30Au, 0);
    vwr16(0x1000A30Eu, 0);
    vwr16(0x1000A310u, 0);
    return RD_RTS;
}

/* FUN_00027062: walk 16-byte records (offset D5w into A0 / A1), D7w+1 of them,
 * slot bit D6 (mod 8) of G_SLOT_ENABLE_MASK. For each enabled one, with
 * w = the word at A0[offset]:
 *     D1w |= w;  D3w &= w;
 *     if (D0w == the word at A1[offset])  { D2w |= w;  D4w &= w; }
 * D5w, D6w and D7w advance exactly as the 68K loop leaves them (dbf: D7w ends
 * at 0xFFFF). */
static uint32_t rd_merge_enabled_slots(void)
{
    uint16_t d0 = (uint16_t)d_reg(0), d1 = (uint16_t)d_reg(1), d2 = (uint16_t)d_reg(2);
    uint16_t d3 = (uint16_t)d_reg(3), d4 = (uint16_t)d_reg(4), d5 = (uint16_t)d_reg(5);
    uint16_t d6 = (uint16_t)d_reg(6), d7 = (uint16_t)d_reg(7);
    uint32_t a0 = a_reg(0), a1 = a_reg(1);
    do {
        charge(5);                               /* btst, beq, addq, addi, dbf */
        if (vrd8(G_SLOT_ENABLE_MASK) & (1u << (d6 & 7))) {
            charge(4);                           /* or, and, cmp, bne */
            uint16_t w = (uint16_t)vrd16(a0 + (uint32_t)(int16_t)d5);
            d1 |= w;
            d3 &= w;
            if (d0 == (uint16_t)vrd16(a1 + (uint32_t)(int16_t)d5)) { charge(2); d2 |= w; d4 &= w; }
        }
        d6++;
        d5 += 0x10;
        if (d7-- == 0) break;
        poll();                                  /* dbf taken */
    } while (1);
    set_d16(1, d1); set_d16(2, d2); set_d16(3, d3); set_d16(4, d4);
    set_d16(5, d5); set_d16(6, d6); set_d16(7, d7);
    return RD_RTS;
}

/* FUN_000270f2: copy the 12-byte template into the slot table at the current
 * slot's offset (A2 = the table, D3w = the offset, as the 68K leaves them) */
static uint32_t rd_store_slot(void)
{
    uint16_t off = (uint16_t)vrd16(G_SLOT_INDEX);
    uint32_t dst = G_SLOT_TABLE + (uint32_t)(int16_t)off;
    vwr16(dst + 0, vrd16(G_SLOT_TEMPLATE + 0));
    vwr16(dst + 2, vrd16(G_SLOT_TEMPLATE + 2));
    vwr16(dst + 4, vrd16(G_SLOT_TEMPLATE + 4));
    vwr16(dst + 6, vrd16(G_SLOT_TEMPLATE + 6));
    vwr8 (dst + 8, vrd8 (G_SLOT_TEMPLATE + 8));
    vwr8 (dst + 9, vrd8 (G_SLOT_TEMPLATE + 9));
    vwr16(dst + 10, vrd16(G_SLOT_TEMPLATE + 10));
    set_a(2, G_SLOT_TABLE);
    set_d16(3, off);
    return RD_RTS;
}


/* ======================= batch 1 (2026-09-23) ======================= */

#define G_RANDOM           0x1000F000u   /* long: the game's pseudo-random generator state */

/* FUN_0000f02e: advance the random generator -- a 32-bit shift register with
 * feedback from bits 31 and 7:  x = (x << 1) + (bit31(x << 1) XOR bit7(x << 1)).
 * D0 = the new state. */
static uint32_t rd_random_next(void)
{
    uint32_t x = vrd32(G_RANDOM) << 1;
    if (((x >> 31) ^ (x >> 7)) & 1) { x += 1; charge(2); }   /* addq + bra */
    vwr32(G_RANDOM, x);
    set_d(0, x);
    return RD_RTS;
}

/* FUN_000046a6: reset the display-list header at the pointer held in 0xC42:
 * zero the first long of each of eight 0x80-byte blocks, then write the
 * terminator 0x8002, 0, -1 at +0x400. A3 = the pointer, D0w ends at 0xFFFF
 * (dbf) and D1w at 0, as the 68K leaves them. */
static uint32_t rd_reset_display_header(void)
{
    uint32_t base = vrd32(0x10008C42u);
    charge(2);                                   /* movea, moveq */
    for (int blk = 7; blk >= 0; blk--) {
        vwr32(base + (uint32_t)(int16_t)(blk << 7), 0);
        charge(4);                               /* move, lsl, move.l, dbf */
        if (blk) poll();                         /* dbf taken */
    }
    charge(4);                                   /* three move.l, rts */
    vwr32(base + 0x400, 0x8002);
    vwr32(base + 0x404, 0);
    vwr32(base + 0x408, 0xFFFFFFFFu);
    set_a(3, base);
    set_d(0, 0x0000FFFFu);
    set_d16(1, 0);
    return RD_RTS;
}

/* FUN_0001514c: if the word count at 0xA22E is not negative, clear count+1
 * words from 0xA230 and set the count to -1. A0 ends past the cleared words. */
static uint32_t rd_clear_pending_words(void)
{
    uint32_t p = 0x1000A230u;
    uint16_t n = (uint16_t)vrd16(0x1000A22Eu);
    charge(3);                                   /* lea, move, bmi */
    if (!(n & 0x8000)) {
        set_d(1, 0);
        charge(1);                               /* moveq */
        uint16_t c = n;
        for (;;) {
            vwr16(p, 0); p += 2;
            charge(2);                           /* move.w, dbf */
            if (c-- == 0) break;
            poll();                              /* dbf taken */
        }
        vwr16(0x1000A22Eu, 0xFFFF);
        charge(1);                               /* move.w #-1 */
        n = 0xFFFF;                              /* dbf leaves D0w at -1 */
    }
    charge(1);                                   /* rts */
    set_a(0, p);
    set_d16(0, n);
    return RD_RTS;
}

/* FUN_0001f61e: unless the mode word 0x4AA6 is 2 or 0x4560 is negative (then
 * it continues at 0x1FE0A), look up this entry's value in the ROM table at
 * 0x1EE2E (8-byte stride, indexed by the word at 0x464C + (D6w >> 5)), store
 * it + 10 at 0x45EC and copy the 26-byte block 0x4562.. to 0x45EE.. */
static uint32_t rd_latch_45ec_block(void)
{
    if ((uint16_t)vrd16(0x1000CAA6u) == 2) return 0x1FE0Au;
    charge(2);                                   /* tst, bmi */
    if ((int16_t)vrd16(0x1000C560u) < 0) return 0x1FE0Au;
    charge(14);                                  /* the rest, rts included */
    uint16_t d1 = (uint16_t)((uint16_t)d_reg(6) >> 5);
    uint16_t d0 = (uint16_t)vrd16(0x1000C64Cu + (uint32_t)(int16_t)d1);
    uint16_t d5 = (uint16_t)(vrd16(0x1EE2Eu + (uint32_t)((int16_t)d0 * 8)) + 10);
    vwr16(0x1000C5ECu, d5);
    for (uint32_t k = 0; k < 24; k += 4) vwr32(0x1000C5EEu + k, vrd32(0x1000C562u + k));
    vwr16(0x1000C606u, vrd16(0x1000C57Au));
    set_d16(1, d1); set_d16(0, d0); set_d16(5, d5);
    return RD_RTS;
}

/* FUN_0002708a: for each of the 16 slots whose bit is CLEAR in the mask at
 * 0x9104, reset its state: 0 at 0x903C[slot], -1 at 0x905E/0x907E/0x909E/
 * 0x90BE[slot] (words), and zero its 16-byte record at 0x91AE (+0,2,4 words,
 * +6 byte, +8,10,12 words). D6w ends at 0xFFFF, D5 = 0, D1w = the last cleared
 * slot * 16, D0w = the mask, as the 68K leaves them. */
static uint32_t rd_reset_disabled_slots(void)
{
    uint16_t mask = (uint16_t)vrd16(0x10009104u);
    set_d16(0, mask);
    charge(3);                                   /* move, moveq, moveq */
    for (int slot = 15; slot >= 0; slot--) {
        charge(3);                               /* btst, bne, dbf */
        if (mask & (1u << slot)) { if (slot) poll(); continue; }
        charge(14);
        uint32_t o2 = (uint32_t)(slot * 2);
        vwr16(0x1000903Cu + o2, 0);
        vwr16(0x1000905Eu + o2, 0xFFFF);
        vwr16(0x1000907Eu + o2, 0xFFFF);
        vwr16(0x1000909Eu + o2, 0xFFFF);
        vwr16(0x100090BEu + o2, 0xFFFF);
        uint32_t r = 0x100091AEu + (uint32_t)(slot << 4);
        vwr16(r + 0, 0); vwr16(r + 2, 0); vwr16(r + 4, 0);
        vwr8 (r + 6, 0);
        vwr16(r + 8, 0); vwr16(r + 10, 0); vwr16(r + 12, 0);
        set_d16(1, (uint16_t)(slot << 4));
        if (slot) poll();                        /* dbf taken */
    }
    charge(1);                                   /* rts */
    set_d(6, 0x0000FFFFu);
    set_d(5, 0);
    return RD_RTS;
}

/* FUN_0003124e: interpolate between a = 2*D1w and b = 2*D2w (sign-extended):
 *     t = the word at A5 + ((0x8000-as-int16 * D4w / D3w) & ~1) + 0x4000
 *     D0 = (a + b) / 2 + (int16)((b - a) * t >> 16)
 * with D1..D5 left as the 68K leaves them.
 * divs.w as a real 68K: when the quotient overflows 16 bits D0 is left unchanged
 * (the lifted build was fixed to match -- tools/gen/lift.py fix_word_divide);
 * rd_div_overflows counts how often that happens in play. */
long rd_div_overflows;
static uint32_t rd_interp_table(void)
{
    int32_t a = (int32_t)(int16_t)d_reg(1) * 2, b = (int32_t)(int16_t)d_reg(2) * 2;
    int32_t d3 = (int16_t)d_reg(3), d4 = (int16_t)d_reg(4);
    int32_t num = (int32_t)(int16_t)0x8000 * d4;                 /* muls.w: D0w = 0x8000 */
    int32_t q = (int32_t)SDIVREM(num, d3, '/'), r = (int32_t)SDIVREM(num, d3, '%');
    uint32_t d0 = (uint32_t)num;                                 /* divs.w overflow: D0 unchanged */
    if (q >= -32768 && q <= 32767) d0 = (uint32_t)r << 16 | ((uint32_t)q & 0xFFFF);
    else { extern int rd_real; if (rd_real) rd_div_overflows++; }
    uint16_t idx = (uint16_t)((((d0 & 0xFFFF) & 0xFFFE) + 0x4000) & 0xFFFF);
    int32_t t = (int16_t)vrd16(a_reg(5) + (uint32_t)(int16_t)idx);
    uint32_t prod = (uint32_t)(b - a) * (uint32_t)t;             /* muls.l: low 32 bits */
    int32_t hi = (int16_t)(prod >> 16);                          /* swap, ext.l */
    int32_t mid = (a + b) >> 1;                                  /* asr.l #1 */
    set_d(1, (uint32_t)a); set_d(2, (uint32_t)b);
    set_d(3, (uint32_t)d3); set_d(4, (uint32_t)d4);
    set_d(5, (uint32_t)mid);
    set_d(0, (uint32_t)(hi + mid));
    return RD_RTS;
}

/* FUN_00004cfa: by (the word at 0xDE0) & 3 -- a jump table in the code at
 * 0x4D12 sending 0, 1 and 3 straight to rts and 2 to the work below: if the
 * byte at 0x91A8 is set, rewrite bit 4 of the sound CPU's word 0x60004020.
 * The bit is set if (0x91A8 bit 3 is clear and the word at 0xA034 has a bit of
 * 0x18) or (bit 3 is set and (0xA04E != 1 or 0xA034 has bit 2)). */
static uint32_t rd_sound_flag_4020(void)
{
    uint16_t sel = (uint16_t)(vrd16(0x10008DE0u) & 3);
    set_d16(0, sel);
    uint16_t off = (uint16_t)vrd16(0x4D12u + sel * 2u);              /* the table, from ROM */
    set_d16(0, off);
    if (0x4D12u + off == 0x4D1Au) return RD_RTS;                     /* cost 5 */
    charge(2);                                                        /* tst.b, beq */
    uint8_t f = (uint8_t)vrd8(0x100091A8u);
    if (!f) return RD_RTS;                                            /* 7 */
    charge(5);                                                        /* move, bclr, moveq, btst, beq */
    uint32_t d1 = (d_reg(1) & 0xFFFF0000u) | vrd16(0x60004020u);
    d1 &= ~0x10u;
    uint32_t d0 = 0x18;
    int set;
    if (f & 8) {
        charge(3);                                                    /* moveq, cmpi, bne */
        d0 = 4;
        if ((uint16_t)vrd16(0x1000A04Eu) != 1) { set = 1; goto write; }
    }
    charge(2);                                                        /* and, beq */
    d0 = (d0 & 0xFFFF0000u) | ((d0 & vrd16(0x1000A034u)) & 0xFFFF);
    set = (d0 & 0xFFFF) != 0;
write:
    if (set) { d1 |= 0x10u; charge(1); }                              /* bset */
    charge(1);                                                        /* move.w to 0x60004020 */
    vwr16(0x60004020u, d1 & 0xFFFF);
    set_d(1, d1);
    set_d(0, d0);
    return RD_RTS;
}

/* FUN_00026e58: if the word at 0xCD6 is 1, gather the eight slots' display
 * values: 0xCD8[i] = word 0x910C[i], 0xCE8[i] = byte 0x912D[2i],
 * 0xCF8[i] = byte 0x91B5[16i], then copy the four words 0x914C.. to 0xCC8..
 * Registers end as the 68K leaves them (A0-A2 past the arrays, D0 = 8,
 * D1w = 0x70, D2 = the last byte, D6w = 0xFFFF). */
static uint32_t rd_gather_slot_display(void)
{
    charge(2);                                                        /* cmpi, bne */
    if ((uint16_t)vrd16(0x10008CD6u) != 1) { charge(1); return RD_RTS; }
    charge(7);                                                        /* 3 lea, 4 moveq */
    uint32_t d2 = 0;
    for (uint32_t i = 0; i < 8; i++) {
        vwr16(0x10008CD8u + i * 2, vrd16(0x1000910Cu + i * 2));
        d2 = vrd8(0x1000912Du + i * 2);
        vwr16(0x10008CE8u + i * 2, d2);
        d2 = vrd8(0x100091B5u + (i << 4));
        vwr16(0x10008CF8u + i * 2, d2);
        charge(9);
        if (i < 7) poll();                                            /* dbf taken */
    }
    charge(5);                                                        /* 4 moves, rts */
    vwr16(0x10008CC8u, vrd16(0x1000914Cu));
    vwr16(0x10008CCAu, vrd16(0x1000914Eu));
    vwr16(0x10008CCCu, vrd16(0x10009150u));
    vwr16(0x10008CCEu, vrd16(0x10009152u));
    set_a(0, 0x10008CE8u); set_a(1, 0x10008CF8u); set_a(2, 0x10008D08u);
    set_d(0, 8); set_d(1, 0x70); set_d(2, d2); set_d(6, 0x0000FFFFu);
    return RD_RTS;
}

/* FUN_0001a1b6: step a per-entry counter toward a target. d = word at
 * 0x96AE[D4w] - D0w: if d >= 0 and d < 2000, or d < 0 and d > -2000, return
 * (through the rts at 0x1A64A); otherwise count the word at 0x96AC[D4w] up
 * (d >= 2000) or down (d <= -2000). D1w = d. */
static uint32_t rd_step_toward(void)
{
    int16_t i = (int16_t)d_reg(4);
    uint16_t d = (uint16_t)(vrd16(0x100096AEu + (uint32_t)i) - (uint16_t)d_reg(0));
    set_d16(1, d);
    charge(2);                                                        /* cmpi, branch */
    uint32_t cnt = 0x100096ACu + (uint32_t)i;
    if (!(d & 0x8000)) {
        if ((int16_t)d < 2000) return 0x1A64Au;
        vwr16(cnt, vrd16(cnt) + 1);
    } else {
        if ((int16_t)d > -2000) return 0x1A64Au;
        vwr16(cnt, vrd16(cnt) - 1);
    }
    charge(2);                                                        /* addq/subq, rts */
    return RD_RTS;
}


/* ======================= batch 2 (2026-09-23) ======================= */

/* FUN_00004564 / FUN_00004120: two STATE DISPATCHERS of one shape. Each picks
 * one of two jump tables in its own code -- the second when the word at 0x802
 * is nonzero -- indexes it by the state word at 0x800 and continues at
 * table + entry (a tail jump; the targets are code not yet converted). The
 * tables stay in ROM: 0x457E / 0x458E and 0x413A / 0x414A. A0 = the table,
 * D0 = the entry, as the 68K leaves them. */
static uint32_t state_dispatch(uint32_t tbl_a, uint32_t tbl_b)
{
    uint32_t tbl = tbl_a;
    if (vrd16(0x10000802u)) { tbl = tbl_b; charge(1); }            /* the second lea */
    int16_t state = (int16_t)vrd16(0x10000800u);
    uint32_t entry = vrd32(tbl + (uint32_t)(state * 4));
    set_a(0, tbl);
    set_d(0, entry);
    return RD_JMP(tbl + entry);                                   /* jmp (0,A0,D0) */
}
static uint32_t rd_dispatch_4564(void) { return state_dispatch(0x457Eu, 0x458Eu); }
static uint32_t rd_dispatch_4120(void) { return state_dispatch(0x413Au, 0x414Au); }

/* FUN_00004988: an INPUT-SEQUENCE recogniser. Each call appends the input byte
 * (0x812) to a 64-entry history at 0x820 (write index 0x860) and advances a
 * match position (0x862) through the sequence at 0x49E0 (0xFF-terminated):
 * when the whole sequence has matched, bit 0 of the byte at 0x864 is set and
 * both positions restart. 0x864 is cleared on every call.
 * (The lifted build read the terminator from 0x49DE until the Ghidra
 * PC-relative fix in tools/gen/lift.py -- see there.) */
static uint32_t rd_input_sequence(void)
{
    vwr16(0x10000864u, 0);
    uint16_t d0 = (uint16_t)(vrd16(0x10000860u) & 0x3F);
    vwr8(0x10000820u + d0, vrd8(0x10000812u));
    uint16_t d1 = (uint16_t)(vrd16(0x10000862u) & 0x3F);
    uint8_t  d2 = (uint8_t)vrd8(0x49E0u + (uint32_t)(int16_t)d1);
    charge(9);
    if (d2 == (uint8_t)vrd8(0x10000820u + d0)) {
        d1 = (uint16_t)((d1 + 1) & 0xF);
        charge(4);                                               /* addq, andi, cmpi, bne */
        if ((uint8_t)vrd8(0x49E0u + (uint32_t)(int16_t)d1) == 0xFF) {
            vwr8(0x10000864u, vrd8(0x10000864u) | 1);
            d0 = 0; d1 = 0;
            charge(3);                                           /* bset, clr, clr */
        }
    }
    charge(2);                                                   /* tst, beq */
    if (d1) {
        d0 = (uint16_t)((d0 + 1) & 0x3F);
        charge(3);                                               /* addq, andi, bne */
        if (!d0) { d1 = 0; charge(1); }
    }
    vwr16(0x10000860u, d0);
    vwr16(0x10000862u, d1);
    charge(3);                                                   /* move, move, rts */
    set_d16(0, d0); set_d16(1, d1); set_d8(2, d2);
    return RD_RTS;
}

/* FUN_0001f674: pack the eight 28-byte records at 0x4560 into the output
 * list at the pointer held in 0xC46 -- skipping any whose first word is
 * negative -- as ten longs each (the first word, three longs, then six words
 * each widened to a long), then terminate the list with -1 and store the new
 * end. Registers end as the 68K leaves them. */
static uint32_t rd_pack_records_4560(void)
{
    uint32_t src = 0x1000C560u, out = vrd32(0x10008C46u);
    uint32_t d0 = 0, d1 = 0;
    charge(5);                                   /* lea, movea, moveq, moveq, move.w */
    for (int n = 7; n >= 0; n--) {
        d0 = vrd16(src);
        charge(2);                               /* move.w, bmi */
        if (!(d0 & 0x8000)) {
            vwr32(out, d0); out += 4;
            for (uint32_t k = 2; k <= 0xA; k += 4) { vwr32(out, vrd32(src + k)); out += 4; }
            for (uint32_t k = 0xE; k <= 0x18; k += 4) {
                d0 = vrd16(src + k); d1 = vrd16(src + k + 2);
                vwr32(out, d0); vwr32(out + 4, d1); out += 8;
            }
            d0 = vrd16(src + 0x1A);
            vwr32(out, d0); out += 4;
            charge(18);
        }
        src += 0x1C;
        charge(2);                               /* adda, dbf */
        if (n) poll();                           /* dbf taken */
    }
    vwr32(out, 0xFFFFFFFFu);
    vwr32(0x10008C46u, out);
    charge(3);                                   /* move.l, move.l, rts */
    set_a(0, src); set_a(1, out);
    set_d(0, d0); set_d(1, d1);
    set_d16(5, 0xFFFF);
    return RD_RTS;
}

/* sin/cos from the game's table at A5: for angle a, sin = -table[(-a) & ~1]
 * and cos = table[((-a) & ~1) + 0x4000]; D0w/D1w/D2w end as the 68K leaves them */
static void a5_sincos(uint32_t angle_addr, uint32_t out_sin, uint32_t out_cos, uint16_t *d0, uint16_t *d1, uint16_t *d2)
{
    uint16_t a = (uint16_t)((-(int32_t)vrd16(angle_addr)) & 0xFFFE);
    uint16_t sn = (uint16_t)vrd16(a_reg(5) + (uint32_t)(int16_t)a);
    a = (uint16_t)(a + 0x4000);
    uint16_t cs = (uint16_t)vrd16(a_reg(5) + (uint32_t)(int16_t)a);
    sn = (uint16_t)-sn;
    vwr16(out_sin, sn); vwr16(out_cos, cs);
    *d0 = a; *d1 = sn; *d2 = cs;
}

/* FUN_0001f574: unless the mode word 0x4AA6 is 2 or 0x4560 is negative (then
 * it continues at 0x1FE0A), build this entry's block at 0x45D0: the ROM-table
 * value (0x1EE2E, indexed by the word at 0x464C + (D6w >> 5)) + 9, the long at
 * 0x4562, (word 0x96BC[D6w] - 0xA108[(0xC40 & 7)] - 0xA0BE) * 25, the long at
 * 0x456A, -sin/cos of the angle at 0x96C6[D6w], the long at 0x4572, -sin/cos of
 * the angle at 0x96BA[D6w], and the word at 0x457A. */
static uint32_t rd_build_45d0_block(void)
{
    if ((uint16_t)vrd16(0x1000CAA6u) == 2) return 0x1FE0Au;
    charge(2);
    if ((int16_t)vrd16(0x1000C560u) < 0) return 0x1FE0Au;
    charge(37);
    int16_t i = (int16_t)d_reg(6);
    uint16_t d1 = (uint16_t)((uint16_t)d_reg(6) >> 5);
    uint16_t d0 = (uint16_t)vrd16(0x1000C64Cu + (uint32_t)(int16_t)d1);
    uint16_t d5 = (uint16_t)(vrd16(0x1EE2Eu + (uint32_t)((int16_t)d0 * 8)) + 9);
    vwr16(0x1000C5D0u, d5);
    vwr32(0x1000C5D2u, vrd32(0x1000C562u));
    d1 = (uint16_t)(vrd16(0x10008C40u) & 7);
    uint16_t v = (uint16_t)(vrd16(0x100096BCu + (uint32_t)i) - vrd16(0x1000A108u + d1 * 2u) - vrd16(0x1000A0BEu));
    uint32_t prod = (uint32_t)((int32_t)(int16_t)v * 25);            /* muls.w #0x19 */
    vwr32(0x1000C5D6u, prod);
    vwr32(0x1000C5DAu, vrd32(0x1000C56Au));
    uint16_t d2;
    a5_sincos(0x100096C6u + (uint32_t)i, 0x1000C5DEu, 0x1000C5E0u, &d0, &d1, &d2);
    vwr32(0x1000C5E2u, vrd32(0x1000C572u));
    a5_sincos(0x100096BAu + (uint32_t)i, 0x1000C5E6u, 0x1000C5E8u, &d0, &d1, &d2);
    vwr16(0x1000C5EAu, vrd16(0x1000C57Au));
    set_d(0, (prod & 0xFFFF0000u) | d0);
    set_d16(1, d1); set_d16(2, d2); set_d16(5, d5);
    return RD_RTS;
}


/* ======================= batch 3 (2026-09-23): I/O ======================= */

/* FUN_0000efee: unless already busy (0xF026), take a keycus sample: output
 * NOT(history[i] AND sample) to the keycus register 0x2000000A, then append
 * the sample to the 16-entry history at 0xF004 (index 0xF024, wrapping).
 * D0w = the output, D1w = the sample, D2w = the new index. */
static uint32_t rd_keycus_sample(void)
{
    charge(2);                                                    /* tst, bne */
    if (vrd16(0x1000F026u) != 0) return RD_RTS;
    charge(13);
    vwr16(0x1000F026u, 1);
    uint16_t i = (uint16_t)(vrd16(0x1000F024u) & 0xF);
    uint16_t hist = (uint16_t)vrd16(0x1000F004u + i * 2u);
    uint16_t sample = (uint16_t)vrd16(0x20000002u);               /* keycus read */
    uint16_t out = (uint16_t)~(hist & sample);
    vwr16(0x2000000Au, out);
    i = (uint16_t)((i + 1) & 0xF);
    vwr16(0x1000F024u, i);
    vwr16(0x1000F004u + i * 2u, sample);
    vwr16(0x1000F026u, 0);
    set_d16(0, out); set_d16(1, sample); set_d16(2, i);
    return RD_RTS;
}

/* FUN_00004640: drive the CPU LEDs (0x50000000, active low) from two flag bits:
 * bit 0 = bit 0 of the byte at 0x80A, inverted when (the word at 0x808 & 7) is
 * 0; bit 1 = bit 1 of the byte at 0x80B, inverted when the sound CPU's byte
 * 0x60004001 has none of bits 1-3 set. Each bit goes back to the byte at 0x80A
 * as it is made (the second overwrites the first), then the two are ORed into
 * the word at 0x80C and written, inverted. */
static uint32_t rd_cpu_leds(void)
{
    uint16_t d0 = (uint16_t)vrd16(0x1000080Cu);
    uint16_t d1 = (uint16_t)((d_reg(1) & 0xFF00u) | vrd8(0x1000080Au));
    uint16_t d2 = (uint16_t)(vrd16(0x10000808u) & 7);
    if (d2 == 0) { d1 = (uint16_t)~d1; charge(1); }
    d1 &= 1;
    vwr8(0x1000080Au, d1);
    d0 |= d1;
    d1 = (uint16_t)((d1 & 0xFF00u) | vrd8(0x1000080Bu));
    d2 = (uint16_t)(((d2 & 0xFF00u) | vrd8(0x60004001u)) & 0xE);
    if (d2 == 0) { d1 = (uint16_t)~d1; charge(1); }
    d1 &= 2;
    vwr8(0x1000080Au, d1);
    d0 |= d1;
    d0 = (uint16_t)~d0;
    vwr16(0x50000000u, d0);
    set_d16(0, d0); set_d16(1, d1); set_d16(2, d2);
    return RD_RTS;
}

/* FUN_00004d52: count calls in the sound CPU's word 0x6000400C; on the 5th and
 * later, hold the sound CPU (0x40000018 = 0), upload 0x9E words from 0x4D74
 * (byte-swapped: the C74 is little-endian) to its shared RAM at 0x60004200 and
 * 14 longs from 0x4EB0 to 0x60004080, terminate that with a zero word, and
 * release it (0x40000018 = 1). */
static uint32_t rd_sound_cpu_upload(void)
{
    uint16_t n = (uint16_t)(vrd16(0x6000400Cu) + 1);
    vwr16(0x6000400Cu, n);
    set_d16(0, n);
    charge(5);                                                    /* move, addq, move, cmpi, ble */
    if ((int16_t)n <= 4) { charge(1); return RD_RTS; }
    vwr8(0x40000018u, 0);                                         /* hold the sound CPU */
    charge(5);                                                    /* move.b, bra, lea, lea, move */
    uint32_t dst = 0x60004200u, src = 0x4D74u;
    uint16_t w = 0;
    for (int k = 0x9D; k >= 0; k--) {
        w = (uint16_t)vrd16(src); src += 2;
        w = (uint16_t)(w >> 8 | w << 8);                          /* ror.w #8 */
        vwr16(dst, w); dst += 2;
        charge(4);                                                /* move, ror, move, dbf */
        if (k) poll();
    }
    dst = 0x60004080u; src = 0x4EB0u;
    charge(3);                                                    /* lea, lea, moveq */
    for (int k = 6; k >= 0; k--) {
        vwr32(dst, vrd32(src)); vwr32(dst + 4, vrd32(src + 4));
        dst += 8; src += 8;
        charge(3);                                                /* move.l, move.l, dbf */
        if (k) poll();
    }
    vwr16(dst, 0);
    vwr8(0x40000018u, 1);                                         /* release it */
    charge(3);                                                    /* move.w, move.b, rts */
    set_a(0, dst); set_a(1, src);
    set_d(0, 0x0000FFFFu);                                        /* moveq 6 then dbf */
    set_d16(1, w);
    return RD_RTS;
}


/* ======================= batch 4 (2026-09-23): the small ones ======================= */

/* Empty functions -- a bare rts (nine of them in the drive: 13EA0, 17DEA, 1A22C,
 * 1E578, 205F4, 2089A, 211DE, 30BC2, 30BE6). */
static uint32_t rd_empty(void) { return RD_RTS; }

/* FUN_0000459e: count up the word at 0x808 (the frame/step counter FUN_00004640 masks with 7) */
static uint32_t rd_count_0808(void) { vwr16(0x10000808u, vrd16(0x10000808u) + 1); return RD_RTS; }

/* FUN_00005a9a: drop one stacked long, then return -- i.e. return to the CALLER'S
 * caller (a 68K idiom: an early exit through two levels) */
static uint32_t rd_return_two_levels(void) { set_a(7, a_reg(7) + 4); return RD_RTS; }

/* FUN_0000e64e / e6cc / e6d6: load parameters and branch (backwards, so the 68K
 * polls) into a shared routine: E64E -> 0xDBC8 with D0 = 15; E6CC / E6D6 ->
 * 0xE262 with D7w = 1 / 0x11 and D6 = 0x21 / 7 */
static uint32_t rd_dbc8_with_15(void)   { set_d(0, 15); return RD_JMP(0xDBC8u); }
static uint32_t rd_e262_1_21(void)      { set_d16(7, 1);    set_d(6, 0x21); return RD_JMP(0xE262u); }
static uint32_t rd_e262_11_7(void)      { set_d16(7, 0x11); set_d(6, 7);    return RD_JMP(0xE262u); }

/* FUN_0001e69e: fill the long at 0x91A4 with 0x77777777 */
static uint32_t rd_fill_91a4(void) { vwr32(0x100091A4u, 0x77777777u); return RD_RTS; }

/* Single writes to the sound CPU's shared RAM (0x600050xx: its command block) */
static uint32_t rd_snd_5016_40c4(void) { vwr16(0x60005016u, 0x40C4); return RD_RTS; }          /* FUN_0001e6ea */
static uint32_t rd_snd_501a_clear(void) { vwr16(0x6000501Au, 0); return RD_RTS; }              /* FUN_0001e926 */
static uint32_t rd_snd_501e_clear(void) { vwr16(0x6000501Eu, 0); return RD_RTS; }              /* FUN_00028646 */
static uint32_t rd_snd_5000_bit13(void) { vwr16(0x60005000u, vrd16(0x60005000u) | 0x2000); return RD_RTS; }  /* FUN_000286f2 */
static uint32_t rd_snd_5012_40cb(void) { vwr16(0x60005012u, 0x40CB); return RD_RTS; }          /* FUN_000286fc */


/* ======================= batch 5 (2026-09-23) ======================= */
/* NOT converted: FUN_0002b056 sets 0x806 = 3 and spins until an interrupt makes
 * it 1 -- a wait on the scheduler, which a check (interrupts held off) cannot
 * run; it stays lifted. */

/* FUN_00014626: if the input sequence completed (bit 0 of 0x864, FUN_00004988),
 * set the word at 0xA394 to 1; then fall into the code at 0x14634 */
static uint32_t rd_seq_done_flag(void)
{
    charge(2);                                                   /* btst, beq */
    if (vrd8(0x10000864u) & 1) { vwr16(0x1000A394u, 1); charge(1); }
    return 0x14634u;                                             /* falls through (no poll) */
}

/* FUN_00015876: terminate the output list at A3 with -1 and store A3 as its end (0xC46) */
static uint32_t rd_close_list(void)
{
    vwr32(a_reg(3), 0xFFFFFFFFu);
    vwr32(0x10008C46u, a_reg(3));
    return RD_RTS;
}

/* FUN_0002c1be: clear the word at 0xD006, then copy the byte at 0x1071 into 0xD007 */
static uint32_t rd_copy_1071_to_d007(void)
{
    vwr16(0x1000D006u, 0);
    vwr8(0x1000D007u, vrd8(0x10001071u));
    return RD_RTS;
}

/* FUN_000058ee: zero the last byte of three palette areas (0x9002FFFF, 0x90037FFF, 0x9003FFFF) */
static uint32_t rd_palette_end_bytes(void)
{
    vwr8(0x9002FFFFu, 0); vwr8(0x90037FFFu, 0); vwr8(0x9003FFFFu, 0);
    return RD_RTS;
}

/* FUN_0000c656: a delay loop -- 60 x (D1 = D1w * 0xFFFF) */
static uint32_t rd_delay_60(void)
{
    uint32_t d1 = d_reg(1);
    charge(1);                                                   /* moveq */
    for (int k = 0x3B; k >= 0; k--) {
        d1 = (d1 & 0xFFFFu) * 0xFFFFu;                           /* mulu.w #-1 */
        charge(2);                                               /* mulu, dbf */
        if (k) poll();
    }
    charge(1);                                                   /* rts */
    set_d(1, d1);
    set_d(0, 0x0000FFFFu);
    return RD_RTS;
}

/* FUN_0000ecce: clear the three words 0xA432..0xA436 */
static uint32_t rd_clear_a432(void)
{
    vwr16(0x1000A432u, 0); vwr16(0x1000A434u, 0); vwr16(0x1000A436u, 0);
    return RD_RTS;
}

/* FUN_0000fe14: copy 0xA21C to 0xA21E and clear D0; then, if the word at 0xA340
 * is zero, continue at 0xFE34, otherwise fall into 0xFE22 */
static uint32_t rd_fe14(void)
{
    vwr16(0x1000A21Eu, vrd16(0x1000A21Cu));
    set_d(0, 0);
    return vrd16(0x1000A340u) == 0 ? 0xFE34u : 0xFE22u;
}

/* FUN_000180f4: 0xA048 = the ROM table at 0x18104 [word 0xA046] (A0 = the table) */
static uint32_t rd_lookup_a048(void)
{
    uint16_t i = (uint16_t)vrd16(0x1000A046u);
    set_d16(0, i);
    set_a(0, 0x18104u);
    vwr16(0x1000A048u, vrd16(0x18104u + (uint32_t)((int16_t)i * 2)));
    return RD_RTS;
}

/* FUN_0001e99c: unless the sound CPU's word 0x6000501A is already 0x80B1, set it to 0x40B1 */
static uint32_t rd_snd_501a_40b1(void)
{
    if ((uint16_t)vrd16(0x6000501Au) == 0x80B1) return RD_RTS;
    vwr16(0x6000501Au, 0x40B1);
    charge(1);
    return RD_RTS;
}

/* FUN_000257c2: A0 = the pointer in the ROM table at 0x2580E [word 0xA048] (8-byte
 * entries); continue at 0x257D8 */
static uint32_t rd_257c2(void)
{
    uint16_t i = (uint16_t)vrd16(0x1000A048u);
    set_d16(0, i);
    set_a(0, vrd32(0x2580Eu + (uint32_t)((int16_t)i * 8)));
    return 0x257D8u;
}

/* FUN_0001228c: count the word at 0xA32C down to zero (D0w = its value, as read or new) */
static uint32_t rd_countdown_a32c(void)
{
    uint16_t v = (uint16_t)vrd16(0x1000A32Cu);
    if (v) { v--; vwr16(0x1000A32Cu, v); charge(2); }
    set_d16(0, v);
    return RD_RTS;
}

/* FUN_0001468a / 0001469c: set the word at +0x2A of the current 128-byte car record
 * (A6 + (0xC40) * 128, A4 = the record) to -1 / 0 */
static void car_record_2a(uint16_t v)
{
    uint16_t d0 = (uint16_t)(vrd16(0x10008C40u) << 7);
    uint32_t rec = 0x10008000u + (uint32_t)(int16_t)d0;
    set_d16(0, d0); set_a(4, rec);
    vwr16(rec + 0x2A, v);
}
static uint32_t rd_car_2a_set(void)   { car_record_2a(0xFFFF); return RD_RTS; }
static uint32_t rd_car_2a_clear(void) { car_record_2a(0);      return RD_RTS; }

/* FUN_0001b2e2: if the word at 0x9100 is nonzero, D0w = (word 0x91A0) << 6 */
static uint32_t rd_b2e2(void)
{
    if (vrd16(0x10009100u)) { set_d16(0, (uint16_t)(vrd16(0x100091A0u) << 6)); charge(2); }
    return RD_RTS;
}

/* FUN_00005742: fill 0x800 longs at 0x90012000 (depth-cue RAM) with 0x30303030 */
static uint32_t rd_fill_czram(void)
{
    uint32_t a = 0x90012000u;
    charge(3);
    for (int k = 0x7FF; k >= 0; k--) { vwr32(a, 0x30303030u); a += 4; charge(2); if (k) poll(); }
    charge(1);
    set_a(0, a); set_d16(0, 0xFFFF); set_d(1, 0x30303030u);
    return RD_RTS;
}

/* FUN_0000575a: fill the 0x1000-word text tilemap at 0x9009E000 with 0x20 (spaces) */
static uint32_t rd_clear_text(void)
{
    uint32_t a = 0x9009E000u;
    charge(3);
    for (int k = 0xFFF; k >= 0; k--) { vwr16(a, 0x20); a += 2; charge(2); if (k) poll(); }
    charge(1);
    set_a(1, a); set_d16(1, 0xFFFF); set_d16(0, 0x20);
    return RD_RTS;
}

/* FUN_0000b89a / 0000b8b0: set the mixer words at 0x90020011/13/15 to 0 / 0x100 */
static uint32_t rd_mixer_11_0(void)
{
    set_a(0, 0x90020000u); set_d(0, 0);
    vwr16(0x90020011u, 0); vwr16(0x90020013u, 0); vwr16(0x90020015u, 0);
    return RD_RTS;
}
static uint32_t rd_mixer_11_100(void)
{
    set_a(0, 0x90020000u); set_d16(0, 0x100);
    vwr16(0x90020011u, 0x100); vwr16(0x90020013u, 0x100); vwr16(0x90020015u, 0x100);
    return RD_RTS;
}

/* FUN_0000ef9c: keycus: write NOT(two samples ANDed) to 0x2000000A */
static uint32_t rd_keycus_pair(void)
{
    uint16_t a = (uint16_t)vrd16(0x20000002u);
    uint16_t b = (uint16_t)vrd16(0x20000002u);
    b = (uint16_t)~(a & b);
    vwr16(0x2000000Au, b);
    set_d16(4, a); set_d16(5, b);
    return RD_RTS;
}


/* ======================= batch 6 (2026-09-23) ======================= */

/* Display-list headers at the output pointer 0xC46: 0x8002 then the current car
 * number (0xC40) as a long. FUN_00015862 leaves the pointer in A3 only;
 * FUN_0001800e masks the car number to 0..7 and stores the pointer back;
 * FUN_0001226e also terminates the list with -1 and stores it back. */
static uint32_t rd_dl_header(void)
{
    uint32_t a3 = vrd32(0x10008C46u);
    vwr32(a3, 0x8002); a3 += 4;
    uint32_t d0 = vrd16(0x10008C40u);
    vwr32(a3, d0); a3 += 4;
    set_a(3, a3); set_d(0, d0);
    return RD_RTS;
}
static uint32_t rd_dl_header_masked(void)
{
    uint32_t a3 = vrd32(0x10008C46u);
    vwr32(a3, 0x8002); a3 += 4;
    uint32_t d0 = ((d_reg(0) & 0xFFFF0000u) | vrd16(0x10008C40u)) & 7;
    vwr32(a3, d0); a3 += 4;
    vwr32(0x10008C46u, a3);
    set_a(3, a3); set_d(0, d0);
    return RD_RTS;
}
static uint32_t rd_dl_header_closed(void)
{
    uint32_t a3 = vrd32(0x10008C46u);
    vwr32(a3, 0x8002); a3 += 4;
    uint32_t d0 = vrd16(0x10008C40u);
    vwr32(a3, d0); a3 += 4;
    vwr32(a3, 0xFFFFFFFFu);
    vwr32(0x10008C46u, a3);
    set_a(3, a3); set_d(0, d0);
    return RD_RTS;
}

/* FUN_000180da: clear bit 0 of the flag byte at +0x16C4 of each of the 16
 * 64-byte entries (D6w ends 0x400, D7w 0xFFFF) */
static uint32_t rd_clear_16c4_bit0(void)
{
    uint16_t d6 = 0;
    charge(2);
    for (int k = 15; k >= 0; k--) {
        uint32_t a = 0x100096C4u + (uint32_t)(int16_t)d6;
        vwr8(a, vrd8(a) & ~1u);
        d6 += 0x40;
        charge(3);                                                /* bclr, addi, dbf */
        if (k) poll();
    }
    charge(1);
    set_d16(6, d6); set_d16(7, 0xFFFF);
    return RD_RTS;
}

/* FUN_0001e760: reset the sound command block: 0x501C/1A/16/18 = 0, 0x5116 = 0xFF */
static uint32_t rd_snd_reset_block(void)
{
    vwr16(0x6000501Cu, 0); vwr16(0x6000501Au, 0); vwr16(0x60005016u, 0); vwr16(0x60005018u, 0);
    vwr16(0x60005116u, 0xFF);
    return RD_RTS;
}

/* FUN_0001fe0c: copy the 12 words at 0x1FE20 (ROM) to 0xCA7C */
static uint32_t rd_init_ca7c(void)
{
    uint32_t src = 0x1FE20u, dst = 0x1000CA7Cu;
    charge(3);
    for (int k = 0xB; k >= 0; k--) { vwr16(dst, vrd16(src)); src += 2; dst += 2; charge(2); if (k) poll(); }
    charge(1);
    set_a(0, src); set_a(1, dst); set_d16(7, 0xFFFF);
    return RD_RTS;
}

/* FUN_00028786: unless the sound CPU's word 0x5000 is 0x405D or 0x805D, set it to 0x405D */
static uint32_t rd_snd_5000_405d(void)
{
    uint16_t v = (uint16_t)vrd16(0x60005000u);
    if (v == 0x405D) return RD_RTS;
    charge(2);
    if (v == 0x805D) return RD_RTS;
    charge(1);
    vwr16(0x60005000u, 0x405D);
    return RD_RTS;
}

/* FUN_0002fb82: restore the 4 KB block at 0xF000 from its copy at 0x1F000, then
 * continue at the address in A4 (a continuation the caller supplied) */
static uint32_t rd_restore_f000(void)
{
    uint32_t src = 0x1001F000u, dst = 0x1000F000u;
    charge(3);
    for (int k = 0x3FF; k >= 0; k--) { vwr32(dst, vrd32(src)); src += 4; dst += 4; charge(2); if (k) poll(); }
    charge(1);                                                    /* jmp (A4) */
    set_a(2, src); set_a(3, dst); set_d16(6, 0xFFFF);
    return RD_JMP(a_reg(4));
}

/* FUN_0002fb34: save the 4 KB block at 0xF000 to 0x1000, then continue at
 * 0x2FD44 with A4 = the old A3 and A3 = 0x2FB56 (the continuation) */
static uint32_t rd_save_f000(void)
{
    uint32_t src = 0x1000F000u, dst = 0x10001000u;
    charge(3);
    for (int k = 0x3FF; k >= 0; k--) { vwr32(dst, vrd32(src)); src += 4; dst += 4; charge(2); if (k) poll(); }
    charge(3);                                                    /* movea, lea, jmp */
    set_a(2, src); set_a(4, a_reg(3)); set_a(3, 0x2FB56u); set_d16(6, 0xFFFF);
    return 0x2FD44u;
}

/* FUN_0001460c: clear the six words 0xA0BC..0xA0C6 */
static uint32_t rd_clear_a0bc(void)
{
    for (uint32_t a = 0x1000A0BCu; a <= 0x1000A0C6u; a += 2) vwr16(a, 0);
    return RD_RTS;
}

/* FUN_0001b2f0: set bit 7 of the flag byte at +0x16C4 of entry 0, or -- if the
 * word at 0x9100 is zero -- of the entry (word 0x91A0) */
static uint32_t rd_set_16c4_bit7(void)
{
    uint32_t d0 = 0;
    charge(3);
    if (vrd16(0x10009100u) == 0) { d0 = (uint16_t)(vrd16(0x100091A0u) << 6); charge(2); }
    uint32_t a = 0x100096C4u + (uint32_t)(int16_t)d0;
    vwr8(a, vrd8(a) | 0x80);
    set_d(0, d0);
    return RD_RTS;
}

/* FUN_0002147a: the long at 0xC346 and word at 0xC34A = 0xFFFF0000 / 0xFFFF... when
 * the mode word 0xA046 is 3, else 0 */
static uint32_t rd_c346_by_mode(void)
{
    uint32_t d0 = 0;
    charge(3);
    if (vrd16(0x1000A046u) == 3) { d0 = 0xFFFF0000u; charge(1); }
    vwr32(0x1000C346u, d0);
    vwr16(0x1000C34Au, d0 & 0xFFFF);
    set_d(0, d0);
    return RD_RTS;
}

/* FUN_00028c82: 0xD7D8 = the low nibble of the ROM byte 0xECDC[(word 0xA046) * 64] */
static uint32_t rd_d7d8_from_table(void)
{
    uint16_t d0 = (uint16_t)(vrd16(0x1000A046u) << 6);
    d0 = (uint16_t)((d0 & 0xFF00u) | vrd8(0xECDCu + (uint32_t)(int16_t)d0)) & 0xF;
    vwr16(0x1000D7D8u, d0);
    set_a(0, 0xECDCu); set_d16(0, d0);
    return RD_RTS;
}

/* FUN_0000ae04: a state dispatcher: table-of-tables at 0xAE22 by (0xDA2) & 1, entry
 * by (0xDA0) & 7, continue at table + entry */
static uint32_t rd_dispatch_ae04(void)
{
    uint16_t i = (uint16_t)(vrd16(0x10008DA2u) & 1);
    uint32_t tbl = vrd32(0xAE22u + (uint32_t)(int16_t)i * 4u);
    uint16_t j = (uint16_t)(vrd16(0x10008DA0u) & 7);
    uint32_t entry = vrd32(tbl + (uint32_t)(int16_t)j * 4u);
    set_a(0, tbl); set_d(0, entry);
    return RD_JMP(tbl + entry);
}

/* FUN_00017fae: 0x48D4 = max(0, word 0xDD4 - word 0x48D2); then 0x48D2 = word 0xDD4 */
static uint32_t rd_delta_dd4(void)
{
    uint16_t cur = (uint16_t)vrd16(0x10008DD4u);
    uint16_t dl = (uint16_t)(cur - vrd16(0x1000C8D2u));
    uint32_t d0 = (d_reg(0) & 0xFFFF0000u) | dl;
    if (dl & 0x8000) { d0 = 0; charge(1); }                       /* moveq 0 */
    vwr16(0x1000C8D4u, d0 & 0xFFFF);
    vwr16(0x1000C8D2u, cur);
    set_d(0, d0); set_d16(1, cur);
    return RD_RTS;
}

/* FUN_0001ea60: when the counter 0x49CE is below 3 and the sound CPU's word
 * 0x60005018 has bit 15 clear, write D1w + 0x40B4 there */
static uint32_t rd_snd_5018_d1(void)
{
    charge(2);
    if ((uint16_t)vrd16(0x1000C9CEu) >= 3) return RD_RTS;
    charge(3);
    uint16_t d0 = (uint16_t)(vrd16(0x60005018u) & 0x8000);
    set_d16(0, d0);
    if (d0) return RD_RTS;
    charge(2);
    uint16_t d1 = (uint16_t)(d_reg(1) + 0x40B4);
    set_d16(1, d1);
    vwr16(0x60005018u, d1);
    return RD_RTS;
}

/* FUN_0002bc68: copy the 24 bytes at 0x10A0 to 0xD20A */
static uint32_t rd_copy_10a0(void)
{
    for (uint32_t k = 0; k < 24; k += 4) vwr32(0x1000D20Au + k, vrd32(0x100010A0u + k));
    set_a(0, 0x100010A0u);
    return RD_RTS;
}

/* FUN_00031724 / 00031742: D5w = D7w * 64 and D1 = the long at 0x317E6 + 4*i,
 * i = (byte 0x1061) & 7 or 0; continue at 0x3177A (a shared tail) */
static uint32_t rd_317_entry(uint16_t i)
{
    set_d16(5, (uint16_t)(d_reg(7) << 6));
    set_d16(3, i);
    set_d(1, vrd32(0x317E6u + (uint32_t)(int16_t)i * 4u));
    return 0x3177Au;
}
static uint32_t rd_31724(void) { return rd_317_entry((uint16_t)(((d_reg(3) & 0xFF00u) | vrd8(0x10001061u)) & 7)); }
static uint32_t rd_31742(void) { return rd_317_entry(0); }

/* FUN_00017dec: D1 = 0x10000000 / (word at 0xECDC + (word 0xA046)*64 + 6), unsigned,
 * stored at 0xA052 */
static uint32_t rd_recip_a052(void)
{
    uint16_t d0 = (uint16_t)(vrd16(0x1000A046u) << 6);
    uint32_t div = vrd16(0xECDCu + 6 + (uint32_t)(int16_t)d0);
    uint32_t q = (uint32_t)UDIVREM(0x10000000u, div, '/');
    vwr32(0x1000A052u, q);
    set_a(0, 0xECDCu); set_d(0, div); set_d(1, q);
    return RD_RTS;
}


/* ======================= batch 7 (2026-09-23) ======================= */

/* FUN_0001bd68: 0xC064 = (word 0xC024 * 0x6666) >> 16 and 0xC066 = (word * 0x9999) >> 16
 * (x * 0.4 and x * 0.6; D0 = the second product, halves swapped, as the 68K leaves it) */
static uint32_t rd_split_c024(void)
{
    uint16_t x = (uint16_t)vrd16(0x1000C024u);
    uint32_t p1 = (uint32_t)x * 0x6666u;
    vwr16(0x1000C064u, p1 >> 16);
    uint32_t p2 = (uint32_t)x * 0x9999u;
    vwr16(0x1000C066u, p2 >> 16);
    set_d(0, p2 << 16 | p2 >> 16);
    return RD_RTS;
}

/* FUN_0002981c / 00029838: D0 = 0 when the mode word 0xA046 is 0 / 4 and the
 * word 0xA04C lies in [0x41A, 0x4B0] / [0, 0x1F4]; otherwise D0 is left alone
 * (a "within the window" test the caller reads back through D0) */
static uint32_t in_window(uint16_t mode, int16_t lo, int16_t hi)
{
    charge(2);                                                    /* cmpi, bne */
    if ((uint16_t)vrd16(0x1000A046u) != mode) return RD_RTS;
    charge(3);                                                    /* move, cmpi, blt */
    int16_t v = (int16_t)vrd16(0x1000A04Cu);
    set_d16(0, (uint16_t)v);
    if (v < lo) return RD_RTS;
    charge(2);                                                    /* cmpi, bgt */
    if (v > hi) return RD_RTS;
    charge(1);
    set_d(0, 0);
    return RD_RTS;
}
static uint32_t rd_window_mode0(void) { return in_window(0, 0x41A, 0x4B0); }
static uint32_t rd_window_mode4(void) { return in_window(4, 0, 0x1F4); }

/* FUN_00005706: write the three bytes of the ROM entry 0x5732[(word 0xA048) & 3]
 * (4-byte entries) to the mixer at 0x90020100, 0x90020180 and 0x90020200 */
static uint32_t rd_mixer_from_table(void)
{
    uint16_t i = (uint16_t)(vrd16(0x1000A048u) & 3);
    uint32_t e = 0x5732u + (uint32_t)(int16_t)i * 4u;
    vwr8(0x90020100u, vrd8(e)); vwr8(0x90020180u, vrd8(e + 1)); vwr8(0x90020200u, vrd8(e + 2));
    set_a(3, 0x5732u); set_a(0, 0x90020101u); set_a(1, 0x90020181u); set_a(2, 0x90020201u);
    set_d16(0, i);
    return RD_RTS;
}

/* FUN_00005876: zero the last 256 bytes of the three palette areas (0x9002FF00,
 * 0x90037F00, 0x9003FF00), then continue at 0xC74E */
static uint32_t rd_palette_tails(void)
{
    uint32_t a0 = 0x9002FF00u, a1 = 0x90037F00u, a2 = 0x9003FF00u;
    charge(5);
    for (int k = 0xFF; k >= 0; k--) {
        vwr8(a0++, 0); vwr8(a1++, 0); vwr8(a2++, 0);
        charge(4);
        if (k) poll();
    }
    charge(1);                                                    /* jmp (absolute: no poll) */
    set_a(0, a0); set_a(1, a1); set_a(2, a2); set_d(0, 0); set_d16(1, 0xFFFF);
    return 0xC74Eu;
}

/* FUN_0000a88e / 0000a8c8: split D1 into eight digits, one per nibble of D0 (most
 * significant first), by the divisor table at 0xA8A8 -- 2160000, 216000, 36000,
 * 3600, 600, 60, 10, 1: frames at 60 per second into minute/second/frame digits
 * -- or at 0xA8E2 -- 6000000 ... 10, 1. D1 ends as the last remainder. */
static uint32_t pack_digits(uint32_t tbl)
{
    uint32_t d0 = 0, d1 = d_reg(1);
    charge(3);
    for (int k = 7; k >= 0; k--) {
        d0 = d0 << 4 | d0 >> 28;                                 /* rol.l #4 */
        uint32_t div = vrd32(tbl); tbl += 4;
        uint32_t q = (uint32_t)UDIVREM(d1, div, '/');
        d1 = (uint32_t)UDIVREM(d1, div, '%');
        d0 |= q;
        charge(6);
        if (k) poll();
    }
    set_d(0, d0); set_d(1, d1); set_d(2, 0x0000FFFFu); set_d(3, 0); set_a(1, tbl);
    return RD_RTS;
}
static uint32_t rd_time_digits(void)    { return pack_digits(0xA8A8u); }
static uint32_t rd_decimal_digits(void) { return pack_digits(0xA8E2u); }

/* FUN_0000fe60: 0xA224 = the low nibble of its old value, unless that is 7 -- then
 * of (the random word 0xF000) + 1, + 2 ... until it is not 7 */
static uint32_t rd_pick_a224(void)
{
    uint16_t d1 = (uint16_t)vrd16(0x1000F000u), d0 = (uint16_t)vrd16(0x1000A224u);
    charge(2);
    for (;;) {
        d0 &= 0xF;
        charge(3);                                                /* andi, cmpi, bne */
        if (d0 != 7) break;
        d1++; d0 = d1;
        charge(3);                                                /* addq, move, bra */
        poll();                                                   /* bra backwards */
    }
    vwr16(0x1000A224u, d0);
    set_d16(0, d0); set_d16(1, d1);
    return RD_RTS;
}

/* FUN_00028db2: if the flag 0xD7EA is set: when 0xD7EC + 1 < 0xD7CC, set bit 1
 * of 0xD880 and count 0xD7EC up; then clear the flag */
static uint32_t rd_d7ea_step(void)
{
    charge(2);
    if (!vrd16(0x1000D7EAu)) return RD_RTS;
    charge(5);                                                    /* move, addq, cmp, bge, clr */
    uint16_t d0 = (uint16_t)(vrd16(0x1000D7ECu) + 1);
    set_d16(0, d0);
    if ((int16_t)d0 < (int16_t)vrd16(0x1000D7CCu)) {
        vwr8(0x1000D880u, vrd8(0x1000D880u) | 2);
        vwr16(0x1000D7ECu, vrd16(0x1000D7ECu) + 1);
        charge(2);
    }
    vwr16(0x1000D7EAu, 0);
    return RD_RTS;
}

/* FUN_0002988e: the first of bits 0..2 set in 0xD882 selects a branch of the
 * jump table at 0x298AC; none set: return */
static uint32_t rd_dispatch_d882(void)
{
    uint8_t f = (uint8_t)vrd8(0x1000D882u);
    uint16_t i = 0;
    charge(1);                                                    /* moveq */
    for (;;) {
        charge(2);                                                /* btst, bne */
        if (f & (1u << (i & 7))) break;
        i++;
        charge(3);                                                /* addq, cmpi, blt */
        if (i >= 3) { set_d(0, i); charge(1); return RD_RTS; }       /* rts */
        poll();
    }
    charge(3);                                                    /* lea, move, jmp */
    int16_t off = (int16_t)vrd16(0x298ACu + i * 2u);
    set_a(0, 0x298ACu);
    set_d(0, (uint32_t)(uint16_t)off);
    return RD_JMP(0x298ACu + (uint32_t)(int32_t)off);
}

/* FUN_0000fe22: 0xA21C = (0x4006 + 1) & 7 normally, or D0w & 7 unchanged when
 * 0xDDE is 0, 0x4790 is set and 0x4796 is set */
static uint32_t rd_a21c(void)
{
    uint16_t d0 = (uint16_t)d_reg(0);
    int keep = 0;
    charge(2);
    if (!vrd16(0x10008DDEu)) {
        charge(2);
        if (vrd16(0x1000C790u)) {
            charge(2);
            if (vrd16(0x1000C796u)) keep = 1;
        }
    }
    if (!keep) { d0 = (uint16_t)(vrd16(0x1000C006u) + 1); charge(2); }
    d0 &= 7;
    vwr16(0x1000A21Cu, d0);
    set_d16(0, d0);
    return RD_RTS;
}

/* FUN_00014532: step the word 0xA04A toward 0xA04C - D2w by one, unless within
 * 3000 (then continue at 0x14B6E) */
static uint32_t rd_step_a04a(void)
{
    uint16_t d = (uint16_t)(vrd16(0x1000A04Cu) - (uint16_t)d_reg(2));
    set_d16(0, d);
    charge(2);
    if (!(d & 0x8000)) {
        if (d < 3000) return 0x14B6Eu;
        vwr16(0x1000A04Au, vrd16(0x1000A04Au) + 1);
    } else {
        if ((int16_t)d > -3000) return 0x14B6Eu;
        vwr16(0x1000A04Au, vrd16(0x1000A04Au) - 1);
    }
    charge(2);
    return RD_RTS;
}

/* FUN_00017dca: 0xC1E8 = 2 * ((word 0xC1E4 * -sin(angle 0xC1FC)) doubled, high word)
 * (sin from the table at A5; D0/D1 end as the 68K leaves them) */
static uint32_t rd_c1e8(void)
{
    uint16_t a = (uint16_t)(vrd16(0x1000C1FCu) & 0xFFFE);
    uint16_t s = (uint16_t)-(int16_t)vrd16(a_reg(5) + (uint32_t)(int16_t)a);
    uint32_t d1 = (uint32_t)((int32_t)(int16_t)vrd16(0x1000C1E4u) * (int32_t)(int16_t)s);
    d1 += d1;
    d1 = d1 << 16 | d1 >> 16;
    d1 = (d1 & 0xFFFF0000u) | (uint16_t)(d1 + d1);
    vwr16(0x1000C1E8u, d1 & 0xFFFF);
    set_d16(0, s); set_d(1, d1);
    return RD_RTS;
}

/* FUN_00019f80: for D7w+1 entries from D6w (64-byte stride), where bit 7 of the
 * flag byte at +0x16C4 is set, copy the word at +0xCAAC to +0x96DC */
static uint32_t rd_copy_flagged(void)
{
    uint16_t d6 = (uint16_t)vrd16(0x1000CA94u), d7 = (uint16_t)vrd16(0x1000CA96u);
    uint16_t d0 = (uint16_t)d_reg(0);
    charge(2);
    for (;;) {
        charge(2);                                                /* btst, beq */
        if (vrd8(0x100096C4u + (uint32_t)(int16_t)d6) & 0x80) {
            d0 = (uint16_t)vrd16(0x1000CAACu + (uint32_t)(int16_t)d6);
            vwr16(0x100096DCu + (uint32_t)(int16_t)d6, d0);
            charge(4);
        }
        d6 += 0x40;
        charge(2);                                                /* addi, dbf */
        if (d7-- == 0) break;
        poll();
    }
    set_d16(0, d0); set_d16(6, d6); set_d16(7, 0xFFFF);
    return RD_RTS;
}

/* FUN_0001b09c: fill 16 longs at 0xC8F4 and 8 at 0xC880 with D0, and store D0w
 * at 0xC988 and 0xA04E */
static uint32_t rd_fill_c8f4(void)
{
    uint32_t d0 = d_reg(0), a = 0x1000C8F4u;
    charge(2);
    for (int k = 15; k >= 0; k--) { vwr32(a, d0); a += 4; charge(2); if (k) poll(); }
    a = 0x1000C880u;
    charge(2);
    for (int k = 7; k >= 0; k--) { vwr32(a, d0); a += 4; charge(2); if (k) poll(); }
    vwr16(0x1000C988u, d0 & 0xFFFF);
    vwr16(0x1000A04Eu, d0 & 0xFFFF);
    charge(3);
    set_a(0, a); set_d16(7, 0xFFFF);
    return RD_RTS;
}

/* FUN_0001bdca: v = (long 0xC26E) >> 14; 0xC076 = (v * word 0xC182) >> 16 and
 * 0xC078 = (v * word 0xC184) >> 16 (16-bit v) */
static uint32_t rd_scale_c26e(void)
{
    uint32_t d0 = vrd32(0x1000C26Eu) >> 14;
    uint16_t v = (uint16_t)d0;
    d0 = (uint32_t)v * (uint16_t)vrd16(0x1000C182u);
    d0 = d0 << 16 | d0 >> 16;
    vwr16(0x1000C076u, d0 & 0xFFFF);
    uint32_t d1 = (uint32_t)v * (uint16_t)vrd16(0x1000C184u);
    d1 = d1 << 16 | d1 >> 16;
    vwr16(0x1000C078u, d1 & 0xFFFF);
    set_d(0, d0); set_d(1, d1);
    return RD_RTS;
}

/* FUN_00028d8a: if the flag 0xD7E0 is set: when 0xA04A < 0xD7CC set bit 0 of
 * 0xD880 and count 0xD7E2 up, else set 0xD7E4 = 1; then clear the flag */
static uint32_t rd_d7e0_step(void)
{
    charge(2);
    if (!vrd16(0x1000D7E0u)) return RD_RTS;
    charge(6);                                                    /* move, cmp, blt, 2 of the branch, clr */
    uint16_t d0 = (uint16_t)vrd16(0x1000A04Au);
    set_d16(0, d0);
    if ((int16_t)d0 < (int16_t)vrd16(0x1000D7CCu)) {
        vwr8(0x1000D880u, vrd8(0x1000D880u) | 1);
        vwr16(0x1000D7E2u, vrd16(0x1000D7E2u) + 1);
    } else vwr16(0x1000D7E4u, 1);
    vwr16(0x1000D7E0u, 0);
    return RD_RTS;
}

/* FUN_0002bab4: send the bytes at 0x104B..0x104E to the sound CPU's words 0x4022..0x4028 */
static uint32_t rd_snd_4022_from_104b(void)
{
    uint16_t d0 = 0;
    for (uint32_t k = 0; k < 4; k++) { d0 = (uint16_t)vrd8(0x1000104Bu + k); vwr16(0x60004022u + k * 2, d0); }
    set_a(0, 0x10001040u); set_d16(0, d0);
    return RD_RTS;
}


/* ======================= batch 8 (2026-09-23) ======================= */
/* Bodies below charge EVERY instruction they stand for (rts included), so their
 * table cost is 0. */

#define A6W(off) (0x10008000u + (uint32_t)(off))       /* (d16,A6), d16 < 0x8000 */

/* FUN_000046d6: read the 2-bit cabinet switch at I/O 0x60004030 (bits 0 and 5),
 * look its type up at 0x46FE and store it at 0xDE0, and that type's value from
 * 0x4706 at 0xDDE */
static uint32_t rd_cabinet_type(void)
{
    uint8_t b = (uint8_t)vrd8(0x60004030u);
    uint16_t idx = (uint16_t)((b & 1) | ((b >> 4) & 2));
    uint16_t type = (uint16_t)vrd16(0x46FEu + idx * 2u);
    vwr16(A6W(0xDE0), type);
    vwr16(A6W(0xDDE), vrd16(0x4706u + (uint32_t)(int16_t)type * 2u));
    set_d16(0, type); set_d16(1, idx);
    charge(12);
    return RD_RTS;
}

/* FUN_0000b6d0: append a 6-long record [0x8000, n, 0x828, n, n, n] to the list at
 * the cursor 0xC46 (n = the word 0xC40), terminate it with -1 and advance the cursor */
static uint32_t rd_append_8000(void)
{
    uint32_t a3 = vrd32(A6W(0xC46)), n = (uint16_t)vrd16(A6W(0xC40));
    vwr32(a3, 0x8000u); vwr32(a3 + 4, n); vwr32(a3 + 8, 0x828u);
    vwr32(a3 + 12, n); vwr32(a3 + 16, n); vwr32(a3 + 20, n);
    a3 += 24;
    vwr32(a3, 0xFFFFFFFFu);
    vwr32(A6W(0xC46), a3);
    set_a(3, a3); set_d(0, n);
    charge(12);
    return RD_RTS;
}

/* FUN_00017c52: save 0xA0AC in 0xC83A, then 0xA0AC = the course table's entry
 * (ROM 0xECDC + course*64 + 0x10 -> word[0xA04C]) + 0xA1D2 + 0xA1D4 + 0x28 */
static uint32_t rd_a0ac(void)
{
    vwr16(A6W(0x483A), vrd16(A6W(0x20AC)));
    uint16_t d7 = (uint16_t)(vrd16(A6W(0x2046)) << 6);
    uint32_t a2 = vrd32(0xECDCu + 0x10u + (uint32_t)(int16_t)d7);
    uint16_t d3 = (uint16_t)vrd16(A6W(0x204C));
    uint16_t d0 = (uint16_t)(vrd16(a2 + (uint32_t)(int16_t)d3 * 2u) + vrd16(A6W(0x21D2)) + vrd16(A6W(0x21D4)) + 0x28);
    vwr16(A6W(0x20AC), d0);
    set_a(0, 0xECDCu); set_a(2, a2);
    set_d16(7, d7); set_d16(3, d3); set_d16(0, d0);
    charge(12);
    return RD_RTS;
}

/* FUN_00017e0e: 0xA076 = the low word of (long 0xA052 << 4) * (word 0xA04C) >> 16,
 * computed as two 16x16 products the way the 68K does it */
static uint32_t rd_a076(void)
{
    uint16_t d0 = (uint16_t)vrd16(A6W(0x204C));
    uint32_t d1 = vrd32(A6W(0x2052)), d2 = d1;
    d1 = (d1 & 0xFFFF0000u) | (uint16_t)(d1 << 4);
    d2 <<= 4; d2 = d2 << 16 | d2 >> 16;
    d1 = (uint32_t)(uint16_t)d1 * d0; d1 = d1 << 16 | d1 >> 16;
    d2 = (uint32_t)(uint16_t)d2 * d0;
    d1 = (d1 & 0xFFFF0000u) | (uint16_t)(d1 + d2);
    vwr16(A6W(0x2076), d1 & 0xFFFF);
    set_d16(0, d0); set_d(1, d1); set_d(2, d2);
    charge(12);
    return RD_RTS;
}

/* FUN_00031288: linear interpolation, D0 = a + (b - a) * D4w / D3w with a = 2*D1w,
 * b = 2*D2w (all sign-extended; D1..D4 left extended) */
static uint32_t rd_lerp(void)
{
    int32_t a = (int32_t)((uint32_t)(int16_t)d_reg(1) << 1), b = (int32_t)((uint32_t)(int16_t)d_reg(2) << 1);
    int32_t d3 = (int16_t)d_reg(3), d4 = (int16_t)d_reg(4);
    uint32_t p = (uint32_t)(b - a) * (uint32_t)d4;                /* muls.l: low 32 bits */
    int32_t q = (int32_t)SDIVREM((int32_t)p, d3, '/');
    set_d(1, (uint32_t)a); set_d(2, (uint32_t)b); set_d(3, (uint32_t)d3); set_d(4, (uint32_t)d4);
    set_d(0, (uint32_t)q + (uint32_t)a);
    charge(12);
    return RD_RTS;
}

/* FUN_00004fe6: clear 0x780..0x97F, then give each of its eight 64-byte slots
 * the header -1 / +0x2C = 5 / +0x2E = 1; clear 0x7EC */
static uint32_t rd_init_slots_780(void)
{
    uint32_t a1 = A6W(0x780);
    charge(2);
    for (int k = 0x7F; k >= 0; k--) { vwr32(a1, 0); a1 += 4; charge(2); if (k) poll(); }
    a1 = A6W(0x780);
    charge(2);
    for (int k = 7; k >= 0; k--) {
        vwr16(a1, 0xFFFF); vwr16(a1 + 0x2C, 5); vwr16(a1 + 0x2E, 1); a1 += 0x40;
        charge(5); if (k) poll();
    }
    vwr16(A6W(0x7EC), 0);
    charge(2);
    set_a(1, a1); set_d(0, 0x0000FFFFu);
    return RD_RTS;
}

/* FUN_00005038: copy the 108-byte block at ROM 0x5204 to WRAM 0x8000, then the
 * seven consecutive 108-byte blocks from 0x5270 into the seven 128-byte records
 * after it (at +0x14 each) */
static uint32_t rd_init_records_8000(void)
{
    uint32_t a0 = 0x10008000u, a1 = 0x5204u;
    charge(3);
    for (int k = 0x35; k >= 0; k--) { vwr16(a0, vrd16(a1)); a0 += 2; a1 += 2; charge(2); if (k) poll(); }
    charge(1);
    a1 = 0x5270u;                                                 /* once: the seven records take consecutive blocks */
    charge(1);
    for (int j = 6; j >= 0; j--) {
        a0 += 0x14;
        charge(2);
        for (int k = 0x35; k >= 0; k--) { vwr16(a0, vrd16(a1)); a0 += 2; a1 += 2; charge(2); if (k) poll(); }
        charge(1);
        if (j) poll();
    }
    charge(1);
    set_a(0, a0); set_a(1, a1); set_d16(0, 0xFFFF); set_d16(1, 0xFFFF);
    return RD_RTS;
}

/* FUN_000104dc: 0x2226 = the signed step (|0xDD2| clamped to 0x3FF) >> 7, with
 * the sign of 0xDD2 -- i.e. -7..7 */
static uint32_t rd_step_2226(void)
{
    uint16_t v = (uint16_t)vrd16(A6W(0xDD2)), d1 = v;
    charge(2);
    if (d1 & 0x8000) { d1 = (uint16_t)-d1; charge(1); }
    charge(2);
    if (d1 > 0x3FF) { d1 = 0x3FF; charge(1); }
    d1 = (uint16_t)((d1 >> 7) & 7);
    charge(4);
    if (v & 0x8000) { d1 = (uint16_t)-d1; charge(1); }
    vwr16(A6W(0x2226), d1);
    charge(2);
    set_d16(1, d1);
    return RD_RTS;
}

/* FUN_0001a22e / 0001a26c: for each active entry (bit 7 of +0x16C4), fetch from
 * its car's table (ROM 0xECDC + car*64 + 0x20 / + 0x14) at the entry's index
 * (+0x16AE): a long into +0x4AC0 / a word into +0x4AC4 */
static uint32_t car_table_fetch(uint32_t tbl_off, int is_long)
{
    uint16_t d4 = (uint16_t)vrd16(A6W(0x4A94)), d7 = (uint16_t)vrd16(A6W(0x4A96));
    uint32_t d0 = d_reg(0), a1 = a_reg(1);
    charge(3);
    for (;;) {
        uint32_t o = (uint32_t)(int16_t)d4;
        charge(2);
        if (vrd8(A6W(0x16C4) + o) & 0x80) {
            uint16_t c = (uint16_t)(vrd16(A6W(0x4AAA) + o) << 6);
            a1 = vrd32(0xECDCu + tbl_off + (uint32_t)(int16_t)c);
            uint16_t i = (uint16_t)vrd16(A6W(0x16AE) + o);
            d0 = (d0 & 0xFFFF0000u) | i;
            if (is_long) vwr32(A6W(0x4AC0) + o, vrd32(a1 + (uint32_t)(int16_t)i * 8u));
            else         vwr16(A6W(0x4AC4) + o, vrd16(a1 + (uint32_t)(int16_t)i * 2u));
            charge(5);
        }
        d4 += 0x40;
        charge(2);
        if (d7-- == 0) break;
        poll();
    }
    charge(1);
    set_a(0, 0xECDCu); set_a(1, a1); set_d(0, d0);
    set_d16(4, d4); set_d16(7, 0xFFFF);
    return RD_RTS;
}
static uint32_t rd_car_fetch_long(void) { return car_table_fetch(0x20, 1); }
static uint32_t rd_car_fetch_word(void) { return car_table_fetch(0x14, 0); }

/* FUN_0001bdec: 0xC07C = ((0xC024 * 0xC020) >> 9 scaled by 0xC19C and 0xC236,
 * each a >>16) / 2, and 0xC07A = its negation (signed 16-bit arithmetic) */
static uint32_t rd_c07a_c07c(void)
{
    int32_t d0 = (int16_t)vrd16(A6W(0x4024)) * (int16_t)vrd16(A6W(0x4020));
    d0 >>= 9;
    uint32_t u = (uint32_t)((int16_t)d0 * (int16_t)vrd16(A6W(0x419C)));
    u = u << 16 | u >> 16;
    u = (uint32_t)((int16_t)u * (int16_t)vrd16(A6W(0x4236)));
    u = u << 16 | u >> 16;
    uint16_t w = (uint16_t)((int16_t)u >> 1);
    vwr16(A6W(0x407C), w);
    w = (uint16_t)-w;
    vwr16(A6W(0x407A), w);
    set_d(0, (u & 0xFFFF0000u) | w);
    charge(13);
    return RD_RTS;
}

/* FUN_0001dfc6: clear the words 0xC332..0xC33A and 0xA0BC..0xA0C6 */
static uint32_t rd_clear_c332(void)
{
    for (uint32_t o = 0x4332; o <= 0x433A; o += 2) vwr16(A6W(o), 0);
    for (uint32_t o = 0x20BC; o <= 0x20C6; o += 2) vwr16(A6W(o), 0);
    set_d(0, 0);
    charge(13);
    return RD_RTS;
}

/* FUN_0000589e: copy D5w+1 groups of three 256-byte runs from A3 into the three
 * palette planes at 0x90028000, 0x90030000 and 0x90038000 */
static uint32_t rd_palette_load(void)
{
    uint32_t a0 = 0x90028000u, a1 = 0x90030000u, a2 = 0x90038000u, a3 = a_reg(3);
    uint16_t d5 = (uint16_t)d_reg(5);
    uint32_t *dst[3] = { &a0, &a1, &a2 };
    charge(3);
    for (;;) {
        for (int p = 0; p < 3; p++) {
            charge(1);
            for (int k = 0xFF; k >= 0; k--) { vwr8((*dst[p])++, vrd8(a3++)); charge(2); if (k) poll(); }
        }
        charge(1);
        if (d5-- == 0) break;
        poll();
    }
    charge(1);
    set_a(0, a0); set_a(1, a1); set_a(2, a2); set_a(3, a3);
    set_d16(4, 0xFFFF); set_d16(5, 0xFFFF);
    return RD_RTS;
}

/* FUN_00005cee / 0000a870: D1 = D0w in packed BCD (six digits: 65535 -> 0x065535),
 * by the shift-and-decimal-add loop; D2b/D3b keep the middle/low digit pairs */
static uint32_t to_bcd(void)
{
    uint32_t d0 = d_reg(0);
    uint16_t v = (uint16_t)d0;
    uint32_t b1 = 0, b2 = 0, b3 = 0;
    charge(4);
    for (int k = 15; k >= 0; k--) {
        R[RR_XF] = (uint8_t)(v >> 15); v = (uint16_t)(v << 1);   /* add.w D0w,D0w: X = carry */
        b3 = rr_abcd(b3, b3); b2 = rr_abcd(b2, b2); b1 = rr_abcd(b1, b1);
        charge(5);
        if (k) poll();
    }
    charge(5);
    set_d(0, d0 & 0xFFFF0000u);
    set_d(1, b1 << 16 | b2 << 8 | b3);
    set_d(2, (d_reg(2) & 0xFFFF0000u) | b2);
    set_d(3, (d_reg(3) & 0xFFFF0000u) | b3);
    set_d(7, 0x0000FFFFu);
    return RD_RTS;
}

/* FUN_0001811a: unless 0x9100 is set (then continue at 0x1818A), for each of the
 * eight 16-byte entries at 0x91AE whose word matches 0x91A2 and whose flag byte
 * (+8) has bit 6: set bit n of 0xC87E and copy its long (+2) to 0xC880 + n*4 */
static uint32_t rd_match_91a2(void)
{
    charge(2);
    if (vrd16(A6W(0x1100))) return 0x1818Au;
    uint16_t d1 = 0x70, d6 = 7, d0 = (uint16_t)d_reg(0);
    charge(2);
    for (;;) {
        uint32_t o = (uint32_t)(int16_t)d1;
        d0 = (uint16_t)vrd16(A6W(0x11AE) + o);
        charge(3);
        if (d0 == (uint16_t)vrd16(A6W(0x11A2))) {
            charge(2);
            if (vrd8(A6W(0x11B6) + o) & 0x40) {
                vwr8(A6W(0x487E), vrd8(A6W(0x487E)) | (1u << (d6 & 7)));
                vwr32(A6W(0x4880) + (uint32_t)(int16_t)d6 * 4u, vrd32(A6W(0x11B0) + o));
                charge(2);
            }
        }
        d1 -= 0x10;
        charge(2);
        if (d6-- == 0) break;
        poll();
    }
    charge(1);
    set_d16(0, d0); set_d16(1, d1); set_d(6, 0x0000FFFFu);
    return RD_RTS;
}

/* FUN_0001bd3c: 0xC03A = 0 when 0xC022 is 0, else
 *   -((0x451E * 0xC084 / max(0xC022, 0x340)) >> 4 - 0xC086)   (divs.w as a 68K) */
static uint32_t rd_c03a(void)
{
    uint32_t d0 = 0;
    uint16_t d1 = (uint16_t)vrd16(A6W(0x4022));
    charge(3);
    if (d1) {
        charge(2);
        if (d1 < 0x340) { d1 = 0x340; charge(1); }
        int32_t num = 0x451E * (int16_t)vrd16(A6W(0x4084));
        int32_t q = (int32_t)SDIVREM(num, (int16_t)d1, '/'), r = (int32_t)SDIVREM(num, (int16_t)d1, '%');
        d0 = (uint32_t)num;                                       /* divs.w overflow: D0 unchanged */
        if (q >= -32768 && q <= 32767) d0 = (uint32_t)r << 16 | ((uint32_t)q & 0xFFFF);
        uint16_t w = (uint16_t)((int16_t)d0 >> 4);
        w = (uint16_t)(w - vrd16(A6W(0x4086)));
        w = (uint16_t)-w;
        d0 = (d0 & 0xFFFF0000u) | w;
        charge(6);
    }
    vwr16(A6W(0x403A), d0 & 0xFFFF);
    charge(2);
    set_d(0, d0); set_d16(1, d1);
    return RD_RTS;
}

/* FUN_00005564: clear the 128 mixer bytes at 0x90020080, then set +1..+6 = 1,
 * +7 = 2, +0x4C = 3 */
static uint32_t rd_mixer_init(void)
{
    uint32_t a0 = 0x90020080u;
    charge(3);
    for (int k = 0x7F; k >= 0; k--) { vwr8(a0++, 0); charge(2); if (k) poll(); }
    a0 = 0x90020080u;
    for (uint32_t i = 1; i <= 6; i++) vwr8(a0 + i, 1);
    vwr8(a0 + 7, 2); vwr8(a0 + 0x4C, 3);
    charge(10);
    set_a(0, a0); set_d(0, 0); set_d16(1, 0xFFFF);
    return RD_RTS;
}

/* FUN_0000efb4: clear the 15 input words at 0xF004, read the switch port
 * 0x20000002 twice, write the inverse of the AND of the two to 0x2000000A,
 * clear 0xF024/0xF026 and store the second read at 0xF004 */
static uint32_t rd_input_reset(void)
{
    uint32_t a1 = A6W(0x7004);
    charge(3);
    for (int k = 0xE; k >= 0; k--) { vwr16(a1, 0); a1 += 2; charge(2); if (k) poll(); }
    uint16_t r0 = (uint16_t)vrd16(0x20000002u), r1 = (uint16_t)vrd16(0x20000002u);
    vwr16(0x2000000Au, (uint16_t)~(r0 & r1));
    vwr16(A6W(0x7026), 0); vwr16(A6W(0x7024), 0);
    uint16_t i = (uint16_t)vrd16(A6W(0x7024));
    vwr16(A6W(0x7004) + (uint32_t)(int16_t)i * 2u, r1);
    charge(10);
    set_a(1, a1); set_d16(0, i); set_d(1, r1);
    return RD_RTS;
}

/* FUN_000157d4: unless the course's byte (ROM 0xECDC + course*64 + 1) is set,
 * jump through the table pair at 0x15808 selected by bit 0 of 0xA344 and the
 * low two bits of 0xA342 */
static uint32_t rd_dispatch_a342(void)
{
    uint16_t d7 = (uint16_t)(vrd16(A6W(0x2046)) << 6);
    uint32_t a2 = 0xECDDu + (uint32_t)(int16_t)d7;
    set_a(0, 0xECDCu); set_d16(7, d7); set_a(2, a2);
    charge(6);
    if (vrd8(a2)) { charge(1); return RD_RTS; }
    uint16_t s = (uint16_t)(vrd16(A6W(0x2344)) & 1);
    uint32_t a0 = vrd32(0x15808u + s * 4u);
    uint16_t i = (uint16_t)(vrd16(A6W(0x2342)) & 3);
    uint32_t d0 = (d_reg(0) & 0xFFFF0000u) | i;
    d0 = vrd32(a0 + (uint32_t)(int16_t)i * 4u);
    set_a(0, a0); set_d(0, d0);
    charge(8);
    return RD_JMP(a0 + d0);
}

const rd_entry rd_table[] = {
    /*  ep        fn                         kill    scratch cost  name */
    /* cost = lifted instructions on the shortest path; longer paths charge() the rest */
    { 0x004120, rd_dispatch_4120,          0x0101, 0,      6, "FUN_00004120" },
    { 0x004564, rd_dispatch_4564,          0x0101, 0,      6, "FUN_00004564" },
    { 0x00459E, rd_count_0808,             0x0000, 0,      2, "FUN_0000459e" },
    { 0x004640, rd_cpu_leds,               0x0007, 0,      18, "FUN_00004640" },
    { 0x0046A6, rd_reset_display_header,   0x0803, 0,      0, "FUN_000046a6" },
    { 0x0046D6, rd_cabinet_type,           0x0003, 0,      0, "FUN_000046d6" },
    { 0x00470E, rd_send_sound_param,       0x0001, 0,      3, "FUN_0000470e" },
    { 0x00472A, rd_update_flag_0804,       0x0001, 0,      7, "FUN_0000472a" },
    { 0x00494C, rd_reset_0816_block,       0x0000, 0,      9, "FUN_0000494c" },
    { 0x004988, rd_input_sequence,         0x0007, 0,      0, "FUN_00004988" },
    { 0x004CFA, rd_sound_flag_4020,        0x0003, 0,      5, "FUN_00004cfa" },
    { 0x004D52, rd_sound_cpu_upload,       0x0303, 0,      0, "FUN_00004d52" },
    { 0x004FE6, rd_init_slots_780,         0x0201, 0,      0, "FUN_00004fe6" },
    { 0x005038, rd_init_records_8000,      0x0303, 0,      0, "FUN_00005038" },
    { 0x005564, rd_mixer_init,             0x0103, 0,      0, "FUN_00005564" },
    { 0x005706, rd_mixer_from_table,       0x0F01, 0,      10, "FUN_00005706" },
    { 0x005742, rd_fill_czram,             0x0103, 0,      0, "FUN_00005742" },
    { 0x00575A, rd_clear_text,             0x0203, 0,      0, "FUN_0000575a" },
    { 0x005876, rd_palette_tails,          0x0F03, 0,      0, "FUN_00005876" },
    { 0x00589E, rd_palette_load,           0x0FFF, 0,      0, "FUN_0000589e" },
    { 0x0058EE, rd_palette_end_bytes,      0x0000, 0,      4, "FUN_000058ee" },
    { 0x005A9A, rd_return_two_levels,      0x0000, 0,      2, "caseD_0 @5A9A" },
    { 0x005CEE, to_bcd,                    0x008F, 0,      0, "FUN_00005cee" },
    { 0x00A870, to_bcd,                    0x008F, 0,      0, "FUN_0000a870" },
    { 0x00A88E, rd_time_digits,            0x020F, 0,      1, "FUN_0000a88e" },
    { 0x00A8C8, rd_decimal_digits,         0x020F, 0,      1, "FUN_0000a8c8" },
    { 0x00AE04, rd_dispatch_ae04,          0x0101, 0,      8, "FUN_0000ae04" },
    { 0x00B6D0, rd_append_8000,            0x0801, 0,      0, "FUN_0000b6d0" },
    { 0x00B89A, rd_mixer_11_0,             0x0101, 0,      6, "FUN_0000b89a" },
    { 0x00B8B0, rd_mixer_11_100,           0x0101, 0,      6, "FUN_0000b8b0" },
    { 0x00C656, rd_delay_60,               0x0003, 0,      0, "FUN_0000c656" },
    { 0x00E64E, rd_dbc8_with_15,           0x0001, 0,      2, "FUN_0000e64e" },
    { 0x00E6CC, rd_e262_1_21,              0x00C0, 0,      3, "FUN_0000e6cc" },
    { 0x00E6D6, rd_e262_11_7,              0x00C0, 0,      3, "FUN_0000e6d6" },
    { 0x00ECCE, rd_clear_a432,             0x0000, 0,      4, "FUN_0000ecce" },
    { 0x00EF9C, rd_keycus_pair,            0x0030, 0,      6, "FUN_0000ef9c" },
    { 0x00EFB4, rd_input_reset,            0x0203, 0,      0, "FUN_0000efb4" },
    { 0x00EFEE, rd_keycus_sample,          0x0007, 0,      1, "FUN_0000efee" },
    { 0x00F02E, rd_random_next,            0x0001, 0,      7, "FUN_0000f02e" },
    { 0x00FE14, rd_fe14,                   0x0001, 0,      4, "FUN_0000fe14" },
    { 0x00FE22, rd_a21c,                   0x0001, 0,      3, "FUN_0000fe22" },
    { 0x00FE60, rd_pick_a224,              0x0003, 0,      2, "FUN_0000fe60" },
    { 0x0104DC, rd_step_2226,              0x0002, 0,      0, "FUN_000104dc" },
    { 0x01226E, rd_dl_header_closed,       0x0801, 0,      8, "FUN_0001226e" },
    { 0x01228C, rd_countdown_a32c,         0x0001, 0,      3, "FUN_0001228c" },
    { 0x013EA0, rd_empty,                  0x0000, 0,      1, "FUN_00013ea0" },
    { 0x014532, rd_step_a04a,              0x0001, 0,      3, "FUN_00014532" },
    { 0x01460C, rd_clear_a0bc,             0x0000, 0,      7, "FUN_0001460c" },
    { 0x014626, rd_seq_done_flag,          0x0000, 0,      0, "FUN_00014626" },
    { 0x01468A, rd_car_2a_set,             0x1001, 0,      5, "FUN_0001468a" },
    { 0x01469C, rd_car_2a_clear,           0x1001, 0,      5, "FUN_0001469c" },
    { 0x015048, rd_vertex_record,          0x0801, 0,      13, "FUN_00015048" },
    { 0x01514C, rd_clear_pending_words,    0x0103, 0,      0, "FUN_0001514c" },
    { 0x0157D4, rd_dispatch_a342,          0x37C1, 0,      0, "FUN_000157d4" },
    { 0x015862, rd_dl_header,              0x0801, 0,      6, "FUN_00015862" },
    { 0x015876, rd_close_list,             0x0000, 0,      3, "FUN_00015876" },
    { 0x017C52, rd_a0ac,                   0x07FF, 0,      0, "FUN_00017c52" },
    { 0x017DCA, rd_c1e8,                   0x0003, 0,      11, "FUN_00017dca" },
    { 0x017DEA, rd_empty,                  0x0000, 0,      1, "FUN_00017dea" },
    { 0x017DEC, rd_recip_a052,             0x0103, 0,      9, "FUN_00017dec" },
    { 0x017E0E, rd_a076,                   0x0007, 0,      0, "FUN_00017e0e" },
    { 0x017FAE, rd_delta_dd4,              0x0003, 0,      7, "FUN_00017fae" },
    { 0x01800E, rd_dl_header_masked,       0x0801, 0,      7, "FUN_0001800e" },
    { 0x0180DA, rd_clear_16c4_bit0,        0x00C0, 0,      0, "FUN_000180da" },
    { 0x0180F4, rd_lookup_a048,            0x0101, 0,      4, "FUN_000180f4" },
    { 0x01811A, rd_match_91a2,             0x00C3, 0,      0, "FUN_0001811a" },
    { 0x019F80, rd_copy_flagged,           0x00C1, 0,      1, "FUN_00019f80" },
    { 0x01A1B6, rd_step_toward,            0x0002, 0,      3, "FUN_0001a1b6" },
    { 0x01A22C, rd_empty,                  0x0000, 0,      1, "FUN_0001a22c" },
    { 0x01A22E, rd_car_fetch_long,         0x03FF, 0,      0, "FUN_0001a22e" },
    { 0x01A26C, rd_car_fetch_word,         0x03FF, 0,      0, "FUN_0001a26c" },
    { 0x01B09C, rd_fill_c8f4,              0x0180, 0,      0, "FUN_0001b09c" },
    { 0x01B2E2, rd_b2e2,                   0x0001, 0,      3, "FUN_0001b2e2" },
    { 0x01B2F0, rd_set_16c4_bit7,          0x0001, 0,      2, "FUN_0001b2f0" },
    { 0x01BD3C, rd_c03a,                   0x0003, 0,      0, "FUN_0001bd3c" },
    { 0x01BD68, rd_split_c024,             0x0001, 0,      9, "FUN_0001bd68" },
    { 0x01BDCA, rd_scale_c26e,             0x0003, 0,      11, "FUN_0001bdca" },
    { 0x01BDEC, rd_c07a_c07c,              0x0001, 0,      0, "FUN_0001bdec" },
    { 0x01DFC6, rd_clear_c332,             0x0001, 0,      0, "FUN_0001dfc6" },
    { 0x01E578, rd_empty,                  0x0000, 0,      1, "FUN_0001e578" },
    { 0x01E69E, rd_fill_91a4,              0x0000, 0,      2, "FUN_0001e69e" },
    { 0x01E6EA, rd_snd_5016_40c4,          0x0000, 0,      2, "FUN_0001e6ea" },
    { 0x01E760, rd_snd_reset_block,        0x0000, 0,      6, "FUN_0001e760" },
    { 0x01E926, rd_snd_501a_clear,         0x0000, 0,      2, "FUN_0001e926" },
    { 0x01E99C, rd_snd_501a_40b1,          0x0000, 0,      3, "FUN_0001e99c" },
    { 0x01EA60, rd_snd_5018_d1,            0x0003, 0,      1, "FUN_0001ea60" },
    { 0x01F574, rd_build_45d0_block,       0x003F, 0,      2, "FUN_0001f574" },
    { 0x01F61E, rd_latch_45ec_block,       0x003F, 0,      2, "FUN_0001f61e" },
    { 0x01F674, rd_pack_records_4560,      0x03FF, 0,      0, "FUN_0001f674" },
    { 0x01FE0C, rd_init_ca7c,              0x0381, 0,      0, "FUN_0001fe0c" },
    { 0x0205F4, rd_empty,                  0x0000, 0,      1, "FUN_000205f4" },
    { 0x02089A, rd_empty,                  0x0000, 0,      1, "FUN_0002089a" },
    { 0x0211DE, rd_empty,                  0x0000, 0,      1, "FUN_000211de" },
    { 0x02147A, rd_c346_by_mode,           0x0001, 0,      3, "FUN_0002147a" },
    { 0x0257C2, rd_257c2,                  0x0101, 0,      4, "FUN_000257c2" },
    { 0x026E58, rd_gather_slot_display,    0x07FF, 0,      0, "FUN_00026e58" },
    { 0x027062, rd_merge_enabled_slots,    0x08FF, 0,      1, "FUN_00027062" },
    { 0x02708A, rd_reset_disabled_slots,   0x08FF, 0,      0, "FUN_0002708a" },
    { 0x0270F2, rd_store_slot,             0x0F1F, 0,      10, "FUN_000270f2" },
    { 0x028646, rd_snd_501e_clear,         0x0000, 0,      2, "FUN_00028646" },
    { 0x0286F2, rd_snd_5000_bit13,         0x0000, 0,      2, "FUN_000286f2" },
    { 0x0286FC, rd_snd_5012_40cb,          0x0000, 0,      2, "FUN_000286fc" },
    { 0x028786, rd_snd_5000_405d,          0x0000, 0,      3, "FUN_00028786" },
    { 0x028C82, rd_d7d8_from_table,        0x0101, 0,      7, "FUN_00028c82" },
    { 0x028D8A, rd_d7e0_step,              0x0001, 0,      1, "FUN_00028d8a" },
    { 0x028DB2, rd_d7ea_step,              0x0001, 0,      1, "FUN_00028db2" },
    { 0x02981C, rd_window_mode0,           0x0001, 0,      1, "FUN_0002981c" },
    { 0x029838, rd_window_mode4,           0x0001, 0,      1, "FUN_00029838" },
    { 0x02988E, rd_dispatch_d882,          0x0101, 0,      0, "FUN_0002988e" },
    { 0x02BAB4, rd_snd_4022_from_104b,     0x0101, 0,      11, "FUN_0002bab4" },
    { 0x02BC68, rd_copy_10a0,              0x0100, 0,      8, "FUN_0002bc68" },
    { 0x02C1BE, rd_copy_1071_to_d007,      0x0000, 0,      3, "FUN_0002c1be" },
    { 0x02FB34, rd_save_f000,              0x1FE7, 0,      0, "FUN_0002fb34" },
    { 0x02FB82, rd_restore_f000,           0x0FDF, 0,      0, "FUN_0002fb82" },
    { 0x030BC2, rd_empty,                  0x0000, 0,      1, "FUN_00030bc2" },
    { 0x030BE6, rd_empty,                  0x0000, 0,      1, "FUN_00030be6" },
    { 0x031246, rd_word_index_x2,          0x0003, 0,      4, "FUN_00031246" },
    { 0x03124E, rd_interp_table,           0x003F, 0,      23, "FUN_0003124e" },
    { 0x031288, rd_lerp,                   0x001F, 0,      0, "FUN_00031288" },
    { 0x031724, rd_31724,                  0x003F, 0,      8, "FUN_00031724" },
    { 0x031742, rd_31742,                  0x003F, 0,      8, "FUN_00031742" },
};
const int rd_count = (int)(sizeof rd_table / sizeof rd_table[0]);
