/*
 * rd_core.c -- dispatch and checking for the readable-C replacements (rd.h).
 *
 * RR_RD=1      run each replacement instead of its lifted function (default)
 * RR_RD=0      lifted code only (exact; what the MAME instruction diff needs)
 * RR_RD=check  run BOTH on every call and compare -- see rd.h for the rules
 * RR_RD=census count calls per lifted function (to choose what to convert)
 *
 * Check mode, per call:
 *   1. snapshot R[] and the shadow stack, start the memory write journal, and
 *      hold off the scheduler (a big rr_budget: no RR_POLL tick fires)
 *   2. run the LIFTED function (the hook is bypassed for this one call)
 *   3. roll memory, R[] and the shadow stack back; run the READABLE one
 *   4. compare; then put the LIFTED result back, so the run continues exactly
 *      as the lifted build would (only the tick is deferred to the call's end)
 * Memory and polygon RAM writes are journalled. An I/O WRITE cannot be rolled
 * back: such a call is counted "unverifiable" (the lifted result stands).
 * I/O READS are allowed -- nothing advances between the two runs, so a read
 * with a side effect shows up as a mismatch rather than slipping through.
 */
#include <setjmp.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rd.h"
#include "rr_hw.h"

void rr_shadow_save(int *n, int *target, uint32_t *ret_to);
void rr_shadow_load(int n, int target, uint32_t ret_to);
void rr_jump(uint32_t pc, uint32_t at);
extern int rd_journal_on;

int rd_on;
int rd_real;             /* 1 while the REAL (non-fuzz) pair runs: for counters in rd_funcs */
int rd_jmp_poll;
/* COVERAGE (trace build, rr_trace): which lifted instructions ran inside checked
 * calls. A pass only vouches for code that ran -- the first planted bug in
 * FUN_00027062's loop body passed because no slot was ever enabled in the run.
 * RR_RD_COV=<file> writes the covered addresses; tools/rd_coverage.py reports
 * them per replaced function. */
int rd_cov_on;
static uint8_t *cov_bits;                  /* one bit per ROM halfword */
void rd_cov_ins(uint32_t pc) { if (pc < RR_ROM_SIZE) cov_bits[pc >> 4] |= (uint8_t)(1u << ((pc >> 1) & 7)); }
static int mode;                 /* 0 off, 1 run, 2 check, 3 census */
static uint32_t bypass_ep;       /* check mode: let this entry's lifted body run */
static int in_check;             /* a replaced function called inside a check runs lifted */

/* ---- write journal ---- */
typedef struct { vaddr_t a; uint8_t *p; uint8_t old; } jr_t;
static jr_t *jr; static int jr_n, jr_cap;
void rd_journal_byte(vaddr_t a, uint8_t *p)
{
    if (jr_n == jr_cap) { jr_cap = jr_cap ? jr_cap * 2 : 4096; jr = realloc(jr, (size_t)jr_cap * sizeof *jr); }
    jr[jr_n].a = a; jr[jr_n].p = p; jr[jr_n].old = *p; jr_n++;
}
static void jr_undo(int from) { for (int i = jr_n - 1; i >= from; i--) *jr[i].p = jr[i].old; jr_n = from; }

/* final effect of one run: (address, byte) for every byte it wrote, first-old kept */
typedef struct { vaddr_t a; uint8_t *p; uint8_t old, val; } eff_t;
static int eff_cmp(const void *x, const void *y)
{
    vaddr_t a = ((const eff_t *)x)->a, b = ((const eff_t *)y)->a;
    return (a > b) - (a < b);
}
static eff_t *collect(int from, int *n)
{
    int k = jr_n - from;
    eff_t *e = malloc((size_t)(k ? k : 1) * sizeof *e);
    int m = 0;
    for (int i = from; i < jr_n; i++) { e[m].a = jr[i].a; e[m].p = jr[i].p; e[m].old = jr[i].old; e[m].val = *jr[i].p; m++; }
    qsort(e, (size_t)m, sizeof *e, eff_cmp);
    int u = 0;                                   /* dedup: keep the FIRST old, the current value */
    for (int i = 0; i < m; i++) {
        if (u && e[u - 1].a == e[i].a) continue;
        e[u++] = e[i];
    }
    *n = u;
    return e;
}

/* ---- registry: tables from every src/rd file, merged in rd_init ---- */
const rd_entry *rd_table;
int rd_count;
static struct { const rd_entry *t; int n; } regs[64];
static int nregs;
void rd_register(const rd_entry *t, int n)
{
    if (nregs == 64) { fprintf(stderr, "[RD] too many rd tables\n"); exit(7); }
    regs[nregs].t = t; regs[nregs].n = n; nregs++;
}
static int ep_cmp(const void *a, const void *b)
{
    uint32_t x = ((const rd_entry *)a)->ep, y = ((const rd_entry *)b)->ep;
    return x < y ? -1 : x > y;
}
/* RR_RD_SKIP / RR_RD_ONLY: comma- or space-separated hex entry points, matched
 * EXACTLY (a bisecting aid: leave some replacements lifted, or run only some) */
static int ep_listed(const char *list, uint32_t ep)
{
    const char *p = list;
    while (*p) {
        char *end;
        unsigned long v = strtoul(p, &end, 16);
        if (end == p) { p++; continue; }
        if ((uint32_t)v == ep) return 1;
        p = end;
    }
    return 0;
}
static void merge_tables(void)
{
    int n = 0;
    for (int i = 0; i < nregs; i++) n += regs[i].n;
    rd_entry *all = malloc((size_t)(n ? n : 1) * sizeof *all);
    n = 0;
    const char *skip = getenv("RR_RD_SKIP"), *only = getenv("RR_RD_ONLY");
    for (int i = 0; i < nregs; i++)
        for (int k = 0; k < regs[i].n; k++) {
            uint32_t ep = regs[i].t[k].ep;
            if (skip && ep_listed(skip, ep)) continue;
            if (only && !ep_listed(only, ep)) continue;
            all[n++] = regs[i].t[k];
        }
    qsort(all, (size_t)n, sizeof *all, ep_cmp);
    rd_table = all; rd_count = n;
}

/* ---- registry lookup ---- */
typedef struct { long seen, calls, pass, fail, unver, fz, fz_fail, fz_unver, cost_bad; double cost_sum; long cost_n; int cost_min, cost_max, disabled; } rd_stat;
static rd_stat *st;
static const rd_entry *find(uint32_t ep, int *idx)
{
    int lo = 0, hi = rd_count - 1;
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        if (rd_table[m].ep == ep) { *idx = m; return &rd_table[m]; }
        if (rd_table[m].ep < ep) lo = m + 1; else hi = m - 1;
    }
    return NULL;
}

/* ---- census ---- */
#define CEN_N 65536
static uint32_t cen_ep[CEN_N]; static long cen_calls[CEN_N];
static void census(uint32_t ep)
{
    uint32_t h = (ep * 2654435761u) & (CEN_N - 1);
    while (cen_ep[h] && cen_ep[h] != ep) h = (h + 1) & (CEN_N - 1);
    cen_ep[h] = ep; cen_calls[h]++;
}

/* the lifted epilogue of a replaced function: rts */
static void do_rts(uint32_t ep)
{
    uint32_t sp = a_reg(7), t = rr_read(sp, 4);
    set_a(7, sp + 4);
    RS4(0x50, t);
    if (rr_return(t)) return;
    RR_POLL();
    rr_jump(t, ep);
}

static const char *regname(int b) { static char s[4]; snprintf(s, sizeof s, "%c%d", b < 8 ? 'D' : 'A', b < 8 ? b : b - 8); return s; }

/* ---- I/O writes: CAPTURED during a check (rr_mem.c), not performed -- both runs'
 * sequences must match (address, size, value, order), then the lifted run's is
 * replayed for real, once. An I/O read after a captured write is unverifiable
 * (the suppressed write could have changed it). */
int rd_io_touched, rd_io_written;
typedef struct { vaddr_t a; int size; uint32_t v; } iow_t;
static iow_t iol[256]; static int iol_n;
void rd_io_log(vaddr_t a, int size, uint32_t v)
{
    rd_io_written = 1;
    if (iol_n < 256) { iol[iol_n].a = a; iol[iol_n].size = size; iol[iol_n].v = v; }
    iol_n++;
}

/* I/O READS are stateful on this board (keycus advances a generator, the serial
 * port bits shift), so they happen ONCE, in the lifted run, logged; the readable
 * run is served the same values and must make the same reads in the same order.
 * Fuzz probes restore g_hw around themselves. */
int rd_ior_replay;
static iow_t ior[256]; static int ior_n, ior_i, ior_bad;
void rd_ior_log(vaddr_t a, int size, uint32_t v)
{
    if (ior_n < 256) { ior[ior_n].a = a; ior[ior_n].size = size; ior[ior_n].v = v; }
    ior_n++;
}
uint32_t rd_ior_serve(vaddr_t a, int size)
{
    if (ior_i >= ior_n || ior_i >= 256 || ior[ior_i].a != a || ior[ior_i].size != size) { ior_bad = 1; ior_i++; return 0; }
    return ior[ior_i++].v;
}

/* ---- stop-at-jump: a TAIL JUMP out of the function under test ends its run --
 * the jump target (another routine; for a dispatcher, a whole state handler)
 * is not part of the function, so both runs are compared AT the jump, and the
 * real call then makes the jump once. rr_jump (gen/rr_lifted.c) asks here
 * first; the jump belongs to the function under test when the instruction it
 * comes from is owned by the same lifted function. */
int rd_stop_on;
static void (*stop_fn)(uint32_t); static uint32_t stop_target; static int stopped;
extern void (*rd_lifted_entry(uint32_t ep))(uint32_t);
static int stop_depth;      /* shadow-stack depth when the checked run started */
int rd_jump_stop(uint32_t t, uint32_t at)
{
    if (!stop_fn || stopped || rd_lifted_entry(at) != stop_fn) return 0;
    /* only the function's OWN jump: not one inside a callee whose code a lifted
     * function shares (Ghidra's function ranges overlap -- FUN_0001e5c4 / 1e62c) */
    { int n, tg; uint32_t rt; rr_shadow_save(&n, &tg, &rt); if (n != stop_depth) return 0; }
    stopped = 1; stop_target = t;
    return 1;
}

/* ---- read log (for fuzzing: which RAM bytes a lifted run looked at) ---- */
int rd_read_log, rd_quiet;
typedef struct { vaddr_t a; uint8_t *p; } rd_t;
static rd_t *rl; static int rl_n, rl_cap;
void rd_read_byte(vaddr_t a, uint8_t *p)
{
    if (rl_n == rl_cap) { rl_cap = rl_cap ? rl_cap * 2 : 1024; rl = realloc(rl, (size_t)rl_cap * sizeof *rl); }
    rl[rl_n].a = a; rl[rl_n].p = p; rl_n++;
}

/* fuzz dictionary: byte values the function read from ROM (tables, sequences
 * it compares against) -- a quarter of mutations draw from it, so an equality
 * test against a table entry is reachable (FUN_00004988's sequence match) */
static uint8_t dict[256]; static int dict_n;
void rd_dict_byte(uint8_t v)
{
    for (int i = 0; i < dict_n; i++) if (dict[i] == v) return;
    if (dict_n < 256) dict[dict_n++] = v;
}

typedef struct { int bad, io, cost, rcost; uint32_t jump; int jpoll; char why[512]; } pair_res;

/* Run the LIFTED function, roll back, run the READABLE one, compare. Both runs
 * start from the current state; the journal keeps everything above `mark`.
 * keep_lifted: leave the lifted result in place (the real call); otherwise
 * leave the state exactly as it was before the pair (a fuzz probe). */
/* ---- registers at every poll (see RR_POLL): the lifted run's sequence and the
 * readable run's must match -- an interrupt can land at any of them ---- */
int rd_poll_rec;                    /* 1 = recording into set A (lifted), 2 = set B (readable) */
#define POLL_CAP 2048
static uint32_t pollA[POLL_CAP][15], pollB[POLL_CAP][15];
static int npollA, npollB;
void rd_poll_snap(void)
{
    int *n = rd_poll_rec == 1 ? &npollA : &npollB;
    uint32_t (*buf)[15] = rd_poll_rec == 1 ? pollA : pollB;
    if (*n < POLL_CAP) for (int r = 0; r < 15; r++) buf[*n][r] = (uint32_t)RG4((uint32_t)(r < 8 ? r * 4 : 0x20 + (r - 8) * 4));
    (*n)++;
}

/* (Also called by rr_trap and by rr_after_call on a plain C return: a probe that
 * traps is abandoned the same way.)
 * A probe (a fuzz round or the learning pair) runs on MUTATED or rolled-back
 * state, and a mutation can send a loop round forever (a scan that never meets
 * its terminator). The instruction budget would then run out and rr_tick run
 * whole frames -- DSP, sound, the next frames of game -- INSIDE the probe, which
 * never returns: the checked function never completes and the run is lost. So a
 * probe gets PROBE_CAP instructions; rr_tick calls rd_budget_out(), which
 * unwinds out of a probe, and the probe is counted unverifiable. Real calls keep
 * the unbounded budget. */
#define PROBE_CAP (1 << 20)
static jmp_buf probe_jb;
static int probe_live;
long rd_probe_timeouts;
void rd_budget_out(void)
{
    if (probe_live) { probe_live = 0; longjmp(probe_jb, 1); }
}

static pair_res run_pair(const rd_entry *e, int keep_lifted)
{
    pair_res res = { 0, 0, 0, 0, 0, 0, "" };
    static uint8_t R0[RR_REGSPACE], RL[RR_REGSPACE];
    int sn0, stg0, snL, stgL; uint32_t srt0, srtL;
    memcpy(R0, R, sizeof R0);
    rr_shadow_save(&sn0, &stg0, &srt0);
    int32_t budget0 = rr_budget;
    const int32_t BIG = keep_lifted ? 1 << 30 : PROBE_CAP;
    int mark = jr_n;
    if (!keep_lifted) {
        if (setjmp(probe_jb)) {                          /* the probe ran past PROBE_CAP */
            rd_stop_on = 0; stop_fn = 0; rd_cov_on = 0; rd_read_log = 0;
            rd_ior_replay = 0; rd_jmp_poll = 0; rd_poll_rec = 0;
            jr_undo(mark);
            memcpy(R, R0, sizeof R0);
            rr_shadow_load(sn0, stg0, srt0);
            rr_budget = budget0;
            rd_probe_timeouts++;
            res.io = 1;
            snprintf(res.why, sizeof res.why, "probe trapped or ran past %d instructions", PROBE_CAP);
            return res;
        }
        probe_live = 1;
    }

    /* lifted */
    rd_io_touched = 0; rd_io_written = 0; iol_n = 0; rr_budget = BIG;
    ior_n = 0; rd_ior_replay = 0;
    bypass_ep = e->ep;
    extern void (*rd_lifted_entry(uint32_t ep))(uint32_t);
    void (*lf)(uint32_t) = rd_lifted_entry(e->ep);
    if (!lf) { fprintf(stderr, "[RD] no lifted function at %06X\n", (unsigned)e->ep); exit(7); }
    if (cov_bits) rd_cov_on = 1;
    stop_fn = lf; stopped = 0; rd_stop_on = 1; stop_depth = sn0;
    npollA = 0; rd_poll_rec = 1;
    lf(e->ep);
    rd_poll_rec = 0;
    rd_stop_on = 0; stop_fn = 0;
    uint32_t ljump = stopped ? stop_target : 0;
    rd_cov_on = 0; rd_read_log = 0;
    res.cost = BIG - rr_budget;
    res.io = rd_io_touched;
    memcpy(RL, R, sizeof RL);
    rr_shadow_save(&snL, &stgL, &srtL);
    int nA; eff_t *A = collect(mark, &nA);
    static iow_t iolA[256]; int iolA_n = iol_n;
    memcpy(iolA, iol, sizeof iol);

    /* roll back, readable */
    jr_undo(mark);
    memcpy(R, R0, sizeof R0);
    rr_shadow_load(sn0, stg0, srt0);
    rd_io_touched = 0; rd_io_written = 0; iol_n = 0; rr_budget = BIG;
    ior_i = 0; ior_bad = 0; rd_ior_replay = 1;
    rd_jmp_poll = 0;
    npollB = 0; rd_poll_rec = 2;
    uint32_t tj = e->fn();
    res.jpoll = rd_jmp_poll; rd_jmp_poll = 0;
    /* a jump that polls first (computed jmp, tail jump): the lifted run polls right
     * before it, the readable run's poll is made later by rd_hook -- same registers */
    if (tj && tj != RD_UNWIND && res.jpoll) rd_poll_snap();
    rd_poll_rec = 0;
    if (!tj) {                                         /* the rts, as the lifted epilogue does it */
        uint32_t sp = a_reg(7), t = rr_read(sp, 4);
        set_a(7, sp + 4);
        RS4(0x50, t);
        /* an rts whose address is not a call site on the shadow stack is a JUMP
         * (the lifted run stops at it, rr_jump -> rd_jump_stop): compared as one,
         * not made -- a probe that overwrote the return address must not run it */
        if (!rr_return(t)) { tj = t; res.jpoll = 1; rd_poll_rec = 2; rd_poll_snap(); rd_poll_rec = 0; }
    }
    if (tj == RD_UNWIND) tj = 0;                       /* unwound: compared by the shadow stack */
    res.rcost = (BIG - rr_budget) + e->cost;
    rd_ior_replay = 0;
    res.io |= rd_io_touched;
    if (ior_n > 256) res.io = 1;
    else if (!res.bad && (ior_bad || ior_i != ior_n)) {
        /* in a PROBE (a mutated pointer landing on a device) the read COUNT can differ
         * only by Ghidra's p-code, which reads a memory operand twice for flags where
         * the 68K reads once (tst.w, add.w to memory): unverifiable there. A real call
         * is compared strictly. */
        if (!keep_lifted) res.io = 1;
        else { res.bad = 1; snprintf(res.why, sizeof res.why, "I/O reads differ (%d made, lifted %d)", ior_i, ior_n); }
    }
    res.jump = ljump;
    if (tj != ljump) { res.bad = 1; snprintf(res.why, sizeof res.why, "leaves by %s %06X, lifted by %s %06X",
                                              tj ? "jump to" : "rts", (unsigned)tj, ljump ? "jump to" : "rts", (unsigned)ljump); }
    int nB; eff_t *B = collect(mark, &nB);

    if (iolA_n > 256 || iol_n > 256) res.io = 1;       /* more I/O than the log holds */
    else if (!res.io && !res.bad) {
        if (iol_n != iolA_n) { res.bad = 1; snprintf(res.why, sizeof res.why, "%d I/O writes, lifted %d", iol_n, iolA_n); }
        for (int k = 0; k < iol_n && !res.bad; k++)
            if (iol[k].a != iolA[k].a || iol[k].size != iolA[k].size || iol[k].v != iolA[k].v) {
                res.bad = 1;
                snprintf(res.why, sizeof res.why, "I/O write %d: %08X <- %X (size %d), lifted %08X <- %X (size %d)", k,
                         (unsigned)iol[k].a, (unsigned)iol[k].v, iol[k].size, (unsigned)iolA[k].a, (unsigned)iolA[k].v, iolA[k].size);
            }
    }

    /* compare */
    if (!res.io && !res.bad) {
        int snB, stgB; uint32_t srtB;
        rr_shadow_save(&snB, &stgB, &srtB);
        if (snB != snL || stgB != stgL || srtB != srtL) { res.bad = 1; snprintf(res.why, sizeof res.why, "return path differs (shadow %d/%d ret %08X vs lifted %d/%d %08X)", snB, stgB, srtB, snL, stgL, srtL); }
        for (int b = 0; b < 14 && !res.bad; b++) {
            uint32_t off = b < 8 ? (uint32_t)b * 4 : 0x20 + (uint32_t)(b - 8) * 4;
            uint32_t vB = (uint32_t)RG4(off);
            uint32_t v0 = (uint32_t)(R0[off] << 24 | R0[off + 1] << 16 | R0[off + 2] << 8 | R0[off + 3]);
            uint32_t vL = (uint32_t)(RL[off] << 24 | RL[off + 1] << 16 | RL[off + 2] << 8 | RL[off + 3]);
            /* outside the kill mask the register must be preserved -- unless the LIFTED
             * run changed it too: Ghidra's mask cannot see through an indirect call
             * (FUN_0000f4da's sub-state handler sets A4), so then it must match lifted */
            if (!(e->kill & (1u << b)) && vB != v0 && vL == v0) { res.bad = 1; snprintf(res.why, sizeof res.why, "%s not preserved: %08X, was %08X", regname(b), vB, v0); }
            else if (!(e->scratch & (1u << b)) && vB != vL) { res.bad = 1; snprintf(res.why, sizeof res.why, "%s = %08X, lifted %08X", regname(b), vB, vL); }
        }
        {   /* RR_RD_FLAGS=1: report (do not fail) N/Z/V/C differing from the lifted run --
             * a caller may branch on them right after the call (bsr ; beq) */
            static int fl = -1; if (fl < 0) fl = getenv("RR_RD_FLAGS") != NULL;
            if (fl && keep_lifted && memcmp(&R[0x44], &RL[0x44], 4)) {
                static uint32_t seen[4096]; static int ns; int k;
                for (k = 0; k < ns && seen[k] != e->ep; k++) ;
                if (k == ns && ns < 4096) { seen[ns++] = e->ep;
                    fprintf(stderr, "[RD] FLAGS %s (%06X): NZVC %d%d%d%d, lifted %d%d%d%d\n", e->name, (unsigned)e->ep,
                            R[0x44], R[0x45], R[0x46], R[0x47], RL[0x44], RL[0x45], RL[0x46], RL[0x47]); }
            }
        }
        if (!res.bad && keep_lifted) {           /* registers at every poll, in order (real calls) */
            if (npollA != npollB) { res.bad = 1; snprintf(res.why, sizeof res.why, "%d polls, lifted %d", npollB, npollA); }
            for (int k = 0; k < npollA && k < POLL_CAP && !res.bad; k++)
                for (int r = 0; r < 15; r++)
                    if (pollA[k][r] != pollB[k][r]) {
                        res.bad = 1;
                        snprintf(res.why, sizeof res.why, "at poll %d of %d: %s = %08X, lifted %08X (registers must be live at a poll: an interrupt saves them)",
                                 k + 1, npollA, r < 14 ? regname(r) : "A6", pollB[k][r], pollA[k][r]);
                        break;
                    }
        }
        for (uint32_t off = 0x38; off <= 0x3C && !res.bad; off += 4) {          /* A6, SP */
            uint32_t vB = (uint32_t)RG4(off), vL = (uint32_t)(RL[off] << 24 | RL[off + 1] << 16 | RL[off + 2] << 8 | RL[off + 3]);
            if (vB != vL) { res.bad = 1; snprintf(res.why, sizeof res.why, "%s = %08X, lifted %08X", off == 0x3C ? "SP" : "A6", vB, vL); }
        }
        uint32_t spx = (uint32_t)(RL[0x3C] << 24 | RL[0x3D] << 16 | RL[0x3E] << 8 | RL[0x3F]);
        int i = 0, j = 0;
        while (!res.bad && (i < nA || j < nB)) {
            vaddr_t a; uint8_t va, vb;
            if (j >= nB || (i < nA && A[i].a < B[j].a)) { a = A[i].a; va = A[i].val; vb = A[i].old; i++; }   /* only the lifted run wrote it */
            else if (i >= nA || B[j].a < A[i].a)    { a = B[j].a; vb = B[j].val; va = B[j].old; j++; }
            else { a = A[i].a; va = A[i].val; vb = B[j].val; i++; j++; }
            if (a < spx && a >= spx - 0x10000u) continue;                       /* dead stack */
            if (va != vb) { res.bad = 1; snprintf(res.why, sizeof res.why, "memory %08X = %02X, lifted %02X", (unsigned)a, vb, va); }
        }
    }

    /* leave the state */
    jr_undo(mark);
    if (keep_lifted) {
        for (int k = 0; k < nA; k++) *A[k].p = A[k].val;
        int jo = rd_journal_on; rd_journal_on = 0;          /* now perform the lifted I/O writes, once */
        for (int k = 0; k < iolA_n && k < 256; k++) rr_write(iolA[k].a, iolA[k].size, iolA[k].v);
        rd_journal_on = jo;
        memcpy(R, RL, sizeof RL);
        rr_shadow_load(snL, stgL, srtL);
        rr_budget = budget0 - res.cost;
    } else {
        memcpy(R, R0, sizeof R0);
        rr_shadow_load(sn0, stg0, srt0);
        rr_budget = budget0;
    }
    free(A); free(B);
    probe_live = 0;
    return res;
}

/* ---- fuzzing (RR_RD_FUZZ=<n>): n mutated re-runs of each real call ---- */
static int fuzz_n;
static uint32_t rng = 0x9E3779B9u;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

static void fuzz(const rd_entry *e, rd_stat *s)
{
    static int trace = -1;
    if (trace < 0) trace = getenv("RR_RD_FUZZTRACE") != NULL;
    if (trace) fprintf(stderr, "[FZ] %s\n", e->name);
    /* what the lifted run READ: learned from one quiet, rolled-back pair */
    rr_hw_t hw_learn = g_hw;
    rl_n = 0; dict_n = 0; rd_read_log = 1;
    extern long rr_ncalls;
    long nc0 = rr_ncalls;
    run_pair(e, 0);
    rd_read_log = 0;
    g_hw = hw_learn;
    /* Only LEAF calls are fuzzed. A mutated caller sends its callees down paths
     * nothing bounds (tail jumps into whole state handlers), and their side
     * effects outside the journal pulled the real run off course (a different
     * song, the race 500 frames late). A caller is still checked on every
     * real call; its callees are fuzzed at their own level. */
    if (rr_ncalls != nc0) return;
    uint32_t sp = a_reg(7);
    static uint8_t Rsave[RR_REGSPACE];
    rr_hw_t hw_save = g_hw;                  /* fuzz probes' I/O reads must not advance the board */
    /* a function the run calls only a handful of times gets 16x the rounds on
     * those calls -- FUN_0001b2f0 is called once in the drive */
    int rounds = fuzz_n * (s->calls < 4 ? 16 : 1);
    /* the function's own word COMPARE immediates (cmpi.w #imm,<ea>, opcode 0x0C40..0x0C7F),
     * scanned from the entry to the first rts: a range test like `0x41A <= x <= 0x4B0`
     * is never met by interesting or random values, only by its bounds (+-1) */
    uint16_t wd[32]; int wd_n = 0;
    for (uint32_t pc = e->ep; pc < e->ep + 0x200u && wd_n < 32; pc += 2) {
        uint16_t op = (uint16_t)rr_read(pc, 2);
        if (op == 0x4E75) break;
        if ((op & 0xFFC0) == 0x0C40 && (op & 0x3F) < 0x3C) wd[wd_n++] = (uint16_t)rr_read(pc + 2, 2);
    }
    for (int k = 0; k < rounds; k++) {
        memcpy(Rsave, R, sizeof Rsave);
        int mark = jr_n;
        if (rl_n > 4096) rl_n = 4096;    /* the log grows across rounds; keep it bounded */
        for (int i = 0; i < rl_n; i++) {                     /* every writable byte it read, not the stack frame */
            vaddr_t a = rl[i].a;
            if (a >= sp - 0x1000u && a < sp + 8u) continue;
            if (k & 1) {
                /* odd rounds: WORD mutations -- an aligned word set to an interesting
                 * 16-bit value (a test like tst.w needs both bytes at once) */
                if ((a & 1) || i + 1 >= rl_n || rl[i + 1].a != a + 1 || (rnd() & 1)) {
                    /* a quarter of the bytes not word-mutated get a byte mutation too, so a
                     * path gated on a word match AND a flag bit (FUN_0001811a) is reachable */
                    if ((rnd() & 3) == 0) goto byte_mut;
                    continue;
                }
                static const uint16_t iw[] = { 0, 1, 2, 3, 4, 0xFFFF, 0x7FFF, 0x8000 };
                uint32_t pk = rnd() % 3u;
                uint16_t w;
                if (pk == 0 && wd_n) w = (uint16_t)(wd[rnd() % (uint32_t)wd_n] + (int)(rnd() % 3) - 1);
                else if (pk == 1) {
                    /* another aligned word this run READ: a compare of two memory words
                     * (cmp.w (a),(b)) is only met when one is copied onto the other */
                    int j = (int)(rnd() % (uint32_t)rl_n);
                    if (!(rl[j].a & 1) && j + 1 < rl_n && rl[j + 1].a == rl[j].a + 1)
                        w = (uint16_t)(*rl[j].p << 8 | *rl[j + 1].p);
                    else w = iw[rnd() % (uint32_t)(sizeof iw / sizeof iw[0])];
                }
                else w = iw[rnd() % (uint32_t)(sizeof iw / sizeof iw[0])];
                rd_journal_byte(a, rl[i].p);         *rl[i].p = (uint8_t)(w >> 8);
                rd_journal_byte(a + 1, rl[i + 1].p); *rl[i + 1].p = (uint8_t)w;
                continue;
            }
            if (rnd() & 1) {
            byte_mut:
                rd_journal_byte(a, rl[i].p);
                static const uint8_t interesting[] = { 0, 1, 2, 3, 4, 7, 8, 0x0F, 0x10, 0x7F, 0x80, 0xFE, 0xFF };
                uint32_t pick = rnd() & 3;
                *rl[i].p = (pick == 0 && dict_n) ? dict[rnd() % (uint32_t)dict_n]
                         : (pick == 1) ? interesting[rnd() % (uint32_t)sizeof interesting]
                         : (uint8_t)rnd();
            }
        }
        /* the DIP switches are read through the I/O hook (rr_hw.c), from g_hw.dsw */
        for (int b = 0; b < 4; b++) if (rnd() & 1) {
            uint8_t *p = (uint8_t *)&g_hw.dsw + b;
            rd_journal_byte(0x50000000u + (uint32_t)b, p); *p = (uint8_t)rnd();
        }
        for (int d = 0; d < 8; d++) {                        /* data registers (never A: they are pointers) */
            uint32_t r = rnd();
            if ((r & 3) == 0) set_d(d, rnd());
            else if ((r & 3) == 1) set_d(d, (uint32_t)((int32_t)(rnd() % 65) - 32));
        }
        rd_quiet = 1;
        rd_read_log = 1;                 /* keep learning: a mutation can open a path that reads more */
        pair_res fr = run_pair(e, 0);
        rd_quiet = 0;
        jr_undo(mark);
        memcpy(R, Rsave, sizeof Rsave);
        g_hw = hw_save;
        s->fz++;
        if (fr.io) s->fz_unver++;
        else if (fr.bad) {
            s->fz_fail++;
            if (s->fz_fail <= 3) fprintf(stderr, "[RD] FUZZ MISMATCH %s (%06X): %s\n", e->name, (unsigned)e->ep, fr.why);
        }
    }
}

/* RR_RD_LEAKCHECK=1: a fuzz round must leave NO trace. Hash the whole machine
 * state the 68K can see (every RAM / register block of g_rr, the CPU register
 * space, g_hw, the instruction budget, the shadow stack) before and after each
 * fuzz round and name the first function whose probes changed it. */
static uint64_t state_hash(void)
{
    uint64_t h = 1469598103934665603ull;
    const uint8_t *b = (const uint8_t *)&g_rr + offsetof(rr_sys_t, wram);
    size_t n = offsetof(rr_sys_t, n_unmapped) - offsetof(rr_sys_t, wram);
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 1099511628211ull;
    for (size_t i = 0; i < RR_REGSPACE; i++) h = (h ^ R[i]) * 1099511628211ull;
    b = (const uint8_t *)&g_hw;
    for (size_t i = 0; i < sizeof g_hw; i++) h = (h ^ b[i]) * 1099511628211ull;
    int sn, stg; uint32_t srt;
    rr_shadow_save(&sn, &stg, &srt);
    h = (h ^ (uint64_t)(uint32_t)rr_budget) * 1099511628211ull;
    h = (h ^ (uint64_t)(uint32_t)sn) * 1099511628211ull;
    h = (h ^ (uint64_t)(uint32_t)stg) * 1099511628211ull;
    h = (h ^ srt) * 1099511628211ull;
    return h;
}
static uint32_t pend_jump; static int pend_poll;
static int check(const rd_entry *e, int idx)
{
    rd_stat *s = &st[idx];
    jr_n = 0; rd_journal_on = 1;
    if (fuzz_n && !e->nofuzz && s->calls < 300) {
        static int leakcheck = -1, leaks;
        if (leakcheck < 0) leakcheck = getenv("RR_RD_LEAKCHECK") != NULL;
        uint64_t h0 = leakcheck ? state_hash() : 0;
        fuzz(e, s);
        if (leakcheck && state_hash() != h0 && leaks++ < 20)
            fprintf(stderr, "[RD] FUZZ LEAK %s (%06X): state differs after its fuzz round (call %ld)\n",
                    e->name, (unsigned)e->ep, s->calls);
    }
    rd_real = 1;
    pair_res r = run_pair(e, 1);
    rd_real = 0;
    rd_journal_on = 0; jr_n = 0;
    pend_jump = r.jump; pend_poll = r.jpoll;           /* rd_hook makes it, outside the check */
    s->calls++; s->cost_sum += r.cost; s->cost_n++;
    if (r.cost < s->cost_min || !s->cost_min) s->cost_min = r.cost;
    if (r.cost > s->cost_max) s->cost_max = r.cost;
    if (!r.io && r.rcost != r.cost) {                   /* timing, not results: counted separately */
        if (!s->cost_bad++) fprintf(stderr, "[RD] COST %s (%06X): charges %d, lifted ran %d instructions\n", e->name, (unsigned)e->ep, r.rcost, r.cost);
    }
    if (r.io) s->unver++;
    else if (r.bad) {
        s->fail++;
        if (s->fail <= 3) fprintf(stderr, "[RD] MISMATCH %s (%06X), call %ld: %s\n", e->name, (unsigned)e->ep, s->calls, r.why);
    } else s->pass++;
    return 1;
}

int rd_hook(uint32_t ep)
{
    if (mode == 3) { census(ep); return 0; }
    if (ep == bypass_ep) { bypass_ep = 0; return 0; }
    int idx; const rd_entry *e = find(ep, &idx);
    if (!e || st[idx].disabled) return 0;
    if (mode == 2) {
        if (in_check) return 0;
        /* A replaced function's callees run LIFTED inside its check, so a callee
         * reached only through converted callers would never be checked itself.
         * After a function's first two checks, every other call runs it lifted
         * and unchecked, which lets its callees be checked at their own level. */
        if (++st[idx].seen > 2 && (st[idx].seen & 1)) return 0;
        in_check = 1; pend_jump = 0; int r = check(e, idx); in_check = 0;
        if (pend_jump) {                               /* the lifted run's tail jump, made once */
            uint32_t t = pend_jump; pend_jump = 0;
            if (pend_poll) RR_POLL();
            rr_jump(t, ep);
        }
        return r;
    }
    rd_jmp_poll = 0;
    uint32_t tj = e->fn();
    rr_budget -= e->cost;
    if (rd_jmp_poll) { rd_jmp_poll = 0; RR_POLL(); }
    if (tj == RD_UNWIND) return 1;                     /* a callee unwound past us: nothing to do */
    if (tj) rr_jump(tj, ep); else do_rts(ep);
    return 1;
}

static void report(void)
{
    if (mode == 3) {
        fprintf(stderr, "[RD] census: calls per lifted function (top 60) -> rd_census.txt\n");
        FILE *f = fopen("rd_census.txt", "w");
        for (int i = 0; i < CEN_N; i++) if (cen_ep[i] && f) fprintf(f, "%06X %ld\n", (unsigned)cen_ep[i], cen_calls[i]);
        if (f) fclose(f);
        return;
    }
    if (mode != 2) return;
    const char *cf = getenv("RR_RD_COV");
    if (cf && cov_bits) {
        FILE *f = fopen(cf, "w");
        for (uint32_t a = 0; f && a < RR_ROM_SIZE; a += 2) if (cov_bits[a >> 4] & (1u << ((a >> 1) & 7))) fprintf(f, "%06X\n", (unsigned)a);
        if (f) fclose(f);
        fprintf(stderr, "[RD] coverage -> %s (tools/rd_coverage.py)\n", cf);
    }
    int np = 0, nf = 0, nu = 0, nz = 0;
    for (int i = 0; i < rd_count; i++) {
        rd_stat *s = &st[i];
        const char *v = !s->calls ? "NOT CALLED" : s->fail ? "FAIL" : s->pass ? "pass" : "unverifiable";
        if (s->fz_fail) v = "FUZZFAIL";
        fprintf(stderr, "[RD] %-8s %06X %-14s calls %7ld pass %7ld fail %5ld unverif %5ld | fuzz %6ld fail %4ld unverif %4ld | cost %.1f (%d..%d)\n", v,
                (unsigned)rd_table[i].ep, rd_table[i].name, s->calls, s->pass, s->fail, s->unver,
                s->fz, s->fz_fail, s->fz_unver, s->cost_n ? s->cost_sum / s->cost_n : 0.0, s->cost_min, s->cost_max);
        if (s->cost_bad) fprintf(stderr, "[RD]          %ld calls charged a different instruction count (timing only)\n", s->cost_bad);
        if (!s->calls) nz++; else if (s->fail || s->fz_fail) nf++; else if (s->pass) np++; else nu++;
    }
    fprintf(stderr, "[RD] %d replacements: %d pass, %d FAIL, %d unverifiable, %d not called\n", rd_count, np, nf, nu, nz);
    if (rd_probe_timeouts) fprintf(stderr, "[RD] %ld probes abandoned (trap, or past the %d-instruction cap; counted unverifiable)\n", rd_probe_timeouts, PROBE_CAP);
    { extern long rd_div_overflows;
      fprintf(stderr, "[RD] divs.w overflows in FUN_0003124e on REAL calls (lifted + readable run each): %ld\n", rd_div_overflows); }
}

void rd_init(void)
{
    const char *e = getenv("RR_RD");
#ifdef RR_TRACE
    const int dflt = 0;      /* the trace build feeds the MAME instruction diff: lifted only */
#else
    const int dflt = 1;
#endif
    mode = !e ? dflt : !strcmp(e, "check") ? 2 : !strcmp(e, "census") ? 3 : atoi(e) ? 1 : 0;
    merge_tables();
    for (int i = 0; i < rd_count; i++)
        if (rd_table[i].scratch & ~rd_table[i].kill) { fprintf(stderr, "[RD] %s declares scratch outside its kill mask\n", rd_table[i].name); exit(7); }
    for (int i = 1; i < rd_count; i++)
        if (rd_table[i].ep <= rd_table[i - 1].ep) { fprintf(stderr, "[RD] two replacements for %06X\n", (unsigned)rd_table[i].ep); exit(7); }
    st = calloc(rd_count > 0 ? (size_t)rd_count : 1u, sizeof *st);
    rd_on = mode != 0 && (rd_count > 0 || mode == 3);
    { const char *f = getenv("RR_RD_FUZZ"); fuzz_n = (mode == 2 && f) ? atoi(f) : 0; }
#ifdef RR_TRACE
    if (mode == 2 && getenv("RR_RD_COV")) cov_bits = calloc(RR_ROM_SIZE / 16, 1);
#else
    if (mode == 2 && getenv("RR_RD_COV")) fprintf(stderr, "[RD] RR_RD_COV needs the trace build (build/rr_trace)\n");
#endif
    { extern int g_rr_draw_extra;                      /* the draw-distance option (rd_b2.c) */
      const char *x = getenv("RR_DRAW_EXTRA");
      if (x) g_rr_draw_extra = atoi(x);
      if (g_rr_draw_extra < 0) g_rr_draw_extra = 0;
      if (g_rr_draw_extra > RR_DRAW_EXTRA_MAX) g_rr_draw_extra = RR_DRAW_EXTRA_MAX;
      if (mode == 2 && g_rr_draw_extra) { fprintf(stderr, "[RD] check mode: draw distance back to the original\n"); g_rr_draw_extra = 0; }
      if (mode == 1 && g_rr_draw_extra) fprintf(stderr, "[RD] draw distance: %d extra track pieces\n", g_rr_draw_extra); }
    if (mode) fprintf(stderr, "[RD] %s, %d readable replacements\n",
                      mode == 1 ? "running" : mode == 2 ? "CHECKING against the lifted code" : "census", rd_count);
    atexit(report);
}
