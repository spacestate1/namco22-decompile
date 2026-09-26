/*
 * tw_dsp.c -- Tokyo Wars' master DSP, the way MAME's namcos22.cpp wires it (Super System 22).
 *
 *  syscon 0x1C (syscon_dspcontrol): 0 holds the master and slave in reset, 1 runs both with the
 *    DSP interrupts on, 0xFF runs the master alone (the 68K's code-upload mode). Leaving reset
 *    restarts the C71 at its BIOS reset vector; the program the BIOS was given survives in extram.
 *  The upload is the DSP BIOS's own protocol through polygon RAM (FUN_0012ED28 on the 68K side:
 *    count at 0xC00C00, words from 0xC00C04, 0xC00030 = 0 when the BIOS has taken a block), so the
 *    BIOS runs from its reset vector and the game program arrives through it -- nothing here loads
 *    the game's DSP program.
 *  INT0 at every vblank and RINT/XINT from a 100 Hz serial pulse, both only while the DSP
 *    interrupts are on (HOLD_LINE: pending until taken).
 *  Point RAM at 0xF80000. The slave DSP's program passes through the master's port 7 (the master
 *    relays what the 68K uploads as blocks 2 and 3); it is kept in slave_ram for the slave's own
 *    translation (T4b) and the slave is not run: its job, walking the master's display list, is
 *    the engine's (engine/slave_list.c).
 *
 * The master PROGRAM is gen/tw_c25.c, translated from the ROM files at build time by the shared
 * tools/gen/c25_translate.py; the interpreter exists only in the dev builds (TW_ORACLE):
 *   TW_C25=oracle    the interpreter runs the program (also what records coverage: C71_COV=file)
 *   TW_C25=lockstep  the translation runs, a shadow master on the interpreter runs beside it on
 *                    the same inputs, and after every slice their state must be EQUAL
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tw_mem.h"
#include "tw_dsp.h"
#include "tw_video.h"
#include "c25.h"
#ifdef TW_ORACLE
#include "c25_oracle.h"
#endif

int32_t  *g_tw_pointrom;
uint32_t  g_tw_pointrom_words;

#define NM 2
static c71_t *m[NM];              /* m[0] the game's master; m[1] the lockstep shadow (oracle builds) */
static int    nm;
#ifdef TW_ORACLE
static uint32_t shadow_poly[TW_POLY_WORDS];
static long     slices, bad_slices;
static bool     lockstep;
#else
#define lockstep 0
#endif
static uint8_t  ctrl;             /* last syscon 0x1C value */
static bool     running, irq_on, faulted;
static uint16_t slave_ram[0x2000];
static int      upload_state, upload_idx;
static long     slave_words;                 /* words the master has relayed to the slave's RAM */
static bool     slave_on;

static void port3_r(void) { upload_state = 0; }
static void slave_w(uint16_t v)          /* upload_code_to_slave_dsp_w */
{
    switch (upload_state) {
    case 0:
        if (v == 0) slave_on = false;
        else if (v == 1) upload_state = 1;
        else if (v == 3 || v == 0x10) slave_on = true;
        break;
    case 1: upload_idx = v; upload_state = 2; break;
    case 2: slave_ram[upload_idx & 0x1FFF] = v; upload_idx++; slave_words++; break;
    }
}

static bool load_pointrom(const char *dir)
{
    static const char *pl[3][4] = {
        {"tw1ptrl0.18k", "tw1ptrl1.16k", "tw1ptrl2.15k", "tw1ptrl3.14k"},
        {"tw1ptrm0.18j", "tw1ptrm1.16j", "tw1ptrm2.15j", "tw1ptrm3.14j"},
        {"tw1ptru0.18f", "tw1ptru1.16f", "tw1ptru2.15f", "tw1ptru3.14f"} };
    const uint32_t chip = 0x80000, n = 4 * chip;         /* the region is three planes, chips in order */
    uint8_t *b = malloc(3 * (size_t)n);
    g_tw_pointrom = malloc(n * sizeof *g_tw_pointrom);
    if (!b || !g_tw_pointrom) return false;
    for (int p = 0; p < 3; p++)
        for (int c = 0; c < 4; c++) {
            char path[1024];
            snprintf(path, sizeof path, "%s/%s", dir, pl[p][c]);
            FILE *f = fopen(path, "rb");
            if (!f || fread(b + (size_t)p * n + (size_t)c * chip, 1, chip, f) != chip) {
                fprintf(stderr, "[DSP] cannot read %s\n", path);
                if (f) fclose(f);
                free(b); return false;
            }
            fclose(f);
        }
    for (uint32_t i = 0; i < n; i++) {
        uint32_t v = (uint32_t)b[2 * (size_t)n + i] << 16 | (uint32_t)b[n + i] << 8 | b[i];
        g_tw_pointrom[i] = (int32_t)(v << 8) >> 8;         /* signed24 */
    }
    g_tw_pointrom_words = n;
    free(b);
    return true;
}

static void setup(c71_t *d, uint32_t *poly)
{
    d->poly = poly;
    d->ptrom = (const uint32_t *)(const void *)g_tw_pointrom;
    d->ptrom_words = g_tw_pointrom_words;
    d->ptram_base = C71_PTRAM_SS22;
    d->ss22 = 1;                          /* a port-2 read runs the PDP command block (Super 22) */
    d->idle_halts = 1;                    /* TI IDLE: INTM 0, halt until an interrupt */
    d->port3_bioz = 1;
    { extern bool tw_c25_exec(c71_t *, int); d->xlat = tw_c25_exec; }
    d->slave_w = slave_w; d->port3_r = port3_r;    /* render_w: not used on Super 22 */
    d->pdp_begin = tw_video_pdp_begin;    /* MAME's render_frame_active: which screen updates draw the list (engine/frame_rule.h) */
    d->render_reset = tw_video_render_refresh;
}

bool tw_dsp_init(const char *dir)
{
    if (!load_pointrom(dir)) return false;
    char bios[1024];
    snprintf(bios, sizeof bios, "%s/c71.bin", dir);
    m[0] = calloc(1, sizeof *m[0]);
    if (!m[0] || !c71_load(m[0], bios, NULL)) { fprintf(stderr, "[DSP] no BIOS at %s\n", bios); return false; }
    setup(m[0], g_tw.poly);
    nm = 1;
#ifdef TW_ORACLE
    const char *e = getenv("TW_C25");
    if (e && !strcmp(e, "oracle")) { c25_oracle_use(m[0]); fprintf(stderr, "[DSP] master program: the interpreter ORACLE\n"); }
    else if (e && !strcmp(e, "lockstep")) {
        m[1] = malloc(sizeof *m[1]);
        *m[1] = *m[0];
        m[1]->poly = shadow_poly;
        c25_oracle_use(m[1]);                      /* the hooks are global: coverage/trace see both */
        nm = 2; lockstep = true;
        fprintf(stderr, "[DSP] lockstep: the translation against the interpreter oracle\n");
    }
#endif
    return true;
}

static void start(void)
{
    for (int i = 0; i < nm; i++) {
        c71_reset(m[i]);
        m[i]->ptram_base = C71_PTRAM_SS22;
        m[i]->pc = 0x0000;                 /* the BIOS reset vector */
    }
    faulted = false;
}

void tw_dsp_control(uint8_t v)
{
    if (!m[0] || v == ctrl) return;        /* MAME: no-op when unchanged */
    ctrl = v;
    if (v == 0) { running = false; irq_on = false; slave_on = false; }
    else if (v == 1) { if (!running) start(); running = true; irq_on = true; slave_on = true; }
    else if (v == 0xFF) { if (!running) start(); running = true; irq_on = false; }
}

#ifdef TW_ORACLE
static const char *compare(const c71_t *a, const c71_t *b)
{
#define F(x) if (a->x != b->x) return #x
    F(pc); F(pfc); F(t); F(acc); F(p); F(arp); F(arb); F(dp); F(pm); F(sxm); F(ovm); F(intm);
    F(c); F(tc); F(cnf); F(imr); F(prd); F(tim); F(tint_pend); F(sp); F(rpt); F(bank); F(latch);
    F(pt_addr); F(pt_data); F(bioz); F(idle); F(ifr); F(steps); F(n_written); F(pdp_base); F(pdp_begins);
#undef F
    if (memcmp(a->ar, b->ar, sizeof a->ar)) return "ar";
    if (memcmp(a->stack, b->stack, sizeof a->stack)) return "stack";
    if (memcmp(a->ram, b->ram, sizeof a->ram)) return "data RAM";
    if (memcmp(a->prog, b->prog, sizeof a->prog)) return "program RAM";
    if (memcmp(a->ptram, b->ptram, sizeof a->ptram)) return "point RAM";
    if (memcmp(a->poly, b->poly, sizeof shadow_poly)) return "polygon RAM";
    return NULL;
}
#endif

void tw_dsp_run(long steps)
{
    if (!m[0] || !running || faulted) return;
#ifdef TW_ORACLE
    if (lockstep) memcpy(shadow_poly, m[0]->poly, sizeof shadow_poly);   /* the 68K's writes since last slice */
#endif
    if (nm == 1) {                        /* the shipped game: same result as `steps` calls of c71_step, a halted DSP fast-forwarded */
        if (!c71_run(m[0], steps)) {
            fprintf(stderr, "[DSP] master stopped at %04X: %s\n", m[0]->cur_pc, m[0]->error);
            faulted = true; return;
        }
        return;
    }
    for (long s = 0; s < steps; s++)
        for (int i = 0; i < nm; i++)
            if (!c71_step(m[i])) {
                fprintf(stderr, "[DSP] master%s stopped at %04X: %s\n", i ? " (oracle shadow)" : "", m[i]->cur_pc, m[i]->error);
                faulted = true; return;
            }
#ifdef TW_ORACLE
    if (lockstep) {
        slices++;
        const char *diff = compare(m[0], m[1]);
        static long inj = -2;                  /* C25_LS_INJECT=<slice>: negative control */
        if (inj == -2) { const char *e = getenv("C25_LS_INJECT"); inj = e ? atol(e) : -1; }
        if (inj == slices) m[0]->ram[0x300] ^= 1, diff = compare(m[0], m[1]);
        if (diff) {
            if (++bad_slices <= 5)
                fprintf(stderr, "[C25-LOCKSTEP] slice %ld: translation != oracle (%s); pc %04X vs %04X\n", slices, diff, m[0]->pc, m[1]->pc);
            uint32_t *keep = m[1]->poly; bool (*x)(c71_t *, int) = m[1]->xlat;     /* resync: report a fault once */
            *m[1] = *m[0]; m[1]->poly = keep; m[1]->xlat = x;
            memcpy(shadow_poly, m[0]->poly, sizeof shadow_poly);
        }
    }
#endif
}

void tw_dsp_vblank(void) { if (running && irq_on) for (int i = 0; i < nm; i++) c71_irq(m[i], 1); }
void tw_dsp_serial(void) { if (running && irq_on) for (int i = 0; i < nm; i++) c71_irq(m[i], 0x10 | 0x20); }
bool tw_dsp_active(void) { return m[0] && running; }
bool tw_dsp_slave_active(void) { return slave_on; }
bool tw_dsp_faulted(void) { return faulted; }
uint32_t tw_dsp_pdp_begins(void) { return m[0] ? m[0]->pdp_begins : 0; }

void tw_dsp_debug(char *buf, int n)
{
    if (!m[0]) { snprintf(buf, n, "dsp: none"); return; }
    snprintf(buf, n, "dsp: ctrl %02X run %d irq %d slave %d (%ld words uploaded) fault %d pc %04X idle %d imr %X ifr %X intm %d pdp %u steps %llu%s",
             ctrl, running, irq_on, slave_on, slave_words, faulted, m[0]->pc, m[0]->idle, m[0]->imr, m[0]->ifr, m[0]->intm,
             m[0]->pdp_begins, (unsigned long long)m[0]->steps, lockstep ? " lockstep" : "");
}

/* at exit in a lockstep run: the verdict the gate script reads */
#ifdef TW_ORACLE
__attribute__((destructor)) static void lockstep_report(void)
{
    if (lockstep) fprintf(stderr, "[C25-LOCKSTEP] END: %ld slices, %ld differing -> %s\n", slices, bad_slices, bad_slices ? "FAIL" : "PASS");
}
#endif
