/* net.h -- ONLINE PLAY for every game: the NMN2 client (docs/NETPLAY.md), shared by Rave Racer, Tokyo Wars, Cyber Sled, ...
 *
 * One UDP socket to a lobby/relay server (server/, `nmn-server`; or the built-in LAN host, engine/net_host.c). A game describes
 * itself once (eng_net_game: its id, link version, room size, the size of its per-frame payload, and a few hooks), then calls
 * eng_net_poll() once per frame. The server puts the player in a ROOM of this game only; the lobby assigns a SLOT (= the cabinet
 * number in the linked game); START sends GO; in the session every poll asks the game for this frame's payload (frame_out) and
 * hands it every peer's newer payload (frame_in). Nothing is lockstepped: like a real cabinet link, each machine runs at its own
 * pace and sees the others' newest packets, the game's own link protocol copes with loss and lateness.
 *
 * Inert by default: no server set -> OFFLINE -> eng_net_poll is a couple of branches, no socket, no syscalls.
 * Rave Racer additionally speaks the old RRN1 protocol to old servers / old built-in hosts (rrn1 = 1 in its eng_net_game).
 *
 * Test switches, read from the environment with the game's prefix (e.g. "TW_NET"): <P>_DEBUG=1 (state transitions, roster,
 * frame counters), <P>_AUTOSTART=1|N (Ready + START once N >= 2 players have sat in the lobby ~2 s), <P>_SAY=text (one chat
 * line in the lobby, one in the session), <P>_ROOM=new[:name]|<id> and <P>_RENAME=name (1.5 s into the lobby),
 * <P>_SIM=loss%,stall_ms[,burst] + <P>_SIM_SEED (a bad Wi-Fi link on the receive side).
 */
#ifndef ENG_NET_H
#define ENG_NET_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ENG_NET_PORT        27750       /* the default UDP port: servers, built-in hosts and LAN discovery */
#define ENG_NET_MAX_PLAYERS 16          /* slots in a room, at most (the server caps it per game: Rave Racer 8, Cyber Sled 2) */
#define ENG_NET_MAX_FRAME   1024        /* a per-frame payload, at most (the server caps it per game) */

typedef struct eng_net_game {
    char     id[3];                 /* the game id: two lowercase letters/digits ("rr", "tw", "cs", "ad", "dd", "tc", "pc") */
    uint16_t ver;                   /* the link version: only equal versions share a room (bump it when the payload changes) */
    uint8_t  max_players;           /* the room size this game asks for when it creates a room */
    uint16_t frame_size;            /* every FRAME carries exactly this many payload bytes */
    const char *env;                /* prefix of the test switches, e.g. "TW_NET" */
    const char *session_word;       /* what a session is called in messages: "race", "battle", "game" */
    int      rrn1;                  /* Rave Racer only: also speak RRN1 (old servers / old LAN hosts); frame_size must be 40 */
    /* hooks (all optional) */
    void (*session_begin)(int slot, int players);       /* GO: this machine is cabinet `slot` of a linked game of `players` */
    void (*session_end)(void);                           /* the session is over: the server ended it, we left, or the link is gone */
    int  (*frame_out)(uint8_t *buf);                     /* once per eng_net_poll in a session (not paused): fill frame_size bytes and
                                                            return 1 to send them, 0 = nothing to send this frame */
    void (*frame_in)(int slot, const uint8_t *buf, uint32_t frame_seq);   /* a peer's payload, newer than the last one from that slot */
    void (*hint)(const char *text, int frames);          /* a one-line message for the player (the game's on-screen hint) */
} eng_net_game;

void eng_net_init(const eng_net_game *g);                /* once, before anything else; the struct must outlive the program */
const eng_net_game *eng_net_game_info(void);

/* Server address ("host" or "host:port", default port 27750). False = it did not resolve. Changing it while connected
 * disconnects first. preset = remember it without resolving (a DNS lookup at every boot would stall an offline start). */
bool eng_net_set_server(const char *host_port);
void eng_net_preset_server(const char *host_port);
const char *eng_net_server(void);
void eng_net_set_name(const char *name);                 /* max 16 bytes on the wire; while connected the roster updates */
const char *eng_net_name(void);

bool eng_net_connect(void);                              /* OFFLINE -> CONNECTING (joins the fullest open lobby of this game, or a new room) */
void eng_net_disconnect(void);                           /* LEAVE, back to OFFLINE */
void eng_net_status(char *buf, size_t n);                /* one line for a menu */
bool eng_net_connected(void);                            /* in a lobby or a session */
bool eng_net_session_active(void);                       /* GO received */
int  eng_net_slot(void);                                 /* our slot, -1 = none */
int  eng_net_ping_ms(void);                              /* -1 = unknown */

/* the lobby */
int  eng_net_roster_count(void);
bool eng_net_roster(int i, char *name, size_t n, int *ready, int *self);   /* false past the roster */
int  eng_net_roster_slot(int i);
void eng_net_set_ready(int ready);                       /* retransmitted until the roster confirms */
bool eng_net_self_ready(void);
bool eng_net_all_ready(void);                            /* 2+ players, every one Ready */
void eng_net_request_start(void);                        /* refused (a note in the status) until everyone is Ready */

/* rooms: the list of THIS game's rooms refreshes every 2 s in the lobby */
int  eng_net_room_count(void);
bool eng_net_room(int i, int *id, int *state, int *players, int *mine, char *name, size_t n);   /* state 0 lobby, 1 in a session */
int  eng_net_room_max(int i);                            /* the room's size (8 for an RRN1 server) */
bool eng_net_room_compatible(int i);                     /* the same link version as ours (only those can be joined) */
void eng_net_switch_room(int id);                        /* leave this room, join that one (a refusal puts us in a free lobby) */
void eng_net_new_room(const char *name);                 /* leave this room, open a new one (NULL / "" = the server names it) */
void eng_net_delete_room(int id);                        /* an EMPTY room not in a session; the list refreshes */
bool eng_net_switching(void);                            /* a room change is in flight */

/* chat (lobby and session): the server relays a line to every member, the sender included */
bool eng_net_chat_send(const char *text);
int  eng_net_chat_count(void);                           /* the last 64 lines */
bool eng_net_chat_line(int i, char *out, size_t n);      /* 0 = oldest: "[HH:MM] Name: text" */
uint32_t eng_net_chat_serial(void);                      /* grows with every line */
void eng_net_chat_clear(void);

/* the LAN: host a game (the built-in server, engine/net_host.c, on UDP 27750, joined over loopback) and find hosts */
bool eng_net_host_start(void);                           /* false = the port is busy */
void eng_net_host_stop(void);
bool eng_net_hosting(void);
void eng_net_discover(void);                             /* a 4 s search; the results stay until the next one */
bool eng_net_discovering(void);
void eng_net_discover_autojoin(int on);                  /* headless tests: join the first host found */
int  eng_net_found_count(void);
bool eng_net_found(int i, char *label, size_t n, char *addr, size_t an);   /* addr = "ip:port" for eng_net_set_server */

/* STALL-AND-WAIT (optional, for a link that cannot tolerate a peer running ahead, e.g. a ring): call before simulating a frame.
 * lead = frames we have sent minus frames received from the slowest peer in the session (0 outside a session). May advance =
 * lead <= max_lead, or the wait has lasted timeout_ms (a dead / leaving peer must not freeze us: we then run on and the game's
 * own link timeout handles it). While it says no, do NOT simulate; call eng_net_poll_paused() so we keep receiving and pinging. */
int  eng_net_lead(void);
bool eng_net_may_advance(int max_lead, int timeout_ms);
uint32_t eng_net_stall_frames(void);
int  eng_net_auto_lead(void);                            /* a max_lead that does not throttle a link of this latency: 2 + our ping in frames */
uint32_t eng_net_wait(int max_lead, int timeout_ms);       /* the same as a blocking call: polls (paused) until it may advance; returns ms waited */                     /* frames waited so far (a counter for logs) */

void eng_net_poll(void);                                 /* once per simulated frame, BEFORE the game reads its link */
void eng_net_poll_paused(void);                          /* the same while the game is paused (a menu): keepalive only, no FRAMEs */
#endif
