# nmn-server

ONE lobby + relay server for the online play of every game in this tree -- Rave Racer, Tokyo Wars, Cyber Sled, and the ones
that follow -- many rooms of different games at once on one UDP port. Protocol: NMN2, `../docs/NETPLAY.md`. It also speaks
Rave Racer's old RRN1 (`../raverace/NETPLAY.md`) and puts those players in the same Rave Racer rooms as new ones (bridged),
so released Rave Racer builds keep working. Rust, standard library only (builds with rustc 1.63+).

```
cargo build --release
./target/release/nmn-server                 # UDP 27750 on every interface
./target/release/nmn-server --port 27750 --bind 0.0.0.0 --name "My server" --max-per-ip 8 --quiet
#   --max-rooms 128           rooms on the server, every game together
#   --max-rooms-per-game 32   rooms of one game
#   --room-ttl-min 10         an empty room stays listed this long
#   --max-session-min 15      a session older than this is ended (everyone back to the lobby)
#   --frame-idle-sec 90       a session with no FRAME for this long is ended
#   --status-sec 60           one status line per occupied room this often (0 = never)
#   --stats-file PATH         keep LIFETIME STATISTICS in PATH (saved every minute; off unless given)

## Statistics

`--stats-file /path/stats.txt` makes the server count, for as long as that file lives: per game the distinct players, joins,
sessions, players per session, minutes played and the day it was last played; joins and sessions per day; the peak number of
players online; and per player NAME (what they typed in the lobby) how many sessions of which game. Read it back on the server:

    nmn-server --show-stats /path/stats.txt [--top 50]

Nothing personal is kept: a player is counted DISTINCT by a salted hash of their IP address (the salt is random per file), never
the address itself -- so people sharing one connection count as one. Docker: `NMN_STATS_FILE=/data/stats.txt` (mount a volume).
What is online right now, from anywhere: `tools/net/nmn_stat.py host[:port]` (the live rooms; no history).
cargo test                                  # protocol conformance tests (no sockets, fake clock)
```

Players enter `host` or `host:port` on their game's Online page. The server answers LAN discovery too. Open UDP 27750 in the
firewall. Per-game limits (players per room, payload size) are the `GAMES` table in `src/lib.rs`; a game that is not listed
plays with the defaults (8 players, 256-byte frames), so a new game needs no server change unless it needs more.

## Run it in Docker

```
git clone https://github.com/spacestate1/namco22-decompile
cd namco22-decompile/server
docker compose up -d --build
```

Options are environment variables: `NMN_NAME="My server" NMN_PORT=27750 NMN_MAX_PER_IP=4 NMN_MAX_ROOMS=128
NMN_ROOM_TTL_MIN=10 docker compose up -d --build` (the old `RRN1_*` names are still read). Host networking is deliberate: a UDP
relay must see every player's real address (the per-IP limit uses it), and LAN discovery needs broadcasts.

## Hardening

Every message type has a length rule (a FRAME is exactly its room's size); an address that holds no slot is answered only for
HELLO / PING / DISCOVER / ROOMS / DELROOM, 30 datagrams/s per IP, and never with more bytes than 3x what it sent in the last
second (+384) -- no amplification; a refusal is a short reply; one IP holds at most `--max-per-ip` slots over all rooms (raise
it for several players behind one NAT); members are rate limited (400 datagrams/s, 250 FRAMEs/s, 5 chat lines / 2 s). Rooms
of different games, versions or frame sizes never mix, and a datagram naming another room than the sender's is dropped.
There is no authentication.
