/*
 * s21_dsp.c -- Cyber Sled (Namco System 21): the DSP board -- five C67 (TMS320C25 + Namco BIOS ROM) and the glue between them,
 * the 68000s and the renderer. See s21_dsp.h. Written from the board's behaviour as MAME models it (namcos21_dsp_c67.cpp is the
 * reference; nothing is copied from it).
 *
 * The C25 instruction semantics and step are the shared engine's (engine/c25/c25_core.c + c25_sem.h). The engine's BUS
 * (engine/c25/c25_bus.c) is the System 22 C71's and is NOT linked here: this file supplies the C67's -- c25_dr/c25_dw/c25_pr/
 * c25_pw/c25_port_in/c25_port_out -- for both chips, telling them apart by the c71_t pointer. What the C67 needs that the C71 path
 * does not (all found reading the two boards against each other):
 *   - it boots from its OWN internal ROM at program 0 (microcomputer mode, MP/MC low), not a BIOS at 0 + a program at 0x4000:
 *     reset PC = 0;
 *   - the TMS320C25's on-chip memory map: registers 0-5 (DRR, DXR, TIM, PRD, IMR, GREG), B2 0x60-0x7F, B0 0x200-0x2FF, B1
 *     0x300-0x3FF; CNFP moves B0 to program 0xFF00-0xFFFF (the C71 bus ignores CNF). Unmapped data/program reads are 0;
 *   - the BIO pin is not wired, and MAME's pin default reads as asserted: BIOZ always branches (bioz = 0 forever);
 *   - the chip's reset state (MAME common_reset): IMR = 0xFFC0, C = 1, INTM cleared, TIM = PRD = 0xFFFF;
 *   - IDLE halts until an interrupt (idle_halts = 1); the frame interrupt is INT0 (HOLD_LINE: pending until taken);
 *   - the slave is RESET (memories kept) at the start of every 3D frame, from inside its own port-2 read.
 * Clocks: 40 MHz crystal, one instruction cycle = 4 clocks -> 10 MIPS for the master; MAME clocks the one slave it runs at 4x
 * (40 MIPS) because that one slave does the work the board splits across four. We count retired INSTRUCTIONS as cycles (MAME
 * charges 1-3 cycles per instruction) -- a timing difference the gate measures.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "c25.h"
#include "s21_dsp.h"
#ifdef C25_DEV_HOOKS
#include "c25_oracle.h"
void (*c25_hook_acc)(int kind, int space, uint32_t a, uint32_t v);
void (*c25_hook_pre)(c71_t *d, int pc);
void (*c25_hook_iter)(void);
void (*c25_hook_post)(c71_t *d);
#endif

/* THE C67 PROGRAMS, TRANSLATED (HARD RULE 1): gen/cs_c67_master.c and gen/cs_c67_slave.c, generated at build time by the shared
 * tools/gen/c25_translate.py --game cs from c67.bin + the data ROM's two uploaded blocks and tools/gen/c67_{master,slave}.cov
 * (addresses only). The master and slave run different programs at the same program addresses (0x8000..): one function per chip.
 * An address with no translation, or program memory not holding the words a case was translated from, TRAPS (the chip stops,
 * s21_dsp_error() names the address) -- PLAN.md "Module E" says how to regrow the coverage. */
bool cs_c67_master(c71_t *d, int pc);
bool cs_c67_slave(c71_t *d, int pc);

#define DSP_BUF_MAX  (4096 * 12)      /* the IDC FIFO and the direct-draw buffer */
#define SOUT_MAX     4096             /* slave output record buffer */
#define PTRAM_SIZE   0x20000          /* point RAM: bytes (quad records of 6) */
#define PTROM_WORDS  0x100000

void (*s21_quad_out)(const s21_quad *q);
void (*s21_swap_out)(void);
void (*s21_slave_word_out)(uint16_t w);
void (*s21_ddraw_out)(int port, uint16_t w);

static struct {
    c71_t m, s;                         /* the master and slave 0 */
    uint16_t rom[0x1000];               /* c67.bin, both chips */
    uint32_t *ptrom;                    /* 24-bit unsigned point ROM, 0x100000 words */
    uint16_t dspram[0x8000];            /* shared with the 68000s */
    uint8_t  ptram[PTRAM_SIZE];
    uint16_t depthcue[2][0x400];
    /* master glue */
    uint16_t port_data[16];
    uint16_t src_addr;                  /* IDC transfer cursor (bit 15: mode) */
    uint16_t ddraw[DSP_BUF_MAX]; int ddraw_n;
    int master_finished;
    /* slave glue */
    uint16_t sin[DSP_BUF_MAX]; int sin_avail, sin_adv, sin_start;
    uint16_t sout[SOUT_MAX]; int sout_n;
    int slave_active;
    /* point ROM / RAM ports */
    uint32_t ptram_idx; uint16_t ptram_ctl;
    uint32_t ptrom_idx; uint32_t point_data; int point_avail;
    int need_kick;
    /* reset lines */
    int held;                           /* system reset asserted: both chips held */
    int slave_reset_pending;            /* kickstart from inside the slave's own instruction */
    char error[160];
    s21_dsp_stats st;
    uint64_t steps_acc[2];             /* retired instructions before each chip reset (c71_reset zeroes steps) */
} B;

/* ------------------------------------------------------- DEV: lockstep ---- */
#ifdef C25_DEV_HOOKS
/* s21_dsp_lockstep(): the TRANSLATED chips drive the board; beside each, a SHADOW chip runs the same instructions on the
 * interpreter oracle. The board's side effects (IDC FIFO, point-ROM cursor, kickstart, slave reset, DSP RAM) happen once, from the
 * translated chip: every access it makes outside the chip (data space outside the on-chip map, every port) is LOGGED, and the
 * shadow's accesses REPLAY that log -- the same kind, address and (for a write) value expected, in order; a read gets the logged
 * value. On-chip memory, program memory and registers are each chip's own. After every slice (167 master / 668 slave
 * instructions) both pairs must be EQUAL: registers, stacks, all of data and program memory, and every logged access consumed. */
typedef struct { uint8_t kind, space; uint16_t v; uint32_t a; } ls_ev;
typedef struct { ls_ev *e; size_t n, rd, cap; } ls_q;
static struct {
    int on;
    c71_t *sh[2];                       /* shadow master, shadow slave (interpreter) */
    ls_q q[2];
    char why[256];                      /* the first replay mismatch of the slice */
    long slices, bad;
    long inject_m, inject_s;            /* GATE_LS_INJECT / GATE_LS_INJECT_S: negative controls (slice numbers) */
} LS;
static int ls_shadow(const c71_t *d) { return LS.on && (d == LS.sh[0] || d == LS.sh[1]); }
static int ls_chip(const c71_t *d) { return d == LS.sh[1] ? 1 : 0; }      /* valid for the shadows */
static void ls_log(int w, int kind, int space, uint32_t a, uint16_t v)
{
    ls_q *q = &LS.q[w];
    if (q->n == q->cap) { q->cap = q->cap ? q->cap * 2 : 4096; q->e = realloc(q->e, q->cap * sizeof *q->e); }
    q->e[q->n++] = (ls_ev){ (uint8_t)kind, (uint8_t)space, v, a };
}
static uint16_t ls_replay(const c71_t *d, int kind, int space, uint32_t a, uint16_t v)
{
    ls_q *q = &LS.q[ls_chip(d)];
    if (q->rd >= q->n) {
        if (!LS.why[0]) snprintf(LS.why, sizeof LS.why, "%s shadow at %04X: %c%c %04X beyond the translation's accesses",
                                 ls_chip(d) ? "slave" : "master", d->cur_pc, kind, space, a);
        return 0;
    }
    const ls_ev *e = &q->e[q->rd++];
    if (e->kind != kind || e->space != space || e->a != a || (kind == 'w' && e->v != v))
        if (!LS.why[0]) snprintf(LS.why, sizeof LS.why, "%s shadow at %04X: %c%c %04X=%04X, translation did %c%c %04X=%04X",
                                 ls_chip(d) ? "slave" : "master", d->cur_pc, kind, space, a, v, e->kind, e->space, e->a, e->v);
    return e->v;
}
#define LS_REAL(d) (LS.on && !ls_shadow(d))
#else
#define ls_shadow(d) 0
#endif

/* -------------------------------------------- polling-loop probe state ---- */
/* While a fast-forward probe runs one iteration of a candidate loop (ff_try below), the bus flags anything that would make a
 * repeat of that iteration differ from the first: a port access, a program write, a data write off the chip or one that changes
 * an on-chip word. tim_touch: the iteration reads or writes TIM itself (then TIM must be part of the fixed point). */
static struct { int on, bad, tim_touch; } P;

/* ------------------------------------------------------------------ chips ---- */

static void chip_reset_state(c71_t *d)
{
    c71_reset(d);
    d->pc = 0; d->imr = 0xFFC0; d->c = 1; d->intm = 0; d->sxm = 1;
    d->tim = 0xFFFF; d->prd = 0xFFFF; d->cnf = 0;
    d->bioz = 0;                        /* BIO not wired: BIOZ always taken */
    d->idle_halts = 1; d->port3_bioz = 0; d->ss22 = 0;
    d->spin_pc = 0;
}

static void chip_reset(c71_t *d)
{
    B.steps_acc[d == &B.s] += d->steps;
    chip_reset_state(d);
#ifdef C25_DEV_HOOKS
    if (LS.on) chip_reset_state(LS.sh[d == &B.s]);
#endif
}

static void kickstart(void);
static int dbg = -1;
static int dbg_on(void) { if (dbg < 0) { const char *e = getenv("S21DSP_LOG"); dbg = e ? atoi(e) : 0; } return dbg; }

/* ------------------------------------------------------------ point ROM ---- */

static uint32_t ptrom_r(uint32_t a) { return B.ptrom[a & (PTROM_WORDS - 1)]; }

/* ------------------------------------------------------------------- IDC ---- */

int s21_dsp_inject;                   /* dev negative control: XOR 1 into the next IDC word (s21_dsp_gate GATE_INJECT) */
static void idc_put(uint16_t w)
{
    if (s21_dsp_inject) { w ^= 1; s21_dsp_inject = 0; }
    if (dbg_on() && B.st.idc_words < 40) fprintf(stderr, "[IDC] put %04X (avail %d) src %04X\n", w, B.sin_avail, B.src_addr);
    int o = (B.sin_start + B.sin_avail++) % DSP_BUF_MAX;
    B.sin[o] = w;
    B.slave_active = 1;
    B.st.idc_words++;
    if (B.sin_avail >= DSP_BUF_MAX && !B.error[0]) snprintf(B.error, sizeof B.error, "IDC overflow");
}

/* The master hands the slave its work: walk DSP RAM from the transfer cursor. Mode 0 (bit 15 clear): blocks of `n, n words`,
 * 0xFFFF ends. Mode 1: the first block is a header (`n` words sent as n+1, n); after that each block names a point-ROM OBJECT --
 * its list of sub-objects is expanded here, each one sent as the block's own context words followed by its primitive words --
 * and 0xFFFF n is a goto (to itself = wait for more: the next DSP-RAM write just past the cursor continues the walk). */
static void idc_transfer(int first)
{
    uint16_t addr = B.src_addr;
    const int mode = addr >> 15;
    addr &= 0x7FFF;
    if (!addr) return;
    for (int guard = 0; guard < 0x40000; guard++) {
        const uint16_t old = addr, code = B.dspram[addr];
        addr = (addr + 1) & 0x7FFF;
        if (!mode) {
            if (code == 0xFFFF) { B.src_addr = 0; return; }
            idc_put(code);
            for (int i = 0; i < code; i++) { idc_put(B.dspram[addr]); addr = (addr + 1) & 0x7FFF; }
        } else if (code == 0xFFFF) {
            addr = B.dspram[addr];
            B.src_addr = addr;
            addr &= 0x7FFF;
            if (old == addr) return;
            continue;
        } else if (first) {
            idc_put(code + 1);
            for (int i = 0; i < code; i++) { idc_put(B.dspram[addr]); addr = (addr + 1) & 0x7FFF; }
        } else {
            uint32_t obj = ptrom_r(code);
            const uint16_t len = B.dspram[addr];
            addr = (addr + 1) & 0x7FFF;
            for (int g2 = 0; g2 < 0x10000; g2++) {
                uint32_t sub = ptrom_r(obj++);
                if (sub == 0xFFFFFF) break;
                const uint16_t prim = (uint16_t)ptrom_r(sub++);
                if (prim > 2) {
                    idc_put(0);
                    idc_put(len + 1);
                    for (int i = 0; i < len; i++) idc_put(B.dspram[(addr + i) & 0x7FFF]);
                    idc_put(0);
                    idc_put(prim + 1);
                    for (int i = 0; i < prim; i++) idc_put((uint16_t)ptrom_r(sub + i));
                }
            }
            addr += len;                /* 16-bit, masked on the next read (as the board's 15-bit counter) */
            addr &= 0xFFFF;
        }
        first = 0;
    }
    if (!B.error[0]) snprintf(B.error, sizeof B.error, "IDC transfer runaway at %04X", B.src_addr);
}

static void dspram_w(uint32_t off, uint16_t v)
{
    off &= 0x7FFF;
    B.dspram[off] = v;
    if (B.src_addr && off == (uint32_t)(1 + (B.src_addr & 0x7FFF))) idc_transfer(0);
}

static uint16_t slave_in_r(void)
{
    if (B.sin_avail <= 0) { if (dbg_on() > 1) fprintf(stderr, "[IDC] slave read EMPTY at %04X\n", B.s.cur_pc); return 0; }
    uint16_t v = B.sin[B.sin_start];
    if (dbg_on() > 1) fprintf(stderr, "[IDC] slave read %04X at %04X\n", v, B.s.cur_pc);
    B.sin_start = (B.sin_start + 1) % DSP_BUF_MAX;
    B.sin_avail--;
    if (B.sin_adv > 0) B.sin_adv--;
    return v;
}

static uint16_t slave_adv_r(void)
{
    if (B.sin_adv < B.sin_avail) B.sin_adv++;
    else if (B.slave_active && B.master_finished && B.src_addr) kickstart();
    return (uint16_t)B.sin_adv;
}

/* ---------------------------------------------------------------- render ---- */

static void quad_emit(const int *sx, const int *sy, const int *z, uint16_t color)
{
    s21_quad q;
    for (int i = 0; i < 4; i++) { q.sx[i] = sx[i]; q.sy[i] = sy[i]; q.z[i] = z[i]; }
    q.color = color;
    B.st.quads++;
    if (s21_quad_out) s21_quad_out(&q);
}

static void direct_quad(const uint16_t *src, uint16_t color)
{
    int sx[4], sy[4], z[4];
    for (int i = 0; i < 4; i++) { sx[i] = (int16_t)src[i * 3]; sy[i] = (int16_t)src[i * 3 + 1]; z[i] = (int16_t)src[i * 3 + 2]; }
    quad_emit(sx, sy, z, color);
}

/* A record from the slave: count, colour, then either 12 words of a direct quad (colour bit 15) or a vertex table (x, y, z per
 * vertex) whose quads are listed in POINT RAM from entry `colour`: 6 bytes each -- code (bit 7 = last), colour low, 4 vertex
 * indices. Returns the words the record used. */
static int ptram_quads(const uint16_t *src, uint32_t idx)
{
    const uint32_t mask = PTRAM_SIZE - 1;
    int maxv = -1;
    idx = (idx * 6) & mask;
    for (int n = 0; n < PTRAM_SIZE / 6; n++) {
        uint8_t code = B.ptram[idx]; idx = (idx + 1) & mask;
        uint16_t color = B.ptram[idx] | (code << 8); idx = (idx + 1) & mask;
        int sx[4], sy[4], z[4];
        for (int i = 0; i < 4; i++) {
            uint8_t v = B.ptram[idx]; idx = (idx + 1) & mask;
            sx[i] = (int16_t)src[v * 3]; sy[i] = (int16_t)src[v * 3 + 1]; z[i] = (int16_t)src[v * 3 + 2];
            if (v > maxv) maxv = v;
        }
        quad_emit(sx, sy, z, color & 0x7FFF);
        if (code & 0x80) break;
    }
    return (maxv + 1) * 3;
}

static void slave_out_w(uint16_t w)
{
    if (s21_slave_word_out) s21_slave_word_out(w);
    if (B.sout_n >= SOUT_MAX) {
        if (!B.error[0]) snprintf(B.error, sizeof B.error, "slave output overflow (%04X)", B.sout[0]);
        B.sout_n = 0;
    }
    B.sout[B.sout_n++] = w;
    const uint16_t count = B.sout[0];
    if (count && B.sout_n > count) {
        uint16_t color = B.sout[1];
        if (color & 0x8000) direct_quad(&B.sout[2], color);
        else ptram_quads(&B.sout[2], color);
        B.st.records++;
        for (int i = 0; i < B.sout_n; i++) B.sout[i] = 0xFFFF;
        B.sout_n = 0;
    } else if (count == 0) {
        if (!B.error[0]) snprintf(B.error, sizeof B.error, "slave output record of length 0");
        B.sout[0] = 0xFFFF; B.sout_n = 0;
    }
}

static void kickstart(void)
{
    if (s21_swap_out) s21_swap_out();
    B.src_addr = 0; B.sout_n = 0; B.master_finished = 0; B.slave_active = 0;
    B.st.kicks++;
    c71_irq(&B.m, 1);                   /* INT0, held until taken */
#ifdef C25_DEV_HOOKS
    if (LS.on) c71_irq(LS.sh[0], 1);
#endif
    B.slave_reset_pending = 1;          /* applied after the slave's current instruction (it may be the caller) */
}

/* ------------------------------------------------------- the C67 bus ---- */

static int is_master(const c71_t *d)
{
#ifdef C25_DEV_HOOKS
    if (LS.on && d == LS.sh[0]) return 1;
#endif
    return d == &B.m;
}

static uint16_t cuskey_r(c71_t *d)
{
    /* the Cyber Sled master program's key check: the value the custom key chip returns to each of its three reads. The reads
     * are the one-word LACs at 0x8060 / 0x8068 / 0x806F; MAME keys its table on the CPU's PC AFTER the fetch (0x8061, ...). */
    switch ((uint16_t)(d->cur_pc + 1)) {
    case 0x8061: return 0xFE95;
    case 0x8069: return 0xFFFF;
    case 0x8070: return 0x016A;
    default: return 0;
    }
}

/* WIDESCREEN (Hor+, PLAN.md "Module H"). Every 3D frame the master program writes the slave's scene header to DSP RAM 0x400
 * (count 0x18, then its on-chip 0x20C..0x223: master 0x85B6, `RPTK 0x17 ; BLKD 0x20C,*+` at 0x85BF). Header word 0x15 (DSP RAM
 * 0x415, slave 0x22F) is the half-size of the window the slave rejects whole vertex groups against (slave 0x85A2: a group whose
 * every vertex lies beyond +-margin in x, or in y, is not drawn): the 68000's view block word 0xE (DSP RAM 0x400E / 0x600E, 242),
 * copied by master 0x85A6. That word ALSO sets the focal lengths (master 0x8401: focal = 242 * cot(FOV/2), the FOV in view word 2),
 * so the 68000's block cannot be widened without changing the projection; the header word is the one place where only the
 * window is. Everything else that bounds x is far outside any screen: the slave's per-vertex clip is +-1023 px (header 0x10..0x13,
 * literals at master 0x85AE), and the master's object cull (0x8CC9 / 0x8D6A) is a frustum built from the same +-1023 and the
 * focal. So s21_dsp_set_view_extra(px) adds px to the margin as the master writes it -- the view is as wide as the picture,
 * the projection (focal, centre) is untouched. 0 (the default) = the board exactly as is. The shadow chips of the lockstep log and
 * check the program's own value, so the gates compare the programs, not the setting. */
static int view_extra;
void s21_dsp_set_view_extra(int px) { view_extra = px > 0 ? (px < 0x3000 ? px : 0x3000) : 0; }

uint16_t c25_dr(c71_t *d, uint32_t a)
{
    a &= 0xFFFF;
    uint16_t v = 0;
    if (a < 6) {
        if (a == 2) { v = d->tim; if (P.on) P.tim_touch = 1; } else if (a == 3) v = d->prd; else if (a == 4) v = d->imr; else v = d->ram[a];
    } else if (a >= 0x60 && a < 0x80) v = d->ram[a];
    else if (a >= 0x200 && a < 0x300) v = d->cnf ? 0 : d->ram[a];
    else if (a >= 0x300 && a < 0x400) v = d->ram[a];
#ifdef C25_DEV_HOOKS
    else if (ls_shadow(d)) v = ls_replay(d, 'r', 'D', a, 0);
#endif
    else {                              /* off the chip: the board */
        if (is_master(d)) {
            if (a >= 0x8000) v = B.dspram[a - 0x8000];
            else if (a >= 0x2000 && a < 0x2010) v = cuskey_r(d);
        }
#ifdef C25_DEV_HOOKS
        if (LS_REAL(d)) ls_log(d == &B.s, 'r', 'D', a, v);
#endif
    }
#ifdef C25_DEV_HOOKS
    if (c25_hook_acc) c25_hook_acc('r', 'D', a, v);
#endif
    return v;
}

void c25_dw(c71_t *d, uint32_t a, uint32_t v)
{
    a &= 0xFFFF; v &= 0xFFFF;
#ifdef C25_DEV_HOOKS
    if (c25_hook_acc) c25_hook_acc('w', 'D', a, v);
#endif
    if (P.on) {                         /* fast-forward probe: an iteration must not change memory (registers are compared) */
        if (a == 2) P.tim_touch = 1;
        else if (a == 3 || a == 4) { /* PRD / IMR: fields, compared */ }
        else if (a >= 0x200 && a < 0x300) { if (!d->cnf && d->ram[a] != v) P.bad = 1; }   /* B0: dropped while CNF maps it to program */
        else if (a < 0x400 && !(a >= 6 && a < 0x60) && !(a >= 0x80 && a < 0x200)) { if (d->ram[a] != v) P.bad = 1; }
        else P.bad = 1;
    }
    if (a < 6) {
        if (a == 2) d->tim = v; else if (a == 3) d->prd = v; else if (a == 4) d->imr = v; else d->ram[a] = v;
    } else if (a >= 0x60 && a < 0x80) d->ram[a] = v;
    else if (a >= 0x200 && a < 0x300) {
        if (!d->cnf) { d->ram[a] = v; d->prog[0xFF00 + (a - 0x200)] = v; }   /* B0, mirrored where CNFP puts it */
    } else if (a >= 0x300 && a < 0x400) d->ram[a] = v;
#ifdef C25_DEV_HOOKS
    else if (ls_shadow(d)) ls_replay(d, 'w', 'D', a, (uint16_t)v);
#endif
    else {
#ifdef C25_DEV_HOOKS
        if (LS_REAL(d)) ls_log(d == &B.s, 'w', 'D', a, (uint16_t)v);
#endif
        if (is_master(d) && a >= 0x8000) {
            if (view_extra && a == 0x8415 && d->cur_pc == 0x85BF) v = (v + (uint32_t)view_extra) & 0xFFFF;   /* widescreen, see below */
            dspram_w(a - 0x8000, v);
        }
        /* 0x2000-0x200F: custom key writes, no effect modelled; everything else unmapped */
    }
}

uint16_t c25_pr(c71_t *d, uint16_t a)
{
    uint16_t v = 0;
    if (a < 0x1000) v = d->prog[a];
    else if (a >= 0xFF00) v = d->cnf ? d->prog[a] : 0;
    else if (a >= 0x8000 && a < (is_master(d) ? 0xC000 : 0x9000)) v = d->prog[a];
#ifdef C25_DEV_HOOKS
    if (c25_hook_acc) c25_hook_acc('r', 'P', a, v);
#endif
    return v;
}

void c25_pw(c71_t *d, uint16_t a, uint16_t v)
{
    if (P.on) P.bad = 1;
#ifdef C25_DEV_HOOKS
    if (c25_hook_acc) c25_hook_acc('w', 'P', a, v);
#endif
    if (a >= 0xFF00) { if (d->cnf) { d->prog[a] = v; d->ram[0x200 + (a - 0xFF00)] = v; } }
    else if (a >= 0x8000 && a < (is_master(d) ? 0xC000 : 0x9000)) {
        if (dbg_on() > 2 && !is_master(d) && a < 0x8004) fprintf(stderr, "[PW] slave prog %04X = %04X at pc %04X\n", a, v, d->cur_pc);
        d->prog[a] = v;
    }
}

static uint16_t master_in(int pa)
{
    switch (pa) {
    case 0:                             /* point ROM low word; latches the high byte for port 1 */
        B.point_data = ptrom_r(B.ptrom_idx++);
        B.point_avail = 1;
        return B.point_data & 0xFFFF;
    case 1:
        if (B.point_avail) { B.point_avail = 0; return (B.point_data >> 16) & 0xFF; }
        return 0x8000;                  /* IDC ack */
    case 8: return 1;                   /* SMU status */
    case 0xB: return 1;                 /* config */
    default: return 0;                  /* 2 IDC transmit, 3 IDC receive, 9 render busy, A config, F: 0 = master */
    }
}

static void master_out(int pa, uint16_t v)
{
    switch (pa) {
    case 2:
        if (s21_ddraw_out) s21_ddraw_out(2, v);
        B.src_addr = v; idc_transfer(1);
        break;
    case 3: B.ptrom_idx = (B.ptrom_idx << 16) | v; break;
    case 8:
        if (~B.port_data[8] & v & 1) B.master_finished = 1;
        B.port_data[8] = v;
        break;
    case 0xB:
        if (s21_ddraw_out) s21_ddraw_out(0xB, v);
        if (~B.port_data[0xB] & v & 1) {
            if (B.ddraw_n == 13 && (B.ddraw[0] & 0x8000)) { direct_quad(&B.ddraw[1], B.ddraw[0]); B.st.ddraw++; }
            B.ddraw_n = 0;
        }
        B.port_data[0xB] = v;
        break;
    case 0xC:
        if (s21_ddraw_out) s21_ddraw_out(0xC, v);
        if (B.ddraw_n < DSP_BUF_MAX) B.ddraw[B.ddraw_n++] = v;
        break;
    default: break;                     /* 0/1 point RAM (unused), 4 IDC setup, A status lamp */
    }
}

static uint16_t slave_in(int pa)
{
    switch (pa) {
    case 0: return slave_in_r();
    case 2: return slave_adv_r();
    case 0xF: return 1;                 /* 1 = slave */
    default: return 0;                  /* 3: render queue free */
    }
}

/* A port read that, in the board's present state, returns the same value and changes nothing, however often it is repeated --
 * allowed inside a fast-forward probe. Nothing but the chip itself can change that state during its run block (see ff_try). */
static int port_pure(const c71_t *d, int pa)
{
    if (is_master(d)) return pa != 0 && !(pa == 1 && B.point_avail);          /* 0: point-ROM read advances; 1: consumes the latch */
    if (pa == 0) return B.sin_avail <= 0;                                       /* FIFO empty: 0, nothing moves */
    if (pa == 2) return B.sin_adv >= B.sin_avail && !(B.slave_active && B.master_finished && B.src_addr);   /* no advance, no kick */
    return 1;
}

uint16_t c25_port_in(c71_t *d, int pa)
{
    uint16_t v;
    if (P.on && !port_pure(d, pa & 0xF)) P.bad = 1;
#ifdef C25_DEV_HOOKS
    if (ls_shadow(d)) v = ls_replay(d, 'r', 'I', (uint32_t)pa, 0);
    else
#endif
    v = is_master(d) ? master_in(pa & 0xF) : slave_in(pa & 0xF);
#ifdef C25_DEV_HOOKS
    if (LS_REAL(d)) ls_log(d == &B.s, 'r', 'I', (uint32_t)pa, v);
#endif
#ifdef C25_DEV_HOOKS
    if (c25_hook_acc) c25_hook_acc('r', 'I', pa, v);
#endif
    return v;
}

void c25_port_out(c71_t *d, int pa, uint16_t v)
{
    if (P.on) P.bad = 1;
#ifdef C25_DEV_HOOKS
    if (c25_hook_acc) c25_hook_acc('w', 'I', pa, v);
    if (ls_shadow(d)) { ls_replay(d, 'w', 'I', (uint32_t)pa, v); return; }
    if (LS_REAL(d)) ls_log(d == &B.s, 'w', 'I', (uint32_t)pa, v);
#endif
    if (is_master(d)) master_out(pa & 0xF, v);
    else if ((pa & 0xF) == 0) slave_out_w(v);
}

/* ------------------------------------------------------- 68000 side ---- */

uint16_t s21_dsp_ram_r(uint32_t off) { return B.dspram[off & 0x7FFF]; }
void s21_dsp_ram_w(uint32_t off, uint16_t data, uint16_t mask)
{
    off &= 0x7FFF;
    dspram_w(off, (uint16_t)((B.dspram[off] & ~mask) | (data & mask)));
}
void s21_dsp_ptram_ctl_w(uint16_t data, uint16_t mask)
{
    B.ptram_ctl = (uint16_t)((B.ptram_ctl & ~mask) | (data & mask));
    B.ptram_idx = 0;
}
uint16_t s21_dsp_ptram_r(void) { return B.ptram[B.ptram_idx]; }
void s21_dsp_ptram_w(uint16_t data, uint16_t mask)
{
    if (mask & 0xFF) { B.ptram[B.ptram_idx] = (uint8_t)data; B.ptram_idx = (B.ptram_idx + 1) & (PTRAM_SIZE - 1); }
}
uint16_t s21_dsp_depthcue_r(uint32_t off) { return B.depthcue[(B.ptram_ctl & 0x20) ? 1 : 0][off & 0x3FF]; }
void s21_dsp_depthcue_w(uint32_t off, uint16_t data, uint16_t mask)
{
    if (mask & 0xFF) B.depthcue[(B.ptram_ctl & 0x20) ? 1 : 0][off & 0x3FF] = data;
}

static void board_reset(void)
{
    if (s21_swap_out) { s21_swap_out(); s21_swap_out(); }
    B.src_addr = 0; B.sin_avail = 0; B.sin_adv = 0; B.sin_start = 0; B.sout_n = 0; B.ddraw_n = 0;
    B.master_finished = 0; B.slave_active = 0;
    B.ptram_idx = 0; B.ptram_ctl = 0; B.ptrom_idx = 0; B.point_data = 0; B.point_avail = 0;
    B.need_kick = 1; B.slave_reset_pending = 0;
}

void s21_dsp_reset(int state)
{
    if (state) { board_reset(); B.held = 1; return; }
    if (B.held) { chip_reset(&B.m); chip_reset(&B.s); B.held = 0; }
}

void s21_dsp_kick(void)
{
    if (!B.need_kick) return;
    B.need_kick = 0;
    kickstart();
    if (B.slave_reset_pending) { chip_reset(&B.s); B.slave_reset_pending = 0; }
}

/* ---------------------------------------------------------------- run ---- */

const char *s21_dsp_error(void) { return B.error[0] ? B.error : NULL; }

static bool chip_fault(c71_t *d, const char *who)
{
    snprintf(B.error, sizeof B.error, "%s DSP at %04X: %s", who, d->cur_pc, d->error);
    return false;
}

#ifdef C25_DEV_HOOKS
static const char *ls_compare(const c71_t *a, const c71_t *b)
{
#define F(x) if (a->x != b->x) return #x
    F(pc); F(pfc); F(t); F(acc); F(p); F(arp); F(arb); F(dp); F(pm); F(sxm); F(ovm); F(intm); F(c); F(tc); F(cnf); F(ov);
    F(imr); F(prd); F(tim); F(tint_pend); F(sp); F(rpt); F(idle); F(ifr); F(steps); F(cur_pc);
#undef F
    if (memcmp(a->ar, b->ar, sizeof a->ar)) return "ar";
    if (memcmp(a->stack, b->stack, sizeof a->stack)) return "stack";
    if (memcmp(a->mstk, b->mstk, sizeof a->mstk)) return "mstk";
    if (memcmp(a->ram, b->ram, sizeof a->ram)) return "data memory";
    if (memcmp(a->prog, b->prog, sizeof a->prog)) return "program memory";
    return NULL;
}
static void ls_resync(int w)
{
    c71_t *r = w ? &B.s : &B.m, *sh = LS.sh[w];
    bool (*x)(c71_t *, int) = sh->xlat;
    memcpy(sh, r, sizeof *sh); sh->xlat = x;
}
static void ls_slice_end(void)
{
    LS.slices++;
    if (LS.inject_m == LS.slices) B.m.ram[0x300] ^= 1;
    if (LS.inject_s == LS.slices) B.s.ram[0x300] ^= 1;
    for (int w = 0; w < 2; w++) {
        const char *diff = ls_compare(w ? &B.s : &B.m, LS.sh[w]);
        char buf[96];
        if (!diff && LS.q[w].rd != LS.q[w].n) { snprintf(buf, sizeof buf, "%zu board accesses not replayed", LS.q[w].n - LS.q[w].rd); diff = buf; }
        if (!diff && LS.why[0] && strstr(LS.why, w ? "slave" : "master") == LS.why) diff = LS.why;
        if (diff) {
            if (++LS.bad <= 10) fprintf(stderr, "[C67-LOCKSTEP] slice %ld %s: translation != oracle (%s); pc %04X vs %04X\n",
                                        LS.slices, w ? "slave" : "master", diff, (w ? &B.s : &B.m)->pc, LS.sh[w]->pc);
            ls_resync(w);
        }
        LS.q[w].n = LS.q[w].rd = 0;
    }
    LS.why[0] = 0;
}
#endif

/* ------------------------------------------------- polling-loop fast-forward ---- */
/* Both C67 programs spend most of their instructions in polling loops that only something OUTSIDE the chip can end (measured
 * over the play capture: the master 78% in 0x81E3-0x81E5 `LAC * ; SUB 6B ; BZ`, comparing a DSP RAM word only the 68000 writes;
 * the slave 67% in 0x8028-0x802A `SAR AR2,TIM ; LAC TIM ; BZ`, spinning until the kickstart resets it). Within one run block
 * (the master's c71_run of a slice, the slave's slice) nothing else runs: the 68000s, the kickstart's INT0 and resets all arrive
 * between blocks or from the slave's own port reads. So when one iteration of a short backward loop leaves every register as it
 * found it, changes no memory and touches no port, every further iteration in the block is the same -- they pass in closed form:
 * the step count, and TIM as tim_skip() advances it (or, if the loop writes TIM itself, TIM is part of the fixed point). This is
 * the engine's idle/spin fast-forward (engine/c25/c25_core.c c71_run) generalised by probing instead of pattern-matching. Exact:
 * the dev lockstep gate runs it on the translated chips against interpreter shadows that step every instruction (and replays the
 * skipped iterations' DSP RAM reads to them). S21DSP_NOFF=1 turns it off; never with the interpreter (coverage needs every step). */
static uint8_t ff_fail[2][0x10000];      /* consecutive failed probes per loop head: stop probing a head at 16 */
static int ff_mode = -1;
static uint64_t ff_skipped[2];
static int ff_on(const c71_t *d)
{
    if (ff_mode < 0) ff_mode = getenv("S21DSP_NOFF") ? 0 : 1;
#ifdef C25_DEV_HOOKS
    if (d->xlat == c25_interp_exec) return 0;
#endif
    return ff_mode;
}

/* TIM over `n` steps of a loop of `gran` instructions that does not touch TIM: as the engine's c71_step ticks it (once per retired
 * instruction, reload from PRD at 0, an event = a step leaving TIM == PRD, TINT_PEND when IMR bit 3). If the timer can interrupt,
 * stop before the event (normal stepping takes it). Returns the steps consumed, a multiple of gran. Same rule as engine tim_ff()
 * (static there: work/proposed_patches/e_c25_tim_skip.patch would export it). */
static long tim_skip(c71_t *d, long n, long gran)
{
    const int wake = !d->intm && (d->imr & 8);
    if (!d->intm && d->tint_pend) return 0;
    uint32_t tim = d->tim, prd = d->prd;
    long k = tim > prd ? (long)(tim - prd) : (long)tim + 1;
    if (wake) {
        long m = n < k - 1 ? n : k - 1;
        m -= m % gran;
        if (m < 0) m = 0;
        d->tim = (uint16_t)(tim - (uint32_t)m);
        return m;
    }
    n -= n % gran;
    if (n < k) { d->tim = (uint16_t)(tim - (uint32_t)n); return n; }
    long r = n - k;
    if (d->imr & 8) d->tint_pend = 1;
    r %= (long)prd + 1;
    d->tim = (uint16_t)(prd - (uint32_t)r);
    return n;
}

typedef struct {
    uint16_t pc, pfc, t, imr, prd, tim, ifr, ar[8], stack[64], mstk[8];
    int32_t acc; int64_t p;
    int arp, arb, dp, pm, sxm, ovm, intm, c, tc, cnf, ov, sp, rpt, idle, tint_pend;
} ff_regs;
static void ff_snap(const c71_t *d, ff_regs *r)
{
    memset(r, 0, sizeof *r);
    r->pc = d->pc; r->pfc = d->pfc; r->t = d->t; r->imr = d->imr; r->prd = d->prd; r->tim = d->tim; r->ifr = d->ifr;
    memcpy(r->ar, d->ar, sizeof r->ar); memcpy(r->stack, d->stack, sizeof r->stack); memcpy(r->mstk, d->mstk, sizeof r->mstk);
    r->acc = d->acc; r->p = d->p; r->arp = d->arp; r->arb = d->arb; r->dp = d->dp; r->pm = d->pm; r->sxm = d->sxm; r->ovm = d->ovm;
    r->intm = d->intm; r->c = d->c; r->tc = d->tc; r->cnf = d->cnf; r->ov = d->ov; r->sp = d->sp; r->rpt = d->rpt; r->idle = d->idle;
    r->tint_pend = d->tint_pend;
}

/* d just took a backward branch to d->pc (a candidate loop head). Probe one iteration (real steps, counted against *left), and if
 * it is a fixed point, skip the whole iterations that fit in *left. Returns false on a chip fault. Stops early (true) after a step
 * that left the slave a reset pending -- the caller applies it. */
static bool ff_try(c71_t *d, int w, long *left)
{
    const uint16_t h = d->pc;
    if (ff_fail[w][h] >= 16 || d->idle || d->rpt || *left < 2) return true;
    ff_regs r0, r1;
    ff_snap(d, &r0);
#ifdef C25_DEV_HOOKS
    const size_t q0 = LS.on ? LS.q[w].n : 0;
#endif
    P.on = 1; P.bad = 0; P.tim_touch = 0;
    long L = 0;
    while (L < 8 && *left > 0) {
        if (!c71_step(d)) { P.on = 0; return false; }
        L++; (*left)--;
        if (B.slave_reset_pending || d->pc == h || P.bad) break;
    }
    P.on = 0;
    ff_snap(d, &r1);
    if (!P.tim_touch) { r1.tim = r0.tim; r1.tint_pend = r0.tint_pend; }
    if (B.slave_reset_pending || P.bad || d->pc != h || memcmp(&r0, &r1, sizeof r0)) {
        if (ff_fail[w][h] < 255) ff_fail[w][h]++;
        return true;
    }
    ff_fail[w][h] = 0;
    long m = P.tim_touch ? *left - *left % L : tim_skip(d, *left, L);
    if (m <= 0) return true;
    const long k = m / L;
    d->steps += (uint64_t)m;
    *left -= m;
    ff_skipped[w] += (uint64_t)m;
#ifdef C25_DEV_HOOKS
    {   /* S21DSP_FF_INJECT=<n>: negative control for the lockstep gate -- the n-th free-running-timer skip advances TIM one too far */
        static long inj = -2, cnt; if (inj == -2) { const char *e = getenv("S21DSP_FF_INJECT"); inj = e ? atol(e) : -1; }
        if (!P.tim_touch && ++cnt == inj) d->tim--;
    }
#endif
#ifdef C25_DEV_HOOKS
    if (LS.on) {                         /* the shadow steps every skipped instruction: give it the iteration's board reads k times */
        const size_t q1 = LS.q[w].n, per = q1 - q0;
        for (long i = 0; i < k; i++)
            for (size_t j = 0; j < per; j++) { const ls_ev e = LS.q[w].e[q0 + j]; ls_log(w, e.kind, e.space, e.a, e.v); }
    }
#else
    (void)k;
#endif
    return true;
}

/* the master's block: c71_run, with the loop fast-forward */
static bool master_run(long n)
{
    c71_t *d = &B.m;
    if (!ff_on(d)) return c71_run(d, n);
    while (n > 0) {
        if (d->idle) return c71_run(d, n);       /* the engine's halted fast-forward */
        if (!c71_step(d)) return false;
        n--;
        if (d->pc < d->cur_pc && d->cur_pc - d->pc <= 8 && n > 1 && !ff_try(d, 0, &n)) return false;
    }
    return true;
}

uint64_t s21_dsp_ff_skipped(int which) { return ff_skipped[which & 1]; }

bool s21_dsp_run(long cycles)
{
    if (B.held) return true;
    /* MAME's scheduling quantum on this board is 1/60000 s: ~167 master cycles; run the two chips in slices of that size */
    while (cycles > 0 && !B.error[0]) {
        long n = cycles < 167 ? cycles : 167;
        if (!master_run(n)) return chip_fault(&B.m, "master");
#ifdef C25_DEV_HOOKS
        if (LS.on && !c71_run(LS.sh[0], n)) return chip_fault(LS.sh[0], "master (oracle shadow)");
#endif
        const int sff = ff_on(&B.s);
        for (long k = n * 4; k > 0;) {
            const long k0 = k;
            if (!c71_step(&B.s)) return chip_fault(&B.s, "slave");
            k--;
            if (sff && !B.slave_reset_pending && B.s.pc < B.s.cur_pc && B.s.cur_pc - B.s.pc <= 8 && k > 1 && !ff_try(&B.s, 1, &k))
                return chip_fault(&B.s, "slave");
#ifdef C25_DEV_HOOKS
            if (LS.on) for (long i = 0; i < k0 - k; i++) if (!c71_step(LS.sh[1])) return chip_fault(LS.sh[1], "slave (oracle shadow)");
#else
            (void)k0;
#endif
            if (B.slave_reset_pending) { chip_reset(&B.s); B.slave_reset_pending = 0; }
        }
#ifdef C25_DEV_HOOKS
        if (LS.on) ls_slice_end();
#endif
        cycles -= n;
    }
    return !B.error[0];
}

/* ---------------------------------------------------------------- init ---- */

static bool load_file(const char *dir, const char *name, uint8_t *dst, size_t n)
{
    char p[1024];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    FILE *f = fopen(p, "rb");
    if (!f) { fprintf(stderr, "[S21DSP] missing %s\n", p); return false; }
    size_t got = fread(dst, 1, n, f);
    fclose(f);
    if (got != n) { fprintf(stderr, "[S21DSP] short read %s (%zu of %zu)\n", p, got, n); return false; }
    return true;
}

bool s21_dsp_init(const char *romdir)
{
    memset(&B, 0, sizeof B);
    uint8_t rom[0x2000];
    if (!load_file(romdir, "c67.bin", rom, sizeof rom)) return false;
    for (int i = 0; i < 0x1000; i++) B.rom[i] = (uint16_t)(rom[i * 2] << 8 | rom[i * 2 + 1]);   /* BE16 words */
    B.ptrom = calloc(PTROM_WORDS, sizeof *B.ptrom);
    static const char *chips[2][3] = { { "cy1-poi-h1.2f", "cy1-poi-lu1.2k", "cy1-poi-ll1.2n" },
                                       { "cy1-poi-h2.2j", "cy1-poi-lu2.2l", "cy1-poi-ll2.2p" } };
    uint8_t *buf = malloc(0x80000);
    if (!B.ptrom || !buf) return false;
    for (int bank = 0; bank < 2; bank++)
        for (int c = 0; c < 3; c++) {
            if (!load_file(romdir, chips[bank][c], buf, 0x80000)) { free(buf); return false; }
            for (uint32_t i = 0; i < 0x80000; i++) B.ptrom[bank * 0x80000 + i] |= (uint32_t)buf[i] << (16 - 8 * c);
        }
    free(buf);
    for (int k = 0; k < 2; k++) {
        c71_t *d = k ? &B.s : &B.m;
        memcpy(d->prog, B.rom, sizeof B.rom);
        chip_reset(d);
        d->xlat = k ? cs_c67_slave : cs_c67_master;   /* the translated programs (s21_dsp_use_oracle: the interpreter, dev) */
    }
    board_reset();
    B.held = 1;                         /* the board comes up with the DSPs held (machine reset asserts the system reset) */
    return true;
}

void s21_dsp_use_oracle(void)
{
#ifdef C25_DEV_HOOKS
    c25_oracle_use(&B.m);
    c25_oracle_use(&B.s);
#endif
}

bool s21_dsp_lockstep(void)
{
#ifdef C25_DEV_HOOKS
    if (LS.on) return true;
    B.m.xlat = cs_c67_master; B.s.xlat = cs_c67_slave;
    for (int w = 0; w < 2; w++) {
        LS.sh[w] = malloc(sizeof *LS.sh[w]);
        if (!LS.sh[w]) return false;
        memcpy(LS.sh[w], w ? &B.s : &B.m, sizeof *LS.sh[w]);
        LS.sh[w]->xlat = c25_interp_exec;           /* the oracle, without its trace hooks (they would count both chips twice) */
    }
    const char *e = getenv("GATE_LS_INJECT"); LS.inject_m = e ? atol(e) : -1;
    e = getenv("GATE_LS_INJECT_S"); LS.inject_s = e ? atol(e) : -1;
    LS.on = 1;
    return true;
#else
    return false;
#endif
}

void s21_dsp_lockstep_report(long *slices, long *bad)
{
#ifdef C25_DEV_HOOKS
    *slices = LS.slices; *bad = LS.bad;
#else
    *slices = 0; *bad = 0;
#endif
}

void s21_dsp_get_stats(s21_dsp_stats *st)
{
    *st = B.st;
    st->m_steps = B.steps_acc[0] + B.m.steps; st->s_steps = B.steps_acc[1] + B.s.steps;
    st->m_pc = B.m.pc; st->s_pc = B.s.pc;
    st->m_held = st->s_held = B.held;
    st->m_idle = B.m.idle; st->s_idle = B.s.idle;
}

const uint16_t *s21_dsp_prog(int which) { return which ? B.s.prog : B.m.prog; }
const uint16_t *s21_dsp_dspram(void) { return B.dspram; }

int s21_dsp_which(const void *chip) { return chip == &B.m ? 0 : chip == &B.s ? 1 : -1; }
