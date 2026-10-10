/*
 * tc2_rom.c -- THE PLAYER'S ROMS: Time Crisis II's chips (MAME's timecrs2 set, the US TSS3 Ver. B program) and the program images made
 * from them at start-up.
 *   - extracted/ is filled from the player's own timecrs2.zip on the first run (src/eng/romzip.c: by name, size and the zip's own CRC),
 *     found in the current folder, roms/, or beside the executable.
 *   - the boot image (tss3verb.2 / .1 byte-interleaved), the sub-CPU's program (tss1vera.3, word-swapped) and the I/O board's
 *     (tssioprog.ic3) are made here in memory -- what tools/make_prog.py writes as boot.bin / sub.bin / io.bin for the translators.
 * The ROM supplies data only: the programs on it run as translated C (gen/), nothing here executes ROM code.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "eng/romzip.h"

static const eng_rom_t chips[] = {
    { "tss3verb.1", 0x200000, NULL, NULL, 0x6E3F232B }, { "tss3verb.2", 0x200000, NULL, NULL, 0xC7BE691F },
    { "tss1vera.3", 0x80000, NULL, NULL, 0x41E41994 },  { "tssioprog.ic3", 0x40000, NULL, NULL, 0xEDAD4538 },
    { "tss1ccrh.7e", 0x200000, NULL, NULL, 0xF998DE1A }, { "tss1ccrl.7f", 0x400000, NULL, NULL, 0x3A325FE7 },
    { "tss1cgll.4m", 0x800000, NULL, NULL, 0x18433AAA }, { "tss1cglm.4k", 0x800000, NULL, NULL, 0x669974C2 },
    { "tss1cgum.4j", 0x800000, NULL, NULL, 0xC22739E1 }, { "tss1cguu.4f", 0x800000, NULL, NULL, 0x76924E04 },
    { "tss1mtah.2j", 0x800000, NULL, NULL, 0x697C26ED }, { "tss1mtal.2h", 0x800000, NULL, NULL, 0xBFC79190 },
    { "tss1mtbh.2m", 0x800000, NULL, NULL, 0x82582776 }, { "tss1mtbl.2f", 0x800000, NULL, NULL, 0xE648BEA4 },
    { "tss1pt0h.7a", 0x400000, NULL, NULL, 0xCDBE0BA8 }, { "tss1pt0l.7c", 0x400000, NULL, NULL, 0x896F0FB4 },
    { "tss1pt1h.5a", 0x400000, NULL, NULL, 0x63647596 }, { "tss1pt1l.5c", 0x400000, NULL, NULL, 0x5A09921F },
    { "tss1pt2h.4a", 0x400000, NULL, NULL, 0x9B06E22D }, { "tss1pt2l.4c", 0x400000, NULL, NULL, 0x4B230D79 },
    { "tss1waveh.2a", 0x800000, NULL, NULL, 0x5C8758B4 }, { "tss1wavel.2c", 0x800000, NULL, NULL, 0xDEAEAD26 },
};
#define NCHIPS ((int)(sizeof chips / sizeof chips[0]))

char tc2_rom_error[1024];   /* the player's message when it fails (a window shows it: src/tc2_main.c) */
/* the ROM folder complete, from the player's zip if need be; 0 = not, with a message for the player */
int tc2_rom_setup(const char *dir, const char *exe_dir)
{
    const char *miss = eng_romzip_missing(dir, chips, NCHIPS);
    if (!miss) return 1;
    static const char *const zips[] = { "timecrs2.zip", "namco_tssio.zip" };   /* a split MAME set keeps the I/O board's program in its device zip */
    char err[512] = "";
    fprintf(stderr, "[ROM] %s/%s missing: looking for timecrs2.zip (here, roms/, beside the program)\n", dir, miss);
    if (eng_romzip_autosetup(dir, exe_dir, zips, 2, chips, NCHIPS, err, sizeof err)) { fprintf(stderr, "[ROM] unpacked timecrs2.zip into %s/\n", dir); return 1; }
    fprintf(stderr, "[ROM] %s\n[ROM] Put MAME's timecrs2.zip (Time Crisis II, the US TSS3 Ver. B set) in the roms/ folder beside the game.\n", err);
    snprintf(tc2_rom_error, sizeof tc2_rom_error, "Time Crisis 2 needs its ROM file (no game data is included).\n\nPut MAME's timecrs2.zip (Time Crisis II, "
             "US TSS3 Ver. B) in the \"roms\" folder next to the game, then start it again.\n\n(%s)", err);
    return 0;
}

static int rd(const char *dir, const char *name, uint8_t *dst, size_t n)
{
    char p[1024]; snprintf(p, sizeof p, "%s/%s", dir, name);
    FILE *f = fopen(p, "rb"); if (!f) { fprintf(stderr, "[ROM] cannot read %s\n", p); return 0; }
    const size_t got = fread(dst, 1, n, f); fclose(f);
    if (got != n) { fprintf(stderr, "[ROM] %s: %zu of %zu bytes\n", p, got, n); return 0; }
    return 1;
}
/* the boot image (4 MB at 0x1FC00000): tss3verb.2 and .1 byte-interleaved, .2 first */
int tc2_rom_boot(const char *dir, uint8_t *boot)
{
    uint8_t *a = malloc(0x200000), *b = malloc(0x200000);
    const int ok = a && b && rd(dir, "tss3verb.1", a, 0x200000) && rd(dir, "tss3verb.2", b, 0x200000);
    if (ok) for (uint32_t k = 0; k < 0x200000; k++) { boot[2 * k] = b[k]; boot[2 * k + 1] = a[k]; }
    free(a); free(b);
    return ok;
}
/* the sub-CPU's (H8/3002) program: tss1vera.3 is stored word-swapped (MAME ROM_LOAD16_WORD_SWAP) */
int tc2_rom_sub(const char *dir, uint8_t *dst, size_t n)
{
    if (n < 0x80000 || !rd(dir, "tss1vera.3", dst, 0x80000)) return 0;
    for (uint32_t k = 0; k < 0x80000; k += 2) { const uint8_t t = dst[k]; dst[k] = dst[k + 1]; dst[k + 1] = t; }
    return 1;
}
/* the I/O board's (H8/3334) program, as it is: its first n bytes (the H8/3334 maps 16 KB of it) */
int tc2_rom_io(const char *dir, uint8_t *dst, size_t n)
{
    uint8_t *all = malloc(0x40000);
    const int ok = all && n <= 0x40000 && rd(dir, "tssioprog.ic3", all, 0x40000);
    if (ok) memcpy(dst, all, n);
    free(all);
    return ok;
}
