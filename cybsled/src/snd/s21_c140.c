/* Namco C140 -- see s21_c140.h */
#include <stdlib.h>
#include <string.h>
#include "s21_c140.h"

void s21_c140_init(s21_c140 *c)
{
	memset(c->reg, 0, sizeof c->reg);
	memset(c->v, 0, sizeof c->v);
	/* the 8-bit compressed format: 3-bit exponent, 5-bit mantissa, sign */
	for (int i = 0; i < 256; i++) {
		int j = (int8_t)i;
		int s1 = j & 7;
		int s2 = abs(j >> 3) & 31;
		int v = (0x80 << s1) & 0xff00;
		v += s2 << (s1 ? s1 + 3 : 4);
		if (j < 0) v = -v;
		c->pcmtbl[i] = (int16_t)v;
	}
}

uint8_t s21_c140_read(s21_c140 *c, uint16_t off)
{
	off &= 0x1ff;
	uint8_t d = c->reg[off];
	if ((off & 0xf) == 0x5 && off < 0x180)
		d = (uint8_t)((c->v[off >> 4].key ? 0x40 : 0) | (c->reg[off] & 0x3f));   /* key-on status */
	else if (off == 0x1f8)
		d++;                                                                      /* timer reload + 1 */
	return d;
}

void s21_c140_write(s21_c140 *c, uint16_t off, uint8_t data)
{
	off &= 0x1ff;
	c->reg[off] = data;
	if (off < 0x180) {
		if ((off & 0xf) == 0x5) {
			s21_c140_voice *v = &c->v[off >> 4];
			const uint8_t *r = &c->reg[off & 0x1f0];
			if ((data & 0x80) || ((data & 0x40) && v->key)) {
				v->key = 1;
				v->ptoffset = 0; v->pos = 0;
				v->lastdt = v->prevdt = v->dltdt = 0;
				v->bank = r[4];
				v->mode = data;
				v->start = r[6] << 8 | r[7];
				v->end = r[8] << 8 | r[9];
				v->loop = r[10] << 8 | r[11];
			} else
				v->key = 0;
		}
		return;
	}
	switch (off) {
	case 0x1fa:                                   /* restart the INT1 timer */
		c->int1(c->ctx, 0);
		if (c->reg[0x1fe] & 1) c->timer(c->ctx, (c->reg[0x1f8] + 1) * 2);
		break;
	case 0x1fe:                                   /* enable / disable INT1 */
		if (data & 1)
			c->timer(c->ctx, -2);                 /* host: assert at once if the timer is idle */
		else {
			c->int1(c->ctx, 0);
			c->timer(c->ctx, -1);
		}
		break;
	}
}

void s21_c140_timer_fired(s21_c140 *c) { c->int1(c->ctx, 1); }

void s21_c140_sample(s21_c140 *c, int32_t *left, int32_t *right)
{
	int32_t lm = 0, rm = 0;
	for (int i = 0; i < 24; i++) {
		s21_c140_voice *v = &c->v[i];
		if (!v->key) continue;
		const uint8_t *r = &c->reg[i * 16];
		int freq = r[2] << 8 | r[3];
		if (!freq) continue;
		int delta = freq * 2;                     /* base rate * 2 / sample rate, output at the base rate */
		int lvol = r[1] * 32 / 24, rvol = r[0] * 32 / 24;
		int sz = v->end - v->start;
		int base = (v->bank << 16) + v->start;
		v->ptoffset += delta;
		int cnt = (v->ptoffset >> 16) & 0x7fff;
		v->ptoffset &= 0xffff;
		v->pos += cnt;
		if (v->pos >= sz) {
			if (v->mode & 0x10) v->pos = v->loop - v->start;
			else { v->key = 0; continue; }
		}
		if (cnt) {
			uint16_t s = c->read_word(c->ctx, (uint32_t)(base + v->pos)) & 0xfff0;
			v->prevdt = v->lastdt;
			v->lastdt = ((v->mode & 0x08) ? c->pcmtbl[(s >> 8) & 0xff] : (int16_t)s) >> 4;
			v->dltdt = v->lastdt - v->prevdt;
		}
		int32_t dt = ((v->dltdt * v->ptoffset) >> 16) + v->prevdt;
		lm += (dt * lvol) >> 9;
		rm += (dt * rvol) >> 9;
	}
	/* MAME accumulates into s16 buffers before clamping; keep that wrap */
	lm = (int16_t)lm; rm = (int16_t)rm;
	*left = lm > 4096 ? 4096 : lm < -4096 ? -4096 : lm;
	*right = rm > 4096 ? 4096 : rm < -4096 ? -4096 : rm;
}
