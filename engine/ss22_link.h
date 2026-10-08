/*
 * ss22_link.h -- the cabinet link of a Super System 22 game (engine/ss22_link.c): the board's C139 SCI (engine/c139.c) wired to the
 * scheduler, and what travels between cabinets once a frame.
 *
 * OFF unless asked for (--link, or <TAG>_LINK): with it off the SCI registers are MAME's stub (status 0x0004, writes ignored) and nothing
 * here runs, so a game's single-cabinet output is exactly what it was. A game supports the link only if its table sets ss22_game.link_cabinet
 * (where its operator setting "cabinet ID" lives).
 *
 * THE RING. The arcade wires the cabinets TX -> RX in a loop; the games relay what they receive and count hops. Here the cabinets present
 * (a bit mask of cabinet ids) are put in a loop in id order: each one's receiver is fed from the PREDECESSOR, the next lower id present
 * (the highest one feeds the lowest).
 *
 * THE PAYLOAD (what the online layer carries, opaque to it): once a frame each cabinet packs the frames its chip transmitted during that
 * frame; every other cabinet gets it, and only the cabinet whose predecessor sent it puts it on its receive line. Format (v1):
 *     u8 1, u8 cabinet, u8 count, then count x { u8 n (words), ceil(n/8) bytes of bit 8 (LSB first), n bytes of bits 7..0 }
 * Tokyo Wars sends 0x46-word frames: 80 bytes each; a frame of play carries 1-4 of them (its own, the relays, its extra objects).
 *
 * TRANSPORTS.
 *  - built in, for testing on one machine: --link ID/N[:PORT][:free] -- cabinet ID of N (0..N-1) over UDP on 127.0.0.1, port PORT+cabinet
 *    (default 27800), in LOCKSTEP: at the end of every frame each cabinet sends its payload and waits for its predecessor's payload of the
 *    SAME frame, so a linked run is deterministic. ":free" does not wait (what an online session does).
 *  - external (the online layer, engine/net*): ss22_link_set_external(cb); cb(frame) is called at the end of every frame, and calls
 *    ss22_link_pack() / ss22_link_unpack() / ss22_link_set_cabinets().
 */
#ifndef ENG_SS22_LINK_H
#define ENG_SS22_LINK_H
#include <stdint.h>
#include <stdbool.h>

#define SS22_LINK_MAX_CABS     8
#define SS22_LINK_PAYLOAD_MAX  1024        /* bytes a frame (12 Tokyo Wars frames; more wait for the next frame) */

/* setup (engine/ss22_run.c): spec "ID/N[:PORT][:free]" or "ID" (an external transport supplies the rest) */
bool ss22_link_setup(const char *spec, const char *tag);
extern bool g_ss22_link_on;

/* the board (engine/ss22_board.c) */
uint16_t ss22_link_reg_read(unsigned reg);
void     ss22_link_reg_write(unsigned reg, uint16_t v);
void     ss22_link_sci_ack(void);

/* the scheduler (engine/ss22_run.c) */
void ss22_link_slice(void);                /* each slice: the chip's clock, and the SCI interrupt delivered now rather than at the frame's end */
void ss22_link_frame(uint32_t frame);      /* the frame boundary: exchange payloads */

/* THE ONLINE LAYER'S API */
int      ss22_link_cabinet(void);                                   /* this cabinet's id (0..SS22_LINK_MAX_CABS-1) */
void     ss22_link_set_cabinet(int id);                             /* renumber this cabinet (GO's slot): the game's own setting too */
void     ss22_link_set_cabinets(unsigned mask);                     /* the cabinets in the session (bit = id); re-forms the ring */
unsigned ss22_link_cabinets(void);
int      ss22_link_predecessor(void);                               /* whose payload feeds this receiver; -1 = none (alone) */
int      ss22_link_pack(uint8_t *buf, int max);                     /* this frame's payload (call once a frame, after the frame ran) */
bool     ss22_link_unpack(int from_cab, const uint8_t *buf, int n); /* a peer's payload: false = malformed. Only the predecessor's is used */
void     ss22_link_set_external(void (*frame_cb)(uint32_t frame));   /* the built-in transport steps aside */
#endif
