/*
 * c25.h -- the System 22 / Super System 22 MASTER DSP: a TMS320C25
 * ("C71") running the game-uploaded master program, with the Namco bus it
 * sees (the 16-bit banked window onto polygon RAM, the point-ROM ports and
 * the PDP block-copy coprocessor).
 *
 * THE SHARED ENGINE'S C25 (engine/c25): the chip's BUS (the Namco memory map,
 * ports and PDP -- board hardware), the instruction SEMANTICS (c25_sem.h) and
 * the STEP loop (interrupts, IDLE, the timer, RPT). What it does NOT contain
 * is a fetch/decode loop: the program each game runs is TRANSLATED to C at
 * build time (tools/gen/c25_translate.py -> gen/<game>_c25.c), and the step
 * calls that translation through `xlat`. The interpreter that decodes the
 * program at run time survives only as the test oracle the translation is
 * gated against (tools/c25oracle/, dev builds).
 *
 * Semantics are the project's validated interpreter's (itself a port of the
 * MiSTer project's tools/pc_tms_interp.py, gated against MAME's own
 * polygon-RAM dumps), moved here unchanged.
 */
#ifndef ENG_C25_H
#define ENG_C25_H
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

    /* ---- board settings (the two boards' cores differed here) -------------- */
    int idle_halts;                   /* 1: IDLE = INTM 0 + halt until an interrupt (TI; Rave Racer).
                                         0: IDLE does nothing (Prop Cycle's host parks the master itself) */
    int port3_bioz;                   /* 1: a port-3 read sets the BIO pin busy again (Rave Racer) */

    /* ---- the translated program --------------------------------------------- */
    /* Executes the instruction at pc with its RPT repeats (pc already known to
     * be at an instruction boundary). Returns false on a fault (d->error). */
    bool (*xlat)(struct c71 *d, int pc);
    const uint16_t *ops;              /* the translation's constant words while one executes */
} c71_t;

/* Development hooks (tools/c25oracle): NULL in the game. */
extern void (*c25_hook_acc)(int kind, int space, uint32_t a, uint32_t v);
extern void (*c25_hook_pre)(c71_t *d, int pc);
extern void (*c25_hook_iter)(void);
extern void (*c25_hook_post)(c71_t *d);

/* Load the BIOS (c71.bin, 8 KB at program 0) and the game's master program
 * (at program 0x4000). Either path may be NULL. Returns false on a read error. */
bool c71_load(c71_t *d, const char *bios_path, const char *prog_path);
/* Reset CPU state (memories kept). */
void c71_reset(c71_t *d);
/* One step: take a pending interrupt, idle, tick the timer, then run the
 * instruction at PC through d->xlat. Returns false on a fault (d->error). */
bool c71_step(c71_t *d);
/* `steps` steps -- the same result as calling c71_step that many times, with a halted (IDLE) DSP fast-forwarded. */
bool c71_run(c71_t *d, long steps);

/* ---- the bus (engine/c25/c25_bus.c), used by the semantics ---------------- */
uint16_t c25_dr(c71_t *d, uint32_t a);            /* data space read */
void     c25_dw(c71_t *d, uint32_t a, uint32_t v);/* data space write */
uint16_t c25_pr(c71_t *d, uint16_t a);            /* program space read (TBLR, MAC, BLKP) */
void     c25_pw(c71_t *d, uint16_t a, uint16_t v);/* program space write (TBLW) */
uint16_t c25_port_in(c71_t *d, int pa);
void     c25_port_out(c71_t *d, int pa, uint16_t v);

/* raise an interrupt (HOLD_LINE: stays pending until taken) */
static inline void c71_irq(c71_t *d, uint16_t bit) { d->ifr |= bit; }

#endif
