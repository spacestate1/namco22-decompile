/*
 * ss22_run.c -- the host for a lifted Super 22 game: the scheduler and the command line, the same for Tokyo Wars and Dirt Dash (each had
 * its own copy, line for line but for the names). A game is an ss22_game (engine/ss22_game.h); its program is gen/<game>_lifted.c and
 * entry_reset never returns, it runs the game's own main loop. The register file, shadow return stack, traps, interrupt entry and trace
 * hook are the shared engine/lift_cpu.c; this file is the scheduler: time advances in rr_tick(), which the lifted code polls on every
 * call and backward branch, and every SLICES slices is one video frame (vblank IRQ).
 *
 *   <game> <rom_dir> [--frames N] [--ppf N] [--dump DIR [--dump-every N]]
 *   <game> <rom_dir> --window [N] [--fullscreen]          play: OpenGL window (Nx the 640x480 picture), sound card, keyboard and pads
 *   <game> <rom_dir> --window [N] --stage NAME             ... starting at that stage: the game's start script plays coins, stage and car, then the player has the cabinet
 *   <game>                                                (no arguments: a double-click) the same window
 *   <game> <rom_dir> --shots DIR N [--frames M]           headless: a 640x480 PPM every N frames (offscreen OpenGL)
 *   <game> <rom_dir> --render-dump DIR FRAME OUT.ppm      MAME's captured video state (tools/mame/vid_dump.lua) through the engine
 * Trace build (<game>_trace): RR_TRACE_FILE / RR_TRACE_MAX (engine/lift_cpu.c) write the trace that tools/namco22/diff_trace_mame.py lines
 * up against tools/mame/regtrace.lua; <TAG>_ENV=<file> replays the DSP / MCU / interrupt environment MAME saw
 * (tools/mame/env_from_trace.py, engine/ss22_env.c).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ss22_game.h"
#include "ss22_host.h"
#include <sys/stat.h>
#include "win_startup.h"
#include "romzip.h"
#include <SDL2/SDL.h>
#ifndef _WIN32
#include <unistd.h>
#endif
#include "lift_rt.h"
#include "lift_cpu.h"

const ss22_game *g_ss22_game;
static char **g_argv;

static int32_t  polls_per_frame;           /* 68K instructions per frame: the game's (MAME's average over its first 300 frames) */
static uint32_t max_frames = 600;
static const char *dump_dir;
static uint32_t dump_every, dump_wram_only;          /* --dump-every N: work RAM alone, every N frames (finding a game's mode word) */
static int  win_scale, win_full, shot_every;       /* --window, --fullscreen, --shots */
static const char *shot_dir;
static bool video_on;

#define SLICES 16
static uint32_t slice;
static int vblank_slices;
static long dsp_steps_per_frame = 166667;  /* the C71 at 40 MHz retires 10 M instructions/s: 1/59.9 s */
static long serial_acc;                    /* MAME's dsp_serial timer: 100 Hz */

void rd_budget_out(void);

#ifdef RR_TRACE
/* <tag>_PCRING=<frame>: at that frame, the 68K's hottest PCs over its last 8192 instructions (where is it stuck?). Trace build only. */
static uint32_t pc_ring[8192]; static unsigned pc_ring_n; static long pc_ring_frame = -1;
extern uint32_t g_ss22_last_pc;                            /* engine/ss22_board.c: the last instruction the trace hook saw */
static void pc_ring_hook(uint32_t pc) { g_ss22_last_pc = pc; pc_ring[pc_ring_n++ & 8191] = pc; }
static void pc_ring_report(void)
{
    uint32_t best[8] = {0}; unsigned cnt[8] = {0};
    for (unsigned i = 0; i < 8192; i++) {
        uint32_t pc = pc_ring[i]; unsigned c = 0;
        for (unsigned j = 0; j < 8192; j++) c += pc_ring[j] == pc;
        int k = -1; for (int q = 0; q < 8; q++) if (best[q] == pc) { k = q; break; }
        if (k >= 0) continue;
        for (int q = 0; q < 8; q++) if (c > cnt[q]) { for (int r = 7; r > q; r--) { best[r] = best[r - 1]; cnt[r] = cnt[r - 1]; } best[q] = pc; cnt[q] = c; break; }
    }
    fprintf(stderr, "[%s] frame %u, hottest PCs of the last 8192 instructions:", g_ss22_game->tag, rr_frame);
    for (int q = 0; q < 8 && cnt[q]; q++) fprintf(stderr, " %06X x%u", best[q], cnt[q]);
    fprintf(stderr, "\n");
}
#endif

/* ---- the cabinet, scripted (headless): what the MCU's ports and A-D channels read ----------------------
 *   --autoplay            the pattern tools/mame/cov_trace_play.lua feeds MAME (the game's script)
 *   --press NAME@F[+LEN]  hold one of the game's buttons (its ss22_press_name table) from frame F for LEN frames (default 6)
 *   --pedal F             the first pedal down from frame F                                                     */
typedef struct { uint16_t bit; long from, len; } press_t;
static press_t presses[32]; static int npress;
static int autoplay; static long pedal_from = -1, input_offset, clock_addr;      /* --clock ADDR: run the script on the game's own frame counter (work RAM) instead of video frames */      /* --input-offset N: the script runs N frames late (our boot lags MAME's) */
static void add_press(const char *spec)
{
    char n[16]; long f, len = 6;
    if (npress >= 32 || sscanf(spec, "%15[a-z]@%ld+%ld", n, &f, &len) < 2) { fprintf(stderr, "[%s] bad --press %s\n", g_ss22_game->tag, spec); return; }
    for (int i = 0; i < g_ss22_game->n_presses; i++)
        if (!strcmp(n, g_ss22_game->presses[i].name)) { presses[npress++] = (press_t){ g_ss22_game->presses[i].bit, f, len }; return; }
    fprintf(stderr, "[%s] unknown button %s\n", g_ss22_game->tag, n);
}
static const char *start_name;                       /* --stage NAME (the game's start script, ss22_game.start) */
static bool start_on;                                /* the start script owns the cabinet this frame: the window's keys stay out of it */
static void update_inputs(void)
{
    if (start_name && g_ss22_game->start) {
        uint16_t p = 0; unsigned wheel = 0x200, pedal1 = 0, pedal2 = 0;
        start_on = g_ss22_game->start(start_name, (long)rr_frame, &p, &wheel, &pedal1, &pedal2);
        if (start_on) { ss22_snd_inputs(p, wheel, pedal1, pedal2); return; }
    }
    if (ss22_host_active() && !autoplay && !npress && pedal_from < 0) return;      /* the window's keys drive the cabinet (ss22_host_frame) */
    long n = (long)rr_frame - input_offset;
    if (clock_addr) n = (long)rr_read((uint32_t)clock_addr, 4);
    uint16_t p = 0; unsigned wheel = 0x200, pedal1 = 0, pedal2 = 0;
    for (int i = 0; i < npress; i++) if (n >= presses[i].from && n < presses[i].from + presses[i].len) p |= presses[i].bit;
    if (pedal_from >= 0 && n >= pedal_from) pedal1 = g_ss22_game->pedal_full[0];
    if (autoplay) g_ss22_game->autoplay(n, &p, &wheel, &pedal1, &pedal2);
    ss22_snd_inputs(p, wheel, pedal1, pedal2);
}

/* --sndsweep HOLD[:PASS[:SLOT_LO:SLOT_HI[:CMD_MAX[:FROM]]]]: a TEST harness for the sound driver's coverage (Prop Cycle's PROPCYCL_SNDSWEEP, at the
 * mailbox instead of the game's sound table). After frame FROM (default 1000) it writes every command word 0x4000|c and 0xC000|c, c = 0..CMD_MAX (default
 * 0x7F), into every slot SLOT_LO..SLOT_HI (default 0..31), each for HOLD frames and then clears the slot. PASS 0 leaves the parameter words
 * alone; PASS 1-4 first fill every parameter word (shared RAM 0x100..0x17E) with 0x0000 / 0x4000 / 0x8000 / 0xFFFF. Run on the game's oracle build with
 * SND_COV=<file> to record the addresses every command reaches: the translated driver (gen/<game>_snd_driver.c) contains only code the oracle ran. */
static struct { int on, hold, pass, slot_lo, slot_hi, cmd_max, from; } sweep;
static void sweep_arg(const char *a)
{
    sweep.on = 1; sweep.hold = 6; sweep.pass = 0; sweep.slot_lo = 0; sweep.slot_hi = 31; sweep.cmd_max = 0x7F; sweep.from = 1000;
    sscanf(a, "%d:%d:%d:%d:%i:%d", &sweep.hold, &sweep.pass, &sweep.slot_lo, &sweep.slot_hi, &sweep.cmd_max, &sweep.from);
    if (sweep.hold < 1) sweep.hold = 1;
}
static void sweep_put16(uint32_t off, uint16_t v) { g_ss22.shared[off] = (uint8_t)(v >> 8); g_ss22.shared[off + 1] = (uint8_t)v; }
static uint32_t sweep_rnd(uint64_t *s) { *s ^= *s << 13; *s ^= *s >> 7; *s ^= *s << 17; return (uint32_t)(*s >> 16); }
static void sweep_tick(void)
{
    static const uint16_t par[5] = { 0, 0x0000, 0x4000, 0x8000, 0xFFFF };
    static long prev_slot = -1;
    if (!sweep.on || rr_frame < (uint32_t)sweep.from || (rr_frame - (uint32_t)sweep.from) % (uint32_t)sweep.hold) return;
    if (sweep.pass >= 5) {                               /* PASS >= 5: random commands and parameters (xorshift seeded by PASS), a few slots at a time */
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
    const long n = (long)((rr_frame - (uint32_t)sweep.from) / (uint32_t)sweep.hold);
    if (prev_slot >= 0) sweep_put16((uint32_t)prev_slot * 2, 0);
    if (n >= nslots * per_slot) {
        if (n == nslots * per_slot) fprintf(stderr, "[SNDSWEEP] done at frame %u\n", rr_frame);
        prev_slot = -1; return;
    }
    const long slot = sweep.slot_lo + n / per_slot, k = n % per_slot;
    const unsigned cmd = (unsigned)(k >> 1), flags = (k & 1) ? 0xC000u : 0x4000u;
    if (sweep.pass > 0) for (uint32_t o = 0x100; o < 0x180; o += 2) sweep_put16(o, par[sweep.pass]);
    sweep_put16((uint32_t)slot * 2, (uint16_t)(flags | cmd));
    prev_slot = slot;
}

static void dump_state(void)
{
    if (!dump_dir) return;
    char p[1024];
    snprintf(p, sizeof p, "%s/wram_f%u.bin", dump_dir, rr_frame);
    FILE *f = fopen(p, "wb");
    if (f) { fwrite(g_ss22.wram, 1, SS22_WRAM_SIZE, f); fclose(f); }
    snprintf(p, sizeof p, "%s/vregs_f%u.bin", dump_dir, rr_frame);           /* the video registers a game sets per kind of screen */
    f = fopen(p, "wb");
    if (f) {
        fwrite(g_ss22.mixer, 1, SS22_MIXER_SIZE, f);
        fwrite(g_ss22.vics_ctl, 1, sizeof g_ss22.vics_ctl, f);
        fwrite(g_ss22.tilemapattr, 1, sizeof g_ss22.tilemapattr, f);
        fwrite(g_ss22.czattr, 1, sizeof g_ss22.czattr, f);
        fwrite(g_ss22.syscon, 1, sizeof g_ss22.syscon, f);
        fwrite(g_ss22.text, 1, SS22_TEXT_SIZE, f);
        fclose(f);
    }
    if (dump_wram_only) return;
    snprintf(p, sizeof p, "%s/shared_f%u.bin", dump_dir, rr_frame);
    f = fopen(p, "wb");
    if (f) { fwrite(g_ss22.shared, 1, SS22_SHARED_SIZE, f); fclose(f); }
    snprintf(p, sizeof p, "%s/poly_f%u.bin", dump_dir, rr_frame);
    f = fopen(p, "wb");
    if (f) {
        for (uint32_t i = 0; i < SS22_POLY_WORDS; i++) {
            uint32_t w = g_ss22.poly[i] | 0xFF000000u;           /* what the 68K reads */
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
    g_ss22_in_vblank = vblank_slices > 0;
    if (vblank_slices > 0) {                            /* VTOTAL 525, VBSTART 480: the vblank is the first 45/525 of a frame after the */
        long full = dsp_steps_per_frame / SLICES;       /* screen update, ~1.37 of our slices; split the slice at VBEND. The master answers */
        long vb = dsp_steps_per_frame * 45 / 525 - (long)(2 - vblank_slices) * full;   /* the vblank INT0 inside it, and its PDP begin then */
        if (vb > full) vb = full;                       /* marks the NEXT update's list current (engine/frame_rule.h) */
        if (vb < 0) vb = 0;
        ss22_dsp_run(vb);
        g_ss22_in_vblank = false;
        ss22_dsp_run(full - vb);
        vblank_slices--;
    } else
        ss22_dsp_run(dsp_steps_per_frame / SLICES);       /* the master DSP runs beside the 68K */
    ss22_snd_slice();                                     /* and so does the sound MCU (ports, A-D, the C352) */
    if ((serial_acc += 100) >= 60 * SLICES) { serial_acc -= 60 * SLICES; ss22_dsp_serial(); }
    if (++slice % SLICES) return;
    rr_frame++;
#ifdef RR_TRACE
    if (pc_ring_frame >= 0 && (rr_frame == (uint32_t)pc_ring_frame || (pc_ring_frame > 0 && rr_frame > (uint32_t)pc_ring_frame && rr_frame % 300 == 0))) pc_ring_report();
#endif
    update_inputs();
    if (rr_frame % 120 == 0) ss22_eeprom_save();
    if (dump_every && rr_frame % dump_every == 0) dump_state();
    sweep_tick();
    if (video_on) {
        ss22_video_prepare();                              /* the master's finished list, before this vblank starts the next */
        if (shot_dir && rr_frame % shot_every == 0) {
            char p[1024]; snprintf(p, sizeof p, "%s/%s_f%u.ppm", shot_dir, g_ss22_game->lname, rr_frame);
            ss22_host_shot(p);
        }
        if (ss22_host_active() && !ss22_host_frame()) {
            const bool restart = ss22_host_restart_requested();       /* File > Restart: power-cycle the cabinet */
            ss22_eeprom_save(); ss22_snd_close(); ss22_host_close(); fprintf(stderr, "[%s] quit at frame %u\n", g_ss22_game->tag, rr_frame);
#ifndef _WIN32
            if (restart) { fflush(NULL); execv("/proc/self/exe", g_argv); perror("restart"); }
#else
            (void)restart;
#endif
            exit(0);
        }
    }
    ss22_dsp_vblank();                                     /* INT0, when the game has enabled the DSP IRQs */
    vblank_slices = 2;
    if (!ss22_env_active()) {                            /* a trace oracle lands the interrupts itself */
        ss22_hw_vblank();
        rr_deliver_irqs(ss22_hw_irq_level);
    }
    if (rr_frame % 60 == 0) {
        char dbg[200]; ss22_dsp_debug(dbg, sizeof dbg);
        fprintf(stderr, "[%s] %s\n", g_ss22_game->tag, dbg);
        ss22_snd_debug(dbg, sizeof dbg);
        fprintf(stderr, "[%s] %s\n", g_ss22_game->tag, dbg);
    }
    if (rr_frame % 60 == 0)
        fprintf(stderr, "[%s] frame %u  traps %u  unmapped %u  romwrites %u  irqs %u/%u/%u/%u/%u/%u/%u  en %X sr %04X\n",
                g_ss22_game->tag, rr_frame, rr_n_traps, g_ss22.n_unmapped, g_ss22.n_romwrite, rr_n_irq[1], rr_n_irq[2], rr_n_irq[3],
                rr_n_irq[4], rr_n_irq[5], rr_n_irq[6], rr_n_irq[7], g_hw.irq_enabled, (unsigned)rr_get_sr());
    if (rr_frame >= max_frames) {
        dump_state();
        ss22_eeprom_save();
        ss22_snd_close();
        ss22_host_close();
        ss22_env_report();
        fprintf(stderr, "[%s] stop at frame %u, %u traps\n", g_ss22_game->tag, rr_frame, rr_n_traps);
        exit(rr_n_traps ? 3 : 0);
    }
}

/* the window's host: the game's name, settings file and cabinet controls (engine/ss22_host.c) */
static void in_init(void) { ss22_input_init(g_ss22_game->input); }
static void in_update(void) { if (!start_on) ss22_input_update(); }      /* the start script has the cabinet until it is over */
static ss22_host_game host_game;

int ss22_main(int argc, char **argv, const ss22_game *g)
{
    g_ss22_game = g;
    g_argv = argv;
    polls_per_frame = g->polls_per_frame;
    char envname[64], cfgfile[64], nvfile[64];
    snprintf(envname, sizeof envname, "%s_ENV", g->tag);
#ifdef RR_TRACE
    { extern void (*rr_trace_hook)(uint32_t); char pr[64]; snprintf(pr, sizeof pr, "%s_PCRING", g->tag); const char *e = getenv(pr);
      if (e && !getenv(envname)) { pc_ring_frame = atol(e); rr_trace_hook = pc_ring_hook; } }
#endif
    eng_win_startup(g->logname);                        /* Windows: the program's own folder, a log file, real pixel sizes */
    snprintf(cfgfile, sizeof cfgfile, "%s_controls.cfg", g->lname);
    snprintf(nvfile, sizeof nvfile, "%s_eeprom.nv", g->lname);
    host_game = (ss22_host_game){ g->name, cfgfile, g->tag, g->lname, in_init, ss22_input_page, ss22_input_event, in_update,
                                  ss22_input_neutral, ss22_snd_set_output };
    const char *rom_dir = "extracted";
    const char *rd_dir = NULL, *rd_out = NULL; int rd_frame = 0, frames_given = 0;
    if (argc == 1) win_scale = -1;                      /* started with no arguments (a double-click, the Windows how-to): play, in a window */
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc) { max_frames = (uint32_t)atoi(argv[++i]); frames_given = 1; }
        else if (!strcmp(argv[i], "--window")) { win_scale = (i + 1 < argc && atoi(argv[i + 1]) > 0) ? atoi(argv[++i]) : -1; }   /* -1: the saved size */
        else if (!strcmp(argv[i], "--fullscreen")) { win_full = 1; if (!win_scale) win_scale = -1; }
        else if (!strcmp(argv[i], "--shots") && i + 2 < argc) { shot_dir = argv[++i]; shot_every = atoi(argv[++i]); if (shot_every < 1) shot_every = 1; }
        else if (!strcmp(argv[i], "--render-dump") && i + 3 < argc) { rd_dir = argv[++i]; rd_frame = atoi(argv[++i]); rd_out = argv[++i]; }
        else if (!strcmp(argv[i], "--ppf") && i + 1 < argc) polls_per_frame = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--dump") && i + 1 < argc) dump_dir = argv[++i];
        else if (!strcmp(argv[i], "--dump-every") && i + 1 < argc) { dump_every = (uint32_t)atoi(argv[++i]); dump_wram_only = 1; }
        else if (!strcmp(argv[i], "--dsp-steps") && i + 1 < argc) dsp_steps_per_frame = atol(argv[++i]);
        else if (!strcmp(argv[i], "--autoplay")) autoplay = 1;
        else if (!strcmp(argv[i], "--stage") && i + 1 < argc) {           /* the game's start script: coins, this stage, the car; then the player */
            start_name = argv[++i];
            if (!g->start) { fprintf(stderr, "[%s] %s has no --stage\n", g->tag, g->name); return 2; }
            if (!g->start(start_name, -1, &(uint16_t){ 0 }, &(unsigned){ 0 }, &(unsigned){ 0 }, &(unsigned){ 0 })) {       /* frame -1 only checks the name */
                fprintf(stderr, "[%s] --stage %s: not one of %s\n", g->tag, start_name, g->start_names ? g->start_names : "(none)"); return 2; }
        }
        else if (!strcmp(argv[i], "--sndsweep") && i + 1 < argc) sweep_arg(argv[++i]);
        else if (!strcmp(argv[i], "--press") && i + 1 < argc) add_press(argv[++i]);
        else if (!strcmp(argv[i], "--pedal") && i + 1 < argc) pedal_from = atol(argv[++i]);
        else if (!strcmp(argv[i], "--clock") && i + 1 < argc) clock_addr = strtol(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--input-offset") && i + 1 < argc) input_offset = atol(argv[++i]);
        else rom_dir = argv[i];
    }
    /* First run: take the ROMs out of MAME's zip if the ROM folder is incomplete (the shared engine/romzip.c) -- how the installed
     * packages and the Windows build are set up; chips unzipped loose into roms/ work too. */
    if (eng_romzip_missing(rom_dir, g->roms, g->n_roms) && !strcmp(rom_dir, "extracted") && !eng_romzip_missing("roms", g->roms, g->n_roms)) rom_dir = "roms";
    if (eng_romzip_missing(rom_dir, g->roms, g->n_roms)) {
        char err[512], *base = SDL_GetBasePath();
        const char *zips[] = { g->zip, "namcoc71.zip" };      /* namcoc71.zip: c71.bin, the DSP BIOS, in the MAME sets whose game zip does not carry it */
        if (!eng_romzip_autosetup(rom_dir, base, zips, 2, g->roms, g->n_roms, err, sizeof err)) {
            fprintf(stderr, "%s needs its ROMs: %s\n", g->name, err);
            if (win_scale) {
                char msg[1024];
                snprintf(msg, sizeof msg, "%s needs its ROMs.\n\nPut %s (the MAME ROM set) in the \"roms\" "
                         "folder next to this program, then start it again. (If it says c71.bin is missing, put MAME's namcoc71.zip there too.)\n\n(%s)", g->name, g->zip, err);
                SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, g->name, msg, NULL);
            }
            SDL_free(base);
            return 2;
        }
        SDL_free(base);
    }
    if (shot_dir) mkdir(shot_dir, 0755);                /* --shots DIR: make the folder */
    if (rd_dir) {                                       /* the renderer gate: MAME's state, no emulation */
        ss22_board_use(g->board);
        if (!ss22_dsp_init(rom_dir) || !ss22_video_init(rom_dir) || !ss22_host_open_headless()) return 2;
        if (!ss22_video_load_dump(rd_dir, rd_frame)) return 2;
        ss22_video_prepare();
        ss22_host_shot(rd_out);
        return 0;
    }
    ss22_board_use(g->board);
    if (!ss22_load_program(rom_dir)) return 2;
    ss22_hw_init(rom_dir);
    if (win_scale) ss22_eeprom_persist(nvfile);         /* a player's session keeps its options and records; headless runs (the gates) never do */
    if (!ss22_dsp_init(rom_dir)) return 2;
    if (!ss22_snd_init(rom_dir)) fprintf(stderr, "[%s] no sound MCU: the game will read no inputs\n", g->tag);
    ss22_env_init(getenv(envname));                     /* dev trace build only */
    if (win_scale || shot_dir) {                        /* video: a window, or offscreen for --shots */
        if (win_scale && !frames_given) max_frames = 0xFFFFFFFFu;
        video_on = ss22_video_init(rom_dir) && (win_scale ? ss22_host_open(&host_game, win_scale, win_full) : ss22_host_open_headless());
        if (!video_on) { fprintf(stderr, "[%s] no picture: video could not start\n", g->tag); if (win_scale) return 2; }
    }
    memset(R, 0, RR_REGSPACE);
    RS4(RR_REG_SP, rr_read(0, 4));                      /* reset SP from vector 0 */
    rr_set_sr(0x2700);                                  /* supervisor, IPL 7 */
    rr_budget = polls_per_frame / SLICES;
    fprintf(stderr, "[%s] reset: SP=%08X PC=%08X\n", g->tag, (uint32_t)RG4(RR_REG_SP), rr_read(4, 4));
    rr_call_push(0xFFFFFFFEu);                          /* bottom of the shadow stack */
    g->entry();                                         /* entry_reset: never returns */
    fprintf(stderr, "[%s] entry_reset returned?!\n", g->tag);
    return 1;
}

/* the readable-C (Phase B) hooks the generated code and engine/lift_cpu.c call, switched off: these games have no readable functions yet
 * (Rave Racer's src/rd/rd_core.c replaces this when one does) */
int rd_on, rd_stop_on, rd_poll_rec, rd_quiet, rd_cov_on;
int rd_hook(uint32_t ep) { (void)ep; return 0; }
int rd_jump_stop(uint32_t t, uint32_t at) { (void)t; (void)at; return 0; }
void rd_poll_snap(void) {}
void rd_budget_out(void) {}
void rd_cov_ins(uint32_t pc) { (void)pc; }
