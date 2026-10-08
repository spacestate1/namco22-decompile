//! nmn-server -- ONE lobby + relay server for every game of the Namco System 22 / 21 decompilation (`docs/NETPLAY.md` is the
//! contract). Many rooms at once, of DIFFERENT games at once: every NMN2 datagram names its game ("rr", "tw", "cs", ...), its
//! link version and its room, and a room only ever talks to members of its own game, version and frame size.
//!
//! The old Rave Racer protocol RRN1 (`raverace/NETPLAY.md`) is spoken alongside: an RRN1 client is a member of a Rave Racer
//! room exactly like an NMN2 one (game "rr", link version 1, 40-byte frames), and the two are BRIDGED -- every message a member
//! receives is encoded in that member's own protocol, so released Rave Racer builds race new ones in the same room.
//!
//! The core is transport-free: [`Hub::handle`] takes one datagram and appends the datagrams to send to an output list,
//! [`Hub::tick`] does the timers. `main.rs` binds a UDP socket around it; the tests drive it with a fake clock.
//!
//! Hardening (a public address): every message type has a length rule (FRAMEs exactly the room's size); an address that holds
//! no slot gets replies only for HELLO / PING / DISCOVER / ROOMS / DELROOM, at most `unknown_pps` datagrams a second per IP, and
//! never more reply bytes than 3x what that IP sent in the last second plus a small allowance (the anti-amplification limit;
//! NMN2 clients pad HELLO and ROOMS so their answers fit), under a global cap; a rejection is a short reply with no roster;
//! one IP holds at most `max_per_ip` slots over all rooms; members are rate limited (all messages, FRAMEs, chat).

use std::collections::HashMap;
use std::net::{IpAddr, SocketAddr};
use std::time::{Duration, Instant};

pub const MAGIC1: &[u8; 4] = b"RRN1";
pub const MAGIC2: &[u8; 4] = b"NMN2";
pub const HDR1: usize = 10;
pub const HDR2: usize = 16;
pub const DEFAULT_PORT: u16 = 27750;
pub const LIVENESS: Duration = Duration::from_secs(5);
pub const GO_RESEND: Duration = Duration::from_millis(500);
/// The hard cap on players in one room (a roster's slot is a byte, the roster count too).
pub const MAX_SLOTS: usize = 16;
/// Longest chat text in bytes.
pub const MAX_CHAT: usize = 96;
/// What an RRN1 client is: Rave Racer, link version 1, 40-byte frames (the 38-byte C139 packet + 2 zero bytes), 8 slots.
pub const LEGACY_GAME: [u8; 2] = *b"rr";
pub const LEGACY_VER: u16 = 1;
pub const LEGACY_FRAME: usize = 40;
pub const LEGACY_SLOTS: u8 = 8;
/// HELLO room selector (RRN1): 255 = a new room
pub const ROOM_NEW: u8 = 255;

pub mod ty {
    pub const HELLO: u8 = 0x01;
    pub const WELCOME: u8 = 0x02;
    pub const ROSTER: u8 = 0x03;
    pub const READY: u8 = 0x04;
    pub const START: u8 = 0x05;
    pub const GO: u8 = 0x06;
    pub const LEAVE: u8 = 0x07;
    pub const FRAME: u8 = 0x08;
    pub const PING: u8 = 0x09;
    pub const PONG: u8 = 0x0A;
    pub const ACK: u8 = 0x0B;
    pub const DISCOVER: u8 = 0x0C;
    pub const ANNOUNCE: u8 = 0x0D;
    pub const CHAT: u8 = 0x0E;
    pub const ROOMS: u8 = 0x0F;
    pub const ROOMLIST: u8 = 0x10;
    pub const RENAME: u8 = 0x11;
    pub const DELROOM: u8 = 0x12;
}

/// Why a HELLO was refused (NMN2 WELCOME byte 6; RRN1 only has its state byte: 1 = racing, else 0).
pub mod why {
    pub const OK: u8 = 0;
    pub const FULL: u8 = 1;
    pub const RACING: u8 = 2;
    pub const NO_ROOM: u8 = 3;
    pub const VERSION: u8 = 4;
    pub const PER_IP: u8 = 5;
    pub const SERVER_FULL: u8 = 6;
    pub const BAD: u8 = 7;
    pub const NOT_MEMBER: u8 = 8;
}

/// HELLO modes (NMN2)
pub mod mode {
    pub const AUTO: u8 = 0; // the fullest open lobby of my game / version / frame size, else a new room
    pub const JOIN: u8 = 1; // the room in the header
    pub const CREATE: u8 = 2; // a new room
}

/// Per-game limits. A game not listed may still play (its id is two lowercase letters or digits) with the defaults below; a
/// listed game's room can never be larger than this, whatever its creator asks for.
pub struct GameLimit {
    pub id: [u8; 2],
    pub max_players: u8,
    pub max_frame: u16,
    pub title: &'static str,
}
pub const GAMES: &[GameLimit] = &[
    GameLimit { id: *b"rr", max_players: 8, max_frame: 40, title: "Rave Racer" },
    GameLimit { id: *b"tw", max_players: 4, max_frame: 720, title: "Tokyo Wars" },
    GameLimit { id: *b"cs", max_players: 2, max_frame: 512, title: "Cyber Sled" },
    GameLimit { id: *b"ad", max_players: 8, max_frame: 256, title: "Ace Driver" },
    GameLimit { id: *b"dd", max_players: 8, max_frame: 256, title: "Dirt Dash" },
    GameLimit { id: *b"tc", max_players: 2, max_frame: 256, title: "Time Crisis" },
    GameLimit { id: *b"pc", max_players: 8, max_frame: 256, title: "Prop Cycle" },
];
pub const DEFAULT_MAX_PLAYERS: u8 = 8;
pub const DEFAULT_MAX_FRAME: u16 = 256;

pub fn game_limit(g: [u8; 2]) -> (u8, u16) {
    for l in GAMES {
        if l.id == g {
            return (l.max_players, l.max_frame);
        }
    }
    (DEFAULT_MAX_PLAYERS, DEFAULT_MAX_FRAME)
}
fn game_ok(g: [u8; 2]) -> bool {
    g.iter().all(|c| c.is_ascii_lowercase() || c.is_ascii_digit())
}
fn gname(g: [u8; 2]) -> String {
    String::from_utf8_lossy(&g).into_owned()
}

#[derive(Clone, Debug)]
pub struct Config {
    /// Shown to discovery (max 16 bytes on the wire).
    pub name: String,
    /// Slots one IP address may hold at once, over all rooms.
    pub max_per_ip: usize,
    /// Datagrams per second accepted from an address that holds no slot.
    pub unknown_pps: u32,
    /// Reply bytes a stranger's IP may receive in one second beyond 3x what it sent (the anti-amplification allowance).
    pub unknown_allow: usize,
    /// All reply bytes to strangers in one second, every IP together.
    pub unknown_bytes_global: usize,
    /// Datagrams per second one member may send (all types).
    pub member_pps: u32,
    /// FRAMEs per second one member may send.
    pub frame_pps: u32,
    /// A race session older than this is ended (the players are put back in the lobby).
    pub max_session: Duration,
    /// A race session in which no FRAME arrives for this long is ended.
    pub frame_idle: Duration,
    /// An empty room stays listed (and joinable) this long after its last player left, then closes.
    pub room_ttl: Duration,
    /// Rooms on the server, every game together.
    pub max_rooms: usize,
    /// Rooms of one game.
    pub max_rooms_per_game: usize,
    /// A status line per occupied room this often (0 = never).
    pub status_every: Duration,
}

impl Default for Config {
    fn default() -> Self {
        Config {
            name: "NMN SERVER".into(),
            max_per_ip: 8,
            unknown_pps: 30,
            unknown_allow: 384,
            unknown_bytes_global: 256 * 1024,
            member_pps: 400,
            frame_pps: 250,
            max_session: Duration::from_secs(15 * 60),
            frame_idle: Duration::from_secs(90),
            room_ttl: Duration::from_secs(10 * 60),
            max_rooms: 128,
            max_rooms_per_game: 32,
            status_every: Duration::from_secs(60),
        }
    }
}

/// (destination, datagram)
pub type Out = Vec<(SocketAddr, Vec<u8>)>;

fn le16(b: &[u8]) -> u16 {
    u16::from_le_bytes([b[0], b[1]])
}
fn le32(b: &[u8]) -> u32 {
    u32::from_le_bytes([b[0], b[1], b[2], b[3]])
}

/// One parsed header.
#[derive(Clone, Copy, Debug)]
pub struct Hdr {
    pub legacy: bool,
    pub kind: u8,
    pub seq: u16,
    pub game: [u8; 2],
    pub ver: u16,
    pub room: u16,
    pub hlen: usize,
}

pub fn parse(d: &[u8]) -> Option<Hdr> {
    if d.len() >= HDR1 && &d[0..4] == MAGIC1 {
        if le16(&d[8..10]) as usize != d.len() - HDR1 {
            return None;
        }
        return Some(Hdr { legacy: true, kind: d[4], seq: le16(&d[6..8]), game: LEGACY_GAME, ver: LEGACY_VER, room: 0, hlen: HDR1 });
    }
    if d.len() >= HDR2 && &d[0..4] == MAGIC2 {
        if le16(&d[8..10]) as usize != d.len() - HDR2 {
            return None;
        }
        return Some(Hdr {
            legacy: false,
            kind: d[4],
            seq: le16(&d[6..8]),
            game: [d[10], d[11]],
            ver: le16(&d[12..14]),
            room: le16(&d[14..16]),
            hlen: HDR2,
        });
    }
    None
}

/// Encode one message. `legacy` = RRN1 (game / version / room are not on that wire).
pub fn enc(legacy: bool, kind: u8, seq: u16, game: [u8; 2], ver: u16, room: u16, payload: &[u8]) -> Vec<u8> {
    let mut v = Vec::with_capacity(HDR2 + payload.len());
    v.extend_from_slice(if legacy { MAGIC1 } else { MAGIC2 });
    v.push(kind);
    v.push(0);
    v.extend_from_slice(&seq.to_le_bytes());
    v.extend_from_slice(&(payload.len() as u16).to_le_bytes());
    if !legacy {
        v.extend_from_slice(&game);
        v.extend_from_slice(&ver.to_le_bytes());
        v.extend_from_slice(&room.to_le_bytes());
    }
    v.extend_from_slice(payload);
    v
}

/// Chat text: printable, at most MAX_CHAT bytes (cut at a character boundary), trimmed.
fn clean_text(raw: &[u8]) -> String {
    let s = String::from_utf8_lossy(raw);
    let mut out = String::new();
    for c in s.chars().filter(|c| !c.is_control()) {
        if out.len() + c.len_utf8() > MAX_CHAT {
            break;
        }
        out.push(c);
    }
    out.trim().to_string()
}

/// A room name: printable, trimmed, at most 24 bytes (cut at a character boundary); empty = none given.
fn clean_label(raw: &[u8]) -> Option<String> {
    let t: String = String::from_utf8_lossy(raw).chars().filter(|c| !c.is_control()).collect();
    let mut t = t.trim().to_string();
    while t.len() > 24 {
        t.pop();
    }
    if t.is_empty() {
        None
    } else {
        Some(t)
    }
}

/// A player name: printable, at most 16 BYTES (cut at a character boundary).
fn clean_name(raw: &[u8]) -> String {
    let s = String::from_utf8_lossy(raw);
    let mut out = String::new();
    for c in s.chars().filter(|c| !c.is_control()) {
        if out.len() + c.len_utf8() > 16 {
            break;
        }
        out.push(c);
    }
    out
}

/// `len, bytes` at p[at..] (clamped to `max` and to what is there); None = nothing there
fn field(p: &[u8], at: usize, max: usize) -> Option<&[u8]> {
    let l = *p.get(at)? as usize;
    let l = l.min(max).min(p.len().saturating_sub(at + 1));
    Some(&p[at + 1..at + 1 + l])
}

struct Member {
    addr: SocketAddr,
    legacy: bool,
    name: String,
    ready: bool,
    last_seen: Instant,
    go_seq: u16,
    go_acked: bool,
    last_go: Instant,
    chat_t: Instant,
    chat_n: u32,
    rate_t: Instant,
    rate_n: u32,
    frame_t: Instant,
    frame_n: u32,
}

struct Room {
    game: [u8; 2],
    ver: u16,
    id: u16,
    label: Option<String>,
    max_players: u8,
    frame_size: u16,
    slots: Vec<Option<Member>>,
    state: u8, // 0 lobby, 1 race
    session_id: u32,
    seq: u16,
    relayed: u64,
    session_start: Instant,
    start_players: usize,
    last_frame: Instant,
    empty_since: Option<Instant>,
}

impl Room {
    fn players(&self) -> usize {
        self.slots.iter().flatten().count()
    }
    fn tag(&self) -> String {
        format!("{}#{}", gname(self.game), self.id)
    }
    fn slot_of(&self, a: SocketAddr) -> Option<usize> {
        self.slots.iter().position(|s| s.as_ref().map_or(false, |s| s.addr == a))
    }
    fn next_seq(&mut self) -> u16 {
        self.seq = self.seq.wrapping_add(1);
        if self.seq == 0 {
            self.seq = 1;
        }
        self.seq
    }
    fn name(&self) -> String {
        if let Some(n) = &self.label {
            return n.clone();
        }
        match self.slots.iter().flatten().next() {
            Some(s) => {
                let mut n = format!("{}'s room", s.name);
                while n.len() > 24 {
                    n.pop();
                }
                n
            }
            None => format!("Room {}", self.id),
        }
    }
    fn msg(&self, legacy: bool, kind: u8, seq: u16, payload: &[u8]) -> Vec<u8> {
        enc(legacy, kind, seq, self.game, self.ver, self.id, payload)
    }
    fn roster_payload(&self) -> Vec<u8> {
        let members: Vec<(usize, &Member)> =
            self.slots.iter().enumerate().filter_map(|(i, s)| s.as_ref().map(|s| (i, s))).collect();
        let mut v = vec![members.len() as u8];
        for (i, s) in members {
            let n = s.name.as_bytes();
            v.extend_from_slice(&[i as u8, s.ready as u8, n.len() as u8]);
            v.extend_from_slice(n);
        }
        v
    }
    fn broadcast(&self, kind: u8, payload: &[u8], out: &mut Out) {
        let (mut m1, mut m2) = (None, None);
        for s in self.slots.iter().flatten() {
            let m = if s.legacy { &mut m1 } else { &mut m2 };
            if m.is_none() {
                *m = Some(self.msg(s.legacy, kind, 0, payload));
            }
            out.push((s.addr, m.as_ref().unwrap().clone()));
        }
    }
    fn roster_changed(&self, out: &mut Out) {
        let mut p = vec![self.state];
        p.extend_from_slice(&self.session_id.to_le_bytes());
        p.extend_from_slice(&self.roster_payload());
        self.broadcast(ty::ROSTER, &p, out);
    }
    fn welcome(&self, to: SocketAddr, legacy: bool, slot: usize, hello_seq: u16, out: &mut Out) {
        let mut p = vec![slot as u8, self.state];
        p.extend_from_slice(&self.session_id.to_le_bytes());
        if !legacy {
            p.push(why::OK);
            p.push(self.max_players);
            p.extend_from_slice(&self.frame_size.to_le_bytes());
        }
        p.extend_from_slice(&self.roster_payload());
        out.push((to, self.msg(legacy, ty::WELCOME, hello_seq, &p)));
    }
    fn send_go(&mut self, i: usize, now: Instant, out: &mut Out) {
        if self.slots[i].as_ref().map_or(false, |s| s.go_seq == 0) {
            let q = self.next_seq();
            self.slots[i].as_mut().unwrap().go_seq = q;
        }
        let mut p = self.session_id.to_le_bytes().to_vec();
        p.extend_from_slice(&self.roster_payload());
        let (addr, legacy, seq) = {
            let s = self.slots[i].as_ref().unwrap();
            (s.addr, s.legacy, s.go_seq)
        };
        out.push((addr, self.msg(legacy, ty::GO, seq, &p)));
        self.slots[i].as_mut().unwrap().last_go = now;
    }
    fn start(&mut self, rnd: u64, now: Instant, out: &mut Out, events: &mut Vec<String>) {
        self.state = 1;
        let mut id = (rnd >> 16) as u32;
        if id == 0 {
            id = 0x5EED;
        }
        self.session_id = id;
        self.relayed = 0;
        self.session_start = now;
        self.start_players = self.players();
        self.last_frame = now;
        events.push(format!("[{}] race start: session {:08x}, {} player(s)", self.tag(), id, self.players()));
        for i in 0..self.slots.len() {
            if let Some(s) = self.slots[i].as_mut() {
                s.go_seq = 0;
                s.go_acked = false;
            }
            if self.slots[i].is_some() {
                self.send_go(i, now, out);
            }
        }
        self.roster_changed(out);
    }
    /// Back to the lobby: every member stays, ready flags and GO bookkeeping are cleared.
    fn end_session(&mut self, why: &str, out: &mut Out, events: &mut Vec<String>) {
        events.push(format!("[{}] session over ({}), relayed {} frames; back to lobby", self.tag(), why, self.relayed));
        self.state = 0;
        self.session_id = 0;
        self.relayed = 0;
        for s in self.slots.iter_mut().flatten() {
            s.ready = false;
            s.go_seq = 0;
            s.go_acked = false;
        }
        self.roster_changed(out);
    }
}

struct Budget {
    t: Instant,
    pkts: u32,
    bytes_in: usize,
    bytes_out: usize,
}

pub struct Hub {
    cfg: Config,
    rooms: Vec<Room>,
    rng: u64,
    limiter: HashMap<IpAddr, Budget>,
    global: (Instant, usize),
    last_gc: Instant,
    last_status: Instant,
    /// Human-readable events (joins, leaves, session start/end, status lines) for the binary to print.
    pub events: Vec<String>,
    /// Counters for the status line.
    pub n_frames: u64,
    pub n_dropped: u64,
    /// Lifetime statistics (`--stats-file`): None = not kept.
    pub stats: Option<stats::Stats>,
}

impl Hub {
    pub fn new(cfg: Config, now: Instant, seed: u64) -> Hub {
        Hub {
            cfg,
            rooms: Vec::new(),
            rng: seed | 1,
            limiter: HashMap::new(),
            global: (now, 0),
            last_gc: now,
            last_status: now,
            events: Vec::new(),
            n_frames: 0,
            n_dropped: 0,
            stats: None,
        }
    }
    pub fn cfg_mut(&mut self) -> &mut Config {
        &mut self.cfg
    }
    pub fn rooms(&self) -> usize {
        self.rooms.len()
    }
    pub fn rooms_of(&self, g: &[u8; 2]) -> usize {
        self.rooms.iter().filter(|r| &r.game == g).count()
    }
    pub fn players(&self) -> usize {
        self.rooms.iter().map(|r| r.players()).sum()
    }
    /// (state, players, session) of a room, for tests and the status line
    pub fn room_info(&self, g: &[u8; 2], id: u16) -> Option<(u8, usize, u32)> {
        self.rooms.iter().find(|r| &r.game == g && r.id == id).map(|r| (r.state, r.players(), r.session_id))
    }

    fn rand(&mut self) -> u64 {
        // xorshift64*: session ids need to be unpredictable enough, not cryptographic
        let mut x = self.rng;
        x ^= x >> 12;
        x ^= x << 25;
        x ^= x >> 27;
        self.rng = x;
        x.wrapping_mul(0x2545F4914F6CDD1D)
    }

    fn member(&self, a: SocketAddr) -> Option<(usize, usize)> {
        for (r, room) in self.rooms.iter().enumerate() {
            if let Some(s) = room.slot_of(a) {
                return Some((r, s));
            }
        }
        None
    }
    fn slots_of_ip(&self, ip: IpAddr) -> usize {
        self.rooms.iter().map(|r| r.slots.iter().flatten().filter(|s| s.addr.ip() == ip).count()).sum()
    }

    fn budget(&mut self, ip: IpAddr, now: Instant) -> Option<&mut Budget> {
        if now.duration_since(self.last_gc) > Duration::from_secs(10) {
            self.limiter.retain(|_, b| now.duration_since(b.t) < Duration::from_secs(2));
            self.last_gc = now;
        }
        if self.limiter.len() > 4096 && !self.limiter.contains_key(&ip) {
            return None; // a flood of distinct sources: shed instead of growing
        }
        let b = self.limiter.entry(ip).or_insert(Budget { t: now, pkts: 0, bytes_in: 0, bytes_out: 0 });
        if now.duration_since(b.t) >= Duration::from_secs(1) {
            *b = Budget { t: now, pkts: 0, bytes_in: 0, bytes_out: 0 };
        }
        Some(b)
    }
    /// A datagram from an address that holds no slot: counted against its IP. True = accept.
    fn allow_unknown(&mut self, ip: IpAddr, len: usize, now: Instant) -> bool {
        let pps = self.cfg.unknown_pps;
        match self.budget(ip, now) {
            Some(b) => {
                b.pkts += 1;
                b.bytes_in += len;
                b.pkts <= pps
            }
            None => false,
        }
    }
    /// A reply to a stranger: only within its IP's anti-amplification budget and the global cap.
    fn reply_unknown(&mut self, to: SocketAddr, m: Vec<u8>, now: Instant, out: &mut Out) {
        if now.duration_since(self.global.0) >= Duration::from_secs(1) {
            self.global = (now, 0);
        }
        if self.global.1 + m.len() > self.cfg.unknown_bytes_global {
            self.n_dropped += 1;
            return;
        }
        let allow = self.cfg.unknown_allow;
        let ok = match self.budget(to.ip(), now) {
            Some(b) => {
                if b.bytes_out + m.len() <= 3 * b.bytes_in + allow {
                    b.bytes_out += m.len();
                    true
                } else {
                    false
                }
            }
            None => false,
        };
        if ok {
            self.global.1 += m.len();
            out.push((to, m));
        } else {
            self.n_dropped += 1;
        }
    }
    /// The short refusal of a HELLO (or of a stranger's PING): no roster, nothing to amplify or leak.
    fn reject(&mut self, to: SocketAddr, h: &Hdr, reason: u8, racing: bool, now: Instant, out: &mut Out) {
        let mut p = vec![0xFF, racing as u8, 0, 0, 0, 0];
        if !h.legacy {
            p.push(reason);
            p.push(0);
            p.extend_from_slice(&[0, 0]);
        }
        p.push(0); // empty roster
        let room = if h.legacy { 0 } else { h.room };
        let m = enc(h.legacy, ty::WELCOME, h.seq, h.game, h.ver, room, &p);
        self.reply_unknown(to, m, now, out);
    }

    fn open_room(&mut self, game: [u8; 2], ver: u16, max_players: u8, frame_size: u16, label: Option<String>, now: Instant) -> Option<usize> {
        // room for one more? at a limit the oldest EMPTY room (of this game first) is closed to make space
        let per_game = self.rooms_of(&game) >= self.cfg.max_rooms_per_game;
        let total = self.rooms.len() >= self.cfg.max_rooms;
        if per_game || total {
            let oldest = self
                .rooms
                .iter()
                .enumerate()
                .filter(|(_, r)| r.players() == 0 && r.state == 0 && (!per_game || r.game == game))
                .min_by_key(|(_, r)| r.empty_since)
                .map(|(i, _)| i);
            match oldest {
                Some(i) => {
                    let r = self.rooms.remove(i);
                    self.events.push(format!("[{}] room closed (made space)", r.tag()));
                }
                None => return None,
            }
        }
        // ids are per game, lowest free from 1: an RRN1 client sees them as a byte, and a game never has 255 rooms
        let mut id = 1u16;
        while self.rooms.iter().any(|r| r.game == game && r.id == id) {
            id += 1;
        }
        let n = max_players as usize;
        let room = Room {
            game,
            ver,
            id,
            label,
            max_players,
            frame_size,
            slots: (0..n).map(|_| None).collect(),
            state: 0,
            session_id: 0,
            seq: 0,
            relayed: 0,
            session_start: now,
            start_players: 0,
            last_frame: now,
            empty_since: Some(now),
        };
        self.events.push(format!(
            "[{}] room opened (v{}, {} players, {}-byte frames{})",
            room.tag(),
            ver,
            max_players,
            frame_size,
            room.label.as_ref().map(|l| format!(", '{}'", l)).unwrap_or_default()
        ));
        self.rooms.push(room);
        Some(self.rooms.len() - 1)
    }

    /// Can this address take a slot in room `r` right now? (the reason when not)
    fn joinable(&self, r: usize, ip: IpAddr) -> Result<(), u8> {
        let room = &self.rooms[r];
        if room.state != 0 {
            return Err(why::RACING);
        }
        if room.players() >= room.slots.len() {
            return Err(why::FULL);
        }
        if self.slots_of_ip(ip) >= self.cfg.max_per_ip {
            return Err(why::PER_IP);
        }
        Ok(())
    }
    /// The fullest joinable room of this kind (ties: the oldest).
    fn pick(&self, game: [u8; 2], ver: u16, frame_size: u16, ip: IpAddr) -> Option<usize> {
        let mut best: Option<usize> = None;
        for r in 0..self.rooms.len() {
            let room = &self.rooms[r];
            if room.game != game || room.ver != ver || room.frame_size != frame_size || self.joinable(r, ip).is_err() {
                continue;
            }
            if best.map_or(true, |b| room.players() > self.rooms[b].players()) {
                best = Some(r);
            }
        }
        best
    }

    /// Charge a stranger's reply of `n` bytes in advance (a join's WELCOME + ROSTER): false = over its budget, send nothing.
    fn fits_unknown(&mut self, ip: IpAddr, n: usize, now: Instant) -> bool {
        if now.duration_since(self.global.0) >= Duration::from_secs(1) {
            self.global = (now, 0);
        }
        if self.global.1 + n > self.cfg.unknown_bytes_global {
            return false;
        }
        let allow = self.cfg.unknown_allow;
        let ok = match self.budget(ip, now) {
            Some(b) if b.bytes_out + n <= 3 * b.bytes_in + allow => {
                b.bytes_out += n;
                true
            }
            _ => false,
        };
        if ok {
            self.global.1 += n;
        }
        ok
    }

    fn join(&mut self, r: usize, from: SocketAddr, legacy: bool, name: String, seq: u16, now: Instant, out: &mut Out) {
        // what the join sends this stranger (WELCOME + ROSTER, each with the roster including it) must fit its budget; when it
        // does not, nothing happens and the client's HELLO retransmission (500 ms) brings more credit
        let est = 2 * (HDR2 + 10 + self.rooms[r].roster_payload().len() + 3 + name.len());
        if !self.fits_unknown(from.ip(), est, now) {
            self.n_dropped += 1;
            return;
        }
        let online = self.players() + 1;
        let room = &mut self.rooms[r];
        let free = room.slots.iter().position(|s| s.is_none()).unwrap();
        self.events.push(format!("[{}] join slot {} '{}' from {}{}", room.tag(), free, name, from, if legacy { " (RRN1)" } else { "" }));
        if let Some(st) = self.stats.as_mut() {
            st.join(room.game, from.ip(), &name, online);
        }
        room.slots[free] = Some(Member {
            addr: from,
            legacy,
            name,
            ready: false,
            last_seen: now,
            go_seq: 0,
            go_acked: false,
            last_go: now,
            chat_t: now,
            chat_n: 0,
            rate_t: now,
            rate_n: 0,
            frame_t: now,
            frame_n: 0,
        });
        room.empty_since = None;
        room.welcome(from, legacy, free, seq, out);
        room.roster_changed(out);
    }

    fn on_hello(&mut self, h: &Hdr, p: &[u8], from: SocketAddr, now: Instant, out: &mut Out) {
        if p.is_empty() {
            return;
        }
        let name = clean_name(field(p, 0, 16).unwrap_or(&[]));
        let after_name = 1 + (p[0] as usize).min(p.len() - 1);
        if h.legacy {
            // RRN1: [name] [selector: 0/absent = the server picks, 1..254 = that room, 255 = new [len name]]
            let sel = p.get(after_name).copied().unwrap_or(0);
            let r = if sel == ROOM_NEW {
                let label = field(p, after_name + 1, 24).and_then(clean_label);
                match self.open_room(LEGACY_GAME, LEGACY_VER, LEGACY_SLOTS, LEGACY_FRAME as u16, label, now) {
                    Some(r) => r,
                    None => return self.reject(from, h, why::SERVER_FULL, false, now, out),
                }
            } else if sel != 0 {
                match self.rooms.iter().position(|r| r.game == LEGACY_GAME && r.id == sel as u16) {
                    Some(r) if self.rooms[r].ver == LEGACY_VER && self.rooms[r].frame_size as usize == LEGACY_FRAME => r,
                    _ => return self.reject(from, h, why::NO_ROOM, false, now, out),
                }
            } else {
                match self.pick(LEGACY_GAME, LEGACY_VER, LEGACY_FRAME as u16, from.ip()) {
                    Some(r) => r,
                    None if self.slots_of_ip(from.ip()) >= self.cfg.max_per_ip => {
                        return self.reject(from, h, why::PER_IP, false, now, out)
                    }
                    None => match self.open_room(LEGACY_GAME, LEGACY_VER, LEGACY_SLOTS, LEGACY_FRAME as u16, None, now) {
                        Some(r) => r,
                        None => {
                            let racing = self.rooms.iter().any(|r| r.game == LEGACY_GAME && r.state == 1);
                            return self.reject(from, h, why::SERVER_FULL, racing, now, out);
                        }
                    },
                }
            };
            if let Err(e) = self.joinable(r, from.ip()) {
                self.events.push(format!("[{}] join from {} refused ({})", self.rooms[r].tag(), from, e));
                return self.reject(from, h, e, e == why::RACING, now, out);
            }
            return self.join(r, from, true, name, h.seq, now, out);
        }
        // NMN2: [name] mode max_players frame_size(2) [label] [padding]
        if !game_ok(h.game) || p.len() < after_name + 4 {
            return self.reject(from, h, why::BAD, false, now, out);
        }
        let md = p[after_name];
        let (lim_players, lim_frame) = game_limit(h.game);
        let want_players = p[after_name + 1].max(2).min(lim_players).min(MAX_SLOTS as u8);
        let frame_size = le16(&p[after_name + 2..after_name + 4]);
        if frame_size == 0 || frame_size > lim_frame {
            return self.reject(from, h, why::BAD, false, now, out);
        }
        let label = field(p, after_name + 4, 24).and_then(clean_label);
        if self.slots_of_ip(from.ip()) >= self.cfg.max_per_ip {
            return self.reject(from, h, why::PER_IP, false, now, out);
        }
        let r = match md {
            mode::JOIN => {
                let r = match self.rooms.iter().position(|r| r.game == h.game && r.id == h.room) {
                    Some(r) => r,
                    None => return self.reject(from, h, why::NO_ROOM, false, now, out),
                };
                if self.rooms[r].ver != h.ver || self.rooms[r].frame_size != frame_size {
                    return self.reject(from, h, why::VERSION, false, now, out);
                }
                r
            }
            mode::CREATE => match self.open_room(h.game, h.ver, want_players, frame_size, label, now) {
                Some(r) => r,
                None => return self.reject(from, h, why::SERVER_FULL, false, now, out),
            },
            mode::AUTO => match self.pick(h.game, h.ver, frame_size, from.ip()) {
                Some(r) => r,
                None => match self.open_room(h.game, h.ver, want_players, frame_size, label, now) {
                    Some(r) => r,
                    None => return self.reject(from, h, why::SERVER_FULL, false, now, out),
                },
            },
            _ => return self.reject(from, h, why::BAD, false, now, out),
        };
        if let Err(e) = self.joinable(r, from.ip()) {
            return self.reject(from, h, e, e == why::RACING, now, out);
        }
        self.join(r, from, false, name, h.seq, now, out);
    }

    /// The room list for a client of game `g` (RRN1: Rave Racer rooms of version 1).
    fn roomlist(&self, h: &Hdr, to: SocketAddr) -> Vec<u8> {
        let rooms: Vec<&Room> = self
            .rooms
            .iter()
            .filter(|r| r.game == h.game && (!h.legacy || (r.ver == LEGACY_VER && r.frame_size as usize == LEGACY_FRAME && r.id < 255)))
            .take(if h.legacy { 20 } else { 32 })
            .collect();
        let mut p = vec![rooms.len() as u8];
        for r in rooms {
            let mut name = r.name();
            while name.len() > 24 {
                name.pop();
            }
            if h.legacy {
                p.push(r.id as u8);
            } else {
                p.extend_from_slice(&r.id.to_le_bytes());
                p.extend_from_slice(&r.ver.to_le_bytes());
            }
            p.push(r.state);
            p.push(r.players() as u8);
            if !h.legacy {
                p.push(r.max_players);
            }
            p.push(r.slot_of(to).is_some() as u8);
            p.push(name.len() as u8);
            p.extend_from_slice(name.as_bytes());
        }
        enc(h.legacy, ty::ROOMLIST, 0, h.game, h.ver, 0, &p)
    }

    fn announce(&self, h: &Hdr) -> Vec<u8> {
        let mine: Vec<&Room> = self.rooms.iter().filter(|r| r.game == h.game).collect();
        let players: usize = mine.iter().map(|r| r.players()).sum();
        let name = clean_name(self.cfg.name.as_bytes());
        if h.legacy {
            let racing = !mine.is_empty() && mine.iter().all(|r| r.state == 1);
            let mut a = vec![racing as u8, players.min(255) as u8, name.len() as u8];
            a.extend_from_slice(name.as_bytes());
            return enc(true, ty::ANNOUNCE, 0, h.game, h.ver, 0, &a);
        }
        let open = mine.iter().filter(|r| r.state == 0 && r.players() < r.slots.len()).count();
        let mut a = vec![name.len() as u8];
        a.extend_from_slice(name.as_bytes());
        a.extend_from_slice(&(players.min(65535) as u16).to_le_bytes());
        a.extend_from_slice(&(mine.len() as u16).to_le_bytes());
        a.extend_from_slice(&(open as u16).to_le_bytes());
        enc(false, ty::ANNOUNCE, 0, h.game, h.ver, 0, &a)
    }

    /// One received datagram.
    pub fn handle(&mut self, data: &[u8], from: SocketAddr, now: Instant, out: &mut Out) {
        let h = match parse(data) {
            Some(h) => h,
            None => return,
        };
        let p = &data[h.hlen..];
        let mem = self.member(from);

        if let Some((r, s)) = mem {
            // a member speaks its own protocol, about its own room (crosstalk: a header naming another room is dropped)
            let ok = {
                let room = &self.rooms[r];
                let m = room.slots[s].as_ref().unwrap();
                m.legacy == h.legacy
                    && (h.legacy || (h.game == room.game && h.ver == room.ver && (h.room == room.id || h.kind == ty::ROOMS || h.kind == ty::DISCOVER)))
            };
            if !ok {
                self.n_dropped += 1;
                return;
            }
            let (pps, fpps) = (self.cfg.member_pps, self.cfg.frame_pps);
            let m = self.rooms[r].slots[s].as_mut().unwrap();
            m.last_seen = now;
            if now.duration_since(m.rate_t) >= Duration::from_secs(1) {
                m.rate_t = now;
                m.rate_n = 0;
            }
            m.rate_n += 1;
            if m.rate_n > pps {
                self.n_dropped += 1;
                return;
            }
            if h.kind == ty::FRAME {
                if now.duration_since(m.frame_t) >= Duration::from_secs(1) {
                    m.frame_t = now;
                    m.frame_n = 0;
                }
                m.frame_n += 1;
                if m.frame_n > fpps {
                    self.n_dropped += 1;
                    return;
                }
            }
            return self.member_msg(r, s, &h, p, data, from, now, out);
        }

        // a stranger: only what earns a reply is charged to the per-IP budget; everything else (a restarted or ghost client's
        // FRAMEs, stray ACKs) is dropped for free, so it cannot starve the PING that tells that client to rejoin (a shared NAT)
        if !matches!(h.kind, ty::HELLO | ty::PING | ty::DISCOVER | ty::ROOMS | ty::DELROOM) {
            return;
        }
        if !h.legacy && !game_ok(h.game) {
            return;
        }
        if !self.allow_unknown(from.ip(), data.len(), now) {
            self.n_dropped += 1;
            return;
        }
        match h.kind {
            ty::HELLO => self.on_hello(&h, p, from, now, out),
            ty::PING => {
                if p.len() >= 4 {
                    // not a member (the server restarted, or timed this client out): the short rejection says rejoin
                    self.reject(from, &h, why::NOT_MEMBER, false, now, out);
                }
            }
            ty::DISCOVER => {
                let m = self.announce(&h);
                self.reply_unknown(from, m, now, out);
            }
            ty::ROOMS => {
                let m = self.roomlist(&h, from);
                self.reply_unknown(from, m, now, out);
            }
            ty::DELROOM => {
                self.delroom(&h, p);
                let m = self.roomlist(&h, from);
                self.reply_unknown(from, m, now, out);
            }
            _ => {}
        }
    }

    /// DELROOM: only an EMPTY room of the asker's game that is not racing
    fn delroom(&mut self, h: &Hdr, p: &[u8]) {
        let id = if h.legacy {
            match p.first() {
                Some(&b) => b as u16,
                None => return,
            }
        } else {
            h.room
        };
        if let Some(r) = self.rooms.iter().position(|r| r.game == h.game && r.id == id) {
            if self.rooms[r].players() == 0 && self.rooms[r].state == 0 && (!h.legacy || self.rooms[r].ver == LEGACY_VER) {
                let t = self.rooms.remove(r).tag();
                self.events.push(format!("[{}] room deleted", t));
            }
        }
    }

    #[allow(clippy::too_many_arguments)]
    fn member_msg(&mut self, r: usize, s: usize, h: &Hdr, p: &[u8], data: &[u8], from: SocketAddr, now: Instant, out: &mut Out) {
        let legacy = h.legacy;
        match h.kind {
            ty::HELLO => self.rooms[r].welcome(from, legacy, s, h.seq, out), // duplicate: re-answer, no second join
            ty::READY => {
                let room = &mut self.rooms[r];
                if room.state == 0 && !p.is_empty() {
                    room.slots[s].as_mut().unwrap().ready = p[0] != 0;
                    room.roster_changed(out);
                }
            }
            ty::START => {
                if self.rooms[r].state == 0 {
                    let rnd = self.rand();
                    let room = &mut self.rooms[r];
                    room.start(rnd, now, out, &mut self.events);
                    if let Some(st) = self.stats.as_mut() {
                        let names: Vec<String> = room.slots.iter().flatten().map(|m| m.name.clone()).collect();
                        st.session_start(room.game, &names);
                    }
                }
            }
            ty::ACK => {
                if p.len() >= 2 {
                    let m = self.rooms[r].slots[s].as_mut().unwrap();
                    if le16(p) == m.go_seq {
                        m.go_acked = true;
                    }
                }
            }
            ty::LEAVE => {
                let room = &mut self.rooms[r];
                let m = room.slots[s].take().unwrap();
                self.events.push(format!("[{}] leave slot {} '{}'", room.tag(), s, m.name));
                room.roster_changed(out);
            }
            ty::FRAME => {
                // exactly one legal size per room: an oversize datagram would be copied to every peer
                let room = &self.rooms[r];
                if room.state != 1 || p.len() != 9 + room.frame_size as usize {
                    self.n_dropped += 1;
                    return;
                }
                if le32(p) != room.session_id || p[8] as usize != s {
                    self.n_dropped += 1;
                    return;
                }
                let mut other: Option<Vec<u8>> = None;
                for (i, m) in room.slots.iter().enumerate() {
                    if let Some(m) = m {
                        if i == s {
                            continue;
                        }
                        if m.legacy == legacy {
                            out.push((m.addr, data.to_vec())); // verbatim
                        } else {
                            if other.is_none() {
                                other = Some(room.msg(m.legacy, ty::FRAME, 0, p)); // bridged: same payload, the peer's header
                            }
                            out.push((m.addr, other.as_ref().unwrap().clone()));
                        }
                    }
                }
                let room = &mut self.rooms[r];
                room.relayed += 1;
                room.last_frame = now;
                self.n_frames += 1;
            }
            ty::PING => {
                if p.len() >= 4 {
                    let m = self.rooms[r].msg(legacy, ty::PONG, 0, &p[..4]);
                    out.push((from, m));
                }
            }
            ty::CHAT => self.on_chat(r, s, p, now, out),
            ty::RENAME => {
                if let Some(raw) = field(p, 0, 16) {
                    let name = clean_name(raw);
                    let room = &mut self.rooms[r];
                    let tag = room.tag();
                    let m = room.slots[s].as_mut().unwrap();
                    if name != m.name {
                        self.events.push(format!("[{}] slot {} renamed '{}' -> '{}'", tag, s, m.name, name));
                        m.name = name;
                        room.roster_changed(out);
                    }
                }
            }
            ty::ROOMS => out.push((from, self.roomlist(h, from))),
            ty::DELROOM => {
                self.delroom(h, p);
                out.push((from, self.roomlist(h, from)));
            }
            ty::DISCOVER => out.push((from, self.announce(h))),
            _ => {} // unknown types: ignored (the contract)
        }
    }

    fn on_chat(&mut self, r: usize, s: usize, p: &[u8], now: Instant, out: &mut Out) {
        let text = clean_text(p);
        if text.is_empty() {
            return;
        }
        let room = &mut self.rooms[r];
        {
            let m = room.slots[s].as_mut().unwrap();
            if now.duration_since(m.chat_t) >= Duration::from_secs(2) {
                m.chat_t = now;
                m.chat_n = 0;
            }
            m.chat_n += 1;
            if m.chat_n > 5 {
                return; // 5 lines per 2 s per player
            }
        }
        let mut pl = vec![s as u8];
        pl.extend_from_slice(text.as_bytes());
        room.broadcast(ty::CHAT, &pl, out); // the sender gets its own line back: what it sees is what was relayed
    }

    /// Timers: liveness drop, GO retransmission, session end, empty-room close, status lines. Call at ~10 Hz or faster.
    pub fn tick(&mut self, now: Instant, out: &mut Out) {
        let cfg = self.cfg.clone();
        for room in self.rooms.iter_mut() {
            let mut changed = false;
            for i in 0..room.slots.len() {
                let gone = room.slots[i].as_ref().map_or(false, |m| now.duration_since(m.last_seen) > LIVENESS);
                if gone {
                    let m = room.slots[i].take().unwrap();
                    self.events.push(format!("[{}] timeout slot {} '{}'", room.tag(), i, m.name));
                    changed = true;
                }
            }
            if changed {
                room.roster_changed(out);
            }
            if room.state == 1 {
                let ended = room.players() == 0 || now.duration_since(room.session_start) > cfg.max_session || now.duration_since(room.last_frame) > cfg.frame_idle;
                if ended {
                    if let Some(st) = self.stats.as_mut() {
                        st.session_end(room.game, room.start_players, now.duration_since(room.session_start).as_secs());
                    }
                }
                if room.players() == 0 {
                    room.end_session("everyone left", out, &mut self.events);
                } else if now.duration_since(room.session_start) > cfg.max_session {
                    room.end_session("time limit", out, &mut self.events);
                } else if now.duration_since(room.last_frame) > cfg.frame_idle {
                    room.end_session("no traffic", out, &mut self.events);
                } else {
                    for i in 0..room.slots.len() {
                        let due = room.slots[i].as_ref().map_or(false, |m| !m.go_acked && now.duration_since(m.last_go) >= GO_RESEND);
                        if due {
                            room.send_go(i, now, out);
                        }
                    }
                }
            }
            let empty = room.players() == 0 && room.state == 0;
            if !empty {
                room.empty_since = None;
            } else if room.empty_since.is_none() {
                room.empty_since = Some(now);
            }
        }
        let mut r = self.rooms.len();
        while r > 0 {
            r -= 1;
            if let Some(t) = self.rooms[r].empty_since {
                if now.duration_since(t) >= cfg.room_ttl {
                    let tag = self.rooms.remove(r).tag();
                    self.events.push(format!("[{}] room closed (empty for {} min)", tag, cfg.room_ttl.as_secs() / 60));
                }
            }
        }
        if cfg.status_every > Duration::from_secs(0) && now.duration_since(self.last_status) >= cfg.status_every {
            self.last_status = now;
            for room in &self.rooms {
                if room.players() > 0 {
                    let rr1 = room.slots.iter().flatten().filter(|m| m.legacy).count();
                    self.events.push(format!(
                        "[status] {} '{}' v{} {} {}/{}{} frames {}",
                        room.tag(),
                        room.name(),
                        room.ver,
                        if room.state == 1 { "racing" } else { "lobby" },
                        room.players(),
                        room.slots.len(),
                        if rr1 > 0 { format!(" ({} RRN1)", rr1) } else { String::new() },
                        room.relayed
                    ));
                }
            }
            self.events.push(format!(
                "[status] {} room(s), {} player(s), {} frames relayed, {} datagrams dropped",
                self.rooms.len(),
                self.players(),
                self.n_frames,
                self.n_dropped
            ));
        }
    }
}

pub mod stats;

#[cfg(test)]
mod tests;
