/*
 * sprite_hw.c — C374 sprite layer, ported from pc_sprite_model.py.
 *
 * Every step mirrors that model deliberately, including the awkward parts;
 * where this file and that Python disagree, the Python is right. It is in
 * turn a port of MAME's draw_sprites chain and is gated pixel-exact against
 * MAME's own composed frames.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "propcycl.h"
#include "sprite_hw.h"

/* MAME nthword over a u32 array: halfword i in big-endian order. */
static inline uint32_t nthword(const uint32_t *w, int i)
{
    return (w[i >> 1] >> (16 * (~i & 1))) & 0xFFFF;
}

static inline int s16(uint32_t v) { return (v & 0x8000) ? (int)v - 0x10000 : (int)v; }

/* rgbaint_t::blend: this*factor + other*(256-factor), >>8 per channel. */
static inline void blend3(uint8_t *c, const uint8_t *o, int factor)
{
    int f = factor & 0xFF;
    for (int k = 0; k < 3; k++)
        c[k] = (uint8_t)(((int)c[k] * f + (int)o[k] * (256 - f)) >> 8);
}

static uint32_t *read_words(const char *path, int *nwords)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    int words = (int)(n / 4);
    uint32_t *w = calloc(words ? words : 1, sizeof *w);
    if (!w) { fclose(f); return NULL; }
    for (int i = 0; i < words; i++) {
        unsigned char b[4];
        if (fread(b, 1, 4, f) != 4) break;
        w[i] = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
               ((uint32_t)b[2] << 8) | b[3];
    }
    fclose(f);
    *nwords = words;
    return w;
}

static uint32_t *live_spr, *live_vicsd;

void sprite_free(sprite_state *st)
{
    if (!st) return;
    /* live states borrow their buffers; only file-loaded ones own them */
    if (st->spr   != live_spr)   free(st->spr);
    if (st->vicsd != live_vicsd) free(st->vicsd);
    st->spr = NULL; st->vicsd = NULL;
    if (st->owns_pal) free((void *)st->pal);
    st->pal = NULL; st->owns_pal = 0; st->valid = 0;
}

int sprite_load_frame_into(sprite_state *st, const char *dir, int frame,
                           const uint8_t *share_pal)
{
    char path[1024];
    memset(st, 0, sizeof *st);

    snprintf(path, sizeof path, "%s/sprite_f%d.bin", dir, frame);
    st->spr = read_words(path, &st->spr_n);
    if (!st->spr) return 0;

    snprintf(path, sizeof path, "%s/vicsd_f%d.bin", dir, frame);
    st->vicsd = read_words(path, &st->vicsd_n);

    snprintf(path, sizeof path, "%s/vicsc_f%d.bin", dir, frame);
    { int n = 0; uint32_t *vc = read_words(path, &n);
      if (vc) { for (int i = 0; i < 0x20 && i < n; i++) st->vicsc[i] = vc[i];
                st->have_vics = (st->vicsd != NULL); free(vc); } }

    /* Palette: 0x18000 raw planar. It does not change frame to frame, so
     * reuse the caller's copy when identical -- otherwise a 900-frame
     * preload carries 86 MB of duplicate palettes. */
    snprintf(path, sizeof path, "%s/pal_f%d.bin", dir, frame);
    {
        FILE *f = fopen(path, "rb");
        if (!f) { sprite_free(st); return 0; }
        uint8_t *p = malloc(0x18000);
        if (!p) { fclose(f); sprite_free(st); return 0; }
        size_t got = fread(p, 1, 0x18000, f);
        fclose(f);
        if (got != 0x18000) { free(p); sprite_free(st); return 0; }
        if (share_pal && memcmp(share_pal, p, 0x18000) == 0) {
            free(p); st->pal = share_pal; st->owns_pal = 0;
        } else {
            st->pal = p; st->owns_pal = 1;
        }
    }
    st->valid = 1;
    return 1;
}

/* ---- one tile ---------------------------------------------------------- */
typedef struct {
    int cxn, cxx, cyn, cyx;
    int fogfactor, fadefactor, alphafactor;
    int alpha_enabled, alpha_pen, prioverchar;
    int compose_alpha;
    const uint8_t *fog_rgb, *fade_rgb;
} tile_ctx;

/* Target rectangle for draw_tile: the full 640x480 canvas, or one
 * sprite's bounding box when drawing z-merged. */
typedef struct { uint8_t *rgba; uint8_t *prio; int ox, oy, w, h; } tile_dst;

static void draw_tile_dst(const tile_dst *dst, const uint8_t *pal,
                          int pal_base, uint32_t code, int flipx, int flipy,
                          int sx, int sy, int sizex, int sizey, const tile_ctx *c);

static void draw_tile(uint8_t *rgba, uint8_t *prio, const uint8_t *pal,
                      int pal_base, uint32_t code, int flipx, int flipy,
                      int sx, int sy, int sizex, int sizey, const tile_ctx *c)
{
    tile_dst d = { rgba, prio, 0, 0, SPR_W, SPR_H };
    draw_tile_dst(&d, pal, pal_base, code, flipx, flipy, sx, sy, sizex, sizey, c);
}

static void draw_tile_dst(const tile_dst *dst, const uint8_t *pal,
                          int pal_base, uint32_t code, int flipx, int flipy,
                          int sx, int sy, int sizex, int sizey, const tile_ctx *c)
{
    int ax = sizex < 0 ? -sizex : sizex;
    int ay = sizey < 0 ? -sizey : sizey;
    int ssw = (int)(((int64_t)ax * 0x10000 / 32 * 32 + 0x8000) >> 16);
    int ssh = (int)(((int64_t)ay * 0x10000 / 32 * 32 + 0x8000) >> 16);
    if (ssw == 0 || ssh == 0) return;
    if ((size_t)code * 1024 + 1024 > (size_t)SPRITE_TOTAL_SIZE) return;
    const uint8_t *tilep = g_sprite_tiles + (size_t)code * 1024;

    int x0 = sizex > 0 ? sx : sx + sizex + 1;
    int y0 = sizey > 0 ? sy : sy + sizey + 1;

    for (int oy = 0; oy < ssh; oy++) {
        int py = y0 + oy;
        if (py < 0 || py >= SPR_H || py < c->cyn || py > c->cyx) continue;
        int by = py - dst->oy;
        if (by < 0 || by >= dst->h) continue;
        /* PIXEL CENTRE, not edge: MAME rasterises the sprite through
         * poly.h, which samples params at the centre (poly.h:791). Edge
         * sampling shifts every texel half a pixel and shows as +/-1 texel
         * errors at the edges of a zoomed sprite. */
        int v = (int)((oy + 0.5) * 32.0 / ssh);
        int tv = flipy ? 31 - v : v;
        const uint8_t *rowb = tilep + tv * 32;
        for (int ox = 0; ox < ssw; ox++) {
            int px = x0 + ox;
            if (px < 0 || px >= SPR_W || px < c->cxn || px > c->cxx) continue;
            int bx = px - dst->ox;
            if (bx < 0 || bx >= dst->w) continue;
            int u = (int)((ox + 0.5) * 32.0 / ssw);
            int tu = flipx ? 31 - u : u;
            uint8_t pen = rowb[tu];
            if (pen == 0xFF) continue;             /* transparent */
            int idx = (pal_base + pen) & 0x7FFF;
            uint8_t rgb[3] = { pal[idx], pal[0x8000 + idx], pal[0x10000 + idx] };
            if (c->fogfactor)  blend3(rgb, c->fog_rgb,  0xFF - c->fogfactor);
            if (c->fadefactor) blend3(rgb, c->fade_rgb, 0xFF - c->fadefactor);
            int o = (by * dst->w + bx) * 4;
            uint8_t *rgba = dst->rgba;
            int out_a = 255;
            int translucent = (c->alphafactor != 0xFF &&
                               (c->alpha_enabled || pen == c->alpha_pen));
            if (translucent) {
                if (c->compose_alpha) {
                    /* Defer the blend to the caller against the real frame.
                     * blend(rgb, under, f) = rgb*f + under*(256-f) >> 8, so
                     * f IS the source alpha for standard GL blending. */
                    out_a = c->alphafactor & 0xFF;
                } else {
                    uint8_t under[3] = { 0, 0, 0 };
                    if (rgba[o + 3]) { under[0]=rgba[o]; under[1]=rgba[o+1]; under[2]=rgba[o+2]; }
                    blend3(rgb, under, c->alphafactor);
                }
            }
            rgba[o] = rgb[0]; rgba[o+1] = rgb[1]; rgba[o+2] = rgb[2];
            rgba[o+3] = (uint8_t)out_a;
            if (dst->prio) dst->prio[by * dst->w + bx] = (uint8_t)(2 | c->prioverchar);
        }
    }
}

/* ---- the sprite list --------------------------------------------------- */
typedef struct { uint32_t src[4], att[2]; } spr_entry;

static int spr_cmp(const void *a, const void *b)
{
    /* painter order: scene nodes render far-to-near = larger zcoord first */
    uint32_t za = ((const spr_entry *)a)->att[0] & 0xFFFFFF;
    uint32_t zb = ((const spr_entry *)b)->att[0] & 0xFFFFFF;
    return (za > zb) ? -1 : (za < zb) ? 1 : 0;
}

static spr_entry g_list[4096];
static int       g_list_n;
static int       g_dx, g_dy, g_ylow;

/* Build the frame's sprite list: main bank then both VICS banks. They go
 * into ONE list because the hardware z-sorts them together -- the score
 * digits are VICS sprites while the translucent plate over them is a
 * main-bank sprite. */
static void build_list_impl(const sprite_state *st)
{
    const uint32_t *w = st->spr; int nw = st->spr_n;
    #define W(i) (((i) >= 0 && (i) < nw) ? w[i] : 0u)
    g_list_n = 0;
    int sprites_on = !((W(0) >> 16) & 1);
    g_ylow = !((W(0) >> 18) & 1);
    g_dx = (int)(W(1) & 0xFFFF) + (int)(W(2) & 0xFFFF) + 0x2D;
    g_dy = (int)(W(3) >> 16) + (0x2A >> (g_ylow ? 1 : 0));
    int base = (int)(W(0) & 0xFFFF);
    int num  = (int)((W(1) >> 16) - (uint32_t)base) + 1;
    if (getenv("PROPCYCL_SPRHDR")) { static int n;
        if (n++ % 120 == 0)
            printf("    [SPRHDR] W0=%08x W1=%08x on=%d base=%d num=%d\n",
                   W(0), W(1), sprites_on, base, num); }
    int n = 0;
    if (sprites_on && num > 0 && num < 0x400) {
        for (int i = 0; i < num && n < 4096; i++, n++) {
            for (int k = 0; k < 4; k++) g_list[n].src[k] = W(0x4000/4 + (base+i)*4 + k);
            for (int k = 0; k < 2; k++) g_list[n].att[k] = W(0x20000/4 + (base+i)*2 + k);
        }
    }
    if (st->have_vics && st->vicsd) {
        const uint32_t *vd = st->vicsd; int vn = st->vicsd_n;
        #define VD(i) (((i) >= 0 && (i) < vn) ? vd[i] : 0u)
        int v_on = !((st->vicsc[0x30/4] >> 24) & 1);
        const int cregs[2] = {0x48, 0x68}, aregs[2] = {0x58, 0x78}, nregs[2] = {0x40, 0x60};
        for (int b = 0; b < 2 && v_on; b++) {
            int n2 = (int)((st->vicsc[nregs[b]/4] >> 4) & 0x1FF);   /* no +1 */
            if (n2 <= 0) continue;
            int sbase = (int)(st->vicsc[cregs[b]/4] & 0xFFFF) / 4;
            int abase = (int)(st->vicsc[aregs[b]/4] & 0xFFFF) / 4;
            for (int i = 0; i < n2 && n < 4096; i++, n++) {
                for (int k = 0; k < 4; k++) g_list[n].src[k] = VD(sbase + i*4 + k);
                for (int k = 0; k < 2; k++) g_list[n].att[k] = VD(abase + i*2 + k);
            }
        }
        #undef VD
    }
    g_list_n = n;
    #undef W
}

/* ---- z-merged path -----------------------------------------------------
 * Same decode as the full-canvas path, but one sprite at a time so the
 * renderer can interleave sprites with polygons in a single z order (which
 * is what MAME's shared radix tree does). */
/* Build the frame's sprite list (main bank + VICS) into g_list. */
static void build_list(const sprite_state *st) { build_list_impl(st); }

typedef struct {
    int xpos, ypos, sizex, sizey, rows, cols, flipx, flipy, linktype;
    uint32_t tile;
    int pal_base;
    tile_ctx c;
} spr_decoded;

static void decode_sprite(const sprite_state *st, const fog_state *fog,
                          int i, spr_decoded *d,
                          uint8_t *fog_rgb, uint8_t *fade_rgb)
{
    const uint32_t *w = st->spr; int nw = st->spr_n;
    #define WQ(k) (((k) >= 0 && (k) < nw) ? w[k] : 0u)
    const uint32_t *src = g_list[i].src, *att = g_list[i].att;
    d->xpos = (int)(src[0] >> 16) - g_dx;
    d->ypos = (int)(src[0] & 0xFFFF) - g_dy;
    d->sizex = (int)(src[1] >> 16);
    d->sizey = (int)(src[1] & 0xFFFF);
    d->flipy = (src[2] >> 3) & 1;
    d->rows  = src[2] & 7;
    d->linktype = (src[2] >> 16) & 0xFF;
    d->flipx = (src[2] >> 7) & 1;
    d->cols  = (src[2] >> 4) & 7;
    d->tile  = src[3] >> 16;
    int alpha = (src[3] >> 8) & 0xFF;
    int color = (att[1] >> 16) & 0xFF;
    int cz = att[1] & 0xFF;
    int fade_en = (att[1] >> 15) & 1;
    int clip = (src[2] >> 23) & 0xE;
    d->c.cxn = -g_dx + s16(WQ(0x80 + clip) >> 16);
    d->c.cxx = -g_dx + s16(WQ(0x80 + clip) & 0xFFFF);
    d->c.cyn = -g_dy + s16(WQ(0x80 + (1 | clip)) >> 16);
    d->c.cyx = -g_dy + s16(WQ(0x80 + (1 | clip)) & 0xFFFF);
    if (d->rows == 0) d->rows = 8;
    if (d->cols == 0) d->cols = 8;
    if ((src[2] >> 9) & 1) d->xpos -= d->sizex * d->cols - 1;
    if ((src[2] >> 8) & 1) d->ypos -= d->sizey * d->rows - 1;
    if (d->flipy) { d->ypos += d->sizey * d->rows - 1; d->sizey = -d->sizey; }
    if (d->flipx) { d->xpos += d->sizex * d->cols - 1; d->sizex = -d->sizex; }
    if (g_ylow) { d->sizey *= 2; d->ypos *= 2; }
    d->c.fogfactor = (!((color >> 7) & 1) && cz > 0) ? cz : 0;
    d->c.fadefactor = (((fog->mixer_flags >> 1) & 1) || fade_en)
                      ? fog->screen_fade_factor : 0;
    d->c.alphafactor = 0xFF - alpha;
    d->c.alpha_enabled = (color & 0x7F) != fog->poly_alpha_color;
    d->c.alpha_pen = fog->poly_alpha_pen;
    d->c.prioverchar = (cz == 0xFE) ? 1 : 0;
    d->c.fog_rgb = fog_rgb; d->c.fade_rgb = fade_rgb;
    d->pal_base = 256 * (color & 0x7F);
    #undef WQ
}

static int item_cmp(const void *a, const void *b)
{
    uint32_t za = ((const sprite_item *)a)->z, zb = ((const sprite_item *)b)->z;
    return (za > zb) ? -1 : (za < zb) ? 1 : 0;   /* far (large z) first */
}

int sprite_collect(const sprite_state *st, const fog_state *fog,
                   sprite_item *out, int max_items)
{
    if (!st || !st->valid || !st->spr || !g_sprite_tiles) return 0;
    build_list(st);
    if (getenv("PROPCYCL_SPRB")) { static int n;
        if (n++ % 120 == 0) printf("    [SPRB] g_list_n=%d\n", g_list_n); }
    uint8_t fr[3] = { fog->fog_rgb[0], fog->fog_rgb[1], fog->fog_rgb[2] };
    uint8_t fa[3] = { fog->screen_fade[0], fog->screen_fade[1], fog->screen_fade[2] };
    int n = 0;
    for (int i = 0; i < g_list_n && n < max_items; i++) {
        spr_decoded d;
        decode_sprite(st, fog, i, &d, fr, fa);
        if (getenv("PROPCYCL_SPRB")) { static int m;
            if (m++ < 8)
              printf("      [SPRB] i=%d xy=(%d,%d) size=%dx%d rows=%d cols=%d "
                     "clip=[%d..%d,%d..%d] tile=%d WQ80=%08x WQ81=%08x nw=%d src2=%08x clip=%d\n", i, d.xpos, d.ypos,
                     d.sizex, d.sizey, d.rows, d.cols,
                     d.c.cxn, d.c.cxx, d.c.cyn, d.c.cyx, d.tile,
                     st->spr ? st->spr[0x80] : 0, st->spr ? st->spr[0x81] : 0, st->spr_n,
                     g_list[i].src[2], (int)((g_list[i].src[2] >> 23) & 0xE)); }
        if (d.sizex == 0 || d.sizey == 0) continue;
        int xa = d.xpos, xb = d.xpos + d.sizex * d.cols;
        int ya = d.ypos, yb = d.ypos + d.sizey * d.rows;
        int minx = xa < xb ? xa : xb, maxx = xa < xb ? xb : xa;
        int miny = ya < yb ? ya : yb, maxy = ya < yb ? yb : ya;
        if (minx < d.c.cxn) minx = d.c.cxn;
        if (maxx > d.c.cxx) maxx = d.c.cxx;
        if (miny < d.c.cyn) miny = d.c.cyn;
        if (maxy > d.c.cyx) maxy = d.c.cyx;
        if (minx < 0) minx = 0;
        if (miny < 0) miny = 0;
        if (maxx > SPR_W - 1) maxx = SPR_W - 1;
        if (maxy > SPR_H - 1) maxy = SPR_H - 1;
        if (minx > maxx || miny > maxy) continue;      /* offscreen */
        out[n].z = g_list[i].att[0] & 0xFFFFFF;
        out[n].x0 = minx; out[n].y0 = miny;
        out[n].w = maxx - minx + 1; out[n].h = maxy - miny + 1;
        out[n].idx = i;
        n++;
    }
    qsort(out, n, sizeof out[0], item_cmp);
    return n;
}

void sprite_render_item(const sprite_state *st, const fog_state *fog,
                        const sprite_item *it, uint8_t *rgba, int compose_alpha)
{
    memset(rgba, 0, (size_t)it->w * it->h * 4);
    if (!st || !st->valid) return;
    uint8_t fr[3] = { fog->fog_rgb[0], fog->fog_rgb[1], fog->fog_rgb[2] };
    uint8_t fa[3] = { fog->screen_fade[0], fog->screen_fade[1], fog->screen_fade[2] };
    spr_decoded d;
    decode_sprite(st, fog, it->idx, &d, fr, fa);
    d.c.compose_alpha = compose_alpha;
    tile_dst dst = { rgba, NULL, it->x0, it->y0, it->w, it->h };
    const uint32_t *w = st->spr;
    int offset = 0;
    for (int row = 0; row < d.rows; row++) {
        for (int col = 0; col < d.cols; col++) {
            uint32_t code = d.tile;
            if (d.linktype == 0xFF) code += offset;
            else code += nthword(w, 0x400 + offset + d.linktype * 4);
            draw_tile_dst(&dst, st->pal, d.pal_base, code, d.flipx, d.flipy,
                          d.xpos + col * d.sizex, d.ypos + row * d.sizey,
                          d.sizex, d.sizey, &d.c);
            offset++;
        }
    }
}

void sprite_render(const sprite_state *st, const fog_state *fog,
                   uint8_t *rgba, uint8_t *prio)
{
    sprite_render_ex(st, fog, rgba, prio, 0);
}

void sprite_render_ex(const sprite_state *st, const fog_state *fog,
                      uint8_t *rgba, uint8_t *prio, int compose_alpha)
{
    memset(rgba, 0, (size_t)SPR_W * SPR_H * 4);
    memset(prio, 0, (size_t)SPR_W * SPR_H);
    if (!st || !st->valid || !st->spr || !g_sprite_tiles) return;

    build_list(st);
    spr_entry *list = g_list;
    int n = g_list_n;
    const uint32_t *w = st->spr;
    int deltax = g_dx, deltay = g_dy, y_lowres = g_ylow;
    int nw = st->spr_n;
    #define W(i) (((i) >= 0 && (i) < nw) ? w[i] : 0u)
    if (n == 0) return;
    qsort(list, n, sizeof list[0], spr_cmp);

    uint8_t fog_rgb[3]  = { fog->fog_rgb[0],    fog->fog_rgb[1],    fog->fog_rgb[2] };
    uint8_t fade_rgb[3] = { fog->screen_fade[0], fog->screen_fade[1], fog->screen_fade[2] };

    for (int i = 0; i < n; i++) {
        const uint32_t *src = list[i].src, *att = list[i].att;
        int xpos  = (int)(src[0] >> 16) - deltax;
        int ypos  = (int)(src[0] & 0xFFFF) - deltay;
        int sizex = (int)(src[1] >> 16);
        int sizey = (int)(src[1] & 0xFFFF);
        int flipy = (src[2] >> 3) & 1;
        int rows  = src[2] & 7;
        int linktype = (src[2] >> 16) & 0xFF;
        int flipx = (src[2] >> 7) & 1;
        int cols  = (src[2] >> 4) & 7;
        uint32_t tile = src[3] >> 16;
        int alpha = (src[3] >> 8) & 0xFF;

        int color = (att[1] >> 16) & 0xFF;
        int cz = att[1] & 0xFF;
        int fade_en = (att[1] >> 15) & 1;
        int prioverchar = (cz == 0xFE) ? 1 : 0;

        int clip = (src[2] >> 23) & 0xE;
        tile_ctx c;
        c.cxn = -deltax + s16(W(0x80 + clip) >> 16);
        c.cxx = -deltax + s16(W(0x80 + clip) & 0xFFFF);
        c.cyn = -deltay + s16(W(0x80 + (1 | clip)) >> 16);
        c.cyx = -deltay + s16(W(0x80 + (1 | clip)) & 0xFFFF);

        if (rows == 0) rows = 8;
        if (cols == 0) cols = 8;
        if ((src[2] >> 9) & 1) xpos -= sizex * cols - 1;   /* right justify */
        if ((src[2] >> 8) & 1) ypos -= sizey * rows - 1;   /* bottom justify */
        if (flipy) { ypos += sizey * rows - 1; sizey = -sizey; }
        if (flipx) { xpos += sizex * cols - 1; sizex = -sizex; }
        if (y_lowres) { sizey *= 2; ypos *= 2; }
        if (sizex == 0 || sizey == 0) continue;

        c.fogfactor = (!((color >> 7) & 1) && cz > 0) ? cz : 0;
        c.fadefactor = (((fog->mixer_flags >> 1) & 1) || fade_en)
                       ? fog->screen_fade_factor : 0;
        c.alphafactor = 0xFF - alpha;
        c.alpha_enabled = (color & 0x7F) != fog->poly_alpha_color;
        c.alpha_pen = fog->poly_alpha_pen;
        c.prioverchar = prioverchar;
        c.compose_alpha = compose_alpha;
        c.fog_rgb = fog_rgb; c.fade_rgb = fade_rgb;

        int pal_base = 256 * (color & 0x7F);
        int offset = 0;
        for (int row = 0; row < rows; row++) {
            for (int col = 0; col < cols; col++) {
                uint32_t code = tile;
                if (linktype == 0xFF) code += offset;
                else code += nthword(w, 0x400 + offset + linktype * 4);
                draw_tile(rgba, prio, st->pal, pal_base, code, flipx, flipy,
                          xpos + col * sizex, ypos + row * sizey,
                          sizex, sizey, &c);
                offset++;
            }
        }
    }
    #undef W
}

/* ---- live memory source ------------------------------------------------
 * The dumps are big-endian words on disk; g_sys holds the same regions as
 * big-endian BYTES (the transpiled code writes them with M68K semantics),
 * so the conversion is the same either way. */
int sprite_load_live(sprite_state *st)
{
    memset(st, 0, sizeof *st);
    if (!live_spr) {
        live_spr   = malloc(SPRITERAM_SIZE);
        live_vicsd = malloc(VICS_DATA_SIZE);
        if (!live_spr || !live_vicsd) return 0;
    }
    const uint8_t *b = g_sys.spriteram;
    if (getenv("PROPCYCL_SPRLD")) { static int n;
        if (n++ % 120 == 0)
            printf("    [SPRLD] hdr %02x%02x%02x%02x %02x%02x%02x%02x clip %02x%02x%02x%02x\n",
                   b[0],b[1],b[2],b[3],b[4],b[5],b[6],b[7],
                   b[0x200],b[0x201],b[0x202],b[0x203]); }
    int n = SPRITERAM_SIZE / 4;
    for (int i = 0; i < n; i++)
        live_spr[i] = ((uint32_t)b[i*4] << 24) | ((uint32_t)b[i*4+1] << 16) |
                      ((uint32_t)b[i*4+2] << 8) | b[i*4+3];
    st->spr = live_spr; st->spr_n = n;

    b = g_sys.vics_data;
    int vn = VICS_DATA_SIZE / 4;
    for (int i = 0; i < vn; i++)
        live_vicsd[i] = ((uint32_t)b[i*4] << 24) | ((uint32_t)b[i*4+1] << 16) |
                        ((uint32_t)b[i*4+2] << 8) | b[i*4+3];
    st->vicsd = live_vicsd; st->vicsd_n = vn;

    b = g_sys.vics_ctrl;
    for (int i = 0; i < 0x20; i++)
        st->vicsc[i] = ((uint32_t)b[i*4] << 24) | ((uint32_t)b[i*4+1] << 16) |
                       ((uint32_t)b[i*4+2] << 8) | b[i*4+3];
    st->have_vics = 1;

    /* Same reason as text_hw: g_sys.palette_ram is the broken one. */
    { const uint8_t *pp = renderer3d_planar_palette();
      st->pal = pp ? pp : g_sys.palette_ram; }   /* borrowed, not owned */
    st->owns_pal = 0;
    st->valid = 1;
    return 1;
}
