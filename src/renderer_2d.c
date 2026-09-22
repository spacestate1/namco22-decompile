/*
 * 2D Renderer - Tilemap + Sprites
 *
 * Text layer: 64x32 grid of 16x16x4bpp tiles = 1024x512 pixels
 * Palette: 3 separate byte planes (R, G, B)
 * Text palette base from videomix[0x1B]
 */
#include "propcycl.h"
#include <GL/gl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/* Use direct palette loaded from ROM/MAME dump (same as renderer_3d.c) */
extern void renderer3d_load_palette(const char *rom_dir);
/* Access the direct palette from renderer_3d.c */
#define PAL_GROUPS 128
#define PAL_ENTRIES 256
extern uint8_t direct_palette[PAL_GROUPS][PAL_ENTRIES][3];
extern int direct_palette_loaded;

/* Helper: look up RGB for the 2D tilemap/sprite layer.
 *
 * Read from g_sys.palette_ram (the live, runtime palette) FIRST. Game code
 * actively populates the title-text palette at runtime (game_logic.c:553+
 * sets palette banks at both 0x7E00 and 0x7F00 bases). The MAME runtime
 * dump (direct_palette) is a snapshot from a different game state — its
 * group 127 has post-attract values where pen 1 = (16,16,16), making title
 * text render near-invisible against the black background.
 *
 * Fall back to direct_palette only when palette_ram appears unpopulated
 * for this index (R+G+B all zero) — covers the case where we're in
 * gameplay state and the runtime palette hasn't yet been set up. */
static void pal_lookup(int pal_idx, uint8_t *r, uint8_t *g, uint8_t *b) {
    if (pal_idx < 0 || pal_idx >= 0x8000) {
        *r = *g = *b = 0;
        return;
    }
    uint8_t pr = g_sys.palette_ram[0x00000 + pal_idx];
    uint8_t pg = g_sys.palette_ram[0x08000 + pal_idx];
    uint8_t pb = g_sys.palette_ram[0x10000 + pal_idx];
    if ((pr | pg | pb) != 0 || !direct_palette_loaded) {
        *r = pr; *g = pg; *b = pb;
        return;
    }
    /* Fallback: palette_ram is zero here, MAME dump is loaded. */
    int grp = pal_idx / PAL_ENTRIES;
    int pen = pal_idx % PAL_ENTRIES;
    *r = direct_palette[grp][pen][0];
    *g = direct_palette[grp][pen][1];
    *b = direct_palette[grp][pen][2];
}

/* Tilemap framebuffer: 1024x512 RGBA */
#define TM_WIDTH  1024
#define TM_HEIGHT 512
static uint32_t tilemap_pixels[TM_WIDTH * TM_HEIGHT];
static GLuint tilemap_texture;

void renderer2d_init(void) {
    glGenTextures(1, &tilemap_texture);
    glBindTexture(GL_TEXTURE_2D, tilemap_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, TM_WIDTH, TM_HEIGHT, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, NULL);
}

void renderer2d_draw_tilemap(void) {
    memset(tilemap_pixels, 0, sizeof(tilemap_pixels));

    int pal_base = (g_sys.videomix[0x1B] & 0x7F) << 8;

    /* Decode 64x32 grid of 16x16 tiles */
    for (int ty = 0; ty < 32; ty++) {
        for (int tx = 0; tx < 64; tx++) {
            int ram_off = (ty * 64 + tx) * 2;
            if (ram_off + 1 >= TEXTRAM_SIZE) continue;

            uint16_t entry = (g_sys.textram[ram_off] << 8) | g_sys.textram[ram_off + 1];
            if (entry == 0x03BF || entry == 0) continue;  /* blank tile — skip */

            int palette_bank = (entry >> 12) & 0xF;
            int flip_y = (entry >> 11) & 1;
            int flip_x = (entry >> 10) & 1;
            int tile_num = entry & 0x3FF;

            /* Each tile: 16x16 pixels, 4bpp = 128 bytes
             * Row = 8 bytes (16 pixels * 4 bits)
             * High nybble = left pixel */
            uint32_t cg_base = (uint32_t)tile_num * 128;
            if (cg_base + 128 > CGRAM_SIZE) continue;

            for (int py = 0; py < 16; py++) {
                for (int px = 0; px < 16; px++) {
                    int fpx = flip_x ? (15 - px) : px;
                    int fpy = flip_y ? (15 - py) : py;

                    /* No byte swap needed - CGRAM data is stored in ROM byte order */
                    int byte_off = cg_base + fpy * 8 + fpx / 2;
                    uint8_t byte_val = (byte_off < CGRAM_SIZE) ? g_sys.cgram[byte_off] : 0xFF;
                    uint8_t pix = (fpx & 1) ? (byte_val & 0x0F) : (byte_val >> 4);

                    /* Namco S22 text layer: pen 0 and 0xF are transparent.
                     * Pen 0 = background/transparent, pens 1-14 = visible. */
                    if (pix == 0 || pix >= 0xF) continue;

                    int pal_idx = pal_base + palette_bank * 16 + pix;
                    uint8_t r = 0, g = 0, b = 0;
                    pal_lookup(pal_idx, &r, &g, &b);

                    int sx = tx * 16 + px;
                    int sy = ty * 16 + py;
                    if (sx < TM_WIDTH && sy < TM_HEIGHT) {
                        tilemap_pixels[sy * TM_WIDTH + sx] =
                            (0xFF << 24) | (b << 16) | (g << 8) | r;
                    }
                }
            }
        }
    }

    /* === Composite sprites into tilemap buffer before upload === */
    {
        extern intptr_t _W[];
        int16_t buf_sel = (int16_t)_W[0xAAF0];
        uint32_t list_base = 0x4AF0 + (uint32_t)(buf_sel & 3) * 0x1800;
        int16_t cur = (int16_t)_W[list_base + 0x16];
        int spr_count = 0;

        /* Tilemap scroll offset — sprites need to be placed relative to scroll */
        int scr_x = ((g_sys.tilemapattr[0] << 8) | g_sys.tilemapattr[1]) - 0x35C;
        int scr_y = ((g_sys.tilemapattr[2] << 8) | g_sys.tilemapattr[3]);

        static int spr_dbg = 0;
        int trace_now = (spr_dbg < 2);
        if (trace_now) {
            int16_t cnt = (int16_t)_W[0x4AEC];
            if (propcycl_verbose()) printf("  [SPR] frame=%u buf_sel=%d list_head=%d count=%d\n",
                   g_sys.frame_count, (int)buf_sel, (int)cur, (int)cnt);
            spr_dbg++;
        }
        while (cur != 0 && spr_count < 256) {
            uint32_t entry = list_base + (uint32_t)cur * 0x18;
            if (entry + 0x18 >= WORK_RAM_SIZE) break;

            int16_t tile_id  = (int16_t)_W[entry + 0x00];
            int sx = (int)(int16_t)_W[entry + 0x02] - 0x280 + scr_x;
            int sy = (int)(int16_t)_W[entry + 0x04] - 0x32A + scr_y;
            int16_t flags = (int16_t)_W[entry + 0x12];
            int hflip = (flags & 0x8) != 0;
            int vflip = (flags & 0x4) != 0;
            if (trace_now) {
                if (propcycl_verbose()) printf("    [SPR] entry %d: tile=0x%X (%d) sx=%d sy=%d flags=0x%X\n",
                       (int)cur, (int)(uint16_t)tile_id, (int)tile_id, sx, sy, (int)flags);
            }

            /* Negative tile_id is a Namco metasprite-flag convention used by
             * sprite_draw_multi_segment and the HUD timer/score code in
             * game_misc.c::FUN_0000e0e8: a positive tile_id from a ROM
             * table gets explicitly negated when passed to sprite_draw_2d.
             * The original M68K renderer treated abs(tile_id) as the tile
             * index; the sign carries auxiliary info (likely h-flip or
             * palette select) that we still don't decode. Without abs()
             * the gameplay HUD digits stay invisible because every entry
             * but one has a negative tile_id. */
            int eff_tile = tile_id < 0 ? -(int)tile_id : (int)tile_id;
            if (g_sprite_tiles && eff_tile > 0 &&
                (uint32_t)eff_tile * SPRITE_TILE_SIZE + SPRITE_TILE_SIZE <= SPRITE_TOTAL_SIZE) {
                uint8_t* tile = g_sprite_tiles + (uint32_t)eff_tile * SPRITE_TILE_SIZE;

                for (int py = 0; py < 32; py++) {
                    for (int px = 0; px < 32; px++) {
                        int srcx = hflip ? (31 - px) : px;
                        int srcy = vflip ? (31 - py) : py;
                        uint8_t pen = tile[srcy * 32 + srcx];
                        if (pen == 0xFF || pen == 0) continue;

                        int dx = sx + px;
                        int dy = sy + py;
                        if (dx < 0 || dx >= TM_WIDTH || dy < 0 || dy >= TM_HEIGHT) continue;

                        /* Per-sprite palette selection. The original sprite list
                         * encodes a palette index in the entry; we don't decode
                         * it yet, so apply known mappings by tile_id range:
                         *   - 0x320..0x33F: Namco logo (red) → palette group 0x1D
                         *   - 0x340..0x35F: TIME UP / red text → group 0x1D
                         *   - default:                        → group 0x74 (cream/gold) */
                        uint32_t pal_base = 0x7400;
                        if (tile_id >= 0x320 && tile_id < 0x360)
                            pal_base = 0x1D00;
                        uint32_t pal_idx = pal_base + pen;
                        uint8_t r, g, b;
                        pal_lookup(pal_idx, &r, &g, &b);

                        tilemap_pixels[dy * TM_WIDTH + dx] =
                            (0xFF << 24) | (b << 16) | (g << 8) | r;
                    }
                }
            }

            int16_t next = (int16_t)_W[entry + 0x16];
            if (next == cur) break;
            cur = next;
            spr_count++;
        }
        if (spr_dbg < 2) {
            if (propcycl_verbose()) printf("  [SPR] total iterated: %d\n", spr_count);
            spr_dbg++;
        }
    }
    /* Reset the sprite list every frame after consuming it. The proper
     * hardware-path reset would be in sprite_dma_kick after spriteram DMA,
     * but that runs before the renderer reads the list — so we reset here
     * after rendering instead.
     *
     * Reset BOTH _W[] and underlying work_ram (BE 32-bit), because
     * sync_wram_to_W at the start of next game_frame will restore W
     * from work_ram and undo a _W-only reset. */
    {
        extern intptr_t _W[];
        int16_t buf_sel = (int16_t)_W[0xAAF0];
        uint32_t list_base = 0x4AF0 + (uint32_t)(buf_sel & 3) * 0x1800;
        _W[0x4AEC] = 0;
        _W[list_base + 0x16] = 0;
        g_sys.work_ram[0x4AEC]     = 0; g_sys.work_ram[0x4AEC + 1] = 0;
        g_sys.work_ram[0x4AEC + 2] = 0; g_sys.work_ram[0x4AEC + 3] = 0;
        g_sys.work_ram[list_base + 0x16]     = 0;
        g_sys.work_ram[list_base + 0x16 + 1] = 0;
        g_sys.work_ram[list_base + 0x16 + 2] = 0;
        g_sys.work_ram[list_base + 0x16 + 3] = 0;
    }

    /* Debug: check tilemap buffer before upload */
    {
        static int tdbg = 0;
        if (tdbg < 2) {
            int npix = 0;
            for (int i = 0; i < TM_WIDTH * TM_HEIGHT; i++)
                if (tilemap_pixels[i] != 0) npix++;
            if (propcycl_verbose()) printf("  [UPLOAD] tilemap_pixels: %d non-zero of %d total\n",
                   npix, TM_WIDTH * TM_HEIGHT);
            /* Per-row count: how many non-zero pixels in each Y row, near sy=160..320 */
            for (int y = 155; y < 330; y += 16) {
                int row_nz = 0, last_x = -1, first_x = -1;
                for (int x = 0; x < TM_WIDTH; x++) {
                    if (tilemap_pixels[y * TM_WIDTH + x] != 0) {
                        if (first_x < 0) first_x = x;
                        last_x = x;
                        row_nz++;
                    }
                }
                if (row_nz > 0)
                    printf("    [TM-row] y=%d: %d non-zero, x=[%d..%d]\n",
                           y, row_nz, first_x, last_x);
            }
            tdbg++;
        }
    }

    /* Skip drawing if tilemap is completely empty — avoids GL state
     * issues that cause the screen to go white over the 3D content */
    {
        int has_content = 0, i;
        for (i = 0; i < TM_WIDTH * TM_HEIGHT; i++) {
            if (tilemap_pixels[i] != 0) { has_content = 1; break; }
        }
        if (!has_content) return;
    }

    /* Upload and draw */
    glBindTexture(GL_TEXTURE_2D, tilemap_texture);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, TM_WIDTH, TM_HEIGHT,
                    GL_RGBA, GL_UNSIGNED_BYTE, tilemap_pixels);

    /* Disable depth test so 2D overlay draws on top of 3D scene */
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_ALPHA_TEST);  /* don't let 3D alpha test interfere with 2D blend */
    glEnable(GL_TEXTURE_2D);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, SCREEN_WIDTH, SCREEN_HEIGHT, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    /* Apply scroll from tilemap attrs
     * Attr[0] = X scroll (subtract 0x35C bias per MAME)
     * Attr[1] = Y scroll */
    int scroll_x = ((g_sys.tilemapattr[0] << 8) | g_sys.tilemapattr[1]) - 0x35C;
    int scroll_y = ((g_sys.tilemapattr[2] << 8) | g_sys.tilemapattr[3]);
    {
        static int scrl_dbg = 0;
        if (scrl_dbg < 2) {
            if (propcycl_verbose()) printf("  [TM] scroll_x=%d scroll_y=%d  attr=[%02X %02X %02X %02X]\n",
                   scroll_x, scroll_y,
                   g_sys.tilemapattr[0], g_sys.tilemapattr[1],
                   g_sys.tilemapattr[2], g_sys.tilemapattr[3]);
            scrl_dbg++;
        }
    }

    /* Scale to fit screen: tilemap is 1024x512, screen is 640x480 */
    float u0 = (float)scroll_x / TM_WIDTH;
    float v0 = (float)scroll_y / TM_HEIGHT;
    float u1 = u0 + (float)SCREEN_WIDTH / TM_WIDTH;
    float v1 = v0 + (float)SCREEN_HEIGHT / TM_HEIGHT;

    glColor4f(1, 1, 1, 1);
    glBindTexture(GL_TEXTURE_2D, tilemap_texture);
    glBegin(GL_QUADS);
    glTexCoord2f(u0, v0); glVertex2f(0, 0);
    glTexCoord2f(u1, v0); glVertex2f(SCREEN_WIDTH, 0);
    glTexCoord2f(u1, v1); glVertex2f(SCREEN_WIDTH, SCREEN_HEIGHT);
    glTexCoord2f(u0, v1); glVertex2f(0, SCREEN_HEIGHT);
    glEnd();

    glDisable(GL_BLEND);
    glDisable(GL_TEXTURE_2D);
}

void renderer2d_draw_sprites(void) {
    /*
     * Render sprites directly into the tilemap_pixels buffer so they
     * get composited into the existing tilemap texture upload.
     * Call this AFTER draw_tilemap fills tilemap_pixels but BEFORE the GL upload.
     *
     * Actually — we need to draw AFTER the tilemap texture is on screen.
     * Instead, render into tilemap_pixels before the upload in draw_tilemap.
     *
     * For now: just draw sprites as a second textured quad using the
     * tilemap texture infrastructure (reuse tilemap_pixels for overlay).
     */
    /* Sprites are now composited in renderer2d_draw_tilemap. Nothing here. */
}

void renderer2d_composite(void) {
    /* Apply screen fade from video mixer */
    uint8_t fade_r = g_sys.videomix[0x00];  /* Fade R */
    uint8_t fade_g = g_sys.videomix[0x01];  /* Fade G */
    uint8_t fade_b = g_sys.videomix[0x02];  /* Fade B */
    uint8_t fade_factor = g_sys.videomix[0x03];  /* Fade control */

    static int comp_dbg = 0;
    if (comp_dbg < 3) {
        if (propcycl_verbose()) printf("  [COMPOSITE] fade RGB=(%d,%d,%d) factor=%d  vm[12]=%d vm[14]=%d\n",
               fade_r, fade_g, fade_b, fade_factor,
               g_sys.videomix[0x12], g_sys.videomix[0x14]);
        comp_dbg++;
    }

    /* Only draw partial fade (factor < 240). Skip near-opaque fades that
     * would completely hide the 3D scene — the game's fade state machine
     * doesn't always complete the fade-out in the reimplementation. */
    if (fade_factor > 0 && fade_factor < 240) {
        float alpha = fade_factor / 255.0f;
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDisable(GL_TEXTURE_2D);

        glBegin(GL_QUADS);
        glColor4f(fade_r / 255.0f, fade_g / 255.0f, fade_b / 255.0f, alpha);
        glVertex2f(0, 0);
        glVertex2f(SCREEN_WIDTH, 0);
        glVertex2f(SCREEN_WIDTH, SCREEN_HEIGHT);
        glVertex2f(0, SCREEN_HEIGHT);
        glEnd();

        glColor4f(1, 1, 1, 1);
        glDisable(GL_BLEND);
    }
}
