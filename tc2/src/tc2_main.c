/*
 * tc2_main.c -- run Time Crisis 2's lifted program from a MAME state snapshot (tools/mame/snapshot.lua): the first milestone of the host.
 *
 *   tc2 SNAPSHOT_PREFIX [INSTRUCTIONS]      e.g.  build/tc2 work/snap_f1500 5000000
 *   tc2 reset [INSTRUCTIONS]                from POWER-ON: the boot monitor at the reset vector, no snapshot
 *
 * Loads boot.bin (tools/make_prog.py), SNAPSHOT.ram (16 MB) and SNAPSHOT.regs (MAME's register names, include/tc2_regmap.h), enters the lifted
 * code at the snapshot's PC and runs INSTRUCTIONS instructions (default 3,000,000 ~ one frame). The trace build (-DRR_TRACE) writes every
 * executed PC to $TC2_TRACE (default tc2_trace.txt), with "IRQ cause" / "ERET" markers, to compare with MAME's own trace from the same point.
 * Devices: src/tc2_ctl.c, tc2_chips.c, tc2_screen.c (ours); the rest replayed from MAME ($TC2_ENV). $TC2_IOLOG logs every device access.
 */
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include "lift_rt.h"
#include "tc2_lifted.h"
#include "tc2_host.h"
#include "tc2_regmap.h"

extern uint8_t tc2_ram[0x1000000], tc2_boot[0x400000];
void tc2_io_flush(void);

static uint64_t ins_total, ins_limit = 3000000;

static int load(const char *path, uint8_t *dst, size_t n)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "[TC2] cannot open %s\n", path); return 0; }
    size_t got = fread(dst, 1, n, f); fclose(f);
    if (got != n) { fprintf(stderr, "[TC2] %s: %zu of %zu bytes\n", path, got, n); return 0; }
    return 1;
}

static uint32_t load_regs(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "[TC2] cannot open %s\n", path); exit(1); }
    char name[64]; char hex[64]; uint32_t pc = 0; int n = 0;
    while (fscanf(f, "%63s %63s", name, hex) == 2) {
        const uint64_t v = strtoull(hex, NULL, 16);
        if (!strcmp(name, "PC")) { pc = (uint32_t)v; continue; }
        for (size_t i = 0; i < sizeof tc2_regmap / sizeof tc2_regmap[0]; i++)
            if (!strcmp(tc2_regmap[i].name, name)) {
                if (tc2_regmap[i].size == 8) RS8(tc2_regmap[i].off, v); else RS4(tc2_regmap[i].off, v);
                n++; break;
            }
    }
    fclose(f);
    fprintf(stderr, "[TC2] %d registers from %s, PC %08X\n", n, path, pc);
    return pc;
}

#ifdef RR_TRACE
static FILE *trace_f;
void rr_trace_ins(uint32_t pc) { if (trace_f) fprintf(trace_f, "%08X\n", pc); }
void tc2_trace_mark(const char *what, uint32_t v) { if (trace_f) fprintf(trace_f, "%s %08X\n", what, v); }
#else
void tc2_trace_mark(const char *what, uint32_t v) { (void)what; (void)v; }
#endif

void tc2_on_exit(void)
{
#ifdef RR_TRACE
    if (trace_f) fclose(trace_f);
#endif
    { void tc2_backup_save(int); tc2_backup_save(1); }
    tc2_io_flush();
    { void tc2_h8_report(void); tc2_h8_report(); }
    { extern unsigned long tc2_render_frames, tc2_render_models, tc2_render_immediate, tc2_c435_unknown;
      extern unsigned long tc2_render_polys; fprintf(stderr, "[3D] %lu frames: %lu model entries, %lu immediate polygons, %lu polygons drawn; %lu unknown C435 commands\n", tc2_render_frames, tc2_render_models, tc2_render_immediate, tc2_render_polys, tc2_c435_unknown); }
    extern unsigned long tc2_env_hits, tc2_env_miss;
    extern uint32_t rr_n_fpe;
    fprintf(stderr, "[TC2] %llu instructions, %u frames, %llu interrupts, %u traps, %u FP exceptions (FCSR %08X); replay: %lu device reads replayed, %lu not in the recording\n",
            (unsigned long long)ins_total, rr_frame, (unsigned long long)rr_n_irq, rr_n_traps, rr_n_fpe, (unsigned)RG4(0x1210), tc2_env_hits, tc2_env_miss);
}
void tc2_on_trap(void) { if (getenv("TC2_STOP_ON_TRAP")) { tc2_on_exit(); exit(3); } }

/* a frame is complete (the start of vertical blank): TC2_SHOT=path:frame writes the text layer as a PPM there */
void tc2_text_render(uint32_t *rgba);
/* $TC2_PRESS = "bits@frame+frames,..." (headless runs): cabinet inputs held for a stretch -- the bits of src/tc2_input.c (1 coin, 4 service,
 * 8 test, 0x10 trigger, 0x20 pedal, 0x40 / 0x80 operator up / down, 0x100 operator enter) */
static void scripted_inputs(uint32_t frame)
{
    static const char *spec; static int init;
    if (!init) { init = 1; spec = getenv("TC2_PRESS"); }
    if (!spec) return;
    extern uint16_t tc2_inputs;
    uint16_t held = 0;
    for (const char *p = spec; *p; ) {
        unsigned bits = 0, at = 0, len = 1; int n = 0;
        if (sscanf(p, "%x@%u+%u%n", &bits, &at, &len, &n) < 2) break;
        if (frame >= at && frame < at + len) held |= (uint16_t)bits;
        p += n ? n : 1; while (*p && *p != ',') p++; if (*p == ',') p++;
    }
    tc2_inputs = held;
}

/* THE GPU GATE: $TC2_GPU_GATE=dir:every -- a hidden GL context in a headless run; every N frames the same built frame is drawn by the GPU
 * back end (src/tc2_gl.c) and by the software one (the oracle), both written as PPMs, and the difference reported */
#include <SDL2/SDL.h>
#include "tc2_build.h"
#include "eng/win_gpu.h"   /* Windows: run on the discrete GPU of a two-GPU laptop */
static void ppm(const char *path, const uint32_t *px)
{
    FILE *f = fopen(path, "wb"); if (!f) return;
    fprintf(f, "P6\n640 480\n255\n");
    for (int i = 0; i < 640 * 480; i++) { const uint8_t c[3] = { (uint8_t)(px[i] >> 16), (uint8_t)(px[i] >> 8), (uint8_t)px[i] }; fwrite(c, 1, 3, f); }
    fclose(f);
}
static void gpu_gate(uint32_t frame)
{
    static int init; static char dir[512]; static unsigned every;
    if (!init) {
        init = 1; const char *e = getenv("TC2_GPU_GATE"); if (!e) return;
        snprintf(dir, sizeof dir, "%s", e); char *c = strrchr(dir, ':'); if (!c) return; *c = 0; every = (unsigned)atoi(c + 1);
        SDL_Init(SDL_INIT_VIDEO);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2); SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
        SDL_Window *w = SDL_CreateWindow("tc2 gate", 0, 0, 64, 64, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
        if (!w || !SDL_GL_CreateContext(w) || !tc2_gl_init()) { fprintf(stderr, "[GATE] no GPU renderer: %s\n", SDL_GetError()); every = 0; }
    }
    if (!every || frame % every) return;
    const tc2_built *b = tc2_built_latest_wait(); if (!b) return;
    static uint32_t s[640 * 480], g[640 * 480];
    tc2_soft_raster(b, s);
    tc2_gl_draw(b); tc2_gl_read(g);
    long sum = 0, big = 0, diff = 0; int mx = 0;
    for (int i = 0; i < 640 * 480; i++) {
        int d = 0;
        for (int sh = 0; sh <= 16; sh += 8) { const int x = abs((int)(s[i] >> sh & 0xFF) - (int)(g[i] >> sh & 0xFF)); sum += x; if (x > d) d = x; }
        if (d) diff++; if (d > 16) big++; if (d > mx) mx = d;
    }
    fprintf(stderr, "[GATE] frame %u: %d polygons; mean %.3f/255, %ld px differ, %ld px > 16 (%.3f%%), max %d\n", frame, b->npoly,
            sum / (640.0 * 480 * 3), diff, big, 100.0 * big / (640 * 480), mx);
    char p[700];
    snprintf(p, sizeof p, "%s/soft_%05u.ppm", dir, frame); ppm(p, s);
    snprintf(p, sizeof p, "%s/gpu_%05u.ppm", dir, frame); ppm(p, g);
    if (getenv("TC2_GATE_WIDEPOLYS")) {
        int st = 0, bl = 0, pv[8] = { 0 };
        for (int i = 0; i < b->npoly; i++) { st += b->polys[i].stencil; bl += b->polys[i].blend; pv[b->polys[i].prioverchar & 7]++; }
        fprintf(stderr, "[GATE]  %d stencil, %d blend, prioverchar 0:%d 2:%d 7:%d, %d entries\n", st, bl, pv[0], pv[2], pv[7], b->fs.list.count);
    }
    if (getenv("TC2_GATE_WIDEPOLYS"))                          /* polygons as wide as the 4:3 screen: what they are */
        for (int i = 0; i < b->npoly; i++) {
            const tc2_poly *q = &b->polys[b->order[i]];
            float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
            for (int k = 0; k < q->n; k++) { const float x = q->pv[k].x + (320 - q->vp_size_x) + q->vp_offset_x, y = q->pv[k].y + (240 - q->vp_size_y) - q->vp_offset_y;
                if (x < x0) x0 = x; if (x > x1) x1 = x; if (y < y0) y0 = y; if (y > y1) y1 = y; }
            if ((q->stencil || x1 - x0 > (getenv("TC2_GATE_WIDEMIN") ? atof(getenv("TC2_GATE_WIDEMIN")) : 600) && y0 > -50 && y1 < 530)) fprintf(stderr, "[GATE]  poly %d: n %d x %.1f..%.1f y %.1f..%.1f pal %04X cmode %d tbase %05X zkey %06X vp %d,%d %d,%d st %d uv0 %.1f,%.1f uv2 %.1f,%.1f\n", i, q->n, x0, x1, y0, y1,
                                       q->color_base, q->cmode, q->tbase, q->zkey, q->vp_size_x, q->vp_size_y, q->vp_offset_x, q->vp_offset_y, q->stencil, q->pv[0].p[1]/q->pv[0].p[0], q->pv[0].p[2]/q->pv[0].p[0], q->pv[2].p[1]/q->pv[2].p[0], q->pv[2].p[2]/q->pv[2].p[0]);
        }
    if (getenv("TC2_GATE_FOG")) {                              /* the fog inputs the game set: C404 fog colour and the C412 PCZ table */
        uint16_t *tc2_c412_pcz(void); const uint16_t *z = tc2_c412_pcz(); int nz = 0;
        for (int i = 0; i < 0x200; i++) nz += z[i] != 0;
        fprintf(stderr, "[GATE]  fog RGB %02X %02X %02X; PCZ %d/512 non-zero:", b->fs.c404[11], b->fs.c404[13], b->fs.c404[15], nz);
        for (int i = 0; i < 0x200; i += 32) fprintf(stderr, " %04X", z[i]);
        fprintf(stderr, "\n");
    }
    if (getenv("TC2_GATE_TEXTMASK")) {                         /* where the text layer is opaque: white */
        for (int i = 0; i < 640 * 480; i++) g[i] = (b->mix[i] & 0x8000) ? (b->fs.pens[b->mix[i] & 0x7FFF] | 0x404040) : 0x000040;
        snprintf(p, sizeof p, "%s/text_%05u.ppm", dir, frame); ppm(p, g);
    }
    if (tc2_gl_scale() > 1 || tc2_gl_wide()) {                                   /* and the GPU picture at its full internal resolution */
        static uint8_t full[(640 + 800) * 4 * 1920 * 3]; const int fh = 480 * tc2_gl_scale();
        const int w = tc2_gl_read_full(full, (int)sizeof full);
        snprintf(p, sizeof p, "%s/gpufull_%05u.ppm", dir, frame);
        FILE *f = w ? fopen(p, "wb") : NULL;
        if (f) { fprintf(f, "P6\n%d %d\n255\n", w, fh); fwrite(full, 1, (size_t)w * fh * 3, f); fclose(f); }
    }
}
/* THE SESSION RECORDER: a window session writes every change of the cabinet inputs (the buttons, the gun's x / y / off-screen) with its
 * frame number to tc2_last.rec (the previous session is kept as tc2_prev.rec); $TC2_REPLAY=<file> plays one back in a headless run --
 * the same inputs at the same frames, so a bug a player reached (a sub-CPU trap in stage 2...) is reproduced exactly. Lines:
 *   F <frame> <inputs hex> <gun x> <gun y> <off>       (the state from the end of that frame's vertical blank on) */
extern uint16_t tc2_inputs, g_ss22_gun_x, g_ss22_gun_y;
extern _Bool g_ss22_gun_off;
static void session_rec(uint32_t frame)
{
    static FILE *rec, *rep; static int init; static char line[128]; static int have;
    if (!init) {
        init = 1;
        const char *r = getenv("TC2_REPLAY");
        if (r) { rep = fopen(r, "r"); fprintf(stderr, rep ? "[REC] replaying %s\n" : "[REC] cannot read %s\n", r); }
        else { int tc2_window_active(void);
               if (tc2_window_active()) { rename("tc2_last.rec", "tc2_prev.rec"); rec = fopen("tc2_last.rec", "w");
                   if (rec) { fprintf(rec, "# TC2 session: F frame inputs gunx guny off\n"); fprintf(stderr, "[REC] recording the inputs to tc2_last.rec\n"); } } }
    }
    if (rep) {
        for (;;) {
            if (!have) { if (!fgets(line, sizeof line, rep)) { fclose(rep); rep = NULL; fprintf(stderr, "[REC] replay ends at frame %u\n", frame); return; } have = 1; }
            if (line[0] != 'F') { have = 0; continue; }
            unsigned f, in, gx, gy, off;
            if (sscanf(line, "F %u %x %u %u %u", &f, &in, &gx, &gy, &off) != 5) { have = 0; continue; }
            if (f > frame) return;
            tc2_inputs = (uint16_t)in; g_ss22_gun_x = (uint16_t)gx; g_ss22_gun_y = (uint16_t)gy; g_ss22_gun_off = off != 0;
            have = 0;
        }
    }
    if (rec) {
        static uint16_t pin = 0xFFFF, px, py; static int poff = -1;
        if (tc2_inputs != pin || g_ss22_gun_x != px || g_ss22_gun_y != py || (int)g_ss22_gun_off != poff) {
            fprintf(rec, "F %u %04X %u %u %d\n", frame, tc2_inputs, g_ss22_gun_x, g_ss22_gun_y, (int)g_ss22_gun_off);
            fflush(rec);                                   /* a crash or ^C keeps everything up to here */
            pin = tc2_inputs; px = g_ss22_gun_x; py = g_ss22_gun_y; poff = g_ss22_gun_off;
        }
    }
}
void tc2_on_vblank(uint32_t frame)
{
    scripted_inputs(frame);
    { static int tf = -2; static const char *tp; if (tf == -2) { const char *e = getenv("TC2_TEXTDUMP"); tf = -1; if (e) { tf = atoi(e); tp = strchr(e, ':'); if (tp) tp++; } }
      if (tf >= 0 && (int)frame == tf && tp) {   /* $TC2_TEXTDUMP=frame:file -- the C361 character RAM + tile map (0x06800000-0x0681FFFF), big-endian */
          uint8_t *tc2_vram_ptr(uint32_t); FILE *f = fopen(tp, "wb"); if (f) { fwrite(tc2_vram_ptr(0x06800000u), 1, 0x20000, f); fclose(f); } } }
    static int pclog = -1; if (pclog < 0) pclog = getenv("TC2_PCLOG") != NULL;
    if (pclog) fprintf(stderr, "[PC] frame %u pc %08X\n", frame, rr_pc);
    { void tc2_render_frame_end(void); tc2_render_frame_end(); }   /* the picture: background, text, the 3D render list (src/tc2_render.c) */
    { void tc2_out_poll(uint16_t); uint8_t tc2_h8_outputs(void); tc2_out_poll(tc2_h8_outputs()); }   /* the gun recoil out (src/tc2_out.c) */
    { void tc2_window_frame(void); tc2_window_frame(); }
    if (rr_frame % 300 == 0) { void tc2_backup_save(int); tc2_backup_save(0); }   /* the backup RAM to its file when it changed */
    session_rec(frame);                                     /* after the window set this frame's inputs: record them, or replay */
    gpu_gate(frame);
    static const char *shot; static unsigned long at; static int init;
    if (!init) { init = 1; const char *e = getenv("TC2_SHOT"); if (e) { static char path[512]; snprintf(path, sizeof path, "%s", e);
                 char *c = strrchr(path, ':'); if (c) { *c = 0; at = strtoul(c + 1, NULL, 0); shot = path; } } }
    { static const char *dir; static unsigned every; static int i2;          /* $TC2_SHOTS=dir:every -- a PPM every N frames */
      if (!i2) { i2 = 1; const char *e = getenv("TC2_SHOTS"); if (e) { static char d[512]; snprintf(d, sizeof d, "%s", e); char *c = strrchr(d, ':'); if (c) { *c = 0; every = (unsigned)atoi(c + 1); dir = d; } } }
      static const char *ldir; static char lst[4096]; static int i3;       /* $TC2_SHOTLIST=dir:f1,f2,a-b,... -- pictures at chosen frames */
      if (!i3) { i3 = 1; const char *e = getenv("TC2_SHOTLIST"); if (e) { static char d[512]; snprintf(d, sizeof d, "%s", e); char *c = strchr(d, ':'); if (c) { *c = 0; ldir = d; snprintf(lst, sizeof lst, ",%s,", c + 1); } } }
      int listed = 0;
      if (ldir) { for (const char *q = lst; *q; ) { unsigned lo = 0, hi = 0; int n = 0; if (sscanf(q, ",%u-%u%n", &lo, &hi, &n) == 2 || (sscanf(q, ",%u%n", &lo, &n) == 1 && (hi = lo, 1))) { if (frame >= lo && frame <= hi) listed = 1; q += n; } else q++; } }
      if (listed && !dir) dir = ldir;
      if (dir && ((every && frame % every == 0) || listed)) {
          static uint32_t px2[640 * 480]; { void tc2_frame_rgba_wait(uint32_t *); tc2_frame_rgba_wait(px2); }
          char p[700]; snprintf(p, sizeof p, "%s/f%05u.ppm", dir, frame);
          FILE *f = fopen(p, "wb"); if (f) { fprintf(f, "P6\n640 480\n255\n");
              for (int i = 0; i < 640 * 480; i++) { const uint8_t c[3] = { (uint8_t)(px2[i] >> 16), (uint8_t)(px2[i] >> 8), (uint8_t)px2[i] }; fwrite(c, 1, 3, f); } fclose(f); } } }
    if (!shot || frame != at) return;
    static uint32_t px[640 * 480];
    { void tc2_frame_rgba_wait(uint32_t *); tc2_frame_rgba_wait(px); }
    FILE *f = fopen(shot, "wb"); if (!f) return;
    fprintf(f, "P6\n640 480\n255\n");
    for (int i = 0; i < 640 * 480; i++) { const uint8_t c[3] = { (uint8_t)(px[i] >> 16), (uint8_t)(px[i] >> 8), (uint8_t)px[i] }; fwrite(c, 1, 3, f); }
    fclose(f);
    fprintf(stderr, "[TC2] frame %u -> %s\n", frame, shot);
}

/* time passed: the screen (vblank, the C361 raster line -- src/tc2_screen.c) and the instruction limit */
void tc2_screen_advance(uint32_t n);
uint32_t tc2_screen_until_event(void);
void tc2_host_step(uint32_t n)
{
    ins_total += n;
    tc2_screen_advance(n);
    { void tc2_h8_run(uint64_t); uint64_t tc2_cycles_now(void); tc2_h8_run(tc2_cycles_now()); }   /* the two H8s catch up (src/tc2_h8.c) */
    if (ins_total >= ins_limit) { tc2_on_exit(); exit(0); }
}
uint32_t tc2_until_event(void) { return tc2_screen_until_event(); }

int main(int argc, char **argv)
{
#ifdef _WIN32
    /* Windows: a double-click or a shortcut starts us anywhere -- work in the program's own folder (roms/, extracted/, tc2.cfg, the
     * backup RAM), and with no console the log goes to tc2.log beside the .exe (what the crash message points at) */
    { wchar_t wp[MAX_PATH]; DWORD n = GetModuleFileNameW(NULL, wp, MAX_PATH);
      if (n > 0 && n < MAX_PATH) { wchar_t *sl = wcsrchr(wp, L'\\'); if (sl) { *sl = 0; _wchdir(wp); } } }
    freopen("tc2.log", "w", stderr); setvbuf(stderr, NULL, _IONBF, 0);
#endif
    /* no arguments (a double-click, the player's launcher): the game in a window from power-on */
    static char *win_argv[] = { NULL, "--window", NULL };
    if (argc < 2) { win_argv[0] = argv[0]; argv = win_argv; argc = 2; }
    if (argc > 2 && strcmp(argv[1], "--window")) ins_limit = strtoull(argv[2], NULL, 0);
    char p[1024];
    /* the ROM folder: from the player's timecrs2.zip on the first run (src/tc2_rom.c), then the boot image made from it */
    {
        char exe_dir[1024] = ".";
        { char *bp = SDL_GetBasePath();                  /* the program's own folder, on every system (was /proc/self/exe) */
          if (bp) { snprintf(exe_dir, sizeof exe_dir, "%s", bp); size_t k = strlen(exe_dir); while (k > 1 && (exe_dir[k - 1] == '/' || exe_dir[k - 1] == '\\')) exe_dir[--k] = 0; SDL_free(bp); } }
        int tc2_rom_setup(const char *, const char *), tc2_rom_boot(const char *, uint8_t *);
        if (!tc2_rom_setup("extracted", exe_dir) || !tc2_rom_boot("extracted", tc2_boot)) {
            extern char tc2_rom_error[];                  /* a window run (a double-click): say it on screen, there may be no console */
            if (!strcmp(argv[1], "--window")) SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Time Crisis 2",
                tc2_rom_error[0] ? tc2_rom_error : "Time Crisis 2 could not read its ROM files in extracted/ (see the log).", NULL);
            return 1;
        }
    }
    { int tc2_load_data(const char *); if (!tc2_load_data("extracted")) return 1; }
    { void tc2_h8_init(const char *); tc2_h8_init("extracted"); }
    { void tc2_render_init(const char *); tc2_render_init("extracted"); }
    if (getenv("TC2_NO_RASTER")) { void tc2_render_soft(int); tc2_render_soft(0); }   /* profiling: the CPU work of a GPU run (no software fill) */
    /* POWER-ON ("reset" instead of a snapshot): the R4650's reset state as MAME's mips3 device_reset gives it -- PC at the reset vector
     * 0xBFC00000 (KSEG1, the boot ROM), Status = BEV | ERL, Compare 0xFFFFFFFF, Count 0, PRId 0x2200 (R4650), the FPU's FIR the same; main
     * RAM zero (MAME's RAM share starts zeroed). The boot monitor then copies the game's ELF to RAM and enters _start (frame 12 on MAME). */
    /* --window [scale] [--fullscreen]: in a window from power-on, at the board's speed (src/tc2_window.c); no instruction limit */
    if (!strcmp(argv[1], "--window")) {
        int scale = 0, fs = 0;
        for (int i = 2; i < argc; i++) { if (!strcmp(argv[i], "--fullscreen")) fs = 1; else scale = atoi(argv[i]); }
        int tc2_window_open(int, int);
        if (!tc2_window_open(scale, fs)) return 1;
        argv[1] = "reset"; ins_limit = UINT64_MAX;
    }
    const int power_on = !strcmp(argv[1], "reset");
    uint32_t pc;
    if (power_on) {
        memset(tc2_ram, 0, sizeof tc2_ram);
        pc = 0xBFC00000u;
        RS8(TC2_R_STATUS, 0x00400004u);                /* BEV | ERL */
        RS8(TC2_R_COMPARE, 0xFFFFFFFFu);
        RS8(TC2_R_COUNT, 0);
        RS8(0x2078, 0x2200);                          /* PRId */
        RS4(0x1200, 0x2200);                          /* FPU FIR */
        fprintf(stderr, "[TC2] power-on: PC BFC00000, Status 00400004\n");
        { void tc2_backup_load(int); int tc2_window_active(void); tc2_backup_load(tc2_window_active()); }
    } else {
        snprintf(p, sizeof p, "%s.ram", argv[1]); if (!load(p, tc2_ram, sizeof tc2_ram)) return 1;
        snprintf(p, sizeof p, "%s.regs", argv[1]);
        pc = load_regs(p);
        int tc2_vram_load(const char *);
        snprintf(p, sizeof p, "%s.vram", argv[1]);
        if (!tc2_vram_load(p)) fprintf(stderr, "[TC2] no %s: the video RAMs start empty (an older snapshot: re-take it with tools/mame/snapshot.lua)\n", p);
        int tc2_chips_load(const char *);
        snprintf(p, sizeof p, "%s.chips", argv[1]);
        if (!tc2_chips_load(p)) fprintf(stderr, "[TC2] no %s: the modelled chips start at reset (an older snapshot: re-take it)\n", p);
    }
    /* CP0 Config: MAME's debugger does not expose it, so it is MAME's own computation (mips3com.cpp compute_config_register for this board:
     * 0x00026030 | DC 8 KB (1 << 6) | IC 8 KB (1 << 9) | big-endian 0x8000, clock divider 2 -> 0) = 0x0002E270 -- 32-byte I and D lines,
     * as the RC4650 datasheet gives. The game's only Config read (0x80000470, the cache flush at 0x800004B0) uses bit 4 (DB) alone; the
     * earlier 0x8010 agreed on that bit (measured: the loop steps 32 bytes on MAME, 6 / 52 / 27 iterations against 12 / 102 / 53 with 16) */
    RS8(0x2080, 0x0002E270);
#ifdef RR_TRACE
    { const char *t = getenv("TC2_TRACE"); trace_f = fopen(t ? t : "tc2_trace.txt", "w"); }
#endif
    rr_budget = TC2_SLICE;
    rr_pc = pc;
    /* the snapshot is taken at the end of a frame (MAME's frame_done): vertical blank is asserted then, and MAME's own trace from that
     * point starts in the exception vector -- so the program resumes the way the CPU does, by taking it first */
    { void tc2_screen_reset(void), tc2_screen_at_vblank(void); if (power_on) tc2_screen_reset(); else tc2_screen_at_vblank(); }
    if (!power_on) {
        void tc2_irq_line(uint32_t, int); tc2_irq_line(TC2_IP_VBLANK, 1);
        tc2_poll_irq();
    }
    rr_jump(pc, pc);
    fprintf(stderr, "[TC2] the lifted program returned to the host at %08X\n", rr_pc);
    tc2_on_exit();
    return 0;
}
