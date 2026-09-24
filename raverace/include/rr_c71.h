/*
 * c71_master.h -- the System 22 / Super System 22 MASTER DSP: a TMS320C25
 * ("C71") running the game-uploaded master program, with the Namco bus it
 * sees (the 16-bit banked window onto polygon RAM, the point-ROM ports and
 * the PDP block-copy coprocessor).
 *
 * A C port of the MiSTer project's tools/pc_tms_interp.py, instruction for
 * instruction. That interpreter is gated against MAME's own polygon-RAM dumps
 * (tools/pc_master_gate.py); tools/master_gate.c gates this port against the
 * Python write-for-write and against the same dumps.
 */
#ifndef RR_C71_H
#define RR_C71_H
#include <stdint.h>
#include <stdbool.h>

#define C71_POLY_WORDS   0x8000       /* polygon RAM, 32-bit words (24 used) */
#define C71_PTRAM_SS22   0xF80000     /* point RAM window: Super System 22 */
#define C71_PTRAM_S22    0xF00000     /* ... and System 22 */
#define C71_PTRAM_WORDS  0x20000

typedef struct c71 {
    /* ---- memories -------------------------------------------------------- */
    uint16_t prog[0x10000];           /* program space: BIOS at 0, game at 0x4000 */
    uint16_t ram[0x10000];            /* data space below 0x8000 */
    uint32_t *poly;                   /* polygon RAM (caller-owned, 0x8000 words). Holds MAME's
                                         32-bit value: CPU/PDP writes are 24-bit SIGN-EXTENDED, so a
                                         bank-1 read of a negative word is 0xFFFF, not 0x00FF.
                                         Consumers mask to 24 bits. */
    const uint32_t *ptrom;            /* point ROM, 24-bit words */
    uint32_t ptrom_words;
    uint32_t ptram[C71_PTRAM_WORDS];  /* point RAM 0xF80000.. */

    /* ---- bus state ------------------------------------------------------- */
    int bank;                         /* port 0xD: dspram16 window bank */
    uint16_t latch;                   /* bank-2 high-word latch */
    uint32_t pt_addr;                 /* port 3: point address (shifted in 16 bits at a time) */
    uint32_t pt_data;                 /* port 1 high word, completed by a port-0 write */
    int bioz;                         /* BIO pin (1 = not ready) */

    /* ---- CPU state ------------------------------------------------------- */
    uint16_t pc, pfc, t;
    int32_t  acc;
    int64_t  p;                       /* product register (LPH can set 32 bits) */
    uint16_t ar[8];
    int arp, arb, dp, pm, sxm, ovm, intm, c, tc, cnf;
    uint16_t imr, prd, tim;
    int tint_pend;
    /* ---- Rave Racer additions (System 22) ---------------------------------- */
    uint32_t ptram_base;              /* C71_PTRAM_S22 or C71_PTRAM_SS22 */
    int ss22;                         /* port-2 read runs the PDP command block (SS22 only) */
    uint16_t pdp_base;                /* port 2 write (S22: display-list output base) */
    int idle;
    uint32_t pdp_begins;              /* port-2 reads: the frame's display list is complete */                         /* IDLE executed, waiting for an interrupt */
    uint16_t ifr;                     /* pending: 1 INT0, 2 INT1, 4 INT2, 0x10 RINT, 0x20 XINT */
    void (*render_w)(uint16_t);       /* port 0xC: direct commands to the render device */
    void (*render_reset)(void);       /* port 8 write */
    void (*pdp_begin)(void);          /* port 2 read */
    void (*slave_w)(uint16_t);
    void (*port3_r)(void);            /* dsp_unk_port3_r: upload state back to READY */        /* port 7: slave upload / enable commands */
    uint16_t stack[64];
    int sp;
    int rpt;

    /* ---- bookkeeping ----------------------------------------------------- */
    uint16_t cur_pc;
    uint64_t steps;
    uint8_t  written[C71_POLY_WORDS]; /* polygon-RAM words the master wrote */
    uint32_t n_written;
    char     error[96];               /* set when execution stops on a fault */
} c71_t;

/* Load the BIOS (c71.bin, 8 KB at program 0) and the game's master program
 * (at program 0x4000). Either path may be NULL. Returns false on a read error. */
bool c71_load(c71_t *d, const char *bios_path, const char *prog_path);
/* Reset CPU state (memories kept). */
void c71_reset(c71_t *d);
/* Execute one instruction (with its RPT repeats). Returns false if it hit an
 * unimplemented opcode or a stack fault; d->error says which. */
bool c71_step(c71_t *d);

/* raise an interrupt (HOLD_LINE: stays pending until taken) */
static inline void c71_irq(c71_t *d, uint16_t bit) { d->ifr |= bit; }

#endif
