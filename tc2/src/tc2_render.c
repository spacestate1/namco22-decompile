/*
 * tc2_render.c -- System 23's PICTURE, our own code: the C435's render list (src/tc2_c435.c) drawn as the hardware draws it, and the frame
 * composed as MAME's namcos23.cpp screen_update describes it --
 *   the C404 background colour (faded with fade flag bit 0) -> the C361 text layer's low-priority pixels -> the polygons -> the text pixels
 *   the polygons cover (MAME's priority map: text marks 4, a polygon pixel turns a mark into 6) -- each layer enabled by the C404's layer flags
 *   (bit 2 text, bit 0 polygons).
 * THE MODELS: the point ROM (tss1pt*: 32-bit words, word = high chip << 16 | low chip) starts with a table of model addresses; a model is a run
 * of polygons -- type word (vertex count, light mode, colour mode, texture base, "last" bit 16), header word h (colour, z-sort mode, back-face
 * test, blend, stencil), an optional z-shift word, a light word (or one packed normal a vertex), then per vertex three s24 coordinates with
 * the texture u/v packed into their top bits. Each vertex goes through the entry's matrix, scale and translation (blended with a second model
 * by the blend factor), is clipped at the near plane, projected (x / z * the viewport's focal length, about its centre) and the polygon is
 * z-sorted (min / max / mid depth, plus the z-shift and the absolute priority) and drawn far to near, perspective-correct: texture tiles
 * from the tile map (tss1ccrl/h: tile number, flips, swap) into the 16x16 8-bit tiles (tss1cg*), the palette bank `colour`, Gouraud shade
 * (0..63 = x n / 64), the C404's polygon colour fade, screen fade, alpha and 50% blend.
 * Immediate polygons (state 0x0000 / 0x0002 / 0x000A) are drawn the same way from their own vertices.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <float.h>
#include <pthread.h>
#include "tc2_c435.h"
#include "tc2_frame.h"
#include "tc2_build.h"

uint8_t *tc2_vram_ptr(uint32_t p);
void tc2_text_scroll(uint16_t linexscroll[1024], uint16_t *yscroll);
uint16_t *tc2_c412_sram(void);

static uint32_t *ptrom; static uint32_t ptrom_limit;
static uint16_t *tmlrom; static uint8_t *tmhrom, *texrom;
static uint32_t *tm_decoded; static uint8_t *tattr_decoded;
static const uint32_t tileid_mask = 0x1FFF00, tile_mask = 0x1FFFF;
static int roms_ok;
unsigned long tc2_render_direct, tc2_render_frames, tc2_render_entries, tc2_render_models, tc2_render_immediate, tc2_render_polys;

static int load(const char *dir, const char *name, uint8_t *dst, size_t n)
{
    char p[1024]; snprintf(p, sizeof p, "%s/%s", dir, name);
    FILE *f = fopen(p, "rb"); if (!f) { fprintf(stderr, "[3D] cannot read %s\n", p); return 0; }
    const size_t got = fread(dst, 1, n, f); fclose(f);
    if (got != n) { fprintf(stderr, "[3D] %s: %zu of %zu bytes\n", p, got, n); return 0; }
    return 1;
}
void tc2_render_init(const char *dir)
{
    static const char *pt[3][2] = { { "tss1pt0h.7a", "tss1pt0l.7c" }, { "tss1pt1h.5a", "tss1pt1l.5c" }, { "tss1pt2h.4a", "tss1pt2l.4c" } };
    ptrom_limit = 0x800000; ptrom = calloc(ptrom_limit + 256, 4);   /* + a pad: a polygon's vertex words are read after the per-polygon limit check */
    uint8_t *h = malloc(0x400000), *l = malloc(0x400000);
    int ok = 1;
    for (int c = 0; c < 3; c++) {
        /* MAME ROM_LOAD32_WORD_SWAP into a big-endian 32-bit region: the "h" chip's 16-bit words are the high halves, byte-swapped */
        ok &= load(dir, pt[c][0], h, 0x400000) & load(dir, pt[c][1], l, 0x400000);
        for (uint32_t k = 0; k < 0x200000; k++)
            ptrom[c * 0x200000 + k] = (uint32_t)(h[2 * k + 1] << 8 | h[2 * k]) << 16 | (uint32_t)(l[2 * k + 1] << 8 | l[2 * k]);
    }
    free(h); free(l);
    tmlrom = malloc(0x400000); tmhrom = malloc(0x200000); texrom = malloc(0x2000000);
    uint8_t *raw = (uint8_t *)tmlrom;
    ok &= load(dir, "tss1ccrl.7f", raw, 0x400000);
    for (uint32_t k = 0; k < 0x200000; k++) tmlrom[k] = (uint16_t)(raw[2 * k] | raw[2 * k + 1] << 8);   /* ROM_REGION16_LE */
    ok &= load(dir, "tss1ccrh.7e", tmhrom, 0x200000);
    static const char *cg[4] = { "tss1cgll.4m", "tss1cglm.4k", "tss1cgum.4j", "tss1cguu.4f" };
    for (int c = 0; c < 4; c++) ok &= load(dir, cg[c], texrom + c * 0x800000, 0x800000);
    tm_decoded = malloc(4u * (tileid_mask | 0xFF) + 4); tattr_decoded = malloc((tileid_mask | 0xFF) + 1);
    for (uint32_t id = 0; id <= (tileid_mask | 0xFF); id++) {
        uint8_t a = tmhrom[id >> 1]; a = (id & 1) ? (a & 15) : (a >> 4);
        tm_decoded[id] = (((uint32_t)tmlrom[id] | (uint32_t)(a & 1) << 16) & tile_mask) << 8;
        tattr_decoded[id] = a >> 1;
    }
    roms_ok = ok;
    if (ok) fprintf(stderr, "[3D] point ROM, texture tiles and tile maps loaded (model table: 0 -> %06X, 1 -> %06X)\n", ptrom[0], ptrom[1]);
}

/* the ROM data the GPU back end uploads (src/tc2_gl.c) */
void tc2_render_assets(const uint8_t **t, const uint32_t **m, const uint8_t **a)
{ *t = roms_ok ? texrom : NULL; *m = tm_decoded; *a = tattr_decoded; }

/* ---------------------------------------------------------------- the C404 (0x06A08000: 16-bit registers) and the palette, from the snapshot */
static const tc2_frame_state *FS;
static int c404(int w) { return FS->c404[2 * w] << 8 | FS->c404[2 * w + 1]; }
static int c404b(int w) { return FS->c404[2 * w + 1]; }      /* most C404 registers keep 8 bits (MAME c404_t: u8 fields) -- the game writes
                                                         * e.g. its polygon fade as (rgb >> 16, rgb >> 8, rgb) unmasked: 00FF FFFF FFFF */
static inline uint32_t pen_rgb(uint32_t n) { return FS->pens[n & 0x7FFF]; }

/* ---------------------------------------------------------------- polygons */
typedef tc2_pvert pvert;
typedef tc2_poly poly;
#define MAX_POLYS TC2_MAX_POLYS
static tc2_built *BLD;                                 /* the frame being built (the worker) */
#define GPOLYS (BLD->polys)
#define GNPOLY (BLD->npoly)
#define GFX (BLD->fx)

static int32_t s24(uint32_t v) { return (v & 0x800000) ? (int32_t)(v | 0xFF000000u) : (int32_t)(v & 0xFFFFFF); }
static int32_t s10(uint32_t v) { return (v & 0x200) ? (int32_t)((0x3FF - (v & 0x3FF)) | 0xFFFFFE00u) : (int32_t)(v & 0x1FF); }

static void transform(const tc2_render_entry *e, int32_t xi, int32_t yi, int32_t zi, float *x, float *y, float *z)
{
    int16_t m[9]; memcpy(m, e->m, sizeof m);
    if (e->transpose) { int16_t t[9]; memcpy(t, m, sizeof t); for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) m[r * 3 + c] = t[c * 3 + r]; }
    *x = ((float)((m[0] * (int64_t)xi + m[1] * (int64_t)yi + m[2] * (int64_t)zi) >> 14) * e->scaling + e->v[0]) / 16384.f;
    *y = ((float)((m[3] * (int64_t)xi + m[4] * (int64_t)yi + m[5] * (int64_t)zi) >> 14) * e->scaling + e->v[1]) / 16384.f;
    *z = ((float)((m[6] * (int64_t)xi + m[7] * (int64_t)yi + m[8] * (int64_t)zi) >> 14) * e->scaling + e->v[2]) / 16384.f;
}
static void matrot(const tc2_render_entry *e, int32_t xi, int32_t yi, int32_t zi, float *x, float *y, float *z)
{
    *x = (e->m[0] * xi + e->m[1] * yi + e->m[2] * zi) / 4194304.f;
    *y = (e->m[3] * xi + e->m[4] * yi + e->m[5] * zi) / 4194304.f;
    *z = (e->m[6] * xi + e->m[7] * yi + e->m[8] * zi) / 4194304.f;
}
/* the near plane: keep z >= zmin, interpolating x, y and the parameters (MAME poly_manager zclip_if_less) */
static int zclip(int n, const pvert *in, pvert *out, float zmin)
{
    int k = 0;
    for (int i = 0; i < n; i++) {
        const pvert *a = &in[i], *b = &in[(i + 1) % n];
        const int ain = a->p[0] >= zmin, bin = b->p[0] >= zmin;
        if (ain) out[k++] = *a;
        if (ain != bin && k < 8) {
            const float t = (zmin - a->p[0]) / (b->p[0] - a->p[0]);
            pvert *o = &out[k++];
            o->x = a->x + (b->x - a->x) * t; o->y = a->y + (b->y - a->y) * t;
            for (int q = 0; q < 4; q++) o->p[q] = a->p[q] + (b->p[q] - a->p[q]) * t;
        }
    }
    return k;
}
static void project(pvert *v, int vp_size_x, int vp_size_y, float fov)
{
    v->x = (float)vp_size_x + fov * v->x;
    v->y = (float)vp_size_y - fov * v->y;
    v->p[0] = 1.0f / v->p[0];
}
static void effects(poly *p, int color_for_alpha, int type_bit21, int h)
{
    p->alpha_enabled = (color_for_alpha & 0x7F) != GFX.alpha_color || type_bit21;
    p->blend = (h >> 10) & 1;
    p->prioverchar = 2;                                   /* text covered by a polygon is drawn again over it (MAME: 2 for polygons) */
}
/* project the clipped vertices, find the z-sort key; 0 = dropped */
static int finish(poly *p, const tc2_render_entry *e, int hsort, uint32_t polyshift, int shift_bit, uint32_t shift_mask_lo, uint32_t shift_sign)
{
    float minz = FLT_MAX, maxz = FLT_MIN;
    for (int i = 0; i < p->n; i++) {
        float z = p->pv[i].p[0] != 0 ? p->pv[i].p[0] : 1.f;
        p->pv[i].x /= z; p->pv[i].y /= z;
        z *= 16384.f;
        if (z > maxz) maxz = z;
        if (z < minz) minz = z;
        project(&p->pv[i], e->vp_size_x, e->vp_size_y, e->vp_fov);
        const float w = p->pv[i].p[0];
        p->pv[i].p[1] *= w; p->pv[i].p[2] *= w; p->pv[i].p[3] *= w;
    }
    if (maxz < 0) return 0;
    int zsort;
    switch (hsort) { case 0: zsort = (int)(minz + 0.5f); break; case 1: zsort = (int)(maxz + 0.5f); break; default: zsort = (int)(0.5f * (minz + maxz) + 0.5f); }
    if (zsort > 0x1FFFFF) zsort = 0x1FFFFF;
    int ap = e->absolute_priority & 7;
    if (polyshift >> 21 & 1) zsort = (int)(polyshift & 0x1FFFFF);
    else {
        zsort += (polyshift >> shift_bit & 1) ? (int)(polyshift | shift_sign) : (int)(polyshift & shift_mask_lo);
        ap += (int)((polyshift & 0x1C0000) >> 18);
    }
    if (zsort < 0) zsort = 0;
    if (zsort > 0x1FFFFF) zsort = 0x1FFFFF;
    p->zkey = (uint32_t)zsort | (uint32_t)ap << 21;
    p->index = (uint32_t)GNPOLY;
    p->vp_size_x = e->vp_size_x; p->vp_size_y = e->vp_size_y; p->vp_offset_x = e->vp_offset_x; p->vp_offset_y = e->vp_offset_y;
    p->entry = (int)(e - FS->list.e); p->model = e->type == TC2_RE_MODEL ? e->model : -1;
    return 1;
}

static void render_model(const tc2_render_entry *e)
{
    const int blend = e->model2 != 0;
    const uint32_t adr = ptrom[e->model], adr2 = ptrom[e->model2];
    if (adr >= ptrom_limit || adr2 >= ptrom_limit) return;
    const uint32_t *data = &ptrom[adr], *data2 = &ptrom[adr2];
    uint32_t offs = 0;
    const float fb_ = e->model_blend_factor / 16384.f, fa_ = 1.f - fb_;
    while (adr + offs < ptrom_limit && adr2 + offs < ptrom_limit && GNPOLY < MAX_POLYS) {
        pvert pv[16];
        const uint32_t type = data[offs++], h = data[offs++];
        const int cmode = (int)((type & 0x70000000u) >> 28);
        const int tbase = (int)(((type & 0x0F000000u) >> 24) << 12 | (type >> 31) << 16);
        const int color = (int)((h >> 24) & 0x7F), lmode = (int)((type >> 19) & 3), ne = (int)((type >> 8) & 15), stencil = (int)((h >> 11) & 1);
        uint32_t polyshift = 0;
        if (type & 0x1000) polyshift = data[offs++];
        uint32_t light = 0, extptr = 0;
        if (lmode == 3) { extptr = offs; offs += (uint32_t)ne; } else light = data[offs++];
        for (int i = 0; i < ne && i < 16; i++) {
            const uint32_t v1 = data[offs], v2 = data[offs + 1], v3 = data[offs + 2];
            float x, y, z;
            transform(e, s24(v1), s24(v2), s24(v3), &x, &y, &z);
            if (blend) {
                float x2, y2, z2;
                transform(e, s24(data2[offs]), s24(data2[offs + 1]), s24(data2[offs + 2]), &x2, &y2, &z2);
                x = x * fa_ + x2 * fb_; y = y * fa_ + y2 * fb_; z = z * fa_ + z2 * fb_;
            }
            offs += 3;
            pv[i].x = x; pv[i].y = y; pv[i].p[0] = z;
            pv[i].p[1] = (float)(((v1 >> 20) & 0xF00) | ((v2 >> 24) & 0xFF)) + (stencil ? 0 : 0.5f) + e->tx;
            pv[i].p[2] = (float)(((v1 >> 16) & 0xF00) | ((v3 >> 24) & 0xFF)) + (stencil ? 0 : 0.5f) + e->ty;
            pv[i].p[3] = 64;
            static const uint8_t shifts[4] = { 24, 16, 8, 0 };
            if (lmode <= 1) pv[i].p[3] = (float)((light >> shifts[i & 3]) & 0xFF);
            else if (lmode == 3) {
                const uint32_t norm = data[extptr];
                int32_t nx = s10((norm >> 20) & 0x3FF), ny = s10((norm >> 10) & 0x3FF), nz = s10(norm & 0x3FF);
                if (blend) {
                    const uint32_t n2 = data2[extptr];
                    nx = (int32_t)(nx * fa_ + s10((n2 >> 20) & 0x3FF) * fb_); ny = (int32_t)(ny * fa_ + s10((n2 >> 10) & 0x3FF) * fb_); nz = (int32_t)(nz * fa_ + s10(n2 & 0x3FF) * fb_);
                }
                extptr++;
                float nrx, nry, nrz, cx, cy, cz;
                matrot(e, nx, ny, nz, &nrx, &nry, &nrz);
                float len = sqrtf(nrx * nrx + nry * nry + nrz * nrz); if (len != 0) { nrx /= len; nry /= len; nrz /= len; }
                matrot(e, e->light_vector[0], e->light_vector[1], e->light_vector[2], &cx, &cy, &cz);
                len = sqrtf(cx * cx + cy * cy + cz * cz); if (len != 0) { cx /= len; cy /= len; cz /= len; }
                float lsi = nrx * cx + nry * cy + nrz * cz; if (lsi < 0) lsi = 0;
                float s = e->light_ambient + e->light_power * lsi; if (s < 0) s = 0; if (s > 64) s = 64;
                pv[i].p[3] = s;
            }
        }
        if (ne < 3) { if (type & 0x10000) break; continue; }
        if (h & 0x20) {                                    /* back-face test on the transformed quad */
            const float z0 = pv[0].p[0], z1 = pv[1].p[0], z2 = pv[2].p[0], z3 = pv[3].p[0];
            const float c1 = pv[2].x * (z0 * pv[1].y - pv[0].y * z1) + pv[2].y * (pv[0].x * z1 - z0 * pv[1].x) + z2 * (pv[0].y * pv[1].x - pv[0].x * pv[1].y);
            const float c2 = pv[0].x * (z2 * pv[3].y - pv[2].y * z3) + pv[0].y * (pv[2].x * z3 - z2 * pv[3].x) + z0 * (pv[2].y * pv[3].x - pv[2].x * pv[3].y);
            if (c1 >= 0.f && c2 >= 0.f) { if (type & 0x10000) break; continue; }
        }
        if (e->mirror_x) for (int i = 0; i < 4; i++) pv[i].x = -pv[i].x;
        poly *p = &GPOLYS[GNPOLY];
        p->n = zclip(ne, pv, p->pv, 0.0001f);
        if (p->n >= 3 && finish(p, e, (int)((h & 0x300) >> 8), polyshift, 17, 0x1FFFF, 0xFFFC0000u)) {
            p->stencil = stencil; p->cmode = cmode; p->tbase = tbase; p->color_base = color << 8;
            effects(p, color, (int)((type >> 21) & 1), (int)h);
            GNPOLY++;
        }
        if (type & 0x10000) break;
    }
}

static void render_immediate(const tc2_render_entry *e)
{
    const tc2_immediate *q = &e->imm;
    const uint32_t type = q->type, h = q->h;
    const int ne = (int)((type >> 8) & 0xF);
    pvert pv[16];
    for (int i = 0; i < ne && i < 4; i++) {
        pv[i].x = (int32_t)q->x[i] / 16384.f; pv[i].y = (int32_t)q->y[i] / 16384.f; pv[i].p[0] = (int32_t)q->z[i] / 16384.f;
        pv[i].p[1] = (float)(int32_t)q->u[i]; pv[i].p[2] = (float)(int32_t)q->v[i]; pv[i].p[3] = (float)(int32_t)q->i[i];
    }
    if (GNPOLY >= MAX_POLYS) return;
    poly *p = &GPOLYS[GNPOLY];
    p->n = zclip(ne < 4 ? ne : 4, pv, p->pv, 0.0001f);
    if (p->n >= 3 && finish(p, e, 2, q->zbias, 16, 0xFFFF, 0xFFFE0000u)) {
        p->stencil = (int)((h >> 11) & 1); p->cmode = 0; p->tbase = 0; p->color_base = (int)(q->pal & 0x7F00);
        effects(p, (int)((q->pal >> 8) & 0x7F), 0, (int)h);
        GNPOLY++;
    }
}

/* a DIRECT quad (MAME render_direct_poly): two triangles (0 1 2, 0 2 3), already projected */
static void render_direct(const tc2_render_entry *e)
{
    const uint16_t *d = e->d;
    const uint32_t polyshift = (uint32_t)(d[1] & 0x1FF) << 12 | (d[0] & 0xFFF);
    static const int idx[2][3] = { { 0, 1, 2 }, { 0, 2, 3 } };
    for (int t = 0; t < 2 && GNPOLY < MAX_POLYS; t++) {
        poly *p = &GPOLYS[GNPOLY];
        p->n = 3;
        for (int j = 0; j < 3; j++) {
            const uint16_t *src = &d[4 + idx[t][j] * 6];
            const int mant = src[5]; int ex = src[4] & 0x3F;
            float w = 1.f;
            if (mant) { w = (float)mant; while (ex < 0x2E) { w /= 2.0f; ex++; } }
            p->pv[j].p[0] = w;
            p->pv[j].p[1] = ((src[0] >> 4) + 0.5f) * w;
            p->pv[j].p[2] = ((src[1] >> 4) + 0.5f) * w;
            p->pv[j].p[3] = (float)(src[4] >> 8) * w;
            p->pv[j].x = (float)((int16_t)src[2] + 320);
            p->pv[j].y = (float)((int16_t)src[3] + 240);
        }
        int zsort = 0, ap = e->absolute_priority & 7;
        if (polyshift >> 21 & 1) zsort = (int)(polyshift & 0x1FFFFF);
        else { zsort += (polyshift >> 17 & 1) ? (int)(polyshift | 0xFFFC0000u) : (int)(polyshift & 0x1FFFF); ap += (int)((polyshift & 0x1C0000) >> 18); }
        if (zsort < 0) zsort = 0;
        if (zsort > 0x1FFFFF) zsort = 0x1FFFFF;
        p->zkey = (uint32_t)zsort | (uint32_t)ap << 21; p->index = (uint32_t)GNPOLY;
        p->stencil = 0; p->color_base = d[2] & 0x7F00;
        p->tbase = (d[2] & 0xF) << 12 | ((d[2] >> 7) & 1) << 16;
        p->cmode = (d[2] & 0x70) >> 4;
        p->vp_size_x = e->vp_size_x; p->vp_size_y = e->vp_size_y; p->vp_offset_x = e->vp_offset_x; p->vp_offset_y = e->vp_offset_y;
        p->entry = (int)(e - FS->list.e); p->model = -1;
        effects(p, (d[2] >> 8) & 0x7F, 0, 0);
        p->blend = 0;
        p->prioverchar = (p->cmode & 7) == 1 ? 7 : 0;
        GNPOLY++;
    }
}

/* ---------------------------------------------------------------- the software fill (the fallback, and the GPU's oracle) */
static uint32_t *fb;
static uint8_t *prio;
static struct { int fade, fadefactor, fadefactor_inv, fr, fg, fb, pfade, pr, pg, pb, alpha, alpha_inv, alpha_pen; } rx;   /* from the frame's C404 */
static uint32_t tex_lookup(uint32_t pbase, int penshift, int penmask, uint32_t u, uint32_t v, uint8_t *pen)
{
    const uint32_t id = ((u >> 4) & 0xFF) | ((v << 4) & tileid_mask);
    const uint32_t tile = tm_decoded[id], a = tattr_decoded[id];
    if (a & 1) v = ~v;
    if (a & 2) u = ~u;
    if (a & 4) { const uint32_t t = u; u = v; v = t; }
    *pen = texrom[(tile | ((v << 4) & 0xF0) | (u & 0x0F)) & 0x1FFFFFF];
    return pen_rgb(pbase + ((uint32_t)(*pen >> penshift) & (uint32_t)penmask));
}
static int stencil_lookup(uint32_t x, uint32_t y)
{
    const uint32_t bit = (x & 15) ^ 15, offs = ((y << 6) | (x >> 4)) & 0x1FFFF;
    return !(FS->sram[offs] >> bit & 1);
}
/* EXACT DEPTH (2026-10-08, the user's request -- MAME sorts like the default and shows the same jumps): the hardware draws whole polygons
 * far to near by ONE depth key each, so neighbouring objects whose keys nearly tie swap places as the camera moves (measured: distant
 * buildings and the bridge in stage 1's boat chase jump in front of each other). With depth_mode on, a pixel is also depth-tested
 * against what a DIFFERENT object of the SAME priority band drew there (1/z, the nearer wins; a tie stays with the draw order); within
 * one object the game's order stands (its painted-on details), and other priority bands (the HUD ...) stay purely sorted. Translucent
 * and blended pixels are tested but write no depth. */
int tc2_depth_mode = -1;
static float zb[640 * 480]; static int16_t eb[640 * 480]; static uint8_t ab[640 * 480];   /* the NEAREST opaque pixel: 1/z, entry + 1, band */
static int near_pass;                                   /* 1: span() only records the nearest opaque pixel (the pre-pass) */
static float depth_keep = 0.98f;                       /* a pixel is hidden only if the other object is nearer by more than this margin ($TC2_DEPTH_TOL, percent) */
int tc2_depth_on(void)                                   /* the setting (Display > Depth; $TC2_DEPTH=0/1 for tests), shared with src/tc2_gl.c */
{
    if (tc2_depth_mode < 0) { const char *e = getenv("TC2_DEPTH"); tc2_depth_mode = e ? *e == '1' : 1;
                              e = getenv("TC2_DEPTH_TOL"); if (e) depth_keep = 1.0f - (float)atof(e) / 100.0f; }
    return tc2_depth_mode;
}
float tc2_depth_keep(void) { tc2_depth_on(); return depth_keep; }
void tc2_depth_set(int on) { tc2_depth_on(); if (!getenv("TC2_DEPTH")) tc2_depth_mode = on != 0; }   /* the menu / tc2.cfg; $TC2_DEPTH wins for tests */
static int seamtest = -1;                                   /* $TC2_SEAMTEST=1: coverage only -- each pixel = the drawn polygon's depth key (magenta = nothing), no text */
static void span(const poly *p, int y, int x0, int x1, const float *s, const float *d)
{
    const int clip_left = (320 - p->vp_size_x) + p->vp_offset_x, clip_right = (320 + p->vp_size_x) + p->vp_offset_x;
    const int clip_top = (240 - p->vp_size_y) - p->vp_offset_y, clip_bottom = (240 + p->vp_size_y) - p->vp_offset_y;
    const int oy = y + clip_top;
    if (oy < 0 || oy >= 480 || oy < clip_top || oy >= clip_bottom) return;
    uint32_t *dest = fb + oy * 640; uint8_t *pm = prio + oy * 640;
    uint32_t pbase = (uint32_t)p->color_base; int penmask = 0xFF, penshift = 0;
    if (p->cmode & 4) { pbase += 0xEC + ((p->cmode & 8) << 1); penmask = 3; penshift = 2 * (~p->cmode & 3); }
    else if (p->cmode & 2) { pbase += 0xE0 + ((p->cmode & 8) << 1); penmask = 0xF; penshift = 4 * (~p->cmode & 1); }
    float z = s[0], u = s[1], v = s[2], i = s[3];
    for (int x = x0; x < x1; x++, z += d[0], u += d[1], v += d[2], i += d[3]) {
        const float ooz = 1.0f / z;
        uint32_t tx = (uint32_t)(int32_t)(u * ooz), ty = (uint32_t)(int32_t)(v * ooz);
        const int vx = x + clip_left;
        if (vx < clip_left || vx >= clip_right || vx < 0 || vx >= 640) continue;
        if (seamtest) {                                   /* coverage: a stencil cutout is the art's own hole, not a seam -- drawn solid */ dest[vx] = seamtest == 2 ? (uint32_t)(p->entry & 0xFFF) << 12 | ((p->zkey & 0x1FFFFF) >> 9) : (p->zkey & 0xFFFFFF); continue; }   /* 1: depth key; 2: entry << 12 | depth >> 9 */
        if (p->stencil && stencil_lookup(tx, ty)) continue;
        if (tc2_depth_mode) {
            const int k = oy * 640 + vx, ent = (int)(p->entry & 0x3FFF) + 1, band = (int)(p->zkey >> 21);
            if (near_pass) { if (z > zb[k]) { zb[k] = z; eb[k] = (int16_t)ent; ab[k] = (uint8_t)band; } continue; }   /* (opaque polygons only) */
            if (eb[k] && ab[k] == band && eb[k] != ent && z < zb[k] * depth_keep) continue;   /* another object of this band is CLEARLY nearer */
        }
        ty += (uint32_t)p->tbase;
        uint8_t pen;
        const uint32_t c = tex_lookup(pbase, penshift, penmask, tx, ty, &pen);
        int r = c >> 16 & 0xFF, g = c >> 8 & 0xFF, b = c & 0xFF;
        int sh = (int)(i * ooz); if (sh < 0) sh = 0; if (sh > 63) sh = 63;
        r = r * sh >> 6; g = g * sh >> 6; b = b * sh >> 6;
        if (rx.pfade) { r = r * rx.pr >> 8; g = g * rx.pg >> 8; b = b * rx.pb >> 8; }
        if (rx.fadefactor != 0xFF) { r = (r * rx.fadefactor + rx.fr * rx.fadefactor_inv) >> 8; g = (g * rx.fadefactor + rx.fg * rx.fadefactor_inv) >> 8; b = (b * rx.fadefactor + rx.fb * rx.fadefactor_inv) >> 8; }
        const int use_alpha = rx.alpha != 0xFF && (p->alpha_enabled || pen == rx.alpha_pen);
        if (use_alpha || p->blend) {
            const uint32_t dc = dest[vx]; const int dr = dc >> 16 & 0xFF, dg = dc >> 8 & 0xFF, db = dc & 0xFF;
            if (use_alpha) { r = (r * rx.alpha + dr * rx.alpha_inv) >> 8; g = (g * rx.alpha + dg * rx.alpha_inv) >> 8; b = (b * rx.alpha + db * rx.alpha_inv) >> 8; }
            else { r = (r + dr) >> 1; g = (g + dg) >> 1; b = (b + db) >> 1; }
        }
        dest[vx] = (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
        pm[vx] = (uint8_t)((pm[vx] & ~1) | p->prioverchar);
    }
}
/* a triangle, scan-converted at pixel centres inside the scissor 0..639 x 0..479 (MAME's render_triangle), parameters linear in screen space */
static void triangle(const poly *p, const pvert *a, const pvert *b, const pvert *c)
{
    const pvert *v[3] = { a, b, c };
    for (int i = 0; i < 2; i++) for (int j = 0; j < 2 - i; j++) if (v[j]->y > v[j + 1]->y) { const pvert *t = v[j]; v[j] = v[j + 1]; v[j + 1] = t; }
    const float area = (v[1]->x - v[0]->x) * (v[2]->y - v[0]->y) - (v[2]->x - v[0]->x) * (v[1]->y - v[0]->y);
    if (area == 0) return;
    float dpdx[4], dpdy[4];
    for (int q = 0; q < 4; q++) {
        const float p0 = v[0]->p[q], p1 = v[1]->p[q], p2 = v[2]->p[q];
        dpdx[q] = ((p1 - p0) * (v[2]->y - v[0]->y) - (p2 - p0) * (v[1]->y - v[0]->y)) / area;
        dpdy[q] = ((p2 - p0) * (v[1]->x - v[0]->x) - (p1 - p0) * (v[2]->x - v[0]->x)) / area;
    }
    int y0 = (int)ceilf(v[0]->y - 0.5f), y1 = (int)ceilf(v[2]->y - 0.5f);
    if (y0 < 0) y0 = 0; if (y1 > 480) y1 = 480;
    for (int y = y0; y < y1; y++) {
        const float yc = (float)y + 0.5f;
        const float xl0 = v[0]->x + (v[2]->x - v[0]->x) * (yc - v[0]->y) / (v[2]->y - v[0]->y);   /* the long edge */
        float xs;
        if (yc < v[1]->y) xs = v[1]->y == v[0]->y ? v[1]->x : v[0]->x + (v[1]->x - v[0]->x) * (yc - v[0]->y) / (v[1]->y - v[0]->y);
        else xs = v[2]->y == v[1]->y ? v[1]->x : v[1]->x + (v[2]->x - v[1]->x) * (yc - v[1]->y) / (v[2]->y - v[1]->y);
        float xa = xl0 < xs ? xl0 : xs, xb = xl0 < xs ? xs : xl0;
        int x0 = (int)ceilf(xa - 0.5f), x1 = (int)ceilf(xb - 0.5f);
        if (x0 < 0) x0 = 0; if (x1 > 640) x1 = 640;
        if (x0 >= x1) continue;
        float s[4];
        for (int q = 0; q < 4; q++) s[q] = v[0]->p[q] + dpdx[q] * ((float)x0 + 0.5f - v[0]->x) + dpdy[q] * (yc - v[0]->y);
        span(p, y, x0, x1, s, dpdx);
    }
}
static const tc2_built *SORTB;
static int cmp_poly(const void *a, const void *b)
{
    const poly *p1 = &SORTB->polys[*(const uint32_t *)a], *p2 = &SORTB->polys[*(const uint32_t *)b];
    if (p1->zkey < p2->zkey) return 1;
    if (p1->zkey > p2->zkey) return -1;
    return p1->index < p2->index ? -1 : p1->index > p2->index ? 1 : 0;
}

/* THE GUN SHOT FLASH: on the frame after an accepted shot the game whitens the whole screen so the cabinet gun's sensor can find
 * the screen -- the C404's screen fade straight to white at full strength (fade FF FF FF, factor FF, flags 03) for ONE frame, from no
 * fade (measured at frame 4802 of a scripted shot; the same as Time Crisis 1). Our gun is the pointer and needs none: with Controls >
 * Gun shot flash OFF (gun_flash=0 in tc2.cfg, or ENG_GUN_FLASH=0) such an instant white is not drawn for up to 2 frames. A real fade
 * to white ramps (the factor climbs over frames) and is left alone. The game itself is untouched: only the frame's copy changes. */
static volatile int gun_flash = 100;                    /* the flash's strength, percent: 100 = as the arcade, 0 = none (Controls > Gun shot flash; F6 cycles) */
void ss22_gl_set_gun_flash(int pct) { gun_flash = pct < 0 ? 0 : pct > 100 ? 100 : pct; }
static void gun_flash_filter(tc2_frame_state *fs)
{
    static int prev_fade, run, env = -1;
    if (env < 0) { const char *e = getenv("ENG_GUN_FLASH"); env = e != NULL; if (e) gun_flash = atoi(e) <= 0 ? 0 : atoi(e) > 100 ? 100 : atoi(e) == 1 ? 100 : atoi(e); }
    uint8_t *r = fs->c404;                               /* 8-bit registers in the low bytes: fade RGB 0x16-0x18, factor 0x19, flags 0x1A */
    const int fade = (r[2 * 0x1A + 1] & 3) && r[2 * 0x19 + 1];
    const int white = fade && r[2 * 0x19 + 1] >= 0xF0 && r[2 * 0x16 + 1] >= 0xF0 && r[2 * 0x17 + 1] >= 0xF0 && r[2 * 0x18 + 1] >= 0xF0;
    if (!white) run = 0;
    else if (run || !prev_fade) run++;                   /* white straight out of no fade: a flash (a ramp reaching white has prev_fade set) */
    prev_fade = fade;
    if (gun_flash < 100 && run && run <= 2) r[2 * 0x19 + 1] = (uint8_t)(r[2 * 0x19 + 1] * gun_flash * gun_flash / 10000);   /* the white fade's factor, squared: the fade's brightness goes with its square root (measured: factor 25/50/75 % = 49/69/86 % of the flash), so "50 %" is a flash HALF as bright */
}

/* THE Z-FIGHT DETECTOR ($TC2_ZFIGHT=max_dz, e.g. 64): the hardware sorts whole polygons (no depth buffer), so the analogue of z-fighting
 * is two polygons of DIFFERENT render-list entries whose sort keys nearly tie while they overlap on screen -- a small move swaps which is
 * drawn on top and the picture flickers. Each such pair (on screen, overlap >= 25% of the smaller, |dz| <= max_dz, same absolute
 * priority band) is logged once a frame with its entries and models; a pair whose order differs from the previous frame is marked FLIP. */
static void zfight_report(const tc2_built *b)
{
    static int maxdz = -2; static unsigned long fn;
    if (maxdz == -2) { const char *e = getenv("TC2_ZFIGHT"); maxdz = e ? atoi(e) : -1; }
    if (maxdz < 0) return;
    fn++;
    static uint64_t prev[4096], cur[4096]; static int nprev; int ncur = 0;
    for (int i = 0; i < b->npoly; i++) {
        const poly *p = &b->polys[b->order[i]];
        float ax0 = 1e9f, ax1 = -1e9f, ay0 = 1e9f, ay1 = -1e9f;
        const int pcl = (320 - p->vp_size_x) + p->vp_offset_x, pct = (240 - p->vp_size_y) - p->vp_offset_y;
        for (int k = 0; k < p->n; k++) { const float x = p->pv[k].x + pcl, y = p->pv[k].y + pct; if (x < ax0) ax0 = x; if (x > ax1) ax1 = x; if (y < ay0) ay0 = y; if (y > ay1) ay1 = y; }
        if (ax0 < 0) ax0 = 0; if (ay0 < 0) ay0 = 0; if (ax1 > 640) ax1 = 640; if (ay1 > 480) ay1 = 480;
        if (ax1 - ax0 < 2 || ay1 - ay0 < 2) continue;
        for (int j = i + 1; j < b->npoly && j < i + 64; j++) {
            const poly *q = &b->polys[b->order[j]];
            if ((p->zkey >> 21) != (q->zkey >> 21)) break;
            const int dz = (int)(p->zkey & 0x1FFFFF) - (int)(q->zkey & 0x1FFFFF);
            if (dz > maxdz) break;                         /* sorted far to near: the rest are further apart */
            if (q->entry == p->entry) continue;
            float bx0 = 1e9f, bx1 = -1e9f, by0 = 1e9f, by1 = -1e9f;
            const int qcl = (320 - q->vp_size_x) + q->vp_offset_x, qct = (240 - q->vp_size_y) - q->vp_offset_y;
            for (int k = 0; k < q->n; k++) { const float x = q->pv[k].x + qcl, y = q->pv[k].y + qct; if (x < bx0) bx0 = x; if (x > bx1) bx1 = x; if (y < by0) by0 = y; if (y > by1) by1 = y; }
            if (bx0 < 0) bx0 = 0; if (by0 < 0) by0 = 0; if (bx1 > 640) bx1 = 640; if (by1 > 480) by1 = 480;
            if (bx1 - bx0 < 2 || by1 - by0 < 2) continue;
            const float ox = (ax1 < bx1 ? ax1 : bx1) - (ax0 > bx0 ? ax0 : bx0), oy = (ay1 < by1 ? ay1 : by1) - (ay0 > by0 ? ay0 : by0);
            if (ox <= 0 || oy <= 0) continue;
            const float aa = (ax1 - ax0) * (ay1 - ay0), ab = (bx1 - bx0) * (by1 - by0), ov = ox * oy / (aa < ab ? aa : ab);
            if (ov < 0.25f) continue;
            /* the pair by (model, model) in the order drawn: the same pair drawn the other way round last frame = a FLIP */
            const uint32_t ma = (uint32_t)(p->model & 0xFFFF) | (uint32_t)(p->entry & 0xFFF) << 16, mb = (uint32_t)(q->model & 0xFFFF) | (uint32_t)(q->entry & 0xFFF) << 16;
            const uint64_t key = (uint64_t)ma << 32 | mb, rev = (uint64_t)mb << 32 | ma;
            int flip = 0; for (int k = 0; k < nprev && !flip; k++) flip = prev[k] == rev;
            if (ncur < 4096) cur[ncur++] = key;
            fprintf(stderr, "[ZFIGHT] frame %lu: entries %d/%d models %d/%d dz %d overlap %.0f%% box %.0f,%.0f-%.0f,%.0f%s\n", fn, p->entry, q->entry, p->model, q->model,
                    dz, 100 * ov, ox > 0 ? (ax0 > bx0 ? ax0 : bx0) : 0, ay0 > by0 ? ay0 : by0, ax1 < bx1 ? ax1 : bx1, ay1 < by1 ? ay1 : by1, flip ? " FLIP" : "");
        }
    }
    memcpy(prev, cur, sizeof cur[0] * (size_t)ncur); nprev = ncur;
}

/* ---------------------------------------------------------------- building a frame (the geometry stage, shared by both back ends) */
static void build(tc2_built *b)
{
    BLD = b; FS = &b->fs;
    gun_flash_filter(&b->fs);
    const tc2_frame_state *fs = &b->fs;
    GFX.pr = c404b(0); GFX.pg = c404b(1); GFX.pb = c404b(2); GFX.alpha_color = c404b(0xF); GFX.alpha_pen = c404b(0x10); GFX.alpha = c404b(0x11);
    GFX.fr = c404b(0x16); GFX.fg = c404b(0x17); GFX.fb = c404b(0x18); GFX.ffactor = c404b(0x19); GFX.fflags = c404b(0x1A);
    GFX.layers = c404b(0x1F);
    { static long lf = -2, lt; static unsigned long fn; fn++;               /* $TC2_C404FRAMES=from:to -- the C404's registers every frame */
      if (lf == -2) { lf = -1; const char *e = getenv("TC2_C404FRAMES"); if (e) sscanf(e, "%ld:%ld", &lf, &lt); }
      if (lf >= 0 && (long)fn >= lf && (long)fn <= lt) { fprintf(stderr, "[C404] %lu:", fn); for (int k = 0; k < 0x20; k++) fprintf(stderr, " %02X", c404b(k)); fprintf(stderr, " | list %d\n", fs->list.count); } }
    static int dump = -1; if (dump < 0) dump = getenv("TC2_C404DUMP") ? atoi(getenv("TC2_C404DUMP")) : 0;
    if (dump) { static unsigned long n; if (++n == (unsigned long)dump) { fprintf(stderr, "C404:"); for (int k = 0; k < 0x20; k++) fprintf(stderr, " %04X", c404(k)); fprintf(stderr, "\n"); } }
    int r = c404b(8), g = c404b(9), bl = c404b(10);
    if ((GFX.fflags & 1) && GFX.ffactor) {
        const int s1 = 0xFF - GFX.ffactor, s2 = 0x100 - s1;
        r = (r * s1 + GFX.fr * s2) >> 8; g = (g * s1 + GFX.fg * s2) >> 8; bl = (bl * s1 + GFX.fb * s2) >> 8;
    }
    GFX.bg = (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)bl;
    GNPOLY = 0;
    if ((GFX.layers & 1) && roms_ok)
        for (int i = 0; i < fs->list.count; i++) {
            const tc2_render_entry *e = &fs->list.e[i];
            if (e->type == TC2_RE_MODEL) { render_model(e); tc2_render_models++; }
            else if (e->type == TC2_RE_IMMEDIATE) { render_immediate(e); tc2_render_immediate++; }
            else if (e->type == TC2_RE_DIRECT) { render_direct(e); tc2_render_direct++; }
        }
    { static int jit = -1; if (jit < 0) { const char *e = getenv("TC2_ZJITTER"); jit = e ? atoi(e) : 0; }   /* TEST ONLY: nudge every z key by up
       * to +-jit (a hash of its entry), to find the pixels whose result depends on a near-tie in the draw order (z-fighting candidates) */
      if (jit > 0) for (int i = 0; i < GNPOLY; i++) { poly *q = &GPOLYS[i]; uint32_t h = (uint32_t)q->entry * 2654435761u; h ^= h >> 15;
          int z = (int)(q->zkey & 0x1FFFFF) + (int)(h % (uint32_t)(2 * jit + 1)) - jit; if (z < 0) z = 0; if (z > 0x1FFFFF) z = 0x1FFFFF;
          q->zkey = (q->zkey & ~0x1FFFFFu) | (uint32_t)z; } }
    for (int i = 0; i < GNPOLY; i++) b->order[i] = (uint32_t)i;
    SORTB = b; qsort(b->order, (size_t)GNPOLY, sizeof b->order[0], cmp_poly);
    zfight_report(b);
    { static long want = -2, built; if (want == -2) { const char *e = getenv("TC2_POLYDUMP"); want = e ? atol(e) : -1; }
      if (++built == want) {      /* $TC2_POLYDUMP=n: every polygon of the n-th built frame in draw order -- where it came from and what it is */
          for (int k = 0; k < fs->list.count; k++) { const tc2_render_entry *e = &fs->list.e[k];
              fprintf(stderr, "[ENTRY] %4d type %d model %5d model2 %5d ap %d v %d %d %d  m %d %d %d / %d %d %d / %d %d %d  scale %.3f  blend %d\n", k, e->type, e->model, e->model2,
                      e->absolute_priority, e->v[0], e->v[1], e->v[2], e->m[0], e->m[1], e->m[2], e->m[3], e->m[4], e->m[5], e->m[6], e->m[7], e->m[8], e->scaling, e->model_blend_factor); }
          for (int k = 0; k < GNPOLY; k++) {
              const poly *q = &GPOLYS[b->order[k]]; float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
              for (int j = 0; j < q->n; j++) { if (q->pv[j].x < x0) x0 = q->pv[j].x; if (q->pv[j].x > x1) x1 = q->pv[j].x; if (q->pv[j].y < y0) y0 = q->pv[j].y; if (q->pv[j].y > y1) y1 = q->pv[j].y; }
              const tc2_render_entry *e = &fs->list.e[q->entry];
              fprintf(stderr, "[POLY] %4d entry %4d type %d model %5d  x %6.1f..%6.1f y %6.1f..%6.1f  zkey %07X  tbase %05X cmode %d color %04X poc %d vp %d,%d off %d,%d blend %d alpha %d stencil %d\n",
                      k, q->entry, e->type, q->model, x0, x1, y0, y1, q->zkey, q->tbase, q->cmode, q->color_base, q->prioverchar, q->vp_size_x, q->vp_size_y, q->vp_offset_x, q->vp_offset_y, q->blend, q->alpha_enabled, q->stencil);
          } } }
    tc2_render_polys += (unsigned long)GNPOLY;
    if (GFX.layers & 4) {
        static uint8_t tpri[640 * 480];                  /* the worker's alone */
        memset(b->mix, 0, sizeof b->mix); memset(tpri, 0, sizeof tpri);
        tc2_text_layer(fs, b->mix, tpri);
        for (int k = 0; k < 640 * 480; k++) if (tpri[k]) b->mix[k] |= 0x8000; else b->mix[k] = 0;
        for (int y = 0; y < 480; y++) {                 /* the bands: a row opaque everywhere, coloured by its commonest pen */
            const uint16_t *row = &b->mix[y * 640];
            int full = 1; for (int x = 0; x < 640 && full; x++) full = (row[x] & 0x8000) != 0;
            b->band[y] = 0;
            if (!full) continue;
            uint16_t best = row[0]; int bestn = 0;
            for (int x = 0; x < 640; x += 8) { int n = 0; for (int x2 = 0; x2 < 640; x2 += 8) n += row[x2] == row[x]; if (n > bestn) { bestn = n; best = row[x]; } }
            b->band[y] = best;
        }
    } else memset(b->band, 0, sizeof b->band);
}

/* the software picture of a built frame: background, low text, the polygons far to near, the text the polygons let through */
void tc2_soft_raster(const tc2_built *b, uint32_t *rgba)
{
    static uint16_t mix[640 * 480]; static uint8_t pri[640 * 480];
    FS = &b->fs;
    const tc2_fx *f = &b->fx;
    rx.fade = (f->fflags & 1) != 0; rx.fadefactor = 0xFF; rx.fadefactor_inv = 1;
    if (rx.fade) { rx.fadefactor = 0xFF - f->ffactor; rx.fadefactor_inv = 0x100 - rx.fadefactor; rx.fr = f->fr; rx.fg = f->fg; rx.fb = f->fb; }
    rx.pfade = f->pr || f->pg || f->pb; rx.pr = f->pr; rx.pg = f->pg; rx.pb = f->pb;
    rx.alpha = 0xFF - f->alpha; rx.alpha_inv = 0x100 - rx.alpha; rx.alpha_pen = f->alpha_pen;
    if (seamtest < 0) { const char *e = getenv("TC2_SEAMTEST"); seamtest = e ? atoi(e) : 0; }
    for (int k = 0; k < 640 * 480; k++) rgba[k] = seamtest ? 0xFF00FF : f->bg;
    memset(pri, 0, sizeof pri);
    if ((f->layers & 4) && !seamtest) {
        for (int k = 0; k < 640 * 480; k++) { mix[k] = b->mix[k] & 0x7FFF; pri[k] = (b->mix[k] & 0x8000) ? 4 : 0; }
        tc2_text_mix(&b->fs, rgba, mix, pri, 4);
    }
    fb = rgba; prio = pri;
    tc2_depth_on();
    if (tc2_depth_mode && !seamtest) {                      /* the pre-pass: the nearest OPAQUE pixel everywhere (the GPU does the same: src/tc2_gl.c) */
        memset(eb, 0, sizeof eb); for (int k = 0; k < 640 * 480; k++) zb[k] = 0.0f;
        near_pass = 1;
        for (int i = 0; i < b->npoly; i++) {
            const poly *p = &b->polys[b->order[i]];
            if (p->blend || (rx.alpha != 0xFF && p->alpha_enabled)) continue;
            for (int k = 1; k + 1 < p->n; k++) triangle(p, &p->pv[0], &p->pv[k], &p->pv[k + 1]);
        }
        near_pass = 0;
    }
    for (int i = 0; i < b->npoly; i++) {
        const poly *p = &b->polys[b->order[i]];
        for (int k = 1; k + 1 < p->n; k++) triangle(p, &p->pv[0], &p->pv[k], &p->pv[k + 1]);   /* a fan */
    }
    if (seamtest) return;                                   /* no text, no gamma */
    if (f->layers & 4) {
        for (int k = 0; k < 640 * 480; k++) if (pri[k] == 6 && !(b->mix[k] & 0x8000)) pri[k] = 0;   /* a "6" with no text pixel under it draws nothing */
        tc2_text_mix(&b->fs, rgba, mix, pri, 6);
    }
    const uint8_t (*gm)[256] = b->fs.gamma;                /* the C404's gamma, last */
    for (int k = 0; k < 640 * 480; k++) { const uint32_t c = rgba[k]; rgba[k] = (uint32_t)gm[0][c >> 16 & 0xFF] << 16 | (uint32_t)gm[1][c >> 8 & 0xFF] << 8 | gm[2][c & 0xFF]; }
}

/* THE WORKER: the frame is BUILT on its own thread from a snapshot taken at the start of vertical blank, while the CPUs run the next frame
 * (and, with no GPU, rasterised there too); the window shows the newest finished picture (one frame behind, as the hardware's frame buffer
 * is), a screenshot waits for its own */
static tc2_built *B[2]; static int front;           /* B[front] = the newest finished frame */
static uint32_t frames[2][640 * 480];
static pthread_t worker; static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static int busy, started, have_frame;
static int soft_raster = 1;                            /* 0 once the window's GPU renderer is up */
void tc2_render_soft(int on) { soft_raster = on; }
static void *work(void *u)
{
    (void)u;
    pthread_mutex_lock(&mu);
    for (;;) {
        while (!busy) pthread_cond_wait(&cv, &mu);
        pthread_mutex_unlock(&mu);
        build(B[!front]);
        if (soft_raster) tc2_soft_raster(B[!front], frames[!front]);
        pthread_mutex_lock(&mu);
        front = !front; have_frame = 1; busy = 0;
        pthread_cond_broadcast(&cv);
    }
    return NULL;
}
static void wait_idle(void) { pthread_mutex_lock(&mu); while (busy) pthread_cond_wait(&cv, &mu); pthread_mutex_unlock(&mu); }

/* at the start of vertical blank (MAME's screen_update): the other list is drawn, then the lists swap and the drawn one is emptied for the
 * C435 to fill (MAME render_run) */
void tc2_render_frame_end(void)
{
    if (!started) { started = 1; B[0] = calloc(1, sizeof *B[0]); B[1] = calloc(1, sizeof *B[1]); pthread_create(&worker, NULL, work, NULL); }
    wait_idle();                                          /* the previous frame is built: the back slot is free */
    tc2_frame_state *snap = &B[!front]->fs;
    const uint8_t *c4 = tc2_vram_ptr(0x06A08000u), *pal = tc2_vram_ptr(0x06A10000u);
    memcpy(snap->c404, c4, sizeof snap->c404);
    { static int on = -1; if (on < 0) { const char *e = getenv("TC2_GAMMA"); on = !(e && *e == '0'); }   /* TC2_GAMMA=0: as MAME (no gamma) */
      for (int ch = 0; ch < 3; ch++) for (int i = 0; i < 256; i++) snap->gamma[ch][i] = on ? c4[0x200 + 0x200 * ch + 2 * i + 1] : (uint8_t)i;
      int blank = 1; for (int i = 1; i < 256 && blank; i++) blank = !snap->gamma[0][i];
      if (blank) for (int ch = 0; ch < 3; ch++) for (int i = 0; i < 256; i++) snap->gamma[ch][i] = (uint8_t)i; }   /* before the game loads it */
    { static long at = -2; static unsigned long fn; fn++;          /* $TC2_C404RAM=frame: the whole 0x800-byte C404 block once */
      if (at == -2) { const char *e = getenv("TC2_C404RAM"); at = e ? atol(e) : -1; }
      if (at > 0 && (long)fn == at) { FILE *f = fopen("work/c404ram.bin", "wb"); if (f) { fwrite(c4, 1, 0x800, f); fclose(f); fprintf(stderr, "[C404] frame %lu -> work/c404ram.bin\n", fn); } } }
    for (uint32_t n = 0; n < 0x8000; n++) { const uint32_t o = 2 * n + 1; snap->pens[n] = (uint32_t)pal[o] << 16 | (uint32_t)pal[0x10000 + o] << 8 | pal[0x20000 + o]; }
    memcpy(snap->text, tc2_vram_ptr(0x06800000u), sizeof snap->text);
    tc2_text_scroll(snap->linexscroll, &snap->yscroll);
    memcpy(snap->sram, tc2_c412_sram(), sizeof snap->sram);
    tc2_render_list *l = &tc2_render[!tc2_render_cur];   /* MAME draws the list filled the frame BEFORE (one frame behind) */
    snap->list.count = l->count;
    memcpy(snap->list.e, l->e, sizeof l->e[0] * (size_t)l->count);
    tc2_render_entries += (unsigned long)l->count;
    tc2_render_frames++;
    tc2_render_cur = !tc2_render_cur;
    tc2_render[tc2_render_cur].count = 0;
    pthread_mutex_lock(&mu); busy = 1; pthread_cond_broadcast(&cv); pthread_mutex_unlock(&mu);
}
/* the newest built frame -- the GPU renderer (main thread). The worker only writes the OTHER slot, and the main thread is the only one that
 * starts a build, so the front slot is stable until the next tc2_render_frame_end */
const tc2_built *tc2_built_latest(void) { pthread_mutex_lock(&mu); const tc2_built *b = have_frame ? B[front] : NULL; pthread_mutex_unlock(&mu); return b; }
const tc2_built *tc2_built_latest_wait(void) { if (started) wait_idle(); return tc2_built_latest(); }
/* the newest finished software picture (0x00RRGGBB, 640 x 480) */
void tc2_frame_rgba(uint32_t *rgba)
{
    pthread_mutex_lock(&mu);
    if (have_frame && soft_raster) memcpy(rgba, frames[front], sizeof frames[0]);
    else if (have_frame) { pthread_mutex_unlock(&mu); tc2_gl_read(rgba); return; }
    else memset(rgba, 0, sizeof frames[0]);
    pthread_mutex_unlock(&mu);
}
/* the picture of the frame that just ended -- screenshots: waits for it */
void tc2_frame_rgba_wait(uint32_t *rgba) { if (started) wait_idle(); tc2_frame_rgba(rgba); }
