/*
 * s21_board.c -- see s21_board.h. The System 21 board as the two 68000s see it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "s21_board.h"
#include "s21_cpu.h"
#include "s21_dsp.h"
#include "s21_snd.h"
#include "s21_io.h"
#include "s21_link.h"                     /* module N2: the C139 link (src/link/) */

s21_board_t g_s21;
int s21_ipf = 19000, s21_slices = 200;
static int only = -1;
void s21_set_only(int cpu) { only = cpu; }

#define CLK68K 12288000
#define FRAME_CYCLES 205005                 /* 12.288 MHz / (38.76922 MHz / 4 * 2 / (616 * 525)) = 59.94 Hz */

/* ---------------- C148 x2 (namco_c148): per-source IRQ levels; the 68000 input LINES are per level ---------------- */
enum { SRC_CPU, SRC_EX, SRC_POS, SRC_SCI, SRC_VBL, NSRC };
typedef struct { uint8_t lvl[NSRC], bus; uint8_t line[8], hold[8]; } c148_t;
static c148_t ic[2];

static void line_set(int cpu, int lvl, int state)  /* state: 0 clear, 1 assert, 2 hold */
{
    if (lvl <= 0 || lvl > 7) return;
    ic[cpu].line[lvl] = state != 0; ic[cpu].hold[lvl] = state == 2;
}
static void src_ack(int cpu, int src) { line_set(cpu, ic[cpu].lvl[src], 0); }
static void lvl_w(int cpu, int src, uint8_t d) { src_ack(cpu, src); ic[cpu].lvl[src] = d & 7; }
static void c148_reset(int cpu) { for (int s = 0; s < NSRC; s++) lvl_w(cpu, s, 0); }

int s21_irq_level(int cpu)
{
    for (int l = 7; l > 0; l--) if (ic[cpu].line[l]) return l;
    return 0;
}
void s21_irq_taken(int cpu, int level) { if (ic[cpu].hold[level]) line_set(cpu, level, 0); }
void s21_board_sci_irq(int cpu) { line_set(cpu, ic[cpu].lvl[SRC_SCI], 1); }   /* the C139 (src/link): until acked at 0x1DC000 */

/* ---------------- resets ---------------- */
static void reset_all_subcpus(int asserted)        /* master C148 ext2 bit 0 (0 = hold) */
{
    int was = g_s21.slave_reset;
    g_s21.slave_reset = asserted;
    if (was && !asserted) s21_cpu1_reset();         /* released: the slave starts from its reset vectors */
    s21_io_reset(asserted);                         /* hold = 1 */
    s21_dsp_reset(asserted);
    if (asserted) c148_reset(1);
}
static void ext1_w(uint8_t d)                       /* sound CPU reset, DSP kick-start */
{
    g_s21.snd_reset = !(d & 1);
    s21_snd_reset(g_s21.snd_reset);
    if (d & 4) s21_dsp_kick();
}

static uint16_t c148_r(int cpu, uint32_t off)
{
    switch (off & 0x3E000) {
    case 0x04000: return ic[cpu].bus;
    case 0x06000: return ic[cpu].lvl[SRC_CPU];
    case 0x08000: return ic[cpu].lvl[SRC_EX];
    case 0x0A000: return ic[cpu].lvl[SRC_POS];
    case 0x0C000: return ic[cpu].lvl[SRC_SCI];
    case 0x0E000: return ic[cpu].lvl[SRC_VBL];
    case 0x16000: src_ack(cpu, SRC_CPU); return 0;
    case 0x18000: src_ack(cpu, SRC_EX); return 0;
    case 0x1A000: src_ack(cpu, SRC_POS); return 0;
    case 0x1C000: src_ack(cpu, SRC_SCI); return 0;
    case 0x1E000: src_ack(cpu, SRC_VBL); return 0;
    case 0x20000: return 7;                         /* ext_r: nothing connected; the C148 reads 7 then (EEPROM ready) */
    default: return 0;
    }
}
static void c148_w(int cpu, uint32_t off, uint16_t d, uint16_t mask)
{
    const int lo = (mask & 0x00FF) != 0;            /* the byte registers are on the odd lane (umask 0x00ff) */
    switch (off & 0x3E000) {
    case 0x04000: if (lo) ic[cpu].bus = d & 7; break;
    case 0x06000: if (lo) lvl_w(cpu, SRC_CPU, (uint8_t)d); break;
    case 0x08000: if (lo) lvl_w(cpu, SRC_EX, (uint8_t)d); break;
    case 0x0A000: if (lo) lvl_w(cpu, SRC_POS, (uint8_t)d); break;
    case 0x0C000: if (lo) lvl_w(cpu, SRC_SCI, (uint8_t)d); break;
    case 0x0E000: if (lo) lvl_w(cpu, SRC_VBL, (uint8_t)d); break;
    case 0x10000: line_set(cpu ^ 1, ic[cpu ^ 1].lvl[SRC_CPU], 1); break;   /* the LINKED C148's cpu irq */
    case 0x16000: src_ack(cpu, SRC_CPU); break;
    case 0x18000: src_ack(cpu, SRC_EX); break;
    case 0x1A000: src_ack(cpu, SRC_POS); break;
    case 0x1C000: src_ack(cpu, SRC_SCI); break;
    case 0x1E000: src_ack(cpu, SRC_VBL); break;
    case 0x22000: if (lo && cpu == 0) ext1_w(d & 7); break;   /* only the master's C148 has the ext callbacks */
    case 0x24000: if (lo && cpu == 0) reset_all_subcpus(!(d & 1)); break;
    default: break;
    }
}

/* ---------------- the memory maps, one 16-bit word at a time ---------------- */
static unsigned n_unmapped;
static void unmapped(int cpu, uint32_t a, int w)
{
    if (n_unmapped++ < 24) fprintf(stderr, "[S21] %s unmapped %s at %06X (frame %u)\n", cpu ? "slave" : "master", w ? "write" : "read", a, g_s21.frame);
}
#define COMBINE(dst, d, m) ((dst) = (uint16_t)(((dst) & ~(m)) | ((d) & (m))))

static uint16_t word_r(int cpu, uint32_t a)
{
    if (a < 0x200000) {
        if (cpu == 0) {
            if (a < 0x100000) return g_s21.mrom[a >> 1];
            if (a < 0x110000) return g_s21.mram[(a - 0x100000) >> 1];
            if (a >= 0x180000 && a < 0x184000) return g_s21.nvram[(a - 0x180000) >> 1];
        } else {
            if (a < 0x080000) return g_s21.srom[a >> 1];
            if (a >= 0x100000 && a < 0x140000) return g_s21.sram[(a - 0x100000) >> 1];
        }
        if (a >= 0x1C0000) return c148_r(cpu, a - 0x1C0000);
        unmapped(cpu, a, 0); return 0;
    }
    if (a < 0x210000) return s21_dsp_ram_r((a - 0x200000) >> 1);
    if (a >= 0x440000 && a < 0x440002) return s21_dsp_ptram_r();
    if (a >= 0x480000 && a < 0x480800) return s21_dsp_depthcue_r((a - 0x480000) >> 1);
    if (a >= 0x700000 && a < 0x720000) return g_s21.spr[(a - 0x700000) >> 1];
    if (a >= 0x720000 && a < 0x720008) return g_s21.sprpos[(a - 0x720000) >> 1];
    if (a >= 0x740000 && a < 0x750000) return g_s21.pal[(a - 0x740000) >> 1];
    if (a >= 0x750000 && a < 0x760000) return g_s21.palext[(a - 0x750000) >> 1];
    if (a >= 0x760000 && a < 0x760002) return g_s21.vena;
    if (a >= 0x800000 && a < 0x900000) return g_s21.data[(a - 0x800000) >> 1];
    if (a >= 0x900000 && a < 0x910000) return g_s21.shared[(a - 0x900000) >> 1];
    if (a >= 0xA00000 && a < 0xA01000) return g_s21.dpram[(a - 0xA00000) >> 1];
    if (a >= 0xB00000 && a < 0xB04000) return g_s21.c139ram[(a - 0xB00000) >> 1];
    if (a >= 0xB80000 && a < 0xB80010) return s21_c139_reg_r(a);   /* C139 (src/link; unlinked: status 4 as before) */
    if (a >= 0xC00000 && a < 0xE00000) return g_s21.edata[(a & 0xFFFFF) >> 1];
    unmapped(cpu, a, 0); return 0;
}

static void word_w(int cpu, uint32_t a, uint16_t d, uint16_t m)
{
    if (a < 0x200000) {
        if (cpu == 0) {
            if (a < 0x100000) return;
            if (a < 0x110000) { COMBINE(g_s21.mram[(a - 0x100000) >> 1], d, m); return; }
            if (a >= 0x180000 && a < 0x184000) { if (m & 0xFF) g_s21.nvram[(a - 0x180000) >> 1] = (uint8_t)d; return; }
        } else {
            if (a < 0x080000) return;
            if (a >= 0x100000 && a < 0x140000) { COMBINE(g_s21.sram[(a - 0x100000) >> 1], d, m); return; }
        }
        if (a >= 0x1C0000) { c148_w(cpu, a - 0x1C0000, d, m); return; }
        unmapped(cpu, a, 1); return;
    }
    if (a < 0x210000) { s21_dsp_ram_w((a - 0x200000) >> 1, d, m); return; }
    if (a >= 0x280000 && a < 0x280002) return;
    if (a >= 0x400000 && a < 0x400002) { s21_dsp_ptram_ctl_w(d, m); return; }
    if (a >= 0x440000 && a < 0x440002) { s21_dsp_ptram_w(d, m); return; }
    if (a >= 0x440002 && a < 0x480000) return;
    if (a >= 0x480000 && a < 0x480800) { s21_dsp_depthcue_w((a - 0x480000) >> 1, d, m); return; }
    if (a >= 0x700000 && a < 0x720000) { s21_video_spriteram_w(g_s21.spr, (a - 0x700000) >> 1, d, m); return; }
    if (a >= 0x720000 && a < 0x720008) { COMBINE(g_s21.sprpos[(a - 0x720000) >> 1], d, m); return; }
    if (a >= 0x740000 && a < 0x750000) { COMBINE(g_s21.pal[(a - 0x740000) >> 1], d, m); return; }
    if (a >= 0x750000 && a < 0x760000) { COMBINE(g_s21.palext[(a - 0x750000) >> 1], d, m); return; }
    if (a >= 0x760000 && a < 0x760002) { COMBINE(g_s21.vena, d, m); return; }
    if (a >= 0x800000 && a < 0x900000) return;
    if (a >= 0x900000 && a < 0x910000) { COMBINE(g_s21.shared[(a - 0x900000) >> 1], d, m); return; }
    if (a >= 0xA00000 && a < 0xA01000) { if (m & 0xFF) g_s21.dpram[(a - 0xA00000) >> 1] = (uint8_t)d; return; }
    if (a >= 0xB00000 && a < 0xB04000) { COMBINE(g_s21.c139ram[(a - 0xB00000) >> 1], d, m); return; }
    if (a >= 0xB80000 && a < 0xB80010) { s21_c139_reg_w(a, d, m); return; }
    if (a >= 0xC00000 && a < 0xE00000) return;
    unmapped(cpu, a, 1);
}

uint32_t s21_bus_read(int cpu, uint32_t a, int size)
{
    if (size == 1) { uint16_t w = word_r(cpu, a & ~1u); return (a & 1) ? (w & 0xFF) : (w >> 8); }
    if (size == 2) return word_r(cpu, a & ~1u);
    return (uint32_t)word_r(cpu, a & ~1u) << 16 | word_r(cpu, (a + 2) & 0xFFFFFE);
}
void s21_bus_write(int cpu, uint32_t a, int size, uint32_t v)
{
    if (size == 1) { if (a & 1) word_w(cpu, a & ~1u, v & 0xFF, 0x00FF); else word_w(cpu, a, (v & 0xFF) << 8, 0xFF00); return; }
    if (size == 2) { word_w(cpu, a & ~1u, (uint16_t)v, 0xFFFF); return; }
    word_w(cpu, a & ~1u, (uint16_t)(v >> 16), 0xFFFF);
    word_w(cpu, (a + 2) & 0xFFFFFE, (uint16_t)v, 0xFFFF);
}

/* the trace oracle (dev): reads whose value comes from outside this 68000 -- the DSPs, the other 68000, the C68, the link chip */
int s21_env_in(int cpu, uint32_t a)
{
    (void)cpu;
    return (a >= 0x200000 && a < 0x210000) || (a >= 0x440000 && a < 0x440002) || (a >= 0x480000 && a < 0x480800) ||
           (a >= 0x700000 && a < 0x720008) || (a >= 0x740000 && a < 0x760000) ||
           (a >= 0x900000 && a < 0x910000) || (a >= 0xA00000 && a < 0xA01000) || (a >= 0xB00000 && a < 0xB80010);
}
void s21_env_irq(int cpu, int level) { (void)cpu; (void)level; }

/* ---------------- ROMs, NVRAM, reset ---------------- */
static bool load_file(const char *dir, const char *name, uint8_t *dst, size_t n)
{
    char p[1024]; snprintf(p, sizeof p, "%s/%s", dir, name);
    FILE *f = fopen(p, "rb");
    if (!f) { fprintf(stderr, "[S21] missing %s\n", p); return false; }
    size_t got = fread(dst, 1, n, f); fclose(f);
    if (got != n) { fprintf(stderr, "[S21] short %s (%zu of %zu)\n", p, got, n); return false; }
    return true;
}
/* ROM_LOAD16_BYTE pair: the 'u' chip is the even (high) byte */
static bool load_pair(const char *dir, const char *u, const char *l, uint16_t *dst, size_t nbytes_each)
{
    uint8_t *bu = malloc(nbytes_each), *bl = malloc(nbytes_each);
    bool ok = load_file(dir, u, bu, nbytes_each) && load_file(dir, l, bl, nbytes_each);
    if (ok) for (size_t i = 0; i < nbytes_each; i++) dst[i] = (uint16_t)(bu[i] << 8 | bl[i]);
    free(bu); free(bl);
    return ok;
}

bool s21_nvram_load(const char *path)
{
    FILE *f = fopen(path, "rb"); if (!f) return false;
    size_t n = fread(g_s21.nvram, 1, sizeof g_s21.nvram, f); fclose(f);
    return n == sizeof g_s21.nvram;
}
bool s21_nvram_save(const char *path)
{
    char tmp[1100]; snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb"); if (!f) return false;
    bool ok = fwrite(g_s21.nvram, 1, sizeof g_s21.nvram, f) == sizeof g_s21.nvram;
    ok = (fclose(f) == 0) && ok;
    return ok && rename(tmp, path) == 0;
}

static void machine_reset(void)
{
    memset(ic, 0, sizeof ic);
    c148_reset(0); c148_reset(1);
    g_s21.snd_reset = 1; s21_snd_reset(1);
    g_s21.slave_reset = 0; reset_all_subcpus(1);   /* the slave, C68 and DSPs held until the master releases them */
    s21_cpu0_reset(); s21_cpu1_reset();
}

/* the operator's restart: what leaving test mode after changing a setting does on the cabinet -- every CPU from its reset vector,
   the NVRAM (settings, POSITION) kept as it is in RAM. Between frames only. */
void s21_board_reset(void)
{
    machine_reset();
    fprintf(stderr, "[S21] board reset at frame %u (NVRAM kept)\n", g_s21.frame);
}

bool s21_init(const char *romdir)
{
    const char *e;
    if ((e = getenv("S21_IPF"))) s21_ipf = atoi(e);
    if ((e = getenv("S21_SLICES"))) s21_slices = atoi(e);
    if (s21_slices < 1) s21_slices = 1;
    bool ok = load_pair(romdir, "cy2-mpr-u.3j", "cy2-mpr-l.1j", g_s21.mrom, 0x80000)
           && load_pair(romdir, "cy2-spr-u.6c", "cy2-spr-l.4c", g_s21.srom, 0x40000)   /* MAME maps 512 KB; the upper halves are 0xFF */
           && load_pair(romdir, "cy1-data-u.3a", "cy1-data-l.1a", g_s21.data, 0x80000)
           && load_pair(romdir, "cy1-edata0-u.3b", "cy1-edata0-l.1b", g_s21.edata, 0x80000);
    if (!ok) return false;
    /* the other modules: DSPs (point ROM, c67.bin), sound (cy1-snd0, cy1-voi0..3), the C68 I/O MCU (c68.bin) */
    if (!s21_dsp_init(romdir)) fprintf(stderr, "[S21] DSP init failed: %s\n", s21_dsp_error() ? s21_dsp_error() : "?");
    {
        static uint8_t snd[0x20000], voi[4][0x80000], c68[0x8000];
        static const char *vn[4] = { "cy1-voi0.12b", "cy1-voi1.12c", "cy1-voi2.12d", "cy1-voi3.12e" };
        bool sok = load_file(romdir, "cy1-snd0.8j", snd, sizeof snd);
        for (int i = 0; i < 4; i++) sok = load_file(romdir, vn[i], voi[i], sizeof voi[i]) && sok;
        const uint8_t *vp[4] = { voi[0], voi[1], voi[2], voi[3] };
        if (sok) s21_snd_init(snd, vp, g_s21.dpram);
        if (load_file(romdir, "c68.bin", c68, sizeof c68)) s21_io_init(c68, g_s21.dpram);
    }
    char p[1024]; snprintf(p, sizeof p, "%s/cybsled.nv", romdir);   /* the default settings incl. calibration (ROM region "nvram") */
    if (!s21_nvram_load(p)) fprintf(stderr, "[S21] no default NVRAM %s -- zeroes\n", p);
    machine_reset();
    s21_link_init_env();                            /* CS_LINK: the C139 link to another cabinet (src/link) */
    return true;
}

void s21_run_frame(void)
{
    static int32_t credit[2];
    static double dsp_acc, io_acc;
    static int dsp_dead;
    const int vbl_slice = s21_slices * 480 / 525;   /* vblank starts at line 480 of 525 */
    const int per = s21_ipf / s21_slices;
    s21_cpu0_set_frame(g_s21.frame); s21_cpu1_set_frame(g_s21.frame);
    s21_link_frame_begin(g_s21.frame);              /* the other cabinet's packets for this frame (lockstep; no-op unlinked) */
    for (int s = 0; s < s21_slices; s++) {
        const int cyc = FRAME_CYCLES / s21_slices;
        for (int c = 0; c < 2; c++) {
            if (only >= 0 && only != c) continue;
            if (c == 1 && g_s21.slave_reset && only != 1) { credit[1] = 0; continue; }
            int32_t b = per + credit[c];
            int32_t ran = c ? s21_cpu1_run(b) : s21_cpu0_run(b);
            credit[c] = b - ran;
            if (credit[c] < -per) credit[c] = -per; else if (credit[c] > per) credit[c] = per;
            g_s21.ins[c] += (uint64_t)ran;
        }
        dsp_acc += (double)cyc * 10.0 / 12.288; io_acc += cyc / 6.0;   /* DSP 10 MHz master cycles; 6809 and M37450 2.048 MHz */
        { long n = (long)dsp_acc; dsp_acc -= n;
          if (!dsp_dead && !s21_dsp_run(n)) { dsp_dead = 1; fprintf(stderr, "[S21] the DSPs stopped (frame %u): %s\n", g_s21.frame, s21_dsp_error() ? s21_dsp_error() : "?"); } }
        { int n = (int)io_acc; io_acc -= n;
          /* both 2.048 MHz CPUs run to ONE absolute board time: each run ends a part-instruction past its target, and asking for
           * n more cycles from there let the 6809 creep ahead of the M37450 (0.36%, ~445,000 cycles after 3,600 frames). Their
           * shared slice origin (src/io/s21_sched.c) then put every A/D completion in the M37450's future, the game restarted the
           * conversion each frame before it finished, and channels 0-3 -- the two sticks -- were never read. */
          static uint64_t board_t; if (!board_t) board_t = s21_io_cycles() > s21_snd_cycles() ? s21_io_cycles() : s21_snd_cycles();
          board_t += (uint64_t)n;
          if (board_t > s21_io_cycles()) s21_io_run((int)(board_t - s21_io_cycles()));
          if (board_t > s21_snd_cycles()) s21_snd_run((int)(board_t - s21_snd_cycles())); }
        s21_c139_slice(s);
        if (s == vbl_slice) {
            line_set(0, ic[0].lvl[SRC_VBL], 2);
            line_set(1, ic[1].lvl[SRC_VBL], 2);
            s21_io_vblank();
        }
    }
    { static int lg = -1; if (lg < 0) lg = getenv("CS_CLKLOG") != NULL;   /* CS_CLKLOG=1: the I/O and sound clocks every 600 frames */
      if (lg && g_s21.frame % 600 == 0) fprintf(stderr, "[CLK] f%u io %llu snd %llu\n", g_s21.frame, (unsigned long long)s21_io_cycles(), (unsigned long long)s21_snd_cycles()); }
    s21_link_frame_end(g_s21.frame);
    g_s21.frame++;
}

void s21_board_quad_hooks(void (*quad)(const void *q), void (*swap)(void))
{
    s21_quad_out = (void (*)(const s21_quad *))quad;
    s21_swap_out = swap;
}
