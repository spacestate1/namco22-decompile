/*
 * tw_net.c -- Tokyo Wars online: the shared NMN2 client (engine/net.c, docs/NETPLAY.md) carrying the cabinet link's per-frame payload
 * (engine/ss22_link.h). The link chip is our C139 model; this file only connects the two.
 *
 *   TW_NET_SERVER=host[:port] [TW_NET_NAME=x]   connect at boot (headless too)       TW_NET_HOST=1      host a LAN game (built-in server)
 *   TW_NET_DISCOVER=1                            join the first LAN host found         TW_NET_AUTOSTART=N (engine/net.c) start once N are in
 *
 * Any of these turns the link on from power-on (`--link <cabinet>` with this as its transport): alone, the cabinet runs as a lone linked
 * cabinet does. GO (session_begin) makes this cabinet number = our slot (the operator setting at EEPROM byte 9 AND the driver's working copy
 * 0xE02000, which the game otherwise reads only at boot), and the ring = the slots in the session. Each frame: our payload out
 * (frame_out), the newest payload of every peer in (frame_in); once a frame the ring predecessor's newest goes onto our receive line.
 * Four cabinets at most (the game's id is 2 bits).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ss22_game.h"
#include "ss22_link.h"
#include "net.h"

#define TW_FRAME_SIZE 720                  /* 9 link frames of 80 bytes + the 3-byte header (a battle sends 7-8 a frame: measured max 643) */


static uint8_t  newest[ENG_NET_MAX_PLAYERS][TW_FRAME_SIZE];
static bool     fresh[ENG_NET_MAX_PLAYERS];
static unsigned session_mask;

static void session_begin(int slot, int players)
{
    unsigned m = 0;
    for (int i = 0; i < eng_net_roster_count(); i++) { int s = eng_net_roster_slot(i); if (s >= 0 && s < 4) m |= 1u << s; }
    if (slot < 0 || slot > 3) { fprintf(stderr, "[TW] online: slot %d -- Tokyo Wars links four cabinets (0-3); staying alone\n", slot); m = 0; }
    else ss22_link_set_cabinet(slot);
    session_mask = m;
    memset(fresh, 0, sizeof fresh);
    ss22_link_set_cabinets(m);
    fprintf(stderr, "[TW] online: cabinet %d of %d, ring %X\n", slot, players, m);
}

static void session_end(void) { session_mask = 0; ss22_link_set_cabinets(0); fprintf(stderr, "[TW] online: session over, cabinet alone\n"); }

static int frame_out(uint8_t *buf)
{
    memset(buf, 0, TW_FRAME_SIZE);
    ss22_link_pack(buf, TW_FRAME_SIZE);
    return 1;
}

static void frame_in(int slot, const uint8_t *buf, uint32_t seq)
{
    (void)seq;
    if (slot < 0 || slot >= ENG_NET_MAX_PLAYERS) return;
    memcpy(newest[slot], buf, TW_FRAME_SIZE);
    fresh[slot] = true;
}

static eng_net_game net_game = {
    "tw", 1, 4, TW_FRAME_SIZE, "TW_NET", "battle", 0,
    session_begin, session_end, frame_out, frame_in, NULL,
};

/* the link's per-frame callback (engine/ss22_link.c, at each frame's end) */
static void tw_net_frame(uint32_t frame)
{
    (void)frame;
    eng_net_poll();
    /* stall-and-wait (engine/net.h): never run more than TW_MAX_LEAD frames ahead of the slowest cabinet, so a faster machine waits for
     * a slower one instead of drifting away; a peer silent for 1 s is given up on (the game's own 8-frame alive rule then drops it) */
    { static int lead = -2; if (lead == -2) { const char *e = getenv("TW_NET_LEAD"); lead = e ? atoi(e) : -1; }   /* -1 = auto: 2 + the ping in frames */
      if (session_mask && lead < 1000) eng_net_wait(lead < 0 ? eng_net_auto_lead() : lead, 1000); }
    int pred = ss22_link_predecessor();
    if (session_mask && pred >= 0 && fresh[pred]) ss22_link_unpack(pred, newest[pred], TW_FRAME_SIZE);
    memset(fresh, 0, sizeof fresh);
}

/* before ss22_main: true = online was asked for (the link is then on from power-on, with this as its transport) */
bool tw_net_boot(void)
{
    const char *srv = getenv("TW_NET_SERVER"), *hst = getenv("TW_NET_HOST"), *dsc = getenv("TW_NET_DISCOVER"), *nm = getenv("TW_NET_NAME");
    const bool want = (srv && *srv) || (hst && *hst == '1') || (dsc && *dsc == '1');
    if (!want) return false;
    eng_net_init(&net_game);
    if (nm && *nm) eng_net_set_name(nm);
    if (hst && *hst == '1') eng_net_host_start();
    else if (dsc && *dsc == '1') { eng_net_discover_autojoin(1); eng_net_discover(); }
    else if (eng_net_set_server(srv)) eng_net_connect();
    ss22_link_set_external(tw_net_frame);
#ifdef _WIN32
    if (!getenv("TW_LINK")) _putenv("TW_LINK=0");          /* the link from power-on; GO renumbers the cabinet */
#else
    if (!getenv("TW_LINK")) setenv("TW_LINK", "0", 1);
#endif
    return true;
}
