/*
 * rr_link.c -- peer behavior of the C139 SCI link chip (see rr_link.h).
 *
 * Ring formats, derived from the ROM driver (src/rd/rd_b4.c rd_link_send /
 * rd_link_receive and decompiled FUN_00026bbe / FUN_00026d2a):
 *
 * TX ring (C139 RAM words 0.., 0x20010000 + word*2), filled by rd_link_send
 * every frame with d4 = 0 (so the frame is always at words 0..39):
 *   words 0..37  the 38 staged payload bytes, one per word's low byte
 *   word  38     0x0028 -- the frame length (38 payload + length + checksum)
 *   word  39     the 16-bit sum of the 38 payload bytes
 * Control reg 0x20020004 is 1 during the fill and 3 after = transmit.
 *
 * RX ring (0x20012000 + idx*2, idx &= 0xFFF): the same 40-word frame, with
 * bit 8 (0x100) set on the checksum word -- that marker is what the IRQ ring
 * scan (FUN_00026d2a) looks for, walking back from reg 0x2002000C - 1 (at most
 * 0x24 words). Its checksum test is
 *   csum_low - (sum of the 39 preceding bytes) == 0xD8
 * where those 39 bytes are the 38 payload bytes + the 0x28 length byte, so
 * csum_low == sum(payload) & 0xFF (0x28 + 0xD8 = 0x100) -- the low byte of
 * the TX checksum unchanged, which is why loopback is a copy plus an id patch.
 * The sender id is payload word 0: FUN_00026d2a reads byte 1 & 0xF, and
 * rd_link_receive rejects the packet unless the whole word is 0x0000..0x000F
 * (byte 0 = 0), != our own number and != 0xFFFF.
 *
 * TX completion: when a transfer finishes the chip clears the size reg
 * 0x2002000A (the handler tests its low byte at 0x2002000B: nonzero = TX
 * error) and sets status bit 2. With 0x1034 nonzero the handler then kicks a
 * peer-directed TX (FUN_00026c84: reg 0xE = ring position, reg 6 = 1 .. 0).
 */
#include <stdio.h>
#include <stdlib.h>
#include "rr_mem.h"
#include "rr_hw.h"
#include "rr_link.h"

#define SCI_RX_BASE  0x1000u    /* RX ring's first C139 word (byte 0x2000 = 0x20012000) */

static int  loopback = -1, debug = -1;
static bool net_active;                   /* rr_net consumes the TX queue during a session */
void rr_link_net_active(bool on) { net_active = on; }
static bool tx_busy;                    /* between reg 6 = 1 and reg 6 = 0 */
static bool rx_frame, tx_done;          /* model-owned status bits 1 / 2 of reg 0 */
static uint32_t rx_wptr;                /* RX ring write index (word, & 0xFFF), mirrors reg 0xC */

static rr_link_pkt_t txq[4]; static int txq_r, txq_w;   /* our transmissions (stage 2 pops) */
/* inbound: the LATEST packet from each cabinet, injected one per poll in round
 * robin. A FIFO went stale with 3+ players (N-1 peers send a frame each per
 * frame, one is injected): it filled, then dropped the NEWEST. Here a newer
 * packet replaces an older one from the same cabinet, so each rival is seen at
 * most ~N frames late and never behind a backlog. Online, rr_main then calls
 * rr_link_inject_next + the game's IRQ until every pending cabinet is in (the
 * real link delivers up to 7 frames a frame), so each rival is seen every frame:
 * with one per frame, 4+ players left the game's 8-frame peer timeout only a few
 * frames of slack and Wi-Fi jitter made rivals vanish. */
static rr_link_pkt_t rxl[8]; static unsigned rx_pending; static int rx_hand = -1;

static uint32_t n_tx, n_kick, n_rx, n_irq, n_bad;

static uint16_t sci_word(uint32_t idx)
{
    const uint8_t *p = &g_rr.sci[(idx & 0x1FFF) * 2];
    return (uint16_t)(p[0] << 8 | p[1]);
}
static void rx_word_put(uint32_t idx, uint16_t v)
{
    uint8_t *p = &g_rr.sci[(SCI_RX_BASE + (idx & 0xFFF)) * 2];
    p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v;
}

static uint16_t shadow_w(uint32_t off) { return (uint16_t)(g_rr.sci_reg[off] << 8 | g_rr.sci_reg[off + 1]); }

/* The frame the game just finished staging: words 0..39 of the TX ring. */
static void tx_capture(void)
{
    if (sci_word(RR_LINK_PKT_LEN) != RR_LINK_FRAME) { n_bad++; return; }   /* length word not 0x28: not a packet */
    rr_link_pkt_t *p = &txq[txq_w];
    for (int k = 0; k < RR_LINK_PKT_LEN; k++) p->data[k] = (uint8_t)sci_word((uint32_t)k);
    p->id = (uint8_t)(p->data[1] & 0xF);
    txq_w = (txq_w + 1) % 4;
    if (txq_w == txq_r) txq_r = (txq_r + 1) % 4;        /* full: drop the oldest */
    n_tx++;
    if (debug == 1 && n_tx == 1) {
        fprintf(stderr, "[LINK] first TX packet, id %d:", p->id);
        for (int k = 0; k < RR_LINK_PKT_LEN; k++) fprintf(stderr, " %02X", p->data[k]);
        fprintf(stderr, "\n");
    }
}

/* A transfer completed: size reg reads 0 (success), status bit 2, SCI IRQ. */
static void tx_complete(void)
{
    g_rr.sci_reg[0xA] = g_rr.sci_reg[0xB] = 0;
    tx_done = true;
    rr_hw_sci_irq();
    n_irq++;
}

uint32_t rr_link_reg_read(uint32_t off, int size)
{
    uint32_t v = 0;
    for (int i = 0; i < size; i++) {
        uint8_t b = g_rr.sci_reg[off + (uint32_t)i];
        if (off + (uint32_t)i == 1) {                    /* status byte: the model owns bits 1 and 2 */
            if (rx_frame) b |= 2;
            if (tx_done)  b |= 4;
        }
        v = (v << 8) | b;
    }
    return v;
}

void rr_link_reg_write(uint32_t off, int size, uint32_t v)
{
    for (int i = 0; i < size; i++)
        g_rr.sci_reg[off + (uint32_t)i] = (uint8_t)(v >> ((size - 1 - i) * 8));
    if (size != 2 || (off & 1)) return;
    uint16_t w = shadow_w(off);
    if (off == 0) {                                      /* the handler's status clear (reg 0 = 0) */
        if (!(w & 2)) rx_frame = false;
        if (!(w & 4)) tx_done = false;
    } else if (off == 4) {
        /* rd_link_send's 1 -> 3 toggle: the fill is done = the chip broadcasts it */
        if (w == 3 && !tx_busy) { tx_capture(); tx_complete(); }
    } else if (off == 6) {
        if (w & 1) { tx_busy = true; n_kick++; }         /* TX start (FUN_00026c84's peer kick) */
        else { tx_busy = false; tx_complete(); }             /* MAME: a write of 0 raises "TX done" even when idle (Ace Driver's link init polls it right after clearing reg 6) */
    } else if (off == 0xC) {
        rx_wptr = w & 0xFFF;                             /* the reset's control words resync the ring */
    }
}

bool rr_link_tx_pop(rr_link_pkt_t *out)
{
    if (txq_r == txq_w) return false;
    *out = txq[txq_r];
    txq_r = (txq_r + 1) % 4;
    return true;
}

bool rr_link_tx_pop_latest(rr_link_pkt_t *out)
{
    bool got = false;
    while (rr_link_tx_pop(out)) got = true;
    return got;
}

void rr_link_rx_push(const rr_link_pkt_t *p)
{
    int id = p->id & 7;
    rxl[id] = *p;
    rx_pending |= 1u << id;
}

/* One inbound packet into the RX ring in the game's exact word format, then
 * the frame-present status bit and the SCI IRQ (delivered at the frame edge). */
static void inject(const rr_link_pkt_t *p)
{
    uint32_t s = rx_wptr;
    unsigned sum = 0;
    for (int k = 0; k < RR_LINK_PKT_LEN; k++) { rx_word_put(s + (uint32_t)k, p->data[k]); sum += p->data[k]; }
    rx_word_put(s + RR_LINK_PKT_LEN, RR_LINK_FRAME);
    rx_word_put(s + RR_LINK_PKT_LEN + 1, (uint16_t)(0x100 | (sum & 0xFF)));
    rx_wptr = (s + RR_LINK_FRAME) & 0xFFF;
    g_rr.sci_reg[0xC] = (uint8_t)(rx_wptr >> 8); g_rr.sci_reg[0xD] = (uint8_t)rx_wptr;
    rx_frame = true;
    rr_hw_sci_irq();
    n_rx++; n_irq++;
}

static void inject_one(void)
{
    int id = rx_hand;
    for (int k = 0; k < 8; k++) {                        /* next pending cabinet after the last one served */
        id = (id + 1) & 7;
        if (rx_pending & (1u << id)) break;
    }
    rx_hand = id;
    rx_pending &= ~(1u << id);
    inject(&rxl[id]);
}

bool rr_link_net_legacy(void)
{
    static int legacy = -1;
    if (legacy < 0) { const char *e = getenv("RR_NET_LEGACY"); legacy = e && *e == '1'; }
    return legacy != 0;
}

bool rr_link_inject_next(void)
{
    if (rr_link_net_legacy()) return false;                            /* RR_NET_LEGACY=1: one peer per frame, as before (A/B) */
    if (!rx_pending || rx_frame) return false;           /* nothing waiting, or the game has not taken the last one */
    inject_one();
    return true;
}

void rr_link_poll(void)
{
    if (loopback < 0) { const char *e = getenv("RR_LINK_LOOPBACK"); loopback = e && *e == '1'; }
    if (debug < 0) { const char *e = getenv("RR_LINK_DEBUG"); debug = e && *e == '1'; }
    if (loopback && !net_active) {         /* a net session owns the TX queue: reflect only when solo */
        /* reflect our own broadcast as if from the next cabinet number, so the
         * solo game's liveness bitmask (WRAM 0x10009104) sees a second cabinet */
        rr_link_pkt_t p;
        if (rr_link_tx_pop(&p)) {
            int me = g_rr.wram[0x1060] & 7;              /* settings group 3 byte 0 = WRAM 0x10001060 */
            p.id = (uint8_t)((me + 1) & 7);
            p.data[0] = 0; p.data[1] = p.id;
            rr_link_rx_push(&p);
        }
    }
    if (rx_pending) inject_one();                        /* the handler's ring scan finds one frame per IRQ */
    if (tx_done || rx_frame) rr_hw_sci_irq();            /* status still set (the ack raced us): re-assert */
    if (debug == 1) {
        static uint32_t last, last_live;
        uint32_t live = g_rr.wram[0x9104] << 8 | g_rr.wram[0x9105];   /* A6W(0x1104) = 0x10009104 */
        if (live != last_live) { fprintf(stderr, "[LINK] peer liveness 0x1104 = %04X\n", live); last_live = live; }
        if (++last >= 60) {
            last = 0;
            fprintf(stderr, "[LINK] tx %u  kick %u  rx %u  irq %u  bad %u\n", n_tx, n_kick, n_rx, n_irq, n_bad);
        }
    }
}

void rr_link_init(void) { rr_link_poll(); }              /* picks up the env switches */
