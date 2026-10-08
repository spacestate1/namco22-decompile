/*
 * eng_net_ui.c -- the ONLINE page of the shared menu (engine/eng_ui.c) for every game on engine/net.c. Rows, top to bottom:
 *
 *   Server        the address (Enter: type it; saved as net_server, default zonesync.net)
 *   Name          the lobby name (Enter: type it; saved as net_name)
 *   Connect       Connect / Disconnect (joins the fullest open lobby of THIS game, or opens a room)
 *   Status        one line
 *   Host LAN      Host a LAN game (the built-in server, engine/net_host.c) / Stop hosting
 *   Find LAN      search the LAN 4 s; the hosts found follow as rows (Enter joins)
 * connected, in addition:
 *   Ready         Yes / No          Start   (when everyone is Ready)          Say...   (a chat line)
 *   New room      (Enter: name it, Enter again creates it)
 *   one row per player in the room, then one row per room of THIS game (Enter: move there; another link version is shown
 *   but cannot be joined)
 * and the last chat lines under the rows. Text is typed with the keyboard (SDL text input), Enter takes it, Esc cancels.
 *
 * THE WINDOWS (Rave Racer's online UI, raverace/src/rr_ui.c, made game-independent): the first row, "Host / join a game...", opens
 *   - not connected: the ONLINE PLAY window -- your name, the LAN (Host a LAN game / Find LAN games, each game found with a Join
 *     button), the public server (Connect to ZoneSync), a custom server (an address box + Connect), the status;
 *   - connected: the LOBBY window -- where you are, your name, the players (ready flags, you), every room of this game with Join
 *     (and X for an empty one), "+ New room..." (a dialog: name it, Create), the CHAT with a Send box, Ready / Start / Disconnect.
 *   It opens by itself when a connection is made while the menu is open. Esc (pad B) closes a window.
 * With the menu closed: T opens a QUICK CHAT box (connected only; Enter sends, Esc / T on an empty box closes); new lines show in
 * an overlay at the bottom-left for 10 s -- in a session only a hint, "New chat message - press T", so the game is not covered.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <SDL2/SDL.h>
#include "eng_ui.h"
#include "eng_cfg.h"
#include "net.h"
#include "eng_net_ui.h"
#include "eng_nk.h"

enum { R_WINDOW, R_SERVER, R_NAME, R_CONNECT, R_STATUS, R_HOST, R_FIND, R_FIXED };
enum { C_READY = R_FIXED, C_START, C_SAY, C_NEWROOM, C_LIST };     /* connected rows; C_LIST = the first player row */
enum { E_NONE, E_SERVER, E_NAME, E_SAY, E_ROOM };

static char server[128] = "zonesync.net";
static char edit_buf[128];
static int editing;

static void stop_edit(void) { editing = E_NONE; SDL_StopTextInput(); }

static void commit(void)
{
    switch (editing) {
    case E_SERVER:
        if (edit_buf[0]) { snprintf(server, sizeof server, "%s", edit_buf); eng_cfg_set("net_server", server); eng_net_preset_server(server); }
        break;
    case E_NAME:
        if (edit_buf[0]) { eng_net_set_name(edit_buf); eng_cfg_set("net_name", eng_net_name()); }
        break;
    case E_SAY: if (edit_buf[0]) eng_net_chat_send(edit_buf); break;
    case E_ROOM: eng_net_new_room(edit_buf); break;
    }
}

static bool edit_cb(const SDL_Event *e, void *u)
{
    (void)u;
    if (!e) { stop_edit(); return true; }                         /* the menu closed */
    if (e->type == SDL_TEXTINPUT) {
        size_t l = strlen(edit_buf);
        for (const char *c = e->text.text; *c && l + 1 < sizeof edit_buf; c++) edit_buf[l++] = *c;
        edit_buf[l] = 0;
        return false;
    }
    if (e->type == SDL_KEYDOWN) {
        switch (e->key.keysym.scancode) {
        case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: commit(); stop_edit(); return true;
        case SDL_SCANCODE_ESCAPE: stop_edit(); return true;
        case SDL_SCANCODE_BACKSPACE: {
            size_t l = strlen(edit_buf);
            while (l > 0 && ((unsigned char)edit_buf[l - 1] & 0xC0) == 0x80) l--;   /* a whole UTF-8 character */
            if (l > 0) l--;
            edit_buf[l] = 0;
            return false; }
        default: return false;
        }
    }
    if (e->type == SDL_CONTROLLERBUTTONDOWN && e->cbutton.button == SDL_CONTROLLER_BUTTON_B) { stop_edit(); return true; }
    return false;
}
static void start_edit(int what, const char *init)
{
    editing = what;
    snprintf(edit_buf, sizeof edit_buf, "%s", init ? init : "");
    SDL_StartTextInput();
    eng_ui_capture_input(edit_cb, NULL);
}

static int nplayers(void) { return eng_net_connected() ? eng_net_roster_count() : 0; }
static int nrows(void)
{
    if (!eng_net_connected()) return R_FIXED + eng_net_found_count();
    return C_LIST + nplayers() + eng_net_room_count();
}
static bool has_value(int r) { (void)r; return false; }
static bool enabled(int r)
{
    if (r == R_STATUS) return true;
    if (eng_net_connected()) {
        if (r == C_START) return eng_net_all_ready();
        if (r >= C_LIST && r < C_LIST + nplayers()) return false;   /* player rows are information */
        if (r >= C_LIST + nplayers()) return eng_net_room_compatible(r - C_LIST - nplayers()) && !eng_net_hosting();
    }
    return true;
}

static void text(int r, char *label, size_t ln, char *value, size_t vn)
{
    value[0] = 0;
    const char *cur = editing ? edit_buf : NULL;
    switch (r) {
    case R_WINDOW: snprintf(label, ln, "%s", eng_net_connected() ? "Lobby / chat..." : "Host / join a game..."); return;
    case R_SERVER: snprintf(label, ln, "Server"); if (editing == E_SERVER) snprintf(value, vn, "%s_", cur); else snprintf(value, vn, "%s", server); return;
    case R_NAME: snprintf(label, ln, "Name"); if (editing == E_NAME) snprintf(value, vn, "%s_", cur); else snprintf(value, vn, "%s", eng_net_name()); return;
    case R_CONNECT: snprintf(label, ln, "%s", eng_net_hosting() ? "Stop hosting" : eng_net_connected() ? "Disconnect" : "Connect"); return;
    case R_STATUS: snprintf(label, ln, "Status"); eng_net_status(value, vn); return;
    case R_HOST: snprintf(label, ln, "%s", eng_net_hosting() ? "Stop hosting" : "Host a LAN game"); return;
    case R_FIND: snprintf(label, ln, "Find LAN games"); snprintf(value, vn, "%s", eng_net_discovering() ? "searching..." : ""); return;
    }
    if (!eng_net_connected()) {
        char lb[96];
        snprintf(label, ln, "Join");
        if (eng_net_found(r - R_FIXED, lb, sizeof lb, NULL, 0)) snprintf(value, vn, "%s", lb);
        return;
    }
    switch (r) {
    case C_READY: snprintf(label, ln, "Ready"); snprintf(value, vn, "%s", eng_net_self_ready() ? "Yes  (Enter: not ready)" : "No  (Enter: ready)"); return;
    case C_START: snprintf(label, ln, "Start"); snprintf(value, vn, "%s", eng_net_all_ready() ? "Start!" : "waiting for everyone to be Ready"); return;
    case C_SAY: snprintf(label, ln, "Say"); if (editing == E_SAY) snprintf(value, vn, "%s_", cur); else snprintf(value, vn, "(Enter: type a line)"); return;
    case C_NEWROOM: snprintf(label, ln, "New room"); if (editing == E_ROOM) snprintf(value, vn, "%s_", cur); else snprintf(value, vn, "(Enter: name it)"); return;
    }
    const int np = nplayers();
    if (r < C_LIST + np) {
        char nm[24]; int rdy = 0, self = 0;
        eng_net_roster(r - C_LIST, nm, sizeof nm, &rdy, &self);
        snprintf(label, ln, "Player %d", eng_net_roster_slot(r - C_LIST) + 1);
        snprintf(value, vn, "%s%s%s", nm, rdy ? "   ready" : "", self ? "   (you)" : "");
        return;
    }
    const int i = r - C_LIST - np;
    int id = 0, state = 0, pl = 0, mine = 0; char nm[40] = "";
    eng_net_room(i, &id, &state, &pl, &mine, nm, sizeof nm);
    snprintf(label, ln, "Room %d", id);
    snprintf(value, vn, "%s  %d/%d  %s%s%s", nm, pl, eng_net_room_max(i), state ? "playing" : "lobby",
             mine ? "  (you are here)" : "", eng_net_room_compatible(i) ? "" : "  [other version]");
}

static void change(int r, int dir)
{
    if (dir != 0 || editing) return;
    switch (r) {
    case R_WINDOW: { extern void eng_net_ui_open_window(void); eng_net_ui_open_window(); return; }
    case R_SERVER: start_edit(E_SERVER, server); return;
    case R_NAME: start_edit(E_NAME, eng_net_name()); return;
    case R_CONNECT:
        if (eng_net_hosting()) eng_net_host_stop();
        else if (eng_net_connected()) eng_net_disconnect();
        else { eng_net_preset_server(server); eng_net_connect(); }
        return;
    case R_STATUS: return;
    case R_HOST: if (eng_net_hosting()) eng_net_host_stop(); else eng_net_host_start(); return;
    case R_FIND: eng_net_discover(); return;
    }
    if (!eng_net_connected()) {
        char ad[64];
        if (eng_net_found(r - R_FIXED, NULL, 0, ad, sizeof ad) && eng_net_set_server(ad)) eng_net_connect();
        return;
    }
    switch (r) {
    case C_READY: eng_net_set_ready(!eng_net_self_ready()); return;
    case C_START: eng_net_request_start(); return;
    case C_SAY: start_edit(E_SAY, ""); return;
    case C_NEWROOM: { char d[40]; snprintf(d, sizeof d, "%.16s's room", eng_net_name()); start_edit(E_ROOM, d); return; }
    }
    const int np = nplayers();
    if (r >= C_LIST + np) {
        int id = 0, mine = 0;
        if (eng_net_room(r - C_LIST - np, &id, NULL, NULL, &mine, NULL, 0) && !mine) eng_net_switch_room(id);
    }
}

static void notes(void (*line)(const char *fmt, ...))
{
    const int n = eng_net_chat_count();
    char ln[160];
    for (int i = n > 6 ? n - 6 : 0; i < n; i++) if (eng_net_chat_line(i, ln, sizeof ln)) line("%s", ln);
    if (!n) line("%s", eng_net_connected() ? "(chat: Say)" : "Online play: Connect to a server, or Host / Find a LAN game.");
}


/* ================= THE WINDOWS (a layer of the shared menu, eng_ui_add_layer) ================= */
enum { W_NONE, W_NAME, W_SERVER, W_CHAT, W_ROOM, W_QCHAT };
static bool win_open, newroom_open, qchat, qchat_skip_t;
static int wedit;                                         /* which box has the keyboard */
static char wbuf[128];
static char dlg_msg[96];
static uint32_t chat_seen, chat_show_until;
static int was_connected;

static void wstop(void) { if (wedit) SDL_StopTextInput(); wedit = W_NONE; }
static void wfocus(int which, const char *init) { wedit = which; snprintf(wbuf, sizeof wbuf, "%s", init ? init : ""); SDL_StartTextInput(); }
void eng_net_ui_open_window(void) { win_open = true; dlg_msg[0] = 0; if (eng_net_connected()) wfocus(W_CHAT, ""); }

static void wcommit(void)                                 /* Enter in a window's box */
{
    switch (wedit) {
    case W_NAME: if (wbuf[0]) { eng_net_set_name(wbuf); eng_cfg_set("net_name", eng_net_name()); } wstop(); break;
    case W_SERVER:
        if (wbuf[0]) { snprintf(server, sizeof server, "%s", wbuf); eng_cfg_set("net_server", server); eng_net_preset_server(server); }
        wstop(); break;
    case W_CHAT: if (wbuf[0]) eng_net_chat_send(wbuf); wbuf[0] = 0; break;        /* the box stays open for the next line */
    case W_QCHAT: if (wbuf[0]) eng_net_chat_send(wbuf); qchat = false; wstop(); break;
    case W_ROOM: eng_net_new_room(wbuf); newroom_open = false; wstop(); break;
    }
}

static bool layer_event(SDL_Event *e, bool menu_open)
{
    if (!menu_open && !qchat) {                           /* the menu is closed: T opens the quick chat (connected only) */
        if (e->type == SDL_KEYDOWN && !e->key.repeat && e->key.keysym.scancode == SDL_SCANCODE_T && !(e->key.keysym.mod & (KMOD_CTRL | KMOD_ALT))
            && eng_net_connected()) { qchat = true; qchat_skip_t = true; wfocus(W_QCHAT, ""); return true; }
        return false;
    }
    if (!menu_open) win_open = false, newroom_open = false;
    const bool ours = qchat || (menu_open && (win_open || newroom_open));
    if (!ours) return false;                              /* the menu's own rows (and their edit boxes) */
    if (wedit) {
        if (e->type == SDL_TEXTINPUT) {
            if (qchat_skip_t) { qchat_skip_t = false; if (!strcmp(e->text.text, "t") || !strcmp(e->text.text, "T")) return true; }
            size_t l = strlen(wbuf);
            for (const char *c = e->text.text; *c && l + 1 < sizeof wbuf && l < 96; c++) wbuf[l++] = *c;
            wbuf[l] = 0;
            return true;
        }
        if (e->type == SDL_KEYUP && e->key.keysym.scancode == SDL_SCANCODE_T) { qchat_skip_t = false; return true; }
        if (e->type == SDL_KEYDOWN) {
            switch (e->key.keysym.scancode) {
            case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: wcommit(); return true;
            case SDL_SCANCODE_ESCAPE:
                if (qchat) qchat = false; else if (newroom_open) newroom_open = false;
                wstop(); return true;
            case SDL_SCANCODE_BACKSPACE: {
                size_t l = strlen(wbuf);
                while (l > 0 && ((unsigned char)wbuf[l - 1] & 0xC0) == 0x80) l--;
                if (l > 0) l--;
                wbuf[l] = 0; return true; }
            case SDL_SCANCODE_T:                          /* T on an empty quick chat box closes it */
                if (qchat && !wbuf[0] && !e->key.repeat && !qchat_skip_t) { qchat = false; wstop(); }
                return true;
            default: return true;                         /* letters arrive as SDL_TEXTINPUT */
            }
        }
        if (e->type == SDL_CONTROLLERBUTTONDOWN && e->cbutton.button == SDL_CONTROLLER_BUTTON_B) {
            if (qchat) qchat = false; else if (newroom_open) newroom_open = false;
            wstop(); return true;
        }
        if (qchat) return e->type == SDL_KEYDOWN || e->type == SDL_KEYUP;   /* typing: the game does not see keys */
        return false;                                     /* the mouse goes on to the windows */
    }
    if ((e->type == SDL_KEYDOWN && e->key.keysym.scancode == SDL_SCANCODE_ESCAPE) ||
        (e->type == SDL_CONTROLLERBUTTONDOWN && e->cbutton.button == SDL_CONTROLLER_BUTTON_B)) {
        if (newroom_open) newroom_open = false; else win_open = false;
        return true;
    }
    return false;
}
static bool layer_modal(void) { return win_open || newroom_open; }

static void labelf(struct nk_context *ctx, nk_flags a, const char *fmt, ...)
{
    char b[200]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    nk_label(ctx, b, a);
}
static bool input_box(struct nk_context *ctx, const char *text, bool focused)   /* a text field: a button drawn as a box */
{
    struct nk_style_button b = ctx->style.button;
    b.normal = nk_style_item_color(focused ? nk_rgb(30, 45, 70) : nk_rgb(25, 25, 30));
    b.hover = nk_style_item_color(nk_rgb(35, 50, 80)); b.active = b.hover;
    b.border_color = focused ? nk_rgb(120, 170, 255) : nk_rgb(90, 90, 100); b.border = 1;
    b.text_alignment = NK_TEXT_LEFT;
    return nk_button_label_styled(ctx, &b, text);
}
static bool green_button(struct nk_context *ctx, const char *t)
{
    struct nk_style_button gb = ctx->style.button;
    gb.normal = nk_style_item_color(nk_rgb(40, 120, 55)); gb.hover = nk_style_item_color(nk_rgb(55, 150, 70)); gb.active = gb.hover;
    gb.text_normal = gb.text_hover = gb.text_active = nk_rgb(255, 255, 255);
    return nk_button_label_styled(ctx, &gb, t);
}
static bool red_button(struct nk_context *ctx, const char *t)
{
    struct nk_style_button rb = ctx->style.button;
    rb.normal = nk_style_item_color(nk_rgb(120, 45, 45)); rb.hover = nk_style_item_color(nk_rgb(160, 60, 60)); rb.active = rb.hover;
    rb.text_normal = rb.text_hover = rb.text_active = nk_rgb(255, 255, 255);
    return nk_button_label_styled(ctx, &rb, t);
}
static void name_row(struct nk_context *ctx)
{
    nk_layout_row_template_begin(ctx, 28);
    nk_layout_row_template_push_static(ctx, 86);
    nk_layout_row_template_push_dynamic(ctx);
    nk_layout_row_template_end(ctx);
    nk_label(ctx, "Your name:", NK_TEXT_LEFT);
    char nb[80];
    if (wedit == W_NAME) snprintf(nb, sizeof nb, "%.16s_   (Enter saves, Esc cancels)", wbuf);
    else snprintf(nb, sizeof nb, "%.16s     (click to change)", eng_net_name());
    if (input_box(ctx, nb, wedit == W_NAME) && wedit != W_NAME) wfocus(W_NAME, eng_net_name());
}

/* THE ONLINE PLAY WINDOW (not connected): the LAN, the public server, a custom server */
static void online_window(struct nk_context *ctx, int ww, int wh)
{
    const float w = ww - 16 < 620 ? (float)ww - 16 : 620.0f, h = wh - 16 < 560 ? (float)wh - 16 : 560.0f;
    if (nk_begin(ctx, "Online play", nk_rect(((float)ww - w) / 2, ((float)wh - h) / 2, w, h), NK_WINDOW_BORDER | NK_WINDOW_TITLE | NK_WINDOW_MOVABLE)) {
        const bool hosting = eng_net_hosting();
        char st[128]; eng_net_status(st, sizeof st);
        name_row(ctx);
        nk_layout_row_dynamic(ctx, 8, 1); nk_spacing(ctx, 1);
        nk_layout_row_dynamic(ctx, 20, 1);
        nk_label(ctx, "On this network (LAN):", NK_TEXT_LEFT);
        nk_layout_row_dynamic(ctx, 32, 2);
        if (nk_button_label(ctx, hosting ? "Stop hosting" : "Host a LAN game")) {
            wstop();
            if (hosting) eng_net_host_stop();
            else if (!eng_net_host_start()) snprintf(dlg_msg, sizeof dlg_msg, "Could not host: UDP port %d is busy.", ENG_NET_PORT);
        }
        if (nk_button_label(ctx, eng_net_discovering() ? "Searching..." : "Find LAN games")) { wstop(); eng_net_discover(); }
        for (int i = 0, n = eng_net_found_count(); i < n; i++) {
            char lb[96], ad[64];
            if (!eng_net_found(i, lb, sizeof lb, ad, sizeof ad)) continue;
            nk_layout_row_template_begin(ctx, 28);
            nk_layout_row_template_push_dynamic(ctx); nk_layout_row_template_push_static(ctx, 70);
            nk_layout_row_template_end(ctx);
            nk_label(ctx, lb, NK_TEXT_LEFT);
            if (green_button(ctx, "Join") && eng_net_set_server(ad)) { wstop(); eng_net_connect(); }
        }
        if (!eng_net_discovering() && eng_net_found_count() == 0) { nk_layout_row_dynamic(ctx, 18, 1); nk_label(ctx, "(no LAN games found yet)", NK_TEXT_LEFT); }

        nk_layout_row_dynamic(ctx, 10, 1); nk_spacing(ctx, 1);
        nk_layout_row_dynamic(ctx, 20, 1);
        nk_label(ctx, "Public server:", NK_TEXT_LEFT);
        nk_layout_row_dynamic(ctx, 34, 1);
        if (nk_button_label(ctx, "Connect to ZoneSync  (zonesync.net)")) {
            wstop(); dlg_msg[0] = 0;
            if (hosting) eng_net_host_stop();
            snprintf(server, sizeof server, "zonesync.net"); eng_cfg_set("net_server", server);
            if (eng_net_set_server(server)) eng_net_connect(); else snprintf(dlg_msg, sizeof dlg_msg, "zonesync.net did not resolve (offline?)");
        }
        nk_layout_row_dynamic(ctx, 8, 1); nk_spacing(ctx, 1);
        nk_layout_row_dynamic(ctx, 20, 1);
        labelf(ctx, NK_TEXT_LEFT, "Custom server  (host or host:port, default port %d):", ENG_NET_PORT);
        const bool custom = server[0] && strcmp(server, "zonesync.net") != 0;
        char fld[140];
        if (wedit == W_SERVER) snprintf(fld, sizeof fld, "%.90s_", wbuf);
        else if (custom) snprintf(fld, sizeof fld, "%.90s", server);
        else snprintf(fld, sizeof fld, "(click here, type an address, press Enter)");
        nk_layout_row_dynamic(ctx, 28, 1);
        if (input_box(ctx, fld, wedit == W_SERVER) && wedit != W_SERVER) wfocus(W_SERVER, custom ? server : "");
        nk_layout_row_dynamic(ctx, 30, 1);
        if (nk_button_label(ctx, "Connect to custom server")) {
            if (wedit == W_SERVER && wbuf[0]) wcommit(); else wstop();
            if (server[0] && strcmp(server, "zonesync.net") != 0) {
                if (hosting) eng_net_host_stop();
                if (eng_net_set_server(server)) eng_net_connect(); else snprintf(dlg_msg, sizeof dlg_msg, "%.60s did not resolve", server);
            } else snprintf(dlg_msg, sizeof dlg_msg, "Type an address in the box above first.");
        }
        if (dlg_msg[0]) { nk_layout_row_dynamic(ctx, 20, 1); nk_label(ctx, dlg_msg, NK_TEXT_LEFT); }
        nk_layout_row_dynamic(ctx, 20, 1);
        labelf(ctx, NK_TEXT_LEFT, "Status: %s", st);
        nk_layout_row_dynamic(ctx, 28, 1);
        if (nk_button_label(ctx, "Close")) { win_open = false; wstop(); }
    }
    nk_end(ctx);
}

/* THE LOBBY WINDOW (connected): players and rooms in a sidebar, the chat beside them, Ready / Start / Disconnect below */
static void lobby_window(struct nk_context *ctx, int ww, int wh)
{
    const float w = ww - 16 < 1100 ? (float)ww - 16 : 1100.0f, h = wh - 16 < 820 ? (float)wh - 16 : 820.0f;
    if (nk_begin(ctx, "Lobby", nk_rect(((float)ww - w) / 2, ((float)wh - h) / 2, w, h), NK_WINDOW_BORDER | NK_WINDOW_TITLE | NK_WINDOW_MOVABLE)) {
        const bool hosting = eng_net_hosting(), joining = eng_net_switching(), sess = eng_net_session_active();
        const eng_net_game *g = eng_net_game_info();
        const char *word = g && g->session_word ? g->session_word : "game";
        char st[128]; eng_net_status(st, sizeof st);
        char myroom[48] = ""; int myplayers = 0, mymax = g ? g->max_players : 8;
        for (int i = 0, n = eng_net_room_count(); i < n; i++) {
            int mine, pl; char nm[40];
            if (eng_net_room(i, NULL, NULL, &pl, &mine, nm, sizeof nm) && mine) { snprintf(myroom, sizeof myroom, "%s", nm); myplayers = pl; mymax = eng_net_room_max(i); }
        }
        nk_layout_row_dynamic(ctx, 24, 1);
        if (joining) nk_label_colored(ctx, "Joining the room...", NK_TEXT_LEFT, nk_rgb(240, 200, 90));
        else if (sess) { char t[96]; snprintf(t, sizeof t, "In a %s%s%s", word, myroom[0] ? "  -  " : "", myroom); nk_label_colored(ctx, t, NK_TEXT_LEFT, nk_rgb(240, 200, 90)); }
        else if (myroom[0]) { char t[96]; snprintf(t, sizeof t, "You are in:  %s   (%d/%d players)", myroom, myplayers, mymax); nk_label_colored(ctx, t, NK_TEXT_LEFT, nk_rgb(120, 220, 130)); }
        else nk_label_colored(ctx, hosting ? "You are hosting a LAN game" : "You are in the lobby", NK_TEXT_LEFT, nk_rgb(120, 220, 130));
        nk_layout_row_dynamic(ctx, 18, 1);
        labelf(ctx, NK_TEXT_LEFT, "%s   -   %s", hosting ? "this computer (LAN)" : (eng_net_server()[0] ? eng_net_server() : "online"), st);
        name_row(ctx);

        const float gh = h - 215 > 220 ? h - 215 : 220;
        const int lines = (int)((gh - 36) / 22), rc = eng_net_roster_count(), nrooms = eng_net_room_count();
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
                    if (!eng_net_roster(i, nm, sizeof nm, &rdy, &self)) continue;
                    nk_layout_row_dynamic(ctx, 20, 1);
                    labelf(ctx, NK_TEXT_LEFT, "%s%s%s", nm, self ? " (you)" : "", rdy ? "  [ready]" : "");
                }
                nk_group_end(ctx);
            }
            nk_layout_row_dynamic(ctx, rh, 1);
            if (nk_group_begin(ctx, "Rooms", NK_WINDOW_BORDER | NK_WINDOW_TITLE)) {
                if (!nrooms) { nk_layout_row_dynamic(ctx, 20, 1); nk_label(ctx, hosting ? "(a LAN game is one room)" : "(waiting for the list...)", NK_TEXT_LEFT); }
                for (int i = 0; i < nrooms; i++) {
                    int id, state, players, mine; char nm[40];
                    if (!eng_net_room(i, &id, &state, &players, &mine, nm, sizeof nm)) continue;
                    const int mx = eng_net_room_max(i);
                    nk_layout_row_template_begin(ctx, 28);
                    nk_layout_row_template_push_dynamic(ctx); nk_layout_row_template_push_static(ctx, 40); nk_layout_row_template_push_static(ctx, 58); nk_layout_row_template_push_static(ctx, 28);
                    nk_layout_row_template_end(ctx);
                    char rn[48]; snprintf(rn, sizeof rn, "%s%s", mine ? "> " : "", nm);
                    if (mine) nk_label_colored(ctx, rn, NK_TEXT_LEFT, nk_rgb(120, 220, 130)); else nk_label(ctx, rn, NK_TEXT_LEFT);
                    labelf(ctx, NK_TEXT_LEFT, "%d/%d", players, mx);
                    if (mine) nk_label_colored(ctx, "HERE", NK_TEXT_CENTERED, nk_rgb(120, 220, 130));
                    else if (!eng_net_room_compatible(i)) nk_label_colored(ctx, "other ver", NK_TEXT_CENTERED, nk_rgb(160, 160, 160));
                    else if (state) nk_label_colored(ctx, "playing", NK_TEXT_CENTERED, nk_rgb(220, 140, 120));
                    else if (players >= mx) nk_label_colored(ctx, "full", NK_TEXT_CENTERED, nk_rgb(220, 140, 120));
                    else if (green_button(ctx, "Join") && !hosting && !sess) eng_net_switch_room(id);
                    if (players == 0 && !mine && !state && nrooms > 1) { if (red_button(ctx, "X") && !hosting) eng_net_delete_room(id); }
                    else nk_spacing(ctx, 1);
                }
                nk_group_end(ctx);
            }
            nk_layout_row_dynamic(ctx, 30, 1);
            if (nk_button_label(ctx, "+ New room...") && !hosting && !sess) { char d[40]; snprintf(d, sizeof d, "%.16s's room", eng_net_name()); newroom_open = true; wfocus(W_ROOM, d); }
            nk_group_end(ctx);
        }
        if (nk_group_begin(ctx, "Chat", NK_WINDOW_BORDER | NK_WINDOW_TITLE)) {
            const int n = eng_net_chat_count(), show = n < lines ? n : lines;
            if (!n) { nk_layout_row_dynamic(ctx, 22, 1); nk_label(ctx, "No messages yet - say hello.", NK_TEXT_LEFT); }
            for (int i = n - show; i < n; i++) {
                char ln[160];
                if (!eng_net_chat_line(i, ln, sizeof ln)) continue;
                nk_layout_row_dynamic(ctx, 22, 1);
                nk_label(ctx, ln, NK_TEXT_LEFT);
            }
            nk_group_end(ctx);
        }
        char say[140];
        if (wedit == W_CHAT) snprintf(say, sizeof say, "%.96s_", wbuf); else snprintf(say, sizeof say, "Type a message...");
        nk_layout_row_template_begin(ctx, 28);
        nk_layout_row_template_push_dynamic(ctx);
        nk_layout_row_template_push_static(ctx, 90);
        nk_layout_row_template_end(ctx);
        if (input_box(ctx, say, wedit == W_CHAT) && wedit != W_CHAT) wfocus(W_CHAT, "");
        if (nk_button_label(ctx, "Send")) { if (wedit == W_CHAT && wbuf[0]) wcommit(); else wfocus(W_CHAT, ""); }
        nk_layout_row_dynamic(ctx, 28, 4);
        char stb[48]; snprintf(stb, sizeof stb, "Start %s", word);
        if (nk_button_label(ctx, eng_net_self_ready() ? "Not ready" : "Ready") && !sess) eng_net_set_ready(!eng_net_self_ready());
        if (nk_button_label(ctx, stb) && !sess) eng_net_request_start();     /* refused until everyone is Ready */
        if (nk_button_label(ctx, hosting ? "Stop hosting" : "Disconnect")) { wstop(); if (hosting) eng_net_host_stop(); else eng_net_disconnect(); }
        if (nk_button_label(ctx, "Close")) { win_open = false; wstop(); }
    }
    nk_end(ctx);
    if (newroom_open) {
        const float dw = ww - 16 < 400 ? (float)ww - 16 : 400.0f, dh = 150;
        if (nk_begin(ctx, "New room", nk_rect(((float)ww - dw) / 2, ((float)wh - dh) / 2, dw, dh), NK_WINDOW_BORDER | NK_WINDOW_TITLE | NK_WINDOW_MOVABLE)) {
            nk_layout_row_dynamic(ctx, 20, 1);
            nk_label(ctx, "Name your room (others will see it in the list):", NK_TEXT_LEFT);
            char rb[64];
            if (wedit == W_ROOM) snprintf(rb, sizeof rb, "%.24s_", wbuf); else snprintf(rb, sizeof rb, "(click to type a name)");
            nk_layout_row_dynamic(ctx, 30, 1);
            if (input_box(ctx, rb, wedit == W_ROOM) && wedit != W_ROOM) wfocus(W_ROOM, "");
            nk_layout_row_dynamic(ctx, 30, 2);
            if (nk_button_label(ctx, "Create")) { if (wedit != W_ROOM) wfocus(W_ROOM, ""); wcommit(); }
            if (nk_button_label(ctx, "Cancel")) { newroom_open = false; wstop(); }
        }
        nk_end(ctx);
        nk_window_set_focus(ctx, "New room");
    }
}

/* the chat over the game (menu closed): the last lines for 10 s after a new one, the quick chat box while T is open */
static void chat_overlay(struct nk_context *ctx, int ww, int wh)
{
    const bool sess = eng_net_session_active();
    const uint32_t ser = eng_net_chat_serial();
    if (ser != chat_seen) {
        chat_seen = ser; chat_show_until = SDL_GetTicks() + 10000;
        if (sess && !qchat) eng_ui_set_hint("New chat message  -  press T to read and reply", 150);
    }
    if (!eng_net_connected()) { if (qchat) { qchat = false; wstop(); } return; }
    const bool show_lines = qchat || (!sess && eng_net_chat_count() > 0 && (int32_t)(chat_show_until - SDL_GetTicks()) > 0);
    if (!show_lines) return;
    const int n = eng_net_chat_count();
    int show = qchat ? 8 : 5; if (show > n) show = n;
    const float h = 12 + show * 20 + (qchat ? 30 : 0), w = ww - 16 < 620 ? (float)ww - 16 : 620.0f;
    if (nk_begin(ctx, "chat", nk_rect(8, (float)wh - h - 44, w, h), NK_WINDOW_NO_SCROLLBAR | NK_WINDOW_NO_INPUT)) {
        for (int i = n - show; i < n; i++) {
            char ln[160];
            if (!eng_net_chat_line(i, ln, sizeof ln)) continue;
            nk_layout_row_dynamic(ctx, 18, 1);
            nk_label(ctx, ln, NK_TEXT_LEFT);
        }
        if (qchat) {
            nk_layout_row_dynamic(ctx, 24, 1);
            labelf(ctx, NK_TEXT_LEFT, "Say: %.96s_   (Enter sends, Esc closes)", wbuf);
        }
    }
    nk_end(ctx);
}

static void layer_draw(struct nk_context *ctx, int ww, int wh, bool menu_open)
{
    const int conn = eng_net_connected() || eng_net_switching();
    { static int t = -1; if (t < 0) t = getenv("ENG_NET_UI_WINDOW") != NULL;   /* tests: the window open on the first menu frame */
      if (t == 1 && menu_open) { t = 2; win_open = true; if (getenv("ENG_NET_UI_NEWROOM")) { newroom_open = true; wfocus(W_ROOM, "Test room"); } } }
    if (conn && !was_connected && menu_open && !eng_net_session_active()) { win_open = true; if (!wedit) wfocus(W_CHAT, ""); }   /* a connection made from the menu: the lobby */
    if (!conn && was_connected && wedit == W_CHAT) wstop();
    was_connected = conn;
    if (!menu_open) { win_open = newroom_open = false; if (wedit && !qchat) wstop(); chat_overlay(ctx, ww, wh); return; }
    if (qchat) { qchat = false; wstop(); }
    if (win_open) { if (conn) lobby_window(ctx, ww, wh); else online_window(ctx, ww, wh); }
}

static const eng_ui_layer layer = { layer_draw, layer_event, layer_modal };

static const eng_ui_page page = { "Online", 560, 110, 24, nrows, has_value, enabled, text, change, notes };

const eng_ui_page *eng_net_ui_page(void)
{
    static int added; if (!added) { added = 1; eng_ui_add_layer(&layer); }
    const char *s = eng_cfg_get("net_server");
    if (s && *s) snprintf(server, sizeof server, "%s", s);
    if (!eng_net_hosting() && !eng_net_connected() && eng_net_server()[0] == 0) eng_net_preset_server(server);   /* never over a host or a
                                                     connection already made (a game's <P>_NET_HOST / _SERVER at boot): it would send the
                                                     client to the saved server instead of its own LAN game */
    const char *n = eng_cfg_get("net_name");
    if (n && *n) eng_net_set_name(n);
    return &page;
}
