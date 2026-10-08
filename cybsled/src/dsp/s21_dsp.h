/*
 * s21_dsp.h -- Cyber Sled (Namco System 21): the DSP board (cybsled/DESIGN.md, module B).
 *
 * Five C67 chips -- TMS320C25s with a 4K-word internal ROM (c67.bin, the Namco BIOS) -- in a 1 master + 4 slave topology. The
 * master DSP walks the 68000s' display list in DSP RAM and feeds the slave through the IDC (inter-DSP channel) FIFO, expanding
 * object references out of point ROM; the slave transforms and projects, and hands finished polygons to the renderer through its
 * port 0. The master can also draw "direct" quads itself (ports B/C). Like MAME's model, ONE slave does the work of four (it runs
 * at four times the clock) -- the four-way split across the real slaves is not modelled.
 *
 * Our own code reproducing the board's behaviour (MAME's namcos21_dsp_c67 is the reference and the oracle, never a source of code
 * here), on the shared engine's TMS320C25 core (engine/c25: the step + semantics). The DSP PROGRAMS are uploaded by the master
 * 68000 at boot (the BIOS copies them out of DSP RAM into program RAM); in a dev build they run on the interpreter oracle
 * (tools/c25oracle), later on their translation (tools/gen/c25_translate.py) -- HARD RULE 1.
 *
 * Addresses below are the 68000 view: DSP RAM 0x200000-0x20FFFF = word offset 0..0x7FFF, point-RAM control 0x400000, point-RAM
 * data 0x440000 (bytes, odd lane), depth cue 0x480000-0x4807FF = word offset 0..0x3FF. `mask` is the 68000 byte-lane mask
 * (0xFF00 upper, 0x00FF lower, 0xFFFF word), MAME's mem_mask.
 */
#ifndef S21_DSP_H
#define S21_DSP_H
#include <stdint.h>
#include <stdbool.h>

/* One quad for the rasteriser: namcos21_3d blit_single_quad's input -- screen x/y relative to the centre, z codes, colour word
 * (bit 15 set = a direct-draw quad with its colour as is; otherwise bits 0-14 from point RAM). Same layout as src/video's. */
#ifndef S21_QUAD_DEFINED
#define S21_QUAD_DEFINED
typedef struct { int sx[4], sy[4], z[4]; uint16_t color; } s21_quad;
#endif

/* Load c67.bin (the internal ROM) and the six point-ROM chips cy1-poi-* from `romdir`. False (with a message) on a missing file. */
bool s21_dsp_init(const char *romdir);

/* 68000 side (both CPUs: the board decodes them identically) */
uint16_t s21_dsp_ram_r(uint32_t off);
void     s21_dsp_ram_w(uint32_t off, uint16_t data, uint16_t mask);
void     s21_dsp_ptram_ctl_w(uint16_t data, uint16_t mask);
uint16_t s21_dsp_ptram_r(void);
void     s21_dsp_ptram_w(uint16_t data, uint16_t mask);
uint16_t s21_dsp_depthcue_r(uint32_t off);
void     s21_dsp_depthcue_w(uint32_t off, uint16_t data, uint16_t mask);

/* the master C148's outputs: system reset (ext2 bit 0 CLEAR = held: pass state = 1 while held), and the sound-reset register's
 * bit 2 (ext1), which starts the DSP frame loop the first time it is set after a board reset */
void s21_dsp_reset(int state);
void s21_dsp_kick(void);

/* Run the DSPs for `cycles` master instruction cycles (10 MHz; the slave runs 4 per master cycle). Returns false if a DSP stopped
 * on a fault (s21_dsp_error()). */
bool s21_dsp_run(long cycles);
const char *s21_dsp_error(void);

/* Widescreen (Hor+): widen the slave's object-reject window by `px` board pixels on each side (the projection is unchanged;
 * s21_dsp.c "WIDESCREEN"). 0 = the board as is (default). Takes effect at the next 3D frame. */
void s21_dsp_set_view_extra(int px);

/* outputs */
extern void (*s21_quad_out)(const s21_quad *q);     /* every finished quad, in submission order */
extern void (*s21_swap_out)(void);                  /* the 3D frame buffer swap (MAME swap_and_clear_poly_framebuffer) */
extern void (*s21_slave_word_out)(uint16_t w);      /* dev/gate: every word the slave writes to its port 0 */
extern void (*s21_ddraw_out)(int port, uint16_t w); /* dev/gate: master ports 0xB / 0xC / 0x2 writes */

/* The DSPs run the TRANSLATED C67 programs (gen/cs_c67_{master,slave}.c) from s21_dsp_init on. Development (s21_dsp_dev,
 * C25_DEV_HOOKS): s21_dsp_use_oracle() puts both chips on the interpreter oracle instead; s21_dsp_lockstep() keeps the
 * translations driving the board and runs an interpreter SHADOW beside each chip, compared after every slice (false in a game
 * build); s21_dsp_lockstep_report() gives the slices compared and how many differed. */
void s21_dsp_use_oracle(void);
bool s21_dsp_lockstep(void);
void s21_dsp_lockstep_report(long *slices, long *bad);
/* instructions passed by the polling-loop fast-forward (which 0 master / 1 slave): counted in the steps, not executed one by one */
uint64_t s21_dsp_ff_skipped(int which);
typedef struct {
    uint64_t m_steps, s_steps;           /* retired instructions */
    uint32_t kicks, quads, records, idc_words, ddraw;
    uint16_t m_pc, s_pc;
    int m_held, s_held, m_idle, s_idle;
} s21_dsp_stats;
void s21_dsp_get_stats(s21_dsp_stats *st);
/* program/data memory for dumps and checks: which 0 master / 1 slave */
const uint16_t *s21_dsp_prog(int which);
const uint16_t *s21_dsp_dspram(void);
int s21_dsp_which(const void *chip);   /* dev hooks: 0 master, 1 slave */
extern int s21_dsp_inject;             /* dev negative control: corrupt the next IDC word */
#endif
