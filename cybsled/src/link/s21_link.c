/*
 * s21_link.c -- see s21_link.h. Cyber Sled's cabinet link: the board's C139 is the SHARED engine model (engine/c139.c, Tokyo Wars' chip)
 * with the System 21 settings (C139_MASKED_IRQ | C139_PTR_LAST | C139_HOST_WORDS); this file is the board glue, the per-frame payload,
 * the lockstep exchange for two directly connected cabinets (CS_LINK) and the hand-over to the online transport (s21_net.c).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include "s21_link.h"
#include "s21_board.h"
#include "c139.h"

#ifndef _WIN32
#include <unistd.h>
#include <poll.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#endif

static struct {
    s21_link_state_t state;
    c139_frame out[S21_LINK_PKT_MAX]; int nout;          /* transmitted since the payload was last packed */
    int        delay, position, log;
    s21_link_transport t; int have_t;
    uint32_t   n_wait_ms, n_pkt_drop, n_rx_frames;
} L = { .position = -1, .delay = 1 };

static bool sci_irq(void) { s21_board_sci_irq(1); return true; }   /* the slave's C148 (SCI level 4 -> its handler 0x37D8) */

static void chip_on(void)
{
    c139_init((uint8_t *)g_s21.c139ram, sci_irq);
    c139_set_mode(C139_MASKED_IRQ | C139_PTR_LAST | C139_HOST_WORDS);
}

/* ================= the board's side ================= */
static uint16_t shadow[8];                               /* what was written, for byte-lane writes */

uint16_t s21_c139_reg_r(uint32_t a)
{
    if (a < 0xB80000 || a >= 0xB80010) return 0;
    const unsigned r = (a >> 1) & 7;
    if (L.state == S21_LINK_OFF) return r == 0 ? 4 : 0; /* unlinked: the stand-in the board always had (MAME's status_r) */
    return c139_reg_read(r);
}

void s21_c139_reg_w(uint32_t a, uint16_t d, uint16_t m)
{
    if (a < 0xB80000 || a >= 0xB80010 || L.state == S21_LINK_OFF) return;
    const unsigned r = (a >> 1) & 7;
    shadow[r] = (uint16_t)((shadow[r] & ~m) | (d & m));
    if (L.log > 2) fprintf(stderr, "[LINK3] f%u w reg%u %04X\n", g_s21.frame, r, shadow[r]);
    c139_reg_write(r, shadow[r]);
}

void s21_c139_slice(int slice)
{
    (void)slice;
    if (L.state == S21_LINK_OFF) return;
    c139_tick();
    c139_frame f;
    while (c139_tx_take(&f)) {
        if (L.log > 1) fprintf(stderr, "[LINK2] f%u TX %u words: %03X %03X %03X .. %03X\n", g_s21.frame, f.n, f.w[0], f.w[1], f.w[2], f.w[f.n - 1]);
        if (L.state == S21_LINK_LOOP) c139_rx_put(&f);   /* a cable from our TX to our RX */
        else if (L.state == S21_LINK_ON || L.state == S21_LINK_NET) {
            if (L.nout < S21_LINK_PKT_MAX) L.out[L.nout++] = f; else L.n_pkt_drop++;
        }                                                /* GONE / not in a session: an unplugged cable */
    }
}

int s21_link_ring_size(void) { return g_s21.c139ram[0x10] & 0xFF; }
s21_link_state_t s21_link_state(void) { return L.state; }

/* ================= the payload ================= */
size_t s21_link_pack_n(uint8_t *o, size_t cap, int max_pkts)
{
    size_t k = 0;
    if (cap < 1) return 0;
    o[k++] = 0;
    int i = 0;
    for (; i < L.nout && o[0] < max_pkts; i++) {
        const c139_frame *p = &L.out[i];
        const unsigned n = p->n > 255 ? 255 : p->n;
        const size_t need = 1 + n + (n + 7u) / 8;
        if (k + need > cap) break;
        o[k++] = (uint8_t)n;
        for (unsigned j = 0; j < n; j++) o[k++] = (uint8_t)p->w[j];
        for (unsigned j = 0; j < n; j += 8) {
            uint8_t b = 0;
            for (unsigned q = 0; q < 8 && j + q < n; q++) if (p->w[j + q] & 0x100) b |= (uint8_t)(1 << q);
            o[k++] = b;
        }
        o[0]++;
    }
    L.n_pkt_drop += (uint32_t)(L.nout - i);
    L.nout = 0;
    return k;
}
size_t s21_link_pack(uint8_t *o, size_t cap) { return s21_link_pack_n(o, cap, S21_LINK_PKT_MAX); }

bool s21_link_unpack(const uint8_t *in, size_t n)
{
    if (n < 1) return false;
    size_t k = 1;
    L.n_rx_frames++;
    for (int i = 0; i < in[0]; i++) {
        if (k >= n) return false;
        c139_frame p; p.n = in[k++];
        const size_t need = p.n + (p.n + 7u) / 8;
        if (k + need > n) return false;
        for (unsigned j = 0; j < p.n; j++) p.w[j] = in[k + j];
        for (unsigned j = 0; j < p.n; j++) if (in[k + p.n + j / 8] >> (j % 8) & 1) p.w[j] |= 0x100;
        k += need;
        if (L.log > 1) fprintf(stderr, "[LINK2] f%u RX %u words: %03X %03X %03X .. %03X\n", g_s21.frame, p.n, p.w[0], p.w[1], p.w[2], p.w[p.n - 1]);
        c139_rx_put(&p);
    }
    return true;                                         /* a fixed-size (online) payload is zero-padded past its packets */
}

/* ================= POSITION (the cabinet number) ================= */
void s21_link_set_position(int pos, bool live)
{
    if (pos < 0) return;
    L.position = pos & 1;
    g_s21.nvram[0x12] = (uint8_t)((g_s21.nvram[0x12] & ~1) | (pos & 1));   /* GAME OPTIONS > POSITION: NVRAM 0x180024 bit 0 */
    uint8_t sum = 0;                                     /* the settings block's check byte (master 0x21AA): the low byte of the */
    for (int i = 0; i < 0x16; i++) sum = (uint8_t)(sum + g_s21.nvram[i]);   /* sum of words 0x180000..0x18002A = byte 0x16 */
    g_s21.nvram[0x16] = sum;
    if (live) {                                          /* a running game: also the working copies the program read at boot */
        g_s21.shared[0x228 / 2] = (uint16_t)(pos & 1);   /* 0x900228: the slave's slot (0x3188) */
        g_s21.shared[0x204 / 2] = (uint16_t)(pos & 1);   /* 0x900204: the cabinet number (master 0x267A) */
        g_s21.shared[0x20E / 2] = (uint16_t)((pos & 1) << 8);   /* 0x90020E: its block offset in 0x904000 */
    }
}

/* ================= lockstep (CS_LINK: two cabinets, no server) ================= */
static double now_ms(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6; }

static void go_gone(const char *why, uint32_t f)
{
    fprintf(stderr, "[LINK] f%u: the other cabinet is gone (%s) -- the link is now an unplugged cable\n", f, why);
    L.state = S21_LINK_GONE;
}

void s21_net_frame(uint32_t frame);                      /* s21_net.c: eng_net_poll + stall-and-wait (online) */

static int net_on;                                      /* s21_net.c is initialised: poll it every frame, whatever the chip's state */
void s21_link_net_poll_on(void) { net_on = 1; }

void s21_link_frame_begin(uint32_t f)
{
    if (L.state == S21_LINK_OFF) { if (net_on) s21_net_frame(f); return; }
    if (f == 0 && L.position >= 0) s21_link_set_position(L.position, false);
    if (net_on && !L.have_t) { s21_net_frame(f); return; }
    if (L.state != S21_LINK_ON || (int)f < L.delay) return;
    static uint8_t buf[S21_LINK_PAYLOAD_MAX];
    static int first = 1;
    const uint32_t want = f - (uint32_t)L.delay;
    const double t0 = now_ms();
    const char *e;
    const double limit = first ? ((e = getenv("CS_LINK_WAIT")) ? atof(e) * 1000 : 120000.0)
                               : ((e = getenv("CS_LINK_TIMEOUT")) ? atof(e) * 1000 : 5000.0);
    for (;;) {
        int n = L.t.recv(L.t.ctx, want, buf, sizeof buf);
        if (n >= 0) {
            first = 0;
            if (!s21_link_unpack(buf, (size_t)n)) fprintf(stderr, "[LINK] f%u: bad payload for frame %u (%d bytes)\n", f, want, n);
            break;
        }
        if (n == -2) { go_gone("closed", f); return; }
        if (now_ms() - t0 > limit) { go_gone("timeout", f); return; }
        if (L.t.poll) L.t.poll(L.t.ctx, 5);
    }
    L.n_wait_ms += (uint32_t)(now_ms() - t0);
}

void s21_link_frame_end(uint32_t f)
{
    if (L.state == S21_LINK_OFF) return;
    if (L.state == S21_LINK_ON) {
        static uint8_t buf[S21_LINK_PAYLOAD_MAX];
        size_t n = s21_link_pack(buf, sizeof buf);
        L.t.send(L.t.ctx, f, buf, n);
        if (L.t.poll) L.t.poll(L.t.ctx, 0);
    } else if (L.state != S21_LINK_NET) L.nout = 0;
    if (L.log) {                                         /* transitions of what the game derives from the link */
        static uint32_t last = 0xFFFFFFFF;
        const uint32_t now = (uint32_t)s21_link_ring_size() << 16 | (uint32_t)(g_s21.c139ram[0x11] & 0xFF) << 8 | (g_s21.c139ram[0x12] & 0xFF);
        if (now != last) fprintf(stderr, "[LINK] f%u ring %u heard %02X%02X\n", f, now >> 16, now >> 8 & 0xFF, now & 0xFF);
        last = now;
        if ((f + 1) % 600 == 0) {
            c139_stats s = c139_get_stats();
            fprintf(stderr, "[LINK] f%u %s: tx %u rx %u irq %u rx-drop %u pkt-drop %u payloads in %u wait %u ms | ring %d 900208 %u\n", f + 1,
                    L.state == S21_LINK_ON ? "lockstep" : L.state == S21_LINK_NET ? "online" : L.state == S21_LINK_LOOP ? "loop" : "unplugged",
                    s.tx, s.rx, s.irq, s.rx_drop, L.n_pkt_drop, L.n_rx_frames, L.n_wait_ms, s21_link_ring_size(), g_s21.shared[0x104]);
        }
    }
    { static FILE *df; static int init;                  /* CS_LINKDUMP=file: shared 0x904000..0x9041FF after every frame (tests) */
      if (!init) { init = 1; const char *e = getenv("CS_LINKDUMP"); if (e) df = fopen(e, "wb"); }
      if (df) fwrite(&g_s21.shared[0x2000], 2, 0x100, df); }
}

bool s21_link_attach(const s21_link_transport *t, int position, int delay)
{
    if (L.have_t) return false;
    L.t = *t; L.have_t = 1;
    L.position = position; L.delay = delay < 1 ? 1 : delay;
    L.state = S21_LINK_ON;
    chip_on();
    return true;
}

void s21_link_detach(void)
{
    if (L.have_t && L.t.close) L.t.close(L.t.ctx);
    L.have_t = 0;
    if (L.state == S21_LINK_ON) L.state = S21_LINK_GONE;
}

/* online (s21_net.c): the chip runs from power-on, as a cabinet with its link cable unplugged until a session starts */
void s21_link_net_mode(void) { if (L.state == S21_LINK_OFF) chip_on(); L.state = S21_LINK_GONE; }
void s21_link_chip_on_live(void)                         /* a session begins: the chip from its reset state (the board is reset next) */
{
    chip_on();
    memset(shadow, 0, sizeof shadow);
    if (L.state == S21_LINK_OFF) L.state = S21_LINK_GONE;
}
void s21_link_unplug(bool unplugged) { if (!L.have_t && L.state != S21_LINK_OFF) { L.state = unplugged ? S21_LINK_GONE : S21_LINK_NET; L.nout = 0; } }

/* ================= UDP transport for the lockstep link =================
 * datagram: "S21L" u8 count, count x { u32 frame (LE), u16 len, payload } -- our last 4 frames each time, so a lost datagram is
 * covered by the next; while waiting, poll() resends the newest every 20 ms (both sides waiting on each other unblock). */
#ifndef _WIN32
typedef struct {
    int fd; struct sockaddr_storage peer; socklen_t plen;
    struct { uint32_t f; uint16_t n; uint8_t d[S21_LINK_PAYLOAD_MAX]; int ok; } rx[64], tx[4];
    int ntx; double last_send;
} udp_t;

static void udp_flush(udp_t *u)
{
    static uint8_t dg[8 + 4 * (6 + S21_LINK_PAYLOAD_MAX)];
    size_t k = 5;
    memcpy(dg, "S21L", 4); dg[4] = 0;
    for (int i = 0; i < 4 && i < u->ntx; i++) {
        const int j = (u->ntx - 1 - i) & 3;
        if (!u->tx[j].ok) break;
        if (k + 6 + u->tx[j].n > 1400 && dg[4]) break;  /* keep a datagram small; the newest always goes */
        const uint32_t f = u->tx[j].f;
        dg[k++] = (uint8_t)f; dg[k++] = (uint8_t)(f >> 8); dg[k++] = (uint8_t)(f >> 16); dg[k++] = (uint8_t)(f >> 24);
        dg[k++] = (uint8_t)u->tx[j].n; dg[k++] = (uint8_t)(u->tx[j].n >> 8);
        memcpy(dg + k, u->tx[j].d, u->tx[j].n); k += u->tx[j].n;
        dg[4]++;
    }
    if (dg[4]) sendto(u->fd, dg, k, 0, (struct sockaddr *)&u->peer, u->plen);
    u->last_send = now_ms();
}
static void udp_send(void *c, uint32_t f, const uint8_t *p, size_t n)
{
    udp_t *u = c;
    const int j = u->ntx & 3;
    u->tx[j].f = f; u->tx[j].n = (uint16_t)n; memcpy(u->tx[j].d, p, n); u->tx[j].ok = 1;
    u->ntx++;
    udp_flush(u);
}
static void udp_drain(udp_t *u)
{
    static uint8_t dg[65536];
    for (;;) {
        ssize_t r = recv(u->fd, dg, sizeof dg, MSG_DONTWAIT);
        if (r < 0) return;
        if (r < 5 || memcmp(dg, "S21L", 4)) continue;
        size_t k = 5;
        for (int i = 0; i < dg[4]; i++) {
            if (k + 6 > (size_t)r) break;
            const uint32_t f = dg[k] | dg[k + 1] << 8 | dg[k + 2] << 16 | (uint32_t)dg[k + 3] << 24;
            const uint16_t n = (uint16_t)(dg[k + 4] | dg[k + 5] << 8);
            k += 6;
            if (k + n > (size_t)r || n > S21_LINK_PAYLOAD_MAX) break;
            const int s = f & 63;
            if (!u->rx[s].ok || u->rx[s].f != f) { u->rx[s].f = f; u->rx[s].n = n; memcpy(u->rx[s].d, dg + k, n); u->rx[s].ok = 1; }
            k += n;
        }
    }
}
static int udp_recv(void *c, uint32_t f, uint8_t *p, size_t cap)
{
    udp_t *u = c;
    udp_drain(u);
    const int s = f & 63;
    if (!u->rx[s].ok || u->rx[s].f != f || u->rx[s].n > cap) return -1;
    memcpy(p, u->rx[s].d, u->rx[s].n);
    return u->rx[s].n;
}
static void udp_poll(void *c, int ms)
{
    udp_t *u = c;
    if (ms > 0) { struct pollfd pf = { u->fd, POLLIN, 0 }; poll(&pf, 1, ms); }
    if (ms > 0 && u->ntx && now_ms() - u->last_send > 20) udp_flush(u);
}
static void udp_close(void *c) { udp_t *u = c; close(u->fd); free(u); }

static bool resolve(const char *hp, struct sockaddr_storage *sa, socklen_t *len, int passive)
{
    char h[256]; const char *colon = strrchr(hp, ':');
    if (!colon || (size_t)(colon - hp) >= sizeof h) return false;
    memcpy(h, hp, (size_t)(colon - hp)); h[colon - hp] = 0;
    struct addrinfo hints = { 0 }, *res;
    hints.ai_family = AF_INET; hints.ai_socktype = SOCK_DGRAM; if (passive) hints.ai_flags = AI_PASSIVE;
    if (getaddrinfo(h[0] ? h : NULL, colon + 1, &hints, &res)) return false;
    memcpy(sa, res->ai_addr, res->ai_addrlen); *len = res->ai_addrlen;
    freeaddrinfo(res);
    return true;
}
#endif

bool s21_net_boot(void);                                 /* s21_net.c: CS_NET_* */

bool s21_link_init_env(void)
{
    const char *e;
    L.log = (e = getenv("CS_LINKLOG")) ? atoi(e) : 0;
    L.position = (e = getenv("CS_LINK_POS")) ? atoi(e) : -1;
    L.delay = (e = getenv("CS_LINK_DELAY")) ? atoi(e) : 1;
    if (L.delay < 1) L.delay = 1;
    if (s21_net_boot()) return true;                     /* online (engine/net.c) */
    e = getenv("CS_LINK");
    if (!e || !*e) return false;
    if (!strcmp(e, "loop")) { chip_on(); L.state = S21_LINK_LOOP; fprintf(stderr, "[LINK] loopback cable (our TX to our RX)\n"); return true; }
#ifndef _WIN32
    const char *comma = strchr(e, ',');
    if (!comma) { fprintf(stderr, "[LINK] CS_LINK=<bindhost:port>,<peerhost:port> or loop\n"); return false; }
    char bind_s[300]; snprintf(bind_s, sizeof bind_s, "%.*s", (int)(comma - e), e);
    udp_t *u = calloc(1, sizeof *u);
    struct sockaddr_storage me; socklen_t mlen;
    if (!resolve(bind_s, &me, &mlen, 1) || !resolve(comma + 1, &u->peer, &u->plen, 0)) { fprintf(stderr, "[LINK] bad address in CS_LINK\n"); free(u); return false; }
    u->fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (u->fd < 0 || bind(u->fd, (struct sockaddr *)&me, mlen) < 0) { fprintf(stderr, "[LINK] bind %s: %s\n", bind_s, strerror(errno)); free(u); return false; }
    s21_link_transport t = { u, udp_send, udp_recv, udp_poll, udp_close };
    s21_link_attach(&t, L.position, L.delay);
    fprintf(stderr, "[LINK] lockstep UDP %s <-> %s, position %d (%s), delay %d frame(s)\n", bind_s, comma + 1, L.position,
            L.position < 0 ? "from the NVRAM" : L.position ? "RIGHT" : "LEFT", L.delay);
    return true;
#else
    fprintf(stderr, "[LINK] no UDP transport in this build\n");
    return false;
#endif
}
