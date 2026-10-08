//! Lifetime statistics, kept in a small text file (`nmn-server --stats-file PATH`, read back with `--show-stats PATH`): how many
//! people have connected, to which games, and what they played. Nothing personal is stored: a player is counted DISTINCT by a
//! salted hash of their IP address (the salt is random per file and never leaves it, so the hashes cannot be reversed by
//! hashing a list of addresses without the file), and remembered by the NAME they typed (what everyone in the lobby already sees).
//!
//! File format, one record per line, `#` comments allowed:
//!   NMN-STATS 1
//!   since <unix>            the first start with this file
//!   salt <hex>
//!   peak <players> <unix>   most players online at once
//!   game <id> <joins> <sessions> <session_players> <minutes> <last_unix>
//!   seen <id|*> <hash>      one distinct player (by IP hash) per game, and over all games ('*')
//!   day <YYYY-MM-DD> <id> <joins> <sessions>
//!   name <id> <sessions> <joins> <last_unix> <name...>   (the name is the rest of the line)

use std::collections::{BTreeMap, BTreeSet};
use std::fmt::Write as _;
use std::hash::{Hash, Hasher};
use std::net::IpAddr;
use std::time::{SystemTime, UNIX_EPOCH};

#[derive(Default, Clone)]
pub struct GameStats {
    pub joins: u64,
    pub sessions: u64,
    pub session_players: u64,
    pub minutes: u64,
    pub last: u64,
}

#[derive(Default, Clone)]
pub struct NameStats {
    pub sessions: u64,
    pub joins: u64,
    pub last: u64,
}

#[derive(Default)]
pub struct Stats {
    pub since: u64,
    salt: u64,
    pub peak: (usize, u64),
    pub games: BTreeMap<String, GameStats>,
    seen: BTreeMap<String, BTreeSet<u64>>,
    pub days: BTreeMap<(String, String), (u64, u64)>,
    pub names: BTreeMap<(String, String), NameStats>,
    pub dirty: bool,
}

pub fn unix_now() -> u64 {
    SystemTime::now().duration_since(UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0)
}

/// "YYYY-MM-DD" (UTC) of a unix time (Howard Hinnant's civil_from_days)
pub fn day_of(t: u64) -> String {
    let z = (t / 86400) as i64 + 719468;
    let era = z.div_euclid(146097);
    let doe = z - era * 146097;
    let yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let d = doy - (153 * mp + 2) / 5 + 1;
    let m = if mp < 10 { mp + 3 } else { mp - 9 };
    let y = yoe + era * 400 + if m <= 2 { 1 } else { 0 };
    format!("{:04}-{:02}-{:02}", y, m, d)
}

fn game_id(g: [u8; 2]) -> String {
    g.iter().map(|&c| if c.is_ascii_alphanumeric() { c as char } else { '?' }).collect()
}

fn clean(name: &str) -> String {
    let s: String = name.chars().filter(|c| !c.is_control()).take(24).collect();
    if s.trim().is_empty() { "?".into() } else { s.trim().to_string() }
}

impl Stats {
    pub fn new(seed: u64) -> Stats {
        Stats { since: unix_now(), salt: seed | 1, ..Default::default() }
    }

    fn hash_ip(&self, ip: IpAddr) -> u64 {
        let mut h = std::collections::hash_map::DefaultHasher::new();
        self.salt.hash(&mut h);
        ip.hash(&mut h);
        h.finish()
    }

    /// a player took a slot in a room of `game`
    pub fn join(&mut self, game: [u8; 2], ip: IpAddr, name: &str, online_now: usize) {
        let (g, t, h) = (game_id(game), unix_now(), self.hash_ip(ip));
        let gs = self.games.entry(g.clone()).or_default();
        gs.joins += 1;
        gs.last = t;
        self.seen.entry(g.clone()).or_default().insert(h);
        self.seen.entry("*".into()).or_default().insert(h);
        self.days.entry((day_of(t), g.clone())).or_default().0 += 1;
        let n = self.names.entry((g, clean(name))).or_default();
        n.joins += 1;
        n.last = t;
        if online_now > self.peak.0 {
            self.peak = (online_now, t);
        }
        self.dirty = true;
    }

    /// a room of `game` started a session with these players
    pub fn session_start(&mut self, game: [u8; 2], names: &[String]) {
        let (g, t) = (game_id(game), unix_now());
        let gs = self.games.entry(g.clone()).or_default();
        gs.sessions += 1;
        gs.session_players += names.len() as u64;
        gs.last = t;
        self.days.entry((day_of(t), g.clone())).or_default().1 += 1;
        for nm in names {
            let n = self.names.entry((g.clone(), clean(nm))).or_default();
            n.sessions += 1;
            n.last = t;
        }
        self.dirty = true;
    }

    /// a session ended after `secs` with `players` still in it
    pub fn session_end(&mut self, game: [u8; 2], players: usize, secs: u64) {
        self.games.entry(game_id(game)).or_default().minutes += players as u64 * secs / 60;
        self.dirty = true;
    }

    pub fn distinct(&self, game: &str) -> usize {
        self.seen.get(game).map_or(0, |s| s.len())
    }

    pub fn to_text(&self) -> String {
        let mut o = String::from("NMN-STATS 1\n");
        let _ = writeln!(o, "since {}\nsalt {:016x}\npeak {} {}", self.since, self.salt, self.peak.0, self.peak.1);
        for (g, s) in &self.games {
            let _ = writeln!(o, "game {} {} {} {} {} {}", g, s.joins, s.sessions, s.session_players, s.minutes, s.last);
        }
        for (g, set) in &self.seen {
            for h in set {
                let _ = writeln!(o, "seen {} {:016x}", g, h);
            }
        }
        for ((d, g), (j, s)) in &self.days {
            let _ = writeln!(o, "day {} {} {} {}", d, g, j, s);
        }
        for ((g, n), s) in &self.names {
            let _ = writeln!(o, "name {} {} {} {} {}", g, s.sessions, s.joins, s.last, n);
        }
        o
    }

    pub fn from_text(t: &str) -> Option<Stats> {
        let mut s = Stats::default();
        let mut lines = t.lines();
        if lines.next()?.trim() != "NMN-STATS 1" {
            return None;
        }
        for l in lines {
            let l = l.trim_end();
            if l.is_empty() || l.starts_with('#') {
                continue;
            }
            let f: Vec<&str> = l.splitn(6, ' ').collect();
            let n = |i: usize| f.get(i).and_then(|v| v.parse::<u64>().ok()).unwrap_or(0);
            match f[0] {
                "since" => s.since = n(1),
                "salt" => s.salt = u64::from_str_radix(f.get(1)?, 16).ok()?,
                "peak" => s.peak = (n(1) as usize, n(2)),
                "game" => {
                    let f: Vec<&str> = l.split(' ').collect();
                    let n = |i: usize| f.get(i).and_then(|v| v.parse::<u64>().ok()).unwrap_or(0);
                    s.games.insert(f.get(1)?.to_string(), GameStats { joins: n(2), sessions: n(3), session_players: n(4), minutes: n(5), last: n(6) });
                }
                "seen" => {
                    s.seen.entry(f.get(1)?.to_string()).or_default().insert(u64::from_str_radix(f.get(2)?, 16).ok()?);
                }
                "day" => {
                    s.days.insert((f.get(1)?.to_string(), f.get(2)?.to_string()), (n(3), n(4)));
                }
                "name" => {
                    s.names.insert((f.get(1)?.to_string(), f.get(5).unwrap_or(&"?").to_string()), NameStats { sessions: n(2), joins: n(3), last: n(4) });
                }
                _ => {}
            }
        }
        if s.salt == 0 {
            return None;
        }
        Some(s)
    }

    /// what `nmn-server --show-stats FILE` prints
    pub fn report(&self, top: usize) -> String {
        let title = |g: &str| match g {
            "rr" => "Rave Racer", "ad" => "Ace Driver", "tw" => "Tokyo Wars", "cs" => "Cyber Sled", "dd" => "Dirt Dash",
            "tc" => "Time Crisis", "pc" => "Prop Cycle", _ => "",
        };
        let mut o = String::new();
        let _ = writeln!(o, "since {} (UTC)   distinct players (by IP) over all games: {}   peak online: {} on {}",
                         day_of(self.since), self.distinct("*"), self.peak.0, if self.peak.1 > 0 { day_of(self.peak.1) } else { "-".into() });
        let _ = writeln!(o, "\n{:<4} {:<12} {:>9} {:>7} {:>9} {:>11} {:>9}  {}", "game", "", "players", "joins", "sessions", "avg/session", "minutes", "last played");
        for (g, s) in &self.games {
            let avg = if s.sessions > 0 { s.session_players as f64 / s.sessions as f64 } else { 0.0 };
            let _ = writeln!(o, "{:<4} {:<12} {:>9} {:>7} {:>9} {:>11.1} {:>9}  {}", g, title(g), self.distinct(g), s.joins, s.sessions, avg, s.minutes, day_of(s.last));
        }
        let _ = writeln!(o, "\nlast 14 days (joins / sessions):");
        let mut days: Vec<&String> = self.days.keys().map(|(d, _)| d).collect::<BTreeSet<_>>().into_iter().collect();
        let start = days.len().saturating_sub(14);
        days.drain(..start);
        for d in days {
            let mut line = format!("  {}", d);
            for ((dd, g), (j, s)) in &self.days {
                if dd == d {
                    let _ = write!(line, "   {} {}/{}", g, j, s);
                }
            }
            let _ = writeln!(o, "{}", line);
        }
        let mut names: Vec<(&(String, String), &NameStats)> = self.names.iter().collect();
        names.sort_by(|a, b| b.1.sessions.cmp(&a.1.sessions).then(b.1.joins.cmp(&a.1.joins)));
        let _ = writeln!(o, "\nplayers by name (most sessions first, top {}):", top);
        for ((g, n), s) in names.into_iter().take(top) {
            let _ = writeln!(o, "  {:<24} {:<3} {:>4} sessions {:>5} joins   last {}", n, g, s.sessions, s.joins, day_of(s.last));
        }
        o
    }
}
