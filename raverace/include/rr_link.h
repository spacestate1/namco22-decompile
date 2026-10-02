/*
 * rr_link.h -- the C139 SCI link chip (cabinet-to-cabinet network).
 *
 * The game's link driver (decompiled FUN_00026b20..FUN_000272d4, readable
 * src/rd/rd_b4.c) runs every frame: rd_link_send stages a 0x26-byte packet
 * (6 slot words + 12 position words + 0xFFFF) and writes it one byte per
 * 16-bit word into the TX ring at C139 RAM words 0..39 (0x20010000), followed
 * by a length word (0x28) and the byte sum, toggling control reg 0x20020004
 * 1 -> 3. The chip would then broadcast it to the peers' RX rings
 * (0x20012000, 0x1000 words) and raise the SCI IRQ (syscon line 2, level 6,
 * autovector FUN_00026bbe): status reg 0x20020000 bit 1 = RX frame present,
 * bit 2 = TX done; reg 0x2002000C = RX ring write pointer (word index).
 */
#ifndef RR_LINK_H
#define RR_LINK_H
#include <stdint.h>
#include <stdbool.h>

#define RR_LINK_PKT_LEN  0x26     /* packet payload bytes (6 slot + 12 position words + terminator) */
#define RR_LINK_FRAME    0x28     /* ring words per frame: payload + length word + checksum word */

typedef struct rr_link_pkt {
    uint8_t id;                          /* sender cabinet 0..7 (= payload word 0: byte 0 = 0, byte 1 = id) */
    uint8_t data[RR_LINK_PKT_LEN];       /* the staged packet bytes, verbatim */
} rr_link_pkt_t;

void rr_link_init(void);                 /* env: RR_LINK_LOOPBACK=1, RR_LINK_DEBUG=1 */

/* C139 register block 0x20020000-0x2002000F (called from rr_hw.c's io hooks). */
uint32_t rr_link_reg_read(uint32_t off, int size);
void     rr_link_reg_write(uint32_t off, int size, uint32_t v);

void rr_link_poll(void);                 /* once per frame, before IRQ delivery */

/* Stage 2 (network client): a packet this cabinet transmitted (pop once per
 * frame and send to the peers), and a packet a peer sent (push; injected into
 * the RX ring at the next rr_link_poll, one per frame). */
bool rr_link_tx_pop(rr_link_pkt_t *out);
bool rr_link_tx_pop_latest(rr_link_pkt_t *out);   /* the NEWEST staged packet, the older ones dropped (online: no backlog delay) */
/* online: inject the next pending peer packet if the game has taken the last one (its SCI handler cleared
 * the frame bit); false = nothing pending, or the last one not taken yet. rr_main runs the game's IRQ after
 * each, so every peer's newest packet reaches the game every frame instead of one peer per frame. */
bool rr_link_inject_next(void);
bool rr_link_net_legacy(void);           /* RR_NET_LEGACY=1: the old link path (one peer a frame, oldest packet sent), for A/B */
void rr_link_rx_push(const rr_link_pkt_t *p);
void rr_link_net_active(bool on);        /* a net session owns the TX queue: RR_LINK_LOOPBACK yields */

#endif
