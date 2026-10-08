# NMN2 -- online play for every game

One protocol, one server, one client library for every game of this tree that links cabinets: Rave Racer (`rr`), Tokyo Wars
(`tw`), Cyber Sled (`cs`), and later Ace Driver (`ad`), Dirt Dash (`dd`), Time Crisis (`tc`), Prop Cycle (`pc`).

| piece | where | what |
|---|---|---|
| the protocol | this file | NMN2: every datagram names its game, link version and room |
| the server | `server/` (`nmn-server`, Rust, std only) | many rooms of many games at once; also speaks Rave Racer's old RRN1 and bridges it |
| the client | `engine/net.c`, `engine/net.h` | connect, rooms, lobby, start, the per-frame payload, chat, LAN host + discovery |
| the LAN host | `engine/net_host.c` | "Host a LAN game": one room inside the game, same protocol |
| the menu page | `engine/eng_net_ui.c` | the Online page of the shared menu (`engine/eng_ui.c`) and Rave Racer's windows made shared: the Online play window (LAN host / find, ZoneSync, a custom server), the Lobby window (players, rooms with Join / delete, New room, chat + Send, Ready / Start / Disconnect), quick chat on T and the chat overlay over the game -- a menu LAYER (`eng_ui_add_layer`) |
| a fake client | `tools/net/nmn_fake.py` | N clients of any game id, checks for crosstalk -- for testing servers |
| server stats | `tools/net/nmn_stat.py` | players / rooms per game on a server, right now |

The old Rave Racer protocol stays documented in `raverace/NETPLAY.md` (RRN1). NMN2 is RRN1 with a longer header and a
generic payload; the message numbers and the lobby rules are the same.

## 1. Transport

UDP, default port **27750** (servers, built-in hosts and LAN discovery). Little-endian. One message per datagram; the largest
legal one is a FRAME of the biggest game payload (1024 bytes + 25), below any MTU problem for the payloads in use.

### Header (16 bytes)

| off | size | field | |
|---|---|---|---|
| 0 | 4 | magic | `"NMN2"` |
| 4 | 1 | type | §2 |
| 5 | 1 | flags | 0; ignored |
| 6 | 2 | seq | reliable messages: per sender, +1 per new message, kept on retransmission; else 0 |
| 8 | 2 | len | payload bytes; a datagram whose len does not match is dropped |
| 10 | 2 | game | ASCII game id: two lowercase letters/digits (`rr`, `tw`, `cs`, ...) |
| 12 | 2 | ver | the game's **link version**: only equal versions share a room |
| 14 | 2 | room | the room id (per game, from 1); 0 = none |

A server answers in the client's game / version; a client drops anything not of its game. A member's datagram that names
another game, version or room than its own room is dropped by the server (ROOMS / DISCOVER excepted): rooms cannot leak
into each other.

## 2. Messages

| type | name | dir | payload |
|---|---|---|---|
| 0x01 | HELLO | c->s, reliable | `name_len u8, name (<=16 B), mode u8, max_players u8, frame_size u16, label_len u8, label (<=24 B)`, then zero padding to **128 bytes** |
| 0x02 | WELCOME | s->c | `slot u8 (0xFF = refused), state u8, session u32, reason u8, max_players u8, frame_size u16, roster` (a refusal carries an empty roster) |
| 0x03 | ROSTER | s->c | `state u8, session u32, roster` -- on every lobby change, self-correcting |
| 0x04 | READY | c->s, reliable | `ready u8`; acknowledged by a ROSTER showing it |
| 0x05 | START | c->s, reliable | empty; acknowledged by GO |
| 0x06 | GO | s->c, reliable | `session u32, roster` (final slots); resent every 500 ms until ACK |
| 0x07 | LEAVE | c->s | empty (sent twice) |
| 0x08 | FRAME | c->s->c | `session u32, frame_seq u32, slot u8, payload[frame_size]` -- relayed to every other member of the room |
| 0x09 | PING | c->s | `client_ms u32` |
| 0x0A | PONG | s->c | the PING's 4 bytes |
| 0x0B | ACK | c->s | `acked_seq u16` (GO) |
| 0x0C | DISCOVER | c->LAN | empty |
| 0x0D | ANNOUNCE | s->c | `name_len, name, players u16, rooms u16, open_rooms u16` (of the asker's game) |
| 0x0E | CHAT | c->s->c | c->s: text (1-96 B UTF-8); s->c: `slot u8, text` to every member, the sender included |
| 0x0F | ROOMS | c->s | empty (a stranger pads it to 400 B, see §4) |
| 0x10 | ROOMLIST | s->c | `count u8`, per room: `id u16, ver u16, state u8, players u8, max_players u8, mine u8, name_len u8, name` -- the asker's GAME only, every version (a client greys out other versions), at most 32 |
| 0x11 | RENAME | c->s | `name_len, name` |
| 0x12 | DELROOM | c->s | empty; the header's room (only an EMPTY room not in a session); answered with ROOMLIST |

`roster := count u8, then per member { slot u8, ready u8, name_len u8, name }`, ordered by slot.

**HELLO modes.** `0` AUTO: the fullest room of this game, version and frame size that is in its lobby and has a free slot,
else a new room. `1` JOIN: the room in the header. `2` CREATE: a new room called `label` (empty = "<name>'s room") of
`max_players` (2..16, capped per game by the server). `frame_size` must be the room's (it is part of the version).
A duplicate HELLO from a member is answered with a fresh WELCOME, never a second join.

**WELCOME reasons** (byte 6): 0 ok, 1 room full, 2 in a session, 3 no such room, 4 another version / frame size,
5 too many slots from this IP, 6 server full (room limit), 7 bad request, 8 not a member (answer to a stranger's PING).

## 3. Lobby, session and liveness (the RRN1 rules, per room)

- Slots 0..max-1, the lowest free one on join; slot = the cabinet number in the linked game; the GO roster is final.
- START from any member starts the session (the clients refuse to send it until everyone is Ready). A session in progress
  turns joins away (reason 2).
- Client retransmission every **500 ms** (HELLO -> WELCOME by seq, READY -> ROSTER, START -> GO). Server: GO every 500 ms.
- Liveness: a member silent for **> 5 s** is dropped. Clients PING at 1 Hz in the lobby and in a session.
- A session ends by itself (everyone back in the lobby, ready flags cleared): last player gone, **no FRAME for 90 s**, or
  **15 minutes**. A PING from a non-member gets the short refusal WELCOME (reason 8): the client rejoins by itself (the server
  restarted or timed it out).
- An empty room stays listed **10 min**, then closes; at a room limit the oldest empty room is closed to make space.
- Chat: 5 lines per 2 s per member.

**The payload is opaque.** The server checks that a FRAME is exactly `9 + frame_size` bytes, carries the room's session and
the sender's slot, then copies it to every other member. Nothing is lockstepped: each machine sends its newest link state
once a frame and takes whatever peers sent; dedup per sender by `frame_seq` (a receiver drops `frame_seq <=` the last seen
from that slot). This is how a real cabinet link behaves (the games' own link protocols time peers out and cope with loss).

## 4. Server (`server/`, `nmn-server`)

```
cd server && cargo build --release && cargo test       # 28 tests, no sockets (fake clock); builds on rustc 1.63
./target/release/nmn-server --port 27750 --name "ZoneSync" --max-per-ip 8
  # --bind ADDR  --max-rooms 128  --max-rooms-per-game 32  --room-ttl-min 10  --max-session-min 15  --frame-idle-sec 90
  # --status-sec 60 (one status line per occupied room)  --quiet
tools/net/nmn_stat.py [host[:port]] [--games rr,cs] [--json]   # WHAT A SERVER HOLDS NOW, per game: players, rooms, open rooms and
  # every room (name, players / size, lobby or playing, link version). Uses only what the server answers anyone (DISCOVER ->
  # ANNOUNCE, ROOMS padded to 400 B -> ROOMLIST); default zonesync.net. HISTORY: `--stats-file PATH` (lifetime counts per game, per
  # day and per player name, distinct players by salted IP hash; `nmn-server --show-stats PATH`, server/README.md)
```

Per-game limits are a table in `server/src/lib.rs` (`GAMES`): Rave Racer 8 players / 40-byte frames, Tokyo Wars 4 / 720,
Cyber Sled 2 / 512, Ace Driver, Dirt Dash, Prop Cycle 8 / 256, Time Crisis 2 / 256. **A game not in the table may still play**
(defaults 8 players, 256 bytes), so adding a game needs no server update unless it needs more.

Hardening: per-type length rules (FRAME exact); an address holding no slot is answered only for HELLO / PING / DISCOVER / ROOMS
/ DELROOM, at most 30 datagrams/s per IP, and **never more reply bytes than 3x what that IP sent in the last second + 384**
(global cap 256 KB/s) -- hence the padded HELLO (a WELCOME + ROSTER fits its credit) and the padded stranger ROOMS; a refusal
is a short reply with no roster; one IP holds at most `--max-per-ip` slots over all rooms; a member may send 400 datagrams/s,
250 FRAMEs/s (a game sending more than 4 per 1/60 s must batch); unknown types are ignored.

**RRN1 alongside, bridged.** Datagrams starting `"RRN1"` are Rave Racer's old protocol (`raverace/NETPLAY.md`). Such a client
is a member of a Rave Racer room exactly like an NMN2 one (game `rr`, link version 1, 40-byte frames, 8 slots), and every
message is encoded in each member's own protocol -- a FRAME from an RRN1 member reaches an NMN2 member with the same payload
under an NMN2 header and vice versa. **So released Rave Racer builds (0.5.x) and new builds race each other in the same
rooms.** An RRN1 HELLO without a room selector goes to the fullest open Rave Racer lobby (or a new room); with one it joins /
creates as before; its ROOMLIST lists the Rave Racer rooms of version 1. RRN1 FRAMEs are still exactly 59 bytes.

**Ring links (an optional optimisation, NOT implemented).** Tokyo Wars' cabinets form a ring: each uses only its ring
predecessor's payload. The server relays every FRAME to every other member (N-1 copies); a room flag "ring" could forward a
FRAME only to the sender's successor in slot order (1 copy), cutting the download of each client to one stream. It would be a
new HELLO create option + a server rule; the clients would not change (they already take only the predecessor's payload).

## 5. Client library (`engine/net.c`)

A game fills one `eng_net_game` and calls `eng_net_poll()` once per simulated frame (before its link is read), or
`eng_net_poll_paused()` while its menu is open (keepalive only):

```c
static eng_net_game game = {
    "tw", 3,            /* game id, link version */
    8,                  /* room size it asks for */
    100,                /* frame_size: bytes of payload per FRAME */
    "TW_NET",           /* env prefix of the test switches */
    "battle",           /* what a session is called in messages */
    0,                  /* rrn1: Rave Racer only */
    session_begin,      /* GO: (slot, players) -- set the cabinet number, arm the link */
    session_end,        /* back to the lobby / offline -- disarm, restore settings */
    frame_out,          /* fill frame_size bytes of this frame's newest link state, return 1 (0 = nothing to send) */
    frame_in,           /* (slot, bytes, frame_seq): a peer's newer state -> into the emulated link */
    hint,               /* a one-line message on screen (e.g. eng_ui_set_hint) */
};
eng_net_init(&game);
```

**Free-running or stall-and-wait.** By default nothing waits: each machine sends its newest state once a frame and takes the
peers' newest (Rave Racer: the game's link tolerates it). A link that cannot let a peer run ahead (Tokyo Wars' ring) calls
`eng_net_wait(max_lead, timeout_ms)` once a frame after `eng_net_poll()`: it blocks (polling, paused) while we have sent more than
`max_lead` frames beyond the slowest peer's last received frame, so a faster machine WAITS for a slower one instead of drifting;
a peer silent for `timeout_ms` is given up on (the game's own link timeout then drops it). While waiting it resends our newest
FRAME every 20 ms -- otherwise a lost last FRAME of a machine that is itself waiting deadlocks both until the timeout (measured:
unpaced Tokyo Wars pairs hit 9 / 22 such timeouts before the resend, 0 after). `eng_net_auto_lead()` = 2 + our ping in frames,
the smallest lead that does not throttle a link of that latency (a peer's frame arrives about one RTT late through the server);
`eng_net_lead()` / `eng_net_may_advance()` are the non-blocking pieces.

The rest of the API (`net.h`): server/name, connect/disconnect, status, roster, ready/start, rooms (list of THIS game, switch,
new, delete), chat, LAN host/discover. Rave Racer's RRN1 fallback: with `rrn1 = 1`, if no NMN2 answer has come after 1.5 s the
HELLO is also sent in RRN1 and the first WELCOME fixes the protocol, so new builds still work with an old `rrn1-server` or an
old build's LAN host. The built-in LAN host (`net_host.c`) speaks NMN2, and RRN1 too when the hosting game is Rave Racer.

Test switches (prefix = the game's `env`): `_DEBUG=1`, `_AUTOSTART=1|N`, `_SAY=text`, `_ROOM=new[:name]|<id>`, `_RENAME=name`,
`_SIM=loss%,stall_ms[,burst]` + `_SIM_SEED`. Debug lines are `[NET] ...` (`-> SESSION`, `GO:`, `frames tx N rx M`, `chat:`).

## 6. How to hook a game -- checklist (Tokyo Wars, Cyber Sled, ...)

1. **Find the link.** The emulated serial/link chip (System 22 / Super 22 C139 SCI; System 21 C139): what one cabinet
   transmits per frame and how a received packet enters the game (ring buffer + IRQ). Make the payload ONE fixed size per
   frame (pad, or batch a frame's packets into one payload with a count byte). This size + any layout change = `frame_size`
   and `ver`.
2. **Pick the id and limits.** `id` from §1 (`tw`, `cs`); `max_players` = cabinets the game links (Cyber Sled 2). If the
   payload exceeds the server table's limit, raise it in `server/src/lib.rs` `GAMES` (and redeploy).
3. **Write the hooks** in the game's own file (e.g. `tokyowar/src/tw_net.c`): `session_begin` sets the cabinet number
   (= slot) and arms the link so the game sees the peers; `frame_out` takes the NEWEST staged packet (never a backlog);
   `frame_in` stores each peer's newest packet; deliver every pending peer's packet at the frame edge (Rave Racer:
   `rr_link_inject_next` + the SCI IRQ until none is pending). `session_end` disarms and restores anything changed (e.g.
   free play).
4. **Call it.** `eng_net_init(&game)` at boot; `eng_net_poll()` once per simulated frame before the link is serviced;
   `eng_net_poll_paused()` while paused. Optional headless bootstrap like Rave Racer's `RR_NET_SERVER` / `RR_NET_NAME` /
   `RR_NET_HOST=1` / `RR_NET_DISCOVER=1` (`raverace/src/rr_main.c`).
5. **Menu.** After `eng_cfg_load()` and `eng_net_init()`: `eng_ui_add_page(eng_net_ui_page())` (saves `net_server`,
   default `zonesync.net`, and `net_name` in the game's cfg).
6. **Build.** Add `${NAMCO22_ENGINE_NET_SRC}` (and `${NAMCO22_ENGINE_NET_UI_SRC}` with the UI group) to the game's sources;
   Windows: link `ws2_32`.
7. **Prove it** (HARD RULE 5): single-player output byte-identical before/after (the client is inert until connected);
   two headless clients over loopback reach GO and stay linked (the game's own peer-alive state; Rave Racer: the link rx
   count keeps growing after the course select) -- `tools/net/net_loopback_test.sh` is the template (`tools/net/net_tw_test.sh` for a
   stall-and-wait game through `nmn-server`, 2-4 cabinets, `REMOTE=host:port` for a server on the internet) (`tools/net/net_lanhost_test.sh` for the built-in LAN host; `tools/net/net_cs_test.sh` for Cyber Sled -- its slot is a boot-time operator setting, so GO restarts the board, see `cybsled/PLAN.md` Module N), and it runs a second game's fake room on
   the same server at the same time; then over the internet against a test instance.
