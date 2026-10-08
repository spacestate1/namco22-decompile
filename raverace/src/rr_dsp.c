/*
 * rr_dsp.c -- the System 22 master DSP, the way MAME's namcos22.cpp wires it.
 *
 *  syscon 0x1A (syscon_dspcontrol): 0 holds the master and slave in reset,
 *    1 runs both with the DSP interrupts on, 0xFF runs the master alone
 *    (the 68K's code-upload mode). Leaving reset restarts the C71 at its
 *    BIOS reset vector; the program the 68K uploaded survives in extram.
 *  INT0 at every vblank and RINT/XINT from a 100 Hz serial pulse, both only
 *    while the DSP interrupts are on (HOLD_LINE: pending until taken).
 *  Point RAM at 0xF00000 (Super System 22 moved it to 0xF80000).
 *  The slave DSP is not run -- MAME does not run it either; its job
 *  (walking the master's display list) is done by the renderer.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rr_mem.h"
#include "c25.h"               /* the shared engine's master DSP (../engine/c25) */
#ifdef RR_ORACLE
#include "c25_oracle.h"
#endif
#include "rr_dsp.h"
#include "rr_game.h"
#include "rr_video.h"
#include "rr_scene.h"

int32_t  *g_pointrom;
uint32_t  g_pointrom_words;

static c71_t *m;
static uint8_t ctrl;          /* last syscon 0x1A value */
static bool running, irq_on, faulted, slave_on;
static uint16_t rbuf[0x1C];
static int rbuf_n, upload_state;            /* MAME m_RenderBufSize, m_dsp_upload_state */

static void render_w(uint16_t v)
{
    if (rbuf_n < 0x1C) { rbuf[rbuf_n++] = v; if (rbuf_n == 0x1C) rr_scene_direct_poly(rbuf); }
}
static void render_reset(void) { rbuf_n = 0; rr_scene_render_refresh(); }
static void port3_r(void) { upload_state = 0; }
extern bool g_rr_in_vblank;
static void pdp_begin(void) { rr_scene_pdp_begin(g_rr_in_vblank); }
/* the slave DSP's external RAM (program and data, 0x8000-0x9FFF): what the master uploads through port 7. Kept (MAME's
 * m_slave_extram) though nothing runs it yet; RR_SLAVEDUMP=<file> writes it, big-endian words, at every slave enable. */
static uint16_t slave_extram[0x2000];
static uint16_t upload_dest;
static void slave_dump(void)
{
    static const char *path; static int init;
    if (!init) { init = 1; path = getenv("RR_SLAVEDUMP"); }
    if (!path) return;
    FILE *f = fopen(path, "wb"); if (!f) return;
    for (int i = 0; i < 0x2000; i++) { fputc(slave_extram[i] >> 8, f); fputc(slave_extram[i] & 0xFF, f); }
    fclose(f);
    fprintf(stderr, "[DSP] slave extram written to %s\n", path);
}
static void slave_w(uint16_t v)          /* upload_code_to_slave_dsp_w */
{
    if (upload_state == 0) {
        if (v == 0) slave_on = false;
        else if (v == 1) upload_state = 1;
        else if (v == 3 || v == 0x10) { slave_on = true; slave_dump(); }
    } else if (upload_state == 1) { upload_dest = v; upload_state = 2; }    /* destination, then data until port 3 read */
    else slave_extram[upload_dest++ & 0x1FFF] = v;
}
static uint32_t seen_begins;

static bool load_pointrom(const char *dir)
{
    const char *const (*pl)[4] = g_rr_game->pot;
    const int chips = g_rr_game->pot_chips;
    const uint32_t chip = 0x80000, n = (uint32_t)chips * chip;
    uint8_t *b = malloc(3 * n);
    g_pointrom = malloc(n * sizeof *g_pointrom);
    for (int p = 0; p < 3; p++)
        for (int c = 0; c < chips; c++) {
            char path[1024];
            snprintf(path, sizeof path, "%s/%s", dir, pl[p][c]);
            FILE *f = fopen(path, "rb");
            if (!f || fread(b + p * n + c * chip, 1, chip, f) != chip) {
                fprintf(stderr, "[DSP] cannot read %s\n", path);
                if (f) fclose(f);
                free(b); return false;
            }
            fclose(f);
        }
    for (uint32_t i = 0; i < n; i++) {
        uint32_t v = (uint32_t)b[2 * n + i] << 16 | (uint32_t)b[n + i] << 8 | b[i];
        g_pointrom[i] = (int32_t)(v << 8) >> 8;
    }
    g_pointrom_words = n;
    free(b);
    return true;
}

bool rr_dsp_init(const char *dir)
{
    if (!load_pointrom(dir)) return false;
    m = calloc(1, sizeof *m);
    if (!m) return false;
    c71_load_builtin_bios(m);                          /* the DSP's BIOS is built in (engine/c25/c71_bios.c): no c71.bin to find */
    m->poly = g_rr.poly;
    m->ptrom = (const uint32_t *)(const void *)g_pointrom;
    m->ptrom_words = g_pointrom_words;
    m->ptram_base = C71_PTRAM_S22;
    m->ss22 = 0;
    m->idle_halts = 1;                    /* TI IDLE: INTM 0, halt until an interrupt */
    m->port3_bioz = 1;
    /* THE PROGRAM: translated to C at build time (gen/rr_c25.c). The oracle
     * builds can run the interpreter instead (RR_C25=oracle) -- the gate. */
    { extern bool rr_c25_exec(c71_t *, int); m->xlat = rr_c25_exec; }
    m->spin_pc = g_rr_game->spin_pc; m->spin_op = g_rr_game->spin_op;    /* the master's LAC *0 / BNEZ poll (~44.9M of ~54M retired steps in attract) */
#ifdef RR_ORACLE
    { const char *e = getenv("RR_C25"); if (e && !strcmp(e, "oracle")) { c25_oracle_use(m); fprintf(stderr, "[DSP] master program: the interpreter ORACLE\n"); } }
#endif
    m->render_w = render_w; m->render_reset = render_reset; m->pdp_begin = pdp_begin; m->slave_w = slave_w; m->port3_r = port3_r;
    return true;
}

static void start(void)
{
    c71_reset(m);
    m->ptram_base = C71_PTRAM_S22;
    m->pc = 0x0000;                      /* the BIOS reset vector */
    m->render_w = render_w; m->render_reset = render_reset; m->pdp_begin = pdp_begin; m->slave_w = slave_w; m->port3_r = port3_r;
    faulted = false;
}

void rr_dsp_control(uint8_t v)
{
    if (!m || v == ctrl) return;         /* MAME: no-op when unchanged */
    ctrl = v;
    if (v == 0) { running = false; irq_on = false; slave_on = false; }
    else if (v == 1) { if (!running) start(); running = true; irq_on = true; slave_on = true; }
    else if (v == 0xFF) { if (!running) start(); running = true; irq_on = false; }
}

void rr_dsp_run(long steps)
{
    if (!m || !running || faulted) return;
    if (!c71_run(m, steps)) {            /* same result as `steps` calls of c71_step; a halted DSP is fast-forwarded */
        fprintf(stderr, "[DSP] master stopped at %04X: %s\n", m->cur_pc, m->error);
        faulted = true; return;
    }
}

void rr_dsp_vblank(void) { if (m && running && irq_on) c71_irq(m, 1); }
void rr_dsp_serial(void) { if (m && running && irq_on) c71_irq(m, 0x10 | 0x20); }
bool rr_dsp_render_done(void) { bool r = m && m->pdp_begins != seen_begins; if (m) seen_begins = m->pdp_begins; return r; }
uint16_t rr_dsp_pdp_base(void) { return m ? m->pdp_base : 0; }

bool rr_dsp_slave_active(void) { return slave_on; }
int32_t rr_dsp_pointram_read(uint32_t a)
{
    if (m && a >= C71_PTRAM_S22 && a < C71_PTRAM_S22 + C71_PTRAM_WORDS)
        return (int32_t)(m->ptram[a - C71_PTRAM_S22] << 8) >> 8;
    return -1;
}

void rr_dsp_debug(char *buf, int n)
{
    if (!m) { snprintf(buf, n, "dsp: none"); return; }
    snprintf(buf, n, "dsp: ctrl %02X run %d irq %d slave %d fault %d pc %04X idle %d imr %X ifr %X intm %d pdp %u steps %llu",
             ctrl, running, irq_on, slave_on, faulted, m->pc, m->idle, m->imr, m->ifr, m->intm, m->pdp_begins,
             (unsigned long long)m->steps);
}
