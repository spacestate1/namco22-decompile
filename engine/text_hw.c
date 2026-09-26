/*
 * text_hw.c — SS22 text tilemap, ported from render_text.py.
 *
 * Spec (namcos22_v.cpp line refs via that model):
 *   tilemap 64x64 tiles of 16x16 px, TILEMAP_SCAN_ROWS
 *   tile word (BE u16 in textram): [15:12] pal [11] flipy [10] flipx [9:0] code
 *   transparent pen 0xF
 *   chars 4bpp packed, 128 B/char, 8 B/row; in CPU (BE) byte order the
 *     addressing is PLAIN -- pixel x in byte x>>1, even x in the high nibble
 *   pen = palbase | tilepal<<4 | pix,  palbase = mixer[0x1B]<<8 & 0x7F00
 *   scroll x = (attr[0]-0x35C)&0x3FF, y = attr[1]&0x3FF
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "text_hw.h"

#define TW 640
#define TH 480

void text_free(text_state *st)
{
    if (!st) return;
    if (st->owns_cg) free((void *)st->cgram);
    st->cgram = NULL; st->owns_cg = 0;
    if (st->owns_pal) free((void *)st->pal);
    st->pal = NULL; st->owns_pal = 0; st->valid = 0;
}

static uint8_t *slurp(const char *path, size_t want)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    uint8_t *b = malloc(want);
    if (!b) { fclose(f); return NULL; }
    size_t got = fread(b, 1, want, f);
    fclose(f);
    if (got != want) { free(b); return NULL; }
    return b;
}

int text_load_frame_into(text_state *st, const char *dir, int frame,
                         const uint8_t *share_pal, const uint8_t *share_cg)
{
    char path[1024];
    memset(st, 0, sizeof *st);

    snprintf(path, sizeof path, "%s/cgram_f%d.bin", dir, frame);
    {   /* 128 KB a frame; usually byte-identical to the previous one, so
         * share rather than carry ~150 MB of duplicates in a long preload. */
        uint8_t *c = slurp(path, 0x20000);
        if (!c) return 0;
        if (share_cg && memcmp(share_cg, c, 0x20000) == 0) {
            free(c); st->cgram = share_cg; st->owns_cg = 0;
        } else { st->cgram = c; st->owns_cg = 1; }
    }

    snprintf(path, sizeof path, "%s/tattr_f%d.bin", dir, frame);
    { uint8_t *a = slurp(path, 0x10);
      if (!a) { text_free(st); return 0; }
      for (int i = 0; i < 8; i++) st->attr[i] = (uint16_t)((a[i*2] << 8) | a[i*2+1]);
      free(a); }

    snprintf(path, sizeof path, "%s/pal_f%d.bin", dir, frame);
    { uint8_t *p = slurp(path, 0x18000);
      if (!p) { text_free(st); return 0; }
      if (share_pal && memcmp(share_pal, p, 0x18000) == 0) {
          free(p); st->pal = share_pal; st->owns_pal = 0;
      } else { st->pal = p; st->owns_pal = 1; }
    }
    st->valid = 1;
    return 1;
}

void text_render(const text_state *st, const fog_state *fog,
                 uint8_t *rgba, int gate_mode)
{
    memset(rgba, 0, (size_t)TW * TH * 4);
    if (!st || !st->valid) return;

    const uint8_t *cg = st->cgram;
    const uint8_t *textram = cg + 0x1E000;
    const uint8_t *pal = st->pal;

    int palbase = (fog->text_palbase << 8) & 0x7F00;
    int sx = (st->attr[0] - 0x35C) & 0x3FF;
    int sy = st->attr[1] & 0x3FF;

    int fade_en = ((fog->mixer_flags & 2) != 0) && fog->screen_fade_factor;
    int fade_f = fog->screen_fade_factor;
    int alpha_f = fog->text_alpha;

    if (gate_mode) {                       /* mixer background, gamma applied */
        uint8_t b0 = fog->bg[0], b1 = fog->bg[1], b2 = fog->bg[2];
        if (fog->have_gamma) { b0 = fog->gamma[0][b0]; b1 = fog->gamma[1][b1];
                               b2 = fog->gamma[2][b2]; }
        for (long i = 0; i < (long)TW * TH; i++) {
            rgba[i*4] = b0; rgba[i*4+1] = b1; rgba[i*4+2] = b2; rgba[i*4+3] = 255;
        }
    }

    for (int y = 0; y < TH; y++) {
        int ty = (y + sy) & 0x3FF;
        int trow = ty >> 4, cy = ty & 15;
        for (int x = 0; x < TW; x++) {
            int tx = (x + sx) & 0x3FF;
            int ti = (trow * 64 + (tx >> 4)) * 2;
            uint16_t d = (uint16_t)((textram[ti] << 8) | textram[ti + 1]);
            int code = d & 0x3FF;
            int tpal = d >> 12;
            int cx = tx & 15;
            if (d & 0x400) cx = 15 - cx;
            int ccy = (d & 0x800) ? 15 - cy : cy;

            /* 4bpp packed: pixel x in byte x>>1, EVEN x in the high nibble */
            const uint8_t *rb = cg + code * 128 + ccy * 8;
            uint8_t byte = rb[cx >> 1];
            int pix = (cx & 1) ? (byte & 0xF) : (byte >> 4);
            if (pix == 0xF) continue;                 /* transparent pen */

            int pen = (palbase | (tpal << 4) | pix) & 0x7FFF;
            int r = pal[pen], g = pal[pen + 0x8000], b = pal[pen + 0x10000];
            if (fade_en) {
                r = (r * (0xFF - fade_f) + fog->screen_fade[0] * fade_f) / 0xFF;
                g = (g * (0xFF - fade_f) + fog->screen_fade[1] * fade_f) / 0xFF;
                b = (b * (0xFF - fade_f) + fog->screen_fade[2] * fade_f) / 0xFF;
            }
            long o = ((long)y * TW + x) * 4;
            int out_a = 255;
            if (alpha_f) {
                int am = fog->text_alpha_mask & 0xF;
                int p8 = pen & 0xFF;
                if ((p8 & 0xF) == am ||
                    (fog->text_alpha_lo <= p8 && p8 <= fog->text_alpha_hi)) {
                    if (gate_mode) {
                        int br = rgba[o], bg2 = rgba[o+1], bb = rgba[o+2];
                        r = (r * (0xFF - alpha_f) + br  * alpha_f) / 0xFF;
                        g = (g * (0xFF - alpha_f) + bg2 * alpha_f) / 0xFF;
                        b = (b * (0xFF - alpha_f) + bb  * alpha_f) / 0xFF;
                    } else {
                        /* defer: blend factor is (0xFF - alpha_f) on the source */
                        out_a = 0xFF - alpha_f;
                    }
                }
            }
            if (gate_mode && fog->have_gamma) {
                r = fog->gamma[0][r]; g = fog->gamma[1][g]; b = fog->gamma[2][b];
            }
            rgba[o] = (uint8_t)r; rgba[o+1] = (uint8_t)g;
            rgba[o+2] = (uint8_t)b; rgba[o+3] = (uint8_t)out_a;
        }
    }
}

/* Point a text_state at the board's RAM: `cg` is the 0x20000-byte CGRAM with the 0x2000-byte text RAM as its tail (the
 * model indexes cg[0x1E000:0x20000] as textram), `pal` the planar palette, `attr` the eight host-order tilemap
 * attribute words. Nothing is copied; the caller keeps them valid while the state is used. */
int text_load_regs(text_state *st, const uint8_t *cg, const uint8_t *pal, const uint16_t attr[8])
{
    memset(st, 0, sizeof *st);
    st->cgram = cg; st->owns_cg = 0;
    st->pal = pal; st->owns_pal = 0;
    for (int i = 0; i < 8; i++) st->attr[i] = attr[i];
    st->valid = 1;
    return 1;
}
