/*
 * rr_host.c -- the SDL2 window, keyboard input and screenshots.
 *
 * The lifted 68K program owns the main loop (L_4000 never returns), so the host
 * is driven from rr_tick() once per video frame: rr_host_frame() presents
 * g_frame_rgb, polls input into g_hw the way MAME's ports hold it, and paces to
 * 60 Hz. Headless runs never call rr_host_open() and are unaffected.
 *
 * Keys (MAME's raverace ports, namcos22.cpp INPUT_PORTS ridgera/raverace):
 *   5 coin 1   6 coin 2   9 service   F2 test
 *   Left/Right steer (ADC.0, centre 0x800, 0x280..0xD80, KEYDELTA 160)
 *   X/Up gas, Z/Down brake (ADC.1/2, 0..0x610)
 *   A shift down, Z shift up, V view change
 *   F12 screenshot -> screenshots/   P pause   Esc quit
 */
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <sys/stat.h>
#include "rr_hw.h"
#include "rr_video.h"
#include "rr_input.h"
#include "rr_font.h"
#include "rr_sound.h"
#include "rr_ui.h"

/* ---- controllers ----------------------------------------------------------
 * Every connected device is opened (up to MAX_DEV): as a GameController when
 * SDL knows its layout (Xbox, DualShock/DualSense, Switch Pro, 8BitDo, ... plus
 * anything in a gamecontrollerdb.txt next to the binary), otherwise as a RAW
 * joystick mapped by axis/button number from rr_controls.cfg (wheels, pedals,
 * arcade sticks). Hot-plug either way. */
#define MAX_DEV 4
static struct { SDL_GameController *gc; SDL_Joystick *js; SDL_JoystickID id; } dev[MAX_DEV];

static void dev_scan(void)
{
    for (int i = 0; i < SDL_NumJoysticks(); i++) {
        SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(i);
        int free_slot = -1; bool open = false;
        for (int d = 0; d < MAX_DEV; d++) {
            if ((dev[d].gc || dev[d].js) && dev[d].id == id) open = true;
            if (!dev[d].gc && !dev[d].js && free_slot < 0) free_slot = d;
        }
        if (open || free_slot < 0) continue;
        if (SDL_IsGameController(i)) {
            if ((dev[free_slot].gc = SDL_GameControllerOpen(i)))
                fprintf(stderr, "[HOST] gamepad %d: %s\n", free_slot, SDL_GameControllerName(dev[free_slot].gc));
        } else if ((dev[free_slot].js = SDL_JoystickOpen(i))) {
            fprintf(stderr, "[HOST] joystick %d: %s (%d axes, %d buttons) -- mapped by number, see rr --joytest\n", free_slot,
                    SDL_JoystickName(dev[free_slot].js), SDL_JoystickNumAxes(dev[free_slot].js), SDL_JoystickNumButtons(dev[free_slot].js));
        }
        dev[free_slot].id = id;
    }
}
static void dev_remove(SDL_JoystickID id)
{
    for (int d = 0; d < MAX_DEV; d++)
        if ((dev[d].gc || dev[d].js) && dev[d].id == id) {
            fprintf(stderr, "[HOST] controller %d removed\n", d);
            if (dev[d].gc) SDL_GameControllerClose(dev[d].gc);
            if (dev[d].js) SDL_JoystickClose(dev[d].js);
            dev[d].gc = NULL; dev[d].js = NULL;
        }
}
static bool dev_is_raw(SDL_JoystickID id)
{
    for (int d = 0; d < MAX_DEV; d++) if (dev[d].js && dev[d].id == id) return true;
    return false;
}
static void load_pad_db(void)
{
    if (SDL_GameControllerAddMappingsFromFile("gamecontrollerdb.txt") > 0) fprintf(stderr, "[HOST] extra pad mappings from gamecontrollerdb.txt\n");
}

static SDL_Window *win;
static SDL_Renderer *ren;
static SDL_Texture *tex;               /* the game picture, at the render size */
static int tex_w, tex_h;
static uint64_t next_ns;
static bool vsync;                    /* presenting paces us (display ~60 Hz) */
/* the board: PIXEL_CLOCK 25.6 MHz / (HTOTAL 814 x VTOTAL 525) = 59.906 Hz */
#define FRAME_NS 16692969ull
static uint64_t now_ns(void)          /* split the scaling: counter * 1e9 overflows 64 bits */
{
    const uint64_t c = SDL_GetPerformanceCounter(), f = SDL_GetPerformanceFrequency();
    return c / f * 1000000000ull + c % f * 1000000000ull / f;
}
static int paused;

static const char *scaling_name[3];
static void apply_scaling(void);
static void apply_fullscreen(void);
static void save_opt(const char *k, const char *v);

/* ---- the menu (Escape / pad Start) is src/rr_ui.c; it pauses the game ----- */

/* ---- display: the same choices as Prop Cycle's Display menu --------------
 * RESOLUTION is the RENDER size, not the window: the software renderer draws
 * the scene at that many pixels (rr_video_set_size) and the picture is scaled
 * to the window. Its HEIGHT is what counts; the width follows the shape --
 * 4:3, or with WIDESCREEN the window's own shape, filled with more track at
 * the sides. 640x480 is the board's own picture and the default. */
static const struct { int w, h; } res_list[] = {
    { 640, 480 }, { 0, 0 },                                 /* the board; native (window pixels) */
    { 800, 600 }, { 960, 720 }, { 1024, 768 }, { 1280, 960 }, { 1440, 1080 }, { 1600, 1200 }, { 1920, 1440 },
    { 1280, 720 }, { 1600, 900 }, { 1920, 1080 }, { 2560, 1440 }, { 3840, 2160 },
    { 1280, 800 }, { 1920, 1200 }, { 2560, 1080 }, { 3440, 1440 },
};
#define NRES ((int)(sizeof res_list / sizeof res_list[0]))
static const char *aspect_name[4] = { "stretch", "4:3", "8:7", "16:9" };
/* the render size for the current window and settings */
static void render_size(int *w, int *h)
{
    int dw = 640, dh = 480;
    if (ren) SDL_GetRendererOutputSize(ren, &dw, &dh);
    if (dw < 1 || dh < 1) { dw = 640; dh = 480; }
    double ar = 4.0 / 3.0;
    if (g_cfg_wide && (double)dw / dh > ar) ar = (double)dw / dh;
    int H = g_cfg_res_h;
    if (H <= 0) H = g_cfg_wide ? dh : (dh < dw * 3 / 4 ? dh : dw * 3 / 4);   /* native */
    if (H < 240) H = 240;
    *h = H;
    *w = ((int)(H * ar + 0.5) + 1) & ~1;
}
static void apply_render_size(void);
static void toggle_record(void);

/* ---- the setters the menu drives: apply, and save to rr_controls.cfg ---- */
void rr_host_set_winmode(int m)
{
    g_cfg_winmode = m < 0 ? 0 : m > 2 ? 2 : m;
    apply_fullscreen();
    char v[4]; snprintf(v, sizeof v, "%d", g_cfg_winmode);
    save_opt("window_mode", v); save_opt("fullscreen", g_cfg_winmode ? "1" : "0");
}
void rr_host_set_scale(int k)
{
    g_cfg_scale = k < 1 ? 1 : k > 4 ? 4 : k;
    char v[4]; snprintf(v, sizeof v, "%d", g_cfg_scale); save_opt("window_scale", v);
    if (!g_cfg_winmode) apply_fullscreen();
}
void rr_host_set_res(int w, int h)
{
    g_cfg_res_w = w; g_cfg_res_h = h;
    /* a wide resolution is what widescreen is for */
    if (h > 0 && w * 3 > h * 4 + 8 && !g_cfg_wide) { g_cfg_wide = 1; save_opt("widescreen", "1"); }
    char v[24];
    if (h <= 0) snprintf(v, sizeof v, "native"); else snprintf(v, sizeof v, "%dx%d", w, h);
    save_opt("resolution", v);
    if (g_cfg_winmode == 2) apply_fullscreen();          /* exclusive: it is the display mode too */
    apply_render_size();
}
void rr_host_set_wide(int on) { g_cfg_wide = on != 0; save_opt("widescreen", g_cfg_wide ? "1" : "0"); apply_render_size(); }
void rr_host_set_aspect(int a) { g_cfg_aspect = a < 0 ? 0 : a > 3 ? 3 : a; save_opt("aspect", aspect_name[g_cfg_aspect]); }
void rr_host_set_scaling(int sc) { g_cfg_scaling = sc < 0 ? 0 : sc > 2 ? 2 : sc; apply_scaling(); save_opt("scaling", scaling_name[g_cfg_scaling]); }
void rr_host_set_volume(int pct)
{
    g_cfg_volume = pct < 0 ? 0 : pct > 100 ? 100 : pct;
    rr_audio_set_volume(g_cfg_volume);
    char v[8]; snprintf(v, sizeof v, "%d", g_cfg_volume); save_opt("volume", v);
}
void rr_host_set_freeplay(bool on)
{
    rr_hw_set_freeplay(on);
    save_opt("free_play", on ? "1" : "0");
    fprintf(stderr, "[HOST] %s (saved to rr_controls.cfg)\n", on ? "free play" : "coins required");
}
void rr_host_toggle_record(void) { toggle_record(); }
int  rr_host_res_count(void) { return NRES; }
void rr_host_res_get(int i, int *w, int *h) { if (i < 0 || i >= NRES) i = 0; *w = res_list[i].w; *h = res_list[i].h; }
void rr_host_render_size(int *w, int *h) { render_size(w, h); }

static const char *shot_dir = "screenshots";

static const char *scaling_name[3] = { "smooth", "sharp", "integer" };

/* apply g_cfg_scaling: the filter (integer placement is picture_rect's) */
static void apply_scaling(void)
{
    SDL_SetTextureScaleMode(tex, g_cfg_scaling == 0 ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);
}
/* The picture's rectangle in drawable pixels, for a picture of bw x bh render
 * pixels: centred, ONE scale for both axes (not SDL's logical size, which
 * rounded the axes separately -- a 1000x500 window got 1.041 x 1.042).
 * Its shape: the render's own (4:3, or the window's with widescreen), unless
 * widescreen is off and the aspect says stretch / 8:7 / 16:9. Integer mode:
 * the largest whole multiple of the render size, or a plain fit if none fits. */
static SDL_Rect picture_rect_for(int bw, int bh, bool menu)
{
    int ow, oh;
    SDL_GetRendererOutputSize(ren, &ow, &oh);
    double ar = (double)bw / bh;
    if (!menu && !g_cfg_wide) {
        if (g_cfg_aspect == 0) { SDL_Rect r = { 0, 0, ow, oh }; return r; }
        ar = g_cfg_aspect == 2 ? 8.0 / 7.0 : g_cfg_aspect == 3 ? 16.0 / 9.0 : 4.0 / 3.0;
    }
    int w, h;
    const bool own_shape = fabs(ar - (double)bw / bh) < 0.01;
    if (g_cfg_scaling == 2 && own_shape && ow >= bw && oh >= bh) {
        const int k = ow / bw < oh / bh ? ow / bw : oh / bh;
        w = bw * k; h = bh * k;
    } else if (ow <= oh * ar) { w = ow; h = (int)(ow / ar + 0.5); }
    else { h = oh; w = (int)(oh * ar + 0.5); }
    SDL_Rect r = { (ow - w) / 2, (oh - h) / 2, w, h };
    return r;
}
static SDL_Rect picture_rect(void) { return picture_rect_for(tex_w ? tex_w : 640, tex_h ? tex_h : 480, false); }
/* ask the renderer for the size the settings and the window now call for */
static void apply_render_size(void)
{
    int w, h; render_size(&w, &h);
    rr_video_set_size(w, h);
}
/* the largest window scale that fits the display's usable area */
static int max_scale(void)
{
    SDL_Rect b;
    if (SDL_GetDisplayUsableBounds(SDL_GetWindowDisplayIndex(win), &b) != 0) return 4;
    int k = b.w / 640 < (b.h - 40) / 480 ? b.w / 640 : (b.h - 40) / 480;   /* leave room for a title bar */
    return k < 1 ? 1 : k > 4 ? 4 : k;
}
static void apply_fullscreen(void)
{
    if (g_cfg_winmode < 0 || g_cfg_winmode > 2) g_cfg_winmode = g_cfg_fullscreen ? 1 : 0;
    g_cfg_fullscreen = g_cfg_winmode != 0;
    if (g_cfg_winmode == 2) {
        /* EXCLUSIVE: switch the monitor to the mode nearest the chosen
         * resolution (native: the desktop's own), so 640x480 can be the
         * real thing on a monitor that has it */
        int disp = SDL_GetWindowDisplayIndex(win); if (disp < 0) disp = 0;
        SDL_DisplayMode want = { 0, g_cfg_res_w, g_cfg_res_h, 0, 0 }, got;
        if (g_cfg_res_h <= 0 || !SDL_GetClosestDisplayMode(disp, &want, &got)) SDL_GetDesktopDisplayMode(disp, &got);
        SDL_SetWindowFullscreen(win, 0);
        SDL_SetWindowDisplayMode(win, &got);
        if (SDL_SetWindowFullscreen(win, SDL_WINDOW_FULLSCREEN) != 0) {
            fprintf(stderr, "[HOST] exclusive %dx%d failed (%s), using desktop fullscreen\n", got.w, got.h, SDL_GetError());
            g_cfg_winmode = 1;
            SDL_SetWindowFullscreen(win, SDL_WINDOW_FULLSCREEN_DESKTOP);
        } else fprintf(stderr, "[HOST] exclusive fullscreen %dx%d @ %d Hz\n", got.w, got.h, got.refresh_rate);
    } else SDL_SetWindowFullscreen(win, g_cfg_winmode ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
    SDL_ShowCursor(g_cfg_fullscreen ? SDL_DISABLE : SDL_ENABLE);
    if (!g_cfg_fullscreen) {                           /* back to the chosen window size, centred */
        int k = g_cfg_scale < max_scale() ? g_cfg_scale : max_scale();
        if (k != g_cfg_scale) fprintf(stderr, "[HOST] %dx does not fit this display; using %dx\n", g_cfg_scale, k);
        SDL_SetWindowSize(win, 640 * k, 480 * k);
        SDL_SetWindowPosition(win, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    }
}
static void save_opt(const char *k, const char *v) { rr_input_set_option("rr_controls.cfg", k, v); }

bool rr_host_open(int scale)
{
    rr_input_load("rr_controls.cfg");
    if (scale > 0) g_cfg_scale = scale;                /* --window N overrides the saved size */
    if (getenv("RR_FULLSCREEN")) { g_cfg_fullscreen = atoi(getenv("RR_FULLSCREEN")) != 0; g_cfg_winmode = g_cfg_fullscreen; }
    if (g_cfg_winmode < 0) g_cfg_winmode = g_cfg_fullscreen ? 1 : 0;
    g_cfg_fullscreen = g_cfg_winmode != 0;
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER) != 0) { fprintf(stderr, "[HOST] SDL: %s\n", SDL_GetError()); return false; }
    win = SDL_CreateWindow("Rave Racer", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 640 * g_cfg_scale, 480 * g_cfg_scale,
                           SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI |
                           (g_cfg_fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0));
    if (!win) { fprintf(stderr, "[HOST] window: %s\n", SDL_GetError()); return false; }
    /* vsync only on a ~60 Hz display: elsewhere it would run the game at the
     * display's rate. RR_VSYNC=0/1 forces it. */
    SDL_DisplayMode dm;
    int hz = SDL_GetCurrentDisplayMode(SDL_GetWindowDisplayIndex(win), &dm) == 0 ? dm.refresh_rate : 0;
    vsync = hz >= 59 && hz <= 61;
    const char *ev = getenv("RR_VSYNC");
    if (ev) vsync = atoi(ev) != 0;
    ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | (vsync ? SDL_RENDERER_PRESENTVSYNC : 0));
    if (!ren) { vsync = false; ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE); }
    fprintf(stderr, "[HOST] display %d Hz, %s\n", hz, vsync ? "vsync" : "timer-paced at 59.906 Hz");

    tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING, 640, 480);
    tex_w = 640; tex_h = 480;
    if (!rr_ui_init(win, ren)) fprintf(stderr, "[HOST] menu: Nuklear init failed\n");
    SDL_SetWindowMinimumSize(win, 320, 240);
    if (g_cfg_winmode == 2) apply_fullscreen();                                /* exclusive: set the mode */
    else if (!g_cfg_fullscreen && g_cfg_scale > max_scale()) apply_fullscreen();   /* too big for this display: shrink */
    apply_render_size();
    apply_scaling();
    if (g_cfg_fullscreen) SDL_ShowCursor(SDL_DISABLE);
    next_ns = now_ns();
    load_pad_db();
    dev_scan();
    if (rr_audio_output_open()) rr_audio_set_volume(g_cfg_volume);
    return tex != NULL;
}

static void screenshot(void)
{
    mkdir(shot_dir, 0755);
    char p[256];
    time_t t = time(NULL);
    struct tm tm; localtime_r(&t, &tm);
    static int n;
    snprintf(p, sizeof p, "%s/rr_%04d%02d%02d_%02d%02d%02d_%d.ppm", shot_dir,
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, n++);
    if (rr_video_write_ppm(p)) fprintf(stderr, "[HOST] saved %s\n", p);
}

static void set_bit(uint16_t bit, int down)       /* active low */
{
    if (down) g_hw.inputs &= (uint16_t)~bit; else g_hw.inputs |= bit;
}

static void ramp(uint16_t *v, int toward, int lo, int hi, int step)
{
    int x = *v;
    if (x < toward) { x += step; if (x > toward) x = toward; }
    else if (x > toward) { x -= step; if (x < toward) x = toward; }
    if (x < lo) x = lo;
    if (x > hi) x = hi;
    *v = (uint16_t)x;
}

static bool held(int act)
{
    const Uint8 *k = SDL_GetKeyboardState(NULL);
    for (int i = 0; i < g_bind[act].nkeys; i++) if (k[g_bind[act].keys[i]]) return true;
    for (int d = 0; d < MAX_DEV; d++) {
        if (dev[d].gc && g_bind[act].pad != SDL_CONTROLLER_BUTTON_INVALID && SDL_GameControllerGetButton(dev[d].gc, g_bind[act].pad)) return true;
        if (dev[d].js && g_joy_button[act] >= 0 && SDL_JoystickGetButton(dev[d].js, g_joy_button[act])) return true;
    }
    return false;
}
static bool pressed(const SDL_Event *e, int act)
{
    if (e->type == SDL_KEYDOWN && !e->key.repeat) {
        for (int i = 0; i < g_bind[act].nkeys; i++) if (e->key.keysym.scancode == g_bind[act].keys[i]) return true;
    }
    if (e->type == SDL_CONTROLLERBUTTONDOWN && g_bind[act].pad != SDL_CONTROLLER_BUTTON_INVALID && e->cbutton.button == g_bind[act].pad) return true;
    return e->type == SDL_JOYBUTTONDOWN && dev_is_raw(e->jbutton.which) && g_joy_button[act] >= 0 && e->jbutton.button == g_joy_button[act];
}

/* analog sources, strongest wins; returns false when every source is neutral */
static bool pad_steer(int *out)                   /* -32767..32767 */
{
    int best = 0;
    for (int d = 0; d < MAX_DEV; d++) {
        int v = 0;
        if (dev[d].gc) {
            v = SDL_GameControllerGetAxis(dev[d].gc, SDL_CONTROLLER_AXIS_LEFTX);
            if (v > -g_pad_deadzone && v < g_pad_deadzone) v = 0;
            else v = (v > 0 ? v - g_pad_deadzone : v + g_pad_deadzone) * 32767 / (32767 - g_pad_deadzone);
        } else if (dev[d].js && g_joy_steer.axis >= 0) {
            v = SDL_JoystickGetAxis(dev[d].js, g_joy_steer.axis);   /* a wheel: no deadzone beyond 1000 */
            if (g_joy_steer.invert) v = -v;
            if (v > -1000 && v < 1000) v = 0;
        }
        if (abs(v) > abs(best)) best = v;
    }
    if (best > 32767) best = 32767;
    if (best < -32767) best = -32767;
    *out = best;
    return best != 0;
}
static bool pad_pedal(bool gas, int *out)         /* 0..0x610 */
{
    int best = 0;
    const rr_joyaxis_t *ax = gas ? &g_joy_gas : &g_joy_brake;
    for (int d = 0; d < MAX_DEV; d++) {
        int v = 0;
        if (dev[d].gc) {
            int t = SDL_GameControllerGetAxis(dev[d].gc, gas ? SDL_CONTROLLER_AXIS_TRIGGERRIGHT : SDL_CONTROLLER_AXIS_TRIGGERLEFT);
            if (t > 1000) v = (t - 1000) * 0x610 / (32767 - 1000);
        } else if (dev[d].js && ax->axis >= 0) {
            int r = SDL_JoystickGetAxis(dev[d].js, ax->axis);
            double f = ax->half ? (r < 0 ? 0.0 : r / 32767.0) : (r + 32768) / 65535.0;
            if (ax->invert) f = 1.0 - f;
            if (f > 0.03) v = (int)((f - 0.03) / 0.97 * 0x610);
        }
        if (v > best) best = v;
    }
    *out = best > 0x610 ? 0x610 : best;
    return best > 0;
}

static void toggle_record(void)
{
    if (rr_input_recording()) { rr_input_record_stop(); return; }
    mkdir("recordings", 0755);
    char p[256]; time_t t = time(NULL); struct tm tm; localtime_r(&t, &tm);
    snprintf(p, sizeof p, "recordings/rr_%04d%02d%02d_%02d%02d%02d.inp",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    rr_input_record_start(p);
}

/* returns false when the user closed the window */
bool rr_host_frame(void)
{
    if (!win) return true;
    /* RR_KEYS_TEST=<frame>:<key>[+<frame>:<key>...]: push REAL key events
     * (down, and up a few frames later) into SDL's queue, so the keyboard path
     * -- opening the menu, moving in it, closing it, and the game's own keys --
     * is exercised end to end without a keyboard. Key names are SDL's. */
    { static char *spec; static bool init; static long nfr;
      if (!init) { init = true; const char *ev = getenv("RR_KEYS_TEST"); if (ev) spec = strdup(ev); }
      ++nfr;
      if (spec) {
          char tmp[1024]; snprintf(tmp, sizeof tmp, "%s", spec);
          for (char *t = strtok(tmp, "+"); t; t = strtok(NULL, "+")) {
              long f = atol(t); const char *kn = strchr(t, ':');
              if (!kn) continue;
              const SDL_Scancode sc = SDL_GetScancodeFromName(kn + 1);
              if (sc == SDL_SCANCODE_UNKNOWN) continue;
              if (nfr == f || nfr == f + 3) {
                  SDL_Event ke; memset(&ke, 0, sizeof ke);
                  ke.type = nfr == f ? SDL_KEYDOWN : SDL_KEYUP;
                  ke.key.state = nfr == f ? SDL_PRESSED : SDL_RELEASED;
                  ke.key.keysym.scancode = sc; ke.key.keysym.sym = SDL_GetKeyFromScancode(sc);
                  ke.key.windowID = SDL_GetWindowID(win);
                  SDL_PushEvent(&ke);
                  if (nfr == f) fprintf(stderr, "[HOST] key test f%ld: %s\n", nfr, kn + 1);
              }
          }
      } }
    SDL_Event e;
    rr_ui_input_begin();
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) return false;
        if (e.type == SDL_JOYDEVICEADDED) dev_scan();
        if (e.type == SDL_JOYDEVICEREMOVED) dev_remove(e.jdevice.which);
        if (e.type == SDL_KEYDOWN && !e.key.repeat &&
            (e.key.keysym.scancode == SDL_SCANCODE_F11 ||
             (e.key.keysym.scancode == SDL_SCANCODE_RETURN && (e.key.keysym.mod & KMOD_ALT)))) {
            g_cfg_winmode = g_cfg_winmode ? 0 : 1; apply_fullscreen();
            save_opt("fullscreen", g_cfg_fullscreen ? "1" : "0"); save_opt("window_mode", g_cfg_winmode ? "1" : "0");
            continue;
        }
        if (rr_ui_is_open()) { rr_ui_event(&e); if (!rr_ui_is_open()) fprintf(stderr, "[HOST] menu closed\n"); continue; }
        if (pressed(&e, RR_QUIT) ||
            (e.type == SDL_CONTROLLERBUTTONDOWN && (e.cbutton.button == SDL_CONTROLLER_BUTTON_START ||
                                                    e.cbutton.button == SDL_CONTROLLER_BUTTON_RIGHTSTICK))) {
            rr_ui_set_open(true); fprintf(stderr, "[HOST] menu open\n"); continue;
        }
        if (pressed(&e, RR_SCREENSHOT)) screenshot();
        if (pressed(&e, RR_RECORD)) toggle_record();
        if (pressed(&e, RR_PAUSE)) { paused = !paused; fprintf(stderr, "[HOST] %s\n", paused ? "paused" : "running"); }
    }
    rr_ui_input_end();
    /* RR_MENU_TEST=<frame>: drive the menu from that frame the way the
     * keyboard does and save what is presented after each step
     * (menu_test_<n>.bmp): Controls page, toggle free play, Display page, step
     * the resolution, close. The menu path, testable with no input device. */
    int menu_test_step = -1;
    { static long at = -2; static long nfr;
      if (at == -2) { const char *ev = getenv("RR_MENU_TEST"); at = ev ? atol(ev) : -1; }
      if (at >= 0) {
          const long k = ++nfr - at;
          if (k >= 0 && k % 10 == 0 && k / 10 <= 4) {
              menu_test_step = (int)(k / 10);
              switch (menu_test_step) {
              case 0: rr_ui_set_open(true); rr_ui_test_goto(3, 0); break;       /* Controls, Free play */
              case 1: rr_ui_test_nav(RR_UI_OK); break;                           /* toggle it */
              case 2: rr_ui_test_goto(1, 0); break;                              /* Display */
              case 3: rr_ui_test_nav(RR_UI_DOWN); rr_ui_test_nav(RR_UI_DOWN); rr_ui_test_nav(RR_UI_DOWN);
                      rr_ui_test_nav(RR_UI_RIGHT); break;                        /* Resolution, next */
              case 4: rr_ui_test_nav(RR_UI_BACK); break;                         /* Esc */
              }
          }
      } }
    if (rr_ui_quit_requested()) return false;
    if (!paused && !rr_ui_is_open() && !rr_input_replaying()) {
        set_bit(0x1000, held(RR_COIN1));
        set_bit(0x0200, held(RR_COIN2));
        set_bit(0x0800, held(RR_SERVICE));
        set_bit(0x0400, held(RR_TEST));          /* PORT_SERVICE */
        set_bit(0x0001, held(RR_SHIFT_DOWN));
        set_bit(0x0002, held(RR_SHIFT_UP));
        set_bit(0x0040, held(RR_VIEW));

        /* an analog source out of its deadzone wins; keys/buttons ramp like MAME's KEYDELTA */
        int sx, pv;
        int dir = (held(RR_STEER_RIGHT) ? 1 : 0) - (held(RR_STEER_LEFT) ? 1 : 0);
        if (!dir && pad_steer(&sx)) g_hw.steer = (uint16_t)(0x800 + sx * 0x580 / 32767);
        else ramp(&g_hw.steer, 0x800 + dir * 0x580, 0x280, 0xD80, dir ? g_steer_speed : g_steer_return);
        if (!held(RR_GAS) && pad_pedal(true, &pv)) g_hw.gas = (uint16_t)pv;
        else ramp(&g_hw.gas, held(RR_GAS) ? 0x610 : 0, 0, 0x610, 160);
        if (!held(RR_BRAKE) && pad_pedal(false, &pv)) g_hw.brake = (uint16_t)pv;
        else ramp(&g_hw.brake, held(RR_BRAKE) ? 0x610 : 0, 0, 0x610, 160);
    }

    /* the window may have been resized: keep the render size in step (a
     * native or widescreen size follows the window; it lands next frame) */
    apply_render_size();
    void *px; int pitch;
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_RenderClear(ren);
    {
        int fw, fh; const uint32_t *fr = rr_video_output(&fw, &fh);
        if (fw != tex_w || fh != tex_h) {                   /* the render size changed */
            SDL_Texture *nt = SDL_CreateTexture(ren, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING, fw, fh);
            if (nt) { SDL_DestroyTexture(tex); tex = nt; tex_w = fw; tex_h = fh; apply_scaling(); }
        }
        if (fw == tex_w && fh == tex_h && SDL_LockTexture(tex, NULL, &px, &pitch) == 0) {
            for (int y = 0; y < fh; y++) memcpy((uint8_t *)px + (size_t)y * pitch, fr + (size_t)y * fw, (size_t)fw * 4);
            SDL_UnlockTexture(tex);
        }
        SDL_Rect dst = picture_rect(); SDL_RenderCopy(ren, tex, NULL, &dst);
    }
    if (rr_ui_is_open()) {                                   /* the menu, over the dimmed game */
        SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 150);
        SDL_RenderFillRect(ren, NULL);
        SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_NONE);
        bool q = false; rr_ui_draw(&q);
    }
    if (menu_test_step >= 0) {
        int ow, oh; SDL_GetRendererOutputSize(ren, &ow, &oh);
        SDL_Surface *sf = SDL_CreateRGBSurfaceWithFormat(0, ow, oh, 32, SDL_PIXELFORMAT_XRGB8888);
        if (sf && SDL_RenderReadPixels(ren, NULL, SDL_PIXELFORMAT_XRGB8888, sf->pixels, sf->pitch) == 0) {
            char pth[64]; snprintf(pth, sizeof pth, "menu_test_%d.bmp", menu_test_step); SDL_SaveBMP(sf, pth);
            fprintf(stderr, "[HOST] menu test step %d -> %s\n", menu_test_step, pth);
        }
        if (sf) SDL_FreeSurface(sf);
    }
    /* RR_DISPLAY_TEST=<frame>: from that frame, step through window sizes,
     * scaling modes, an odd window and fullscreen on/off, 30 frames apart; log
     * window / drawable / viewport and save what was actually presented. */
    { static long at = -2, n;
      if (at == -2) { const char *ev = getenv("RR_DISPLAY_TEST"); at = ev ? atol(ev) : -1; }
      if (at >= 0 && ++n >= at && (n - at) % 30 == 0) {
          int step = (int)((n - at) / 30);
          static const char *what[] = { "1x smooth", "2x sharp", "3x integer", "4x smooth", "odd 1000x500 integer",
                                        "odd 1000x500 smooth", "fullscreen", "back to window 2x", NULL };
          if (step > 0) {                             /* capture the state set on the previous step */
              int ow, oh, ww, wh; SDL_Rect vp = picture_rect();
              SDL_GetRendererOutputSize(ren, &ow, &oh); SDL_GetWindowSize(win, &ww, &wh);
              fprintf(stderr, "[DISPLAY] %-22s window %4dx%-4d drawable %4dx%-4d picture %d,%d %dx%d scale %.4f x %.4f fs=%d\n",
                      what[step - 1], ww, wh, ow, oh, vp.x, vp.y, vp.w, vp.h, vp.w / 640.0, vp.h / 480.0,
                      (SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN_DESKTOP) == SDL_WINDOW_FULLSCREEN_DESKTOP);
              SDL_Surface *sf = SDL_CreateRGBSurfaceWithFormat(0, ow, oh, 32, SDL_PIXELFORMAT_XRGB8888);
              if (sf && SDL_RenderReadPixels(ren, NULL, SDL_PIXELFORMAT_XRGB8888, sf->pixels, sf->pitch) == 0) {
                  char pth[64]; snprintf(pth, sizeof pth, "display_%d.bmp", step - 1); SDL_SaveBMP(sf, pth);
              }
              if (sf) SDL_FreeSurface(sf);
          }
          if (what[step]) {
              switch (step) {
              case 0: g_cfg_scale = 1; g_cfg_scaling = 0; break;
              case 1: g_cfg_scale = 2; g_cfg_scaling = 1; break;
              case 2: g_cfg_scale = 3; g_cfg_scaling = 2; break;
              case 3: g_cfg_scale = 4; g_cfg_scaling = 0; break;
              case 4: g_cfg_scaling = 2; break;
              case 5: g_cfg_scaling = 0; break;
              case 6: g_cfg_winmode = 1; break;
              case 7: g_cfg_winmode = 0; g_cfg_scale = 2; break;
              }
              apply_scaling();
              if (step == 4) SDL_SetWindowSize(win, 1000, 500);
              else if (step != 5) apply_fullscreen();
          } else at = -1;
      } }
    SDL_RenderPresent(ren);

    /* pacing: vsync blocks in RenderPresent; otherwise sleep to the board's
     * 59.906 Hz, the last millisecond spun for precision */
    if (!vsync) {
        next_ns += FRAME_NS;
        uint64_t now = now_ns();
        if (next_ns > now) {
            uint64_t left = next_ns - now;
            if (left > 2000000ull) SDL_Delay((Uint32)((left - 1500000ull) / 1000000ull));
            while (now_ns() < next_ns) ;
        } else if (now - next_ns > 100000000ull) next_ns = now;   /* fell behind: resync, no catch-up burst */
    }
    return true;
}

bool rr_host_paused(void) { return paused || rr_ui_is_open(); }

void rr_host_close(void)
{
    rr_input_record_stop();
    for (int d = 0; d < MAX_DEV; d++) if (dev[d].gc || dev[d].js) dev_remove(dev[d].id);
    if (win) { rr_ui_shutdown(); SDL_DestroyTexture(tex); SDL_DestroyRenderer(ren); SDL_DestroyWindow(win); SDL_Quit(); win = NULL; }
}

/* rr --joytest: list every device and print axis/button/hat changes live, so a
 * wheel or stick can be mapped in rr_controls.cfg. Ctrl-C to stop. */
int rr_host_joytest(void)
{
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (SDL_Init(SDL_INIT_GAMECONTROLLER) != 0) { fprintf(stderr, "SDL: %s\n", SDL_GetError()); return 1; }
    load_pad_db();
    SDL_Delay(200); SDL_PumpEvents();
    printf("%d device(s). Move every axis and press every button; Ctrl-C to stop.\n", SDL_NumJoysticks());
    SDL_Joystick *js[16] = {0};
    for (int i = 0; i < SDL_NumJoysticks() && i < 16; i++) {
        js[i] = SDL_JoystickOpen(i);
        printf("  %d: %s -- %s, %d axes, %d buttons, %d hats\n", i, SDL_JoystickName(js[i]),
               SDL_IsGameController(i) ? "GAMEPAD (standard layout, no mapping needed)" : "RAW joystick (map by number)",
               SDL_JoystickNumAxes(js[i]), SDL_JoystickNumButtons(js[i]), SDL_JoystickNumHats(js[i]));
    }
    fflush(stdout);
    SDL_Event e;
    for (;;) {
        if (!SDL_WaitEvent(&e)) break;
        if (e.type == SDL_QUIT) break;
        if (e.type == SDL_JOYAXISMOTION && (e.jaxis.value > 4000 || e.jaxis.value < -4000 || (e.jaxis.value > -300 && e.jaxis.value < 300)))
            printf("dev %d  axis %d = %6d\n", e.jaxis.which, e.jaxis.axis, e.jaxis.value);
        if (e.type == SDL_JOYBUTTONDOWN) printf("dev %d  button %d down\n", e.jaxis.which, e.jbutton.button);
        if (e.type == SDL_JOYHATMOTION) printf("dev %d  hat %d = %d\n", e.jhat.which, e.jhat.hat, e.jhat.value);
        if (e.type == SDL_JOYDEVICEADDED) printf("device added: %s\n", SDL_JoystickNameForIndex(e.jdevice.which));
        fflush(stdout);
    }
    SDL_Quit();
    return 0;
}
