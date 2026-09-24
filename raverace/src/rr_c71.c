/*
 * c71_master.c -- the master DSP (TMS320C25) and its Namco bus.
 *
 * A line-for-line C port of the MiSTer project's tools/pc_tms_interp.py.
 * Every semantic below -- including the ones the Python comments justify
 * against MAME's tms32025.cpp (SUBC's unsigned compare, SFR's SXM-gated
 * sign, NORM's two guards, TBLR/TBLW/BLKP/BLKD, the per-instruction timer)
 * -- is kept exactly, because the Python is what is gated against MAME.
 * Arithmetic that Python does on unbounded integers is done here in int64
 * and then wrapped or saturated where the Python calls s32()/sat().
 */
#include <stdio.h>
#include <string.h>
#include "rr_c71.h"

static int32_t s32(int64_t v) { return (int32_t)(uint32_t)(uint64_t)v; }
static int32_t s16(uint32_t v) { return (int16_t)(uint16_t)v; }
/* MAME's signed24(): polygon RAM and point data are sign-extended to 32 bits */
static uint32_t sx24(uint32_t v) { return (uint32_t)((int32_t)(v << 8) >> 8); }

/* ---------------------------------------------------------------- bus ---- */

static uint32_t point_read(c71_t *d, uint32_t a)
{
    a &= 0xFFFFFF;
    if (a < d->ptrom_words) return sx24(d->ptrom[a]);
    if (a >= d->ptram_base && a < d->ptram_base + C71_PTRAM_WORDS)
        return sx24(d->ptram[a - d->ptram_base]);
    return 0xFFFFFFFF;
}

static void point_write(c71_t *d, uint32_t a, uint32_t v)
{
    a &= 0xFFFFFF;
    /* Writes anywhere are accepted, but only the point-RAM window reads back
     * (the Python keeps a dict yet reads it only in that window). */
    if (a >= d->ptram_base && a < d->ptram_base + C71_PTRAM_WORDS)
        d->ptram[a - d->ptram_base] = v & 0xFFFFFF;
}

static void mark(c71_t *d, uint32_t i)
{
    if (!d->written[i]) { d->written[i] = 1; d->n_written++; }
}

static uint32_t poly_r(c71_t *d, uint32_t i) { return d->poly[i & 0x7FFF]; }
static void poly_w24(c71_t *d, uint32_t i, uint32_t v)
{
    d->poly[i & 0x7FFF] = sx24(v);          /* MAME stores the 24 bits sign-extended */
    mark(d, i & 0x7FFF);
}

/* the 16-bit banked window onto polygon RAM (MAME namcos22_dspram16_r/w) */
static uint16_t dsp_r(c71_t *d, uint32_t off)
{
    uint32_t v = d->poly[off & 0x7FFF];
    switch (d->bank & 3) {
    case 0: return v & 0xFFFF;
    case 1: return (v >> 16) & 0xFFFF;
    case 2: d->latch = (v >> 16) & 0xFFFF; return v & 0xFFFF;
    default: return 0;
    }
}

static void dsp_w(c71_t *d, uint32_t off, uint16_t data)
{
    uint32_t v = d->poly[off & 0x7FFF];
    uint16_t lo = v & 0xFFFF, hi = (v >> 16) & 0xFFFF;
    switch (d->bank & 3) {
    case 0: lo = data; break;
    case 1: hi = data; break;
    case 2: lo = data; hi = d->latch; break;
    default: break;
    }
    d->poly[off & 0x7FFF] = ((uint32_t)hi << 16) | lo;
    mark(d, off & 0x7FFF);
}

/* MAME pdp_handle_commands, memory effects only */
static void pdp_run(c71_t *d)
{
    uint32_t offs = poly_r(d, 0x7FFF) & 0x7FFF;
    for (int guard = 0; guard < 200000; guard++) {
        offs &= 0x7FFF;
        uint32_t start = offs;
        uint32_t cmd = poly_r(d, offs) & 0xFFFF, src, dst, n;
        offs++;
        switch (cmd) {
        case 0xFFF0: break;
        case 0xFFF5:
            dst = poly_r(d, offs++); n = poly_r(d, offs++);
            point_write(d, dst, n);
            break;
        case 0xFFF6:
            src = poly_r(d, offs++); dst = poly_r(d, offs++);
            poly_w24(d, dst & 0x7FFF, point_read(d, src));
            break;
        case 0xFFF7:
            src = poly_r(d, offs++); dst = poly_r(d, offs++); n = poly_r(d, offs++) & 0xFFFF;
            for (uint32_t k = 0; k < n; k++, src++, dst++)
                poly_w24(d, dst & 0x7FFF, poly_r(d, src & 0x7FFF));
            break;
        case 0xFFF8: offs++; break;
        case 0xFFFA:
            src = poly_r(d, offs++); dst = poly_r(d, offs++); n = poly_r(d, offs++) & 0xFFFF;
            for (uint32_t k = 0; k < n; k++, src++, dst++)
                poly_w24(d, dst & 0x7FFF, point_read(d, src));
            break;
        case 0xFFFB:
            dst = poly_r(d, offs++); n = poly_r(d, offs++) & 0xFFFF;
            for (uint32_t k = 0; k < n; k++, dst++, offs++)
                point_write(d, dst, poly_r(d, offs & 0x7FFF));
            break;
        case 0xFFFC:
            src = poly_r(d, offs++); dst = poly_r(d, offs++); n = poly_r(d, offs++) & 0xFFFF;
            for (uint32_t k = 0; k < n; k++, src++, dst++)
                point_write(d, dst, point_read(d, src));
            break;
        case 0xFFFD:                              /* direct render cmd: skip */
            n = poly_r(d, offs++) & 0xFFFF; offs += n;
            break;
        case 0xFFFE: offs++; break;               /* marker + arg */
        case 0xFFFF:                              /* goto; bail on goto-self */
            offs = poly_r(d, offs) & 0x7FFF;
            if (offs == start) return;
            break;
        default:
            return;                               /* a display-list record */
        }
    }
    snprintf(d->error, sizeof d->error, "PDP runaway");
}

/* -------------------------------------------------------- data space ---- */

static uint16_t dr(c71_t *d, uint32_t a)
{
    a &= 0xFFFF;
    if (a >= 0x8000) return dsp_r(d, a - 0x8000);
    if (a >= 0x4000) return d->prog[a];
    if (a == 4) return d->imr;
    if (a == 2) return d->tim;
    return d->ram[a];
}

static void dw(c71_t *d, uint32_t a, uint32_t v)
{
    a &= 0xFFFF; v &= 0xFFFF;
    if (a == 2) d->tim = v;
    if (a == 3) d->prd = v;
    if (a == 4) d->imr = v;
    if (a >= 0x8000) dsp_w(d, a - 0x8000, v);
    else if (a >= 0x4000) d->prog[a] = v;
    else d->ram[a] = v;
}

/* --------------------------------------------------------------- ports ---- */

static uint16_t port_in(c71_t *d, int pa)
{
    uint32_t v;
    switch (pa) {
    /* MAME point_loword_r / point_hiword_ir: the HIGH read carries the
     * post-increment and a busy bit in bit 15 */
    case 0: v = point_read(d, d->pt_addr); return v & 0xFFFF;
    case 1: v = point_read(d, d->pt_addr); d->pt_addr++; return 0x8000 | ((v >> 16) & 0xFF);
    case 2: if (d->ss22) pdp_run(d); d->bioz = 0; d->pdp_begins++; if (d->pdp_begin) d->pdp_begin(); return 0;   /* PDP begin, BIO ready */
    case 3: d->bioz = 1; if (d->port3_r) d->port3_r(); return 0;                   /* dsp_unk_port3_r: BIO busy again */
    case 8: return 0x0000;                           /* dsp_unk8_r: not busy */
    case 9: return 0x0063;                           /* custom_ic_status_r */
    default: return 0;
    }
}

static void port_out(c71_t *d, int pa, uint16_t v)
{
    switch (pa) {
    /* MAME point_hiword_w / point_loword_iw / point_address_w */
    case 1: d->pt_data = (uint32_t)v << 16; break;
    case 0: d->pt_data |= v; point_write(d, d->pt_addr++, d->pt_data); break;
    case 3: d->pt_addr = (d->pt_addr << 16) | v; break;
    case 0xD: d->bank = v & 3; break;
    case 2: d->pdp_base = v; break;
    case 8: if (d->render_reset) d->render_reset(); break;
    case 0xC: if (d->render_w) d->render_w(v); break;
    case 7: if (d->slave_w) d->slave_w(v); break;
    default: break;          /* 7 slave upload, 8, A, B, C render, E led: ignored */
    }
}

/* ---------------------------------------------------------- addressing ---- */

static uint16_t rc_add(uint16_t a, uint16_t b, int sub)
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

static uint16_t ind(c71_t *d, int op)
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

static uint32_t dma(c71_t *d, int lo)
{
    if (lo & 0x80) return ind(d, lo);
    return ((uint32_t)d->dp << 7) | (lo & 0x7F);
}

/* ------------------------------------------------------------- helpers ---- */

static int64_t load16(c71_t *d, uint32_t v, int shift)
{
    int64_t x = v & 0xFFFF;
    if (d->sxm && (x & 0x8000)) x -= 0x10000;
    return x * ((int64_t)1 << shift);
}

static int32_t sat(c71_t *d, int64_t v)
{
    if (d->ovm) {
        if (v > 0x7FFFFFFFLL) return 0x7FFFFFFF;
        if (v < -0x80000000LL) return (int32_t)0x80000000;
    }
    return s32(v);
}

static int64_t pshift(c71_t *d)
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
static void add_c(c71_t *d, int64_t alu)
{
    uint32_t old = (uint32_t)d->acc;
    d->acc = sat(d, (int64_t)(int32_t)old + s32(alu));
    d->c = old > (uint32_t)d->acc;
}

static void sub_c(c71_t *d, int64_t alu)
{
    uint32_t old = (uint32_t)d->acc;
    d->acc = sat(d, (int64_t)(int32_t)old - s32(alu));
    d->c = !(old < (uint32_t)d->acc);
}

static bool fault(c71_t *d, const char *what, int op, int pc)
{
    snprintf(d->error, sizeof d->error, "%s %04X at %04X", what, op & 0xFFFF, pc);
    return false;
}

static bool push(c71_t *d, uint16_t v)
{
    if (d->sp >= 64) { snprintf(d->error, sizeof d->error, "stack overflow"); return false; }
    d->stack[d->sp++] = v; return true;
}

/* ------------------------------------------------------------ execute ---- */

static bool misc(c71_t *d, int op, int pc)
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
    else if (lo == 0x1F) { d->intm = 0; d->idle = 1; }   /* IDLE: global interrupt enable (INTM=0), halt until one is taken */
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

static bool exec1(c71_t *d, int pc, int op, int it)
{
    int hi = (op >> 8) & 0xFF, lo = op & 0xFF;
    uint32_t a, v;

    if (hi <= 0x0F)        add_c(d, load16(d, dr(d, dma(d, lo)), hi));                       /* ADD */
    else if (hi <= 0x1F)   sub_c(d, load16(d, dr(d, dma(d, lo)), hi & 0xF));                 /* SUB */
    else if (hi <= 0x2F)   d->acc = s32(load16(d, dr(d, dma(d, lo)), hi & 0xF));
    else if (hi <= 0x37)   d->ar[hi & 7] = dr(d, dma(d, lo));
    else if (hi == 0x38)   d->p = (int64_t)s16(d->t) * s16(dr(d, dma(d, lo)));
    else if (hi == 0x39) { add_c(d, pshift(d));
                           v = dr(d, dma(d, lo)); d->t = v; d->p = (int64_t)s16(v) * s16(v); }
    else if (hi == 0x3A) { add_c(d, pshift(d));
                           d->p = (int64_t)s16(d->t) * s16(dr(d, dma(d, lo))); }
    else if (hi == 0x3B) { sub_c(d, pshift(d));
                           d->p = (int64_t)s16(d->t) * s16(dr(d, dma(d, lo))); }
    else if (hi == 0x58) {                                  /* TBLR */
        d->pfc = (uint16_t)d->acc;
        dw(d, dma(d, lo), d->prog[d->pfc]);
        d->pfc++;
    }
    else if (hi == 0x59) {                                  /* TBLW */
        d->pfc = (uint16_t)d->acc;
        d->prog[d->pfc] = dr(d, dma(d, lo));
        d->pfc++;
    }
    else if (hi == 0x5A) { sub_c(d, pshift(d));
                           v = dr(d, dma(d, lo)); d->t = v; d->p = (int64_t)s16(v) * s16(v); }
    else if (hi == 0x56) { a = dma(d, lo); dw(d, a + 1, dr(d, a)); }          /* DMOV */
    else if (hi == 0x53)   d->p = (d->p & 0xFFFF) | ((int64_t)dr(d, dma(d, lo)) << 16);  /* LPH */
    else if (hi == 0x3C)   d->t = dr(d, dma(d, lo));                           /* LT */
    else if (hi == 0x3D) { v = dr(d, dma(d, lo));                              /* LTA */
                           add_c(d, pshift(d)); d->t = v; }
    else if (hi == 0x3E) { d->t = dr(d, dma(d, lo)); d->acc = s32(pshift(d)); }  /* LTP */
    else if (hi == 0x3F) { a = dma(d, lo); d->t = dr(d, a);                    /* LTD */
                           add_c(d, pshift(d)); dw(d, a + 1, dr(d, a)); }
    else if (hi == 0x40)   d->acc = s32((int64_t)dr(d, dma(d, lo)) << 16);     /* ZALH */
    else if (hi == 0x41)   d->acc = dr(d, dma(d, lo));                          /* ZALS */
    else if (hi == 0x42)   d->acc = s32(load16(d, dr(d, dma(d, lo)), d->t & 0xF));     /* LACT */
    else if (hi == 0x43) {                                  /* ADDC */
        uint32_t old = (uint32_t)d->acc;
        d->acc = sat(d, (int64_t)(int32_t)old + dr(d, dma(d, lo)) + d->c);
        if ((uint32_t)d->acc != old) d->c = old > (uint32_t)d->acc;
    }
    else if (hi == 0x4F) {                                  /* SUBB */
        uint32_t old = (uint32_t)d->acc;
        d->acc = sat(d, (int64_t)(int32_t)old - dr(d, dma(d, lo)) - (d->c ? 0 : 1));
        if ((uint32_t)d->acc != old) d->c = !(old < (uint32_t)d->acc);
    }
    else if (hi == 0x44) {                                  /* SUBH: 16-bit on the high word */
        uint32_t old = (uint32_t)d->acc; uint16_t oh = old >> 16, m = dr(d, dma(d, lo));
        uint16_t nh = (uint16_t)(oh - m);
        if (oh < nh) d->c = 0;
        if ((int16_t)((oh ^ m) & (oh ^ nh)) < 0 && d->ovm) nh = (int16_t)oh < 0 ? 0x8000 : 0x7FFF;
        d->acc = (int32_t)(((uint32_t)nh << 16) | (old & 0xFFFF));
    }
    else if (hi == 0x45)   sub_c(d, dr(d, dma(d, lo)));                                 /* SUBS */
    else if (hi == 0x46)   sub_c(d, load16(d, dr(d, dma(d, lo)), d->t & 0xF));          /* SUBT */
    else if (hi == 0x49)   add_c(d, dr(d, dma(d, lo)));                                 /* ADDS */
    else if (hi == 0x4A)   add_c(d, load16(d, dr(d, dma(d, lo)), d->t & 0xF));          /* ADDT */
    else if (hi == 0x47) {                                  /* SUBC */
        uint32_t x = dr(d, dma(d, lo)) & 0xFFFF;
        if (d->sxm && (x & 0x8000)) x |= 0xFFFF0000u;
        uint32_t alu = x << 15, old = (uint32_t)d->acc, res = old - alu;
        d->c = !(old < res);
        d->acc = old >= alu ? (int32_t)((res << 1) | 1) : (int32_t)(old << 1);
    }
    else if (hi == 0x7B)   d->acc = s32(((int64_t)dr(d, dma(d, lo)) << 16) | 0x8000);  /* ZALR */
    else if (hi == 0x4B)   d->rpt = dr(d, dma(d, lo)) & 0xFF;                          /* RPT */
    else if (hi == 0x48) {                                  /* ADDH: 16-bit on the high word */
        uint32_t old = (uint32_t)d->acc; uint16_t oh = old >> 16, m = dr(d, dma(d, lo));
        uint16_t nh = (uint16_t)(oh + m);
        if (oh > nh) d->c = 1;                              /* set on carry, never cleared */
        if ((int16_t)((nh ^ m) & (oh ^ nh)) < 0 && d->ovm) nh = (int16_t)oh < 0 ? 0x8000 : 0x7FFF;
        d->acc = (int32_t)(((uint32_t)nh << 16) | (old & 0xFFFF));
    }
    else if (hi == 0x4C)   d->acc = (int32_t)((uint32_t)d->acc ^ dr(d, dma(d, lo)));   /* XOR */
    else if (hi == 0x4D)   d->acc = (int32_t)((uint32_t)d->acc | dr(d, dma(d, lo)));   /* OR */
    else if (hi == 0x4E)   d->acc = (int32_t)((uint32_t)d->acc & dr(d, dma(d, lo)));   /* AND */
    else if (hi == 0x54) { if (!push(d, dr(d, dma(d, lo)))) return false; }         /* PSHD */
    else if (hi == 0x7A) {                                                          /* POPD */
        if (d->sp == 0) { snprintf(d->error, sizeof d->error, "POPD on empty stack at %04X", pc); return false; }
        dw(d, dma(d, lo), d->stack[--d->sp]);
    }
    else if (hi == 0x57)   d->tc = (dr(d, dma(d, lo)) >> (15 - (d->t & 0xF))) & 1;    /* BITT */
    else if (hi == 0x5B) { v = dr(d, dma(d, lo)); d->t = v; sub_c(d, pshift(d)); }  /* LTS */
    else if (hi == 0x5C || hi == 0x5D) {                                            /* MACD / MAC */
        if (it == 0) d->pfc = d->prog[d->pc++];
        add_c(d, pshift(d));
        a = dma(d, lo); v = dr(d, a);
        if (hi == 0x5C && ((lo & 0x80) || it == 0)) dw(d, a + 1, v);
        d->t = v;
        d->p = (int64_t)s16(v) * s16(d->prog[d->pfc]);
        d->pfc++;
    }
    else if (hi == 0x5E || hi == 0x5F) {                                            /* BC / BNC */
        uint16_t tgt = d->prog[d->pc++];
        if (lo & 0x80) ind(d, lo);
        if ((hi == 0x5E) == (d->c != 0)) d->pc = tgt;
    }
    else if (hi == 0x7C || hi == 0x7D) {                                            /* SPL / SPH */
        uint32_t x = (uint32_t)s32(pshift(d));
        dw(d, dma(d, lo), hi == 0x7C ? (x & 0xFFFF) : (x >> 16));
    }
    else if (hi >= 0xA0 && hi <= 0xBF)                                              /* MPYK */
        d->p = (int64_t)s16(d->t) * ((int16_t)(uint16_t)(op << 3) >> 3);
    else if (hi == 0x50) { v = dr(d, dma(d, lo));                              /* LST */
                           d->arp = (v >> 13) & 7; d->ovm = (v >> 11) & 1;
                           d->intm = (v >> 9) & 1; d->dp = v & 0x1FF; }
    else if (hi == 0x51) { v = dr(d, dma(d, lo));                              /* LST1 */
                           d->arb = (v >> 13) & 7; d->tc = (v >> 11) & 1;
                           d->sxm = (v >> 10) & 1; d->c = (v >> 9) & 1; d->pm = v & 3; }
    else if (hi == 0x78)   dw(d, dma(d, lo), ((d->arp & 7) << 13) | (d->ovm << 11) | (1 << 10)
                                              | (d->intm << 9) | (d->dp & 0x1FF));   /* SST */
    else if (hi == 0x79)   dw(d, dma(d, lo), ((d->arb & 7) << 13) | (d->tc << 11) | (d->sxm << 10)
                                              | (d->c << 9) | (d->pm & 3));          /* SST1 */
    else if (hi == 0x52)   d->dp = dr(d, dma(d, lo)) & 0x1FF;                  /* LDP */
    else if (hi == 0x55) { if (lo & 0x80) ind(d, lo); }                        /* MAR/LARP */
    else if (hi >= 0x60 && hi <= 0x67)                                         /* SACL */
        dw(d, dma(d, lo), (uint32_t)(((int64_t)d->acc << (hi & 7)) & 0xFFFF));
    else if (hi >= 0x68 && hi <= 0x6F)                                         /* SACH */
        dw(d, dma(d, lo), (uint32_t)((((int64_t)d->acc << (hi & 7)) >> 16) & 0xFFFF));
    else if (hi >= 0x70 && hi <= 0x77) dw(d, dma(d, lo), d->ar[hi & 7]);       /* SAR */
    else if (hi == 0x7E)   d->ar[d->arp] += lo;                                /* ADRK (8-bit k) */
    else if (hi == 0x7F)   d->ar[d->arp] -= lo;                                /* SBRK */
    else if (hi >= 0x80 && hi <= 0x8F) { a = dma(d, lo); dw(d, a, port_in(d, hi & 0xF)); }  /* IN */
    else if (hi >= 0xE0 && hi <= 0xEF) port_out(d, hi & 0xF, dr(d, dma(d, lo)));            /* OUT */
    else if (hi >= 0x90 && hi <= 0x9F) d->tc = (dr(d, dma(d, lo)) >> (15 - (hi & 0xF))) & 1; /* BIT */
    else if (hi == 0xC8 || hi == 0xC9) d->dp = op & 0x1FF;                     /* LDPK */
    else if (hi == 0xCA)   d->acc = lo;                                        /* LACK */
    else if (hi >= 0xC0 && hi <= 0xC7) d->ar[hi & 7] = lo;                     /* LARK */
    else if (hi == 0xCB)   d->rpt = lo;                                        /* RPTK */
    else if (hi == 0xCC)   add_c(d, lo);                                       /* ADDK */
    else if (hi == 0xCD)   sub_c(d, lo);                                       /* SUBK */
    else if (hi == 0xCF)   d->p = (int64_t)(uint16_t)d->t * (uint16_t)dr(d, dma(d, lo));   /* MPYU */
    else if (hi == 0xCE)   return misc(d, op, pc);
    else if ((hi & 0xF0) == 0xD0) {                         /* D-block, long immediate */
        uint16_t imm = d->prog[d->pc++];
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
        uint16_t tgt = d->prog[d->pc++];
        if (lo != 0x80) ind(d, lo);
        d->pc = tgt;
    }
    else if (hi == 0xFE && (lo & 0x80)) {                   /* CALL */
        uint16_t tgt = d->prog[d->pc++];
        if (lo != 0x80) ind(d, lo);
        if (!push(d, d->pc)) return false;
        d->pc = tgt;
    }
    else if ((hi >= 0xF1 && hi <= 0xF6) || hi == 0xF8 || hi == 0xF9 || hi == 0xFA) {
        if (!(lo & 0x80)) return fault(d, "op", op, pc);
        uint16_t tgt = d->prog[d->pc++];
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
        uint16_t tgt = d->prog[d->pc++];
        uint16_t cur = d->ar[d->arp];
        ind(d, lo);
        if (cur != 0) d->pc = tgt;
    }
    else if (hi == 0xFC || hi == 0xFD) {                    /* BLKP / BLKD */
        if (it == 0) d->pfc = d->prog[d->pc++];
        uint16_t src = hi == 0xFC ? d->prog[d->pfc] : dr(d, d->pfc);
        dw(d, dma(d, lo), src);
        d->pfc++;
    }
    else return fault(d, "op", op, pc);
    return true;
}

bool c71_step(c71_t *d)
{
    /* interrupts at the instruction boundary, in TMS32025 priority order:
     * INT0 2, INT1 4, INT2 6, TINT 0x18, RINT 0x1A, XINT 0x1C */
    if (!d->intm) {
        static const struct { uint16_t bit, imr, vec; } iv[] = {
            {1, 1, 2}, {2, 2, 4}, {4, 4, 6}, {8, 8, 0x18}, {0x10, 0x10, 0x1A}, {0x20, 0x20, 0x1C} };
        if (d->tint_pend) d->ifr |= 8;
        for (int k = 0; k < 6; k++)
            if ((d->ifr & iv[k].bit) && (d->imr & iv[k].imr)) {
                if (!push(d, d->pc)) return false;
                d->pc = iv[k].vec; d->intm = 1; d->ifr &= ~iv[k].bit; d->idle = 0;
                if (iv[k].bit == 8) d->tint_pend = 0;
                break;
            }
    }
    if (d->idle) {                        /* halted: nothing retires, the timer runs */
        d->tim = d->tim == 0 ? d->prd : (uint16_t)(d->tim - 1);
        if (d->tim == d->prd && (d->imr & 8)) d->tint_pend = 1;
        return true;
    }
    int pc = d->pc;
    d->cur_pc = pc;
    d->steps++;
    /* TIM ticks once per retired instruction, reloading from PRD at 0 */
    d->tim = d->tim == 0 ? d->prd : (uint16_t)(d->tim - 1);
    if (d->tim == d->prd && (d->imr & 8)) d->tint_pend = 1;
    int op = d->prog[pc];
    d->pc = pc + 1;
    int n = d->rpt + 1;
    d->rpt = 0;
    for (int it = 0; it < n; it++)
        if (!exec1(d, pc, op, it)) return false;
    return true;
}

void c71_reset(c71_t *d)
{
    d->bank = 0; d->latch = 0; d->pt_addr = 0; d->pt_data = 0; d->bioz = 1;
    d->pc = 0x4000; d->pfc = 0; d->t = 0; d->acc = 0; d->p = 0;
    memset(d->ar, 0, sizeof d->ar);
    d->arp = d->arb = d->dp = d->pm = 0;
    d->sxm = 1; d->ovm = 0; d->intm = 0; d->c = 0; d->tc = 0; d->cnf = 0;
    d->imr = 9; d->prd = 0xFFFF; d->tim = 0xFFFF; d->tint_pend = 0;
    d->idle = 0; d->ifr = 0;
    d->sp = 0; d->rpt = 0; d->steps = 0; d->error[0] = 0;
    memset(d->written, 0, sizeof d->written); d->n_written = 0;
}

static bool load_words(uint16_t *dst, size_t max_words, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    uint8_t b[2]; size_t i = 0;
    while (i < max_words && fread(b, 1, 2, f) == 2) dst[i++] = (uint16_t)(b[0] << 8 | b[1]);
    fclose(f);
    return true;
}

bool c71_load(c71_t *d, const char *bios_path, const char *prog_path)
{
    bool ok = true;
    if (prog_path) ok = ok && load_words(d->prog + 0x4000, 0xC000, prog_path);
    if (bios_path) ok = ok && load_words(d->prog, 0x4000, bios_path);
    return ok;
}
