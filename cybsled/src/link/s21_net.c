/*
 * s21_net.c -- Cyber Sled online: the shared NMN2 client (engine/net.c, docs/NETPLAY.md) carrying the C139 link's per-frame payload
 * (s21_link.c). Pattern of tokyowar/src/tw_net.c.
 *
 *   CS_NET_SERVER=host[:port] [CS_NET_NAME=x]   connect at boot (headless too)     CS_NET_HOST=1   host a LAN game (built-in server)
 *   CS_NET_DISCOVER=1                            join the first LAN host found       CS_NET_AUTOSTART=2 (engine/net.c) start when both are in
 *   CS_NET_LEAD=<n>                              stall-and-wait lead (default: eng_net_auto_lead(); 1000 = never wait)
 *
 * Any of these turns the chip on from power-on, as a cabinet whose link cable is unplugged; GO (session_begin) sets POSITION = our slot
 * (the operator setting in the NVRAM and the working copies the program read from it) and plugs the cable in. Each frame: the packets
 * our chip transmitted go out in one fixed-size payload (frame_out), every peer payload's packets go onto our receive line (frame_in).
 * The game's own protocol (its 16-frame "heard" window and the ring's hop count) copes with lateness and loss, as on the real cable.
 * From the window: the Online page of the menu (s21_net_window_init).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "s21_link.h"
#include "s21_board.h"
#include "s21_io.h"
#include "c139.h"
#include "net.h"
#ifdef S21_WITH_HOST
#include "eng_ui.h"
#include "eng_net_ui.h"
#endif

static int in_session, inited;

/* ONLINE PLAY NEEDS NO COINS AND GIVES EVERYONE TIME (Rave Racer's rule, raverace/src/rr_net.c): GO turns FREE PLAY on -- the
 * operator setting COIN OPTIONS > FREE PLAY, NVRAM byte 8 (the master copies NVRAM words 0x180004.. to 0x901B30.. at boot, master
 * 0x221A; the slave's test page shows 0x901B3C as FREE PLAY YES/NO, 0x01DAAC), read at boot, so it goes in before the board
 * restart -- and a 20 s window opens in which each player presses Start; whoever has not by then gets it pressed for them. The
 * cabinet's own setting comes back when the session ends and is never written to cs21.nv (s21_net_restore_settings). */
#define NV_FREEPLAY 8
#define START_WAIT_FRAMES (20 * 60)
#define AUTO_START_FRAMES 8
static int fp_saved = -1, wait_left, start_left;
static void nv_checksum(void)                                /* the settings block's check byte (master 0x21AA) */
{
    uint8_t sum = 0;
    for (int i = 0; i < 0x16; i++) sum = (uint8_t)(sum + g_s21.nvram[i]);
    g_s21.nvram[0x16] = sum;
}
static void session_free_play(int on)
{
    if (on) { if (fp_saved < 0) { fp_saved = g_s21.nvram[NV_FREEPLAY]; g_s21.nvram[NV_FREEPLAY] = 1; nv_checksum(); } }
    else if (fp_saved >= 0) { g_s21.nvram[NV_FREEPLAY] = (uint8_t)fp_saved; fp_saved = -1; nv_checksum(); }
}
void s21_net_restore_settings(void) { session_free_play(0); }

static void say(const char *t, int frames)
{
#ifdef S21_WITH_HOST
    eng_ui_set_hint(t, frames);
#else
    (void)frames; fprintf(stderr, "[CS] %s\n", t);
#endif
}
static void start_window(void)                               /* each frame of a session, after the cabinet's inputs are in */
{
    s21_inputs in;
    if (wait_left > 0) {
        s21_io_get_inputs(&in);
        if (!(in.mcub & 0x80)) { wait_left = 0; say("Online battle: go! Pick your course, then wait for your opponent", 240); return; }
        if (--wait_left == 0) start_left = AUTO_START_FRAMES;
        else if (wait_left % 30 == 0) {
            char msg[96]; snprintf(msg, sizeof msg, "ONLINE BATTLE  -  press Start   (%d s)", (wait_left + 59) / 60);
            say(msg, 45);
        }
        return;
    }
    if (start_left > 0) {
        s21_io_get_inputs(&in); in.mcub &= (uint8_t)~0x80; s21_io_set_inputs(&in);
        if (start_left-- == AUTO_START_FRAMES) say("Online battle: starting for you (Start was not pressed)", 180);
    }
}

static void session_begin(int slot, int players)
{
    if (slot < 0 || slot > 1) { fprintf(stderr, "[CS] online: slot %d -- Cyber Sled links two cabinets (0 LEFT, 1 RIGHT); staying alone\n", slot); return; }
    /* POSITION is an operator setting the program reads at boot (master 0x267A, slave 0x3188): writing the working copies into a
     * running game traps (the master's attract sequencer, 0x03C68A -> 0, seen when the switch landed mid-sequence). So do what the
     * cabinet does when the operator changes it: the setting into the NVRAM, then the board restarts -- both cabinets from boot. */
    s21_link_set_position(slot, false);
    session_free_play(1);
    wait_left = START_WAIT_FRAMES; start_left = 0;
    s21_link_chip_on_live();
    s21_board_reset();
    s21_link_unplug(false);
    in_session = 1;
    fprintf(stderr, "[CS] online: cabinet %d (%s) of %d, link plugged in at frame %u\n", slot, slot ? "RIGHT" : "LEFT", players, g_s21.frame);
}

static void session_end(void)
{
    in_session = 0; wait_left = start_left = 0;
    session_free_play(0);
    s21_link_unplug(true);
    fprintf(stderr, "[CS] online: session over at frame %u, link unplugged\n", g_s21.frame);
}

static int frame_out(uint8_t *buf)
{
    memset(buf, 0, S21_NET_FRAME);
    s21_link_pack_n(buf, S21_NET_FRAME, 4);
    return 1;
}

static void frame_in(int slot, const uint8_t *buf, uint32_t seq)
{
    (void)slot; (void)seq;
    if (in_session) s21_link_unpack(buf, S21_NET_FRAME);
}

static eng_net_game net_game = {
    "cs", 1, S21_LINK_PLAYERS, S21_NET_FRAME, "CS_NET", "battle", 0,
    session_begin, session_end, frame_out, frame_in, say,
};

static void init_once(void) { if (!inited) { inited = 1; eng_net_init(&net_game); s21_link_net_poll_on(); } }

/* s21_link_frame_begin, online: before the frame runs */
void s21_net_frame(uint32_t frame)
{
    (void)frame;
    eng_net_poll();
    static int lead = -2;
    if (lead == -2) { const char *e = getenv("CS_NET_LEAD"); lead = e ? atoi(e) : -1; }
    if (in_session && lead < 1000) eng_net_wait(lead < 0 ? eng_net_auto_lead() : lead, 1000);
    if (in_session) start_window();
}

void s21_net_paused(void) { if (inited) eng_net_poll_paused(); }

bool s21_net_boot(void)
{
    const char *srv = getenv("CS_NET_SERVER"), *hst = getenv("CS_NET_HOST"), *dsc = getenv("CS_NET_DISCOVER"), *nm = getenv("CS_NET_NAME");
    const bool want = (srv && *srv) || (hst && *hst == '1') || (dsc && *dsc == '1');
    if (!want) return false;
    init_once();
    s21_link_net_mode();                                 /* headless online: the chip on from power-on, unplugged */
    if (nm && *nm) eng_net_set_name(nm);
    if (hst && *hst == '1') eng_net_host_start();
    else if (dsc && *dsc == '1') { eng_net_discover_autojoin(1); eng_net_discover(); }
    else if (eng_net_set_server(srv)) eng_net_connect();
    fprintf(stderr, "[CS] online: %s\n", hst && *hst == '1' ? "hosting a LAN game" : dsc && *dsc == '1' ? "looking for a LAN host" : srv);
    return true;
}

#ifdef S21_WITH_HOST
/* the window: the Online page. The client stays inert (no socket) until the player connects; at GO the chip is switched on in the state
 * the driver's init (slave 0x3692) leaves it, so a player who connects mid-game is linked without a reset. */
void s21_net_window_init(void)
{
    init_once();                                         /* inert until the player connects: no socket, the chip off until GO */
    eng_ui_add_page(eng_net_ui_page());
}
#endif
