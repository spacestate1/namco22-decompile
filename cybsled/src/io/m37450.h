/* M37450 (Mitsubishi 740 family) -- the CPU state of the C68 I/O MCU of Namco System 21, shared by the DEV interpreter
 * (m37450.c, m740_step: the oracle, HARD RULE 2, never linked into a shipped game) and the TRANSLATED program
 * (tools/gen/m740_translate.py -> gen/<game>_c68.c, <prefix>_step: what the game runs). Both execute m740_sem.h.
 * The semantics were written from the 740 programming manual's instruction set and MAME's m740/m3745x behaviour (reference only):
 * every instruction issues the same bus accesses (incl. dummy reads) MAME's core does, one cycle each, so a
 * cycle count -- and the instruction at which an interrupt is taken -- is comparable with a MAME trace.
 * The CPU's internal peripheral block (ports, A/D, interrupt registers) lives in s21_io.c, behind rd/wr. */
#ifndef M37450_H
#define M37450_H
#include <stdint.h>

enum { M740_C = 0x01, M740_Z = 0x02, M740_I = 0x04, M740_D = 0x08, M740_B = 0x10, M740_T = 0x20, M740_V = 0x40, M740_N = 0x80 };

typedef struct m740 {
	uint16_t pc;          /* address of the NEXT opcode byte after the prefetch (= MAME m_PC) */
	uint16_t npc;         /* address of the instruction being executed (trace PC) */
	uint8_t a, x, y, s, p;
	uint8_t ir;           /* prefetched opcode */
	int tbase;            /* 0 or 0x100: T-mode instruction table (MAME m_inst_state_base) */
	int irq_taken;
	int irq_line;         /* level: any enabled interrupt request (set by the peripheral block) */
	uint16_t irq_vector;  /* vector of the highest-priority active request (stale when none, like MAME) */
	int halted;           /* WIT / STP: waiting for an interrupt */
	uint64_t cycles;      /* bus cycles (MAME m740 cycles = clock / 4) */
	uint8_t (*rd)(void *ctx, uint16_t a);
	void (*wr)(void *ctx, uint16_t a, uint8_t v);
	void *ctx;
	void (*trace)(void *ctx, struct m740 *c);  /* optional: called before each instruction with npc set */
	int replay;           /* DEV: interrupts come only from force_irq_pc (an environment replay) */
	int32_t force_irq_pc; /* DEV: set by the trace hook -> take an interrupt into this PC before the instruction */
	int trapped;          /* the translation reached code it has no translation for: the CPU stops (see trap_pc) */
	uint16_t trap_pc;
} m740_t;

void m740_reset(m740_t *c);
/* DEV oracle: run one instruction (or an interrupt entry); returns the cycles it took */
int m740_step(m740_t *c);

#endif
