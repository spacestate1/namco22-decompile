/*
 * rr_net.c -- online play for the System 22 runtime (Rave Racer, Ace Driver): the shared client engine/net.c (NMN2, and
 * RRN1 for Rave Racer with old servers / old LAN hosts; docs/NETPLAY.md) plus what is this board's own:
 *
 *  - the payload: the game's C139 link packet (src/rr_link.c), 38 bytes + 2 zero bytes = 40 on the wire. We send our NEWEST
 *    staged packet each frame (no backlog); a peer's packet goes into the link through rr_link_rx_push, and rr_main injects
 *    every pending peer's newest packet at the frame edge, each through its own SCI interrupt.
 *  - GO: the lobby slot becomes this cabinet's link number (re-poked for 6 s: the boot-time settings reload would overwrite
 *    it), free play is turned on for the session, and a 20 s window opens in which each player steps on the gas themselves
 *    (attract -> car select); a machine whose player has not pressed by then presses for them.
 *
 * The rr_net_* API (rr_net.h) is what rr_main.c / rr_ui.c always called; it maps onto eng_net_*. Test switches keep their
 * RR_NET_ prefix (RR_NET_DEBUG, _AUTOSTART, _SAY, _ROOM, _RENAME, _SIM, _SIM_SEED).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "rr_game.h"
#include "net.h"
#include "rr_net.h"
#include "rr_link.h"
#include "rr_hw.h"
#include "rr_ui.h"

#define WIRE_PKT 40                              /* the 38-byte link packet + 2 zero fill bytes (RRN1's FRAME packet) */

static int cab_reapply;                          /* frames left re-poking the cabinet number */
static int my_slot = -1;

/* ONLINE PLAY NEEDS NO COINS AND GIVES EVERYONE TIME: GO turns free play on (the cabinet's own setting comes back when the
 * session ends) and opens a 20 s window in which each player steps on the gas themselves -- what takes the game from attract
 * to its car select. A banner counts the window down on every machine. If a player has not pressed by then the machine
 * presses for them, so nobody is left behind. */
static int fp_saved = -1;
static int wait_left, gas_left;
#define GAS_WAIT_FRAMES (20 * 60)
#define AUTO_GAS_FRAMES 120
static void session_free_play(bool on)
{
    if (on) { if (fp_saved < 0) { fp_saved = rr_hw_freeplay() ? 1 : 0; rr_hw_set_freeplay(true); } }
    else if (fp_saved >= 0) { rr_hw_set_freeplay(fp_saved != 0); fp_saved = -1; }
}
void rr_net_apply_inputs(void)                   /* once per simulated frame, after the host's own input */
{
    if (!eng_net_session_active()) { wait_left = gas_left = 0; return; }
    char msg[96];
    if (wait_left > 0) {
        if (g_hw.gas > 0x200) {
            wait_left = 0;
            rr_ui_set_hint("Online race: go! Choose your car, then wait for the others at the course select", 240);
        } else {
            wait_left--;
            if (wait_left == 0) gas_left = AUTO_GAS_FRAMES;
            else if (wait_left % 30 == 0) {
                snprintf(msg, sizeof msg, "ONLINE RACE  -  step on the gas to start   (%d s)", (wait_left + 59) / 60);
                rr_ui_set_hint(msg, 45);
            }
        }
        return;
    }
    if (gas_left > 0) {
        g_hw.gas = (uint16_t)g_rr_game->gas_max; gas_left--;
        if (gas_left == AUTO_GAS_FRAMES - 1) rr_ui_set_hint("Online race: starting for you (no gas pressed)", 180);
    }
}

/* ---- the hooks ---- */
static void session_begin(int slot, int players)
{
    (void)players;
    if (!rr_link_net_legacy()) { rr_link_pkt_t junk; rr_link_tx_pop_latest(&junk); }   /* packets staged before the race: never sent */
    my_slot = slot;
    /* the lobby slot IS the cabinet number; the EEPROM patch wins only after the boot-time settings reload (~frame 300) */
    rr_hw_set_link_cabinet(slot);
    cab_reapply = 360;
    rr_link_net_active(true);                    /* session: we own the link TX queue, loopback yields */
    session_free_play(true);
    wait_left = GAS_WAIT_FRAMES; gas_left = 0;
}
static void session_end(void)
{
    session_free_play(false);
    rr_link_net_active(false);
}
static uint32_t n_staged_warn;
static int frame_out(uint8_t *buf)
{
    if (cab_reapply > 0) { rr_hw_set_link_cabinet(my_slot); cab_reapply--; }
    rr_link_pkt_t p;
    if (!(rr_link_net_legacy() ? rr_link_tx_pop(&p) : rr_link_tx_pop_latest(&p))) return 0;   /* RR_NET_LEGACY=1: the oldest, as before (A/B) */
    if (p.id != my_slot && n_staged_warn++ % 60 == 0) { const char *e = getenv("RR_NET_DEBUG"); if (e && *e == '1') fprintf(stderr, "[NET] staged id %d != slot %d\n", p.id, my_slot); }
    memcpy(buf, p.data, RR_LINK_PKT_LEN);
    memset(buf + RR_LINK_PKT_LEN, 0, WIRE_PKT - RR_LINK_PKT_LEN);
    return 1;
}
static void frame_in(int slot, const uint8_t *buf, uint32_t fseq)
{
    (void)fseq;
    if (slot >= 8) return;
    rr_link_pkt_t pkt;
    memcpy(pkt.data, buf, RR_LINK_PKT_LEN);
    pkt.id = (uint8_t)slot;
    pkt.data[0] = 0; pkt.data[1] = (uint8_t)slot;          /* the receive path's sender check */
    rr_link_rx_push(&pkt);
}
static void hint(const char *t, int frames) { rr_ui_set_hint(t, frames); }

/* Rave Racer is "rr" (and speaks RRN1 to old servers); Ace Driver, on the same runtime, is a different game: "ad" */
static eng_net_game game = { "rr", 1, 8, WIRE_PKT, "RR_NET", "race", 1, session_begin, session_end, frame_out, frame_in, hint };
static void init(void)
{
    static int done;
    if (done) return;
    done = 1;
    if (g_rr_game && g_rr_game->name && strcmp(g_rr_game->name, "raverace") != 0) {
        game.id[0] = 'a'; game.id[1] = 'd';
        if (strcmp(g_rr_game->name, "acedrive") != 0) { game.id[0] = g_rr_game->name[0]; game.id[1] = g_rr_game->name[1]; }
        game.rrn1 = 0;
    }
    eng_net_init(&game);
}

/* ---- the rr_net API over eng_net ---- */
bool rr_net_set_server(const char *hp) { init(); return eng_net_set_server(hp); }
const char *rr_net_server(void) { return eng_net_server(); }
void rr_net_preset_server(const char *hp) { init(); eng_net_preset_server(hp); }
void rr_net_set_name(const char *name) { init(); eng_net_set_name(name); }
const char *rr_net_name(void) { return eng_net_name(); }
bool rr_net_connect(void) { init(); return eng_net_connect(); }
void rr_net_disconnect(void) { eng_net_disconnect(); }
void rr_net_status(char *buf, size_t n) { init(); eng_net_status(buf, n); }
bool rr_net_connected(void) { return eng_net_connected(); }
bool rr_net_session_active(void) { return eng_net_session_active(); }
bool rr_net_roster(int i, char *name, size_t n, int *ready, int *self) { return eng_net_roster(i, name, n, ready, self); }
int  rr_net_roster_count(void) { return eng_net_roster_count(); }
void rr_net_set_ready(int ready) { eng_net_set_ready(ready); }
void rr_net_request_start(void) { eng_net_request_start(); }
bool rr_net_host_start(void) { init(); return eng_net_host_start(); }
void rr_net_host_stop(void) { eng_net_host_stop(); }
bool rr_net_hosting(void) { return eng_net_hosting(); }
void rr_net_discover(void) { init(); eng_net_discover(); }
bool rr_net_discovering(void) { return eng_net_discovering(); }
void rr_net_discover_autojoin(int on) { eng_net_discover_autojoin(on); }
int  rr_net_found_count(void) { return eng_net_found_count(); }
bool rr_net_found(int i, char *label, size_t n, char *addr, size_t an) { return eng_net_found(i, label, n, addr, an); }
bool rr_net_chat_send(const char *text) { return eng_net_chat_send(text); }
int  rr_net_chat_count(void) { return eng_net_chat_count(); }
bool rr_net_chat_line(int i, char *out, size_t n) { return eng_net_chat_line(i, out, n); }
uint32_t rr_net_chat_serial(void) { return eng_net_chat_serial(); }
void rr_net_chat_clear(void) { eng_net_chat_clear(); }
int  rr_net_room_count(void) { return eng_net_room_count(); }
bool rr_net_room(int i, int *id, int *state, int *players, int *mine, char *name, size_t n) { return eng_net_room(i, id, state, players, mine, name, n); }
void rr_net_switch_room(int id) { eng_net_switch_room(id); }
void rr_net_new_room(const char *name) { eng_net_new_room(name); }
void rr_net_delete_room(int id) { eng_net_delete_room(id); }
bool rr_net_switching(void) { return eng_net_switching(); }
void rr_net_poll_paused(void) { init(); eng_net_poll_paused(); }
void rr_net_poll(void) { init(); eng_net_poll(); }
