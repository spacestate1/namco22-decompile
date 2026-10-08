/*
 * eng.c -- the engine's asset slots (eng.h): texture ROMs, point data and
 * the resolved palette. The game fills them; the renderer only reads.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "eng.h"

uint8_t *g_texture_data = NULL;
uint8_t *g_texture_tilemap = NULL;

const int32_t   *g_eng_pointrom;
uint32_t         g_eng_pointrom_n;
eng_point_ext_fn g_eng_pointram;

uint8_t  direct_palette[ENG_PAL_GROUPS][ENG_PAL_ENTRIES][3];
int      direct_palette_loaded = 0;
uint16_t g_eng_pal_gen[ENG_PAL_GROUPS];

static int load_at(const char *dir, const char *name, uint8_t *dst, size_t n)
{
    char p[1024];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    FILE *f = fopen(p, "rb");
    if (!f) { fprintf(stderr, "  Cannot open: %s\n", p); return 0; }
    size_t got = fread(dst, 1, n, f);
    fclose(f);
    if (got != n) { fprintf(stderr, "  Short read: %s (%zu/%zu)\n", p, got, n); return 0; }
    return 1;
}

/* A chip SHORTER than its slot (Cyber Commando's 1 MB tile map in the 2 MB textilemap region): MAME loads it at the start of a
 * zero-filled region, so the rest of the slot reads 0. The size must still be a whole 512 KB multiple (a truncated file is an error). */
static int load_upto(const char *dir, const char *name, uint8_t *dst, size_t n)
{
    char p[1024];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    FILE *f = fopen(p, "rb");
    if (!f) { fprintf(stderr, "  Cannot open: %s\n", p); return 0; }
    size_t got = fread(dst, 1, n, f);
    fclose(f);
    if (got == 0 || got % 0x80000) { fprintf(stderr, "  Short read: %s (%zu/%zu)\n", p, got, n); return 0; }
    memset(dst + got, 0, n - got);
    return 1;
}

int eng_load_texture_roms(const char *dir, const char *const cg[8],
                          const char *ccrl, const char *ccrh)
{
    if (!g_texture_data) g_texture_data = calloc(TEXTURE_TOTAL_SIZE, 1);
    if (!g_texture_tilemap) g_texture_tilemap = calloc(TEXTUREMAP_SIZE, 1);
    if (!g_texture_data || !g_texture_tilemap) return 0;
    int ok = 1;
    for (int i = 0; i < 8; i++)
        ok = ok && (!cg[i] || load_at(dir, cg[i], g_texture_data + (size_t)i * 0x200000, 0x200000));
    ok = ok && load_upto(dir, ccrl, g_texture_tilemap, 0x200000);
    ok = ok && load_at(dir, ccrh, g_texture_tilemap + 0x200000, 0x80000);
    return ok;
}

int g_eng_tex_tile16 = 0;

void eng_texture_tilemap_sys22_fixup(void)
{
    if (!g_texture_tilemap) return;
    for (int i = 0; i < 0x100000; i++) {
        const uint8_t b = g_texture_tilemap[0x200000 + (i >> 1)];
        const int attr = (i & 1) ? (b & 0xF) : (b >> 4);      /* HIGH nibble on the even index (MAME's unpack) */
        if (attr & 1) continue;
        const uint16_t t = (uint16_t)(g_texture_tilemap[2 * i] | g_texture_tilemap[2 * i + 1] << 8);
        const uint16_t f = (uint16_t)((t & 0x3fff) | 0x8000);
        g_texture_tilemap[2 * i] = (uint8_t)f;
        g_texture_tilemap[2 * i + 1] = (uint8_t)(f >> 8);
    }
    g_eng_tex_tile16 = 1;
}

void eng_palette_from_planar(const uint8_t *planar, size_t plane_stride)
{
    for (int g = 0; g < ENG_PAL_GROUPS; g++) {
        uint8_t grp[ENG_PAL_ENTRIES][3];
        for (int p = 0; p < ENG_PAL_ENTRIES; p++) {
            const size_t i = (size_t)g * 256 + (size_t)p;
            grp[p][0] = planar[i];
            grp[p][1] = planar[i + plane_stride];
            grp[p][2] = planar[i + 2 * plane_stride];
        }
        if (memcmp(grp, direct_palette[g], sizeof grp)) {
            memcpy(direct_palette[g], grp, sizeof grp);
            g_eng_pal_gen[g]++;
        }
    }
    direct_palette_loaded = 1;
}
