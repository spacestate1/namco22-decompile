/* ss22_out.c -- see ss22_out.h */
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
#include "ss22_game.h"
#include "ss22_input.h"
#include "ss22_out.h"
#include "lift_cpu.h"                                    /* rr_frame, for the SS22_OUTDBG log */

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
    const char *e = getenv("SS22_OUTDBG"); dbg = e && *e == '1';
    e = getenv("SS22_RUMBLE"); if (e && *e == '0') rumble_on = 0;
    for (int i = 0; i < MAX_CLIENTS; i++) clients[i] = OUT_BAD;
    snprintf(rom, sizeof rom, "%s", g_ss22_game->zip ? g_ss22_game->zip : "ss22");     /* "timecris.zip" -> the set name MAME announces */
    char *dot = strrchr(rom, '.'); if (dot) *dot = 0;
    int port = 8000; e = getenv("SS22_OUT_PORT"); if (e) port = atoi(e);
    if (port <= 0) return;
#ifdef _WIN32
    WSADATA w; if (WSAStartup(MAKEWORD(2, 2), &w) != 0) return;
#endif
    const char *bind_to = getenv("SS22_OUT_BIND"); if (!bind_to || !*bind_to) bind_to = "127.0.0.1";
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

void ss22_out_poll(uint16_t outs)
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
    for (int i = 0; i < 16; i++) {
        if (!(diff >> i & 1)) continue;
        char b[40]; snprintf(b, sizeof b, "mcuout%d = %d\r", i, (outs >> i) & 1);
        if (dbg) fprintf(stderr, "[OUT] f%u %s\n", rr_frame, b);
        send_all(b);
    }
    const uint16_t rising = (uint16_t)(diff & outs & g_ss22_game->recoil_mask);
    if (rising && rumble_on) ss22_input_rumble(0xC000, 0xFFFF, 90);              /* the gun / handle solenoid fired: a short, hard kick */
    if (rising) ss22_input_kick();                                               /* and on a force-feedback wheel (Tokyo Wars' handle) */
    prev = outs;
}

void ss22_out_close(void)
{
    for (int i = 0; i < MAX_CLIENTS; i++) if (clients[i] != OUT_BAD) { out_close(clients[i]); clients[i] = OUT_BAD; }
    if (lsock != OUT_BAD) { out_close(lsock); lsock = OUT_BAD; }
}
