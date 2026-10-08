/*
 * s21_cpu.h -- the two lifted 68000 programs of Cyber Sled (System 21), hosted in one binary (cybsled/DESIGN.md, module A).
 *
 * Each CPU is ONE relocatable unit: its lifted program (gen/cm_lifted.c = master, gen/cs_lifted.c = slave), its own copy of the
 * shared 68K runtime (engine/lift_cpu.c: the register file R[], the shadow return stack, interrupt entry, the trace hook), the trace
 * oracle's environment replay (engine/lift_env.c, trace builds) and s21_cpu_glue.c. The unit is partially linked (ld -r) and every
 * symbol except the API below is made local (objcopy --keep-global-symbols), so the two R[] and the two shadow stacks never meet and
 * the shared runtime is used unchanged. Each CPU runs on its own coroutine (ucontext): its RR_POLL hands control back to the
 * scheduler when its instruction budget is spent, so the board interleaves the two in slices.
 *
 * What the unit needs from the board (s21_board.c): s21_bus_read/s21_bus_write (the CPU's memory map), s21_irq_level (the highest
 * pending 68K level on the CPU's input lines), s21_irq_taken (the CPU took that level: a HOLD_LINE request clears),
 * s21_env_in (a device address, for the trace oracle) and s21_env_irq (the oracle raised a level).
 */
#ifndef S21_CPU_H
#define S21_CPU_H
#include <stdint.h>

#define S21_CPU_DECL(N) \
    void     s21_cpu##N##_reset(void);              /* restart from the reset vectors at the next run (abandons the current stack) */ \
    int32_t  s21_cpu##N##_run(int32_t budget);      /* run ~budget instructions (to the next poll after it is spent); -> executed */ \
    int      s21_cpu##N##_halted(void);             /* the program returned from its entry or stopped */ \
    uint32_t s21_cpu##N##_reg(int off);             /* a 32-bit register (lift_rt.h offsets: D0 0x00, A0 0x20, SP 0x3C) */ \
    uint32_t s21_cpu##N##_sr(void); \
    void     s21_cpu##N##_set_frame(uint32_t f);    /* the frame number traps and traces name */ \
    uint32_t s21_cpu##N##_traps(void); \
    int      s21_cpu##N##_env_init(const char *path); /* trace builds: replay MAME's environment (engine/lift_env.c); 0 = not built in */ \
    int      s21_cpu##N##_env_active(void); \
    void     s21_cpu##N##_hooks(int on);            /* entry hooks on: s21_dbg_sub may replace a routine as it is entered (debug) */
S21_CPU_DECL(0)
S21_CPU_DECL(1)

/* provided by the board */
uint32_t s21_bus_read(int cpu, uint32_t a, int size);
void     s21_bus_write(int cpu, uint32_t a, int size, uint32_t v);
int      s21_irq_level(int cpu);
void     s21_irq_taken(int cpu, int level);
int      s21_env_in(int cpu, uint32_t a);
void     s21_env_irq(int cpu, int level);
int      s21_dbg_sub(int cpu, uint32_t entry, uint32_t *alt);   /* src/board/s21_debug.c: run *alt instead of entry (1) or not (0) */
#endif
