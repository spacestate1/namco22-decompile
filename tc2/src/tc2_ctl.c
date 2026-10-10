#include <stdlib.h>
/*
 * tc2_ctl.c -- the System 23 "CTL" block at 0x0D000000 (our own code for a fixed-function part; its behaviour as MAME's namcos23.cpp gives it,
 * ctl_* handlers, which the boot path matches instruction for instruction):
 *   0x0D000000 W  the board's 8 LEDs (kept, not shown yet)
 *   0x0D000002 R  status: the DIP switches (low byte) | 0x0400 while the C361 (raster) interrupt is pending
 *   0x0D000004 RW / 0x0D000006 RW   two 12-bit SERIAL input shifters for the operator switch ports P1 / P2: a write latches the port, each
 *                 read returns bit 11 as 0xFFFF / 0x0000 and shifts left, filling with 1s. Both start at 0 (MAME's machine_reset).
 *   0x0D00000A W  vertical-blank acknowledge (src/tc2_mem.c)
 * The ports are active low: P1 = the "Dev Service" switches (0xFFF = none pressed), P2 unused (0xFFF), DIP = 0xFF (all off: no service
 * mode, POST not skipped).
 */
#include <stdint.h>
#include "lift_rt.h"
#include "tc2_host.h"
int tc2_irq_pending(uint32_t bit);

uint16_t tc2_ctl_p1 = 0xFFF, tc2_ctl_p2 = 0xFFF, tc2_ctl_dsw = 0xFF;
__attribute__((constructor)) static void ctl_dsw_env(void) { const char *e = getenv("TC2_DSW"); if (e) tc2_ctl_dsw = (uint16_t)strtoul(e, NULL, 16); }   /* test: $TC2_DSW=FE = the Service Mode DIP on */
uint8_t tc2_ctl_leds;
static uint16_t shifter[2];

/* returns 1 and the value when the CTL block owns the address */
int tc2_ctl_read(uint32_t p, int size, uint32_t *v)
{
    if (size != 2 || p < 0x0D000002u || p > 0x0D000006u || (p & 1)) return 0;
    if (p == 0x0D000002u) { *v = tc2_ctl_dsw | (tc2_irq_pending(TC2_IP_RASTER) ? 0x400u : 0); return 1; }
    const int k = p == 0x0D000006u;
    *v = (shifter[k] & 0x800) ? 0xFFFFu : 0x0000u;
    shifter[k] = (uint16_t)(shifter[k] << 1 | 1);
    return 1;
}

int tc2_ctl_write(uint32_t p, int size, uint32_t v)
{
    if (p == 0x0D000000u) { tc2_ctl_leds = (uint8_t)v; return 1; }
    if (size != 2 || (p != 0x0D000004u && p != 0x0D000006u)) return 0;
    shifter[p == 0x0D000006u] = p == 0x0D000006u ? tc2_ctl_p2 : tc2_ctl_p1;
    return 1;
}
