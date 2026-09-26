/*
 * eng_ui.c -- see eng_ui.h. The panel is Rave Racer's (raverace/src/rr_ui.c), made game-independent: the pages are
 * tables of callbacks, and the File / Display / Audio pages every game shares live here.
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
#include "../third_party/nuklear.h"
#include "../third_party/nuklear_sdl_gl2.h"

#include "eng_ui.h"
#include "eng_display.h"

#define MAXPAGES 8
static struct nk_context *ctx;
static SDL_Window *uwin;
static bool open_, quit_req, restart_req;
static float ui_scale = 1.0f;                 /* drawable pixels per window unit (HiDPI) */
static const eng_ui_page *pages[MAXPAGES];
static int npages;
static int tab, row;                          /* row -1 = the title strip */
static bool kb_moved;                         /* keep the selected row in view after a key */
static void (*cap_cb)(SDL_Scancode, void *);
static void *cap_u;

static int cyc(int v, int d, int n) { return ((v + d) % n + n) % n; }

/* ---- File ------------------------------------------------------------------ */
static int file_n(void) { return 3; }
static bool file_val(int r) { (void)r; return false; }
static void file_text(int r, char *l, size_t ln, char *v, size_t vn)
{
    (void)vn; *v = 0;
    snprintf(l, ln, "%s", r == 0 ? "Resume" : r == 1 ? "Restart" : "Exit");
}
static void file_change(int r, int dir)
{
    if (dir != 0) return;
    if (r == 0) open_ = false;
    else if (r == 1) { restart_req = true; quit_req = true; }     /* power-cycle: the host re-launches the program */
    else quit_req = true;
}
static const eng_ui_page page_file = { "File", 220, 0, 0, file_n, file_val, NULL, file_text, file_change, NULL };

/* ---- Display: the choices of engine/eng_display.h ------------------------------ */
enum { D_WIDE, D_MODE, D_SIZE, D_RES, D_ASPECT, D_SCALING, D_N };
static int disp_n(void) { return D_N; }
static bool disp_enabled(int r)
{
    if (r == D_SIZE) return g_eng_disp.winmode == 0;
    if (r == D_ASPECT) return !g_eng_disp.wide;
    return true;
}
static void disp_text(int r, char *l, size_t ln, char *v, size_t vn)
{
    *v = 0;
    switch (r) {
    case D_WIDE:    snprintf(l, ln, "Widescreen"); snprintf(v, vn, "%s", g_eng_disp.wide ? "ON (fill the window)" : "OFF (4:3)"); break;
    case D_MODE:    snprintf(l, ln, "Window mode"); snprintf(v, vn, "%s", eng_disp_winmode_name(g_eng_disp.winmode)); break;
    case D_SIZE:    snprintf(l, ln, "Window size");
                    snprintf(v, vn, "%dx  (%d x %d)", g_eng_disp.scale, eng_disp_win_w(g_eng_disp.scale), eng_disp_win_h(g_eng_disp.scale)); break;
    case D_RES:     snprintf(l, ln, "Resolution"); eng_disp_res_label(g_eng_disp.res_w, g_eng_disp.res_h, v, vn); break;
    case D_ASPECT:  snprintf(l, ln, "Aspect ratio"); snprintf(v, vn, "%s", eng_disp_aspect_name(g_eng_disp.aspect)); break;
    case D_SCALING: snprintf(l, ln, "Scaling"); snprintf(v, vn, "%s", eng_disp_scaling_name(g_eng_disp.scaling)); break;
    }
}
static void disp_change(int r, int dir)
{
    if (!disp_enabled(r)) return;
    const int d = dir ? dir : 1;
    switch (r) {
    case D_WIDE:    eng_disp_set_wide(!g_eng_disp.wide); break;
    case D_MODE:    eng_disp_set_winmode(cyc(g_eng_disp.winmode, d, 3)); break;
    case D_SIZE:    eng_disp_set_scale(cyc(g_eng_disp.scale - 1, d, 4) + 1); break;
    case D_RES: {
        int i = 0, n = eng_disp_res_count(), w, h;
        for (int k = 0; k < n; k++) { eng_disp_res_get(k, &w, &h); if (w == g_eng_disp.res_w && h == g_eng_disp.res_h) i = k; }
        eng_disp_res_get(cyc(i, d, n), &w, &h);
        eng_disp_set_res(w, h);
        break; }
    case D_ASPECT:  eng_disp_set_aspect(cyc(g_eng_disp.aspect, d, 4)); break;
    case D_SCALING: eng_disp_set_scaling(cyc(g_eng_disp.scaling, d, 3)); break;
    }
}
static void labelf(nk_flags align, const char *fmt, ...)
{
    char b[160];
    va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    nk_label(ctx, b, align);
}
static void disp_notes(void (*line)(const char *fmt, ...))
{
    (void)line;
    int rw, rh, ow, oh;
    eng_disp_render_size(&rw, &rh);
    SDL_GL_GetDrawableSize(uwin, &ow, &oh);
    nk_spacing(ctx, 1);
    labelf(NK_TEXT_LEFT, "Rendering %d x %d  ->  window %d x %d", rw, rh, ow, oh);
    labelf(NK_TEXT_LEFT, "Resolution = render size; the window keeps its size.");
    labelf(NK_TEXT_LEFT, g_eng_disp.wide ? "Widescreen: more of the scene at the sides." : " ");
    /* the resolutions as a scrollable list, as in Prop Cycle's Display menu (a combo would be a popup inside a popup) */
    nk_layout_row_dynamic(ctx, 18, 1);
    nk_label(ctx, "Resolution (render size, scaled to the window)", NK_TEXT_LEFT);
    nk_layout_row_dynamic(ctx, 132, 1);
    if (nk_group_begin(ctx, "resolutions", NK_WINDOW_BORDER)) {
        nk_layout_row_dynamic(ctx, 18, 1);
        for (int i = 0; i < eng_disp_res_count(); i++) {
            int w, h; char b[64];
            eng_disp_res_get(i, &w, &h);
            eng_disp_res_label(w, h, b, sizeof b);
            const bool on = w == g_eng_disp.res_w && h == g_eng_disp.res_h;
            if (nk_option_label(ctx, b, on) && !on) eng_disp_set_res(w, h);
        }
        nk_group_end(ctx);
    }
}
static const eng_ui_page page_display = { "Display", 440, 120, 26, disp_n, NULL, disp_enabled, disp_text, disp_change, disp_notes };

/* ---- Audio ------------------------------------------------------------------- */
static int aud_n(void) { return 1; }
static void aud_text(int r, char *l, size_t ln, char *v, size_t vn)
{
    (void)r; snprintf(l, ln, "Volume"); snprintf(v, vn, "%d%%", g_eng_disp.volume);
}
static void aud_change(int r, int dir)
{
    (void)r;
    int v = g_eng_disp.volume + (dir ? dir * 5 : 10);
    if (v > 100) v = dir ? 100 : 0;                          /* Enter wraps 100 -> mute */
    if (v < 0) v = 0;
    eng_disp_set_volume(v);
}
static void aud_notes(void (*line)(const char *fmt, ...))
{
    (void)line;
    nk_layout_row_dynamic(ctx, 24, 4);
    if (nk_button_label(ctx, "Mute")) eng_disp_set_volume(0);
    if (nk_button_label(ctx, "25%"))  eng_disp_set_volume(25);
    if (nk_button_label(ctx, "50%"))  eng_disp_set_volume(50);
    if (nk_button_label(ctx, "100%")) eng_disp_set_volume(100);
}
static const eng_ui_page page_audio = { "Audio", 300, 120, 26, aud_n, NULL, NULL, aud_text, aud_change, aud_notes };

/* ---- the panel ------------------------------------------------------------------- */
static int nrows(int t) { return pages[t]->nrows(); }
static bool has_value(int t, int r) { return pages[t]->has_value ? pages[t]->has_value(r) : true; }
static bool row_enabled(int t, int r) { return pages[t]->enabled ? pages[t]->enabled(r) : true; }
static bool is_audio(int t) { return pages[t] == &page_audio; }

void eng_ui_add_page(const eng_ui_page *p) { if (npages < MAXPAGES) pages[npages++] = p; }

bool eng_ui_init(SDL_Window *win, const char *title)
{
    (void)title;
    uwin = win;
    ctx = nk_sdl_init(win);
    if (!ctx) return false;
    /* HiDPI: draw at the drawable's density, lay out in window units (the backend's mouse coordinates are window units) */
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
    /* the three every game shares come first; the game's pages are added after */
    if (npages == 0) { pages[npages++] = &page_file; pages[npages++] = &page_display; pages[npages++] = &page_audio; }
    else {                                     /* pages were added before init: put the standard ones in front */
        const eng_ui_page *g[MAXPAGES]; int n = npages;
        memcpy(g, pages, sizeof g);
        npages = 0;
        pages[npages++] = &page_file; pages[npages++] = &page_display; pages[npages++] = &page_audio;
        for (int i = 0; i < n && npages < MAXPAGES; i++) pages[npages++] = g[i];
    }
    tab = 1; row = 0;
    if (getenv("ENG_UI_OPEN")) {               /* tests: open on a page at startup, "<page>[:<row>]" */
        int p = 1, r = 0; sscanf(getenv("ENG_UI_OPEN"), "%d:%d", &p, &r);
        eng_ui_goto(p, r); open_ = true;
    }
    return true;
}
void eng_ui_shutdown(void) { if (ctx) nk_sdl_shutdown(); ctx = NULL; }
bool eng_ui_is_open(void) { return open_; }
bool eng_ui_quit_requested(void) { return quit_req; }
bool eng_ui_restart_requested(void) { return restart_req; }
void eng_ui_set_open(bool on) { open_ = on; cap_cb = NULL; if (on && row >= nrows(tab)) row = 0; }
void eng_ui_input_begin(void) { if (ctx) nk_input_begin(ctx); }
void eng_ui_input_end(void)   { if (ctx) nk_input_end(ctx); }
void eng_ui_capture_key(void (*cb)(SDL_Scancode, void *), void *u) { cap_cb = cb; cap_u = u; }
bool eng_ui_capturing(void) { return cap_cb != NULL; }
void eng_ui_goto(int p, int r) { if (p >= 0 && p < npages) { tab = p; row = r; } }

/* one navigation step from the keyboard, a pad or a hat */
enum { K_UP, K_DOWN, K_LEFT, K_RIGHT, K_OK, K_BACK, K_TABPREV, K_TABNEXT };
static void nav(int k)
{
    const int n = nrows(tab);
    switch (k) {
    case K_UP:    row = row <= -1 ? n - 1 : row - 1; break;
    case K_DOWN:  row = row >= n - 1 ? -1 : row + 1; break;
    case K_LEFT:  if (row < 0) tab = cyc(tab, -1, npages); else if (has_value(tab, row)) pages[tab]->change(row, -1); break;
    case K_RIGHT: if (row < 0) tab = cyc(tab, +1, npages); else if (has_value(tab, row)) pages[tab]->change(row, +1); break;
    case K_OK:    if (row < 0) row = 0; else if (row_enabled(tab, row)) pages[tab]->change(row, 0); break;
    case K_BACK:  open_ = false; break;
    case K_TABPREV: tab = cyc(tab, -1, npages); row = 0; break;
    case K_TABNEXT: tab = cyc(tab, +1, npages); row = 0; break;
    }
    if (row >= nrows(tab)) row = nrows(tab) - 1;
    kb_moved = true;
}
void eng_ui_nav(char c)
{
    static const char keys[] = "udlrobpn";
    static const int code[] = { K_UP, K_DOWN, K_LEFT, K_RIGHT, K_OK, K_BACK, K_TABPREV, K_TABNEXT };
    const char *p = strchr(keys, c);
    if (p && *p) nav(code[p - keys]);
}

bool eng_ui_event(SDL_Event *e)
{
    if (!ctx || !open_) return false;
    if (cap_cb) {                                        /* the next key is the new binding */
        if (e->type == SDL_KEYDOWN && !e->key.repeat) {
            void (*cb)(SDL_Scancode, void *) = cap_cb; cap_cb = NULL;
            cb(e->key.keysym.scancode == SDL_SCANCODE_ESCAPE ? SDL_SCANCODE_UNKNOWN : e->key.keysym.scancode, cap_u);
        } else if (e->type == SDL_CONTROLLERBUTTONDOWN && e->cbutton.button == SDL_CONTROLLER_BUTTON_B) {
            void (*cb)(SDL_Scancode, void *) = cap_cb; cap_cb = NULL;
            cb(SDL_SCANCODE_UNKNOWN, cap_u);
        }
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
    if (k >= 0) { nav(k); return true; }
    if (e->type == SDL_KEYDOWN || e->type == SDL_KEYUP) return true;   /* the game does not see keys while open */
    nk_sdl_handle_event(e);                              /* mouse */
    return true;
}

/* ---- drawing ---------------------------------------------------------------- */
static void note_line(const char *fmt, ...)      /* what a page's notes() calls: one line under the rows */
{
    char b[160];
    va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    nk_label(ctx, b, NK_TEXT_LEFT);
}

void eng_ui_draw(bool *quit)
{
    if (quit_req && quit) *quit = true;
    if (!ctx || !open_) return;
    int ww, wh;
    SDL_GetWindowSize(uwin, &ww, &wh);
    /* THE MENU BAR across the top of the window. The chosen page drops down under its title; on the keyboard the bar is the
     * row above the first row of the dropdown. */
    float title_w[MAXPAGES], title_x[MAXPAGES], x = 4;
    for (int t = 0; t < npages; t++) {
        title_w[t] = 30.0f + 9.0f * (float)strlen(pages[t]->name);
        if (title_w[t] < 50) title_w[t] = 50;
        title_x[t] = x; x += title_w[t] + 4;
    }
    if (nk_begin(ctx, "menubar", nk_rect(0, 0, (float)ww, 28), NK_WINDOW_NO_SCROLLBAR)) {
        nk_menubar_begin(ctx);
        nk_layout_row_begin(ctx, NK_STATIC, 20, npages + 1);
        for (int t = 0; t < npages; t++) {
            nk_layout_row_push(ctx, title_w[t]);
            if (nk_select_label(ctx, pages[t]->name, NK_TEXT_CENTERED, t == tab) && t != tab) { tab = t; row = 0; }
        }
        nk_layout_row_push(ctx, (float)ww - x - 8 > 60 ? (float)ww - x - 8 : 60);
        nk_label(ctx, row < 0 ? "<- Left/Right ->   Down: open   Esc: close" : "Arrows, Enter, Esc", NK_TEXT_RIGHT);
        nk_menubar_end(ctx);
    }
    nk_end(ctx);

    /* the dropdown: sized to its page, under its title, inside the window */
    const eng_ui_page *pg = pages[tab];
    const int n0 = nrows(tab);
    const float rh = pg->row_h > 0 ? pg->row_h : 26;
    const float lw = pg->label_w > 0 ? pg->label_w : 120;
    float dh = 48 + n0 * (rh + 4) + (pg == &page_display ? 88 + 160 : pg->notes ? 44 : 0);
    if (dh > wh - 34) dh = (float)wh - 34;
    float dw = (pg->drop_w > 0 ? pg->drop_w : 340) < ww - 8 ? (pg->drop_w > 0 ? pg->drop_w : 340) : (float)ww - 8;
    float dx = title_x[tab];
    if (dx + dw > ww - 4) dx = ww - 4 - dw;
    if (dx < 4) dx = 4;
    char dname[16]; snprintf(dname, sizeof dname, "drop%d", tab);   /* one window per page: its own size */
    const struct nk_rect r = nk_rect(dx, 30, dw, dh);

    if (nk_begin(ctx, dname, r, NK_WINDOW_BORDER)) {
        nk_window_set_bounds(ctx, dname, r);
        if (row < 0 && kb_moved) { nk_window_set_scroll(ctx, 0, 0); kb_moved = false; }

        /* the rows: [label][<][value][>]; the selected row is highlighted */
        for (int i = 0; i < n0; i++) {
            char label[64], value[96];
            pg->text(i, label, sizeof label, value, sizeof value);
            const bool en = row_enabled(tab, i), val = has_value(tab, i);
            nk_layout_row_template_begin(ctx, rh);
            nk_layout_row_template_push_static(ctx, pg == &page_file ? 0 : lw);
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
            if (val && nk_button_symbol(ctx, NK_SYMBOL_TRIANGLE_LEFT)) { row = i; pg->change(i, -1); }
            if (is_audio(tab)) {                         /* the volume is a slider too */
                int v = g_eng_disp.volume;
                if (nk_slider_int(ctx, 0, &v, 100, 1) && v != g_eng_disp.volume) eng_disp_set_volume(v);
            } else if (nk_button_label(ctx, value[0] ? value : label)) { row = i; pg->change(i, 0); }
            if (val && nk_button_symbol(ctx, NK_SYMBOL_TRIANGLE_RIGHT)) { row = i; pg->change(i, +1); }
            if (!en) nk_widget_disable_end(ctx);
        }
        if (pg->notes) { nk_layout_row_dynamic(ctx, 18, 1); pg->notes(note_line); }
    }
    nk_end(ctx);

    nk_sdl_render(NK_ANTI_ALIASING_ON);        /* scales window units to the drawable itself */
}
