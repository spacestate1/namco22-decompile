/*
 * pc_live2d.c -- Prop Cycle's LIVE 2D/fog state: g_sys's memory handed to the shared engine's Super 22 modules
 * (engine/fog_hw.c, text_hw.c, sprite_hw.c), which know nothing about this game's globals. Tokyo Wars does the same
 * from its own memory in tokyowar/src/tw_video.c.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "propcycl.h"
#include "fog_hw.h"
#include "text_hw.h"
#include "sprite_hw.h"
#include "pc_live2d.h"

double g_perf_fog;
static double fperf(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

int fog_load_live(void)
{
    double _t0 = fperf();
    const uint8_t *cz = g_sys.czram;
    uint16_t czattr[8], words[256];
    for (int i = 0; i < 8; i++)                       /* czattr at +0x00 */
        czattr[i] = (uint16_t)((cz[i * 2] << 8) | cz[i * 2 + 1]);
    for (int i = 0; i < 256; i++)
        words[i] = (uint16_t)((cz[0x200 + i * 2] << 8) | cz[0x200 + i * 2 + 1]);
    /* CZRAM BANKS -- a simplification, and MEASURED to be unexercised.
     *
     * The hardware has four banks but 0x810200 exposes only one window, so
     * all four recalc tables are built from it. Whether that matters depends
     * on `fog_quad`, which picks the table with
     *     bank = (czattr[6] >> ((cz_type & 3) * 2)) & 3
     * so it matters only if cz_type or czattr[6] is ever non-zero.
     *
     * Measured 2026-09-14: cz_type is 0 on 10161 of 10161 quads across the
     * six oracle frames AND four real gameplay frames (censused from
     * --geodump's cztype field), and czattr[6] reads 0x0000 throughout a
     * coin-to-gameplay run. Bank 0 is the only table ever consulted.
     *
     * Following the bank live is NOT simply `czattr[5] & 3`: the capture
     * script writes 0x81000A to force the window, but the game itself holds
     * 0x4444 there, so that register is not a plain bank index and its real
     * meaning is unconfirmed. Do not "fix" this by guessing at it -- confirm
     * the register against the ROM first. */
    const uint16_t *banks[4] = { words, words, words, words };
    fog_load_regs(g_sys.videomix, czattr, banks);
    g_perf_fog += fperf() - _t0;
    return 1;
}

static uint8_t *live_cg;

int text_load_live(text_state *st)
{
    if (!live_cg) { live_cg = malloc(0x20000); if (!live_cg) { memset(st, 0, sizeof *st); return 0; } }
    /* the model indexes cg[0x1E000:0x20000] as textram, and g_sys keeps the
     * two regions separately -- joining them reproduces that layout */
    memcpy(live_cg, g_sys.cgram, CGRAM_SIZE);
    memcpy(live_cg + CGRAM_SIZE, g_sys.textram, TEXTRAM_SIZE);
    /* NOT g_sys.palette_ram -- that is the broken one (see
     * renderer3d_load_palette). Using it made live HUD text render as
     * solid magenta blocks. */
    const uint8_t *pp = renderer3d_planar_palette();
    uint16_t attr[8];
    for (int i = 0; i < 8; i++)
        attr[i] = (uint16_t)((g_sys.tilemapattr[i*2] << 8) | g_sys.tilemapattr[i*2 + 1]);
    return text_load_regs(st, live_cg, pp ? pp : g_sys.palette_ram, attr);
}

int sprite_load_live(sprite_state *st)
{
    const uint8_t *b = g_sys.vics_ctrl;
    uint32_t vicsc[0x20];
    for (int i = 0; i < 0x20; i++)
        vicsc[i] = ((uint32_t)b[i*4] << 24) | ((uint32_t)b[i*4+1] << 16) |
                   ((uint32_t)b[i*4+2] << 8) | b[i*4+3];
    /* Same reason as text_hw: g_sys.palette_ram is the broken one. */
    const uint8_t *pp = renderer3d_planar_palette();
    return sprite_load_regs(st, g_sys.spriteram, SPRITERAM_SIZE, g_sys.vics_data, VICS_DATA_SIZE, vicsc,
                            pp ? pp : g_sys.palette_ram);
}
