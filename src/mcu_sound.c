/* See include/mcu_sound.h for what this is and why it exists. */
#include "mcu_sound.h"
#include "m37710.h"
#include "propcycl.h"
#include "c352.h"

/* The game state, for the gameplay-only shared-RAM dump below. */
extern intptr_t _W[];
#define W _W

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* audio_hle owns the chip instance and the mixer lock; it hands us these two
 * so the MCU's writes land in the very chip that is being played. */
extern void audio_hle_c352_write(unsigned word_off, unsigned val);

const char *g_sharedump = 0;
int g_mculog = 0;
int g_mcu_voice = -1;   /* PROPCYCL_MCUVOICE */
int g_mcu_pc    = -1;   /* PROPCYCL_MCUPC: print registers at one PC */             /* PROPCYCL_MCULOG: per-frame driver activity */
static long g_voice_writes, g_ctrl_writes;

#define MCU_CLOCK   16384000u
#define CYC_FRAME   (MCU_CLOCK / 60)          /* 273066 */

static m37710_t  g_cpu;
static uint8_t  *g_rom;                       /* 0x80000, ROM_REGION16_LE */
static uint8_t   g_iram[0x380];               /* 0x000080-0x0003FF */
static uint8_t   g_c352sh[0x1000];            /* shadow, to assemble 16-bit writes */
static bool      g_ready;

static uint64_t  g_int0_last, g_int2_last;

static uint8_t bus_r(void *u, uint32_t a)
{
    (void)u;
    if (a >= 0x200000 && a < 0x280000) return g_rom[a - 0x200000];
    if (a >= 0x00C000 && a < 0x010000) return g_rom[a];              /* mirror */
    if (a >= 0x004000 && a < 0x00C000) return g_sys.commsram[(a - 0x004000) ^ 1];
    if (a >= 0x002000 && a < 0x003000) return g_c352sh[a - 0x002000];
    if (a <  0x000400)                 return g_iram[a - 0x000080];
    return 0;
}

static void bus_w(void *u, uint32_t a, uint8_t v)
{
    (void)u;
    if (a >= 0x004000 && a < 0x00C000) { g_sys.commsram[(a - 0x004000) ^ 1] = v; return; }
    if (a >= 0x002000 && a < 0x003000) {
        unsigned off = a - 0x002000;
        g_c352sh[off] = v;
        /* The C352 is a 16-bit device. The core writes a word as low byte
         * then high byte, so the word is complete on the ODD address -- and
         * the chip's key-on execute register only acts on a full 16-bit
         * write, so it has to be issued as one. */
        if (off & 1) {
            unsigned w = off >> 1;
            if (w < 0x100) g_voice_writes++; else g_ctrl_writes++;
            /* PROPCYCL_MCUVOICE=<n>: every register write to one voice, with
             * the MCU PC that made it. A voice keyed with wave_bank 0 and
             * wave_start 0 is playing the first bytes of the wave ROM on a
             * short loop, which is a pure tone -- so the question is always
             * "which routine programmed it", never "what does it sound
             * like". */
            /* PROPCYCL_SNDLOG also reports every KEY-ON the driver issues,
             * with the sample the voice is pointed at. Two triggers that key
             * the SAME wave_bank/wave_start are literally the same sample --
             * which is what "I hear the selection sound twice" looks like
             * from here, and it is not answerable by ear. */
            if ((w & 7) == 3 && (((unsigned)g_c352sh[off - 1] | ((unsigned)v << 8)) & 0x4000)) {
                extern int g_sndlog;
                unsigned vo = w >> 3;
                if (g_sndlog)
                    fprintf(stderr, "[KEYON ] f%-6u voice %-2u  bank=%02X start=%04X end=%04X  vol=%04X freq=%04X\n",
                            (unsigned)g_sys.frame_count, vo,
                            g_c352sh[vo*16+8] | (g_c352sh[vo*16+9] << 8),
                            g_c352sh[vo*16+10] | (g_c352sh[vo*16+11] << 8),
                            g_c352sh[vo*16+12] | (g_c352sh[vo*16+13] << 8),
                            g_c352sh[vo*16+0] | (g_c352sh[vo*16+1] << 8),
                            g_c352sh[vo*16+4] | (g_c352sh[vo*16+5] << 8));
            }
            { extern int g_mcu_voice;
              if (g_mcu_voice >= 0 && w < 0x100 && (int)(w >> 3) == g_mcu_voice) {
                  static const char *RN[8] = {"vol_f","vol_r","freq","flags",
                                              "bank","start","end","loop"};
                  fprintf(stderr, "[VOICE%02d] %-6s = %04X   pc=%04X\n",
                          g_mcu_voice, RN[w & 7],
                          (unsigned)g_c352sh[off - 1] | ((unsigned)v << 8), g_cpu.pc);
              } }
            audio_hle_c352_write(w, (unsigned)g_c352sh[off - 1] | ((unsigned)v << 8));
        }
        return;
    }
    if (a <  0x000400)                 { g_iram[a - 0x000080] = v; return; }
    /* 0x301000 watchdog, 0x308000 MB87078 volume -- both write-only, ignored. */
}

bool mcu_sound_init(const char *rom_dir)
{
    char path[1024];
    FILE *f;
    size_t got;

    g_ready = false;
    if (!rom_dir) return false;
    snprintf(path, sizeof path, "%s/pr1data.8k", rom_dir);
    f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "[MCU] no %s -- the sound driver cannot run, "
                        "the game will be silent\n", path);
        return false;
    }
    if (!g_rom) g_rom = (uint8_t *)calloc(1, 0x80000u);
    if (!g_rom) { fclose(f); return false; }
    got = fread(g_rom, 1, 0x80000u, f);
    fclose(f);
    if (got != 0x80000u) {
        fprintf(stderr, "[MCU] %s is %zu bytes, expected 524288\n", path, got);
        return false;
    }

    memset(g_iram, 0, sizeof g_iram);
    memset(g_c352sh, 0, sizeof g_c352sh);
    m37710_init(&g_cpu, bus_r, bus_w, NULL);
    m37710_reset(&g_cpu);
    g_int0_last = 0;
    g_int2_last = CYC_FRAME / 2;            /* INT2 leads INT0 by half a frame */
    /* The board's video timer has been running long before the 68K releases
     * the MCU from reset, and both pins are held, so by the time the driver
     * first clears its interrupt mask each has already been asserted. MAME's
     * trace shows exactly that -- INT2 taken at the very instruction that
     * opens the mask, INT0 as soon as its handler returns. */
    m37710_irq(&g_cpu, 0x10);
    m37710_irq(&g_cpu, 0x14);
    { extern int g_m377_sfrlog; if (getenv("PROPCYCL_MCUSFR")) g_m377_sfrlog = 1; }
    if (getenv("PROPCYCL_MCUTRAP")) g_cpu.pchist_on = 1;
    { const char *e = getenv("PROPCYCL_MCUVOICE"); if (e) g_mcu_voice = atoi(e); }
    { const char *e = getenv("PROPCYCL_MCUPC"); if (e) g_mcu_pc = (int)strtol(e, 0, 16); }
    g_ready = true;
    printf("[MCU] sound driver loaded (pr1data.8k), reset to PC=%04X\n", g_cpu.pc);
    return true;
}

bool mcu_sound_ready(void) { return g_ready; }

/* PROPCYCL_SHAREDUMP=<prefix>:<f>,<f>,... -- the shared RAM in the MCU's own
 * byte order, for diffing against tools/overnight/dump_shareram.lua's capture
 * of the real machine. Ours is held in 68K order, hence the ^1. */
void mcu_sound_sharedump(const char *prefix, unsigned frame)
{
    char path[1024]; FILE *f; unsigned i;
    snprintf(path, sizeof path, "%s_%s.bin", prefix, frame ? "play300" : "play");
    f = fopen(path, "wb");
    if (!f) return;
    for (i = 0; i < COMMSRAM_SIZE; i++) fputc(g_sys.commsram[i ^ 1], f);
    fclose(f);

    /* And the C352 register file as the driver last left it -- 32 voices x
     * 8 registers x 16 bits, straight out of the shadow, in the same order
     * tools/overnight/dump_shareram.lua reads them from the real chip. A
     * voice whose wave_start and wave_end are a few samples apart is a
     * two-sample loop, which is what a pure tone sounds like. */
    { extern void audio_hle_c352_dump(const char *);
      snprintf(path, sizeof path, "%s_%s_c352.txt", prefix, frame ? "play300" : "play");
      audio_hle_c352_dump(path); }
    fprintf(stderr, "[MCU] wrote %s\n", path);
}

static uint8_t g_shadow[COMMSRAM_SIZE];   /* what the MCU last left behind */

void mcu_sound_run_frame(void)
{
    uint64_t end;
    if (!g_ready) return;

    /* Which bytes did the 68K change since we last ran? The driver keeps its
     * whole working state in this same RAM, so a stray write from the game
     * side does not just lose a command, it can stop the driver dead. */
    if (g_mculog) {
        unsigned i, n = 0;
        for (i = 0; i < COMMSRAM_SIZE; i++)
            if (g_sys.commsram[i] != g_shadow[i]) {
                if (n < 12)
                    fprintf(stderr, "[MCU]   68K wrote commsram[%04X] %02X -> %02X\n",
                            i, g_shadow[i], g_sys.commsram[i]);
                n++;
            }
        if (n) fprintf(stderr, "[MCU]   (%u bytes changed by the 68K this frame)\n", n);
    }

    /* PROPCYCL_SLOTWATCH=1: the four music command words (slots 0-3) and the
     * hit-reaction sound's slot 27, every time EITHER side changes one --
     * "68K" is a change the game made since the MCU last ran, "MCU" one the
     * driver made itself this frame. A song that stops at its loop point is
     * either the game clearing bit 15 or the driver deciding the song is
     * over, and this says which. */
    static const int SW_SLOT[5] = { 0, 1, 2, 3, 27 };
    static int slotwatch = -1;
    static unsigned sw_last[5];
    if (slotwatch < 0) {
        const char *e = getenv("PROPCYCL_SLOTWATCH");
        slotwatch = (e && *e && *e != '0');
        for (int s = 0; s < 5; s++) sw_last[s] = comms_r16(g_sys.commsram, SW_SLOT[s] * 2);
    }
    if (slotwatch)
        for (int s = 0; s < 5; s++) {
            unsigned v = comms_r16(g_sys.commsram, SW_SLOT[s] * 2);
            if (v != sw_last[s])
                fprintf(stderr, "[SLOT] f%-6u st=%ld/%ld  68K slot %d  %04X -> %04X\n",
                        (unsigned)g_sys.frame_count, (long)W[0x0CBC], (long)W[0x0CC0],
                        SW_SLOT[s], sw_last[s], v);
            sw_last[s] = v;
        }

    end = g_cpu.cycles + CYC_FRAME;
    while (g_cpu.cycles < end && !g_cpu.unimpl_hit) {
        /* Only the two interrupt PINS the board drives are ours to supply:
         * namcos22.cpp's `mcu_irq` scanline timer asserts IRQ0 at scanline
         * 480 (vector 0xFFF4) and IRQ2 at 240 (0xFFF0), once each per frame.
         * Everything else -- Timer A0's 120Hz tick, Timer B0, the A-D
         * converter that reads the handlebar -- the CPU raises from its own
         * peripheral registers, because those rates are not constants: the
         * driver starts and stops Timer B0 at run time by rewriting the
         * count-start register (measured: 0x40 alternating 0x0D and 0x2D). */
        if (g_cpu.cycles - g_int0_last >= CYC_FRAME) {
            g_int0_last = g_cpu.cycles; m37710_irq(&g_cpu, 0x14);
        }
        if (g_cpu.cycles - g_int2_last >= CYC_FRAME) {
            g_int2_last = g_cpu.cycles; m37710_irq(&g_cpu, 0x10);
        }
        m37710_run(&g_cpu, 64);
    }
    memcpy(g_shadow, g_sys.commsram, sizeof g_shadow);
    if (slotwatch)
        for (int s = 0; s < 5; s++) {
            unsigned v = comms_r16(g_sys.commsram, SW_SLOT[s] * 2);
            if (v != sw_last[s])
                fprintf(stderr, "[SLOT] f%-6u st=%ld/%ld  MCU slot %d  %04X -> %04X\n",
                        (unsigned)g_sys.frame_count, (long)W[0x0CBC], (long)W[0x0CC0],
                        SW_SLOT[s], sw_last[s], v);
            sw_last[s] = v;
        }

    /* Dump the shared RAM once the game is actually IN gameplay, and again
     * 300 frames later -- the same two points tools/overnight/dump_shareram.lua
     * takes on the real machine. Attract is not a useful comparison: the
     * machine is nearly silent there, so matching it there proves nothing. */
    if (g_sharedump && W[0x0CBC] == 3 && W[0x0CC0] == 3) {
        static unsigned play_at;
        unsigned fc = (unsigned)g_sys.frame_count;
        if (!play_at) { play_at = fc; mcu_sound_sharedump(g_sharedump, 0); }
        else if (fc == play_at + 300) mcu_sound_sharedump(g_sharedump, 300);
    }
    if (g_mculog) {
        static uint8_t last_cs = 0xFF;
        if (g_cpu.sfr[0x40] != last_cs) {
            fprintf(stderr, "[MCU] f%u  count-start %02X -> %02X  (pc=%04X)\n",
                    (unsigned)g_sys.frame_count, last_cs, g_cpu.sfr[0x40], g_cpu.pc);
            last_cs = g_cpu.sfr[0x40];
        }
        static long last_ctrl, last_voice;
        unsigned fc = (unsigned)g_sys.frame_count;
        static uint64_t last_irq;
        if ((fc % 60) == 0)
            fprintf(stderr, "[MCU] f%-5u st=%ld/%ld  +%ld ctrl +%ld voice  +%llu irq  "
                            "cnt_start=%02X  $82=%02X $83=%02X  B0mode=%02X B0reload=%04X\n",
                    fc, (long)W[0x0CBC], (long)W[0x0CC0],
                    g_ctrl_writes - last_ctrl, g_voice_writes - last_voice,
                    (unsigned long long)(g_cpu.irq_taken - last_irq),
                    g_cpu.sfr[0x40], g_iram[2], g_iram[3],
                    g_cpu.sfr[0x5B], g_cpu.sfr[0x50] | (g_cpu.sfr[0x51] << 8));
        last_irq = g_cpu.irq_taken;
        last_ctrl = g_ctrl_writes; last_voice = g_voice_writes;
    }
    if (g_cpu.unimpl_hit) {
        fprintf(stderr, "[MCU] unimplemented opcode 0x%03X at %06X -- driver halted\n",
                g_cpu.unimpl_op, g_cpu.unimpl_pc);
        g_ready = false;
    }
}
