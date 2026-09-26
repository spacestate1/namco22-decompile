/* See include/mcu_sound.h for what this is and why it exists. */
#include "mcu_sound.h"
#include "m37710.h"
#include "propcycl.h"
#include "c352.h"

/* How the sound program's instructions get executed: the TRANSLATED program
 * (gen/snd_driver.c) in the game; the fetch/decode oracle in
 * propcycl_sndoracle (tools/sndoracle/, test only). Same contract as
 * m37710_run: run until `cycles` have elapsed. */
int  snd_run(m37710_t *c, int cycles);
void snd_executor_init(void);

/* The game state, for the gameplay-only shared-RAM dump below. */
extern intptr_t _W[];
#define W _W

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* audio_hle owns the chip instance and the mixer lock; it hands us these two
 * so the MCU's writes land in the very chip that is being played. */
extern void audio_hle_c352_write(unsigned word_off, unsigned val);
extern unsigned audio_hle_c352_read(unsigned word_off);
extern void audio_hle_c352_sync(uint64_t mcu_cycles);

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
/* The next cycle at which the board does something to the sound CPU: a pin, or
 * the end of the frame (the 68K runs, the chip trace is sampled). */
static uint64_t  g_pin_next = UINT64_MAX;
uint64_t mcu_sound_next_pin_event(void) { return g_pin_next; }
/* Assert whichever board pins are due. Called from the frame loop and from
 * inside snd_call() (a translated routine run from readable C), so a long
 * readable routine does not hold the pins back. */
void mcu_sound_raise_due_pins(void)
{
    if (g_cpu.cycles >= g_int0_last + CYC_FRAME) {
        g_int0_last += CYC_FRAME; m37710_irq(&g_cpu, 0x14);
    }
    if (g_cpu.cycles >= g_int2_last + CYC_FRAME) {
        g_int2_last += CYC_FRAME; m37710_irq(&g_cpu, 0x10);
    }
}
static FILE     *g_c352log;
/* PROPCYCL_CHIPTRACE=<file>: the rewrite's gate against the oracle. Per game
 * frame, every key-on (voice + the registers it was keyed with) and the whole
 * register image as the DRIVER wrote it. Timing inside a frame is not in it:
 * a readable routine runs atomically, so interrupts land at different cycles,
 * but what the driver tells the chip each frame must not change. */
static FILE     *g_chiptrace;

static uint8_t bus_r(void *u, uint32_t a)
{
    (void)u;
    if (a >= 0x200000 && a < 0x280000) return g_rom[a - 0x200000];
    if (a >= 0x00C000 && a < 0x010000) return g_rom[a];              /* mirror */
    if (a >= 0x004000 && a < 0x00C000) return g_sys.commsram[(a - 0x004000) ^ 1];
    if (a >= 0x002000 && a < 0x003000) {
        /* The chip's live register (see audio_hle_c352_sync), little-endian. */
        unsigned off = a - 0x002000;
        audio_hle_c352_sync(g_cpu.cycles);
        unsigned w = audio_hle_c352_read(off >> 1);
        return (uint8_t)((off & 1) ? w >> 8 : w);
    }
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
        /* PROPCYCL_C352LOG=<file>: every C352 register byte write in
         * tools/overnight/dump_c352.lua's format (machine time in seconds,
         * byte offset, data, mask), so the driver's output can be compared
         * against MAME's write for write (tools/overnight/c352_stream_gate.py). */
        if (g_c352log)
            fprintf(g_c352log, "%.9f %x %x %x\n", (double)g_cpu.cycles / MCU_CLOCK, off & ~1u,
                    (off & 1) ? (unsigned)v << 8 : v, (off & 1) ? 0xff00u : 0x00ffu);
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
            if (g_chiptrace && w == 0x202) {
                for (unsigned vo = 0; vo < 32; vo++) {
                    const uint8_t *r = &g_c352sh[vo * 16];
                    if (r[7] & 0x40)
                        fprintf(g_chiptrace, "K %u v%u %02X%02X %02X%02X %02X%02X %02X%02X %02X%02X %02X%02X %02X%02X %02X%02X\n",
                                (unsigned)g_sys.frame_count, vo, r[1], r[0], r[3], r[2], r[5], r[4], r[7], r[6],
                                r[9], r[8], r[11], r[10], r[13], r[12], r[15], r[14]);
                }
            }
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
            audio_hle_c352_sync(g_cpu.cycles);
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
    snd_executor_init();
    g_int0_last = 0;
    g_int2_last = CYC_FRAME / 2;            /* INT2 leads INT0 by half a frame */
    /* PROPCYCL_SNDJITTER=<cycles>: shift the board's pin schedule. A TEST knob:
     * it asks whether the driver's OUTPUT (the order of its chip writes)
     * depends on exactly when interrupts land, or only its timing does. */
    { const char *e = getenv("PROPCYCL_SNDJITTER");
      if (e) { long k = atol(e); g_int0_last += (uint64_t)k; g_int2_last += (uint64_t)k; } }
    /* The board's video timer has been running long before the 68K releases
     * the MCU from reset, and both pins are held, so by the time the driver
     * first clears its interrupt mask each has already been asserted. MAME's
     * trace shows exactly that -- INT2 taken at the very instruction that
     * opens the mask, INT0 as soon as its handler returns. */
    m37710_irq(&g_cpu, 0x10);
    m37710_irq(&g_cpu, 0x14);
    { extern int g_m377_sfrlog; if (getenv("PROPCYCL_MCUSFR")) g_m377_sfrlog = 1; }
    if (getenv("PROPCYCL_MCUTRAP")) g_cpu.pchist_on = 1;
    { const char *e = getenv("PROPCYCL_C352LOG");
      if (e && (g_c352log = fopen(e, "w"))) { setvbuf(g_c352log, NULL, _IOFBF, 1 << 20); fprintf(g_c352log, "# ours: time = MCU cycles / %u\n", MCU_CLOCK); } }
    { const char *e = getenv("PROPCYCL_CHIPTRACE");
      if (e && (g_chiptrace = fopen(e, "w"))) setvbuf(g_chiptrace, NULL, _IOFBF, 1 << 20); }
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
    static int SW_SLOT[5] = { 0, 1, 2, 3, 27 };
    static int slotwatch = -1;
    static unsigned sw_last[5];
    if (slotwatch < 0) {
        const char *e = getenv("PROPCYCL_SLOTWATCH");
        slotwatch = (e && *e && *e != '0');
        if (slotwatch && atoi(e) > 3) SW_SLOT[4] = atoi(e);   /* =<slot>: watch that slot instead of 27 */
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

    /* PROPCYCL_MBOXLOG=<file>: every command word (slots 0-31) the GAME changed
     * since the driver last ran, tagged with the game's frame counter
     * W[0x0C98] -- the same line tools/overnight/mbox_trace.lua writes from
     * MAME, so the 68K->driver stream can be diffed apart from the driver. */
    { static FILE *mb; static int init;
      if (!init) { const char *e = getenv("PROPCYCL_MBOXLOG"); init = 1; if (e) mb = fopen(e, "w"); }
      if (mb) {
          for (int o = 0; o < 0x40; o += 2) {
              unsigned v = comms_r16(g_sys.commsram, o), was = comms_r16(g_shadow, o);
              if (v != was) fprintf(mb, "MBOX off=%x val=%04x fc=%ld st=%ld ph=%ld\n", o, v,
                                    (long)(int32_t)W[0x0C98], (long)W[0x0CBC], (long)W[0x0CC4]);
          }
          fflush(mb);
      } }

    /* Frame ends lie on a FIXED grid. Taking `now + CYC_FRAME` instead let every
     * extension below push all later frames back, so a build that extends
     * (the oracle) and one that never needs to (a tick written as C) drifted a
     * whole tick apart by frame 2670 of a coin run and saw the game's commands
     * on different ticks. An extension now borrows from the next frame only. */
    static uint64_t frame_grid;
    if (!frame_grid) frame_grid = g_cpu.cycles;
    frame_grid += CYC_FRAME;
    end = frame_grid;
    /* A FRAME DOES NOT END INSIDE A TICK. The 68K and the sound CPU run
     * concurrently on the board; here they take turns a frame at a time, and
     * the game writes its mailbox commands between turns. Ending a turn in the
     * middle of the 120 Hz tick body (the driver sets 0xD3 while it runs,
     * 0xD684..0xD6DE) let the rest of that tick see the NEXT frame's commands
     * -- 21% of frame ends in a coin run -- a pure artifact of the turn-taking,
     * and one a tick written as plain C cannot reproduce. So the turn runs on
     * to the end of the tick (bounded, in case the flag is ever left set). */
    const uint64_t hard_end = end + CYC_FRAME / 4;
    while ((g_cpu.cycles < end || (g_iram[0xD3 - 0x80] && g_cpu.cycles < hard_end)) && !g_cpu.unimpl_hit) {
        /* Only the two interrupt PINS the board drives are ours to supply:
         * namcos22.cpp's `mcu_irq` scanline timer asserts IRQ0 at scanline
         * 480 (vector 0xFFF4) and IRQ2 at 240 (0xFFF0), once each per frame.
         * Everything else -- Timer A0's 120Hz tick, Timer B0, the A-D
         * converter that reads the handlebar -- the CPU raises from its own
         * peripheral registers, because those rates are not constants: the
         * driver starts and stops Timer B0 at run time by rewriting the
         * count-start register (measured: 0x40 alternating 0x0D and 0x2D). */
        /* The pins are asserted on an exact grid (MAME: a scanline timer),
         * not "at the first 64-cycle chunk boundary after they are due" --
         * that made every later pin depend on how far the last instruction
         * overran a chunk, so the pin times were a function of the executor. */
        mcu_sound_raise_due_pins();
        g_pin_next = g_cpu.cycles < end ? end : hard_end;
        if (g_int0_last + CYC_FRAME < g_pin_next) g_pin_next = g_int0_last + CYC_FRAME;
        if (g_int2_last + CYC_FRAME < g_pin_next) g_pin_next = g_int2_last + CYC_FRAME;
        { extern uint64_t snd_spin_fast(m37710_t *, uint64_t); snd_spin_fast(&g_cpu, g_pin_next); }   /* a wait loop, jumped exactly */
        uint64_t n = g_pin_next - g_cpu.cycles;
        snd_run(&g_cpu, n < 64 ? (int)n : 64);
    }
    /* PROPCYCL_SNDSTATE=<file>[:<frame>]: per frame, a hash of the driver's
     * memory (its RAM 0x80-0x3FF and the shared RAM it keeps its records in);
     * at <frame>, the bytes themselves. Two builds' files name the first frame
     * and the first byte where the driver's STATE parts -- which the chip
     * stream only shows once it reaches a register. */
    { static FILE *sf; static long dump_at = -2;
      if (dump_at == -2) { const char *e = getenv("PROPCYCL_SNDSTATE"); dump_at = -1;
          if (e) { char path[512]; snprintf(path, sizeof path, "%s", e); char *k = strrchr(path, ':');
                   if (k) { *k = 0; dump_at = atol(k + 1); } sf = fopen(path, "w"); } }
      if (sf) {
          uint64_t h = 1469598103934665603ull;
          for (unsigned i = 0; i < sizeof g_iram; i++) {
              if (i + 0x80 >= 0x240 && i + 0x80 < 0x280) continue;   /* the stack: return addresses, saved registers */
              h ^= g_iram[i]; h *= 1099511628211ull; }
          for (unsigned i = 0; i < 0x8000; i++) { h ^= g_sys.commsram[i]; h *= 1099511628211ull; }
          fprintf(sf, "H %u %016llx\n", (unsigned)g_sys.frame_count, (unsigned long long)h);
          if ((long)g_sys.frame_count == dump_at) {
              for (unsigned i = 0; i < sizeof g_iram; i++) fprintf(sf, "B %04X %02X\n", 0x80 + i, g_iram[i]);
              for (unsigned i = 0; i < 0x8000; i++) fprintf(sf, "B %04X %02X\n", 0x4000 + i, g_sys.commsram[i ^ 1]);
          }
          fflush(sf);
      } }
    audio_hle_c352_sync(g_cpu.cycles);      /* the chip's output up to the end of this frame */
    if (g_chiptrace) {
        fprintf(g_chiptrace, "F %u", (unsigned)g_sys.frame_count);
        for (unsigned i = 0; i < 0x200; i += 2)
            fprintf(g_chiptrace, " %02X%02X", g_c352sh[i + 1], g_c352sh[i]);
        fprintf(g_chiptrace, " | %02X%02X\n", g_c352sh[0x401], g_c352sh[0x400]);
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
