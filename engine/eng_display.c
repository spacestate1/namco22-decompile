/* eng_display.c -- see eng_display.h. The logic is Rave Racer's (raverace/src/rr_host.c), made game-independent. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "eng_display.h"
#include "eng_cfg.h"

#define BASE_W 640
#define BASE_H 480

eng_display_t g_eng_disp = { 0, 0, 2, 640, 480, 1, 0, 100 };
static SDL_Window *win;
static void (*volume_hook)(int);

static const struct { int w, h; } res_list[] = {
    { 640, 480 }, { 0, 0 },                                 /* the board; native (window pixels) */
    { 800, 600 }, { 960, 720 }, { 1024, 768 }, { 1280, 960 }, { 1440, 1080 }, { 1600, 1200 }, { 1920, 1440 },
    { 1280, 720 }, { 1600, 900 }, { 1920, 1080 }, { 2560, 1440 }, { 3840, 2160 },
    { 1280, 800 }, { 1920, 1200 }, { 2560, 1080 }, { 3440, 1440 },
};
#define NRES ((int)(sizeof res_list / sizeof res_list[0]))
static const char *winmode_name[3] = { "Windowed", "Fullscreen (desktop)", "Fullscreen (exclusive)" };
static const char *aspect_cfg[4]   = { "stretch", "4:3", "8:7", "16:9" };
static const char *aspect_name_[4] = { "Stretch to window", "4:3", "8:7", "16:9" };
static const char *scaling_cfg[3]  = { "smooth", "sharp", "integer" };
static const char *scaling_name_[3] = { "Smooth", "Sharp", "Integer" };

int  eng_disp_res_count(void) { return NRES; }
void eng_disp_res_get(int i, int *w, int *h) { if (i < 0 || i >= NRES) i = 0; *w = res_list[i].w; *h = res_list[i].h; }
void eng_disp_res_label(int w, int h, char *out, size_t n)
{
    if (h <= 0) snprintf(out, n, "Native (window size)");
    else if (w == BASE_W && h == BASE_H) snprintf(out, n, "640 x 480  (arcade)");
    else snprintf(out, n, "%d x %d", w, h);
}
const char *eng_disp_winmode_name(int m) { return winmode_name[m < 0 ? 0 : m > 2 ? 2 : m]; }
const char *eng_disp_aspect_name(int a)  { return aspect_name_[a < 0 ? 0 : a > 3 ? 3 : a]; }
const char *eng_disp_scaling_name(int s) { return scaling_name_[s < 0 ? 0 : s > 2 ? 2 : s]; }

/* ---- the window's size ---------------------------------------------------- */
int eng_disp_win_w(int k) { return g_eng_disp.wide ? ((BASE_H * k * 16 / 9) + 1) & ~1 : BASE_W * k; }
int eng_disp_win_h(int k) { return BASE_H * k; }

static void out_size(int *w, int *h)             /* the window's drawable, in pixels */
{
    *w = BASE_W; *h = BASE_H;
    if (win) SDL_GL_GetDrawableSize(win, w, h);
    if (*w < 1 || *h < 1) { *w = BASE_W; *h = BASE_H; }
}

/* the largest window scale that fits the display's usable area */
static int max_scale(void)
{
    SDL_Rect b;
    if (!win || SDL_GetDisplayUsableBounds(SDL_GetWindowDisplayIndex(win), &b) != 0) return 4;
    const int kw = b.w / eng_disp_win_w(1), kh = (b.h - 40) / BASE_H;      /* leave room for a title bar */
    int k = kw < kh ? kw : kh;
    return k < 1 ? 1 : k > 4 ? 4 : k;
}

void eng_disp_render_size(int *w, int *h)
{
    int dw, dh;
    out_size(&dw, &dh);
    double ar = 4.0 / 3.0;
    if (g_eng_disp.wide && (double)dw / dh > ar) ar = (double)dw / dh;
    int H = g_eng_disp.res_h;
    if (H <= 0) H = g_eng_disp.wide ? dh : (dh < dw * 3 / 4 ? dh : dw * 3 / 4);   /* native */
    if (H < 240) H = 240;
    *h = H;
    *w = ((int)(H * ar + 0.5) + 1) & ~1;
}

/* The picture's rectangle in drawable pixels: centred, ONE scale for both axes. Its shape: the render's own (4:3, or the
 * window's with widescreen), unless widescreen is off and the aspect says stretch / 8:7 / 16:9. Integer mode: the largest
 * whole multiple of the render size, or a plain fit if none fits. */
SDL_Rect eng_disp_picture_rect(int bw, int bh)
{
    int ow, oh;
    out_size(&ow, &oh);
    double ar = (double)bw / bh;
    if (!g_eng_disp.wide) {
        if (g_eng_disp.aspect == 0) { SDL_Rect r = { 0, 0, ow, oh }; return r; }
        ar = g_eng_disp.aspect == 2 ? 8.0 / 7.0 : g_eng_disp.aspect == 3 ? 16.0 / 9.0 : 4.0 / 3.0;
    }
    int w, h;
    const bool own_shape = fabs(ar - (double)bw / bh) < 0.01;
    if (g_eng_disp.scaling == 2 && own_shape && ow >= bw && oh >= bh) {
        const int k = ow / bw < oh / bh ? ow / bw : oh / bh;
        w = bw * k; h = bh * k;
    } else if (ow <= oh * ar) { w = ow; h = (int)(ow / ar + 0.5); }
    else { h = oh; w = (int)(oh * ar + 0.5); }
    SDL_Rect r = { (ow - w) / 2, (oh - h) / 2, w, h };
    return r;
}

bool eng_disp_sharp(void) { return g_eng_disp.scaling != 0; }

/* ---- applying the window mode ---------------------------------------------- */
static void apply_window(void)
{
    if (!win) return;
    if (g_eng_disp.winmode < 0 || g_eng_disp.winmode > 2) g_eng_disp.winmode = 0;
    if (g_eng_disp.winmode == 2) {
        /* EXCLUSIVE: switch the monitor to the mode nearest the chosen resolution (native: the desktop's own) */
        int disp = SDL_GetWindowDisplayIndex(win); if (disp < 0) disp = 0;
        SDL_DisplayMode want = { 0, g_eng_disp.res_w, g_eng_disp.res_h, 0, 0 }, got;
        if (g_eng_disp.res_h <= 0 || !SDL_GetClosestDisplayMode(disp, &want, &got)) SDL_GetDesktopDisplayMode(disp, &got);
        SDL_SetWindowFullscreen(win, 0);
        SDL_SetWindowDisplayMode(win, &got);
        if (SDL_SetWindowFullscreen(win, SDL_WINDOW_FULLSCREEN) != 0) {
            fprintf(stderr, "[DISPLAY] exclusive %dx%d failed (%s), using desktop fullscreen\n", got.w, got.h, SDL_GetError());
            g_eng_disp.winmode = 1;
            SDL_SetWindowFullscreen(win, SDL_WINDOW_FULLSCREEN_DESKTOP);
        } else fprintf(stderr, "[DISPLAY] exclusive fullscreen %dx%d @ %d Hz\n", got.w, got.h, got.refresh_rate);
    } else SDL_SetWindowFullscreen(win, g_eng_disp.winmode ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
    SDL_ShowCursor(g_eng_disp.winmode ? SDL_DISABLE : SDL_ENABLE);
    if (!g_eng_disp.winmode) {                          /* back to the chosen window size, centred */
        const int ms = max_scale();
        const int k = g_eng_disp.scale < ms ? g_eng_disp.scale : ms;
        if (k != g_eng_disp.scale) fprintf(stderr, "[DISPLAY] %dx does not fit this display; using %dx\n", g_eng_disp.scale, k);
        SDL_SetWindowSize(win, eng_disp_win_w(k), eng_disp_win_h(k));
        SDL_SetWindowPosition(win, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    }
}

/* ---- loading ---------------------------------------------------------------- */
void eng_disp_load(const char *cfg_path, int scale, bool fullscreen)
{
    eng_cfg_load(cfg_path);
    eng_display_t *d = &g_eng_disp;
    d->wide = eng_cfg_int("widescreen", 0) != 0;
    d->winmode = eng_cfg_int("window_mode", eng_cfg_int("fullscreen", 0) ? 1 : 0);
    if (d->winmode < 0 || d->winmode > 2) d->winmode = 0;
    d->scale = eng_cfg_int("window_scale", 2);
    if (d->scale < 1 || d->scale > 4) d->scale = 2;
    d->res_w = BASE_W; d->res_h = BASE_H;
    const char *r = eng_cfg_get("resolution");
    if (r && !strcmp(r, "native")) { d->res_w = 0; d->res_h = 0; }
    else if (r) { int w, h; if (sscanf(r, "%dx%d", &w, &h) == 2 && w > 0 && h > 0) { d->res_w = w; d->res_h = h; } }
    d->aspect = 1;
    const char *a = eng_cfg_get("aspect");
    for (int i = 0; a && i < 4; i++) if (!strcmp(a, aspect_cfg[i])) d->aspect = i;
    d->scaling = 0;
    const char *s = eng_cfg_get("scaling");
    for (int i = 0; s && i < 3; i++) if (!strcmp(s, scaling_cfg[i])) d->scaling = i;
    d->volume = eng_cfg_int("volume", 100);
    if (d->volume < 0) d->volume = 0;
    if (d->volume > 100) d->volume = 100;
    if (scale > 0) d->scale = scale > 4 ? 4 : scale;             /* --window N overrides the saved size */
    if (fullscreen && !d->winmode) d->winmode = 1;
    if (getenv("ENG_WIDESCREEN")) d->wide = atoi(getenv("ENG_WIDESCREEN")) != 0;   /* tests: force it without touching the cfg */
}

void eng_disp_attach(SDL_Window *w)
{
    win = w;
    if (g_eng_disp.winmode == 2) apply_window();                                  /* exclusive: set the mode */
    else if (!g_eng_disp.winmode && g_eng_disp.scale > max_scale()) apply_window();   /* too big for this display: shrink */
    if (g_eng_disp.winmode) SDL_ShowCursor(SDL_DISABLE);
    if (volume_hook) volume_hook(g_eng_disp.volume);
}

/* ---- the setters the menu drives: apply, and save ---------------------------- */
void eng_disp_set_winmode(int m)
{
    g_eng_disp.winmode = m < 0 ? 0 : m > 2 ? 2 : m;
    apply_window();
    eng_cfg_set_int("window_mode", g_eng_disp.winmode);
    eng_cfg_set_int("fullscreen", g_eng_disp.winmode ? 1 : 0);
}
void eng_disp_toggle_fullscreen(void) { eng_disp_set_winmode(g_eng_disp.winmode ? 0 : 1); }
void eng_disp_set_scale(int k)
{
    g_eng_disp.scale = k < 1 ? 1 : k > 4 ? 4 : k;
    eng_cfg_set_int("window_scale", g_eng_disp.scale);
    if (!g_eng_disp.winmode) apply_window();
}
void eng_disp_set_res(int w, int h)
{
    g_eng_disp.res_w = w; g_eng_disp.res_h = h;
    /* a wide resolution is what widescreen is for */
    if (h > 0 && w * 3 > h * 4 + 8 && !g_eng_disp.wide) { g_eng_disp.wide = 1; eng_cfg_set("widescreen", "1"); }
    char v[24];
    if (h <= 0) snprintf(v, sizeof v, "native"); else snprintf(v, sizeof v, "%dx%d", w, h);
    eng_cfg_set("resolution", v);
    if (g_eng_disp.winmode == 2) apply_window();          /* exclusive: it is the display mode too */
}
void eng_disp_set_wide(int on)
{
    g_eng_disp.wide = on != 0;
    eng_cfg_set("widescreen", g_eng_disp.wide ? "1" : "0");
    if (!g_eng_disp.winmode) apply_window();              /* a window takes the new shape */
}
void eng_disp_set_aspect(int a) { g_eng_disp.aspect = a < 0 ? 0 : a > 3 ? 3 : a; eng_cfg_set("aspect", aspect_cfg[g_eng_disp.aspect]); }
void eng_disp_set_scaling(int s) { g_eng_disp.scaling = s < 0 ? 0 : s > 2 ? 2 : s; eng_cfg_set("scaling", scaling_cfg[g_eng_disp.scaling]); }
void eng_disp_set_volume_hook(void (*fn)(int)) { volume_hook = fn; }
void eng_disp_set_volume(int pct)
{
    g_eng_disp.volume = pct < 0 ? 0 : pct > 100 ? 100 : pct;
    if (volume_hook) volume_hook(g_eng_disp.volume);
    eng_cfg_set_int("volume", g_eng_disp.volume);
}
