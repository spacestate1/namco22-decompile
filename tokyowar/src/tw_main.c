/*
 * tw_main.c -- host for the lifted Tokyo Wars program (T3: the 68K alone).
 *
 * gen/tw_lifted.c is the 68EC020 program; entry_reset (L_4000) never returns, it runs the game's
 * own main loop. The register file, shadow return stack, traps, interrupt entry and trace hook
 * are the shared engine/lift_cpu.c; this file is the scheduler: time advances in rr_tick(),
 * which the lifted code polls on every call and backward branch, and every SLICES slices is
 * one video frame (vblank IRQ). The memory map is src/tw_mem.c, the devices src/tw_hw.c.
 *
 *   tw <rom_dir> [--frames N] [--ppf N] [--dump DIR]
 *   tw <rom_dir> --window [N] [--fullscreen]              play: OpenGL window (Nx the 640x480 picture), sound card, keyboard and pads
 *   tw <rom_dir> --shots DIR N [--frames M]               headless: a 640x480 PPM every N frames (offscreen OpenGL)
 *   tw <rom_dir> --render-dump DIR FRAME OUT.ppm          MAME's captured video state (tools/mame/vid_dump.lua) through the engine
 * Trace build (tw_trace): RR_TRACE_FILE / RR_TRACE_MAX (engine/lift_cpu.c) write the trace that
 * tools/namco22/diff_trace_mame.py lines up against tools/mame/regtrace.lua; TW_ENV=<file> replays the
 * DSP / MCU / interrupt environment MAME saw (tools/mame/env_from_trace.py, src/tw_env.c).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tw_mem.h"
#include "tw_hw.h"
#include "tw_env.h"
#include "tw_dsp.h"
#include "tw_snd.h"
#include "tw_video.h"
#include "tw_host.h"
#include <sys/stat.h>
#include "tw_rom.h"
#include "win_startup.h"
#include <SDL2/SDL.h>
#ifndef _WIN32
#include <unistd.h>
#endif
static char **g_argv;
#include "lift_rt.h"
#include "lift_cpu.h"
#include "tw_lifted.h"

static int32_t  polls_per_frame = 66772;   /* 68K instructions per frame: MAME's average over its first 300 frames */
static uint32_t max_frames = 600;
static const char *dump_dir;
static int  win_scale, win_full, shot_every;       /* --window, --fullscreen, --shots */
static const char *shot_dir;
static bool video_on;

#define SLICES 16
static uint32_t slice;
static int vblank_slices;
static long dsp_steps_per_frame = 166667;  /* the C71 at 40 MHz retires 10 M instructions/s: 1/59.9 s */
static long serial_acc;                    /* MAME's dsp_serial timer: 100 Hz */

void rd_budget_out(void);

/* ---- the cabinet, scripted (headless): what the MCU's ports and A-D channels read ----------------------
 *   --autoplay            the coin/start/pedal/wheel/trigger pattern tools/mame/cov_trace_play.lua feeds MAME
 *   --press NAME@F[+LEN]  hold coin|start|service|test|ltrig|rtrig from frame F for LEN frames (default 6)
 *   --pedal F             forward pedal down from frame F                                                     */
typedef struct { uint16_t bit; long from, len; } press_t;
static press_t presses[32]; static int npress;
static int autoplay; static long pedal_from = -1, input_offset, clock_addr;      /* --clock ADDR: run the script on the game's own frame counter (work RAM) instead of video frames */      /* --input-offset N: the script runs N frames late (our boot lags MAME's) */
static void add_press(const char *spec)
{
    static const struct { const char *n; uint16_t b; } names[] = {
        {"coin", TW_IN_COIN}, {"start", TW_IN_START}, {"service", TW_IN_SERVICE}, {"test", TW_IN_TEST},
        {"ltrig", TW_IN_LTRIG}, {"rtrig", TW_IN_RTRIG} };
    char n[16]; long f, len = 6;
    if (npress >= 32 || sscanf(spec, "%15[a-z]@%ld+%ld", n, &f, &len) < 2) { fprintf(stderr, "[TW] bad --press %s\n", spec); return; }
    for (unsigned i = 0; i < sizeof names / sizeof *names; i++)
        if (!strcmp(n, names[i].n)) { presses[npress++] = (press_t){ names[i].b, f, len }; return; }
    fprintf(stderr, "[TW] unknown button %s\n", n);
}
static void update_inputs(void)
{
    if (tw_host_active() && !autoplay && !npress && pedal_from < 0) return;      /* the window's keys drive the cabinet (tw_host_frame) */
    long n = (long)rr_frame - input_offset;
    if (clock_addr) n = (long)rr_read((uint32_t)clock_addr, 4);
    uint16_t p = 0; unsigned wheel = 0x200, fwd = 0;
    for (int i = 0; i < npress; i++) if (n >= presses[i].from && n < presses[i].from + presses[i].len) p |= presses[i].bit;
    if (pedal_from >= 0 && n >= pedal_from) fwd = 0x100;
    if (autoplay) {
        if (n % 600 >= 300 && n % 600 < 306) p |= TW_IN_COIN;
        if (n % 300 >= 150 && n % 300 < 156 && n > 400) p |= TW_IN_START;
        if (n > 700) fwd = 0x100;
        wheel = ((n / 90) % 2 == 0) ? 0x100 : 0x300;
        if (n % 40 < 4) p |= TW_IN_LTRIG;
        if (n % 55 < 4) p |= TW_IN_RTRIG;
    }
    tw_snd_inputs(p, wheel, fwd, 0);
}

static void dump_state(void)
{
    if (!dump_dir) return;
    char p[1024];
    snprintf(p, sizeof p, "%s/wram_f%u.bin", dump_dir, rr_frame);
    FILE *f = fopen(p, "wb");
    if (f) { fwrite(g_tw.wram, 1, TW_WRAM_SIZE, f); fclose(f); }
    snprintf(p, sizeof p, "%s/shared_f%u.bin", dump_dir, rr_frame);
    f = fopen(p, "wb");
    if (f) { fwrite(g_tw.shared, 1, TW_SHARED_SIZE, f); fclose(f); }
    snprintf(p, sizeof p, "%s/poly_f%u.bin", dump_dir, rr_frame);
    f = fopen(p, "wb");
    if (f) {
        for (uint32_t i = 0; i < TW_POLY_WORDS; i++) {
            uint32_t w = g_tw.poly[i] | 0xFF000000u;           /* what the 68K reads */
            uint8_t b[4] = { (uint8_t)(w >> 24), (uint8_t)(w >> 16), (uint8_t)(w >> 8), (uint8_t)w };
            fwrite(b, 1, 4, f);
        }
        fclose(f);
    }
}

void rr_tick(void)
{
    rd_budget_out();
    rr_budget = polls_per_frame / SLICES;
    if (rr_in_irq) { rr_budget = 1000; return; }        /* finish the handler first */
    g_tw_in_vblank = vblank_slices > 0;
    if (vblank_slices > 0) {                            /* VTOTAL 525, VBSTART 480: the vblank is the first 45/525 of a frame after the */
        long full = dsp_steps_per_frame / SLICES;       /* screen update, ~1.37 of our slices; split the slice at VBEND. The master answers */
        long vb = dsp_steps_per_frame * 45 / 525 - (long)(2 - vblank_slices) * full;   /* the vblank INT0 inside it, and its PDP begin then */
        if (vb > full) vb = full;                       /* marks the NEXT update's list current (engine/frame_rule.h) */
        if (vb < 0) vb = 0;
        tw_dsp_run(vb);
        g_tw_in_vblank = false;
        tw_dsp_run(full - vb);
        vblank_slices--;
    } else
        tw_dsp_run(dsp_steps_per_frame / SLICES);       /* the master DSP runs beside the 68K */
    tw_snd_slice();                                     /* and so does the sound MCU (ports, A-D, the C352) */
    if ((serial_acc += 100) >= 60 * SLICES) { serial_acc -= 60 * SLICES; tw_dsp_serial(); }
    if (++slice % SLICES) return;
    rr_frame++;
    update_inputs();
    if (video_on) {
        tw_video_prepare();                              /* the master's finished list, before this vblank starts the next */
        if (shot_dir && rr_frame % shot_every == 0) {
            char p[1024]; snprintf(p, sizeof p, "%s/tw_f%u.ppm", shot_dir, rr_frame);
            tw_host_shot(p);
        }
        if (tw_host_active() && !tw_host_frame()) {
            const bool restart = tw_host_restart_requested();       /* File > Restart: power-cycle the cabinet */
            tw_snd_close(); tw_host_close(); fprintf(stderr, "[TW] quit at frame %u\n", rr_frame);
#ifndef _WIN32
            if (restart) { fflush(NULL); execv("/proc/self/exe", g_argv); perror("[TW] restart"); }
#else
            (void)restart;
#endif
            exit(0);
        }
    }
    tw_dsp_vblank();                                     /* INT0, when the game has enabled the DSP IRQs */
    vblank_slices = 2;
    if (!tw_env_active()) {                            /* a trace oracle lands the interrupts itself */
        tw_hw_vblank();
        rr_deliver_irqs(tw_hw_irq_level);
    }
    if (rr_frame % 60 == 0) {
        char dbg[200]; tw_dsp_debug(dbg, sizeof dbg);
        fprintf(stderr, "[TW] %s\n", dbg);
        tw_snd_debug(dbg, sizeof dbg);
        fprintf(stderr, "[TW] %s\n", dbg);
    }
    if (rr_frame % 60 == 0)
        fprintf(stderr, "[TW] frame %u  traps %u  unmapped %u  romwrites %u  irqs %u/%u/%u/%u/%u/%u/%u  en %X sr %04X\n",
                rr_frame, rr_n_traps, g_tw.n_unmapped, g_tw.n_romwrite, rr_n_irq[1], rr_n_irq[2], rr_n_irq[3],
                rr_n_irq[4], rr_n_irq[5], rr_n_irq[6], rr_n_irq[7], g_hw.irq_enabled, (unsigned)rr_get_sr());
    if (rr_frame >= max_frames) {
        dump_state();
        tw_snd_close();
        tw_host_close();
        tw_env_report();
        fprintf(stderr, "[TW] stop at frame %u, %u traps\n", rr_frame, rr_n_traps);
        exit(rr_n_traps ? 3 : 0);
    }
}

int main(int argc, char **argv)
{
    g_argv = argv;
    eng_win_startup("tokyowar.log");                   /* Windows: the program's own folder, a log file, real pixel sizes */
    const char *rom_dir = "extracted";
    const char *rd_dir = NULL, *rd_out = NULL; int rd_frame = 0, frames_given = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc) { max_frames = (uint32_t)atoi(argv[++i]); frames_given = 1; }
        else if (!strcmp(argv[i], "--window")) { win_scale = (i + 1 < argc && atoi(argv[i + 1]) > 0) ? atoi(argv[++i]) : -1; }   /* -1: the saved size */
        else if (!strcmp(argv[i], "--fullscreen")) { win_full = 1; if (!win_scale) win_scale = -1; }
        else if (!strcmp(argv[i], "--shots") && i + 2 < argc) { shot_dir = argv[++i]; shot_every = atoi(argv[++i]); if (shot_every < 1) shot_every = 1; }
        else if (!strcmp(argv[i], "--render-dump") && i + 3 < argc) { rd_dir = argv[++i]; rd_frame = atoi(argv[++i]); rd_out = argv[++i]; }
        else if (!strcmp(argv[i], "--ppf") && i + 1 < argc) polls_per_frame = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--dump") && i + 1 < argc) dump_dir = argv[++i];
        else if (!strcmp(argv[i], "--dsp-steps") && i + 1 < argc) dsp_steps_per_frame = atol(argv[++i]);
        else if (!strcmp(argv[i], "--autoplay")) autoplay = 1;
        else if (!strcmp(argv[i], "--press") && i + 1 < argc) add_press(argv[++i]);
        else if (!strcmp(argv[i], "--pedal") && i + 1 < argc) pedal_from = atol(argv[++i]);
        else if (!strcmp(argv[i], "--clock") && i + 1 < argc) clock_addr = strtol(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--input-offset") && i + 1 < argc) input_offset = atol(argv[++i]);
        else rom_dir = argv[i];
    }
    /* First run: take the ROMs out of MAME's tokyowar.zip if the ROM folder is incomplete (src/tw_rom.c, the shared engine/romzip.c)
     * -- how the installed packages and the Windows build are set up; chips unzipped loose into roms/ work too. */
    if (tw_rom_missing(rom_dir) && !strcmp(rom_dir, "extracted") && !tw_rom_missing("roms")) rom_dir = "roms";
    if (tw_rom_missing(rom_dir)) {
        char err[512], *base = SDL_GetBasePath();
        if (!tw_rom_setup(rom_dir, base, err, sizeof err)) {
            fprintf(stderr, "Tokyo Wars needs its ROMs: %s\n", err);
            if (win_scale) {
                char msg[1024];
                snprintf(msg, sizeof msg, "Tokyo Wars needs its ROMs.\n\nPut tokyowar.zip (the MAME ROM set) in the \"roms\" "
                         "folder next to this program, then start it again.\n\n(%s)", err);
                SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Tokyo Wars", msg, NULL);
            }
            SDL_free(base);
            return 2;
        }
        SDL_free(base);
    }
    if (shot_dir) mkdir(shot_dir, 0755);                /* --shots DIR: make the folder */
    if (rd_dir) {                                       /* the renderer gate: MAME's state, no emulation */
        if (!tw_dsp_init(rom_dir) || !tw_video_init(rom_dir) || !tw_host_open_headless()) return 2;
        if (!tw_video_load_dump(rd_dir, rd_frame)) return 2;
        tw_video_prepare();
        tw_host_shot(rd_out);
        return 0;
    }
    if (!tw_load_program(rom_dir)) return 2;
    tw_hw_init(rom_dir);
    if (!tw_dsp_init(rom_dir)) return 2;
    if (!tw_snd_init(rom_dir)) fprintf(stderr, "[TW] no sound MCU: the game will read no inputs\n");
    tw_env_init(getenv("TW_ENV"));                      /* dev trace build only */
    if (win_scale || shot_dir) {                        /* video: a window, or offscreen for --shots */
        if (win_scale && !frames_given) max_frames = 0xFFFFFFFFu;
        video_on = tw_video_init(rom_dir) && (win_scale ? tw_host_open(win_scale, win_full) : tw_host_open_headless());
        if (!video_on) { fprintf(stderr, "[TW] no picture: video could not start\n"); if (win_scale) return 2; }
    }
    memset(R, 0, RR_REGSPACE);
    RS4(RR_REG_SP, rr_read(0, 4));                      /* reset SP from vector 0 */
    rr_set_sr(0x2700);                                  /* supervisor, IPL 7 */
    rr_budget = polls_per_frame / SLICES;
    fprintf(stderr, "[TW] reset: SP=%08X PC=%08X\n", (uint32_t)RG4(RR_REG_SP), rr_read(4, 4));
    rr_call_push(0xFFFFFFFEu);                          /* bottom of the shadow stack */
    L_4000();                                           /* entry_reset: never returns */
    fprintf(stderr, "[TW] entry_reset returned?!\n");
    return 1;
}
