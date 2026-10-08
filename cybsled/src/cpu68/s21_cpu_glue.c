/*
 * s21_cpu_glue.c -- one 68000 of System 21 as a unit (see s21_cpu.h). Compiled twice, -DS21_CPU=0 (master) and -DS21_CPU=1 (slave),
 * each time beside its own lifted program and its own copy of engine/lift_cpu.c / lift_env.c, which are compiled with
 * -Dgetenv=s21_unit_getenv so each CPU's trace takes its own file (RR_TRACE_FILE_M / RR_TRACE_FILE_S, RR_TRACE_MAX_M / _S).
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32                                          /* Windows: the coroutine is a FIBER (no ucontext there) */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600                            /* IsThreadAFiber: Vista and later */
#endif
#include <windows.h>
#else
#include <ucontext.h>
#include <sys/mman.h>
#endif
#include "lift_rt.h"
#include "lift_cpu.h"
#include "lift_env.h"
#include "s21_cpu.h"

#ifndef S21_CPU
#error "compile with -DS21_CPU=0 (master) or 1 (slave)"
#endif
#if S21_CPU == 0
#define API(x) s21_cpu0_##x
#define SFX "_M"
#define TAG "cs21-master"
#else
#define API(x) s21_cpu1_##x
#define SFX "_S"
#define TAG "cs21-slave"
#endif

/* ---- what engine/lift_cpu.c and the lifted code expect from a host (the readable-C checker hooks are off here) ---- */
int rd_on, rd_stop_on, rd_poll_rec, rd_quiet, rd_cov_on;
/* rd_hook: every lifted function's entry calls it while rd_on is set (s21_cpuN_hooks). Used for the developer test mode only (PLAN.md
 * Module I): the board names a routine to run INSTEAD of the one being entered -- what a dispatch table of a development build named
 * there (s21_debug.c). The replacement runs in this function's place, on this CPU's coroutine, and its rts returns to the same caller. */
void (*rd_lifted_entry(uint32_t ep))(uint32_t);
int rd_hook(uint32_t ep)
{
    uint32_t alt;
    if (!s21_dbg_sub(S21_CPU, ep, &alt)) return 0;
    void (*f)(uint32_t) = rd_lifted_entry(alt);
    if (!f) { fprintf(stderr, "[%s] debug: %06X is not a lifted entry\n", TAG, alt); return 0; }
    f(alt);
    return 1;
}
int rd_jump_stop(uint32_t t, uint32_t at) { (void)t; (void)at; return 0; }
void rd_poll_snap(void) {}
void rd_budget_out(void) {}
void rd_cov_ins(uint32_t pc) { (void)pc; }
void (*rr_read_probe)(uint32_t, int);          /* engine/lift_env.c: sees every read (unrecorded device reads) */

/* per-CPU environment names for the copies of the runtime in this unit */
char *s21_unit_getenv(const char *n)
{
    char b[64];
    if (!strcmp(n, "RR_TRACE_FILE") || !strcmp(n, "RR_TRACE_MAX") || !strcmp(n, "RR_TRACE_PCONLY")) {
        snprintf(b, sizeof b, "%s%s", n, SFX); return getenv(b);
    }
    if (!strcmp(n, "RR_TRACE_OFF")) {                   /* no trace file for this CPU: track PCs only, write nothing */
        snprintf(b, sizeof b, "RR_TRACE_FILE%s", SFX);
        return getenv(b) ? getenv("RR_TRACE_OFF") : (char *)"1";
    }
    return getenv(n);
}

/* ---- memory: the 68000 has a 24-bit address bus ---- */
uint32_t rr_read(uint32_t a, int size)
{
    if (rr_read_probe) rr_read_probe(a & 0xFFFFFFu, size);
    return s21_bus_read(S21_CPU, a & 0xFFFFFFu, size);
}
void rr_write(uint32_t a, int size, uint32_t v) { s21_bus_write(S21_CPU, a & 0xFFFFFFu, size, v); }

/* ---- the coroutine ---- */
#define STACK_SZ (64u << 20)                            /* deep lifted call chains; mmap'd, committed only as touched */
static int started, halted, env_on, powered;
static void entry(void);
#ifdef _WIN32
static LPVOID sched_fib, cpu_fib;                      /* the board's scheduler (the main thread as a fiber) and this 68000 */
static void WINAPI fib_entry(LPVOID p) { (void)p; entry(); }
#define TO_SCHED() SwitchToFiber(sched_fib)
#define TO_CPU()   SwitchToFiber(cpu_fib)
#else
static ucontext_t sched_ctx, cpu_ctx;
static char *stacks[2];                                /* two: a reset from inside the CPU's own coroutine starts on the other */
static int cur_stack;
#define TO_SCHED() swapcontext(&cpu_ctx, &sched_ctx)
#define TO_CPU()   swapcontext(&sched_ctx, &cpu_ctx)
#endif
static int32_t budget0;

static void deliver(void)
{
    if (env_on || rr_in_irq > 4) return;                /* the trace oracle lands interrupts itself, on MAME's instructions */
    for (;;) {
        int lvl = s21_irq_level(S21_CPU);
        int mask = (rr_get_sr() >> 8) & 7;
        if (lvl == 0 || (lvl <= mask && lvl != 7)) return;
        s21_irq_taken(S21_CPU, lvl);                    /* a HOLD_LINE request (the vblank) clears when taken */
        rr_irq_enter(lvl);
        if (s21_irq_level(S21_CPU) == lvl) return;      /* not acknowledged by its handler: once per poll, not forever */
    }
}

void rr_tick(void)
{
    TO_SCHED();                                         /* back to the board's scheduler; we resume with a new budget */
    deliver();
}

static void entry(void)
{
    if (!powered) { memset(R, 0, sizeof R); powered = 1; }   /* a 68000 reset loads SSP, PC and SR only; D0-A6 keep their values */
    rr_shadow_load(0, -1, 0);
    rr_in_irq = 0;
    RS4(RR_REG_SP, rr_read(0, 4));
    rr_set_sr(0x2700);
    rr_call_push(0xFFFFFFFEu);
    rr_jump(rr_read(4, 4), 0xFFFFFFFFu);
    fprintf(stderr, "[%s] the program returned from its entry (frame %u)\n", TAG, rr_frame);
    halted = 1;
    for (;;) TO_SCHED();
}

void API(reset)(void) { started = 0; halted = 0; }

static void make_entry(void)
{
#ifdef _WIN32
    /* called from the scheduler (a first run, or after a reset): the old fiber is not running, so it can go */
    if (!sched_fib) sched_fib = IsThreadAFiber() ? GetCurrentFiber() : ConvertThreadToFiber(NULL);
    if (!sched_fib) { fprintf(stderr, "[%s] ConvertThreadToFiber failed (%lu)\n", TAG, (unsigned long)GetLastError()); exit(1); }
    if (cpu_fib) DeleteFiber(cpu_fib);
    cpu_fib = CreateFiber(STACK_SZ, fib_entry, NULL);  /* STACK_SZ reserved, committed as touched */
    if (!cpu_fib) { fprintf(stderr, "[%s] CreateFiber failed (%lu)\n", TAG, (unsigned long)GetLastError()); exit(1); }
#else
    cur_stack ^= 1;
    if (!stacks[cur_stack]) {
        stacks[cur_stack] = mmap(NULL, STACK_SZ, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (stacks[cur_stack] == MAP_FAILED) { perror("mmap"); exit(1); }
    }
    getcontext(&cpu_ctx);
    cpu_ctx.uc_stack.ss_sp = stacks[cur_stack]; cpu_ctx.uc_stack.ss_size = STACK_SZ; cpu_ctx.uc_link = NULL;
    makecontext(&cpu_ctx, entry, 0);
#endif
}

int32_t API(run)(int32_t budget)
{
    if (halted) return 0;
    if (!started) { make_entry(); started = 1; }
    rr_budget = budget0 = budget;
    TO_CPU();
    return budget0 - rr_budget;
}

int      API(halted)(void) { return halted; }
uint32_t API(reg)(int off) { return (uint32_t)RG4((uint32_t)off); }
uint32_t API(sr)(void) { return rr_get_sr(); }
void     API(set_frame)(uint32_t f) { rr_frame = f; }
uint32_t API(traps)(void) { return rr_n_traps; }
void     API(hooks)(int on) { rd_on = on; }

/* ---- the trace oracle (dev): engine/lift_env.c with this CPU's board hooks ---- */
static int env_in(uint32_t a) { return s21_env_in(S21_CPU, a & 0xFFFFFFu); }
static void env_irq(int lvl) { s21_env_irq(S21_CPU, lvl); }
static void no_keycus(uint32_t v) { (void)v; }
static const lift_env_board_t env_board = { TAG, 0, 0, no_keycus, env_in, NULL, env_irq };
#ifdef RR_TRACE
/* MAME's trace also shows where the master RESET this 68000 (env_from_trace.py's "X <n>": main-flow instruction n is the reset PC
 * again); with only this CPU running, the reset is replayed at that instruction, from inside the CPU's coroutine */
static uint64_t xs[256]; static int nxs, xpos; static uint64_t xcnt;
static void (*env_hook)(uint32_t);
static void reset_hook(uint32_t pc)
{
    if (!rr_in_irq) {
        if (xpos < nxs && xcnt == xs[xpos]) { xpos++; make_entry(); setcontext(&cpu_ctx); }
        xcnt++;
    }
    if (env_hook) env_hook(pc);
}
#endif
int API(env_init)(const char *path)
{
    env_on = lift_env_init(path, &env_board);
#ifdef RR_TRACE
    if (env_on) {
        FILE *f = fopen(path, "r"); char line[128]; unsigned long long n;
        while (f && fgets(line, sizeof line, f)) if (line[0] == 'X' && sscanf(line + 2, "%llu", &n) == 1 && nxs < 256) xs[nxs++] = n;
        if (f) fclose(f);
        env_hook = rr_trace_hook; rr_trace_hook = reset_hook;
        if (nxs) fprintf(stderr, "[%s] %d resets to replay\n", TAG, nxs);
    }
#endif
    return env_on;
}
int API(env_active)(void) { return env_on; }
