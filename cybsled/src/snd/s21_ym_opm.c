/* The YM2151 behind the s21_ym interface: OUR OWN model (opm.c). This is what the game links. The ymfm oracle
 * (s21_ym.cpp) implements the same interface only in DEV builds (-DS21_OPM_ORACLE=ON) and in opm_gate. */
#include <stdlib.h>
#include "s21_ym.h"
#include "opm.h"

struct s21_ym { opm_t o; };

s21_ym  *s21_ym_create(uint32_t clock) { s21_ym *y = calloc(1, sizeof *y); opm_init(&y->o, clock); return y; }
void     s21_ym_destroy(s21_ym *y) { free(y); }
void     s21_ym_reset(s21_ym *y) { opm_reset(&y->o); }
uint32_t s21_ym_sample_rate(s21_ym *y) { return opm_sample_rate(&y->o); }
void     s21_ym_write(s21_ym *y, uint64_t now, int off, uint8_t data) { opm_write(&y->o, now, off, data); }
uint8_t  s21_ym_read(s21_ym *y, uint64_t now, int off) { return opm_read(&y->o, now, off); }
void     s21_ym_generate(s21_ym *y, uint64_t now, int32_t out[2]) { opm_generate(&y->o, now, out); }
int      s21_ym_irq(s21_ym *y, uint64_t *edges, uint64_t *first)
{
	if (edges) *edges = y->o.irq_edges;
	if (first) *first = y->o.irq_first;
	return y->o.irq;
}
