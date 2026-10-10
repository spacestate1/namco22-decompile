/*
 * h8_periph.h -- the ON-CHIP PERIPHERALS of Time Crisis 2's two H8s, our own code: the sub-CPU's H8/3002 (H8/300H) and the gun I/O board's
 * H8/3334 (H8/300). From the chips' hardware manuals (Hitachi H8/3002 ADE-602-077, H8/3334 ADE-602-061); register addresses and interrupt
 * vector numbers checked against MAME's device maps (h83002.cpp, h83337.cpp), whose traces the translated programs were gated against.
 *
 * What is modelled -- exactly what the two programs use (tools/h8gate H8GATE_WLOG lists every register write):
 *   interrupt controller  IER / ISR / ISCR (IRQn pins, level or falling edge), IPRA / IPRB + SYSCR.UE (the H8/300H's two priority levels)
 *   H8/3002 ITU           5 channels: TSTR, TCR (TPSC prescale, CCLR), TIER, TSR (IMFA / IMFB / OVF), TCNT, GRA, GRB
 *   H8/3334 FRT           the 16-bit free-running counter (TCR CKS); the 8-bit timers 0/1: TCR, TCSR, TCORA/B, TCNT
 *   SCI 0/1               async and clocked-synchronous; TDR / RDR / SSR (TDRE RDRF ORER FER PER TEND) / SCR, the frame time from BRR,
 *                         or an external clock; RXI / TXI / TEI / ERI. A transmitted byte leaves through tx(); a byte arriving is
 *                         h8p_sci_receive()'d with its arrival time; in clocked mode a received byte is asked of sync_rx() (the device clocked)
 *   A/D                   ADCSR / ADCR / the data registers (single mode; the inputs the board wires: constant 0 on both)
 *   ports                 DDR / DR, read = (DR & DDR) | (pins & ~DDR) over the port's existing bits, writes reported to the board
 *   watchdog              its two write-protected words (stored)
 * Time: the CPU's own state count (h8_cpu.cycles). h8p_sync(p) brings the peripherals up to it; h8p_next_event() says how many states the
 * CPU may run before something can change; h8p_irq() picks the interrupt to take now.
 */
#ifndef H8_PERIPH_H
#define H8_PERIPH_H
#include <stdint.h>
#include "h8_sem.h"

enum { H8P_3002, H8P_3334 };

typedef struct {
    uint8_t smr, brr, scr, tdr, ssr, rdr;
    uint32_t ext_bit;                           /* states per bit when SCR.CKE selects the external clock (0 = none wired) */
    uint8_t  ssr_read;                          /* SSR as last read: a flag clears by writing 0 only after it was read as 1 */
    int      tx_busy, tx_loaded, tx_has_next; int64_t tx_end; uint8_t tx_shift, tx_next;
    int      rx_busy; int64_t rx_done;          /* clocked mode: a byte being clocked in */
    uint8_t  rxq[256]; int64_t rxt[256]; int rxh, rxn;   /* async: bytes on the line, with the state their stop bit ends */
} h8p_sci;

typedef struct h8p {
    int chip;
    h8_cpu *cpu;
    int64_t now;                                /* states the peripherals have been brought up to */
    void *ctx;                                  /* the board */
    uint8_t (*port_in)(void *ctx, int port);                      /* the pins of port n (1..0xB) */
    void    (*port_out)(void *ctx, int port, uint8_t data, uint8_t ddr);
    void    (*sci_tx)(void *ctx, int ch, uint8_t byte, int64_t at);   /* a byte has left (its last bit ends at state `at`) */
    uint8_t (*sci_sync_rx)(void *ctx, int ch);                    /* clocked mode: the 8 bits the device shifts in */
    void    (*sci_sync_tx)(void *ctx, int ch, uint8_t byte);      /* clocked mode: the 8 bits shifted out */
    /* interrupt controller */
    uint8_t ier, isr, iscr, ipra, iprb, syscr;
    uint8_t irq_pin;                            /* IRQn asserted (1 = the pin is low, active) */
    /* ports: index = port number 1..0xB (A = 0xA, B = 0xB) */
    uint8_t dr[12], ddr[12], pmask[12];
    /* H8/3002 ITU */
    struct { uint8_t tcr, tior, tier, tsr; uint16_t tcnt, gra, grb; } t16[5];
    uint8_t tstr, tsnc, tmdr, tfcr, toer, tocr;
    /* H8/3334 FRT + 8-bit timers */
    uint16_t frt_cnt; uint8_t frt_tcr, frt_tier, frt_tcsr;
    struct { uint8_t tcr, tcsr, tcora, tcorb, tcnt; } t8[2];
    uint8_t stcr;
    h8p_sci sci[2];
    /* A/D */
    uint8_t adcsr, adcr; uint16_t addr[4]; int64_t adc_done;
    /* watchdog */
    uint8_t wd_tcsr, wd_tcnt;
    uint8_t reg[256];                           /* every other register: kept as written */
} h8p;

void     h8p_init(h8p *p, int chip, h8_cpu *cpu, void *ctx);
void     h8p_reset(h8p *p);
void     h8p_sync(h8p *p);                       /* bring the peripherals up to p->cpu->cycles */
int64_t  h8p_next_event(h8p *p);                 /* states from now to the next possible interrupt-relevant change (capped) */
int      h8p_irq(h8p *p, int *set_ui);           /* the vector to take now (CCR permitting), or -1 */
void     h8p_irq_taken(h8p *p, int vector);      /* an edge IRQ's ISR bit clears when it is taken */
void     h8p_set_irq_pin(h8p *p, int n, int asserted);
int      h8p_owns(const h8p *p, uint32_t a);     /* a register address of this chip */
uint8_t  h8p_read(h8p *p, uint32_t a);
void     h8p_write(h8p *p, uint32_t a, uint8_t v);
void     h8p_write_word(h8p *p, uint32_t a, uint16_t v);  /* a 16-bit register write (the watchdog's protected words) */
void     h8p_sci_receive(h8p *p, int ch, uint8_t byte, int64_t at);   /* async: a byte whose stop bit ends at state `at` of THIS cpu */
void     h8p_port_changed(h8p *p, int port);    /* the board's pins changed: nothing latched, reads see them directly */
#endif
