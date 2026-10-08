//! nmn-server [--port 27750] [--bind 0.0.0.0] [--name NAME] [--max-per-ip N] [--max-rooms N] [--max-rooms-per-game N]
//!            [--room-ttl-min N] [--max-session-min N] [--frame-idle-sec N] [--status-sec N] [--quiet] [--stats-file PATH]
//! nmn-server --show-stats PATH [--top N]      the lifetime statistics a server kept (see src/stats.rs)
//!
//! One lobby + relay for every game's online play (NMN2, docs/NETPLAY.md) and Rave Racer's old RRN1 on the same port.
//! Single thread, std only: a UDP socket with a short read timeout drives `Hub::handle` / `Hub::tick`.

use nmn_server::{stats::Stats, Config, Hub, Out, DEFAULT_PORT};
use std::io::ErrorKind;
use std::net::UdpSocket;
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

fn usage() -> ! {
    eprintln!(
        "usage: nmn-server [--port N] [--bind ADDR] [--name NAME] [--max-per-ip N] [--max-rooms N] [--max-rooms-per-game N]\n\
         \x20                 [--room-ttl-min N] [--max-session-min N] [--frame-idle-sec N] [--status-sec N] [--quiet]\n\
         \x20                 [--stats-file PATH]          keep lifetime statistics (players, games, sessions) in PATH\n\
         \x20      nmn-server --show-stats PATH [--top N]   print them"
    );
    std::process::exit(2);
}

fn num<T: std::str::FromStr>(v: String) -> T {
    v.parse().unwrap_or_else(|_| usage())
}

fn main() {
    let mut port = DEFAULT_PORT;
    let mut bind = String::from("0.0.0.0");
    let mut cfg = Config::default();
    let mut quiet = false;
    let mut stats_file: Option<String> = None;
    let mut show: Option<String> = None;
    let mut top = 25usize;
    let mut args = std::env::args().skip(1);
    while let Some(a) = args.next() {
        let mut val = |what: &str| {
            args.next().unwrap_or_else(|| {
                eprintln!("{} needs a value", what);
                usage()
            })
        };
        match a.as_str() {
            "--port" => port = num(val("--port")),
            "--bind" => bind = val("--bind"),
            "--name" => cfg.name = val("--name"),
            "--room-ttl-min" => cfg.room_ttl = Duration::from_secs(60 * num::<u64>(val("--room-ttl-min"))),
            "--max-rooms" => cfg.max_rooms = num(val("--max-rooms")),
            "--max-rooms-per-game" => cfg.max_rooms_per_game = num(val("--max-rooms-per-game")),
            "--max-per-ip" => cfg.max_per_ip = num(val("--max-per-ip")),
            "--max-session-min" => cfg.max_session = Duration::from_secs(60 * num::<u64>(val("--max-session-min"))),
            "--frame-idle-sec" => cfg.frame_idle = Duration::from_secs(num(val("--frame-idle-sec"))),
            "--status-sec" => cfg.status_every = Duration::from_secs(num(val("--status-sec"))),
            "--quiet" => quiet = true,
            "--stats-file" => stats_file = Some(val("--stats-file")),
            "--show-stats" => show = Some(val("--show-stats")),
            "--top" => top = num(val("--top")),
            _ => usage(),
        }
    }
    if let Some(p) = show {
        match std::fs::read_to_string(&p).ok().and_then(|t| Stats::from_text(&t)) {
            Some(s) => print!("{}", s.report(top)),
            None => {
                eprintln!("[nmn] {}: no statistics there (not written yet, or not a stats file)", p);
                std::process::exit(1);
            }
        }
        return;
    }
    cfg.max_rooms = cfg.max_rooms.max(1);
    cfg.max_rooms_per_game = cfg.max_rooms_per_game.clamp(1, 254); // an RRN1 client sees room ids as a byte

    let sock = UdpSocket::bind((bind.as_str(), port)).unwrap_or_else(|e| {
        eprintln!("[nmn] cannot bind {}:{}: {}", bind, port, e);
        std::process::exit(1);
    });
    sock.set_read_timeout(Some(Duration::from_millis(20))).ok();
    let seed = SystemTime::now().duration_since(UNIX_EPOCH).map(|d| d.as_nanos() as u64).unwrap_or(1)
        ^ ((std::process::id() as u64) << 32);
    if !quiet {
        println!(
            "[nmn] server '{}' on UDP {}:{} (NMN2 + RRN1), {} rooms ({} per game), {} slots per IP",
            cfg.name, bind, port, cfg.max_rooms, cfg.max_rooms_per_game, cfg.max_per_ip
        );
    }
    let mut hub = Hub::new(cfg, Instant::now(), seed);
    if let Some(p) = &stats_file {
        hub.stats = Some(match std::fs::read_to_string(p) {
            Ok(t) => Stats::from_text(&t).unwrap_or_else(|| {
                eprintln!("[nmn] {}: not a statistics file -- refusing to overwrite it", p);
                std::process::exit(1);
            }),
            Err(_) => Stats::new(seed.rotate_left(17)),
        });
        if !quiet {
            println!("[nmn] statistics kept in {} (saved every minute)", p);
        }
    }
    let mut last_save = Instant::now();

    let mut buf = [0u8; 2048];
    let mut out: Out = Vec::new();
    let mut last_tick = Instant::now();
    loop {
        match sock.recv_from(&mut buf) {
            Ok((n, from)) => hub.handle(&buf[..n], from, Instant::now(), &mut out),
            // a timeout is the tick; ConnectionReset is Windows reporting an ICMP unreachable for a past send
            Err(e) if matches!(e.kind(), ErrorKind::WouldBlock | ErrorKind::TimedOut | ErrorKind::ConnectionReset) => {}
            Err(e) => {
                eprintln!("[nmn] recv: {}", e);
                std::thread::sleep(Duration::from_millis(100));
            }
        }
        let now = Instant::now();
        if now.duration_since(last_tick) >= Duration::from_millis(20) {
            hub.tick(now, &mut out);
            last_tick = now;
        }
        for (to, d) in out.drain(..) {
            let _ = sock.send_to(&d, to);
        }
        if let (Some(p), Some(st)) = (&stats_file, hub.stats.as_mut()) {
            if st.dirty && now.duration_since(last_save) >= Duration::from_secs(60) {
                let tmp = format!("{}.tmp", p);   // write + rename: a crash never leaves half a file
                if std::fs::write(&tmp, st.to_text()).and_then(|_| std::fs::rename(&tmp, p)).is_ok() {
                    st.dirty = false;
                } else {
                    eprintln!("[nmn] cannot write {}", p);
                }
                last_save = now;
            }
        }
        for e in hub.events.drain(..) {
            if !quiet {
                println!("[nmn] {}", e);
            }
        }
    }
}
