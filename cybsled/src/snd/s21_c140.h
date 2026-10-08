/* Namco C140 PCM chip (24 voices) -- our own code reproducing the chip's behaviour (as documented by MAME's c140
 * device, a reference only). Output: one stereo sample per call at the chip's base rate (clock = 21333 Hz here). */
#ifndef S21_C140_H
#define S21_C140_H
#include <stdint.h>

typedef struct s21_c140_voice {
	int key, ptoffset, pos;
	int32_t lastdt, prevdt, dltdt;
	int bank, mode, start, end, loop;
} s21_c140_voice;

typedef struct s21_c140 {
	uint8_t reg[0x200];
	s21_c140_voice v[24];
	int16_t pcmtbl[256];
	uint16_t (*read_word)(void *ctx, uint32_t word_addr);   /* the sample ROM, a 16-bit bus */
	void *ctx;
	/* INT1 timer, reported to the host: (re)armed with a period in base-rate ticks, or stopped (-1) */
	void (*int1)(void *ctx, int state);
	void (*timer)(void *ctx, int ticks);
} s21_c140;

void    s21_c140_init(s21_c140 *c);
uint8_t s21_c140_read(s21_c140 *c, uint16_t off);
void    s21_c140_write(s21_c140 *c, uint16_t off, uint8_t data);
void    s21_c140_timer_fired(s21_c140 *c);                        /* the host's timer expired */
void    s21_c140_sample(s21_c140 *c, int32_t *left, int32_t *right);   /* clamped to +-4096 */

#endif
