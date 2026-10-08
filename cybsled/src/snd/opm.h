/* opm.h -- OUR OWN YM2151 (OPM), written for Cyber Sled's sound board from the chip's documented behaviour (the
 * Yamaha YM2151 data sheet / application manual and the public descriptions of the OPM's operator, envelope, phase,
 * LFO, noise, timer and DAC structure). It is a model of the chip, not an emulator port: no MAME / ymfm code was
 * read or translated. ymfm stays in the DEV tree only, as the oracle this is gated against (cybsled/PLAN.md,
 * "Module F").
 *
 * Time is the chip's own clock count (3.579545 MHz on System 21), supplied by the caller on every access, exactly
 * like the s21_ym interface it sits behind. One output sample = 64 clocks. */
#ifndef OPM_H
#define OPM_H
#include <stdint.h>

typedef struct {
	uint32_t phase;        /* 20-bit phase accumulator */
	uint16_t att;          /* 10-bit envelope attenuation (0 = loudest, 0x3FF = silent), 0.09375 dB per step */
	uint8_t  state;        /* OPM_EG_* */
	uint8_t  key;          /* the key-on bit as last written (register 0x08 / CSM) */
	uint8_t  keyed;        /* the key state the envelope has acted on */
	int16_t  out[2];       /* this operator's last two outputs (M1's self-feedback) */
} opm_op;

typedef struct opm {
	uint32_t clock;
	uint8_t  reg[256];
	uint8_t  addr;
	opm_op   op[32];       /* register order: slot = operator * 8 + channel, operator 0..3 = M1, M2, C1, C2 */
	/* envelope generator */
	uint32_t eg_div;       /* samples since the last envelope clock (the envelope runs every third sample) */
	uint32_t eg_cnt;       /* the global envelope counter */
	/* LFO */
	uint32_t lfo_acc;      /* frequency accumulator */
	uint8_t  lfo_phase;    /* 8-bit waveform position */
	uint8_t  lfo_rnd;      /* the noise waveform's current value */
	uint8_t  lfo_rnd0;     /* ... and its value at the last return to phase 0 (an LFO reset restores it) */
	int32_t  lfo_am;       /* current AM depth applied (0..255 after AMD) */
	int32_t  lfo_pm;       /* current PM (-128..127 after PMD) */
	uint8_t  amd, pmd;     /* register 0x19 holds either depth, selected by bit 7: the chip keeps both */
	/* noise generator */
	uint32_t noise_lfsr;   /* 17-bit */
	uint32_t noise_cnt;
	uint32_t noise_hist;   /* the last 32 bits the LFSR shifted out, newest in bit 0 (the LFO's noise waveform reads them) */
	uint8_t  noise_bit;
	/* timers: expiry in chip clocks (-1 = stopped), status flags, busy window */
	int64_t  tend[2];
	uint8_t  status;
	uint8_t  csm;          /* a CSM key-on pulse is pending for the next sample */
	uint64_t now, busy_end;
	uint64_t irq_edges, irq_first;   /* DEV statistics: IRQ line rising edges, the clock of the first */
	int      irq;
	void   (*irq_cb)(void *ctx, int state);
	void    *irq_ctx;
	/* DEV statistics: timer overflows, with the clock of the first of each */
	uint64_t tfire[2], tfirst[2];
} opm_t;

void     opm_init(opm_t *o, uint32_t clock);
void     opm_reset(opm_t *o);
uint32_t opm_sample_rate(const opm_t *o);                         /* clock / 64 */
void     opm_write(opm_t *o, uint64_t now, int off, uint8_t data);  /* off 0 = address, 1 = data */
uint8_t  opm_read(opm_t *o, uint64_t now, int off);                 /* the status register (either offset) */
void     opm_generate(opm_t *o, uint64_t now, int32_t out[2]);      /* one sample: L, R as the DAC sees them (/32768 = 1.0) */
void     opm_set_irq(opm_t *o, void (*cb)(void *ctx, int state), void *ctx);
void     opm_advance(opm_t *o, uint64_t now);                       /* bring the timers up to 'now' */

#endif
