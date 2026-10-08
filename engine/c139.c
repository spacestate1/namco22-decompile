/* c139.c -- see c139.h */
#include <string.h>
#include "c139.h"

#define RING_BASE 0x1000u                /* the receive ring's first word */

static uint8_t *ram;
static bool (*irq_cb)(void);
static uint16_t reg[8];
static bool tx_busy, tx_done, rx_flag;   /* a transfer under way / finished, not yet seen by the handler / a received frame in the ring */
static bool line_up, read_since_up, read_rx;   /* the line is raised / the handler read status since / what it saw */
static c139_frame txq[C139_QUEUE], rxq[C139_QUEUE];
static int txr, txw, rxr, rxw;
static c139_stats st;
static unsigned mode;                    /* C139_* settings (c139.h) */

static uint16_t ram_w(uint32_t idx)
{
    idx &= 0x1FFF;
    if (mode & C139_HOST_WORDS) return ((uint16_t *)ram)[idx];
    return (uint16_t)(ram[idx * 2] << 8 | ram[idx * 2 + 1]);
}
static void ram_put(uint32_t idx, uint16_t v)
{
    idx &= 0x1FFF;
    if (mode & C139_HOST_WORDS) { ((uint16_t *)ram)[idx] = v; return; }
    ram[idx * 2] = (uint8_t)(v >> 8); ram[idx * 2 + 1] = (uint8_t)v;
}
void c139_set_mode(unsigned flags) { mode = flags; if (flags & C139_MASKED_IRQ) reg[1] = 0xF; }
bool c139_tx_busy(void) { return tx_busy; }

void c139_init(uint8_t *r, bool (*irq)(void))
{
    ram = r; irq_cb = irq; mode = 0;
    memset(reg, 0, sizeof reg);
    tx_busy = tx_done = rx_flag = line_up = read_since_up = read_rx = false;
    txr = txw = rxr = rxw = 0;
    memset(&st, 0, sizeof st);
}

uint16_t c139_reg_read(unsigned r)
{
    r &= 7;
    if (r == 0) {
        read_since_up = true; read_rx = rx_flag;
        if (mode & C139_MASKED_IRQ) return (uint16_t)((reg[0] & ~6u) | (rx_flag ? 2 : 0) | (tx_busy ? 0 : 4));
        return (uint16_t)(0x0004 | (rx_flag ? 2 : 0));
    }
    if (r == 5) return tx_busy ? reg[5] : 0;
    return reg[r];
}

/* send `n` words from word `p`: a pointer into the receive ring wraps inside the ring, as the game's own scan of it does */
static void transmit(uint32_t p, unsigned n)
{
    c139_frame *f = &txq[txw];
    if (n > C139_FRAME_MAX) n = C139_FRAME_MAX;
    f->n = (uint16_t)n;
    for (unsigned k = 0; k < n; k++) {
        uint32_t idx = p >= RING_BASE ? RING_BASE + ((p - RING_BASE + k) & 0xFFF) : ((p + k) & 0x1FFF);
        f->w[k] = (uint16_t)(ram_w(idx) & 0x1FF);
    }
    txw = (txw + 1) % C139_QUEUE;
    if (txw == txr) { txr = (txr + 1) % C139_QUEUE; st.tx_drop++; }
    st.tx++;
}

void c139_reg_write(unsigned r, uint16_t v)
{
    r &= 7;
    reg[r] = v;
    if (r == 1 && (mode & C139_MASKED_IRQ)) reg[1] &= 0xF;
    if (r == 0) { if (!(v & 2)) rx_flag = false; }
    else if (r == 5) {
        if ((v & 0xFF) && !tx_busy) { transmit(reg[7] & 0x1FFF, v & 0x1FF); tx_busy = true; }
    }
}

static void place(const c139_frame *f)
{
    uint32_t p = reg[6] & 0xFFF;
    if (mode & C139_PTR_LAST) {                          /* the pointer names the last word: write after it, end on the last */
        for (unsigned k = 0; k < f->n; k++) { p = (p + 1) & 0xFFF; ram_put(RING_BASE + p, f->w[k]); }
        reg[6] = (uint16_t)((reg[6] & 0xF000) | p);
        rx_flag = true;
        st.rx++;
        return;
    }
    for (unsigned k = 0; k < f->n; k++) ram_put(RING_BASE + ((p + k) & 0xFFF), f->w[k]);
    p = (p + f->n) & 0xFFF;
    reg[6] = (uint16_t)((reg[6] & 0xF000) | p);
    rx_flag = true;
    st.rx++;
}

void c139_tick(void)
{
    if (mode & C139_MASKED_IRQ) {                        /* the transmitter's idle is a level; each cause masks itself as it interrupts */
        tx_busy = false; tx_done = false;
        if (!rx_flag && rxr != rxw) { place(&rxq[rxr]); rxr = (rxr + 1) % C139_QUEUE; }
        uint16_t m = 0;
        if (rx_flag && !(reg[1] & 2)) m |= 2;
        if (!(reg[1] & 4)) m |= 4;
        if (m && irq_cb()) { reg[1] |= m; st.irq++; }  /* a disabled line: try again next tick */
        return;
    }
    if (tx_busy) { tx_busy = false; tx_done = true; }
    if (!rx_flag && !line_up && !tx_done && rxr != rxw) { place(&rxq[rxr]); rxr = (rxr + 1) % C139_QUEUE; }
    if ((tx_done || rx_flag) && !line_up && irq_cb()) {
        line_up = true; read_since_up = false;
        st.irq++;
    }
}

void c139_irq_ack(void)
{
    if (!line_up) return;
    line_up = false;
    if (!read_since_up) { tx_done = false; rx_flag = false; return; }   /* a handler that never looked (a test mode): the event is gone */
    if (!read_rx) tx_done = false;                                      /* it took the "TX done" path */
}

bool c139_line(void) { return line_up; }
void c139_line_drop(void) { line_up = false; }

bool c139_tx_take(c139_frame *out)
{
    if (txr == txw) return false;
    *out = txq[txr];
    txr = (txr + 1) % C139_QUEUE;
    return true;
}

void c139_rx_put(const c139_frame *f)
{
    rxq[rxw] = *f;
    rxw = (rxw + 1) % C139_QUEUE;
    if (rxw == rxr) { rxr = (rxr + 1) % C139_QUEUE; st.rx_drop++; }
}

c139_stats c139_get_stats(void) { return st; }
