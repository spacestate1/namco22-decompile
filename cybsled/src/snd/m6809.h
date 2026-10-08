/* MC6809E -- the CPU state of the Namco System 21 sound CPU, shared by the DEV interpreter (m6809.c, m6809_step: the
 * oracle, HARD RULE 2, never shipped) and the TRANSLATED program (tools/gen/m6809_translate.py -> gen/<game>_6809.c,
 * <prefix>_step: what the game runs). Both execute m6809_sem.h. Instruction set and cycle counts from the Motorola
 * MC6809 datasheet; behaviour checked against MAME's m6809 (reference only). Interrupts are sampled before each
 * instruction, as MAME does. */
#ifndef M6809_H
#define M6809_H
#include <stdint.h>

enum { CC_C = 0x01, CC_V = 0x02, CC_Z = 0x04, CC_N = 0x08, CC_I = 0x10, CC_H = 0x20, CC_F = 0x40, CC_E = 0x80 };

typedef struct m6809 {
	uint16_t pc, x, y, u, s;
	uint8_t a, b, dp, cc;
	int firq_line, irq_line, nmi_pending;
	int cwai, sync;           /* waiting states */
	uint64_t cycles;
	uint8_t (*rd)(void *ctx, uint16_t a);
	void (*wr)(void *ctx, uint16_t a, uint8_t v);
	void *ctx;
	void (*trace)(void *ctx, struct m6809 *c);   /* before each instruction (not on interrupt entries) */
	int replay;               /* DEV: interrupts come only from force_firq_pc (an environment replay), never the lines */
	int32_t force_firq_pc;    /* DEV: set by the trace hook -> take a FIRQ into this PC instead of the instruction */
	int bad_op;               /* last illegal opcode seen (DEV diagnostic) */
	uint8_t (*fetch)(void *ctx, uint16_t a);   /* DEV (oracle only): instruction bytes from here instead of rd (a lockstep
	                                              shadow, whose rd replays the translated CPU's logged data accesses) */
	int bank;                 /* the translation's view of the banked window: set by the board (the 0x0000-0x3FFF ROM bank) */
	int trapped;              /* the translation reached code it has no translation for: the CPU stops (see trap_pc) */
	uint16_t trap_pc;
} m6809_t;

void m6809_reset(m6809_t *c);
int  m6809_step(m6809_t *c);     /* DEV oracle: one instruction or one interrupt entry; returns cycles */

#endif
