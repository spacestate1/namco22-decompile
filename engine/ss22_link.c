/* ss22_link.c -- see ss22_link.h */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET lk_sock;
#define LK_BAD INVALID_SOCKET
#define lk_close closesocket
#else
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
typedef int lk_sock;
#define LK_BAD (-1)
#define lk_close close
#endif
#include "ss22_link.h"
#include "ss22_board.h"
#include "ss22_game.h"
#include "c139.h"
#include "lift_cpu.h"

bool g_ss22_link_on;
static int me = 0, ncab = 1, dbg;
static unsigned cab_mask = 1;
static char tagname[16] = "SS22";
static void (*ext_cb)(uint32_t);

/* ---- the chip on the board ---- */
static bool raise_sci(void)
{
    if (!(g_hw.irq_enabled & 4u)) return false;          /* syscon line 2 = the SCI */
    g_hw.irq_state |= 4u;
    return true;
}

uint16_t ss22_link_reg_read(unsigned reg) { return c139_reg_read(reg); }
void     ss22_link_reg_write(unsigned reg, uint16_t v) { c139_reg_write(reg, v); }
void     ss22_link_sci_ack(void) { c139_irq_ack(); }

/* what the chip sent, waiting for the frame's payload */
static c139_frame outq[64]; static int outr, outw; static uint32_t out_drop;
static void drain_tx(void)
{
    c139_frame f;
    while (c139_tx_take(&f)) {
        outq[outw] = f; outw = (outw + 1) % 64;
        if (outw == outr) { outr = (outr + 1) % 64; out_drop++; }
    }
}

/* Up to 4 chip events a slice (64 a frame): with four cabinets a frame of battle is ~9 transfers + ~8 received frames each, every one an
 * interrupt, and the handler starts the next transfer from inside the previous one's. */
void ss22_link_slice(void)
{
    for (int k = 0; k < 4; k++) {
        if (c139_line() && !(g_hw.irq_state & 4u)) c139_line_drop();     /* the game disabled the line under the chip */
        c139_tick();
        drain_tx();
        if (!(g_hw.irq_state & 4u)) break;
        rr_deliver_irqs(ss22_hw_irq_level);                              /* the handler runs now (it relays and sends the next queued frame) */
        if (g_hw.irq_state & 4u) break;                                  /* masked (the 68K is in a higher handler): next slice */
    }
}

/* ---- the ring ---- */
int      ss22_link_cabinet(void) { return me; }
void     ss22_link_set_cabinet(int id)
{
    if (id < 0 || id >= SS22_LINK_MAX_CABS) return;
    cab_mask = (cab_mask & ~(1u << me)) | (1u << id);
    me = id;
    if (g_ss22_game->link_cabinet) g_ss22_game->link_cabinet(id);
}
unsigned ss22_link_cabinets(void) { return cab_mask; }
void     ss22_link_set_cabinets(unsigned mask) { cab_mask = (mask | (1u << me)) & ((1u << SS22_LINK_MAX_CABS) - 1); }
int ss22_link_predecessor(void)
{
    for (int k = 1; k < SS22_LINK_MAX_CABS; k++) {
        int c = (me - k + SS22_LINK_MAX_CABS) % SS22_LINK_MAX_CABS;
        if (cab_mask & (1u << c)) return c;
    }
    return -1;
}

/* ---- the payload ---- */
static uint64_t pk_bytes; static uint32_t pk_n, pk_max;
int ss22_link_pack(uint8_t *buf, int max)
{
    drain_tx();
    if (max < 3) return 0;
    int n = 3, count = 0;
    while (outr != outw && count < 255) {
        const c139_frame *f = &outq[outr];
        int words = f->n > 255 ? 255 : f->n, need = 1 + (words + 7) / 8 + words;
        if (n + need > max) break;
        buf[n++] = (uint8_t)words;
        uint8_t *bits = &buf[n]; memset(bits, 0, (size_t)(words + 7) / 8); n += (words + 7) / 8;
        for (int k = 0; k < words; k++) {
            if (f->w[k] & 0x100) bits[k >> 3] |= (uint8_t)(1u << (k & 7));
            buf[n++] = (uint8_t)f->w[k];
        }
        outr = (outr + 1) % 64; count++;
    }
    buf[0] = 1; buf[1] = (uint8_t)me; buf[2] = (uint8_t)count;
    pk_bytes += (uint64_t)n; pk_n++; if ((uint32_t)n > pk_max) pk_max = (uint32_t)n;
    return n;
}

static uint32_t rx_frames, rx_bad;
bool ss22_link_unpack(int from_cab, const uint8_t *buf, int n)
{
    if (n < 3 || buf[0] != 1 || buf[1] != from_cab) { rx_bad++; return false; }
    const bool mine = from_cab == ss22_link_predecessor();
    int p = 3;
    for (int i = 0; i < buf[2]; i++) {
        if (p >= n) { rx_bad++; return false; }
        int words = buf[p++], nb = (words + 7) / 8;
        if (p + nb + words > n) { rx_bad++; return false; }
        if (mine) {
            c139_frame f; f.n = (uint16_t)words;
            for (int k = 0; k < words; k++) f.w[k] = (uint16_t)(buf[p + nb + k] | ((buf[p + (k >> 3)] >> (k & 7) & 1) << 8));
            c139_rx_put(&f);
            rx_frames++;
        }
        p += nb + words;
    }
    return true;
}

void ss22_link_set_external(void (*cb)(uint32_t)) { ext_cb = cb; }

/* ---- the built-in transport: UDP on 127.0.0.1, one port per cabinet, lockstep ---- */
#define HIST 128
static lk_sock sock = LK_BAD;
static int port_base = 27800, lockstep = 1;
static struct { uint8_t data[SS22_LINK_PAYLOAD_MAX]; int n; uint32_t frame; bool have; } hist[HIST];          /* mine, for re-sends */
static struct { uint8_t data[SS22_LINK_PAYLOAD_MAX]; int n; uint32_t frame; bool have; } got[SS22_LINK_MAX_CABS][HIST];
static uint32_t last_used[SS22_LINK_MAX_CABS];                                                                  /* free mode: frames taken */
static uint32_t n_sent, n_resent, n_reqs, n_waits; static double wait_s;

enum { MSG_DATA = 1, MSG_REQ = 2 };
static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

static void send_to(int cab, const uint8_t *m, int n)
{
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons((uint16_t)(port_base + cab)); a.sin_addr.s_addr = htonl(0x7F000001u);
    sendto(sock, (const char *)m, (size_t)n, 0, (struct sockaddr *)&a, sizeof a);
}
static void send_data(int cab, int slot)
{
    uint8_t m[SS22_LINK_PAYLOAD_MAX + 16];
    memcpy(m, "S2LK", 4); m[4] = MSG_DATA; m[5] = (uint8_t)me;
    uint32_t f = hist[slot].frame; m[6] = (uint8_t)f; m[7] = (uint8_t)(f >> 8); m[8] = (uint8_t)(f >> 16); m[9] = (uint8_t)(f >> 24);
    memcpy(&m[10], hist[slot].data, (size_t)hist[slot].n);
    send_to(cab, m, 10 + hist[slot].n);
}
static void send_req(int cab, uint32_t f)
{
    uint8_t m[10]; memcpy(m, "S2LK", 4); m[4] = MSG_REQ; m[5] = (uint8_t)me;
    m[6] = (uint8_t)f; m[7] = (uint8_t)(f >> 8); m[8] = (uint8_t)(f >> 16); m[9] = (uint8_t)(f >> 24);
    send_to(cab, m, 10); n_reqs++;
}

/* everything waiting on the socket; wait up to `ms` for the first datagram */
static void pump(int ms)
{
#ifndef _WIN32
    struct pollfd pf = { sock, POLLIN, 0 };
    if (ms > 0 && poll(&pf, 1, ms) <= 0) return;
#else
    if (ms > 0) { fd_set s; FD_ZERO(&s); FD_SET(sock, &s); struct timeval tv = { 0, ms * 1000 }; if (select(0, &s, NULL, NULL, &tv) <= 0) return; }
#endif
    uint8_t m[SS22_LINK_PAYLOAD_MAX + 16];
    for (;;) {
        int n = (int)recv(sock, (char *)m, sizeof m, 0);
        if (n < 10) return;
        if (memcmp(m, "S2LK", 4) || m[5] >= SS22_LINK_MAX_CABS) continue;
        int from = m[5]; uint32_t f = m[6] | m[7] << 8 | m[8] << 16 | (uint32_t)m[9] << 24;
        if (m[4] == MSG_DATA && n - 10 <= SS22_LINK_PAYLOAD_MAX) {
            int s = (int)(f % HIST);
            memcpy(got[from][s].data, &m[10], (size_t)(n - 10)); got[from][s].n = n - 10; got[from][s].frame = f; got[from][s].have = true;
        } else if (m[4] == MSG_REQ) {
            int s = (int)(f % HIST);
            if (hist[s].have && hist[s].frame == f) { send_data(from, s); n_resent++; }
        }
    }
}

static bool udp_open(void)
{
#ifdef _WIN32
    WSADATA w; if (WSAStartup(MAKEWORD(2, 2), &w)) return false;
#endif
    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock == LK_BAD) return false;
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons((uint16_t)(port_base + me)); a.sin_addr.s_addr = htonl(0x7F000001u);
    if (bind(sock, (struct sockaddr *)&a, sizeof a)) { fprintf(stderr, "[%s] link: cannot bind 127.0.0.1:%d\n", tagname, port_base + me); return false; }
    int big = 1 << 20; setsockopt(sock, SOL_SOCKET, SO_RCVBUF, (const char *)&big, sizeof big);
#ifdef _WIN32
    u_long one = 1; ioctlsocket(sock, FIONBIO, &one);
#else
    fcntl(sock, F_SETFL, fcntl(sock, F_GETFL, 0) | O_NONBLOCK);
#endif
    return true;
}

static void udp_frame(uint32_t f)
{
    int s = (int)(f % HIST);
    hist[s].n = ss22_link_pack(hist[s].data, SS22_LINK_PAYLOAD_MAX); hist[s].frame = f; hist[s].have = true;
    for (int c = 0; c < SS22_LINK_MAX_CABS; c++) if (c != me && (cab_mask & (1u << c))) send_data(c, s);
    n_sent++;
    int pred = ss22_link_predecessor();
    if (pred < 0) return;
    if (!lockstep) {                                       /* free-running: ONE of the predecessor's frames a frame, in order; a gap or a backlog */
        pump(0);                                           /* of more than 4 is skipped to the newest (the receiver must not take a burst) */
        uint32_t newest = 0; bool any = false;
        for (int k = 0; k < HIST; k++) if (got[pred][k].have && got[pred][k].frame > last_used[pred]) { any = true; if (got[pred][k].frame > newest) newest = got[pred][k].frame; }
        if (!any) return;
        uint32_t want = last_used[pred] + 1; int ws = (int)(want % HIST);
        if (!(got[pred][ws].have && got[pred][ws].frame == want) || newest - want > 4) { want = newest; ws = (int)(want % HIST); }
        ss22_link_unpack(pred, got[pred][ws].data, got[pred][ws].n);
        for (int k = 0; k < HIST; k++) if (got[pred][k].frame <= want) got[pred][k].have = false;
        last_used[pred] = want;
        return;
    }
    double t0 = now_s(), last_req = t0;
    const double limit = f < 900 ? 60.0 : 5.0;            /* the first frames wait for the other process to start */
    for (;;) {
        pump(0);
        if (got[pred][s].have && got[pred][s].frame == f) break;
        double t = now_s();
        if (t - last_req > 0.02) { send_req(pred, f); last_req = t; }
        if (t - t0 > limit) {
            fprintf(stderr, "[%s] link: cabinet %d sent nothing for frame %u in %.0f s -- dropped from the ring\n", tagname, pred, f, limit);
            cab_mask &= ~(1u << pred);
            return;
        }
        pump(2);
    }
    double w = now_s() - t0; if (w > 0.001) { n_waits++; wait_s += w; }
    ss22_link_unpack(pred, got[pred][s].data, got[pred][s].n);
    got[pred][s].have = false;
}

void ss22_link_frame(uint32_t f)
{
    drain_tx();
    if (ext_cb) ext_cb(f);
    else if (sock != LK_BAD) udp_frame(f);
    if (dbg && f % 60 == 0) {
        c139_stats st = c139_get_stats();
        fprintf(stderr, "[%s] link f%u cab %d ring %X pred %d | chip tx %u rx %u irq %u drop %u/%u | payload out avg %.0f max %u B, in %u frames, bad %u | sent %u resent %u req %u waits %u (%.2f s)\n",
                tagname, f, me, cab_mask, ss22_link_predecessor(), st.tx, st.rx, st.irq, st.tx_drop, st.rx_drop, pk_n ? (double)pk_bytes / pk_n : 0.0, pk_max, rx_frames, rx_bad,
                n_sent, n_resent, n_reqs, n_waits, wait_s);
        if (g_ss22_game->link_debug) g_ss22_game->link_debug(f);
    }
}

bool ss22_link_setup(const char *spec, const char *tag)
{
    snprintf(tagname, sizeof tagname, "%s", tag);
    if (!g_ss22_game->link_cabinet) { fprintf(stderr, "[%s] %s has no cabinet link\n", tag, g_ss22_game->name); return false; }
    { char v[64]; snprintf(v, sizeof v, "%s_LINKDBG", tag); const char *e = getenv(v); dbg = e && *e == '1'; }
    int id = 0, n = 0, port = 0;
    int k = sscanf(spec, "%d/%d:%d", &id, &n, &port);
    if (k < 1 || id < 0 || id >= SS22_LINK_MAX_CABS || (k >= 2 && (n < 1 || n > SS22_LINK_MAX_CABS || id >= n))) {
        fprintf(stderr, "[%s] --link %s: expected ID/N[:PORT][:free]\n", tag, spec); return false; }
    me = id; ncab = k >= 2 ? n : 1;
    if (k >= 3 && port > 0) port_base = port;
    if (strstr(spec, ":free")) lockstep = 0;
    cab_mask = (1u << ncab) - 1;
    if (k < 2) cab_mask = 1u << me;
    c139_init(g_ss22.sci, raise_sci);
    g_ss22_game->link_cabinet(me);                          /* the game's own setting: which cabinet this one is */
    g_ss22_link_on = true;
    if (k >= 2 && ncab > 1 && !udp_open()) return false;
    fprintf(stderr, "[%s] link: cabinet %d of %d%s%s\n", tag, me, ncab, k >= 2 && ncab > 1 ? (lockstep ? ", local lockstep" : ", local free-running") : "",
            k >= 2 && ncab > 1 ? "" : " (no built-in transport)");
    return true;
}
