/*
 * rd.h -- READABLE C for Rave Racer's 68K program ("Phase B").
 *
 * Each readable function replaces one lifted function (gen/rr_lifted.c) at its
 * entry point. It works on the SAME machine state the lifted code does -- the
 * 68K registers in R[] and memory through vrd/vwr at original addresses (one
 * memory model, pc-reverse GUARDRAILS R1-R5) -- but is written as ordinary C.
 *
 * Every replacement is proven against its lifted twin, not trusted:
 * RR_RD=check runs BOTH on every real call from real play, with the memory
 * writes journalled and rolled back between them, and compares
 *   - EVERY register D0-D7/A0-A6/SP must end equal to the lifted run's,
 *     except those the entry declares SCRATCH (dead after the call); scratch
 *     must lie inside Ghidra's kill mask (rr_kXXXX) and those registers, when
 *     outside it, must be unchanged. (Ghidra's masks can be too generous --
 *     FUN_000270f2's names nine registers and writes two -- so the mask alone
 *     is never trusted as the comparison.)
 *   - every memory byte either wrote, except dead stack below the final SP
 *   - the return path (shadow return stack)
 * and then restores the lifted result, so a checked run plays exactly like
 * the lifted one. A mismatch disables that replacement and is reported.
 *
 * Timing is RESULTS-exact, not instruction-exact: a replacement charges the
 * lifted function's measured mean instruction count (`cost`), so interrupts
 * land close to, not exactly at, the lifted build's instruction. RR_RD=0 runs
 * the lifted code alone, which is what the MAME instruction diff needs.
 *
 * RR_RD=1 (default) run replacements | 0 lifted only | check | census
 */
#ifndef RD_H
#define RD_H
#include <stdint.h>
#include "rr_mem.h"
#include "rr_lift_rt.h"

/* register numbering for the kill / output masks: D0..D7 = bits 0..7,
 * A0..A5 = bits 8..13 (the order ComputeRegEffects.java uses) */
#define RD_D(n) (1u << (n))
#define RD_A(n) (1u << (8 + (n)))

typedef struct {
    uint32_t    ep;        /* entry address                                  */
    uint32_t  (*fn)(void); /* the readable body: returns 0 to rts (supplied by
                            * the dispatcher), or an address to JUMP to -- the
                            * 68K tail-branches into other functions' code */
    uint16_t    kill;      /* Ghidra kill mask (rr_kXXXX)                    */
    uint16_t    scratch;   /* registers left DEAD: not compared (subset of kill) */
    int         cost;      /* lifted instructions per call (measured by check) */
    const char *name;
    int         nofuzz;    /* 1: data-driven JUMP TARGET (a dispatcher) -- mutating
                            * its state only jumps into garbage, so it is checked
                            * on every real call but never fuzzed */
} rd_entry;

/* The registry: every file in src/rd/ registers its own table with
 * RD_REGISTER(tbl) (a constructor, so batches never edit a shared list);
 * rd_init merges them into rd_table, sorted by entry address, and refuses
 * a duplicate entry point. */
extern const rd_entry *rd_table;
extern int             rd_count;
void rd_register(const rd_entry *t, int n);
#define RD_REGISTER(tbl) \
    __attribute__((constructor)) static void rd_register_##tbl(void) \
    { rd_register(tbl, (int)(sizeof tbl / sizeof tbl[0])); }
extern int            rd_on;
int rd_hook(uint32_t ep);          /* called from each lifted entry; 1 = handled */

/* ---- register access for readable code ---- */
static inline uint32_t d_reg(int n)             { return (uint32_t)RG4((uint32_t)n * 4); }
static inline uint32_t a_reg(int n)             { return (uint32_t)RG4(0x20 + (uint32_t)n * 4); }
static inline void     set_d(int n, uint32_t v) { RS4((uint32_t)n * 4, v); }
static inline void     set_a(int n, uint32_t v) { RS4(0x20 + (uint32_t)n * 4, v); }
static inline void     set_d16(int n, uint16_t v) { RS2((uint32_t)n * 4 + 2, v); }   /* move.w into Dn */
static inline void     set_d8(int n, uint8_t v)   { RS1((uint32_t)n * 4 + 3, v); }   /* move.b into Dn */
/* Instruction accounting, so interrupts land where the lifted build puts them.
 * The dispatcher charges the entry's fixed `cost` after the body; a longer path
 * charge()s the difference. A function with a LOOP must match the lifted code's
 * poll points too: the lifted code polls the scheduler when a backward branch
 * (dbf, bra/bcc backwards) is TAKEN, so such a body charges its instructions in
 * order and calls poll() exactly there -- with cost 0 (or only what follows the
 * loop). Without that, a tick due mid-loop lands after the function instead, and
 * a timing word (WRAM 0x79A) drifts while picture and sound stay identical. */
static inline void charge(int n) { rr_budget -= n; }
static inline void poll(void) { RR_POLL(); }
/* condition codes (R bytes 0x44 N, 0x45 Z, 0x46 V, 0x47 C): a caller may branch on
 * them straight after the call (bsr f ; beq ...), so a function whose last flag-setting
 * instruction matters must leave them as the 68K does */
static inline void rd_flags_nzvc(int n, int z, int v, int c) { RS1(0x44, n); RS1(0x45, z); RS1(0x46, v); RS1(0x47, c); }
/* cmp.w / cmpi.w: flags of dst - src */
static inline void rd_flags_cmp16(uint16_t dst, uint16_t src)
{
    uint16_t r = (uint16_t)(dst - src);
    rd_flags_nzvc((r & 0x8000) != 0, r == 0, ((dst ^ src) & (dst ^ r) & 0x8000) != 0, src > dst);
}

#define RD_RTS 0u                  /* return value: a plain rts */
/* A computed jmp (jmp (An,Dn)) polls the scheduler before it jumps, like a taken
 * backward branch; a forward bcc into other code does not. RD_JMP(t) returns t
 * with that flag set aside (not in the address: a data-driven target can be odd). */
extern int rd_jmp_poll;
#define RD_JMP(t) (rd_jmp_poll = 1, (uint32_t)(t))
/* 68K cdecl stack argument i (0 = first), as seen at entry: SP -> return address */
static inline uint32_t stack_arg(int i)         { return rr_read(a_reg(7) + 4 + 4 * (uint32_t)i, 4); }
/* ---- calls from readable code (a function that is not a leaf) ----
 * Exactly what the lifted code does at `bsr`/`jsr`: push the return address on
 * the 68K stack, register it on the shadow stack, poll the scheduler, call.
 * The callee runs through its own entry hook, so it is itself readable where
 * one exists (lifted inside a check). Charge your own instructions up to and
 * INCLUDING the bsr/jsr before calling -- the callee charges its own.
 * Returns:
 *   0          the callee returned here normally: carry on
 *   RD_UNWIND  a callee unwound the stack past this frame: return RD_UNWIND at once
 *   other      the callee returned to a different address: return it (the
 *              dispatcher continues there, as the lifted code would)
 * so every call site reads:  if ((r = rd_call(L_2400, 0x468))) return r;      */
#define RD_UNWIND 0xFFFFFFFFu
void rr_jump(uint32_t pc, uint32_t at);
void rr_call_ind(uint32_t t, uint32_t at);
extern uint32_t rr_ret_to;
int  rr_call_push(uint32_t ret);
int  rr_after_call(int j);
static inline uint32_t rd_call_common(uint32_t ret, int j)
{
    if (rr_after_call(j)) return RD_UNWIND;
    return rr_ret_to == ret ? 0u : rr_ret_to;
}
static inline uint32_t rd_call(void (*fn)(void), uint32_t ret)
{
    uint32_t sp = a_reg(7) - 4;
    set_a(7, sp); vwr32(sp, ret);
    int j = rr_call_push(ret);
    RR_POLL();
    fn();
    return rd_call_common(ret, j);
}
/* jsr (An) / jsr through a table: `at` is the jsr's own address */
static inline uint32_t rd_call_ind(uint32_t target, uint32_t ret, uint32_t at)
{
    uint32_t sp = a_reg(7) - 4;
    set_a(7, sp); vwr32(sp, ret);
    int j = rr_call_push(ret);
    RR_POLL();
    rr_call_ind(target, at);
    return rd_call_common(ret, j);
}
/* the draw-distance option: extra track pieces ahead (rd_b2.c) */
extern int g_rr_draw_extra;
#define RR_DRAW_EXTRA_MAX 24

#endif
