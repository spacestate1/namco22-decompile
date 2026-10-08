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
#include "eng_vsync.h"
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
#include "rr_game.h"
#include "rr_input.h"
#include "rr_hw.h"
#include "rr_sound.h"
#include "rr_net.h"

static struct nk_context *ctx;
static SDL_Window *uwin;
static bool open_, quit_req;
static int rebinding = -1;             /* the action waiting for a key, or -1 */
static int editing = -1;               /* the Online page's text row being edited (O_SERVER/O_NAME), or -1 */
static char edit_buf[128];             /* its text while editing */
#define E_ROOMNAME 101                /* the name box of the New room dialog */
#define E_CHAT 100                    /* the chat line being typed (the Online window's Say box, or the quick chat on T) */
static bool qchat;                     /* the in-game quick chat box is open (T while connected) */
static bool chat_skip_t;               /* the T that opened it also arrives as text input: swallow it */
static uint32_t chat_seen, chat_show_until;
static bool newroom_open;               /* the New room dialog (name the room, Create) */
static bool dlg_open;                  /* the floating Online play window */
static char dlg_msg[96];               /* a one-line complaint shown in the Online window (cleared when it is dealt with) */
static int dlg_mode = 1;               /* 0 = Local LAN, 1 = Internet game (the default: zonesync.net) */
static bool was_session;               /* GO edge detector: close the menu when a race is armed */
static float ui_scale = 1.0f;          /* drawable pixels per window unit (HiDPI) */

enum { T_FILE, T_DISPLAY, T_AUDIO, T_CONTROLS, T_RECORD, T_ONLINE, T_DEBUG, T_N };
static const char *tab_name[T_N] = { "File", "Display", "Audio", "Controls", "Record", "Online", "Debug" };
/* the Debug page only for a game whose table lists developer screens (rr_game.h rr_debug_t) */
static bool tab_shown(int t) { return t != T_DEBUG || (g_rr_game->debug && g_rr_game->ndebug > 0); }
static int tab = T_DISPLAY;
static int row = 0;                    /* -1 = the tab strip */
static bool kb_moved;                  /* keep the selected row in view after a key */

/* ---- the rows ------------------------------------------------------------- */
enum { D_WIDE, D_DRAW, D_MODE, D_SIZE, D_RES, D_ASPECT, D_SCALING, D_HUD, D_FPS, D_N };
enum { C_FREEPLAY, C_FFB, C_FFB_DIR, C_N };      /* the Controls page's rows before the bindings */
enum { O_SERVER, O_NAME, O_HOST, O_FIND, O_CONNECT, O_STATUS, O_LOBBY0 };   /* then, not connected: one row per LAN game found; connected: the players, Ready, Start */
static int online_rows(void) { return O_LOBBY0 + (rr_net_connected() ? rr_net_roster_count() + 2 : rr_net_found_count()); }
static int online_self_ready(void)
{
    char nm[32]; int rdy, self;
    for (int i = 0; i < rr_net_roster_count(); i++)
        if (rr_net_roster(i, nm, sizeof nm, &rdy, &self) && self) return rdy;
    return 0;
}
static int nrows(int t)
{
    switch (t) {
    case T_FILE: return 4;
    case T_DISPLAY: return D_N;
    case T_AUDIO: return 1;
    case T_CONTROLS: return C_N + RR_ACT_N;
    case T_RECORD: return 1;
    case T_ONLINE: return online_rows();
    case T_DEBUG: return g_rr_game->ndebug;
    }
    return 0;
}
/* does the row take Left/Right (a value), or is it an action? */
static bool has_value(int t, int r)
{
    if (t == T_DISPLAY || t == T_AUDIO) return true;
    if (t == T_CONTROLS) return r < C_N;
    if (t == T_RECORD) return true;
    if (t == T_ONLINE) return r == O_SERVER || r == O_NAME || (rr_net_connected() && r == O_LOBBY0 + rr_net_roster_count());   /* server / name / ready */
    return false;
}
static bool row_enabled(int t, int r)
{
    if (t == T_DISPLAY && r == D_SIZE) return g_cfg_winmode == 0;
    if (t == T_DISPLAY && r == D_ASPECT) return !g_cfg_wide;
    if (t == T_DISPLAY && r == D_HUD) return g_cfg_wide;
    return true;
}
static void row_text(int t, int r, char *label, size_t ln, char *value, size_t vn)
{
    static const char *wm[3] = { "Windowed", "Fullscreen (desktop)", "Fullscreen (exclusive)" };
    static const char *asp[4] = { "Stretch to window", "4:3", "8:7", "16:9" };
    static const char *sc[3] = { "Smooth", "Sharp", "Integer" };
    *value = 0;
    switch (t) {
    case T_FILE:
        if (r == 1) snprintf(label, ln, "Test mode: %s", rr_host_test_on() ? "ON" : "OFF");
        else if (r == 2) snprintf(label, ln, "Service button (press)");
        else snprintf(label, ln, "%s", r == 0 ? "Resume" : "Exit");
        break;
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
        case D_FPS:     snprintf(label, ln, "Frame rate"); snprintf(value, vn, "%s", eng_vsync_rate_name(g_cfg_fps)); break;
        case D_HUD:     { extern int g_eng_hud_edges_on; snprintf(label, ln, "Widescreen HUD"); snprintf(value, vn, "%s", g_eng_hud_edges_on ? "at the screen edges" : "original (4:3 centre)"); break; }
        }
        break;
    case T_AUDIO: snprintf(label, ln, "Volume"); snprintf(value, vn, "%d%%", g_cfg_volume); break;
    case T_CONTROLS:
        if (r == C_FREEPLAY) { snprintf(label, ln, "Free play"); snprintf(value, vn, "%s", rr_hw_freeplay() ? "ON" : "OFF (coins)"); }
        else if (r == C_FFB) {
            snprintf(label, ln, "Force feedback");
            if (g_cfg_ffb_strength) snprintf(value, vn, "%d%%%s", g_cfg_ffb_strength, rr_host_ffb_wheel() ? "" : " (no FFB wheel bound)");
            else snprintf(value, vn, "OFF");
        }
        else if (r == C_FFB_DIR) { snprintf(label, ln, "FFB direction"); snprintf(value, vn, "%s", g_cfg_ffb_invert ? "reversed" : "normal"); }
        else {
            const int a = r - C_N;
            if (g_rr_game->act_label && g_rr_game->act_label[a]) snprintf(label, ln, "%s", g_rr_game->act_label[a]);   /* the game's own names */
            else {
            snprintf(label, ln, "%s", rr_input_action_name(a));
            for (char *c = label; *c; c++) if (*c == '_') *c = ' ';
            if (label[0] >= 'a' && label[0] <= 'z') label[0] = (char)(label[0] - 'a' + 'A');
            }
            if (rebinding == a) snprintf(value, vn, "key, button or move control...");
            else rr_input_binding_label(a, value, vn);
        }
        break;
    case T_RECORD: snprintf(label, ln, "Record input"); snprintf(value, vn, "%s", rr_input_recording() ? "ON  (recording...)" : "OFF"); break;
    case T_DEBUG: snprintf(label, ln, "%s", g_rr_game->debug[r].name); snprintf(value, vn, "Launch"); break;
    case T_ONLINE: {
        const int rc = rr_net_roster_count();
        switch (r) {
        case O_SERVER: snprintf(label, ln, "Server");
            if (editing == O_SERVER) snprintf(value, vn, "%.90s_", edit_buf);     /* the cursor */
            else snprintf(value, vn, "%.90s", g_cfg_net_server[0] ? g_cfg_net_server : "(not set)");
            break;
        case O_NAME: snprintf(label, ln, "Name");
            if (editing == O_NAME) snprintf(value, vn, "%.16s_", edit_buf);       /* names are 16 bytes on the wire */
            else snprintf(value, vn, "%s", g_cfg_net_name[0] ? g_cfg_net_name : "PLAYER");
            break;
        case O_HOST: snprintf(label, ln, "%s", rr_net_hosting() ? "Stop hosting" : "Host / join a game..."); break;
        case O_FIND: snprintf(label, ln, "Find LAN games"); snprintf(value, vn, "%s", rr_net_discovering() ? "searching..." : ""); break;
        case O_CONNECT: snprintf(label, ln, "%s", rr_net_connected() ? "Disconnect" : "Connect"); break;
        case O_STATUS: snprintf(label, ln, "Status"); rr_net_status(value, vn); break;
        default:
            if (!rr_net_connected()) {
                char lb[96];
                snprintf(label, ln, "Join");
                if (rr_net_found(r - O_LOBBY0, lb, sizeof lb, NULL, 0)) snprintf(value, vn, "%s", lb);
            } else if (r >= O_LOBBY0 && r < O_LOBBY0 + rc) {
                char nm[32]; int rdy, self;
                snprintf(label, ln, "Player");
                if (rr_net_roster(r - O_LOBBY0, nm, sizeof nm, &rdy, &self))
                    snprintf(value, vn, "%d: %s%s%s", r - O_LOBBY0, nm, self ? " (you)" : "", rdy ? " [ready]" : "");
            } else if (r == O_LOBBY0 + rc) { snprintf(label, ln, "Ready"); snprintf(value, vn, "%s", online_self_ready() ? "ON" : "OFF"); }
            else snprintf(label, ln, "Start race");
        }
        break; }
    }
}
static void begin_edit(int r)
{
    editing = r;
    if (r == E_CHAT) edit_buf[0] = 0;
    else if (r == E_ROOMNAME) snprintf(edit_buf, sizeof edit_buf, "%.16s's room", rr_net_name());
    else snprintf(edit_buf, sizeof edit_buf, "%s", r == O_SERVER ? g_cfg_net_server : (g_cfg_net_name[0] ? g_cfg_net_name : rr_net_name()));
    SDL_StartTextInput();
}
static void stop_edit(void);
/* click on a text box: it takes the keyboard, and any other box that had it lets go */
static void focus_edit(int r) { if (editing != r) { stop_edit(); begin_edit(r); } }
/* a text box: dark field, light border (blue while it has the keyboard), left-aligned text -- not a push button */
static bool input_box(const char *text, bool active)
{
    struct nk_style_button b = ctx->style.button;
    b.normal = nk_style_item_color(nk_rgb(22, 22, 26)); b.hover = nk_style_item_color(nk_rgb(30, 30, 36)); b.active = b.hover;
    b.border_color = active ? nk_rgb(110, 165, 255) : nk_rgb(105, 105, 115); b.border = active ? 2.0f : 1.0f; b.rounding = 2.0f;
    b.text_alignment = NK_TEXT_LEFT; b.text_normal = nk_rgb(235, 235, 235); b.text_hover = nk_rgb(255, 255, 255); b.text_active = nk_rgb(255, 255, 255);
    return nk_button_label_styled(ctx, &b, text);
}
/* ONE way out of every text-entry state, so none can be left half-open (text input on, keys swallowed, nothing visible) */
static void stop_edit(void)
{
    if (editing >= 0 || qchat) SDL_StopTextInput();
    editing = -1; qchat = false; chat_skip_t = false;
}
/* ONE way to close the whole menu: dialogs, typing, key-rebinding all reset, so it opens clean the next time */
static void close_menu(void)
{
    open_ = false; dlg_open = false; newroom_open = false; rebinding = -1;
    stop_edit();
}
static int cyc(int v, int d, int n) { return ((v + d) % n + n) % n; }
static int cyc_tab(int v, int d) { do v = cyc(v, d, T_N); while (!tab_shown(v)); return v; }
/* dir: 0 = Enter / click, -1 / +1 = Left / Right */
static void row_change(int t, int r, int dir)
{
    if (!row_enabled(t, r)) return;
    const int d = dir ? dir : 1;
    switch (t) {
    case T_FILE:
        if (r == 1) { rr_host_set_test(!rr_host_test_on()); close_menu(); }        /* on to see the test menu; off to leave it */
        else if (dir == 0) {
            if (r == 0) close_menu();
            else if (r == 2) { rr_host_service_pulse(); close_menu(); }
            else quit_req = true;
        }
        break;
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
        case D_HUD:     { extern int g_eng_hud_edges_on; rr_host_set_hud_edges(!g_eng_hud_edges_on); break; }
        case D_FPS:     { extern void rr_host_set_fps(int); rr_host_set_fps(eng_vsync_rate_step(g_cfg_fps, d)); break; }
        }
        break;
    case T_AUDIO: {
        int v = g_cfg_volume + (dir ? dir * 5 : 10);
        if (v > 100) v = dir ? 100 : 0;                     /* Enter wraps 100 -> mute */
        if (v < 0) v = 0;
        rr_host_set_volume(v);
        break; }
    case T_CONTROLS:
        if (r == C_FREEPLAY) rr_host_set_freeplay(!rr_hw_freeplay());
        else if (r == C_FFB) rr_host_set_ffb_strength(dir ? g_cfg_ffb_strength + 10 * dir : (g_cfg_ffb_strength + 10) % 110);   /* Enter cycles */
        else if (r == C_FFB_DIR) rr_host_set_ffb_invert(!g_cfg_ffb_invert);
        else if (dir == 0) { rebinding = r - C_N; rr_input_capture_begin(rebinding); }
        break;
    case T_RECORD: rr_host_toggle_record(); break;
    case T_DEBUG: if (dir == 0) { rr_debug_launch(r); close_menu(); } break;
    case T_ONLINE: {
        const int rc = rr_net_roster_count();
        if (r == O_SERVER || r == O_NAME) {
            if (dir == 0) {                          /* Enter: modal text entry (SDL_TEXTINPUT in rr_ui_event) */
                editing = r;
                snprintf(edit_buf, sizeof edit_buf, "%s", r == O_SERVER ? g_cfg_net_server : g_cfg_net_name);
                SDL_StartTextInput();
            }
        } else if (r == O_HOST && dir == 0) {
            if (rr_net_hosting()) rr_net_host_stop(); else dlg_open = true;     /* the window picks LAN or Internet */
        } else if (r == O_FIND && dir == 0) {
            rr_net_discover();
        } else if (r == O_CONNECT && dir == 0) {
            if (rr_net_hosting()) rr_net_host_stop();            /* leaving a game you host closes it */
            else if (rr_net_connected()) rr_net_disconnect(); else rr_net_connect();
        } else if (!rr_net_connected()) {                        /* a found game: Enter joins it */
            char ad[64];
            if (dir == 0 && r >= O_LOBBY0 && rr_net_found(r - O_LOBBY0, NULL, 0, ad, sizeof ad)) { rr_host_set_net_server(ad); rr_net_connect(); }
        } else if (r == O_LOBBY0 + rc) rr_net_set_ready(!online_self_ready());
        else if (r == O_LOBBY0 + rc + 1 && dir == 0) rr_net_request_start();
        break; }
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
static void ui_debug_poll(void)                          /* RR_UI_DEBUG=1: print the menu state whenever it changes (the stress tests read it) */
{
    static int dbg = -1, last = -1;
    if (dbg < 0) { const char *e = getenv("RR_UI_DEBUG"); dbg = e && *e == '1'; }
    if (!dbg) return;
    const int st = (open_ ? 1 : 0) | (dlg_open ? 2 : 0) | (editing >= 0 ? 4 : 0) | (qchat ? 8 : 0) | (rebinding >= 0 ? 16 : 0);
    if (st != last) { last = st; fprintf(stderr, "[UI] open=%d dialog=%d editing=%d quickchat=%d rebinding=%d\n", st & 1, !!(st & 2), !!(st & 4), !!(st & 8), !!(st & 16)); }
}
bool rr_ui_is_open(void) { ui_debug_poll(); return open_; }
bool rr_ui_chat_typing(void) { return qchat; }
/* T while connected (menu closed) opens the quick chat box; while it is open it takes every event. True = consumed. */
bool rr_ui_chat_event(SDL_Event *e)
{
    if (!ctx || open_) return false;
    if (qchat) {
        if (e->type == SDL_KEYUP && e->key.keysym.scancode == SDL_SCANCODE_T) chat_skip_t = false;      /* the opening T is released: its text event (if any) has come and gone */
        if (e->type == SDL_KEYDOWN && !e->key.repeat && e->key.keysym.scancode == SDL_SCANCODE_T && !edit_buf[0] && !chat_skip_t) { stop_edit(); return true; }   /* T on an empty box closes it (a T in the middle of a message is a letter) */
        rr_ui_event(e); return true;
    }
    if (e->type == SDL_KEYDOWN && !e->key.repeat && e->key.keysym.scancode == SDL_SCANCODE_T && rr_net_connected()) {
        qchat = true; editing = E_CHAT; edit_buf[0] = 0; chat_skip_t = true; SDL_StartTextInput();
        return true;
    }
    return false;
}
/* true while the chat overlay has something to show: typing, or a line arrived in the last 10 s */
bool rr_ui_chat_active(void)
{
    if (!ctx || open_) return false;
    const uint32_t ser = rr_net_chat_serial();
    const bool race = rr_net_session_active();
    if (ser != chat_seen) {
        chat_seen = ser; chat_show_until = SDL_GetTicks() + 10000;
        if (race && !qchat) rr_ui_set_hint("New chat message  -  press T to read and reply", 150);    /* in a race the chat stays closed */
    }
    if (!rr_net_connected()) { if (qchat) stop_edit(); return false; }
    if (race) return qchat;                                                    /* in a race: only while the player has it open (T) */
    return qchat || (rr_net_chat_count() > 0 && (int32_t)(chat_show_until - SDL_GetTicks()) > 0);
}
static void chat_overlay(int ww, int wh)
{
    const int n = rr_net_chat_count();
    int show = qchat ? 8 : 5; if (show > n) show = n;
    const float h = 12 + show * 20 + (qchat ? 30 : 0);
    if (h < 30) return;
    const float w = ww < 520 ? (float)ww - 16 : 500.0f;
    if (nk_begin(ctx, "chat", nk_rect(8, (float)wh - h - 44, w, h), NK_WINDOW_NO_SCROLLBAR)) {
        for (int i = n - show; i < n; i++) {
            char ln[128];
            if (!rr_net_chat_line(i, ln, sizeof ln)) continue;
            nk_layout_row_dynamic(ctx, 18, 1);
            nk_label(ctx, ln, NK_TEXT_LEFT);
        }
        if (qchat) {
            char f[140]; snprintf(f, sizeof f, "> %.96s_   (Enter sends, T or Esc closes)", edit_buf);
            nk_layout_row_dynamic(ctx, 24, 1);
            nk_label(ctx, f, NK_TEXT_LEFT);
        }
    }
    nk_end(ctx);
}
bool rr_ui_quit_requested(void) { return quit_req; }
void rr_ui_set_open(bool on) { if (!on) { close_menu(); return; } open_ = true; rebinding = -1; if (row >= nrows(tab)) row = 0; }
void rr_ui_input_begin(void) { if (ctx) nk_input_begin(ctx); }
void rr_ui_input_end(void)   { if (ctx) nk_input_end(ctx); }

/* one navigation step from the keyboard, a pad or a hat */
enum { K_UP, K_DOWN, K_LEFT, K_RIGHT, K_OK, K_BACK, K_TABPREV, K_TABNEXT };
static void nav(int k)
{
    const int n = nrows(tab);
    if (dlg_open && k != K_BACK) return;             /* the Online play window is mouse/touch; Esc/B closes it */
    switch (k) {
    case K_UP:    row = row <= -1 ? n - 1 : row - 1; break;
    case K_DOWN:  row = row >= n - 1 ? -1 : row + 1; break;
    case K_LEFT:  if (row < 0) tab = cyc_tab(tab, -1); else if (has_value(tab, row)) row_change(tab, row, -1); break;
    case K_RIGHT: if (row < 0) tab = cyc_tab(tab, +1); else if (has_value(tab, row)) row_change(tab, row, +1); break;
    case K_OK:    if (row < 0) row = 0; else row_change(tab, row, 0); break;
    case K_BACK:  if (newroom_open) { newroom_open = false; stop_edit(); } else if (dlg_open) { dlg_open = false; stop_edit(); } else close_menu(); break;
    case K_TABPREV: tab = cyc_tab(tab, -1); row = 0; break;
    case K_TABNEXT: tab = cyc_tab(tab, +1); row = 0; break;
    }
    if (row >= nrows(tab)) row = nrows(tab) - 1;
}

bool rr_ui_event(SDL_Event *e)
{
    if (!ctx || (!open_ && !qchat)) return false;
    if (editing >= 0) {                            /* the Online page's modal text entry: every event is ours */
        if (e->type == SDL_TEXTINPUT) {
            if (chat_skip_t) { chat_skip_t = false; if ((e->text.text[0] == 't' || e->text.text[0] == 'T') && !e->text.text[1]) return true; }
            const size_t bl = strlen(edit_buf), tl = strlen(e->text.text);
            const size_t cap = editing == O_NAME ? 16 : editing == E_ROOMNAME ? 24 : editing == E_CHAT ? 96 : sizeof edit_buf - 2;   /* names are 16 bytes on the wire */
            if (bl + tl <= cap) memcpy(edit_buf + bl, e->text.text, tl + 1);
        } else if (e->type == SDL_KEYDOWN) {
            switch (e->key.keysym.scancode) {
            case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER:
                if (editing == E_CHAT) {                                       /* send; the Online window keeps the box open, the quick chat closes */
                    if (edit_buf[0]) rr_net_chat_send(edit_buf);
                    edit_buf[0] = 0;                                           /* the panel stays open: T on an empty box or Esc closes it */
                    break;
                }
                if (editing == E_ROOMNAME) { rr_net_new_room(edit_buf); newroom_open = false; stop_edit(); break; }   /* Enter creates the room */
                if (editing == O_SERVER) rr_host_set_net_server(edit_buf);     /* apply + save + resolve */
                else rr_host_set_net_name(edit_buf);
                stop_edit();
                break;
            case SDL_SCANCODE_ESCAPE: stop_edit(); break;
            case SDL_SCANCODE_BACKSPACE: {
                size_t l = strlen(edit_buf);
                if (l) { edit_buf[--l] = 0; while (l > 0 && (edit_buf[l - 1] & 0xC0) == 0x80) edit_buf[--l] = 0; }  /* whole UTF-8 char */
                break; }
            default: break;
            }
        } else if (e->type == SDL_MOUSEMOTION || e->type == SDL_MOUSEBUTTONDOWN || e->type == SDL_MOUSEBUTTONUP || e->type == SDL_MOUSEWHEEL) {
            nk_sdl_handle_event(e);                    /* typing never blocks the mouse: Send / Close / Ready / the X stay clickable */
        }
        return true;
    }
    if (rebinding >= 0) {
        if (rr_input_capture_event(rebinding, e)) rebinding = -1;
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
void rr_ui_test_goto(int t, int r) { tab = t; row = r; stop_edit(); }

/* ---- drawing ---------------------------------------------------------------- */
static void labelf(nk_flags align, const char *fmt, ...)
{
    char b[160];
    va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    nk_label(ctx, b, align);
}

/* a one-line hint at the bottom of the window while the menu is closed (a pad has no Esc: how to reach the menu) */
static char hint_text[96]; static int hint_left;
void rr_ui_set_hint(const char *text, int frames) { snprintf(hint_text, sizeof hint_text, "%s", text ? text : ""); hint_left = frames; }
bool rr_ui_hint_active(void) { return ctx && !open_ && hint_left > 0 && hint_text[0]; }

/* THE ONLINE PLAY WINDOW: opened by "Host / join a game...". A drop-down picks Local LAN (host one, or list the games
 * found on the network) or Internet game (type a server IP or URL). Text entry reuses the modal editor above. */
static void online_dialog(int ww, int wh)
{
    const float w = 460 < ww - 8 ? 460.0f : (float)ww - 8, h = 410 < wh - 40 ? 410.0f : (float)wh - 40;
    if (nk_begin(ctx, "Online play", nk_rect(((float)ww - w) / 2, ((float)wh - h) / 2, w, h),
                 NK_WINDOW_BORDER | NK_WINDOW_TITLE | NK_WINDOW_MOVABLE)) {
        static const char *modes[2] = { "Local LAN", "Internet game" };
        const bool conn = rr_net_connected(), hosting = rr_net_hosting();
        char st[128]; rr_net_status(st, sizeof st);
        nk_layout_row_dynamic(ctx, 26, 2);
        nk_label(ctx, "Game type", NK_TEXT_LEFT);
        if (!conn) dlg_mode = nk_combo(ctx, modes, 2, dlg_mode, 26, nk_vec2(200, 80));
        else nk_label(ctx, modes[dlg_mode], NK_TEXT_LEFT);

        /* your name (shown to the others in the lobby; changeable any time, also inside the lobby) */
        nk_layout_row_template_begin(ctx, 28);
        nk_layout_row_template_push_static(ctx, 86);
        nk_layout_row_template_push_dynamic(ctx);
        nk_layout_row_template_end(ctx);
        nk_label(ctx, "Your name:", NK_TEXT_LEFT);
        char nbx[64];
        if (editing == O_NAME) snprintf(nbx, sizeof nbx, "%.16s_   (Enter saves)", edit_buf);
        else snprintf(nbx, sizeof nbx, "%.16s     (click to change)", rr_net_name());
        if (input_box(nbx, editing == O_NAME)) focus_edit(O_NAME);

        if (!conn && dlg_mode == 0) {                       /* ---- Local LAN ---- */
            nk_layout_row_dynamic(ctx, 28, 2);
            if (nk_button_label(ctx, "Host a LAN game")) rr_net_host_start();
            if (nk_button_label(ctx, rr_net_discovering() ? "Searching..." : "Find LAN games")) rr_net_discover();
            nk_layout_row_dynamic(ctx, 20, 1);
            nk_label(ctx, "Games on this network (click one to join):", NK_TEXT_LEFT);
            const int nf = rr_net_found_count();
            if (!nf) { nk_layout_row_dynamic(ctx, 22, 1); nk_label(ctx, rr_net_discovering() ? "searching..." : "none found - press Find LAN games", NK_TEXT_LEFT); }
            for (int i = 0; i < nf; i++) {
                char lb[96], ad[64], line[176];
                if (!rr_net_found(i, lb, sizeof lb, ad, sizeof ad)) continue;
                snprintf(line, sizeof line, "%s   [%s]", lb, ad);
                nk_layout_row_dynamic(ctx, 26, 1);
                if (nk_button_label(ctx, line)) { rr_host_set_net_server(ad); rr_net_connect(); }
            }
        } else if (!conn) {                                 /* ---- Internet game ---- */
            /* 1. the public server: one click, no typing */
            nk_layout_row_dynamic(ctx, 20, 1);
            nk_label(ctx, "Public server:", NK_TEXT_LEFT);
            nk_layout_row_dynamic(ctx, 34, 1);
            if (nk_button_label(ctx, "Connect to ZoneSync  (zonesync.net)")) {
                stop_edit(); dlg_msg[0] = 0;
                if (hosting) rr_net_host_stop();
                if (rr_net_set_server("zonesync.net")) rr_net_connect();
            }
            /* 2. a custom server: its own address box and its own Connect button */
            nk_layout_row_dynamic(ctx, 8, 1); nk_spacing(ctx, 1);
            nk_layout_row_dynamic(ctx, 20, 1);
            nk_label(ctx, "Custom server  (host or host:port, default port 27750):", NK_TEXT_LEFT);
            const bool is_custom = g_cfg_net_server[0] && strcmp(g_cfg_net_server, "zonesync.net") != 0;
            char fld[130];
            if (editing == O_SERVER) snprintf(fld, sizeof fld, "%.90s_", edit_buf);
            else if (is_custom) snprintf(fld, sizeof fld, "%.90s", g_cfg_net_server);
            else snprintf(fld, sizeof fld, "(click here, type an address, press Enter)");
            nk_layout_row_dynamic(ctx, 28, 1);
            if (input_box(fld, editing == O_SERVER)) { if (editing != O_SERVER) { focus_edit(O_SERVER); if (!is_custom) edit_buf[0] = 0; } }   /* a text box; it starts empty unless a custom address is already saved */
            if (editing == O_SERVER) { nk_layout_row_dynamic(ctx, 18, 1); nk_label(ctx, "Enter saves the address, Esc cancels", NK_TEXT_LEFT); }
            nk_layout_row_dynamic(ctx, 30, 1);
            if (nk_button_label(ctx, "Connect to custom server")) {
                bool resolved = false;
                if (editing == O_SERVER && edit_buf[0]) { rr_host_set_net_server(edit_buf); resolved = true; }   /* the box is still open: use what was typed */
                stop_edit();
                if (g_cfg_net_server[0] && strcmp(g_cfg_net_server, "zonesync.net") != 0) {
                    if (hosting) rr_net_host_stop();
                    if (resolved || rr_net_set_server(g_cfg_net_server)) rr_net_connect();
                } else snprintf(dlg_msg, sizeof dlg_msg, "Type an address in the box above first.");
            }
        }
        if (dlg_msg[0] && !conn) { nk_layout_row_dynamic(ctx, 20, 1); nk_label(ctx, dlg_msg, NK_TEXT_LEFT); }
        nk_layout_row_dynamic(ctx, 20, 1);
        labelf(NK_TEXT_LEFT, "Status: %s", st);
        nk_layout_row_dynamic(ctx, 28, 1);
        if (nk_button_label(ctx, "Close")) { dlg_open = false; dlg_msg[0] = 0; stop_edit(); }
    }
    nk_end(ctx);
}

/* THE LOBBY WINDOW: opens by itself as soon as a connection is made (hosted or joined). A sidebar of the players in the room
 * (ready flags, "you" marked) beside the chat; the Say box below takes the keyboard straight away. Replaces the Online play
 * window while connected; Close hides it (the menu row "Host / join a game..." brings it back). */
static void lobby_window(int ww, int wh)
{
    const float w = ww - 16 < 1100 ? (float)ww - 16 : 1100.0f, h = wh - 16 < 820 ? (float)wh - 16 : 820.0f;     /* nearly the whole window: the chat is the main thing */
    if (nk_begin(ctx, "Lobby", nk_rect(((float)ww - w) / 2, ((float)wh - h) / 2, w, h), NK_WINDOW_BORDER | NK_WINDOW_TITLE | NK_WINDOW_MOVABLE | NK_WINDOW_CLOSABLE)) {
        const bool hosting = rr_net_hosting(), joining = rr_net_switching();
        { static int t = -1; if (t < 0) t = getenv("RR_UI_NEWROOM") != NULL; if (t == 1) { t = 2; newroom_open = true; focus_edit(E_ROOMNAME); } }   /* test hook: open the New room dialog once */
        char st[128]; rr_net_status(st, sizeof st);
        /* WHERE YOU ARE: the room you are in, in green, and the server beneath it */
        char myroom[48] = ""; int myplayers = 0;
        for (int i = 0, n = rr_net_room_count(); i < n; i++) { int mine, pl; char nm[40]; if (rr_net_room(i, NULL, NULL, &pl, &mine, nm, sizeof nm) && mine) { snprintf(myroom, sizeof myroom, "%s", nm); myplayers = pl; } }
        nk_layout_row_dynamic(ctx, 24, 1);
        if (joining) nk_label_colored(ctx, "Joining the room...", NK_TEXT_LEFT, nk_rgb(240, 200, 90));
        else if (myroom[0]) { char t[96]; snprintf(t, sizeof t, "You are in:  %s   (%d/8 players)", myroom, myplayers); nk_label_colored(ctx, t, NK_TEXT_LEFT, nk_rgb(120, 220, 130)); }
        else nk_label_colored(ctx, hosting ? "You are hosting a LAN game" : "You are in the lobby", NK_TEXT_LEFT, nk_rgb(120, 220, 130));
        nk_layout_row_dynamic(ctx, 18, 1);
        labelf(NK_TEXT_LEFT, "%s   -   %s", hosting ? "this computer (LAN)" : (rr_net_server()[0] ? rr_net_server() : "online"), st);

        /* your name: click the box, type, Enter (everyone in the room sees the change at once) */
        nk_layout_row_template_begin(ctx, 28);
        nk_layout_row_template_push_static(ctx, 86);
        nk_layout_row_template_push_dynamic(ctx);
        nk_layout_row_template_end(ctx);
        nk_label(ctx, "Your name:", NK_TEXT_LEFT);
        char nb[64];
        if (editing == O_NAME) snprintf(nb, sizeof nb, "%.16s_   (Enter saves, Esc cancels)", edit_buf);
        else snprintf(nb, sizeof nb, "%.16s     (click to change)", rr_net_name());
        if (input_box(nb, editing == O_NAME)) focus_edit(O_NAME);

        /* THE LAYOUT: a sidebar on the left (the players, then the rooms table with Join / New room) and the CHAT on the right at the
         * full height -- it is the main thing, so it gets everything the sidebar does not */
        const float gh = h - 215 > 220 ? h - 215 : 220;
        const int lines = (int)((gh - 36) / 22), rc = rr_net_roster_count(), nrooms = rr_net_room_count();
        nk_layout_row_template_begin(ctx, gh);
        nk_layout_row_template_push_static(ctx, w < 560 ? 250 : 320);
        nk_layout_row_template_push_dynamic(ctx);
        nk_layout_row_template_end(ctx);
        if (nk_group_begin(ctx, "Sidebar", NK_WINDOW_NO_SCROLLBAR)) {
            const float ph = gh * 0.30f < 84 ? 84 : gh * 0.30f, rh = gh - ph - 50;
            nk_layout_row_dynamic(ctx, ph, 1);
            if (nk_group_begin(ctx, "Players", NK_WINDOW_BORDER | NK_WINDOW_TITLE)) {
                for (int i = 0; i < rc; i++) {
                    char nm[32]; int rdy, self;
                    if (!rr_net_roster(i, nm, sizeof nm, &rdy, &self)) continue;
                    nk_layout_row_dynamic(ctx, 20, 1);
                    labelf(NK_TEXT_LEFT, "%s%s%s", nm, self ? " (you)" : "", rdy ? "  [ready]" : "");
                }
                nk_group_end(ctx);
            }
            /* THE ROOMS TABLE: every room on the server, one row each, with its own JOIN button. Your room is marked ">" and says HERE. */
            nk_layout_row_dynamic(ctx, rh, 1);
            if (nk_group_begin(ctx, "Rooms", NK_WINDOW_BORDER | NK_WINDOW_TITLE)) {
                if (!nrooms) { nk_layout_row_dynamic(ctx, 20, 1); nk_label(ctx, hosting ? "(a LAN game is one room)" : "(waiting for the list...)", NK_TEXT_LEFT); }
                for (int i = 0; i < nrooms; i++) {
                    int id, state, players, mine; char nm[40];
                    if (!rr_net_room(i, &id, &state, &players, &mine, nm, sizeof nm)) continue;
                    nk_layout_row_template_begin(ctx, 28);
                    nk_layout_row_template_push_dynamic(ctx); nk_layout_row_template_push_static(ctx, 34); nk_layout_row_template_push_static(ctx, 58); nk_layout_row_template_push_static(ctx, 28);
                    nk_layout_row_template_end(ctx);
                    char rn[48]; snprintf(rn, sizeof rn, "%s%s", mine ? "> " : "", nm);
                    if (mine) nk_label_colored(ctx, rn, NK_TEXT_LEFT, nk_rgb(120, 220, 130)); else nk_label(ctx, rn, NK_TEXT_LEFT);
                    labelf(NK_TEXT_LEFT, "%d/8", players);
                    if (mine) nk_label_colored(ctx, "HERE", NK_TEXT_CENTERED, nk_rgb(120, 220, 130));
                    else if (state) nk_label_colored(ctx, "racing", NK_TEXT_CENTERED, nk_rgb(220, 140, 120));
                    else if (players >= 8) nk_label_colored(ctx, "full", NK_TEXT_CENTERED, nk_rgb(220, 140, 120));
                    else {                                                    /* open: a green Join button on the row */
                        struct nk_style_button gb = ctx->style.button;
                        gb.normal = nk_style_item_color(nk_rgb(40, 120, 55)); gb.hover = nk_style_item_color(nk_rgb(55, 150, 70)); gb.active = gb.hover;
                        gb.text_normal = nk_rgb(255, 255, 255); gb.text_hover = nk_rgb(255, 255, 255); gb.text_active = nk_rgb(255, 255, 255);
                        if (nk_button_label_styled(ctx, &gb, "Join") && !hosting) rr_net_switch_room(id);
                    }
                    if (players == 0 && !mine && !state && nrooms > 1) {              /* an empty room can be deleted */
                        struct nk_style_button rb = ctx->style.button;
                        rb.normal = nk_style_item_color(nk_rgb(120, 45, 45)); rb.hover = nk_style_item_color(nk_rgb(160, 60, 60)); rb.active = rb.hover;
                        rb.text_normal = nk_rgb(255, 255, 255); rb.text_hover = nk_rgb(255, 255, 255); rb.text_active = nk_rgb(255, 255, 255);
                        if (nk_button_label_styled(ctx, &rb, "X") && !hosting) rr_net_delete_room(id);
                    } else nk_spacing(ctx, 1);
                }
                nk_group_end(ctx);
            }
            nk_layout_row_dynamic(ctx, 30, 1);
            if (nk_button_label(ctx, "+ New room...") && !hosting) { newroom_open = true; focus_edit(E_ROOMNAME); }
            nk_group_end(ctx);
        }
        if (nk_group_begin(ctx, "Chat", NK_WINDOW_BORDER | NK_WINDOW_TITLE)) {
            const int n = rr_net_chat_count(), show = n < lines ? n : lines;
            if (!n) { nk_layout_row_dynamic(ctx, 22, 1); nk_label(ctx, "No messages yet - say hello.", NK_TEXT_LEFT); }
            for (int i = n - show; i < n; i++) {
                char ln[128];
                if (!rr_net_chat_line(i, ln, sizeof ln)) continue;
                nk_layout_row_dynamic(ctx, 22, 1);
                nk_label(ctx, ln, NK_TEXT_LEFT);
            }
            nk_group_end(ctx);
        }

        /* the entry box and its Send button (Enter sends too) */
        char say[140];
        if (editing == E_CHAT) snprintf(say, sizeof say, "%.96s_", edit_buf);
        else snprintf(say, sizeof say, "Type a message...");
        nk_layout_row_template_begin(ctx, 28);
        nk_layout_row_template_push_dynamic(ctx);
        nk_layout_row_template_push_static(ctx, 90);
        nk_layout_row_template_end(ctx);
        if (input_box(say, editing == E_CHAT)) focus_edit(E_CHAT);
        if (nk_button_label(ctx, "Send")) {
            if (editing == E_CHAT && edit_buf[0]) { rr_net_chat_send(edit_buf); edit_buf[0] = 0; }     /* keeps the box open for the next line */
            else focus_edit(E_CHAT);
        }
        nk_layout_row_dynamic(ctx, 28, 4);
        if (nk_button_label(ctx, online_self_ready() ? "Not ready" : "Ready")) rr_net_set_ready(!online_self_ready());
        if (nk_button_label(ctx, "Start race")) rr_net_request_start();       /* refused until everyone is Ready */
        if (nk_button_label(ctx, hosting ? "Stop hosting" : "Disconnect")) { if (hosting) rr_net_host_stop(); else rr_net_disconnect(); }
        if (nk_button_label(ctx, "Close")) { dlg_open = false; stop_edit(); }
    }
    nk_end(ctx);
    if (newroom_open) {                                       /* THE NEW ROOM DIALOG: name it, Create (Enter) or Cancel (Esc) */
        const float dw = ww - 16 < 400 ? (float)ww - 16 : 400.0f, dh = 150;
        if (nk_begin(ctx, "New room", nk_rect(((float)ww - dw) / 2, ((float)wh - dh) / 2, dw, dh), NK_WINDOW_BORDER | NK_WINDOW_TITLE | NK_WINDOW_MOVABLE)) {
            nk_layout_row_dynamic(ctx, 20, 1);
            nk_label(ctx, "Name your room (others will see it in the list):", NK_TEXT_LEFT);
            char rb[64];
            if (editing == E_ROOMNAME) snprintf(rb, sizeof rb, "%.24s_", edit_buf);
            else snprintf(rb, sizeof rb, "(click to type a name)");
            nk_layout_row_dynamic(ctx, 30, 1);
            if (input_box(rb, editing == E_ROOMNAME)) focus_edit(E_ROOMNAME);
            nk_layout_row_dynamic(ctx, 30, 2);
            if (nk_button_label(ctx, "Create")) { rr_net_new_room(editing == E_ROOMNAME ? edit_buf : ""); newroom_open = false; stop_edit(); }
            if (nk_button_label(ctx, "Cancel")) { newroom_open = false; stop_edit(); }
        }
        nk_end(ctx);
        nk_window_set_focus(ctx, "New room");
    }
    if (nk_window_is_hidden(ctx, "Lobby")) {                  /* the title-bar close box: really close it, so it can open again later */
        nk_window_close(ctx, "Lobby"); dlg_open = false; stop_edit();
    }
}

void rr_ui_draw(bool *quit)
{
    if (quit_req) *quit = true;

    /* GO armed a race: leave the menu so the player can coin up */
    const bool sess = rr_net_session_active();
    if (sess && !was_session) {
        close_menu();
    }
    was_session = sess;
    if (editing >= 0 && !qchat) {                            /* a text box only exists while it is on screen: never leave typing mode behind a closed window */
        const bool lobby = dlg_open && (rr_net_connected() || rr_net_switching()) && !sess;
        const bool ok = open_ && (editing == E_ROOMNAME ? newroom_open : editing == E_CHAT ? lobby : (tab == T_ONLINE || dlg_open));
        if (!ok) stop_edit();
    }
    { static bool auto_done;                                 /* once per connection: the first time the menu is open while connected, the lobby window opens and the chat box takes the keyboard */
      if (!rr_net_connected()) { if (!rr_net_switching()) auto_done = false; }
      else if (open_ && !auto_done && !sess) { auto_done = true; dlg_open = true; if (editing < 0) begin_edit(E_CHAT); } }
    if (rr_ui_hint_active()) {
        hint_left--;
        int hw, hh; SDL_GetWindowSize(uwin, &hw, &hh);
        const float w = 440 < hw - 8 ? 440.0f : (float)hw - 8;
        if (nk_begin(ctx, "hint", nk_rect(((float)hw - w) / 2, (float)hh - 36, w, 28), NK_WINDOW_NO_SCROLLBAR)) {
            nk_layout_row_dynamic(ctx, 18, 1);
            nk_label(ctx, hint_text, NK_TEXT_CENTERED);
        }
        nk_end(ctx);
        chat_overlay(hw, hh);
        nk_sdl_render(NK_ANTI_ALIASING_ON);
        return;
    }
    if (ctx && !open_) {                         /* menu closed: only the chat overlay (the quick chat box, recent lines) */
        int cw, ch; SDL_GetWindowSize(uwin, &cw, &ch);
        chat_overlay(cw, ch);
        nk_sdl_render(NK_ANTI_ALIASING_ON);
        return;
    }
    if (!ctx) return;
    int ww, wh;
    SDL_GetWindowSize(uwin, &ww, &wh);
    /* THE MENU BAR across the top of the window, as in Prop Cycle. The chosen
     * page drops down under its title; on the keyboard the bar is the row
     * above the first row of the dropdown. */
    static const float title_w[T_N] = { 50, 80, 70, 90, 80, 70, 64 };
    float title_x[T_N], x = 4; int nshown = 0;
    for (int t = 0; t < T_N; t++) { title_x[t] = x; if (tab_shown(t)) { x += title_w[t] + 4; nshown++; } }
    if (nk_begin(ctx, "menubar", nk_rect(0, 0, (float)ww, 28), NK_WINDOW_NO_SCROLLBAR)) {
        nk_menubar_begin(ctx);
        nk_layout_row_begin(ctx, NK_STATIC, 20, nshown + 1);
        for (int t = 0; t < T_N; t++) {
            if (!tab_shown(t)) continue;
            nk_layout_row_push(ctx, title_w[t]);
            if (nk_select_label(ctx, tab_name[t], NK_TEXT_CENTERED, t == tab) && t != tab) { tab = t; row = 0; }
        }
        nk_layout_row_push(ctx, (float)ww - x - 8 > 60 ? (float)ww - x - 8 : 60);
        nk_label(ctx, row < 0 ? "<- Left/Right ->   Down: open   Esc: close" : "Arrows, Enter, Esc", NK_TEXT_RIGHT);
        nk_menubar_end(ctx);
    }
    nk_end(ctx);

    /* the dropdown: sized to its page, under its title, inside the window */
    static const float drop_w[T_N] = { 300, 440, 300, 340, 380, 420, 460 };
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
            labelf(NK_TEXT_LEFT, "Select, then press key/button or move control (Esc cancels)");
        } else if (tab == T_RECORD) {
            labelf(NK_TEXT_LEFT, "Records the cabinet inputs per frame to recordings/;");
            labelf(NK_TEXT_LEFT, "replay with rr --replay FILE. F9 toggles without the menu.");
        } else if (tab == T_DEBUG) {
            labelf(NK_TEXT_LEFT, "Developer screens left in the program (nothing in the game reaches them).");
            labelf(NK_TEXT_LEFT, "They run while the Test switch is on: F2 or File > Test mode returns to the game.");
            if (row >= 0 && row < g_rr_game->ndebug && g_rr_game->debug[row].help) labelf(NK_TEXT_LEFT, "%s", g_rr_game->debug[row].help);
        } else if (tab == T_ONLINE) {
            labelf(NK_TEXT_LEFT, "Address entry needs a keyboard.");
            if (!rr_net_connected()) labelf(NK_TEXT_LEFT, "LAN: one player picks Host a LAN game, the others Find LAN games and Join. Or type a server host:port.");
        }

    }
    nk_end(ctx);
    if (dlg_open) { if ((rr_net_connected() || rr_net_switching()) && !rr_net_session_active()) lobby_window(ww, wh); else online_dialog(ww, wh); }

    nk_sdl_render(NK_ANTI_ALIASING_ON);        /* scales window units to the drawable itself */
}
