/* csz_game.c -- CRISIS ZONE's table for the System 23 host (../tc2/include/tc2_game.h): MAME's crszone set (World, CSZO4 Ver. B),
 * System 23 EVOLUTION 2. Values from MAME's namcos23.cpp (crszone_state, ROM_START(crszone)) and the I/O board's device (namco_csz1). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tc2_game.h"

static const eng_rom_t chips[] = {
    { "cszo4verb.ic4", 0x800000, NULL, NULL, 0x6192533D }, { "cszo3verb.ic1", 0x80000, NULL, NULL, 0xC790743B },
    { "csz1prg0a.8f", 0x20000, NULL, NULL, 0x8EDC36B3 },
    { "csz1mtah.2j", 0x800000, NULL, NULL, 0x66B076AD }, { "csz1mtal.2h", 0x800000, NULL, NULL, 0x38DC639A },
    { "csz1mtbh.2m", 0x800000, NULL, NULL, 0xBDEC4188 }, { "csz1mtbl.2f", 0x800000, NULL, NULL, 0x9C8F8D7A },
    { "csz1cgll.4m", 0x800000, NULL, NULL, 0x0BCD41F2 }, { "csz1cglm.4k", 0x800000, NULL, NULL, 0xD4AF93D1 },
    { "csz1cgum.4j", 0x800000, NULL, NULL, 0x913C98B5 }, { "csz1cguu.5f", 0x800000, NULL, NULL, 0xE1D1BF24 },
    { "csz1ccrl.7f", 0x400000, NULL, NULL, 0x1C20768D }, { "csz1ccrh.7e", 0x200000, NULL, NULL, 0xBC2FA03C },
    { "csz1pt0h.7a", 0x400000, NULL, NULL, 0xE82F1ABB }, { "csz1pt0l.7c", 0x400000, NULL, NULL, 0xB0D66AFE },
    { "csz1pt1h.5a", 0x400000, NULL, NULL, 0xE54F80AD }, { "csz1pt1l.5c", 0x400000, NULL, NULL, 0x527171C8 },
    { "csz1pt2h.4a", 0x400000, NULL, NULL, 0xE295137A }, { "csz1pt2l.4c", 0x400000, NULL, NULL, 0xC87D6DBD },
    { "csz1pt3h.3a", 0x400000, NULL, NULL, 0x05F65BDF }, { "csz1pt3l.3c", 0x400000, NULL, NULL, 0x5D077C0F },
    { "csz1wavel.2c", 0x800000, NULL, NULL, 0xD0D74132 }, { "csz1waveh.2a", 0x800000, NULL, NULL, 0xDE9D14A8 },
};
static const char *const zips[] = { "crszone.zip", "namco_csz1.zip" };   /* a split MAME set may keep the I/O board's program in its device zip */

int tc2_rom_read(const char *dir, const char *name, uint8_t *dst, size_t n);   /* ../tc2/src/tc2_rom.c */

static void unswap(uint8_t *d, size_t n) { for (size_t k = 0; k + 1 < n; k += 2) { const uint8_t t = d[k]; d[k] = d[k + 1]; d[k + 1] = t; } }
/* the boot ROM (8 MB at 0x1F800000): cszo4verb.ic4 word-swapped (MAME ROM_LOAD16_WORD_SWAP into "user1"). The reset vector 0x1FC00000 is its
 * second half: the NMON monitor ("for SYSTEM23 EV2") and, 0x20000 on, the game's ELF (crszone/tools/make_prog.py prints the layout) */
static int rom_boot(const char *dir, uint8_t *boot)
{
    if (!tc2_rom_read(dir, "cszo4verb.ic4", boot, 0x800000)) return 0;
    unswap(boot, 0x800000);
    return 1;
}
/* the sub-CPU's (H8/3002) program: cszo3verb.ic1, word-swapped as Time Crisis 2's */
static int rom_sub(const char *dir, uint8_t *dst, size_t n)
{
    if (n < 0x80000 || !tc2_rom_read(dir, "cszo3verb.ic1", dst, 0x80000)) return 0;
    unswap(dst, 0x80000);
    return 1;
}
/* the I/O board's (H8/3334) program: the first n bytes of csz1prg0a.8f (128 KB; the board maps 16 KB, the rest is 0xFF) */
static int rom_io(const char *dir, uint8_t *dst, size_t n)
{
    uint8_t *all = malloc(0x20000);
    const int ok = all && n <= 0x20000 && tc2_rom_read(dir, "csz1prg0a.8f", all, 0x20000);
    if (ok) { memcpy(dst, all, n); if (n > 0x4ED && dst[0x4EC] == 0xF8 && dst[0x4ED] == 0x04) dst[0x4ED] = 0x05; }   /* MAME's patch (tools/make_prog.py says why) */
    free(all);
    return ok;
}

const tc2_game_t tc2_game = {
    .title = "Crisis Zone", .tag = "crszone", .window_title = "Crisis Zone", .mame_set = "crszone", .io_analog_out = 1, .recoil_hold = 1,
    .speech_bank_lo = 0xDB, .speech_bank_hi = 0xFF,   /* the call-outs (0xDB/0xDC/0xE1, centred) and the shouts (0xFD, panned): voices 30/31, and 17 doubling 0xFD */
    .zip_help = "MAME's crszone.zip (Crisis Zone, World CSZO4 Ver. B)",
    .zips = zips, .nzips = 2, .chips = chips, .nchips = (int)(sizeof chips / sizeof chips[0]),
    .evo2 = 1,
    .beam_wait_lo = 0x801F76F0u, .beam_wait_hi = 0x801F772Cu,   /* the zone select's "read the counter until two reads agree": called only by the
                                                                  * two ticker waits (0x801A3F54, 0x801A40A4), which loop on it for most of a frame */
    .clock = 203212800u,                          /* BUSCLOCK * 6 */
    .px_num = 500, .px_den = 3969,                /* 25.6 MHz / 203.2128 MHz */
    .sub_div = 12,                                /* 16.9344 MHz */
    .io_num = 32, .io_den = 441,                  /* 14.7456 MHz */
    .c352_div = 2304,                             /* 88200 Hz */
    /* MAME crszone_state::machine_start: IRQ0 RS-232, IRQ1 sub-CPU + C422, IRQ2 vblank, IRQ3 C361 + C451, IRQ4 C450 (no C435 interrupt) */
    .irq_prompt = 1,                              /* ~550 C450 DMA interrupts a frame: take each at once, as MAME */
    .ip = { [TC2_SRC_RS232] = TC2_IP(0), [TC2_SRC_SUB] = TC2_IP(1), [TC2_SRC_C422] = TC2_IP(1), [TC2_SRC_VBLANK] = TC2_IP(2),
            [TC2_SRC_C361] = TC2_IP(3), [TC2_SRC_C451] = TC2_IP(3), [TC2_SRC_C450] = TC2_IP(4), [TC2_SRC_TIMER] = TC2_IP(5) },
    .boot_size = 0x800000, .rom_boot = rom_boot, .rom_sub = rom_sub, .rom_io = rom_io, .io_rom_size = 0x4000, .c417_status = 0x0000, .no_direct_buf = 1, .depth_default = 0, .gun_x0 = 0x040, .gun_xw = 0x33F - 0x040, .gun_y0 = 0x020, .gun_yh = 0x0FF - 0x020, .sub_p7 = 0x80, .io_6001_held = 0x08,
    .data_rom = { { "csz1mtah.2j", "csz1mtal.2h" }, { "csz1mtbh.2m", "csz1mtbl.2f" } },
    .npt = 4, .pt_rom = { { "csz1pt0h.7a", "csz1pt0l.7c" }, { "csz1pt1h.5a", "csz1pt1l.5c" }, { "csz1pt2h.4a", "csz1pt2l.4c" }, { "csz1pt3h.3a", "csz1pt3l.3c" } },
    .ccrl = "csz1ccrl.7f", .ccrh = "csz1ccrh.7e", .cg = { "csz1cgll.4m", "csz1cglm.4k", "csz1cgum.4j", "csz1cguu.5f" },
    .wave = { "csz1wavel.2c", "csz1waveh.2a" },
};
