/*
 * tc2_link.c -- THE TWIN-CABINET LINK of Time Crisis 2: which cabinet this is (the Link ID), the C422 RS-422 communication controller
 * as our own model, and online play (src/eng/net.c, NMN2 through zonesync.net or the built-in LAN host) carrying it.
 *
 * THE CABINET (Link ID). The game is built for two cabinets side by side, Left/Red and Right/Blue; the I/O board tells it which one it is
 * (MAME 0.289 timecrs2 "Link ID", JVS_PLAYER1 0x4000 = BUTTON4 = I/O board 0x6001 bit 2, src/tc2_h8.c io_switches). The menu's Cabinet
 * page chooses it (`link_id` in tc2.cfg; $TC2_LINKID=0/1 for tests); in an online session the slot decides (slot 0 Left/Red, 1 Right/Blue).
 *
 * THE C422 (MAME: "RS422 Networking (Twin Cabinet) Communication Controller", registers 0x06400000-0x0640000F, RAM 0x06200000-0x06203FFF;
 * MAME keeps both as plain memory: "LAN unemulated"). Its behaviour here is read off the game's own driver, 0x8000BB6C..0x8000C3A0:
 *   +0 STATUS   bit 1 = a frame has been received, bit 2 = the transmit block is done (the interrupt handler 0x8000BEAC tests both; the
 *               receive handler 0x8000BD24 clears bit 1 by writing it back)
 *   +2 MASK     0 bits enable the status bits' interrupt (reset 0x00FF; the driver clears bits 1/2). POWER-ON TEST: 0xFFFB raises the
 *               interrupt, 0x000F acknowledges it (MAME c422_irq_w; kept as it was)
 *   +6 CONTROL  2 = transmit, 3 = stop / set up; any write clears status bit 2
 *   +A COUNT    words left to transmit (the driver writes at most 0xFF a block and reads the low byte as "still busy")
 *   +C RX PTR   index (0..0xFFF) of the last word written into the receive ring
 *   +E TX ADDR  word index in the C422 RAM of the next word to transmit (the driver writes it once a packet; the chip steps it)
 *   RAM 0x0000-0x1FFF  four transmit slots of 0x400 words ((0x802F75CA + 0x400) & 0xC00): word 0/1 = length, then the packet
 *   RAM 0x2000-0x3FFF  the receive ring: 4096 words, the byte in bits 0-7, bit 8 = the last word of a frame
 * A packet on the wire (0x8000BF38): 0x5A, seq hi, seq lo, a status byte, a rotating status byte, the payload (0x802F3910 bytes from
 * 0x802F3912), a checksum (the negated sum), length hi, length lo | 0x100. The driver sends each frame's packet up to four times
 * (0x802F75C6 = 3 resends); the receiver keeps a 16-entry queue of the frames it found and checks each one's 0x5A and checksum.
 * The model is ARMED only while a link is up (an online session, or $TC2_LINK for a direct test pair): unarmed, the registers stay plain
 * memory exactly as MAME has them, so single-player runs are unchanged.
 *
 * ON THE NETWORK. Every word the chip transmits is kept; each game frame the complete frames sent since the last one go out as ONE
 * payload (tc2_net_*), the peer's arrive into a queue and are written into the receive ring one frame per interrupt, the next only
 * after the game has acknowledged the last. Stall-and-wait keeps the two machines within a few frames of each other.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include "tc2_game.h"
#include "eng/eng_cfg.h"
extern uint32_t rr_frame;

/* ---------------- the cabinet (Link ID) ---------------- */
static int link_id_cfg = -1;                   /* the menu's choice (-1 = not loaded yet) */
static int link_id_forced = -1;                /* an online session's slot, -1 = none */
int tc2_link_id(void)
{
    if (!tc2_game.twin) return 0;
    if (link_id_forced >= 0) return link_id_forced;
    { static long flip = -2; if (flip == -2) { const char *e = getenv("TC2_LINKID_FLIP"); flip = e ? atol(e) : -1; }   /* tests: flip at that frame */
      if (flip > 0 && rr_frame >= (uint32_t)flip) { flip = -1; link_id_cfg = !tc2_link_id(); fprintf(stderr, "[LINK] f%u Link ID flipped to %d\n", rr_frame, link_id_cfg); } }
    if (link_id_cfg < 0) {
        const char *e = getenv("TC2_LINKID");
        link_id_cfg = e ? (atoi(e) != 0) : (eng_cfg_int("link_id", 0) != 0);
    }
    return link_id_cfg;
}
void tc2_link_id_set(int id) { link_id_cfg = id != 0; eng_cfg_set_int("link_id", link_id_cfg); }
void tc2_link_id_force(int id) { link_id_forced = id; }

/* ---------------- the C422 ---------------- */
void tc2_irq_set(int src, int on);
uint64_t tc2_cycles_now(void);
uint8_t *tc2_vram_ptr(uint32_t p);
extern uint32_t rr_frame;

static uint16_t reg[8];                        /* +0 .. +E */
static int armed;                              /* the active model (a link is up); 0 = plain registers, as MAME */
static int post_irq;                           /* the POWER-ON TEST's raise (0xFFFB to +2) */
static int tx_busy; static uint64_t tx_done_at;
static int dbg = -1;
static uint16_t ram_rd(uint32_t word) { const uint8_t *p = tc2_vram_ptr(0x06200000u + (word & 0x1FFFu) * 2); return (uint16_t)(p[0] << 8 | p[1]); }
static void ram_wr(uint32_t word, uint16_t v) { uint8_t *p = tc2_vram_ptr(0x06200000u + (word & 0x1FFFu) * 2); p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }

/* the transmitted words (bit 8 = a frame's last word): whole frames wait here for the transport */
#define TXQ 8192
static uint16_t txq[TXQ]; static int txq_n;    /* complete frames only, back to back */
static uint16_t cur[1024]; static int cur_n;   /* the frame being transmitted */
/* received frames, waiting for the ring */
#define RXQ 8192
#define RXQ_KEEP 4096                         /* ~16 gameplay packets waiting at most */
static uint16_t rxq[RXQ]; static int rxq_n;

static void irq_update(void)
{
    const int on = post_irq || (armed && (reg[0] & ~reg[1] & 6) != 0);   /* unarmed: plain registers, as MAME -- no line from them */
    tc2_irq_set(TC2_SRC_C422, on);
}
static uint64_t cyc_per_word(void)              /* the line's speed: 11-bit characters at $TC2_LINK_BAUD (default 2 Mbit/s) */
{
    static uint64_t c; if (!c) { const char *e = getenv("TC2_LINK_BAUD"); const double baud = e ? atof(e) : 2e6; c = (uint64_t)(tc2_game.clock * 11.0 / (baud > 1000 ? baud : 2e6)); if (!c) c = 1; }
    return c;
}
static void tx_word(uint16_t w)
{
    if (cur_n < (int)(sizeof cur / sizeof cur[0])) cur[cur_n++] = (uint16_t)(w & 0x1FF);
    if (w & 0x100) {                             /* the frame's last word */
        if (txq_n + cur_n <= TXQ) { memcpy(txq + txq_n, cur, (size_t)cur_n * 2); txq_n += cur_n; }
        else if (dbg) fprintf(stderr, "[LINK] f%u transmit queue full: a frame of %d words dropped\n", rr_frame, cur_n);
        cur_n = 0;
    }
}
static void tx_start(void)
{
    const int n = reg[5] & 0xFF;
    for (int i = 0; i < n; i++) tx_word(ram_rd(reg[7] + (uint32_t)i));
    reg[7] = (uint16_t)(reg[7] + n);
    tx_busy = 1; tx_done_at = tc2_cycles_now() + (uint64_t)(n ? n : 1) * cyc_per_word();
}

int tc2_c422_read(unsigned k, uint32_t *v)
{
    *v = reg[k & 7];
    return 1;
}
void tc2_c422_write(unsigned k, uint16_t v)
{
    k &= 7;
    reg[k] = v;
    if (k == 1 && (v == 0xFFFBu || v == 0x000Fu)) { post_irq = v == 0xFFFBu; tc2_irq_set(TC2_SRC_C422, post_irq || (armed && (reg[0] & ~reg[1] & 6))); return; }
    if (!armed) return;                          /* plain registers, as MAME */
    if (k == 3) {                                /* a write to CONTROL acknowledges "transmit done" (the driver writes it before every
                                                  * block and when the packet is over; it never clears status bit 2 itself) */
        reg[0] &= (uint16_t)~4;
        if (v == 2 && !tx_busy) tx_start();
    }
    irq_update();
}

/* the receive ring: one queued frame at a time, the next once the game has taken the last (status bit 1 clear) */
static void rx_deliver(void)
{
    if (!rxq_n || (reg[0] & 2)) return;
    int n = 0; while (n < rxq_n && !(rxq[n] & 0x100)) n++;
    if (n == rxq_n) { rxq_n = 0; return; }       /* no end marker: not a frame */
    n++;
    uint16_t p = reg[6];
    for (int i = 0; i < n; i++) { p = (uint16_t)((p + 1) & 0xFFF); ram_wr(0x1000u + p, rxq[i]); }
    reg[6] = p;
    memmove(rxq, rxq + n, (size_t)(rxq_n - n) * 2); rxq_n -= n;
    reg[0] |= 2;
    irq_update();
}

/* time passed (src/tc2_main.c tc2_host_step) */
void tc2_link_step(void)
{
    if (!armed) return;
    if (tx_busy && tc2_cycles_now() >= tx_done_at) { tx_busy = 0; reg[5] &= 0xFF00; reg[0] |= 4; irq_update(); }
    rx_deliver();
}

/* the transport's side */
void tc2_link_arm(int on)
{
    if (dbg < 0) { const char *e = getenv("TC2_LINKLOG"); dbg = e ? (atoi(e) > 1 ? 2 : 1) : 0; }
    if (armed == (on != 0)) return;
    armed = on != 0; tx_busy = 0; txq_n = cur_n = 0; rxq_n = 0;
    if (!armed) reg[0] &= (uint16_t)~6;          /* a session ending with a frame or "transmit done" pending: the C422's interrupt is level
                                                  * and an unarmed write never re-evaluates it, so the line would stay up for good */
    if (dbg) fprintf(stderr, "[LINK] f%u C422 %s\n", rr_frame, armed ? "ARMED (the link is up)" : "unarmed (plain registers)");
    /* A link that comes up AFTER boot (a session joined from the menu): unarmed, the driver's last "transmit" write (CONTROL = 2, a
     * count left in +A) started nothing, so "transmit done" never came and the driver still waits for it -- it would never send again,
     * and the partner would never see this cabinet (LINKED crossed out). Run that pending block now; its packet is partial, and the
     * receiver's checksum drops it. Measured: a copy armed at f2070 sent 0 link frames to the end of the run without this. */
    if (armed && reg[3] == 2 && (reg[5] & 0xFF)) tx_start();
    irq_update();
}
int tc2_link_armed(void) { return armed; }
static int in_session;
int tc2_link_in_session(void) { return in_session; }
static int holding;
static int coin_pending, coin_pulse;          /* coins put in while held, paid in after; the pulse being sent (frames left, + on / - off) */
int tc2_link_hold_start(void) { return holding; }   /* src/tc2_h8.c: the coin, trigger and pedal are held off while 1 */
int tc2_link_coin_inject(void) { return coin_pulse > 0; }   /* src/tc2_h8.c: a held coin being paid in */
/* the frames transmitted since the last call, back to back (bit 8 ends each); returns the word count */
int tc2_link_take(uint16_t *out, int max)
{
    int n = 0;
    while (n < txq_n) {                          /* whole frames that fit */
        int e = n; while (e < txq_n && !(txq[e] & 0x100)) e++;
        if (e >= txq_n || e + 1 > max) break;
        n = e + 1;
    }
    memcpy(out, txq, (size_t)n * 2);
    memmove(txq, txq + n, (size_t)(txq_n - n) * 2); txq_n -= n;
    return n;
}
void tc2_link_give(const uint16_t *w, int n)
{
    if (n > RXQ) return;
    int drop = 0;
    while (rxq_n + n > RXQ || (rxq_n && rxq_n + n > RXQ_KEEP)) {   /* full, or more than a few frames behind: the oldest frames go -- a real-time
                                                                  * link has no use for stale ones (before the game's driver runs, e.g. during
                                                                  * boot, the queue filled to 8000 words and was then played out late) */
        int e = 0; while (e < rxq_n && !(rxq[e] & 0x100)) e++;
        e = e < rxq_n ? e + 1 : rxq_n;
        memmove(rxq, rxq + e, (size_t)(rxq_n - e) * 2); rxq_n -= e; drop++;
    }
    if (drop && dbg) fprintf(stderr, "[LINK] f%u receive queue behind: %d old frame(s) dropped\n", rr_frame, drop);
    memcpy(rxq + rxq_n, w, (size_t)n * 2); rxq_n += n;
}
void tc2_c422_load(unsigned k, uint16_t v) { reg[k & 7] = v; }   /* a MAME snapshot's registers (src/tc2_chips.c) */

/* ---------------- online: the NMN2 client carries the link (src/eng/net.c, ../docs/NETPLAY.md) ----------------
 * The PAYLOAD (TC2_FRAME_SIZE bytes, every frame): byte 0 = the number of link frames k, then k times { BE16 id, BE16 n, n bytes },
 * oldest first -- one byte a word, bit 8 (end of frame) being the last word's. A packet the driver resends is carried once. The sender
 * keeps its recent link frames numbered; each payload carries the newest ones that fit, which repeats the last frame's too -- a lost
 * datagram costs nothing -- and the receiver hands the game only the ids it has not had yet. A gameplay packet is ~250 bytes (measured:
 * 242 payload bytes + 8), so the payload is 1 KB: the server needs Time Crisis 2 ("t2") in its table (server/src/lib.rs GAMES).
 *   TC2_NET_SERVER=host[:port] [TC2_NET_NAME=x]   connect at boot (headless too)     TC2_NET_HOST=1   host a LAN game (built-in server)
 *   TC2_NET_DISCOVER=1                             join the first LAN host found       TC2_NET_AUTOSTART=N (src/eng/net.c)
 *   TC2_NET_LEAD=n   stall-and-wait lead (default auto: 2 + the ping in frames)       TC2_LINKLOG=1   [LINK] lines */
#include "eng/net.h"
extern uint8_t tc2_ram[0x1000000];
#define TC2_FRAME_SIZE 1024
#define HIST 64
static struct { uint16_t id; int n; uint16_t w[1024]; } hist[HIST];   /* our transmitted link frames, by id % HIST */
static uint16_t next_id, rx_last; static int rx_have, net_on;
static unsigned long n_tx_frames, n_tx_dup, n_rx_frames, n_rx_dup;
static long lag_sum, lag_n; static int lag_max;               /* [LINK]: how many of our frames old the peer's payloads are on arrival */
/* THE AGREED START (online only). The game's byte 0x802F3502 is the PARTNER's link state as this cabinet sees it (1 attract, 2 credited /
 * selecting, 4 in a game). Measured (two linked copies, LAN and zonesync, both players shooting): the RIGHT player's kills count only when the
 * LEFT cabinet starts its game first and the right joins once that game is under way (25 s later: both play linked through the stage); when the
 * right starts first, or joins within ~6 s of the left, the right cabinet ends up showing its partner as "COM" and its kills stop counting
 * (the score freezes, the enemies stand). So the right cabinet's trigger and pedal are held, until it is in a game, while the left has not been
 * in its game for $TC2_JOIN_WAIT frames (default 1500 = 25 s). The COIN too: what decides it is when the right cabinet is CREDITED (credited
 * 1.8 s into the left's game: broken; 20 s in: linked) -- coins put in while held are counted and paid in once the hold ends. Each payload's trailer carries the byte its sender sees, which tells the
 * partner its OWN state. A partner on an older build sends no trailer: then nothing is held, as before. */
static int self_st = -1;                       /* our state as the partner sees it (its trailer); -1 = unknown */
static uint32_t p_since, p_since_ms; static int p_prev = -1;   /* the frame (and wall-clock ms) the partner's state last changed */
#include <SDL.h>
static void hint(const char *t, int frames) { void eng_ui_set_hint(const char *, int); eng_ui_set_hint(t, frames); }

static void session_begin(int slot, int players)
{
    if (slot < 0 || slot > 1) { fprintf(stderr, "[TC2] online: slot %d -- Time Crisis 2 links two cabinets; staying alone\n", slot); return; }
    tc2_link_id_force(slot);
    rx_have = 0; next_id = 0; in_session = 1; self_st = p_prev = -1; holding = 0; coin_pending = coin_pulse = 0;
    memset(hist, 0, sizeof hist); n_tx_frames = n_tx_dup = n_rx_frames = n_rx_dup = 0;   /* a second session in this run: frame_out repeats the newest
                                                                                         * frames it finds in the history -- the last session's, otherwise */
    tc2_link_arm(1);
    fprintf(stderr, "[TC2] online: cabinet %s (slot %d of %d) -- the link is up\n", slot ? "Right/Blue" : "Left/Red", slot, players);
    { void eng_ui_set_open(bool); eng_ui_set_open(false); }   /* out of the lobby and into the game: a menu left open would hide it (it no longer holds it) */
    hint(slot ? "Linked: you are the RIGHT / BLUE cabinet -- wait for the left player to start" : "Linked: you are the LEFT / RED cabinet -- coin in and start first", 400);
}
static void session_end(void)
{
    in_session = 0; tc2_link_arm(0); tc2_link_id_force(-1); self_st = -1;   /* holding: released by tc2_link_frame, which then pays held coins */
    fprintf(stderr, "[TC2] online: session over (link frames out %lu, in %lu, repeats %lu)\n", n_tx_frames, n_rx_frames, n_rx_dup);
    hint("The link is down: playing alone", 300);
}
static int frame_out(uint8_t *buf)
{
    static uint16_t w[TXQ];
    for (int n; (n = tc2_link_take(w, (int)(sizeof w / sizeof w[0]))) > 0; ) {   /* this frame's transmitted frames into the history */
        for (int s = 0; s < n; ) {
            int e = s; while (e < n && !(w[e] & 0x100)) e++;
            const int len = e - s + 1;
            const int prev = (uint16_t)(next_id - 1) % HIST;     /* the driver sends each packet up to four times: once is enough here */
            const int dup = n_tx_frames && hist[prev].n == len && !memcmp(hist[prev].w, w + s, (size_t)len * 2);
            if (len <= 1024 && !dup) { hist[next_id % HIST].id = next_id; hist[next_id % HIST].n = len; memcpy(hist[next_id % HIST].w, w + s, (size_t)len * 2); next_id++; n_tx_frames++; }
            else if (dup) n_tx_dup++;
            s = e + 1;
        }
    }
    memset(buf, 0, TC2_FRAME_SIZE);
    int k = 0, bytes = 1;                          /* the newest frames that fit, counted backwards */
    while (k < HIST && (unsigned long)k < n_tx_frames && k < 255) {
        const int sz = 4 + hist[(uint16_t)(next_id - 1 - k) % HIST].n;
        if (bytes + sz > TC2_FRAME_SIZE - 8) break;     /* the last 8 bytes: the trailer (state, frame) */
        bytes += sz; k++;
    }
    buf[0] = (uint8_t)k; int o = 1;
    for (int i = k - 1; i >= 0; i--) {
        const uint16_t id = (uint16_t)(next_id - 1 - i); const int h = id % HIST;
        buf[o++] = (uint8_t)(id >> 8); buf[o++] = (uint8_t)id; buf[o++] = (uint8_t)(hist[h].n >> 8); buf[o++] = (uint8_t)hist[h].n;
        for (int j = 0; j < hist[h].n; j++) buf[o++] = (uint8_t)hist[h].w[j];   /* the byte; bit 8 is set on the last word only */
    }
    if (o <= TC2_FRAME_SIZE - 8) { buf[TC2_FRAME_SIZE - 8] = 'T'; buf[TC2_FRAME_SIZE - 7] = '2'; buf[TC2_FRAME_SIZE - 6] = tc2_ram[0x2F3502]; }   /* the agreed start */
    if (o <= TC2_FRAME_SIZE - 4) { buf[TC2_FRAME_SIZE - 4] = (uint8_t)(rr_frame >> 24); buf[TC2_FRAME_SIZE - 3] = (uint8_t)(rr_frame >> 16);   /* the sender's frame: [LINK] lag */
                                   buf[TC2_FRAME_SIZE - 2] = (uint8_t)(rr_frame >> 8); buf[TC2_FRAME_SIZE - 1] = (uint8_t)rr_frame; }
    return 1;
}
static void frame_in(int slot, const uint8_t *buf, uint32_t seq)
{
    (void)seq;
    if (!in_session || slot == eng_net_slot()) return;
    { const uint32_t sf = (uint32_t)buf[TC2_FRAME_SIZE - 4] << 24 | (uint32_t)buf[TC2_FRAME_SIZE - 3] << 16 | (uint32_t)buf[TC2_FRAME_SIZE - 2] << 8 | buf[TC2_FRAME_SIZE - 1];
      if (sf) { const int lag = (int)(rr_frame - sf); lag_sum += lag; lag_n++; if (lag > lag_max) lag_max = lag; } }
    if (buf[TC2_FRAME_SIZE - 8] == 'T' && buf[TC2_FRAME_SIZE - 7] == '2') self_st = buf[TC2_FRAME_SIZE - 6];
    const int k = buf[0]; int o = 1;
    for (int i = 0; i < k; i++) {
        if (o + 4 > TC2_FRAME_SIZE - 8) return;
        const uint16_t id = (uint16_t)(buf[o] << 8 | buf[o + 1]); const int n = buf[o + 2] << 8 | buf[o + 3]; o += 4;
        if (n < 1 || n > 1024 || o + n > TC2_FRAME_SIZE - 8) return;
        if (!rx_have || (int16_t)(id - rx_last) > 0) {
            uint16_t w[1024];
            for (int j = 0; j < n; j++) w[j] = buf[o + j];
            w[n - 1] |= 0x100;
            tc2_link_give(w, n); rx_last = id; rx_have = 1; n_rx_frames++;
        } else n_rx_dup++;
        o += n;
    }
}
/* the lobby's and the chat's name for a slot: the slot IS the cabinet side in the linked game (session_begin) */
static const char *slot_label(int slot) { return slot == 0 ? "Left / Red (Richard)" : slot == 1 ? "Right / Blue (Keith)" : ""; }
static eng_net_game net_game = {
    "t2", 1, 2, TC2_FRAME_SIZE, "TC2_NET", "game", 0,
    session_begin, session_end, frame_out, frame_in, hint, slot_label,
};

/* ---------------- TWO PLAYERS ON THIS COMPUTER, ONE CLICK (Online > "Second player on this computer", launch.sh --2p, $TC2_LOCAL2P=1) ----------------
 * The arcade's LINK PLAY with both cabinets on one PC: this copy hosts a LAN game and starts a SECOND COPY of itself ($TC2_INSTANCE=2:
 * its own tc2_2.cfg -- made from ours, without our gun -- tc2_2_backup.nv, tc2_2_last.rec, tc2_2.log) that finds the LAN game and joins;
 * both are set to Ready and the game starts by itself once the two are in. Each copy reads ITS OWN gun (src/eng/eng_rawptr.h): ours is
 * Controls > Gun device, the second copy gets the first other device, or picks on its own Controls page. The windows: two monitors --
 * one each; one monitor -- side by side (src/tc2_window.c place_2p). The second copy is the Right/Blue cabinet (lobby slot 1). */
#include "eng/eng_rawptr.h"
#ifdef _WIN32
static void envset(const char *k, const char *v) { SetEnvironmentVariableA(k, v); }
static bool spawn_self(char *why, size_t n)
{
    wchar_t exe[MAX_PATH]; if (!GetModuleFileNameW(NULL, exe, MAX_PATH)) { snprintf(why, n, "GetModuleFileName failed"); return false; }
    wchar_t cmd[MAX_PATH + 16]; _snwprintf(cmd, MAX_PATH + 16, L"\"%ls\" --window", exe);
    STARTUPINFOW si; memset(&si, 0, sizeof si); si.cb = sizeof si; PROCESS_INFORMATION pi;
    if (!CreateProcessW(exe, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) { snprintf(why, n, "CreateProcess failed (%lu)", (unsigned long)GetLastError()); return false; }
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    return true;
}
#else
#include <unistd.h>
#include <sys/wait.h>
#include <errno.h>
static void envset(const char *k, const char *v) { if (v) setenv(k, v, 1); else unsetenv(k); }
static bool spawn_self(char *why, size_t n)               /* the AppImage itself when we are one, else our own binary; forked twice so no zombie waits */
{
    char exe[1024]; const char *ai = getenv("APPIMAGE");
    if (ai && *ai) snprintf(exe, sizeof exe, "%s", ai);
    else { const ssize_t k = readlink("/proc/self/exe", exe, sizeof exe - 1); if (k <= 0) { snprintf(why, n, "cannot find my own program file"); return false; } exe[k] = 0; }
    const pid_t p = fork();
    if (p < 0) { snprintf(why, n, "fork: %s", strerror(errno)); return false; }
    if (p == 0) { if (fork() == 0) { for (int fd = 3; fd < 1024; fd++) close(fd);   /* our sockets (the LAN host's UDP port), logs, GL/audio fds */
                                     execl(exe, exe, "--window", (char *)NULL); _exit(127); } _exit(0); }
    int st; waitpid(p, &st, 0);
    return true;
}
#endif
static bool local2p_start(char *msg, size_t n)
{
    if (!tc2_game.twin || !net_on) { snprintf(msg, n, "not a twin-cabinet game"); return false; }
    if (link_id_forced >= 0) { snprintf(msg, n, "Already in a linked game."); return false; }
    if (!eng_net_hosting() && !eng_net_host_start()) { snprintf(msg, n, "Could not host: UDP port %d is busy (another copy running?).", ENG_NET_PORT); return false; }
    eng_net_set_autostart(2);
    char gun[160] = ""; const int me = eng_rawptr_selected();           /* the second copy's gun: the first device that is not ours */
    for (int i = 0; i < eng_rawptr_count(); i++) if (i != me) { snprintf(gun, sizeof gun, "%s", eng_rawptr_at(i)->id); break; }
    int tc2_window_local2p_place(void); const int disp = tc2_window_local2p_place();
    char twop[32], nm[48]; snprintf(twop, sizeof twop, "2:%d", disp); snprintf(nm, sizeof nm, "%.13s 2", eng_net_name());
    envset("TC2_INSTANCE", "2"); envset("TC2_2P", twop); envset("TC2_NET_DISCOVER", "1"); envset("TC2_NET_AUTOSTART", "2"); envset("TC2_NET_NAME", nm);
    envset("TC2_GUN_DEVICE", gun[0] ? gun : NULL); envset("TC2_LOCAL2P", NULL); envset("TC2_NET_HOST", NULL); envset("TC2_NET_SERVER", NULL);
    char why[160] = ""; const bool ok = spawn_self(why, sizeof why);
    envset("TC2_INSTANCE", NULL); envset("TC2_2P", NULL); envset("TC2_NET_DISCOVER", NULL); envset("TC2_NET_AUTOSTART", NULL); envset("TC2_NET_NAME", NULL); envset("TC2_GUN_DEVICE", NULL);
    if (!ok) { snprintf(msg, n, "Could not start a second copy: %s", why); eng_net_set_autostart(0); return false; }
    fprintf(stderr, "[TC2] two players on this computer: hosting, second copy started (its gun: %s)\n", gun[0] ? gun : me < 0 ? "none chosen -- both copies read every gun until one is picked" : "none left");
    snprintf(msg, n, "Second copy started: it joins this game by itself and the game starts when both are in. Put a coin in each window (5).%s",
             me < 0 ? " Pick this copy's gun in Controls > Gun device." : "");
    hint("Starting the second player's copy...", 240);
    return true;
}
bool tc2_local2p_start(char *msg, size_t n) { return local2p_start(msg, n); }

/* once at start-up, after the cfg is loaded: the client (inert until the menu or $TC2_NET_* connects it) */
void tc2_link_init(void)
{
    if (!tc2_game.twin || net_on) return;
    if (dbg < 0) { const char *e = getenv("TC2_LINKLOG"); dbg = e ? (atoi(e) > 1 ? 2 : 1) : 0; }
    eng_net_init(&net_game); net_on = 1;
    { void eng_net_ui_set_local2p(bool (*)(char *, size_t), const char *, const char *);
      eng_net_ui_set_local2p(local2p_start, "Second player on this computer", "two guns, one each (Controls > Gun device)"); }
    const char *srv = getenv("TC2_NET_SERVER"), *hst = getenv("TC2_NET_HOST"), *dsc = getenv("TC2_NET_DISCOVER"), *nm = getenv("TC2_NET_NAME");
    if (nm && *nm) eng_net_set_name(nm);
    if (hst && *hst == '1') eng_net_host_start();
    else if (dsc && *dsc == '1') { eng_net_discover_autojoin(1); eng_net_discover(); }
    else if (srv && *srv && eng_net_set_server(srv)) eng_net_connect();
}
/* once a game frame (src/tc2_main.c tc2_on_vblank): the client, then stall-and-wait so neither cabinet runs ahead of the other */
static unsigned ram16(uint32_t a) { return (unsigned)(tc2_ram[a & 0xFFFFFF] << 8 | tc2_ram[(a + 1) & 0xFFFFFF]); }
void tc2_link_frame(void)
{
    if (dbg > 0 && rr_frame % 60 == 0)                /* the game's link state: payload length, state, silence count, resends left; the C422 */
        fprintf(stderr, "[LINK] f%u len %u state %u silent %u resend %d seq %u | C422 st %04X mask %04X ctl %04X cnt %04X rx %04X tx %04X%s\n", rr_frame,
                ram16(0x2F3910), tc2_ram[0x2F3502], ram16(0x2F3504), (int16_t)ram16(0x2CEDE6), ram16(0x2CEDD8),
                reg[0], reg[1], reg[3], reg[5], reg[6], reg[7], armed ? " ARMED" : "");
    if (dbg) { static unsigned prev = 0x100; const unsigned stt = tc2_ram[0x2F3502];   /* the game's link state: when it changes, with the wall clock (two copies compare) */
        if (stt != prev) { long long ms = 0;
#ifndef _WIN32
            struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ms = (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
            fprintf(stderr, "[LINKST] f%u state %u -> %u at %lld ms\n", rr_frame, prev, stt, ms); prev = stt; } }
    static long lb_from = -1, lb_to; if (lb_from < 0) { const char *e = getenv("TC2_LINKB"); lb_from = 1300; lb_to = 2400; if (e) sscanf(e, "%ld-%ld", &lb_from, &lb_to); }
    if (dbg > 1 && rr_frame >= (uint32_t)lb_from && rr_frame <= (uint32_t)lb_to) {   /* $TC2_LINKLOG=2: the game's link block 0x802F3500.. every frame (a linked-vs-COM start, side by side) */
        fprintf(stderr, "[LINKB] f%u", rr_frame);
        for (int i = 0; i < 0x20; i++) fprintf(stderr, "%s%02X", i % 4 ? "" : " ", tc2_ram[0x2F3500 + i]);
        fprintf(stderr, " |"); for (int i = 0; i < 0x10; i++) fprintf(stderr, "%s%02X", i % 4 ? "" : " ", tc2_ram[0x2F3910 + i]);
        fprintf(stderr, "\n");
    }
    if (!net_on) return;
    { static int at = -2;                                 /* $TC2_LOCAL2P=1 (launch.sh --2p): the one-click local two-player start, once the window is up */
      if (at == -2) { const char *e = getenv("TC2_LOCAL2P"); at = e && *e == '1' ? 60 : -1; }
      if (at > 0 && rr_frame >= (uint32_t)at) { at = -1; char m[200]; int tc2_window_active(void); if (tc2_window_active()) { local2p_start(m, sizeof m); fprintf(stderr, "[TC2] %s\n", m); } } }
    eng_net_poll();
    {                                                   /* the agreed start (above): who may start now */
        static long wait = -1; if (wait < 0) { const char *e = getenv("TC2_JOIN_WAIT"); wait = e ? atol(e) : 1500; }
        const int pst = tc2_ram[0x2F3502];
        if (pst != p_prev) { p_prev = pst; p_since = rr_frame; p_since_ms = SDL_GetTicks(); }
        /* the wait counts in frames OR wall-clock time, whichever ends first: over a slow link stall-and-wait runs both games below 60
         * fps (measured on zonesync 2026-10-25: ~20 fps between Portugal and Brazil), and 1500 frames were then more than a minute of
         * a dead gun for the right player -- reported as "P2 no inputs" */
        const uint32_t left_ms = (uint32_t)(wait * 1000 / 60), in_ms = SDL_GetTicks() - p_since_ms;
        const int left_done = pst == 4 && (rr_frame - p_since >= (uint32_t)wait || in_ms >= left_ms);
        int h = 0;                                      /* the right cabinet joins the left: never first, and not until the left's game is under way */
        if (in_session && eng_net_slot() == 1 && self_st >= 0 && self_st != 4 && !left_done) h = 1;
        { extern uint16_t tc2_inputs; static int prev_coin;   /* coins while held: counted, then paid in 4 frames on / 36 off each. Runs outside a
                                                               * session too, so a pulse under way when the link drops ends, and held coins are paid */
          const int c = tc2_inputs & 0x0001;
          if (h && c && !prev_coin) coin_pending++;
          prev_coin = c;
          if (coin_pulse > 0) { if (--coin_pulse == 0) coin_pulse = -36; }
          else if (coin_pulse < 0) coin_pulse++;
          else if (!h && coin_pending > 0) { coin_pending--; coin_pulse = 4; } }
        if (h != holding) {
            holding = h;
            if (dbg) fprintf(stderr, "[LINKST] f%u start %s (us %d, partner %d)\n", rr_frame, h ? "HELD" : "free", self_st, pst);
            if (in_session && (h || self_st != 4)) hint(h ? (pst == 4 ? "The left player's game is starting -- you can join in a moment" : "Linked: the LEFT player starts first, then you join")
                   : "Linked: press the trigger to join your partner", h ? 600 : 300);
        }
        if (h && pst == 4 && rr_frame % 60 == 0) {      /* held while the left's game starts: say how long */
            char t[96]; const uint32_t r = in_ms < left_ms ? (left_ms - in_ms + 999) / 1000 : 0;
            snprintf(t, sizeof t, "The left player's game is starting -- you can join in %u s", (unsigned)r); hint(t, 90);
        }
    }
    if (in_session) {
        static int lead = -2; if (lead == -2) { const char *e = getenv("TC2_NET_LEAD"); lead = e ? atoi(e) : -1; }
        if (lead < 1000) eng_net_wait(lead < 0 ? eng_net_auto_lead() : lead, 1000);
        if (dbg && rr_frame % (dbg > 1 ? 60 : 600) == 0) {
            fprintf(stderr, "[LINK] f%u link frames out %lu (+%lu resends), in %lu (repeats %lu), ping %d ms, peer payload age %.1f frames (max %d), %d words waiting for the ring\n",
                    rr_frame, n_tx_frames, n_tx_dup, n_rx_frames, n_rx_dup, eng_net_ping_ms(), lag_n ? (double)lag_sum / lag_n : 0.0, lag_max, rxq_n);
            lag_sum = lag_n = 0; lag_max = 0;
        }
    }
}
void tc2_link_paused(void) { if (net_on) eng_net_poll_paused(); }   /* the menu is open: keep the session alive, send nothing */

/* ---------------- the menu: Cabinet ---------------- */
#include "eng/eng_ui.h"
static int cab_rows(void) { return 1; }
static bool cab_enabled(int r) { (void)r; return link_id_forced < 0; }
static void cab_text(int r, char *l, size_t ln, char *v, size_t vn)
{
    (void)r; snprintf(l, ln, "Cabinet (player)");
    snprintf(v, vn, "%s%s", tc2_link_id() ? "Right / Blue (player 2)" : "Left / Red (player 1)", link_id_forced >= 0 ? " -- set by the online session" : "");
}
static void cab_change(int r, int dir) { (void)r; (void)dir; if (link_id_forced < 0) tc2_link_id_set(!tc2_link_id()); }
static void cab_notes(void (*line)(const char *, ...))
{
    line("Two cabinets side by side: Left/Red (Richard)");
    line("and Right/Blue (Keith). Alone, play either one;");
    line("it takes effect at the next game. Online, the");
    line("session picks the side (Online page). Two guns on");
    line("this PC: Online > Second player on this computer.");
}
static const eng_ui_page cab_page = { "Cabinet", 420, 140, 0, cab_rows, NULL, cab_enabled, cab_text, cab_change, cab_notes };
const eng_ui_page *tc2_link_page(void) { return &cab_page; }
