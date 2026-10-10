/* tc2_out.c -- TIME CRISIS 2's OWN COPY of namco-2x-systems engine/ss22_out.c (copied 2026-10-05; TC2 is independent: change it here).
 * The cabinet's OUTPUTS leaving the game: the gun I/O board's output latch (0x6002, src/tc2_h8.c) published every frame with MAME's
 * network-output protocol (tcp 127.0.0.1:8000, "mame_start = timecrs2", then "name = value" on every change) for MAMEHooker / Hook of
 * the Reaper to drive a Sinden / GUN4IR / Blamcon gun's recoil, and a short kick on every connected pad when the gun solenoid fires.
 *   0x6002 bit 0 = the gun's RECOIL SOLENOID (measured: a 3-frame pulse on every shot the game accepts, none out of ammo / in cover)
 *   bits 1, 2   = the other two outputs MAME's namco_tss_io_device passes on (unnamed there)
 * Published names: gun_recoil (bit 0) and out0 / out1 / out2 (the raw bits). TC2_OUT_PORT=<n> (0 = off), TC2_OUT_BIND=<addr>,
 * TC2_RUMBLE=0 (no pad kick), TC2_OUTDBG=1 (log every change). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
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

#define MAX_CLIENTS 4
static out_sock lsock = OUT_BAD, clients[MAX_CLIENTS];
static int inited, dbg, rumble_on = 1;
static uint16_t prev;
static char rom[32];

static void nonblock(out_sock s)
{
#ifdef _WIN32
    u_long one = 1; ioctlsocket(s, FIONBIO, &one);
#else
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
#endif
}

static void init(void)
{
    inited = 1;
    const char *e = getenv("TC2_OUTDBG"); dbg = e && *e == '1';
    e = getenv("TC2_RUMBLE"); if (e && *e == '0') rumble_on = 0;
    for (int i = 0; i < MAX_CLIENTS; i++) clients[i] = OUT_BAD;
    snprintf(rom, sizeof rom, "timecrs2");                                     /* the set name MAME announces */
    int port = 8000; e = getenv("TC2_OUT_PORT"); if (e) port = atoi(e);
    if (port <= 0) return;
#ifdef _WIN32
    WSADATA w; if (WSAStartup(MAKEWORD(2, 2), &w) != 0) return;
#endif
    const char *bind_to = getenv("TC2_OUT_BIND"); if (!bind_to || !*bind_to) bind_to = "127.0.0.1";
    lsock = socket(AF_INET, SOCK_STREAM, 0);
    if (lsock == OUT_BAD) return;
    int one = 1; setsockopt(lsock, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof one);
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, bind_to, &a.sin_addr) != 1 || bind(lsock, (struct sockaddr *)&a, sizeof a) != 0 || listen(lsock, 4) != 0) {
        fprintf(stderr, "[OUT] output port %s:%d not available (another emulator running?): outputs are not published\n", bind_to, port);
        out_close(lsock); lsock = OUT_BAD; return;
    }
    nonblock(lsock);
    fprintf(stderr, "[OUT] outputs on tcp %s:%d (MAME network-output protocol, set '%s')\n", bind_to, port, rom);
}

static void send_all(const char *s)
{
    const int n = (int)strlen(s);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i] == OUT_BAD) continue;
        if (send(clients[i], s, n, SEND_FLAGS) != n) { out_close(clients[i]); clients[i] = OUT_BAD; }   /* gone, or too slow a reader: drop it */
    }
}

void tc2_out_poll(uint16_t outs)
{
    if (!inited) init();
    if (lsock != OUT_BAD) {
        for (;;) {                                           /* new listeners */
            out_sock c = accept(lsock, NULL, NULL);
            if (c == OUT_BAD) break;
            int slot = -1; for (int i = 0; i < MAX_CLIENTS; i++) if (clients[i] == OUT_BAD) { slot = i; break; }
            if (slot < 0) { out_close(c); continue; }
            nonblock(c);
            char b[64]; snprintf(b, sizeof b, "mame_start = %s\r", rom);
            if (send(c, b, (int)strlen(b), SEND_FLAGS) < 0) { out_close(c); continue; }
            clients[slot] = c;
        }
    }
    const uint16_t diff = (uint16_t)(outs ^ prev);
    if (!diff) return;
    for (int i = 0; i < 3; i++) {
        if (!(diff >> i & 1)) continue;
        char b[40]; snprintf(b, sizeof b, "out%d = %d\r", i, (outs >> i) & 1);
        if (dbg) fprintf(stderr, "[OUT] %s\n", b);
        send_all(b);
    }
    if (diff & 1) {                                          /* the gun solenoid */
        char b[40]; snprintf(b, sizeof b, "gun_recoil = %d\r", outs & 1);
        send_all(b);
        if ((outs & 1) && rumble_on) ss22_input_rumble(0xC000, 0xFFFF, 90);   /* a short, hard kick */
    }
    prev = outs;
}

void tc2_out_close(void)
{
    for (int i = 0; i < MAX_CLIENTS; i++) if (clients[i] != OUT_BAD) { out_close(clients[i]); clients[i] = OUT_BAD; }
    if (lsock != OUT_BAD) { out_close(lsock); lsock = OUT_BAD; }
}
