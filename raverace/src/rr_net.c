/*
 * rr_net.c -- the RRN1 client (raverace/NETPLAY.md; the reference server is
 * server/, or the built-in host rr_netd.c).
 *
 * OFFLINE -> CONNECTING (HELLO, 500 ms retry) -> LOBBY (roster tracking,
 * READY/START, PING 1 Hz) -> SESSION (GO: the lobby slot becomes this
 * cabinet's link number, and the game's C139 link packets -- rr_link.c --
 * are exchanged as FRAMEs at 60 Hz). The game itself never knows the
 * difference from a wired 8-cabinet link: peer FRAMEs are injected through
 * rr_link_rx_push; at the frame edge every peer's newest packet goes into the
 * game, each through its own SCI interrupt (rr_main.c, rr_link_inject_next).
 * We send our NEWEST staged packet each frame (no backlog). RR_NET_SIM=loss%,
 * spike_ms[,burst] imitates a bad Wi-Fi link on the receive side, for tests.
 *
 * Inert by default: no server configured -> OFFLINE -> rr_net_poll is one
 * branch. Debug counters with RR_NET_DEBUG=1; headless bootstrap with
 * RR_NET_SERVER / RR_NET_NAME / RR_NET_AUTOSTART (wired in rr_main.c).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <SDL.h>                /* SDL_GetTicks (lazy-inits the timer: safe headless) */
#include "rr_sock.h"
#include "rr_netd.h"
#include "rr_net.h"
#include "rr_link.h"
#include "rr_hw.h"
#include "rr_ui.h"

#define RR_NET_PORT 27750
#define RETRY_MS    500          /* reliable client messages (HELLO/READY/START) */
#define PING_MS     1000         /* lobby keepalive, and the session fallback */
#define DEAD_MS     10000        /* server silence in connecting/lobby = lost */

enum { ST_OFFLINE, ST_CONNECTING, ST_LOBBY, ST_SESSION };
static const char *st_name[] = { "OFFLINE", "CONNECTING", "LOBBY", "SESSION" };

enum { T_HELLO = 0x01, T_WELCOME, T_ROSTER, T_READY, T_START, T_GO, T_LEAVE,
       T_FRAME, T_PING, T_PONG, T_ACK, T_DISCOVER, T_ANNOUNCE, T_CHAT,
       T_ROOMS, T_ROOMLIST, T_RENAME, T_DELROOM };       /* rrn1-server rooms (NETPLAY.md "Rooms"): list, and a new name */

static void leave_at_exit(void);
static int exit_hook, srv_pending;               /* srv_pending: srv_text is set but not resolved yet */

/* ---- chat: the last lines, oldest first; CHAT is relayed by the server to every member, the sender included ---- */
#define CHAT_LINES 64
#define CHAT_TEXT 96
static char chat_buf[CHAT_LINES][8 + 17 + 2 + CHAT_TEXT + 1];      /* "[HH:MM] " + "Name: text" */
static int chat_n, chat_head;                    /* lines stored, index of the oldest */
static uint32_t chat_serial;                     /* grows with every line received (the UI's 'something new' test) */
static void chat_push(const char *line)
{
    int i = (chat_head + chat_n) % CHAT_LINES;
    if (chat_n == CHAT_LINES) { chat_head = (chat_head + 1) % CHAT_LINES; i = (chat_head + CHAT_LINES - 1) % CHAT_LINES; } else chat_n++;
    time_t t = time(NULL); struct tm *tm = localtime(&t);                /* when it was posted: this computer's clock, as it arrived */
    if (tm) snprintf(chat_buf[i], sizeof chat_buf[i], "[%02d:%02d] %s", tm->tm_hour, tm->tm_min, line);
    else snprintf(chat_buf[i], sizeof chat_buf[i], "%s", line);
    chat_serial++;
}
int rr_net_chat_count(void) { return chat_n; }
uint32_t rr_net_chat_serial(void) { return chat_serial; }
bool rr_net_chat_line(int i, char *out, size_t n)    /* 0 = oldest */
{
    if (i < 0 || i >= chat_n) return false;
    snprintf(out, n, "%s", chat_buf[(chat_head + i) % CHAT_LINES]);
    return true;
}
void rr_net_chat_clear(void) { chat_n = chat_head = 0; }

static int st = ST_OFFLINE;
static rr_sock_t sock = RR_SOCK_BAD;
static struct sockaddr_storage srv; static socklen_t srv_len;
static char srv_text[128];                       /* as given to rr_net_set_server */
static char my_name[24] = "PLAYER";
static char note[64];                            /* why we are offline, when we are */
static uint16_t next_seq = 1;
static int my_slot = -1;
static uint32_t session_id, frame_seq;
static uint32_t last_fseq[8];                    /* per-cabinet dedupe guard */
static struct { uint8_t slot, ready; char name[17]; } roster[8];
static int roster_n;
static uint32_t t_state, last_rx, last_ping_tx, last_frame_tx;
static uint32_t hello_tx, hello_seq;             /* CONNECTING's retransmit */
static int pend_ready, ready_val; static uint16_t ready_seq; static uint32_t ready_tx;
static int pend_start; static uint16_t start_seq; static uint32_t start_tx;
static int cab_reapply;                          /* frames left re-poking the cabinet number (the boot-time settings reload would overwrite the WRAM one) */
static int ping_ms = -1;
static int autostart = -1, debug = -1; static uint32_t autostart_t0;
static uint32_t n_ftx, n_frx, n_roster, n_poll;

static uint32_t now_ms(void) { return SDL_GetTicks(); }

/* ONLINE PLAY NEEDS NO COINS AND GIVES EVERYONE TIME: GO turns free play on (the cabinet's own setting comes back when the
 * session ends) and opens a 20 s window in which each player steps on the gas themselves -- what takes the game from attract
 * to its car select. A banner counts the window down on every machine. If a player has not pressed by then the machine
 * presses for them, so nobody is left behind. */
static int fp_saved = -1;                        /* the free-play setting before the session, or -1 */
static int wait_left, gas_left;                  /* frames left in the wait for the player's gas / of the automatic press */
#define GAS_WAIT_FRAMES (20 * 60)
#define AUTO_GAS_FRAMES 120
static void session_free_play(bool on)
{
    if (on) { if (fp_saved < 0) { fp_saved = rr_hw_freeplay() ? 1 : 0; rr_hw_set_freeplay(true); } }
    else if (fp_saved >= 0) { rr_hw_set_freeplay(fp_saved != 0); fp_saved = -1; }
}
void rr_net_apply_inputs(void)                   /* once per simulated frame, after the host's own input */
{
    if (st != ST_SESSION) { wait_left = gas_left = 0; return; }
    char msg[96];
    if (wait_left > 0) {
        if (g_hw.gas > 0x200) {                  /* the player stepped on the gas themselves */
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
        g_hw.gas = 0x610; gas_left--;
        if (gas_left == AUTO_GAS_FRAMES - 1) rr_ui_set_hint("Online race: starting for you (no gas pressed)", 180);
    }
}

static void set_state(int ns)
{
    if (ns == st) return;
    if (st == ST_SESSION) session_free_play(false);
    if (debug == 1) fprintf(stderr, "[NET] %s -> %s\n", st_name[st], st_name[ns]);
    st = ns;
    t_state = now_ms();
    rr_link_net_active(st == ST_SESSION);        /* session: we own the link TX queue, loopback yields */
}

static void send_msg(int type, uint16_t seq, const void *pl, int len)
{
    uint8_t b[256];
    memcpy(b, "RRN1", 4);
    b[4] = (uint8_t)type; b[5] = 0;
    b[6] = (uint8_t)seq; b[7] = (uint8_t)(seq >> 8);
    b[8] = (uint8_t)len; b[9] = (uint8_t)(len >> 8);
    if (len) memcpy(b + 10, pl, (size_t)len);
    sendto(sock, (const char *)b, 10 + len, 0, (struct sockaddr *)&srv, srv_len);
}

/* ---- rooms (rrn1-server): the list the server last sent, and the room the next HELLO asks for ---- */
#define MAX_ROOMS 20
static struct { uint8_t id, state, players, mine; char name[32]; } rooms_[MAX_ROOMS];
static int rooms_n;
static uint32_t rooms_tx;
static char hello_room_name[32];                /* the name for a room this HELLO creates (ROOM_NEW), "" = the server names it */
static int hello_room_sel;                       /* 0 = the server picks, 1..254 = that room, 255 = a new room */
static int switching;                            /* a room change is in flight: a refusal puts us back in the lobby instead of dropping us */
#define ROOM_NEW 255

static void send_hello(void)
{
    /* printable UTF-8, <= 16 bytes (the contract); retransmits keep the seq
     * (set in rr_net_connect), the WELCOME answers with it */
    uint8_t pl[48]; int n = 0;
    for (const char *c = my_name; *c && n < 16; c++)
        if (*c >= ' ' && (unsigned char)*c != 0x7F) pl[1 + n++] = (uint8_t)*c;
    if (!n) pl[1 + n++] = '?';
    pl[0] = (uint8_t)n;
    int len = 1 + n;
    if (hello_room_sel) pl[len++] = (uint8_t)hello_room_sel;        /* the optional room selector (old servers never see it: auto stays the 1-field HELLO) */
    if (hello_room_sel == 255 && hello_room_name[0]) {              /* a new room: its name, len + bytes */
        int k = 0;
        for (const char *c = hello_room_name; *c && k < 24; c++) if ((unsigned char)*c >= ' ' && (unsigned char)*c != 0x7F) pl[len + 1 + k++] = (uint8_t)*c;
        pl[len] = (uint8_t)k; len += 1 + k;
    }
    send_msg(T_HELLO, hello_seq, pl, len);
    hello_tx = now_ms();
}

/* datagrams count only from the server we talk to: the socket is unconnected,
 * so without this anyone who can reach the port could send GO/ROSTER/FRAME */
static int from_server(const struct sockaddr_storage *a) { return rr_addr_eq(a, &srv); }

static void sim_reset(void);
static void sock_open(void)
{
    sim_reset();                                     /* RR_NET_SIM: nothing held from an old connection reaches the new one */
    rr_sock_init();
    sock = socket(srv.ss_family, SOCK_DGRAM, 0);
    if (sock == RR_SOCK_BAD) return;
    rr_sock_nonblock(sock);
}

static void fail(const char *why)                /* lost/unreachable: close, OFFLINE with the reason */
{
    { const char *e = getenv("RR_NET_DEBUG"); if (e && *e == '1') fprintf(stderr, "[NET] fail: %s\n", why); }
    snprintf(note, sizeof note, "%s", why);
    if (sock != RR_SOCK_BAD) { closesocket(sock); sock = RR_SOCK_BAD; }
    pend_ready = pend_start = 0;
    my_slot = -1; roster_n = 0; session_id = 0;
    set_state(ST_OFFLINE);
}

bool rr_net_set_server(const char *host_port)
{
    srv_pending = 0;
    rr_sock_init();
    if (st != ST_OFFLINE) rr_net_disconnect();
    char host[100], port[8] = "27750";
    const char *colon = strrchr(host_port, ':');
    if (colon && strchr(host_port, ':') == colon && colon != host_port) {  /* host:port (one colon: not IPv6) */
        size_t hl = (size_t)(colon - host_port);
        if (hl >= sizeof host) hl = sizeof host - 1;
        memcpy(host, host_port, hl); host[hl] = 0;
        snprintf(port, sizeof port, "%s", colon + 1);
    } else snprintf(host, sizeof host, "%s", host_port);
    struct addrinfo hint, *res = NULL;
    memset(&hint, 0, sizeof hint);
    hint.ai_family = AF_INET; hint.ai_socktype = SOCK_DGRAM;   /* IPv4 only: the servers are IPv4 UDP, and an AAAA lookup can stall the window on some Windows resolvers */
    if (getaddrinfo(host, port, &hint, &res) != 0 || !res) {
        snprintf(note, sizeof note, "bad server address '%s'", host_port);
        if (debug == 1) fprintf(stderr, "[NET] %s\n", note);
        return false;
    }
    memcpy(&srv, res->ai_addr, res->ai_addrlen);
    srv_len = (socklen_t)res->ai_addrlen;
    freeaddrinfo(res);
    snprintf(srv_text, sizeof srv_text, "%s", host_port);
    note[0] = 0;
    return true;
}
const char *rr_net_server(void) { return srv_text; }

static void send_rename(void)
{
    uint8_t pl[17]; int n = 0;
    for (const char *c = my_name; *c && n < 16; c++)
        if (*c >= ' ' && (unsigned char)*c != 0x7F) pl[1 + n++] = (uint8_t)*c;
    if (!n) pl[1 + n++] = '?';
    pl[0] = (uint8_t)n;
    send_msg(T_RENAME, 0, pl, 1 + n);
}
void rr_net_set_name(const char *name)           /* also while connected: the server updates the roster for everyone */
{
    char old[sizeof my_name]; snprintf(old, sizeof old, "%s", my_name);
    snprintf(my_name, sizeof my_name, "%s", name && *name ? name : "PLAYER");
    if ((st == ST_LOBBY || st == ST_SESSION) && sock != RR_SOCK_BAD && strcmp(old, my_name)) send_rename();
}
const char *rr_net_name(void) { return my_name; }

void rr_net_preset_server(const char *host_port)   /* remembered, NOT resolved: a DNS lookup at every boot would stall an offline start */
{
    snprintf(srv_text, sizeof srv_text, "%s", host_port ? host_port : "");
    srv_pending = srv_text[0] != 0; srv_len = 0;
}

bool rr_net_connect(void)
{
    if (!srv_text[0]) { snprintf(note, sizeof note, "no server set"); return false; }
    if (srv_pending) {                           /* the first connect resolves the remembered address */
        char hp[sizeof srv_text]; snprintf(hp, sizeof hp, "%s", srv_text);
        srv_pending = 0;
        if (!rr_net_set_server(hp)) return false;
    }
    if (st != ST_OFFLINE) return true;
    note[0] = 0;
    ping_ms = -1;
    hello_room_sel = 0; hello_room_name[0] = 0; switching = 0; rooms_n = 0;
    sock_open();
    if (sock == RR_SOCK_BAD) { snprintf(note, sizeof note, "socket failed"); return false; }
    set_state(ST_CONNECTING);
    if (!exit_hook) { exit_hook = 1; atexit(leave_at_exit); }
    last_rx = now_ms();                          /* the connect attempt itself starts the silence clock */
    hello_seq = next_seq++;
    send_hello();
    return true;
}

static void send_leave(void)                     /* UDP: said twice, the server takes it idempotently */
{
    if (sock != RR_SOCK_BAD && (st == ST_LOBBY || st == ST_SESSION)) { send_msg(T_LEAVE, 0, NULL, 0); send_msg(T_LEAVE, 0, NULL, 0); }
}
static void leave_at_exit(void) { send_leave(); }   /* the game closing: free the slot now instead of after the 5 s timeout */
static int exit_hook;

bool rr_net_chat_send(const char *text)
{
    if (!text || (st != ST_LOBBY && st != ST_SESSION)) return false;
    char t[CHAT_TEXT + 1]; size_t n = 0;
    for (const char *c = text; *c && n < CHAT_TEXT; c++) if ((unsigned char)*c >= ' ' && (unsigned char)*c != 0x7F) t[n++] = *c;
    if (n > 0) {                                     /* a cut in the middle of a UTF-8 character drops that character (a complete one stays) */
        int k = (int)n - 1; while (k > 0 && ((unsigned char)t[k] & 0xC0) == 0x80) k--;
        const unsigned char lead = (unsigned char)t[k];
        if (lead >= 0xC0) { int need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : 2; if ((int)n - k < need) n = (size_t)k; }
    }
    while (n && t[n - 1] == ' ') n--;
    if (!n) return false;
    send_msg(T_CHAT, 0, t, (int)n);
    return true;
}

void rr_net_disconnect(void)
{
    if (sock != RR_SOCK_BAD) {
        send_leave();
        closesocket(sock); sock = RR_SOCK_BAD;
    }
    pend_ready = pend_start = 0;
    my_slot = -1; roster_n = 0; session_id = 0; ping_ms = -1; rooms_n = 0; switching = 0;
    note[0] = 0;
    set_state(ST_OFFLINE);
}

void rr_net_status(char *buf, size_t n)
{
    switch (st) {
    case ST_OFFLINE:    snprintf(buf, n, note[0] ? "Offline (%s)" : "Offline", note); break;
    case ST_CONNECTING: snprintf(buf, n, "Connecting to %s ...", srv_text); break;
    case ST_LOBBY:      snprintf(buf, n, "%sLobby slot %d, %d player(s), ping %d ms%s%s", rr_netd_running() ? "Hosting: " : "", my_slot, roster_n, ping_ms, note[0] ? " - " : "", note); break;
    case ST_SESSION:    snprintf(buf, n, "Race armed (session %08X), slot %d", session_id, my_slot); break;
    }
}
bool rr_net_connected(void) { return st == ST_LOBBY || st == ST_SESSION; }
bool rr_net_session_active(void) { return st == ST_SESSION; }

bool rr_net_roster(int i, char *name, size_t n, int *ready, int *self)
{
    if (i < 0 || i >= roster_n) return false;
    snprintf(name, n, "%s", roster[i].name);
    *ready = roster[i].ready;
    *self = roster[i].slot == my_slot;
    return true;
}
int rr_net_roster_count(void) { return roster_n; }

int rr_net_room_count(void) { return rooms_n; }
bool rr_net_room(int i, int *id, int *state, int *players, int *mine, char *name, size_t n)
{
    if (i < 0 || i >= rooms_n) return false;
    if (id) *id = rooms_[i].id;
    if (state) *state = rooms_[i].state;
    if (players) *players = rooms_[i].players;
    if (mine) *mine = rooms_[i].mine;
    if (name && n) snprintf(name, n, "%s", rooms_[i].name);
    return true;
}
/* leave this room and ask the server for another (id 1..254) or a brand new one (ROOM_NEW); a refusal lands us in a free lobby */
void rr_net_switch_room(int id)
{
    if (st != ST_LOBBY || sock == RR_SOCK_BAD) return;
    if (id < 1 || id > 255) return;
    send_leave();
    pend_ready = pend_start = 0; my_slot = -1; roster_n = 0; session_id = 0;
    hello_room_sel = id; switching = 1;
    hello_seq = next_seq++;
    last_rx = now_ms();
    rr_net_chat_clear();
    set_state(ST_CONNECTING);
    send_hello();
}
void rr_net_new_room(const char *name)
{
    snprintf(hello_room_name, sizeof hello_room_name, "%s", name ? name : "");
    rr_net_switch_room(ROOM_NEW);
}
void rr_net_delete_room(int id)                  /* only an empty, not racing room goes; the server answers with the fresh list */
{
    if ((st != ST_LOBBY && st != ST_SESSION) || sock == RR_SOCK_BAD || id < 1 || id > 254) return;
    uint8_t b = (uint8_t)id;
    send_msg(T_DELROOM, 0, &b, 1);
    rooms_tx = 0;                                /* ask for the list again right away */
}
bool rr_net_switching(void) { return switching && st == ST_CONNECTING; }

void rr_net_set_ready(int ready)
{
    if (st != ST_LOBBY) return;
    if (debug == 1) fprintf(stderr, "[NET] ready -> %d\n", ready);
    ready_val = ready ? 1 : 0;
    pend_ready = 1;
    ready_seq = next_seq++;
    uint8_t b = (uint8_t)ready_val;
    send_msg(T_READY, ready_seq, &b, 1);
    ready_tx = now_ms();
}

static bool all_ready(void)
{
    if (roster_n < 2) return false;
    for (int i = 0; i < roster_n; i++) if (!roster[i].ready) return false;
    return true;
}
void rr_net_request_start(void)
{
    if (st != ST_LOBBY) return;
    if (!all_ready()) { snprintf(note, sizeof note, "waiting: everyone must be Ready"); if (debug == 1) fprintf(stderr, "[NET] start refused: not everyone is Ready (%d in roster)\n", roster_n); return; }
    note[0] = 0;
    pend_start = 1;
    start_seq = next_seq++;
    send_msg(T_START, start_seq, NULL, 0);
    start_tx = now_ms();
}

/* roster := u8 count; entries { u8 slot, u8 ready, u8 name_len, char name[] } */
static int parse_roster(const uint8_t *p, int len, int off)
{
    if (off >= len) return -1;
    int count = p[off++];
    if (count > 8) return -1;
    int n = 0;
    for (int i = 0; i < count; i++) {
        if (off + 3 > len) return -1;
        uint8_t slot = p[off], rdy = p[off + 1], nl = p[off + 2]; off += 3;
        if (off + nl > len) return -1;
        if (slot < 8) {
            int m = nl > 16 ? 16 : nl;           /* truncate long names (the contract) */
            roster[n].slot = slot; roster[n].ready = rdy ? 1 : 0;
            memcpy(roster[n].name, p + off, (size_t)m); roster[n].name[m] = 0;
            n++;
        }
        off += nl;
    }
    return n;
}

static uint32_t rd32(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static void roster_update(const uint8_t *p, int len, int off)
{
    int n = parse_roster(p, len, off);
    if (n < 0) return;
    roster_n = n;
    n_roster++;
    /* a ROSTER showing our new flag is READY's acknowledgment */
    for (int i = 0; i < n; i++)
        if (roster[i].slot == my_slot && pend_ready && roster[i].ready == ready_val) pend_ready = 0;
    if (debug == 1) {
        fprintf(stderr, "[NET] roster %d:", n);
        for (int i = 0; i < n; i++) fprintf(stderr, "  %d:%s%s", roster[i].slot, roster[i].name, roster[i].ready ? "*" : "");
        fprintf(stderr, "\n");
    }
}

static void on_go(uint16_t seq, const uint8_t *p, int len)
{
    if (st != ST_LOBBY && st != ST_SESSION) return;
    if (len < 5) return;
    uint32_t sid = rd32(p);
    uint8_t ack[2] = { (uint8_t)seq, (uint8_t)(seq >> 8) };
    send_msg(T_ACK, 0, ack, 2);                  /* the server retransmits GO until this */
    if (st == ST_SESSION && sid == session_id) return;   /* a retransmit: acked, done */
    int n = parse_roster(p, len, 4);
    if (n < 0) return;
    roster_n = n;
    int found = 0;
    for (int i = 0; i < n; i++) if (roster[i].slot == my_slot) found = 1;
    if (!found) { fail("not in the GO roster"); return; }
    session_id = sid;
    frame_seq = 0;
    if (!rr_link_net_legacy()) { rr_link_pkt_t junk; rr_link_tx_pop_latest(&junk); }   /* packets staged before the race: never sent */
    for (int i = 0; i < 8; i++) last_fseq[i] = 0xFFFFFFFFu;
    pend_start = pend_ready = 0;
    /* the lobby slot IS the cabinet number; the EEPROM patch wins only after the
     * boot-time settings reload (~frame 300), so keep re-poking for a while */
    rr_hw_set_link_cabinet(my_slot);
    cab_reapply = 360;
    set_state(ST_SESSION);
    session_free_play(true);
    wait_left = GAS_WAIT_FRAMES; gas_left = 0;
    fprintf(stderr, "[NET] GO: session %08X, slot %d, %d players\n", sid, my_slot, n);
}

static void on_frame(const uint8_t *p, int len)
{
    if (st != ST_SESSION || len != 49) return;
    if (rd32(p) != session_id) return;                     /* another session's traffic */
    uint32_t fs = rd32(p + 4);
    uint8_t cab = p[8];
    if (cab >= 8 || cab == my_slot) return;
    if (last_fseq[cab] != 0xFFFFFFFFu && fs <= last_fseq[cab]) return;   /* dedupe / reorder guard */
    last_fseq[cab] = fs;
    /* the 40 wire bytes are the 38-byte game payload + 2 zero fill bytes */
    rr_link_pkt_t pkt;
    memcpy(pkt.data, p + 9, RR_LINK_PKT_LEN);
    pkt.id = cab;
    pkt.data[0] = 0; pkt.data[1] = cab;                    /* the receive path's sender check */
    rr_link_rx_push(&pkt);
    n_frx++;
}

static void on_message(int type, uint16_t seq, const uint8_t *p, int len)
{
    switch (type) {
    case T_WELCOME:
        if ((st == ST_LOBBY || st == ST_SESSION) && len >= 6 && p[0] == 0xFF) {
            /* the server does not know us: it restarted, or timed us out (a long stall). Rejoin; a race that is
             * running turns the HELLO away, and that is reported as the reason we are offline. */
            fprintf(stderr, "[NET] the server dropped us (%s): rejoining\n", st == ST_SESSION ? "mid race" : "in the lobby");
            const int was = st == ST_SESSION;
            if (sock != RR_SOCK_BAD) { closesocket(sock); sock = RR_SOCK_BAD; }
            pend_ready = pend_start = 0; my_slot = -1; roster_n = 0; session_id = 0;
            set_state(ST_OFFLINE);
            rr_ui_set_hint(was ? "Online: connection to the server was lost mid race - rejoining" : "Online: server restarted - rejoining", 240);
            rr_net_connect();
            return;
        }
        if (st != ST_CONNECTING || seq != hello_seq || len < 6) return;
        if (p[0] == 0xFF) {
            if (switching) {                               /* that room is racing / full / gone: ask the server for a lobby instead of dropping out */
                switching = 0; hello_room_sel = 0; hello_seq = next_seq++;
                chat_push(p[1] == 1 ? "* that room is racing: you are in a free lobby instead" : "* that room is full or gone: you are in a free lobby instead");
                send_hello();
                return;
            }
            fail(p[1] == 1 ? "race in progress, try later" : "lobby full"); return;
        }
        if (p[0] >= 8) return;
        switching = 0;
        my_slot = p[0];
        roster_update(p, len, 6);
        fprintf(stderr, "[NET] joined %s as slot %d ('%s')\n", srv_text, my_slot, my_name);
        set_state(ST_LOBBY);
        break;
    case T_ROSTER:
        if (len < 5) return;
        if (st == ST_SESSION && p[0] == 0) {               /* the server ended the session */
            session_id = 0;
            set_state(ST_LOBBY);
            rr_ui_set_hint("Online: the race is over - back in the lobby", 300);
            chat_push("* the race session ended: back in the lobby");
        }
        if (st == ST_LOBBY || st == ST_SESSION) roster_update(p, len, 5);
        break;
    case T_CHAT:
        if (len >= 2 && (st == ST_LOBBY || st == ST_SESSION)) {
            char line[17 + 2 + CHAT_TEXT + 1], who[24] = "";
            for (int i = 0; i < roster_n; i++) if (roster[i].slot == p[0]) snprintf(who, sizeof who, "%s", roster[i].name);
            if (!who[0]) snprintf(who, sizeof who, "P%d", p[0]);
            int n = len - 1; if (n > CHAT_TEXT) n = CHAT_TEXT;
            char tx[CHAT_TEXT + 1]; int k = 0;
            for (int i = 0; i < n; i++) if (p[1 + i] >= ' ' && p[1 + i] != 0x7F) tx[k++] = (char)p[1 + i];
            tx[k] = 0;
            snprintf(line, sizeof line, "%s: %s", who, tx);
            chat_push(line);
            if (debug == 1) fprintf(stderr, "[NET] chat: %s\n", line);
        }
        break;
    case T_ROOMLIST:
        if (len >= 1 && (st == ST_LOBBY || st == ST_SESSION)) {
            int n = p[0] > MAX_ROOMS ? MAX_ROOMS : p[0], off = 1, k = 0;
            for (int i = 0; i < n; i++) {
                if (off + 5 > len) break;
                const int nl = p[off + 4];
                if (off + 5 + nl > len) break;
                rooms_[k].id = p[off]; rooms_[k].state = p[off + 1]; rooms_[k].players = p[off + 2]; rooms_[k].mine = p[off + 3];
                const int m = nl > 31 ? 31 : nl; memcpy(rooms_[k].name, p + off + 5, (size_t)m); rooms_[k].name[m] = 0;
                k++; off += 5 + nl;
            }
            rooms_n = k;
            if (debug == 1) { fprintf(stderr, "[NET] rooms:"); for (int i = 0; i < k; i++) fprintf(stderr, "  #%d '%s' %d/8 %s%s", rooms_[i].id, rooms_[i].name, rooms_[i].players, rooms_[i].state ? "racing" : "lobby", rooms_[i].mine ? " (mine)" : ""); fprintf(stderr, "\n"); }
        }
        break;
    case T_GO: on_go(seq, p, len); break;
    case T_FRAME: on_frame(p, len); break;
    case T_PONG:
        if (len >= 4) ping_ms = (int)(now_ms() - rd32(p));
        break;
    default: break;                                        /* unknown types: ignore (the contract) */
    }
}

static void send_frame(void)
{
    rr_link_pkt_t p;
    if (!(rr_link_net_legacy() ? rr_link_tx_pop(&p) : rr_link_tx_pop_latest(&p))) return;   /* RR_NET_LEGACY=1: the oldest, as before (A/B) */              /* the newest: a backlog was a constant ~50 ms of delay */
    uint8_t pl[49];
    pl[0] = (uint8_t)session_id; pl[1] = (uint8_t)(session_id >> 8);
    pl[2] = (uint8_t)(session_id >> 16); pl[3] = (uint8_t)(session_id >> 24);
    pl[4] = (uint8_t)frame_seq; pl[5] = (uint8_t)(frame_seq >> 8);
    pl[6] = (uint8_t)(frame_seq >> 16); pl[7] = (uint8_t)(frame_seq >> 24);
    if (debug == 1 && p.id != my_slot && n_ftx % 60 == 0) fprintf(stderr, "[NET] staged id %d != slot %d\n", p.id, my_slot);
    pl[8] = p.id;                                            /* our cabinet number = our slot */
    memcpy(pl + 9, p.data, RR_LINK_PKT_LEN);
    pl[9 + RR_LINK_PKT_LEN] = pl[10 + RR_LINK_PKT_LEN] = 0;  /* 38 + 2 zero fill = 40 */
    frame_seq++;
    send_msg(T_FRAME, 0, pl, 49);
    last_frame_tx = now_ms();
    n_ftx++;
}

static void handle_raw(const uint8_t *b, int n, uint32_t now)
{
    if (n < 10 || memcmp(b, "RRN1", 4)) return;
    int len = b[8] | b[9] << 8;
    if (len != n - 10) return;                             /* bad length: drop (the contract) */
    last_rx = now;
    on_message(b[4], (uint16_t)(b[6] | b[7] << 8), b + 10, len);
}

static void send_ping(uint32_t now)
{
    uint8_t pl[4] = { (uint8_t)now, (uint8_t)(now >> 8), (uint8_t)(now >> 16), (uint8_t)(now >> 24) };
    send_msg(T_PING, 0, pl, 4);
    last_ping_tx = now;
}


/* ---- LAN discovery --------------------------------------------------------
 * DISCOVER is broadcast on the LAN (limited broadcast, every interface's directed
 * broadcast where the OS lists them, and loopback for a host on this machine);
 * every host answers ANNOUNCE { state, players, name }. Results are kept until the
 * next search. Runs for 4 s (resent each second), on its own socket. */
#define DISC_MS 4000
static rr_sock_t dsock = RR_SOCK_BAD;
static uint32_t d_start, d_tx; static int d_auto;
static struct { struct sockaddr_storage a; char addr[64], name[17]; uint8_t state, players; } found[8];
static int found_n;

static void disc_send_one(uint32_t ip_be)
{
    uint8_t b[10]; memcpy(b, "RRN1", 4); b[4] = T_DISCOVER; b[5] = 0; b[6] = b[7] = b[8] = b[9] = 0;
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_addr.s_addr = ip_be; a.sin_port = htons(RR_NET_PORT);
    sendto(dsock, (const char *)b, sizeof b, 0, (struct sockaddr *)&a, sizeof a);
}
static void disc_send(void)
{
    disc_send_one(htonl(INADDR_BROADCAST));
    disc_send_one(htonl(INADDR_LOOPBACK));                       /* a host on this machine */
#ifndef _WIN32
    struct ifaddrs *ifs = NULL;
    if (getifaddrs(&ifs) == 0) {
        for (struct ifaddrs *i = ifs; i; i = i->ifa_next)
            if (i->ifa_addr && i->ifa_broadaddr && i->ifa_addr->sa_family == AF_INET && (i->ifa_flags & IFF_BROADCAST) && (i->ifa_flags & IFF_UP))
                disc_send_one(((struct sockaddr_in *)i->ifa_broadaddr)->sin_addr.s_addr);
        freeifaddrs(ifs);
    }
#endif
    d_tx = now_ms();
}

void rr_net_discover(void)
{
    rr_sock_init();
    if (dsock != RR_SOCK_BAD) { closesocket(dsock); dsock = RR_SOCK_BAD; }
    dsock = socket(AF_INET, SOCK_DGRAM, 0);
    if (dsock == RR_SOCK_BAD) { snprintf(note, sizeof note, "socket failed"); return; }
    int on = 1; setsockopt(dsock, SOL_SOCKET, SO_BROADCAST, (const char *)&on, sizeof on);
    rr_sock_nonblock(dsock);
    found_n = 0;
    d_start = now_ms();
    disc_send();
}
bool rr_net_discovering(void) { return dsock != RR_SOCK_BAD; }
void rr_net_discover_autojoin(int on) { d_auto = on; }
int rr_net_found_count(void) { return found_n; }
bool rr_net_found(int i, char *label, size_t n, char *addr, size_t an)
{
    if (i < 0 || i >= found_n) return false;
    snprintf(label, n, "%s  %s  %d player(s)%s", found[i].name, found[i].addr, found[i].players, found[i].state ? "  [race running]" : "");
    if (addr) snprintf(addr, an, "%s", found[i].addr);
    return true;
}

static void disc_poll(void)
{
    if (dsock == RR_SOCK_BAD) return;
    uint8_t b[512];
    for (int k = 0; k < 16; k++) {
        struct sockaddr_storage from; socklen_t fl = sizeof from;
        int n = (int)recvfrom(dsock, (char *)b, sizeof b, 0, (struct sockaddr *)&from, &fl);
        if (n < 0) break;
        if (n < 13 || memcmp(b, "RRN1", 4) || b[4] != T_ANNOUNCE) continue;
        if ((b[8] | b[9] << 8) != n - 10) continue;
        const uint8_t *p = b + 10; int nl = p[2]; if (n - 13 < nl) continue;
        int idx = -1;
        for (int i = 0; i < found_n; i++) if (rr_addr_eq(&found[i].a, &from)) idx = i;
        if (idx < 0) { if (found_n >= 8) continue; idx = found_n++; }
        found[idx].a = from;
        char host[64] = "?"; getnameinfo((struct sockaddr *)&from, fl, host, sizeof host, NULL, 0, NI_NUMERICHOST);
        uint16_t port = from.ss_family == AF_INET ? ntohs(((struct sockaddr_in *)&from)->sin_port) : RR_NET_PORT;
        snprintf(found[idx].addr, sizeof found[idx].addr, "%s:%u", host, port);
        found[idx].state = p[0]; found[idx].players = p[1];
        if (nl > 16) nl = 16;
        int m = 0; for (int i = 0; i < nl; i++) if (p[3 + i] >= ' ' && p[3 + i] != 0x7F) found[idx].name[m++] = (char)p[3 + i];
        found[idx].name[m] = 0;
        if (debug == 1) fprintf(stderr, "[NET] found %s '%s' %d player(s)\n", found[idx].addr, found[idx].name, found[idx].players);
    }
    uint32_t now = now_ms();
    if (now - d_tx >= 1000 && now - d_start < DISC_MS) disc_send();
    if (now - d_start >= DISC_MS) { closesocket(dsock); dsock = RR_SOCK_BAD; }
    if (d_auto && found_n > 0 && st == ST_OFFLINE && !found[0].state) {     /* headless test: join the first host found */
        d_auto = 0;
        if (rr_net_set_server(found[0].addr)) rr_net_connect();
    }
}

/* ---- hosting: the built-in server, and this client joined to it over loopback ---- */
bool rr_net_host_start(void)
{
    if (rr_netd_running()) return true;
    if (st != ST_OFFLINE) rr_net_disconnect();
    if (!rr_netd_start(RR_NET_PORT)) { snprintf(note, sizeof note, "cannot host: port %d is busy", RR_NET_PORT); return false; }
    rr_netd_set_name(my_name);
    if (!rr_net_set_server("127.0.0.1")) { rr_netd_stop(); return false; }
    return rr_net_connect();
}
void rr_net_host_stop(void) { rr_net_disconnect(); rr_netd_stop(); }
bool rr_net_hosting(void) { return rr_netd_running(); }

static int poll_paused;                          /* rr_net_poll_paused: the game is frozen (menu) */
void rr_net_poll_paused(void) { poll_paused = 1; rr_net_poll(); poll_paused = 0; }

/* ---- RR_NET_SIM=loss%,spike_ms[,burst]: a bad Wi-Fi link, on the receive side (tests only) ----
 * each received message is dropped with probability loss% (after a drop the next burst-1 are dropped too);
 * and about every 2 s (at random) the link STALLS for spike_ms/2..spike_ms: everything received meanwhile is
 * held and then arrives at once, in order -- what a Wi-Fi retry storm, a background scan or a power-save wake
 * looks like from the game. Independent random delays per packet would mostly reorder them instead, which
 * is not what Wi-Fi does. Off unless the variable is set. */
static int sim_loss = -1, sim_jit, sim_burst, sim_left;
static uint32_t sim_rng = 12345, sim_hold_until, sim_last;
#define SIM_Q 256
static struct { uint32_t at; int n; uint8_t b[512]; } simq[SIM_Q];
static int simq_n;
static void sim_reset(void) { simq_n = 0; sim_left = 0; }
static uint32_t sim_rand(void) { sim_rng = sim_rng * 1103515245u + 12345u; return sim_rng >> 8; }
static void sim_init(void)
{
    sim_loss = 0;
    const char *e = getenv("RR_NET_SIM");
    if (!e || !*e) return;
    sim_burst = 1;
    if (sscanf(e, "%d,%d,%d", &sim_loss, &sim_jit, &sim_burst) < 1) sim_loss = 0;
    if (sim_burst < 1) sim_burst = 1;
    const char *s = getenv("RR_NET_SIM_SEED"); if (s) sim_rng = (uint32_t)strtoul(s, NULL, 0);
    fprintf(stderr, "[NET] RR_NET_SIM: loss %d%%, stalls of %d ms (~1 per 2 s), burst %d\n", sim_loss, sim_jit, sim_burst);
}
static bool sim_on(void) { if (sim_loss < 0) sim_init(); return sim_loss > 0 || sim_jit > 0; }
static void handle_raw(const uint8_t *b, int n, uint32_t now);

void rr_net_poll(void)
{
    if (debug < 0) { const char *e = getenv("RR_NET_DEBUG"); debug = e && *e == '1'; }
    if (autostart < 0) { const char *e = getenv("RR_NET_AUTOSTART"); autostart = e ? atoi(e) : 0; }   /* 1 = start once 2+ are ready; N>=2 = wait for N players */
    rr_netd_poll();                                        /* the built-in host (inert unless hosting) */
    disc_poll();                                           /* LAN search (inert unless searching) */
    if (st == ST_OFFLINE) return;                          /* inert: no client socket, no syscalls */
    uint32_t now = now_ms();
    { static int say_lobby, say_race; static uint32_t say_t;     /* headless test: RR_NET_SAY=text sends one line in the lobby and one in the race */
      static const char *say; static int say_init;
      if (!say_init) { say_init = 1; say = getenv("RR_NET_SAY"); }
      if (say && *say) {
          if (st != ST_LOBBY && st != ST_SESSION) say_t = now;
          else if (now - say_t > 1500) {
              if (st == ST_LOBBY && !say_lobby) { say_lobby = 1; rr_net_chat_send(say); }
              if (st == ST_SESSION && !say_race) { say_race = 1; rr_net_chat_send(say); }
          }
      } }

    { static int t_init, t_room_done, t_name_done; static const char *t_room, *t_name;      /* headless tests: RR_NET_ROOM=new|<id> and RR_NET_RENAME=<name>, 1.5 s into the lobby */
      if (!t_init) { t_init = 1; t_room = getenv("RR_NET_ROOM"); t_name = getenv("RR_NET_RENAME"); }
      if (st == ST_LOBBY && now_ms() - t_state > 1500) {
          if (t_room && *t_room && !t_room_done) { t_room_done = 1; if (!strncmp(t_room, "new", 3)) rr_net_new_room(t_room[3] == ':' ? t_room + 4 : NULL); else rr_net_switch_room(atoi(t_room)); }
          else if (t_name && *t_name && !t_name_done) { t_name_done = 1; rr_net_set_name(t_name); }
      } }
    uint8_t b[512];
    for (int i = 0; i < 32; i++) {                         /* bounded: never stall the frame */
        if (sock == RR_SOCK_BAD) break;                    /* a message just failed the connection (e.g. "race in progress"): keep ITS reason, do not read a closed socket */
        struct sockaddr_storage from; socklen_t fl = sizeof from;
        int n = (int)recvfrom(sock, (char *)b, sizeof b, 0, (struct sockaddr *)&from, &fl);
        if (n < 0) {
            if (!rr_sock_would_block()) { if (st != ST_SESSION) fail("socket error"); }
            break;
        }
        if (!from_server(&from)) continue;                 /* not the server: drop */
        if (sim_on()) {                                    /* RR_NET_SIM: lose it, or hold it a while */
            if (sim_left > 0) { sim_left--; continue; }
            if ((int)(sim_rand() % 100) < sim_loss) { sim_left = sim_burst - 1; continue; }
            if (sim_jit && (int32_t)(now - sim_hold_until) >= 0 && now != sim_last && sim_rand() % 2000 < (now - sim_last > 100 ? 100 : now - sim_last))
                sim_hold_until = now + (uint32_t)sim_jit / 2 + sim_rand() % ((uint32_t)sim_jit / 2 + 1);   /* a stall starts (~1 per 2 s) */
            sim_last = now;
            if (simq_n < SIM_Q && n <= (int)sizeof simq[0].b) {
                simq[simq_n].at = (int32_t)(sim_hold_until - now) > 0 ? sim_hold_until : now;
                simq[simq_n].n = n; memcpy(simq[simq_n].b, b, (size_t)n); simq_n++;
            }
            continue;
        }
        handle_raw(b, n, now);
    }
    while (simq_n > 0 && (int32_t)(now - simq[0].at) >= 0 && sock != RR_SOCK_BAD) {   /* RR_NET_SIM: the held messages, in order */
        uint8_t t[512]; const int n = simq[0].n; memcpy(t, simq[0].b, (size_t)n);
        memmove(&simq[0], &simq[1], (size_t)(--simq_n) * sizeof simq[0]);
        handle_raw(t, n, now);
    }

    switch (st) {
    case ST_CONNECTING:
        if (now - hello_tx >= RETRY_MS) send_hello();      /* the WELCOME's seq matches hello_seq */
        if (now - last_rx > DEAD_MS) fail("no response from server");
        break;
    case ST_LOBBY:
        if (pend_ready && now - ready_tx >= RETRY_MS) { uint8_t v = (uint8_t)ready_val; send_msg(T_READY, ready_seq, &v, 1); ready_tx = now; }
        if (pend_start && now - start_tx >= RETRY_MS) { send_msg(T_START, start_seq, NULL, 0); start_tx = now; }
        if (now - last_ping_tx >= PING_MS) send_ping(now);
        if (now - rooms_tx >= 2000) { send_msg(T_ROOMS, 0, NULL, 0); rooms_tx = now; }   /* the room list, kept fresh while the lobby is on screen */
        if (now - last_rx > DEAD_MS) { fail("connection lost"); break; }
        if (autostart) {                                   /* headless test: start once the lobby has settled at 2+ */
            if (roster_n >= (autostart > 1 ? autostart : 2)) {
                if (!autostart_t0) autostart_t0 = now;
                else if (now - autostart_t0 >= 2000 && !pend_start) { for (int i = 0; i < roster_n; i++) if (roster[i].slot == my_slot && !roster[i].ready && !pend_ready) rr_net_set_ready(1); if (all_ready()) rr_net_request_start(); }
            } else autostart_t0 = 0;
        }
        break;
    case ST_SESSION:
        if (!poll_paused) {                                /* the game is running: our car's packet */
            if (cab_reapply > 0) { rr_hw_set_link_cabinet(my_slot); cab_reapply--; }
            send_frame();                                  /* the newest staged link packet, once per frame */
        }
        if (now - last_ping_tx >= PING_MS)
            send_ping(now);                                /* no car to report: PING is the liveness fallback */
        /* a dead server mid-race is NOT fatal: the game's own 8-frame peer
         * timeout drops the rivals and the race plays out alone */
        break;
    }

    if (debug == 1 && ++n_poll % 60 == 0)
        fprintf(stderr, "[NET] %s slot %d  frames tx %u rx %u  roster %u  ping %d ms\n",
                st_name[st], my_slot, n_ftx, n_frx, n_roster, ping_ms);
}
