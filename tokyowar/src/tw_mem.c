/*
 * tw_mem.c -- see include/tw_mem.h. Super System 22 map from MAME namcos22.cpp
 * (namcos22s_am) and the handlers in namcos22.cpp / namcos22_v.cpp.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tw_mem.h"

tw_sys_t g_tw;

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
    fprintf(stderr, "[TWMEM] %s %d-byte access at 0x%06X\n", what, size, a);
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
static uint32_t keycus_h(uint32_t unit) { return tw_hw_keycus_r(unit); }

static uint32_t sci_h(uint32_t unit) { return unit == 0 ? 0x0004 : 0; }          /* namcos22_sci_r */

static uint32_t portbit_h(uint32_t unit) { return tw_hw_portbit_r(unit); }
static void     portbit_wh(uint32_t unit, uint32_t d, uint32_t m) { (void)d; (void)m; tw_hw_portbit_w(unit); }

static uint32_t czattr_h(uint32_t unit) { return g_tw.czattr[unit & 7]; }
static void czattr_wh(uint32_t unit, uint32_t d, uint32_t m)
{
    unit &= 7;
    g_tw.czattr[unit] = (uint16_t)((g_tw.czattr[unit] & ~m) | (d & m));
}

/* namcos22s_czram_r/w: 32-bit handler over four banks of 16-bit words */
static uint32_t czram_h(uint32_t unit)
{
    int bank = g_tw.czattr[5] & 3;
    unit &= 0x7F;
    return (uint32_t)g_tw.czram[bank][unit * 2] << 16 | g_tw.czram[bank][unit * 2 + 1];
}
static void czram_wh(uint32_t unit, uint32_t d, uint32_t m)
{
    unit &= 0x7F;
    for (int bank = 0; bank < 4; bank++) {
        if (~g_tw.czattr[4] >> (bank * 4) & 1) {            /* write enable when the bit is 0 */
            uint32_t prev = (uint32_t)g_tw.czram[bank][unit * 2] << 16 | g_tw.czram[bank][unit * 2 + 1];
            uint32_t t = (prev & ~m) | (d & m);
            g_tw.czram[bank][unit * 2] = (uint16_t)(t >> 16);
            g_tw.czram[bank][unit * 2 + 1] = (uint16_t)t;
        }
    }
}

/* spotram: 0x860000 address, 0x860002 write, 0x860004 read, 0x860006 enable (u16 handlers) */
static uint32_t spot_h(uint32_t unit)
{
    if (unit == 2) {
        uint16_t r = g_tw.spotram[g_tw.spot_addr >> 1 & 0x7FF];
        g_tw.spot_addr = (uint16_t)(g_tw.spot_addr + 2);
        return r;
    }
    return 0;
}
static void spot_wh(uint32_t unit, uint32_t d, uint32_t m)
{
    switch (unit) {
    case 0: g_tw.spot_addr = (uint16_t)((g_tw.spot_addr & ~m) | (d & m)); break;
    case 1: { uint16_t *p = &g_tw.spotram[g_tw.spot_addr >> 1 & 0x7FF]; *p = (uint16_t)((*p & ~m) | (d & m)); g_tw.spot_addr = (uint16_t)(g_tw.spot_addr + 2); break; }
    case 3: g_tw.spot_enable = (uint16_t)((g_tw.spot_enable & ~m) | (d & m)); break;
    default: break;
    }
}

static uint32_t tmattr_h(uint32_t unit)
{
    unit &= 7;
    if (unit == 5) return 0x8000;                           /* "current scanline?" */
    if (unit == 7) return 0;
    return g_tw.tilemapattr[unit];
}
static void tmattr_wh(uint32_t unit, uint32_t d, uint32_t m)
{
    unit &= 7;
    g_tw.tilemapattr[unit] = (uint16_t)((g_tw.tilemapattr[unit] & ~m) | (d & m));
}

/* namcos22s_vics_control_r/w: 32-bit registers; reg 0 reads 0, the four size regs read without their busy bit */
static uint32_t vicsctl_h(uint32_t unit)
{
    uint32_t r = g_tw.vics_ctl[unit & 0x1F];
    switch ((unit & 0x1F) * 4) {
    case 0x00: return 0;
    case 0x40: case 0x50: case 0x60: case 0x70: return r & 0x7FFFFFFFu;
    default: return r;
    }
}
static void vicsctl_wh(uint32_t unit, uint32_t d, uint32_t m)
{
    unit &= 0x1F;
    g_tw.vics_ctl[unit] = (g_tw.vics_ctl[unit] & ~m) | (d & m);
}

/* namcos22s_chipselect_w: C304 / C399 enable bits, written many times during boot */
static void chipsel_wh(uint32_t unit, uint32_t d, uint32_t m)
{
    (void)unit;
    if (m & 0x00FF0000u) g_tw.chipselect = d >> 16;
    else if (m & 0xFF000000u) g_tw.chipselect = d >> 24;
}

static uint32_t syscon_h(uint32_t unit) { return g_tw.syscon[unit & 0x1F]; }
static void syscon_wh(uint32_t unit, uint32_t d, uint32_t m) { (void)m; tw_hw_syscon_w(unit & 0x1F, (uint8_t)d); }

/* polygon RAM (namcos22_dspram_r/w): 32-bit words, only d0-23 connected, stored sign-extended */
static uint32_t poly_h(uint32_t unit) { return g_tw.poly[unit & (TW_POLY_WORDS - 1)] | 0xFF000000u; }
static void poly_wh(uint32_t unit, uint32_t d, uint32_t m)
{
    unit &= TW_POLY_WORDS - 1;
    if (m & 0x00FF0000u) { m |= 0xFF000000u; d = (uint32_t)((int32_t)(d << 8) >> 8); }   /* signed24 */
    g_tw.poly[unit] = (g_tw.poly[unit] & ~m) | (d & m);
}

/* the EEPROM: an 8-bit device on lanes D31-D24 and D15-D8 (umask32 0xff00ff00): byte n of the
 * device is at the EVEN address 0x460000 + 2n, and the odd lanes are open bus */
static uint32_t eeprom_read(uint32_t off, int size)
{
    uint32_t v = 0;
    for (int i = 0; i < size; i++) {
        uint32_t p = off + (uint32_t)i;
        v = v << 8 | ((p & 1) ? 0u : g_tw.eeprom[(p >> 1) & (TW_EEPROM_SIZE - 1)]);
    }
    return v;
}
static void eeprom_write(uint32_t off, int size, uint32_t v)
{
    for (int i = 0; i < size; i++) {
        uint32_t p = off + (uint32_t)i;
        if (!(p & 1)) g_tw.eeprom[(p >> 1) & (TW_EEPROM_SIZE - 1)] = (uint8_t)(v >> ((size - 1 - i) * 8));
    }
}

#define IN(base, len) (a >= (base) && a < (base) + (len))

uint32_t rr_read(uint32_t a, int size)
{
    a &= 0xFFFFFFu;
    if (a + (uint32_t)size <= TW_ROM_SIZE) return be_rd(g_tw.rom, a, size);
    if (a >= 0xE00000u) {
        if (a - 0xE00000u + (uint32_t)size <= TW_WRAM_SIZE) return be_rd(g_tw.wram, a - 0xE00000u, size);
    } else switch (a >> 16) {
    case 0x40: if (IN(0x400000u, 0x20)) return dev_read(a - 0x400000u, size, 2, keycus_h); break;
    case 0x41: if (IN(0x410000u, TW_SCI_SIZE)) return be_rd(g_tw.sci, a - 0x410000u, size); break;
    case 0x42: if (IN(0x420000u, 0x10)) return dev_read(a - 0x420000u, size, 2, sci_h); break;
    case 0x44: if (IN(0x440000u, 4)) { uint32_t d = tw_hw_dsw(); return size == 4 ? d : size == 2 ? (d >> ((a & 2) ? 0 : 16)) & 0xFFFF : (d >> ((3 - (a & 3)) * 8)) & 0xFF; } break;
    case 0x45: if (IN(0x450008u, 4)) return dev_read(a - 0x450008u, size, 2, portbit_h); break;
    case 0x46: if (IN(0x460000u, 0x4000)) return eeprom_read(a - 0x460000u, size); break;
    case 0x70: if (IN(0x700000u, 0x20)) return dev_read(a - 0x700000u, size, 1, syscon_h); break;
    case 0x81:
        if (IN(0x810000u, 0x10)) return dev_read(a - 0x810000u, size, 2, czattr_h);
        if (IN(0x810200u, 0x200)) return dev_read(a - 0x810200u, size, 4, czram_h);
        break;
    case 0x82: case 0x83:
        if (IN(0x824000u, TW_MIXER_SIZE)) return be_rd(g_tw.mixer, a - 0x824000u, size);
        if (IN(0x828000u, TW_PAL_SIZE)) return be_rd(g_tw.pal, a - 0x828000u, size);
        break;
    case 0x86: if (IN(0x860000u, 8)) return dev_read(a - 0x860000u, size, 2, spot_h); break;
    case 0x88: case 0x89:
        if (IN(0x89E000u, TW_TEXT_SIZE)) return be_rd(g_tw.text, a - 0x89E000u, size);   /* over the CGRAM tail */
        if (IN(0x880000u, TW_CGRAM_SIZE)) return be_rd(g_tw.cgram, a - 0x880000u, size);
        break;
    case 0x8A: if (IN(0x8A0000u, 0x10)) return dev_read(a - 0x8A0000u, size, 2, tmattr_h); break;
    case 0x90: if (IN(0x900000u, TW_VICS_SIZE)) return be_rd(g_tw.vics, a - 0x900000u, size); break;
    case 0x94: if (IN(0x940000u, 0x80)) return dev_read(a - 0x940000u, size, 4, vicsctl_h); break;
    case 0x98: case 0x99: case 0x9A: if (IN(0x980000u, TW_SPRITE_SIZE)) return be_rd(g_tw.sprite, a - 0x980000u, size); break;
    case 0xA0: if (IN(0xA04000u, TW_SHARED_SIZE)) return be_rd(g_tw.shared, a - 0xA04000u, size); break;
    case 0xC0: case 0xC1: return dev_read(a - 0xC00000u, size, 4, poly_h);
    default: break;
    }
    g_tw.n_unmapped++;
    complain("unmapped read", a, size);
    return 0;
}

void rr_write(uint32_t a, int size, uint32_t v)
{
    a &= 0xFFFFFFu;
    if (a < TW_ROM_SIZE) { g_tw.n_romwrite++; complain("ROM write", a, size); return; }
    if (a >= 0xE00000u) {
        if (a - 0xE00000u + (uint32_t)size <= TW_WRAM_SIZE) { be_wr(g_tw.wram, a - 0xE00000u, size, v); return; }
    } else switch (a >> 16) {
    case 0x40: if (IN(0x400000u, 0x20)) return; break;                              /* keycus_w: ignored */
    case 0x41: if (IN(0x410000u, TW_SCI_SIZE)) { be_wr(g_tw.sci, a - 0x410000u, size, v); return; } break;
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
        if (IN(0x824000u, TW_MIXER_SIZE)) { be_wr(g_tw.mixer, a - 0x824000u, size, v); return; }
        if (IN(0x828000u, TW_PAL_SIZE)) { be_wr(g_tw.pal, a - 0x828000u, size, v); return; }
        break;
    case 0x86: if (IN(0x860000u, 8)) { dev_write(a - 0x860000u, size, v, 2, spot_wh); return; } break;
    case 0x88: case 0x89:
        if (IN(0x89E000u, TW_TEXT_SIZE)) {                                          /* namcos22_textram_w: text RAM and the CGRAM tail */
            be_wr(g_tw.text, a - 0x89E000u, size, v);
            return;
        }
        if (IN(0x880000u, TW_CGRAM_SIZE)) { be_wr(g_tw.cgram, a - 0x880000u, size, v); return; }
        break;
    case 0x8A: if (IN(0x8A0000u, 0x10)) { dev_write(a - 0x8A0000u, size, v, 2, tmattr_wh); return; } break;
    case 0x90: if (IN(0x900000u, TW_VICS_SIZE)) { be_wr(g_tw.vics, a - 0x900000u, size, v); return; } break;
    case 0x94: if (IN(0x940000u, 0x80)) { dev_write(a - 0x940000u, size, v, 4, vicsctl_wh); return; } break;
    case 0x98: case 0x99: case 0x9A: if (IN(0x980000u, TW_SPRITE_SIZE)) { be_wr(g_tw.sprite, a - 0x980000u, size, v); return; } break;
    case 0xA0: if (IN(0xA04000u, TW_SHARED_SIZE)) { if (g_tw_mbox && a - 0xA04000u < 0x200) tw_mbox_log(a - 0xA04000u, size, v); be_wr(g_tw.shared, a - 0xA04000u, size, v); return; } break;
    case 0xC0: case 0xC1: dev_write(a - 0xC00000u, size, v, 4, poly_wh); return;
    default: break;
    }
    g_tw.n_unmapped++;
    complain("unmapped write", a, size);
}

/* ---- ROM images --------------------------------------------------------------------------- */
bool tw_load_program(const char *dir)
{
    /* MAME ROM_LOAD32_BYTE: tw2ver-a.4/.3/.2/.1 -> byte 0/1/2/3 of every long */
    static const char *lane[4] = { "tw2ver-a.4", "tw2ver-a.3", "tw2ver-a.2", "tw2ver-a.1" };
    for (int l = 0; l < 4; l++) {
        char p[1024]; snprintf(p, sizeof p, "%s/%s", dir, lane[l]);
        FILE *f = fopen(p, "rb");
        if (!f) { fprintf(stderr, "cannot open %s\n", p); return false; }
        for (uint32_t i = 0; i < TW_ROM_SIZE / 4; i++) {
            int c = fgetc(f);
            if (c == EOF) { fclose(f); fprintf(stderr, "short %s\n", p); return false; }
            g_tw.rom[i * 4 + (uint32_t)l] = (uint8_t)c;
        }
        fclose(f);
    }
    return true;
}

bool tw_load_eeprom(const char *dir)
{
    char p[1024]; snprintf(p, sizeof p, "%s/tokyowar_defaults.nv", dir);
    FILE *f = fopen(p, "rb");
    if (!f) { fprintf(stderr, "[TW] no %s -- EEPROM blank\n", p); return false; }
    size_t n = fread(g_tw.eeprom, 1, TW_EEPROM_SIZE, f);
    fclose(f);
    if (n != TW_EEPROM_SIZE) fprintf(stderr, "[TW] short EEPROM image (%zu bytes)\n", n);
    return n == TW_EEPROM_SIZE;
}
