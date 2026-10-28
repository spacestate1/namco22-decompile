/* ak_game.c -- ANGLER KING's table for the System 23 host (../tc2/include/tc2_game.h): MAME's aking set (Japan, AG1 Ver. A), SUPER System 23
 * (MAME machine ss23: the System 23 map and clocks, as Time Crisis 2). Values from MAME's namcos23.cpp (ss23, ROM_START(aking)); the I/O board
 * is JVS namco_fca10 ("FCA-1 ... Multipurpose + Rotary Encoder"), an H8/3334 like Time Crisis 2's gun board. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tc2_game.h"

static const eng_rom_t chips[] = {
    { "ag1vera.ic2", 0x200000, NULL, NULL, 0xDC98FEFB }, { "ag1vera.ic1", 0x200000, NULL, NULL, 0xF1A08D5C },
    { "ag1vera.ic3", 0x80000, NULL, NULL, 0x266AC71C },
    { "asc3_io-c1.ic14", 0x20000, NULL, NULL, 0xB6627A0E },  /* the I/O board's program as aking.zip carries it (MAME 0.289 lists asc3_io-c.ic14) */
    { "ag1mtah.2j", 0x800000, NULL, NULL, 0xF2D8CA9D }, { "ag1mtal.2h", 0x800000, NULL, NULL, 0x7FACBFD4 },
    { "ag1mtbh.2m", 0x800000, NULL, NULL, 0x890BDB52 }, { "ag1mtbl.2f", 0x800000, NULL, NULL, 0x62D771C9 },
    { "ag1cgll.4m", 0x800000, NULL, NULL, 0x9DB7E939 }, { "ag1cglm.4k", 0x800000, NULL, NULL, 0x17792DBA },
    { "ag1cgum.4j", 0x800000, NULL, NULL, 0x5DFA863D }, { "ag1cguu.4f", 0x800000, NULL, NULL, 0x86396786 },
    { "ag1ccrl.7f", 0x400000, NULL, NULL, 0x86BBE1F9 }, { "ag1ccrh.7e", 0x200000, NULL, NULL, 0xABE2AAB1 },
    { "ag1pt0h.7a", 0x400000, NULL, NULL, 0xB5582CA7 }, { "ag1pt0l.7c", 0x400000, NULL, NULL, 0x10E7F54F },
    { "ag1pt1h.5a", 0x400000, NULL, NULL, 0x25E4776A }, { "ag1pt1l.5c", 0x400000, NULL, NULL, 0x5D3D7099 },
    { "ag1pt2h.4a", 0x400000, NULL, NULL, 0xF0EB9012 }, { "ag1pt2l.4c", 0x400000, NULL, NULL, 0xBF92C054 },
    { "ag1pt3h.3a", 0x400000, NULL, NULL, 0xE11A12CE }, { "ag1pt3l.3c", 0x400000, NULL, NULL, 0x04B475DB },
    { "ag1wavel.2c", 0x800000, NULL, NULL, 0xD7FEFBD4 }, { "ag1waveh.2a", 0x800000, NULL, NULL, 0x37A61DAA },
};
static const char *const zips[] = { "aking.zip" };

int tc2_rom_read(const char *dir, const char *name, uint8_t *dst, size_t n);   /* ../tc2/src/tc2_rom.c */

/* the boot ROM (4 MB at 0x1FC00000): ag1vera.ic2 and .ic1 byte-interleaved, .ic2 first (MAME ROM_LOAD16_BYTE), as Time Crisis 2's;
 * the NMON monitor ("for SYSTEM23 P1") and, 0x20000 on, the game's ELF (tools/make_prog.py prints the layout) */
static int rom_boot(const char *dir, uint8_t *boot)
{
    uint8_t *a = malloc(0x200000), *b = malloc(0x200000);
    const int ok = a && b && tc2_rom_read(dir, "ag1vera.ic1", a, 0x200000) && tc2_rom_read(dir, "ag1vera.ic2", b, 0x200000);
    if (ok) for (size_t k = 0; k < 0x200000; k++) { boot[2 * k] = b[k]; boot[2 * k + 1] = a[k]; }
    free(a); free(b);
    return ok;
}
static void unswap(uint8_t *d, size_t n) { for (size_t k = 0; k + 1 < n; k += 2) { const uint8_t t = d[k]; d[k] = d[k + 1]; d[k + 1] = t; } }
/* the sub-CPU's (H8/3002) program: ag1vera.ic3, word-swapped (MAME ROM_LOAD16_WORD_SWAP) */
static int rom_sub(const char *dir, uint8_t *dst, size_t n)
{
    if (n < 0x80000 || !tc2_rom_read(dir, "ag1vera.ic3", dst, 0x80000)) return 0;
    unswap(dst, 0x80000);
    return 1;
}
/* the I/O board's (H8/3334) program: the first n bytes of asc3_io-c1.ic14 (128 KB; only the first 8 KB hold code) */
static int rom_io(const char *dir, uint8_t *dst, size_t n)
{
    uint8_t *all = malloc(0x20000);
    const int ok = all && n <= 0x20000 && tc2_rom_read(dir, "asc3_io-c1.ic14", all, 0x20000);
    if (ok) memcpy(dst, all, n);
    free(all);
    return ok;
}

const tc2_game_t tc2_game = {
    .title = "Angler King", .tag = "aking", .window_title = "Angler King", .mame_set = "aking", .subtitles = "aking_subtitles.txt", .uv_edge = 1, .cz_fog = 1,
    .super23 = 1, .io_asca3 = 1,                                /* the C361 acknowledge at 0x0100005C (MAME 0.289 namcoss23_state); the I/O board an FCA-1 = ASCA-3 */
    .zip_help = "MAME's aking.zip (Angler King, Japan, AG1 Ver. A)",
    .zips = zips, .nzips = 1, .chips = chips, .nchips = (int)(sizeof chips / sizeof chips[0]),
    .clock = 169344000u,                          /* BUSCLOCK * 5 (MAME ss23) */
    .px_num = 200, .px_den = 1323,                /* 25.6 MHz / 169.344 MHz */
    .sub_div = 10,                                /* 16.9344 MHz */
    .io_num = 64, .io_den = 735,                  /* 14.7456 MHz */
    .c352_div = 1920,                             /* 88200 Hz */
    .ip = { [TC2_SRC_VBLANK] = TC2_IP(0), [TC2_SRC_C361] = TC2_IP(1), [TC2_SRC_SUB] = TC2_IP(1), [TC2_SRC_C435] = TC2_IP(2),
            [TC2_SRC_C422] = TC2_IP(3), [TC2_SRC_TIMER] = TC2_IP(5) },   /* System 23's routing, as Time Crisis 2 */
    .boot_size = 0x400000, .rom_boot = rom_boot, .rom_sub = rom_sub, .rom_io = rom_io, .io_rom_size = 0x4000, .c417_status = 0x008E, .depth_default = 0,
    .gun_x0 = 0, .gun_xw = 1023, .gun_y0 = 0, .gun_yh = 1023,   /* no gun: the pointer aims the ROD in 0..1023 (../tc2/src/tc2_input.c ak_send_inputs) */
    .data_rom = { { "ag1mtah.2j", "ag1mtal.2h" }, { "ag1mtbh.2m", "ag1mtbl.2f" } },
    .npt = 4, .pt_rom = { { "ag1pt0h.7a", "ag1pt0l.7c" }, { "ag1pt1h.5a", "ag1pt1l.5c" }, { "ag1pt2h.4a", "ag1pt2l.4c" }, { "ag1pt3h.3a", "ag1pt3l.3c" } },
    .ccrl = "ag1ccrl.7f", .ccrh = "ag1ccrh.7e", .cg = { "ag1cgll.4m", "ag1cglm.4k", "ag1cgum.4j", "ag1cguu.4f" },
    .wave = { "ag1wavel.2c", "ag1waveh.2a" },
};
