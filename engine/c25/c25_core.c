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
    if (d->sp >= 64) { snprintf(d->error, sizeof d->error, "stack overflow"); return false; }
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
