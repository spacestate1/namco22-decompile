/*
 * rr_hw.c -- the System 22 devices the 68020 talks to, per MAME namcos22.cpp.
 *
 *  syscon 0x40000000: 0x00-0x04 IRQ level+enable (bit 4 enables on S22) for
 *    hblank/?/SCI/unknown/VBLANK, 0x05-0x09 acknowledges, 0x16 watchdog,
 *    0x18 MCU run (0 = held in reset), 0x1A DSP control (0 off, 1 run, 0xFF
 *    upload).
 *  keycus 0x20000000: no protection on Rave Racer -- MAME returns a fresh
 *    random value that never repeats twice running; here a seeded LCG.
 *  I/O board: MAME does not emulate the second C74 for driving games; each
 *    vblank (with the MCU running) handle_driving_io() writes the switches,
 *    wheel+32, gas+992, brake+3008 into shared RAM 0x30..0x36 and coins at
 *    0x3A. Done here the same way.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "rr_mem.h"
#include "rr_hw.h"
#include "rr_dsp.h"
#include "rr_sound.h"

rr_hw_t g_hw;

static FILE *keycus_replay;
static uint16_t keycus_next(void)
{
    unsigned v;
    if (keycus_replay && fscanf(keycus_replay, "%x", &v) == 1) return g_hw.keycus_rng = (uint16_t)v;
    uint16_t old = g_hw.keycus_rng;
    do { g_hw.lcg = g_hw.lcg * 1103515245u + 12345u; g_hw.keycus_rng = (uint16_t)(g_hw.lcg >> 16); }
    while (g_hw.keycus_rng == old);
    return g_hw.keycus_rng;
}

static void irqlevel(int line, uint8_t data)
{
    uint32_t bit = 1u << line;
    g_hw.irq_enabled &= ~bit;
    if (data & 0x10) g_hw.irq_enabled |= bit;
    if ((g_hw.irq_state & bit) && !(g_hw.irq_enabled & bit)) g_hw.irq_state &= ~bit;
}

static bool io_read(vaddr_t a, int size, uint32_t *out)
{
    if (a >= 0x20000000u && a < 0x20000010u) { *out = keycus_next(); return true; }
    if (a >= 0x50000000u && a < 0x50000004u) {
        uint32_t d = g_hw.dsw;
        *out = size == 4 ? d : size == 2 ? (d >> ((a & 2) ? 0 : 16)) & 0xFFFF : (d >> ((3 - (a & 3)) * 8)) & 0xFF;
        return true;
    }
    if (a >= 0x50000008u && a < 0x5000000Cu) {                  /* serial port bits */
        int i = (a >> 1) & 1;
        *out = g_hw.portbits[i] & 1;
        g_hw.portbits[i] = g_hw.portbits[i] >> 1 | 0x8000;
        return true;
    }
    if (a >= 0x60000000u && a < 0x60004000u) { *out = 0; return true; }   /* nopw region */
    if (a >= 0x48000000u && a < 0x48000040u) { *out = 0; return true; }   /* diag device */
    if (a >= 0x90040000u && a < 0x90080000u) { *out = 0; return true; }   /* diag ROM */
    return false;
}

static bool io_write(vaddr_t a, int size, uint32_t v)
{
    if (a >= 0x40000000u && a < 0x40000020u) {
        /* byte-wide registers; a word/long write covers several */
        for (int i = 0; i < size; i++) {
            uint32_t off = (a & 0x1F) + i;
            uint8_t d = (uint8_t)(v >> ((size - 1 - i) * 8));
            if (off <= 4) irqlevel(off, d);
            else if (off <= 9) g_hw.irq_state &= ~(1u << (off - 5));
            else if (off == 0x18) { g_hw.mcu_run = d != 0; rr_sound_set_run(g_hw.mcu_run); }
            else if (off == 0x1A) { g_hw.dsp_ctrl = d; rr_dsp_control(d); }
            g_rr.syscon[off] = d;
        }
        return true;
    }
    if (a >= 0x20000000u && a < 0x20000010u) return true;        /* keycus_w: ignored */
    if (a >= 0x50000000u && a < 0x50000004u) { g_hw.cpuleds = v; return true; }
    if (a >= 0x50000008u && a < 0x5000000Cu) { g_hw.portbits[(a >> 1) & 1] = 0xFFFF; return true; }
    if (a >= 0x60000000u && a < 0x60004000u) return true;
    if (a >= 0x90000000u && a < 0x90000004u) return true;         /* PCB LED */
    if (a >= 0x48000000u && a < 0x48000040u) return true;
    return false;
}

bool rr_hw_init(const char *rom_dir)
{
    memset(&g_hw, 0, sizeof g_hw);
    g_hw.lcg = 0x2545F491u;
    g_hw.dsw = 0xFFFFFFFFu;             /* every DIP off */
    g_hw.portbits[0] = g_hw.portbits[1] = 0xFFFF;
    g_hw.steer = 0x800; g_hw.gas = 0; g_hw.brake = 0; g_hw.inputs = 0xFEFF;   /* MAME default: active-low bits 1, cabinet field 0x2100 = 0x2000 (Standard) */
    char p[1024];
    snprintf(p, sizeof p, "%s/rv1eeprm.9e", rom_dir);
    FILE *f = fopen(p, "rb");
    if (f) { if (fread(g_rr.eeprom, 1, RR_EEPROM_SIZE, f) != RR_EEPROM_SIZE) fprintf(stderr, "[HW] short eeprom\n"); fclose(f); }
    else fprintf(stderr, "[HW] no %s -- EEPROM blank\n", p);
    rr_set_io_hooks(io_read, io_write);
    if (!rr_dsp_init(rom_dir)) fprintf(stderr, "[HW] master DSP unavailable\n");
    const char *kf = getenv("RR_KEYCUS_FILE");      /* replay MAME's random keycus reads */
    if (kf && !(keycus_replay = fopen(kf, "r"))) fprintf(stderr, "[HW] cannot open %s\n", kf);
    return true;
}

static void shared_w16(uint32_t off, uint16_t v)
{
    g_rr.shared[off] = (uint8_t)(v >> 8);
    g_rr.shared[off + 1] = (uint8_t)v;
}

void rr_hw_vblank(void)
{
    if (g_hw.mcu_run) {                              /* handle_driving_io */
        shared_w16(0x30, g_hw.inputs);
        shared_w16(0x32, (uint16_t)(g_hw.steer + 32));
        shared_w16(0x34, (uint16_t)(g_hw.gas + 992));
        shared_w16(0x36, (uint16_t)(g_hw.brake + 3008));
        int coin = (g_hw.inputs & 0x1000) >> 12 | (g_hw.inputs & 0x0200) >> 8;
        if (!(coin & 1) && (g_hw.old_coin & 1)) g_hw.credits1++;
        if (!(coin & 2) && (g_hw.old_coin & 2)) g_hw.credits2++;
        g_hw.old_coin = coin;
        shared_w16(0x3A, (uint16_t)(g_hw.credits1 << 8 | g_hw.credits2));
    }
    if (g_hw.irq_enabled & (1u << 4)) g_hw.irq_state |= 1u << 4;
}

/* the highest-priority pending, enabled IRQ as a 68K level (0 = none) */
int rr_hw_irq_level(void)
{
    int best = 0;
    for (int line = 0; line < 5; line++)
        if (g_hw.irq_state & (1u << line)) {
            int lvl = g_rr.syscon[line] & 7;
            if (lvl > best) best = lvl;
        }
    return best;
}

/* FREE PLAY is the game's own coin option (test mode -> COIN OPTIONS -> FREE PLAY):
 * settings group 2 is loaded by ROM 0x02AE7E from EEPROM 0x200 + 2*0x40 (+0x20 for
 * the backup copy) into WRAM 0x10001040, 30 data bytes + a BE16 ~(byte sum). Byte 3
 * is the flag the attract screen and the credit logic test (0x00C32E `tst.b
 * $10001043`). Set it in the EEPROM image with a valid checksum, and in WRAM so a
 * running game sees it at once -- what the test menu itself does. */
static void eeprom_block_set(uint32_t blk, int byte, uint8_t v)
{
    uint8_t *b = g_rr.eeprom + blk;
    unsigned sum = 0;
    for (int i = 0; i < 30; i++) sum += b[i];
    uint16_t stored = (uint16_t)(b[30] << 8 | b[31]);
    if ((uint16_t)~sum != stored) return;              /* not a valid copy: leave it for the game to repair */
    b[byte] = v;
    sum = 0;
    for (int i = 0; i < 30; i++) sum += b[i];
    b[30] = (uint8_t)((uint16_t)~sum >> 8); b[31] = (uint8_t)~sum;
}
void rr_hw_set_freeplay(bool on)
{
    eeprom_block_set(0x280, 3, on ? 1 : 0);
    eeprom_block_set(0x2A0, 3, on ? 1 : 0);
    g_rr.wram[0x1043] = on ? 1 : 0;
}
bool rr_hw_freeplay(void) { return g_rr.wram[0x1043] != 0; }
