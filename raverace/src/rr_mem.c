/*
 * rr_mem.c -- see include/rr_mem.h.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rr_mem.h"

rr_sys_t g_rr;

static rr_io_read_fn  s_io_r;
static rr_io_write_fn s_io_w;
void rr_set_io_hooks(rr_io_read_fn r, rr_io_write_fn w) { s_io_r = r; s_io_w = w; }

/* A big-endian byte region: returns the base and the byte offset, or NULL. */
static uint8_t *region(vaddr_t a, uint32_t *off, uint32_t *len, bool *ro)
{
    *ro = false;
    if (a < RR_ROM_SIZE) { *off = a; *len = RR_ROM_SIZE; *ro = true; return g_rr.rom; }
    uint32_t hi = a & 0xF8000000u;
    if (hi == 0x10000000u || hi == 0x18000000u) {            /* work RAM + mirror */
        uint32_t o = a & 0x07FFFFFFu;
        if (o < RR_WRAM_SIZE) { *off = o; *len = RR_WRAM_SIZE; return g_rr.wram; }
        return NULL;
    }
#define R(base, size, arr) \
    if (a >= (base) && a < (base) + (size)) { *off = a - (base); *len = (size); return (arr); }
    R(0x20010000u, RR_SCI_SIZE,    g_rr.sci)
    R(0x58000000u, RR_EEPROM_SIZE, g_rr.eeprom)
    R(0x60004000u, RR_SHARED_SIZE, g_rr.shared)
    R(0x90010000u, RR_CZRAM_SIZE,  g_rr.czram)
    R(0x90020000u, RR_MIXER_SIZE,  g_rr.mixer)
    R(0x90028000u, RR_PAL_SIZE,    g_rr.pal)
    R(0x9009E000u, RR_TEXT_SIZE,   g_rr.text)     /* before CGRAM: it overlaps its tail */
    R(0x90080000u, RR_CGRAM_SIZE,  g_rr.cgram)
    R(0x40000000u, 0x20,           g_rr.syscon)
    R(0x900A0000u, 0x10,           g_rr.tilemapattr)
    R(0x20020000u, 0x10,           g_rr.sci_reg)
#undef R
    return NULL;
}

static void complain(const char *what, vaddr_t a, int size)
{
    static uint32_t seen[256]; static int nseen;
    uint32_t page = a & ~0xFFFu;
    for (int i = 0; i < nseen; i++) if (seen[i] == page) return;
    if (nseen < 256) seen[nseen++] = page;
    fprintf(stderr, "[RRMEM] %s %d-byte access at 0x%08X\n", what, size, a);
}

/* Polygon RAM: 32-bit words, 24 bits connected, stored sign-extended the way
 * MAME's namcos22_dspram_w does (the master DSP port reads it that way). */
static bool poly_access(vaddr_t a, int size, bool wr, uint32_t *v)
{
    if (a < 0x70000000u || a >= 0x70000000u + RR_POLY_WORDS * 4) return false;
    uint32_t idx = (a - 0x70000000u) >> 2, sh = (3 - (a & 3)) * 8;
    uint32_t word = g_rr.poly[idx];
    if (!wr) {
        uint32_t full = word | 0xFF000000u;                  /* only d0-23 connected */
        if (size == 4) *v = full;
        else if (size == 2) *v = (full >> ((a & 2) ? 0 : 16)) & 0xFFFF;
        else *v = (full >> sh) & 0xFF;
        return true;
    }
    uint32_t nv;
    if (size == 4) nv = *v;
    else if (size == 2) { uint32_t s2 = (a & 2) ? 0 : 16; nv = (word & ~(0xFFFFu << s2)) | ((*v & 0xFFFF) << s2); }
    else nv = (word & ~(0xFFu << sh)) | ((*v & 0xFF) << sh);
    g_rr.poly[idx] = (uint32_t)((int32_t)(nv << 8) >> 8);    /* signed24 */
    return true;
}

extern int rd_journal_on, rd_read_log, rd_quiet, rd_io_written, rd_io_touched;
void rd_io_log(vaddr_t a, int size, uint32_t v);
extern int rd_ior_replay;
void rd_ior_log(vaddr_t a, int size, uint32_t v);
uint32_t rd_ior_serve(vaddr_t a, int size);
void rd_read_byte(vaddr_t a, uint8_t *p);
void rd_dict_byte(uint8_t v);
uint32_t rr_read(vaddr_t a, int size)
{
    uint32_t v = 0;
    if (rd_journal_on && s_io_r && !(a >= 0x70000000u && a < 0x70000000u + RR_POLY_WORDS * 4)) {
        uint32_t o_, l_; bool r_;
        if (!region(a, &o_, &l_, &r_)) {           /* an I/O read during a check (src/rd) */
            if (rd_ior_replay) return rd_ior_serve(a, size);   /* the readable run: the lifted run's value */
            if (s_io_r(a, size, &v)) {                          /* the lifted run: real, once, logged */
                if (rd_io_written) rd_io_touched = 1;           /* a captured write came first */
                rd_ior_log(a, size, v);
                return v;
            }
            rd_ior_log(a, size, 0);        /* unmapped (a fuzzed pointer): logged too, reads 0 below */
        }
    }
    if (s_io_r && s_io_r(a, size, &v)) return v;
    if (poly_access(a, size, false, &v)) return v;
    uint32_t off, len; bool ro;
    uint8_t *m = region(a, &off, &len, &ro);
    if (!m || off + (uint32_t)size > len) { g_rr.n_unmapped++; if (!rd_quiet) complain("unmapped read", a, size); return 0; }
    if (rd_read_log) for (int i = 0; i < size; i++) {
        if (!ro) rd_read_byte(a + (uint32_t)i, &m[off + i]);
        else rd_dict_byte(m[off + i]);                 /* ROM values: the fuzzer's dictionary */
    }
    for (int i = 0; i < size; i++) v = (v << 8) | m[off + i];
    return v;
}

#ifdef RR_TRACE
/* RR_WATCH=<addr>: every write covering that byte, with the writing PC (trace build only) */
extern uint32_t rr_trace_pc;
static void watch(vaddr_t a, int size, uint32_t v)
{
    static long w = -2;
    if (w == -2) { const char *e = getenv("RR_WATCH"); w = e ? strtol(e, NULL, 0) : -1; }
    if (w >= 0 && (uint32_t)w >= a && (uint32_t)w < a + (uint32_t)size)
        fprintf(stderr, "[WATCH] %08X <- %0*X (size %d) at PC %06X\n", (unsigned)a, size * 2, (unsigned)v, size, (unsigned)rr_trace_pc);
}
#endif
/* Readable-C checker journal (src/rd/rd_core.c): while on, every byte a write
 * changes is recorded with its old value so the write can be rolled back and
 * compared; an access the journal cannot undo (I/O hooks, polygon RAM) marks
 * the call unverifiable instead. */
int  rd_journal_on;
void rd_journal_byte(vaddr_t a, uint8_t *p);

void rr_write(vaddr_t a, int size, uint32_t v)
{
#ifdef RR_TRACE
    watch(a, size, v);
#endif
    if (rd_journal_on && s_io_w && !(a >= 0x70000000u && a < 0x70000000u + RR_POLY_WORDS * 4)) {
        uint32_t o_, l_; bool r_;
        if (!region(a, &o_, &l_, &r_)) { rd_io_log(a, size, v); return; }   /* src/rd: captured, replayed later */
    }
    if (s_io_w && s_io_w(a, size, v)) return;
    if (rd_journal_on && a >= 0x70000000u && a < 0x70000000u + RR_POLY_WORDS * 4) {   /* journal the word's 4 host bytes */
        uint32_t base = a & ~3u;
        uint8_t *w = (uint8_t *)&g_rr.poly[(base - 0x70000000u) >> 2];
        for (int i = 0; i < 4; i++) rd_journal_byte(base + (uint32_t)i, w + i);
    }
    if (poly_access(a, size, true, &v)) return;
    uint32_t off, len; bool ro;
    uint8_t *m = region(a, &off, &len, &ro);
    if (!m || off + (uint32_t)size > len) { g_rr.n_unmapped++; if (!rd_quiet) complain("unmapped write", a, size); return; }
    if (ro) { g_rr.n_romwrite++; complain("ROM write", a, size); return; }
    if (rd_journal_on) for (int i = 0; i < size; i++) rd_journal_byte(a + (uint32_t)i, &m[off + i]);
    for (int i = size - 1; i >= 0; i--) { m[off + i] = (uint8_t)v; v >>= 8; }
}

bool rr_load_program(const char *dir)
{
    /* MAME ROM_LOAD32_BYTE offsets: uub 0, umb 1, lmb 2, llb 3 */
    static const char *lane[4] = { "rv2_prguub.6d", "rv2_prgumb.8d", "rv2_prglmb.2d", "rv2_prgllb.4d" };
    for (int l = 0; l < 4; l++) {
        char p[1024]; snprintf(p, sizeof p, "%s/%s", dir, lane[l]);
        FILE *f = fopen(p, "rb");
        if (!f) { fprintf(stderr, "cannot open %s\n", p); return false; }
        for (uint32_t i = 0; i < RR_ROM_SIZE / 4; i++) {
            int c = fgetc(f);
            if (c == EOF) { fclose(f); fprintf(stderr, "short %s\n", p); return false; }
            g_rr.rom[i * 4 + l] = (uint8_t)c;
        }
        fclose(f);
    }
    return true;
}
