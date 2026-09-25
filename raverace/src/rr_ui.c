/*
 * rr_ui.c -- the menu (Escape, or pad Start): Nuklear, as in Prop Cycle.
 *
 * One panel over the paused game, with the pages Prop Cycle's menu bar has
 * that apply here -- File / Display / Audio / Controls / Record -- as tabs
 * across the top and one row per setting. It works entirely from the
 * KEYBOARD or a PAD as well as the mouse (a popup menu bar needs a mouse click
 * to open, so this is a tabbed panel instead):
 *
 *   Up/Down       move between rows (above the first row: the tab strip)
 *   Left/Right    change the value (on the tab strip: switch page)
 *   Enter/Space   select / toggle / step the value
 *   Tab, PgUp/PgDn, pad LB/RB   switch page from anywhere
 *   Esc (pad B / Start)         close
 *
 * Every change goes through the host's setters (rr_host.c), which apply it and
 * save it to rr_controls.cfg at once.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <SDL.h>
#include "eng_gl.h"

#define NK_INCLUDE_FIXED_TYPES
#define NK_INCLUDE_STANDARD_IO
#define NK_INCLUDE_STANDARD_VARARGS
#define NK_INCLUDE_DEFAULT_ALLOCATOR
#define NK_INCLUDE_VERTEX_BUFFER_OUTPUT
#define NK_INCLUDE_FONT_BAKING
#define NK_INCLUDE_DEFAULT_FONT
#define NK_IMPLEMENTATION
#define NK_SDL_GL2_IMPLEMENTATION
#include "../../third_party/nuklear.h"          /* the tree's one copy, shared with Prop Cycle */
#include "../../third_party/nuklear_sdl_gl2.h"  /* Prop Cycle's backend: the window is OpenGL */

#include "rr_ui.h"
#include "rr_input.h"
#include "rr_hw.h"
#include "rr_sound.h"

static struct nk_context *ctx;
static SDL_Window *uwin;
static bool open_, quit_req;
static int rebinding = -1;             /* the action waiting for a key, or -1 */
static float ui_scale = 1.0f;          /* drawable pixels per window unit (HiDPI) */

enum { T_FILE, T_DISPLAY, T_AUDIO, T_CONTROLS, T_RECORD, T_N };
static const char *tab_name[T_N] = { "File", "Display", "Audio", "Controls", "Record" };
static int tab = T_DISPLAY;
static int row = 0;                    /* -1 = the tab strip */
static bool kb_moved;                  /* keep the selected row in view after a key */

/* ---- the rows ------------------------------------------------------------- */
enum { D_WIDE, D_DRAW, D_MODE, D_SIZE, D_RES, D_ASPECT, D_SCALING, D_N };
static int nrows(int t)
{
    switch (t) {
    case T_FILE: return 2;
    case T_DISPLAY: return D_N;
    case T_AUDIO: return 1;
    case T_CONTROLS: return 1 + RR_ACT_N;
    case T_RECORD: return 1;
    }
    return 0;
}
/* does the row take Left/Right (a value), or is it an action? */
static bool has_value(int t, int r)
{
    if (t == T_DISPLAY || t == T_AUDIO) return true;
    if (t == T_CONTROLS) return r == 0;
    if (t == T_RECORD) return true;
    return false;
}
static bool row_enabled(int t, int r)
{
    if (t == T_DISPLAY && r == D_SIZE) return g_cfg_winmode == 0;
    if (t == T_DISPLAY && r == D_ASPECT) return !g_cfg_wide;
    return true;
}
static void row_text(int t, int r, char *label, size_t ln, char *value, size_t vn)
{
    static const char *wm[3] = { "Windowed", "Fullscreen (desktop)", "Fullscreen (exclusive)" };
    static const char *asp[4] = { "Stretch to window", "4:3", "8:7", "16:9" };
    static const char *sc[3] = { "Smooth", "Sharp", "Integer" };
    *value = 0;
    switch (t) {
    case T_FILE: snprintf(label, ln, "%s", r == 0 ? "Resume" : "Exit"); break;
    case T_DISPLAY:
        switch (r) {
        case D_DRAW:    snprintf(label, ln, "Draw distance"); snprintf(value, vn, "%s", rr_host_draw_name(g_cfg_draw)); break;
        case D_WIDE:    snprintf(label, ln, "Widescreen"); snprintf(value, vn, "%s", g_cfg_wide ? "ON (fill the window)" : "OFF (4:3)"); break;
        case D_MODE:    snprintf(label, ln, "Window mode"); snprintf(value, vn, "%s", wm[g_cfg_winmode]); break;
        case D_SIZE:    snprintf(label, ln, "Window size"); { extern int rr_host_win_w(int), rr_host_win_h(int);
                          snprintf(value, vn, "%dx  (%d x %d)", g_cfg_scale, rr_host_win_w(g_cfg_scale), rr_host_win_h(g_cfg_scale)); } break;
        case D_RES:     snprintf(label, ln, "Resolution");
                        if (g_cfg_res_h <= 0) snprintf(value, vn, "Native (window size)");
                        else if (g_cfg_res_w == 640 && g_cfg_res_h == 480) snprintf(value, vn, "640 x 480  (arcade)");
                        else snprintf(value, vn, "%d x %d", g_cfg_res_w, g_cfg_res_h);
                        break;
        case D_ASPECT:  snprintf(label, ln, "Aspect ratio"); snprintf(value, vn, "%s", asp[g_cfg_aspect]); break;
        case D_SCALING: snprintf(label, ln, "Scaling"); snprintf(value, vn, "%s", sc[g_cfg_scaling]); break;
        }
        break;
    case T_AUDIO: snprintf(label, ln, "Volume"); snprintf(value, vn, "%d%%", g_cfg_volume); break;
    case T_CONTROLS:
        if (r == 0) { snprintf(label, ln, "Free play"); snprintf(value, vn, "%s", rr_hw_freeplay() ? "ON" : "OFF (coins)"); }
        else {
            const int a = r - 1;
            snprintf(label, ln, "%s", rr_input_action_name(a));
            for (char *c = label; *c; c++) if (*c == '_') *c = ' ';
            if (label[0] >= 'a' && label[0] <= 'z') label[0] = (char)(label[0] - 'a' + 'A');
            const char *k = rebinding == a ? "press a key..." : SDL_GetScancodeName(g_bind[a].nkeys ? g_bind[a].keys[0] : SDL_SCANCODE_UNKNOWN);
            snprintf(value, vn, "%s", (k && *k) ? k : "(none)");
        }
        break;
    case T_RECORD: snprintf(label, ln, "Record input"); snprintf(value, vn, "%s", rr_input_recording() ? "ON  (recording...)" : "OFF"); break;
    }
}
static int cyc(int v, int d, int n) { return ((v + d) % n + n) % n; }
/* dir: 0 = Enter / click, -1 / +1 = Left / Right */
static void row_change(int t, int r, int dir)
{
    if (!row_enabled(t, r)) return;
    const int d = dir ? dir : 1;
    switch (t) {
    case T_FILE: if (dir == 0) { if (r == 0) open_ = false; else quit_req = true; } break;
    case T_DISPLAY:
        switch (r) {
        case D_WIDE:    rr_host_set_wide(!g_cfg_wide); break;
        case D_DRAW:    rr_host_set_draw(cyc(g_cfg_draw, d, 4)); break;
        case D_MODE:    rr_host_set_winmode(cyc(g_cfg_winmode, d, 3)); break;
        case D_SIZE:    rr_host_set_scale(cyc(g_cfg_scale - 1, d, 4) + 1); break;
        case D_RES: {
            int i = 0, n = rr_host_res_count(), w, h;
            for (int k = 0; k < n; k++) { rr_host_res_get(k, &w, &h); if (w == g_cfg_res_w && h == g_cfg_res_h) i = k; }
            rr_host_res_get(cyc(i, d, n), &w, &h);
            rr_host_set_res(w, h);
            break; }
        case D_ASPECT:  rr_host_set_aspect(cyc(g_cfg_aspect, d, 4)); break;
        case D_SCALING: rr_host_set_scaling(cyc(g_cfg_scaling, d, 3)); break;
        }
        break;
    case T_AUDIO: {
        int v = g_cfg_volume + (dir ? dir * 5 : 10);
        if (v > 100) v = dir ? 100 : 0;                     /* Enter wraps 100 -> mute */
        if (v < 0) v = 0;
        rr_host_set_volume(v);
        break; }
    case T_CONTROLS:
        if (r == 0) rr_host_set_freeplay(!rr_hw_freeplay());
        else if (dir == 0) rebinding = r - 1;
        break;
    case T_RECORD: rr_host_toggle_record(); break;
    }
}

/* ---- lifecycle -------------------------------------------------------------- */
bool rr_ui_init(SDL_Window *win)
{
    uwin = win;
    ctx = nk_sdl_init(win);
    if (!ctx) return false;
    /* HiDPI: draw at the drawable's density, lay out in window units (the
     * backend's mouse coordinates are window units) */
    int ww, wh, ow, oh;
    SDL_GetWindowSize(win, &ww, &wh);
    SDL_GL_GetDrawableSize(win, &ow, &oh);
    ui_scale = ww > 0 ? (float)ow / ww : 1.0f;
    if (ui_scale < 1.0f) ui_scale = 1.0f;
    struct nk_font_atlas *atlas;
    struct nk_font_config cfg = nk_font_config(0);
    nk_sdl_font_stash_begin(&atlas);
    struct nk_font *font = nk_font_atlas_add_default(atlas, 15 * ui_scale, &cfg);
    nk_sdl_font_stash_end();
    font->handle.height /= ui_scale;
    nk_style_set_font(ctx, &font->handle);
    return true;
}
void rr_ui_shutdown(void) { if (ctx) nk_sdl_shutdown(); ctx = NULL; }
bool rr_ui_is_open(void) { return open_; }
bool rr_ui_quit_requested(void) { return quit_req; }
void rr_ui_set_open(bool on) { open_ = on; rebinding = -1; if (on && row >= nrows(tab)) row = 0; }
void rr_ui_input_begin(void) { if (ctx) nk_input_begin(ctx); }
void rr_ui_input_end(void)   { if (ctx) nk_input_end(ctx); }

/* one navigation step from the keyboard, a pad or a hat */
enum { K_UP, K_DOWN, K_LEFT, K_RIGHT, K_OK, K_BACK, K_TABPREV, K_TABNEXT };
static void nav(int k)
{
    const int n = nrows(tab);
    switch (k) {
    case K_UP:    row = row <= -1 ? n - 1 : row - 1; break;
    case K_DOWN:  row = row >= n - 1 ? -1 : row + 1; break;
    case K_LEFT:  if (row < 0) tab = cyc(tab, -1, T_N); else if (has_value(tab, row)) row_change(tab, row, -1); break;
    case K_RIGHT: if (row < 0) tab = cyc(tab, +1, T_N); else if (has_value(tab, row)) row_change(tab, row, +1); break;
    case K_OK:    if (row < 0) row = 0; else row_change(tab, row, 0); break;
    case K_BACK:  open_ = false; break;
    case K_TABPREV: tab = cyc(tab, -1, T_N); row = 0; break;
    case K_TABNEXT: tab = cyc(tab, +1, T_N); row = 0; break;
    }
    if (row >= nrows(tab)) row = nrows(tab) - 1;
}

bool rr_ui_event(SDL_Event *e)
{
    if (!ctx || !open_) return false;
    if (rebinding >= 0) {                                /* the next key is the new binding */
        if (e->type == SDL_KEYDOWN && !e->key.repeat) {
            if (e->key.keysym.scancode != SDL_SCANCODE_ESCAPE) rr_input_bind_key(rebinding, e->key.keysym.scancode);
            rebinding = -1;
        } else if (e->type == SDL_CONTROLLERBUTTONDOWN && e->cbutton.button == SDL_CONTROLLER_BUTTON_B) rebinding = -1;
        return true;
    }
    int k = -1;
    if (e->type == SDL_KEYDOWN) {
        const bool rep = e->key.repeat;                  /* held arrows repeat; the rest do not */
        switch (e->key.keysym.scancode) {
        case SDL_SCANCODE_UP: k = K_UP; break;
        case SDL_SCANCODE_DOWN: k = K_DOWN; break;
        case SDL_SCANCODE_LEFT: k = K_LEFT; break;
        case SDL_SCANCODE_RIGHT: k = K_RIGHT; break;
        case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: case SDL_SCANCODE_SPACE: if (!rep) k = K_OK; break;
        case SDL_SCANCODE_ESCAPE: case SDL_SCANCODE_BACKSPACE: if (!rep) k = K_BACK; break;
        case SDL_SCANCODE_TAB: if (!rep) k = (e->key.keysym.mod & KMOD_SHIFT) ? K_TABPREV : K_TABNEXT; break;
        case SDL_SCANCODE_PAGEUP: if (!rep) k = K_TABPREV; break;
        case SDL_SCANCODE_PAGEDOWN: if (!rep) k = K_TABNEXT; break;
        default: break;
        }
    } else if (e->type == SDL_CONTROLLERBUTTONDOWN) {
        switch (e->cbutton.button) {
        case SDL_CONTROLLER_BUTTON_DPAD_UP: k = K_UP; break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: k = K_DOWN; break;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT: k = K_LEFT; break;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: k = K_RIGHT; break;
        case SDL_CONTROLLER_BUTTON_A: k = K_OK; break;
        case SDL_CONTROLLER_BUTTON_B: case SDL_CONTROLLER_BUTTON_START: case SDL_CONTROLLER_BUTTON_RIGHTSTICK: k = K_BACK; break;
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: k = K_TABPREV; break;
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: k = K_TABNEXT; break;
        default: break;
        }
    } else if (e->type == SDL_JOYHATMOTION) {           /* raw sticks: the hat navigates */
        if (e->jhat.value & SDL_HAT_UP) k = K_UP; else if (e->jhat.value & SDL_HAT_DOWN) k = K_DOWN;
        else if (e->jhat.value & SDL_HAT_LEFT) k = K_LEFT; else if (e->jhat.value & SDL_HAT_RIGHT) k = K_RIGHT;
    }
    if (k >= 0) { nav(k); kb_moved = true; return true; }
    if (e->type == SDL_KEYDOWN || e->type == SDL_KEYUP) return true;   /* the game does not see keys while open */
    nk_sdl_handle_event(e);                              /* mouse */
    return true;
}
/* RR_MENU_TEST and friends drive the menu without an input device */
void rr_ui_test_nav(int k) { nav(k); kb_moved = true; }
void rr_ui_test_goto(int t, int r) { tab = t; row = r; }

/* ---- drawing ---------------------------------------------------------------- */
static void labelf(nk_flags align, const char *fmt, ...)
{
    char b[160];
    va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    nk_label(ctx, b, align);
}

void rr_ui_draw(bool *quit)
{
    if (quit_req) *quit = true;
    if (!ctx || !open_) return;
    int ww, wh;
    SDL_GetWindowSize(uwin, &ww, &wh);
    /* THE MENU BAR across the top of the window, as in Prop Cycle. The chosen
     * page drops down under its title; on the keyboard the bar is the row
     * above the first row of the dropdown. */
    static const float title_w[T_N] = { 50, 80, 70, 90, 80 };
    float title_x[T_N], x = 4;
    for (int t = 0; t < T_N; t++) { title_x[t] = x; x += title_w[t] + 4; }
    if (nk_begin(ctx, "menubar", nk_rect(0, 0, (float)ww, 28), NK_WINDOW_NO_SCROLLBAR)) {
        nk_menubar_begin(ctx);
        nk_layout_row_begin(ctx, NK_STATIC, 20, T_N + 1);
        for (int t = 0; t < T_N; t++) {
            nk_layout_row_push(ctx, title_w[t]);
            if (nk_select_label(ctx, tab_name[t], NK_TEXT_CENTERED, t == tab) && t != tab) { tab = t; row = 0; }
        }
        nk_layout_row_push(ctx, (float)ww - x - 8 > 60 ? (float)ww - x - 8 : 60);
        nk_label(ctx, row < 0 ? "<- Left/Right ->   Down: open   Esc: close" : "Arrows, Enter, Esc", NK_TEXT_RIGHT);
        nk_menubar_end(ctx);
    }
    nk_end(ctx);

    /* the dropdown: sized to its page, under its title, inside the window */
    static const float drop_w[T_N] = { 220, 440, 300, 340, 380 };
    const int n0 = nrows(tab);
    const float rh0 = tab == T_CONTROLS ? 20 : 26;
    float dh = 48 + n0 * (rh0 + 4) + (tab == T_DISPLAY ? 88 : tab == T_FILE ? 0 : 44);
    if (dh > wh - 34) dh = (float)wh - 34;
    float dw = drop_w[tab] < ww - 8 ? drop_w[tab] : (float)ww - 8;
    float dx = title_x[tab];
    if (dx + dw > ww - 4) dx = ww - 4 - dw;
    if (dx < 4) dx = 4;
    char dname[16]; snprintf(dname, sizeof dname, "drop%d", tab);   /* one window per page: its own size */
    const struct nk_rect r = nk_rect(dx, 30, dw, dh);

    if (nk_begin(ctx, dname, r, NK_WINDOW_BORDER)) {
        nk_window_set_bounds(ctx, dname, r);
        if (row < 0 && kb_moved) { nk_window_set_scroll(ctx, 0, 0); kb_moved = false; }

        /* the rows: [label][<][value][>]; the selected row is highlighted */
        const int n = nrows(tab);
        const float rh = tab == T_CONTROLS ? 20 : 26;
        for (int i = 0; i < n; i++) {
            char label[64], value[96];
            row_text(tab, i, label, sizeof label, value, sizeof value);
            const bool en = row_enabled(tab, i), val = has_value(tab, i);
            nk_layout_row_template_begin(ctx, rh);
            nk_layout_row_template_push_static(ctx, tab == T_CONTROLS ? 120 : tab == T_FILE ? 0 : 120);
            if (val) nk_layout_row_template_push_static(ctx, 24);
            nk_layout_row_template_push_dynamic(ctx);
            if (val) nk_layout_row_template_push_static(ctx, 24);
            nk_layout_row_template_end(ctx);
            if (!en) nk_widget_disable_begin(ctx);
            if (i == row && kb_moved) {                  /* scroll the selected row into view */
                const struct nk_rect b = nk_widget_bounds(ctx), c = nk_window_get_content_region(ctx);
                nk_uint sx = 0, sy = 0; nk_window_get_scroll(ctx, &sx, &sy);
                if (b.y < c.y) { float ny = (float)sy - (c.y - b.y) - 4; nk_window_set_scroll(ctx, sx, (nk_uint)(ny < 0 ? 0 : ny)); }
                else if (b.y + b.h > c.y + c.h) nk_window_set_scroll(ctx, sx, (nk_uint)(sy + (b.y + b.h - (c.y + c.h)) + 4));
                kb_moved = false;
            }
            if (nk_select_label(ctx, label, NK_TEXT_LEFT, i == row)) row = i;
            if (val && nk_button_symbol(ctx, NK_SYMBOL_TRIANGLE_LEFT)) { row = i; row_change(tab, i, -1); }
            if (tab == T_AUDIO) {                        /* the volume is a slider too */
                int v = g_cfg_volume;
                if (nk_slider_int(ctx, 0, &v, 100, 1) && v != g_cfg_volume) rr_host_set_volume(v);
            } else if (nk_button_label(ctx, value[0] ? value : label)) { row = i; row_change(tab, i, 0); }
            if (val && nk_button_symbol(ctx, NK_SYMBOL_TRIANGLE_RIGHT)) { row = i; row_change(tab, i, +1); }
            if (!en) nk_widget_disable_end(ctx);
        }

        /* notes under the rows */
        nk_layout_row_dynamic(ctx, 18, 1);
        if (tab == T_DISPLAY) {
            int rw, rh2, ow, oh; rr_host_render_size(&rw, &rh2); SDL_GL_GetDrawableSize(uwin, &ow, &oh);
            nk_spacing(ctx, 1);
            labelf(NK_TEXT_LEFT, "Rendering %d x %d  ->  window %d x %d", rw, rh2, ow, oh);
            labelf(NK_TEXT_LEFT, "Resolution = render size; the window keeps its size.");
            labelf(NK_TEXT_LEFT, g_cfg_wide ? "Widescreen: more track at the sides." : "");
        } else if (tab == T_AUDIO) {
            nk_layout_row_dynamic(ctx, 24, 4);
            if (nk_button_label(ctx, "Mute")) rr_host_set_volume(0);
            if (nk_button_label(ctx, "25%"))  rr_host_set_volume(25);
            if (nk_button_label(ctx, "50%"))  rr_host_set_volume(50);
            if (nk_button_label(ctx, "100%")) rr_host_set_volume(100);
        } else if (tab == T_CONTROLS) {
            labelf(NK_TEXT_LEFT, "Enter, then press the new key (Esc cancels)");
        } else if (tab == T_RECORD) {
            labelf(NK_TEXT_LEFT, "Records the cabinet inputs per frame to recordings/;");
            labelf(NK_TEXT_LEFT, "replay with rr --replay FILE. F9 toggles without the menu.");
        }

    }
    nk_end(ctx);

    nk_sdl_render(NK_ANTI_ALIASING_ON);        /* scales window units to the drawable itself */
}
