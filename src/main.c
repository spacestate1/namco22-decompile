/*
 * Prop Cycle - Main entry point
 * SDL2 + OpenGL window, frame loop
 *
 * Usage: ./propcycl [rom_dir]
 *        ./propcycl [rom_dir] --screenshot [file.ppm] [frames]
 */
#include "propcycl.h"
#include "vaddr.h"
#ifndef W
#define W _W
#endif
#include "fog_hw.h"
#include <time.h>
static double perf_now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec*1e-9;}
double g_perf_game, g_perf_render;
#include "sprite_hw.h"
#include "text_hw.h"
#include "ui_menu.h"
#include "render_target.h"
#include "rom_zip.h"
#include <signal.h>
#ifdef _WIN32
#include <windows.h>
#include <process.h>
#include <io.h>
#else
#include <unistd.h>
#endif
#include "geo_hw.h"
#include "screenshot.h"
#include "audio.h"
#include "audio_hle.h"
int  framedump_load(const char *path);
static int g_geodump_n;

/* Golden stream v2, byte-for-byte the layout pc_geo_fixed.py emit_stream2
 * writes:
 *   zsort(24) flags(24) color(24) cmode(4) bank(4) cztype(2) czval(13) nv(3)
 *   then nv x [sx16 sy16 vz u_f v_f b_f] as 32-bit two's complement.
 * Quads with fewer than 3 clipped verts are skipped, matching the oracle. */
static void geodump_cb(const geo_quad *q, void *user)
{
    FILE *f = (FILE *)user;
    if (q->nrv < 3) return;
    fprintf(f, "%06X %06X %06X %01X %01X %01X %04X %01X",
            (unsigned)(q->zsort & 0xffffff), (unsigned)(q->flags_raw & 0xffffff),
            (unsigned)(q->color & 0xffffff), q->cmode & 0xf, q->texbank & 0xf,
            q->cz_type & 0xf, (unsigned)(q->cz_value & 0xffff), q->nrv & 0xf);
    for (int i = 0; i < q->nrv; i++)
        fprintf(f, " %08X %08X %08X %08X %08X %08X",
                (unsigned)q->rv[i].sx16, (unsigned)q->rv[i].sy16,
                (unsigned)q->rv[i].z,    (unsigned)q->rv[i].uf,
                (unsigned)q->rv[i].vf,   (unsigned)q->rv[i].bf);
    fputc('\n', f);
    g_geodump_n++;
}
void framedump_render(geo_quad_cb cb, void *user);
#include "trace.h"
#include <SDL2/SDL.h>
#include <GL/gl.h>
#include <stdlib.h>
#include <signal.h>

static SDL_Window* window;
static SDL_GLContext glctx;
static bool running = true;

/* Screenshot mode */
static bool headless = false;
static bool no_audio = false;   /* --noaudio; see AUDIO_PLAN.md phase 1 */

/* Live-capture directory for the F12 key.  Read once at startup and never
 * from the frame loop -- see the mismatch register row 40: getenv() inside
 * the loop faults on a truncated environment pointer. */
static char shot_dir[256] = "screenshots";
static bool shot_pending = false;   /* F12 armed a capture */
static int  shot_every = 0;         /* PROPCYCL_SHOT_EVERY=N */
FILE *g_sndcmd_f;                   /* PROPCYCL_SNDCMDLOG=<path> */
static const char* screenshot_path = "screenshot.ppm";
static int screenshot_frame = 10;  /* capture after N frames */

static FILE *g_flight_fp = NULL;
static void flight_log(void)
{
    if (!g_flight_fp) return;
    extern intptr_t _W[];
    #define W32(i) ((long)(int32_t)_W[(i)])
    fprintf(g_flight_fp, "F %u st=%ld sub=%ld course=%ld p=%ld,%ld,%ld cam=%ld,%ld,%ld floor=%ld"
            " c1464=%ld c1964=%ld yaw12EC=%ld spd=%ld pitch=%ld hdg=%ld cell=%ld gx=%ld gz=%ld layers=%ld e10=%ld e38=%ld z2918=%ld z2908=%ld ped2C04=%ld zones=%ld h1324=%ld s12D4=%ld s12D0=%ld s12E4=%ld s12DC=%ld score=%ld tgt=%ld\n",
            g_sys.frame_count, W32(0x0CBC), W32(0x0CC0), W32(0x0E0C),
            W32(0x0D00), W32(0x0D04), W32(0x0D08), W32(0x0CDC), W32(0x0CE0), W32(0x0CE4),
            W32(0x1328), W32(0x1464), W32(0x1964), W32(0x12EC), W32(0x0D48), W32(0x0D0C), W32(0x0D10),
            W32(0x0D24), W32(0x0D28), W32(0x0D2C), W32(0x0964), W32(0x0E10), W32(0x0E38),
            (long)(uint32_t)_W[0x2918], (long)(uint32_t)_W[0x2908], W32(0x2C04), W32(0x291C),
            W32(0x1324), W32(0x12D4), W32(0x12D0), W32(0x12E4), W32(0x12DC), W32(0x0E4C), W32(0x15FE2));
    /* terrain placements: mode-0x8000 data entries [model, x, y, z] in the live buffer */
    uintptr_t wp = (uintptr_t)_W[0x0CA4], base = (uintptr_t)g_sys.dspram;
    if (wp > base && wp < base + DSPRAM_SIZE) {
        uint32_t off = (uint32_t)(wp - base); uint32_t b0 = (off >= 0x18400) ? 0x18400 : 0x10400;
        int n = (int)((off - b0) / 4), mode = 0, i = 0, k;
        while (i < n) {
            int32_t w; memcpy(&w, g_sys.dspram + b0 + (size_t)i * 4, 4);
            if (w == 0x8000 || w == 0x8001 || w == 0x8002) { mode = w; i += 2; continue; }
            if (w == 0x8008) { for (k = 10; k <= 14 && i + k < n; k++) { int32_t t; memcpy(&t, g_sys.dspram + b0 + (size_t)(i+k)*4, 4); if (t == -1) break; } i += k + 1; continue; }
            if (w == 0x8009) { i += 4; continue; }
            if (w == 0x800a) { i += 6; continue; }
            if (w == 0x8010) break;
            if (w >= 0x8000) { i++; continue; }
            if (mode == 0x8000) {
                int32_t x, y, z; memcpy(&x, g_sys.dspram + b0 + (size_t)(i+1)*4, 4);
                memcpy(&y, g_sys.dspram + b0 + (size_t)(i+2)*4, 4); memcpy(&z, g_sys.dspram + b0 + (size_t)(i+3)*4, 4);
                if (w + 0x45 >= 1168 && w + 0x45 <= 1700)
                    fprintf(g_flight_fp, "T %d %d %d %d\n", w + 0x45, x, y, z);
                i += 4;
            } else i += 11;
        }
    }
    /* Once, when gameplay starts: the dynamic collision-zone table, in the same
     * layout tools/overnight zones.lua dumps out of MAME, so the two can be
     * diffed entry for entry. */
    { static int zdone; if (!zdone && _W[0x0CC0] == 3) { zdone = 1;
        fprintf(g_flight_fp, "# zones: count W[291C]=%ld  table W[2918]=%lX\n", W32(0x291C), (long)(uint32_t)_W[0x2918]);
        for (int i = 0; i < 16; i++)
            fprintf(g_flight_fp, "Z %2d x=%ld y=%ld z=%ld hdg=%lu ex=%ld ez=%ld f2A40=%ld\n", i,
                W32(0x2920 + i*4), W32(0x2950 + i*4), W32(0x2980 + i*4), (unsigned long)(uint32_t)_W[0x29B0 + i*4],
                W32(0x29E0 + i*4), W32(0x2A10 + i*4), W32(0x2A40 + i*4));
        fprintf(g_flight_fp, "RAW"); for (int s2 = 0x291C; s2 <= 0x2A6C; s2 += 4) fprintf(g_flight_fp, " %08X", (unsigned)(uint32_t)_W[s2]); fprintf(g_flight_fp, "\n"); } }
    /* PROPCYCL_MASKDUMP=<frame>: the per-layer tile->face mask at
     * W[0x0014 + layer*0x100 + row*16 + col], one slot per byte, 3 layers.
     * `world_stage_render` seeds `terrain_cell_find_triangle`'s start face
     * from this array (ROM 0x00461E), so a wrong entry sends the whole
     * neighbour walk to a wrong -- possibly invalid -- face. Matching MAME
     * probe: tools/overnight/probe_mask.lua */
    { static long mf = -1; static int init;
      if (!init) { init = 1; const char *e = getenv("PROPCYCL_MASKDUMP"); if (e) mf = atol(e); }
      if (mf >= 0 && (long)g_sys.frame_count == mf && g_flight_fp) {
          fprintf(g_flight_fp, "# MASK at f%u cell=%ld layers=%ld p=%ld,%ld,%ld\n",
                  g_sys.frame_count, W32(0x0D24), W32(0x0964),
                  W32(0x0D00), W32(0x0D04), W32(0x0D08));
          for (int L = 0; L < 3; L++) {
              for (int row = 0; row < 16; row++) {
                  fprintf(g_flight_fp, "M %d %2d", L, row);
                  for (int col = 0; col < 16; col++)
                      fprintf(g_flight_fp, " %02X",
                              (unsigned)((uint32_t)_W[0x0014 + L*0x100 + row*16 + col] & 0xFF));
                  fprintf(g_flight_fp, "\n");
              }
          }
      } }
    /* THE SHADOW BLOCK, W[0x2A70..0x2AAC]. `hud_draw_wings` (ROM 0x015C50 --
     * misnamed: it draws the PLAYER'S SHADOW, not the wings) reads exactly
     * these sixteen slots: the resolved ground height under the centre, the
     * two wing tips and the nose, plus the per-axis slopes it derives from
     * them, and it writes the mixer's shadow origin W[0xEB08..0xEB10]. They
     * are copies of the terrain probe records (game_terrain.c ~1001), so a
     * probe that resolves wrongly moves the shadow, which is why this is
     * logged beside the floor. Matching MAME probe: tools/overnight/probe_shadow.lua */
    if (g_flight_fp) {
        fprintf(g_flight_fp, "S %u", g_sys.frame_count);
        for (int s2 = 0x2A70; s2 <= 0x2AAC; s2 += 4) fprintf(g_flight_fp, " %ld", W32(s2));
        fprintf(g_flight_fp, " eb=%ld,%ld,%ld slope=%ld,%ld,%ld,%ld,%ld,%ld\n",
                W32(0xEB08), W32(0xEB0C), W32(0xEB10),
                W32(0x166B4), W32(0x166B8), W32(0x166BC),
                W32(0x166C0), W32(0x166C4), W32(0x166C8));
    }
    /* PROPCYCL_FLIGHTBLOCK=<from>,<to>: the six terrain probe records
     * W[0x1324..0x19FF] every frame in that window, same layout as
     * terrblock.lua dumps out of MAME, for a field-by-field diff. */
    { static long bf = -1, bt = -1; static int init;
      if (!init) { init = 1; const char *e = getenv("PROPCYCL_FLIGHTBLOCK"); if (e) sscanf(e, "%ld,%ld", &bf, &bt); }
      if (bf >= 0 && (long)g_sys.frame_count >= bf && (long)g_sys.frame_count <= bt) {
          fprintf(g_flight_fp, "B %u py=%ld floor=%ld\n", g_sys.frame_count, W32(0x0D04), W32(0x1328));
          for (int s2 = 0x1324; s2 <= 0x19FC; s2 += 4) fprintf(g_flight_fp, "%X %08X\n", s2, (unsigned)(uint32_t)_W[s2]);
      } }
    fflush(g_flight_fp);
    #undef W32
}

int propcycl_verbose(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("PROPCYCL_VERBOSE"); v = (e && *e != '0'); }
    return v;
}

static void save_screenshot(const char* path) {
    /* Headless widescreen (PROPCYCL_WIDE) renders wider than 640: read back
     * the whole scene, not its left 640 columns. 640 otherwise. */
    extern float g_scene_x0, g_scene_x1;
    const int SW = headless ? (int)(g_scene_x1 - g_scene_x0 + 0.5f) : SCREEN_WIDTH;
    uint8_t* pixels = malloc((size_t)SW * SCREEN_HEIGHT * 3);
    if (!pixels) { fprintf(stderr, "malloc failed for screenshot\n"); return; }

    /* Rows TIGHTLY packed: GL pads each row to 4 bytes by default, and a
     * widescreen width whose SW*3 is not a multiple of 4 (1138 for 21:9)
     * overran this buffer by 2 bytes a row and aborted in free(). */
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, SW, SCREEN_HEIGHT, GL_RGB, GL_UNSIGNED_BYTE, pixels);

    /* Final-stage gamma, applied to the whole frame including background --
     * the last step of the reference pixel chain (pc_raster_model.py). It
     * is a per-channel LUT at scanout, so doing it on the read-back frame
     * is exact rather than an approximation. No-op when the capture carries
     * no mixer data. */
    if (g_fog_valid && g_fog.have_gamma) {
        for (int i = 0; i < SW * SCREEN_HEIGHT; i++)
            fog_apply_gamma(&pixels[i*3], &pixels[i*3+1], &pixels[i*3+2]);
    }

    FILE* f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "Can't open %s\n", path); free(pixels); return; }

    fprintf(f, "P6\n%d %d\n255\n", SW, SCREEN_HEIGHT);
    /* OpenGL gives bottom-up, flip to top-down */
    for (int y = SCREEN_HEIGHT - 1; y >= 0; y--)
        fwrite(pixels + (size_t)y * SW * 3, 3, SW, f);

    fclose(f);
    free(pixels);
    printf("Screenshot saved to %s\n", path);
}


/* ---- P: pause toggle + player position report ---------------------------
 *
 * Pausing stops the GAME from advancing but keeps the window pumping events
 * and redrawing, so the frame you paused on stays on screen and P releases it.
 * The position report is printed on every press (both directions), because the
 * point of it is to say WHERE you are when something looks wrong -- the slots
 * are the same ones tools/overnight and the distance dump read, so a number
 * quoted from here can be fed straight back into the offline tools.
 *
 * The grid cell is included because almost everything position-gated in this
 * codebase is indexed by it: terrain chunk visibility, the collision layer
 * records, the scenery LOD tier and the per-cell zone list. "It disappears in
 * the second part of the stage" becomes a cell number with this. */
static bool g_paused;

static void report_player_position(void)
{
    extern intptr_t _W[];
    long px = (long)_W[0x0D00], py = (long)_W[0x0D04], pz = (long)_W[0x0D08];
    long cx = (long)_W[0x0CDC], cy = (long)_W[0x0CE0], cz = (long)_W[0x0CE4];
    long hdg = (long)(_W[0x0DAC] & 0xFFFF);
    long camhdg = (long)(_W[0x0CEC] & 0xFFFF);
    long spd = (long)_W[0x0D48];
    int gx = (int)(px / 0x18000), gz = (int)(pz / 0x18000);
    int course = (int)_W[0x0E0C];
    int cell = gx + gz * 8;
    int base[4] = { 1173, 1301, 1429, 1557 };
    printf("[POS] %s  frame %u  state %d/%d  course %d\n",
           g_paused ? "PAUSED " : "RESUMED",
           (unsigned)g_sys.frame_count, (int)_W[0x0CBC], (int)_W[0x0CC0], course);
    printf("      player  x=%-9ld y=%-9ld z=%-9ld  heading=%-6ld speed=%ld\n",
           px, py, pz, hdg, spd);
    printf("      camera  x=%-9ld y=%-9ld z=%-9ld  heading=%ld\n", cx, cy, cz, camhdg);
    printf("      grid    cell=%d  (grid_x=%d grid_z=%d)  terrain chunk model=%d\n",
           cell, gx, gz,
           (course >= 0 && course < 4 && cell >= 0 && cell < 128) ? base[course] + cell : -1);
    fflush(stdout);
    /* State the watched group's condition on this frame, if one is armed.
     * The whole point of pausing on a defect is to be told what it is. */
    { extern void watchlog_report_now(void); watchlog_report_now(); }
    /* Dump the whole frame's object list -- but only when PAUSING, not when
     * resuming. P prints on both edges, and two lines per pause would make
     * pause_diff.py compare a pause with itself. */
    if (g_paused) { extern void blinklog_dump_codes(void); blinklog_dump_codes(); }
}

/* Pause / resume. While paused the PAUSE CAMERA is live (renderer_3d.c
 * pausecam_apply, controls in input.c input_pausecam_update); resuming zeroes
 * it, which snaps the view back to the game's own camera. */
static void toggle_pause(const char *why)
{
    extern int g_pausecam_on;
    extern float g_pausecam_yaw, g_pausecam_pitch, g_pausecam_dist;
    g_paused = !g_paused;
    g_pausecam_on = g_paused;
    g_pausecam_yaw = g_pausecam_pitch = g_pausecam_dist = 0.0f;
    { extern void blinklog_mark(const char *, unsigned);
      char msg[64];
      snprintf(msg, sizeof msg, "%s pressed (%s)", why, g_paused ? "paused" : "resumed");
      blinklog_mark(msg, g_sys.frame_count); }
    report_player_position();
}

static bool init_sdl(void) {
    /* For headless: use offscreen video driver */
    if (headless) {
        SDL_SetHint(SDL_HINT_VIDEODRIVER, "offscreen");
    }

    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return false;
    }

    /* Use compatibility profile for legacy GL (glBegin/glEnd) */
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);

    Uint32 flags = SDL_WINDOW_OPENGL;
    if (!headless) flags |= SDL_WINDOW_SHOWN;
    else flags |= SDL_WINDOW_HIDDEN;

    /* Window scale multiplier — 2× the logical 640×480 game framebuffer
     * for a 1280×960 display window. Game logic and screenshots still use
     * the logical SCREEN_WIDTH×SCREEN_HEIGHT; the GL viewport scales it. */
    int win_w = SCREEN_WIDTH, win_h = SCREEN_HEIGHT;
    /* PROPCYCL_WIDE=<aspect> (e.g. 1.7778): the headless form of the
     * Widescreen display mode -- a wider offscreen frame with the 3D scene
     * widened to fill it, for PROPCYCL_SHOT_EVERY captures. */
    if (headless) {
        const char *e = getenv("PROPCYCL_WIDE");
        float a = e ? (float)atof(e) : 0.0f;
        if (a > (float)SCREEN_WIDTH / SCREEN_HEIGHT) {
            extern float g_scene_x0, g_scene_x1;
            win_w = ((int)(SCREEN_HEIGHT * a + 0.5f)) & ~1;
            g_scene_x0 = -(win_w - SCREEN_WIDTH) / 2.0f;
            g_scene_x1 = SCREEN_WIDTH + (win_w - SCREEN_WIDTH) / 2.0f;
        }
    }
    if (!headless) {
        /* 2x, but never bigger than the screen: a 1280x960 window on a
         * 1366x768 laptop, or on a 1080p one at 150% scaling, covered the
         * whole screen and looked like fullscreen. Fit 90% of the usable
         * area (taskbar excluded), keep 4:3, and let the player resize. */
        SDL_Rect ub;
        win_w = SCREEN_WIDTH * 2; win_h = SCREEN_HEIGHT * 2;
        if (SDL_GetDisplayUsableBounds(0, &ub) == 0 && ub.w > 0 && ub.h > 0) {
            double k = 2.0, kw = 0.9 * ub.w / SCREEN_WIDTH, kh = 0.9 * ub.h / SCREEN_HEIGHT;
            if (kw < k) k = kw;
            if (kh < k) k = kh;
            if (k < 1.0) k = 1.0;
            win_w = (int)(SCREEN_WIDTH * k); win_h = (int)(SCREEN_HEIGHT * k);
        }
        /* PROPCYCL_WINDOW=<w>x<h>: the starting window size, e.g. to check
         * the resolution scaler against a 16:9 window without a desktop */
        { const char *e = getenv("PROPCYCL_WINDOW"); int w, h;
          if (e && sscanf(e, "%dx%d", &w, &h) == 2 && w >= 160 && h >= 120) { win_w = w; win_h = h; } }
        flags |= SDL_WINDOW_RESIZABLE;
    }
    window = SDL_CreateWindow("Prop Cycle",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        win_w, win_h, flags);
    if (!window) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        return false;
    }

    glctx = SDL_GL_CreateContext(window);
    if (!glctx) {
        fprintf(stderr, "SDL_GL_CreateContext failed: %s\n", SDL_GetError());
        return false;
    }

    if (!headless) SDL_GL_SetSwapInterval(1);

    {   /* Which OpenGL did we get? Logged, because "white window" on
         * Windows is often the built-in software GL 1.1 ("GDI Generic"),
         * which is what Windows gives you without a graphics driver or over
         * Remote Desktop, and it cannot run this renderer. */
        const char *ven = (const char *)glGetString(GL_VENDOR);
        const char *ren = (const char *)glGetString(GL_RENDERER);
        const char *ver = (const char *)glGetString(GL_VERSION);
        printf("OpenGL: %s | %s | %s\n", ven ? ven : "?", ren ? ren : "?", ver ? ver : "?");
        if (!headless && ren && strstr(ren, "GDI Generic")) {
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Prop Cycle",
                "No OpenGL graphics driver was found (Windows gave the basic "
                "'GDI Generic' renderer).\n\nInstall or update the graphics "
                "driver for your GPU. This also happens over Remote Desktop.",
                window);
            return false;
        }
    }

    /* AUDIO (AUDIO_PLAN.md phase 1). Never opened headless: every gate in
     * this repo runs that way, and a device there would add a clock the
     * frame loop does not control. --noaudio / PROPCYCL_NOAUDIO also skip it.
     * Silence until the C352 installs itself as the source (phase 2). */
    { bool want_audio = !headless && !no_audio && getenv("PROPCYCL_NOAUDIO") == NULL;
      audio_init(want_audio); }
    /* WHICH GL. A headless run lands on llvmpipe, where the flush time is
     * software rasterisation and says nothing about the frame rate on a real
     * GPU -- worth printing so a perf number is never read out of context. */
    { const char *r = (const char *)glGetString(GL_RENDERER);
      const char *v = (const char *)glGetString(GL_VERSION);
      printf("  [GL] %s | %s%s\n", r ? r : "?", v ? v : "?",
             headless ? "  (headless)" : ""); }

    /* Scale the GL viewport to match the window size so the logical
     * 640×480 game framebuffer is upscaled to fill the 1280×960 window. */
    glViewport(0, 0, win_w, win_h);
    return true;
}

static int viewer_model = 0;  /* --model: 3D model viewer mode */

/* --geotest quad printer: one line per quad, four vertices in 1/16 pixel.
 * Format is matched by tools/geo_gate.py on the Python side. */
static void geotest_cb(const geo_quad *q, void *user)
{
    (void)user;
    printf("Q zsort=%06X color=%06X behind=%d", q->zsort, q->color, q->behind);
    for (int i = 0; i < 4; i++) {
        if (q->v[i].valid)
            printf("  v%d=%d,%d,z=%d,u=%u,v=%u,b=%d", i,
                   q->v[i].sx16, q->v[i].sy16, q->v[i].z,
                   q->v[i].u, q->v[i].v, q->v[i].bri);
        else
            printf("  v%d=CLIP", i);
    }
    printf("\n");
}
static const char *framedump_path = NULL;
static const char *geodump_path = NULL;
static const char *sprtest_dir = NULL, *sprtest_out = NULL;
static int sprtest_frame = 0;
static const char *texttest_dir = NULL, *texttest_out = NULL;
static int texttest_frame = 0;
static int geotest_model = -1;      /* --geotest <id>: gate geo_hw.c */
static int autostart_course = -1;  /* --autostart [course]: auto-force stage start */

/* L3 differential trace (GUARDRAILS §7 step 1) */
static const char* trace_dir = NULL;     /* --trace <dir> */
static uint32_t trace_max_fc = 0;        /* --trace-max <N> */
static const char* keycus_log = NULL;    /* --keycus <file> (MAME recording) */

/* Ctrl-C / SIGTERM handler — flips the same `running` flag the SDL_QUIT
 * and Esc/Q paths use, so the main loop drops out at the next iteration
 * and the SDL_GL_DeleteContext / SDL_Quit cleanup runs normally. Without
 * this, hitting Ctrl-C while the game is in a slow per-frame physics
 * pass (sub=3 with the slice-5 fixes runs at ~1 fps) leaves SDL/GL in a
 * half-torn-down state and glibc trips SIGABRT during signal-driven
 * shutdown, producing the "Aborted (core dumped)" you saw. */
static void on_signal_quit(int sig) {
    (void)sig;
    running = false;
}

int main(int argc, char* argv[]) {
#ifdef _WIN32
    /* Double-clicked, or started from a shortcut: work from the program's own
     * folder, where extracted/, roms/ and the settings live. */
    { char *base = SDL_GetBasePath(); if (base) { _chdir(base); SDL_free(base); } }
    /* A windowed program has no console, so everything printed goes to
     * propcycl.log beside the exe -- the first thing to read when it fails. */
    if (freopen("propcycl.log", "w", stdout)) {
        setvbuf(stdout, NULL, _IONBF, 0);
        _dup2(_fileno(stdout), _fileno(stderr));
    }
    /* Report real pixel sizes; without this Windows stretches the window by
     * the display scaling (150% turns 1280x960 into 1920x1440). */
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
#endif
    /* Install signal handlers BEFORE any SDL/GL state exists so we always
     * have a clean shutdown path. */
    {
#ifdef _WIN32
        signal(SIGINT,  on_signal_quit);
        signal(SIGTERM, on_signal_quit);
#else
        struct sigaction sa = {0};
        sa.sa_handler = on_signal_quit;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = SA_RESTART;
        sigaction(SIGINT,  &sa, NULL);
        sigaction(SIGTERM, &sa, NULL);
        sigaction(SIGHUP,  &sa, NULL);
#endif
    }

    const char* rom_dir = "extracted";

    /* Parse arguments */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--joytest") == 0) {
            extern int input_joytest(void);
            return input_joytest();
        } else if (strcmp(argv[i], "--noaudio") == 0) {
            no_audio = true;
        } else if (strcmp(argv[i], "--screenshot") == 0) {
            headless = true;
            if (i + 1 < argc && argv[i+1][0] != '-')
                screenshot_path = argv[++i];
            if (i + 1 < argc && argv[i+1][0] != '-')
                screenshot_frame = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--model") == 0 && i + 1 < argc) {
            viewer_model = (int)strtol(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "--trace") == 0 && i + 1 < argc) {
            trace_dir = argv[++i];
        } else if (strcmp(argv[i], "--trace-max") == 0 && i + 1 < argc) {
            trace_max_fc = (uint32_t)strtoul(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "--keycus") == 0 && i + 1 < argc) {
            keycus_log = argv[++i];
        } else if (strcmp(argv[i], "--framedump") == 0 && i + 1 < argc) {
            /* Render a CAPTURED MAME polygon-RAM dump instead of the live
             * game, so the input is deterministic and the output can be
             * diffed against MAME's own capture of that frame. */
            framedump_path = argv[++i];
        } else if (strcmp(argv[i], "--sprtest") == 0 && i + 3 < argc) {
            /* Gate for src/sprite_hw.c against pc_sprite_model.py: render
             * ONE frame's sprite layer in isolation, no GL, no 3D. */
            sprtest_dir = argv[++i];
            sprtest_frame = atoi(argv[++i]);
            sprtest_out = argv[++i];
        } else if (strcmp(argv[i], "--texttest") == 0 && i + 3 < argc) {
            /* Gate for src/text_hw.c against render_text.py. */
            texttest_dir = argv[++i];
            texttest_frame = atoi(argv[++i]);
            texttest_out = argv[++i];
        } else if (strcmp(argv[i], "--geodump") == 0 && i + 1 < argc) {
            /* Whole-FRAME counterpart of --geotest: run a captured MAME
             * polygon dump through the geometry stage and write every quad
             * in the oracle's golden-stream v2 format, so
             * tools/frame_gate.py can diff camera, projection and scale
             * against pc_geo_fixed.py --emit-stream2. --geotest only ever
             * covered one model under an identity view, which is exactly
             * the part of the camera that cannot be wrong. */
            geodump_path = argv[++i];
        } else if (strcmp(argv[i], "--geotest") == 0 && i + 1 < argc) {
            /* Gate for src/geo_hw.c against its oracle
             * (Prop Cycle MiSTer tools/pc_geo_fixed.py, itself gated
             * byte-exact vs MAME). Runs ONE model through the ported
             * geometry stage with an identity Q15 view matrix, zero
             * translation and the game's own zoom (0x780), and prints the
             * projected 1/16-pixel screen coordinates. The Python side is
             * driven with the same state, so any divergence is a port bug
             * and not a modelling difference. */
            geotest_model = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--level") == 0 && i + 1 < argc) {
            /* --level adv:N | nov:N [@frame]: start that stage directly,
             * through src/level_select.c -- the same as Escape -> Levels. */
            setenv("PROPCYCL_LEVEL", argv[++i], 1);
        } else if (strcmp(argv[i], "--autostart") == 0) {
            autostart_course = 0;
            if (i + 1 < argc && argv[i+1][0] != '-')
                autostart_course = atoi(argv[++i]);
            if (autostart_course < 0 || autostart_course > 3) autostart_course = 0;
        } else if (argv[i][0] != '-') {
            rom_dir = argv[i];
        }
    }

    printf("Prop Cycle - Namco System Super 22 reimplementation\n");
    /* rom_loader.c prints this itself; printing it here too made every
     * launch show the line twice. */
    if (headless)
        printf("Headless mode: screenshot after %d frames → %s\n",
               screenshot_frame, screenshot_path);

    /* Clear all state */
    memset(&g_sys, 0, sizeof(g_sys));

    /* First run: unpack the ROMs from propcycl.zip if the ROM folder is not
     * complete yet (src/rom_zip.c). This is how the Windows build is set up
     * -- put propcycl.zip beside propcycl.exe -- and it saves Linux users a
     * step. */
    /* The chips unzipped loose into roms/ work too. */
    if (!rom_dir_complete(rom_dir) && !strcmp(rom_dir, "extracted") && rom_dir_complete("roms"))
        rom_dir = "roms";
    if (!rom_dir_complete(rom_dir)) {
        char err[512], *base = SDL_GetBasePath();
        if (!rom_zip_autosetup(rom_dir, base, err, sizeof err) && !headless) {
            char msg[1024];
            snprintf(msg, sizeof msg,
                     "Prop Cycle needs its ROMs.\n\n"
                     "Put your propcycl.zip (the MAME ROM set) in the \"roms\" "
                     "folder next to this program, then start it again.\n\n(%s)", err);
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Prop Cycle", msg, NULL);
        }
        SDL_free(base);
    }

    /* Load all ROMs */
    if (!rom_load_all(rom_dir)) {
        fprintf(stderr, "Failed to load ROMs\n");
        return 1;
    }

    /* Init SDL + OpenGL */
    /* --geodump is a PURE GEOMETRY path: it runs framedump_render and writes
     * the golden stream, and never touches GL. It did not set `headless`,
     * though, so SDL_Init tried to open a real display and the process died
     * at startup on any headless box -- BEFORE reaching the dump at all.
     *
     * That failure was silent where it mattered: tools/frame_gate.py does not
     * remove its `ours_<tag>.gstream` before running, so when the binary died
     * the gate diffed a STALE file from a previous build and still printed
     * PASS. Frame-gate results were therefore not testing the current binary
     * at all. Use the offscreen driver here, as --screenshot already does. */
    /* --geodump/--geotest/--sprtest/--texttest are all offline gates: they
     * write a file and exit, and none of them needs a window. Without this
     * they die at SDL_Init on a machine with no display and every gate that
     * drives them reports MISSING OUTPUT -- or, worse, silently diffs a stale
     * file from a previous run (which is how six frame gates read PASS for a
     * whole session against nine-hour-old data). */
    if (geodump_path || geotest_model >= 0 || sprtest_dir || texttest_dir)
        headless = true;
    if (!init_sdl()) return 1;

    /* HLE sound-command layer (AUDIO_PLAN.md phase 3 fallback) -- pre-
     * extracted assets, never the wave ROM at runtime. See audio_hle.h. */
    audio_hle_init(NULL, rom_dir);

    /* Init subsystems */
    ui_init(window);   /* also in headless, so screenshots can show the menu */
    { extern int g_bbox_code; const char *e = getenv("PROPCYCL_BBOX");
      if (e) g_bbox_code = atoi(e); }
    { extern int g_zord_on; g_zord_on = getenv("PROPCYCL_ZORD") ? 1 : 0; }
    { extern int g_dsp32; const char *e = getenv("PROPCYCL_DSP32"); g_dsp32 = (e && *e && *e != '0'); }
    { extern int g_guardband_off; const char *e = getenv("PROPCYCL_GUARDBAND_LEGACY"); g_guardband_off = (e && *e && *e != '0'); }
    { extern int g_only_code; const char *e = getenv("PROPCYCL_ONLY_CODE");
      if (e && *e) g_only_code = atoi(e); }
    { extern int g_vdump; const char *e = getenv("PROPCYCL_VDUMP");
      if (e && *e) g_vdump = atoi(e); }
    /* The 13-word 0x8008 full-matrix fix (the player's shadow). Read here,
     * once, per register row 40 -- getenv inside the frame loop faults. */
    { extern int g_no_nmat, g_nmat_order, g_nmat_replace, g_nmat_compose, g_nmat_nohalf; const char *e;
      if ((e = getenv("PROPCYCL_NMAT_NOHALF"))) g_nmat_nohalf = (*e != '0');
      if ((e = getenv("PROPCYCL_NMAT_COMPOSE"))) g_nmat_compose = (*e != '0');
      if ((e = getenv("PROPCYCL_NMAT_REPLACE"))) g_nmat_replace = (*e != '0');
      if ((e = getenv("PROPCYCL_NO_NMAT")))     g_no_nmat    = (*e != '0');
      if ((e = getenv("PROPCYCL_NMAT_ORDER")))  g_nmat_order = atoi(e); }
    /* Arm the render phase timers only when someone is going to read them --
     * they cost ~10% of runtime otherwise (see rperf() in renderer_3d.c). */
    { extern int g_perf_enabled; const char *e = getenv("PROPCYCL_PERF");
      g_perf_enabled = (e && *e && *e != '0');
      if (!g_perf_enabled) { const char *pf = getenv("PROPCYCL_PERFFRAME");
                             g_perf_enabled = (pf && *pf && *pf != '0'); } }
    { extern int g_tex_orphan; const char *e = getenv("PROPCYCL_TEXORPHAN");
      if (e && *e) g_tex_orphan = atoi(e); }
    { extern size_t tex_cache_budget; const char *e = getenv("PROPCYCL_TEXBUDGET");
      if (e && *e) { long mb = atol(e); if (mb > 0) tex_cache_budget = (size_t)mb * 1024 * 1024; } }
    { extern int g_tex_fixedcap; const char *e = getenv("PROPCYCL_TEX_FIXEDCAP");
      if (e && *e) g_tex_fixedcap = atoi(e); }
    { extern int g_tex_pow2sample; const char *e = getenv("PROPCYCL_TEX_POW2SAMPLE");
      if (e && *e) g_tex_pow2sample = atoi(e); }
    { extern int g_tex_pow2alloc; const char *e = getenv("PROPCYCL_TEX_POW2ALLOC");
      if (e && *e) g_tex_pow2alloc = atoi(e); }
    { extern int g_tex_fifo; const char *e = getenv("PROPCYCL_TEX_FIFO");
      if (e && *e) g_tex_fifo = atoi(e); }
    { extern int g_tex_clipbox; const char *e = getenv("PROPCYCL_TEX_CLIPBOX");
      if (e && *e) g_tex_clipbox = atoi(e); }
    { extern int g_wide_hud_center; const char *e = getenv("PROPCYCL_WIDE_HUD_CENTER");
      if (e && *e) g_wide_hud_center = atoi(e); }
    { extern int g_rot8002; const char *e = getenv("PROPCYCL_ROT8002");
      if (e && *e) g_rot8002 = atoi(e); }
    { extern int g_8002_flagord; if (getenv("PROPCYCL_NO_8002_FLAGORD")) g_8002_flagord = 0; }
    { extern int g_8002_dbg; g_8002_dbg = getenv("PROPCYCL_8002DBG") ? 1 : 0; }
    { extern int g_attract_log; g_attract_log = getenv("PROPCYCL_ATTRACT_LOG") ? 1 : 0; }
    { extern int g_no_cursor_bound; g_no_cursor_bound = getenv("PROPCYCL_NO_CURSOR_BOUND") ? 1 : 0; }
    { extern int g_hard_8010; g_hard_8010 = getenv("PROPCYCL_HARD_8010") ? 1 : 0; }
    { extern int g_no_objshift; g_no_objshift = getenv("PROPCYCL_NO_OBJSHIFT") ? 1 : 0; }
    { extern int g_orbitfix; const char *e = getenv("PROPCYCL_ORBITFIX");
      g_orbitfix = (e && *e != '0'); }
    { extern int g_max_data; const char *e = getenv("PROPCYCL_MAXDATA");
      if (e && *e) g_max_data = atoi(e); }
    { extern int g_cmd_dump; g_cmd_dump = getenv("PROPCYCL_CMDDUMP") ? 1 : 0; }
    { extern int g_rot8008; const char *e = getenv("PROPCYCL_ROT8008");
      if (e) g_rot8008 = atoi(e); }
    { extern int g_snr_sinfirst; const char *e = getenv("PROPCYCL_SNR_SINFIRST");
      if (e) g_snr_sinfirst = atoi(e); }
    { extern int g_root15; const char *e = getenv("PROPCYCL_ROOT15");
      if (e) g_root15 = atoi(e); }
    /* Read by VALUE, not by presence: `=0` must mean OFF. The old form
     * promoted a parsed 0 back to 1, so PROPCYCL_ZONEDBG=0 turned the
     * feature ON -- register row 119's footgun, which is how two "with and
     * without" runs came back byte-identical. `=1` is the level-1 default
     * for a bare or non-numeric value. */
    { extern int g_zone_dbg; const char *e = getenv("PROPCYCL_ZONEDBG");
      g_zone_dbg = 0;
      if (e && *e) { if (e[0] == '0' && e[1] == '\0') g_zone_dbg = 0;
                     else { g_zone_dbg = atoi(e); if (g_zone_dbg == 0) g_zone_dbg = 1; } } }
    { extern int g_wrt_dbg; g_wrt_dbg = getenv("PROPCYCL_WRTDBG") ? 1 : 0; }
    { extern int g_no_zones; g_no_zones = getenv("PROPCYCL_NO_ZONES") ? 1 : 0; }
    { extern int g_wot_frame; const char *e = getenv("PROPCYCL_WOTDBG"); if (e) g_wot_frame = atoi(e); }
    { extern int g_tunlog; const char *e = getenv("PROPCYCL_TUNLOG"); if (e) g_tunlog = atoi(e); if (g_tunlog < 1) g_tunlog = 0; }
    { extern void flight_replay_load(const char *);
      const char *e = getenv("PROPCYCL_REPLAY"); if (e && *e) flight_replay_load(e); }
    { extern void flight_rec_start(void);
      if (getenv("PROPCYCL_RECORD")) flight_rec_start(); }
    { extern int g_layerfix; if (getenv("PROPCYCL_NO_LAYERFIX")) g_layerfix = 0; }
    { extern int g_votefix; if (getenv("PROPCYCL_NO_VOTEFIX")) g_votefix = 0; }
    { extern int g_menustick; if (getenv("PROPCYCL_NO_MENUSTICK")) g_menustick = 0; }
    { extern int g_edgefix; if (getenv("PROPCYCL_NO_EDGEFIX")) g_edgefix = 0; }
    { extern int g_listwho; const char *e = getenv("PROPCYCL_LISTWHO"); if (e) g_listwho = atoi(e); }
    { extern int g_no_emptylist; if (getenv("PROPCYCL_NO_EMPTYLIST")) g_no_emptylist = 1; }
    { extern int g_raw_objshift; if (getenv("PROPCYCL_RAW_OBJSHIFT")) g_raw_objshift = 1; }
    { extern int g_hud_vp1only; if (getenv("PROPCYCL_HUD_VP1ONLY")) g_hud_vp1only = 1; }
    { extern int g_aorec; if (getenv("PROPCYCL_AOREC")) g_aorec = 1; }
    { extern int g_texthash_fade; if (getenv("PROPCYCL_TEXTHASH_FADE")) g_texthash_fade = 1; }
    { extern int g_train; if (getenv("PROPCYCL_NO_TRAIN")) g_train = 0;
      if (getenv("PROPCYCL_TRAIN")) g_train = 1; }
    { extern int g_train_chunk; const char *e = getenv("PROPCYCL_TRAIN_CHUNK"); if (e) g_train_chunk = atoi(e); }
    { extern int g_train_y; const char *e = getenv("PROPCYCL_TRAIN_Y"); if (e) g_train_y = atoi(e); }
    { extern int g_keydelta; const char *e = getenv("PROPCYCL_KEYDELTA"); if (e && atoi(e) > 0) g_keydelta = atoi(e); }
    { extern int g_stagedbg; if (getenv("PROPCYCL_STAGEDBG")) g_stagedbg = 1; }
    /* THE SAVED RANKING FILE (hiscore_load / eeprom_write_block). An
     * interactive run keeps it in propcycl_scores.nv beside
     * propcycl_controls.cfg; a HEADLESS run (every gate and test) neither
     * reads nor writes it unless PROPCYCL_SCOREFILE names one, so a player's
     * scores can never change what a gate measures. PROPCYCL_SCOREFILE=""
     * turns it off interactively too. */
    { extern char g_score_path[512]; const char *e = getenv("PROPCYCL_SCOREFILE");
      if (e) snprintf(g_score_path, sizeof g_score_path, "%s", e);
      else if (!headless) snprintf(g_score_path, sizeof g_score_path, "propcycl_scores.nv"); }
    { extern int g_slot_legacy; if (getenv("PROPCYCL_SLOT_LEGACY")) g_slot_legacy = 1; }
    { extern int g_tie_emit; if (getenv("PROPCYCL_TIE_EMIT")) g_tie_emit = 1; }
    { extern int g_lampdbg; if (getenv("PROPCYCL_LAMPLOG")) g_lampdbg = 1; }
    { extern int g_vp_ap_legacy; if (getenv("PROPCYCL_VP_AP_LEGACY")) g_vp_ap_legacy = 1; }
    { extern int g_vp_view_legacy; if (getenv("PROPCYCL_VP_VIEW_LEGACY")) g_vp_view_legacy = 1; }
    { extern void seamtest_parse(const char *); const char *e = getenv("PROPCYCL_SEAMTEST");
      if (e) seamtest_parse(e); }
    { extern int g_seam_legacy; if (getenv("PROPCYCL_SEAM_LEGACY")) g_seam_legacy = 1; }
    { extern int g_fadelog; const char *e = getenv("PROPCYCL_FADELOG"); if (e) g_fadelog = atoi(e) ? atoi(e) : 1; }
    /* PROPCYCL_FLIGHTLOG=<path>: one line per frame of the flight-vs-terrain
     * state -- player xyz, camera xyz, the resolved floor, the contact flags,
     * the grid cell -- plus every terrain chunk placement in the display list
     * (model, camera-relative xyz). tools/overnight/terrain_floor_check.py
     * rebuilds the DRAWN mesh from those placements and the point ROM and
     * compares its height under the player with the floor the collision code
     * resolved; tools/overnight/probe_flight.lua logs the same fields out of
     * MAME for a frame-locked diff. */
    { const char *e = getenv("PROPCYCL_FLIGHTLOG");
      if (e && *e) g_flight_fp = fopen(e, "w"); }
    { extern int g_asm_mode; const char *e = getenv("PROPCYCL_ASM");
      if (e) g_asm_mode = atoi(e); }
    { extern int g_nodes_at; const char *e = getenv("PROPCYCL_NODES");
      g_nodes_at = e ? atoi(e) : -1; }
    { extern int g_nodes_full; const char *e = getenv("PROPCYCL_NODES_FULL");
      g_nodes_full = e ? atoi(e) : 0; }
    { extern int g_tdb_log; const char *e = getenv("PROPCYCL_TDBLOG");
      g_tdb_log = (e && *e != '0'); }
    { extern int g_draw_mat; const char *e = getenv("PROPCYCL_DRAWMAT");
      g_draw_mat = (e && *e != '0'); }
    { extern int g_euler_8002, g_euler_8008; const char *e;
      if ((e = getenv("PROPCYCL_GEO_EULER")))      g_euler_8002 = atoi(e);
      if ((e = getenv("PROPCYCL_GEO_EULER_8008"))) g_euler_8008 = atoi(e); }
    { extern int g_euler_p; const char *e;
      if ((e = getenv("PROPCYCL_GEO_EULER_P"))) g_euler_p = atoi(e); }
    { const char *e = getenv("PROPCYCL_SNDCMDLOG");
      if (e) g_sndcmd_f = fopen(e, "w"); }
    { extern int g_mat_dump; g_mat_dump = getenv("PROPCYCL_MATDUMP") ? 1 : 0; }
    { extern int g_ik_dump; g_ik_dump = getenv("PROPCYCL_IKDUMP") ? 1 : 0; }
    { extern int g_rig_dump; g_rig_dump = getenv("PROPCYCL_RIGDUMP") ? 1 : 0; }
    { const char *e = getenv("PROPCYCL_SPRCALL"); extern int g_sprcall;
      g_sprcall = e ? atoi(e) : 0; if (e && g_sprcall == 0) g_sprcall = 1; }
    { extern int g_cglog; g_cglog = getenv("PROPCYCL_CGLOG") ? 1 : 0; }
    { extern void blinklog_init(void); blinklog_init(); }
    { extern int g_rig_ik_gameplay; const char *e = getenv("PROPCYCL_RIG_IK_GAMEPLAY");
      if (e) g_rig_ik_gameplay = atoi(e); }
    /* PROPCYCL_FEEDDUMP=<dir>:<frame> — write our LIVE 2D feed at that frame in
     * the same layout the MAME capture uses, so the two can be diffed. Rows 26
     * and 35 are live-feed bugs (sprite_hw and text_hw are 100.00% exact on the
     * gameplay capture), and this is what makes the feed itself measurable. */
    { extern char g_feed_dir[]; extern int g_feed_frame;
      const char *e = getenv("PROPCYCL_FEEDDUMP");
      if (e) { const char *c = strrchr(e, ':');
               if (c) { size_t n = (size_t)(c - e); if (n > 250) n = 250;
                        memcpy(g_feed_dir, e, n); g_feed_dir[n] = 0;
                        g_feed_frame = atoi(c + 1); } } }
    /* PROPCYCL_SHOTDIR overrides where F12 writes. Relative paths resolve
     * against the cwd, which launch.sh sets to the project root. */
    { extern int g_coll_dbg; g_coll_dbg = getenv("PROPCYCL_COLLDBG") ? 1 : 0; }
    { extern int g_no_props; g_no_props = getenv("PROPCYCL_NO_PROPS") ? 1 : 0; }
    { extern int g_loop_dbg; g_loop_dbg = getenv("PROPCYCL_LOOPDBG") ? 1 : 0; }
    { extern int g_aolog; g_aolog = getenv("PROPCYCL_AOLOG") ? 1 : 0; }
    { extern int g_lodlog; g_lodlog = getenv("PROPCYCL_LODLOG") ? 1 : 0; }
    { const char *e = getenv("PROPCYCL_ANIMOBJ"); extern int g_animobj;
      /* ON by default since register row 160: the records now carry the ROM's
       * own coordinates, so the train and the dynamic collision zones are
       * real. PROPCYCL_ANIMOBJ=0 turns the whole system off for A/B. */
      g_animobj = e ? (atoi(e) != 0) : 1; }
    { extern int g_scenery_off; g_scenery_off = getenv("PROPCYCL_NO_SCENERY") ? 1 : 0; }
    /* PROPCYCL_SCENPROBE=<lo>,<hi>: name the ROM record, LOD tier and frame
     * count behind every scenery placement in that model-id range. */
    { extern int g_lod_fartier; g_lod_fartier = getenv("PROPCYCL_LOD_FARTIER") ? 1 : 0; }
    { extern int g_contactlog; g_contactlog = getenv("PROPCYCL_CONTACTLOG") ? 1 : 0; }
    { extern int g_sndlog;     g_sndlog     = getenv("PROPCYCL_SNDLOG")     ? 1 : 0; }
    { extern int g_sndtrig;    g_sndtrig    = getenv("PROPCYCL_SNDTRIG")    ? 1 : 0; }
    { extern int g_mculog;     g_mculog     = getenv("PROPCYCL_MCULOG")     ? 1 : 0; }
    /* PROPCYCL_TEST_TIMEHOLD=1: the stage timer never runs out, so a level
     * lasts as long as the run -- for listening past a song's second loop
     * point, which an 80 s level does not reach. Test harness only. */
    { extern int g_test_timehold; const char *e = getenv("PROPCYCL_TEST_TIMEHOLD");
      g_test_timehold = (e && *e && *e != '0'); }
    { extern int g_test_pass; const char *e = getenv("PROPCYCL_TEST_PASS");
      g_test_pass = e ? atoi(e) : 0; }
    /* Read here, never mid-game: getenv() in the frame loop faults (row 40). */
    { extern int g_orbit_ea; const char *e = getenv("PROPCYCL_ORBIT_EA");
      if (e) g_orbit_ea = (*e != '0'); }
    { extern int g_vp_zoom_legacy; const char *e = getenv("PROPCYCL_VP_ZOOM_LEGACY");
      g_vp_zoom_legacy = (e && *e && *e != '0'); }
    { extern int g_findcode; const char *e = getenv("PROPCYCL_FINDCODE");
      if (e) g_findcode = atoi(e); }
    { extern const char *g_sharedump; g_sharedump = getenv("PROPCYCL_SHAREDUMP"); }
    { extern void watchlog_init(const char *); watchlog_init(getenv("PROPCYCL_WATCH")); }
    /* PROPCYCL_PICK=<code>: highlight that object without the menu, so the
     * picker can be verified headlessly and a code can be pointed at from a
     * script. */
    { extern void render_pick_set(int); const char *e = getenv("PROPCYCL_PICK");
      if (e) render_pick_set(atoi(e)); }
    { extern int g_scenprobe_lo, g_scenprobe_hi;
      const char *e = getenv("PROPCYCL_SCENPROBE");
      if (e) sscanf(e, "%d,%d", &g_scenprobe_lo, &g_scenprobe_hi); }
    { extern int g_rig_split; g_rig_split = getenv("PROPCYCL_RIG_SPLIT") ? 1 : 0; }
    { extern int g_hud_screen_off; g_hud_screen_off = getenv("PROPCYCL_NO_HUDFIX") ? 1 : 0; }
    { extern int g_hud_by_model; g_hud_by_model = getenv("PROPCYCL_HUD_BY_MODEL") ? 1 : 0; }
    { extern int g_hud_norot; g_hud_norot = getenv("PROPCYCL_HUD_NOROT") ? 1 : 0; }
    { extern int g_hud_flat; g_hud_flat = getenv("PROPCYCL_HUD_FLAT") ? 1 : 0; }
    { extern int g_menu_pick; const char *e = getenv("PROPCYCL_TEST_MENU_PICK");
      if (e) g_menu_pick = atoi(e); }
    { const char *e = getenv("PROPCYCL_SHOTDIR");
      if (e && *e) { strncpy(shot_dir, e, sizeof shot_dir - 1);
                     shot_dir[sizeof shot_dir - 1] = 0; } }
    /* PROPCYCL_SHOT_EVERY=N captures every N frames without a keypress, so a
     * headless or offscreen run produces the same PNGs F12 does. */
    { const char *e = getenv("PROPCYCL_SHOT_EVERY");
      if (e) shot_every = atoi(e); }
    input_init();
    renderer2d_init();
    renderer3d_init();
    renderer3d_load_palette(rom_dir);
    if (framedump_path) {
        if (!framedump_load(framedump_path)) return 1;
        setenv("PROPCYCL_FRAMEDUMP", "1", 1);
        setenv("PROPCYCL_GEO_HW", "1", 1);
    }

    /* --texttest: render one frame's text layer and exit. No GL needed. */
    if (texttest_dir) {
        if (!fog_load_frame(texttest_dir, texttest_frame)) {
            fprintf(stderr, "texttest: no mixer data for frame %d\n", texttest_frame);
            return 1;
        }
        text_state ts;
        if (!text_load_frame_into(&ts, texttest_dir, texttest_frame, NULL, NULL)) {
            fprintf(stderr, "texttest: no cgram/attr for frame %d\n", texttest_frame);
            return 1;
        }
        uint8_t *rgba = malloc((size_t)640 * 480 * 4);
        text_render(&ts, &g_fog, rgba, 1);          /* gate mode */
        FILE *pf = fopen(texttest_out, "wb");
        if (pf) {
            fprintf(pf, "P6\n640 480\n255\n");
            for (long i = 0; i < 640L * 480; i++) fwrite(&rgba[i*4], 1, 3, pf);
            fclose(pf);
        }
        printf("texttest: wrote %s\n", texttest_out);
        return 0;
    }

    /* --sprtest: render one frame's sprite layer and exit. No GL needed. */
    if (sprtest_dir) {
        if (!fog_load_frame(sprtest_dir, sprtest_frame)) {
            fprintf(stderr, "sprtest: no mixer/fog data for frame %d\n", sprtest_frame);
            return 1;
        }
        sprite_state st;
        if (!sprite_load_frame_into(&st, sprtest_dir, sprtest_frame, NULL)) {
            fprintf(stderr, "sprtest: no sprite data for frame %d\n", sprtest_frame);
            return 1;
        }
        uint8_t *rgba = malloc((size_t)SPR_W * SPR_H * 4);
        uint8_t *prio = malloc((size_t)SPR_W * SPR_H);
        sprite_render(&st, &g_fog, rgba, prio);
        long painted = 0;
        for (long i = 0; i < (long)SPR_W * SPR_H; i++) if (rgba[i*4+3]) painted++;
        /* The reference model gamma-corrects its layer output, because the
         * hardware compose applies gamma after the layer is built. Match it
         * here so the gate compares like with like; in the real renderer
         * gamma is applied once over the whole composed frame instead. */
        for (long i = 0; i < (long)SPR_W * SPR_H; i++)
            if (rgba[i*4+3]) fog_apply_gamma(&rgba[i*4], &rgba[i*4+1], &rgba[i*4+2]);
        FILE *pf = fopen(sprtest_out, "wb");
        if (pf) {
            fprintf(pf, "P6\n%d %d\n255\n", SPR_W, SPR_H);
            for (long i = 0; i < (long)SPR_W * SPR_H; i++) {
                /* untouched pixels as white, matching the model's canvas */
                if (rgba[i*4+3]) fwrite(&rgba[i*4], 1, 3, pf);
                else { unsigned char wht[3] = {255,255,255}; fwrite(wht,1,3,pf); }
            }
            fclose(pf);
        }
        printf("sprtest: %ld sprite pixels -> %s\n", painted, sprtest_out);
        return 0;
    }

    /* --geodump: whole-frame golden stream, then exit. No GL needed. */
    if (geodump_path) {
        if (!framedump_path) {
            fprintf(stderr, "--geodump requires --framedump <poly_fN.bin>\n");
            return 1;
        }
        FILE *gf = fopen(geodump_path, "w");
        if (!gf) { fprintf(stderr, "cannot write %s\n", geodump_path); return 1; }
        fprintf(gf, "# propcycl geo stream v2 dump=%s\n", framedump_path);
        framedump_render(geodump_cb, gf);
        fclose(gf);
        printf("geodump: %d quads -> %s\n", g_geodump_n, geodump_path);
        return 0;
    }

    /* --geotest: dump one model's projected quads and exit (see the arg
     * parser above for why). Identity Q15 view, zero translation, zoom
     * 0x780 = the value dsp_viewport_setup writes. */
    if (geotest_model >= 0) {
        geo_view gv;
        memset(&gv, 0, sizeof gv);
        gv.m[0][0] = 0x7FFF; gv.m[1][1] = 0x7FFF; gv.m[2][2] = 0x7FFF;
        gv.t[0] = 0; gv.t[1] = 0; gv.t[2] = 0;
        gv.zoom_mant = 1920; gv.zoom_shift = 0;
        gv.vx = 0; gv.vy = 0;
        geo_hw_set_view(&gv);
        printf("GEOTEST model=%d zoom=%d/%d\n",
               geotest_model, gv.zoom_mant, gv.zoom_shift);
        geo_hw_object(geotest_model, geotest_cb, NULL);
        printf("GEOTEST end\n");
        return 0;
    }

    /* L3 trace setup (before game_init so init-time keycus reads replay) */
    if (keycus_log) trace_keycus_load(keycus_log);
    if (trace_dir && !trace_init(trace_dir, trace_max_fc)) return 1;

    /* Run game init (entry_reset chain) */
    printf("Running game init...\n");
    game_init();
    /* the saved free-play choice (Escape -> Controls), interactive runs only */
    { extern int g_freeplay_cfg;
      if (!headless && g_freeplay_cfg >= 0 && !getenv("PROPCYCL_COINPLAY")) {
          W16_SET(0x3FF4, g_freeplay_cfg);
          printf("  %s (propcycl_controls.cfg)\n", g_freeplay_cfg ? "free play" : "coins required");
      } }
    { extern bool master_dsp_init(const char *); master_dsp_init(rom_dir); }
    printf("Game init complete, entering main loop\n");

    /* Model viewer mode */
    if (viewer_model > 0) {
        renderer3d_set_viewer_model(viewer_model);
        printf("Model viewer mode: use Left/Right arrows to cycle, Q to quit\n");
    }

    /* Main frame loop */
    while (running) {
        /* PROPCYCL_MENU_CLICK=<frame>:<x>,<y>[;<frame>:<x>,<y>...] -- headless
         * test hook: a left click on the Escape menu at that frame (window
         * pixels), so a screenshot can show an open dropdown. Use with
         * PROPCYCL_MENU_OPEN=1. Button down on the frame, up on the next. */
        if (headless) {
            static int nclk = -1, cf[8], cx[8], cy[8];
            if (nclk < 0) {
                const char *e = getenv("PROPCYCL_MENU_CLICK");
                nclk = 0;
                while (e && *e && nclk < 8) {
                    if (sscanf(e, "%d:%d,%d", &cf[nclk], &cx[nclk], &cy[nclk]) == 3) nclk++;
                    e = strchr(e, ';'); if (e) e++;
                }
            }
            if (nclk > 0) {
                ui_input_begin();
                for (int k = 0; k < nclk; k++) {
                    int fc = (int)g_sys.frame_count;
                    if (fc != cf[k] && fc != cf[k] + 1) continue;
                    SDL_Event e; memset(&e, 0, sizeof e);
                    e.type = SDL_MOUSEMOTION; e.motion.x = cx[k]; e.motion.y = cy[k];
                    ui_handle_event(&e);
                    memset(&e, 0, sizeof e);
                    e.type = (fc == cf[k]) ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
                    e.button.button = SDL_BUTTON_LEFT; e.button.clicks = 1;
                    e.button.x = cx[k]; e.button.y = cy[k];
                    ui_handle_event(&e);
                }
                ui_input_end();
            }
        }
        if (!headless) {
            SDL_Event ev;
            ui_input_begin();
            while (SDL_PollEvent(&ev)) {
                if (ev.type == SDL_QUIT) running = false;
                { extern void input_pad_event(const SDL_Event *);
                  input_pad_event(&ev); }        /* controller hotplug */

                /* Mouse drives the map view when it is up and the menu is not. */
                if (!ui_is_open() && ui_map_active()) {
                    if (ev.type == SDL_MOUSEMOTION)
                        ui_map_mouse(ev.motion.xrel, ev.motion.yrel,
                                     SDL_GetMouseState(NULL, NULL), 0);
                    else if (ev.type == SDL_MOUSEWHEEL)
                        ui_map_mouse(0, 0, 0, ev.wheel.y);
                }

                /* A pad has no Escape key -- a Steam Deck has no keyboard at
                 * all -- so the RIGHT-STICK CLICK opens and closes the menu
                 * (unused by the game). The Deck's touchscreen is the mouse
                 * for the menu once it is up. Checked before the menu takes
                 * the event, so the same button also closes it. */
                if (ev.type == SDL_CONTROLLERBUTTONDOWN &&
                    ev.cbutton.button == SDL_CONTROLLER_BUTTON_RIGHTSTICK) {
                    ui_toggle();
                    continue;
                }

                /* Start on a pad PAUSES during gameplay (and resumes from any
                 * pause) -- the pause camera below then lets the player orbit
                 * the rider. Everywhere else it stays the cabinet START. */
                if (ev.type == SDL_CONTROLLERBUTTONDOWN &&
                    ev.cbutton.button == SDL_CONTROLLER_BUTTON_START && !ui_is_open()) {
                    extern int input_in_gameplay(void);
                    if (g_paused || input_in_gameplay()) { toggle_pause("Start"); continue; }
                }

                /* Escape always reaches us; everything else goes to the menu
                 * while it is open, so menu clicks never leak into the game. */
                if (!(ev.type == SDL_KEYDOWN &&
                      (ev.key.keysym.sym == SDLK_ESCAPE ||
                       ev.key.keysym.sym == SDLK_F12)) &&
                    ui_handle_event(&ev))
                    continue;

                if (ev.type == SDL_KEYDOWN) {
                    switch (ev.key.keysym.sym) {
                    case SDLK_ESCAPE:
                        /* Escape opens the menu now; Quit lives in it, so a
                         * stray Escape cannot end a session by accident. */
                        ui_toggle();
                        break;
                    /* Q no longer quits: it collides with Q = camera/pan up,
                     * and quitting is a menu item now (Escape -> Quit). */
                    case SDLK_RIGHT: case SDLK_PERIOD:
                        if (viewer_model > 0) {
                            viewer_model++;
                            renderer3d_set_viewer_model(viewer_model);
                        }
                        break;
                    case SDLK_LEFT: case SDLK_COMMA:
                        if (viewer_model > 1) {
                            viewer_model--;
                            renderer3d_set_viewer_model(viewer_model);
                        }
                        break;
                    case SDLK_g:
                    case SDLK_1: case SDLK_2: case SDLK_3: case SDLK_4: {
                        /* Start a stage directly. 1-3 = NOVICE Cliff Rock /
                         * Wind Woods / Industarn, 4 = ADVANCED day 4 (the
                         * final stage, which NOVICE cannot reach without the
                         * debug switch); G restarts the current course.
                         *
                         * These used to force state 4 with W[0x0E0C] set and
                         * nothing else -- which stopped selecting a course the
                         * moment register row 107 restored the real CONTROLS
                         * -> MODE SELECT -> STAGE SELECT chain, because the
                         * STAGE SELECT overwrites the course. They now go
                         * through src/level_select.c, which walks that chain
                         * and picks the course AT the stage select. */
                        extern void level_select_request(int adv, int course);
                        int course;
                        switch (ev.key.keysym.sym) {
                        case SDLK_1: course = 0; break;
                        case SDLK_2: course = 1; break;
                        case SDLK_3: course = 2; break;
                        case SDLK_4: course = 3; break;
                        default: course = (int)g_sys.work_ram[0x0E0F] & 3; break;
                        }
                        level_select_request(course == 3, course);
                        if (viewer_model > 0) {
                            viewer_model = 0;
                            renderer3d_set_viewer_model(0);
                        }
                        break;
                    }
                    case SDLK_t:
                        /* Force title screen (state 6 = state_title_init) */
                        printf("[INPUT] Forcing title screen (state 6)...\n");
                        g_sys.work_ram[0x0CBC]=0; g_sys.work_ram[0x0CBD]=0;
                        g_sys.work_ram[0x0CBE]=0; g_sys.work_ram[0x0CBF]=6;
                        break;
                    case SDLK_l:
                        renderer3d_toggle_level_view();
                        break;
                    case SDLK_F9:
                        /* Toggle the flight recorder without opening the
                         * menu -- a player chasing a spot needs to arm this
                         * mid-flight, and the menu eats input while open. */
                        { extern void flight_rec_toggle(void); flight_rec_toggle(); }
                        break;
                    case SDLK_p:
                        toggle_pause("P");
                        break;
                    case SDLK_F12:
                        /* Only ARM it. The event pump runs after the swap,
                         * where the back buffer's contents are undefined --
                         * the grab happens just before the next swap, with a
                         * complete frame in the buffer. */
                        shot_pending = true;
                        break;
                    default: break;
                    }
                }
            }
            ui_input_end();     /* commits the frame's input to Nuklear */
        }

        /* Continuous key state. While the menu is up the keyboard belongs
         * to it, so nothing here runs. */
        if (!headless && !ui_is_open()) {
            const Uint8 *keys = SDL_GetKeyboardState(NULL);
            static Uint32 last_ticks;
            Uint32 now = SDL_GetTicks();
            float dt = last_ticks ? (now - last_ticks) / 1000.0f : 0.016f;
            last_ticks = now;
            if (dt > 0.1f) dt = 0.1f;          /* don't lurch after a stall */

            if (ui_map_active()) {
                /* Map viewer owns WASD/QE here. This call was missing
                 * entirely, so the pan keys were dead: ui_map_input() was
                 * defined and declared but never invoked, and the block
                 * below drives the LEGACY camera, which the geo_hw map
                 * view does not read. */
                ui_map_input(keys, dt);
            } else {
                float spd = 100.0f;
                if (keys[SDL_SCANCODE_LSHIFT]) spd = 1000.0f;
                if (keys[SDL_SCANCODE_W]) renderer3d_move_camera(0, 0, spd);
                if (keys[SDL_SCANCODE_S]) renderer3d_move_camera(0, 0, -spd);
                if (keys[SDL_SCANCODE_A]) renderer3d_move_camera(-spd, 0, 0);
                if (keys[SDL_SCANCODE_D]) renderer3d_move_camera(spd, 0, 0);
                if (keys[SDL_SCANCODE_Q]) renderer3d_move_camera(0, spd, 0);
                if (keys[SDL_SCANCODE_E]) renderer3d_move_camera(0, -spd, 0);
            }
        }

        /* Signal vblank to game logic */
        g_sys.vblank_pending = true;

        /* Poll input → MCU shared RAM */
        input_poll();

        /* PROPCYCL_STAGEDBG: follow W[0x0E0C] (the COURSE) through the whole
         * menu chain, printing on every change of (state, sub, course).
         *
         * THE COURSE SELECTION WORKS. `PROPCYCL_TEST_STAGE=N` sets the stage
         * cursor, `FUN_00008c82` maps it through the ROM table at W[0x0C7C],
         * and the course survives sub 1 -> 2 -> 5 -> 3 into gameplay: this
         * trace reads `f1803 state=3 sub=1 course=2` and still `course=2` at
         * sub 3. What did NOT work was the MEASUREMENT -- flight_rec.c wrote
         * `course=` into its header, and the header is written when the
         * recorder STARTS, which for PROPCYCL_RECORD is frame 0, thousands of
         * frames before the stage select has run. Every recording therefore
         * said course 0 whatever was selected, and that read as "no flag ever
         * reaches another course". The course is now on each F line instead.
         *
         * `--autostart N` is a different thing and genuinely does not select
         * a course -- see the PROPCYCL_COURSE note in CLAUDE.md. Use
         * PROPCYCL_TEST_STAGE to reach a course headlessly, and confirm with
         * this trace at sub 3, never at a menu frame. */
        { static int on = -1; extern intptr_t _W[];
          if (on < 0) on = getenv("PROPCYCL_STAGEDBG") ? 1 : 0;
          if (on) { static long pc = -99, ps = -99, pb = -99;
              long st = (long)_W[0x0CBC], sub = (long)_W[0x0CC0], co = (long)_W[0x0E0C];
              if (co != pc || st != ps || sub != pb) {
                  fprintf(stderr, "[COURSE] f%u state=%ld sub=%ld course=%ld\n",
                          g_sys.frame_count, st, sub, co);
                  pc = co; ps = st; pb = sub; } } }

        /* PROPCYCL_TEST_MENUSTICK=1: pulse the handlebar left/right while a
         * menu is up and log the cursor. The menus are the one place a stick
         * PULSE matters rather than a held deflection -- the cursor moves on
         * the EDGE of the direction flag -- so the steer square wave cannot
         * exercise them and nothing headless ever had. */
        { static int on = -1; extern intptr_t _W[];
          if (on < 0) { const char *e = getenv("PROPCYCL_TEST_MENUSTICK"); on = e ? atoi(e) : 0; if (e && on == 0) on = 1; }
          if (on) {
              long st = (long)_W[0x0CBC], sub = (long)_W[0x0CC0];
              int menu = (st == 3 && (sub == 12 || sub == 13 || sub == 0 || sub == 1));
              if (menu && on >= 2) {
                  unsigned ph = g_sys.frame_count % 120;
                  extern void input_force_analog(int x, int y);
                  input_force_analog(ph < 40 ? 0x0BF : (ph < 80 ? 0x33F : 0x1FF), 0x1FF);
              }
              static long psub = -1, pcur = -1;
              long cur = (long)(int16_t)_W[0x0C82];
              static long pdir = -1; long dnow = (long)_W[0x2BD8];
              if (menu && (sub != psub || cur != pcur || dnow != pdir)) { pdir = dnow;
                  fprintf(stderr, "[MENUSTK] f%u sub=%ld cursor=%ld/max=%ld dirHI=%04lX heldLO=%04lX m2BDC=%08lX adc=%ld ctr=%ld\n",
                          g_sys.frame_count, sub, cur, (long)(int16_t)_W[0x0C80],
                          ((long)_W[0x2BD8] >> 16) & 0xFFFF, (long)_W[0x2BD8] & 0xFFFF,
                          (long)_W[0x2BDC] & 0xFFFFFFFF, (long)_W[0x2BC8], (long)_W[0x3FD0]);
                  psub = sub; pcur = cur;
              }
          } }

        /* --autostart: force stage start at frame 10.
         *
         * This used to be gated on !headless, so a --screenshot run never
         * entered state 4 (stage_start_init) at all -- it fell through to
         * state 3 and skipped the whole stage-start sequence, including
         * the starting platform. That made every headless capture
         * unrepresentative of what the interactive game shows, which is
         * exactly the sequence being debugged. Same path both ways now. */
        if (autostart_course >= 0 && g_sys.frame_count == 10) {
            if (propcycl_verbose()) printf("[AUTO] Forcing stage start (course %d)\n", autostart_course);
            g_sys.work_ram[0x0E0C]=0; g_sys.work_ram[0x0E0D]=0;
            g_sys.work_ram[0x0E0E]=0; g_sys.work_ram[0x0E0F]=(uint8_t)autostart_course;
            g_sys.work_ram[0x0CBC]=0; g_sys.work_ram[0x0CBD]=0;
            g_sys.work_ram[0x0CBE]=0; g_sys.work_ram[0x0CBF]=4;
            g_sys.work_ram[0x0D24]=0; g_sys.work_ram[0x0D25]=0;
            g_sys.work_ram[0x0D26]=0; g_sys.work_ram[0x0D27]=0;
            g_sys.work_ram[0x0C98]=0; g_sys.work_ram[0x0C99]=0;
            g_sys.work_ram[0x0C9A]=0; g_sys.work_ram[0x0C9B]=0;
        }

        /* PROPCYCL_LEVEL / --level: start a stage directly at a frame
         * (default 10), the headless form of Escape -> Levels. */
        { static int done; static long at = -1; static int adv, course;
          if (!done) { done = 1;
              extern int level_select_parse(const char *, int *, int *, long *);
              if (!level_select_parse(getenv("PROPCYCL_LEVEL"), &adv, &course, &at)) at = -1; }
          if (at >= 0 && (long)g_sys.frame_count == at) {
              extern void level_select_request(int adv, int course);
              level_select_request(adv, course); } }
        /* PROPCYCL_LEVEL=end:<clear|bad>[:phase][@frame]: straight to the
         * story ENDING (level_select.c); PROPCYCL_ENDING_TOTAL sets the total. */
        { static int done; static long at = -1; static int cleared, phase;
          if (!done) { done = 1;
              extern int level_select_parse_ending(const char *, int *, int *, long *);
              if (!level_select_parse_ending(getenv("PROPCYCL_LEVEL"), &cleared, &phase, &at)) at = -1; }
          if (at >= 0 && (long)g_sys.frame_count == at) {
              extern void level_select_ending(int cleared, int phase, int32_t total);
              const char *t = getenv("PROPCYCL_ENDING_TOTAL");
              level_select_ending(cleared, phase, (t && *t) ? (int32_t)atol(t) : 8995); } }

        /* Headless: PROPCYCL_TEST_MENU=1 → jump to title state and kick the
         * recreated arcade mode-select menu so we can screenshot it. */
        if (headless && g_sys.frame_count == 10
            && getenv("PROPCYCL_TEST_MENU") != NULL) {
            printf("  [SIM] Kicking arcade mode-select menu at frame 10\n");
            /* Two-step:
             *   step 1: state=6 once so state_title_init runs (sets up
             *           tilemap scroll + scene_load + cz_ram_init).
             *   step 2 (next frame): we'll set sub-state=1 explicitly so
             *           state_title_run dispatches to the classic title
             *           screen path that populates sprites. The init
             *           function clears bit 0 of W[0x3FB4], so we must
             *           set sub-state AFTER init has run. */
            g_sys.work_ram[0x0CBC]=0; g_sys.work_ram[0x0CBD]=0;
            g_sys.work_ram[0x0CBE]=0; g_sys.work_ram[0x0CBF]=6;
            /* Credit count = 1 (W[0x2C0E]) so menu trigger gate accepts.
             * Written to _W[] directly: W[0x2C0E] is a pinned slot (see
             * game_stubs.c), so a work_ram poke would never reach it. */
            _W[0x2C0E] = 1;
            /* sub-state and MENU_PHASE deferred to frame 12 — set after
             * state_title_init has run (state=6 → 7) and cleared
             * W[0x3FB4]&0xfffe. */
        }
        /* Frame 12: now state=7 (init advanced it). Force sub-state to
         * dispatch a specific title path. Defaults to 1 (classic title);
         * PROPCYCL_TEST_TITLE_SUB=N overrides to N (0x00..0x20 cover all
         * title_run case values — service menu, ROM test, etc.).
         * PROPCYCL_TEST_MENU_PHASE=N additionally arms the arcade
         * mode/course-select menu. */
        if (headless && g_sys.frame_count == 12
            && getenv("PROPCYCL_TEST_MENU") != NULL) {
            const char* sp = getenv("PROPCYCL_TEST_TITLE_SUB");
            int sub_title = sp ? (int)strtol(sp, NULL, 0) : 1;
            g_sys.work_ram[0x3FB4]=0; g_sys.work_ram[0x3FB5]=0;
            g_sys.work_ram[0x3FB6]=(uint8_t)((sub_title >> 8) & 0xFF);
            g_sys.work_ram[0x3FB7]=(uint8_t)(sub_title & 0xFF);
            const char* p = getenv("PROPCYCL_TEST_MENU_PHASE");
            int ph = p ? atoi(p) : 0;
            g_sys.work_ram[0x16A40]=0; g_sys.work_ram[0x16A41]=0;
            g_sys.work_ram[0x16A42]=0; g_sys.work_ram[0x16A43]=(uint8_t)ph;
            printf("  [SIM] frame 12: sub_title=0x%X, menu_phase=%d\n",
                   sub_title, ph);
        }

        /* Headless: force stage start at frame 10. Course selection via
         * PROPCYCL_COURSE env var (0-3), defaults to 0.
         * Set PROPCYCL_NO_AUTOSTART=1 to skip and observe the real attract
         * sequence (Namco logo → cinematic flyover → demo gameplay → ...). */
        if (headless && g_sys.frame_count == 10
            && getenv("PROPCYCL_NO_AUTOSTART") == NULL
            && getenv("PROPCYCL_TEST_MENU") == NULL) {
            const char* env_course = getenv("PROPCYCL_COURSE");
            int hl_course = env_course ? atoi(env_course) : 0;
            if (hl_course < 0 || hl_course > 3) hl_course = 0;
            printf("  [SIM] Forcing stage start (course %d) at frame 10\n", hl_course);
            g_sys.work_ram[0x0E0C]=0; g_sys.work_ram[0x0E0D]=0;
            g_sys.work_ram[0x0E0E]=0; g_sys.work_ram[0x0E0F]=(uint8_t)hl_course;
            g_sys.work_ram[0x0CBC]=0; g_sys.work_ram[0x0CBD]=0;
            g_sys.work_ram[0x0CBE]=0; g_sys.work_ram[0x0CBF]=4;
            g_sys.work_ram[0x0D24]=0; g_sys.work_ram[0x0D25]=0;
            g_sys.work_ram[0x0D26]=0; g_sys.work_ram[0x0D27]=0;
            g_sys.work_ram[0x0C98]=0; g_sys.work_ram[0x0C99]=0;
            g_sys.work_ram[0x0C9A]=0; g_sys.work_ram[0x0C9B]=0;
            /* PROPCYCL_STORY=1: ADVANCED/story mode (W[0x3FF4]=0) instead of
             * the NOVICE single-stage flag -- for reproducing the
             * course-to-course cutscene/results progression headlessly. */
            if (getenv("PROPCYCL_STORY")) {
                g_sys.work_ram[0x3FF4]=0; g_sys.work_ram[0x3FF5]=0;
                g_sys.work_ram[0x3FF6]=0; g_sys.work_ram[0x3FF7]=0;
            }
        }

        /* Log game state every 10 frames in headless */
        if (headless && g_sys.frame_count % 10 == 0) {
            uint32_t state = (g_sys.work_ram[0x0CBC] << 24) | (g_sys.work_ram[0x0CBD] << 16) |
                             (g_sys.work_ram[0x0CBE] << 8) | g_sys.work_ram[0x0CBF];
            uint32_t sub = (g_sys.work_ram[0x0CC0] << 24) | (g_sys.work_ram[0x0CC1] << 16) |
                           (g_sys.work_ram[0x0CC2] << 8) | g_sys.work_ram[0x0CC3];
            printf("  frame %d: state=%d sub=%d\n", g_sys.frame_count, state, sub);
        }

        /* Run one game frame */
        /* A --framedump replay draws captured MAME state, so the game's own
         * logic contributes NOTHING to the picture -- but it still runs, and
         * it is unstable: the attract path floods math_atan2 with
         * divide-by-zero and segfaults intermittently around frame 580-680.
         * That took the replay down with it. Skip it: replaying a capture
         * and running the game loop are separate jobs.
         * PROPCYCL_FRAMEDUMP_RUNGAME=1 restores it for debugging. */
        /* PROPCYCL_TEST_PAUSE=<frame>: pause headlessly at a frame, exactly
         * as the P key does. A user watched the lake surface blink on and off
         * WHILE PAUSED -- game state frozen, picture changing -- which makes
         * it a renderer defect and not a game one, and makes it reproducible
         * without a window or a hand on the keyboard. */
        { static long pf = -2;
          if (pf == -2) { const char *e = getenv("PROPCYCL_TEST_PAUSE");
                          pf = e ? atol(e) : -1; }
          if (pf >= 0 && (long)g_sys.frame_count == pf) {
              g_paused = true; { extern int g_pausecam_on; g_pausecam_on = 1; } report_player_position(); } }
        /* PROPCYCL_WARP=x,y,z[,frame[,heading]]: force the player to a world position
         * at a given frame (default: any frame >= 0, i.e. as soon as
         * gameplay is running), for testing collision/zone features whose
         * ROM position is known but that no scripted input reaches -- e.g.
         * an 0xE0/E1/E5 spray-trigger zone. Sets speed to 0 and leaves
         * everything else (heading, pitch) alone. */
        /* PROPCYCL_WARPSEQ=<hold>@x,y,z;x,y,z;... -- warp to each point in
         * turn, holding it `hold` frames, in ONE run. A single-point warp
         * costs a whole ~3300-frame boot to gameplay, so a 78-point sweep of
         * the tunnel cells is hours; this makes it one run.
         *
         * `hold` must be generous. THE PER-LAYER TRIANGLE CACHE NEEDS TIME
         * TO SETTLE: `W[0x28F0 + layer*4]` holds up to two cached triangles
         * per layer and `world_object_tick` consults them BEFORE calling
         * `terrain_cell_find_triangle`, so for the first frames after a warp
         * the column is resolved against triangles belonging to wherever the
         * player just was. Sampling three frames in reports a column that is
         * not the one the game settles on -- measured at cell 43's tunnel
         * mouth, three frames in reads `faces=1 contact=1` (a wall that is
         * not there) where fifty frames in reads `faces=3 contact=0`, which
         * is what MAME gives at the same point. 50 is the working minimum. */
        { extern intptr_t _W[];
          static long wx=0, wy=0, wz=0, wf=-2, wh=-1;
          static char *seq = NULL; static long seq_hold = 0, seq_next = 0; static int seq_done = 0;
          static char seq_buf[8192]; static char *seq_cur = NULL;
          if (wf == -2) { const char *e = getenv("PROPCYCL_WARP");
                          wf = -1;
                          if (e) { long f = 0; int n = sscanf(e, "%ld,%ld,%ld,%ld,%ld", &wx,&wy,&wz,&f,&wh);
                                    if (n >= 3) wf = (n >= 4) ? f : 0;
                                    if (n < 5) wh = -1; }
                          e = getenv("PROPCYCL_WARPSEQ");
                          if (e) { const char *at = strchr(e, '@');
                                   seq_hold = at ? atol(e) : 60;
                                   if (seq_hold < 1) seq_hold = 60;
                                   snprintf(seq_buf, sizeof seq_buf, "%s", at ? at + 1 : e);
                                   seq = seq_buf; seq_cur = seq_buf; } }
          /* advance the sequence: arm the next point when the hold expires */
          if (seq && !seq_done && (int32_t)_W[0x0CBC] == 3 && (int32_t)_W[0x0CC0] == 3
              && (long)g_sys.frame_count >= seq_next) {
              char *semi = seq_cur ? strchr(seq_cur, ';') : NULL;
              if (semi) *semi = 0;
              if (seq_cur && *seq_cur
                  && sscanf(seq_cur, "%ld,%ld,%ld", &wx, &wy, &wz) == 3) {
                  wf = (long)g_sys.frame_count;
                  seq_next = (long)g_sys.frame_count + seq_hold;
                  fprintf(stderr, "[WARPSEQ] point (%ld,%ld,%ld) held %ld frames from f%u\n",
                          wx, wy, wz, seq_hold, g_sys.frame_count);
                  seq_cur = semi ? semi + 1 : NULL;
                  if (!seq_cur || !*seq_cur) seq_done = 1;
              } else { seq_done = 1; }
          }
          if (wf >= 0 && (int32_t)_W[0x0CBC] == 3 && (int32_t)_W[0x0CC0] == 3 && (long)g_sys.frame_count >= wf) {
              /* Write g_sys.work_ram (big-endian), not just _W[] -- game_frame()
               * calls sync_wram_to_W() as its first step, which rebuilds _W[]
               * FROM work_ram every frame, silently discarding a _W[]-only
               * write before game logic ever sees it. */
              #define WARP_SET32(off, v) do { \
                  g_sys.work_ram[(off)]   = (uint8_t)(((uint32_t)(v)) >> 24); \
                  g_sys.work_ram[(off)+1] = (uint8_t)(((uint32_t)(v)) >> 16); \
                  g_sys.work_ram[(off)+2] = (uint8_t)(((uint32_t)(v)) >> 8);  \
                  g_sys.work_ram[(off)+3] = (uint8_t)((uint32_t)(v)); } while(0)
              WARP_SET32(0x0D00, (int32_t)wx);
              WARP_SET32(0x0D04, (int32_t)wy);
              WARP_SET32(0x0D08, (int32_t)wz);
              WARP_SET32(0x0D48, 0);
              _W[0x0D00] = (int32_t)wx; _W[0x0D04] = (int32_t)wy; _W[0x0D08] = (int32_t)wz;
              _W[0x0D48] = 0;
              /* optional 5th field: a HEADING (0..65535), written to the same
               * three slots tools/overnight/probe_objects.lua writes on MAME --
               * camera heading, camera target heading and player heading -- so
               * a live frame and a MAME capture can be framed identically. */
              if (wh >= 0 && !seq) {
                  WARP_SET32(0x0CEC, (int32_t)wh); _W[0x0CEC] = (int32_t)wh;
                  WARP_SET32(0x0DAC, (int32_t)wh); _W[0x0DAC] = (int32_t)wh;
                  WARP_SET32(0x0DA0, (int32_t)wh); _W[0x0DA0] = (int32_t)wh;
              }
              #undef WARP_SET32
              if (!seq) { wf = -1;
                          fprintf(stderr, "[WARPDBG] WARPED at f=%u\n", g_sys.frame_count); } } }
        if (g_paused) {
            /* Hold the game where it is. The renderer still runs below, so the
             * paused frame keeps being drawn and the window stays live; only
             * the simulation is frozen. The pause camera moves meanwhile. */
            if (!ui_is_open()) { extern void input_pausecam_update(void); input_pausecam_update(); }
            /* PROPCYCL_TEST_PAUSECAM=<yaw>,<pitch>,<dist>: the headless form,
             * for a --screenshot taken while paused (with PROPCYCL_TEST_PAUSE). */
            { static int done; static float ty, tp, td; static int have;
              if (!done) { done = 1; const char *e = getenv("PROPCYCL_TEST_PAUSECAM");
                           if (e) have = sscanf(e, "%f,%f,%f", &ty, &tp, &td) >= 2; }
              if (have) { extern float g_pausecam_yaw, g_pausecam_pitch, g_pausecam_dist;
                          g_pausecam_yaw = ty; g_pausecam_pitch = tp; g_pausecam_dist = td; } }
        } else if (!framedump_path || getenv("PROPCYCL_FRAMEDUMP_RUNGAME")) {
            /* Reset the billboard/banner picker lists before this frame's
             * game logic (which is what populates them) runs, so they stay
             * stable -- and last frame's snapshot readable -- while paused,
             * matching the object picker's own "frozen while paused"
             * convention (objlist_build() by contrast is rebuilt every
             * render call from data the SAME frame's game_frame() already
             * produced, so it needs no separate reset here). */
            extern void billboard_calls_reset(void); billboard_calls_reset();
            extern void banner_calls_reset(void); banner_calls_reset();
            /* Flight replay pins the recorded position before the game
             * runs, so the collision column is resolved exactly where the
             * player was; the recorder then writes whatever the column came
             * out as. Both are no-ops unless armed. */
            { extern void flight_replay_pin(void); flight_replay_pin(); }
            /* Advance the pedal pulse counter exactly once per SIMULATED
             * frame. It used to advance in input_poll(), which runs even when
             * this branch does not -- so a pause banked thrust and delivered
             * it in a single frame on resume. See src/input.c. */
            { extern void input_pedal_step(void); input_pedal_step(); }
            double _t=perf_now(); game_frame(); g_perf_game += perf_now()-_t;  /* frame_count incremented below */
            { extern void flight_rec_tick(void);      flight_rec_tick();
              extern void flight_replay_report(void); flight_replay_report(); }
        }
        audio_hle_tick();
        /* PROPCYCL_SNDCMDLOG=<path>: the six sound-command words the 68K
         * writes to the MCU every frame, so ours can be diffed against
         * MAME's own stream (tools/overnight/cap_sound.lua's CPU half). */
        { extern FILE *g_sndcmd_f;
          FILE *f = g_sndcmd_f;
          if (f) {
              const uint8_t *c = g_sys.commsram;
              #define CW(o) ((c[(o)] << 8) | c[(o)+1])
              fprintf(f, "%u %04X %04X %04X %04X %04X %04X %04X\n",
                      g_sys.frame_count, CW(0x0102), CW(0x0104), CW(0x010A),
                      CW(0x010C), CW(0x010E), CW(0x0110), CW(0x7D2E));
              #undef CW
          } }
        flight_log();

        /* Dump palette RAM after a few frames of init */
        if (g_sys.frame_count == 5) {
            FILE *pf = fopen("palette_dump.bin", "wb");
            if (pf) {
                for (int i = 0; i < 8192; i++) {
                    uint8_t r = g_sys.palette_ram[i];
                    uint8_t g = g_sys.palette_ram[0x8000 + i];
                    uint8_t b = g_sys.palette_ram[0x10000 + i];
                    fputc(r, pf); fputc(g, pf); fputc(b, pf);
                }
                fclose(pf);
                if (propcycl_verbose())
                    printf("PALETTE DUMPED to palette_dump.bin (8192 entries)\n");
            }
        }

        /* Render — clear color comes from videomix[0x08..0x0A], which is
         * what set_background_color() writes to. Falls back to slight blue
         * tint if videomix hasn't been initialized yet. */
        {
            uint8_t bgr = g_sys.videomix[0x08];
            uint8_t bgg = g_sys.videomix[0x09];
            uint8_t bgb = g_sys.videomix[0x0A];
            /* replaying a capture: use that frame's own mixer bg */
            extern int renderer3d_framedump_bg(uint8_t *, uint8_t *, uint8_t *);
            if (framedump_path) renderer3d_framedump_bg(&bgr, &bgg, &bgb);
            { extern int g_seamtest; if (g_seamtest) { bgr = 255; bgg = 0; bgb = 255; } }
            if (bgr == 0 && bgg == 0 && bgb == 0 && g_sys.frame_count < 2) {
                glClearColor(0.0f, 0.0f, 0.1f, 1.0f);
            } else {
                glClearColor(bgr / 255.0f, bgg / 255.0f, bgb / 255.0f, 1.0f);
            }
        }
        /* Viewport: honour the chosen aspect. The scene is a fixed 640x480,
         * so a non-4:3 window either stretches it or gets bars. Clear the
         * whole window black first so the bars are bars, not stale pixels. */
        if (!headless) {
            /* DRAWABLE pixels, not window units: on a scaled (HiDPI /
             * Wayland fractional) desktop they differ, and a viewport in
             * window units covered only part of the picture. */
            /* The chosen RESOLUTION is the size drawn here; rt_end() scales
             * it to the window, which keeps its own size (render_target.c). */
            int ww, wh;
            { int rw, rh; ui_render_res(&rw, &rh); rt_begin(window, rw, rh, &ww, &wh); }
            float want = ui_aspect();
            glDisable(GL_SCISSOR_TEST);
            glViewport(0, 0, ww, wh);
            glClearColor(0, 0, 0, 1);
            glClear(GL_COLOR_BUFFER_BIT);
            /* WIDESCREEN: the 3D scene widens to the window's own shape
             * (renderer_3d.c g_scene_x0/x1); a window narrower than 4:3
             * just falls back to 4:3 with bars. */
            { extern float g_scene_x0, g_scene_x1;
              g_scene_x0 = 0.0f; g_scene_x1 = (float)SCREEN_WIDTH;
              if (want < 0.0f) {
                  /* A WHOLE number of scene pixels per side: the clip and
                   * scissor code works in integers, and a fractional edge
                   * left a sub-pixel sliver there. The <1/640 aspect error
                   * this rounds away is invisible. */
                  int E = (int)(((float)SCREEN_HEIGHT * ww / (wh > 0 ? wh : 1) - SCREEN_WIDTH) / 2.0f + 0.5f);
                  if (E > 0) {
                      g_scene_x0 = (float)-E;
                      g_scene_x1 = (float)(SCREEN_WIDTH + E);
                  } else want = 4.0f / 3.0f;
              } }
            if (want > 0.0f) {
                int vw = ww, vh = (int)(ww / want + 0.5f);
                if (vh > wh) { vh = wh; vw = (int)(wh * want + 0.5f); }
                glViewport((ww - vw) / 2, (wh - vh) / 2, vw, vh);
            }
        }
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        /* PROPCYCL_NO_3D / PROPCYCL_NO_2D: isolate a layer. Added because a
         * blank-looking frame cannot tell you WHICH layer failed, and the
         * attract screen turned out to be almost entirely 3D. */
        /* PROPCYCL_TEST_WARP=<frame>:<x>,<y>,<z>[,<heading>] -- put the player
         * THERE and carry on.
         *
         * This file's own rule is that a headless pass proves nothing about a
         * defect the player's POSITION gates (register row 90), and a reported
         * one is usually somewhere a scripted input never reaches: a user
         * reported an object flickering at cell 90 (x=240862 z=1107315) while
         * the furthest any autopilot or steer profile here had ever got was
         * z=945921. Rather than fly there, warp there -- the numbers come
         * straight off the P key's [POS] report, so a bug report turns into a
         * reproduction with no guessing.
         *
         * The cell, the terrain layer records, the LOD tier and the zone list
         * are all recomputed from the position each frame, so writing it is
         * enough; nothing else needs resetting. */
        { static long wf = -2; static long wx, wy, wz, wh; static int whset;
          if (wf == -2) {
              const char *e = getenv("PROPCYCL_TEST_WARP");
              wf = -1;
              if (e) {
                  const char *c = strchr(e, ':');
                  if (c) { wf = atol(e);
                           whset = (sscanf(c + 1, "%ld,%ld,%ld,%ld", &wx, &wy, &wz, &wh) == 4); }
              }
          }
          if (wf >= 0 && (long)g_sys.frame_count == wf) {
              /* Write WORK_RAM, not _W[]. _W is rebuilt from work_ram by
               * sync_wram_to_W at the top of every frame, so a warp written
               * into _W is gone before the game reads it -- measured: the
               * player was back on its own trajectory at cell 27 ten frames
               * after a warp to cell 90. Big-endian 32-bit, like the
               * --autostart block above. */
              #define WARP_PUT(off, v) do { long _v = (long)(v); \
                  g_sys.work_ram[(off)+0] = (uint8_t)(_v >> 24); \
                  g_sys.work_ram[(off)+1] = (uint8_t)(_v >> 16); \
                  g_sys.work_ram[(off)+2] = (uint8_t)(_v >> 8);  \
                  g_sys.work_ram[(off)+3] = (uint8_t)(_v); } while (0)
              WARP_PUT(0x0D00, wx); WARP_PUT(0x0D04, wy); WARP_PUT(0x0D08, wz);
              /* The CAMERA has to come too. terrain_chunk_visibility is driven
               * by the camera position, not the player's, and camera_update
               * only smooths toward the player a sixteenth at a time -- warping
               * the player alone left the camera thousands of cells behind and
               * the frame drew nothing but the sky for 581 frames. */
              WARP_PUT(0x0CDC, wx); WARP_PUT(0x0CE0, wy); WARP_PUT(0x0CE4, wz);
              if (whset) { WARP_PUT(0x0DAC, wh); WARP_PUT(0x0CEC, wh); }
              #undef WARP_PUT
              printf("[WARP] f%ld -> player (%ld,%ld,%ld)%s  cell=%ld\n",
                     wf, wx, wy, wz, whset ? " heading set" : "",
                     (long)((wx / 0x18000) + (wz / 0x18000) * 8));
              fflush(stdout);
          } }

        /* PROPCYCL_TEST_COIN diagnostics: show the coin travelling from
         * commsram through the game's own MCU read into W[]. */
        if (getenv("PROPCYCL_TEST_COIN")) {
            extern intptr_t _W[];
            static int shown = 0;
            unsigned sv = (unsigned)_W[0x2B80], ed = (unsigned)_W[0x2B82];
            long at = atol(getenv("PROPCYCL_TEST_COIN"));
            if (g_sys.frame_count >= at - 2 && g_sys.frame_count <= at + 8) {
                (void)shown;
                printf("  [COIN] f=%d commsram[7D02]=%02X W[2B80]=%04X "
                       "edge W[2B82]=%04X credits W[2C14]=%ld\n",
                       g_sys.frame_count, g_sys.commsram[0x7D02], sv, ed,
                       (long)_W[0x2C14]);
                shown++;
            }
        }
        if (getenv("PROPCYCL_NO_3D") == NULL) { double _t=perf_now(); renderer3d_render_frame(); g_perf_render += perf_now()-_t; }
        /* PROPCYCL_PERFFRAME=<ms>: log every frame slower than <ms>, with the
         * phase split, so a stutter can be attributed instead of guessed at. */
        { static double _pf = -1; static double _lastg, _lastr, _lastp, _lastf, _lastx;
          if (_pf < 0) { const char *e = getenv("PROPCYCL_PERFFRAME"); _pf = e ? atof(e) : 0; }
          if (_pf > 0) {
              extern double g_perf_pdp, g_perf_flush, g_perf_txt;
              extern int geohw_quads_drawn, tex_frame_hits, tex_frame_misses, tex_cache_evictions;
              extern size_t tex_cache_bytes;
              double dg = g_perf_game-_lastg, dr = g_perf_render-_lastr;
              double dp = g_perf_pdp-_lastp, df = g_perf_flush-_lastf, dx = g_perf_txt-_lastx;
              if ((dg+dr)*1000.0 >= _pf)
                  fprintf(stderr, "[SLOW] f%d %.2fms = game %.2f + render %.2f "
                          "(walk %.2f flush %.2f text %.2f)  quads %d  tex hit %d miss %d evict %d\n",
                          (int)g_sys.frame_count, (dg+dr)*1000, dg*1000, dr*1000,
                          dp*1000, df*1000, dx*1000, geohw_quads_drawn,
                          tex_frame_hits, tex_frame_misses, tex_cache_evictions, tex_cache_bytes>>20);
              _lastg=g_perf_game; _lastr=g_perf_render; _lastp=g_perf_pdp;
              _lastf=g_perf_flush; _lastx=g_perf_txt;
          } }
        /* A captured polygon-RAM dump contains ONLY 3D geometry -- no
         * sprite list, no text layer. Drawing the 2D layer during framedump
         * playback therefore composites whatever stale state the game loop
         * happens to hold, which is where the stray Japanese glyphs came
         * from: the sprite path resolves ids 0x14A-0x151 as raw 32x32 tiles
         * and those land in the Japanese font region of the sprite ROM
         * (tools/l3/BASELINE.md, title-logo pass). The reference rasteriser
         * draws 3D only, so this also makes our output directly comparable.
         * PROPCYCL_FORCE_2D=1 puts it back for debugging. */
        /* Skip the legacy renderer_2d layer whenever something better is
         * already drawing 2D:
         *   - a framedump replay (a capture has no live sprite/text state,
         *     so this would composite stale game RAM),
         *   - the map viewer (a geometry inspector -- the stray Japanese
         *     glyphs over the map were this path drawing a text RAM the
         *     game never filled),
         *   - and the LIVE GAME once sprite_hw/text_hw have live state.
         *
         * That last case is why live gameplay looked wrecked: BOTH 2D
         * paths were running. text_hw/sprite_hw are pixel-exact ports
         * (100.00% against their reference models) and already draw the
         * text layer live; renderer_2d then painted over the result with
         * the magenta blocks of its known-broken fade/composite, plus
         * sprite ids resolved as raw 32x32 tiles that land in the
         * Japanese font region of the sprite ROM -- the stray glyphs.
         * PROPCYCL_FORCE_2D=1 still forces the legacy layer back on. */
        int fd_mode = ((framedump_path != NULL) || ui_map_active() ||
                       renderer3d_live_2d_ok()) &&
                      (getenv("PROPCYCL_FORCE_2D") == NULL);
        if (!fd_mode) {
            if (getenv("PROPCYCL_NO_2D") == NULL) renderer2d_draw_tilemap();
            renderer2d_composite();
        }

        /* the frame is finished: scale it from the render target into the window */
        if (!headless) rt_end(window, ui_aspect() == 0.0f);

        {
            /* LAST: the Nuklear backend saves/restores GL state around its
             * own draw, so anything after it would be fighting that. */
            bool ui_quit = false;
            ui_draw(window, &ui_quit);
            if (ui_quit) running = false;
        }
        /* Before the swap: the back buffer holds the finished frame,
         * UI included. Headless has a real GL context too (offscreen
         * driver), so a frame-triggered capture works there as well. */
        if (shot_pending ||
            (shot_every > 0 && g_sys.frame_count % (unsigned)shot_every == 0)) {
            shot_pending = false;
            screenshot_capture(window, shot_dir, g_sys.frame_count);
            { extern void blinklog_mark(const char *, unsigned);
              blinklog_mark("screenshot", g_sys.frame_count); }
        }

        /* PROPCYCL_VOIDLOG=<path|1>: report BLACK HOLES in the picture.
         *
         * A user reported the water above a cave turning off and on. None of
         * the display-list detectors could see it -- not BLINK (object dropped),
         * not CULLED (object emitted, every quad culled), not PARTIAL (object
         * keeps drawing but loses most of its faces) -- and their screenshot
         * showed why: a large area of the frame is simply BLACK, i.e. nothing
         * was ever there to lose. So measure the SYMPTOM instead of guessing at
         * a cause, and report it with the position needed to reproduce it.
         *
         * Their frame was 20.2% near-black against a 0.9% baseline, so the
         * signal is enormous and a coarse threshold is fine. The readback costs
         * a glReadPixels, hence every Nth frame and only when asked for.
         *   PROPCYCL_VOIDPCT=<n>  threshold, default 8
         *   PROPCYCL_VOIDEVERY=<n> frame interval, default 4 */
        { static int vinit, vevery = 4, vpct = 8; static FILE *vfp;
          static unsigned char *vbuf; static size_t vcap; extern intptr_t _W[];
          if (!vinit) {
              const char *e = getenv("PROPCYCL_VOIDLOG");
              vinit = 1;
              if (e && *e && strcmp(e, "0")) {
                  vfp = (!strcmp(e, "1")) ? stderr : fopen(e, "w");
                  if (!vfp) vfp = stderr;
                  { const char *t = getenv("PROPCYCL_VOIDPCT");   if (t) vpct = atoi(t); }
                  { const char *t = getenv("PROPCYCL_VOIDEVERY"); if (t) vevery = atoi(t); }
                  if (vevery < 1) vevery = 1;
                  { int dw, dh; SDL_GL_GetDrawableSize(window, &dw, &dh);
                    if (dw < 1) dw = SCREEN_WIDTH; if (dh < 1) dh = SCREEN_HEIGHT;
                    vcap = (size_t)dw * dh * 3;
                    vbuf = malloc(vcap); if (!vbuf) vcap = 0; }
                  fprintf(vfp, "# void log: report frames >= %d%% near-black, "
                               "sampled every %d frames\n", vpct, vevery);
              }
          }
          /* GAMEPLAY ONLY. The attract, tutorial and menu screens are
           * legitimately dark -- the pre-gameplay frames alone produced
           * hundreds of 13-14%% reports -- and a detector that cries wolf
           * through the whole boot is not one anybody will read. */
          if (vfp && vbuf && (g_sys.frame_count % (unsigned)vevery) == 0 &&
              _W[0x0CBC] == 3 && _W[0x0CC0] == 3) {
              /* THE WHOLE DRAWABLE, not a corner of it. The window is
               * SCREEN_WIDTH*2 wide, and clamping the readback to
               * SCREEN_WIDTH x SCREEN_HEIGHT sampled the bottom-LEFT QUADRANT
               * -- which is exactly where this defect appears, so it read 27%
               * black on a frame whose full image is 1.3%. A detector that
               * silently measures a quarter of the picture is worse than none. */
              int vw, vh; SDL_GL_GetDrawableSize(window, &vw, &vh);
              if (vw < 1) vw = SCREEN_WIDTH; if (vh < 1) vh = SCREEN_HEIGHT;
              /* the window can be resized (or switched resolution) since vbuf was sized */
              { size_t need = (size_t)vw * vh * 3;   /* vcap is the size actually allocated */
                if (need > vcap) { unsigned char *nb = realloc(vbuf, need); if (nb) { vbuf = nb; vcap = need; } else { vw = 0; vh = 0; }   /* no room: skip this sample */ } }
              glPixelStorei(GL_PACK_ALIGNMENT, 1);   /* any window width (see save_screenshot) */
              glReadPixels(0, 0, vw, vh, GL_RGB, GL_UNSIGNED_BYTE, vbuf);
              /* Threshold where a SCREENSHOT would, not where the raw buffer
               * does. screenshot_capture() applies the final-stage gamma on
               * readback and the window does not, so the same frame measured
               * 23% black raw and 4.2% in its own saved PNG. Rather than gamma
               * every pixel (1.2M calls a frame), invert the LUT once: the
               * largest raw value whose gamma output is still under the
               * threshold. */
              unsigned char thr[3] = { 24, 24, 24 };
              if (g_fog_valid && g_fog.have_gamma) {
                  for (int c = 0; c < 3; c++) {
                      int t = 0;
                      for (int v = 0; v < 256; v++) if (g_fog.gamma[c][v] < 24) t = v;
                      thr[c] = (unsigned char)t;
                  }
              }
              long n = 0, tot = (long)vw * vh;
              for (long i = 0; i < tot; i++) {
                  const unsigned char *q = vbuf + i * 3;
                  if (q[0] <= thr[0] && q[1] <= thr[1] && q[2] <= thr[2]) n++;
              }
              if (tot > 0 && n * 100 >= tot * vpct) {
                  long px = (long)_W[0x0D00], pz = (long)_W[0x0D08];
                  long gx = px / 0x18000, gz = pz / 0x18000;
                  int course = (int)_W[0x0E0C];
                  int bases[4] = { 1173, 1301, 1429, 1557 };
                  fprintf(vfp, "VOID f%-7u %4ld%% black  player (%ld,%ld,%ld) "
                               "heading=%ld  cell=%ld (gx=%ld gz=%ld) chunk=%d\n",
                          (unsigned)g_sys.frame_count, n * 100 / tot,
                          px, (long)_W[0x0D04], pz, (long)(_W[0x0DAC] & 0xFFFF),
                          gx + gz * 8, gx, gz,
                          (course >= 0 && course < 4) ? bases[course] + (int)(gx + gz * 8) : -1);
                  fflush(vfp);
              }
          } }

        if (!headless) SDL_GL_SwapWindow(window);
        else if (headless)
            glFinish();  /* ensure rendering completes */

        g_sys.frame_count++;

        /* PROPCYCL_EXIT_AT=<frame> [PROPCYCL_FPS_FROM=<frame>]: quit after that
         * frame and report the REAL frame pacing -- wall clock from swap to
         * swap, vsync included, i.e. what the player sees. A frame over 25 ms
         * at 60 Hz is a missed vsync (the frame showed twice). */
        {   static int exit_at = -2, fps_from;
            static double *gap; static int ngap; static Uint64 last;
            if (exit_at == -2) {
                const char *e = getenv("PROPCYCL_EXIT_AT");
                exit_at = e ? atoi(e) : -1;
                e = getenv("PROPCYCL_FPS_FROM"); fps_from = e ? atoi(e) : 0;
                if (exit_at > 0) gap = malloc(sizeof(double) * (exit_at + 1));
            }
            if (exit_at > 0) {
                Uint64 now = SDL_GetPerformanceCounter();
                if (last && (int)g_sys.frame_count > fps_from && ngap <= exit_at)
                    gap[ngap++] = (double)(now - last) * 1000.0 / SDL_GetPerformanceFrequency();
                last = now;
                if ((int)g_sys.frame_count >= exit_at) {
                    double sum = 0, mx = 0; int slow = 0, i, j;
                    for (i = 0; i < ngap; i++) { sum += gap[i]; if (gap[i] > mx) mx = gap[i]; if (gap[i] > 25.0) slow++; }
                    for (i = 1; i < ngap; i++) { double v = gap[i]; for (j = i; j > 0 && gap[j-1] > v; j--) gap[j] = gap[j-1]; gap[j] = v; }
                    if (ngap) printf("[FPS] frames %d-%d: %.1f fps average, frame ms p50 %.2f p99 %.2f max %.2f, "
                                     "%d of %d frames missed vsync (>25 ms)\n",
                                     fps_from + 1, exit_at, ngap * 1000.0 / sum, gap[ngap / 2],
                                     gap[(int)(ngap * 0.99)], mx, slow, ngap);
                    fflush(stdout);
                    running = false;
                }
            }
        }

        /* Headless: capture and exit after N frames */
        if (headless && (int)g_sys.frame_count >= screenshot_frame) {
            { extern int g_bbox_code, g_bx0,g_bx1,g_by0,g_by1,g_bn;
              if (g_bbox_code >= 0)
                  printf("  [BBOX] code=%d verts=%d screen x[%d..%d] w=%d  y[%d..%d] h=%d\n",
                         g_bbox_code, g_bn, g_bx0, g_bx1, g_bx1-g_bx0, g_by0, g_by1, g_by1-g_by0); }
            save_screenshot(screenshot_path);

            /* Dump DSP command buffer for scene analysis */
            {
                FILE *df = fopen("dspram_dump.bin", "wb");
                if (df) {
                    fwrite(g_sys.dspram, 1, DSPRAM_SIZE, df);
                    fclose(df);
                    printf("DSP RAM dumped to dspram_dump.bin (%d bytes)\n", DSPRAM_SIZE);
                }
            }

            running = false;
        }

        /* Safety: print frame count periodically in headless */
        if (headless && g_sys.frame_count % 10 == 0)
            printf("  frame %d / %d\n", g_sys.frame_count, screenshot_frame);
    }

    trace_finish();
    SDL_GL_DeleteContext(glctx);
    SDL_DestroyWindow(window);
    { extern double g_perf_w2r, g_perf_r2w, g_perf_fog;
      extern int tex_frame_hits, tex_frame_misses, tex_cache_evictions;
      extern double g_perf_pdp, g_perf_flush, g_perf_txt, g_perf_txr, g_perf_txc, g_perf_txu;
      extern double g_perf_bake, g_perf_gl, g_perf_clip;
      { extern double g_bake_texels; extern int tex_reallocs, tex_subimages; if (getenv("PROPCYCL_PERF")) printf("[PERF]   flush = bake %.3f + clip %.3f + gl %.3f   (texels baked %.0fM, realloc %d / reuse %d)\n", g_perf_bake, g_perf_clip, g_perf_gl, g_bake_texels/1e6, tex_reallocs, tex_subimages); }
      if (getenv("PROPCYCL_PERF"))
        fprintf(stderr, "[PERF] game %.3f (sync %.3f)  render %.3f = walk %.3f + flush %.3f + text %.3f (fog %.3f)  evict %d\n",
                g_perf_game, g_perf_w2r+g_perf_r2w, g_perf_render,
                g_perf_pdp, g_perf_flush, g_perf_txt, g_perf_fog, tex_cache_evictions);
      if (getenv("PROPCYCL_PERF")) {
        fprintf(stderr, "[PERF]   text = render %.3f + alphacount %.3f + upload %.3f\n", g_perf_txr, g_perf_txc, g_perf_txu);
        { extern long g_txt_calls, g_txt_miss;
          fprintf(stderr, "[PERF]   text cache: %ld calls, %ld misses (%.1f%%)\n",
                  g_txt_calls, g_txt_miss, g_txt_calls ? 100.0*g_txt_miss/g_txt_calls : 0.0); } } }
    SDL_Quit();

    /* File -> Restart: start the program again with the same arguments,
     * like switching the cabinet off and on. */
    if (!headless && ui_restart_requested()) {
        fflush(NULL);
#ifdef _WIN32
        char self[4096];
        if (GetModuleFileNameA(NULL, self, sizeof self)) {
            /* _spawnv, not _execv: _execv on Windows returns the console to
             * the caller before the new process is up. */
            if (_spawnv(_P_NOWAIT, self, (const char *const *)argv) != -1) return 0;
        }
#else
        char self[4096];
        ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
        if (n > 0) { self[n] = 0; execv(self, argv); }
        execvp(argv[0], argv);
#endif
        perror("restart failed");
        return 1;
    }
    return 0;
}
