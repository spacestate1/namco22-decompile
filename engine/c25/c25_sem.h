/*
 * c25_sem.h -- the TMS320C25 instruction SEMANTICS, one copy for both uses:
 * the translated programs (gen/<game>_c25.c: c25_exec with a CONSTANT opcode,
 * which the compiler folds to that one instruction) and the test oracle
 * (tools/c25oracle: the same function fed from program memory). That the two
 * run the same source is what lets the translation be gated as EQUAL to the
 * oracle, which is gated against MAME. Moved unchanged from the validated
 * interpreter (formerly raverace/src/rr_c71.c exec1/misc); the second word of a
 * two-word instruction comes through c25_imm(): the translation's constant
 * when it supplies one (d->ops), program memory otherwise.
 */
#ifndef ENG_C25_SEM_H
#define ENG_C25_SEM_H
#include <stdio.h>
#include "c25.h"

static inline int32_t s32(int64_t v) { return (int32_t)(uint32_t)(uint64_t)v; }
static inline int32_t s16(uint32_t v) { return (int16_t)(uint16_t)v; }

/* the word after the opcode; PC moves past it either way */
static inline uint16_t c25_imm(c71_t *d)
{
    uint16_t v = d->ops ? d->ops[1] : d->prog[d->pc];
    d->pc++;
    return v;
}

/* ---------------------------------------------------------- addressing ---- */

static inline uint16_t rc_add(uint16_t a, uint16_t b, int sub)
{
    if (sub) b = (uint16_t)(~b + 1);
    uint16_t res = 0; int carry = 0;
    for (int bit = 15; bit >= 0; bit--) {
        int s = ((a >> bit) & 1) + ((b >> bit) & 1) + carry;
        res |= (uint16_t)((s & 1) << bit);
        carry = s >> 1;
    }
    return res;
}

static inline uint16_t ind(c71_t *d, int op)
{
    uint16_t ar = d->ar[d->arp];
    int mode = (op >> 4) & 7, nar = op & 7, upd = (op >> 3) & 1;
    switch (mode) {
    case 1: d->ar[d->arp] = ar - 1; break;
    case 2: d->ar[d->arp] = ar + 1; break;
    case 4: d->ar[d->arp] = rc_add(ar, d->ar[0], 1); break;
    case 5: d->ar[d->arp] = ar - d->ar[0]; break;
    case 6: d->ar[d->arp] = ar + d->ar[0]; break;
    case 7: d->ar[d->arp] = rc_add(ar, d->ar[0], 0); break;
    default: break;
    }
    if (upd) { d->arb = d->arp; d->arp = nar; }
    return ar;
}

static inline uint32_t dma(c71_t *d, int lo)
{
    if (lo & 0x80) return ind(d, lo);
    return ((uint32_t)d->dp << 7) | (lo & 0x7F);
}

/* ------------------------------------------------------------- helpers ---- */

static inline int64_t load16(c71_t *d, uint32_t v, int shift)
{
    int64_t x = v & 0xFFFF;
    if (d->sxm && (x & 0x8000)) x -= 0x10000;
    return x * ((int64_t)1 << shift);
}

static inline int32_t sat(c71_t *d, int64_t v)
{
    if (d->ovm) {
        if (v > 0x7FFFFFFFLL) return 0x7FFFFFFF;
        if (v < -0x80000000LL) return (int32_t)0x80000000;
    }
    return s32(v);
}

static inline int64_t pshift(c71_t *d)
{
    int64_t p = s32(d->p);
    switch (d->pm) {
    case 0: return p;
    case 1: return s32(p * 2);
    case 2: return s32(p * 16);
    default: return p >> 6;             /* arithmetic, like Python's >> */
    }
}

/* Accumulator add/subtract with the C25 carry rule, exactly as MAME's
 * tms320c2x core computes it: after the (possibly saturated) result, an add
 * carries when the new ACC is unsigned-below the old one, and a subtract
 * clears C (borrow) when the new ACC is unsigned-above it. The interpreter
 * this was ported from never modelled C except SC/RC/LST1, so ADDH followed by
 * ADDC -- the master's 32-bit add idiom -- lost the carry between the halves
 * and every such sum came out one LSB low. */
static inline void add_c(c71_t *d, int64_t alu)
{
    uint32_t old = (uint32_t)d->acc;
    d->acc = sat(d, (int64_t)(int32_t)old + s32(alu));
    d->c = old > (uint32_t)d->acc;
}

static inline void sub_c(c71_t *d, int64_t alu)
{
    uint32_t old = (uint32_t)d->acc;
    d->acc = sat(d, (int64_t)(int32_t)old - s32(alu));
    d->c = !(old < (uint32_t)d->acc);
}

static inline bool fault(c71_t *d, const char *what, int op, int pc)
{
    snprintf(d->error, sizeof d->error, "%s %04X at %04X", what, op & 0xFFFF, pc);
    return false;
}

static inline bool push(c71_t *d, uint16_t v)
{
    if (d->sp >= 64) { snprintf(d->error, sizeof d->error, "stack overflow"); return false; }
    d->stack[d->sp++] = v; return true;
}

/* ------------------------------------------------------------ execute ---- */

static inline bool misc(c71_t *d, int op, int pc)
{
    int lo = op & 0xFF;
    int32_t a;
    if (lo == 0x04) d->cnf = 0;
    else if (lo == 0x05) d->cnf = 1;
    else if (lo == 0x00) d->intm = 0;
    else if (lo == 0x01) d->intm = 1;
    else if (lo == 0x02) d->ovm = 0;
    else if (lo == 0x03) d->ovm = 1;
    else if (lo == 0x06) d->sxm = 0;
    else if (lo == 0x07) d->sxm = 1;
    else if (lo >= 0x08 && lo <= 0x0B) d->pm = lo & 3;
    else if (lo == 0x1B) {                                  /* ABS */
        a = d->acc;
        if (a < 0) { d->acc = s32(-(int64_t)a); if ((uint32_t)d->acc == 0x80000000u && d->ovm) d->acc = 0x7FFFFFFF; }
        d->c = 0;
    }
    else if (lo == 0x0E || lo == 0x0F || lo == 0x20 || lo == 0x21 || lo == 0x36 || lo == 0x37) { /* FORT/RTXM/STXM/RFSM/SFSM */ }
    else if (lo == 0x1C) { if (!push(d, (uint16_t)d->acc)) return false; }           /* PUSH */
    else if (lo == 0x1D) {                                                               /* POP */
        if (d->sp == 0) { snprintf(d->error, sizeof d->error, "POP on empty stack at %04X", pc); return false; }
        d->acc = d->stack[--d->sp];
    }
    else if (lo == 0x1E) { if (!push(d, d->pc)) return false; d->pc = 0x001E; }          /* TRAP */
    else if (lo == 0x27) d->acc = (int32_t)~(uint32_t)d->acc;                            /* CMPL */
    else if (lo == 0x34) { uint32_t o = (uint32_t)d->acc;                                /* ROL */
                           d->acc = (int32_t)((o << 1) | (uint32_t)d->c); d->c = o >> 31; }
    else if (lo == 0x35) { uint32_t o = (uint32_t)d->acc;                                /* ROR */
                           d->acc = (int32_t)((o >> 1) | ((uint32_t)d->c << 31)); d->c = o & 1; }
    else if (lo == 0x0C || lo == 0x0D) { /* RXF/SXF */ }
    else if (lo == 0x30) d->c = 0;
    else if (lo == 0x31) d->c = 1;
    else if (lo == 0x32) d->tc = 0;
    else if (lo == 0x33) d->tc = 1;
    else if (lo == 0x14) d->acc = s32(pshift(d));
    else if (lo == 0x15) add_c(d, pshift(d));               /* APAC */
    else if (lo == 0x16) sub_c(d, pshift(d));               /* SPAC */
    else if (lo == 0x18) { d->c = ((uint32_t)d->acc >> 31) & 1; d->acc = s32((int64_t)d->acc * 2); }  /* SFL */
    else if (lo == 0x19) {                                  /* SFR */
        uint32_t old = (uint32_t)d->acc, nv = (old >> 1) & 0x7FFFFFFF;
        if (d->sxm && (old & 0x80000000u)) nv |= 0x80000000u;
        d->acc = (int32_t)nv; d->c = old & 1;
    }
    else if (lo == 0x1F) { if (d->idle_halts) { d->intm = 0; d->idle = 1; } }   /* IDLE: (TI) INTM=0, halt until an interrupt is taken */
    else if (lo == 0x23) {                                  /* NEG */
        if ((uint32_t)d->acc == 0x80000000u) { if (d->ovm) d->acc = 0x7FFFFFFF; }
        else d->acc = s32(-(int64_t)d->acc);
        d->c = d->acc == 0;
    }
    else if (lo == 0x24) {                                  /* CALA */
        if (!push(d, d->pc)) return false;
        d->pc = (uint16_t)d->acc;
    }
    else if (lo == 0x25) d->pc = (uint16_t)d->acc;           /* BACC */
    else if (lo == 0x26) {                                  /* RET */
        if (d->sp == 0) { snprintf(d->error, sizeof d->error, "RET on empty stack at %04X", pc); return false; }
        d->pc = d->stack[--d->sp];
    }
    else if (lo >= 0x50 && lo <= 0x57) {                    /* CMPR */
        uint16_t x = d->ar[d->arp], y = d->ar[0];
        switch (lo & 3) {
        case 0: d->tc = x == y; break;
        case 1: d->tc = x < y; break;
        case 2: d->tc = x > y; break;
        default: d->tc = x != y; break;
        }
    }
    else if (lo & 0x80) {                                   /* NORM */
        a = d->acc;
        if (a != 0 && (((a >> 30) & 1) == ((a >> 31) & 1))) {
            d->tc = 0; d->acc = s32((int64_t)a * 2); ind(d, lo);
        } else d->tc = 1;
    }
    else return fault(d, "CE", op, pc);
    return true;
}

/* ONE instruction word `op` at `pc`, iteration `it` of its RPT repeats. */
static inline bool c25_exec(c71_t *d, int pc, int op, int it)
{
    int hi = (op >> 8) & 0xFF, lo = op & 0xFF;
    uint32_t a, v;

    if (hi <= 0x0F)        add_c(d, load16(d, c25_dr(d, dma(d, lo)), hi));                       /* ADD */
    else if (hi <= 0x1F)   sub_c(d, load16(d, c25_dr(d, dma(d, lo)), hi & 0xF));                 /* SUB */
    else if (hi <= 0x2F)   d->acc = s32(load16(d, c25_dr(d, dma(d, lo)), hi & 0xF));
    else if (hi <= 0x37)   d->ar[hi & 7] = c25_dr(d, dma(d, lo));
    else if (hi == 0x38)   d->p = (int64_t)s16(d->t) * s16(c25_dr(d, dma(d, lo)));
    else if (hi == 0x39) { add_c(d, pshift(d));
                           v = c25_dr(d, dma(d, lo)); d->t = v; d->p = (int64_t)s16(v) * s16(v); }
    else if (hi == 0x3A) { add_c(d, pshift(d));
                           d->p = (int64_t)s16(d->t) * s16(c25_dr(d, dma(d, lo))); }
    else if (hi == 0x3B) { sub_c(d, pshift(d));
                           d->p = (int64_t)s16(d->t) * s16(c25_dr(d, dma(d, lo))); }
    else if (hi == 0x58) {                                  /* TBLR */
        d->pfc = (uint16_t)d->acc;
        c25_dw(d, dma(d, lo), c25_pr(d, d->pfc));
        d->pfc++;
    }
    else if (hi == 0x59) {                                  /* TBLW */
        d->pfc = (uint16_t)d->acc;
        c25_pw(d, d->pfc, c25_dr(d, dma(d, lo)));
        d->pfc++;
    }
    else if (hi == 0x5A) { sub_c(d, pshift(d));
                           v = c25_dr(d, dma(d, lo)); d->t = v; d->p = (int64_t)s16(v) * s16(v); }
    else if (hi == 0x56) { a = dma(d, lo); c25_dw(d, a + 1, c25_dr(d, a)); }          /* DMOV */
    else if (hi == 0x53)   d->p = (d->p & 0xFFFF) | ((int64_t)c25_dr(d, dma(d, lo)) << 16);  /* LPH */
    else if (hi == 0x3C)   d->t = c25_dr(d, dma(d, lo));                           /* LT */
    else if (hi == 0x3D) { v = c25_dr(d, dma(d, lo));                              /* LTA */
                           add_c(d, pshift(d)); d->t = v; }
    else if (hi == 0x3E) { d->t = c25_dr(d, dma(d, lo)); d->acc = s32(pshift(d)); }  /* LTP */
    else if (hi == 0x3F) { a = dma(d, lo); d->t = c25_dr(d, a);                    /* LTD */
                           add_c(d, pshift(d)); c25_dw(d, a + 1, c25_dr(d, a)); }
    else if (hi == 0x40)   d->acc = s32((int64_t)c25_dr(d, dma(d, lo)) << 16);     /* ZALH */
    else if (hi == 0x41)   d->acc = c25_dr(d, dma(d, lo));                          /* ZALS */
    else if (hi == 0x42)   d->acc = s32(load16(d, c25_dr(d, dma(d, lo)), d->t & 0xF));     /* LACT */
    else if (hi == 0x43) {                                  /* ADDC */
        uint32_t old = (uint32_t)d->acc;
        d->acc = sat(d, (int64_t)(int32_t)old + c25_dr(d, dma(d, lo)) + d->c);
        if ((uint32_t)d->acc != old) d->c = old > (uint32_t)d->acc;
    }
    else if (hi == 0x4F) {                                  /* SUBB */
        uint32_t old = (uint32_t)d->acc;
        d->acc = sat(d, (int64_t)(int32_t)old - c25_dr(d, dma(d, lo)) - (d->c ? 0 : 1));
        if ((uint32_t)d->acc != old) d->c = !(old < (uint32_t)d->acc);
    }
    else if (hi == 0x44) {                                  /* SUBH: 16-bit on the high word */
        uint32_t old = (uint32_t)d->acc; uint16_t oh = old >> 16, m = c25_dr(d, dma(d, lo));
        uint16_t nh = (uint16_t)(oh - m);
        if (oh < nh) d->c = 0;
        if ((int16_t)((oh ^ m) & (oh ^ nh)) < 0 && d->ovm) nh = (int16_t)oh < 0 ? 0x8000 : 0x7FFF;
        d->acc = (int32_t)(((uint32_t)nh << 16) | (old & 0xFFFF));
    }
    else if (hi == 0x45)   sub_c(d, c25_dr(d, dma(d, lo)));                                 /* SUBS */
    else if (hi == 0x46)   sub_c(d, load16(d, c25_dr(d, dma(d, lo)), d->t & 0xF));          /* SUBT */
    else if (hi == 0x49)   add_c(d, c25_dr(d, dma(d, lo)));                                 /* ADDS */
    else if (hi == 0x4A)   add_c(d, load16(d, c25_dr(d, dma(d, lo)), d->t & 0xF));          /* ADDT */
    else if (hi == 0x47) {                                  /* SUBC */
        uint32_t x = c25_dr(d, dma(d, lo)) & 0xFFFF;
        if (d->sxm && (x & 0x8000)) x |= 0xFFFF0000u;
        uint32_t alu = x << 15, old = (uint32_t)d->acc, res = old - alu;
        d->c = !(old < res);
        d->acc = old >= alu ? (int32_t)((res << 1) | 1) : (int32_t)(old << 1);
    }
    else if (hi == 0x7B)   d->acc = s32(((int64_t)c25_dr(d, dma(d, lo)) << 16) | 0x8000);  /* ZALR */
    else if (hi == 0x4B)   d->rpt = c25_dr(d, dma(d, lo)) & 0xFF;                          /* RPT */
    else if (hi == 0x48) {                                  /* ADDH: 16-bit on the high word */
        uint32_t old = (uint32_t)d->acc; uint16_t oh = old >> 16, m = c25_dr(d, dma(d, lo));
        uint16_t nh = (uint16_t)(oh + m);
        if (oh > nh) d->c = 1;                              /* set on carry, never cleared */
        if ((int16_t)((nh ^ m) & (oh ^ nh)) < 0 && d->ovm) nh = (int16_t)oh < 0 ? 0x8000 : 0x7FFF;
        d->acc = (int32_t)(((uint32_t)nh << 16) | (old & 0xFFFF));
    }
    else if (hi == 0x4C)   d->acc = (int32_t)((uint32_t)d->acc ^ c25_dr(d, dma(d, lo)));   /* XOR */
    else if (hi == 0x4D)   d->acc = (int32_t)((uint32_t)d->acc | c25_dr(d, dma(d, lo)));   /* OR */
    else if (hi == 0x4E)   d->acc = (int32_t)((uint32_t)d->acc & c25_dr(d, dma(d, lo)));   /* AND */
    else if (hi == 0x54) { if (!push(d, c25_dr(d, dma(d, lo)))) return false; }         /* PSHD */
    else if (hi == 0x7A) {                                                          /* POPD */
        if (d->sp == 0) { snprintf(d->error, sizeof d->error, "POPD on empty stack at %04X", pc); return false; }
        c25_dw(d, dma(d, lo), d->stack[--d->sp]);
    }
    else if (hi == 0x57)   d->tc = (c25_dr(d, dma(d, lo)) >> (15 - (d->t & 0xF))) & 1;    /* BITT */
    else if (hi == 0x5B) { v = c25_dr(d, dma(d, lo)); d->t = v; sub_c(d, pshift(d)); }  /* LTS */
    else if (hi == 0x5C || hi == 0x5D) {                                            /* MACD / MAC */
        if (it == 0) d->pfc = c25_imm(d);
        add_c(d, pshift(d));
        a = dma(d, lo); v = c25_dr(d, a);
        if (hi == 0x5C && ((lo & 0x80) || it == 0)) c25_dw(d, a + 1, v);
        d->t = v;
        d->p = (int64_t)s16(v) * s16(c25_pr(d, d->pfc));
        d->pfc++;
    }
    else if (hi == 0x5E || hi == 0x5F) {                                            /* BC / BNC */
        uint16_t tgt = c25_imm(d);
        if (lo & 0x80) ind(d, lo);
        if ((hi == 0x5E) == (d->c != 0)) d->pc = tgt;
    }
    else if (hi == 0x7C || hi == 0x7D) {                                            /* SPL / SPH */
        uint32_t x = (uint32_t)s32(pshift(d));
        c25_dw(d, dma(d, lo), hi == 0x7C ? (x & 0xFFFF) : (x >> 16));
    }
    else if (hi >= 0xA0 && hi <= 0xBF)                                              /* MPYK */
        d->p = (int64_t)s16(d->t) * ((int16_t)(uint16_t)(op << 3) >> 3);
    else if (hi == 0x50) { v = c25_dr(d, dma(d, lo));                              /* LST */
                           d->arp = (v >> 13) & 7; d->ovm = (v >> 11) & 1;
                           d->intm = (v >> 9) & 1; d->dp = v & 0x1FF; }
    else if (hi == 0x51) { v = c25_dr(d, dma(d, lo));                              /* LST1 */
                           d->arb = (v >> 13) & 7; d->tc = (v >> 11) & 1;
                           d->sxm = (v >> 10) & 1; d->c = (v >> 9) & 1; d->pm = v & 3; }
    else if (hi == 0x78)   c25_dw(d, dma(d, lo), ((d->arp & 7) << 13) | (d->ovm << 11) | (1 << 10)
                                              | (d->intm << 9) | (d->dp & 0x1FF));   /* SST */
    else if (hi == 0x79)   c25_dw(d, dma(d, lo), ((d->arb & 7) << 13) | (d->tc << 11) | (d->sxm << 10)
                                              | (d->c << 9) | (d->pm & 3));          /* SST1 */
    else if (hi == 0x52)   d->dp = c25_dr(d, dma(d, lo)) & 0x1FF;                  /* LDP */
    else if (hi == 0x55) { if (lo & 0x80) ind(d, lo); }                        /* MAR/LARP */
    else if (hi >= 0x60 && hi <= 0x67)                                         /* SACL */
        c25_dw(d, dma(d, lo), (uint32_t)(((int64_t)d->acc << (hi & 7)) & 0xFFFF));
    else if (hi >= 0x68 && hi <= 0x6F)                                         /* SACH */
        c25_dw(d, dma(d, lo), (uint32_t)((((int64_t)d->acc << (hi & 7)) >> 16) & 0xFFFF));
    else if (hi >= 0x70 && hi <= 0x77) c25_dw(d, dma(d, lo), d->ar[hi & 7]);       /* SAR */
    else if (hi == 0x7E)   d->ar[d->arp] += lo;                                /* ADRK (8-bit k) */
    else if (hi == 0x7F)   d->ar[d->arp] -= lo;                                /* SBRK */
    else if (hi >= 0x80 && hi <= 0x8F) { a = dma(d, lo); c25_dw(d, a, c25_port_in(d, hi & 0xF)); }  /* IN */
    else if (hi >= 0xE0 && hi <= 0xEF) c25_port_out(d, hi & 0xF, c25_dr(d, dma(d, lo)));            /* OUT */
    else if (hi >= 0x90 && hi <= 0x9F) d->tc = (c25_dr(d, dma(d, lo)) >> (15 - (hi & 0xF))) & 1; /* BIT */
    else if (hi == 0xC8 || hi == 0xC9) d->dp = op & 0x1FF;                     /* LDPK */
    else if (hi == 0xCA)   d->acc = lo;                                        /* LACK */
    else if (hi >= 0xC0 && hi <= 0xC7) d->ar[hi & 7] = lo;                     /* LARK */
    else if (hi == 0xCB)   d->rpt = lo;                                        /* RPTK */
    else if (hi == 0xCC)   add_c(d, lo);                                       /* ADDK */
    else if (hi == 0xCD)   sub_c(d, lo);                                       /* SUBK */
    else if (hi == 0xCF)   d->p = (int64_t)(uint16_t)d->t * (uint16_t)c25_dr(d, dma(d, lo));   /* MPYU */
    else if (hi == 0xCE)   return misc(d, op, pc);
    else if ((hi & 0xF0) == 0xD0) {                         /* D-block, long immediate */
        uint16_t imm = c25_imm(d);
        int sh = hi & 0xF;
        switch (lo) {
        case 0x00: d->ar[sh & 7] = imm; break;                                  /* LRLK */
        case 0x01: d->acc = s32(load16(d, imm, sh)); break;                     /* LALK */
        case 0x02: add_c(d, load16(d, imm, sh)); break;                         /* ADLK */
        case 0x03: sub_c(d, load16(d, imm, sh)); break;                         /* SBLK */
        case 0x04: d->acc = (int32_t)((uint32_t)d->acc & (uint32_t)((uint64_t)imm << sh)); break;
        case 0x05: d->acc = (int32_t)((uint32_t)d->acc | (uint32_t)((uint64_t)imm << sh)); break;
        case 0x06: d->acc = (int32_t)((uint32_t)d->acc ^ (uint32_t)((uint64_t)imm << sh)); break;
        default: return fault(d, "D-block", op, pc);
        }
    }
    else if (hi == 0xFF && (lo & 0x80)) {                   /* B */
        uint16_t tgt = c25_imm(d);
        if (lo != 0x80) ind(d, lo);
        d->pc = tgt;
    }
    else if (hi == 0xFE && (lo & 0x80)) {                   /* CALL */
        uint16_t tgt = c25_imm(d);
        if (lo != 0x80) ind(d, lo);
        if (!push(d, d->pc)) return false;
        d->pc = tgt;
    }
    else if ((hi >= 0xF1 && hi <= 0xF6) || hi == 0xF8 || hi == 0xF9 || hi == 0xFA) {
        if (!(lo & 0x80)) return fault(d, "op", op, pc);
        uint16_t tgt = c25_imm(d);
        if (lo != 0x80) ind(d, lo);
        int32_t x = d->acc; int take = 0;
        switch (hi) {
        case 0xF1: take = x > 0; break;   case 0xF2: take = x <= 0; break;
        case 0xF3: take = x < 0; break;   case 0xF4: take = x >= 0; break;
        case 0xF5: take = x != 0; break;  case 0xF6: take = x == 0; break;
        case 0xF8: take = d->tc == 0; break;
        case 0xF9: take = d->tc == 1; break;
        case 0xFA: take = d->bioz == 0; break;
        }
        if (take) d->pc = tgt;
    }
    else if (hi == 0xFB && (lo & 0x80)) {                   /* BANZ */
        uint16_t tgt = c25_imm(d);
        uint16_t cur = d->ar[d->arp];
        ind(d, lo);
        if (cur != 0) d->pc = tgt;
    }
    else if (hi == 0xFC || hi == 0xFD) {                    /* BLKP / BLKD */
        if (it == 0) d->pfc = c25_imm(d);
        uint16_t src = hi == 0xFC ? c25_pr(d, d->pfc) : c25_dr(d, d->pfc);
        c25_dw(d, dma(d, lo), src);
        d->pfc++;
    }
    else return fault(d, "op", op, pc);
    return true;
}


#endif
