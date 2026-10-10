/*
 * h8_periph.c -- the H8/3002's and H8/3334's on-chip peripherals, our own code (see h8_periph.h for what and from where).
 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "h8_periph.h"

#define CAP_STATES 4096                         /* h8p_next_event's horizon: the board re-asks at least this often */

/* ---------------------------------------------------------------- ports */
/* the port registers: [port] = { ddr address, dr address } (low byte of the address), 0 = none */
static const uint8_t ports3002[12][2] = { [4] = { 0xC5, 0xC7 }, [6] = { 0xC9, 0xCB }, [7] = { 0, 0xCE }, [8] = { 0xCD, 0xCF },
                                          [9] = { 0xD0, 0xD2 }, [10] = { 0xD1, 0xD3 }, [11] = { 0xD4, 0xD6 } };
static const uint8_t ports3334[12][2] = { [1] = { 0xB0, 0xB2 }, [2] = { 0xB1, 0xB3 }, [3] = { 0xB4, 0xB6 }, [4] = { 0xB5, 0xB7 },
                                          [5] = { 0xB8, 0xBA }, [6] = { 0xB9, 0xBB }, [7] = { 0, 0xBE }, [8] = { 0xBD, 0xBF },
                                          [9] = { 0xC0, 0xC1 } };
static const uint8_t (*port_tab(const h8p *p))[2] { return p->chip == H8P_3002 ? ports3002 : ports3334; }

static void port_out(h8p *p, int n)
{
    const uint8_t m = p->pmask[n];
    if (p->port_out) p->port_out(p->ctx, n, (uint8_t)((p->dr[n] | ~p->ddr[n]) & ~m), (uint8_t)(p->ddr[n] & ~m));
}
static uint8_t port_read(h8p *p, int n)
{
    const uint8_t m = p->pmask[n];
    uint8_t v = (uint8_t)(m | (p->dr[n] & p->ddr[n]));
    if ((uint8_t)(p->ddr[n] & ~m) != (uint8_t)~m && p->port_in) v |= (uint8_t)(p->port_in(p->ctx, n) & ~p->ddr[n]);
    return v;
}
void h8p_port_changed(h8p *p, int port) { (void)p; (void)port; }

/* ---------------------------------------------------------------- timers */
static const uint8_t itu_div[8] = { 1, 2, 4, 8, 0, 0, 0, 0 };            /* TPSC: phi/1 /2 /4 /8; 4-7 external clocks (none wired) */
static int t16_running(const h8p *p, int ch) { return (p->tstr >> ch & 1) && itu_div[p->t16[ch].tcr & 7]; }

/* the counters: a compare match (and the clear it may cause) happens as the counter LEAVES the matching value -- the next count goes to 0
 * instead (so a cleared period is GR + 1 counts, as the manuals give); an overflow as it leaves 0xFFFF. Ticks to the next of those that
 * `want` asks for (bit0 A, bit1 B, bit2 overflow): */
static uint32_t t16_dist(const h8p *p, int ch, int want)
{
    const uint16_t c = p->t16[ch].tcnt;
    uint32_t d = (want & 4) ? 0x10000u - c : 0x20000u;
    if (want & 1) { const uint32_t a = (uint32_t)(uint16_t)(p->t16[ch].gra - c) + 1; if (a < d) d = a; }
    if (want & 2) { const uint32_t b = (uint32_t)(uint16_t)(p->t16[ch].grb - c) + 1; if (b < d) d = b; }
    return d;
}
static void t16_ticks(h8p *p, int ch, int64_t n)
{
    while (n > 0) {
        const uint32_t d = t16_dist(p, ch, 7);
        if ((int64_t)d > n) { p->t16[ch].tcnt = (uint16_t)(p->t16[ch].tcnt + n); return; }
        n -= d;
        const uint16_t v = (uint16_t)(p->t16[ch].tcnt + d - 1);          /* the value being left */
        const int cclr = p->t16[ch].tcr >> 5 & 3;
        uint16_t next = (uint16_t)(v + 1);
        if (v == 0xFFFF) p->t16[ch].tsr |= 4;                             /* OVF */
        if (v == p->t16[ch].gra) { p->t16[ch].tsr |= 1; if (cclr == 1) next = 0; }   /* IMFA */
        if (v == p->t16[ch].grb) { p->t16[ch].tsr |= 2; if (cclr == 2) next = 0; }   /* IMFB */
        p->t16[ch].tcnt = next;
    }
}

/* the H8/3334's 8-bit timers: CKS 1..3 = phi/8, /64, /1024, or with STCR.ICKS phi/2, /32 or /128, /256 or /2048 */
static uint32_t t8_div(const h8p *p, int ch)
{
    static const uint16_t d[2][2][4] = { { { 0, 8, 64, 1024 }, { 0, 2, 32, 256 } }, { { 0, 8, 64, 1024 }, { 0, 2, 128, 2048 } } };
    const int cks = p->t8[ch].tcr & 7;
    if (cks > 3) return 0;                                      /* external clock: none wired */
    return d[ch][p->stcr >> ch & 1][cks];
}
static uint32_t t8_dist(const h8p *p, int ch)
{
    const uint8_t c = p->t8[ch].tcnt;
    uint32_t d = 0x100u - c;
    const uint32_t a = (uint32_t)(uint8_t)(p->t8[ch].tcora - c) + 1; if (a < d) d = a;
    const uint32_t b = (uint32_t)(uint8_t)(p->t8[ch].tcorb - c) + 1; if (b < d) d = b;
    return d;
}
static void t8_ticks(h8p *p, int ch, int64_t n)
{
    while (n > 0) {
        const uint32_t d = t8_dist(p, ch);
        if ((int64_t)d > n) { p->t8[ch].tcnt = (uint8_t)(p->t8[ch].tcnt + n); return; }
        n -= d;
        const uint8_t v = (uint8_t)(p->t8[ch].tcnt + d - 1);
        const int cclr = p->t8[ch].tcr >> 3 & 3;
        uint8_t next = (uint8_t)(v + 1);
        if (v == 0xFF) p->t8[ch].tcsr |= 0x20;                              /* OVF */
        if (v == p->t8[ch].tcora) { p->t8[ch].tcsr |= 0x40; if (cclr == 1) next = 0; }   /* CMFA */
        if (v == p->t8[ch].tcorb) { p->t8[ch].tcsr |= 0x80; if (cclr == 2) next = 0; }   /* CMFB */
        p->t8[ch].tcnt = next;
    }
}
static const uint8_t frt_div[4] = { 2, 8, 32, 0 };

/* ---------------------------------------------------------------- SCI */
enum { TDRE = 0x80, RDRF = 0x40, ORER = 0x20, FER = 0x10, PER = 0x08, TEND = 0x04 };
static int sci_sync_mode(const h8p_sci *s) { return (s->smr & 0x80) != 0; }
static uint32_t sci_bit(const h8p_sci *s)
{
    if (s->scr & 2) return s->ext_bit ? s->ext_bit : 147;       /* CKE1: the external clock */
    const uint32_t n = 1u << (2 * (s->smr & 3));
    return (sci_sync_mode(s) ? 4u : 32u) * n * ((uint32_t)s->brr + 1);
}
static uint32_t sci_frame(const h8p_sci *s)
{
    if (sci_sync_mode(s)) return 8 * sci_bit(s);
    return (1 + ((s->smr & 0x40) ? 7 : 8) + ((s->smr & 0x20) ? 1 : 0) + ((s->smr & 0x08) ? 2 : 1)) * sci_bit(s);
}
static uint32_t sci_stop(const h8p_sci *s) { return sci_sync_mode(s) ? 0 : ((s->smr & 0x08) ? 2 : 1); }
static void sci_rx_byte(h8p_sci *s, uint8_t b)
{
    if (s->ssr & RDRF) { s->ssr |= ORER; return; }
    s->rdr = b; s->ssr |= RDRF;
}
/* a frame starts at t with `byte`: straight from TDR (TDRE set again at once), or the byte loaded at the previous frame's stop bit */
static void sci_tx_start(h8p *p, int ch, int64_t t, uint8_t byte, int from_tdr)
{
    h8p_sci *s = &p->sci[ch];
    const int was_busy = s->tx_busy;
    s->tx_shift = byte; if (from_tdr) s->ssr |= TDRE; s->tx_busy = 1; s->tx_loaded = 0;
    /* MAME's clocking (h8_sci.cpp): from idle the clock's first event is the next multiple of its step (a half bit clocked, 1/16 bit
     * async) and the first bit goes out there; a clocked frame that follows another directly takes one bit more (its first bit waits a step) */
    if (!was_busy || !from_tdr) {
        const int64_t step = sci_sync_mode(s) ? sci_bit(s) / 2 : sci_bit(s) / 16;
        if (!was_busy && step > 0) t = (t / step + 1) * step;
    }
    s->tx_end = t + sci_frame(s) + (sci_sync_mode(s) && was_busy ? sci_bit(s) : 0);
    if (sci_sync_mode(s) && (s->scr & 0x10) && !s->rx_busy && !(s->ssr & (ORER | FER | PER))) { s->rx_busy = 1; s->rx_done = s->tx_end; }
}
/* clocked receive alone (RE without TE): a byte is clocked in when the receiver is idle and has no error (MAME sync_rx_start) */
static void sci_sync_rx_start(h8p *p, int ch, int64_t t)
{
    h8p_sci *s = &p->sci[ch];
    if (!s->rx_busy && sci_sync_mode(s) && (s->scr & 0x10) && !(s->scr & 0x20) && !(s->ssr & (ORER | FER | PER))) {
        const int64_t step = sci_bit(s) / 2;
        s->rx_busy = 1; s->rx_done = (step ? (t / step + 1) * step : t) + sci_frame(s);
    }
}
static void sci_advance(h8p *p, int ch, int64_t to)
{
    h8p_sci *s = &p->sci[ch];
    for (int guard = 0; guard < 4096; guard++) {
        /* the transmitter's next point: the start of the stop bit (the byte is out: the receiver has it, a waiting TDR is loaded --
         * MAME's ST_STOP), then the frame's end (the next frame starts, or TEND) */
        const int64_t tload = s->tx_end - (int64_t)sci_stop(s) * sci_bit(s);
        int64_t at = -1; int which = 0;
        if (s->tx_busy) { const int64_t tt = s->tx_loaded ? s->tx_end : tload; if (tt <= to) { at = tt; which = s->tx_loaded ? 2 : 1; } }
        if (s->rx_busy && s->rx_done <= to && (at < 0 || s->rx_done < at)) { at = s->rx_done; which = 3; }
        if (s->rxn && s->rxt[s->rxh] <= to && (at < 0 || s->rxt[s->rxh] < at)) { at = s->rxt[s->rxh]; which = 4; }
        if (at < 0) return;
        switch (which) {
        case 1:                                                   /* the last data bit is out */
            s->tx_loaded = 1;
            if (sci_sync_mode(s)) { if (p->sci_sync_tx) p->sci_sync_tx(p->ctx, ch, s->tx_shift); }
            else if (p->sci_tx) p->sci_tx(p->ctx, ch, s->tx_shift, at + sci_bit(s) / 2);   /* the receiver takes it in the stop bit's middle */
            if (sci_stop(s) == 0) continue;                        /* clocked: the frame ends here too */
            if ((s->scr & 0x20) && !(s->ssr & TDRE)) { s->tx_next = s->tdr; s->tx_has_next = 1; s->ssr |= TDRE; }
            break;
        case 2:                                                   /* the frame's end */
            if (s->rx_busy && s->rx_done <= at) {                 /* a clocked receive that ran with this frame */
                s->rx_busy = 0;
                if (s->scr & 0x10) sci_rx_byte(s, p->sci_sync_rx ? p->sci_sync_rx(p->ctx, ch) : 0xFF);
            }
            if (s->tx_has_next) { s->tx_has_next = 0; sci_tx_start(p, ch, at, s->tx_next, 0); }
            else if ((s->scr & 0x20) && !(s->ssr & TDRE)) sci_tx_start(p, ch, at, s->tdr, 1);
            else { s->tx_busy = 0; s->ssr |= TEND; }
            break;
        case 3:
            s->rx_busy = 0;
            if (s->scr & 0x10) sci_rx_byte(s, p->sci_sync_rx ? p->sci_sync_rx(p->ctx, ch) : 0xFF);
            break;
        default: {
            const uint8_t b = s->rxq[s->rxh]; s->rxh = (s->rxh + 1) & 255; s->rxn--;
            if ((s->scr & 0x10) && !sci_sync_mode(s)) sci_rx_byte(s, b);
        } }
    }
}
void h8p_sci_receive(h8p *p, int ch, uint8_t byte, int64_t at)
{
    h8p_sci *s = &p->sci[ch];
    if (s->rxn == 256) return;
    const int k = (s->rxh + s->rxn) & 255;
    s->rxq[k] = byte; s->rxt[k] = at; s->rxn++;
}

/* ---------------------------------------------------------------- time */
void h8p_sync(h8p *p)
{
    const int64_t to = p->cpu->cycles, dt = to - p->now;
    if (dt <= 0) return;
    /* the prescalers run from power-on: a counter steps at absolute multiples of its divider (MAME: (time + phase) >> shift for the
     * 16-bit timers, (time + div/2) / div for the 8-bit ones), whenever it was started */
    const int64_t from = p->now;
    if (p->chip == H8P_3002) {
        for (int ch = 0; ch < 5; ch++) if (t16_running(p, ch)) {
            const int64_t div = itu_div[p->t16[ch].tcr & 7];
            t16_ticks(p, ch, to / div - from / div);
        }
    } else {
        if (frt_div[p->frt_tcr & 3]) {
            const int64_t div = frt_div[p->frt_tcr & 3], n = to / div - from / div;
            if (p->frt_cnt + n > 0xFFFF) p->frt_tcsr |= 2;     /* OVF */
            p->frt_cnt = (uint16_t)(p->frt_cnt + n);
        }
        for (int ch = 0; ch < 2; ch++) { const int64_t div = t8_div(p, ch); if (div)
            t8_ticks(p, ch, (to + div / 2) / div - (from + div / 2) / div); }
    }
    for (int ch = 0; ch < 2; ch++) sci_advance(p, ch, to);
    if ((p->adcsr & 0x20) && p->adc_done <= to) {
        memset(p->addr, 0, sizeof p->addr);                     /* the board's analogue inputs: constant 0 (MAME set_constant(0)) */
        p->adcsr |= 0x80;
        if (p->adcsr & 0x10) p->adc_done += (p->adcsr & 8) ? 134 : 266;    /* scan: again */
        else p->adcsr &= (uint8_t)~0x20;
    }
    p->now = to;
}

static int64_t min64(int64_t a, int64_t b) { return a < b ? a : b; }
int64_t h8p_next_event(h8p *p)
{
    const int64_t now = p->cpu->cycles;
    int64_t e = CAP_STATES;
    if (p->chip == H8P_3002) {
        for (int ch = 0; ch < 5; ch++) if (t16_running(p, ch) && (p->t16[ch].tier & 7)) {
            const int64_t div = itu_div[p->t16[ch].tcr & 7];
            e = min64(e, (now / div + t16_dist(p, ch, p->t16[ch].tier & 7)) * div - now);
        }
    } else {
        for (int ch = 0; ch < 2; ch++) { const int64_t div = t8_div(p, ch); if (div && (p->t8[ch].tcr & 0xE0))
            e = min64(e, ((now + div / 2) / div + t8_dist(p, ch)) * div - div / 2 - now); }
    }
    for (int ch = 0; ch < 2; ch++) {
        const h8p_sci *s = &p->sci[ch];
        if (s->tx_busy) e = min64(e, (s->tx_loaded ? s->tx_end : s->tx_end - (int64_t)sci_stop(s) * sci_bit(s)) - now);
        if (s->rx_busy) e = min64(e, s->rx_done - now);
        if (s->rxn) e = min64(e, s->rxt[s->rxh] - now);
    }
    if (p->adcsr & 0x20) e = min64(e, p->adc_done - now);
    return e < 1 ? 1 : e;
}

/* ---------------------------------------------------------------- interrupts */
/* is the source behind `vec` requesting? (vectors as the two chips number them) */
static int requesting(const h8p *p, int vec)
{
    const h8p_sci *s;
    if (p->chip == H8P_3002) {
        if (vec >= 12 && vec <= 17) { const int n = vec - 12; return (p->ier >> n & 1) && (p->isr >> n & 1); }
        if (vec >= 24 && vec < 44) {
            const int ch = (vec - 24) / 4, k = (vec - 24) % 4;
            if (k == 3) return 0;
            return (p->t16[ch].tier >> k & 1) && (p->t16[ch].tsr >> k & 1);
        }
        if (vec >= 52 && vec <= 59) { s = &p->sci[(vec - 52) / 4]; goto sci; }
        if (vec == 60) return (p->adcsr & 0xC0) == 0xC0;
        return 0;
    }
    if (vec >= 4 && vec <= 11) { const int n = vec - 4; return (p->ier >> n & 1) && (p->isr >> n & 1); }
    if (vec >= 19 && vec <= 24) {
        const int ch = (vec - 19) / 3, k = (vec - 19) % 3;               /* CMIA CMIB OVI */
        static const uint8_t en[3] = { 0x40, 0x80, 0x20 }, fl[3] = { 0x40, 0x80, 0x20 };
        return (p->t8[ch].tcr & en[k]) && (p->t8[ch].tcsr & fl[k]);
    }
    if (vec >= 27 && vec <= 34) { s = &p->sci[(vec - 27) / 4]; vec = 52 + (vec - 27) % 4; goto sci; }
    if (vec == 35) return (p->adcsr & 0xC0) == 0xC0;
    return 0;
sci:
    switch ((vec - 52) % 4) {
    case 0: return (s->scr & 0x40) && (s->ssr & (ORER | FER | PER));   /* ERI */
    case 1: return (s->scr & 0x40) && (s->ssr & RDRF);                 /* RXI */
    case 2: return (s->scr & 0x80) && (s->ssr & TDRE);                 /* TXI */
    default: return (s->scr & 0x04) && (s->ssr & TEND);                /* TEI */
    }
}
/* the H8/3002's priority bit for a vector (IPRA / IPRB, UE = 0) */
static int prio1(const h8p *p, int vec)
{
    int bit = -1, b = 0;
    if (vec == 12) bit = 7; else if (vec == 13) bit = 6; else if (vec <= 15) bit = 5; else if (vec <= 17) bit = 4;
    else if (vec <= 23) bit = 3; else if (vec < 28) bit = 2; else if (vec < 32) bit = 1; else if (vec < 36) bit = 0;
    else if (vec < 40) { b = 1; bit = 7; } else if (vec < 44) { b = 1; bit = 6; } else if (vec < 52) { b = 1; bit = 5; }
    else if (vec < 56) { b = 1; bit = 3; } else if (vec < 60) { b = 1; bit = 2; } else { b = 1; bit = 1; }
    return ((b ? p->iprb : p->ipra) >> bit) & 1;
}
/* is ANY source requesting? -- the union of every case of requesting() above, in one pass. Nearly every call finds nothing, and the
 * per-vector walk below was ~15% of a run (perf, 2026-10-08); this answers "no" without it. Must cover exactly what requesting() does. */
static int any_request(const h8p *p)
{
    int sci = 0;
    for (int ch = 0; ch < 2; ch++) {
        const h8p_sci *q = &p->sci[ch];
        sci |= ((q->scr & 0x40) && (q->ssr & (ORER | FER | PER | RDRF))) || ((q->scr & 0x80) && (q->ssr & TDRE)) || ((q->scr & 0x04) && (q->ssr & TEND));
    }
    if (sci || (p->adcsr & 0xC0) == 0xC0) return 1;
    if (p->chip == H8P_3002) {
        if (p->ier & p->isr & 0x3F) return 1;
        for (int ch = 0; ch < 5; ch++) if (p->t16[ch].tier & p->t16[ch].tsr & 7) return 1;
        return 0;
    }
    if (p->ier & p->isr) return 1;
    for (int ch = 0; ch < 2; ch++) if ((p->t8[ch].tcr & p->t8[ch].tcsr) & 0xE0) return 1;
    return 0;
}
int h8p_irq(h8p *p, int *set_ui)
{
    const uint8_t ccr = p->cpu->ccr;
    *set_ui = 0;
    if (!any_request(p)) return -1;
    const int nvec = p->chip == H8P_3002 ? 61 : 36;
    if (p->chip == H8P_3334 || (p->syscr & 8)) {               /* the H8/300, or UE = 1: I masks everything */
        if (ccr & H8_I) return -1;
        for (int v = 4; v < nvec; v++) if (requesting(p, v)) return v;
        return -1;
    }
    if ((ccr & H8_I) && (ccr & H8_UI)) return -1;
    for (int v = 12; v < nvec; v++) if (requesting(p, v) && prio1(p, v)) { *set_ui = 1; return v; }
    if (ccr & H8_I) return -1;
    for (int v = 12; v < nvec; v++) if (requesting(p, v)) return v;
    return -1;
}
void h8p_irq_taken(h8p *p, int vec)
{
    const int base = p->chip == H8P_3002 ? 12 : 4, n = vec - base;
    if (n >= 0 && n < 8 && (p->iscr >> n & 1)) p->isr &= (uint8_t)~(1u << n);   /* edge-sensed: cleared by taking it */
}
void h8p_set_irq_pin(h8p *p, int n, int asserted)
{
    const uint8_t b = (uint8_t)(1u << n), was = p->irq_pin & b;
    p->irq_pin = asserted ? (p->irq_pin | b) : (p->irq_pin & ~b);
    if (p->iscr & b) { if (asserted && !was) p->isr |= b; }     /* falling edge */
    else p->isr = asserted ? (p->isr | b) : (p->isr & ~b);      /* level */
}

/* ---------------------------------------------------------------- registers */
int h8p_owns(const h8p *p, uint32_t a)
{
    return p->chip == H8P_3002 ? (a >= 0xFFFF1C && a <= 0xFFFFFF) : (a >= 0xFF80 && a <= 0xFFFF);
}

uint8_t h8p_read(h8p *p, uint32_t a)
{
    h8p_sync(p);
    const uint8_t o = (uint8_t)a;
    for (int n = 1; n < 12; n++) {
        if (port_tab(p)[n][1] == o) return port_read(p, n);
        if (port_tab(p)[n][0] == o && o) return 0xFF;
    }
    if (p->chip == H8P_3002) {
        if (o >= 0x64 && o < 0xA0 && o != 0x90 && o != 0x91) {
            const int ch = o < 0x6E ? 0 : o < 0x78 ? 1 : o < 0x82 ? 2 : o < 0x92 ? 3 : 4;
            const uint8_t base = ch == 4 ? 0x92 : (uint8_t)(0x64 + ch * 10), r = (uint8_t)(o - base);
            switch (r) {
            case 0: return p->t16[ch].tcr | 0x80;
            case 1: return p->t16[ch].tior | 0x88;
            case 2: return p->t16[ch].tier | 0xF8;
            case 3: return p->t16[ch].tsr | 0xF8;
            case 4: return (uint8_t)(p->t16[ch].tcnt >> 8);
            case 5: return (uint8_t)p->t16[ch].tcnt;
            case 6: return (uint8_t)(p->t16[ch].gra >> 8);
            case 7: return (uint8_t)p->t16[ch].gra;
            case 8: return (uint8_t)(p->t16[ch].grb >> 8);
            case 9: return (uint8_t)p->t16[ch].grb;
            default: return p->reg[o];
            }
        }
        switch (o) {
        case 0x60: return p->tstr | 0xE0;
        case 0x61: return p->tsnc | 0xE0;
        case 0x62: return p->tmdr | 0x80;
        case 0x63: return p->tfcr | 0xC0;
        case 0x90: return p->toer | 0xC0;
        case 0x91: return p->tocr | 0xEC;
        case 0xF2: return p->syscr;
        case 0xF4: return p->iscr | 0xC0;
        case 0xF5: return p->ier | 0xC0;
        case 0xF6: return p->isr | 0xC0;
        case 0xF8: return p->ipra;
        case 0xF9: return p->iprb | 0x10;
        }
    } else {
        switch (o) {
        case 0x90: return p->frt_tier | 0x01;
        case 0x91: return p->frt_tcsr;
        case 0x92: return (uint8_t)(p->frt_cnt >> 8);
        case 0x93: return (uint8_t)p->frt_cnt;
        case 0x96: return p->frt_tcr;
        case 0xC3: return p->stcr;
        case 0xC4: return p->syscr;
        case 0xC6: return p->iscr;
        case 0xC7: return p->ier;
        case 0xC8: case 0xD0: return p->t8[o == 0xD0].tcr;
        case 0xC9: case 0xD1: return p->t8[o == 0xD1].tcsr | 0x10;
        case 0xCA: case 0xD2: return p->t8[o == 0xD2].tcora;
        case 0xCB: case 0xD3: return p->t8[o == 0xD3].tcorb;
        case 0xCC: case 0xD4: return p->t8[o == 0xD4].tcnt;
        }
    }
    /* SCI: H8/3002 0xB0 / 0xB8, H8/3334 0xD8 / 0x88 */
    for (int ch = 0; ch < 2; ch++) {
        const uint8_t base = p->chip == H8P_3002 ? (uint8_t)(0xB0 + 8 * ch) : (ch ? 0x88 : 0xD8);
        if (o >= base && o < base + 6) {
            h8p_sci *s = &p->sci[ch];
            switch (o - base) {
            case 0: return s->smr; case 1: return s->brr; case 2: return s->scr; case 3: return s->tdr;
            case 4: s->ssr_read = s->ssr; return s->ssr; case 5: return s->rdr;
            }
        }
    }
    if (o == 0xE8) return p->adcsr;
    if (o == 0xE9) return p->adcr;
    if (o >= 0xE0 && o <= 0xE7) { const uint16_t d = p->addr[(o - 0xE0) >> 1]; return (o & 1) ? (uint8_t)((d & 3) << 6) : (uint8_t)(d >> 2); }
    if (o == 0xA8) return p->wd_tcsr | (p->chip == H8P_3002 ? 0x18 : 0x10);
    if (o == 0xA9) return p->wd_tcnt;
    return p->reg[o];
}

void h8p_write(h8p *p, uint32_t a, uint8_t v)
{
    h8p_sync(p);
    const uint8_t o = (uint8_t)a;
    p->reg[o] = v;
    for (int n = 1; n < 12; n++) {
        if (port_tab(p)[n][1] == o) { p->dr[n] = v; port_out(p, n); return; }
        if (port_tab(p)[n][0] == o && o) { p->ddr[n] = (uint8_t)(v | p->pmask[n]); port_out(p, n); return; }
    }
    if (p->chip == H8P_3002) {
        if (o >= 0x64 && o < 0xA0 && o != 0x90 && o != 0x91) {
            const int ch = o < 0x6E ? 0 : o < 0x78 ? 1 : o < 0x82 ? 2 : o < 0x92 ? 3 : 4;
            const uint8_t base = ch == 4 ? 0x92 : (uint8_t)(0x64 + ch * 10), r = (uint8_t)(o - base);
            switch (r) {
            case 0: p->t16[ch].tcr = v & 0x7F; break;
            case 1: p->t16[ch].tior = v & 0x77; break;
            case 2: p->t16[ch].tier = v & 7; break;
            case 3: p->t16[ch].tsr &= (uint8_t)(v | ~7u); break;                  /* flags clear by writing 0 */
            case 4: p->t16[ch].tcnt = (uint16_t)((p->t16[ch].tcnt & 0x00FF) | v << 8); break;
            case 5: p->t16[ch].tcnt = (uint16_t)((p->t16[ch].tcnt & 0xFF00) | v); break;
            case 6: p->t16[ch].gra = (uint16_t)((p->t16[ch].gra & 0x00FF) | v << 8); break;
            case 7: p->t16[ch].gra = (uint16_t)((p->t16[ch].gra & 0xFF00) | v); break;
            case 8: p->t16[ch].grb = (uint16_t)((p->t16[ch].grb & 0x00FF) | v << 8); break;
            case 9: p->t16[ch].grb = (uint16_t)((p->t16[ch].grb & 0xFF00) | v); break;
            }
            return;
        }
        switch (o) {
        case 0x60: p->tstr = v & 0x1F; return;
        case 0x61: p->tsnc = v & 0x1F; return;
        case 0x62: p->tmdr = v & 0x7F; return;
        case 0x63: p->tfcr = v & 0x3F; return;
        case 0x90: p->toer = v & 0x3F; return;
        case 0x91: p->tocr = v & 0x13; return;
        case 0xF2: p->syscr = v; return;
        case 0xF4: p->iscr = v & 0x3F; return;
        case 0xF5: p->ier = v & 0x3F; return;
        case 0xF6: p->isr &= v; return;
        case 0xF8: p->ipra = v; return;
        case 0xF9: p->iprb = v & 0xEE; return;
        }
    } else {
        switch (o) {
        case 0x90: p->frt_tier = v; return;
        case 0x91: p->frt_tcsr = (uint8_t)((p->frt_tcsr & v) | (v & 1)); return;
        case 0x92: p->frt_cnt = (uint16_t)((p->frt_cnt & 0x00FF) | v << 8); return;
        case 0x93: p->frt_cnt = (uint16_t)((p->frt_cnt & 0xFF00) | v); return;
        case 0x96: p->frt_tcr = v; return;
        case 0xC3: p->stcr = v; return;
        case 0xC4: p->syscr = v; return;
        case 0xC6: p->iscr = v; return;
        case 0xC7: p->ier = v; return;
        case 0xC8: case 0xD0: p->t8[o == 0xD0].tcr = v; return;
        case 0xC9: case 0xD1: { const int ch = o == 0xD1; p->t8[ch].tcsr = (uint8_t)((p->t8[ch].tcsr & v & 0xE0) | (v & 0x1F)); return; }
        case 0xCA: case 0xD2: p->t8[o == 0xD2].tcora = v; return;
        case 0xCB: case 0xD3: p->t8[o == 0xD3].tcorb = v; return;
        case 0xCC: case 0xD4: p->t8[o == 0xD4].tcnt = v; return;
        }
    }
    for (int ch = 0; ch < 2; ch++) {
        const uint8_t base = p->chip == H8P_3002 ? (uint8_t)(0xB0 + 8 * ch) : (ch ? 0x88 : 0xD8);
        if (o >= base && o < base + 6) {
            h8p_sci *s = &p->sci[ch];
            switch (o - base) {
            case 0: s->smr = v; break;
            case 1: s->brr = v; break;
            case 2: if (!(v & 0x10)) s->rx_busy = 0; s->scr = v; break;
            case 3: s->tdr = v; break;
            case 4:                                               /* MAME h8_sci ssr_w: a flag clears only where it was READ as 1; TDRE only
                                                                   * with TE; TEND / MPB read-only, MPBT written */
                if ((s->scr & 0x20) && (s->ssr & s->ssr_read & TDRE) && !(v & TDRE)) s->ssr &= (uint8_t)~(TDRE | TEND);
                s->ssr = (uint8_t)(((s->ssr & (~s->ssr_read | v | TDRE | TEND | 0x02)) & ~1u) | (v & 1));
                s->ssr_read &= s->ssr;
                if (!s->tx_busy && !(s->ssr & TDRE)) sci_tx_start(p, ch, p->now, s->tdr, 1);
                break;
            case 5: break;
            }
            sci_sync_rx_start(p, ch, p->now);
            return;
        }
    }
    if (o == 0xE8) {
        const uint8_t was = p->adcsr;
        p->adcsr = (uint8_t)((p->adcsr & v & 0x80) | (v & 0x7F));
        if ((v & 0x20) && !(was & 0x20)) p->adc_done = p->now + ((v & 8) ? 134 : 266);
        return;
    }
    if (o == 0xE9) { p->adcr = v & 0x80; return; }
}

/* the watchdog's words: 0xA5xx -> TCSR, 0x5Axx -> TCNT (a word write; the board routes it here as its two bytes) */
static void wd_word(h8p *p, uint16_t w)
{
    if ((w >> 8) == 0xA5) p->wd_tcsr = (uint8_t)w;
    else if ((w >> 8) == 0x5A) p->wd_tcnt = (uint8_t)w;
}

void h8p_reset(h8p *p)
{
    p->now = p->cpu->cycles;
    p->ier = p->isr = p->iscr = 0; p->ipra = p->iprb = 0; p->irq_pin = 0;
    p->syscr = p->chip == H8P_3002 ? 0x0B : 0x09;
    memset(p->t16, 0, sizeof p->t16);
    for (int ch = 0; ch < 5; ch++) p->t16[ch].gra = p->t16[ch].grb = 0xFFFF;
    p->tstr = p->tsnc = p->tmdr = p->tfcr = p->toer = p->tocr = 0;
    p->frt_cnt = 0; p->frt_tcr = p->frt_tier = p->frt_tcsr = 0;
    memset(p->t8, 0, sizeof p->t8);
    for (int ch = 0; ch < 2; ch++) p->t8[ch].tcora = p->t8[ch].tcorb = 0xFF;
    p->stcr = 0;
    for (int ch = 0; ch < 2; ch++) {
        const uint32_t ext = p->sci[ch].ext_bit;
        memset(&p->sci[ch], 0, sizeof p->sci[ch]);
        p->sci[ch].brr = 0xFF; p->sci[ch].ssr = TDRE | TEND; p->sci[ch].ext_bit = ext;
    }
    p->adcsr = p->adcr = 0; memset(p->addr, 0, sizeof p->addr);
    p->wd_tcsr = p->wd_tcnt = 0;
    memset(p->dr, 0, sizeof p->dr); memset(p->ddr, 0, sizeof p->ddr); memset(p->pmask, 0, sizeof p->pmask);
    if (p->chip == H8P_3002) {                       /* MAME h83002: H8_PORT(port, default ddr, mask) */
        p->pmask[6] = 0x80; p->ddr[6] = 0x80;
        p->pmask[8] = 0xE0; p->ddr[8] = 0xF0;
        p->pmask[9] = 0xC0;
    } else {
        p->pmask[5] = 0xF8; p->ddr[5] = 0xF8;
        p->pmask[8] = 0x80; p->ddr[8] = 0x80;
    }
    for (int n = 1; n < 12; n++) if (port_tab(p)[n][1]) port_out(p, n);
    (void)wd_word;
}

void h8p_init(h8p *p, int chip, h8_cpu *cpu, void *ctx)
{
    memset(p, 0, sizeof *p);
    p->chip = chip; p->cpu = cpu; p->ctx = ctx;
}

/* the board calls this for the watchdog's 16-bit writes (both chips: 0xFFA8 / 0xFFFFA8) */
void h8p_write_word(h8p *p, uint32_t a, uint16_t v)
{
    h8p_sync(p);
    if ((uint8_t)a == 0xA8) { wd_word(p, v); return; }
    h8p_write(p, a, (uint8_t)(v >> 8)); h8p_write(p, a + 1, (uint8_t)v);
}
