/*
 * ss22_board.c -- see ss22_board.h. Super System 22 map from MAME namcos22.cpp (namcos22s_am) and the handlers in namcos22.cpp /
 * namcos22_v.cpp, and the devices the 68EC020 talks to:
 *
 *  syscon 0x700000 (ss22_syscon_w): 0x00-0x03 IRQ level+enable (VBLANK, scanline, SCI, ?), 0x04-0x07 acknowledge, 0x14 watchdog,
 *    0x16 sound MCU enable, 0x1C DSP control. On Super 22 a line is enabled by a NON-ZERO level (System 22 tests bit 4).
 *  keycus 0x400000: a game's protection answer is one fixed value at one 16-bit unit and nothing else; every other read is a fresh
 *    random value. MAME draws it with machine().rand(), so a trace comparison replays the values MAME returned (<tag>_KEYCUS_FILE).
 *  DSW 0x440000: every switch off (0xFFFFFFFF: test mode off).
 *  portbit 0x450008: two serial bit ports, reset by a write; the MAME defaults (0xFFFF).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ss22_board.h"

static const ss22_board_cfg *cfg;
static uint32_t ss22_hw_keycus_r(uint32_t unit);
static uint32_t ss22_hw_dsw(void);
static uint32_t ss22_hw_portbit_r(uint32_t unit);
static void     ss22_hw_portbit_w(uint32_t unit);
static void     ss22_hw_syscon_w(uint32_t off, uint8_t data);
static int      g_ss22_mbox;
static void     ss22_mbox_log(uint32_t off, int size, uint32_t v);
void (*g_ss22_dsp_control)(uint8_t v);
void (*g_ss22_snd_set_run)(bool run);

ss22_sys_t g_ss22;
#ifdef RR_TRACE
uint32_t g_ss22_last_pc;                           /* the last instruction the trace hook saw (named in an unmapped-access report) */
#endif

static uint32_t be_rd(const uint8_t *m, uint32_t off, int size)
{
    uint32_t v = 0;
    for (int i = 0; i < size; i++) v = v << 8 | m[off + i];
    return v;
}
static void be_wr(uint8_t *m, uint32_t off, int size, uint32_t v)
{
    for (int i = size - 1; i >= 0; i--) { m[off + i] = (uint8_t)v; v >>= 8; }
}

static void complain(const char *what, uint32_t a, int size)
{
    static uint32_t seen[256]; static int nseen;
    uint32_t page = a & ~0xFFFu;
    for (int i = 0; i < nseen; i++) if (seen[i] == page) return;
    if (nseen < 256) seen[nseen++] = page;
#ifdef RR_TRACE
    { extern uint32_t g_ss22_last_pc; fprintf(stderr, "[%sMEM] %s %d-byte access at 0x%06X (after the instruction at 0x%06X)\n", cfg->tag, what, size, a, g_ss22_last_pc); }
#else
    fprintf(stderr, "[%sMEM] %s %d-byte access at 0x%06X\n", cfg->tag, what, size, a);
#endif
}

/* ---- MAME handler-width semantics --------------------------------------------------------
 * A device whose handler is `w` bytes wide (1, 2 or 4) is called once per w-byte unit the
 * access touches, high unit first; a narrower access takes its lanes from one call. */
typedef uint32_t (*rd_fn)(uint32_t unit);
typedef void     (*wr_fn)(uint32_t unit, uint32_t data, uint32_t mask);

static uint32_t dev_read(uint32_t off, int size, int w, rd_fn h)
{
    if (size >= w) {
        uint32_t v = 0;
        for (int i = 0; i < size; i += w) v = (w == 4 ? 0 : v << (8 * w)) | h((off + (uint32_t)i) / (uint32_t)w);
        return v;
    }
    uint32_t val = h(off / (uint32_t)w);
    int shift = (w - (int)(off % (uint32_t)w) - size) * 8;
    return (val >> shift) & (size == 1 ? 0xFFu : 0xFFFFu);
}
static void dev_write(uint32_t off, int size, uint32_t v, int w, wr_fn h)
{
    uint32_t wmask = w == 4 ? 0xFFFFFFFFu : w == 2 ? 0xFFFFu : 0xFFu;
    if (size >= w) {
        for (int i = 0; i < size; i += w)
            h((off + (uint32_t)i) / (uint32_t)w, (w == 4 ? v : (v >> (8 * (size - i - w)))) & wmask, wmask);
        return;
    }
    uint32_t smask = size == 1 ? 0xFFu : 0xFFFFu;
    int shift = (w - (int)(off % (uint32_t)w) - size) * 8;
    h(off / (uint32_t)w, (v & smask) << shift, smask << shift);
}

/* ---- device handlers (namcos22.cpp / namcos22_v.cpp) ---- */
static uint32_t keycus_h(uint32_t unit) { return ss22_hw_keycus_r(unit); }

static uint32_t sci_h(uint32_t unit) { return unit == 0 ? 0x0004 : 0; }          /* namcos22_sci_r */

static uint32_t portbit_h(uint32_t unit) { return ss22_hw_portbit_r(unit); }
static void     portbit_wh(uint32_t unit, uint32_t d, uint32_t m) { (void)d; (void)m; ss22_hw_portbit_w(unit); }

static uint32_t czattr_h(uint32_t unit) { return g_ss22.czattr[unit & 7]; }
static void czattr_wh(uint32_t unit, uint32_t d, uint32_t m)
{
    unit &= 7;
    g_ss22.czattr[unit] = (uint16_t)((g_ss22.czattr[unit] & ~m) | (d & m));
}

/* namcos22s_czram_r/w: 32-bit handler over four banks of 16-bit words */
static uint32_t czram_h(uint32_t unit)
{
    int bank = g_ss22.czattr[5] & 3;
    unit &= 0x7F;
    return (uint32_t)g_ss22.czram[bank][unit * 2] << 16 | g_ss22.czram[bank][unit * 2 + 1];
}
static void czram_wh(uint32_t unit, uint32_t d, uint32_t m)
{
    unit &= 0x7F;
    for (int bank = 0; bank < 4; bank++) {
        if (~g_ss22.czattr[4] >> (bank * 4) & 1) {            /* write enable when the bit is 0 */
            uint32_t prev = (uint32_t)g_ss22.czram[bank][unit * 2] << 16 | g_ss22.czram[bank][unit * 2 + 1];
            uint32_t t = (prev & ~m) | (d & m);
            g_ss22.czram[bank][unit * 2] = (uint16_t)(t >> 16);
            g_ss22.czram[bank][unit * 2 + 1] = (uint16_t)t;
        }
    }
}

/* spotram: 0x860000 address, 0x860002 write, 0x860004 read, 0x860006 enable (u16 handlers) */
static uint32_t spot_h(uint32_t unit)
{
    if (unit == 2) {
        uint16_t r = g_ss22.spotram[g_ss22.spot_addr >> 1 & 0x7FF];
        g_ss22.spot_addr = (uint16_t)(g_ss22.spot_addr + 2);
        return r;
    }
    return 0;
}
static void spot_wh(uint32_t unit, uint32_t d, uint32_t m)
{
    switch (unit) {
    case 0: g_ss22.spot_addr = (uint16_t)((g_ss22.spot_addr & ~m) | (d & m)); break;
    case 1: { uint16_t *p = &g_ss22.spotram[g_ss22.spot_addr >> 1 & 0x7FF]; *p = (uint16_t)((*p & ~m) | (d & m)); g_ss22.spot_addr = (uint16_t)(g_ss22.spot_addr + 2); break; }
    case 3: g_ss22.spot_enable = (uint16_t)((g_ss22.spot_enable & ~m) | (d & m)); break;
    default: break;
    }
}

static uint32_t tmattr_h(uint32_t unit)
{
    unit &= 7;
    if (unit == 5) return 0x8000;                           /* "current scanline?" */
    if (unit == 7) return 0;
    return g_ss22.tilemapattr[unit];
}
static void tmattr_wh(uint32_t unit, uint32_t d, uint32_t m)
{
    unit &= 7;
    g_ss22.tilemapattr[unit] = (uint16_t)((g_ss22.tilemapattr[unit] & ~m) | (d & m));
}

/* namcos22s_vics_control_r/w: 32-bit registers; reg 0 reads 0, the four size regs read without their busy bit */
static uint32_t vicsctl_h(uint32_t unit)
{
    uint32_t r = g_ss22.vics_ctl[unit & 0x1F];
    switch ((unit & 0x1F) * 4) {
    case 0x00: return 0;
    case 0x40: case 0x50: case 0x60: case 0x70: return r & 0x7FFFFFFFu;
    default: return r;
    }
}
static void vicsctl_wh(uint32_t unit, uint32_t d, uint32_t m)
{
    unit &= 0x1F;
    g_ss22.vics_ctl[unit] = (g_ss22.vics_ctl[unit] & ~m) | (d & m);
}

/* namcos22s_chipselect_w: C304 / C399 enable bits, written many times during boot */
static void chipsel_wh(uint32_t unit, uint32_t d, uint32_t m)
{
    (void)unit;
    if (m & 0x00FF0000u) g_ss22.chipselect = d >> 16;
    else if (m & 0xFF000000u) g_ss22.chipselect = d >> 24;
}

static uint32_t syscon_h(uint32_t unit) { return g_ss22.syscon[unit & 0x1F]; }
static void syscon_wh(uint32_t unit, uint32_t d, uint32_t m) { (void)m; ss22_hw_syscon_w(unit & 0x1F, (uint8_t)d); }

/* polygon RAM (namcos22_dspram_r/w): 32-bit words, only d0-23 connected, stored sign-extended */
static uint32_t poly_h(uint32_t unit) { return g_ss22.poly[unit & (SS22_POLY_WORDS - 1)] | 0xFF000000u; }
static void poly_wh(uint32_t unit, uint32_t d, uint32_t m)
{
    unit &= SS22_POLY_WORDS - 1;
    if (m & 0x00FF0000u) { m |= 0xFF000000u; d = (uint32_t)((int32_t)(d << 8) >> 8); }   /* signed24 */
    g_ss22.poly[unit] = (g_ss22.poly[unit] & ~m) | (d & m);
}

/* the EEPROM: an 8-bit device on lanes D31-D24 and D15-D8 (umask32 0xff00ff00): byte n of the
 * device is at the EVEN address 0x460000 + 2n, and the odd lanes are open bus */
static uint32_t eeprom_read(uint32_t off, int size)
{
    uint32_t v = 0;
    for (int i = 0; i < size; i++) {
        uint32_t p = off + (uint32_t)i;
        v = v << 8 | ((p & 1) ? 0u : g_ss22.eeprom[(p >> 1) & (SS22_EEPROM_SIZE - 1)]);
    }
    return v;
}
static void eeprom_write(uint32_t off, int size, uint32_t v)
{
    for (int i = 0; i < size; i++) {
        uint32_t p = off + (uint32_t)i;
        if (!(p & 1)) g_ss22.eeprom[(p >> 1) & (SS22_EEPROM_SIZE - 1)] = (uint8_t)(v >> ((size - 1 - i) * 8));
    }
}

#define IN(base, len) (a >= (base) && a < (base) + (len))

uint32_t rr_read(uint32_t a, int size)
{
    a &= 0xFFFFFFu;
    if (a + (uint32_t)size <= SS22_ROM_SIZE) return be_rd(g_ss22.rom, a, size);
    if (a >= 0xE00000u) {
        if (a - 0xE00000u + (uint32_t)size <= SS22_WRAM_SIZE) return be_rd(g_ss22.wram, a - 0xE00000u, size);
    } else switch (a >> 16) {
    case 0x40: if (IN(0x400000u, 0x20)) return dev_read(a - 0x400000u, size, 2, keycus_h); break;
    case 0x41: if (IN(0x410000u, SS22_SCI_SIZE)) return be_rd(g_ss22.sci, a - 0x410000u, size); break;
    case 0x42: if (IN(0x420000u, 0x10)) return dev_read(a - 0x420000u, size, 2, sci_h); break;
    case 0x44: if (IN(0x440000u, 4)) { uint32_t d = ss22_hw_dsw(); return size == 4 ? d : size == 2 ? (d >> ((a & 2) ? 0 : 16)) & 0xFFFF : (d >> ((3 - (a & 3)) * 8)) & 0xFF; } break;
    case 0x45: if (IN(0x450008u, 4)) return dev_read(a - 0x450008u, size, 2, portbit_h); break;
    case 0x46: if (IN(0x460000u, 0x4000)) return eeprom_read(a - 0x460000u, size); break;
    case 0x70: if (IN(0x700000u, 0x20)) return dev_read(a - 0x700000u, size, 1, syscon_h); break;
    case 0x81:
        if (IN(0x810000u, 0x10)) return dev_read(a - 0x810000u, size, 2, czattr_h);
        if (IN(0x810200u, 0x200)) return dev_read(a - 0x810200u, size, 4, czram_h);
        break;
    case 0x82: case 0x83:
        if (IN(0x824000u, SS22_MIXER_SIZE)) return be_rd(g_ss22.mixer, a - 0x824000u, size);
        if (IN(0x828000u, SS22_PAL_SIZE)) return be_rd(g_ss22.pal, a - 0x828000u, size);
        break;
    case 0x86: if (IN(0x860000u, 8)) return dev_read(a - 0x860000u, size, 2, spot_h); break;
    case 0x88: case 0x89:
        if (IN(0x89E000u, SS22_TEXT_SIZE)) return be_rd(g_ss22.text, a - 0x89E000u, size);   /* over the CGRAM tail */
        if (IN(0x880000u, SS22_CGRAM_SIZE)) return be_rd(g_ss22.cgram, a - 0x880000u, size);
        break;
    case 0x8A: if (IN(0x8A0000u, 0x10)) return dev_read(a - 0x8A0000u, size, 2, tmattr_h); break;
    case 0x90: if (IN(0x900000u, SS22_VICS_SIZE)) return be_rd(g_ss22.vics, a - 0x900000u, size); break;
    case 0x94: if (IN(0x940000u, 0x80)) return dev_read(a - 0x940000u, size, 4, vicsctl_h); break;
    case 0x98: case 0x99: case 0x9A: if (IN(0x980000u, SS22_SPRITE_SIZE)) return be_rd(g_ss22.sprite, a - 0x980000u, size); break;
    case 0xA0: if (IN(0xA04000u, SS22_SHARED_SIZE)) return be_rd(g_ss22.shared, a - 0xA04000u, size); break;
    case 0xC0: case 0xC1: return dev_read(a - 0xC00000u, size, 4, poly_h);
    default: break;
    }
    g_ss22.n_unmapped++;
    complain("unmapped read", a, size);
    return 0;
}

void rr_write(uint32_t a, int size, uint32_t v)
{
    a &= 0xFFFFFFu;
    if (a < SS22_ROM_SIZE) { g_ss22.n_romwrite++; complain("ROM write", a, size); return; }
    if (a >= 0xE00000u) {
        if (a - 0xE00000u + (uint32_t)size <= SS22_WRAM_SIZE) { be_wr(g_ss22.wram, a - 0xE00000u, size, v); return; }
    } else switch (a >> 16) {
    case 0x40: if (IN(0x400000u, 0x20)) return; break;                              /* keycus_w: ignored */
    case 0x41: if (IN(0x410000u, SS22_SCI_SIZE)) { be_wr(g_ss22.sci, a - 0x410000u, size, v); return; } break;
    case 0x42: if (IN(0x420000u, 0x10)) return; break;                              /* namcos22_sci_w: nothing */
    case 0x43: if (IN(0x430000u, 4)) return; break;                                 /* cpuleds */
    case 0x45: if (IN(0x450008u, 4)) { dev_write(a - 0x450008u, size, v, 2, portbit_wh); return; } break;
    case 0x46: if (IN(0x460000u, 0x4000)) { eeprom_write(a - 0x460000u, size, v); return; } break;
    case 0x70: if (IN(0x700000u, 0x20)) { dev_write(a - 0x700000u, size, v, 1, syscon_wh); return; } break;
    case 0x80: if (IN(0x800000u, 4)) { dev_write(a - 0x800000u, size, v, 4, chipsel_wh); return; } break;
    case 0x81:
        if (IN(0x810000u, 0x10)) { dev_write(a - 0x810000u, size, v, 2, czattr_wh); return; }
        if (IN(0x810200u, 0x200)) { dev_write(a - 0x810200u, size, v, 4, czram_wh); return; }
        break;
    case 0x82: case 0x83:
        if (IN(0x820000u, 0x300)) return;                                           /* nopw */
        if (IN(0x824000u, SS22_MIXER_SIZE)) { be_wr(g_ss22.mixer, a - 0x824000u, size, v); return; }
        if (IN(0x828000u, SS22_PAL_SIZE)) { be_wr(g_ss22.pal, a - 0x828000u, size, v); return; }
        break;
    case 0x86: if (IN(0x860000u, 8)) { dev_write(a - 0x860000u, size, v, 2, spot_wh); return; } break;
    case 0x88: case 0x89:
        if (IN(0x89E000u, SS22_TEXT_SIZE)) {                                          /* namcos22_textram_w: text RAM and the CGRAM tail */
            be_wr(g_ss22.text, a - 0x89E000u, size, v);
            return;
        }
        if (IN(0x880000u, SS22_CGRAM_SIZE)) { be_wr(g_ss22.cgram, a - 0x880000u, size, v); return; }
        break;
    case 0x8A: if (IN(0x8A0000u, 0x10)) { dev_write(a - 0x8A0000u, size, v, 2, tmattr_wh); return; } break;
    case 0x90: if (IN(0x900000u, SS22_VICS_SIZE)) { be_wr(g_ss22.vics, a - 0x900000u, size, v); return; } break;
    case 0x94: if (IN(0x940000u, 0x80)) { dev_write(a - 0x940000u, size, v, 4, vicsctl_wh); return; } break;
    case 0x98: case 0x99: case 0x9A: if (IN(0x980000u, SS22_SPRITE_SIZE)) { be_wr(g_ss22.sprite, a - 0x980000u, size, v); return; } break;
    case 0xA0: if (IN(0xA04000u, SS22_SHARED_SIZE)) { if (g_ss22_mbox && a - 0xA04000u < 0x200) ss22_mbox_log(a - 0xA04000u, size, v); be_wr(g_ss22.shared, a - 0xA04000u, size, v); return; } break;
    case 0xC0: case 0xC1: dev_write(a - 0xC00000u, size, v, 4, poly_wh); return;
    default: break;
    }
    g_ss22.n_unmapped++;
    complain("unmapped write", a, size);
}

/* ---- the devices ------------------------------------------------------------------------------------------- */
ss22_hw_t g_hw;

static FILE *keycus_replay;
static int keycus_forced; static uint16_t keycus_forced_v;
/* trace oracle: the value MAME's keycus answered with at the read that follows (the game's env module) */
void ss22_hw_keycus_force(uint32_t v) { keycus_forced = 1; keycus_forced_v = (uint16_t)v; }

static uint32_t ss22_hw_keycus_r(uint32_t unit)
{
    if (unit == cfg->keycus_unit) return cfg->keycus_value;
    if (keycus_forced) { keycus_forced = 0; return g_hw.keycus_rng = keycus_forced_v; }
    unsigned v;
    if (keycus_replay && fscanf(keycus_replay, "%x", &v) == 1) return g_hw.keycus_rng = (uint16_t)v;
    uint16_t old = g_hw.keycus_rng;
    do { g_hw.lcg = g_hw.lcg * 1103515245u + 12345u; g_hw.keycus_rng = (uint16_t)(g_hw.lcg >> 16); }
    while (g_hw.keycus_rng == old);                     /* never the same twice in a row */
    return g_hw.keycus_rng;
}

static uint32_t ss22_hw_dsw(void) { return g_hw.dsw; }

static uint32_t ss22_hw_portbit_r(uint32_t unit)
{
    unit &= 1;
    uint32_t ret = g_hw.portbits[unit] & 1;
    g_hw.portbits[unit] = (uint16_t)(g_hw.portbits[unit] >> 1 | 0x8000);
    return ret;
}
static void ss22_hw_portbit_w(uint32_t unit) { g_hw.portbits[unit & 1] = 0xFFFF; }

static void irq_level(uint32_t line, uint8_t data)
{
    uint32_t bit = 1u << line;
    g_hw.irq_enabled &= ~bit;
    if (data & 7) g_hw.irq_enabled |= bit;              /* Super 22: enabled by a non-zero level */
    if ((g_hw.irq_state & bit) && !(g_hw.irq_enabled & bit)) g_hw.irq_state &= ~bit;
}

static void ss22_hw_syscon_w(uint32_t off, uint8_t data)
{
    if (off <= 3) irq_level(off, data);
    else if (off <= 7) g_hw.irq_state &= ~(1u << (off - 4));
    else if (off == 0x16) { g_hw.mcu_run = data != 0; if (g_ss22_snd_set_run) g_ss22_snd_set_run(g_hw.mcu_run); }
    else if (off == 0x1C) { g_hw.dsp_ctrl = data; if (g_ss22_dsp_control) g_ss22_dsp_control(data); }
    g_ss22.syscon[off] = data;
}

static FILE *mbox_f;
static void ss22_mbox_log(uint32_t off, int size, uint32_t v)
{
    extern uint32_t rr_frame;
    fprintf(mbox_f, "%u %x %d %x\n", rr_frame, off, size, v);
}

void ss22_board_use(const ss22_board_cfg *c) { cfg = c; }
bool ss22_load_program(const char *rom_dir) { return cfg->load_program(rom_dir); }

bool ss22_hw_init(const char *rom_dir)
{
    char v[64];
    snprintf(v, sizeof v, "%s_MBOXLOG", cfg->tag);
    { const char *m = getenv(v); if (m && (mbox_f = fopen(m, "w"))) g_ss22_mbox = 1; }
    memset(&g_hw, 0, sizeof g_hw);
    g_hw.lcg = 0x2545F491u;
    g_hw.dsw = 0xFFFFFFFFu;
    g_hw.portbits[0] = g_hw.portbits[1] = 0xFFFF;
    cfg->load_eeprom(rom_dir);
    snprintf(v, sizeof v, "%s_KEYCUS_FILE", cfg->tag);
    const char *kf = getenv(v);                          /* replay MAME's random keycus reads */
    if (kf && !(keycus_replay = fopen(kf, "r"))) fprintf(stderr, "[%s] cannot open %s\n", cfg->tag, kf);
    return true;
}

/* the frame's vblank: raise the vblank IRQ line (syscon line 0) if the game enabled it */
void ss22_hw_vblank(void)
{
    if (g_hw.irq_enabled & 1u) g_hw.irq_state |= 1u;
}

/* the scanline IRQ (syscon line 1, posirq) */
void ss22_hw_scanline(void)
{
    if (g_hw.irq_enabled & 2u) g_hw.irq_state |= 2u;
}

/* the highest pending, enabled IRQ as a 68K level (0 = none) */
int ss22_hw_irq_level(void)
{
    int best = 0;
    for (int line = 0; line < 4; line++)
        if (g_hw.irq_state & (1u << line)) {
            int lvl = g_ss22.syscon[line] & 7;
            if (lvl > best) best = lvl;
        }
    return best;
}

/* Interactive runs keep the machine's EEPROM (the options, the lap records, the ranking) in a file beside the game, as MAME's nvram does:
 * loaded at boot when it holds exactly one image, rewritten (temp file + rename) whenever the game has changed it. Headless runs never touch
 * it, so a player's records cannot change what a gate measures. A file the game does not accept is formatted by the game itself. */
static char    nv_path[512];
static uint8_t nv_saved[SS22_EEPROM_SIZE];
static bool    nv_on;

void ss22_eeprom_persist(const char *path)
{
    snprintf(nv_path, sizeof nv_path, "%s", path);
    FILE *f = fopen(nv_path, "rb");
    if (f) {
        uint8_t buf[SS22_EEPROM_SIZE + 1];
        size_t n = fread(buf, 1, sizeof buf, f);
        fclose(f);
        if (n == SS22_EEPROM_SIZE) { memcpy(g_ss22.eeprom, buf, SS22_EEPROM_SIZE); fprintf(stderr, "[%s] EEPROM loaded from %s\n", cfg->tag, nv_path); }
        else fprintf(stderr, "[%s] %s is not an EEPROM image (%zu bytes) -- ignored\n", cfg->tag, nv_path, n);
    }
    memcpy(nv_saved, g_ss22.eeprom, SS22_EEPROM_SIZE);
    nv_on = true;
}

void ss22_eeprom_save(void)
{
    if (!nv_on || !memcmp(nv_saved, g_ss22.eeprom, SS22_EEPROM_SIZE)) return;
    char tmp[sizeof nv_path + 8];
    snprintf(tmp, sizeof tmp, "%s.tmp", nv_path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return;
    bool ok = fwrite(g_ss22.eeprom, 1, SS22_EEPROM_SIZE, f) == SS22_EEPROM_SIZE;
    ok = (fclose(f) == 0) && ok;
    if (!ok) { remove(tmp); return; }
#ifdef _WIN32
    remove(nv_path);
#endif
    if (rename(tmp, nv_path) == 0) memcpy(nv_saved, g_ss22.eeprom, SS22_EEPROM_SIZE);
}
