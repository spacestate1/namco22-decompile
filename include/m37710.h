/* Mitsubishi M37710 (7700 series) CPU core -- the Prop Cycle sound MCU.
 *
 * WHY THIS EXISTS. AUDIO_PLAN.md's "route not taken": the sound driver lives
 * in pr1data.8k and nothing else can produce the real music. Every attempt to
 * model its OUTPUT instead has the master-DSP failure mode -- a captured
 * register script replays identically no matter what the player does
 * (measured: two runs at pedal 40 and 127 are bit-identical, max sample
 * difference 0). The driver has to actually run.
 *
 * Board wiring (namcos22.cpp, namcos22s_state::mcu_program):
 *     0x002000-0x002fff  C352            -> src/c352.c
 *     0x004000-0x00bfff  shared RAM      -> g_sys.commsram (0x8000, exact)
 *     0x00c000-0x00ffff  ROM @ 0xc000
 *     0x200000-0x27ffff  ROM @ 0
 *     0x308000-0x308003  MB87078 volume  (byte lane 0x00ff)
 * Clock 49.152MHz/3 = 16.384MHz. ROM_REGION16_LE: the image is LITTLE-endian,
 * unlike every other ROM in this project.
 *
 * Vectors read out of the image: RESET 0xC030, NMI 0xC185, and the rest a
 * regular table of 3-byte jmp stubs at 0xFFE0..0xFFFD -- i.e. a well-formed
 * program, not a mis-mapped image.
 *
 * The 7700 is 65C816-shaped: 16-bit A/X/Y whose widths are selected by the m
 * and x flags, a program bank PG and data bank DT, a movable direct page DPR,
 * and an interrupt priority level in the high byte of PS. It adds a B
 * accumulator reached through a 0x42 prefix, and multiply/divide.
 */
#ifndef PROPCYCL_M37710_H
#define PROPCYCL_M37710_H

#include <stdint.h>
#include <stdbool.h>

enum {                      /* PS, low byte */
    M377_C = 0x0001, M377_Z = 0x0002, M377_I = 0x0004, M377_D = 0x0008,
    M377_X = 0x0010,        /* index width:       1 = 8-bit  */
    M377_M = 0x0020,        /* accumulator width: 1 = 8-bit  */
    M377_V = 0x0040, M377_N = 0x0080
};
#define M377_IPL_SHIFT 8    /* PS bits 8-10: interrupt priority level */

typedef struct m37710_s m37710_t;

/* The bus. 16-bit accesses are little-endian on this part. */
typedef uint8_t (*m377_read8_fn )(void *user, uint32_t addr);
typedef void    (*m377_write8_fn)(void *user, uint32_t addr, uint8_t val);

struct m37710_s {
    uint16_t a, b, x, y, s, pc, dpr;
    uint8_t  pg, dt;            /* program bank, data bank */
    uint16_t ps;
    bool     stopped;           /* STP/WAI */
    /* A short ring of recent PCs, kept only when a probe asks for it. A
     * stray store into the peripheral block is invisible at the store
     * itself -- what matters is how the index got there -- so the trap
     * prints the path that led in. */
    uint16_t pchist[64]; uint8_t pchist_n; uint8_t pchist_on;
    uint64_t irq_taken;         /* how many interrupts have been serviced */
    uint32_t pending;           /* one bit per 0xFFE0-relative vector/2 */

    /* The internal peripheral block at 0x000000-0x00007F. These are part of
     * the CPU, not the board -- MAME puts them in the device too -- and the
     * timers have to live with them because their periods come straight out
     * of these registers. Modelling them outside meant guessing rates, and
     * the driver does not hold still: it starts and stops Timer B0 at run
     * time by rewriting the count-start register (measured: 0x40 alternates
     * 0x0D and 0x2D, bit 5 being B0). */
    uint8_t  sfr[0x80];
    uint64_t t_next[8];         /* cycle of each timer's next underflow */
    uint8_t  t_on;              /* which of the 8 are counting */

    /* A-D converter. On this board the SAME MCU reads the handlebar and the
     * pedal, so the sound driver services it too and its interrupt (0xFFD6,
     * priority 5 -- the highest the driver programs) is part of the normal
     * instruction stream. `analog` is filled by the host from the cabinet
     * controls; a channel left at 0 simply converts to 0. */
    uint16_t analog[8];
    uint16_t ad_result[8];
    uint64_t ad_due;            /* 0 = no conversion in flight */
    bool     use_b;             /* set for one instruction by the 0x42 prefix */
    uint64_t cycles;

    m377_read8_fn  read8;
    m377_write8_fn write8;
    void          *user;

    /* Opcode coverage, so the probe can report what the real program needs
     * in the order it needs it rather than guessing from a datasheet. */
    uint32_t op_count[512];     /* 0x00-0xFF, plus 0x100+op for the 0x42 page */
    uint8_t  op_known[512];
    uint32_t unimpl_pc;
    uint16_t unimpl_op;
    bool     unimpl_hit;
};

void m37710_init (m37710_t *c, m377_read8_fn r, m377_write8_fn w, void *user);
void m37710_reset(m37710_t *c);

/* Runs until `cycles` have elapsed or an unimplemented opcode is hit.
 * Returns the number of instructions retired; check `unimpl_hit`. */
int  m37710_run  (m37710_t *c, int cycles);

/* Raise an interrupt. `vector_offset` is relative to the table base at
 * 0xFFE0, so Timer A0 (0xFFEE) is 0x0E and external INT0 (0xFFF4) is 0x14.
 * Its priority is read LIVE from that source's control register (SFR 0x75
 * for Timer A0, 0x7A for Timer B0, 0x7D for INT0, 0x7F for INT2) at the
 * moment the request is arbitrated, not latched when it is raised -- the
 * driver programs those registers while requests are already pending.
 *
 * The request is LATCHED, never dropped: it is taken when the mask clears
 * AND its priority exceeds the current IPL. Both halves matter. The driver
 * runs with interrupts disabled across its critical sections and expects the
 * tick still to arrive afterwards; and it relies on a handler NOT being
 * preempted by a lower-priority source, which is what the IPL in PS is for.
 * Measured: without the IPL our stream left MAME's at instruction 74,850,
 * taking a Timer A0 inside the INT2 handler that the real part refuses. */
/* Recognise a latched interrupt now, at this instruction boundary. Called
 * for you by m37710_run(); exposed so a tracer can sample the PC at the same
 * point the hardware decides. */
void m37710_service(m37710_t *c);

/* Raise one of the EXTERNAL interrupt pins. The board drives INT0 and INT2
 * (namcos22.cpp's `mcu_irq` scanline timer, at 480 and 240); the internal
 * timers raise themselves from the registers above. */
void m37710_irq  (m37710_t *c, int vector_offset);

#endif /* PROPCYCL_M37710_H */
