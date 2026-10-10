/*
 * tc2_text.c -- the C361 TEXT LAYER, drawn into an RGBA frame: our own code, as MAME's namcos23.cpp draw_text_layer / mix_text_layer
 * -- from a frame snapshot (src/tc2_frame.h), composed with the polygons by src/tc2_render.c.
 *   tile map  0x0681E000: 64 x 64 big-endian 16-bit entries, 16x16 pixels each: xxxx palette | x flip-y | x flip-x | xx xxxx xxxx code
 *   characters 0x06800000: 32 bytes a row pair... each 16x16 character is 32 32-bit words, 4 bits a pixel, pen 0xF transparent
 *   palette   0x06A10000: pen n = (R, G, B) at byte 2n+1 of three 64 KB planes (MAME paletteram_w)
 *   C404      0x06A08000: words 8/9/10 background RGB, 0x14 text alpha mask, 0x15 text alpha factor, 0x16-0x18 fade RGB, 0x19 fade factor,
 *             0x1A fade flags (bit 1: fade the text), 0x1B palette base (<< 8, & 0x7F00)
 *   scroll    the C361's per-row x scroll and y scroll (src/tc2_screen.c)
 */
#include <stdint.h>
#include <string.h>
#include "tc2_frame.h"

static uint16_t be16p(const uint8_t *m) { return (uint16_t)(m[0] << 8 | m[1]); }
static uint32_t be32p(const uint8_t *m) { return (uint32_t)m[0] << 24 | (uint32_t)m[1] << 16 | (uint32_t)m[2] << 8 | m[3]; }
static int c404(const tc2_frame_state *fs, int w) { return fs->c404[2 * w] << 8 | fs->c404[2 * w + 1]; }
static int c404b(const tc2_frame_state *fs, int w) { return fs->c404[2 * w + 1]; }   /* the 8-bit registers */

/* the C361 layer into a pen map (MAME draw_text_layer): mix[] = the pen, pri[] = 4 where a pixel is opaque */
void tc2_text_layer(const tc2_frame_state *fs, uint16_t *mix, uint8_t *pri)
{
    const uint8_t *chr = fs->text, *map = fs->text + 0x1E000;
    const uint32_t palbase = ((uint32_t)c404(fs, 0x1B) << 8) & 0x7F00u;
    for (uint32_t y = 0; y < 480; y++) {
        const uint32_t sy = (y + fs->yscroll) & 0x3FF, tmy = sy >> 4, ty = sy & 0xF;
        const uint16_t sx = fs->linexscroll[sy];
        /* per 16-pixel tile span: the tile entry and the glyph row are the same for all of its pixels (they were fetched per pixel, ~20%
         * of a run); a span whose glyph row is all pen 0xF (transparent: the blank tiles, most of the screen) is skipped whole */
        for (int x = 0; x < 640; ) {
            const uint32_t px = ((uint32_t)x + sx) & 0x3FF, tmx = px >> 4, tx0 = px & 0xF;
            const int span = (int)(16 - tx0) < 640 - x ? (int)(16 - tx0) : 640 - x;
            const uint16_t t = be16p(map + 2u * ((tmy << 6) | (tmx & 0x3F)));
            const uint32_t code = (uint32_t)(t & 0x03FF) << 5;
            const uint32_t fxm = (t & 0x0400) ? 0x00 : 0x3C, fym = (t & 0x0800) ? 0x1E : 0x00;
            const uint32_t ca = code | ((ty << 1) ^ fym);
            const uint64_t cd = (uint64_t)be32p(chr + 4u * ca) << 32 | be32p(chr + 4u * (ca | 1));
            if (cd != ~0ull) {
                const uint16_t pen_hi = (uint16_t)(palbase | ((uint32_t)(t >> 12) << 4));
                for (int k = 0; k < span; k++) {
                    const uint32_t val = (uint32_t)(cd >> (((tx0 + (uint32_t)k) << 2) ^ fxm)) & 0xF;
                    if (val == 0xF) continue;
                    mix[y * 640 + x + k] = (uint16_t)(pen_hi | val);
                    pri[y * 640 + x + k] = 4;
                }
            }
            x += span;
        }
    }
}
/* the text pixels whose priority mark is prival, onto the frame (MAME mix_text_layer: the screen fade with fade flag bit 1, the text alpha) */
void tc2_text_mix(const tc2_frame_state *fs, uint32_t *rgba, const uint16_t *mix, const uint8_t *pri, int prival)
{
    const int fade = (c404b(fs, 0x1A) & 2) && c404b(fs, 0x19);
    const int ff = 0xFF - c404b(fs, 0x19), ffi = 0x100 - ff, fr = c404b(fs, 0x16), fg = c404b(fs, 0x17), fb = c404b(fs, 0x18);
    const int af = c404b(fs, 0x15), a_mask = c404b(fs, 0x14), c12 = c404b(fs, 0x12), c13 = c404b(fs, 0x13);
    const int alpha = 0xFF - af, alphai = 0x100 - alpha;
    for (int k = 0; k < 640 * 480; k++) {
        if (pri[k] != prival) continue;
        const uint32_t pen = mix[k], c = fs->pens[pen & 0x7FFF];
        int r = c >> 16 & 0xFF, g = c >> 8 & 0xFF, b = c & 0xFF;
        if (fade) { r = (r * ff + fr * ffi) >> 8; g = (g * ff + fg * ffi) >> 8; b = (b * ff + fb * ffi) >> 8; }
        if (af && ((int)(pen & 0xF) == a_mask || ((int)pen >= c12 && (int)pen <= c13))) {
            const uint32_t d = rgba[k];
            r = (r * alpha + (int)(d >> 16 & 0xFF) * alphai) >> 8; g = (g * alpha + (int)(d >> 8 & 0xFF) * alphai) >> 8; b = (b * alpha + (int)(d & 0xFF) * alphai) >> 8;
        }
        rgba[k] = (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
    }
}
/* the whole picture (src/tc2_render.c composes it): kept under its old name for the window */
void tc2_text_render(uint32_t *rgba) { void tc2_frame_rgba(uint32_t *); tc2_frame_rgba(rgba); }
