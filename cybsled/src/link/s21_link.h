/*
 * s21_link.h -- the C139 serial link chip (our own model of its behaviour) and the cabinet-to-cabinet link of Cyber Sled (module N2,
 * PLAN.md "Module N").
 *
 * MAME does NOT implement the link: namco_c139.cpp keeps the 16 KB RAM, answers status 4 and nothing else ("TODO: Make this to
 * actually work!"), and namcos21_c67.cpp never connects its IRQ. Everything below is derived from the game's own driver, the SLAVE
 * 68000's code at 0x3188 / 0x3692 / 0x3700 / 0x37D8 (the level-4 SCI handler; the master never touches the chip).
 *
 * The game's protocol (slave 0x3188..0x3A1E): a RING of up to 16 "slots"; each cabinet owns slot POSITION (operator setting
 * GAME OPTIONS > POSITION, 1 LEFT / 2 RIGHT = NVRAM 0x180024 bit 0, read into shared 0x900228). A packet is 0x5E 9-bit words:
 *   [0] slot  [1] 0x99  [2] hop count  [3..0x5C] 90 payload bytes (one per word)  [0x5D] byte sum | 0x100 (the frame marker)
 * Every frame each cabinet sends its own slot's packet (hop 1); a receiver adds 1 to the hop (and to the sum), keeps the payload
 * (C139 RAM 0x803 + slot*0x80) and forwards the packet out of its RX ring; a packet that comes back to its owner gives the ring's
 * size (hop - 1; two cabinets = 1) -> C139 RAM word 0x10 -> shared 0x900208 = "linked" for the master. The slots heard in the last 16 frames are
 * published at C139 RAM words 0x11/0x12.
 *
 * The chip as the driver uses it (reg n = 0xB80000 + 2n):
 *   reg 0 status: bit 1 a frame received, bit 2 the transmitter idle; a write clears bit 1
 *   reg 1 IRQ mask: bit 1 RX, bit 2 TX, 1 = masked; the chip sets the bit when it raises that IRQ (the handler clears it to re-arm:
 *         `andi #9 / #$b / #$d`), so each cause interrupts once
 *   reg 3 TX control: bit 0 1 -> 0 starts a transfer of reg 5 words from word address reg 7 (`E, 6=1, A, 6=0` at 0x3822)
 *   reg 5 TX length (words); cleared when a transfer completes
 *   reg 6 RX pointer: word index of the LAST word received (the handler scans back from it for bit 8); the RX ring is words
 *         0x1000..0x1FFF (init 0x1000)
 *   reg 7 TX start word address (own packets 0x800 + slot * 0x80, forwards in the RX ring, wrapping inside it)
 * Unknown on the real chip (not used by this driver): regs 2 and 4, the wire speed. A transfer takes S21_C139_TX_SLICES board slices.
 *
 * Cabinets are linked in LOCKSTEP: the packets a cabinet transmits during frame f are delivered to the other during frame f + D
 * (D = the link delay, >= 1), one per RX interrupt, at fixed points of the frame -- so both machines are deterministic functions of
 * their inputs and stay in step whatever the transport's timing.
 */
#ifndef S21_LINK_H
#define S21_LINK_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define S21_LINK_PLAYERS      2       /* cabinets: POSITION 0 (LEFT) and 1 (RIGHT) */
#define S21_LINK_PKT_MAX      12      /* packets a frame (a frame carries 2 in a two-cabinet ring: own + forward) */
#define S21_LINK_PAYLOAD_MAX  (1 + S21_LINK_PKT_MAX * (1 + 255 + 32))   /* bytes per frame, worst case (3457); typical 215 */
#define S21_C139_TX_SLICES    3

/* ---- the board's side (src/board/s21_board.c) ---- */
uint16_t s21_c139_reg_r(uint32_t a);                     /* 0xB80000..0xB8000F; unlinked: the old stand-in (status 4) */
void     s21_c139_reg_w(uint32_t a, uint16_t d, uint16_t m);
void     s21_c139_slice(int slice);                      /* once per scheduler slice: transfers, RX delivery, the IRQ */
void     s21_link_frame_begin(uint32_t frame);           /* before a frame: the peer's packets for it (waits, lockstep) */
void     s21_link_frame_end(uint32_t frame);             /* after a frame: our packets out */
void     s21_board_sci_irq(int cpu);                     /* s21_board.c: assert that 68000's C148 SCI source */

/* ---- the online layer's side ---- */
/* A transport carries one opaque payload per frame each way. send: our payload for frame f (once per frame, in order). recv: the
 * peer's payload for frame f into p -> its length (>= 0), -1 = not here yet (the link calls again, after poll()), -2 = peer gone.
 * poll: wait up to ms for traffic (and resend what is unacknowledged); may be NULL. */
typedef struct {
    void *ctx;
    void (*send)(void *ctx, uint32_t f, const uint8_t *p, size_t n);
    int  (*recv)(void *ctx, uint32_t f, uint8_t *p, size_t cap);
    void (*poll)(void *ctx, int ms);
    void (*close)(void *ctx);
} s21_link_transport;

/* Attach a transport: position = this cabinet's slot (0 LEFT, 1 RIGHT; written to the NVRAM's POSITION setting before frame 0 --
 * the machine's own setting, so attach before the first frame), delay = D frames (>= 1). Returns false if already attached. */
bool   s21_link_attach(const s21_link_transport *t, int position, int delay);
void   s21_link_detach(void);                            /* = a pulled cable: the chip keeps running unconnected */
bool   s21_link_init_env(void);                          /* CS_LINK=loop | <bindhost:port>,<peerhost:port> (UDP), CS_LINK_POS, CS_LINK_DELAY */
/* The payload format, for a layer that moves frames itself (the transport callbacks use it already):
 *   u8 count; count x { u8 nwords; nwords x u8 (low bytes); ceil(nwords/8) x u8 (bit 8 of each word, LSB first) } */
size_t s21_link_pack(uint8_t *out, size_t cap);          /* the packets this cabinet transmitted in the frame just run */
bool   s21_link_unpack(const uint8_t *in, size_t n);     /* the peer's packets -> delivered during the next frame */

typedef enum { S21_LINK_OFF, S21_LINK_LOOP, S21_LINK_ON, S21_LINK_GONE, S21_LINK_NET } s21_link_state_t;   /* ON = lockstep, NET = online session */
s21_link_state_t s21_link_state(void);                   /* GONE: the peer stopped answering (timeout / -2); the game then shows
                                                          * its own link loss, the chip keeps running unconnected */
int    s21_link_ring_size(void);
size_t s21_link_pack_n(uint8_t *out, size_t cap, int max_pkts);   /* pack at most max_pkts (the rest are counted as dropped) */
void   s21_link_set_position(int pos, bool live);        /* POSITION 0/1 into the NVRAM (+ check byte); live: also the game's working copies */
void   s21_link_net_mode(void);                          /* s21_net.c: the chip on, unplugged until a session */
void   s21_link_unplug(bool unplugged);                  /* s21_net.c: session over / session begins */
void   s21_link_chip_on_live(void);                      /* s21_net.c: a window connecting mid-game: the chip on now, in the state the driver init leaves */
void   s21_link_net_poll_on(void);                       /* s21_net.c: the frame hook polls the online client */

/* ---- online (src/link/s21_net.c, engine/net.c, docs/NETPLAY.md): game id "cs", 2 players, S21_NET_FRAME bytes a frame ----
 *   CS_NET_SERVER=host[:port] [CS_NET_NAME=x]   CS_NET_HOST=1 (built-in LAN host)   CS_NET_DISCOVER=1   CS_NET_AUTOSTART=2 (engine)
 *   CS_NET_LEAD=<n> (stall-and-wait lead, default auto; 1000 = off)   window: the Online page of the menu (s21_net_window_init) */
#define S21_NET_FRAME 432                                /* 1 + 4 packets x (1 + 94 + 12): a frame carries <= 2 in a two-cabinet ring */
void   s21_net_window_init(void);                        /* after ss22_host_open: eng_net_init + the Online menu page */
void   s21_net_restore_settings(void);               /* before saving cs21.nv: the cabinet's own FREE PLAY back (a session sets it) */
void   s21_net_paused(void);                             /* the host's menu/pause loop: keepalive */                         
#endif
