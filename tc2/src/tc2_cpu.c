/*
 * tc2_cpu.c -- the runtime of Time Crisis 2's LIFTED MIPS program (tools/lift/lift.py): the register file, the shadow return stack, traps,
 * the R4650's exception entry (one vector, 0x80000180, with SR.BEV clear) and eret, the CP0 timer (Count / Compare), and the trace hook.
 * Adapted from the System 22 project's 68K runtime (lift_cpu.c) and independent of it.
 *
 * The lifted code turns every jal into a C call, so `jr ra` needs a shadow stack to find the C frame it returns to (include/lift_rt.h).
 * MIPS pops nothing on a return: a call site matches when the return address AND the stack pointer equal what they were at the call.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lift_rt.h"
#include "tc2_mips_rt.h"
#include "tc2_lifted.h"
#include "tc2_host.h"

uint8_t R[RR_REGSPACE];
int32_t rr_budget;
uint32_t rr_pc, rr_n_traps, rr_frame;
int rr_in_irq;
uint64_t rr_n_irq;

/* the readable-C checker's hooks (the System 22 project's src/rd): not used here, the generated code still names them */
int rd_on, rd_stop_on, rd_poll_rec;
int rd_hook(uint32_t ep) { (void)ep; return 0; }
int rd_jump_stop(uint32_t t, uint32_t at) { (void)t; (void)at; return 0; }
void rd_poll_snap(void) {}

static uint32_t sp32(void) { return (uint32_t)RG4(TC2_R_SP + 4); }     /* the low half of the 64-bit sp: the address */

void rr_div0(const char *what)
{
    static unsigned n;
    if (++n <= 3) fprintf(stderr, "[TC2] %s at %08X (MIPS: the result is undefined, no exception); continuing with 0\n", what, rr_pc);
}

void rr_trap(uint32_t at, uint32_t target, const char *what)
{
    if (++rr_n_traps <= 40) fprintf(stderr, "[TRAP] f%u at 0x%08X -> 0x%08X: %s\n", rr_frame, at, target, what);
    if (rr_n_traps == 40) fprintf(stderr, "[TRAP] (further traps counted, not printed)\n");
    tc2_on_trap();
}

/* an FP op the R4650 traps on (include/lift_rt.h rr_fp_cause): the game's handler 0x80000358 clears the cause bits and jumps to the boot
 * monitor, whose FPE slot is the default (0x803F0F5C = 0xBFC015C0): its register dump and debugger -- the board stops. Reported like a trap;
 * execution goes on with MAME's result. MAME takes the same exception only for div.s by zero with Z enabled: there our run and MAME's part. */
uint32_t rr_n_fpe;
void rr_fpe(uint32_t cause, uint32_t fcsr, char op)
{
    static const char *nm[6] = { "inexact", "underflow", "overflow", "divide-by-zero", "invalid", "unimplemented (E: denormal operand / conversion)" };
    if (++rr_n_fpe <= 40) {
        char w[160] = ""; uint32_t hit = (cause & 0x20000u) | ((cause >> 5 & fcsr & 0xF80u) << 5);
        for (int k = 0; k < 6; k++) if (hit >> (12 + k) & 1) { strcat(w, w[0] ? ", " : ""); strcat(w, nm[k]); }
        const int mame = op == '/' && (hit & 0x8000u) && (fcsr & 0x400u);
        fprintf(stderr, "[FPE] f%u at 0x%08X (%c): %s -- the R4650 traps here and the game stops in the boot monitor; %s\n", rr_frame, rr_pc, op, w,
                mame ? "MAME raises it too" : "MAME does not raise it (its result is used)");
        if (rr_n_fpe == 40) fprintf(stderr, "[FPE] (further FP exceptions counted, not printed)\n");
    }
    tc2_on_trap();
}

static void fatal(int code, const char *why)
{
    fprintf(stderr, "[TC2] %s -- stopping (pc %08X, frame %u)\n", why, rr_pc, rr_frame);
    { extern int tc2_crashing; tc2_crashing = 1; }          /* tc2_on_exit: the backup RAM is NOT saved from a broken state */
    tc2_on_exit();
    { char m[256]; snprintf(m, sizeof m, "%s\n(pc %08X, frame %u)", why, rr_pc, rr_frame); void tc2_crash_box(const char *); tc2_crash_box(m); }
    exit(code);
}

/* ---- the shadow return stack ---- */
#define SHADOW_MAX 4096
#define SH_IRQ 0xFFFFFFFFu
static uint32_t sh_ret[SHADOW_MAX], sh_sp[SHADOW_MAX];
static int sh_n, sh_target = -1;
uint32_t rr_ret_to;

int rr_call_push(uint32_t ret)
{
    if (sh_n >= SHADOW_MAX) fatal(4, "shadow stack overflow");
    sh_ret[sh_n] = ret; sh_sp[sh_n] = sp32();
    return sh_n++;
}

int rr_after_call(int j)
{
    if (sh_target < 0) { char w[96]; snprintf(w, sizeof w, "call frame %d came back without a return", j); fatal(5, w); }
    if (sh_target < j) return 1;           /* unwinding further up */
    sh_target = -1;
    return 0;
}

int rr_return(uint32_t t)
{
    const uint32_t sp = sp32();
    for (int k = sh_n - 1; k >= 0; k--)    /* exact: the return address and the stack pointer of the call */
        if (sh_ret[k] == t && sh_sp[k] == sp) { sh_n = k; sh_target = k; rr_ret_to = t; return 1; }
    for (int k = sh_n - 1; k >= 0; k--)    /* the address only (a function that left sp moved) */
        if (sh_ret[k] == t) { sh_n = k; sh_target = k; rr_ret_to = t; return 1; }
    return 0;
}

/* eret, as the R4650 does it: with Status.ERL set, to ErrorEPC clearing ERL; otherwise to EPC clearing EXL (Ghidra's p-code only jumps to EPC).
 * Back from an exception the host delivered (an IRQ frame on the shadow stack): unwind to its C frame, tc2_take_exception resumes there.
 * With none, eret is a JUMP -- the boot monitor enters the game that way: EPC = 0x80000380 (_start), then eret at 0xBFC011EC. */
void rr_rte(void)
{
    const uint64_t sr = RG8(TC2_R_STATUS);
    uint32_t target;
    if (sr & 4) { target = (uint32_t)RG8(TC2_R_ERROREPC); RS8(TC2_R_STATUS, sr & ~4ULL); }
    else        { target = (uint32_t)RG8(TC2_R_EPC);      RS8(TC2_R_STATUS, sr & ~2ULL); }
    for (int k = sh_n - 1; k >= 0; k--)
        if (sh_ret[k] == SH_IRQ) { sh_n = k; sh_target = k; rr_ret_to = SH_IRQ; return; }
    tc2_trace_mark("ERET-JUMP", target);
    rr_jump(target, rr_pc);
}

/* ---- the R4650's interrupts ----
 * Taken when SR.IE = 1, SR.EXL = SR.ERL = 0 and (Cause.IP & SR.IM) != 0: EPC = the instruction about to run, Cause.ExcCode = 0 (Int),
 * SR.EXL = 1, and the general exception vector runs (0x80000180; 0xBFC00380 with SR.BEV set). The lifted code polls at calls and backward
 * branches, so an interrupt lands there -- between instructions, as on the CPU, but only at those points. */
static int irq_pending(void)
{
    const uint64_t sr = RG8(TC2_R_STATUS), cause = RG8(TC2_R_CAUSE);
    return (sr & 1) && !(sr & 6) && ((cause & sr) & 0xFF00);
}

static void take_exception(void)
{
    const uint64_t sr = RG8(TC2_R_STATUS);
    RS8(TC2_R_EPC, (uint64_t)(int64_t)(int32_t)rr_pc);
    RS8(TC2_R_CAUSE, RG8(TC2_R_CAUSE) & ~0x7CULL);                    /* ExcCode = 0: an interrupt */
    RS8(TC2_R_STATUS, sr | 2);                                        /* EXL */
    const uint32_t vec = (sr & (1u << 22)) ? 0xBFC00380u : 0x80000180u;
    rr_n_irq++; rr_in_irq++;
    tc2_trace_mark("IRQ", (uint32_t)RG8(TC2_R_CAUSE));
    if (sh_n >= SHADOW_MAX) fatal(4, "shadow stack overflow");
    sh_ret[sh_n] = SH_IRQ; sh_sp[sh_n] = sp32(); const int j = sh_n++;
    rr_jump(vec, rr_pc);
    if (sh_target != j) fatal(6, "the exception handler did not end in eret");
    sh_target = -1;
    rr_in_irq--;
    tc2_trace_mark("ERET", 0);
}

/* ---- CP0: the timer. Count runs at half the pipeline clock; Count == Compare raises IP7 (Cause bit 15); writing Compare clears it. ---- */
static uint64_t count_frac;                     /* instructions not yet turned into Count ticks */
void tc2_advance_count(uint32_t instructions)
{
    count_frac += instructions;
    const uint32_t ticks = (uint32_t)(count_frac / 2); count_frac %= 2;
    const uint32_t c0 = (uint32_t)RG8(TC2_R_COUNT), cmp = (uint32_t)RG8(TC2_R_COMPARE);
    const uint32_t c1 = c0 + ticks;
    if ((uint32_t)(cmp - c0 - 1) < ticks)       /* Count passed Compare in this step */
        tc2_irq_line(0x8000, 1);
    RS8(TC2_R_COUNT, (uint64_t)(int64_t)(int32_t)c1);
}

/* THE INTERRUPT LINES: Cause.IP2..IP7 (0xFC00) are the CPU's interrupt PINS, read-only (the R4650 manual; MAME's set_cop0_reg keeps them:
 * CAUSE = (CAUSE & 0xfc00) | (val & ~0xfc00)). The host's devices drive them through tc2_irq_line; an mtc0 to Cause changes only the rest
 * (the software bits IP0/IP1) -- the game's exception entry does `mtc0 zero,Cause` (0x80000284), which must not drop a pending line. */
static uint32_t irq_lines;                                             /* 0xFC00: IP2 vblank, IP3 C361/sub-CPU, IP4 C435, IP5 C422, IP7 timer */
static uint32_t irq_src[2];                                            /* who drives them: 0 the main board's devices, 1 the sub-CPU (IP3, shared
                                                                        * with the C361: MAME's irq_update ORs m_main_irqcause) */
static void irq_apply(void)
{
    irq_lines = irq_src[0] | irq_src[1];
    RS8(TC2_R_CAUSE, (RG8(TC2_R_CAUSE) & ~0xFC00ULL) | irq_lines);
}
void tc2_irq_source(int src, uint32_t bit, int on) { irq_src[src] = on ? (irq_src[src] | bit) : (irq_src[src] & ~bit); irq_apply(); }
void tc2_irq_line(uint32_t bit, int on) { tc2_irq_source(0, bit, on); }
int tc2_irq_pending(uint32_t bit) { return (irq_src[0] & bit) != 0; }   /* the main board's own request (the C361's, for its status reads) */
void tc2_irq_sync_from_cause(void) { irq_src[0] = (uint32_t)RG8(TC2_R_CAUSE) & 0xFC00u; irq_src[1] = 0; irq_apply(); }   /* a snapshot's lines, as MAME's Cause shows them */

void rr_mips_cop0_write(uint32_t pc, uint32_t off)
{
    (void)pc;
    if (off == TC2_R_COMPARE) tc2_irq_line(0x8000, 0);                    /* writing Compare acknowledges the timer */
    if (off == TC2_R_CAUSE) RS8(TC2_R_CAUSE, (RG8(TC2_R_CAUSE) & ~0xFC00ULL) | irq_lines);   /* the pins are read-only */
}
void rr_mips_setCopReg(uint32_t pc, uint64_t sel, uint64_t reg, uint64_t val) { (void)sel; (void)reg; (void)val; rr_trap(pc, 0, "mtc0 to an unnamed register"); }
void rr_mips_cacheOp(uint32_t pc, uint64_t op, uint64_t addr) { (void)pc; (void)op; (void)addr; }   /* no cache to model */
void rr_mips_setISAMode(uint32_t pc, uint64_t mode) { (void)pc; (void)mode; }                         /* no MIPS16 on the R4650 */
void rr_mips_trap(uint32_t pc, uint64_t code) { rr_trap(pc, (uint32_t)code, "trap instruction"); }
void rr_mips_SYNC(uint32_t pc, uint64_t stype) { (void)pc; (void)stype; }

/* the scheduler: every poll once the budget is spent -- time advances, the host's devices step, interrupts are delivered */
static int32_t granted = TC2_SLICE;
static uint64_t cycles_done;                                           /* cycles of finished slices */
uint64_t tc2_cycles_now(void) { return cycles_done + (uint64_t)(granted - rr_budget); }   /* the exact cycle, mid-slice too */

void rr_tick(void)
{
    const int32_t used = granted - rr_budget;
    cycles_done += (uint64_t)used;
    tc2_advance_count((uint32_t)used);
    tc2_host_step((uint32_t)used);
    if (rr_in_irq == 0 && irq_pending()) take_exception();
    const uint32_t ev = tc2_until_event();                             /* the next poll lands on the next interrupt event, not up to a slice late */
    granted = (int32_t)(ev && ev < TC2_SLICE ? ev : TC2_SLICE);
    rr_budget = granted;
}


void tc2_poll_irq(void) { if (rr_in_irq == 0 && irq_pending()) take_exception(); }
