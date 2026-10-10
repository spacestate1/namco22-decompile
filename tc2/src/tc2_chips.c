/*
 * tc2_chips.c -- System 23 register-level chips, our own code for fixed-function parts. Behaviour as MAME's namcos23.cpp (mips_map; the gate
 * is the instruction match against MAME from power-on and from the frame-1500 snapshot). The interrupt wiring is MAME's machine_start:
 * CPU IRQ0 = vblank (Cause IP2), IRQ1 = C361 raster + sub-CPU (IP3), IRQ2 = C435 (IP4), IRQ3 = C422 (IP5), IRQ5 = RS-232 (IP7).
 *
 *   C435 (3D command/DMA)  0x01000028 R   busy flag: always 1 (MAME: "c435 read busy flag")
 *   C417 (geometry)        0x02000000 R   status: 0x008E | test_mode << 15 ;  W: C435 PIO command word (the renderer's input -- not
 *                                          consumed yet)
 *                          0x02000002 RW  RAM address           0x02000008 RW  RAM[address] (64K words)
 *                          0x02000004 W   point-ROM address (shifted in 16 bits a write)
 *                          0x02000006 R   "test done": 0, leaves test mode ;  W: point-ROM address = 0, enters test mode
 *                          0x0200000E W   acknowledges the C435 interrupt (IP4)
 *                          0x0200000A/C   point-ROM reads: not modelled yet (the point-ROM chips' word layout) -- still replayed
 *   C422                   0x06400000-0x0640000F  sixteen-bit registers; a write to +2 of 0xFFFB raises its interrupt (IP5), 0x000F acks it
 *   C412 (frame buffers)   0x0C000004 W flags   0x0C000006 R 0x0002 ("the game uploads")   0x0C000010 / 12 RW the word address (low / high)
 *                          0x0C000014 RW the word at it -- SDRAM A 0x000000, SDRAM B 0x100000 (1M words each), SRAM 0x200000 (128K: the alpha-
 *                          cutout tiles), PCZ RAM 0x220000 (512); a write steps the address, a read does not; 0x0C000018 R toggles bit 0
 *   C421                   0x0C400000 RW the word at the address (DRAM A 0x00000, DRAM B 0x40000, SRAM 0x80000; the address in the low 20
 *                          bits, no step on a write inside them -- MAME's c421_ram_w as written), 0x0C400004 / 06 RW the address (high / low)
 *   (the POWER ON TEST writes and reads all of these back: MAME passes it with exactly this behaviour)
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "lift_rt.h"
#include "tc2_host.h"

#define IP_C435 0x1000u
#define IP_C422 0x2000u
static struct { uint16_t adr; uint32_t ptrom_adr; int test_mode; uint16_t ram[0x10000]; } c417;
static uint16_t c422[8];
static struct { uint16_t sdram_a[0x100000], sdram_b[0x100000], sram[0x20000], pczram[0x200]; uint32_t adr; uint16_t status; } c412;
static struct { uint16_t dram_a[0x40000], dram_b[0x40000], sram[0x8000]; uint32_t adr; } c421;
static uint16_t *c412_at(uint32_t a)
{
    if (a < 0x100000u) return &c412.sdram_a[a & 0xFFFFF];
    if (a < 0x200000u) return &c412.sdram_b[a & 0xFFFFF];
    if (a < 0x220000u) return &c412.sram[a & 0x1FFFF];
    if (a < 0x220200u) return &c412.pczram[a & 0x1FF];
    return NULL;
}
static uint16_t *c421_at(uint32_t a)
{
    a &= 0xFFFFF;
    if (a < 0x40000u) return &c421.dram_a[a & 0x3FFFF];
    if (a < 0x80000u) return &c421.dram_b[a & 0x3FFFF];
    if (a < 0x88000u) return &c421.sram[a & 0x7FFF];
    return NULL;
}
uint16_t *tc2_c412_sram(void) { return c412.sram; }      /* for the renderer */
unsigned long tc2_c435_pio_words;     /* command words written to the C435 PIO port (counted: the renderer will consume them) */

void tc2_irq_line(uint32_t bit, int on);
static void cause_set(uint32_t bit, int on) { tc2_irq_line(bit, on); }

/* the addresses these models own (src/tc2_mem.c drops their replay records) */
int tc2_chips_owns(uint32_t p)
{
    return (p >= 0x01000028u && p <= 0x0100002Bu) || (p >= 0x02000000u && p <= 0x02000009u) || (p >= 0x06400000u && p <= 0x0640000Fu)
        || (p >= 0x0C000004u && p <= 0x0C000019u) || (p >= 0x0C400000u && p <= 0x0C400007u) || p == 0x0E800000u;
}

int tc2_chips_read(uint32_t p, int size, uint32_t *v)
{
    if (p == 0x01000028u && size == 4) { *v = 1; return 1; }
    if (size != 2) return 0;
    switch (p) {
    case 0x02000000u: *v = 0x008Eu | (c417.test_mode ? 0x8000u : 0); return 1;
    case 0x02000002u: *v = c417.adr; return 1;
    case 0x02000006u: c417.test_mode = 0; *v = 0; return 1;
    case 0x02000008u: *v = c417.ram[c417.adr]; return 1;
    case 0x0C000006u: *v = 0x0002; return 1;
    case 0x0C000010u: *v = (uint16_t)c412.adr; return 1;
    case 0x0C000012u: *v = (uint16_t)(c412.adr >> 16); return 1;
    case 0x0C000014u: { const uint16_t *m = c412_at(c412.adr); *v = m ? *m : 0xFFFF; return 1; }
    case 0x0C000018u: c412.status ^= 1; *v = c412.status; return 1;
    case 0x0C400000u: { const uint16_t *m = c421_at(c421.adr); *v = m ? *m : 0xFFFF; return 1; }
    case 0x0C400004u: *v = (uint16_t)(c421.adr >> 16); return 1;
    case 0x0C400006u: *v = (uint16_t)c421.adr; return 1;
    case 0x0E800000u: *v = 3; return 1;                     /* MAME sub_comm_status_r: the RS-232 link to the sub-board, always ready */
    }
    if (p >= 0x06400000u && p <= 0x0640000Fu) { *v = c422[(p - 0x06400000u) >> 1]; return 1; }
    return 0;
}

int tc2_chips_write(uint32_t p, int size, uint32_t v)
{
    if (size != 2) return 0;
    switch (p) {
    case 0x02000000u: tc2_c435_pio_words++; return 1;
    case 0x02000002u: c417.adr = (uint16_t)v; return 1;
    case 0x02000004u: c417.ptrom_adr = c417.ptrom_adr << 16 | (v & 0xFFFFu); return 1;
    case 0x02000006u: c417.ptrom_adr = 0; c417.test_mode = 1; return 1;
    case 0x02000008u: c417.ram[c417.adr] = (uint16_t)v; return 1;
    case 0x0200000Eu: cause_set(IP_C435, 0); return 1;
    case 0x0C000004u: return 1;                             /* C412 flags (bit 0: CZ on) */
    case 0x0C000010u: c412.adr = (c412.adr & 0xFFFF0000u) | (v & 0xFFFFu); return 1;
    case 0x0C000012u: c412.adr = (c412.adr & 0x0000FFFFu) | (v & 0xFFFFu) << 16; return 1;
    case 0x0C000014u: {
        uint16_t *m = c412_at(c412.adr);
        if (c412.adr >= 0x220000u && c412.adr < 0x220200u) {     /* $TC2_PCZLOG: every PCZ write that changes the table */
            static int dbg = -1; if (dbg < 0) dbg = getenv("TC2_PCZLOG") != NULL;
            if (dbg && m && *m != (uint16_t)v) { extern uint32_t rr_frame; fprintf(stderr, "[PCZ] frame %u: [%03X] %04X -> %04X\n", rr_frame, c412.adr & 0x1FF, *m, (unsigned)(uint16_t)v); }
        }
        if (m) *m = (uint16_t)v; c412.adr++; return 1; }
    case 0x0C400000u: { uint16_t *m = c421_at(c421.adr); if (m) *m = (uint16_t)v; else c421.adr += 2; return 1; }
    case 0x0C400004u: c421.adr = (c421.adr & 0x0000FFFFu) | (v & 0xFFFFu) << 16; return 1;
    case 0x0C400006u: c421.adr = (c421.adr & 0xFFFF0000u) | (v & 0xFFFFu); return 1;
    }
    if (p >= 0x06400000u && p <= 0x0640000Fu) {
        const unsigned k = (p - 0x06400000u) >> 1;
        if (k == 1) { if (v == 0xFFFBu) cause_set(IP_C422, 1); else if (v == 0x000Fu) cause_set(IP_C422, 0); }
        c422[k] = (uint16_t)v;
        return 1;
    }
    return 0;
}

/* a snapshot's chip state (tools/mame/snapshot.lua writes SNAP.chips) */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
int tc2_chips_load(const char *path)
{
    FILE *f = fopen(path, "r"); if (!f) return 0;
    char name[32]; int n = 0;
    while (fscanf(f, "%31s", name) == 1) {
        if (!strcmp(name, "c422")) { unsigned k, v; if (fscanf(f, "%u %x", &k, &v) == 2 && k < 8) c422[k] = (uint16_t)v; }
        else if (!strcmp(name, "c417_test")) { if (fscanf(f, "%d", &c417.test_mode) != 1) break; }
        else if (!strcmp(name, "c417_adr")) { unsigned v; if (fscanf(f, "%x", &v) == 1) c417.adr = (uint16_t)v; }
        else if (!strcmp(name, "c417_ram")) {
            static char buf[0x10000 * 4 + 8];
            if (fscanf(f, "%262150s", buf) == 1)
                for (unsigned a = 0; a < 0x10000; a++) { char h[5] = { buf[4*a], buf[4*a+1], buf[4*a+2], buf[4*a+3], 0 }; c417.ram[a] = (uint16_t)strtoul(h, NULL, 16); }
        }
        n++;
    }
    fclose(f);
    fprintf(stderr, "[TC2] chip state from %s (%d entries)\n", path, n);
    return 1;
}

uint16_t *tc2_c412_pcz(void) { return c412.pczram; }
