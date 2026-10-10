/*
 * h8_sem.h -- the H8/300 and H8/300H instruction SEMANTICS for the translated H8 programs (tools/h8/h8_translate.py emits calls into this):
 * Time Crisis 2's sub-CPU (H8/3002: H8/300H, advanced mode, 24-bit addresses) and its I/O board's H8/3334 (H8/300, 16-bit). Our own code,
 * from the H8/300H programming manual (Hitachi ADE-602-053); checked by running the translations against MAME's traces of both CPUs.
 *
 * Registers: ER0-ER7 (32-bit; R0-R7 their low halves, E0-E7 the high ones, RnH/RnL the bytes of Rn); ER7 = SP. CCR = I UI H U N Z V C
 * (bits 7..0). Word / long accesses are to even addresses (bit 0 ignored). Normal mode (the H8/300): addresses are 16 bits, the stack
 * holds 16-bit words, vectors are 16-bit. The host supplies the memory (h8->rd8/rd16/wr8/wr16 + ctx).
 */
#ifndef H8_SEM_H
#define H8_SEM_H
#include <stdint.h>

typedef struct h8_cpu {
    uint32_t er[8];
    uint32_t pc;
    uint8_t  ccr;
    int      advanced;                         /* 1 = H8/300H advanced mode (24-bit); 0 = H8/300 / normal mode (16-bit) */
    uint32_t amask;                            /* 0xFFFFFF or 0xFFFF */
    int64_t  cycles;                           /* states executed */
    int32_t  budget;                           /* states left in this slice (run() returns at <= 0) */
    int      sleeping;
    uint8_t  (*rd8)(void *ctx, uint32_t a);
    uint16_t (*rd16)(void *ctx, uint32_t a);
    void     (*wr8)(void *ctx, uint32_t a, uint8_t v);
    void     (*wr16)(void *ctx, uint32_t a, uint16_t v);
    void     *ctx;
    void     (*trace)(void *ctx, uint32_t pc);  /* NULL = none (the instruction-match runs) */
} h8_cpu;

enum { H8_C = 0x01, H8_V = 0x02, H8_Z = 0x04, H8_N = 0x08, H8_U = 0x10, H8_H = 0x20, H8_UI = 0x40, H8_I = 0x80 };

/* ---- registers ---- */
static inline uint8_t  h8_r8(const h8_cpu *c, int n) { return n < 8 ? (uint8_t)(c->er[n] >> 8) : (uint8_t)c->er[n - 8]; }
static inline void     h8_w8(h8_cpu *c, int n, uint8_t v) { if (n < 8) c->er[n] = (c->er[n] & 0xFFFF00FFu) | (uint32_t)v << 8; else c->er[n - 8] = (c->er[n - 8] & 0xFFFFFF00u) | v; }
static inline uint16_t h8_r16(const h8_cpu *c, int n) { return n < 8 ? (uint16_t)c->er[n] : (uint16_t)(c->er[n - 8] >> 16); }
static inline void     h8_w16(h8_cpu *c, int n, uint16_t v) { if (n < 8) c->er[n] = (c->er[n] & 0xFFFF0000u) | v; else c->er[n - 8] = (c->er[n - 8] & 0x0000FFFFu) | (uint32_t)v << 16; }

/* ---- memory ---- */
static inline uint8_t  h8_rb(h8_cpu *c, uint32_t a) { return c->rd8(c->ctx, a & c->amask); }
static inline uint16_t h8_rw(h8_cpu *c, uint32_t a) { return c->rd16(c->ctx, a & c->amask & ~1u); }
static inline uint32_t h8_rl(h8_cpu *c, uint32_t a) { a &= c->amask & ~1u; return (uint32_t)c->rd16(c->ctx, a) << 16 | c->rd16(c->ctx, (a + 2) & c->amask); }
static inline void     h8_wb(h8_cpu *c, uint32_t a, uint8_t v) { c->wr8(c->ctx, a & c->amask, v); }
static inline void     h8_ww(h8_cpu *c, uint32_t a, uint16_t v) { c->wr16(c->ctx, a & c->amask & ~1u, v); }
static inline void     h8_wl(h8_cpu *c, uint32_t a, uint32_t v) { a &= c->amask & ~1u; c->wr16(c->ctx, a, (uint16_t)(v >> 16)); c->wr16(c->ctx, (a + 2) & c->amask, (uint16_t)v); }

/* the address in a register: ERn in advanced mode, Rn in normal mode */
static inline uint32_t h8_ea(const h8_cpu *c, int n) { return c->advanced ? (c->er[n] & 0xFFFFFF) : (c->er[n] & 0xFFFF); }
static inline void     h8_ea_add(h8_cpu *c, int n, int32_t d)
{
    if (c->advanced) c->er[n] += (uint32_t)d;
    else c->er[n] = (c->er[n] & 0xFFFF0000u) | ((c->er[n] + (uint32_t)d) & 0xFFFF);
}

/* ---- flags ---- */
#define H8_NZ(c, v, bits) ((c)->ccr = (uint8_t)(((c)->ccr & ~(H8_N | H8_Z)) | (((v) >> ((bits) - 1) & 1) ? H8_N : 0) | ((v) == 0 ? H8_Z : 0)))
static inline void h8_logic(h8_cpu *c, uint32_t v, int bits) { H8_NZ(c, v, bits); c->ccr &= (uint8_t)~H8_V; }   /* mov, and, or, xor, not, ext */

/* add / sub / cmp: H = the carry out of bit 3 (b), 11 (w), 27 (l); C = the carry out of the top bit; V = signed overflow */
static inline uint32_t h8_add(h8_cpu *c, uint32_t a, uint32_t b, int bits, int carry_in, int keep_z)
{
    const uint64_t m = bits == 32 ? 0xFFFFFFFFull : (1ull << bits) - 1, sb = 1ull << (bits - 1), hb = 1ull << (bits - 4);
    const uint64_t r = ((uint64_t)a & m) + ((uint64_t)b & m) + (uint64_t)carry_in;
    const uint32_t res = (uint32_t)(r & m);
    const uint64_t h = (((uint64_t)a & (hb - 1)) + ((uint64_t)b & (hb - 1)) + (uint64_t)carry_in) & hb;
    uint8_t f = c->ccr & (uint8_t)~(H8_H | H8_N | H8_V | H8_C | (keep_z ? 0 : H8_Z));
    if (h) f |= H8_H;
    if (res & sb) f |= H8_N;
    if (keep_z) { if (res) f &= (uint8_t)~H8_Z; } else if (!res) f |= H8_Z;
    if (~((uint64_t)a ^ b) & ((uint64_t)a ^ res) & sb) f |= H8_V;
    if (r >> bits & 1) f |= H8_C;
    c->ccr = f;
    return res;
}
static inline uint32_t h8_sub(h8_cpu *c, uint32_t a, uint32_t b, int bits, int borrow_in, int keep_z)
{
    const uint64_t m = bits == 32 ? 0xFFFFFFFFull : (1ull << bits) - 1, sb = 1ull << (bits - 1), hb = 1ull << (bits - 4);
    const uint32_t res = (uint32_t)(((uint64_t)a - (uint64_t)b - (uint64_t)borrow_in) & m);
    uint8_t f = c->ccr & (uint8_t)~(H8_H | H8_N | H8_V | H8_C | (keep_z ? 0 : H8_Z));
    if (((uint64_t)a & (hb - 1)) < ((uint64_t)b & (hb - 1)) + (uint64_t)borrow_in) f |= H8_H;
    if (res & sb) f |= H8_N;
    if (keep_z) { if (res) f &= (uint8_t)~H8_Z; } else if (!res) f |= H8_Z;
    if (((uint64_t)a ^ b) & ((uint64_t)a ^ res) & sb) f |= H8_V;
    if (((uint64_t)a & m) < ((uint64_t)b & m) + (uint64_t)borrow_in) f |= H8_C;
    c->ccr = f;
    return res;
}
/* inc / dec: N Z V, C and H unchanged */
static inline uint32_t h8_incdec(h8_cpu *c, uint32_t a, int32_t d, int bits)
{
    const uint64_t m = bits == 32 ? 0xFFFFFFFFull : (1ull << bits) - 1, sb = 1ull << (bits - 1);
    const uint32_t res = (uint32_t)(((uint64_t)a + (uint64_t)(int64_t)d) & m);
    uint8_t f = c->ccr & (uint8_t)~(H8_N | H8_Z | H8_V);
    if (res & sb) f |= H8_N;
    if (!res) f |= H8_Z;
    if (d > 0 ? (!(a & sb) && (res & sb)) : ((a & sb) && !(res & sb))) f |= H8_V;
    c->ccr = f;
    return res;
}

/* shifts and rotates: n = 1 (the H8/300H's #2 forms run it twice) */
static inline uint32_t h8_shift(h8_cpu *c, const char op, uint32_t v, int bits)
{
    const uint32_t m = bits == 32 ? 0xFFFFFFFFu : (1u << bits) - 1, sb = 1u << (bits - 1);
    uint32_t r; int cout; int ovf = 0;
    v &= m;
    switch (op) {
    case 'a': r = (v << 1) & m; cout = (v & sb) != 0; ovf = ((v ^ r) & sb) != 0; break;          /* shal */
    case 'r': r = (v >> 1) | (v & sb); cout = v & 1; break;                                       /* shar */
    case 'l': r = (v << 1) & m; cout = (v & sb) != 0; break;                                      /* shll */
    case 's': r = v >> 1; cout = v & 1; break;                                                    /* shlr */
    case 'L': r = ((v << 1) | (v >> (bits - 1))) & m; cout = (v & sb) != 0; break;              /* rotl */
    case 'R': r = (v >> 1) | ((v & 1) ? sb : 0); cout = v & 1; break;                             /* rotr */
    case 'X': r = ((v << 1) | (c->ccr & H8_C)) & m; cout = (v & sb) != 0; break;                  /* rotxl */
    default:  r = (v >> 1) | ((c->ccr & H8_C) ? sb : 0); cout = v & 1; break;                     /* rotxr 'x' */
    }
    uint8_t f = c->ccr & (uint8_t)~(H8_N | H8_Z | H8_V | H8_C);
    if (r & sb) f |= H8_N;
    if (!r) f |= H8_Z;
    if (ovf) f |= H8_V;
    if (cout) f |= H8_C;
    c->ccr = f;
    return r;
}

/* branch conditions, by the low nibble of the Bcc opcode (bra/brn/bhi/bls/bcc/bcs/bne/beq/bvc/bvs/bpl/bmi/bge/blt/bgt/ble) */
static inline int h8_cond(const h8_cpu *c, int cc)
{
    const int C = c->ccr & H8_C, V = (c->ccr & H8_V) != 0, Z = (c->ccr & H8_Z) != 0, N = (c->ccr & H8_N) != 0;
    switch (cc & 15) {
    case 0: return 1;               case 1: return 0;
    case 2: return !C && !Z;        case 3: return C || Z;
    case 4: return !C;              case 5: return C != 0;
    case 6: return !Z;              case 7: return Z;
    case 8: return !V;              case 9: return V;
    case 10: return !N;             case 11: return N;
    case 12: return N == V;         case 13: return N != V;
    case 14: return !Z && N == V;   default: return Z || N != V;
    }
}

/* the stack: a long (advanced mode) or a word (normal) */
static inline void h8_push_pc(h8_cpu *c, uint32_t pc)
{
    if (c->advanced) { h8_ea_add(c, 7, -4); h8_wl(c, h8_ea(c, 7), pc & 0xFFFFFF); }
    else { h8_ea_add(c, 7, -2); h8_ww(c, h8_ea(c, 7), (uint16_t)pc); }
}
static inline uint32_t h8_pop_pc(h8_cpu *c)
{
    uint32_t pc;
    if (c->advanced) { pc = h8_rl(c, h8_ea(c, 7)) & 0xFFFFFF; h8_ea_add(c, 7, 4); }
    else { pc = h8_rw(c, h8_ea(c, 7)); h8_ea_add(c, 7, 2); }
    return pc;
}

/* EXCEPTIONS (interrupts, trapa): push PC and CCR (advanced mode: one long, CCR in its top byte; normal: the PC word then the CCR word),
 * set I (UI too when ui), jump through the vector (advanced: longs at vec*4; normal: words at vec*2) */
static inline void h8_exception(h8_cpu *c, int vec, int set_ui)
{
    if (c->advanced) { h8_ea_add(c, 7, -4); h8_wl(c, h8_ea(c, 7), (uint32_t)c->ccr << 24 | (c->pc & 0xFFFFFF)); }
    else { h8_ea_add(c, 7, -2); h8_ww(c, h8_ea(c, 7), (uint16_t)c->pc); h8_ea_add(c, 7, -2); h8_ww(c, h8_ea(c, 7), (uint16_t)(c->ccr << 8 | c->ccr)); }
    c->ccr |= H8_I; if (set_ui) c->ccr |= H8_UI;
    c->pc = c->advanced ? (h8_rl(c, (uint32_t)vec * 4) & 0xFFFFFF) : h8_rw(c, (uint32_t)vec * 2);
    c->sleeping = 0;
}
/* an INTERRUPT taken (not trapa, whose time is the instruction's): the exception plus its states as MAME charges them (h8.lst "irq": an
 * internal state, the two stack writes, the vector read -- two words in advanced mode --, an internal state, the prefetch) */
static inline void h8_interrupt(h8_cpu *c, int vec, int set_ui)
{
    h8_exception(c, vec, set_ui);
    const int st = c->advanced ? 14 : 12;
    c->cycles += st; c->budget -= st;
}
/* RESET (power-on, or the main CPU restarting the sub-CPU): I set (the other flags and the registers kept), PC from vector 0 */
static inline void h8_reset(h8_cpu *c)
{
    c->ccr |= H8_I;
    c->pc = c->advanced ? (h8_rl(c, 0) & 0xFFFFFF) : h8_rw(c, 0);
    c->sleeping = 0;
}
static inline void h8_rte(h8_cpu *c)
{
    if (c->advanced) { const uint32_t v = h8_rl(c, h8_ea(c, 7)); h8_ea_add(c, 7, 4); c->ccr = (uint8_t)(v >> 24); c->pc = v & 0xFFFFFF; }
    else { c->ccr = (uint8_t)h8_rw(c, h8_ea(c, 7)); h8_ea_add(c, 7, 2); c->pc = h8_rw(c, h8_ea(c, 7)); h8_ea_add(c, 7, 2); }
}

/* daa / das: the decimal adjust of the H8/300 manual's tables (C and H in, the corrected byte and C out; N Z set, V undefined -> kept) */
static inline uint8_t h8_daa(h8_cpu *c, uint8_t v, int sub)
{
    const int C = c->ccr & H8_C, H = (c->ccr & H8_H) != 0;
    int adj = 0, cout = C;
    if (!sub) {
        if (C || v > 0x99) { adj |= 0x60; cout = 1; }
        if (H || (v & 15) > 9) adj |= 0x06;
        v = (uint8_t)(v + adj);
    } else {
        if (C) adj |= 0x60;
        if (H) adj |= 0x06;
        v = (uint8_t)(v - adj);
    }
    uint8_t f = c->ccr & (uint8_t)~(H8_N | H8_Z | H8_C);
    if (v & 0x80) f |= H8_N;
    if (!v) f |= H8_Z;
    if (cout) f |= H8_C;
    c->ccr = f;
    return v;
}
#endif
