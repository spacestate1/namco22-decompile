/*
 * c139.h -- the Namco C139 SCI, the serial chip that links cabinets (System 22, Super System 22, System 21), as OUR model of its
 * behaviour. MAME has none (namcos22_sci_r returns 0x0004 at register 0 and ignores every write; the sets are MACHINE_NODEVICE_LAN), so
 * the semantics here come from the games' own drivers, read in their lifted 68K (Tokyo Wars FUN_00092F82 init, FUN_00093072 send,
 * FUN_000932A0 the SCI interrupt handler, FUN_0009362E the per-frame latch).
 *
 * The chip: 16 KB of buffer RAM (0x2000 big-endian 16-bit words; words 0x0000-0x0FFF are where a game builds what it sends, words
 * 0x1000-0x1FFF the RECEIVE RING) and eight 16-bit registers:
 *   0 status    read: bit 1 = a frame was received (MAME's stub reads 0x0004; bit 2 is kept set as it reads there). Write 0 clears it.
 *   1 control   stored and read back (Tokyo Wars writes 0xF / 0xD / 0xB / 9 around each transfer; no effect is modelled)
 *   2 fifo      stored (0 at Tokyo Wars' init)
 *   3 tx ctrl   stored (Tokyo Wars toggles 1 .. 0 around a transfer when more are queued)
 *   4 -         stored (0xFF at init)
 *   5 tx size   WRITING a non-zero low byte TRANSMITS that many words, starting at word (reg 7); reads back non-zero (busy) until
 *               the transfer is done, then 0, and the chip interrupts ("TX done")
 *   6 rx ptr    the receive ring's write pointer (low 12 bits = word index into 0x1000-0x1FFF); the game sets it at init and reads it
 *               in the interrupt to find the newest frame
 *   7 tx ptr    word index of the frame to send (0x0000-0x1FFF: a cabinet RELAYS a received frame by sending it straight out of the
 *               receive ring, pointer 0x1000 + index)
 * Words are 9 bits on the wire: bit 8 marks a frame's last word (the game writes it in its own buffer: Tokyo Wars' checksum word is
 * 0x100 | sum); a received frame lands in the ring word for word, bit 8 included, which is what the interrupt handler scans back for.
 *
 * The link is a RING: a cabinet's transmitter feeds the next cabinet's receiver. Each cabinet relays what it receives (the game
 * decides: Tokyo Wars counts hops in the frame and stops at 5), and knows the link is whole when its own frame comes back round.
 *
 * Time: a transfer completes at the next c139_tick() after it starts (the host ticks once per scheduler slice, 1/16 of a frame), and
 * one received frame is placed in the ring per tick while the previous one has been taken (status bit 1 cleared). The interrupt line is
 * raised through the callback; the board calls c139_irq_ack() when the game acknowledges it (syscon).
 */
#ifndef ENG_C139_H
#define ENG_C139_H
#include <stdint.h>
#include <stdbool.h>

#define C139_FRAME_MAX  0x100            /* words in one transfer (Tokyo Wars sends 0x46) */
#define C139_QUEUE      64               /* frames held each way */

typedef struct c139_frame {
    uint16_t n;                          /* words */
    uint16_t w[C139_FRAME_MAX];          /* 9-bit words */
} c139_frame;

/* ram: the board's SCI buffer, big-endian, 0x4000 bytes. irq: raise the SCI interrupt line; false = the line is disabled (the game has
 * not enabled it), and the chip tries again at the next tick. */
void     c139_init(uint8_t *ram, bool (*irq)(void));
uint16_t c139_reg_read(unsigned reg);                    /* reg 0..7 */
void     c139_reg_write(unsigned reg, uint16_t v);
void     c139_tick(void);                                /* once a slice: finish a transfer, place a received frame, raise the line */
void     c139_irq_ack(void);                             /* the game acknowledged the SCI interrupt */
bool     c139_line(void);                                /* the chip holds its interrupt line up */
void     c139_line_drop(void);                           /* the board dropped the line without an acknowledge (the game disabled it): events stay pending */

/* Board / driver settings (default 0 = the behaviour above, Tokyo Wars'). System 21 Cyber Sled's driver (its slave 68000, 0x3692 init,
 * 0x3700 queue, 0x37D8 handler) needs two more of the chip:
 *   C139_MASKED_IRQ  reg 1 is the IRQ MASK: bit 1 RX, bit 2 TX (1 = masked); the chip interrupts for "a frame received" (status bit 1)
 *                    and for "the transmitter IDLE" (a level, not an event) when that cause is unmasked, and sets the cause's mask bit as
 *                    it does -- the handler re-arms with `andi #9 / #$b / #$d`, and the driver's queue routine STARTS sending by clearing
 *                    bit 2 (an idle transmitter then interrupts at once). Writing reg 0 clears bit 1. No line/acknowledge bookkeeping:
 *                    the board's interrupt controller holds the line until the game acknowledges it there.
 *   C139_PTR_LAST    reg 6 is the index of the LAST word received (Cyber Sled's handler scans back from the word AT the pointer; Tokyo
 *                    Wars' and Rave Racer's from pointer - 1, i.e. the next word to be written) -- the drivers disagree, so it is a setting
 *   C139_HOST_WORDS  the buffer RAM is host-order uint16_t[0x2000] (the board's own array) instead of big-endian bytes */
#define C139_MASKED_IRQ  1u
#define C139_PTR_LAST    2u
#define C139_HOST_WORDS  4u
void     c139_set_mode(unsigned flags);                  /* after c139_init */
bool     c139_tx_busy(void);

/* the wire */
bool     c139_tx_take(c139_frame *out);                  /* the next frame this chip sent (oldest first) */
void     c139_rx_put(const c139_frame *f);               /* a frame arriving on the receive line */

typedef struct { uint32_t tx, rx, irq, rx_drop, tx_drop; } c139_stats;
c139_stats c139_get_stats(void);
#endif
