/*
 * tw_hw.c -- the Super System 22 devices the 68EC020 talks to, per MAME namcos22.cpp.
 *
 *  syscon 0x700000 (ss22_syscon_w): 0x00-0x03 IRQ level+enable (VBLANK, scanline, SCI, ?),
 *    0x04-0x07 acknowledge, 0x14 watchdog, 0x16 sound MCU enable, 0x1C DSP control. On Super
 *    22 a line is enabled by a NON-ZERO level (System 22 tests bit 4).
 *  keycus 0x400000: Tokyo Wars' protection answer is 0x01A8 at word offset 4 and nothing
 *    else; every other read is a fresh random value. MAME draws it with machine().rand(), so a
 *    trace comparison replays the values MAME returned (TW_KEYCUS_FILE).
 *  DSW 0x440000: every switch off (0xFFFFFFFF: test mode off).
 *  portbit 0x450008: two serial bit ports, reset by a write; the MAME defaults (0xFFFF).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tw_mem.h"
#include "tw_hw.h"
#include "tw_dsp.h"
#include "tw_snd.h"

tw_hw_t g_hw;

static FILE *keycus_replay;

uint32_t tw_hw_keycus_r(uint32_t unit)
{
    if (unit == 4) return 0x01A8;                       /* NAMCOS22_TOKYO_WARS */
    unsigned v;
    if (keycus_replay && fscanf(keycus_replay, "%x", &v) == 1) return g_hw.keycus_rng = (uint16_t)v;
    uint16_t old = g_hw.keycus_rng;
    do { g_hw.lcg = g_hw.lcg * 1103515245u + 12345u; g_hw.keycus_rng = (uint16_t)(g_hw.lcg >> 16); }
    while (g_hw.keycus_rng == old);                     /* never the same twice in a row */
    return g_hw.keycus_rng;
}

uint32_t tw_hw_dsw(void) { return g_hw.dsw; }

uint32_t tw_hw_portbit_r(uint32_t unit)
{
    unit &= 1;
    uint32_t ret = g_hw.portbits[unit] & 1;
    g_hw.portbits[unit] = (uint16_t)(g_hw.portbits[unit] >> 1 | 0x8000);
    return ret;
}
void tw_hw_portbit_w(uint32_t unit) { g_hw.portbits[unit & 1] = 0xFFFF; }

static void irq_level(uint32_t line, uint8_t data)
{
    uint32_t bit = 1u << line;
    uint8_t oldlevel = g_tw.syscon[line] & 7, newlevel = data & 7;
    g_hw.irq_enabled &= ~bit;
    if (newlevel) g_hw.irq_enabled |= bit;              /* Super 22: enabled by a non-zero level */
    (void)oldlevel;
    if ((g_hw.irq_state & bit) && !(g_hw.irq_enabled & bit)) g_hw.irq_state &= ~bit;
}

void tw_hw_syscon_w(uint32_t off, uint8_t data)
{
    if (off <= 3) irq_level(off, data);
    else if (off <= 7) g_hw.irq_state &= ~(1u << (off - 4));
    else if (off == 0x16) { g_hw.mcu_run = data != 0; tw_snd_set_run(g_hw.mcu_run); }
    else if (off == 0x1C) { g_hw.dsp_ctrl = data; tw_dsp_control(data); }
    g_tw.syscon[off] = data;
}

int g_tw_mbox;
static FILE *mbox_f;
void tw_mbox_log(uint32_t off, int size, uint32_t v)
{
    extern uint32_t rr_frame;
    fprintf(mbox_f, "%u %x %d %x\n", rr_frame, off, size, v);
}

bool tw_hw_init(const char *rom_dir)
{
    { const char *m = getenv("TW_MBOXLOG"); if (m && (mbox_f = fopen(m, "w"))) g_tw_mbox = 1; }
    memset(&g_hw, 0, sizeof g_hw);
    g_hw.lcg = 0x2545F491u;
    g_hw.dsw = 0xFFFFFFFFu;
    g_hw.portbits[0] = g_hw.portbits[1] = 0xFFFF;
    tw_load_eeprom(rom_dir);
    const char *kf = getenv("TW_KEYCUS_FILE");          /* replay MAME's random keycus reads */
    if (kf && !(keycus_replay = fopen(kf, "r"))) fprintf(stderr, "[TW] cannot open %s\n", kf);
    return true;
}

/* the frame's vblank: raise the vblank IRQ line (syscon line 0) if the game enabled it */
void tw_hw_vblank(void)
{
    if (g_hw.irq_enabled & 1u) g_hw.irq_state |= 1u;
}

/* the scanline IRQ (syscon line 1, posirq) */
void tw_hw_scanline(void)
{
    if (g_hw.irq_enabled & 2u) g_hw.irq_state |= 2u;
}

/* the highest pending, enabled IRQ as a 68K level (0 = none) */
int tw_hw_irq_level(void)
{
    int best = 0;
    for (int line = 0; line < 4; line++)
        if (g_hw.irq_state & (1u << line)) {
            int lvl = g_tw.syscon[line] & 7;
            if (lvl > best) best = lvl;
        }
    return best;
}
