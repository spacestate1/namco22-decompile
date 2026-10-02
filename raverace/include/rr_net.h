/* rr_net.h -- online play: the RRN1 client (raverace/NETPLAY.md).
 *
 * One UDP socket to a lobby/relay server; the lobby assigns this cabinet a
 * slot (its in-game link number) and a race session relays the game's own
 * 38-byte C139 link packets (src/rr_link.c) between the players. Driven by
 * rr_net_poll() once per frame from rr_tick (before rr_link_poll); with no
 * server configured it is inert -- no socket, one early-out branch a frame.
 */
#ifndef RR_NET_H
#define RR_NET_H
#include <stdbool.h>
#include <stddef.h>

/* Server address ("host" or "host:port", default port 27750). False = the
 * address did not resolve. Changing it while connected disconnects first. */
bool rr_net_set_server(const char *host_port);
const char *rr_net_server(void);
void rr_net_preset_server(const char *host_port);   /* remember an address without resolving it (resolved on the first connect) */
void rr_net_set_name(const char *name);      /* lobby name, max 16 bytes on the wire */
const char *rr_net_name(void);

bool rr_net_connect(void);                   /* OFFLINE -> CONNECTING; false = no server set */
void rr_net_disconnect(void);                /* LEAVE, back to OFFLINE */

void rr_net_status(char *buf, size_t n);     /* one line for the menu's Status row */
bool rr_net_connected(void);                 /* in the lobby or in a session */
bool rr_net_session_active(void);            /* GO received: a race is armed */

/* Lobby rows 0..7: false when i is past the roster. */
bool rr_net_roster(int i, char *name, size_t n, int *ready, int *self);
int  rr_net_roster_count(void);
void rr_net_set_ready(int ready);            /* READY, retransmitted until the roster confirms */
void rr_net_request_start(void);             /* START, retransmitted until GO */

/* Host a game on this LAN: starts the built-in server (rr_netd.c) and joins it. */
bool rr_net_host_start(void);                /* false = port busy */
void rr_net_host_stop(void);
bool rr_net_hosting(void);

/* Find games on the LAN (4 s search; results stay until the next one). */
void rr_net_discover(void);
bool rr_net_discovering(void);
void rr_net_discover_autojoin(int on);       /* headless: join the first host found */
int  rr_net_found_count(void);
bool rr_net_found(int i, char *label, size_t n, char *addr, size_t an);   /* addr = "ip:port" for rr_net_set_server */

/* Chat (lobby and race): the server relays a line to every member, the sender included. */
bool rr_net_chat_send(const char *text);     /* false = not connected / empty */
int  rr_net_chat_count(void);                /* lines kept (the last 64) */
bool rr_net_chat_line(int i, char *out, size_t n);   /* 0 = oldest; "Name: text" */
uint32_t rr_net_chat_serial(void);           /* grows with every line received */
void rr_net_chat_clear(void);

/* Rooms (rrn1-server only; the built-in LAN host is one room and sends no list): the list refreshes every 2 s in the lobby. */
int  rr_net_room_count(void);                /* 0 = no list (yet, or a server without rooms) */
bool rr_net_room(int i, int *id, int *state, int *players, int *mine, char *name, size_t n);   /* state 0 = lobby, 1 = racing; mine = the room you are in */
void rr_net_switch_room(int id);             /* leave this room, join that one (a refusal puts you in a free lobby) */
void rr_net_new_room(const char *name);      /* leave this room, open a new one called name (NULL / "" = the server names it) */
void rr_net_delete_room(int id);             /* delete an EMPTY room that is not racing (the server refuses otherwise); the list refreshes */
bool rr_net_switching(void);                 /* a room change is in flight (the lobby window stays up) */

void rr_net_apply_inputs(void);              /* once per simulated frame, after the host input: the automatic gas at GO */
void rr_net_poll_paused(void);               /* the same while the game is paused (menu): keepalive only, no link FRAMEs */
void rr_net_poll(void);                      /* once per frame, before rr_link_poll */

#endif
