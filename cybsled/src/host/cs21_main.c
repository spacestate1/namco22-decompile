/*
 * cs21_main.c -- Cyber Sled (System 21):
 *   cs21 <romdir> [--window [N]] [--fullscreen]          the game in a window (engine/ss22_host.c: menu bar, display modes, pacing, sound)
 *   cs21 <romdir> --frames N [--shots DIR EVERY]         headless (the software picture to PPMs: the exact 496x480 oracle frame)
 *        [--glshots DIR EVERY [WxH]]                     headless GL picture at WxH (default 496x480) to DIR/fNNNNN_gl.ppm
 *        [--gldraw WxH]                                  headless, the window's work: the GL picture drawn every frame at WxH,
 *                                                        no software picture (perf A/B against a plain headless run)
 *        [--glgate EVERY [DIR]]                          the GL picture at 496x480 against the software one, same frame: per
 *                                                        frame mean error and % identical pixels (DIR: both pictures + a diff)
 *   dev: [--only master|slave] [--env-m FILE] [--env-s FILE] (cs21_trace: replay MAME's environment, tools/t3_run.sh)
 *        [--ipf N] [--slices N] [--stats EVERY] [--coin F] [--start F] [--play F] (headless scripted cabinet)
 * Settings: cs21_controls.cfg; the NVRAM (settings, calibration, high scores) in cs21.nv beside it, interactive runs only.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "s21_board.h"
#include "s21_cpu.h"
#ifdef S21_WITH_HOST
#include "ss22_host.h"
#include "audio_out.h"
#include "s21_host.h"
#include "s21_video.h"
#include "s21_snd.h"
#include "s21_io.h"
#include "s21_debug.h"
#include "s21_link.h"
#include "win_startup.h"
#include "win_gpu.h"                   /* Windows: run on the discrete GPU of a two-GPU laptop (the exports the drivers look for) */
#endif

#ifdef S21_WITH_HOST
#include "eng_gl.h"
#include "eng_display.h"
#include "s21_video_int.h"
const uint8_t *s21_host_picture(void) { return s21_video_rgb(); }
static int pic_gl = 1;                              /* the window's picture: GL (default) or software (CS_PICTURE=sw) */
int s21_host_use_gl(void) { return pic_gl; }
void s21_host_gl_unavailable(void) { if (pic_gl) fprintf(stderr, "[cs21] no GL picture: the software picture\n"); pic_gl = 0; s21_video_set_sw3d(true); }
bool eng_gl_open_headless(int w, int h);
bool eng_gl_write_ppm(const char *path, int vw, int vh);
void ss22_draw(int vw, int vh);                    /* src/host/s21_glutil.c: the picture as the window draws it */

/* --glgate: the GL picture (496x480, drawn into the headless window) against the software picture of the same frame */
static struct { long frames, px, same; double err; int worst_f; double worst; } gg;
static void glgate_frame(int f, const char *dir)
{
    static uint8_t gl[S21_W * S21_H * 3];
    if (!s21_video_draw_gl(0, 0, S21_W, S21_H)) { fprintf(stderr, "[GLGATE] no GL picture\n"); exit(1); }
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, S21_W, S21_H, GL_RGB, GL_UNSIGNED_BYTE, gl);
    const uint8_t *sw = s21_video_rgb();
    long same = 0; double err = 0;
    for (int y = 0; y < S21_H; y++) {
        const uint8_t *g = gl + (size_t)(S21_H - 1 - y) * S21_W * 3, *o = sw + (size_t)y * S21_W * 3;
        for (int x = 0; x < S21_W; x++, g += 3, o += 3) {
            const int d = abs(g[0] - o[0]) + abs(g[1] - o[1]) + abs(g[2] - o[2]);
            if (!d) same++;
            err += d / 3.0;
        }
    }
    const long n = (long)S21_W * S21_H;
    gg.frames++; gg.px += n; gg.same += same; gg.err += err;
    if (err / n > gg.worst) { gg.worst = err / n; gg.worst_f = f; }
    long nsh = 0, ncond = 0;                         /* what the frame exercises: shadow ops on polygon pixels, z-mixed ones */
    const uint8_t *g2 = s21v_gl2d();
    for (long i = 0; i < n; i++) { const unsigned op = g2[i * 4 + 2] | g2[i * 4 + 3] << 8; nsh += op == 0xfffe || op == 0xfffd || op == 0xfffc || op == 0xfffb; ncond += op == 0xfffc || op == 0xfffb || (op >= 0x9000 && op < 0xa000); }
    fprintf(stderr, "[GLGATE] f%05d quads %d zmix %d shadow-px %ld zmix-px %ld: mean error %.4f/255, identical %.3f %% (%ld px differ)\n", f,
            s21_video_quads_last(), s21v_zmix(), nsh, ncond, err / n, 100.0 * same / n, n - same);
    if (dir) {
        char p[1024];
        { snprintf(p, sizeof p, "%s/f%05d_quads.txt", dir, f); FILE *qf = fopen(p, "w");     /* the visible frame's quads, for offline checks */
          int nq; const uint16_t *qc; const int *qz; const s21_quad *qq = s21v_vis_quads(&nq, &qc, &qz);
          for (int k = 0; qf && k < nq; k++) {
              for (int i = 0; i < 4; i++) fprintf(qf, "%d %d %d ", qq[k].sx[i], qq[k].sy[i], qq[k].z[i]);
              fprintf(qf, "%u %u %d\n", qq[k].color, qc[k], qz[k]);
          }
          if (qf) fclose(qf); }
        snprintf(p, sizeof p, "%s/f%05d_sw.ppm", dir, f); s21_video_write_ppm(p);
        snprintf(p, sizeof p, "%s/f%05d_gl.ppm", dir, f); eng_gl_write_ppm(p, S21_W, S21_H);
    }
}
static void glgate_summary(void)
{
    if (!gg.frames) return;
    fprintf(stderr, "[GLGATE] %ld frames: mean error %.4f/255, identical %.4f %% of %ld px; worst frame f%05d %.4f/255\n", gg.frames,
            gg.err / gg.px, 100.0 * gg.same / gg.px, gg.px, gg.worst_f, gg.worst);
}
static int snd_live;
static void snd_set_output(bool live) { snd_live = live; }
static const ss22_host_game game = {
    .title = "Cyber Sled", .cfg_file = "cs21_controls.cfg", .tag = "CS", .shot_prefix = "cs",
    .input_init = s21_input_init, .input_page = s21_input_page, .input_event = s21_input_event,
    .input_update = s21_input_update, .input_neutral = s21_input_neutral, .snd_set_output = snd_set_output,
    .extra_page = s21_debug_page,
    .paused_tick = s21_net_paused,                  /* online keepalive while the menu is open (src/link/s21_net.c) */
};
/* the sound board makes 48 kHz stereo (src/snd); engine/audio_out.c takes the C352's 85,333 Hz four-channel stream, so it is
 * stretched to that rate (linear). TODO (HARD RULE 3): an input-rate setting in engine/audio_out.c instead (proposed patch). */
static void audio_frame(void)
{
    static double acc; static int16_t last[2];
    static int16_t in[2 * 1024], out[4 * 2048];
    int n = s21_snd_mix(in, 801);                   /* ~one frame of 48 kHz */
    if (n <= 0) return;
    acc += n * (85333.333 / 48000.0);
    int m = (int)acc; acc -= m;
    if (m > 2048) m = 2048;
    for (int i = 0; i < m; i++) {
        double t = (double)i * n / m; int k = (int)t; double f = t - k;
        for (int c = 0; c < 2; c++) {
            int a = k == 0 ? last[c] : in[2 * (k - 1) + c], b = in[2 * k + c];
            out[4 * i + c] = (int16_t)(a + (b - a) * f); out[4 * i + 2 + c] = 0;
        }
    }
    last[0] = in[2 * (n - 1)]; last[1] = in[2 * (n - 1) + 1];
    if (snd_live) eng_audio_push(out, m);
}
#endif

extern int g_eng_disp_native_default;
int main(int argc, char **argv)
{
#ifdef S21_WITH_HOST
    g_eng_disp_native_default = 1;    /* the board is 496x480: draw at the window's size, so the picture is resized once (s21_glutil.c) */
#endif
#ifdef S21_WITH_HOST
    eng_win_startup("cybsled.log");     /* Windows: the program's own folder, the log beside the .exe, real pixel sizes (a no-op elsewhere) */
    static char *dflt[] = { NULL, "extracted", "--window", NULL };
    if (argc == 1) { dflt[0] = argv[0]; argv = dflt; argc = 3; }   /* no arguments (a double-click, the Windows how-to): play, in a window */
#endif
    if (argc < 2) {
        fprintf(stderr, "usage: %s <romdir> [--window [N]] [--fullscreen] [--frames N] [--shots DIR EVERY] [--only master|slave] [--env-m F] [--env-s F]\n", argv[0]);
        return 2;
    }
    const char *romdir = argv[1], *env_m = NULL, *env_s = NULL, *shots = NULL, *glshots = NULL, *glgate_dir = NULL;
    int glshot_every = 0, glshot_w = 496, glshot_h = 480, glgate = 0, gldraw = 0;
    int coin_f = -1, start_f = -1, play_f = -1;
    int frames = -1, stats = 0, onlyc = -1, window = 0, scale = 0, fs = 0, every = 60;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--window")) { window = 1; if (i + 1 < argc && argv[i + 1][0] >= '1' && argv[i + 1][0] <= '9') scale = atoi(argv[++i]); }
        else if (!strcmp(argv[i], "--fullscreen")) { window = 1; fs = 1; }
        else if (!strcmp(argv[i], "--shots") && i + 2 < argc) { shots = argv[++i]; every = atoi(argv[++i]); }
        else if (!strcmp(argv[i], "--glshots") && i + 2 < argc) {
            glshots = argv[++i]; glshot_every = atoi(argv[++i]);
            if (i + 1 < argc && sscanf(argv[i + 1], "%dx%d", &glshot_w, &glshot_h) == 2) i++;
        }
        else if (!strcmp(argv[i], "--gldraw") && i + 1 < argc) { gldraw = 1; sscanf(argv[++i], "%dx%d", &glshot_w, &glshot_h); }
        else if (!strcmp(argv[i], "--glgate") && i + 1 < argc) {
            glgate = atoi(argv[++i]);
            if (i + 1 < argc && argv[i + 1][0] != '-') glgate_dir = argv[++i];
        }
        else if (!strcmp(argv[i], "--only") && i + 1 < argc) { i++; onlyc = !strcmp(argv[i], "slave") ? 1 : 0; }
        else if (!strcmp(argv[i], "--env-m") && i + 1 < argc) env_m = argv[++i];
        else if (!strcmp(argv[i], "--env-s") && i + 1 < argc) env_s = argv[++i];
        else if (!strcmp(argv[i], "--ipf") && i + 1 < argc) setenv("S21_IPF", argv[++i], 1);
        else if (!strcmp(argv[i], "--slices") && i + 1 < argc) setenv("S21_SLICES", argv[++i], 1);
        else if (!strcmp(argv[i], "--stats") && i + 1 < argc) stats = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--coin") && i + 1 < argc) coin_f = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--start") && i + 1 < argc) start_f = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--play") && i + 1 < argc) play_f = atoi(argv[++i]);
        else { fprintf(stderr, "unknown option %s\n", argv[i]); return 2; }
    }
    if (frames < 0) frames = window ? 0x7FFFFFFF : 600;
#ifdef S21_WITH_HOST
    if (!(romdir = cs21_rom_setup(romdir, window))) return 1;   /* first run: the chips out of MAME's zips (src/host/cs21_roms.c) */
#endif
    if (!s21_init(romdir)) return 1;
    s21_set_only(onlyc);
    /* the C67 programs run TRANSLATED (s21_dsp, gen/cs_c67_*.c, module E): no interpreter in the game (HARD RULE 1) */
    if (env_m && !s21_cpu0_env_init(env_m)) { fprintf(stderr, "master environment not loaded (not a trace build?)\n"); return 1; }
    if (env_s && !s21_cpu1_env_init(env_s)) { fprintf(stderr, "slave environment not loaded (not a trace build?)\n"); return 1; }
#ifdef S21_WITH_HOST
    s21_video_bind(g_s21.pal, g_s21.palext, g_s21.spr, g_s21.sprpos, &g_s21.vena);
    s21_board_quad_hooks((void (*)(const void *))s21_video_quad, s21_video_swap);
    if (!s21_video_init(romdir)) fprintf(stderr, "[cs21] sprite ROMs missing: no sprites\n");
    { const char *e = getenv("CS_PICTURE"); if (e && !strcmp(e, "sw")) pic_gl = 0; }
    int vmode = S21_OUT_SW;                         /* headless: the software picture, the oracle (--shots) */
    { const char *e = getenv("CS_NVRAM");             /* tests: a headless run from a given NVRAM file (read only) */
      if (!window && e && s21_nvram_load(e)) fprintf(stderr, "[cs21] NVRAM from %s (not saved)\n", e); }
    if (window) {
        if (s21_nvram_load("cs21.nv")) fprintf(stderr, "[cs21] NVRAM from cs21.nv\n");
        if (!ss22_host_open(&game, scale, fs)) return 1;
        s21_net_window_init();                      /* the Online page (src/link/s21_net.c): inert until the player connects */
    } else if (glshots || glgate || gldraw) {
        if (glgate) { glshot_w = S21_W; glshot_h = S21_H; }
        if (!eng_gl_open_headless(glshot_w, glshot_h)) return 1;
        vmode = gldraw ? S21_OUT_GL : S21_OUT_BOTH;
        if (gldraw) s21_video_set_sw3d(false);
        { const char *e = getenv("ENG_WIDESCREEN"); if (e) g_eng_disp.wide = atoi(e) != 0; }   /* Hor+ widescreen in the GL shots (window: the cfg) */
    }
    if (shots) mkdir(shots, 0755);
    if (glshots) mkdir(glshots, 0755);
    if (glgate_dir) mkdir(glgate_dir, 0755);
#else
    if (window || shots) { fprintf(stderr, "this build has no window or picture (cs21_trace): use cs21\n"); return 2; }
#endif
    for (int f = 0; f < frames; f++) {
#ifdef S21_WITH_HOST
        if (!window && getenv("CS_DEBUG") && coin_f < 0 && start_f < 0 && play_f < 0) {   /* headless Debug page test (s21_debug_page.c) */
            s21_inputs in; s21_io_default_inputs(&in);
            static int init, ndp, an_from = 0, an_to = 0x7FFFFFFF, tan_set; static int dpf[16], dpl[16]; static unsigned tan[4];
            if (!init) {                     /* CS_DEBUG_GUN=f[+len],..: Gun from f for len (6) frames; CS_TEST_AN=a0,a1,a2,a3 [CS_TEST_AN_AT=from:to] */
                init = 1; const char *e = getenv("CS_DEBUG_GUN");
                for (const char *c = e; c && *c && ndp < 16; ) { dpl[ndp] = 6; if (sscanf(c, "%d+%d", &dpf[ndp], &dpl[ndp]) >= 1) ndp++; c = strchr(c, ','); if (c) c++; }
                e = getenv("CS_TEST_AN"); tan_set = e && sscanf(e, "%x,%x,%x,%x", &tan[0], &tan[1], &tan[2], &tan[3]) == 4;
                e = getenv("CS_TEST_AN_AT"); if (e) sscanf(e, "%d:%d", &an_from, &an_to);
            }
            for (int k = 0; k < ndp; k++) if (f >= dpf[k] && f < dpf[k] + dpl[k]) in.mcuh &= (uint8_t)~0x20;
            if (tan_set && f >= an_from && f < an_to) for (int a = 0; a < 4; a++) in.an[a] = (uint8_t)tan[a];
            { static int hf = -1, ht, hm, hp, dm = -1;       /* CS_TEST_H=from:to:mcuh-bits[:mcub-bits] held (active low); CS_TEST_DSW=bits ON */
              if (hf == -1) { const char *e = getenv("CS_TEST_H"); hf = -2; if (e) sscanf(e, "%d:%d:%x:%x", &hf, &ht, &hm, &hp); e = getenv("CS_TEST_DSW"); dm = e ? (int)strtol(e, NULL, 16) : 0; }
              if (f >= hf && f < ht) { in.mcuh &= (uint8_t)~hm; in.mcub &= (uint8_t)~hp; }
              in.dsw &= (uint8_t)~dm; }
            s21_debug_frame(&in);
            s21_io_set_inputs(&in);
        }
        if (!window && getenv("CS_FUZZ")) {      /* tests (coverage of the translated 6809 / C68): a seeded random walk over every cabinet
                                                    input -- buttons held for random spans, coin / service pulses, levers anywhere, the Test
                                                    switch ON over CS_FUZZ_TEST=from:to (the operator test mode); CS_FUZZ=<seed> */
            static uint32_t rng; static int init, tf = -1, tt = -1, hold_left; static s21_inputs held;
            if (!init) { init = 1; rng = (uint32_t)strtoul(getenv("CS_FUZZ"), NULL, 0) * 2654435761u + 1; const char *e = getenv("CS_FUZZ_TEST"); if (e) sscanf(e, "%d:%d", &tf, &tt);
                         s21_io_default_inputs(&held); }
            #define FZ() (rng = rng * 1103515245u + 12345u, rng >> 8)
            if (hold_left-- <= 0) {
                s21_io_default_inputs(&held);
                hold_left = (int)(FZ() % 40) + 2;
                held.mcuh = (uint8_t)~(FZ() & 0x2A);                 /* gun 0x20, missile 0x08, view 0x02 */
                held.mcub = (uint8_t)((FZ() % 4 == 0) ? 0x7F : 0xFF);    /* start */
                held.mcuc = (uint8_t)~((FZ() % 25 == 0 ? 0x20 : 0) | (FZ() % 30 == 0 ? 0x80 : 0));   /* coin, service */
                for (int a = 0; a < 4; a++) held.an[a] = (uint8_t)(FZ() % 3 == 0 ? 0x7F : FZ() & 0xFF);
            }
            #undef FZ
            s21_inputs in = held;
            in.dsw = (uint8_t)((in.dsw & ~1) | ((f >= tf && f < tt) ? 0 : 1));   /* bit 0 low = the Test switch on */
            s21_io_set_inputs(&in);
        } else
        if (!window && (coin_f >= 0 || start_f >= 0 || play_f >= 0)) {   /* headless scripted cabinet: coin, start, then gun/missile + sticks */
            s21_inputs in; s21_io_default_inputs(&in);
            if (f >= coin_f && f < coin_f + 6) in.mcuc &= (uint8_t)~0x20;
            if (f >= start_f && f < start_f + 6) in.mcub &= (uint8_t)~0x80;
            if (play_f >= 0 && f >= play_f) {
                static int nogun = -1; if (nogun < 0) nogun = getenv("CS_TEST_NOGUN") != NULL;   /* tests: levers only, no gun/missile */
                if (!nogun && f % 20 < 6) in.mcuh &= (uint8_t)~0x20;
                if (!nogun && f % 150 < 4) in.mcuh &= (uint8_t)~0x08;
                in.an[1] = (uint8_t)(0x7F - 0x30 * ((f / 90) % 2)); in.an[0] = (uint8_t)(0x7F - 0x30 * ((f / 120) % 2));
                static int tan_set = -1; static unsigned tan[4];   /* CS_TEST_AN=an0,an1,an2,an3 (hex): hold the levers there (tests) */
                if (tan_set < 0) { const char *e = getenv("CS_TEST_AN"); tan_set = e && sscanf(e, "%x,%x,%x,%x", &tan[0], &tan[1], &tan[2], &tan[3]) == 4; }
                if (tan_set) for (int a = 0; a < 4; a++) in.an[a] = (uint8_t)tan[a];
            }
            s21_io_set_inputs(&in);
        }
        if (window) { int rw, rh; eng_disp_render_size(&rw, &rh); s21_view_for(rw, rh); }   /* Hor+: the DSPs' view for the picture */
        else if (glshots || gldraw) s21_view_for(glshot_w, glshot_h);
#endif
        s21_run_frame();
        { static int every = -1; static const char *dir; if (every < 0) { const char *e = getenv("CS_RAMDUMP"); every = e ? atoi(e) : 0; dir = getenv("CS_RAMDUMP_DIR"); }
          if (every > 0 && dir && (f + 1) % every == 0) {    /* tests: the master's work RAM (big-endian words) every N frames */
              char p[512]; snprintf(p, sizeof p, "%s/m%05d.bin", dir, f + 1); FILE *o = fopen(p, "wb");
              if (o) { for (int i = 0; i < 0x8000; i++) { uint8_t b2[2] = { (uint8_t)(g_s21.mram[i] >> 8), (uint8_t)g_s21.mram[i] }; fwrite(b2, 1, 2, o); } fclose(o); } } }
#ifdef S21_WITH_HOST
        if (window) {                                /* the window: only the picture it shows */
            if (pic_gl) s21_video_set_sw3d(false);
            s21_video_end_frame(pic_gl ? S21_OUT_GL : S21_OUT_SW);
        } else s21_video_end_frame(vmode);
        audio_frame();
        if (glgate && (f + 1) % glgate == 0) glgate_frame(f + 1, glgate_dir);
        if (gldraw) { ss22_draw(glshot_w, glshot_h); if (!getenv("CS_NOFINISH")) glFinish(); }   /* glFinish: the frame done before the next, as a swap would */
        if (glshots && glshot_every > 0 && (f + 1) % glshot_every == 0) {
            char p[1024]; snprintf(p, sizeof p, "%s/f%05d_gl.ppm", glshots, f + 1);
            ss22_draw(glshot_w, glshot_h);              /* as the window draws it: 4:3, pillarboxed when wider */
            eng_gl_write_ppm(p, glshot_w, glshot_h);
        }
        if (shots && every > 0 && (f + 1) % every == 0) {
            char p[1024]; snprintf(p, sizeof p, "%s/f%05d.ppm", shots, f + 1);
            s21_video_write_ppm(p);
        }
        if (window && !ss22_host_frame()) break;
#endif
        if (stats && (f + 1) % stats == 0)
            fprintf(stderr, "[cs21] frame %d: ins master %llu slave %llu (slave %s), traps %u/%u, vena %04X\n", f + 1,
                    (unsigned long long)g_s21.ins[0], (unsigned long long)g_s21.ins[1], g_s21.slave_reset ? "held" : "running",
                    s21_cpu0_traps(), s21_cpu1_traps(), g_s21.vena);
    }
#ifdef S21_WITH_HOST
    glgate_summary();
    if (window) {
        s21_net_restore_settings();
        if (s21_nvram_save("cs21.nv")) fprintf(stderr, "[cs21] NVRAM saved to cs21.nv\n");
        ss22_host_close();
    }
#endif
    return 0;
}
