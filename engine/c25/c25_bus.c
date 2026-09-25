/*
 * c25_bus.c -- the master DSP's view of the board (engine/c25/c25.h): the
 * data-space map (on-chip RAM, the program-RAM alias at 0x4000, the 16-bit
 * banked window onto polygon RAM at 0x8000), the I/O ports (point ROM/RAM
 * address and data, PDP begin, render device, slave upload) and the PDP's
 * block-copy commands. Board hardware, not program interpretation; moved
 * unchanged from the validated interpreter.
 */
#include <stdio.h>
#include <string.h>
#include "c25.h"

void (*c25_hook_acc)(int kind, int space, uint32_t a, uint32_t v);
void (*c25_hook_pre)(c71_t *d, int pc);
void (*c25_hook_iter)(void);
void (*c25_hook_post)(c71_t *d);

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

static uint16_t dr0(c71_t *d, uint32_t a)
{
    a &= 0xFFFF;
    if (a >= 0x8000) return dsp_r(d, a - 0x8000);
    if (a >= 0x4000) return d->prog[a];
    if (a == 4) return d->imr;
    if (a == 2) return d->tim;
    return d->ram[a];
}

uint16_t c25_dr(c71_t *d, uint32_t a) { uint16_t v = dr0(d, a); if (c25_hook_acc) c25_hook_acc('r', 'D', a & 0xFFFF, v); return v; }

void c25_dw(c71_t *d, uint32_t a, uint32_t v)
{
    a &= 0xFFFF; v &= 0xFFFF;
    if (c25_hook_acc) c25_hook_acc('w', 'D', a, v);
    if (a == 2) d->tim = v;
    if (a == 3) d->prd = v;
    if (a == 4) d->imr = v;
    if (a >= 0x8000) dsp_w(d, a - 0x8000, v);
    else if (a >= 0x4000) d->prog[a] = v;
    else d->ram[a] = v;
}

/* --------------------------------------------------------------- ports ---- */

static uint16_t port_in0(c71_t *d, int pa)
{
    uint32_t v;
    switch (pa) {
    /* MAME point_loword_r / point_hiword_ir: the HIGH read carries the
     * post-increment and a busy bit in bit 15 */
    case 0: v = point_read(d, d->pt_addr); return v & 0xFFFF;
    case 1: v = point_read(d, d->pt_addr); d->pt_addr++; return 0x8000 | ((v >> 16) & 0xFF);
    case 2: if (d->ss22) pdp_run(d); d->bioz = 0; d->pdp_begins++; if (d->pdp_begin) d->pdp_begin(); return 0;   /* PDP begin, BIO ready */
    case 3: if (d->port3_bioz) d->bioz = 1; if (d->port3_r) d->port3_r(); return 0;   /* dsp_unk_port3_r: BIO busy again */
    case 8: return 0x0000;                           /* dsp_unk8_r: not busy */
    case 9: return 0x0063;                           /* custom_ic_status_r */
    default: return 0;
    }
}

uint16_t c25_port_in(c71_t *d, int pa) { uint16_t v = port_in0(d, pa); if (c25_hook_acc) c25_hook_acc('r', 'I', pa, v); return v; }

void c25_port_out(c71_t *d, int pa, uint16_t v)
{
    if (c25_hook_acc) c25_hook_acc('w', 'I', pa, v);
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

uint16_t c25_pr(c71_t *d, uint16_t a) { uint16_t v = d->prog[a]; if (c25_hook_acc) c25_hook_acc('r', 'P', a, v); return v; }
void c25_pw(c71_t *d, uint16_t a, uint16_t v) { if (c25_hook_acc) c25_hook_acc('w', 'P', a, v); d->prog[a] = v; }

