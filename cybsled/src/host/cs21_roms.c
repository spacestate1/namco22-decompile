/*
 * cs21_roms.c -- the first run: Cyber Sled's ROMs straight out of MAME's zips (the shared engine/romzip.c, as every game does).
 *
 * The set is three zips: cybsled.zip (the game), namcoc67.zip (c67.bin, the C67 DSP's internal ROM) and namcoc68.zip (c68.bin, the
 * C68 I/O MCU's internal ROM); a MAME set that carries the two BIOS files inside cybsled.zip works too. Every chip is taken by name and
 * size, checked against the zip's own CRC and written into the ROM folder (extracted/). The zips are looked for in the current folder,
 * roms/, the program's folder and its roms/. The CY1 files under cybsleda/ in the zip are not used. tools/setup_roms.py stays the
 * developer's way (it also checks every chip against MAME's CRC32 and builds the program images Ghidra reads; the game needs neither).
 */
#include <stdio.h>
#include <string.h>
#include <SDL2/SDL.h>
#include "romzip.h"
#include "s21_host.h"

static const eng_rom_t cs_roms[] = {          /* MAME -listxml cybsled / namcoc67 / namcoc68 (CRCs: docs/ROM_CHECKSUMS.md) */
    { "cy2-mpr-u.3j", 0x80000 },   { "cy2-mpr-l.1j", 0x80000 },      /* master 68000 */
    { "cy2-spr-u.6c", 0x80000 },   { "cy2-spr-l.4c", 0x80000 },      /* slave 68000 */
    { "cy1-snd0.8j", 0x20000 },                                       /* 6809 sound CPU */
    { "cy1-obj0.5s", 0x80000 },    { "cy1-obj1.5x", 0x80000 },    { "cy1-obj2.3s", 0x80000 },    { "cy1-obj3.3x", 0x80000 },
    { "cy1-obj4.4s", 0x80000 },    { "cy1-obj5.4x", 0x80000 },    { "cy1-obj6.2s", 0x80000 },    { "cy1-obj7.2x", 0x80000 },   /* C355 sprites */
    { "cy1-data-u.3a", 0x80000 },  { "cy1-data-l.1a", 0x80000 },     /* the shared data ROM (incl. the DSP programs) */
    { "cy1-edata0-u.3b", 0x80000 },{ "cy1-edata0-l.1b", 0x80000 },   /* the extra data ROM */
    { "cy1-poi-h1.2f", 0x80000 },  { "cy1-poi-lu1.2k", 0x80000 },  { "cy1-poi-ll1.2n", 0x80000 },    /* point ROM */
    { "cy1-poi-h2.2j", 0x80000 },  { "cy1-poi-lu2.2l", 0x80000 },  { "cy1-poi-ll2.2p", 0x80000 },
    { "cy1-voi0.12b", 0x80000 },   { "cy1-voi1.12c", 0x80000 },   { "cy1-voi2.12d", 0x80000 },   { "cy1-voi3.12e", 0x80000 },  /* C140 samples */
    { "cybsled.nv", 0x2000 },                                         /* MAME's default NVRAM: the settings and calibration */
    { "c67.bin", 0x2000 },                                            /* namcoc67.zip */
    { "c68.bin", 0x8000 },                                            /* namcoc68.zip */
};
#define N_ROMS ((int)(sizeof cs_roms / sizeof cs_roms[0]))

const char *cs21_rom_setup(const char *romdir, int window)
{
    if (eng_romzip_missing(romdir, cs_roms, N_ROMS) && !strcmp(romdir, "extracted") && !eng_romzip_missing("roms", cs_roms, N_ROMS))
        return "roms";                                   /* chips unzipped loose into roms/ work too */
    if (!eng_romzip_missing(romdir, cs_roms, N_ROMS)) return romdir;
    static const char *const zips[] = { "cybsled.zip", "namcoc67.zip", "namcoc68.zip" };
    char err[512], *base = SDL_GetBasePath();
    const bool ok = eng_romzip_autosetup(romdir, base, zips, 3, cs_roms, N_ROMS, err, sizeof err);
    SDL_free(base);
    if (ok) return romdir;
    fprintf(stderr, "Cyber Sled needs its ROMs: %s\n", err);
    if (window) {
        char msg[1024];
        snprintf(msg, sizeof msg, "Cyber Sled needs its ROMs.\n\nPut cybsled.zip, namcoc67.zip and namcoc68.zip (the MAME ROM sets) in the "
                 "\"roms\" folder next to this program, then start it again.\n\n(%s)", err);
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Cyber Sled", msg, NULL);
    }
    return NULL;
}
