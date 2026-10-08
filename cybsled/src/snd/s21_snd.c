/* Namco System 21 sound board -- see s21_snd.h. Memory map and wiring reproduce MAME's namcos21_c67 (reference only). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "s21_snd.h"
#include "m6809_sem.h"
#include "s21_c140.h"
#include "s21_ym.h"
#include "../io/s21_sched.h"

#define C140_HZ   21333                  /* 49.152 MHz / 384 / 6, as an integer device clock */
#define YM_CLOCK  3579545
#define RING      (1 << 16)

/* THE 6809: the program TRANSLATED to C at build time (gen/cs_6809.c, tools/gen/m6809_translate.py; module J of
   cybsled/PLAN.md) -- the game library s21_snd has nothing else. The DEV library s21_snd_dev (-DS21_SND_DEV) also
   carries the interpreter m6809.c, the oracle, and runs it by default; S21_SND_CPU=xlat selects the translation there. */
int cs_6809_step(m6809_t *c);
extern const uint16_t cs_6809_spin[];
extern const int cs_6809_nspin;
static int (*cpu_step)(m6809_t *c) = cs_6809_step;
static int ff_on;                         /* the exact polling-loop fast-forward (translation only; S21_SND_NOFF=1: off) */
static int ff_mon, ff_impure;             /* during a probe pass: did the pass touch anything but plain memory? */
static uint8_t *cov_map;                  /* DEV S21_6809_COV=<file>: executed instruction addresses, [bank 0..7 | 8 = fixed][pc] */
static const char *cov_path;

static struct {
	m6809_t cpu;
	s21_c140 c140;
	s21_ym *ym;
	const uint8_t *rom;
	const uint8_t *voi[4];
	uint8_t *dp;
	uint8_t ram[0x2000];
	int bank;
	int held;
	uint64_t cyc;                         /* board time in 6809 cycles (runs while the CPU is held, like MAME's chips) */
	/* C140 INT1 timer: due cycle, -1 = idle */
	int64_t c140_due;
	/* audio generation: next sample times in 6809 cycles (double) */
	double c140_next, ym_next, out_next;
	double c140_t[2], ym_t[2];
	int32_t c140_l[2], c140_r[2], ym_l[2], ym_r[2];
	int16_t ring[RING * 2];
	unsigned rd_i, wr_i;
	void (*trace)(uint16_t, uint64_t, void *); void *trace_u;
	void (*dpw)(uint16_t, uint8_t, void *); void *dpw_u;
	uint8_t (*rp)(uint16_t, uint8_t, void *); void *rp_u;
	int pending_reset;
	void (*ymlog)(uint64_t clk, int kind, int off, uint8_t v, void *u); void *ymlog_u;   /* DEV: every YM2151 access */
} sb;

static uint64_t ym_now(void) { return (uint64_t)((double)sb.cyc * YM_CLOCK / S21_SND_HZ); }

static uint16_t c140_rom(void *ctx, uint32_t w)
{
	(void)ctx;
	/* System 21: ((offset & 0x300000) >> 1) | (offset & 0x7ffff) into a 16-bit BE region whose even (high) bytes
	   hold voi0..3 at word 0, 0x80000, 0x100000, 0x180000 -> chip = bits 20-21, the byte in the high lane */
	w &= 0x3fffff;
	const uint8_t *chip = sb.voi[(w >> 20) & 3];
	return (uint16_t)(chip[w & 0x7ffff] << 8);
}
static void c140_int1(void *ctx, int state) { (void)ctx; sb.cpu.firq_line = state; }
static void c140_timer(void *ctx, int ticks)
{
	(void)ctx;
	if (ticks == -1) sb.c140_due = -1;
	else if (ticks == -2) { if (sb.c140_due < 0) sb.cpu.firq_line = 1; }   /* enable: the first interrupt at once */
	else sb.c140_due = (int64_t)s21_sched_slice_start(sb.cyc) + ((int64_t)ticks * S21_SND_HZ + C140_HZ - 1) / C140_HZ;
}

static uint8_t dev_rd(uint16_t a);
static uint8_t rd(void *ctx, uint16_t a)
{
	(void)ctx;
	if (a >= 0x4000 && a < 0x8000) {
		if (ff_mon && a < 0x7000) ff_impure = 1;              /* a chip read: its value moves with time */
		uint8_t v = dev_rd(a); return sb.rp ? sb.rp(a, v, sb.rp_u) : v;
	}
	if (a < 0x4000) return sb.rom[(sb.bank % 8) * 0x4000 + a];
	if (a >= 0xc000) return sb.rom[a - 0xc000];
	if (a >= 0x8000 && a < 0xa000) return sb.ram[a - 0x8000];
	return 0;
}
static uint8_t dev_rd(uint16_t a)
{
	if (a < 0x4000) return sb.rom[(sb.bank % 8) * 0x4000 + a];
	if (a >= 0xc000) return sb.rom[a - 0xc000];
	if (a >= 0x8000 && a < 0xa000) return sb.ram[a - 0x8000];
	if (a >= 0x7000 && a < 0x8000) return sb.dp[a & 0x7ff];
	if (a >= 0x5000 && a < 0x6000) return s21_c140_read(&sb.c140, a & 0x1ff);
	if (a == 0x4000 || a == 0x4001) {
		uint64_t c = ym_now(); uint8_t v = s21_ym_read(sb.ym, c, a & 1);
		if (sb.ymlog) sb.ymlog(c, 1, a & 1, v, sb.ymlog_u);
		return v;
	}
	return 0;
}

static void wr(void *ctx, uint16_t a, uint8_t v)
{
	(void)ctx;
	if (ff_mon) {                                             /* a probe pass: only no-op writes keep it a pure loop */
		if (a >= 0x8000 && a < 0xa000) { if (sb.ram[a - 0x8000] != v) ff_impure = 1; }
		else if ((a >= 0x4000 && a < 0x6000) || (a >= 0x7000 && a < 0x8000) || a == 0xc001) ff_impure = 1;
	}
	if (a >= 0x8000 && a < 0xa000) { sb.ram[a - 0x8000] = v; return; }
	if (a >= 0x7000 && a < 0x8000) { sb.dp[a & 0x7ff] = v; if (sb.dpw) sb.dpw(a & 0x7ff, v, sb.dpw_u); return; }
	if (a >= 0x5000 && a < 0x6000) { s21_c140_write(&sb.c140, a & 0x1ff, v); return; }
	if (a == 0x4000 || a == 0x4001) {
		uint64_t c = ym_now();
		if (sb.ymlog) sb.ymlog(c, 0, a & 1, v, sb.ymlog_u);
		s21_ym_write(sb.ym, c, a & 1, v); return;
	}
	if (a == 0xc001) { sb.bank = (v >> 4) & 15; sb.cpu.bank = sb.bank; return; }   /* the translation keys banked code on it */
}

static void trace_cb(void *ctx, m6809_t *c)
{
	(void)ctx;
	if (cov_map) cov_map[(c->pc < 0x4000 ? (sb.bank % 8) : 8) * 0x10000 + c->pc] = 1;
	if (sb.trace) sb.trace(c->pc, sb.cyc, sb.trace_u);
	if (sb.pending_reset) { sb.pending_reset = 0; m6809_sem_reset(c); c->force_firq_pc = -3; }   /* replayed sound reset */
}

/* DEV, by environment (any binary that links module D, e.g. cs21 headless):
     S21_YMLOG=<file>    every YM2151 access as 16-byte records (the opm_gate stream format)
     S21_SND_WAV=<file>  every mixed output frame s21_snd_mix() hands out, as a 48 kHz stereo WAV */
static FILE *env_ymlog, *env_wav;
static uint32_t env_wav_frames;
static void env_ymlog_cb(uint64_t clk, int kind, int off, uint8_t v, void *u)
{
	(void)u;
	uint8_t r[16] = { 0 };
	memcpy(r, &clk, 8); r[8] = (uint8_t)kind; r[9] = (uint8_t)off; r[10] = v;
	fwrite(r, 16, 1, env_ymlog);
}
static void env_wav_header(void)
{
	uint32_t data = env_wav_frames * 4, rate = S21_SND_OUT_HZ, br = rate * 4, x;
	uint16_t h;
	fseek(env_wav, 0, SEEK_SET);
	fwrite("RIFF", 1, 4, env_wav); x = 36 + data; fwrite(&x, 4, 1, env_wav);
	fwrite("WAVEfmt ", 1, 8, env_wav); x = 16; fwrite(&x, 4, 1, env_wav);
	h = 1; fwrite(&h, 2, 1, env_wav); h = 2; fwrite(&h, 2, 1, env_wav);
	fwrite(&rate, 4, 1, env_wav); fwrite(&br, 4, 1, env_wav); h = 4; fwrite(&h, 2, 1, env_wav); h = 16; fwrite(&h, 2, 1, env_wav);
	fwrite("data", 1, 4, env_wav); fwrite(&data, 4, 1, env_wav);
	fseek(env_wav, 0, SEEK_END);
}
/* S21_6809_COV: MERGED into the file (coverage only grows): "PCPC" (fixed window) or "PCPC B" (banked window, bank B) */
static void cov_write(void)
{
	if (!cov_map || !cov_path) return;
	FILE *f = fopen(cov_path, "r");
	char l[64];
	if (f) {
		while (fgets(l, sizeof l, f)) {
			unsigned pc; int b;
			int k = sscanf(l, "%x %d", &pc, &b);
			if (k >= 1 && pc < 0x10000) cov_map[(k == 2 && pc < 0x4000 ? b % 8 : 8) * 0x10000 + pc] = 1;
		}
		fclose(f);
	}
	if (!(f = fopen(cov_path, "w"))) { perror(cov_path); return; }
	for (int b = 0; b < 9; b++)
		for (int pc = 0; pc < 0x10000; pc++)
			if (cov_map[b * 0x10000 + pc]) { if (b == 8) fprintf(f, "%04X\n", pc); else fprintf(f, "%04X %d\n", pc, b); }
	fclose(f);
	cov_path = NULL;                      /* once (env_close may be registered more than once) */
}
static void env_close(void)
{
	cov_write();
	if (env_ymlog) { fclose(env_ymlog); env_ymlog = NULL; }
	if (env_wav) { env_wav_header(); fclose(env_wav); env_wav = NULL; }
}

int s21_snd_init(const uint8_t *snd, const uint8_t *voi[4], uint8_t *dpram)
{
	if (sb.ym) s21_ym_destroy(sb.ym);
	memset(&sb, 0, sizeof sb);
	sb.rom = snd; sb.dp = dpram;
	for (int i = 0; i < 4; i++) sb.voi[i] = voi[i];
	sb.cpu.rd = rd; sb.cpu.wr = wr; sb.cpu.ctx = &sb; sb.cpu.trace = trace_cb;
	sb.c140.read_word = c140_rom; sb.c140.int1 = c140_int1; sb.c140.timer = c140_timer; sb.c140.ctx = &sb;
	s21_c140_init(&sb.c140);
	sb.ym = s21_ym_create(YM_CLOCK);
	sb.c140_due = -1;
	sb.held = 1;                          /* machine_reset: the 6809 is held until the master's C148 releases it */
	{
		const char *e = getenv("S21_SND_CPU");
#ifdef S21_SND_DEV
		cpu_step = (e && !strcmp(e, "xlat")) ? cs_6809_step : m6809_step;
#else
		(void)e;
#endif
		e = getenv("S21_SND_NOFF");
		ff_on = cpu_step == cs_6809_step && !(e && *e && *e != '0');
		if (!cov_map && (e = getenv("S21_6809_COV")) && *e) { cov_map = calloc(9, 0x10000); cov_path = e; atexit(env_close); }
	}
	sb.c140_next = 0; sb.ym_next = 0; sb.out_next = 0;
	{
		const char *e;
		if (!env_ymlog && (e = getenv("S21_YMLOG")) && *e && (env_ymlog = fopen(e, "wb"))) atexit(env_close);
		if (env_ymlog) s21_snd_set_ym_log(env_ymlog_cb, NULL);
		if (!env_wav && (e = getenv("S21_SND_WAV")) && *e && (env_wav = fopen(e, "wb"))) { env_wav_header(); atexit(env_close); }
	}
	return 0;
}

void s21_snd_reset(int hold)
{
	if (hold) sb.held = 1;
	else if (sb.held) {
		sb.held = 0;
		m6809_sem_reset(&sb.cpu);
	}
}

static void ring_put(int16_t l, int16_t r)
{
	if (sb.wr_i - sb.rd_i >= RING) sb.rd_i++;    /* overrun: drop the oldest */
	sb.ring[(sb.wr_i & (RING - 1)) * 2] = l;
	sb.ring[(sb.wr_i & (RING - 1)) * 2 + 1] = r;
	sb.wr_i++;
}

static double lerp(double t, const double *ts, const int32_t *v)
{
	if (ts[1] <= ts[0]) return v[1];
	double f = (t - ts[0]) / (ts[1] - ts[0]);
	if (f < 0) f = 0; if (f > 1) f = 1;
	return v[0] + (v[1] - v[0]) * f;
}

/* bring the chips' outputs up to the board time */
static void audio_to_now(void)
{
	const double c140_p = (double)S21_SND_HZ / C140_HZ, ym_p = 64.0 * S21_SND_HZ / YM_CLOCK, out_p = (double)S21_SND_HZ / S21_SND_OUT_HZ;
	double now = (double)sb.cyc;
	for (;;) {
		double t = sb.c140_next < sb.ym_next ? sb.c140_next : sb.ym_next;
		if (sb.out_next < t) t = sb.out_next;
		if (t > now) break;
		if (t == sb.c140_next) {
			int32_t l, r;
			s21_c140_sample(&sb.c140, &l, &r);
			sb.c140_t[0] = sb.c140_t[1]; sb.c140_l[0] = sb.c140_l[1]; sb.c140_r[0] = sb.c140_r[1];
			sb.c140_t[1] = t; sb.c140_l[1] = l; sb.c140_r[1] = r;
			sb.c140_next += c140_p;
		} else if (t == sb.ym_next) {
			int32_t o[2];
			uint64_t c = (uint64_t)(t * YM_CLOCK / S21_SND_HZ);
			s21_ym_generate(sb.ym, c, o);
			if (sb.ymlog) sb.ymlog(c, 2, 0, 0, sb.ymlog_u);
			sb.ym_t[0] = sb.ym_t[1]; sb.ym_l[0] = sb.ym_l[1]; sb.ym_r[0] = sb.ym_r[1];
			sb.ym_t[1] = t; sb.ym_l[1] = o[0]; sb.ym_r[1] = o[1];
			sb.ym_next += ym_p;
		} else {
			double cl = lerp(t, sb.c140_t, sb.c140_l), cr = lerp(t, sb.c140_t, sb.c140_r);
			double yl = lerp(t, sb.ym_t, sb.ym_l), yr = lerp(t, sb.ym_t, sb.ym_r);
			/* Cyber Sled: route 0 (left) -> speaker channel 1 (rear), route 1 (right) -> channel 0 (front) */
			double ch0 = 0.25 * cr / 4096.0 + 0.15 * yr / 32768.0;
			double ch1 = 0.25 * cl / 4096.0 + 0.15 * yl / 32768.0;
			int s0 = (int)(ch0 * 32767.0), s1 = (int)(ch1 * 32767.0);
			if (s0 > 32767) s0 = 32767; if (s0 < -32768) s0 = -32768;
			if (s1 > 32767) s1 = 32767; if (s1 < -32768) s1 = -32768;
			ring_put((int16_t)s0, (int16_t)s1);
			sb.out_next += out_p;
		}
	}
}

/* THE EXACT POLLING-LOOP FAST-FORWARD. The program idles in `STA $D001 ; BRA` (0xD059: 99% of its instructions) until
   the C140 timer's FIRQ. Within one s21_snd_run nothing outside the 6809 changes (the 68000s and the C68 run between
   calls) except at the C140 timer event, so at a polling-loop head (cs_6809_spin, from the translator) with no
   interrupt pending: run ONE pass normally while watching the bus; if it came back to the head with every register
   unchanged, touched only plain memory (RAM / ROM / DPRAM reads, no-op writes) and took no interrupt, every further
   pass is identical and costs the same C cycles -- add whole passes, stopping 2 passes short of the next event (the
   timer or the end of the run), so the boundary is crossed by the same instruction as without it. The audio is
   generated up to the new time exactly as it would have been (no chip register changes in between). Returns 1 when
   it ran anything (the caller re-checks), 0 to take the normal path. Never on the interpreter. */
static int is_spin(uint16_t pc)
{
	int lo = 0, hi = cs_6809_nspin - 1;
	while (lo <= hi) { int m = (lo + hi) / 2; if (cs_6809_spin[m] == pc) return 1; if (cs_6809_spin[m] < pc) lo = m + 1; else hi = m - 1; }
	return 0;
}
static int ff_try(uint64_t end)
{
	m6809_t *c = &sb.cpu;
	if (sb.trace || sb.rp || sb.pending_reset || cov_map || c->force_firq_pc != -1) return 0;
	if (c->sync || c->cwai || c->nmi_pending || (c->firq_line && !(c->cc & CC_F)) || (c->irq_line && !(c->cc & CC_I))) return 0;
	const uint16_t head = c->pc;
	if (head < 0x4000 || !is_spin(head)) return 0;
	const m6809_t s0 = *c;
	const uint64_t t0 = sb.cyc;
	int k = 0;
	ff_mon = 1; ff_impure = 0;
	do {
		if (sb.c140_due >= 0 && (int64_t)sb.cyc >= sb.c140_due) break;   /* the event comes first: the normal loop */
		int n = cpu_step(c);
		sb.cyc += n; audio_to_now(); k++;
	} while (sb.cyc < end && c->pc != head && k < 16 && !c->trapped);
	ff_mon = 0;
	if (!k) return 0;
	if (c->pc != head || ff_impure || c->trapped || c->x != s0.x || c->y != s0.y || c->u != s0.u || c->s != s0.s ||
	    c->a != s0.a || c->b != s0.b || c->dp != s0.dp || c->cc != s0.cc || c->bank != s0.bank || c->firq_line != s0.firq_line ||
	    c->sync || c->cwai)
		return 1;                         /* not a pure loop (this time): what ran was ordinary execution */
	const uint64_t C = sb.cyc - t0;
	uint64_t limit = end;
	if (sb.c140_due >= 0 && (uint64_t)sb.c140_due < limit) limit = (uint64_t)sb.c140_due;
	if (C && limit > sb.cyc + 3 * C) {
		uint64_t m = (limit - sb.cyc) / C - 2;
		sb.cyc += m * C; c->cycles += m * C;
		audio_to_now();
	}
	return 1;
}

void s21_snd_run(int cycles)
{
	uint64_t end = sb.cyc + cycles;
	while (sb.cyc < end) {
		if (sb.c140_due >= 0 && (int64_t)sb.cyc >= sb.c140_due) { s21_sched_event((uint64_t)sb.c140_due); sb.c140_due = -1; s21_c140_timer_fired(&sb.c140); }
		int n;
		if (sb.held || sb.cpu.trapped) {      /* held in reset, or the translation trapped: time goes on, the CPU does not */
			n = (int)(end - sb.cyc);
			if (sb.c140_due >= 0 && (uint64_t)sb.c140_due < end) n = (int)(sb.c140_due - (int64_t)sb.cyc);
			if (n < 1) n = 1;
		} else {
			if (ff_on && ff_try(end)) continue;
			n = cpu_step(&sb.cpu);
		}
		sb.cyc += n;
		audio_to_now();
	}
}

int s21_snd_available(void) { return (int)(sb.wr_i - sb.rd_i); }

int s21_snd_mix(int16_t *out, int n)
{
	int k = 0;
	while (k < n && sb.rd_i != sb.wr_i) {
		out[k * 2] = sb.ring[(sb.rd_i & (RING - 1)) * 2];
		out[k * 2 + 1] = sb.ring[(sb.rd_i & (RING - 1)) * 2 + 1];
		sb.rd_i++; k++;
	}
	if (env_wav && k) { fwrite(out, 4, (size_t)k, env_wav); env_wav_frames += (uint32_t)k; }
	return k;
}

uint64_t s21_snd_cycles(void) { return sb.cyc; }
void s21_snd_set_trace(void (*fn)(uint16_t, uint64_t, void *), void *u) { sb.trace = fn; sb.trace_u = u; }
void s21_snd_set_dpram_write_hook(void (*fn)(uint16_t, uint8_t, void *), void *u) { sb.dpw = fn; sb.dpw_u = u; }
int s21_snd_bad_op(void) { return sb.cpu.bad_op; }
void s21_snd_clear_bad_op(void) { sb.cpu.bad_op = 0; }
int s21_snd_bank(void) { return sb.bank; }
int s21_snd_trapped(uint16_t *pc) { if (pc) *pc = sb.cpu.trap_pc; return sb.cpu.trapped; }
const char *s21_snd_cpu_name(void) { return cpu_step == cs_6809_step ? (ff_on ? "translated (fast-forward on)" : "translated") : "interpreter (DEV oracle)"; }
void s21_snd_set_replay(uint8_t (*rp)(uint16_t, uint8_t, void *), void *u) { sb.rp = rp; sb.rp_u = u; sb.cpu.replay = rp != NULL; }
void s21_snd_force_firq(uint16_t pc) { sb.cpu.force_firq_pc = pc; }
void s21_snd_force_reset(void) { sb.pending_reset = 1; }
void s21_snd_set_ym_log(void (*fn)(uint64_t, int, int, uint8_t, void *), void *u) { sb.ymlog = fn; sb.ymlog_u = u; }
