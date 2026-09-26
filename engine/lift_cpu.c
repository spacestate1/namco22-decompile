/*
 * lift_cpu.c -- the 68K host runtime shared by every lifted System 22 game: the register
 * file, the shadow return stack, traps, interrupt entry and the instruction-trace hook.
 *
 * Extracted VERBATIM from raverace/src/rr_main.c (2026-09-25) when Tokyo Wars became the
 * second lifted game (HARD RULE 3: one copy). What stays with each game: the scheduler
 * (rr_tick: how many 68K instructions make a frame, which chips step with it), the memory
 * map (rr_read / rr_write), the devices that raise interrupts, and main().
 *
 * Lifted code (tools/namco22/lift.py) turns every 68K call into a C call, so the 68K's rts
 * needs a shadow stack to find the C frame to unwind to; see engine/lift_rt.h.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lift_rt.h"
#include "lift_cpu.h"

uint8_t R[RR_REGSPACE];
int32_t rr_budget;
uint32_t rr_frame, rr_n_traps;
int rr_in_irq;
uint32_t rr_n_irq[8];

void rd_budget_out(void);   /* src/rd: unwind a checker probe (budget ran out, or it trapped) */
void rr_trap(uint32_t at, uint32_t target, const char *what)
{
    rd_budget_out();                                 /* a probe's mutated state trapped: abandon the probe */
    { extern int rd_quiet; if (rd_quiet) return; }   /* a fuzz probe's mutated state (src/rd), not the game */
    rr_n_traps++;
    if (rr_n_traps <= 40)
        fprintf(stderr, "[TRAP] f%u at 0x%06X -> 0x%08X: %s\n", rr_frame, at, target, what);
    if (rr_n_traps == 40) fprintf(stderr, "[TRAP] (further traps counted, not printed)\n");
}

#ifdef RR_TRACE
/* RR_TRACE_FILE=path RR_TRACE_MAX=n: "PC D0..D7 A0..A7" per instruction */
static FILE *trace_f;
void rr_trace_mark(const char *m) { if (trace_f) fprintf(trace_f, "%s\n", m); }
static uint64_t trace_n, trace_max = 2000000;
static int trace_pconly;
uint32_t rr_trace_pc;                         /* current instruction, for RR_WATCH */
void (*rr_trace_hook)(uint32_t pc);           /* a game's trace ORACLE: called before every instruction */
void rr_trace_ins(uint32_t pc)
{
    if (rr_trace_hook) rr_trace_hook(pc);
    rr_trace_pc = pc;
    {   /* RR_WATCHPC=a,b,...: at each listed PC print D0/D1 and the flags (state BEFORE it runs) */
        static uint32_t wp[16]; static int nwp = -1;
        if (nwp < 0) { nwp = 0; const char *e = getenv("RR_WATCHPC");
            for (char *q = (char *)e; e && *q && nwp < 16; ) { char *end; unsigned long v = strtoul(q, &end, 16);
                if (end == q) { q++; continue; } wp[nwp++] = (uint32_t)v; q = end; } }
        for (int i = 0; i < nwp; i++) if (wp[i] == pc) {
            fprintf(stderr, "[PCW] %06X D0=%08X D1=%08X N%dZ%dV%dC%d\n", pc, (uint32_t)RG4(0), (uint32_t)RG4(4),
                    (int)RG1(0x44), (int)RG1(0x45), (int)RG1(0x46), (int)RG1(0x47)); break; }
    }
    { extern int rd_cov_on; extern void rd_cov_ins(uint32_t); if (rd_cov_on) rd_cov_ins(pc); }
    static int off = -1;
    if (off < 0) off = getenv("RR_TRACE_OFF") != NULL;
    if (off) return;                          /* PC tracking only (RR_WATCH without a trace file) */
    if (!trace_f) {
        const char *p = getenv("RR_TRACE_FILE");
        trace_f = fopen(p ? p : "rr_trace.txt", "w");
        const char *m = getenv("RR_TRACE_MAX");
        if (m) trace_max = strtoull(m, NULL, 0);
        trace_pconly = getenv("RR_TRACE_PCONLY") != NULL;   /* PCs only: ~9 bytes/instruction */
    }
    if (trace_n++ >= trace_max) { fclose(trace_f); fprintf(stderr, "[TRACE] %llu instructions\n", (unsigned long long)trace_max); exit(0); }
    fprintf(trace_f, "%08X", pc);
    if (trace_pconly) { fputc('\n', trace_f); return; }
    for (int i = 0; i < 16; i++) fprintf(trace_f, " %08X", (uint32_t)RG4(i * 4));
    fputc('\n', trace_f);
}
#endif

#define get_sr rr_get_sr
#define set_sr rr_set_sr

/* ---- shadow return stack (see rr_lift_rt.h) ---- */
#define SHADOW_MAX 4096
#define SH_IRQ 0xFFFFFFFFu
static uint32_t sh_ret[SHADOW_MAX], sh_sp[SHADOW_MAX];
static int sh_n, sh_target = -1;          /* sh_target: frame index the unwind stops at */
uint32_t rr_ret_to;
long rr_ncalls;             /* calls made (bsr/jsr/IRQ): the checker's fuzz skips callers */

int rr_call_push(uint32_t ret)
{
    if (sh_n >= SHADOW_MAX) { fprintf(stderr, "[RR] shadow stack overflow at %08X\n", ret); exit(4); }
    sh_ret[sh_n] = ret; sh_sp[sh_n] = (uint32_t)RG4(RR_REG_SP);   /* SP after the push */
    rr_ncalls++;
    return sh_n++;
}
int rr_irq_push(void) { int j = rr_call_push(SH_IRQ); return j; }

int rr_after_call(int j)
{
    if (sh_target < 0) {                   /* callee came back with a plain C return (trap) */
        rd_budget_out();                   /* inside a checker probe: abandon the probe, not the run */
        fprintf(stderr, "[RR] call frame %d returned without rts -- stopping\n", j); exit(5);
    }
    if (sh_target < j) return 1;           /* unwinding further up */
    sh_target = -1;                        /* this call site: resume at rr_ret_to */
    return 0;
}

int rr_return(uint32_t t)
{
    uint32_t sp = (uint32_t)RG4(RR_REG_SP);   /* after the pop */
    for (int k = sh_n - 1; k >= 0; k--)    /* exact: address and stack depth */
        if (sh_ret[k] == t && sh_sp[k] + 4 == sp) { sh_n = k; sh_target = k; rr_ret_to = t; return 1; }
    for (int k = sh_n - 1; k >= 0; k--)    /* address only (a frame reshaped the stack) */
        if (sh_ret[k] == t && sh_ret[k] != SH_IRQ) { sh_n = k; sh_target = k; rr_ret_to = t; return 1; }
    return 0;
}

/* Readable-C checker (src/rd/rd_core.c): the shadow stack's live state is its
 * depth, unwind target and last return address -- slots above sh_n are dead. */
void rr_shadow_save(int *n, int *target, uint32_t *ret_to) { *n = sh_n; *target = sh_target; *ret_to = rr_ret_to; }
void rr_shadow_load(int n, int target, uint32_t ret_to) { sh_n = n; sh_target = target; rr_ret_to = ret_to; }

void rr_rte(void)
{
    for (int k = sh_n - 1; k >= 0; k--)
        if (sh_ret[k] == SH_IRQ) { sh_n = k; sh_target = k; rr_ret_to = SH_IRQ; return; }
    fprintf(stderr, "[RR] rte with no interrupt on the shadow stack\n"); exit(6);
}


/* Take ONE interrupt of level lvl now: save SR, raise the mask, call the autovector handler
 * (VBR + (24 + lvl) * 4) and restore SR when it rtes. Also what a trace oracle calls to land an
 * interrupt on an exact instruction. */
void rr_irq_enter(int lvl)
{
    uint32_t sr = get_sr();
    uint32_t vec = (uint32_t)RG4(RR_REG_VBR) + (24 + lvl) * 4;
    uint32_t handler = rr_read(vec, 4);
    set_sr((sr & ~0x0700u) | 0x2000u | ((uint32_t)lvl << 8));
    rr_in_irq++;
#ifdef RR_TRACE
    { void rr_trace_mark(const char *); char b[32]; snprintf(b, sizeof b, "IRQ %d %08X", lvl, handler); rr_trace_mark(b); }
#endif
    rr_n_irq[lvl]++;
    int j = rr_irq_push();
    rr_jump(handler, 0xFFFFFFFFu);
    if (sh_target != j) { fprintf(stderr, "[RR] irq handler %08X ended without rte\n", handler); exit(6); }
    sh_target = -1;
#ifdef RR_TRACE
    { void rr_trace_mark(const char *); rr_trace_mark("RTE"); }
#endif
    rr_in_irq--;
    set_sr(sr);
}

void rr_deliver_irqs(int (*level_fn)(void))
{
    if (rr_in_irq > 4) return;
    for (;;) {
        int lvl = level_fn();
        uint32_t sr = get_sr();
        int mask = (sr >> 8) & 7;
        if (lvl == 0 || (lvl <= mask && lvl != 7)) return;
        rr_irq_enter(lvl);
        /* a handler that does not acknowledge would re-enter forever */
        if (level_fn() == lvl) return;
    }
}
