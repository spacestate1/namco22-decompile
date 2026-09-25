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

int eng_load_texture_roms(const char *dir, const char *const cg[8],
                          const char *ccrl, const char *ccrh)
{
    if (!g_texture_data) g_texture_data = calloc(TEXTURE_TOTAL_SIZE, 1);
    if (!g_texture_tilemap) g_texture_tilemap = calloc(TEXTUREMAP_SIZE, 1);
    if (!g_texture_data || !g_texture_tilemap) return 0;
    int ok = 1;
    for (int i = 0; i < 8; i++)
        ok = ok && load_at(dir, cg[i], g_texture_data + (size_t)i * 0x200000, 0x200000);
    ok = ok && load_at(dir, ccrl, g_texture_tilemap, 0x200000);
    ok = ok && load_at(dir, ccrh, g_texture_tilemap + 0x200000, 0x80000);
    return ok;
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
