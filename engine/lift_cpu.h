/* lift_cpu.h -- the 68K host runtime shared by every lifted System 22 game (engine/lift_cpu.c). */
#ifndef LIFT_CPU_H
#define LIFT_CPU_H
#include <stdint.h>

#define RR_REG_SP  0x3C
#define RR_REG_SR  0x200
#define RR_REG_VBR 0x108

extern uint32_t rr_frame;          /* the host's frame counter: traps and traces name it */
extern uint32_t rr_n_traps;
extern int      rr_in_irq;         /* > 0 while a host-delivered interrupt handler runs */
extern uint32_t rr_n_irq[8];       /* interrupts delivered, per level */

/* generated (<base>_lifted.c): run whichever lifted function owns `target` */
void rr_jump(uint32_t target, uint32_t at);

/* Deliver every pending interrupt above the SR mask. level_fn() = the highest pending 68K
 * level (0 = none). Ghidra's rte p-code is a bare return that restores neither SR nor SP, so
 * an interrupt pushes NO frame: the host saves SR, raises the mask to the level, calls the
 * autovector handler (VBR + (24 + level) * 4) and restores SR afterwards. */
void rr_deliver_irqs(int (*level_fn)(void));
/* take one interrupt of this level now (see rr_deliver_irqs) */
void rr_irq_enter(int lvl);

/* readable-C checker (src/rd): the shadow stack's live state */
void rr_shadow_save(int *n, int *target, uint32_t *ret_to);
void rr_shadow_load(int n, int target, uint32_t ret_to);

#ifdef RR_TRACE
void rr_trace_mark(const char *m);   /* a marker line in the instruction trace */
/* called with the PC before every instruction, in the trace build: lets a game feed its trace
 * comparison the environment MAME recorded (dev only -- never linked into a shipped game) */
extern void (*rr_trace_hook)(uint32_t pc);
#endif
#endif
