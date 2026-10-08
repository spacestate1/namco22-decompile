//! Conformance tests: RRN1 (the old Rave Racer protocol, ported from the rrn1-server suite) and NMN2 (several games at once,
//! isolation, version rules, bridging, hardening). No sockets: a fake clock drives `Hub` directly.
use super::*;
use std::net::{Ipv4Addr, SocketAddrV4};

fn addr(ip: u8, port: u16) -> SocketAddr {
    SocketAddr::V4(SocketAddrV4::new(Ipv4Addr::new(10, 0, 0, ip), port))
}
fn m1(kind: u8, seq: u16, p: &[u8]) -> Vec<u8> {
    enc(true, kind, seq, LEGACY_GAME, LEGACY_VER, 0, p)
}
fn hello1(name: &str) -> Vec<u8> {
    let mut p = vec![name.len() as u8];
    p.extend_from_slice(name.as_bytes());
    m1(ty::HELLO, 7, &p)
}
fn hello1_room(name: &str, sel: u8, label: Option<&str>) -> Vec<u8> {
    let mut p = vec![name.len() as u8];
    p.extend_from_slice(name.as_bytes());
    p.push(sel);
    if let Some(l) = label {
        p.push(l.len() as u8);
        p.extend_from_slice(l.as_bytes());
    }
    m1(ty::HELLO, 7, &p)
}
fn frame1(session: u32, fseq: u32, cab: u8) -> Vec<u8> {
    let mut p = session.to_le_bytes().to_vec();
    p.extend_from_slice(&fseq.to_le_bytes());
    p.push(cab);
    p.extend_from_slice(&[0xAB; 40]);
    m1(ty::FRAME, 0, &p)
}

/// an NMN2 client's view: game, version, frame size
#[derive(Clone, Copy)]
struct G {
    game: [u8; 2],
    ver: u16,
    fsize: u16,
    maxp: u8,
}
const RR: G = G { game: *b"rr", ver: 1, fsize: 40, maxp: 8 };
const TW: G = G { game: *b"tw", ver: 3, fsize: 100, maxp: 8 };
const CS: G = G { game: *b"cs", ver: 1, fsize: 64, maxp: 8 };
impl G {
    fn m(&self, kind: u8, seq: u16, room: u16, p: &[u8]) -> Vec<u8> {
        enc(false, kind, seq, self.game, self.ver, room, p)
    }
    fn hello(&self, name: &str, md: u8, room: u16, label: Option<&str>) -> Vec<u8> {
        let mut p = vec![name.len() as u8];
        p.extend_from_slice(name.as_bytes());
        p.push(md);
        p.push(self.maxp);
        p.extend_from_slice(&self.fsize.to_le_bytes());
        let l = label.unwrap_or("");
        p.push(l.len() as u8);
        p.extend_from_slice(l.as_bytes());
        while p.len() < 128 {
            p.push(0); // padding: the anti-amplification credit
        }
        self.m(ty::HELLO, 9, room, &p)
    }
    fn frame(&self, room: u16, session: u32, fseq: u32, slot: u8, fill: u8) -> Vec<u8> {
        let mut p = session.to_le_bytes().to_vec();
        p.extend_from_slice(&fseq.to_le_bytes());
        p.push(slot);
        p.extend(std::iter::repeat(fill).take(self.fsize as usize));
        self.m(ty::FRAME, 0, room, &p)
    }
    fn rooms(&self) -> Vec<u8> {
        self.m(ty::ROOMS, 0, 0, &[0u8; 400])
    }
}

struct T {
    h: Hub,
    now: Instant,
}
impl T {
    fn with(cfg: Config) -> T {
        let now = Instant::now();
        T { h: Hub::new(cfg, now, 12345), now }
    }
    fn new() -> T {
        T::with(Config { unknown_pps: 1000, unknown_allow: 1 << 20, status_every: Duration::from_secs(0), ..Config::default() })
    }
    fn send(&mut self, from: SocketAddr, d: &[u8]) -> Out {
        let mut o = Out::new();
        self.h.handle(d, from, self.now, &mut o);
        o
    }
    fn advance(&mut self, ms: u64) -> Out {
        self.now += Duration::from_millis(ms);
        let mut o = Out::new();
        self.h.tick(self.now, &mut o);
        o
    }
}
fn kinds(o: &Out) -> Vec<u8> {
    o.iter().map(|(_, d)| d[4]).collect()
}
fn to(o: &Out, a: SocketAddr) -> Vec<&Vec<u8>> {
    o.iter().filter(|(t, _)| *t == a).map(|(_, d)| d).collect()
}
fn first(o: &Out, a: SocketAddr, kind: u8) -> Vec<u8> {
    to(o, a).into_iter().find(|d| d[4] == kind).unwrap_or_else(|| panic!("no 0x{:02x} to {}", kind, a)).clone()
}
fn has(o: &Out, a: SocketAddr, kind: u8) -> bool {
    to(o, a).iter().any(|d| d[4] == kind)
}
fn pl(d: &[u8]) -> &[u8] {
    &d[parse(d).unwrap().hlen..]
}
/// (slot, reason) of a WELCOME in either protocol (reason 0 for RRN1)
fn welcome(o: &Out, a: SocketAddr) -> (u8, u8) {
    let w = first(o, a, ty::WELCOME);
    let h = parse(&w).unwrap();
    let p = pl(&w);
    (p[0], if h.legacy { 0 } else { p[6] })
}
fn room_of(d: &[u8]) -> u16 {
    parse(d).unwrap().room
}

// ---------------------------------------------------------------- RRN1 (released Rave Racer builds) --------------------

#[test]
fn rrn1_join_assigns_lowest_free_slot_and_welcome_echoes_hello_seq() {
    let mut t = T::new();
    let o = t.send(addr(1, 1), &hello1("ALPHA"));
    let w = first(&o, addr(1, 1), ty::WELCOME);
    assert_eq!(&w[0..4], MAGIC1);
    assert_eq!(le16(&w[6..8]), 7, "WELCOME must carry the HELLO's seq");
    assert_eq!(w[10], 0, "first slot");
    let o = t.send(addr(2, 1), &hello1("BETA"));
    assert_eq!(welcome(&o, addr(2, 1)).0, 1);
    assert_eq!(kinds(&o).iter().filter(|k| **k == ty::ROSTER).count(), 2, "ROSTER to both members");
    t.send(addr(1, 1), &m1(ty::LEAVE, 0, &[]));
    let o = t.send(addr(3, 1), &hello1("GAMMA"));
    assert_eq!(welcome(&o, addr(3, 1)).0, 0);
}

#[test]
fn rrn1_duplicate_hello_is_answered_without_a_second_join() {
    let mut t = T::new();
    t.send(addr(1, 1), &hello1("A"));
    let o = t.send(addr(1, 1), &hello1("A"));
    assert_eq!(kinds(&o), vec![ty::WELCOME]);
    assert_eq!(t.h.players(), 1);
}

fn race1() -> (T, SocketAddr, SocketAddr, u32) {
    let mut t = T::new();
    let (a, b) = (addr(1, 5000), addr(2, 5000));
    t.send(a, &hello1("A"));
    t.send(b, &hello1("B"));
    let o = t.send(a, &m1(ty::START, 9, &[]));
    let go_a = first(&o, a, ty::GO);
    let go_b = first(&o, b, ty::GO);
    let sid = le32(&go_a[10..14]);
    t.send(a, &m1(ty::ACK, 0, &go_a[6..8]));
    t.send(b, &m1(ty::ACK, 0, &go_b[6..8]));
    (t, a, b, sid)
}

#[test]
fn rrn1_start_sends_go_to_all_and_resends_until_acked() {
    let mut t = T::new();
    let (a, b) = (addr(1, 1), addr(2, 1));
    t.send(a, &hello1("A"));
    t.send(b, &hello1("B"));
    let o = t.send(a, &m1(ty::START, 3, &[]));
    let ga = first(&o, a, ty::GO);
    let gb = first(&o, b, ty::GO);
    assert_eq!(le32(&ga[10..14]), le32(&gb[10..14]));
    assert_ne!(le32(&ga[10..14]), 0);
    t.send(a, &m1(ty::ACK, 0, &ga[6..8]));
    let o = t.advance(600);
    assert!(!has(&o, a, ty::GO), "acked: no resend");
    assert!(has(&o, b, ty::GO), "not acked: resent");
    assert_eq!(le16(&first(&o, b, ty::GO)[6..8]), le16(&gb[6..8]), "a resend keeps its seq");
}

#[test]
fn rrn1_frames_are_relayed_verbatim_to_others_only_and_bad_ones_dropped() {
    let (mut t, a, b, sid) = race1();
    let f = frame1(sid, 1, 0);
    let o = t.send(a, &f);
    assert_eq!(o, vec![(b, f.clone())]);
    assert!(t.send(a, &frame1(sid ^ 1, 2, 0)).is_empty(), "wrong session");
    assert!(t.send(a, &frame1(sid, 2, 1)).is_empty(), "wrong cab id");
    let mut long = frame1(sid, 3, 0);
    long.push(0);
    long[8] += 1;
    assert!(t.send(a, &long).is_empty(), "wrong size");
    assert!(t.send(addr(9, 9), &frame1(sid, 4, 0)).is_empty(), "a stranger");
}

#[test]
fn malformed_datagrams_are_ignored() {
    let mut t = T::new();
    for d in [&b"RRN1"[..], &b"XXXX\x01\x00\x00\x00\x00\x00"[..], &b"RRN1\x01\x00\x00\x00\x05\x00ab"[..], &b"NMN2\x01\x00\x00\x00\x00\x00rr"[..]] {
        assert!(t.send(addr(1, 1), d).is_empty());
    }
    let mut bad = RR.hello("X", mode::AUTO, 0, None);
    bad[8] = 3; // length field lies
    assert!(t.send(addr(1, 1), &bad).is_empty());
    assert_eq!(t.h.players(), 0);
}

#[test]
fn names_are_cleaned_and_truncated_by_bytes() {
    let mut t = T::new();
    let o = t.send(addr(1, 1), &hello1("AB\x07CDEFGHIJKLMNOPQRSTUV"));
    let w = first(&o, addr(1, 1), ty::WELCOME);
    let name_len = w[10 + 6 + 1 + 2] as usize;
    assert_eq!(name_len, 15, "16 bytes taken, the control character stripped");
    let o = t.send(addr(2, 1), &hello1("ééééééééé")); // 18 bytes of 2-byte characters: cut at 16, not mid-character
    let w = first(&o, addr(2, 1), ty::WELCOME);
    assert!(String::from_utf8(w[10..].to_vec()).is_ok() || w.windows(2).filter(|x| x == &"é".as_bytes()).count() == 8);
}

#[test]
fn rrn1_liveness_timeout_frees_slot_and_ends_session() {
    let (mut t, a, b, _) = race1();
    for _ in 0..12 {
        t.send(a, &m1(ty::PING, 0, &[1, 2, 3, 4]));
        t.advance(500);
    }
    assert_eq!(t.h.players(), 1, "b timed out");
    for _ in 0..12 {
        t.advance(500);
    }
    assert_eq!(t.h.players(), 0);
    assert_eq!(t.h.room_info(&LEGACY_GAME, 1).map(|r| r.0), Some(0), "back in the lobby");
    let _ = b;
}

#[test]
fn rrn1_ping_echoes_and_discover_announces() {
    let mut t = T::new();
    t.send(addr(1, 1), &hello1("A"));
    let o = t.send(addr(1, 1), &m1(ty::PING, 0, &[9, 8, 7, 6]));
    let p = first(&o, addr(1, 1), ty::PONG);
    assert_eq!(&p[10..14], &[9, 8, 7, 6]);
    let o = t.send(addr(5, 5), &m1(ty::DISCOVER, 0, &[]));
    let a = first(&o, addr(5, 5), ty::ANNOUNCE);
    assert_eq!(&a[0..4], MAGIC1);
    assert_eq!(a[11], 1, "one Rave Racer player");
}

#[test]
fn ping_from_a_non_member_gets_the_short_rejection() {
    let mut t = T::new();
    let o = t.send(addr(1, 1), &m1(ty::PING, 0, &[1, 2, 3, 4]));
    assert_eq!(welcome(&o, addr(1, 1)).0, 0xFF);
    assert!(first(&o, addr(1, 1), ty::WELCOME).len() <= 17, "short");
    let o = t.send(addr(2, 1), &TW.m(ty::PING, 0, 1, &[1, 2, 3, 4]));
    assert_eq!(welcome(&o, addr(2, 1)), (0xFF, why::NOT_MEMBER));
}

#[test]
fn unknown_sources_are_rate_limited_members_are_not() {
    let mut t = T::with(Config { status_every: Duration::from_secs(0), ..Config::default() });
    let mut answered = 0;
    for i in 0..100 {
        if !t.send(addr(9, 100 + i), &m1(ty::DISCOVER, 0, &[])).is_empty() {
            answered += 1;
        }
    }
    assert_eq!(answered, 30, "30 a second per IP");
    t.send(addr(1, 1), &hello1("A"));
    let mut pongs = 0;
    for _ in 0..100 {
        pongs += t.send(addr(1, 1), &m1(ty::PING, 0, &[0; 4])).len();
    }
    assert_eq!(pongs, 100);
}

fn run1(t: &mut T, a: SocketAddr, b: SocketAddr, secs: u64, sid: Option<u32>) {
    for k in 0..secs * 2 {
        t.send(a, &m1(ty::PING, 0, &[0; 4]));
        t.send(b, &m1(ty::PING, 0, &[0; 4]));
        if let Some(s) = sid {
            t.send(a, &frame1(s, k as u32, 0));
        }
        t.advance(500);
    }
}

#[test]
fn rrn1_session_ends_on_time_limit_or_silence_and_players_stay() {
    let (mut t, a, b, sid) = race1();
    run1(&mut t, a, b, 60, Some(sid));
    assert_eq!(t.h.room_info(&LEGACY_GAME, 1).unwrap().0, 1, "frames keep it alive");
    run1(&mut t, a, b, 95, None);
    assert_eq!(t.h.room_info(&LEGACY_GAME, 1).unwrap(), (0, 2, 0), "90 s with no frame: back to the lobby");
    let o = t.send(a, &m1(ty::START, 11, &[]));
    assert!(has(&o, b, ty::GO), "a new race can start at once");
    let (mut t, a, b, sid) = race1();
    run1(&mut t, a, b, 15 * 60 + 5, Some(sid));
    assert_eq!(t.h.room_info(&LEGACY_GAME, 1).unwrap(), (0, 2, 0), "15 minutes");
}

fn chat1(text: &[u8]) -> Vec<u8> {
    m1(ty::CHAT, 0, text)
}

#[test]
fn chat_is_relayed_to_everyone_cleaned_and_rate_limited() {
    let (mut t, a, b, _) = race1();
    let o = t.send(b, &chat1(b"  hi\x01 there  "));
    let c = first(&o, a, ty::CHAT);
    assert_eq!(&c[10..], b"\x01hi there");
    assert!(has(&o, b, ty::CHAT), "the sender gets it back");
    let mut n = 0;
    for _ in 0..10 {
        n += t.send(a, &chat1(b"x")).len();
    }
    assert_eq!(n, 5 * 2, "5 lines per 2 s per player, each to both members");
    let long = vec![b'z'; 300];
    t.advance(2100);
    for _ in 0..4 {
        t.send(a, &m1(ty::PING, 0, &[0; 4]));
    }
    let o = t.send(a, &chat1(&long));
    assert_eq!(first(&o, a, ty::CHAT).len(), 10 + 1 + MAX_CHAT);
    assert!(t.send(addr(9, 9), &chat1(b"spam")).is_empty(), "strangers cannot chat");
}

#[test]
fn a_flood_of_stranger_frames_does_not_starve_its_ping() {
    let mut t = T::with(Config { status_every: Duration::from_secs(0), ..Config::default() });
    for i in 0..500 {
        t.send(addr(3, 3), &frame1(1, i, 0));
    }
    let o = t.send(addr(3, 3), &m1(ty::PING, 0, &[0; 4]));
    assert_eq!(welcome(&o, addr(3, 3)).0, 0xFF);
}

#[test]
fn per_ip_slot_cap_counts_every_room() {
    let mut t = T::with(Config { max_per_ip: 3, unknown_pps: 1000, unknown_allow: 1 << 20, ..Config::default() });
    t.send(addr(1, 1), &hello1("A"));
    t.send(addr(1, 2), &TW.hello("B", mode::AUTO, 0, None));
    t.send(addr(1, 3), &CS.hello("C", mode::AUTO, 0, None));
    let o = t.send(addr(1, 4), &hello1("D"));
    assert_eq!(welcome(&o, addr(1, 4)).0, 0xFF);
    let o = t.send(addr(1, 5), &TW.hello("E", mode::CREATE, 0, None));
    assert_eq!(welcome(&o, addr(1, 5)), (0xFF, why::PER_IP));
    assert_eq!(t.h.players(), 3);
}

#[test]
fn rrn1_a_second_room_opens_while_the_first_is_racing_and_newcomers_gather() {
    let (mut t, _a, _b, _) = race1();
    let o = t.send(addr(3, 1), &hello1("C"));
    assert_eq!(welcome(&o, addr(3, 1)).0, 0);
    assert_eq!(t.h.rooms_of(&LEGACY_GAME), 2);
    let o = t.send(addr(4, 1), &hello1("D"));
    assert_eq!(welcome(&o, addr(4, 1)).0, 1, "into the fullest lobby (room 2), not a third room");
    assert_eq!(t.h.rooms_of(&LEGACY_GAME), 2);
}

#[test]
fn rrn1_rejects_when_every_room_is_racing_at_the_limit() {
    let mut t = T::with(Config { max_rooms_per_game: 1, unknown_pps: 1000, unknown_allow: 1 << 20, ..Config::default() });
    t.send(addr(1, 1), &hello1("A"));
    t.send(addr(1, 1), &m1(ty::START, 1, &[]));
    let o = t.send(addr(2, 1), &hello1("B"));
    let w = first(&o, addr(2, 1), ty::WELCOME);
    assert_eq!((w[10], w[11]), (0xFF, 1), "race in progress");
    assert!(w.len() <= 17, "short");
}

fn roomlist(o: &Out, a: SocketAddr) -> Vec<(u16, u16, u8, u8, u8, String)> {
    let d = first(o, a, ty::ROOMLIST);
    let h = parse(&d).unwrap();
    let p = pl(&d);
    let mut v = Vec::new();
    let mut off = 1;
    for _ in 0..p[0] {
        let (id, ver, rest) = if h.legacy { (p[off] as u16, LEGACY_VER, off + 1) } else { (le16(&p[off..]), le16(&p[off + 2..]), off + 4) };
        let (state, players) = (p[rest], p[rest + 1]);
        let (maxp, mine, nl_at) = if h.legacy { (8, p[rest + 2], rest + 3) } else { (p[rest + 2], p[rest + 3], rest + 4) };
        let nl = p[nl_at] as usize;
        let name = String::from_utf8_lossy(&p[nl_at + 1..nl_at + 1 + nl]).into_owned();
        let _ = maxp;
        v.push((id, ver, state, players, mine, name));
        off = nl_at + 1 + nl;
    }
    v
}

#[test]
fn rrn1_rooms_list_create_named_join_and_delete() {
    let mut t = T::new();
    t.send(addr(1, 1), &hello1_room("A", ROOM_NEW, Some("Fast lane")));
    t.send(addr(2, 1), &hello1_room("B", ROOM_NEW, None));
    let o = t.send(addr(1, 1), &m1(ty::ROOMS, 0, &[]));
    let l = roomlist(&o, addr(1, 1));
    assert_eq!(l.len(), 2);
    assert_eq!(l[0].5, "Fast lane");
    assert_eq!(l[0].4, 1, "mine");
    assert_eq!(l[1].5, "B's room");
    let o = t.send(addr(3, 1), &hello1_room("C", 2, None));
    assert_eq!(welcome(&o, addr(3, 1)).0, 1, "joined room 2 as its second player");
    let o = t.send(addr(4, 1), &hello1_room("D", 77, None));
    assert_eq!(welcome(&o, addr(4, 1)).0, 0xFF, "no such room");
    t.send(addr(1, 1), &m1(ty::LEAVE, 0, &[]));
    let o = t.send(addr(2, 1), &m1(ty::DELROOM, 0, &[2]));
    assert_eq!(roomlist(&o, addr(2, 1)).len(), 2, "occupied: kept");
    let o = t.send(addr(2, 1), &m1(ty::DELROOM, 0, &[1]));
    assert_eq!(roomlist(&o, addr(2, 1)).len(), 1, "empty: deleted");
}

#[test]
fn rename_updates_the_roster() {
    let mut t = T::new();
    t.send(addr(1, 1), &hello1("A"));
    let o = t.send(addr(1, 1), &m1(ty::RENAME, 0, b"\x04Zed!"));
    let r = first(&o, addr(1, 1), ty::ROSTER);
    assert!(r.windows(4).any(|w| w == b"Zed!"));
}

#[test]
fn empty_rooms_close_after_their_ttl_and_make_space_at_the_limit() {
    let mut t = T::with(Config { max_rooms_per_game: 2, unknown_pps: 1000, unknown_allow: 1 << 20, ..Config::default() });
    t.send(addr(1, 1), &TW.hello("A", mode::CREATE, 0, None));
    t.send(addr(1, 1), &TW.m(ty::LEAVE, 0, 1, &[]));
    t.send(addr(2, 1), &TW.hello("B", mode::CREATE, 0, None));
    t.send(addr(3, 1), &TW.hello("C", mode::CREATE, 0, None)); // at the limit: the empty room 1 makes space
    assert_eq!(t.h.rooms_of(&TW.game), 2);
    assert_eq!(t.h.players(), 2);
    t.send(addr(2, 1), &TW.m(ty::LEAVE, 0, 2, &[]));
    for _ in 0..(10 * 60 * 2 + 2) {
        t.send(addr(3, 1), &TW.m(ty::PING, 0, 1, &[0; 4]));
        t.advance(500);
    }
    assert_eq!(t.h.rooms_of(&TW.game), 1, "the empty room closed after 10 minutes, the occupied one stays");
}

// ---------------------------------------------------------------- NMN2 -------------------------------------------------

#[test]
fn nmn2_welcome_carries_the_room_and_its_shape() {
    let mut t = T::new();
    let o = t.send(addr(1, 1), &TW.hello("A", mode::CREATE, 0, Some("Tokyo")));
    let w = first(&o, addr(1, 1), ty::WELCOME);
    let h = parse(&w).unwrap();
    assert!(!h.legacy);
    assert_eq!((h.game, h.ver, h.room, h.seq), (TW.game, TW.ver, 1, 9));
    let p = pl(&w);
    assert_eq!((p[0], p[1], p[6], p[7], le16(&p[8..])), (0, 0, why::OK, 4, 100), "Tokyo Wars links 4 cabinets");
    let o = t.send(addr(2, 1), &TW.hello("B", mode::JOIN, 1, None));
    assert_eq!(welcome(&o, addr(2, 1)), (1, why::OK));
    let o = t.send(addr(3, 1), &TW.hello("C", mode::AUTO, 0, None));
    assert_eq!(room_of(&first(&o, addr(3, 1), ty::WELCOME)), 1, "auto: the open lobby");
}

#[test]
fn nmn2_refusals_say_why() {
    let mut t = T::new();
    t.send(addr(1, 1), &TW.hello("A", mode::CREATE, 0, None));
    let old = G { ver: 2, ..TW };
    let o = t.send(addr(2, 1), &old.hello("B", mode::JOIN, 1, None));
    assert_eq!(welcome(&o, addr(2, 1)), (0xFF, why::VERSION));
    let small = G { fsize: 50, ..TW };
    let o = t.send(addr(2, 2), &small.hello("B", mode::JOIN, 1, None));
    assert_eq!(welcome(&o, addr(2, 2)), (0xFF, why::VERSION), "another frame size is another version");
    let o = t.send(addr(2, 3), &TW.hello("B", mode::JOIN, 9, None));
    assert_eq!(welcome(&o, addr(2, 3)), (0xFF, why::NO_ROOM));
    let o = t.send(addr(2, 4), &old.hello("B", mode::AUTO, 0, None));
    assert_eq!(room_of(&first(&o, addr(2, 4), ty::WELCOME)), 2, "auto never mixes versions: a room of its own");
    let huge = G { fsize: 721, ..TW };
    let o = t.send(addr(2, 5), &huge.hello("B", mode::CREATE, 0, None));
    assert_eq!(welcome(&o, addr(2, 5)), (0xFF, why::BAD), "over the game's frame limit");
    let bad = G { game: *b"R!", ..TW };
    assert!(t.send(addr(2, 6), &bad.hello("B", mode::CREATE, 0, None)).is_empty(), "a game id must be 2 lowercase letters/digits");
    t.send(addr(1, 1), &TW.m(ty::START, 1, 1, &[]));
    let o = t.send(addr(2, 7), &TW.hello("B", mode::JOIN, 1, None));
    assert_eq!(welcome(&o, addr(2, 7)), (0xFF, why::RACING));
    let w = first(&o, addr(2, 7), ty::WELCOME);
    assert!(w.len() <= HDR2 + 11, "short");
}

#[test]
fn nmn2_game_limits_cap_the_room() {
    let mut t = T::new();
    let o = t.send(addr(1, 1), &CS.hello("A", mode::CREATE, 0, None)); // asks for 8, Cyber Sled has 2 cabinets
    assert_eq!(pl(&first(&o, addr(1, 1), ty::WELCOME))[7], 2);
    t.send(addr(2, 1), &CS.hello("B", mode::JOIN, 1, None));
    let o = t.send(addr(3, 1), &CS.hello("C", mode::JOIN, 1, None));
    assert_eq!(welcome(&o, addr(3, 1)), (0xFF, why::FULL));
}

/// two players in a started race of game g, room `room`; returns the session id
fn race2(t: &mut T, g: G, a: SocketAddr, b: SocketAddr) -> (u16, u32) {
    let o = t.send(a, &g.hello("P1", mode::CREATE, 0, None));
    let room = room_of(&first(&o, a, ty::WELCOME));
    t.send(b, &g.hello("P2", mode::JOIN, room, None));
    t.send(a, &g.m(ty::READY, 1, room, &[1]));
    t.send(b, &g.m(ty::READY, 1, room, &[1]));
    let o = t.send(a, &g.m(ty::START, 2, room, &[]));
    let ga = first(&o, a, ty::GO);
    let gb = first(&o, b, ty::GO);
    t.send(a, &g.m(ty::ACK, 0, room, &ga[6..8]));
    t.send(b, &g.m(ty::ACK, 0, room, &gb[6..8]));
    (room, le32(pl(&ga)))
}

#[test]
fn two_games_race_at_once_without_crosstalk() {
    let mut t = T::new();
    let (r1, r2, t1, t2) = (addr(1, 1), addr(1, 2), addr(2, 1), addr(2, 2));
    let (rroom, rsid) = race2(&mut t, RR, r1, r2);
    let (troom, tsid) = race2(&mut t, TW, t1, t2);
    assert_eq!((rroom, troom), (1, 1), "room ids are per game");
    for k in 0..120u32 {
        let o = t.send(r1, &RR.frame(rroom, rsid, k, 0, 0x11));
        assert_eq!(o.len(), 1);
        assert_eq!(o[0].0, r2);
        assert_eq!(pl(&o[0].1)[9..], [0x11u8; 40][..]);
        let o = t.send(t2, &TW.frame(troom, tsid, k, 1, 0x22));
        assert_eq!(o.len(), 1);
        assert_eq!(o[0].0, t1);
        assert_eq!(parse(&o[0].1).unwrap().game, TW.game);
        if k % 30 == 0 {
            t.advance(500);
        }
    }
    // a member's packet naming the other game or the other room goes nowhere
    assert!(t.send(r1, &TW.frame(troom, tsid, 999, 0, 0x33)).is_empty());
    assert!(t.send(t1, &TW.frame(2, tsid, 999, 0, 0x33)).is_empty());
    assert!(t.send(t1, &RR.frame(rroom, rsid, 999, 0, 0x33)).is_empty());
    // ROOMS lists this game's rooms only
    let o = t.send(addr(5, 5), &TW.rooms());
    let l = roomlist(&o, addr(5, 5));
    assert_eq!(l.len(), 1);
    assert_eq!((l[0].0, l[0].1, l[0].2, l[0].3), (1, TW.ver, 1, 2));
    let o = t.send(addr(5, 6), &CS.rooms());
    assert!(roomlist(&o, addr(5, 6)).is_empty());
    // chat stays in its room
    let o = t.send(t1, &TW.m(ty::CHAT, 0, troom, b"tw only"));
    assert!(o.iter().all(|(a, _)| *a == t1 || *a == t2));
    assert_eq!(t.h.room_info(&RR.game, 1).unwrap().0, 1);
    assert_eq!(t.h.room_info(&TW.game, 1).unwrap().0, 1);
}

#[test]
fn rrn1_and_nmn2_rave_racer_clients_share_a_room_bridged() {
    let mut t = T::new();
    let (old, new) = (addr(1, 1), addr(2, 1));
    t.send(old, &hello1("Old"));
    let o = t.send(new, &RR.hello("New", mode::AUTO, 0, None));
    assert_eq!(welcome(&o, new), (1, why::OK), "the NMN2 client lands in the RRN1 client's lobby");
    let r = first(&o, old, ty::ROSTER);
    assert_eq!(&r[0..4], MAGIC1, "each member hears its own protocol");
    let r2 = first(&o, new, ty::ROSTER);
    assert_eq!(&r2[0..4], MAGIC2);
    assert_eq!(pl(&r), pl(&r2), "the same roster");
    let o = t.send(new, &RR.m(ty::START, 3, 1, &[]));
    let (go1, go2) = (first(&o, old, ty::GO), first(&o, new, ty::GO));
    let sid = le32(pl(&go2));
    assert_eq!(le32(pl(&go1)), sid);
    t.send(old, &m1(ty::ACK, 0, &go1[6..8]));
    t.send(new, &RR.m(ty::ACK, 0, 1, &go2[6..8]));
    let o = t.send(old, &frame1(sid, 5, 0));
    assert_eq!(o.len(), 1);
    assert_eq!(&o[0].1[0..4], MAGIC2);
    assert_eq!(pl(&o[0].1), pl(&frame1(sid, 5, 0)), "bridged: the same payload under an NMN2 header");
    let o = t.send(new, &RR.frame(1, sid, 6, 1, 0x5A));
    assert_eq!(&o[0].1[0..4], MAGIC1);
    assert_eq!(o[0].1.len(), 59, "an RRN1 FRAME is 59 bytes");
    let o = t.send(old, &m1(ty::ROOMS, 0, &[]));
    assert_eq!(roomlist(&o, old)[0].3, 2);
}

#[test]
fn stranger_replies_never_exceed_the_amplification_budget() {
    let mut t = T::with(Config { unknown_pps: 1000, status_every: Duration::from_secs(0), ..Config::default() });
    for i in 0..30u16 {
        let o = t.send(addr(1, 100 + i), &TW.hello(&format!("player number {:02}", i), mode::CREATE, 0, Some("a long room name here!")));
        assert_eq!(welcome(&o, addr(1, 100 + i)).1, why::OK);
        t.advance(1100);
    }
    t.advance(1100);
    let tiny = TW.m(ty::ROOMS, 0, 0, &[]);
    let o = t.send(addr(9, 1), &tiny);
    assert!(o.is_empty(), "an unpadded ROOMS gets no list bigger than its credit");
    let o = t.send(addr(9, 2), &TW.rooms());
    assert_eq!(roomlist(&o, addr(9, 2)).len(), 30, "a padded one does");
    // whatever a stranger IP sends in a second, it gets back at most 3x + the allowance
    t.advance(1100);
    let (mut sent, mut got) = (0usize, 0usize);
    for _ in 0..100 {
        sent += tiny.len();
        got += t.send(addr(9, 3), &tiny).iter().map(|(_, d)| d.len()).sum::<usize>();
        let p = TW.m(ty::PING, 0, 0, &[0; 4]);
        sent += p.len();
        got += t.send(addr(9, 3), &p).iter().map(|(_, d)| d.len()).sum::<usize>();
    }
    assert!(got <= 3 * sent + Config::default().unknown_allow, "got {} for {}", got, sent);
    // the released clients still join with the default budget (their HELLO is not padded)
    let o = t.send(addr(9, 4), &hello1("Old client"));
    assert_eq!(welcome(&o, addr(9, 4)).0, 0);
}

#[test]
fn members_are_rate_limited() {
    let mut t = T::new();
    let (a, b) = (addr(1, 1), addr(2, 1));
    let (room, sid) = race2(&mut t, TW, a, b);
    let mut relayed = 0;
    for k in 0..400 {
        relayed += t.send(a, &TW.frame(room, sid, k, 0, 1)).len();
    }
    assert_eq!(relayed, Config::default().frame_pps as usize, "FRAMEs per second per member");
    t.advance(1000);
    t.send(b, &TW.m(ty::PING, 0, room, &[0; 4]));
    assert_eq!(t.send(a, &TW.frame(room, sid, 1000, 0, 1)).len(), 1, "a new second, a new budget");
}

#[test]
fn every_message_type_has_a_length_rule() {
    let mut t = T::new();
    let (a, b) = (addr(1, 1), addr(2, 1));
    let (room, sid) = race2(&mut t, CS, a, b);
    let mut f = CS.frame(room, sid, 1, 0, 0);
    f.truncate(f.len() - 1);
    let l = (f.len() - HDR2) as u16;
    f[8..10].copy_from_slice(&l.to_le_bytes());
    assert!(t.send(a, &f).is_empty(), "a FRAME must be exactly the room's size");
    assert!(t.send(a, &CS.m(ty::ACK, 0, room, &[1])).is_empty());
    assert!(t.send(a, &CS.m(ty::PING, 0, room, &[1, 2])).is_empty(), "PING needs 4 bytes");
    assert!(t.send(a, &CS.m(ty::READY, 0, room, &[])).is_empty());
    let o = t.send(addr(3, 3), &CS.m(ty::HELLO, 1, 0, &[1, b'x', 0])); // no frame size
    assert_eq!(welcome(&o, addr(3, 3)), (0xFF, why::BAD));
}

#[test]
fn status_lines_name_each_occupied_room() {
    let mut t = T::with(Config { unknown_pps: 1000, unknown_allow: 1 << 20, status_every: Duration::from_secs(60), ..Config::default() });
    let (a, b) = (addr(1, 1), addr(2, 1));
    race2(&mut t, TW, a, b);
    t.send(addr(3, 1), &hello1("Old"));
    t.h.events.clear();
    for _ in 0..62 {
        for a in [addr(1, 1), addr(2, 1)] {
            t.send(a, &TW.m(ty::PING, 0, 1, &[0; 4]));
        }
        t.send(addr(3, 1), &m1(ty::PING, 0, &[0; 4]));
        t.advance(1000);
    }
    let s: Vec<&String> = t.h.events.iter().filter(|e| e.starts_with("[status]")).collect();
    assert!(s.iter().any(|e| e.contains("tw#1") && e.contains("racing")), "{:?}", s);
    assert!(s.iter().any(|e| e.contains("rr#1") && e.contains("RRN1")), "{:?}", s);
}

#[test]
fn stats_count_and_round_trip() {
    use crate::stats::{day_of, Stats};
    let mut st = Stats::new(42);
    let a: IpAddr = "10.0.0.1".parse().unwrap();
    let b: IpAddr = "10.0.0.2".parse().unwrap();
    st.join(*b"cs", a, "Alice", 1);
    st.join(*b"cs", b, "Bob", 2);
    st.join(*b"cs", a, "Alice", 2); // the same person again: one distinct player
    st.join(*b"rr", a, "Alice", 1);
    st.session_start(*b"cs", &["Alice".to_string(), "Bob".to_string()]);
    st.session_end(*b"cs", 2, 600);
    assert_eq!(st.distinct("cs"), 2);
    assert_eq!(st.distinct("rr"), 1);
    assert_eq!(st.distinct("*"), 2);
    assert_eq!(st.games["cs"].joins, 3);
    assert_eq!(st.games["cs"].sessions, 1);
    assert_eq!(st.games["cs"].minutes, 20);
    assert_eq!(st.peak.0, 2);
    let t = st.to_text();
    assert!(!t.contains("10.0.0."), "no addresses in the file");
    let back = Stats::from_text(&t).expect("parses");
    assert_eq!(back.to_text(), t);
    assert_eq!(back.distinct("*"), 2);
    let r = back.report(10);
    assert!(r.contains("Cyber Sled") && r.contains("Alice") && r.contains("Bob"));
    assert_eq!(day_of(0), "1970-01-01");
    assert_eq!(day_of(1_759_795_200), "2025-10-07");
    assert!(Stats::from_text("not a stats file").is_none());
}
