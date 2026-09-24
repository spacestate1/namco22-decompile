/*
 * snd_board.c -- the System 22 SOUND BOARD as the C74 sees it (namcos22.cpp
 * mcu_s22_program), shared by the game and the test oracle:
 *   0x000000-0x00007F  on-chip peripherals (m377_periph.c)
 *   0x000080-0x00027F  on-chip RAM (M37702: 512 bytes; 0x3FF allowed)
 *   0x002000-0x002FFF  C352                     -> rr_audio (the engine's mixer)
 *   0x004000-0x00BFFF  shared RAM with the 68K  -> g_rr.shared, byte lanes ^1
 *   0x00C000-0x00FFFF  c74.bin, the chip's internal mask ROM
 *   0x200000-0x27FFFF  rv1data.6r, the sound program + data
 * Port 4 reads 0x10: "sound role" (mcu_port4_s22_r). No external interrupt pins
 * are driven: "C74 on the CPU board has no periodic interrupts, it runs entirely
 * off Timer A0". Clock 49.152 MHz / 3. The ROM images are ASSETS: the game never
 * decodes instructions from them -- snd_execute() is the translated program
 * (gen/snd_driver.c) in the game and the fetch/decode oracle in rr_sndoracle.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "m37710.h"
#include "rr_mem.h"
#include "rr_sound.h"
#include "snd_board.h"

m37710_t g_snd_cpu;
uint8_t g_snd_bios[0x4000], *g_snd_data;
static uint8_t iram[0x380], c352sh[0x1000];
static bool ready, running;
#define cpu g_snd_cpu

#define MCU_HZ 16384000ull
#define FPS_X1000 59906ull

static uint8_t bus_r(void *u, uint32_t a)
{
    (void)u;
    if (a >= 0x200000 && a < 0x280000) return g_snd_data[a - 0x200000];
    if (a >= 0x00C000 && a < 0x010000) return g_snd_bios[a - 0xC000];
    if (a >= 0x004000 && a < 0x00C000) return g_rr.shared[(a - 0x4000) ^ 1];
    if (a >= 0x002000 && a < 0x003000) {
        uint16_t w = rr_audio_c352_read((a - 0x2000) >> 1);
        return (a & 1) ? (uint8_t)(w >> 8) : (uint8_t)w;
    }
    if (a >= 0x80 && a < 0x400) return iram[a - 0x80];
    return 0;
}

static void bus_w(void *u, uint32_t a, uint8_t v)
{
    (void)u;
    if (a >= 0x004000 && a < 0x00C000) { g_rr.shared[(a - 0x4000) ^ 1] = v; return; }
    if (a >= 0x002000 && a < 0x003000) {
        unsigned off = a - 0x2000;
        c352sh[off] = v;
        if (off & 1) rr_audio_c352_write(off >> 1, (uint16_t)(c352sh[off - 1] | v << 8));   /* a word completes on the odd byte */
        return;
    }
    if (a >= 0x80 && a < 0x400) { iram[a - 0x80] = v; return; }
}

static bool slurp(const char *dir, const char *n, uint8_t *dst, size_t len)
{
    char p[1024]; snprintf(p, sizeof p, "%s/%s", dir, n);
    FILE *f = fopen(p, "rb");
    bool ok = f && fread(dst, 1, len, f) == len;
    if (f) fclose(f);
    if (!ok) fprintf(stderr, "[SND] cannot read %s\n", p);
    return ok;
}

bool rr_sound_init(const char *dir)
{
    g_snd_data = calloc(1, 0x80000);
    if (!slurp(dir, "c74.bin", g_snd_bios, sizeof g_snd_bios) || !slurp(dir, "rv1data.6r", g_snd_data, 0x80000)) return false;
    m37710_init(&cpu, bus_r, bus_w, NULL);
    snd_executor_init();
    ready = true;
    return true;
}

void rr_sound_set_run(bool run)
{
    if (!ready) return;
    if (getenv("RR_SNDLOG")) fprintf(stderr, "[SND] syscon 0x18 <- %d (was %d) after %llu cycles, pc=%02X%04X irqs=%llu\n",
                                     run, running, (unsigned long long)cpu.cycles, cpu.pg, cpu.pc, (unsigned long long)cpu.irq_taken);
    if (run == running) return;
    running = run;
    if (run) {                                 /* leaving reset restarts the chip */
        memset(iram, 0, sizeof iram);
        m37710_reset(&cpu);
        static int resets;
        if (++resets <= 3 || getenv("RR_SNDLOG")) fprintf(stderr, "[SND] sound CPU out of reset (#%d), PC=%04X\n", resets, cpu.pc);
    }
}

void snd_board_port4(void)                        /* input bits of P4 read 0x10 */
{
    uint8_t dir = cpu.sfr[0x0C];
    cpu.sfr[0x0A] = (uint8_t)((cpu.sfr[0x0A] & dir) | (0x10 & ~dir));
}

void rr_sound_slice(void)
{
    if (!ready || !running || cpu.unimpl_hit) return;
    static uint64_t owed;                         /* cycles, x 1000 * slices */
    owed += MCU_HZ * 1000ull;
    const uint64_t den = FPS_X1000 * RR_SND_SLICES;
    int cyc = (int)(owed / den);
    owed -= (uint64_t)cyc * den;
    snd_execute(&cpu, cpu.cycles + (uint64_t)cyc);
    if (cpu.unimpl_hit) fprintf(stderr, "[SND] cannot execute opcode 0x%03X at %06X -- sound halted\n", cpu.unimpl_op, cpu.unimpl_pc);
}
