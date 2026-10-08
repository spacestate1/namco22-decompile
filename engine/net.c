/*
 * net.c -- the NMN2 online-play client every game shares (engine/net.h, docs/NETPLAY.md). Grown out of Rave Racer's RRN1
 * client (raverace/src/rr_net.c, 2026-09/10): the state machine, the timings and the debug lines are that client's, made
 * game-independent (game id, link version, room size and payload size come from the game's eng_net_game; the game's link
 * is reached only through its hooks).
 *
 * OFFLINE -> CONNECTING (HELLO, 500 ms retry) -> LOBBY (roster, READY/START, PING 1 Hz, the room list every 2 s) -> SESSION
 * (GO: the lobby slot is this cabinet's link number; the game's payload goes out as one FRAME per poll, the newest the game
 * has; peers' FRAMEs reach the game through frame_in, deduplicated per slot). A server that forgets us (restart, timeout)
 * answers our PING with the short rejection WELCOME: we rejoin by ourselves.
 *
 * Rave Racer (game->rrn1) also speaks the old RRN1 protocol: if no NMN2 answer has come after 1.5 s the HELLO is sent in both
 * protocols and whichever WELCOME arrives fixes the protocol for the connection -- an old rrn1-server or an old build's
 * built-in host still works. A new nmn-server answers the NMN2 HELLO first and bridges to RRN1 members itself.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <time.h>
#include <SDL2/SDL.h>           /* SDL_GetTicks (lazy-inits the timer: safe headless) */
#include "net_sock.h"
#include "net.h"
#include "net_host.h"

#define RETRY_MS    500          /* reliable client messages (HELLO/READY/START) */
#define PING_MS     1000         /* lobby keepalive, and the session fallback */
#define DEAD_MS     10000        /* server silence in connecting/lobby = lost */
#define RRN1_AFTER_MS 1500       /* rrn1 games: no NMN2 answer by then -> also say HELLO in RRN1 */
#define HELLO_PAD   128          /* an NMN2 HELLO is padded to this: the server's anti-amplification credit for our WELCOME */
#define BUF_MAX     (16 + 9 + ENG_NET_MAX_FRAME + 64)

enum { ST_OFFLINE, ST_CONNECTING, ST_LOBBY, ST_SESSION };
static const char *st_name[] = { "OFFLINE", "CONNECTING", "LOBBY", "SESSION" };

enum { T_HELLO = 0x01, T_WELCOME, T_ROSTER, T_READY, T_START, T_GO, T_LEAVE,
       T_FRAME, T_PING, T_PONG, T_ACK, T_DISCOVER, T_ANNOUNCE, T_CHAT,
       T_ROOMS, T_ROOMLIST, T_RENAME, T_DELROOM };
enum { M_AUTO = 0, M_JOIN = 1, M_CREATE = 2 };          /* NMN2 HELLO modes */
#define ROOM_NEW 255                                    /* RRN1 HELLO selector: a new room */

static const eng_net_game *G;
static eng_net_game g_none = { "zz", 1, 2, 1, "ENG_NET", "game", 0, NULL, NULL, NULL, NULL, NULL };

static void leave_at_exit(void);
static int exit_hook, srv_pending;               /* srv_pending: srv_text is set but not resolved yet */

static const char *envs(const char *suffix)      /* <prefix>_<suffix> */
{
    char k[64]; snprintf(k, sizeof k, "%s_%s", G->env ? G->env : "ENG_NET", suffix);
    return getenv(k);
}
static void hint(const char *t, int frames) { if (G->hint) G->hint(t, frames); }

/* ---- chat: the last lines, oldest first; CHAT is relayed by the server to every member, the sender included ---- */
#define CHAT_LINES 64
#define CHAT_TEXT 96
static char chat_buf[CHAT_LINES][8 + 24 + 2 + CHAT_TEXT + 1];      /* "[HH:MM] " + "Name: text" */
static int chat_n, chat_head;
static uint32_t chat_serial;
static void chat_push(const char *line)
{
    int i = (chat_head + chat_n) % CHAT_LINES;
    if (chat_n == CHAT_LINES) { chat_head = (chat_head + 1) % CHAT_LINES; i = (chat_head + CHAT_LINES - 1) % CHAT_LINES; } else chat_n++;
    time_t t = time(NULL); struct tm *tm = localtime(&t);
    if (tm) snprintf(chat_buf[i], sizeof chat_buf[i], "[%02d:%02d] %s", tm->tm_hour, tm->tm_min, line);
    else snprintf(chat_buf[i], sizeof chat_buf[i], "%s", line);
    chat_serial++;
}
static void chat_pushf(const char *fmt, ...)
{
    char b[160]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    chat_push(b);
}
int eng_net_chat_count(void) { return chat_n; }
uint32_t eng_net_chat_serial(void) { return chat_serial; }
bool eng_net_chat_line(int i, char *out, size_t n)
{
    if (i < 0 || i >= chat_n) return false;
    snprintf(out, n, "%s", chat_buf[(chat_head + i) % CHAT_LINES]);
    return true;
}
void eng_net_chat_clear(void) { chat_n = chat_head = 0; }

static int st = ST_OFFLINE;
static int proto = 2;                            /* the connection's protocol: 2 = NMN2, 1 = RRN1 (rrn1 games, old servers) */
static eng_sock_t sock = ENG_SOCK_BAD;
static struct sockaddr_storage srv; static socklen_t srv_len;
static char srv_text[128];
static char my_name[24] = "PLAYER";
static char note[64];                            /* why we are offline, or a lobby remark */
static uint16_t next_seq = 1;
static int my_slot = -1;
static uint16_t my_room;                         /* NMN2: the room we are in (the header's room), 0 = none yet */
static uint32_t session_id, frame_seq;
static uint32_t last_fseq[ENG_NET_MAX_PLAYERS];
static struct { uint8_t slot, ready; char name[17]; } roster[ENG_NET_MAX_PLAYERS];
static int roster_n;
static uint32_t t_state, t_connect, last_rx, last_ping_tx;
static uint32_t hello_tx; static uint16_t hello_seq;
static int pend_ready, ready_val; static uint16_t ready_seq; static uint32_t ready_tx;
static int pend_start; static uint16_t start_seq; static uint32_t start_tx;
static int ping_ms = -1;
static int autostart = -1, debug = -1; static uint32_t autostart_t0;
static uint32_t n_ftx, n_frx, n_roster, n_poll;
static uint8_t last_frame[9 + ENG_NET_MAX_FRAME]; static int have_last_frame;   /* our newest FRAME payload (stall-and-wait resends it) */

static uint32_t now_ms(void) { return SDL_GetTicks(); }

void eng_net_init(const eng_net_game *g)
{
    G = g ? g : &g_none;
    if (G->frame_size > ENG_NET_MAX_FRAME) fprintf(stderr, "[NET] frame_size %u > %d: online play disabled\n", G->frame_size, ENG_NET_MAX_FRAME);
}
const eng_net_game *eng_net_game_info(void) { return G; }
static int usable(void) { if (!G) G = &g_none; return G->frame_size > 0 && G->frame_size <= ENG_NET_MAX_FRAME; }

static void set_state(int ns)
{
    if (ns == st) return;
    const int was = st;
    if (debug == 1) fprintf(stderr, "[NET] %s -> %s\n", st_name[st], st_name[ns]);
    st = ns;
    t_state = now_ms();
    if (was == ST_SESSION && G->session_end) G->session_end();
}

static int put_hdr(uint8_t *b, int p, int type, uint16_t seq, uint16_t room, int len)
{
    memcpy(b, p == 1 ? "RRN1" : "NMN2", 4);
    b[4] = (uint8_t)type; b[5] = 0;
    b[6] = (uint8_t)seq; b[7] = (uint8_t)(seq >> 8);
    b[8] = (uint8_t)len; b[9] = (uint8_t)(len >> 8);
    if (p == 1) return 10;
    b[10] = (uint8_t)G->id[0]; b[11] = (uint8_t)G->id[1];
    b[12] = (uint8_t)G->ver; b[13] = (uint8_t)(G->ver >> 8);
    b[14] = (uint8_t)room; b[15] = (uint8_t)(room >> 8);
    return 16;
}
static void send_raw_p(int p, int type, uint16_t seq, uint16_t room, const void *pl, int len)
{
    uint8_t b[BUF_MAX];
    if (len < 0 || len > (int)sizeof b - 16 || sock == ENG_SOCK_BAD) return;
    int h = put_hdr(b, p, type, seq, room, len);
    if (len) memcpy(b + h, pl, (size_t)len);
    sendto(sock, (const char *)b, h + len, 0, (struct sockaddr *)&srv, srv_len);
}
static void send_msg(int type, uint16_t seq, const void *pl, int len) { send_raw_p(proto, type, seq, my_room, pl, len); }

/* ---- rooms: the list the server last sent, and the room the next HELLO asks for ---- */
#define MAX_ROOMS 32
static struct { uint16_t id, ver; uint8_t state, players, maxp, mine; char name[32]; } rooms_[MAX_ROOMS];
static int rooms_n;
static uint32_t rooms_tx;
static char hello_room_name[32];
static int hello_room_sel;                       /* 0 = the server picks, 1..65534 = that room, -1 = a new room */
static int switching;

static int name_bytes(uint8_t *o, const char *s, int max)       /* printable bytes, at most max; "?" if none */
{
    int n = 0;
    for (const char *c = s; *c && n < max; c++) if ((unsigned char)*c >= ' ' && (unsigned char)*c != 0x7F) o[n++] = (uint8_t)*c;
    if (!n) o[n++] = '?';
    return n;
}

static void send_hello_p(int p)
{
    uint8_t pl[160]; int len;
    int n = name_bytes(pl + 1, my_name, 16); pl[0] = (uint8_t)n; len = 1 + n;
    if (p == 1) {                                /* RRN1: [name] [selector [len name]] */
        int sel = hello_room_sel < 0 ? ROOM_NEW : hello_room_sel;
        if (sel > 254) sel = 0;
        if (sel) pl[len++] = (uint8_t)sel;
        if (sel == ROOM_NEW && hello_room_name[0]) {
            int k = 0;
            for (const char *c = hello_room_name; *c && k < 24; c++) if ((unsigned char)*c >= ' ' && (unsigned char)*c != 0x7F) pl[len + 1 + k++] = (uint8_t)*c;
            pl[len] = (uint8_t)k; len += 1 + k;
        }
        send_raw_p(1, T_HELLO, hello_seq, 0, pl, len);
    } else {                                     /* NMN2: [name] mode max_players frame_size [label] padding */
        pl[len++] = (uint8_t)(hello_room_sel < 0 ? M_CREATE : hello_room_sel > 0 ? M_JOIN : M_AUTO);
        pl[len++] = G->max_players;
        pl[len++] = (uint8_t)G->frame_size; pl[len++] = (uint8_t)(G->frame_size >> 8);
        int k = 0;
        if (hello_room_sel < 0)
            for (const char *c = hello_room_name; *c && k < 24; c++) if ((unsigned char)*c >= ' ' && (unsigned char)*c != 0x7F) pl[len + 1 + k++] = (uint8_t)*c;
        pl[len] = (uint8_t)k; len += 1 + k;
        while (len < HELLO_PAD) pl[len++] = 0;
        send_raw_p(2, T_HELLO, hello_seq, (uint16_t)(hello_room_sel > 0 ? hello_room_sel : 0), pl, len);
    }
}
static void send_hello(void)
{
    if (st == ST_CONNECTING && !switching && G->rrn1 && proto == 2) {
        send_hello_p(2);                                                /* both, once NMN2 has had its chance */
        if (now_ms() - t_connect >= RRN1_AFTER_MS) send_hello_p(1);
    } else send_hello_p(proto);
    hello_tx = now_ms();
}

/* datagrams count only from the server we talk to: the socket is unconnected, so without this anyone who can reach the
 * port could send GO/ROSTER/FRAME */
static int from_server(const struct sockaddr_storage *a) { return eng_addr_eq(a, &srv); }

static void sim_reset(void);
static void sock_open(void)
{
    sim_reset();
    eng_sock_init();
    sock = socket(srv.ss_family, SOCK_DGRAM, 0);
    if (sock == ENG_SOCK_BAD) return;
    eng_sock_nonblock(sock);
}

static void fail(const char *why)                /* lost/unreachable: close, OFFLINE with the reason */
{
    if (debug == 1) fprintf(stderr, "[NET] fail: %s\n", why);
    snprintf(note, sizeof note, "%s", why);
    if (sock != ENG_SOCK_BAD) { closesocket(sock); sock = ENG_SOCK_BAD; }
    pend_ready = pend_start = 0;
    my_slot = -1; roster_n = 0; session_id = 0; my_room = 0;
    set_state(ST_OFFLINE);
}

bool eng_net_set_server(const char *host_port)
{
    if (!G) G = &g_none;
    srv_pending = 0;
    eng_sock_init();
    if (st != ST_OFFLINE) eng_net_disconnect();
    char host[100], port[8];
    snprintf(port, sizeof port, "%d", ENG_NET_PORT);
    const char *colon = strrchr(host_port, ':');
    if (colon && strchr(host_port, ':') == colon && colon != host_port) {  /* host:port (one colon: not IPv6) */
        size_t hl = (size_t)(colon - host_port);
        if (hl >= sizeof host) hl = sizeof host - 1;
        memcpy(host, host_port, hl); host[hl] = 0;
        snprintf(port, sizeof port, "%s", colon + 1);
    } else snprintf(host, sizeof host, "%s", host_port);
    struct addrinfo hint_, *res = NULL;
    memset(&hint_, 0, sizeof hint_);
    hint_.ai_family = AF_INET; hint_.ai_socktype = SOCK_DGRAM;   /* IPv4 only: the servers are IPv4 UDP, and an AAAA lookup can stall the window on some Windows resolvers */
    if (getaddrinfo(host, port, &hint_, &res) != 0 || !res) {
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
const char *eng_net_server(void) { return srv_text; }

static void send_rename(void)
{
    uint8_t pl[17]; int n = name_bytes(pl + 1, my_name, 16); pl[0] = (uint8_t)n;
    send_msg(T_RENAME, 0, pl, 1 + n);
}
void eng_net_set_name(const char *name)
{
    char old[sizeof my_name]; snprintf(old, sizeof old, "%s", my_name);
    snprintf(my_name, sizeof my_name, "%s", name && *name ? name : "PLAYER");
    if ((st == ST_LOBBY || st == ST_SESSION) && sock != ENG_SOCK_BAD && strcmp(old, my_name)) send_rename();
}
const char *eng_net_name(void) { return my_name; }

void eng_net_preset_server(const char *host_port)
{
    snprintf(srv_text, sizeof srv_text, "%s", host_port ? host_port : "");
    srv_pending = srv_text[0] != 0; srv_len = 0;
}

bool eng_net_connect(void)
{
    if (!G) G = &g_none;
    if (!usable()) { snprintf(note, sizeof note, "online play not available"); return false; }
    if (!srv_text[0]) { snprintf(note, sizeof note, "no server set"); return false; }
    if (srv_pending) {
        char hp[sizeof srv_text]; snprintf(hp, sizeof hp, "%s", srv_text);
        srv_pending = 0;
        if (!eng_net_set_server(hp)) return false;
    }
    if (st != ST_OFFLINE) return true;
    note[0] = 0;
    ping_ms = -1;
    proto = 2; my_room = 0;
    hello_room_sel = 0; hello_room_name[0] = 0; switching = 0; rooms_n = 0;
    sock_open();
    if (sock == ENG_SOCK_BAD) { snprintf(note, sizeof note, "socket failed"); return false; }
    set_state(ST_CONNECTING);
    if (!exit_hook) { exit_hook = 1; atexit(leave_at_exit); }
    last_rx = t_connect = now_ms();
    hello_seq = next_seq++;
    send_hello();
    return true;
}

static void send_leave(void)                     /* UDP: said twice, the server takes it idempotently */
{
    if (sock != ENG_SOCK_BAD && (st == ST_LOBBY || st == ST_SESSION)) { send_msg(T_LEAVE, 0, NULL, 0); send_msg(T_LEAVE, 0, NULL, 0); }
}
static void leave_at_exit(void) { send_leave(); }

bool eng_net_chat_send(const char *text)
{
    if (!text || (st != ST_LOBBY && st != ST_SESSION)) return false;
    char t[CHAT_TEXT + 1]; size_t n = 0;
    for (const char *c = text; *c && n < CHAT_TEXT; c++) if ((unsigned char)*c >= ' ' && (unsigned char)*c != 0x7F) t[n++] = *c;
    if (n > 0) {                                 /* a cut in the middle of a UTF-8 character drops that character */
        int k = (int)n - 1; while (k > 0 && ((unsigned char)t[k] & 0xC0) == 0x80) k--;
        const unsigned char lead = (unsigned char)t[k];
        if (lead >= 0xC0) { int need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : 2; if ((int)n - k < need) n = (size_t)k; }
    }
    while (n && t[n - 1] == ' ') n--;
    if (!n) return false;
    send_msg(T_CHAT, 0, t, (int)n);
    return true;
}

void eng_net_disconnect(void)
{
    if (sock != ENG_SOCK_BAD) {
        send_leave();
        closesocket(sock); sock = ENG_SOCK_BAD;
    }
    pend_ready = pend_start = 0;
    my_slot = -1; roster_n = 0; session_id = 0; ping_ms = -1; rooms_n = 0; switching = 0; my_room = 0;
    note[0] = 0;
    set_state(ST_OFFLINE);
}

void eng_net_status(char *buf, size_t n)
{
    switch (st) {
    case ST_OFFLINE:    snprintf(buf, n, note[0] ? "Offline (%s)" : "Offline", note); break;
    case ST_CONNECTING: snprintf(buf, n, "Connecting to %s ...", srv_text); break;
    case ST_LOBBY:      snprintf(buf, n, "%sLobby slot %d, %d player(s), ping %d ms%s%s", eng_nethost_running() ? "Hosting: " : "", my_slot, roster_n, ping_ms, note[0] ? " - " : "", note); break;
    case ST_SESSION:    snprintf(buf, n, "%c%s armed (session %08X), slot %d", G->session_word[0] & ~0x20, G->session_word + 1, session_id, my_slot); break;
    }
}
bool eng_net_connected(void) { return st == ST_LOBBY || st == ST_SESSION; }
bool eng_net_session_active(void) { return st == ST_SESSION; }
int  eng_net_slot(void) { return my_slot; }
int  eng_net_ping_ms(void) { return ping_ms; }

bool eng_net_roster(int i, char *name, size_t n, int *ready, int *self)
{
    if (i < 0 || i >= roster_n) return false;
    if (name && n) snprintf(name, n, "%s", roster[i].name);
    if (ready) *ready = roster[i].ready;
    if (self) *self = roster[i].slot == my_slot;
    return true;
}
int eng_net_roster_slot(int i) { return i >= 0 && i < roster_n ? roster[i].slot : -1; }
int eng_net_roster_count(void) { return roster_n; }
bool eng_net_self_ready(void)
{
    for (int i = 0; i < roster_n; i++) if (roster[i].slot == my_slot) return roster[i].ready != 0;
    return false;
}

int eng_net_room_count(void) { return rooms_n; }
bool eng_net_room(int i, int *id, int *state, int *players, int *mine, char *name, size_t n)
{
    if (i < 0 || i >= rooms_n) return false;
    if (id) *id = rooms_[i].id;
    if (state) *state = rooms_[i].state;
    if (players) *players = rooms_[i].players;
    if (mine) *mine = rooms_[i].mine;
    if (name && n) snprintf(name, n, "%s", rooms_[i].name);
    return true;
}
int eng_net_room_max(int i) { return i >= 0 && i < rooms_n ? rooms_[i].maxp : 0; }
bool eng_net_room_compatible(int i) { return i >= 0 && i < rooms_n && rooms_[i].ver == (proto == 1 ? 1 : G->ver); }

void eng_net_switch_room(int id)
{
    if (st != ST_LOBBY || sock == ENG_SOCK_BAD) return;
    if (id < -1 || id == 0 || id > 65534 || (proto == 1 && id > 255)) return;
    if (proto == 1 && id == ROOM_NEW) id = -1;
    send_leave();
    pend_ready = pend_start = 0; my_slot = -1; roster_n = 0; session_id = 0;
    hello_room_sel = id; switching = 1;
    hello_seq = next_seq++;
    last_rx = now_ms();
    eng_net_chat_clear();
    my_room = 0;
    set_state(ST_CONNECTING);
    send_hello();
}
void eng_net_new_room(const char *name)
{
    snprintf(hello_room_name, sizeof hello_room_name, "%s", name ? name : "");
    eng_net_switch_room(-1);
}
void eng_net_delete_room(int id)
{
    if ((st != ST_LOBBY && st != ST_SESSION) || sock == ENG_SOCK_BAD || id < 1 || id > 65534) return;
    if (proto == 1) { if (id > 254) return; uint8_t b = (uint8_t)id; send_msg(T_DELROOM, 0, &b, 1); }
    else send_raw_p(2, T_DELROOM, 0, (uint16_t)id, NULL, 0);
    rooms_tx = 0;
}
bool eng_net_switching(void) { return switching && st == ST_CONNECTING; }

void eng_net_set_ready(int ready)
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

bool eng_net_all_ready(void)
{
    if (roster_n < 2) return false;
    for (int i = 0; i < roster_n; i++) if (!roster[i].ready) return false;
    return true;
}
void eng_net_request_start(void)
{
    if (st != ST_LOBBY) return;
    if (!eng_net_all_ready()) { snprintf(note, sizeof note, "waiting: everyone must be Ready"); if (debug == 1) fprintf(stderr, "[NET] start refused: not everyone is Ready (%d in roster)\n", roster_n); return; }
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
    if (count > ENG_NET_MAX_PLAYERS) return -1;
    int n = 0;
    for (int i = 0; i < count; i++) {
        if (off + 3 > len) return -1;
        uint8_t slot = p[off], rdy = p[off + 1], nl = p[off + 2]; off += 3;
        if (off + nl > len) return -1;
        if (slot < ENG_NET_MAX_PLAYERS) {
            int m = nl > 16 ? 16 : nl;
            roster[n].slot = slot; roster[n].ready = rdy ? 1 : 0;
            memcpy(roster[n].name, p + off, (size_t)m); roster[n].name[m] = 0;
            n++;
        }
        off += nl;
    }
    return n;
}

static uint32_t rd32(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

static void roster_update(const uint8_t *p, int len, int off)
{
    int n = parse_roster(p, len, off);
    if (n < 0) return;
    roster_n = n;
    n_roster++;
    for (int i = 0; i < n; i++)                  /* a ROSTER showing our new flag is READY's acknowledgment */
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
    frame_seq = 0; have_last_frame = 0;
    for (int i = 0; i < ENG_NET_MAX_PLAYERS; i++) last_fseq[i] = 0xFFFFFFFFu;
    pend_start = pend_ready = 0;
    set_state(ST_SESSION);
    if (G->session_begin) G->session_begin(my_slot, n);
    fprintf(stderr, "[NET] GO: session %08X, slot %d, %d players\n", sid, my_slot, n);
}

static void on_frame(const uint8_t *p, int len)
{
    if (st != ST_SESSION || len != 9 + (int)G->frame_size) return;
    if (rd32(p) != session_id) return;                     /* another session's traffic */
    uint32_t fs = rd32(p + 4);
    uint8_t slot = p[8];
    if (slot >= ENG_NET_MAX_PLAYERS || slot == my_slot) return;
    if (last_fseq[slot] != 0xFFFFFFFFu && fs <= last_fseq[slot]) return;   /* dedupe / reorder guard */
    last_fseq[slot] = fs;
    if (G->frame_in) G->frame_in(slot, p + 9, fs);
    n_frx++;
}

static const char *why_text(int why)
{
    switch (why) {
    case 1: return "room full";
    case 2: return "in progress, try later";
    case 3: return "no such room";
    case 4: return "another version of the game";
    case 5: return "too many players from your address";
    case 6: return "server full";
    case 7: return "the server refused the request";
    default: return "refused";
    }
}

static void on_message(int p_, int type, uint16_t seq, uint16_t room, const uint8_t *p, int len)
{
    switch (type) {
    case T_WELCOME: {
        const int ro = p_ == 1 ? 6 : 10;                   /* where the roster starts */
        if ((st == ST_LOBBY || st == ST_SESSION) && len >= 6 && p[0] == 0xFF) {
            if (p_ != proto) return;
            /* the server does not know us: it restarted, or timed us out (a long stall). Rejoin; a session that is running
             * turns the HELLO away, and that is reported as the reason we are offline. */
            fprintf(stderr, "[NET] the server dropped us (%s): rejoining\n", st == ST_SESSION ? "mid race" : "in the lobby");
            const int was = st == ST_SESSION;
            if (sock != ENG_SOCK_BAD) { closesocket(sock); sock = ENG_SOCK_BAD; }
            pend_ready = pend_start = 0; my_slot = -1; roster_n = 0; session_id = 0; my_room = 0;
            set_state(ST_OFFLINE);
            char h[96];
            if (was) snprintf(h, sizeof h, "Online: connection to the server was lost mid %s - rejoining", G->session_word);
            else snprintf(h, sizeof h, "Online: server restarted - rejoining");
            hint(h, 240);
            eng_net_connect();
            return;
        }
        if (st != ST_CONNECTING || seq != hello_seq || len < ro) return;
        if (p[0] == 0xFF) {
            const int why = p_ == 2 ? p[6] : (p[1] == 1 ? 2 : 1);
            if (switching) {                               /* that room is busy / gone: ask for a lobby instead of dropping out */
                switching = 0; hello_room_sel = 0; hello_seq = next_seq++;
                chat_pushf("* that room %s: you are in a free lobby instead", why == 2 ? "is busy" : why == 4 ? "runs another version" : "is full or gone");
                send_hello();
                return;
            }
            if (p_ == 1) fail(p[1] == 1 ? "race in progress, try later" : "lobby full");
            else { char w[64]; snprintf(w, sizeof w, "%s", why_text(why)); fail(w); }
            return;
        }
        if (p[0] >= ENG_NET_MAX_PLAYERS) return;
        if (p_ == 2 && rd16(p + 8) != G->frame_size) return;   /* not a room for our payload: ignore (the server checks this too) */
        proto = p_;
        my_room = p_ == 2 ? room : 0;
        switching = 0;
        my_slot = p[0];
        roster_update(p, len, ro);
        fprintf(stderr, "[NET] joined %s as slot %d ('%s')%s\n", srv_text, my_slot, my_name, proto == 1 ? " [RRN1]" : "");
        set_state(ST_LOBBY);
        break; }
    case T_ROSTER:
        if (len < 5 || p_ != proto) return;
        if (st == ST_SESSION && p[0] == 0) {               /* the server ended the session */
            session_id = 0;
            set_state(ST_LOBBY);
            char h[96]; snprintf(h, sizeof h, "Online: the %s is over - back in the lobby", G->session_word);
            hint(h, 300);
            chat_pushf("* the %s session ended: back in the lobby", G->session_word);
        }
        if (st == ST_LOBBY || st == ST_SESSION) roster_update(p, len, 5);
        break;
    case T_CHAT:
        if (len >= 2 && p_ == proto && (st == ST_LOBBY || st == ST_SESSION)) {
            char line[24 + 2 + CHAT_TEXT + 1], who[24] = "";
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
        if (len >= 1 && p_ == proto && (st == ST_LOBBY || st == ST_SESSION)) {
            int n = p[0] > MAX_ROOMS ? MAX_ROOMS : p[0], off = 1, k = 0;
            for (int i = 0; i < n; i++) {
                const int fix = p_ == 1 ? 5 : 9;           /* bytes before the name */
                if (off + fix > len) break;
                const int nl = p[off + fix - 1];
                if (off + fix + nl > len) break;
                if (p_ == 1) { rooms_[k].id = p[off]; rooms_[k].ver = 1; rooms_[k].state = p[off + 1]; rooms_[k].players = p[off + 2]; rooms_[k].maxp = 8; rooms_[k].mine = p[off + 3]; }
                else { rooms_[k].id = rd16(p + off); rooms_[k].ver = rd16(p + off + 2); rooms_[k].state = p[off + 4]; rooms_[k].players = p[off + 5]; rooms_[k].maxp = p[off + 6]; rooms_[k].mine = p[off + 7]; }
                const int m = nl > 31 ? 31 : nl; memcpy(rooms_[k].name, p + off + fix, (size_t)m); rooms_[k].name[m] = 0;
                k++; off += fix + nl;
            }
            rooms_n = k;
            if (debug == 1) { fprintf(stderr, "[NET] rooms:"); for (int i = 0; i < k; i++) fprintf(stderr, "  #%d '%s' %d/%d %s%s", rooms_[i].id, rooms_[i].name, rooms_[i].players, rooms_[i].maxp, rooms_[i].state ? "racing" : "lobby", rooms_[i].mine ? " (mine)" : ""); fprintf(stderr, "\n"); }
        }
        break;
    case T_GO: if (p_ == proto) on_go(seq, p, len); break;
    case T_FRAME: if (p_ == proto) on_frame(p, len); break;
    case T_PONG:
        if (len >= 4 && p_ == proto) ping_ms = (int)(now_ms() - rd32(p));
        break;
    default: break;                                        /* unknown types: ignore (the contract) */
    }
}

static void send_frame(void)
{
    uint8_t pl[9 + ENG_NET_MAX_FRAME];
    if (!G->frame_out || !G->frame_out(pl + 9)) return;
    pl[0] = (uint8_t)session_id; pl[1] = (uint8_t)(session_id >> 8);
    pl[2] = (uint8_t)(session_id >> 16); pl[3] = (uint8_t)(session_id >> 24);
    pl[4] = (uint8_t)frame_seq; pl[5] = (uint8_t)(frame_seq >> 8);
    pl[6] = (uint8_t)(frame_seq >> 16); pl[7] = (uint8_t)(frame_seq >> 24);
    pl[8] = (uint8_t)my_slot;                    /* our cabinet number = our slot */
    frame_seq++;
    send_msg(T_FRAME, 0, pl, 9 + G->frame_size);
    memcpy(last_frame, pl, 9 + G->frame_size); have_last_frame = 1;
    n_ftx++;
}
/* while we wait for a peer (stall-and-wait) our newest FRAME is sent again now and then: if it was lost, a peer that is waiting for
 * US would otherwise wait for a frame that never comes (both sides stalled until the timeout). Same frame_seq: a receiver that has
 * it drops the copy. */
static void resend_last_frame(void)
{
    if (st == ST_SESSION && have_last_frame) send_msg(T_FRAME, 0, last_frame, 9 + G->frame_size);
}

/* one datagram from the server: either protocol (only the connection's own counts once joined), our game only */
static void handle_raw(const uint8_t *b, int n, uint32_t now)
{
    if (n < 10) return;
    int p_;
    if (!memcmp(b, "NMN2", 4)) {
        if (n < 16 || b[10] != (uint8_t)G->id[0] || b[11] != (uint8_t)G->id[1]) return;
        p_ = 2;
    } else if (!memcmp(b, "RRN1", 4) && G->rrn1) p_ = 1;
    else return;
    const int h = p_ == 1 ? 10 : 16;
    int len = b[8] | b[9] << 8;
    if (len != n - h) return;                    /* bad length: drop (the contract) */
    last_rx = now;
    on_message(p_, b[4], (uint16_t)(b[6] | b[7] << 8), p_ == 2 ? rd16(b + 14) : 0, b + h, len);
}

static void send_ping(uint32_t now)
{
    uint8_t pl[4] = { (uint8_t)now, (uint8_t)(now >> 8), (uint8_t)(now >> 16), (uint8_t)(now >> 24) };
    send_msg(T_PING, 0, pl, 4);
    last_ping_tx = now;
}

/* ---- LAN discovery --------------------------------------------------------
 * DISCOVER is broadcast on the LAN (limited broadcast, every interface's directed broadcast where the OS lists them, and
 * loopback for a host on this machine); every host answers ANNOUNCE. Results are kept until the next search. Runs for 4 s
 * (resent each second), on its own socket. */
#define DISC_MS 4000
static eng_sock_t dsock = ENG_SOCK_BAD;
static uint32_t d_start, d_tx; static int d_auto;
static struct { struct sockaddr_storage a; char addr[72], name[17]; uint8_t state; uint16_t players, rooms; } found[8];
static int found_n;

static void disc_send_one(uint32_t ip_be)
{
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_addr.s_addr = ip_be; a.sin_port = htons(ENG_NET_PORT);
    uint8_t b[16];
    int h = put_hdr(b, 2, T_DISCOVER, 0, 0, 0);
    sendto(dsock, (const char *)b, h, 0, (struct sockaddr *)&a, sizeof a);
    if (G->rrn1) { h = put_hdr(b, 1, T_DISCOVER, 0, 0, 0); sendto(dsock, (const char *)b, h, 0, (struct sockaddr *)&a, sizeof a); }
}
static void disc_send(void)
{
    disc_send_one(htonl(INADDR_BROADCAST));
    disc_send_one(htonl(INADDR_LOOPBACK));
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

void eng_net_discover(void)
{
    if (!G) G = &g_none;
    eng_sock_init();
    if (dsock != ENG_SOCK_BAD) { closesocket(dsock); dsock = ENG_SOCK_BAD; }
    dsock = socket(AF_INET, SOCK_DGRAM, 0);
    if (dsock == ENG_SOCK_BAD) { snprintf(note, sizeof note, "socket failed"); return; }
    int on = 1; setsockopt(dsock, SOL_SOCKET, SO_BROADCAST, (const char *)&on, sizeof on);
    eng_sock_nonblock(dsock);
    found_n = 0;
    d_start = now_ms();
    disc_send();
}
bool eng_net_discovering(void) { return dsock != ENG_SOCK_BAD; }
void eng_net_discover_autojoin(int on) { d_auto = on; }
int eng_net_found_count(void) { return found_n; }
bool eng_net_found(int i, char *label, size_t n, char *addr, size_t an)
{
    if (i < 0 || i >= found_n) return false;
    if (label && n) snprintf(label, n, "%s  %s  %d player(s)%s", found[i].name, found[i].addr, found[i].players, found[i].state ? "  [race running]" : "");
    if (addr) snprintf(addr, an, "%s", found[i].addr);
    return true;
}

static void disc_poll(void)
{
    if (dsock == ENG_SOCK_BAD) return;
    uint8_t b[512];
    for (int k = 0; k < 16; k++) {
        struct sockaddr_storage from; socklen_t fl = sizeof from;
        int n = (int)recvfrom(dsock, (char *)b, sizeof b, 0, (struct sockaddr *)&from, &fl);
        if (n < 0) break;
        int p_ = !memcmp(b, "NMN2", 4) && n >= 16 ? 2 : !memcmp(b, "RRN1", 4) && n >= 10 && G->rrn1 ? 1 : 0;
        if (!p_ || b[4] != T_ANNOUNCE) continue;
        const int h = p_ == 1 ? 10 : 16;
        if ((b[8] | b[9] << 8) != n - h) continue;
        if (p_ == 2 && (b[10] != (uint8_t)G->id[0] || b[11] != (uint8_t)G->id[1])) continue;
        const uint8_t *p = b + h; const int len = n - h;
        int nl, name_at; uint8_t state = 0; uint16_t players, rooms = 1;
        if (p_ == 1) { if (len < 3) continue; nl = p[2]; name_at = 3; if (len - 3 < nl) continue; state = p[0]; players = p[1]; }
        else { if (len < 1) continue; nl = p[0]; name_at = 1; if (len < 1 + nl + 6) continue; players = rd16(p + 1 + nl); rooms = rd16(p + 3 + nl); state = rd16(p + 5 + nl) == 0 && rooms > 0; }
        int idx = -1;
        for (int i = 0; i < found_n; i++) if (eng_addr_eq(&found[i].a, &from)) idx = i;
        if (idx >= 0 && p_ == 1) continue;                /* the NMN2 answer of the same host is the better one */
        if (idx < 0) { if (found_n >= 8) continue; idx = found_n++; }
        found[idx].a = from;
        char host[64] = "?"; getnameinfo((struct sockaddr *)&from, fl, host, sizeof host, NULL, 0, NI_NUMERICHOST);
        uint16_t port = from.ss_family == AF_INET ? ntohs(((struct sockaddr_in *)&from)->sin_port) : ENG_NET_PORT;
        snprintf(found[idx].addr, sizeof found[idx].addr, "%s:%u", host, port);
        found[idx].state = state; found[idx].players = players; found[idx].rooms = rooms;
        if (nl > 16) nl = 16;
        int m = 0; for (int i = 0; i < nl; i++) if (p[name_at + i] >= ' ' && p[name_at + i] != 0x7F) found[idx].name[m++] = (char)p[name_at + i];
        found[idx].name[m] = 0;
        if (debug == 1) fprintf(stderr, "[NET] found %s '%s' %d player(s)\n", found[idx].addr, found[idx].name, found[idx].players);
    }
    uint32_t now = now_ms();
    if (now - d_tx >= 1000 && now - d_start < DISC_MS) disc_send();
    if (now - d_start >= DISC_MS) { closesocket(dsock); dsock = ENG_SOCK_BAD; }
    if (d_auto && found_n > 0 && st == ST_OFFLINE && !found[0].state) {     /* headless test: join the first host found */
        d_auto = 0;
        if (eng_net_set_server(found[0].addr)) eng_net_connect();
    }
}

/* ---- hosting: the built-in server, and this client joined to it over loopback ---- */
bool eng_net_host_start(void)
{
    if (!G) G = &g_none;
    if (eng_nethost_running()) return true;
    if (st != ST_OFFLINE) eng_net_disconnect();
    if (!eng_nethost_start(ENG_NET_PORT, G)) { snprintf(note, sizeof note, "cannot host: port %d is busy", ENG_NET_PORT); return false; }
    eng_nethost_set_name(my_name);
    if (!eng_net_set_server("127.0.0.1")) { eng_nethost_stop(); return false; }
    return eng_net_connect();
}
void eng_net_host_stop(void) { eng_net_disconnect(); eng_nethost_stop(); }
bool eng_net_hosting(void) { return eng_nethost_running(); }

/* ---- stall-and-wait ---- */
static uint32_t stall_t0, n_stall;
int eng_net_lead(void)
{
    if (st != ST_SESSION) return 0;
    int64_t lowest = -1; int any = 0;
    for (int i = 0; i < roster_n; i++) {
        const int s = roster[i].slot;
        if (s == my_slot || s >= ENG_NET_MAX_PLAYERS) continue;
        const int64_t got = last_fseq[s] == 0xFFFFFFFFu ? 0 : (int64_t)last_fseq[s] + 1;   /* frames received from that peer */
        if (!any || got < lowest) lowest = got;
        any = 1;
    }
    if (!any) return 0;
    const int64_t lead = (int64_t)frame_seq - lowest;
    return lead > 0x7FFFFFFF ? 0x7FFFFFFF : lead < -0x7FFFFFFF ? -0x7FFFFFFF : (int)lead;
}
bool eng_net_may_advance(int max_lead, int timeout_ms)
{
    if (eng_net_lead() <= max_lead) { stall_t0 = 0; return true; }
    const uint32_t now = now_ms();
    if (!stall_t0) stall_t0 = now ? now : 1;
    if ((int)(now - stall_t0) >= timeout_ms) {
        if (debug == 1 && now - stall_t0 < (uint32_t)timeout_ms + 20) {
            fprintf(stderr, "[NET] stall timeout (lead %d, sent %u, from peers:", eng_net_lead(), frame_seq);
            for (int i = 0; i < roster_n; i++) if (roster[i].slot != my_slot) fprintf(stderr, " %d:%d", roster[i].slot, (int)last_fseq[roster[i].slot]);
            fprintf(stderr, "): running on\n");
        }
        return true;
    }
    n_stall++;
    return false;
}
uint32_t eng_net_stall_frames(void) { return n_stall; }
int eng_net_auto_lead(void) { const int p = ping_ms < 0 ? 0 : ping_ms; return 2 + (p * 60 + 999) / 1000; }   /* a peer's frame comes ~our RTT late (theirs + ours, each half) */
static int poll_paused;
uint32_t eng_net_wait(int max_lead, int timeout_ms)
{
    const uint32_t t0 = now_ms();
    uint32_t last_resend = t0;
    while (!eng_net_may_advance(max_lead, timeout_ms)) {   /* bounded by timeout_ms: may_advance gives up then */
        poll_paused = 1; eng_net_poll(); poll_paused = 0;
        if (now_ms() - last_resend >= 20) { resend_last_frame(); last_resend = now_ms(); }
        SDL_Delay(1);
    }
    return now_ms() - t0;
}

void eng_net_poll_paused(void) { poll_paused = 1; eng_net_poll(); poll_paused = 0; }

/* ---- <P>_SIM=loss%,spike_ms[,burst]: a bad Wi-Fi link, on the receive side (tests only) ----
 * each received message is dropped with probability loss% (after a drop the next burst-1 are dropped too); and about every
 * 2 s (at random) the link STALLS for spike_ms/2..spike_ms: everything received meanwhile is held and then arrives at once,
 * in order -- what a Wi-Fi retry storm, a background scan or a power-save wake looks like from the game. */
static int sim_loss = -1, sim_jit, sim_burst, sim_left;
static uint32_t sim_rng = 12345, sim_hold_until, sim_last;
#define SIM_Q 256
static struct { uint32_t at; int n; uint8_t b[BUF_MAX]; } *simq;
static int simq_n;
static void sim_reset(void) { simq_n = 0; sim_left = 0; }
static uint32_t sim_rand(void) { sim_rng = sim_rng * 1103515245u + 12345u; return sim_rng >> 8; }
static void sim_init(void)
{
    sim_loss = 0;
    const char *e = envs("SIM");
    if (!e || !*e) return;
    sim_burst = 1;
    if (sscanf(e, "%d,%d,%d", &sim_loss, &sim_jit, &sim_burst) < 1) sim_loss = 0;
    if (sim_burst < 1) sim_burst = 1;
    const char *s = envs("SIM_SEED"); if (s) sim_rng = (uint32_t)strtoul(s, NULL, 0);
    simq = calloc(SIM_Q, sizeof *simq);
    if (!simq) { sim_loss = sim_jit = 0; return; }
    fprintf(stderr, "[NET] %s_SIM: loss %d%%, stalls of %d ms (~1 per 2 s), burst %d\n", G->env, sim_loss, sim_jit, sim_burst);
}
static bool sim_on(void) { if (sim_loss < 0) sim_init(); return sim_loss > 0 || sim_jit > 0; }

void eng_net_poll(void)
{
    if (!G) G = &g_none;
    if (debug < 0) { const char *e = envs("DEBUG"); debug = e && *e == '1'; }
    if (autostart < 0) { const char *e = envs("AUTOSTART"); autostart = e ? atoi(e) : 0; }   /* 1 = start once 2+ are ready; N>=2 = wait for N players */
    eng_nethost_poll();                                    /* the built-in host (inert unless hosting) */
    disc_poll();                                           /* LAN search (inert unless searching) */
    if (st == ST_OFFLINE) return;                          /* inert: no client socket, no syscalls */
    uint32_t now = now_ms();
    { static int say_lobby, say_race; static uint32_t say_t;     /* headless test: <P>_SAY=text: one line in the lobby, one in the session */
      static const char *say; static int say_init;
      if (!say_init) { say_init = 1; say = envs("SAY"); }
      if (say && *say) {
          if (st != ST_LOBBY && st != ST_SESSION) say_t = now;
          else if (now - say_t > 1500) {
              if (st == ST_LOBBY && !say_lobby) { say_lobby = 1; eng_net_chat_send(say); }
              if (st == ST_SESSION && !say_race) { say_race = 1; eng_net_chat_send(say); }
          }
      } }
    { static int t_init, t_room_done, t_name_done; static const char *t_room, *t_name;      /* headless tests: <P>_ROOM=new[:name]|<id>, <P>_RENAME=<name> */
      if (!t_init) { t_init = 1; t_room = envs("ROOM"); t_name = envs("RENAME"); }
      if (st == ST_LOBBY && now_ms() - t_state > 1500) {
          if (t_room && *t_room && !t_room_done) { t_room_done = 1; if (!strncmp(t_room, "new", 3)) eng_net_new_room(t_room[3] == ':' ? t_room + 4 : NULL); else eng_net_switch_room(atoi(t_room)); }
          else if (t_name && *t_name && !t_name_done) { t_name_done = 1; eng_net_set_name(t_name); }
      } }
    uint8_t b[BUF_MAX];
    for (int i = 0; i < 32; i++) {                         /* bounded: never stall the frame */
        if (sock == ENG_SOCK_BAD) break;                   /* a message just failed the connection: keep ITS reason */
        struct sockaddr_storage from; socklen_t fl = sizeof from;
        int n = (int)recvfrom(sock, (char *)b, sizeof b, 0, (struct sockaddr *)&from, &fl);
        if (n < 0) {
            if (!eng_sock_would_block()) { if (st != ST_SESSION) fail("socket error"); }
            break;
        }
        if (!from_server(&from)) continue;                 /* not the server: drop */
        if (sim_on()) {                                    /* <P>_SIM: lose it, or hold it a while */
            if (sim_left > 0) { sim_left--; continue; }
            if ((int)(sim_rand() % 100) < sim_loss) { sim_left = sim_burst - 1; continue; }
            if (sim_jit && (int32_t)(now - sim_hold_until) >= 0 && now != sim_last && sim_rand() % 2000 < (now - sim_last > 100 ? 100 : now - sim_last))
                sim_hold_until = now + (uint32_t)sim_jit / 2 + sim_rand() % ((uint32_t)sim_jit / 2 + 1);
            sim_last = now;
            if (simq_n < SIM_Q && n <= (int)sizeof simq[0].b) {
                simq[simq_n].at = (int32_t)(sim_hold_until - now) > 0 ? sim_hold_until : now;
                simq[simq_n].n = n; memcpy(simq[simq_n].b, b, (size_t)n); simq_n++;
            }
            continue;
        }
        handle_raw(b, n, now);
    }
    while (simq_n > 0 && (int32_t)(now - simq[0].at) >= 0 && sock != ENG_SOCK_BAD) {
        const int n = simq[0].n; memcpy(b, simq[0].b, (size_t)n);
        memmove(&simq[0], &simq[1], (size_t)(--simq_n) * sizeof simq[0]);
        handle_raw(b, n, now);
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
        if (now - rooms_tx >= 2000) { send_msg(T_ROOMS, 0, NULL, 0); rooms_tx = now; }   /* the room list, kept fresh */
        if (now - last_rx > DEAD_MS) { fail("connection lost"); break; }
        if (autostart) {                                   /* headless test: start once the lobby has settled at 2+ */
            if (roster_n >= (autostart > 1 ? autostart : 2)) {
                if (!autostart_t0) autostart_t0 = now;
                else if (now - autostart_t0 >= 2000 && !pend_start) { for (int i = 0; i < roster_n; i++) if (roster[i].slot == my_slot && !roster[i].ready && !pend_ready) eng_net_set_ready(1); if (eng_net_all_ready()) eng_net_request_start(); }
            } else autostart_t0 = 0;
        }
        break;
    case ST_SESSION:
        if (!poll_paused) send_frame();                    /* the game is running: its newest payload, once per frame */
        if (now - last_ping_tx >= PING_MS) send_ping(now); /* PING is the liveness fallback */
        /* a dead server mid-session is NOT fatal: the game's own link timeout drops the rivals and it plays out alone */
        break;
    }

    if (debug == 1 && ++n_poll % 60 == 0)
    {
        char sb[32] = "";
        if (n_stall) snprintf(sb, sizeof sb, "  stalled %u", n_stall);
        fprintf(stderr, "[NET] %s slot %d  frames tx %u rx %u  roster %u  ping %d ms%s\n",
                st_name[st], my_slot, n_ftx, n_frx, n_roster, ping_ms, sb);
    }
}
