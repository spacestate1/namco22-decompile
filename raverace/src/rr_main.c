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
#include "rr_lifted.h"
#include "rr_input.h"
#include "rr_sound.h"

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
static int32_t test_coin = -1, test_gas = -1;   /* --coin F: pulse coin 1 at frame F; --gas F: hold gas from F */

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
    }
    if (test_coin >= 0) {                          /* scripted inputs for headless captures */
        if (frame == (uint32_t)test_coin) g_hw.inputs &= (uint16_t)~0x1000;
        if (frame == (uint32_t)test_coin + 6) g_hw.inputs |= 0x1000;
    }
    if (test_gas >= 0 && frame >= (uint32_t)test_gas) g_hw.gas = 0x610;
    if (windowed) {
        do {
            if (!rr_host_frame()) { perf_report(); rr_audio_close(); rr_host_close(); fprintf(stderr, "[RR] window closed at frame %u\n", frame); exit(0); }
        } while (rr_host_paused());
    }
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
    rr_hw_vblank();
    rr_deliver_irqs(rr_hw_irq_level);
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
        else if (!strcmp(argv[i], "--gl")) use_gl = 1;
#ifdef RR_ORACLE
        else if (!strcmp(argv[i], "--sw")) use_gl = 0;
#endif
        else if (!strcmp(argv[i], "--freeplay")) freeplay = 1;
        else if (!strcmp(argv[i], "--coins")) freeplay = 0;
        else if (!strcmp(argv[i], "--record") && i + 1 < argc) rec_path = argv[++i];
        else if (!strcmp(argv[i], "--replay") && i + 1 < argc) rep_path = argv[++i];
        else if (!strcmp(argv[i], "--joytest")) return rr_host_joytest();
        else if (!strcmp(argv[i], "--write-controls")) return rr_input_write("rr_controls.cfg") ? 0 : 1;
        else if (!strcmp(argv[i], "--coin") && i + 1 < argc) test_coin = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--gas") && i + 1 < argc) test_gas = atoi(argv[++i]);
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
    if (rr_romzip_missing(rom_dir) && !strcmp(rom_dir, "extracted") && !rr_romzip_missing("roms"))
        rom_dir = "roms";
    if (rr_romzip_missing(rom_dir)) {
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
    if (freeplay >= 0) rr_hw_set_freeplay(freeplay);
    else if (windowed && g_cfg_freeplay >= 0) {        /* the saved menu choice, windowed runs only */
        rr_hw_set_freeplay(g_cfg_freeplay);
        fprintf(stderr, "[RR] %s (rr_controls.cfg)\n", g_cfg_freeplay ? "free play" : "coins required");
    }
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
    L_4000();                                   /* entry_reset: never returns */
    fprintf(stderr, "[RR] entry_reset returned?!\n");
    return 1;
}
