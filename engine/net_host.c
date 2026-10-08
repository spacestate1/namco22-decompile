/*
 * net_host.c -- the built-in NMN2 lobby/relay of one room (engine/net_host.h). Grown out of Rave Racer's RRN1 host
 * (raverace/src/rr_netd.c); behaviour mirrors server/ (nmn-server): lowest-free slot, WELCOME answers HELLO by seq, ROSTER on
 * every lobby change, GO resent every 500 ms until ACKed, FRAMEs relayed to the other members (re-headed for a member of
 * the other protocol), 5 s liveness, CHAT relayed (5 lines / 2 s), a session ends by itself (last player gone, no FRAME for
 * 90 s, or 15 min), a PING from a non-member gets the short rejection WELCOME; DISCOVER -> ANNOUNCE; ROOMS is not answered (one room).
 * Only the hosting game's id / version / frame size may join; RRN1 only when that game is Rave Racer (eng_net_game.rrn1).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <SDL2/SDL.h>
#include "net_sock.h"
#include "net_host.h"

#define LIVENESS_MS 5000
#define GO_RESEND_MS 500
#define MAX_CHAT 96
#define SESSION_MAX_MS (15u * 60u * 1000u)
#define FRAME_IDLE_MS 90000u
#define BUF (16 + 9 + ENG_NET_MAX_FRAME + 64)
enum { T_HELLO = 0x01, T_WELCOME, T_ROSTER, T_READY, T_START, T_GO, T_LEAVE,
       T_FRAME, T_PING, T_PONG, T_ACK, T_DISCOVER, T_ANNOUNCE, T_CHAT, T_ROOMS, T_ROOMLIST, T_RENAME, T_DELROOM };
enum { WHY_OK = 0, WHY_FULL = 1, WHY_RACING = 2, WHY_NOROOM = 3, WHY_VERSION = 4, WHY_BAD = 7, WHY_NOTMEMBER = 8 };
#define ROOM_ID 1

typedef struct {
    int used, ready, go_acked, legacy;
    struct sockaddr_storage a; socklen_t al;
    char name[17];
    uint32_t last_seen, last_go; uint16_t go_seq;
    uint32_t chat_t; int chat_n;
} slot_t;

static const eng_net_game *G;
static eng_sock_t sk = ENG_SOCK_BAD;
static slot_t sl[ENG_NET_MAX_PLAYERS];
static int nslots;
static int state;                          /* 0 lobby, 1 session */
static uint32_t sid, rng, sess_start, last_frame;
static uint16_t nseq = 1;
static char host_name[17] = "HOST";

static uint32_t now_ms(void) { return SDL_GetTicks(); }
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }   /* not libc rand(): the game's own state stays untouched */

static int hdr(uint8_t *b, int legacy, int type, uint16_t seq, int len)
{
    memcpy(b, legacy ? "RRN1" : "NMN2", 4);
    b[4] = (uint8_t)type; b[5] = 0;
    b[6] = (uint8_t)seq; b[7] = (uint8_t)(seq >> 8);
    b[8] = (uint8_t)len; b[9] = (uint8_t)(len >> 8);
    if (legacy) return 10;
    b[10] = (uint8_t)G->id[0]; b[11] = (uint8_t)G->id[1];
    b[12] = (uint8_t)G->ver; b[13] = (uint8_t)(G->ver >> 8);
    b[14] = ROOM_ID; b[15] = 0;
    return 16;
}
static void sendm(const struct sockaddr_storage *a, socklen_t al, int legacy, int type, uint16_t seq, const uint8_t *pl, int len)
{
    uint8_t b[BUF];
    if (len > (int)sizeof b - 16) return;
    int h = hdr(b, legacy, type, seq, len);
    if (len) memcpy(b + h, pl, (size_t)len);
    sendto(sk, (const char *)b, h + len, 0, (const struct sockaddr *)a, al);
}

static int roster_payload(uint8_t *o)              /* count, then { slot, ready, name_len, name } by slot */
{
    int n = 1, c = 0;
    for (int i = 0; i < nslots; i++) {
        if (!sl[i].used) continue;
        int nl = (int)strlen(sl[i].name);
        o[n++] = (uint8_t)i; o[n++] = (uint8_t)sl[i].ready; o[n++] = (uint8_t)nl;
        memcpy(o + n, sl[i].name, (size_t)nl); n += nl;
        c++;
    }
    o[0] = (uint8_t)c;
    return n;
}
static int players(void) { int c = 0; for (int i = 0; i < nslots; i++) c += sl[i].used; return c; }
static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

static void to_all(int type, const uint8_t *pl, int n)
{
    for (int i = 0; i < nslots; i++) if (sl[i].used) sendm(&sl[i].a, sl[i].al, sl[i].legacy, type, 0, pl, n);
}
static void roster_changed(void)
{
    uint8_t pl[512]; pl[0] = (uint8_t)state; put32(pl + 1, sid);
    int n = 5 + roster_payload(pl + 5);
    to_all(T_ROSTER, pl, n);
}

static void welcome(const struct sockaddr_storage *a, socklen_t al, int legacy, int slot, int why, uint16_t seq)
{
    uint8_t pl[512]; int n = 0;
    pl[n++] = slot < 0 ? 0xFF : (uint8_t)slot; pl[n++] = (uint8_t)state; put32(pl + n, sid); n += 4;
    if (!legacy) { pl[n++] = (uint8_t)why; pl[n++] = (uint8_t)nslots; pl[n++] = (uint8_t)G->frame_size; pl[n++] = (uint8_t)(G->frame_size >> 8); }
    if (slot >= 0) n += roster_payload(pl + n); else pl[n++] = 0;
    sendm(a, al, legacy, T_WELCOME, seq, pl, n);
}

static void send_go(int i)
{
    uint8_t pl[512]; put32(pl, sid);
    int n = 4 + roster_payload(pl + 4);
    if (!sl[i].go_seq) { if (!nseq) nseq = 1; sl[i].go_seq = nseq++; }
    sendm(&sl[i].a, sl[i].al, sl[i].legacy, T_GO, sl[i].go_seq, pl, n);
    sl[i].last_go = now_ms();
}

static int slot_of(const struct sockaddr_storage *a)
{
    for (int i = 0; i < nslots; i++) if (sl[i].used && eng_addr_eq(&sl[i].a, a)) return i;
    return -1;
}

static void on_hello(const struct sockaddr_storage *a, socklen_t al, int legacy, uint16_t seq, const uint8_t *p, int len, int slot)
{
    if (slot >= 0) { welcome(a, al, legacy, slot, WHY_OK, seq); return; }          /* duplicate HELLO: re-answer */
    if (len < 1) return;
    int nl = p[0]; if (nl > 16) nl = 16; if (nl > len - 1) nl = len - 1;
    if (!legacy) {                                                   /* NMN2: [name] mode max_players frame_size ... */
        const int at = 1 + (p[0] < len - 1 ? p[0] : len - 1);
        if (len < at + 4) { welcome(a, al, 0, -1, WHY_BAD, seq); return; }
        const int fs = p[at + 2] | p[at + 3] << 8;
        if (fs != G->frame_size) { welcome(a, al, 0, -1, WHY_VERSION, seq); return; }
    }
    if (state == 1) { welcome(a, al, legacy, -1, WHY_RACING, seq); return; }       /* session running: reject */
    int free_i = -1;
    for (int i = 0; i < nslots; i++) if (!sl[i].used) { free_i = i; break; }
    if (free_i < 0) { welcome(a, al, legacy, -1, WHY_FULL, seq); return; }        /* lobby full */
    slot_t *s = &sl[free_i];
    memset(s, 0, sizeof *s);
    s->used = 1; s->legacy = legacy; s->a = *a; s->al = al; s->last_seen = now_ms();
    int k = 0;
    for (int i = 0; i < nl; i++) if (p[1 + i] >= ' ' && p[1 + i] != 0x7F) s->name[k++] = (char)p[1 + i];
    s->name[k] = 0;
    welcome(a, al, legacy, free_i, WHY_OK, seq);
    roster_changed();
}

static void on_start(void)
{
    int ready = 0; for (int i = 0; i < nslots; i++) ready += sl[i].used && sl[i].ready;
    if (players() < 2 || ready != players()) return;               /* the session waits until everyone is Ready */
    state = 1;
    sess_start = last_frame = now_ms();
    do { sid = rnd(); } while (!sid);
    fprintf(stderr, "[NETD] race start: session %08X, %d player(s)\n", sid, players());
    for (int i = 0; i < nslots; i++) if (sl[i].used) { sl[i].go_seq = 0; sl[i].go_acked = 0; send_go(i); }
    roster_changed();
}

static void on_chat(int slot, const uint8_t *p, int len)
{
    uint8_t pl[1 + MAX_CHAT + 4]; int n = 0;
    for (int i = 0; i < len && n < MAX_CHAT; i++) {              /* printable bytes only (UTF-8 continuation bytes pass), cut at MAX_CHAT */
        if (p[i] < ' ' || p[i] == 0x7F) continue;
        pl[1 + n++] = p[i];
    }
    if (n > 0) {                                                 /* a cut in the middle of a UTF-8 character drops that character */
        int k = n; while (k > 1 && (pl[k] & 0xC0) == 0x80) k--;
        if (pl[k] >= 0xC0) { int need = pl[k] >= 0xF0 ? 4 : pl[k] >= 0xE0 ? 3 : 2; if (n - k + 1 < need) n = k - 1; }
    }
    while (n > 0 && pl[1] == ' ') { memmove(pl + 1, pl + 2, (size_t)--n); }
    while (n > 0 && pl[n] == ' ') n--;
    if (n <= 0) return;
    uint32_t now = now_ms();
    slot_t *s = &sl[slot];
    if (now - s->chat_t >= 2000) { s->chat_t = now; s->chat_n = 0; }
    if (++s->chat_n > 5) return;                                 /* 5 lines per 2 s per player */
    pl[0] = (uint8_t)slot;
    to_all(T_CHAT, pl, 1 + n);
}

static void end_session(const char *why)
{
    fprintf(stderr, "[NETD] session over (%s); back to lobby\n", why);
    state = 0; sid = 0;
    for (int i = 0; i < nslots; i++) if (sl[i].used) { sl[i].ready = 0; sl[i].go_seq = 0; sl[i].go_acked = 0; }
    roster_changed();
}

static void on_datagram(const uint8_t *b, int n, const struct sockaddr_storage *a, socklen_t al)
{
    int legacy;
    if (n >= 10 && !memcmp(b, "RRN1", 4) && G->rrn1) legacy = 1;
    else if (n >= 16 && !memcmp(b, "NMN2", 4) && b[10] == (uint8_t)G->id[0] && b[11] == (uint8_t)G->id[1]) legacy = 0;
    else return;                                                    /* another protocol or another game: not ours */
    const int h = legacy ? 10 : 16;
    int len = b[8] | b[9] << 8;
    if (len != n - h) return;
    int type = b[4]; uint16_t seq = (uint16_t)(b[6] | b[7] << 8);
    const uint8_t *p = b + h;
    if (!legacy && (b[12] | b[13] << 8) != G->ver && type != T_DISCOVER && type != T_ROOMS) {
        if (type == T_HELLO) welcome(a, al, 0, -1, WHY_VERSION, seq);
        return;
    }
    int slot = slot_of(a);
    if (slot >= 0 && sl[slot].legacy != legacy) return;
    if (slot >= 0) sl[slot].last_seen = now_ms();
    switch (type) {
    case T_HELLO: on_hello(a, al, legacy, seq, p, len, slot); break;
    case T_READY: if (slot >= 0 && state == 0 && len >= 1) { sl[slot].ready = p[0] ? 1 : 0; roster_changed(); } break;
    case T_START: if (slot >= 0 && state == 0) on_start(); break;
    case T_ACK: if (slot >= 0 && len >= 2 && (uint16_t)(p[0] | p[1] << 8) == sl[slot].go_seq) sl[slot].go_acked = 1; break;
    case T_LEAVE: if (slot >= 0) { sl[slot].used = 0; roster_changed(); } break;
    case T_FRAME:
        if (slot < 0 || state != 1 || len != 9 + G->frame_size) break;   /* exactly the room's size (no oversize relay) */
        if ((uint32_t)(p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24) != sid || p[8] != slot) break;
        last_frame = now_ms();
        for (int i = 0; i < nslots; i++) {
            if (!sl[i].used || i == slot) continue;
            if (sl[i].legacy == legacy) sendto(sk, (const char *)b, (size_t)n, 0, (const struct sockaddr *)&sl[i].a, sl[i].al);
            else sendm(&sl[i].a, sl[i].al, sl[i].legacy, T_FRAME, 0, p, len);     /* bridged: the same payload, the peer's header */
        }
        break;
    case T_PING:
        if (len < 4) break;
        if (slot >= 0) sendm(a, al, legacy, T_PONG, 0, p, 4);
        else welcome(a, al, legacy, -1, WHY_NOTMEMBER, 0);            /* not a member (restart / timed out): the short rejection says rejoin */
        break;
    case T_CHAT: if (slot >= 0) on_chat(slot, p, len); break;
    case T_RENAME:
        if (slot >= 0 && len >= 1) {
            int nl = p[0]; if (nl > 16) nl = 16; if (nl > len - 1) nl = len - 1;
            int k = 0; for (int i = 0; i < nl; i++) if (p[1 + i] >= ' ' && p[1 + i] != 0x7F) sl[slot].name[k++] = (char)p[1 + i];
            sl[slot].name[k] = 0; roster_changed();
        }
        break;
    /* ROOMS is not answered: a LAN game is one room (the client shows no room list, as with Rave Racer's old host) */
    case T_DISCOVER: {                                              /* LAN discovery: who is hosting, and is there room */
        uint8_t pl[32]; int nl = (int)strlen(host_name), k = 0;
        if (legacy) { pl[k++] = (uint8_t)state; pl[k++] = (uint8_t)players(); pl[k++] = (uint8_t)nl; memcpy(pl + k, host_name, (size_t)nl); k += nl; }
        else {
            pl[k++] = (uint8_t)nl; memcpy(pl + k, host_name, (size_t)nl); k += nl;
            const int open = state == 0 && players() < nslots;
            pl[k++] = (uint8_t)players(); pl[k++] = 0; pl[k++] = 1; pl[k++] = 0; pl[k++] = (uint8_t)open; pl[k++] = 0;
        }
        sendm(a, al, legacy, T_ANNOUNCE, 0, pl, k);
        break; }
    default: break;
    }
}

bool eng_nethost_start(int port, const eng_net_game *g)
{
    if (sk != ENG_SOCK_BAD) return true;
    if (!g || !g->frame_size || g->frame_size > ENG_NET_MAX_FRAME) return false;
    G = g;
    nslots = g->max_players < 2 ? 2 : g->max_players > ENG_NET_MAX_PLAYERS ? ENG_NET_MAX_PLAYERS : g->max_players;
    eng_sock_init();
    sk = socket(AF_INET, SOCK_DGRAM, 0);
    if (sk == ENG_SOCK_BAD) return false;
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_ANY); a.sin_port = htons((uint16_t)port);
    if (bind(sk, (struct sockaddr *)&a, sizeof a) != 0) { closesocket(sk); sk = ENG_SOCK_BAD; return false; }
    eng_sock_nonblock(sk);
    memset(sl, 0, sizeof sl); state = 0; sid = 0;
    rng = (uint32_t)time(NULL) ^ (now_ms() * 2654435761u) ^ 0x9E3779B9u; if (!rng) rng = 1;
    fprintf(stderr, "[NETD] hosting on UDP port %d (%s, %d players)\n", port, g->id, nslots);
    return true;
}

void eng_nethost_stop(void)
{
    if (sk == ENG_SOCK_BAD) return;
    closesocket(sk); sk = ENG_SOCK_BAD;
    memset(sl, 0, sizeof sl); state = 0; sid = 0;
    fprintf(stderr, "[NETD] stopped\n");
}

bool eng_nethost_running(void) { return sk != ENG_SOCK_BAD; }
void eng_nethost_set_name(const char *name) { snprintf(host_name, sizeof host_name, "%s", name && *name ? name : "HOST"); }

void eng_nethost_poll(void)
{
    if (sk == ENG_SOCK_BAD) return;
    uint8_t b[BUF];
    for (int i = 0; i < 64; i++) {                                  /* bounded: never stall the frame */
        struct sockaddr_storage from; socklen_t fl = sizeof from;
        int n = (int)recvfrom(sk, (char *)b, sizeof b, 0, (struct sockaddr *)&from, &fl);
        if (n < 0) break;
        on_datagram(b, n, &from, fl);
    }
    uint32_t now = now_ms();
    for (int i = 0; i < nslots; i++)
        if (sl[i].used && now - sl[i].last_seen > LIVENESS_MS) {
            fprintf(stderr, "[NETD] timeout slot %d '%s'\n", i, sl[i].name);
            sl[i].used = 0; roster_changed();
        }
    if (state == 1) {
        if (!players()) end_session("everyone left");
        else if (now - sess_start > SESSION_MAX_MS) end_session("time limit");
        else if (now - last_frame > FRAME_IDLE_MS) end_session("no traffic");
        else for (int i = 0; i < nslots; i++)
            if (sl[i].used && !sl[i].go_acked && now - sl[i].last_go >= GO_RESEND_MS) send_go(i);
    }
}
