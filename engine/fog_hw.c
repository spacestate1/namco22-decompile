/*
 * fog_hw.c — CZ depth fog + poly/screen fade state.
 *
 * Ported from pc_raster_model.py FogState (itself a port of
 * namcos22_v.cpp recalc_czram / drawquad). The awkward parts are faithful
 * on purpose and marked; "tidying" them changes output.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fog_hw.h"

fog_state g_fog;
int       g_fog_valid;

void fog_invalidate(void) { g_fog_valid = 0; }

static int read_file(const char *path, uint8_t *buf, size_t want)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    size_t got = fread(buf, 1, want, f);
    fclose(f);
    return got == want;
}

/* namcos22_v.cpp recalc_czram, via the reference model.
 *
 * The `continue` on a non-increasing entry is load-bearing: it skips the
 * small/large tracking AND the prev update, not just the fill loop. The
 * reference comment flags this as where MAME diverges on non-monotonic
 * czram, so it must be reproduced rather than restructured. */
static void recalc_bank(fog_state *fs, const uint16_t *czram, int bank)
{
    uint8_t *table = fs->recalc[bank];
    int reverse = ((fs->czattr[4] >> (bank * 4)) & 2) ? 0xff : 0;
    int small_val = 0x2000, small_off = reverse;
    int large_val = 0,      large_off = reverse ^ 0xff;
    int prev = 0;

    memset(table, 0, 0x2000);
    for (int i = 0; i < 0x100; i++) {
        int factor = i ^ reverse;
        int val = czram[factor];
        if (val > 0x2000) val = 0x2000;
        if (i > 0) {
            if (prev >= val) continue;          /* see note above */
            for (int j = prev; j < val; j++) table[j] = (uint8_t)factor;
        }
        if (val < small_val) { small_val = val; small_off = factor; }
        if (val > large_val) { large_val = val; large_off = factor; }
        prev = val;
    }
    for (int j = 0; j < small_val && j < 0x2000; j++) table[j] = (uint8_t)small_off;
    for (int j = large_val; j < 0x2000; j++)          table[j] = (uint8_t)large_off;
}

int fog_load_frame(const char *dir, int frame)
{
    fog_state fs;
    if (!fog_load_frame_into(&fs, dir, frame)) { g_fog_valid = 0; return 0; }
    g_fog = fs; g_fog_valid = 1;
    return 1;
}

int fog_load_frame_into(fog_state *out, const char *dir, int frame)
{
    char path[1024];
    uint8_t cz[16], mix[0x400], raw[0x200];
    fog_state fs;
    memset(&fs, 0, sizeof fs);

    snprintf(path, sizeof path, "%s/czattr_f%d.bin", dir, frame);
    if (!read_file(path, cz, sizeof cz)) return 0;
    for (int i = 0; i < 8; i++)                     /* 68020: big-endian */
        fs.czattr[i] = (uint16_t)((cz[i * 2] << 8) | cz[i * 2 + 1]);

    snprintf(path, sizeof path, "%s/mix_f%d.bin", dir, frame);
    if (!read_file(path, mix, sizeof mix)) return 0;
    fs.poly_fade[0] = mix[0x00]; fs.poly_fade[1] = mix[0x01]; fs.poly_fade[2] = mix[0x02];
    fs.poly_fade_enabled = !(mix[0x00] == 0xff && mix[0x01] == 0xff && mix[0x02] == 0xff);
    fs.fog_rgb[0] = mix[0x05]; fs.fog_rgb[1] = mix[0x06]; fs.fog_rgb[2] = mix[0x07];
    fs.bg[0] = mix[0x08]; fs.bg[1] = mix[0x09]; fs.bg[2] = mix[0x0a];
    fs.screen_fade[0] = mix[0x16]; fs.screen_fade[1] = mix[0x17]; fs.screen_fade[2] = mix[0x18];
    fs.screen_fade_factor = mix[0x19];
    fs.mixer_flags = mix[0x1a];
    fs.poly_alpha_color = mix[0x0f];
    fs.poly_alpha_pen   = mix[0x10];
    fs.text_alpha_lo   = mix[0x12];
    fs.text_alpha_hi   = mix[0x13];
    fs.text_alpha_mask = mix[0x14];
    fs.text_alpha      = mix[0x15];
    fs.text_palbase    = mix[0x1b];
    { int sf = mix[0x0e] << 8 | mix[0x0d]; fs.spot_factor = sf < 0x100 ? 0 : sf & 0xff; }
    memcpy(fs.gamma[0], mix + 0x100, 256);
    memcpy(fs.gamma[1], mix + 0x200, 256);
    memcpy(fs.gamma[2], mix + 0x300, 256);
    fs.have_gamma = 1;

    for (int bank = 0; bank < 4; bank++) {
        snprintf(path, sizeof path, "%s/czram%d_f%d.bin", dir, bank, frame);
        if (!read_file(path, raw, sizeof raw)) return 0;
        uint16_t words[256];
        for (int i = 0; i < 256; i++)
            words[i] = (uint16_t)((raw[i * 2] << 8) | raw[i * 2 + 1]);
        recalc_bank(&fs, words, bank);
    }

    *out = fs;
    return 1;
}

/* FogState.quad_fog */
int fog_quad(uint32_t color, int cz_type, const uint8_t **table, int *sdelta)
{
    if (!g_fog_valid) return 0;
    /* The gate is on the MIDDLE byte of the colour word, not the low one:
     * the oracle builds its quad with color=(color_raw >> 8) & 0xff and
     * then tests `color & 0x80`, so this is BIT 15 of the raw word -- the
     * flag that sits directly above the 7-bit palette group
     * ((color >> 8) & 0x7F). Testing bit 7 of the raw word instead fogged
     * every quad including the sky dome, which turned the sky cyan. */
    if (((color >> 8) & 0xff) & 0x80) return 0;  /* BIT(~color,7) gate */
    int bank = (g_fog.czattr[6] >> ((cz_type & 3) * 2)) & 3;
    if (!((g_fog.czattr[4] >> (bank * 4)) & 4)) return 0;

    int delta = g_fog.czattr[bank];
    /* sign-extend the low byte, exactly as the reference does: the 0x8000
     * test looks at the WORD's sign bit but the value taken is the low byte
     * either way. Simplifying this to a plain int8 cast changes the result. */
    delta = (delta & 0x8000) ? ((delta | 0xff00) - 0x10000) : (delta & 0x00ff);

    *table = g_fog.recalc[bank];
    *sdelta = delta;
    return 1;
}

int fog_alpha_for_z(const uint8_t *table, int sdelta, int32_t z)
{
    if (!table) return 255;
    int cz = (int)(z >> 8);
    if (cz < 0) cz = 0;
    if (cz > 0x1fff) cz = 0x1fff;
    int ff = table[cz] + sdelta;
    if (ff <= 0) return 255;                     /* no fog contribution */
    if (ff > 0xff) ff = 0xff;
    return 0xff - ff;                            /* blend() alpha */
}

void fog_apply_gamma(uint8_t *r, uint8_t *g, uint8_t *b)
{
    if (!g_fog_valid || !g_fog.have_gamma) return;
    *r = g_fog.gamma[0][*r];
    *g = g_fog.gamma[1][*g];
    *b = g_fog.gamma[2][*b];
}

/* The board's registers -> a fog_state. `mix` is the 0x400-byte video mixer (0x824000), `czattr` the eight
 * host-order words at 0x810000, `czram[b]` bank b's 256 host-order words. Both Super 22 boards (Prop Cycle,
 * Tokyo Wars) supply them; the byte-order and bank-window differences are the caller's. */
int fog_state_from_regs(fog_state *out, const uint8_t *mix, const uint16_t czattr[8], const uint16_t *const czram[4])
{
    fog_state fs;
    memset(&fs, 0, sizeof fs);
    for (int i = 0; i < 8; i++) fs.czattr[i] = czattr[i];

    fs.poly_fade[0] = mix[0x00]; fs.poly_fade[1] = mix[0x01]; fs.poly_fade[2] = mix[0x02];
    fs.poly_fade_enabled = !(mix[0x00] == 0xff && mix[0x01] == 0xff && mix[0x02] == 0xff);
    fs.fog_rgb[0] = mix[0x05]; fs.fog_rgb[1] = mix[0x06]; fs.fog_rgb[2] = mix[0x07];
    fs.bg[0] = mix[0x08]; fs.bg[1] = mix[0x09]; fs.bg[2] = mix[0x0a];
    fs.screen_fade[0] = mix[0x16]; fs.screen_fade[1] = mix[0x17]; fs.screen_fade[2] = mix[0x18];
    fs.screen_fade_factor = mix[0x19];
    fs.mixer_flags = mix[0x1a];
    fs.poly_alpha_color = mix[0x0f];
    fs.poly_alpha_pen   = mix[0x10];
    fs.text_alpha_lo   = mix[0x12];
    fs.text_alpha_hi   = mix[0x13];
    fs.text_alpha_mask = mix[0x14];
    fs.text_alpha      = mix[0x15];
    fs.text_palbase    = mix[0x1b];
    { int sf = mix[0x0e] << 8 | mix[0x0d]; fs.spot_factor = sf < 0x100 ? 0 : sf & 0xff; }
    memcpy(fs.gamma[0], mix + 0x100, 256);
    memcpy(fs.gamma[1], mix + 0x200, 256);
    memcpy(fs.gamma[2], mix + 0x300, 256);
    /* Only trust the LUTs if the game has actually written them. A live
     * boot starts with the mixer zeroed, and an all-zero LUT maps every
     * colour to black -- the whole frame renders black while the census
     * still reports thousands of quads and text pixels drawn. A capture
     * always has real tables; live memory does not until the game sets
     * them up. */
    fs.have_gamma = 0;
    for (int i = 0; i < 256 && !fs.have_gamma; i++)
        if (fs.gamma[0][i] || fs.gamma[1][i] || fs.gamma[2][i]) fs.have_gamma = 1;

    /* Prop Cycle exposes ONE czram window (0x810200) and builds all four bank tables from it -- MEASURED to be
     * unexercised there (cz_type and czattr[6] read 0 throughout; see src/pc_live2d.c). Tokyo Wars keeps four real
     * banks and hands them in separately. */
    for (int bank = 0; bank < 4; bank++) recalc_bank(&fs, czram[bank], bank);
    *out = fs;
    return 1;
}

int fog_load_regs(const uint8_t *mix, const uint16_t czattr[8], const uint16_t *const czram[4])
{
    fog_state fs;
    fog_state_from_regs(&fs, mix, czattr, czram);
    g_fog = fs;
    g_fog_valid = 1;
    return 1;
}
