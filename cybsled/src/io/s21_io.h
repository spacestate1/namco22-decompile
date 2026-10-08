/* Namco System 21 C68 I/O MCU (M37450 running c68.bin) with the cabinet inputs -- module D of cybsled/DESIGN.md.
 * The program runs TRANSLATED (gen/cs_c68.c from tools/gen/m740_translate.py, PLAN.md "Module K"); the DEV library
 * s21_io_dev adds the interpreter oracle (m37450.c): env S21_C68=xlat|oracle|lockstep, S21_C68_COV, S21_C68_BUSHASH. */
#ifndef S21_IO_H
#define S21_IO_H
#include <stdint.h>

#define S21_IO_HZ (49152000 / 6 / 4)   /* M37450 bus cycles per second: 8.192 MHz / 4 = 2.048 MHz */

typedef struct s21_inputs {
	uint8_t mcub;      /* 0x80 START1, 0x40 START2 (active low) */
	uint8_t mcuc;      /* 0x20 COIN1, 0x10 COIN2, 0x40 Service2 (toggle), 0x80 Service1 (active low) */
	uint8_t mcuh;      /* 0x20 Gun, 0x08 Missile, 0x02 Viewport (active low) */
	uint8_t dsw;       /* 0x01 Service Mode (low = on), 0x20 PCM ROM (0 = 4M), 0x80 Screen Stop */
	uint8_t an[8];     /* AN0 right stick Y, AN1 left Y, AN2 right X, AN3 left X: centre 0x7F, range 0x3F..0xBF */
	uint8_t dial[4];   /* 0x3000..0x3003, unused on Cyber Sled (0xFF) */
} s21_inputs;

/* c68: the 32 KB c68.bin image; dpram: the 0x800-byte dual-port RAM shared with the 68000s (low byte lane) and the 6809 */
int  s21_io_init(const uint8_t *c68, uint8_t *dpram);
void s21_io_default_inputs(s21_inputs *in);
void s21_io_set_inputs(const s21_inputs *in);
void s21_io_get_inputs(s21_inputs *in);   /* what the cabinet holds this frame (online: the Start countdown, src/link/s21_net.c) */
void s21_io_reset(int hold);    /* C148 ext2 bit0 = 0 -> hold in reset (MAME reset_all_subcpus); 1 -> run (reset on release) */
void s21_io_vblank(void);       /* vblank: asserts the M37450 INT1 (acked by the program reading 0x6000) */
void s21_io_run(int cycles);    /* advance by M37450 bus cycles (S21_IO_HZ per second) */
uint64_t s21_io_cycles(void);
/* DEV hooks */
void s21_io_set_trace(void (*fn)(uint16_t pc, uint64_t cyc, void *u), void *u);
void s21_io_set_dpram_write_hook(void (*fn)(uint16_t off, uint8_t v, void *u), void *u);
/* DEV environment replay: device reads (0x00D6..0x00FF, 0x2000..0x6FFF) return rd(addr, our value); the M37450's
   interrupts come only from s21_io_force_irq(), called from the trace hook */
void s21_io_set_replay(uint8_t (*rd)(uint16_t a, uint8_t ours, void *u), void *u);
void s21_io_force_irq(uint16_t handler_pc);
void s21_io_force_reset(void);   /* from the trace hook: restart the program instead of the next instruction */
void s21_io_use_mode(const char *mode);   /* DEV (s21_io_dev only): "xlat" | "oracle" | "lockstep", before s21_io_init */

#endif
