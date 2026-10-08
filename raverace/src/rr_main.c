/*
 * rr_main.c -- host for the lifted Rave Racer program.
 *
 * The 68020 program is gen/rr_lifted.c: entry_reset (L_4000) never returns,
 * it runs the game's own main loop. Time advances in rr_tick(), which the
 * lifted code polls on every call and backward branch (RR_POLL). Every
 * RR_POLLS_PER_FRAME polls is one video frame: the vblank devices run, the
 * vblank IRQ is raised, and any pending IRQ above the SR mask is delivered by
 * calling its autovector handler (vector table at VBR = 0).
 *
 * Ghidra's rte p-code is a bare return that restores neither SR nor SP, so an
 * interrupt pushes NO frame: the host saves SR, raises the mask to the
 * interrupt's level, calls the handler, and restores SR afterwards.
 *
 *   rr <rom_dir> [--frames N] [--dump DIR]    headless run
 */
#include <SDL.h>
#include "rr_romzip.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rr_mem.h"
#include "rr_hw.h"
#include "rr_dsp.h"
#include "rr_video.h"
#include "rr_gl.h"
#include <GL/gl.h>
#include "rr_scene.h"
#include "rr_lift_rt.h"
#include "lift_cpu.h"
#include "rr_game.h"
#include "rr_ui.h"
#include "rr_lifted.h"
#include "rr_input.h"
#include "rr_sound.h"
#include "rr_link.h"
#include "rr_net.h"

bool rr_host_open(int scale);
bool rr_host_frame(void);
bool rr_host_paused(void);
void rr_host_close(void);
int rr_host_joytest(void);
static int windowed;
/* THE RENDERER. 1 = the shared engine's OpenGL pipeline (../engine, src/rr_gl.c):
 * the game's renderer, always, in the window and headless. 0 = src/rr_video.c,
 * the software MAME port -- a test ORACLE, compiled only into the dev binaries
 * (RR_ORACLE: rr_oracle, rr_trace, rr_sndoracle), where it is the headless
 * default and --gl selects the engine. */
int g_rr_gl = 1;
static int gl_w = 640, gl_h = 480;             /* headless picture size (RR_RENDER_SIZE) */
static bool gl_ok;                             /* a GL context and the video ROMs: there is a picture */

/* --perf: per-frame emulation time (68K + DSP + render, host sleep excluded) and
 * render time, reported as percentiles at exit. Quote these, never one run's fps. */
#include <time.h>
#include "tex_bake.h"
#include "win_gpu.h"          /* Windows: run on the discrete GPU of a two-GPU laptop (Optimus / PowerXpress) */
static FILE *perflog;                         /* RR_PERFLOG=<file>: one line per frame -- see the perf block */
static double perflog_texels;
static int perf_on;
static double *perf_frame, *perf_video;
static uint32_t perf_n, perf_cap;
static double t_frame_start;
static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static int cmpd(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }
static void perf_report(void)
{
    if (!perf_on || !perf_n) return;
    const char *nm[2] = { "frame", "video" };
    double *arr[2] = { perf_frame, perf_video };
    for (int k = 0; k < 2; k++) {
        double *v = malloc(perf_n * sizeof *v), sum = 0; int over = 0;
        memcpy(v, arr[k], perf_n * sizeof *v);
        for (uint32_t i = 0; i < perf_n; i++) { sum += v[i]; if (v[i] > 16.667) over++; }
        qsort(v, perf_n, sizeof *v, cmpd);
        fprintf(stderr, "[PERF] %-5s n=%u mean %.2f p50 %.2f p90 %.2f p99 %.2f max %.2f ms  over16.67: %d\n", nm[k], perf_n,
                sum / perf_n, v[perf_n / 2], v[perf_n * 9 / 10], v[perf_n * 99 / 100], v[perf_n - 1], over);
        free(v);
    }
}
static const char *rec_path, *rep_path;
static int freeplay = -1;
static int32_t test_coin = -1, test_gas = -1;   /* --coin F[,F2...]: pulse coin 1 at each frame (Ace Driver takes two a credit); --gas F: hold gas from F */
static int32_t test_coins[8]; static int n_test_coins;
static struct { int32_t at; int32_t value; } test_steer[8]; static int n_steer;
static struct { int32_t at, idx, value; } test_adc[32]; static int n_adc;   /* --adc F:I:V (repeatable): twin-stick ADC.I at V (0x47..0xB7, centre 0x7F) from frame F */   /* --steer F:V (repeatable): the wheel at V (0..0xFFF, centre 0x800) from frame F */

#define REG_SP   RR_REG_SP
#define REG_SR   RR_REG_SR

static int32_t  polls_per_frame = 83000;   /* 68K instructions per frame (MAME: 4.99M in 60) */
static uint32_t max_frames = 600;
#define frame rr_frame              /* the shared runtime names the frame in its traps */
static const char *dump_dir;
static const char *shot_dir;          /* --shots DIR EVERY: a PPM every EVERY frames */
static uint32_t shot_every;

void rd_budget_out(void);   /* src/rd: unwind a checker probe (budget ran out, or it trapped) */

/* The 68K host runtime -- register file, shadow return stack, traps, interrupt entry, the
 * instruction-trace hook -- is engine/lift_cpu.c, shared with every lifted game. */
#define get_sr rr_get_sr
#define set_sr rr_set_sr

static void dump_state(void)
{
    if (!dump_dir) return;
    char p[1024];
    snprintf(p, sizeof p, "%s/wram_f%u.bin", dump_dir, frame);
    FILE *f = fopen(p, "wb");
    if (f) { fwrite(g_rr.wram, 1, RR_WRAM_SIZE, f); fclose(f); }
    snprintf(p, sizeof p, "%s/text_f%u.bin", dump_dir, frame);      /* the text tilemap: what marks a screen type (the HUD) */
    f = fopen(p, "wb");
    if (f) { fwrite(g_rr.text, 1, RR_TEXT_SIZE, f); fclose(f); }
    snprintf(p, sizeof p, "%s/pal_f%u.bin", dump_dir, frame);       /* palette RAM 0x90028000 (planar R/G/B) */
    f = fopen(p, "wb");
    if (f) { fwrite(g_rr.pal, 1, RR_PAL_SIZE, f); fclose(f); }
    snprintf(p, sizeof p, "%s/mixer_f%u.bin", dump_dir, frame);     /* mixer 0x90020000 */
    f = fopen(p, "wb");
    if (f) { fwrite(g_rr.mixer, 1, RR_MIXER_SIZE, f); fclose(f); }
    snprintf(p, sizeof p, "%s/shared_f%u.bin", dump_dir, frame);    /* 68K byte order */
    f = fopen(p, "wb");
    if (f) { fwrite(g_rr.shared, 1, RR_SHARED_SIZE, f); fclose(f); }
    snprintf(p, sizeof p, "%s/poly_f%u.bin", dump_dir, frame);
    f = fopen(p, "wb");
    if (f) {
        for (uint32_t i = 0; i < RR_POLY_WORDS; i++) {
            uint32_t w = g_rr.poly[i] | 0xFF000000u;           /* what the 68K reads */
            uint8_t b[4] = { w >> 24, w >> 16, w >> 8, w };
            fwrite(b, 1, 4, f);
        }
        fclose(f);
    }
}

/* Time advances in SLICES: each slice the 68K spends polls_per_frame/SLICES
 * polls and the master DSP its share of a frame (40 MHz C71 = 10 MIPS =
 * ~166,667 instructions per 60 Hz frame); every SLICES slices is a frame. */
#define SLICES 16
#define DSP_STEPS_PER_FRAME 166667L
static uint32_t slice, serial_acc;
/* VTOTAL 525, VBSTART 480 (namcos22.cpp): vblank is the first 45/525 of a frame
 * after the screen update, ~1.37 of our 16 slices. The master answers the
 * vblank INT0 inside that window, and MAME's pdp_begin then sets pdp_frame one
 * AHEAD so the next screen update keeps the list (render_frame_active). */
bool g_rr_in_vblank;
static int vblank_slices;

/* --sndsweep HOLD[:PASS[:SLOT_LO:SLOT_HI[:CMD_MAX[:FROM]]]]: the sound driver's coverage harness (engine/ss22_run.c's, for this board). Rave Racer's
 * mailbox is the same as the Super 22 games' but at shared 0x1000: command words 0x1000 + slot*2, parameter words 0x1100..0x117E. PASS 0 writes
 * every command 0x4000|c and 0xC000|c into every slot in turn; PASS 1-4 fill the parameters with 0x0000/0x4000/0x8000/0xFFFF first; PASS >= 5
 * writes random commands and parameters (xorshift seeded by PASS). Run on rr_sndoracle with SND_COV=<file> (tools/grind_snd.sh). */
static struct { int on, hold, pass, slot_lo, slot_hi, cmd_max, from; } sweep;
static void sweep_arg(const char *a)
{
    sweep.on = 1; sweep.hold = 6; sweep.pass = 0; sweep.slot_lo = 0; sweep.slot_hi = 31; sweep.cmd_max = 0x7F; sweep.from = 1000;
    sscanf(a, "%d:%d:%d:%d:%i:%d", &sweep.hold, &sweep.pass, &sweep.slot_lo, &sweep.slot_hi, &sweep.cmd_max, &sweep.from);
    if (sweep.hold < 1) sweep.hold = 1;
}
#define SWEEP_BASE 0x1000
static void sweep_put16(uint32_t off, uint16_t v) { g_rr.shared[SWEEP_BASE + off] = (uint8_t)(v >> 8); g_rr.shared[SWEEP_BASE + off + 1] = (uint8_t)v; }
static uint32_t sweep_rnd(uint64_t *s) { *s ^= *s << 13; *s ^= *s >> 7; *s ^= *s << 17; return (uint32_t)(*s >> 16); }
static void sweep_tick(void)
{
    static const uint16_t par[5] = { 0, 0x0000, 0x4000, 0x8000, 0xFFFF };
    static long prev_slot = -1;
    if (!sweep.on || frame < (uint32_t)sweep.from || (frame - (uint32_t)sweep.from) % (uint32_t)sweep.hold) return;
    if (sweep.pass >= 5) {
        static uint64_t rs;
        if (!rs) rs = 0x9E3779B97F4A7C15ull * (uint64_t)sweep.pass;
        for (int i = 0; i < 4; i++) {
            uint32_t sl = (uint32_t)sweep.slot_lo + sweep_rnd(&rs) % (uint32_t)(sweep.slot_hi - sweep.slot_lo + 1);
            uint32_t on = sweep_rnd(&rs), fl = sweep_rnd(&rs), c = sweep_rnd(&rs) % (uint32_t)(sweep.cmd_max + 1);
            sweep_put16(sl * 2, (on & 3) ? (uint16_t)(((fl & 1) ? 0xC000u : 0x4000u) | c) : 0);
        }
        for (uint32_t o = 0x100; o < 0x180; o += 2) { uint32_t v = sweep_rnd(&rs); if (v & 1) sweep_put16(o, (uint16_t)(v >> 1)); }
        return;
    }
    const long per_slot = 2L * (sweep.cmd_max + 1), nslots = sweep.slot_hi - sweep.slot_lo + 1;
    const long n = (long)((frame - (uint32_t)sweep.from) / (uint32_t)sweep.hold);
    if (prev_slot >= 0) sweep_put16((uint32_t)prev_slot * 2, 0);
    if (n >= nslots * per_slot) { if (n == nslots * per_slot) fprintf(stderr, "[SNDSWEEP] done at frame %u\n", frame); prev_slot = -1; return; }
    const long slot = sweep.slot_lo + n / per_slot, k = n % per_slot;
    if (sweep.pass > 0) for (uint32_t o = 0x100; o < 0x180; o += 2) sweep_put16(o, par[sweep.pass]);
    sweep_put16((uint32_t)slot * 2, (uint16_t)(((k & 1) ? 0xC000u : 0x4000u) | (unsigned)(k >> 1)));
    prev_slot = slot;
}

/* ---- the Debug page: developer screens nothing in the program reaches (rr_game.h rr_debug_t) ----
 * At a frame boundary, once the vblank interrupt is delivered and while the program is in its MAIN context (the coroutine task
 * stack empty -- inside the mode task the slot would be overwritten by its own next yield), the screen's entry is written into the
 * mode task's slot, exactly as the program's own setter does, and the Test switch goes ON. The main loop then resumes the screen;
 * the screen leaves through the program's own exit when the Test switch goes OFF. RR_DEBUG=<i>@<frame>: headless. */
static int debug_pending = -1;
static uint32_t debug_test_at;               /* the frame the Test switch went on for this launch (0: not yet) */
static uint32_t debug_slot_was;              /* the mode task's slot when the Test switch went on */
void rr_debug_launch(int i)
{
    if (!g_rr_game->debug || i < 0 || i >= g_rr_game->ndebug) return;
    debug_pending = i; debug_test_at = 0;
    fprintf(stderr, "[DEBUG] launching \"%s\" (0x%X)\n", g_rr_game->debug[i].name, g_rr_game->debug[i].entry);
}
/* Two steps, because the program reacts to the Test switch itself: switching it on makes the main loop install the operator
 * test mode into the very slot the screen goes in. So: the Test switch on first; the moment the slot changes (the test mode
 * installed, not yet run -- it would leave its menu on the text layer) the screen replaces it. After 120 frames without a
 * change the screen goes in anyway. */
static void debug_boundary(void)
{
    { static int at = -2, idx; if (at == -2) { const char *e = getenv("RR_DEBUG"); at = -1; if (e && sscanf(e, "%d@%d", &idx, &at) != 2) at = -1; }
      if (at >= 0 && frame == (uint32_t)at) rr_debug_launch(idx); }
    if (debug_pending < 0 || rr_in_irq) return;
    if (!debug_test_at) {
        g_hw.inputs &= (uint16_t)~0x0400;               /* the Test switch ON (headless; a window's host keeps it on) */
        if (windowed) rr_host_set_test(true);
        debug_test_at = frame;
        debug_slot_was = rr_read(g_rr_game->task_slot, 4);
        return;
    }
    if (rr_read(g_rr_game->task_sp, 4) != g_rr_game->task_sp_empty) return;     /* not the main context: try next frame */
    if (rr_read(g_rr_game->task_slot, 4) == debug_slot_was && frame < debug_test_at + 120) return;
    const rr_debug_t *d = &g_rr_game->debug[debug_pending];
    debug_pending = -1;
    rr_write(g_rr_game->task_slot, 4, d->entry);
    fprintf(stderr, "[DEBUG] \"%s\" installed in the mode task at frame %u\n", d->name, frame);
}

void rr_tick(void)
{
    rd_budget_out();
    rr_budget = polls_per_frame / SLICES;
    if (rr_in_irq) { rr_budget = 1000; return; }      /* finish the handler first */
    g_rr_in_vblank = vblank_slices > 0;
    if (vblank_slices > 0) {                       /* split the vblank slice at VBEND */
        long vb = DSP_STEPS_PER_FRAME * 45 / 525 - (long)(2 - vblank_slices) * (DSP_STEPS_PER_FRAME / SLICES);
        long full = DSP_STEPS_PER_FRAME / SLICES;
        if (vb > full) vb = full;
        if (vb < 0) vb = 0;
        rr_dsp_run(vb);
        g_rr_in_vblank = false;
        rr_dsp_run(full - vb);
        vblank_slices--;
    } else
        rr_dsp_run(DSP_STEPS_PER_FRAME / SLICES);
    rr_sound_slice();                              /* the sound program's share of this slice */
    rr_audio_slice();                              /* and the mixer's samples */
    serial_acc += 100;                             /* 100 Hz serial pulse */
    if (serial_acc >= 60 * SLICES) { serial_acc -= 60 * SLICES; rr_dsp_serial(); }
    if (++slice % SLICES) return;
    frame++;
    sweep_tick();
    double tv0 = perf_on ? now_ms() : 0;
    if (g_rr_gl && gl_ok) {                        /* screen update at the end of the frame */
        rr_gl_prepare(rr_dsp_slave_active());
        /* headless: draw every frame (RR_GL_DRAWALL=1) so --perf counts the GL
         * cost too; glFinish so it is the GPU's time, not just the submission */
        static int drawall = -1;
        if (drawall < 0) { const char *e = getenv("RR_GL_DRAWALL"); drawall = e && *e == '1'; }
        if (drawall && !windowed) { rr_gl_draw(gl_w, gl_h); glFinish(); }
    }
#ifdef RR_ORACLE
    else if (!g_rr_gl) rr_video_frame(rr_dsp_slave_active());
#endif
    if (perf_on) {
        double t1 = now_ms();
        if (perf_n == perf_cap) { perf_cap = perf_cap ? perf_cap * 2 : 4096;
            perf_frame = realloc(perf_frame, perf_cap * sizeof *perf_frame); perf_video = realloc(perf_video, perf_cap * sizeof *perf_video); }
        if (t_frame_start > 0 && frame > 60) { perf_frame[perf_n] = t1 - t_frame_start; perf_video[perf_n] = t1 - tv0; perf_n++; }
        { extern void tex_caplog_frame(int); tex_caplog_frame((int)frame); }
        if (perflog && t_frame_start > 0) {         /* frame total_ms video_ms quads hits misses reallocs texels-this-frame placeholders refined tier-fallbacks (the last three cumulative) */
            fprintf(perflog, "%u %.3f %.3f %d %d %d %d %.0f %d %d %d\n", frame, t1 - t_frame_start, t1 - tv0, rr_gl_quads(), tex_frame_hits, tex_frame_misses,
                    tex_reallocs, g_bake_texels - perflog_texels, tex_placeholders, tex_refined, tex_tier_fallbacks);
            perflog_texels = g_bake_texels;
        }
    }
    for (int c = 0; c < n_test_coins; c++) {       /* scripted inputs for headless captures */
        if (frame == (uint32_t)test_coins[c]) g_hw.inputs &= (uint16_t)~0x1000;
        if (frame == (uint32_t)test_coins[c] + 6) g_hw.inputs |= 0x1000;
    }
    if (test_gas >= 0 && frame >= (uint32_t)test_gas) g_hw.gas = (uint16_t)g_rr_game->gas_max;
    { static int nv = -1; static uint32_t vf[16];          /* RR_TEST_VIEW=f1,f2,...: press VIEW CHANGE (active low 0x0040) for 30 frames at each */
      if (nv < 0) { nv = 0; const char *e = getenv("RR_TEST_VIEW"); while (e && *e && nv < 16) { char *q; const unsigned long v = strtoul(e, &q, 0); if (q == e) break; vf[nv++] = (uint32_t)v; e = q; if (*e == ',') e++; else break; } }
      for (int i = 0; i < nv; i++) { if (frame == vf[i]) g_hw.inputs &= (uint16_t)~0x0040; if (frame == vf[i] + 30) g_hw.inputs |= 0x0040; } }
    for (int i = 0; i < n_steer; i++) if (frame >= (uint32_t)test_steer[i].at) g_hw.steer = (uint16_t)test_steer[i].value;
    for (int i = 0; i < n_adc; i++) if (frame >= (uint32_t)test_adc[i].at) g_hw.adc[test_adc[i].idx & 3] = (uint8_t)test_adc[i].value;
    if (windowed) {
        if (frame % 120 == 0) rr_hw_eeprom_save();     /* the test menu's settings and the records, once the game has changed them */
        do {
            if (!rr_host_frame()) { perf_report(); rr_hw_eeprom_save(); rr_audio_close(); rr_host_close(); fprintf(stderr, "[RR] window closed at frame %u\n", frame); exit(0); }
            /* the Online menu lives in this loop: the built-in host and the lobby (HELLO/WELCOME, ROSTER, READY,
             * START) must keep running while it is open, or "Host a LAN game" never gets its own HELLO.
             * In a race session too, as a keepalive (pings, and the built-in host keeps relaying for the
             * others): without it the server dropped a player whose menu was open for 5 s */
            if (rr_host_paused()) rr_net_poll_paused();
        } while (rr_host_paused());
    }
    if (!windowed) {                               /* RR_PACE=1: a headless run at the real 59.9 Hz (network tests: jitter in ms means frames) */
        static int pace = -1; static uint32_t t0;
        if (pace < 0) { const char *e = getenv("RR_PACE"); pace = e && *e == '1'; t0 = SDL_GetTicks(); }
        if (pace) {
            const uint32_t t = SDL_GetTicks();
            uint32_t due = t0 + (uint32_t)((double)frame * 1000.0 / 59.9);
            if ((int32_t)(t - due) > 50) { t0 = t - (uint32_t)((double)frame * 1000.0 / 59.9); due = t; }   /* fell behind (a stall): carry on at 60 Hz, no catch-up burst */
            if ((int32_t)(due - t) > 0) SDL_Delay(due - t);
        }
    }
    rr_net_apply_inputs();                         /* online: the automatic gas that starts every machine together */
    rr_input_frame(frame);                         /* replay overrides, recorder logs */
    if (perf_on) t_frame_start = now_ms();
    if (shot_dir && shot_every && frame % shot_every == 0) {
        char p[1024]; snprintf(p, sizeof p, "%s/f%05u.ppm", shot_dir, frame);
        if (g_rr_gl && gl_ok && !windowed) { rr_gl_draw(gl_w, gl_h); rr_gl_write_ppm(p, gl_w, gl_h); }
#ifdef RR_ORACLE
        else if (!g_rr_gl) rr_video_write_ppm(p);
#endif
    }
    vblank_slices = 2;
    rr_dsp_vblank();
    if (!rr_env_active()) rr_hw_vblank();      /* (a trace oracle refreshes at MAME's own vblank interrupt: rr_env.c) */
    rr_net_poll();                       /* online play: peer FRAMEs into the link queue before the poll injects one */
    rr_link_poll();                      /* a received link packet's SCI IRQ lands at this frame edge */
    if (!rr_env_active()) {                  /* a trace oracle lands the interrupts itself */
        rr_deliver_irqs(rr_hw_irq_level);
        /* online: the other pending rivals' packets, each through the game's own SCI handler (offline nothing is pending) */
        for (int k = 0; k < 7; k++) {
            if (g_hw.irq_state & ~(1u << 2)) break;      /* only while the SCI is all that is pending: never re-run another, unacknowledged handler */
            if (!rr_link_inject_next()) break;
            rr_deliver_irqs(rr_hw_irq_level);
        }
    }
    /* a Debug-menu launch leaves the lifted call stack here (never returns if one is pending): AFTER the vblank interrupt,
     * whose handler rewinds the program's cooperative task list (Ace Driver: the pointer at A6-0x7FD0) -- launched before it,
     * the screen's first yield (0x4C6E) read past the end of the list and jumped to 0 */
    debug_boundary();
    if (dump_dir) {             /* RR_DUMP_EVERY=n (default 60), RR_DUMP_FROM=f: dump cadence */
        static unsigned every, from; static int init;
        if (!init) { const char *e = getenv("RR_DUMP_EVERY"), *f = getenv("RR_DUMP_FROM");
                     every = e && atoi(e) > 0 ? (unsigned)atoi(e) : 60; from = f ? (unsigned)atoi(f) : 0; init = 1; }
        if ((frame >= from && frame % every == 0) || frame == max_frames) dump_state();
    }
    if (frame % 60 == 0)
        fprintf(stderr, "[RR] frame %u  traps %u  unmapped %u  romwrites %u  irqs %u/%u/%u/%u/%u/%u/%u  en %02X pc_sr %04X\n",
                frame, rr_n_traps, g_rr.n_unmapped, g_rr.n_romwrite, rr_n_irq[1], rr_n_irq[2], rr_n_irq[3], rr_n_irq[4], rr_n_irq[5], rr_n_irq[6], rr_n_irq[7],
                g_hw.irq_enabled, (unsigned)get_sr());
    if (frame % 60 == 0) { char b[256]; rr_dsp_debug(b, sizeof b); fprintf(stderr, "     %s\n", b); }
    if (frame >= max_frames) {
        dump_state();
        perf_report();
        rr_input_record_stop();
        rr_audio_close();
        fprintf(stderr, "[RR] stop at frame %u, %u traps\n", frame, rr_n_traps);
        exit(rr_n_traps ? 3 : 0);
    }
}

#ifdef _WIN32
void rr_win_startup(void);          /* src/rr_win.c */
#endif
int main(int argc, char **argv)
{
#ifdef _WIN32
    rr_win_startup();               /* the program's folder, raveracer.log, DPI */
#endif
    const char *rom_dir = "extracted";
#ifdef RR_ORACLE
    int use_gl = -1;                            /* -1: GL in a window, the oracle headless */
#else
    int use_gl = 1;                             /* the game: always the engine */
#endif
    if (argc == 1) windowed = -1;               /* started with no arguments (a double-click): play */
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc) max_frames = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--dump") && i + 1 < argc) dump_dir = argv[++i];
        else if (!strcmp(argv[i], "--ppf") && i + 1 < argc) polls_per_frame = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--shots") && i + 2 < argc) { shot_dir = argv[++i]; shot_every = (uint32_t)atoi(argv[++i]); }
        else if (!strcmp(argv[i], "--render-dump")) i += 3;
        else if (!strcmp(argv[i], "--fullscreen")) { if (!windowed) windowed = -1; setenv("RR_FULLSCREEN", "1", 1); }
        else if (!strcmp(argv[i], "--window")) {
            windowed = -1;                                      /* -1: the saved window size */
            if (i + 1 < argc && argv[i + 1][0] >= '1' && argv[i + 1][0] <= '9' && !argv[i + 1][1]) windowed = atoi(argv[++i]);
        }
        else if (!strcmp(argv[i], "--perf")) perf_on = 1;
        else if (!strcmp(argv[i], "--sndsweep") && i + 1 < argc) sweep_arg(argv[++i]);
        else if (!strcmp(argv[i], "--perflog") && i + 1 < argc) { perflog = fopen(argv[++i], "w"); perf_on = perflog != NULL; }
        else if (!strcmp(argv[i], "--gl")) use_gl = 1;
#ifdef RR_ORACLE
        else if (!strcmp(argv[i], "--sw")) use_gl = 0;
#endif
        else if (!strcmp(argv[i], "--freeplay")) freeplay = 1;
        else if (!strcmp(argv[i], "--coins")) freeplay = 0;
        else if (!strcmp(argv[i], "--record") && i + 1 < argc) rec_path = argv[++i];
        else if (!strcmp(argv[i], "--replay") && i + 1 < argc) rep_path = argv[++i];
        else if (!strcmp(argv[i], "--joytest")) return rr_host_joytest();
        else if (!strcmp(argv[i], "--write-controls")) return rr_input_write(g_rr_game->cfg_file) ? 0 : 1;
        else if (!strcmp(argv[i], "--coin") && i + 1 < argc) {
            for (char *q = argv[++i]; *q && n_test_coins < 8; ) { char *e; long v = strtol(q, &e, 0); if (e == q) break; test_coins[n_test_coins++] = (int32_t)v; q = *e == ',' ? e + 1 : e; }
            test_coin = n_test_coins ? test_coins[0] : -1;
        }
        else if (!strcmp(argv[i], "--gas") && i + 1 < argc) test_gas = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--adc") && i + 1 < argc && n_adc < 32) {
            int f, k, v; if (sscanf(argv[++i], "%d:%d:%i", &f, &k, &v) == 3) { test_adc[n_adc].at = f; test_adc[n_adc].idx = k; test_adc[n_adc].value = v; n_adc++; }
        }
        else if (!strcmp(argv[i], "--steer") && i + 1 < argc && n_steer < 8) {
            int f, v; if (sscanf(argv[++i], "%d:%i", &f, &v) == 2) { test_steer[n_steer].at = f; test_steer[n_steer].value = v; n_steer++; }
        }
        else rom_dir = argv[i];
    }
#ifdef RR_ORACLE
    { const char *e = getenv("RR_RENDER");
      if (e && !strcmp(e, "sw")) use_gl = 0;
      if (e && !strcmp(e, "gl")) use_gl = 1; }
#endif
    for (int i = 1; i + 3 < argc; i++)          /* --render-dump DIR FRAME OUT.ppm: renderer gate, no CPU */
        if (!strcmp(argv[i], "--render-dump")) {
            if (!rr_dsp_init(rom_dir)) return 2;
#ifdef RR_ORACLE
            if (use_gl != 1) return rr_video_init(rom_dir) && rr_video_render_dump(argv[i + 1], atoi(argv[i + 2]), argv[i + 3]) ? 0 : 1;
#endif
            /* the captured state through the engine's pipeline */
            if (!rr_scene_load_capture(argv[i + 1], atoi(argv[i + 2])) || !rr_gl_init(rom_dir) || !rr_gl_open_headless(640, 480)) return 2;
            rr_scene_force_walk();
            rr_gl_prepare(true);
            rr_gl_draw(640, 480);
            fprintf(stderr, "[GL] render-dump: %d quads\n", rr_gl_quads());
            return rr_gl_write_ppm(argv[i + 3], 640, 480) ? 0 : 1;
        }
    if (rep_path && !rr_input_replay_open(rep_path)) return 2;
    if (rec_path && !rr_input_record_start(rec_path)) return 2;
    g_rr_gl = use_gl < 0 ? (windowed != 0) : use_gl;
    if (windowed) {
        if (!rr_host_open(windowed > 0 ? windowed : 0)) return 2;
        max_frames = 0xFFFFFFFFu;
    } else if (g_rr_gl) {
        const char *e = getenv("RR_RENDER_SIZE"); int w, h;
        if (e && sscanf(e, "%dx%d", &w, &h) == 2 && w >= 64 && h >= 48) { gl_w = w; gl_h = h; }
        /* no GL here: the game still runs (sound, traces, dumps), without a picture */
        gl_ok = rr_gl_open_headless(gl_w, gl_h);
        if (!gl_ok && shot_dir) { fprintf(stderr, "[RR] --shots needs OpenGL\n"); return 2; }
    }
    /* First run: take the ROMs out of MAME's raverace.zip + namcoc74.zip if the ROM
     * folder is incomplete (src/rr_romzip.c) -- how the Windows build is set up;
     * chips unzipped loose into roms/ work too. */
    if (g_rr_game->autosetup && rr_romzip_missing(rom_dir) && !strcmp(rom_dir, "extracted") && !rr_romzip_missing("roms"))
        rom_dir = "roms";
    if (g_rr_game->roms) {                       /* a game with a chip table (Ace Driver): the engine's table-driven unpacker */
        if (eng_romzip_missing(rom_dir, g_rr_game->roms, g_rr_game->nroms) && !strcmp(rom_dir, "extracted") &&
            !eng_romzip_missing("roms", g_rr_game->roms, g_rr_game->nroms))
            rom_dir = "roms";
        if (eng_romzip_missing(rom_dir, g_rr_game->roms, g_rr_game->nroms)) {
            char err[512], *base = SDL_GetBasePath();
            if (!eng_romzip_autosetup(rom_dir, base, g_rr_game->zips, g_rr_game->nzips, g_rr_game->roms, g_rr_game->nroms, err, sizeof err)) {
                char zl[256] = ""; for (int z = 0; z < g_rr_game->nzips; z++) { strncat(zl, z ? (z + 1 == g_rr_game->nzips ? " and " : ", ") : "", sizeof zl - strlen(zl) - 1); strncat(zl, g_rr_game->zips[z], sizeof zl - strlen(zl) - 1); }
                fprintf(stderr, "%s needs its ROMs (%s): %s\n", g_rr_game->title, zl, err);
                if (windowed) {
                    char msg[1024];
                    snprintf(msg, sizeof msg, "%s needs its ROMs.\n\nPut %s (the MAME ROM sets) in the \"roms\" folder next to this program, "
                             "then start it again.\n\n(%s)", g_rr_game->title, zl, err);
                    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, g_rr_game->title, msg, NULL);
                }
                SDL_free(base);
                return 2;
            }
            SDL_free(base);
        }
    }
    if (g_rr_game->autosetup && rr_romzip_missing(rom_dir)) {
        char err[512], *base = SDL_GetBasePath();
        if (!rr_romzip_autosetup(rom_dir, base, err, sizeof err)) {
            fprintf(stderr, "Rave Racer needs its ROMs: %s\n", err);
            if (windowed) {
                char msg[1024];
                snprintf(msg, sizeof msg,
                         "Rave Racer needs its ROMs.\n\n"
                         "Put raverace.zip and namcoc74.zip (the MAME ROM sets) in the \"roms\" "
                         "folder next to this program, then start it again.\n\n(%s)", err);
                SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Rave Racer", msg, NULL);
            }
            SDL_free(base);
            return 2;
        }
        SDL_free(base);
    }
    if (!rr_load_program(rom_dir)) return 2;
    rr_audio_init(rom_dir);
    rr_sound_init(rom_dir);
    rr_hw_init(rom_dir);
#ifdef RR_TRACE
    rr_env_init();                         /* RR_ENV: the trace oracle's environment (dev builds) */
#endif
    if (windowed) rr_hw_eeprom_persist(g_rr_game->nv_file);   /* a player's session keeps its settings and records; headless runs never do */
    if (freeplay >= 0) rr_hw_set_freeplay(freeplay);
    else if (windowed && g_cfg_freeplay >= 0) {        /* the saved menu choice, windowed runs only */
        rr_hw_set_freeplay(g_cfg_freeplay);
        fprintf(stderr, "[RR] %s (rr_controls.cfg)\n", g_cfg_freeplay ? "free play" : "coins required");
    }
    if (windowed) rr_hw_set_steering_motor(g_cfg_ffb_strength > 0);   /* the game only drives the motor with its own option ON */
    { const char *e = getenv("RR_LINK_CABINET");              /* temporary: the lobby/config will drive this */
      if (e) rr_hw_set_link_cabinet(atoi(e)); }
    if (g_cfg_net_name[0]) rr_net_set_name(g_cfg_net_name);        /* the saved lobby name (windowed: from rr_controls.cfg) */
    if (windowed && g_cfg_net_server[0]) rr_net_preset_server(g_cfg_net_server);  /* remembered, but connecting stays a menu action */
    { const char *srv = getenv("RR_NET_SERVER");            /* headless/test bootstrap: connect at boot (env wins over cfg) */
      if (srv && *srv) {
          const char *nm = getenv("RR_NET_NAME");
          if (nm && *nm) rr_net_set_name(nm);
          if (rr_net_set_server(srv)) rr_net_connect();
      }
      const char *hst = getenv("RR_NET_HOST");                /* headless/test: host a LAN game (the built-in server) */
      if (hst && *hst == '1') {
          const char *nm = getenv("RR_NET_NAME"); if (nm && *nm) rr_net_set_name(nm);
          rr_net_host_start();
      }
      const char *dsc = getenv("RR_NET_DISCOVER");            /* headless/test: search the LAN and join the first host */
      if (dsc && *dsc == '1') {
          const char *nm = getenv("RR_NET_NAME"); if (nm && *nm) rr_net_set_name(nm);
          rr_net_discover_autojoin(1); rr_net_discover();
      } }
#ifdef RR_ORACLE
    if (!g_rr_gl && !rr_video_init(rom_dir)) fprintf(stderr, "[RR] video ROMs missing\n");
#endif
    if (g_rr_gl) {
        if (!rr_gl_init(rom_dir)) { fprintf(stderr, "[RR] video ROMs missing\n"); return 2; }
        if (windowed) gl_ok = true;             /* the window's context */
    }
    memset(R, 0, sizeof R);
    RS4(REG_SP, rr_read(0, 4));                 /* reset SP from vector 0 */
    set_sr(0x2700);                             /* supervisor, IPL 7 */
    rr_budget = polls_per_frame / SLICES;
    fprintf(stderr, "[RR] reset: SP=%08X PC=%08X\n", (uint32_t)RG4(REG_SP), rr_read(4, 4));
    rr_call_push(0xFFFFFFFEu);                  /* bottom of the shadow stack */
    { extern void rd_init(void); rd_init(); }   /* readable-C replacements (src/rd) */
    g_rr_game->entry();                         /* entry_reset (this game's reset PC): never returns */
    fprintf(stderr, "[RR] entry_reset returned?!\n");
    return 1;
}
