/* m377_periph.c -- the 7700-family ON-CHIP PERIPHERALS: reset state, the timers,
 * the A-D converter and interrupt arbitration/entry. Shared by the test oracle and
 * the translated sound program -- this is what calls the program's interrupt
 * handlers at the rates the program itself programs. Split from pc-reverse's
 * m37710.c; the instruction semantics are in m377_sem.h, and executing
 * instructions is done either by the oracle's fetch/decode loop
 * (tools/sndoracle/m37710.c, test only) or by gen/snd_driver.c (the game).
 *
 * (Original notes follow.)
 *
 * STATE OF THIS FILE: the skeleton plus the instruction groups the real
 * program actually reaches first. It is built to be driven by
 * tools/m37710/probe.c, which runs pr1data.8k and reports the opcode it
 * stops on, so the implementation order is dictated by the ROM instead of by
 * a datasheet reading. An unimplemented opcode STOPS and is reported; it is
 * never silently skipped, because a core that quietly ignores instructions
 * produces plausible-looking wrong output, which is the failure this whole
 * effort is trying to get away from.
 */
#include "m37710.h"
#include <string.h>
#include <stdio.h>

/* ---- bus helpers. 16-bit accesses are little-endian on this part. ------- */
/* The internal peripheral block is part of the CPU, so it is answered here
 * and the host bus never sees 0x000000-0x00007F. */
#include "m377_sem.h"
void m37710_init(m37710_t *c, m377_read8_fn r, m377_write8_fn w, void *user)
{
    memset(c, 0, sizeof *c);
    c->read8 = r; c->write8 = w; c->user = user;
}

void m37710_reset(m37710_t *c)
{
    c->pg = 0; c->dt = 0; c->dpr = 0; c->s = 0x01FF;
    c->ps = M377_M | M377_X | M377_I;      /* 8-bit A and index, IRQs masked */
    c->pending = 0;
    c->irq_taken = 0;
    c->t_on = 0;
    c->ad_due = 0;
    memset(c->ad_result, 0, sizeof c->ad_result);
    memset(c->sfr, 0, sizeof c->sfr);
    /* UARTs as MAME resets them (m37710.cpp device_reset): control 0 = 8,
     * control 1 = 2 -- "transmit buffer empty", which MAME never clears (its
     * transmit-buffer write is a no-op). The Rave Racer C74 BIOS writes UART0
     * (0x32) and then spins on BBC #2,$35 at 0xC340 until that bit is set. */
    c->sfr[0x34] = c->sfr[0x3C] = 8;
    c->sfr[0x35] = c->sfr[0x3D] = 2;
    memset(c->t_next, 0, sizeof c->t_next);
    c->a = c->b = c->x = c->y = 0;
    c->stopped = false; c->unimpl_hit = false; c->cycles = 0;
    c->pc = rd16(c, 0xFFFE);
}

extern int g_m377_sfrlog;
static void timers_tick(m37710_t *c);
static void adc_tick(m37710_t *c);

/* Take an interrupt. `vector_offset` is relative to the table base at
 * 0xFFE0, so Timer A0 (0xFFEE) is 0x0E.
 *
 * WAI wakes on an interrupt even while the mask is set -- that is the whole
 * point of it -- so the halt is cleared before the mask is tested.
 *
 * A request that arrives with the mask set is LATCHED, not dropped. The
 * driver spends most of its time in `LDA $83 ; BEQ` at 0xC12B waiting for an
 * interrupt handler to set that flag, and it raises the mask around its own
 * critical sections; dropping a request made there would lose whole frames
 * of work. m37710_run() retires the latch as soon as the mask clears. */
static void take_irq(m37710_t *c, int vector_offset)
{
    push8(c, c->pg); push16(c, c->pc); push16(c, c->ps);
    setf(c, M377_I, true); setf(c, M377_D, false);
    c->pg = 0; c->pc = rd16(c, (uint32_t)(0xFFE0 + vector_offset));
}

int g_m377_sfrlog = 0;
int g_m377_watch = 0;   /* probes set this to trace peripheral programming */

/* THE MASKABLE INTERRUPT TABLE, in hardware priority-arbitration order.
 *
 * `pending` is a bit per row. Rows are ordered by vector address, which is
 * also the order the part breaks ties in -- MAME's own arbitration scans from
 * the highest source down and keeps the first with a strictly greater
 * priority, so an equal priority leaves the incumbent alone.
 *
 * Every row's priority is the low 3 bits of its control register, read LIVE
 * when the request is arbitrated. It cannot be latched at raise time: the
 * driver programs these registers while requests are already pending, and
 * latching gave Timer B0 a stale priority that beat INT2 to the first
 * interrupt after boot -- a divergence from MAME at instruction 74,842. */
typedef struct { uint16_t vec; uint8_t ctrl; } irq_row_t;
static const irq_row_t IRQ_TAB[] = {
    { 0xFFD6, 0x70 },   /* A-D converter  -- the handlebar and pedal */
    { 0xFFDC, 0x71 },   /* UART0 transmit */
    { 0xFFDE, 0x72 },   /* UART0 receive  */
    { 0xFFD8, 0x73 },   /* UART1 transmit */
    { 0xFFDA, 0x74 },   /* UART1 receive  */
    { 0xFFEE, 0x75 },   /* Timer A0 -- the sound driver's 120Hz tick */
    { 0xFFEC, 0x76 },   /* Timer A1 */
    { 0xFFEA, 0x77 },   /* Timer A2 */
    { 0xFFE8, 0x78 },   /* Timer A3 -- the PEDAL encoder, an event counter */
    { 0xFFE6, 0x79 },   /* Timer A4 */
    { 0xFFE4, 0x7A },   /* Timer B0 */
    { 0xFFE2, 0x7B },   /* Timer B1 */
    { 0xFFE0, 0x7C },   /* Timer B2 */
    { 0xFFF4, 0x7D },   /* external INT0 -- board, once a frame */
    { 0xFFF2, 0x7E },   /* external INT1 */
    { 0xFFF0, 0x7F },   /* external INT2 -- board, once a frame */
};
#define IRQ_N ((int)(sizeof IRQ_TAB / sizeof IRQ_TAB[0]))
enum { IRQ_ADC = 0, IRQ_TA0 = 5, IRQ_TB0 = 10, IRQ_INT0 = 13, IRQ_INT2 = 15 };

/* Timer n (0-4 = A0..A4, 5-7 = B0..B2) -> its row above. */
static const uint8_t TIMER_ROW[8] = { IRQ_TA0, 6, 7, 8, 9, IRQ_TB0, 11, 12 };

static int vec_to_row(int vector_offset)
{
    uint16_t v = (uint16_t)(0xFFE0 + vector_offset);
    int i;
    for (i = 0; i < IRQ_N; i++) if (IRQ_TAB[i].vec == v) return i;
    return -1;
}

void m37710_irq(m37710_t *c, int vector_offset)
{
    int row = vec_to_row(vector_offset);
    c->stopped = false;                 /* WAI wakes even with the mask set */
    if (row >= 0) c->pending |= 1u << row;
    m37710_service(c);
}

/* Bring the peripherals up to the current cycle and recognise the
 * highest-priority pending source, if the mask allows it and it outranks the
 * level we are already running at. Interrupts are recognised at
 * INSTRUCTION BOUNDARIES, so a tracer that wants to compare its PC stream
 * against MAME's has to call this before it samples the PC -- otherwise it
 * records the instruction the interrupt is about to displace and the two
 * streams look one apart when they are not.
 *
 * The current level lives in PS bits 8-10, which is why RTI and PLP restore
 * it for free: popping the saved status word is what leaves a handler. A
 * source whose control register holds priority 0 is disabled and can never
 * win, since the comparison is strict. */
void m37710_service(m37710_t *c)
{
    int i, best = -1, bestpri;
    timers_tick(c);
    adc_tick(c);
    if (!c->pending || flag(c, M377_I)) return;
    bestpri = (c->ps >> M377_IPL_SHIFT) & 7;
    for (i = IRQ_N - 1; i >= 0; i--) {
        int pri;
        if (!(c->pending & (1u << i))) continue;
        pri = c->sfr[IRQ_TAB[i].ctrl] & 7;
        if (pri > bestpri) { best = i; bestpri = pri; }
    }
    if (best < 0) return;
    c->pending &= ~(1u << best);
    c->irq_taken++;
    take_irq(c, (int)IRQ_TAB[best].vec - 0xFFE0);
    c->ps = (uint16_t)((c->ps & ~(7u << M377_IPL_SHIFT))
                       | ((unsigned)bestpri << M377_IPL_SHIFT));
}

/* Arm timer `n` from its own registers, exactly as MAME's recalc_timer does.
 * Only mode 0 ("timer mode") counts the clock: mode 1 is an event counter
 * fed by an external pin, which is what Timer A3 is on this board -- the
 * PEDAL's optical sensor -- and what A2 is set to as well. */
static void timer_arm(m37710_t *c, int n)
{
    static const unsigned TSCALE[4] = { 2, 16, 64, 512 };
    unsigned mode   = c->sfr[0x56 + n];
    unsigned reload = (unsigned)c->sfr[0x46 + n * 2] | ((unsigned)c->sfr[0x47 + n * 2] << 8);
    if (!(c->sfr[0x40] & (1u << n))) { c->t_on &= (uint8_t)~(1u << n); return; }
    if ((mode & 3) != 0)             { c->t_on &= (uint8_t)~(1u << n); return; }
    if (reload == 0 && (mode & 0xC0) == 0) { c->t_on &= (uint8_t)~(1u << n); return; }
    c->t_on |= (uint8_t)(1u << n);
    c->t_next[n] = c->cycles + (uint64_t)(reload + 1) * TSCALE[(mode >> 6) & 3];
}

/* Internal peripheral registers. Writing the count-start register arms every
 * timer whose bit just went high, which is the only edge MAME recalculates
 * on too -- a reload written while a timer runs takes effect at its next
 * underflow, not immediately. */
void m377_sfr_w(m37710_t *c, uint32_t a, uint8_t v)
{
    uint8_t prev = c->sfr[a];
    /* THE STRAY-STORE TRAP. The driver programs its peripherals once, out
     * of a handful of known routines; a write to a timer register from
     * anywhere else is an index that has gone wrong upstream, and it is
     * fatal -- clearing the count-start register stops the tick that wakes
     * the driver's main loop. Print the path in, not just the store. */
    if (c->pchist_on && (a == 0x40 || (a >= 0x50 && a <= 0x5F)) &&
        !(c->pc >= 0xC030 && c->pc <= 0xC700) && !(c->pc >= 0xCD90 && c->pc <= 0xCDC0) &&
        c->pc != 0xC346 && c->pc != 0xC253 && c->pc != 0xD24F && c->pc != 0xD1D4 &&
        c->pc != 0xC342 && c->pc != 0xC345) {
        int k;
        fprintf(stderr, "[M377] STRAY peripheral write SFR[%02X]=%02X (was %02X) at pc=%04X "
                        "A=%04X B=%04X X=%04X Y=%04X DPR=%04X DT=%02X ps=%04X\n  path:",
                a, v, prev, c->pc, c->a, c->b, c->x, c->y, c->dpr, c->dt, c->ps);
        for (k = 0; k < 64; k++)
            fprintf(stderr, " %04X", c->pchist[(c->pchist_n + k) & 63]);
        fprintf(stderr, "\n");
        c->pchist_on = 0;                        /* once is enough */
    }
    if (g_m377_sfrlog)
        fprintf(stderr, "SFR[%02X]=%02X (was %02X) pc=%02X%04X A=%04X B=%04X X=%04X Y=%04X "
                        "DPR=%04X DT=%02X ps=%04X cyc=%llu\n",
                a, v, prev, c->pg, c->pc, c->a, c->b, c->x, c->y, c->dpr, c->dt, c->ps,
                (unsigned long long)c->cycles);
    if (a == 0x35 || a == 0x3D)                 /* MAME uart*_ctrl_reg1_w: bit 1 (TX empty) is kept */
        v = (uint8_t)((prev & ((v & 4) ? 0xFA : 0x0A)) | (v & 0x05));
    if (a == 0x34 || a == 0x3C)                 /* keep MAME's reset bit 3 behaviour for ctrl 0 reads */
        v = (uint8_t)v;
    c->sfr[a] = v;
    if (a == 0x1E) {
        /* A-D control. Bit 6 starts a conversion; it completes 57*2*(4 or 2)
         * clocks later, and the interrupt is raised only when the conversion
         * STOPS -- so a repeat or sweep run keeps converting first. */
        if ((v & 0x40) && !(prev & 0x40))
            c->ad_due = c->cycles + 57u * 2u * ((v & 0x80) ? 2u : 4u);
        else if (!(v & 0x40))
            c->ad_due = 0;
        return;
    }
    if (a == 0x40) {
        int n;
        for (n = 0; n < 8; n++)
            if ((v & (1u << n)) && !(prev & (1u << n))) timer_arm(c, n);
            else if (!(v & (1u << n)))                  c->t_on &= (uint8_t)~(1u << n);
    }
}

/* Finish a conversion that is due. Mirrors MAME's ad_timer_cb: convert the
 * selected channel, advance it in sweep mode, and interrupt only once the
 * run stops. */
static void adc_tick(m37710_t *c)
{
    unsigned line, ctl;
    if (!c->ad_due || c->cycles < c->ad_due) return;
    ctl  = c->sfr[0x1E];
    line = ctl & 7;
    c->ad_result[line] = c->analog[line];
    if (ctl & 0x10) { ctl = (ctl & 0xF8) | ((line + 1) & 7); c->sfr[0x1E] = (uint8_t)ctl; }
    if ((ctl & 8) || ((ctl & 0x10) && line != (unsigned)(c->sfr[0x1F] & 3) * 2 + 1)) {
        c->ad_due = c->cycles + 57u * 2u * ((ctl & 0x80) ? 2u : 4u);
    } else {
        c->ad_due = 0;
        c->sfr[0x1E] = (uint8_t)(ctl & 0xBF);
        c->pending |= 1u << IRQ_ADC;
        c->stopped = false;
    }
}

/* Advance the timers to the current cycle, raising what has underflowed. */
static void timers_tick(m37710_t *c)
{
    int n;
    if (!c->t_on) return;
    for (n = 0; n < 8; n++) {
        if (!(c->t_on & (1u << n))) continue;
        while (c->cycles >= c->t_next[n]) {
            static const unsigned TSCALE[4] = { 2, 16, 64, 512 };
            unsigned mode   = c->sfr[0x56 + n];
            unsigned reload = (unsigned)c->sfr[0x46 + n * 2] | ((unsigned)c->sfr[0x47 + n * 2] << 8);
            c->t_next[n] += (uint64_t)(reload + 1) * TSCALE[(mode >> 6) & 3];
            c->pending |= 1u << TIMER_ROW[n];
            c->stopped = false;
        }
    }
}

/* ---- the dispatch ------------------------------------------------------ */
