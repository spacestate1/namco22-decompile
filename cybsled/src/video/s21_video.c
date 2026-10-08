/*
 * s21_video.c -- Cyber Sled (Namco System 21): the picture. See s21_video.h for what it reproduces and the interface.
 *
 * Layout of the frame (MAME namcos21_c67 screen_update):
 *   video enable bit 6 clear  -> black
 *   pens = 0xFF; sprites priority 2; the visible 3D frame (pen != 0); pri1 = palette-ext word 0 bits 8-10:
 *     0 / 2: sprites priority 0;  else (4): priority-0 sprites into a copy of the picture, kept where the sprite pen has bit 12
 *     or 14 set and its priority-table depth <= the 3D z there, or where the pen is < 0x1000;  then sprites priority 3.
 * Sprite pixels are mixed by Cyber Sled's rule (src ^= 0xF00; low byte 0xFF = draw only over the backdrop, 0x00 / 0x01 =
 * shadows: the pen below moves to polygon bank 1 / 2 (0x4000 / 0x6000 | pen & 0x1FFF), else 0x1000 | src).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "s21_video.h"
#include "s21_video_int.h"

/* ---- the board's RAM (s21_video_bind) ---- */
static const uint16_t *v_pal, *v_palx, *v_spr, *v_pos, *v_ven;
void s21_video_bind(const uint16_t *pal, const uint16_t *palext, const uint16_t *spr, const uint16_t *sprpos, const uint16_t *vena)
{
    v_pal = pal; v_palx = palext; v_spr = spr; v_pos = sprpos; v_ven = vena;
}

/* C355: page-1 tables are mirrored into page 0 as they are written (namco_c355spr spriteram_w) */
void s21_video_spriteram_w(uint16_t *spr, uint32_t woff, uint16_t data, uint16_t mem_mask)
{
    woff &= 0xFFFF;
    spr[woff] = (uint16_t)((spr[woff] & ~mem_mask) | (data & mem_mask));
    if (woff >= 0x10000 / 2 && woff < 0x12000 / 2) { uint32_t o = woff - 0x10000 / 2; spr[o] = (uint16_t)((spr[o] & ~mem_mask) | (data & mem_mask)); }
    else if (woff >= 0x14000 / 2 && woff < 0x14400 / 2) { uint32_t o = woff - 0x12000 / 2; spr[o] = (uint16_t)((spr[o] & ~mem_mask) | (data & mem_mask)); }
}

/* ---- sprite tiles: 0x4000 tiles of 16x16, 8 bpp, ROM_LOAD32_BYTE obj0..3 at 0, obj4..7 at 0x200000 ---- */
#define TILE_BYTES 0x400000
static uint8_t *tiles;
static bool slurp(const char *dir, const char *name, uint8_t *dst, size_t n)
{
    char p[1024]; snprintf(p, sizeof p, "%s/%s", dir, name);
    FILE *f = fopen(p, "rb");
    if (!f) { fprintf(stderr, "[S21V] cannot open %s\n", p); return false; }
    size_t got = fread(dst, 1, n, f); fclose(f);
    if (got != n) { fprintf(stderr, "[S21V] short %s\n", p); return false; }
    return true;
}
bool s21_video_init(const char *romdir)
{
    static const char *obj[8] = { "cy1-obj0.5s", "cy1-obj1.5x", "cy1-obj2.3s", "cy1-obj3.3x",
                                  "cy1-obj4.4s", "cy1-obj5.4x", "cy1-obj6.2s", "cy1-obj7.2x" };
    if (!tiles) tiles = malloc(TILE_BYTES);
    uint8_t *chip = malloc(0x80000);
    if (!tiles || !chip) return false;
    for (int i = 0; i < 8; i++) {
        if (!slurp(romdir, obj[i], chip, 0x80000)) { free(chip); return false; }
        uint8_t *base = tiles + (i >= 4 ? 0x200000 : 0) + (i & 3);
        for (int k = 0; k < 0x80000; k++) base[k * 4] = chip[k];
    }
    free(chip);
    return true;
}

/* ---- the 3D rasteriser (namcos21_3d, 0x10 palettes, depth not reversed) ---- */
#define FB (S21_W * S21_H)
#define PENMASK 0x1e00
static uint16_t zbuf_a[FB], pens_a[FB], zbuf_b[FB], pens_b[FB];
static uint16_t *w_z = zbuf_a, *w_p = pens_a, *v_z = zbuf_b, *v_p = pens_b;      /* work / visible */
#define QMAX 16384
static s21_quad q_work[QMAX], q_vis[QMAX];   /* the frames' FRONT-FACING quads, for the GL picture (s21_video_gl.c) */
static uint16_t q_work_col[QMAX], q_vis_col[QMAX]; static int q_work_z[QMAX], q_vis_z[QMAX];
static int nq_work, nq_vis, nq_lost;
static bool sw3d = true;                     /* run the software rasteriser (the oracle); off = the GL picture draws the quads */
void s21_video_set_sw3d(bool on)
{
    if (on && !sw3d) for (int i = 0; i < FB; i++) { w_z[i] = v_z[i] = 0x8000; w_p[i] = v_p[i] = 0; }
    sw3d = on;
}

typedef struct { double x, y, z; } vtx;
typedef struct { double x, z; } edg;

static void scanline(const edg *e1, const edg *e2, int sy, uint16_t color, int zsort)
{
    if (e1->x > e2->x) { const edg *t = e1; e1 = e2; e2 = t; }
    uint16_t *dest = w_p + sy * S21_W, *zb = w_z + sy * S21_W;
    int x0 = (int)e1->x, x1 = (int)e2->x;
    if (x1 - x0) {
        if (x0 < 0) x0 = 0;
        if (x1 > S21_W) x1 = S21_W;
        for (int x = x0; x < x1; x++)
            if (zsort < zb[x]) { dest[x] = color; zb[x] = (uint16_t)zsort; }
    }
}
static void tri(const vtx *v0, const vtx *v1, const vtx *v2, uint16_t color, int zsort)
{
    for (;;) {
        if (v0->y > v1->y) { const vtx *t = v0; v0 = v1; v1 = t; }
        else if (v1->y > v2->y) { const vtx *t = v1; v1 = v2; v2 = t; }
        else break;
    }
    int ystart = (int)v0->y, yend = (int)v2->y, dy = yend - ystart;
    if (!dy) return;
    edg e1, e2;
    double dx2dy = (v2->x - v0->x) / dy, dz2dy = (v2->z - v0->z) / dy, dx1dy, dz1dy;
    e2.x = v0->x; e2.z = v0->z;
    int crop = -ystart;
    if (crop > 0) { e2.x += dx2dy * crop; e2.z += dz2dy * crop; }
    ystart = (int)v0->y; yend = (int)v1->y; dy = yend - ystart;
    if (dy) {
        e1.x = v0->x; e1.z = v0->z;
        dx1dy = (v1->x - v0->x) / dy; dz1dy = (v1->z - v0->z) / dy;
        crop = -ystart;
        if (crop > 0) { e1.x += dx1dy * crop; e1.z += dz1dy * crop; ystart = 0; }
        if (yend > S21_H) yend = S21_H;
        for (int y = ystart; y < yend; y++) {
            scanline(&e1, &e2, y, color, zsort);
            e2.x += dx2dy; e2.z += dz2dy; e1.x += dx1dy; e1.z += dz1dy;
        }
    }
    ystart = (int)v1->y; yend = (int)v2->y; dy = yend - ystart;
    if (dy) {
        e1.x = v1->x; e1.z = v1->z;
        dx1dy = (v2->x - v1->x) / dy; dz1dy = (v2->z - v1->z) / dy;
        crop = -ystart;
        if (crop > 0) { e1.x += dx1dy * crop; e1.z += dz1dy * crop; ystart = 0; }
        if (yend > S21_H) yend = S21_H;
        for (int y = ystart; y < yend; y++) {
            scanline(&e1, &e2, y, color, zsort);
            e2.x += dx2dy; e2.z += dz2dy; e1.x += dx1dy; e1.z += dz1dy;
        }
    }
}
void s21_video_quad(const s21_quad *q)
{
    const int64_t c1 = (int64_t)(q->sx[1] - q->sx[0]) * (q->sy[2] - q->sy[0]) - (int64_t)(q->sy[1] - q->sy[0]) * (q->sx[2] - q->sx[0]);
    const int64_t c2 = (int64_t)(q->sx[3] - q->sx[2]) * (q->sy[0] - q->sy[2]) - (int64_t)(q->sy[3] - q->sy[2]) * (q->sx[0] - q->sx[2]);
    if (c1 >= 0 && c2 >= 0) return;                         /* back-facing */
    const uint8_t code = (uint8_t)(q->color >> 8);
    uint16_t color = (uint16_t)(0x2000 | PENMASK | (q->color & 0xff));    /* polygon palettes from 0x2000 */
    if (!(code & 2)) color |= 0x100;                        /* 0x10 palettes of 0x200: bit 9 picks the half */
    int zsort = 0;
    for (int i = 0; i < 4; i++) zsort += q->z[i];
    zsort /= 4;
    if (zsort < 0) zsort = 0;
    color = (uint16_t)(color - (zsort >> 2 & PENMASK));    /* depth cue: a darker palette set further away */
    vtx v[4];
    for (int i = 0; i < 4; i++) { v[i].x = S21_W / 2 + q->sx[i]; v[i].y = S21_H / 2 + q->sy[i]; v[i].z = q->z[i]; }
    if (sw3d) {
        tri(&v[0], &v[1], &v[2], color, zsort);
        tri(&v[2], &v[3], &v[0], color, zsort);
    }
    if (nq_work < QMAX) { q_work[nq_work] = *q; q_work_col[nq_work] = color; q_work_z[nq_work] = zsort; nq_work++; }
    else nq_lost++;
}
void s21_video_swap(void)
{
    uint16_t *t;
    t = w_z; w_z = v_z; v_z = t;
    t = w_p; w_p = v_p; v_p = t;
    if (sw3d) for (int i = 0; i < FB; i++) { w_z[i] = 0x8000; w_p[i] = 0; }
    memcpy(q_vis, q_work, sizeof q_work[0] * nq_work); memcpy(q_vis_col, q_work_col, sizeof q_work_col[0] * nq_work);
    memcpy(q_vis_z, q_work_z, sizeof q_work_z[0] * nq_work);
    nq_vis = nq_work; nq_work = 0;
}
int s21_video_quads_last(void) { return nq_vis; }
const s21_quad *s21v_vis_quads(int *n, const uint16_t **col, const int **z) { *n = nq_vis; *col = q_vis_col; *z = q_vis_z; return q_vis; }
int s21v_quads_lost(void) { return nq_lost; }

/* ---- C355 sprites (System 21: colbase 0x1000, 16 colours x 256, scroll offsets (0, 0x20), unbuffered, external prifill) ---- */
#define COLBASE 0x1000
typedef struct {
    bool disable; int size, cx0, cx1, cy0, cy1, offset, color, pri; bool flipx, flipy;
    int tile[256], x[256], y[256], zoomx[256], zoomy[256];
    int fmt;                            /* the format entry (widescreen backdrop: the slices loaded next to it) */
} c355_spr;
static c355_spr spl[256];
static int nspl;
static uint16_t render_bm[FB];          /* (pri << 12) | ((pal + c) & 0xfff), 0xffff = empty */

static inline int sext(int v, int bits) { v &= (1 << bits) - 1; return (v ^ (1 << (bits - 1))) - (1 << (bits - 1)); }
static inline uint16_t sptable(int entry, int attr) { return v_spr[(0x0000 / 2) + ((entry << 3) + attr)]; }
static inline uint16_t splist(int entry)           { return v_spr[(0x2000 / 2) + entry]; }
static inline uint16_t spclip(int entry, int attr) { return v_spr[(0x2400 / 2) + ((entry << 2) + attr)]; }
static inline uint16_t spfmt(int entry, int attr)  { return v_spr[(0x4000 / 2) + (((entry << 2) + attr) & 0x1fff)]; }
static inline uint16_t sptile(int entry)           { return v_spr[(0x8000 / 2) + (entry & 0x7fff)]; }

static void single_sprite(uint16_t which, c355_spr *s)
{
    const uint16_t palette = sptable(which, 6);
    s->pri = (palette >> 4) & 0xf;
    const uint16_t fmt_off = sptable(which, 0) & 0x7ff;
    s->offset = sptable(which, 1);
    int hpos = sptable(which, 2), vpos = sptable(which, 3);
    uint16_t hsize = sptable(which, 4), vsize = sptable(which, 5);
    int xscroll = sext(v_pos[1], 10), yscroll = sext(v_pos[0], 9);          /* 480 lines: a 10-bit x scroll */
    xscroll += 0; yscroll += 0x20;
    hpos -= xscroll; vpos -= yscroll;
    const int ce = (palette >> 8) & 0xf;
    s->cx0 = spclip(ce, 0) - xscroll; s->cx1 = spclip(ce, 1) - xscroll;
    s->cy0 = spclip(ce, 2) - yscroll; s->cy1 = spclip(ce, 3) - yscroll;
    hpos = sext(hpos & 0x7ff, 11); vpos = sext(vpos & 0x7ff, 11);
    int tile_index = spfmt(fmt_off, 0);
    s->fmt = fmt_off;
    const uint16_t format = spfmt(fmt_off, 1);
    const int dx = spfmt(fmt_off, 2) & 0x1ff, dy = spfmt(fmt_off, 3) & 0x1ff;
    int num_cols = (format >> 4) & 0xf, num_rows = format & 0xf;
    if (num_cols == 0) num_cols = 0x10;
    const bool flipx = (hsize & 0x8000) != 0;
    hsize &= 0x3ff;
    if (hsize == 0) { s->disable = true; return; }
    uint32_t zoomx = ((uint32_t)hsize << 16) / (uint32_t)(num_cols * 16);
    int32_t dxz = (int32_t)(((uint32_t)(dx & 0xff) * zoomx + 0x8000) >> 16);
    if (dx & 0x100) dxz = -dxz;
    hpos += flipx ? dxz : -dxz;
    if (num_rows == 0) num_rows = 0x10;
    const bool flipy = (vsize & 0x8000) != 0;
    vsize &= 0x3ff;
    if (vsize == 0) { s->disable = true; return; }
    uint32_t zoomy = ((uint32_t)vsize << 16) / (uint32_t)(num_rows * 16);
    int32_t dyz = (int32_t)(((uint32_t)(dy & 0xff) * zoomy + 0x8000) >> 16);
    if (dy & 0x100) dyz = -dyz;
    vpos += flipy ? dyz : -dyz;
    s->flipx = flipx; s->flipy = flipy;
    s->size = num_rows * num_cols;
    s->color = palette & 15;
    uint32_t src_h = num_rows * 16, scr_h = vsize;
    int y = vpos, ind = 0;
    for (int row = 0; row < num_rows; row++) {
        const int th = (int)(16 * scr_h / src_h);
        zoomy = (scr_h << 16) / src_h;
        if (flipy) y -= th;
        uint32_t src_w = num_cols * 16, scr_w = hsize;
        int x = hpos;
        for (int col = 0; col < num_cols; col++) {
            const int tw = (int)(16 * scr_w / src_w);
            zoomx = (scr_w << 16) / src_w;
            if (flipx) x -= tw;
            const uint16_t t = sptile(tile_index++);
            s->tile[ind] = t;
            if (!(t & 0x8000)) { s->x[ind] = x << 16; s->y[ind] = y << 16; s->zoomx[ind] = (int)zoomx; s->zoomy[ind] = (int)zoomy; }
            if (!flipx) x += tw;
            scr_w -= tw; src_w -= 16; ind++;
        }
        if (!flipy) y += th;
        scr_h -= th; src_h -= 16;
    }
}

static void zoomdraw(int cx0, int cx1, int cy0, int cy1, uint32_t code, uint32_t color, bool flipx, bool flipy,
                     int hpos, int vpos, int hsize, int vsize, int prival, int sw, int sh)
{
    if (!hsize || !vsize || !sw || !sh) return;
    const uint32_t pal = COLBASE + 256 * (color % 16);
    const uint8_t *src_base = tiles + (size_t)(code % 0x4000) * 256;
    int dx = (16 << 16) / sw, dy = (16 << 16) / sh;
    int ex = hpos + sw, ey = vpos + sh, xib, yi;
    if (flipx) { xib = (sw - 1) * dx; dx = -dx; } else xib = 0;
    if (flipy) { yi = (sh - 1) * dy; dy = -dy; } else yi = 0;
    if (hpos < cx0) { int p = cx0 - hpos; hpos += p; xib += p * dx; }
    if (vpos < cy0) { int p = cy0 - vpos; vpos += p; yi += p * dy; }
    if (ex > cx1 + 1) ex = cx1 + 1;
    if (ey > cy1 + 1) ey = cy1 + 1;
    if (ex <= hpos) return;
    for (int y = vpos; y < ey; y++) {
        const uint8_t *src = src_base + (yi >> 16) * 16;
        uint16_t *dest = render_bm + y * S21_W;
        int xi = xib;
        for (int x = hpos; x < ex; x++) {
            const uint8_t c = src[xi >> 16];
            if (c != 0xff) dest[x] = (uint16_t)(((prival & 0xf) << 12) | ((pal + c) & 0xfff));
            xi += dx;
        }
        yi += dy;
    }
}

static void build_and_render_sprites(void)
{
    nspl = 0;
    for (int i = 0; i < 256; i++) {
        c355_spr *s = &spl[nspl++];
        s->disable = false;
        const uint16_t which = splist(i);
        single_sprite(which & 0xff, s);
        if (which & 0x100) break;
    }
    for (int i = 0; i < FB; i++) render_bm[i] = 0xffff;
    for (int k = 0; k < nspl; k++) {
        const c355_spr *s = &spl[k];
        if (s->disable) continue;
        int cx0 = s->cx0 < 0 ? 0 : s->cx0, cx1 = s->cx1 > S21_W - 1 ? S21_W - 1 : s->cx1;
        int cy0 = s->cy0 < 0 ? 0 : s->cy0, cy1 = s->cy1 > S21_H - 1 ? S21_H - 1 : s->cy1;
        if (cx0 > cx1 || cy0 > cy1) continue;
        for (int ind = 0; ind < s->size; ind++) {
            if (s->tile[ind] & 0x8000) continue;
            const int sh = (s->zoomy[ind] * 16 + 0x8000) >> 16, sw = (s->zoomx[ind] * 16 + 0x8000) >> 16;
            zoomdraw(cx0, cx1, cy0, cy1, (uint32_t)(s->tile[ind] + s->offset), (uint32_t)s->color, s->flipx, s->flipy,
                     s->x[ind] >> 16, s->y[ind] >> 16, s->zoomx[ind], s->zoomy[ind], s->pri, sw, sh);
        }
    }
}
int s21_video_sprites_last(void) { return nspl; }

/* the composed frame: palette indices, RGB, and which pass drew each pixel */
static uint16_t pens[FB], mixbm[FB];
static uint8_t rgb[FB * 3], from3d[FB], layer[FB];   /* layer: 0 backdrop, 1+pri sprite, 8 = 3D */

/* Cyber Sled's sprite mix (namcos21_c67 sprite_mix_callback) */
static inline bool mix(uint16_t *dest, uint16_t src, int srcpri, int pri)
{
    if ((srcpri & 3) != pri) return false;
    src ^= 0xf00;
    switch (src & 0xff) {
    case 0xff: if (*dest == 0xff) *dest = (uint16_t)((src & 0xf00) | 0xff); else return false; break;
    case 0x00: *dest = (*dest != 0xff) ? (uint16_t)(0x4000 | (*dest & 0x1fff)) : (uint16_t)((src & 0xf00) | 0x00); break;
    case 0x01: *dest = (*dest != 0xff) ? (uint16_t)(0x6000 | (*dest & 0x1fff)) : (uint16_t)((src & 0xf00) | 0x01); break;
    default: *dest = (uint16_t)(COLBASE | src); break;
    }
    return true;
}
static void draw_sprites(uint16_t *bm, int pri, uint8_t *from3d)
{
    for (int i = 0; i < FB; i++) {
        const uint16_t s = render_bm[i];
        if (s == 0xffff) continue;
        if (mix(&bm[i], s & 0xfff, (s >> 12) & 0xf, pri)) { if (from3d) from3d[i] = 0; if (bm == pens) layer[i] = (uint8_t)((1 + pri) | ((((s & 0xff) ^ 0) <= 1) ? 0x10 : 0)); }
    }
}

/* ---- the frame ---- */
void s21_video_begin_frame(void) {}

static void compose(void)
{
    memset(from3d, 0, sizeof from3d); memset(layer, 0, sizeof layer);
    if (!v_ven || !(v_ven[0] & 0x40)) {                           /* video disabled: black */
        for (int i = 0; i < FB; i++) pens[i] = 0xffff;            /* (0xffff: the black pen marker below) */
        return;
    }
    for (int i = 0; i < FB; i++) pens[i] = 0xff;
    build_and_render_sprites();
    draw_sprites(pens, 2, NULL);
    for (int i = 0; i < FB; i++) if (v_p[i]) { pens[i] = v_p[i]; from3d[i] = 1; layer[i] = 8; }
    const int pri1 = (v_palx[0] >> 8) & 7;
    if (pri1 == 0 || pri1 == 2) draw_sprites(pens, 0, from3d);
    else {
        uint16_t pri[16];
        for (int i = 0; i < 16; i++) pri[i] = (uint16_t)(i == 0 ? 0x7fc0 : pri[i - 1] / 1.24);
        memcpy(mixbm, pens, sizeof pens);
        draw_sprites(mixbm, 0, NULL);
        for (int i = 0; i < FB; i++) {
            const uint16_t s = mixbm[i];
            if (((s & 0x5000) && pri[s >> 8 & 0xf] <= v_z[i]) || s < 0x1000) {
                if (s != pens[i]) { from3d[i] = 0; layer[i] = 1; }
                pens[i] = s;
            }
        }
    }
    draw_sprites(pens, 3, from3d);
}
static void to_rgb(void)
{
    for (int i = 0; i < FB; i++) {
        uint8_t *o = rgb + i * 3;
        if (pens[i] == 0xffff) { o[0] = o[1] = o[2] = 0; continue; }
        const int p = pens[i] & 0x7fff;
        const uint16_t m = v_pal[p], x = v_palx[p];
        o[0] = (uint8_t)(m >> 8); o[1] = (uint8_t)m; o[2] = (uint8_t)x;    /* xBRG_888: R = main high, G = main low, B = ext low */
    }
}

/* ---- the GL picture's CPU half: the 2D mixed symbolically (see s21_video.h), the palette as RGB ----
 * Per board pixel at most ONE sprite pixel survives (render_bm keeps the last sprite drawn there, whatever its priority), so each
 * pixel takes at most one mix() and the result is a pair:
 *   no polygon there:  backdrop 0xFF mixed with that sprite pixel (the priority-0 z-mix keeps everything when the z buffer is
 *                      empty: every threshold is <= 0x7FC0 < the cleared 0x8000) -- a concrete pen;
 *   a polygon pen P:   priority 2 is under the 3D (P stays); an 0xFF-low "backdrop only" sprite fails (P is not 0xFF); a shadow
 *                      moves P to bank 0x4000 / 0x6000; anything else replaces it -- and with palette-ext pri1 = 4 a priority-0
 *                      result is kept only where the z-mix test passes on THAT polygon pixel's z, so the decision is the shader's.
 * Encoding (RGBA8): R,G = the no-polygon pen; B,A = the op: 0xFFFF keep P, 0xFFFE / 0xFFFD shadow bank 0x4000 / 0x6000, a pen
 * 0x1000..0x1FFF replace, and the same three made conditional on the z-mix: 0xFFFC / 0xFFFB / 0x8000 | pen. */
static uint8_t gl2d[FB * 4] __attribute__((aligned(16))), palrgb[0x8000 * 3];
static uint16_t gl_pritab[16];
static int gl_video_on, gl_zmix;
/* the per-pixel encoding above, for one sprite-bitmap value (a pure function of it and gl_zmix) */
static uint32_t encode_px(uint16_t s)
{
    {
        if (s == 0xffff) return 0xffff00ffu;                     /* no sprite: backdrop 0xFF / keep the polygon */
        uint16_t nod = 0xff, op = 0xffff;
        const int sp = (s >> 12) & 0xf, p = sp & 3;
        if (p != 1) {
            mix(&nod, s & 0xfff, sp, p);
            if (p != 2) {
                const uint16_t src = (uint16_t)((s & 0xfff) ^ 0xf00);
                switch (src & 0xff) {
                case 0xff: op = 0xffff; break;
                case 0x00: op = 0xfffe; break;
                case 0x01: op = 0xfffd; break;
                default: op = (uint16_t)(COLBASE | src); break;
                }
                if (p == 0 && gl_zmix && op != 0xffff) op = op == 0xfffe ? 0xfffc : op == 0xfffd ? 0xfffb : (uint16_t)(op | 0x8000);
            }
        }
        return (uint32_t)nod | (uint32_t)op << 16;
    }
}
/* ... for a bitmap of n pixels, through a table of all 65536 values per z-mix mode (built once: 2 x 256 KB) */
static void encode_2d(const uint16_t *bm, uint32_t *o32, int n)
{
    static uint32_t *lut[2];
    uint32_t *t = lut[gl_zmix != 0];
    if (!t) { t = lut[gl_zmix != 0] = malloc(65536 * sizeof *t); for (int v = 0; v < 65536; v++) t[v] = encode_px((uint16_t)v); }
    for (int i = 0; i < n; i++) o32[i] = t[bm[i]];
}

/* ---- WIDESCREEN (Hor+, PLAN.md "Module H"): the sky across the side strips ----
 * The sky of every stage is a PANORAMA of 256-px slices, each a priority-2 sprite (under the 3D) of 16 x 16 tiles: the stage loads
 * its slices into format entries 0x460.. once (static for the stage: attract stage A05D 4474 50B2 B6CF FF8D, by tile-list hash) and
 * every frame shows THREE of them edge to edge, slid with the heading and clipped to the 4:3 screen (sprite table entries 1-3). The
 * order is not a ring of the loaded slices: the attract stage shows ... 462 460 ... and ... 462 463 ... (MAME's own sprite RAM shows
 * the same), so the slice beside the view is not computable from the three on screen. It is what the game itself showed there:
 *   - while the edge slice stays the edge slice, its neighbour stays what it was;
 *   - when the view slides, the slice that just left the view on a side is the neighbour on that side;
 *   - otherwise the neighbour seen beside that slice before, if the game has only ever shown ONE there;
 *   - otherwise (a stage's first frames, before the game has shown it) the loaded slice next to it in format order -- the load
 *     order, which is the panorama's order except where the game repeats slices (then it may be the other candidate).
 * The neighbour is drawn from a snapshot of its sprite (taken whenever it was on screen) into the strips beside the board
 * (W2 = 496 + 2E wide, the board in the middle) and the board columns the clip window left empty, never over a pixel a sprite drew.
 * HUD, text, logos and sprites at other priorities stay where the board drew them. E = 0: none of this runs. */
#define WE_MAX 1024
static int wide_e;
void s21_video_set_wide_extra(int e) { wide_e = e < 0 ? 0 : e > WE_MAX ? WE_MAX : e; }
static uint16_t wbm[(S21_W + 2 * WE_MAX) * S21_H];
static uint32_t gl2dw[(S21_W + 2 * WE_MAX) * S21_H];
static int gl2d_w = S21_W;                              /* the width of what s21v_gl2d() returns */
static int wide_chain_n, wide_side[2], wide_miss[2], wide_guess[2];   /* the last frame: chain length, neighbours drawn / guessed / missing (CS_WIDELOG) */

static void sprite_extent(const c355_spr *s, int *x0, int *x1)
{
    *x0 = 1 << 30; *x1 = -(1 << 30);
    for (int ind = 0; ind < s->size; ind++) {
        if (s->tile[ind] & 0x8000) continue;
        const int x = s->x[ind] >> 16, w = (s->zoomx[ind] * 16 + 0x8000) >> 16;
        if (x < *x0) *x0 = x;
        if (x + w > *x1) *x1 = x + w;
    }
}
static uint32_t slice_key(const c355_spr *s)           /* the slice's identity: its tile list, palette, size */
{
    uint32_t h = 2166136261u;
    for (int q = 0; q < s->size; q++) h = (h ^ (uint32_t)(s->tile[q] & 0xffff)) * 16777619u;
    h = (h ^ (uint32_t)s->offset) * 16777619u; h = (h ^ (uint32_t)(s->color | s->size << 4 | s->pri << 16)) * 16777619u;
    return h ? h : 1;
}
#define NSLICE 32
static struct { uint32_t key; c355_spr spr; int w; uint32_t nb[2][4]; int nnb[2]; } slc[NSLICE];   /* nb[0] left, nb[1] right */
static int nslc;
static int slice_find(uint32_t key) { for (int i = 0; i < nslc; i++) if (slc[i].key == key) return i; return -1; }
static int slice_note(const c355_spr *s, uint32_t key, int w)
{
    int i = slice_find(key);
    if (i < 0) { i = nslc < NSLICE ? nslc++ : (int)(key % NSLICE); slc[i].key = key; slc[i].nnb[0] = slc[i].nnb[1] = 0; }
    slc[i].spr = *s; slc[i].w = w;                      /* the latest look (position irrelevant: drawn relative to its own extent) */
    return i;
}
static void slice_pair(int side, uint32_t a, uint32_t b)   /* b seen on `side` of a */
{
    const int i = slice_find(a);
    if (i < 0) return;
    for (int k = 0; k < slc[i].nnb[side]; k++) if (slc[i].nb[side][k] == b) return;
    if (slc[i].nnb[side] < 4) slc[i].nb[side][slc[i].nnb[side]++] = b;
}
/* draw slice snapshot `s` with its left edge at board x `xl`, outside [ex0, ex1], only into empty pixels of wbm */
static void slice_draw(const c355_spr *s, int xl, int ex0, int ex1, int cy0, int cy1)
{
    const int W2 = S21_W + 2 * wide_e;
    int sx0, sx1; sprite_extent(s, &sx0, &sx1);
    const uint32_t pal = COLBASE + 256 * ((uint32_t)s->color % 16);
    for (int ind = 0; ind < s->size; ind++) {
        if (s->tile[ind] & 0x8000) continue;
        const uint32_t code = (uint32_t)(s->tile[ind] + s->offset) % 0x4000;
        const int sw = (s->zoomx[ind] * 16 + 0x8000) >> 16, sh = (s->zoomy[ind] * 16 + 0x8000) >> 16;
        if (!sw || !sh) continue;
        const uint8_t *src_base = tiles + (size_t)code * 256;
        int dx = (16 << 16) / sw, dy = (16 << 16) / sh, xib, yi;
        if (s->flipx) { xib = (sw - 1) * dx; dx = -dx; } else xib = 0;
        if (s->flipy) { yi = (sh - 1) * dy; dy = -dy; } else yi = 0;
        const int hp = (s->x[ind] >> 16) - sx0 + xl, vp = s->y[ind] >> 16;
        if (hp >= ex0 && hp + sw <= ex1 + 1) continue;          /* all inside where the board drew it */
        int y0 = vp, y1 = vp + sh;
        if (y0 < cy0) { yi += (cy0 - y0) * dy; y0 = cy0; }
        if (y1 > cy1 + 1) y1 = cy1 + 1;
        if (hp + sw <= -wide_e || hp >= S21_W + wide_e || y0 >= y1) continue;
        for (int y = y0; y < y1; y++, yi += dy) {
            const uint8_t *src = src_base + (yi >> 16) * 16;
            uint16_t *dest = wbm + (size_t)y * W2 + wide_e;
            for (int seg = 0; seg < 2; seg++) {               /* the tile's columns left, then right of the board's own [ex0, ex1] */
                int xa = seg ? (hp > ex1 + 1 ? hp : ex1 + 1) : (hp > -wide_e ? hp : -wide_e);
                int xb = seg ? (hp + sw < S21_W + wide_e ? hp + sw : S21_W + wide_e) : (hp + sw < ex0 ? hp + sw : ex0);
                int xi = xib + (xa - hp) * dx;
                for (int x = xa; x < xb; x++, xi += dx) {
                    if (dest[x] != 0xffff) continue;
                    const uint8_t c = src[xi >> 16];
                    if (c != 0xff) dest[x] = (uint16_t)(((s->pri & 0xf) << 12) | ((pal + c) & 0xfff));
                }
            }
        }
    }
}
static uint32_t prev_chain[8]; static int prev_n; static uint32_t prev_nb[2];   /* last frame's chain (keys, left to right) and neighbours */
static void wide_backdrop(void)
{
    /* the chain: priority-2 sprites of the same shape (size, zoom) laid edge to edge */
    int mem[8], xs[8], nm = 0, w = 0;
    for (int k = 0; k < nspl && nm < 8; k++) {
        const c355_spr *a = &spl[k];
        if (a->disable || (a->pri & 3) != 2 || a->size < 1) continue;
        int x0, x1; sprite_extent(a, &x0, &x1);
        if (x1 <= x0) continue;
        if (!nm) w = x1 - x0;
        else if (x1 - x0 != w || a->size != spl[mem[0]].size) continue;
        int n = nm++;
        while (n > 0 && xs[n - 1] > x0) { xs[n] = xs[n - 1]; mem[n] = mem[n - 1]; n--; }
        xs[n] = x0; mem[n] = k;
    }
    wide_chain_n = 0; wide_side[0] = wide_side[1] = wide_miss[0] = wide_miss[1] = wide_guess[0] = wide_guess[1] = 0;
    for (int m = 1; m < nm; m++) if (xs[m] - xs[m - 1] != w) nm = 0;   /* not one edge-to-edge chain: leave it */
    if (nm < 2) { prev_n = 0; return; }
    wide_chain_n = nm;
    uint32_t key[8];
    for (int m = 0; m < nm; m++) { key[m] = slice_key(&spl[mem[m]]); slice_note(&spl[mem[m]], key[m], w); }
    for (int m = 1; m < nm; m++) { slice_pair(1, key[m - 1], key[m]); slice_pair(0, key[m], key[m - 1]); }
    uint32_t nb[2] = { 0, 0 };
    for (int side = 0; side < 2; side++) {
        const uint32_t edge = side ? key[nm - 1] : key[0];
        if (prev_n && (side ? prev_chain[prev_n - 1] : prev_chain[0]) == edge) nb[side] = prev_nb[side];      /* same edge slice */
        else for (int m = 0; m < prev_n && !nb[side]; m++)                                                     /* the view slid */
            if (prev_chain[m] == edge) { const int o = side ? m + 1 : m - 1; if (o >= 0 && o < prev_n) nb[side] = prev_chain[o]; }
        const int i = slice_find(edge);
        if (!nb[side] && i >= 0 && slc[i].nnb[side] == 1) nb[side] = slc[i].nb[side][0];                     /* only ever one */
        if (nb[side]) slice_pair(side, edge, nb[side]);
    }
    memcpy(prev_chain, key, sizeof key[0] * nm); prev_n = nm; prev_nb[0] = nb[0]; prev_nb[1] = nb[1];
    const c355_spr *s = &spl[mem[0]];
    const int ex0 = s->cx0 < 0 ? 0 : s->cx0, ex1 = s->cx1 > S21_W - 1 ? S21_W - 1 : s->cx1;   /* where the board drew the chain */
    const int cy0 = s->cy0 < 0 ? 0 : s->cy0, cy1 = s->cy1 > S21_H - 1 ? S21_H - 1 : s->cy1;
    if (cy0 > cy1) return;
    for (int m = 0; m < nm; m++) slice_draw(&spl[mem[m]], xs[m], ex0, ex1, cy0, cy1);   /* the chain into the columns its clip left empty */
    /* not shown by the game yet (a stage's first frames): the loaded slice next to the edge one in format order -- the stage's load
     * order, which is the panorama's order except where the game repeats slices (the guess can then be the other candidate) */
    static c355_spr guess[2];
    bool guessed[2] = { false, false };
    for (int side = 0; side < 2; side++) {
        if (nb[side]) continue;
        const c355_spr *e = &spl[mem[side ? nm - 1 : 0]];
        const int shape = spfmt(e->fmt, 1);
        int lo = e->fmt, hi = e->fmt;
        while (lo > e->fmt - 16 && lo > 0 && spfmt(lo - 1, 1) == shape) lo--;
        while (hi < e->fmt + 16 && hi < 0x7ff && spfmt(hi + 1, 1) == shape) hi++;
        if (hi - lo < 2) continue;
        const int f = side ? (e->fmt == hi ? lo : e->fmt + 1) : (e->fmt == lo ? hi : e->fmt - 1);
        guess[side] = *e;
        const int ti = spfmt(f, 0);
        for (int q = 0; q < e->size; q++) { const uint16_t t = sptile(ti + q); if ((t & 0x8000) != (e->tile[q] & 0x8000)) { guessed[side] = false; goto next; } guess[side].tile[q] = t; }
        guessed[side] = true;
      next:;
    }
    wide_miss[0] = !nb[0] && !guessed[0] && xs[0] > -wide_e; wide_miss[1] = !nb[1] && !guessed[1] && xs[nm - 1] + w < S21_W + wide_e;   /* a strip part left at the backdrop pen */
    for (int side = 0; side < 2; side++) {
        uint32_t k = nb[side]; int xl = side ? xs[nm - 1] + w : xs[0] - w;
        if (!k && guessed[side] && (side ? xl < S21_W + wide_e : xl + w > -wide_e)) { slice_draw(&guess[side], xl, ex0, ex1, cy0, cy1); wide_guess[side]++; }
        for (int depth = 0; k && depth < 4; depth++) {          /* the neighbour, and its own neighbours while the strip needs them */
            if (side ? xl >= S21_W + wide_e : xl + w <= -wide_e) break;
            const int i = slice_find(k);
            if (i < 0) break;
            slice_draw(&slc[i].spr, xl, ex0, ex1, cy0, cy1);
            wide_side[side]++;
            k = slc[i].nnb[side] == 1 ? slc[i].nb[side][0] : 0;
            xl += side ? w : -w;
        }
    }
}

static void prepare_gl(bool sprites_built)
{
    gl_video_on = v_ven && (v_ven[0] & 0x40);
    if (!gl_video_on) return;
    if (!sprites_built) build_and_render_sprites();
    const int pri1 = (v_palx[0] >> 8) & 7;
    gl_zmix = !(pri1 == 0 || pri1 == 2);
    for (int i = 0; i < 16; i++) gl_pritab[i] = (uint16_t)(i == 0 ? 0x7fc0 : gl_pritab[i - 1] / 1.24);
    if (!wide_e) { encode_2d(render_bm, (uint32_t *)gl2d, FB); gl2d_w = S21_W; }
    else {                                              /* widescreen: the board in the middle of a W2-wide 2D, the backdrop continued */
        const int W2 = S21_W + 2 * wide_e;
        for (int y = 0; y < S21_H; y++) {
            uint16_t *r = wbm + (size_t)y * W2;
            for (int x = 0; x < wide_e; x++) { r[x] = 0xffff; r[wide_e + S21_W + x] = 0xffff; }
            memcpy(r + wide_e, render_bm + (size_t)y * S21_W, S21_W * sizeof *wbm);
        }
        wide_backdrop();
        encode_2d(wbm, gl2dw, W2 * S21_H);
        gl2d_w = W2;
        static int lg = -1; if (lg < 0) lg = getenv("CS_WIDELOG") != NULL;
        if (lg) fprintf(stderr, "[WIDE2D] E %d: sky chain %d slices, neighbours drawn left %d right %d, guessed left %d right %d, missing left %d right %d\n", wide_e, wide_chain_n, wide_side[0], wide_side[1], wide_guess[0], wide_guess[1], wide_miss[0], wide_miss[1]);
    }
    for (int p = 0; p < 0x8000; p++) {
        const uint16_t m = v_pal[p], x = v_palx[p];
        palrgb[p * 3 + 0] = (uint8_t)(m >> 8); palrgb[p * 3 + 1] = (uint8_t)m; palrgb[p * 3 + 2] = (uint8_t)x;
    }
}
const uint8_t *s21v_gl2d(void) { return gl2d_w == S21_W ? gl2d : (const uint8_t *)gl2dw; }
int s21v_gl2d_w(void) { return gl2d_w; }
const uint8_t *s21v_palrgb(void) { return palrgb; }
const uint16_t *s21v_pritab(void) { return gl_pritab; }
int s21v_video_on(void) { return gl_video_on; }
int s21v_zmix(void) { return gl_zmix; }

void s21_video_end_frame(int mode)
{
    if (mode & S21_OUT_SW) { compose(); to_rgb(); }
    if (mode & S21_OUT_GL) prepare_gl(mode & S21_OUT_SW);    /* compose() has drawn this frame's sprites into render_bm already */
}

const uint8_t *s21_video_rgb(void) { return rgb; }
const uint16_t *s21_video_pens(void) { return pens; }
const uint8_t *s21_video_layers(void) { return layer; }
bool s21_video_write_ppm(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    fprintf(f, "P6\n%d %d\n255\n", S21_W, S21_H);
    fwrite(rgb, 1, sizeof rgb, f);
    fclose(f);
    return true;
}
