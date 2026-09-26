/*
 * c25_core.c -- the master DSP's STEP (engine/c25/c25.h): interrupts at the
 * instruction boundary in TMS32025 priority order, IDLE, the timer, and then
 * the instruction at PC through the TRANSLATED program (d->xlat). Moved from
 * the validated interpreter; the fetch and decode it did are gone -- the
 * translation carries every opcode as a constant.
 */
#include <stdio.h>
#include <string.h>
#include "c25.h"

static bool push(c71_t *d, uint16_t v)
{
    if (d->sp >= 8) { for (int i = 1; i < 8; i++) d->stack[i - 1] = d->stack[i]; d->sp = 7; }   /* 8 levels, the oldest is lost (see c25_sem.h push) */
    d->stack[d->sp++] = v; return true;
}

bool c71_step(c71_t *d)
{
    /* interrupts at the instruction boundary, in TMS32025 priority order:
     * INT0 2, INT1 4, INT2 6, TINT 0x18, RINT 0x1A, XINT 0x1C */
    if (!d->intm) {
        static const struct { uint16_t bit, imr, vec; } iv[] = {
            {1, 1, 2}, {2, 2, 4}, {4, 4, 6}, {8, 8, 0x18}, {0x10, 0x10, 0x1A}, {0x20, 0x20, 0x1C} };
        if (d->tint_pend) d->ifr |= 8;
        for (int k = 0; k < 6; k++)
            if ((d->ifr & iv[k].bit) && (d->imr & iv[k].imr)) {
                if (!push(d, d->pc)) return false;
                d->pc = iv[k].vec; d->intm = 1; d->ifr &= ~iv[k].bit; d->idle = 0;
                if (iv[k].bit == 8) d->tint_pend = 0;
                break;
            }
    }
    if (d->idle) {                        /* halted: nothing retires, the timer runs */
        d->tim = d->tim == 0 ? d->prd : (uint16_t)(d->tim - 1);
        if (d->tim == d->prd && (d->imr & 8)) d->tint_pend = 1;
        return true;
    }
    int pc = d->pc;
    if (c25_hook_pre) c25_hook_pre(d, pc);
    d->cur_pc = pc;
    d->steps++;
    /* TIM ticks once per retired instruction, reloading from PRD at 0 */
    d->tim = d->tim == 0 ? d->prd : (uint16_t)(d->tim - 1);
    if (d->tim == d->prd && (d->imr & 8)) d->tint_pend = 1;
    /* the instruction itself, with its RPT repeats: the translated program */
    if (!d->xlat) { snprintf(d->error, sizeof d->error, "no program translation"); return false; }
    if (!d->xlat(d, pc)) return false;
    if (c25_hook_post) c25_hook_post(d);
    return true;
}

/* Run `steps` steps, exactly as that many calls of c71_step would -- but an IDLE DSP is fast-forwarded.
 *
 * The C71 runs at 10 MIPS, so a frame is ~166,667 steps, and the master DSP spends ~93% of them halted on IDLE
 * waiting for the next frame's interrupt (measured on Rave Racer: ~12,000 retired instructions a frame). Each of
 * those halted steps did nothing but tick the timer -- and cost a whole c71_step call: about a third of the game's
 * CPU time on a fast PC, most of it on a slow one.
 *
 * While the DSP is halted, a step changes only TIM and TINT_PEND (see c71_step): nothing retires, no hook runs, no
 * counter moves. And nothing can wake the DSP inside this call except the timer: the other interrupts are raised
 * BETWEEN calls (c71_irq from the frame loop). So after one real step that leaves it halted:
 *   - if the timer can wake it (INTM 0, TINT enabled) it is skipped up to the step BEFORE the timer event;
 *   - otherwise every remaining step is skipped, TINT_PEND set if an event fell inside (INTM 1 case).
 * The timer arithmetic is closed-form: TIM counts down to 0, reloads PRD, and an "event" is a step that leaves
 * TIM == PRD. tools/c25_run_test.c checks this against c71_step, step for step, on random states. */
static long idle_skip(c71_t *d, long n)
{
    const int wake = !d->intm && (d->imr & 8);
    if (!d->intm && d->tint_pend) return 0;                 /* the next step takes the timer interrupt */
    uint32_t tim = d->tim, prd = d->prd;
    /* steps until the next event (a step that leaves TIM == PRD) */
    long k = tim > prd ? (long)(tim - prd) : (long)tim + 1;
    if (wake) {
        long m = n < k - 1 ? n : k - 1;                    /* stop before the event: normal stepping takes it */
        d->tim = (uint16_t)(tim - (uint32_t)m);            /* m < k, so no reload and no event in between */
        return m;
    }
    if (n < k) { d->tim = (uint16_t)(tim - (uint32_t)n); return n; }
    long r = n - k;                                         /* the event step, then whole periods, then a remainder */
    if (d->imr & 8) d->tint_pend = 1;
    long period = (long)prd + 1;
    r %= period;
    d->tim = (uint16_t)(prd - (uint32_t)r);
    return n;
}

bool c71_run(c71_t *d, long steps)
{
    while (steps > 0) {
        if (!c71_step(d)) return false;
        steps--;
        if (d->idle && steps > 0) steps -= idle_skip(d, steps);
    }
    return true;
}

void c71_reset(c71_t *d)
{
    d->bank = 0; d->latch = 0; d->pt_addr = 0; d->pt_data = 0; d->bioz = 1;
    d->pc = 0x4000; d->pfc = 0; d->t = 0; d->acc = 0; d->p = 0;
    memset(d->ar, 0, sizeof d->ar);
    d->arp = d->arb = d->dp = d->pm = 0;
    d->sxm = 1; d->ovm = 0; d->intm = 0; d->c = 0; d->tc = 0; d->cnf = 0;
    d->imr = 9; d->prd = 0xFFFF; d->tim = 0xFFFF; d->tint_pend = 0;
    d->idle = 0; d->ifr = 0;
    d->sp = 0; d->rpt = 0; d->steps = 0; d->error[0] = 0;
    memset(d->written, 0, sizeof d->written); d->n_written = 0;
}

static bool load_words(uint16_t *dst, size_t max_words, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    uint8_t b[2]; size_t i = 0;
    while (i < max_words && fread(b, 1, 2, f) == 2) dst[i++] = (uint16_t)(b[0] << 8 | b[1]);
    fclose(f);
    return true;
}

bool c71_load(c71_t *d, const char *bios_path, const char *prog_path)
{
    bool ok = true;
    if (prog_path) ok = ok && load_words(d->prog + 0x4000, 0xC000, prog_path);
    if (bios_path) ok = ok && load_words(d->prog, 0x4000, bios_path);
    return ok;
}
