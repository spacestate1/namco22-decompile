/*
 * pc_palette.c -- Prop Cycle's palette: the ROM/MAME snapshot the shared
 * texture bake reads (engine/eng.h direct_palette), plus the planar copy the
 * 2D stages want. Split from the old renderer_texture.c when the bake moved
 * into engine/tex_bake.c; the board-specific half stays with the game.
 */
#include "renderer_internal.h"
#include <stdlib.h>
#include <stdio.h>

/* ========== Direct Palette ========== */

/* 128 palette groups × 256 entries × RGB. Loaded once at startup from
 * MAME runtime dump (preferred) or ROM static data (fallback). */
#define PAL_GROUPS  ENG_PAL_GROUPS
#define PAL_ENTRIES ENG_PAL_ENTRIES

void renderer3d_load_palette(const char *rom_dir) {
    char path[512];
    FILE *f;
    int loaded_mame = 0;

    /* Try MAME runtime palette dump first (includes runtime fog/lighting) */
    snprintf(path, sizeof(path), "%s/palette_mame_runtime.bin", rom_dir);
    f = fopen(path, "rb");
    if (f) {
        uint8_t *buf = malloc(128 * 256 * 3);
        if (buf && fread(buf, 1, 128 * 256 * 3, f) == 128 * 256 * 3) {
            for (int g = 0; g < 128; g++)
                for (int p = 0; p < 256; p++) {
                    int off = (g * 256 + p) * 3;
                    direct_palette[g][p][0] = buf[off];
                    direct_palette[g][p][1] = buf[off + 1];
                    direct_palette[g][p][2] = buf[off + 2];
                }
            loaded_mame = 1;
            printf("  [3D] Palette: MAME runtime dump (%s)\n", path);
        }
        if (buf) free(buf);
        fclose(f);
    }

    if (!loaded_mame) {
        /* Group 0 is black on the machine (MAME's runtime palette). The
         * packed table at 0xB5EC0 is NOT group 0: sprite_ram_header_init
         * (ROM 0x023198) loads it into groups 116..124 -- see below. */
        memset(direct_palette[0], 0, sizeof direct_palette[0]);
        /* Groups 1-127: planar R/G/B at ROM 0x21B204, 768 bytes per group */
        for (int g = 0; g < 127; g++) {
            int b = 0x21B204 + g * 768;
            for (int p = 0; p < 256; p++) {
                if (b + 512 + p < ROM_SIZE) {
                    direct_palette[g + 1][p][0] = g_sys.rom[b + p];
                    direct_palette[g + 1][p][1] = g_sys.rom[b + 256 + p];
                    direct_palette[g + 1][p][2] = g_sys.rom[b + 512 + p];
                }
            }
        }
        /* Groups 116..124: packed RGB at 0xB5EC0, 0x300 bytes a group, count
         * the BE16 at 0xB85B4 -- what the game itself loads at boot. */
        int npk = (g_sys.rom[0xB85B4] << 8) | g_sys.rom[0xB85B5];
        for (int k = 0; k < npk && 116 + k < PAL_GROUPS; k++)
            for (int p = 0; p < PAL_ENTRIES; p++) {
                int s = 0xB5EC0 + k * 0x300 + p * 3;
                if (s + 2 < ROM_SIZE) {
                    direct_palette[116 + k][p][0] = g_sys.rom[s];
                    direct_palette[116 + k][p][1] = g_sys.rom[s + 1];
                    direct_palette[116 + k][p][2] = g_sys.rom[s + 2];
                }
            }
        printf("  [3D] Palette: ROM static (group 127, the runtime colour ramps, is filled by the game)\n");
    }
    direct_palette_loaded = 1;
}

/*
 * The same palette in the PLANAR layout the 2D stages want (R at +0,
 * G at +0x8000, B at +0x10000), built once from direct_palette.
 *
 * text_hw/sprite_hw take a raw planar block. In a framedump replay they get
 * the capture's, which is why they gate 100.00% exact. On the LIVE path
 * they were handed g_sys.palette_ram -- the palette this project documents
 * as broken and which renderer3d_load_palette() exists to bypass. The
 * result was the HUD text drawing as solid magenta blocks over gameplay.
 */
static uint8_t planar_pal[0x18000];
static int planar_pal_built;

/*
 * WHICH PENS THE GAME ITSELF HAS WRITTEN.
 *
 * Measured against MAME's palette RAM on the CONTROLS tutorial screen
 * (tools/overnight/dump_menu_feed.lua vs PROPCYCL_FEEDDUMP): the snapshot
 * above is identical to the machine's on 32659 of 32768 pens, and ALL 109 it
 * gets wrong are in group 127 -- 0x7F00..0x7FFF, the runtime colour-ramp bank
 * that cz_load_color_ramp fills per screen. That is why the tutorial's red
 * arrow drew grey: its pens are 0x7F51..0x7F5E and the snapshot carried a
 * different screen's ramp there.
 *
 * g_sys.palette_ram cannot simply replace the snapshot -- the game only loads
 * a fraction of it (551/32768 match MAME), so a blanket switch would trade 109
 * wrong pens for 32217. Instead each palette writer marks the pens it touched,
 * and those -- and only those -- come from the game. The composition can
 * therefore only ever move agreement UP, and it converges on the machine as
 * more ramp loaders are wired in.
 */
static uint8_t pal_written[0x8000];

void palette_mark_written(int pen)
{
    if (pen < 0 || pen >= 0x8000) return;
    if (!pal_written[pen]) { pal_written[pen] = 1; planar_pal_built = 0; }
    else planar_pal_built = 0;      /* value may have changed */
}

const uint8_t *renderer3d_planar_palette(void) {
    if (!direct_palette_loaded) return NULL;
    if (!planar_pal_built) {
        for (int g = 0; g < PAL_GROUPS; g++)
            for (int p = 0; p < PAL_ENTRIES; p++) {
                int idx = g * 256 + p;
                if (pal_written[idx]) {
                    planar_pal[0x00000 + idx] = g_sys.palette_ram[0x00000 + idx];
                    planar_pal[0x08000 + idx] = g_sys.palette_ram[0x08000 + idx];
                    planar_pal[0x10000 + idx] = g_sys.palette_ram[0x10000 + idx];
                } else {
                    planar_pal[0x00000 + idx] = direct_palette[g][p][0];
                    planar_pal[0x08000 + idx] = direct_palette[g][p][1];
                    planar_pal[0x10000 + idx] = direct_palette[g][p][2];
                }
            }
        planar_pal_built = 1;
    }
    return planar_pal;
}

