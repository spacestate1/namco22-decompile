/* Namco System 21 sound board: MC6809E (cy1-snd0.8j) + YM2151 + C140 + the DPRAM shared with the 68000s and the C68
 * -- module D of cybsled/DESIGN.md. The 6809 program is TRANSLATED to C at build time (gen/cs_6809.c, module J of
 * cybsled/PLAN.md): the game library s21_snd runs only that; the DEV library s21_snd_dev adds the interpreter (m6809.c,
 * the oracle, its default; S21_SND_CPU=xlat selects the translation). Output: stereo int16 at S21_SND_OUT_HZ, mixed with MAME's Cyber Sled routes
 * (front / rear speakers: C140 0.25, YM2151 0.15, left and right swapped onto channels 1 / 0). */
#ifndef S21_SND_H
#define S21_SND_H
#include <stdint.h>

#define S21_SND_HZ     2048000      /* 6809E: 49.152 MHz / 24 */
#define S21_SND_OUT_HZ 48000

/* snd: the 128 KB cy1-snd0.8j; voi[0..3]: the four 512 KB cy1-voi chips; dpram: the shared 0x800-byte DPRAM */
int  s21_snd_init(const uint8_t *snd, const uint8_t *voi[4], uint8_t *dpram);
void s21_snd_reset(int hold);   /* C148 ext1 bit0 = 0 -> hold the 6809 in reset; 1 -> run (reset on release) */
void s21_snd_run(int cycles);   /* advance by 6809 cycles (S21_SND_HZ per second); audio is generated as time passes */
int  s21_snd_mix(int16_t *out, int n);   /* take up to n stereo frames (interleaved L R); returns the frames given */
int  s21_snd_available(void);
uint64_t s21_snd_cycles(void);
/* DEV hooks */
void s21_snd_set_trace(void (*fn)(uint16_t pc, uint64_t cyc, void *u), void *u);
void s21_snd_set_dpram_write_hook(void (*fn)(uint16_t off, uint8_t v, void *u), void *u);
int  s21_snd_bad_op(void);
void s21_snd_clear_bad_op(void);
int  s21_snd_bank(void);                       /* the 0x0000-0x3FFF ROM bank */
int  s21_snd_trapped(uint16_t *pc);           /* the translation reached untranslated code (the CPU stopped there) */
const char *s21_snd_cpu_name(void);
/* environment: S21_SND_CPU=xlat (DEV library: run the translation), S21_SND_NOFF=1 (no polling-loop fast-forward),
   S21_6809_COV=<file> (merge the executed instruction addresses into <file>: tools/gen/cs_6809.cov) */
/* DEV environment replay (an instruction-exact gate against a MAME trace): device reads (0x4000..0x7FFF) return
   rd(addr, our value); the 6809's interrupts come only from s21_snd_force_firq(), called from the trace hook */
void s21_snd_set_replay(uint8_t (*rd)(uint16_t a, uint8_t ours, void *u), void *u);
void s21_snd_force_firq(uint16_t handler_pc);
void s21_snd_force_reset(void);
/* DEV: every YM2151 access, in the chip's clocks: kind 0 = write (off, value), 1 = read (off, value returned),
   2 = one output sample generated at that clock */
void s21_snd_set_ym_log(void (*fn)(uint64_t clk, int kind, int off, uint8_t v, void *u), void *u);   /* from the trace hook: restart the program instead of the next instruction */

#endif
