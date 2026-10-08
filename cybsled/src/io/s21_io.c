/* Namco System 21 C68 I/O board: M37450 + its peripherals + the cabinet inputs. See s21_io.h.
 * Behaviour reproduced from MAME's namco68 / m3745x devices (a reference only, not copied).
 *
 * THE PROGRAM (c68.bin) RUNS TRANSLATED (HARD RULE 1, PLAN.md "Module K"): gen/cs_c68.c, generated at build time by
 * tools/gen/m740_translate.py, executes m740_sem.h -- cs_c68_step() / cs_c68_reset(). The game library (s21_io) has no
 * interpreter. The DEV library (s21_io_dev, -DS21_IO_DEV) adds the oracle m37450.c and selects with S21_C68=
 *   oracle (the DEV default) | xlat | lockstep  -- lockstep: the translation drives the board, an interpreter SHADOW executes
 *   every instruction on a replay of the translation's bus accesses (ROM excepted) and every register is compared;
 * S21_C68_COV=<file>: the coverage of the m740_translate.py input ("PPPP LEN" per executed instruction, merged into the
 *   file at exit); S21_C68_BUSHASH=1: a hash of every bus access, at exit. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "s21_io.h"
#include "m37450.h"
#include "s21_sched.h"

void cs_c68_reset(m740_t *c);   /* gen/cs_c68.c: the translated program */
int  cs_c68_step(m740_t *c);

/* M3745x interrupt request bits */
#define IRQ1_INT1       0x04
#define IRQ2_ADC        0x20
#define ADCTRL_COMPLETE 0x08

static struct {
	m740_t cpu;
	const uint8_t *rom;           /* 0x8000..0xFFFF */
	uint8_t *dp;                  /* 0x800 bytes */
	uint8_t ram0[0xc0], ram1[0x100];
	uint8_t ports[4], ddrs[4];    /* P3, P4, P5, P6 */
	uint8_t req1, req2, ctrl1, ctrl2, adctrl;
	uint64_t adc_due;             /* cycle at which the A/D conversion completes (0 = idle) */
	int held;                     /* in reset */
	int mux;                      /* player mux (P3 bit 7) */
	s21_inputs in;
	void (*trace)(uint16_t, uint64_t, void *); void *trace_u;
	void (*dpw)(uint16_t, uint8_t, void *); void *dpw_u;
	uint8_t (*rp)(uint16_t, uint8_t, void *); void *rp_u;
	int pending_reset;
} io;

#ifdef S21_IO_DEV
/* ---- DEV: the oracle, the lockstep shadow, coverage, the bus hash ---- */
enum { C68_XLAT, C68_ORACLE, C68_LOCKSTEP };
static int c68_mode = -1;
static m740_t sh;                                 /* lockstep: the interpreter shadow */
typedef struct { uint16_t a; uint8_t v, w; } bus_t;
static bus_t lg[64]; static int lg_n, lg_i, lg_bad;
static uint64_t ls_steps, ls_diff, ls_first;
static uint8_t *cov_len; static const char *cov_path;
static uint64_t bh_all = 1469598103934665603ull, bh_dev = 1469598103934665603ull, bh_n; static int bh_on;
static void bh(uint64_t *h, uint64_t x) { for (int i = 0; i < 8; i++) { *h ^= (x >> (8 * i)) & 0xff; *h *= 1099511628211ull; } }
static void bus_note(int w, uint16_t a, uint8_t v)
{
	if (bh_on) {
		uint64_t x = (uint64_t)w << 56 | (uint64_t)a << 40 | (uint64_t)v << 32 | (io.cpu.cycles & 0xffffffffu);
		bh(&bh_all, x); bh_n++;
		if (a < 0x8000 || w) bh(&bh_dev, x);      /* without program-space reads: comparable translation vs oracle */
	}
	if (c68_mode == C68_LOCKSTEP && (w || a < 0x8000)) {
		if (lg_n < (int)(sizeof lg / sizeof lg[0])) lg[lg_n++] = (bus_t){ a, v, (uint8_t)w };
		else lg_bad = 1;
	}
}
#define BUS_NOTE(w, a, v) bus_note(w, a, v)
static void dev_init(void);
#else
#define BUS_NOTE(w, a, v) ((void)0)
#endif

static uint8_t bitswap8(uint16_t v, int b7, int b6, int b5, int b4, int b3, int b2, int b1, int b0)
{
	return (uint8_t)(((v >> b7 & 1) << 7) | ((v >> b6 & 1) << 6) | ((v >> b5 & 1) << 5) | ((v >> b4 & 1) << 4) |
	                 ((v >> b3 & 1) << 3) | ((v >> b2 & 1) << 2) | ((v >> b1 & 1) << 1) | (v >> b0 & 1));
}

/* the 16 request bits, MAME's order: (req1 & ctrl1) << 8 | (req2 & ctrl2); line i -> vector 0xFFFC - 2 * cpu_line */
static void recalc_irqs(void)
{
	static const int line[16] = { -1, -1, -1, 11, 12, 13, -1, -1, -1, -1, 2, 3, 4, -1, -1, -1 };
	uint16_t all = (uint16_t)(((io.req1 & io.ctrl1) << 8) | (io.req2 & io.ctrl2));
	int best = 99;
	for (int i = 0; i < 16; i++)
		if ((all >> i & 1) && line[i] >= 0 && line[i] < best) best = line[i];
	io.cpu.irq_line = best != 99;
	if (best != 99) io.cpu.irq_vector = (uint16_t)(0xfffc - 2 * best);
}

static uint8_t port_in(int n)
{
	switch (n) {
	case 0: return bitswap8(io.in.mcuc, 3, 2, 1, 0, 7, 6, 5, 4);                       /* P3: coins / service */
	case 2: {                                                                           /* P5: muxed player inputs */
		uint16_t r = (uint16_t)(io.in.mcuh << 8 | io.in.mcub);
		return io.mux ? bitswap8(r, 6, 8, 10, 12, 4, 2, 0, 14) : bitswap8(r, 7, 9, 11, 13, 5, 3, 1, 15);
	}
	default: return 0;                                                                  /* P4, P6 */
	}
}
static uint8_t read_port(int n) { return (uint8_t)((port_in(n) & ~io.ddrs[n]) | (io.ports[n] & io.ddrs[n])); }
static void send_port(int n, uint8_t v) { if (n == 0) io.mux = (v & 0x80) ? 1 : 0; }

static uint8_t dev_rd(uint16_t a);
static unsigned long io_nrp;   /* DEV: replayed reads so far */
static uint8_t rd(void *ctx, uint16_t a)
{
	(void)ctx;
	uint8_t v;
	if (io.rp && ((a >= 0x00d6 && a < 0x0100) || (a >= 0x2000 && a < 0x7000))) { v = io.rp(a, dev_rd(a), io.rp_u); io_nrp++;
		{ static long lo = -2, hi; if (lo == -2) { const char *e = getenv("S21_C68_RPDUMP"); lo = -1; if (e) sscanf(e, "%ld,%ld", &lo, &hi); }
		  if (lo >= 0 && (long)io_nrp >= lo && (long)io_nrp <= hi) fprintf(stderr, "[C68 RP] %lu %X %X ours %X at %04X cyc %llu\n", io_nrp, a, v, dev_rd(a), io.cpu.npc, (unsigned long long)io.cpu.cycles); } }
	else v = dev_rd(a);
	BUS_NOTE(0, a, v);
	return v;
}
static uint8_t dev_rd(uint16_t a)
{
	if (a < 0x00c0) return io.ram0[a];
	if (a >= 0x0100 && a < 0x0200) return io.ram1[a - 0x100];
	if (a >= 0x00d6 && a <= 0x00dd) {
		switch (a - 0xd6) {
		case 0: return read_port(0);
		case 1: return io.ddrs[0];
		case 2: return read_port(1);
		case 4: return read_port(2);
		case 5: return io.ddrs[2];
		case 6: return read_port(3);
		case 7: return io.ddrs[3];
		default: return 0xff;
		}
	}
	if (a == 0x00e2) { io.req2 &= ~IRQ2_ADC; recalc_irqs();
		{ static int lg = -1; static unsigned long cnt[8]; if (lg < 0) lg = getenv("CS_ADLOG") != NULL;   /* CS_ADLOG=1: A/D reads per channel */
		  unsigned long c = ++cnt[io.adctrl & 7]; if (lg && (c & (c - 1)) == 0) fprintf(stderr, "[AD] ch %d read %lu times, value %02X\n", io.adctrl & 7, cnt[io.adctrl & 7], io.in.an[io.adctrl & 7]); }
		return io.in.an[io.adctrl & 7]; }
	if (a == 0x00e3) return io.adctrl;
	if (a >= 0x00fc && a <= 0x00ff) {
		switch (a & 3) { case 0: return io.req1; case 1: return io.req2; case 2: return io.ctrl1; default: return io.ctrl2; }
	}
	if (a >= 0x8000) return io.rom[a - 0x8000];
	if (a == 0x2000) return io.in.dsw;
	if (a >= 0x3000 && a <= 0x3003) return io.in.dial[a & 3];
	if (a >= 0x5000 && a <= 0x57ff) return io.dp[a & 0x7ff];
	if (a >= 0x6000 && a <= 0x6fff) { io.req1 &= ~IRQ1_INT1; recalc_irqs(); return 0; }   /* VBL ack */
	return 0;
}

static void wr(void *ctx, uint16_t a, uint8_t v)
{
	(void)ctx;
	BUS_NOTE(1, a, v);
	if (a < 0x00c0) { io.ram0[a] = v; return; }
	if (a >= 0x0100 && a < 0x0200) { io.ram1[a - 0x100] = v; return; }
	if (a >= 0x00d6 && a <= 0x00dd) {
		int o = a - 0xd6;
		switch (o) {
		case 0: send_port(0, v & io.ddrs[0]); io.ports[0] = v; break;
		case 1: send_port(0, io.ports[0] & v); io.ddrs[0] = v; break;
		case 2: io.ports[1] = v; break;
		case 4: io.ports[2] = v; break;
		case 5: io.ddrs[2] = v; break;
		case 6: io.ports[3] = v; break;
		case 7: io.ddrs[3] = v; break;
		}
		return;
	}
	if (a == 0x00e3) {
		io.adctrl = v;
		{ static int lg = -1; static unsigned long n; if (lg < 0) lg = getenv("CS_ADLOG") != NULL;
		  ++n;
		  if (lg && (n & (n - 1)) == 0) fprintf(stderr, "[AD] start #%lu ctrl %02X cyc %llu held %d\n", n, v, (unsigned long long)io.cpu.cycles, io.held); }   /* CS_ADLOG=1 */
		if (!(v & ADCTRL_COMPLETE)) io.adc_due = s21_sched_slice_start(io.cpu.cycles) + 50;
		return;
	}
	if (a >= 0x00fc && a <= 0x00ff) {
		switch (a & 3) { case 0: io.req1 = v; break; case 1: io.req2 = v; break; case 2: io.ctrl1 = v; break; default: io.ctrl2 = v; }
		recalc_irqs();
		return;
	}
	if (a >= 0x5000 && a <= 0x57ff) {
		io.dp[a & 0x7ff] = v;
		if (io.dpw) io.dpw(a & 0x7ff, v, io.dpw_u);
	}
}

/* interrupt entries are not traced (MAME prints them as the interrupted PC; the gate strips those lines) */
static void cpu_reset(void);
static void trace_cb(void *ctx, m740_t *c)
{
	(void)ctx;
#ifdef S21_IO_DEV
	if (cov_len && !c->irq_taken) cov_len[c->npc] = 1;
	{ static long long n, lo = -2, hi; if (lo == -2) { const char *e = getenv("S21_C68_REGDUMP"); lo = -1; if (e) sscanf(e, "%lld,%lld", &lo, &hi); }   /* DEV: S21_C68_REGDUMP=from,to (instruction numbers) */
	  if (lo >= 0 && n >= lo && n <= hi) fprintf(stderr, "[C68 REG] #%lld %04X%s a %02X x %02X y %02X s %02X p %02X t %d [80] %02X [81] %02X cyc %llu\n", n, c->npc,
	      c->irq_taken ? " (irq)" : "", c->a, c->x, c->y, c->s, c->p, c->tbase != 0, io.ram0[0x80], io.ram0[0x81], (unsigned long long)c->cycles);
	  if (!c->irq_taken) n++; }
	{ static int k, lo2 = -2, hi2; if (lo2 == -2) { const char *e = getenv("S21_C68_REGAT"); lo2 = -1; if (e) sscanf(e, "%d,%d", &lo2, &hi2); }   /* DEV: around the k-th pass of 0x83C1 */
	  if (c->npc == 0x83C1 && !c->irq_taken) k++;
	  if (lo2 >= 0 && k >= lo2 && k <= hi2 && c->npc >= 0x8398 && c->npc <= 0x83C8) fprintf(stderr, "[C68 AT] rp %lu k %d %04X a %02X p %02X [80] %02X [81] %02X [D6]ports %02X ddr %02X\n", io_nrp, k, c->npc, c->a, c->p, io.ram0[0x80], io.ram0[0x81], io.ports[0], io.ddrs[0]); }
#endif
	if (io.trace && !c->irq_taken) io.trace(c->npc, c->cycles, io.trace_u);
	if (io.pending_reset) {                       /* replayed reset */
		io.pending_reset = 0; cpu_reset(); c->force_irq_pc = -3;
#ifdef S21_IO_DEV
		if (c68_mode == C68_LOCKSTEP) sh.force_irq_pc = -3;
#endif
	}
}

void s21_io_default_inputs(s21_inputs *in)
{
	memset(in, 0xff, sizeof *in);
	in->dsw = 0xdf;                                  /* PCM ROM = 4M, everything else off */
	for (int i = 0; i < 4; i++) in->an[i] = 0x7f;
}

int s21_io_init(const uint8_t *c68, uint8_t *dpram)
{
	memset(&io, 0, sizeof io);
#ifdef S21_IO_DEV
	dev_init();
#endif
	io.rom = c68; io.dp = dpram;
	s21_io_default_inputs(&io.in);
	io.cpu.rd = rd; io.cpu.wr = wr; io.cpu.ctx = &io; io.cpu.trace = trace_cb;
	io.held = 1;                                     /* the sub CPUs start in reset until the master releases them */
	return 0;
}

void s21_io_set_inputs(const s21_inputs *in) { io.in = *in; }
void s21_io_get_inputs(s21_inputs *in) { *in = io.in; }

#ifdef S21_IO_DEV
/* the shadow's bus: program space from the image; everything else replays the translation's accesses, in order */
static uint8_t sh_rd(void *ctx, uint16_t a)
{
	(void)ctx;
	if (a >= 0x8000) return io.rom[a - 0x8000];
	if (lg_i >= lg_n || lg[lg_i].w || lg[lg_i].a != a) { lg_bad = 1; return 0; }
	return lg[lg_i++].v;
}
static void sh_wr(void *ctx, uint16_t a, uint8_t v)
{
	(void)ctx;
	if (lg_i >= lg_n || !lg[lg_i].w || lg[lg_i].a != a || lg[lg_i].v != v) { lg_bad = 1; return; }
	lg_i++;
}
static void sh_sync(void)                         /* the shadow takes the translation's state (power-on, a reset) */
{
	sh = io.cpu;
	sh.rd = sh_rd; sh.wr = sh_wr; sh.trace = NULL; sh.ctx = NULL;
	sh.ir = sh.irq_taken ? 0x00 : sh_rd(NULL, sh.npc);    /* the translation never fetches the opcode: the shadow's prefetch */
	lg_n = lg_i = lg_bad = 0;
}
static int lockstep_step(void)
{
	m740_t *c = &io.cpu;
	uint16_t vec0 = c->irq_vector;
	lg_n = lg_i = lg_bad = 0;
	int r = cs_c68_step(c);
	if (c->trapped) return r;
	{ static long long inj = -2; if (inj == -2) { const char *e = getenv("S21_C68_LS_INJECT"); inj = e ? atoll(e) : -1; }   /* negative control */
	  if (inj >= 0 && (long long)ls_steps == inj) c->a ^= 0x01; }
	sh.irq_line = c->irq_line; sh.irq_vector = vec0; sh.replay = c->replay;
	m740_step(&sh);
	ls_steps++;
	int bad = lg_bad || lg_i != lg_n || sh.a != c->a || sh.x != c->x || sh.y != c->y || sh.s != c->s || sh.p != c->p ||
	          sh.pc != c->pc || sh.npc != c->npc || sh.tbase != c->tbase || sh.irq_taken != c->irq_taken ||
	          sh.halted != c->halted || sh.cycles != c->cycles;
	if (bad) {
		if (!ls_diff++) {
			ls_first = ls_steps;
			fprintf(stderr, "[C68 LOCKSTEP] step %llu: translation != oracle -- xlat pc %04X npc %04X a %02X x %02X y %02X s %02X p %02X cyc %llu / "
			        "oracle pc %04X npc %04X a %02X x %02X y %02X s %02X p %02X cyc %llu; bus %d of %d accesses replayed%s\n",
			        (unsigned long long)ls_steps, c->pc, c->npc, c->a, c->x, c->y, c->s, c->p, (unsigned long long)c->cycles,
			        sh.pc, sh.npc, sh.a, sh.x, sh.y, sh.s, sh.p, (unsigned long long)sh.cycles, lg_i, lg_n, lg_bad ? " (an access differed)" : "");
		}
		sh_sync();                                /* resynchronise: count every differing step, not one cascade */
	}
	return r;
}

static void dev_exit(void)
{
	if (cov_len && cov_path) {                    /* merge with the file: coverage only grows */
		static const char len[] =
			"1222222312113332221222231311333232222223121133322212222313113332"
			"1212222312113332221222231311333212122223121133322212222313113332"
			"2222222312113332221222231311131222222223121133322222222313113332"
			"2212222312113332221222231311333222222223121133322222222313113332";
		FILE *f = fopen(cov_path, "r");
		char l[64]; unsigned pc;
		if (f) { while (fgets(l, sizeof l, f)) if (sscanf(l, "%x", &pc) == 1 && pc < 0x10000) cov_len[pc] = 1; fclose(f); }
		if ((f = fopen(cov_path, "w"))) {
			int n = 0;
			fprintf(f, "# the C68 (c68.bin) instructions the oracle executed: \"PPPP LEN\" -- addresses only, no ROM data (PLAN.md module K)\n");
			for (unsigned a = 0; a < 0x10000; a++)
				if (cov_len[a]) { fprintf(f, "%04X %c\n", a, a >= 0x8000 ? len[io.rom[a - 0x8000]] : '0'); n++; }
			fclose(f);
			fprintf(stderr, "[C68] coverage: %d instruction addresses -> %s\n", n, cov_path);
		}
	}
	if (bh_on) fprintf(stderr, "[C68] bus hash: %llu accesses, all %016llx, without program-space reads %016llx\n",
	                   (unsigned long long)bh_n, (unsigned long long)bh_all, (unsigned long long)bh_dev);
	if (c68_mode == C68_LOCKSTEP) fprintf(stderr, "[C68 LOCKSTEP] %llu steps, %llu differing%s\n", (unsigned long long)ls_steps,
	                                      (unsigned long long)ls_diff, ls_diff ? " -- FAIL" : " -- translation == oracle");
}
static void dev_init_mode(const char *m)
{
	static int once;
	c68_mode = !m || !strcmp(m, "oracle") ? C68_ORACLE : !strcmp(m, "xlat") ? C68_XLAT : !strcmp(m, "lockstep") ? C68_LOCKSTEP : -1;
	if (c68_mode < 0) { fprintf(stderr, "S21_C68=%s: xlat | oracle | lockstep\n", m); exit(2); }
	fprintf(stderr, "[C68] %s\n", c68_mode == C68_ORACLE ? "DEV: the interpreter (oracle)" : c68_mode == C68_LOCKSTEP ?
	        "DEV: the translation, in lockstep with an interpreter shadow" : "the translated program");
	if (once++) return;
	if ((cov_path = getenv("S21_C68_COV"))) cov_len = calloc(0x10000, 1);
	bh_on = getenv("S21_C68_BUSHASH") != NULL;
	atexit(dev_exit);
}
static void dev_init(void) { if (c68_mode < 0) dev_init_mode(getenv("S21_C68")); }
void s21_io_use_mode(const char *m) { dev_init_mode(m); }
#endif

static int cpu_step(void)
{
#ifdef S21_IO_DEV
	if (c68_mode == C68_ORACLE) return m740_step(&io.cpu);
	if (c68_mode == C68_LOCKSTEP) return lockstep_step();
#endif
	return cs_c68_step(&io.cpu);
}

static void cpu_reset(void)
{
	memset(io.ports, 0, sizeof io.ports); memset(io.ddrs, 0, sizeof io.ddrs);
	io.req1 = io.req2 = io.ctrl1 = io.ctrl2 = io.adctrl = 0;
	io.adc_due = 0; io.mux = 0;
#ifdef S21_IO_DEV
	if (c68_mode == C68_ORACLE) m740_reset(&io.cpu);
	else
#endif
	cs_c68_reset(&io.cpu);
	recalc_irqs();
#ifdef S21_IO_DEV
	if (c68_mode == C68_LOCKSTEP) sh_sync();
#endif
}

void s21_io_reset(int hold)
{
	if (hold) io.held = 1;
	else if (io.held) { io.held = 0; cpu_reset(); }
}

void s21_io_vblank(void) { io.req1 |= IRQ1_INT1; recalc_irqs(); }

void s21_io_run(int cycles)
{
	if (io.held) { io.cpu.cycles += cycles; return; }
	uint64_t end = io.cpu.cycles + cycles;
	while (io.cpu.cycles < end) {
		if (io.held || io.cpu.trapped) { io.cpu.cycles = end; break; }   /* held by a reset during this slice / stopped */
		cpu_step();
		if (io.adc_due && io.cpu.cycles >= io.adc_due) {
			s21_sched_event(io.adc_due);   /* a device timer ends MAME's timeslice: the next one starts at its time */
			io.adc_due = 0;
			io.adctrl |= ADCTRL_COMPLETE;
			io.req2 |= IRQ2_ADC;
			recalc_irqs();
		}
	}
}

uint64_t s21_io_cycles(void) { return io.cpu.cycles; }
void s21_io_set_trace(void (*fn)(uint16_t, uint64_t, void *), void *u) { io.trace = fn; io.trace_u = u; }
void s21_io_set_dpram_write_hook(void (*fn)(uint16_t, uint8_t, void *), void *u) { io.dpw = fn; io.dpw_u = u; }
void s21_io_set_replay(uint8_t (*rp)(uint16_t, uint8_t, void *), void *u) { io.rp = rp; io.rp_u = u; io.cpu.replay = rp != NULL; }
void s21_io_force_irq(uint16_t pc)
{
	io.cpu.force_irq_pc = pc;
#ifdef S21_IO_DEV
	if (c68_mode == C68_LOCKSTEP) sh.force_irq_pc = pc;
#endif
}
void s21_io_force_reset(void) { io.pending_reset = 1; }
