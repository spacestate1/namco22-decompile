/*
 * tw_host.c -- Tokyo Wars in a window: OpenGL context, the menu bar, the display modes (widescreen, window mode/size,
 * resolution, aspect, scaling), F12 screenshots, vsync or a timer at the board's 59.906 Hz, the sound card. The picture itself
 * is the shared engine's (engine/ss22_gl.c), and so are the menu (engine/eng_ui.c, Nuklear, as in Prop Cycle) and the display
 * settings (engine/eng_display.c); this file is what a game has to supply around them.
 *
 * Keys: Esc opens the menu (File / Display / Audio / Controls; pad R3 too), P pause, F11 or Alt+Enter fullscreen, F12
 * screenshot (screenshots/), and the cabinet's (tw_input.h, rebindable in the menu). Settings are saved to tw_controls.cfg.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <SDL2/SDL.h>
#include "eng_gl.h"
#include "render_target.h"
#include "ss22_gl.h"
#include "audio_out.h"
#include "eng_display.h"
#include "eng_ui.h"
#include "tw_host.h"
#include "tw_input.h"
#include "tw_snd.h"

#define FRAME_NS 16693000ull                      /* 1 / 59.906 Hz, 25.6 MHz / 814 / 525 */
#define CFG_FILE "tw_controls.cfg"

static SDL_Window *win;
static SDL_GLContext glc;
static bool vsync, paused, shot_pending, headless_open, restart_req;
static uint64_t next_ns, vs_t0;
static int vs_frames;

static uint64_t now_ns(void)                     /* split the scaling: counter * 1e9 overflows 64 bits (and this works on Windows too) */
{
    const uint64_t c = SDL_GetPerformanceCounter(), f = SDL_GetPerformanceFrequency();
    return c / f * 1000000000ull + c % f * 1000000000ull / f;
}

bool tw_host_active(void) { return win != NULL; }
bool tw_host_restart_requested(void) { return restart_req; }

static void volume_hook(int pct) { eng_audio_set_volume(pct); }

bool tw_host_open(int scale, bool fs)      /* scale <= 0: the saved window size */
{
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER) != 0) { fprintf(stderr, "[HOST] SDL: %s\n", SDL_GetError()); return false; }
    eng_gl_context_attributes();
    eng_disp_load(CFG_FILE, scale, fs);                /* the saved display choices; --window N / --fullscreen override */
    win = SDL_CreateWindow("Tokyo Wars", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                           eng_disp_win_w(g_eng_disp.scale), eng_disp_win_h(g_eng_disp.scale),
                           SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI |
                           (g_eng_disp.winmode ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0));
    if (!win) { fprintf(stderr, "[HOST] window: %s\n", SDL_GetError()); return false; }
    SDL_DisplayMode dm;
    const int hz = SDL_GetCurrentDisplayMode(SDL_GetWindowDisplayIndex(win), &dm) == 0 ? dm.refresh_rate : 0;
    vsync = hz >= 59 && hz <= 61;                 /* vsync only on a ~60 Hz display: elsewhere it would run the game at the display's rate */
    if (getenv("TW_VSYNC")) vsync = atoi(getenv("TW_VSYNC")) != 0;
#ifdef _WIN32
    { extern SDL_GLContext eng_gl_create_win(SDL_Window **, const char **); const char *miss = NULL; glc = eng_gl_create_win(&win, &miss); }
#else
    glc = SDL_GL_CreateContext(win);
#endif
    if (!glc) { fprintf(stderr, "[HOST] OpenGL context: %s\n", SDL_GetError()); return false; }
    SDL_GL_MakeCurrent(win, glc);
    if (vsync && SDL_GL_SetSwapInterval(1) != 0) vsync = false;
    if (!vsync) SDL_GL_SetSwapInterval(0);
    fprintf(stderr, "[HOST] OpenGL: %s; display %d Hz, %s\n", (const char *)glGetString(GL_RENDERER), hz, vsync ? "vsync" : "timer-paced at 59.906 Hz");
    SDL_SetWindowMinimumSize(win, 320, 240);
    tw_input_init();                               /* after the cfg: the key bindings */
    eng_ui_add_page(tw_input_page());
    if (!eng_ui_init(win, "Tokyo Wars")) fprintf(stderr, "[HOST] menu: Nuklear init failed\n");
    eng_disp_set_volume_hook(volume_hook);
    const bool audio = eng_audio_open();
    eng_disp_attach(win);                          /* exclusive fullscreen, a size too big for this display; the volume */
    if (audio) tw_snd_set_output(true);
    next_ns = now_ns();
    return true;
}

bool tw_host_open_headless(void)
{
    headless_open = eng_gl_open_headless(640, 480);
    return headless_open;
}

void tw_host_shot(const char *path)
{
    /* an offscreen 640x480 draw of the prepared frame, read back exactly as the engine drew it */
    int vw, vh;
    if (win) {
        rt_begin(win, 640, 480, &vw, &vh);
        ss22_draw(vw, vh);
        eng_gl_write_ppm(path, vw, vh);
    } else {
        vw = 640; vh = 480;
        ss22_draw(vw, vh);
        eng_gl_write_ppm(path, vw, vh);
    }
    FILE *chk = fopen(path, "rb");                   /* say so when the folder is missing, rather than "saved" */
    if (chk) { fclose(chk); fprintf(stderr, "[HOST] saved %s\n", path); } else fprintf(stderr, "[HOST] could not write %s (does the folder exist?)\n", path);
}

/* The prepared frame into the window: the engine draws it at the render size into the shared render target, which is scaled
 * into the picture rectangle; the menu, when open, goes over it. */
static void present(void)
{
    int rw, rh, vw, vh;
    eng_disp_render_size(&rw, &rh);
    rt_begin(win, rw, rh, &vw, &vh);
    ss22_draw(vw, vh);
    if (shot_pending) {                              /* F12: what the engine drew, before it is scaled to the window */
        shot_pending = false;
        char p[96]; time_t t = time(NULL); struct tm tm; localtime_r(&t, &tm); static int n;
        mkdir("screenshots", 0755);
        snprintf(p, sizeof p, "screenshots/tw_%04d%02d%02d_%02d%02d%02d_%d.ppm", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                 tm.tm_hour, tm.tm_min, tm.tm_sec, n++);
        if (eng_gl_write_ppm(p, vw, vh)) fprintf(stderr, "[HOST] saved %s\n", p);
    }
    const SDL_Rect r = eng_disp_picture_rect(rw, rh);
    rt_end_rect(win, r.x, r.y, r.w, r.h, eng_disp_sharp());   /* clears the window round the picture itself; a no-op when rt_begin drew straight into it */
    bool quit = false;
    eng_ui_draw(&quit);
    { static int dbg = -1, n, shot = -1;             /* TW_HOSTDBG=1: what is in the window's back buffer just before it is shown;
                                                      * TW_WINSHOT=<n>: that back buffer, whole, to screenshots/tw_window_<n>.ppm at swap n */
      if (dbg < 0) { dbg = getenv("TW_HOSTDBG") != NULL; shot = getenv("TW_WINSHOT") ? atoi(getenv("TW_WINSHOT")) : 0; }
      ++n;
      if (dbg || shot) {
          int dw, dh; SDL_GL_GetDrawableSize(win, &dw, &dh);
          glReadBuffer(GL_BACK);
          if (dbg && n % 60 == 0) {
              unsigned char px[3 * 16]; int lit = 0;
              for (int i = 0; i < 16; i++) {
                  glReadPixels(r.x + r.w * (i % 4 + 1) / 5, r.y + r.h * (i / 4 + 1) / 5, 1, 1, GL_RGB, GL_UNSIGNED_BYTE, px + 3 * i);
                  lit += px[3 * i] | px[3 * i + 1] | px[3 * i + 2] ? 1 : 0;
              }
              fprintf(stderr, "[HOST] frame %d: window %dx%d, render %dx%d, picture %d,%d %dx%d, %d of 16 sample pixels lit, GL error 0x%X\n",
                      n, dw, dh, rw, rh, r.x, r.y, r.w, r.h, lit, glGetError());
          }
          if (shot && n == shot) {
              char p[64]; snprintf(p, sizeof p, "screenshots/tw_window_%d.ppm", n);
              mkdir("screenshots", 0755);
              if (eng_gl_write_ppm(p, dw, dh)) fprintf(stderr, "[HOST] saved %s (%dx%d)\n", p, dw, dh);
          }
      } }
    SDL_GL_SwapWindow(win);
}

/* the window's events: the menu first (it swallows them while open), then the host's own keys. false = quit */
static bool pump(void)
{
    SDL_Event e;
    eng_ui_input_begin();
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) { eng_ui_input_end(); return false; }
        tw_input_event(&e);
        if (eng_ui_event(&e)) continue;
        if (e.type == SDL_KEYDOWN && !e.key.repeat) {
            const SDL_Scancode sc = e.key.keysym.scancode;
            if (sc == SDL_SCANCODE_ESCAPE) { eng_ui_set_open(true); tw_input_neutral(); }
            if (sc == SDL_SCANCODE_P) { paused = !paused; fprintf(stderr, "[HOST] %s\n", paused ? "paused" : "running"); }
            if (sc == SDL_SCANCODE_F12) shot_pending = true;
            if (sc == SDL_SCANCODE_F11 || (sc == SDL_SCANCODE_RETURN && (e.key.keysym.mod & KMOD_ALT))) eng_disp_toggle_fullscreen();
        }
        if (e.type == SDL_CONTROLLERBUTTONDOWN && e.cbutton.button == SDL_CONTROLLER_BUTTON_RIGHTSTICK) { eng_ui_set_open(true); tw_input_neutral(); }
    }
    eng_ui_input_end();
    bool quit = false;
    if (eng_ui_quit_requested()) { quit = true; restart_req = eng_ui_restart_requested(); }
    return !quit;
}

static void pace(void)
{
    if (vsync && vs_frames < 60) {                   /* trust vsync only if it blocks: an unmapped or occluded window (Wayland, NVIDIA) swaps at once */
        if (vs_frames++ == 10) vs_t0 = now_ns();
        if (vs_frames == 60 && (now_ns() - vs_t0) / 50 < 12000000ull) {
            vsync = false; SDL_GL_SetSwapInterval(0); next_ns = now_ns();
            fprintf(stderr, "[HOST] vsync does not block here (%.1f ms a frame): timer-paced at 59.906 Hz\n", (double)((now_ns() - vs_t0) / 50) / 1e6);
        }
    }
    if (!vsync) {                                    /* sleep to the board's 59.906 Hz, the last millisecond spun for precision */
        next_ns += FRAME_NS;
        uint64_t now = now_ns();
        if (next_ns > now) {
            const uint64_t left = next_ns - now;
            if (left > 2000000ull) SDL_Delay((Uint32)((left - 1500000ull) / 1000000ull));
            while (now_ns() < next_ns) ;
        } else if (now - next_ns > 100000000ull) next_ns = now;   /* fell behind: resync, no catch-up burst */
    }
}

bool tw_host_frame(void)
{
    if (!win) return true;
    { static int at = -2, mp = 1, mr = 0, n; static const char *keys;      /* TW_MENU_AT=<frame>[:<page>:<row>], TW_MENU_KEYS=<u d l r o b n p ...>: tests drive the menu */
      if (at == -2) { at = -1; const char *e = getenv("TW_MENU_AT"); if (e) sscanf(e, "%d:%d:%d", &at, &mp, &mr); keys = getenv("TW_MENU_KEYS"); }
      if (at >= 0 && ++n == at) { if (mp >= 0) { eng_ui_goto(mp, mr); eng_ui_set_open(true); tw_input_neutral(); for (const char *k = keys; k && *k; k++) eng_ui_nav(*k); }
        if (getenv("TW_MENU_PRESS")) {                                   /* TW_MENU_PRESS=<key name>: a key press for the menu (a binding row waiting for one) */
            SDL_Event ke; memset(&ke, 0, sizeof ke);
            ke.type = SDL_KEYDOWN; ke.key.state = SDL_PRESSED; ke.key.keysym.scancode = SDL_GetScancodeFromName(getenv("TW_MENU_PRESS"));
            SDL_PushEvent(&ke);
        } } }
    if (!pump()) return false;
    if (!paused && !eng_ui_is_open()) tw_input_update();
    present();
    pace();
    while (paused || eng_ui_is_open()) {             /* the game stops (P, or the menu); the window keeps answering */
        if (!pump()) return false;
        if (paused || eng_ui_is_open()) {
            if (eng_ui_is_open()) { present(); if (!vsync) SDL_Delay(12); }
            else SDL_Delay(10);
        }
        if (!paused && !eng_ui_is_open()) next_ns = now_ns();
    }
    return true;
}

void tw_host_close(void)
{
    eng_audio_close();
    eng_ui_shutdown();
    if (glc) SDL_GL_DeleteContext(glc);
    if (win) SDL_DestroyWindow(win);
    win = NULL; glc = NULL;
}
