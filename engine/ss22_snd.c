/*
 * ss22_snd.c -- the Super System 22 SOUND MCU BOARD as the M37710 sees it (namcos22.cpp mcu_program):
 *   0x000000-0x00007F  on-chip peripherals (engine/snd/m377_periph.c: timers, A-D, interrupts)
 *   0x000080-0x0003FF  on-chip RAM
 *   0x002000-0x002FFF  C352                     (engine/c352.c, clocked in exact MCU time)
 *   0x004000-0x00BFFF  shared RAM with the 68K  -> g_ss22.shared, byte lanes ^1
 *   0x00C000-0x00FFFF  the game's sound ROM [0xC000..] the S22-BIOS
 *   0x200000-0x27FFFF  the game's sound ROM      the sound program + sequence data
 *   0x308000-0x308003  MB87078 volume           (write-only, ignored -- as in Prop Cycle)
 * Ports: P4 latches the I/O control (d3 selects which half of the 16-bit INPUTS word P5 reads,
 * d5/d6 strobe P5's output latch); P5 reads INPUTS; P6 reads 0. A-D channel 0 is the wheel, the pedals are the
 * game's channels (ss22_snd_cfg). The board drives IRQ2 at scanline 240 and IRQ0 at scanline 480 of a 525-line frame
 * ("mcu_irq"); the MCU is held in RESET until the 68K writes syscon 0x16, and by then both pins have
 * been asserted, so leaving reset takes both at once. Clock 49.152 MHz / 3 = 16.384 MHz.
 *
 * Time: the MCU runs in 1/16-frame slices on a fixed cycle grid (the 68K is not run meanwhile); the C352
 * is generated for exactly the MCU time that has passed at every register access, one chip frame per 192
 * MCU cycles (16.384 MHz / (24.576 MHz / 288)).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ss22_game.h"
#include "m37710.h"
#include "c352.h"
#include "audio_out.h"

/* how the sound program's instructions get executed: gen/ss22_snd_driver.c (the translation) in the game,
 * tools/sndoracle/sound_oracle.c (fetch/decode) in dd_oracle */
int  snd_run(m37710_t *c, int cycles);
void snd_executor_init(void);

int g_mcu_pc = -1;                       /* a debug hook the oracle's decode loop references */

#define MCU_HZ     16384000ull
#define FPS_X1000  59906ull              /* the board's video rate x 1000 (25.6 MHz / 814 / 525) */
#define SLICES     16u                   /* dd_main's slices per frame */
#define VLINES     525u
#define CHIP_DIV   192u                  /* MCU cycles per C352 output frame */

static m37710_t  cpu;
static uint8_t  *rom;                    /* the sound ROM, 0x80000 */
static uint8_t   iram[0x380];
static uint8_t   c352sh[0x1000];         /* the register file as the MCU wrote it (a word completes on the odd byte) */
static c352_t    chip;
static uint8_t  *wave;
static bool      ready, running, faulted, chip_ok, out_live;

static uint64_t  cyc_owed;               /* board cycles owed to the grid, x FPS_X1000 * SLICES */
static uint64_t  grid;                   /* board time at the end of the last slice, MCU cycles since power-on */
static uint64_t  base;                   /* board time at which the MCU left reset: board = base + cpu.cycles */
static uint64_t  pin_n;                  /* next board pin event: 2n = IRQ2 at line 240, 2n+1 = IRQ0 at line 480 of frame n */
static int64_t   pin_shift;              /* MCU cycles added to every pin time (DD_SNDPHASE: where in its frame the reset release falls) */
static double    want_phase = 97;        /* the scanline of its frame at which the MCU leaves reset (DD_SNDPHASE overrides, < 0 = wherever the 68K gets there) */
static uint64_t  chip_done;              /* C352 frames generated so far */

static uint16_t  pressed;                /* the INPUTS bits currently held */
static uint16_t  adc[4] = { 0x200, 0, 0, 0 };            /* the A-D channels: 0 the wheel, the pedals as the game wires them */
static uint8_t   iocontrol, outdata;
static uint16_t  outputs;

static FILE     *dump;
static uint32_t  dump_frames;
static FILE     *c352log;

/* ---- board time --------------------------------------------------------------------------------- */
/* <tag>_<name>: the game's test environment variable */
static const char *genv(const char *name)
{
    char v[64]; snprintf(v, sizeof v, "%s_%s", g_ss22_game->tag, name);
    return getenv(v);
}

static uint64_t now(void) { return running ? base + cpu.cycles : grid; }

static void wav_header(FILE *f, uint32_t frames)
{
    uint32_t bytes = frames * 4, v;
    uint8_t h[44] = { 'R','I','F','F' };
#define P32(o, x) do { v = (x); h[o] = v; h[o+1] = v >> 8; h[o+2] = v >> 16; h[o+3] = v >> 24; } while (0)
    P32(4, 36 + bytes); memcpy(h + 8, "WAVEfmt ", 8); P32(16, 16);
    h[20] = 1; h[22] = 2; P32(24, 85333); P32(28, 85333 * 4); h[32] = 4; h[34] = 16;
    memcpy(h + 36, "data", 4); P32(40, bytes);
    fseek(f, 0, SEEK_SET); fwrite(h, 1, 44, f);
}

/* generate the chip's output up to board time t */
static void chip_sync(uint64_t t)
{
    if (!chip_ok) return;
    uint64_t target = t / CHIP_DIV;
    static int16_t buf[4 * 512];
    while (chip_done < target) {
        uint64_t left = target - chip_done;
        int n = left > 512 ? 512 : (int)left;
        c352_generate(&chip, buf, n);
        chip_done += (uint64_t)n;
        if (out_live) eng_audio_push(buf, n);
        if (dump) {
            for (int i = 0; i < n; i++) fwrite(&buf[i * 4], 2, 2, dump);      /* the front pair */
            dump_frames += (uint32_t)n;
        }
    }
}

/* ---- the MCU's bus --------------------------------------------------------------------------------- */
static uint8_t bus_r(void *u, uint32_t a)
{
    (void)u;
    if (a >= 0x200000 && a < 0x280000) return rom[a - 0x200000];
    if (a >= 0x00C000 && a < 0x010000) return rom[a];
    if (a >= 0x004000 && a < 0x00C000) return g_ss22.shared[(a - 0x4000) ^ 1];
    if (a >= 0x002000 && a < 0x003000) {
        unsigned off = a - 0x2000;
        chip_sync(now());
        unsigned w = chip_ok ? c352_read(&chip, off >> 1) : 0;
        return (uint8_t)((off & 1) ? w >> 8 : w);
    }
    if (a >= 0x80 && a < 0x400) return iram[a - 0x80];
    return 0;
}

static void bus_w(void *u, uint32_t a, uint8_t v)
{
    (void)u;
    if (a >= 0x004000 && a < 0x00C000) { g_ss22.shared[(a - 0x4000) ^ 1] = v; return; }
    if (a >= 0x002000 && a < 0x003000) {
        unsigned off = a - 0x2000;
        c352sh[off] = v;
        if (c352log)
            fprintf(c352log, "%.9f %x %x %x\n", (double)now() / MCU_HZ, off & ~1u,
                    (off & 1) ? (unsigned)v << 8 : v, (off & 1) ? 0xff00u : 0x00ffu);
        if (off & 1) {                   /* the core writes low then high: the word is complete on the odd byte */
            chip_sync(now());
            if (chip_ok) c352_write(&chip, off >> 1, (uint16_t)(c352sh[off - 1] | v << 8), 0xffff);
        }
        return;
    }
    if (a >= 0x80 && a < 0x400) { iram[a - 0x80] = v; return; }
    /* 0x300000 / 0x301000 / 0x308000: nopr / watchdog / MB87078 -- ignored */
}

/* ---- ports (mcu_port4/5/6) -------------------------------------------------------------------------- */
static uint8_t port_r(void *u, unsigned reg, uint8_t latch)
{
    (void)u;
    const uint8_t dir = cpu.sfr[reg == 0x0A ? 0x0C : reg == 0x0B ? 0x0D : 0x10];
    uint8_t in;
    switch (reg) {
    case 0x0A: in = iocontrol; break;                                                     /* mcu_port4_r */
    case 0x0B: {                                                                          /* mcu_port5_r */
        const uint16_t inputs = (uint16_t)(g_ss22_game->snd.inputs_idle & ~pressed);      /* IP_ACTIVE_LOW, idle high (Dirt Dash: bit 9 is the cabinet switch, Standard = 0) */
        in = (iocontrol & 8) ? (uint8_t)(inputs & 0xFF) : (uint8_t)(inputs >> 8);
        break;
    }
    default: in = 0; break;                                                               /* mcu_port6_r: discarded */
    }
    return (uint8_t)((latch & dir) | (in & ~dir));
}

static void port_w(void *u, unsigned reg, uint8_t v)
{
    (void)u;
    if (reg == 0x0B) outdata = v;                                                         /* mcu_port5_w */
    else if (reg == 0x0A) {                                                               /* mcu_port4_w */
        if (~iocontrol & v & 0x20) outputs = (uint16_t)((outputs & 0xFF00) | outdata);    /* d5: strobe out 0-7 */
        if (~iocontrol & v & 0x40) outputs = (uint16_t)((outputs & 0x00FF) | outdata << 8);/* d6: strobe out 8-15 */
        iocontrol = v;
    }
}

/* ---- lifecycle ---------------------------------------------------------------------------------------- */
static bool slurp(const char *dir, const char *n, uint8_t *dst, size_t len)
{
    char p[1024]; snprintf(p, sizeof p, "%s/%s", dir, n);
    FILE *f = fopen(p, "rb");
    bool ok = f && fread(dst, 1, len, f) == len;
    if (f) fclose(f);
    if (!ok) fprintf(stderr, "[SND] cannot read %s\n", p);
    return ok;
}

bool ss22_snd_init(const char *dir)
{
    const ss22_snd_cfg *c = &g_ss22_game->snd;
    g_ss22_snd_set_run = ss22_snd_set_run;           /* syscon 0x16 reaches this MCU (engine/ss22_board.c) */
    rom = calloc(1, 0x80000);
    wave = calloc(1, c->wave_size);
    if (!rom || !wave || !slurp(dir, c->rom, rom, 0x80000)) return false;
    chip_ok = slurp(dir, c->wave[0], wave + c->wave_off[0], 0x400000);
    if (chip_ok && c->wave[1]) chip_ok = slurp(dir, c->wave[1], wave + c->wave_off[1], 0x400000);
    if (chip_ok) { c352_init(&chip, wave, c->wave_size); c352_reset(&chip); }
    m37710_init(&cpu, bus_r, bus_w, NULL);
    cpu.port_r = port_r; cpu.port_w = port_w;
    snd_executor_init();
    const char *d = genv("AUDIODUMP");
    if (d && (dump = fopen(d, "wb"))) { wav_header(dump, 0); fprintf(stderr, "[SND] dumping the front pair to %s\n", d); }
    const char *l = genv("C352LOG");
    if (l && (c352log = fopen(l, "w"))) { setvbuf(c352log, NULL, _IOFBF, 1 << 20); fprintf(c352log, "# ours: time = MCU cycles / %llu\n", (unsigned long long)MCU_HZ); }
    if (genv("MCUTRAP")) cpu.pchist_on = 1;
    if (genv("SNDPHASE")) want_phase = atof(genv("SNDPHASE"));
    ready = true;
    ss22_snd_inputs(0, 0x200, 0, 0);                      /* wheel centred, pedals up, nothing pressed */
    return true;
}

/* board time of scanline `line` of frame k, in MCU cycles */
static uint64_t pin_time(uint64_t k, uint64_t line)
{
    const unsigned __int128 num = (unsigned __int128)MCU_HZ * 1000u, den = (unsigned __int128)FPS_X1000 * VLINES;
    return (uint64_t)(((unsigned __int128)(k * VLINES + line) * num) / den);
}
static int64_t pin_time_n(uint64_t n)
{
    return (int64_t)pin_time(n >> 1, (n & 1) ? 480 : 240) + pin_shift;
}

void ss22_snd_set_run(bool run)
{
    if (!ready || run == running) return;
    if (genv("SNDLOG")) fprintf(stderr, "[SND] syscon 0x16 <- %d after %llu board cycles\n", run, (unsigned long long)grid);
    if (!run) { running = false; return; }               /* held in reset: the state stays, time keeps passing */
    chip_sync(grid);
    m37710_reset(&cpu);                                  /* leaving reset restarts the chip */
    running = true; base = grid;
    /* both board pins have long been asserted (HOLD_LINE): taken as soon as the driver opens its mask */
    m37710_irq(&cpu, 0x10);
    m37710_irq(&cpu, 0x14);
    /* the next scheduled pin after now. The 68K's own time is a poll budget, not cycles, so WHERE in its frame the
     * release falls is an accident of that model. It matters: the driver opens its interrupt mask ~1 frame after
     * reset and the first NEW pin it sees is IRQ2 (line 240) or IRQ0 (line 480) by that phase, which orders its whole
     * early interleaving. The default is where the machine's 68K released it, measured off MAME's own C352 write
     * times (its first register write, at 3.34166 s, is line ~97 of frame 200); DD_SNDPHASE=<line> overrides */
    pin_shift = 0;
    if (want_phase >= 0) {
        const uint64_t fr = pin_time(1, 0);
        pin_shift = (int64_t)grid - (int64_t)((grid / fr) * fr + (uint64_t)(want_phase * (double)fr / 525.0));
    }
    pin_n = 0;
    while (pin_time_n(pin_n) <= (int64_t)grid) pin_n++;
    fprintf(stderr, "[SND] sound CPU out of reset at board cycle %llu, PC=%04X\n", (unsigned long long)grid, cpu.pc);
}

static void raise_due_pins(void)
{
    for (;;) {
        if ((int64_t)now() < pin_time_n(pin_n)) return;
        m37710_irq(&cpu, (pin_n & 1) ? 0x14 : 0x10);    /* IRQ0 at 480 (vector 0xFFF4), IRQ2 at 240 (0xFFF0) */
        pin_n++;
    }
}

static uint64_t next_pin_time(void) { return (uint64_t)pin_time_n(pin_n); }

void ss22_snd_slice(void)
{
    if (!ready) return;
    cyc_owed += MCU_HZ * 1000ull;
    const uint64_t d = FPS_X1000 * SLICES;
    const uint64_t cyc = cyc_owed / d;
    cyc_owed -= cyc * d;
    if (running && !cpu.unimpl_hit) {
        const uint64_t end = grid + cyc;                 /* a fixed grid: an overrun is borrowed from the next slice */
        while (base + cpu.cycles < end && !cpu.unimpl_hit) {
            raise_due_pins();
            uint64_t stop = end, np = next_pin_time();
            if (np < stop) stop = np;
            { extern uint64_t snd_spin_fast(m37710_t *, uint64_t); snd_spin_fast(&cpu, stop - base); }   /* a wait loop, jumped exactly */
            uint64_t n = stop - (base + cpu.cycles);
            if (n < 1) n = 1;
            snd_run(&cpu, n < 64 ? (int)n : 64);
        }
        if (cpu.unimpl_hit) {
            fprintf(stderr, "[SND] cannot execute at %06X (op 0x%03X) -- sound halted\n", cpu.unimpl_pc, cpu.unimpl_op);
            faulted = true; running = false;
        }
    }
    grid += cyc;
    if (!running) chip_sync(grid);
    else chip_sync(base + cpu.cycles > grid ? base + cpu.cycles : grid);
}

bool ss22_snd_faulted(void) { return faulted; }
void ss22_snd_set_output(bool on) { out_live = on; }
void ss22_snd_inputs(uint16_t p, unsigned wheel, unsigned pedal1, unsigned pedal2)
{
    const ss22_snd_cfg *c = &g_ss22_game->snd;
    pressed = p;
    adc[0] = (uint16_t)(wheel & 0x3FF);
    adc[c->adc_pedal[0]] = (uint16_t)(pedal1 & 0x3FF);
    adc[c->adc_pedal[1]] = (uint16_t)(pedal2 & 0x3FF);
    cpu.analog[0] = adc[0]; cpu.analog[c->adc_pedal[0]] = adc[c->adc_pedal[0]]; cpu.analog[c->adc_pedal[1]] = adc[c->adc_pedal[1]];
}
uint16_t ss22_snd_outputs(void) { return outputs; }

void ss22_snd_close(void)
{
    if (dump) { wav_header(dump, dump_frames); fclose(dump); dump = NULL;
                fprintf(stderr, "[SND] wrote %u samples (%.1f s)\n", dump_frames, dump_frames / 85333.0); }
    if (c352log) { fclose(c352log); c352log = NULL; }
}

void ss22_snd_debug(char *buf, int n)
{
    if (!ready) { snprintf(buf, n, "snd: none"); return; }
    snprintf(buf, n, "snd: run %d fault %d pc %02X%04X irqs %llu cycles %llu ps %04X out %04X in %04X ad %03X/%03X/%03X",
             running, faulted, cpu.pg, cpu.pc, (unsigned long long)cpu.irq_taken, (unsigned long long)cpu.cycles,
             cpu.ps, outputs, (unsigned)(g_ss22_game->snd.inputs_idle & ~pressed), adc[0], adc[g_ss22_game->snd.adc_pedal[0]], adc[g_ss22_game->snd.adc_pedal[1]]);
}
