/* The YM2151 behind one interface, two implementations:
 *   s21_ym_opm.c -- OUR OWN chip model (opm.c): the game (cs21) and the default build of module D;
 *   s21_ym.cpp   -- Aaron Giles' ymfm (BSD-3), compiled from the reference tree, NOT copied: the DEV ORACLE only
 *                   (opm_gate, or module D's build with -DS21_OPM_ORACLE=ON). See cybsled/PLAN.md "Module F".
 * Time is the chip's own clock count (3.579545 MHz), supplied by the caller on every access. */
#ifndef S21_YM_H
#define S21_YM_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct s21_ym s21_ym;
s21_ym  *s21_ym_create(uint32_t clock);
void     s21_ym_destroy(s21_ym *y);
void     s21_ym_reset(s21_ym *y);
uint32_t s21_ym_sample_rate(s21_ym *y);                         /* clock / 64 */
void     s21_ym_write(s21_ym *y, uint64_t now, int off, uint8_t data);
uint8_t  s21_ym_read(s21_ym *y, uint64_t now, int off);
int      s21_ym_irq(s21_ym *y, uint64_t *edges, uint64_t *first);  /* DEV: the IRQ line, its rising edges, the first */
void     s21_ym_generate(s21_ym *y, uint64_t now, int32_t out[2]);   /* one sample; ints as ymfm emits (/32768 = 1.0) */

#ifdef __cplusplus
}
#endif
#endif
