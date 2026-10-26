/* tc2_out.c -- the System 23 host's CABINET OUTPUTS (Time Crisis 2, Crisis Zone), published with MAME's network-output protocol
 * (tcp 127.0.0.1:8000: "mame_start = <set>" on connect, then "name = value" on every change, "mame_stop = 1" at exit) for Hook of the
 * Reaper / MAMEHooker to drive a Sinden / GUN4IR / Blamcon / OpenFIRE gun's recoil, and a short kick on every connected pad.
 * (Was TC2's own copy of engine/ss22_out.c, 2026-10-05.)
 *   gun_recoil   the I/O board's latch 0x6002 bit 0 = the gun's RECOIL (TC2: a 3-frame pulse on every shot the game accepts; Crisis Zone:
 *                bit 0 is on whenever the player is out of cover, so gun_recoil = bit 0 while the trigger is held -- GitHub #59)
 *   out0..out2   the latch's raw bits (MAME's namco_tss_io_device output_w, unnamed there)
 *   analog0      Crisis Zone's I/O board (namco_csz1) analog output 0x6003 (MAME: analog_output_w)
 *   lamp0..lamp7 the main board's LEDs (0x0D000000), the names MAME itself publishes for these sets (namcos23.cpp ctl_w: lamp i = bit 7-i)
 * MAME 0.289 leaves the I/O board's output callbacks unconnected for timecrs2 and crszone, so a MAME game file has no recoil signal to map:
 * tc2/packaging/hotr/<set>.txt are ready Hook of the Reaper DefaultLG files for gun_recoil (GitHub #48).
 * THE GAME LOOP NEVER TOUCHES A SOCKET (2026-10-12: a player measured constant micro-stutter in Crisis Zone with the outputs on and none
 * with them off): poll() only queues "name = value" lines; a thread of its own listens, accepts (TCP_NODELAY) and sends. The lamps
 * changed about every other frame and were sent to Hook of the Reaper all the time; they are off unless Display > Lamp outputs is on.
 * Display > Recoil outputs (out_net) / Lamp outputs (out_lamps) / Pad rumble on recoil (out_rumble), saved in the game's cfg; TC2_OUT_PORT=<n> (0 = off),
 * TC2_OUT_BIND=<addr>, TC2_RUMBLE=0, TC2_OUTDBG=1 (log every change). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET out_sock;
#define OUT_BAD INVALID_SOCKET
#define out_close closesocket
#define SEND_FLAGS 0
#else
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
typedef int out_sock;
#define OUT_BAD (-1)
#define out_close close
#ifdef MSG_NOSIGNAL
#define SEND_FLAGS MSG_NOSIGNAL
#else
#define SEND_FLAGS 0
#endif
#endif
#include "eng/ss22_input.h"
#include <stdbool.h>
#include "tc2_game.h"

#define MAX_CLIENTS 4
int tc2_out_net = 1, tc2_out_rumble = 1, tc2_out_lamps = 0;   /* Display > Recoil outputs / Pad rumble on recoil / Lamp outputs */
static int inited, dbg, port_env = -1;
static uint16_t prev;
static uint8_t prev_leds, prev_aout;
static int have_prev;
static char rom[32];

/* ---- the queue between the game loop and the sender thread ---- */
#define QN 256
static char q[QN][48]; static int qhead, qtail;                 /* qhead: next to write (game loop), qtail: next to send (thread) */
static pthread_mutex_t qm = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t qc = PTHREAD_COND_INITIALIZER;
static int want_net, quit_req, thread_up, open_failed;   /* open_failed: the port was taken -- retried only when the menu turns it on again */
static pthread_t thr;

static void queue_line(const char *line)
{
    pthread_mutex_lock(&qm);
    const int nx = (qhead + 1) % QN;
    if (nx != qtail) { snprintf(q[qhead], sizeof q[0], "%s", line); qhead = nx; }   /* full (no listener reading): drop, never wait */
    pthread_cond_signal(&qc);
    pthread_mutex_unlock(&qm);
}

/* ---- the sender thread: everything with a socket happens here ---- */
static out_sock lsock = OUT_BAD, clients[MAX_CLIENTS];

static void nonblock(out_sock s)
{
#ifdef _WIN32
    u_long one = 1; ioctlsocket(s, FIONBIO, &one);
#else
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
#endif
}
static void send_all(const char *s)
{
    const int n = (int)strlen(s);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i] == OUT_BAD) continue;
        if (send(clients[i], s, n, SEND_FLAGS) != n) { out_close(clients[i]); clients[i] = OUT_BAD; }   /* gone, or too slow a reader: drop it */
    }
}
static void net_open(void)
{
    const int port = port_env >= 0 ? port_env : 8000;
    if (port <= 0 || lsock != OUT_BAD) return;
#ifdef _WIN32
    static int wsa; if (!wsa) { WSADATA w; if (WSAStartup(MAKEWORD(2, 2), &w) != 0) return; wsa = 1; }
#endif
    const char *bind_to = getenv("TC2_OUT_BIND"); if (!bind_to || !*bind_to) bind_to = "127.0.0.1";
    lsock = socket(AF_INET, SOCK_STREAM, 0);
    if (lsock == OUT_BAD) return;
    int one = 1; setsockopt(lsock, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof one);
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, bind_to, &a.sin_addr) != 1 || bind(lsock, (struct sockaddr *)&a, sizeof a) != 0 || listen(lsock, 4) != 0) {
        fprintf(stderr, "[OUT] output port %s:%d not available (another emulator running?): outputs are not published\n", bind_to, port);
        out_close(lsock); lsock = OUT_BAD;
        pthread_mutex_lock(&qm); open_failed = 1; pthread_mutex_unlock(&qm);
        return;
    }
    nonblock(lsock);
    fprintf(stderr, "[OUT] outputs on tcp %s:%d (MAME network-output protocol, set '%s')\n", bind_to, port, rom);
}
static void net_close(void)
{
    send_all("mame_stop = 1\r");                            /* what MAME sends at exit: Hook of the Reaper closes the gun's port on it */
    for (int i = 0; i < MAX_CLIENTS; i++) if (clients[i] != OUT_BAD) { out_close(clients[i]); clients[i] = OUT_BAD; }
    if (lsock != OUT_BAD) { out_close(lsock); lsock = OUT_BAD; }
}
static void *sender(void *arg)
{
    (void)arg;
    char batch[QN][48]; int nb;
    for (;;) {
        pthread_mutex_lock(&qm);
        if (qtail == qhead && !quit_req) {                   /* nothing to send: wake at least every 50 ms to accept a listener */
            struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 50 * 1000000L; if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
            pthread_cond_timedwait(&qc, &qm, &ts);
        }
        nb = 0; while (qtail != qhead) { memcpy(batch[nb++], q[qtail], sizeof q[0]); qtail = (qtail + 1) % QN; }
        const int quit = quit_req, net = want_net && !open_failed;
        pthread_mutex_unlock(&qm);
        if (net && !quit && lsock == OUT_BAD) net_open();     /* once: a taken port is not retried every 50 ms (it filled the log) */
        if (!net && lsock != OUT_BAD) net_close();
        if (lsock != OUT_BAD) for (;;) {                     /* new listeners */
            out_sock c = accept(lsock, NULL, NULL);
            if (c == OUT_BAD) break;
            int slot = -1; for (int i = 0; i < MAX_CLIENTS; i++) if (clients[i] == OUT_BAD) { slot = i; break; }
            if (slot < 0) { out_close(c); continue; }
            nonblock(c);
            int one = 1; setsockopt(c, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);   /* a recoil goes out now, not batched */
            char b[64]; snprintf(b, sizeof b, "mame_start = %s\r", rom);
            if (send(c, b, (int)strlen(b), SEND_FLAGS) < 0) { out_close(c); continue; }
            clients[slot] = c;
        }
        for (int i = 0; i < nb; i++) send_all(batch[i]);
        if (quit) { net_close(); return NULL; }
    }
}

static void init(void)
{
    inited = 1;
    const char *e = getenv("TC2_OUTDBG"); dbg = e && *e == '1';
    e = getenv("TC2_RUMBLE"); if (e && *e == '0') tc2_out_rumble = 0;
    for (int i = 0; i < MAX_CLIENTS; i++) clients[i] = OUT_BAD;
    snprintf(rom, sizeof rom, "%s", tc2_game.mame_set);                                     /* the set name MAME announces */
    e = getenv("TC2_OUT_PORT"); if (e) port_env = atoi(e);
    e = getenv("TC2_OUT_LAMPS"); if (e) tc2_out_lamps = *e == '1';
    want_net = tc2_out_net && port_env != 0;
    if (pthread_create(&thr, NULL, sender, NULL) == 0) thread_up = 1;
}

/* the menu's settings, read with the rest of the game's cfg (eng/ss22_input.c) */
void tc2_out_load(void)
{
    int eng_cfg_int(const char *, int);
    tc2_out_net = eng_cfg_int("out_net", 1) != 0;
    tc2_out_rumble = eng_cfg_int("out_rumble", 1) != 0;
    tc2_out_lamps = eng_cfg_int("out_lamps", 0) != 0;
    const char *e = getenv("TC2_RUMBLE"); if (e && *e == '0') tc2_out_rumble = 0;
}
void tc2_out_set_net(int on)
{
    bool eng_cfg_set_int(const char *, int);
    tc2_out_net = on != 0; eng_cfg_set_int("out_net", tc2_out_net);
    pthread_mutex_lock(&qm); want_net = tc2_out_net && port_env != 0; open_failed = 0; pthread_cond_signal(&qc); pthread_mutex_unlock(&qm);   /* off: port 8000 is free; on: try the port again */
}
void tc2_out_set_rumble(int on)
{
    bool eng_cfg_set_int(const char *, int);
    tc2_out_rumble = on != 0; eng_cfg_set_int("out_rumble", tc2_out_rumble);
}
void tc2_out_set_lamps(int on)
{
    bool eng_cfg_set_int(const char *, int);
    tc2_out_lamps = on != 0; eng_cfg_set_int("out_lamps", tc2_out_lamps);
    have_prev = 0;                                           /* resend the current state of everything */
}

static void pub(const char *name, int v)
{
    char b[48]; snprintf(b, sizeof b, "%s = %d\r", name, v);
    if (dbg) { extern uint32_t rr_frame; fprintf(stderr, "[OUT] f%u %s\n", rr_frame, b); }
    if (tc2_out_net) queue_line(b);
}

/* once a frame, from the game loop: queue what changed (no socket here) */
void tc2_out_poll(uint16_t outs)
{
    extern uint8_t tc2_ctl_leds; uint8_t tc2_h8_aout(void);
    if (!inited) init();
    const uint8_t leds = tc2_ctl_leds, aout = tc2_h8_aout();
    const uint16_t diff = have_prev ? (uint16_t)(outs ^ prev) : 0x7;
    const uint8_t ldiff = have_prev ? (uint8_t)(leds ^ prev_leds) : 0xFF;
    if (tc2_out_lamps) for (int i = 0; i < 8; i++) if (ldiff >> (7 - i) & 1) { char n[8]; snprintf(n, sizeof n, "lamp%d", i); pub(n, leds >> (7 - i) & 1); }
    if (tc2_out_lamps && tc2_game.io_analog_out && (!have_prev || aout != prev_aout)) pub("analog0", aout);
    for (int i = 0; i < 3; i++) if (diff >> i & 1) { char n[8]; snprintf(n, sizeof n, "out%d", i); pub(n, (outs >> i) & 1); }
    /* the gun solenoid. Crisis Zone (recoil_hold): bit 0 is on whenever the player is OUT OF COVER, not only while the machine gun fires
     * (GitHub #59: the recoil went off on the pedal alone) -- so its recoil is bit 0 AND the trigger held */
    static int prev_rec;
    extern uint16_t tc2_inputs;
    const int rec = (outs & 1) && (!tc2_game.recoil_hold || (tc2_inputs & 0x0010));
    if (!have_prev || rec != prev_rec) {
        pub("gun_recoil", rec);
        if (have_prev && rec && tc2_out_rumble) ss22_input_rumble(0xC000, 0xFFFF, 90);   /* a short, hard kick */
    }
    if (tc2_game.recoil_hold && rec && tc2_out_rumble) {     /* a machine gun: keep the pad shaking while it fires */
        static unsigned n; if (++n % 5 == 0) ss22_input_rumble(0x9000, 0xD000, 110);
    }
    prev_rec = rec;
    prev = outs; prev_leds = leds; prev_aout = aout; have_prev = 1;
}

void tc2_out_close(void)
{
    if (!thread_up) return;
    pthread_mutex_lock(&qm); quit_req = 1; pthread_cond_signal(&qc); pthread_mutex_unlock(&qm);
    pthread_join(thr, NULL); thread_up = 0;                 /* the thread sends "mame_stop = 1" and closes */
}
