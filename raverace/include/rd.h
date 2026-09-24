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

extern const rd_entry rd_table[];
extern const int      rd_count;
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
#define RD_RTS 0u                  /* return value: a plain rts */
/* A computed jmp (jmp (An,Dn)) polls the scheduler before it jumps, like a taken
 * backward branch; a forward bcc into other code does not. RD_JMP(t) returns t
 * with that flag set aside (not in the address: a data-driven target can be odd). */
extern int rd_jmp_poll;
#define RD_JMP(t) (rd_jmp_poll = 1, (uint32_t)(t))
/* 68K cdecl stack argument i (0 = first), as seen at entry: SP -> return address */
static inline uint32_t stack_arg(int i)         { return rr_read(a_reg(7) + 4 + 4 * (uint32_t)i, 4); }
#endif
