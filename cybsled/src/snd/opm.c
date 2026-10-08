




/* opm.c -- our own YM2151 (OPM). See opm.h for what this is and is not.
 *
 * The chip, as modelled here (one output sample per 64 input clocks):
 *   32 operators = 8 channels x {M1, M2, C1, C2}; register order slot = operator * 8 + channel.
 *   Phase:    KC (octave, note) + KF (1/64 semitone) + LFO PM + DT2 -> a position on a 768-step-per-octave scale ->
 *             frequency number x 2^octave, DT1 added, times MUL (0 = 1/2) -> a 20-bit phase accumulator.
 *   Operator: 10 phase bits + modulation -> quarter-wave log-sine ROM (attenuation, 4.8 format) + envelope -> the
 *             exponent ROM -> a 13-bit magnitude and a sign: a 14-bit output.
 *   Envelope: attack / decay (D1R) / sustain (D2R) / release (RR), rates 0..63 with key scaling, clocked every 3rd
 *             sample from a global counter; 10-bit attenuation, D1L sustain level, TL, LFO AM.
 *   Channels: 8 algorithms, M1 self-feedback, L / R enables; noise replaces C2 of channel 8.
 *   Output:   the channels summed, limited to 16 bits and passed through the YM3012's 10-bit-mantissa / 3-bit-exponent
 *             floating-point format.
 *   Timers:   A (10 bits, 64 clocks per count), B (8 bits, 1024 clocks per count), flags, IRQ line, CSM. */
#include <math.h>
#include <string.h>
#include "opm.h"

enum { OPM_EG_ATTACK, OPM_EG_DECAY, OPM_EG_SUSTAIN, OPM_EG_RELEASE };

static uint16_t logsin[256];    /* -log2(sin) of a quarter wave, 4.8 fixed point */
static uint16_t expo[256];      /* 2^x fractional part, 10 bits */
static uint16_t fnum[768];      /* frequency numbers for one octave (with one fraction bit), 64 steps per semitone, from C# */
static int tables_ready;

/* the DT1 detune offsets by key code (0..31), DT1 = 1..3 (the data sheet's detune table, every entry confirmed by
   measurement: opm_gate dt1) */
static const uint8_t dt1_tab[3][32] = {
	{ 0,0,0,0,1,1,1,1, 1,1,1,1,2,2,2,2, 2,3,3,3,4,4,4,5, 5,6,6,7,8,8,8,8 },
	{ 1,1,1,1,2,2,2,2, 2,3,3,3,4,4,4,5, 5,6,6,7,8,8,9,10, 11,12,13,14,16,16,16,16 },
	{ 2,2,2,2,2,3,3,3, 4,4,4,5,5,6,6,7, 8,8,9,10,11,12,13,14, 16,17,19,20,22,22,22,22 },
};
/* DT2: +0, +600, +781, +950 cents, in 1/64-semitone steps */
static const uint16_t dt2_tab[4] = { 0, 384, 500, 608 };

/* envelope increments: for rates below 48 one of four 8-step patterns (by rate & 3) applied every 2^(11 - rate/4)
   envelope clocks of a 12-bit global counter; from 48 up the pattern runs every clock with growing step sizes */
static const uint8_t eg_pat[4][8] = {
	{ 0,1,0,1,0,1,0,1 }, { 0,1,0,1,1,1,0,1 }, { 0,1,1,1,0,1,1,1 }, { 0,1,1,1,1,1,1,1 },
};
/* rates 48..59: step 2^((rate - 48) / 4), doubled on the marked clocks (0, 2, 4 or 6 of 8 by rate & 3) */
static const uint8_t eg_pat_hi[4][8] = {
	{ 0,0,0,0,0,0,0,0 }, { 0,0,0,1,0,0,0,1 }, { 0,1,0,1,0,1,0,1 }, { 0,1,1,1,0,1,1,1 },
};

/* the 16-step interpolation inside each quarter-semitone group of the frequency ROM, and which pattern each of the
   48 groups of an octave uses (measured; see build_tables) */
static const uint8_t opm_fine[15][16] = {
	{ 0, 1, 2, 3, 4, 5, 6, 7, 9, 10, 11, 12, 14, 15, 16, 17 },
	{ 0, 1, 2, 3, 5, 6, 7, 8, 10, 11, 12, 13, 15, 16, 17, 18 },
	{ 0, 1, 2, 3, 5, 6, 7, 8, 10, 11, 13, 14, 15, 16, 18, 19 },
	{ 0, 1, 2, 3, 5, 6, 8, 9, 11, 12, 13, 14, 16, 17, 19, 20 },
	{ 0, 1, 2, 3, 5, 6, 8, 9, 11, 12, 14, 15, 17, 18, 19, 20 },
	{ 0, 1, 3, 4, 6, 7, 9, 10, 12, 13, 15, 16, 18, 19, 21, 22 },
	{ 0, 1, 3, 4, 6, 7, 9, 10, 12, 14, 15, 17, 18, 20, 21, 23 },
	{ 0, 1, 3, 4, 6, 8, 9, 11, 13, 14, 16, 17, 19, 21, 22, 24 },
	{ 0, 1, 3, 4, 6, 8, 9, 11, 13, 15, 16, 18, 20, 21, 23, 24 },
	{ 0, 1, 3, 5, 7, 8, 10, 12, 14, 15, 17, 19, 21, 22, 24, 26 },
	{ 0, 1, 3, 5, 7, 8, 10, 12, 14, 16, 18, 19, 21, 23, 25, 26 },
	{ 0, 1, 3, 5, 7, 9, 11, 12, 15, 16, 18, 20, 22, 24, 26, 27 },
	{ 0, 1, 3, 5, 7, 9, 11, 12, 15, 17, 19, 20, 23, 24, 26, 28 },
	{ 0, 2, 4, 6, 7, 10, 11, 14, 16, 18, 20, 22, 23, 26, 27, 30 },
	{ 0, 2, 4, 6, 7, 10, 11, 14, 16, 18, 20, 22, 25, 28, 29, 32 },
};
static const uint8_t opm_fine_id[48] = {
	0, 0, 0, 1, 1, 1, 2, 1, 2, 2, 3, 3, 3, 3, 4, 4, 4, 5, 5, 5, 6, 6, 7, 7,
	7, 8, 8, 9, 9, 9, 10, 11, 11, 11, 12, 12, 12, 13, 13, 13, 13, 13, 14, 14, 14, 14, 14, 14
};

static void build_tables(void)
{
	if (tables_ready) return;
	for (int i = 0; i < 256; i++) {
		double s = sin((2.0 * i + 1.0) * M_PI / 1024.0);
		logsin[i] = (uint16_t)lround(-log2(s) * 256.0);
		expo[i] = (uint16_t)lround((pow(2.0, i / 256.0) - 1.0) * 1024.0);
	}
	/* the frequency numbers: one octave = 12 semitones x 4 quarter-semitone groups x 16 KF fine steps. Each group starts
	   at floor(K x 2^(group / 48)) -- equal temperament, K = 1299.32 (C# with one fraction bit, octave 0 being this
	   >> 2) -- and the 16 fine steps inside a group follow one of 15 interpolation patterns (opm_fine). The chip
	   holds these as ROM; the base formula and the patterns were measured from the chip's output (opm_gate freq), the
	   data sheet giving only the equal-tempered pitch (A4 = 440 Hz at KC 0x4A). */
	for (int g = 0; g < 48; g++)
		for (int j = 0; j < 16; j++)
			fnum[g * 16 + j] = (uint16_t)(floor(1299.32 * pow(2.0, g / 48.0)) + opm_fine[opm_fine_id[g]][j]);
	tables_ready = 1;
}

uint32_t opm_sample_rate(const opm_t *o) { return o->clock / 64; }

void opm_reset(opm_t *o)
{
	uint32_t clk = o->clock;
	void (*cb)(void *, int) = o->irq_cb; void *ctx = o->irq_ctx;
	memset(o, 0, sizeof *o);
	o->clock = clk; o->irq_cb = cb; o->irq_ctx = ctx;
	for (int i = 0; i < 32; i++) { o->op[i].att = 0x3ff; o->op[i].state = OPM_EG_RELEASE; }
	o->noise_lfsr = 0xffff;   /* the state out of reset (measured: sixteen 1s, then the x^17 + x^14 + 1 sequence) */
	o->noise_bit = 1;
	o->noise_cnt = 0;
	o->tend[0] = o->tend[1] = -1;
	for (int ch = 0; ch < 8; ch++) o->reg[0x20 + ch] = 0xc0;   /* both outputs enabled out of reset */
}

void opm_init(opm_t *o, uint32_t clock)
{
	build_tables();
	memset(o, 0, sizeof *o);
	o->clock = clock;
	opm_reset(o);
}

void opm_set_irq(opm_t *o, void (*cb)(void *ctx, int state), void *ctx) { o->irq_cb = cb; o->irq_ctx = ctx; }

/* ---------------------------------------------------------------- timers */
static uint64_t timer_period(const opm_t *o, int t)
{
	if (t == 0) return 64ull * (1024 - ((o->reg[0x10] << 2) | (o->reg[0x11] & 3)));
	return 1024ull * (256 - o->reg[0x12]);
}

static void update_irq(opm_t *o)
{
	int line = (o->status & 3) != 0;
	if (line != o->irq) {
		o->irq = line;
		if (line && !o->irq_edges++) o->irq_first = o->now;
		if (o->irq_cb) o->irq_cb(o->irq_ctx, line);
	}
}

static void timer_fire(opm_t *o, int t)
{
	uint8_t ctl = o->reg[0x14];
	if (!o->tfire[t]++) o->tfirst[t] = o->now;
	if (ctl & (4 << t)) { o->status |= (uint8_t)(1 << t); update_irq(o); }
	if (t == 0 && (ctl & 0x80)) o->csm = 1;      /* CSM: timer A keys every operator on for one sample (data sheet: on, then off) */
	o->tend[t] = (int64_t)(o->now + timer_period(o, t));
}

void opm_advance(opm_t *o, uint64_t until)
{
	for (;;) {
		int t = -1;
		for (int i = 0; i < 2; i++)
			if (o->tend[i] >= 0 && (uint64_t)o->tend[i] <= until && (t < 0 || o->tend[i] < o->tend[t])) t = i;
		if (t < 0) break;
		o->now = (uint64_t)o->tend[t];
		timer_fire(o, t);
	}
	o->now = until;
}

/* ---------------------------------------------------------------- registers */
static void key_slot(opm_t *o, int slot, int on) { o->op[slot].key = (uint8_t)on; }

static void write_reg(opm_t *o, uint8_t a, uint8_t v)
{
	uint8_t old = o->reg[a];
	o->reg[a] = v;
	switch (a) {
	case 0x01:                                    /* test: bit 1 holds the LFO in reset */
		if (v & 2) { o->lfo_phase = 0; o->lfo_acc = 0; o->lfo_rnd = o->lfo_rnd0; }   /* back to the noise value of phase 0 */
		break;
	case 0x19:                                    /* AMD (bit 7 = 0) or PMD (bit 7 = 1) */
		if (v & 0x80) o->pmd = v & 0x7f; else o->amd = v & 0x7f;
		break;
	case 0x08: {                                  /* key on: channel in bits 0-2, M1 / C1 / M2 / C2 in bits 3-6 */
		int ch = v & 7;
		key_slot(o, 0 * 8 + ch, (v >> 3) & 1);    /* M1 */
		key_slot(o, 2 * 8 + ch, (v >> 4) & 1);    /* C1 */
		key_slot(o, 1 * 8 + ch, (v >> 5) & 1);    /* M2 */
		key_slot(o, 3 * 8 + ch, (v >> 6) & 1);    /* C2 */
		break;
	}
	case 0x14:                                    /* timer control */
		for (int t = 0; t < 2; t++) {
			if ((v & (1 << t)) && !(old & (1 << t))) o->tend[t] = (int64_t)(o->now + timer_period(o, t));
			else if (!(v & (1 << t))) o->tend[t] = -1;
		}
		if (v & 0x10) o->status &= (uint8_t)~1;
		if (v & 0x20) o->status &= (uint8_t)~2;
		update_irq(o);
		break;
	default:
		break;
	}
}

void opm_write(opm_t *o, uint64_t now, int off, uint8_t data)
{
	opm_advance(o, now);
	if (!(off & 1)) { o->addr = data; return; }
	write_reg(o, o->addr, data);
	o->busy_end = now + 64;
}

uint8_t opm_read(opm_t *o, uint64_t now, int off)
{
	(void)off;
	opm_advance(o, now);
	return (uint8_t)(o->status | (now < o->busy_end ? 0x80 : 0));
}

/* ---------------------------------------------------------------- one sample */
static int keycode(const opm_t *o, int ch) { return (o->reg[0x28 + ch] >> 2) & 31; }

static uint32_t phase_step(const opm_t *o, int slot)
{
	int ch = slot & 7;
	uint8_t r40 = o->reg[0x40 + slot], rc0 = o->reg[0xc0 + slot];
	int kc = o->reg[0x28 + ch] & 0x7f, kf = o->reg[0x30 + ch] >> 2;
	int note = kc & 15;
	int lin = (kc >> 4) * 768 + (note - (note >> 2)) * 64 + kf;
	int pms = (o->reg[0x38 + ch] >> 4) & 7;
	if (pms) {
		int pm = o->lfo_pm;
		static const int8_t sh[8] = { 0, -5, -4, -3, -2, -1, 1, 2 };
		pm = sh[pms] < 0 ? pm >> -sh[pms] : pm << sh[pms];
		lin += pm;
	}
	lin += dt2_tab[rc0 >> 6];
	if (lin < 0) lin = 0;
	if (lin > 8 * 768 - 1) lin = 8 * 768 - 1;
	int oct = lin / 768;
	uint32_t f = ((uint32_t)fnum[lin % 768] << oct) >> 2;
	int dt1 = (r40 >> 4) & 7;
	if (dt1 & 3) {
		int d = dt1_tab[(dt1 & 3) - 1][keycode(o, ch)];
		f = (dt1 & 4) ? f - d : f + d;
	}
	f &= 0x1ffff;
	int mul = r40 & 15;
	return mul ? f * mul : f >> 1;
}

static int eg_rate(const opm_t *o, int slot, int state)
{
	int r;
	switch (state) {
	case OPM_EG_ATTACK:  r = o->reg[0x80 + slot] & 31; break;
	case OPM_EG_DECAY:   r = o->reg[0xa0 + slot] & 31; break;
	case OPM_EG_SUSTAIN: r = o->reg[0xc0 + slot] & 31; break;
	default:             r = ((o->reg[0xe0 + slot] & 15) << 1) | 1; break;
	}
	if (!r) return 0;
	int ks = o->reg[0x80 + slot] >> 6;
	r = 2 * r + (keycode(o, slot & 7) >> (3 - ks));
	return r > 63 ? 63 : r;
}

static void eg_keys(opm_t *o)
{
	for (int s = 0; s < 32; s++) {
		opm_op *p = &o->op[s];
		int key = p->key | o->csm;
		if (key && !p->keyed) {
			p->state = OPM_EG_ATTACK;
			p->phase = 0;
			if (eg_rate(o, s, OPM_EG_ATTACK) >= 62) p->att = 0;
		} else if (!key && p->keyed)
			p->state = OPM_EG_RELEASE;
		p->keyed = (uint8_t)key;
	}
	o->csm = 0;
}

static void eg_clock(opm_t *o)
{
	uint32_t c = o->eg_cnt;
	for (int s = 0; s < 32; s++) {
		opm_op *p = &o->op[s];
		if (p->state == OPM_EG_ATTACK && p->att == 0) p->state = OPM_EG_DECAY;
		if (p->state == OPM_EG_DECAY) {
			int d1l = o->reg[0xe0 + s] >> 4;
			int sl = d1l == 15 ? 0x3e0 : d1l << 5;
			if (p->att >= sl) p->state = OPM_EG_SUSTAIN;
		}
		int rate = eg_rate(o, s, p->state);
		int inc;
		if (rate == 0) inc = 0;
		else if (rate < 48) {
			int sh = 11 - (rate >> 2);
			if (c & ((1u << sh) - 1)) continue;
			inc = eg_pat[rate & 3][(c >> sh) & 7];
		} else {
			int base = 1 << ((rate - 48) >> 2);
			inc = base << eg_pat_hi[rate & 3][c & 7];
			if (rate >= 60) inc = 8;
		}
		if (!inc) continue;
		if (p->state == OPM_EG_ATTACK) {
			if (rate >= 62) continue;
			int a = p->att;
			a += (~a * inc) >> 4;
			p->att = (uint16_t)(a < 0 ? 0 : a);
		} else {
			int a = p->att + inc;
			p->att = (uint16_t)(a > 0x3ff ? 0x3ff : a);
		}
	}
}

static void lfo_clock(opm_t *o)
{
	uint8_t f = o->reg[0x18];
	uint32_t step = (16u + (f & 15)) << (f >> 4);
	uint32_t prev = o->lfo_acc;
	if (!(o->reg[0x01] & 2)) o->lfo_acc += step;
	if ((o->lfo_acc ^ prev) & ~((1u << 22) - 1)) {   /* the waveform advances every 2^22 of the accumulator */
		o->lfo_phase = (uint8_t)(o->lfo_acc >> 22);
		o->lfo_rnd = (uint8_t)~o->noise_hist;   /* the last 8 noise bits, inverted, oldest as the MSB */
		if (!o->lfo_phase) o->lfo_rnd0 = o->lfo_rnd;
	}
	int p = o->lfo_phase, am, pm;
	switch (o->reg[0x1b] & 3) {
	case 0:  am = 255 - p; pm = p < 128 ? p : p - 256; break;             /* saw */
	case 1:  am = p < 128 ? 255 : 0; pm = p < 128 ? 127 : -128; break;                             /* square */
	case 2:  am = p < 128 ? 254 - 2 * p : 2 * p - 256;                                             /* triangle */
	         pm = p < 64 ? 2 * p + 1 : p < 128 ? 254 - 2 * p : p < 192 ? 255 - 2 * p : 2 * p - 512; break;
	default: am = o->lfo_rnd; pm = (int8_t)o->lfo_rnd; break;                                      /* noise */
	}
	o->lfo_am = am * o->amd >> 7;
	o->lfo_pm = pm * o->pmd >> 7;
}

/* the noise generator: a 17-bit LFSR (x^17 + x^14 + 1) stepping every half sample (32 clocks); its output bit is
   latched every 32 - NFRQ half samples. The output of a sample uses the latch as of its first half. */
static void noise_half(opm_t *o)
{
	if (++o->noise_cnt >= 32u - (o->reg[0x0f] & 31)) { o->noise_cnt = 0; o->noise_bit = (uint8_t)(o->noise_lfsr & 1); }
	uint32_t r = o->noise_lfsr;
	o->noise_hist = (o->noise_hist << 1) | (r & 1);
	o->noise_lfsr = (r >> 1) | (((r ^ (r >> 3)) & 1) << 16);
}

static int32_t op_calc(uint32_t phase10, uint32_t env)
{
	uint32_t i = phase10 & 0xff;
	if (phase10 & 0x100) i ^= 0xff;
	uint32_t a = logsin[i] + (env << 2);
	uint32_t sh = a >> 8;
	int32_t v = sh >= 16 ? 0 : (int32_t)((((uint32_t)expo[~a & 0xff] | 0x400) << 2) >> sh);
	return (phase10 & 0x200) ? -v : v;
}

static uint32_t op_env(const opm_t *o, int slot)
{
	uint32_t e = o->op[slot].att + ((o->reg[0x60 + slot] & 0x7f) << 3);
	int ams = o->reg[0x38 + (slot & 7)] & 3;
	if (ams && (o->reg[0xa0 + slot] & 0x80)) e += (uint32_t)o->lfo_am << (ams - 1);
	return e > 0x3ff ? 0x3ff : e;
}

static int32_t channel_out(opm_t *o, int ch)
{
	opm_op *m1 = &o->op[ch], *m2 = &o->op[8 + ch], *c1 = &o->op[16 + ch], *c2 = &o->op[24 + ch];
	uint8_t r20 = o->reg[0x20 + ch];
	int fb = (r20 >> 3) & 7, alg = r20 & 7;
	int32_t mod = fb ? (m1->out[0] + m1->out[1]) >> (10 - fb) : 0;
	int32_t om1 = op_calc((m1->phase >> 10) + (uint32_t)mod, op_env(o, ch));
	m1->out[1] = m1->out[0]; m1->out[0] = (int16_t)om1;
	int32_t oc1, om2, oc2, out;
	uint32_t e_c1 = op_env(o, 16 + ch), e_m2 = op_env(o, 8 + ch), e_c2 = op_env(o, 24 + ch);
#define OP(p, m, e) op_calc(((p)->phase >> 10) + (uint32_t)((m) >> 1), (e))
	switch (alg) {
	case 0: oc1 = OP(c1, om1, e_c1); om2 = OP(m2, oc1, e_m2); oc2 = OP(c2, om2, e_c2); out = oc2; break;
	case 1: oc1 = OP(c1, 0, e_c1); om2 = OP(m2, om1 + oc1, e_m2); oc2 = OP(c2, om2, e_c2); out = oc2; break;
	case 2: oc1 = OP(c1, 0, e_c1); om2 = OP(m2, oc1, e_m2); oc2 = OP(c2, om1 + om2, e_c2); out = oc2; break;
	case 3: oc1 = OP(c1, om1, e_c1); om2 = OP(m2, 0, e_m2); oc2 = OP(c2, oc1 + om2, e_c2); out = oc2; break;
	case 4: oc1 = OP(c1, om1, e_c1); om2 = OP(m2, 0, e_m2); oc2 = OP(c2, om2, e_c2); out = oc1 + oc2; break;
	case 5: oc1 = OP(c1, om1, e_c1); om2 = OP(m2, om1, e_m2); oc2 = OP(c2, om1, e_c2); out = oc1 + om2 + oc2; break;
	case 6: oc1 = OP(c1, om1, e_c1); om2 = OP(m2, 0, e_m2); oc2 = OP(c2, 0, e_c2); out = oc1 + om2 + oc2; break;
	default: oc1 = OP(c1, 0, e_c1); om2 = OP(m2, 0, e_m2); oc2 = OP(c2, 0, e_c2); out = om1 + oc1 + om2 + oc2; break;
	}
#undef OP
	if (ch == 7 && (o->reg[0x0f] & 0x80)) {       /* noise replaces C2 of channel 8 */
		/* the noise "operator" is linear in the envelope: (1023 - attenuation) x 2, signed by the noise bit */
		int32_t n = (int32_t)((~e_c2 & 0x3ff) << 1);
		n = o->noise_bit ? n : -n;
		out += n - oc2;
	}
	return out;
}

static int32_t dac(int32_t v)
{
	if (v > 32767) v = 32767;
	if (v < -32768) v = -32768;
	int sh = 0;
	while (sh < 7 && ((v >> sh) > 511 || (v >> sh) < -512)) sh++;
	return (v >> sh) << sh;
}

void opm_generate(opm_t *o, uint64_t now, int32_t out[2])
{
	opm_advance(o, now);
	lfo_clock(o);
	noise_half(o);             /* the second half of the previous sample (it sees writes made since) */
	noise_half(o);             /* the first half of this one */
	eg_keys(o);
	if (++o->eg_div >= 3) { o->eg_div = 0; o->eg_cnt = (o->eg_cnt + 1) & 0xfff; eg_clock(o); }
	for (int s = 0; s < 32; s++) o->op[s].phase = (o->op[s].phase + phase_step(o, s)) & 0xfffff;
	int32_t l = 0, r = 0;
	for (int ch = 0; ch < 8; ch++) {
		int32_t v = channel_out(o, ch);
		uint8_t r20 = o->reg[0x20 + ch];
		if (r20 & 0x40) l += v;
		if (r20 & 0x80) r += v;
	}
	out[0] = dac(l);
	out[1] = dac(r);
}

/* DEV: the phase step the model would add for a slot now (opm_gate's frequency check) */
uint32_t opm_dev_phase_step(opm_t *o, int slot);
uint32_t opm_dev_phase_step(opm_t *o, int slot) { return phase_step(o, slot); }
