/*
 * quad_gl.c -- the engine's polygon rasteriser (moved out of Prop Cycle's
 * renderer_3d.c; the game-specific pickers, probes and the 2D layers stay
 * with each game). Draws geo_hw quads with OpenGL 1.x fixed function:
 * per-quad baked textures (tex_bake.c), perspective-correct texcoords,
 * per-vertex shade with GL_RGB_SCALE headroom, the board's depth fog as a
 * second blended pass, and the scene clip window as a scissor.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdbool.h>
#include <time.h>
#include "eng_gl.h"
#include "eng.h"
#include "geo_hw.h"
#include "tex_bake.h"
#include "quad_gl.h"

float  g_scene_x0 = 0.0f, g_scene_x1 = (float)ENG_SCREEN_W;
int  (*g_eng_quad_dx)(const geo_quad *q);
double g_perf_bake, g_perf_gl, g_perf_clip;
int    g_bri_min = 9999, g_bri_max = -9999;
int    g_fogged_quads, g_fogA_min = 999, g_fogA_max = -999;
int    g_tex_clipbox = 0;
int    g_eng_degen_uv_legacy = 0;

/* Perf timers are gated: a frame makes thousands of these calls (Prop Cycle
 * register row 121's note in renderer_3d.c). The game sets the flag once. */
int g_perf_enabled = 0;
double eng_now(void)
{
    struct timespec ts;
    if (!g_perf_enabled) return 0.0;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* The per-pixel chain, per vertex: cz = min(z >> 8, 0x1fff);
 * ff = table[cz] + sdelta; alpha = 0xff - min(ff, 0xff) (0 = fully fogged). */
int eng_fog_alpha(const eng_fog *f, int32_t z)
{
    if (!f->tab) return f->alpha_const < 0 ? 255 : f->alpha_const;
    int cz = (int)(z >> 8);
    if (cz < 0) cz = 0;
    if (cz > 0x1fff) cz = 0x1fff;
    int ff = f->tab[cz] + f->sdelta;
    if (ff <= 0) return 255;                     /* no fog contribution */
    if (ff > 0xff) ff = 0xff;
    return 0xff - ff;
}

static int g_sort_tie_emit;
static int zcmp(const void *a, const void *b)
{
    const geo_quad *qa = (const geo_quad *)a, *qb = (const geo_quad *)b;
    if (qa->zsort != qb->zsort)
        return (qa->zsort > qb->zsort) ? -1 : 1;   /* far (large) first */
    /* TIES DRAW IN REVERSE SUBMISSION ORDER -- the FIRST quad emitted at a
     * zsort ends up ON TOP. MAME's namcos22_renderer::new_scenenode PREPENDS
     * a leaf to an occupied radix bucket (`leaf->next = node`), and
     * render_scene_nodes walks each bucket from its head, so the most recent
     * insertion is drawn first (Prop Cycle register row 175). */
    if (g_sort_tie_emit) return (qa->order < qb->order) ? -1 : (qa->order > qb->order) ? 1 : 0;
    return (qa->order > qb->order) ? -1 : (qa->order < qb->order) ? 1 : 0;
}

void eng_quad_sort(geo_quad *buf, int n, int tie_emit)
{
    g_sort_tie_emit = tie_emit;
    qsort(buf, (size_t)n, sizeof buf[0], zcmp);
}

/* ---- screen-space polygon clip -----------------------------------------
 * geo_hw's near-plane clip puts intersection vertices on z=1, which the
 * oracle itself notes "project far off-canvas" -- coordinates in the
 * billions, saturated to INT32. A hardware rasteriser simply does not write
 * those pixels; GL cannot rasterise them at all.
 *
 * The old workaround clamped each vertex to +/-4096. That bounds the number
 * but MOVES the vertex, so the edge slopes change and the quad becomes a
 * screen-spanning wedge -- the "polygons exploding now and then" artifact,
 * appearing precisely on partially-clipped quads.
 *
 * Clipping the polygon against the screen rectangle preserves the on-screen
 * shape exactly. Attributes interpolate linearly in SCREEN space, which is
 * correct for s=u/z, t=v/z, w=1/z (that is what makes the texturing
 * perspective-correct) and for the shade value. */
typedef struct { float x, y, s, t, w, bri; } geo_sv;

static int clip_edge(const geo_sv *in, int n, geo_sv *out, int axis,
                     float limit, int keep_greater)
{
    int m = 0;
    for (int i = 0; i < n && m < 30; i++) {
        const geo_sv *a = &in[i], *b = &in[(i + 1) % n];
        float av = axis ? a->y : a->x, bv = axis ? b->y : b->x;
        int ain = keep_greater ? (av >= limit) : (av <= limit);
        int bin = keep_greater ? (bv >= limit) : (bv <= limit);
        if (ain) out[m++] = *a;
        if (ain != bin && m < 30) {
            float d = bv - av;
            float t = (d != 0.0f) ? (limit - av) / d : 0.0f;
            geo_sv v;
            v.x = a->x + (b->x - a->x) * t;
            v.y = a->y + (b->y - a->y) * t;
            v.s = a->s + (b->s - a->s) * t;
            v.t = a->t + (b->t - a->t) * t;
            v.w = a->w + (b->w - a->w) * t;
            v.bri = a->bri + (b->bri - a->bri) * t;
            out[m++] = v;
        }
    }
    return m;
}

static int clip_to_screen(const geo_sv *in, int n, geo_sv *out)
{
    geo_sv a[32], b[32];
    if (n > 16) n = 16;
    for (int i = 0; i < n; i++) a[i] = in[i];
    n = clip_edge(a, n, b, 0, g_scene_x0, 1);           if (n < 3) return 0;
    n = clip_edge(b, n, a, 0, g_scene_x1, 0);           if (n < 3) return 0;
    n = clip_edge(a, n, b, 1, 0.0f, 1);                 if (n < 3) return 0;
    n = clip_edge(b, n, a, 1, (float)ENG_SCREEN_H, 0); if (n < 3) return 0;
    for (int i = 0; i < n && i < 32; i++) out[i] = a[i];
    return n;
}

/* ---- per-quad vertex arrays -------------------------------------------
 * The flush was 13.5 ms of a 15.9 ms gameplay frame -- over the 16.67 ms a
 * 60 Hz frame allows -- and it was not fill rate or texture baking (38-50
 * cache misses a frame against ~1917 quads). It was GL CALL COUNT: the main
 * pass alone issued glBegin + 3 calls per vertex + glEnd, so a four-vertex
 * quad cost 14 calls and the frame cost ~29,000.
 *
 * Client-side vertex arrays collapse that to one glDrawArrays. The arrays
 * are static, so the pointers are set once per flush rather than per quad,
 * and the data is written in the same order the immediate-mode path emitted
 * it -- identical geometry, identical attributes, byte-identical output. */
static float qa_xy[32 * 2], qa_rgba[32 * 4], qa_st[32 * 4];

void eng_draw_begin(void)
{
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glVertexPointer(2, GL_FLOAT, 0, qa_xy);
    glColorPointer(4, GL_FLOAT, 0, qa_rgba);
    glTexCoordPointer(4, GL_FLOAT, 0, qa_st);
}
void eng_draw_end(void)
{
    glDisableClientState(GL_VERTEX_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
}

static void draw_quad_one(const geo_quad *q, const eng_draw_cfg *cfg)
{
    if (cfg->flat_white) {                 /* coverage probe: flat white, no texture */
        if (q->nrv < 3) return;
        glDisable(GL_TEXTURE_2D);
        glDisable(GL_ALPHA_TEST);
        glDisable(GL_BLEND);
        int n = q->nrv > 32 ? 32 : q->nrv;
        for (int i = 0; i < n; i++) {
            qa_rgba[i*4+0] = qa_rgba[i*4+1] = qa_rgba[i*4+2] = qa_rgba[i*4+3] = 1.0f;
            qa_xy[i*2+0] = q->rv[i].sx16 / 16.0f; qa_xy[i*2+1] = q->rv[i].sy16 / 16.0f;
        }
        glDrawArrays(GL_POLYGON, 0, n);
        glEnable(GL_ALPHA_TEST);
        return;
    }

    /* TEXTURED, not flat. The colour word is a palette SELECTOR
     * ((color>>8)&0x7F picks a 256-entry group); the pen comes from the
     * texture. Sampling one centre texel per quad gave a sky dome exactly
     * one flat colour, which is why the last three attempts looked like
     * nothing -- the fault was the shading model, not the pens.
     *
     * bake_quad_texture() already does the validated two-stage tilemap
     * lookup over a UV bounding box and hands back a GL texture. */
    int pal_group = (q->color >> 8) & 0x7F;
    /* SOLID quads (System 22 object flags, namcos22_v.cpp poly3d_drawquad):
     * no texture -- pen 0 of a palette base the flags choose, and with flag
     * bits 1-2 the base is cz_adjust's low 15 bits and shading is off. */
    int solid = q->objectflags != 0, solid_noshade = 0;
    float solid_rgb[3] = { 0, 0, 0 };
    if (solid) {
        int col = (int)((q->color >> 8) & 0xff), pen;
        if (q->objectflags & 6) { pen = q->cz_adjust & 0x7fff; solid_noshade = 1; }
        else pen = ((col & 0x7f) << 8) + (((q->cz_adjust >> 16) & 0x7f) & (col | 0x1f));
        pen &= 0x7fff;
        for (int c = 0; c < 3; c++) solid_rgb[c] = direct_palette[pen >> 8][pen & 0xff][c] / 255.0f;
    }
    /* The texture is keyed on the WHOLE QUAD's UV box, not the clipped
     * polygon's. A quad cut by the near plane gets interpolated UVs that move
     * every frame as the camera moves, so keying on them baked a brand-new
     * texture for it EVERY FRAME: 127,705 distinct textures over one 6000-frame
     * level, against a 65,536-slot cache. The bake is one texel per source
     * texel, so a wider box leaves every sampled texel where it was; the
     * clipped vertices always lie inside it. PROPCYCL_TEX_CLIPBOX=1 restores
     * the clipped box for A/B. */
    int min_u = 0xFFFF, min_v = 0xFFFF, max_u = 0, max_v = 0;
    if (!g_tex_clipbox) {
        min_u = q->uvbox[0]; max_u = q->uvbox[1];
        min_v = q->uvbox[2]; max_v = q->uvbox[3];
    } else
    for (int i = 0; i < q->nrv; i++) {
        int uu = (int)q->rv[i].u, vv = (int)q->rv[i].v;
        if (uu < min_u) min_u = uu;  if (uu > max_u) max_u = uu;
        if (vv < min_v) min_v = vv;  if (vv > max_v) max_v = vv;
    }
    int range_u = max_u - min_u + 1, range_v = max_v - min_v + 1;
    if (range_u < 1) range_u = 1;
    if (range_v < 1) range_v = 1;
    /* Scene clip window. GL scissor is bottom-left origin; our ortho is
     * top-down (glOrtho(0,W,H,0,...)), hence the y flip. */
    /* Widescreen: a viewport whose clip spans the whole 640 width is a
     * FULL-FRAME one and widens to the scene's edges; a real sub-window (the
     * results map, the name-entry lens, the credits window) keeps its own. */
    const int sx0 = (int)g_scene_x0, sx1 = (int)g_scene_x1 - 1;
    const int fullw = q->clip[0] <= 0 && q->clip[1] >= ENG_SCREEN_W - 1;
    const int hdx = g_eng_quad_dx ? g_eng_quad_dx(q) : 0;      /* widescreen HUD: this quad's move outward (0 = stays) */
    int cminx = fullw ? sx0 : (q->clip[0] < 0 ? 0 : q->clip[0]) + hdx;
    int cmaxx = fullw ? sx1 : (q->clip[1] > ENG_SCREEN_W - 1 ? ENG_SCREEN_W - 1 : q->clip[1]) + hdx;
    int cminy = q->clip[2] < 0 ? 0 : q->clip[2];
    int cmaxy = q->clip[3] > ENG_SCREEN_H - 1 ? ENG_SCREEN_H - 1 : q->clip[3];
    if (cminx > cmaxx || cminy > cmaxy) return;      /* fully clipped away */
    int scissored = !(cminx == sx0 && cminy == 0 &&
                      cmaxx == sx1 && cmaxy == ENG_SCREEN_H - 1);
    if (scissored) {
        /* glScissor takes WINDOW pixels, but the scene is a 640x480 ortho
         * drawn into whatever viewport main.c set for the window (scaled,
         * letterboxed). Map the clip window through that viewport -- in
         * 640x480 scene pixels a larger window cut the ending's credits
         * window and the name-entry lens down to a corner (register row 192).
         * Headless (viewport 0,0,640,480) is unchanged. */
        GLint vp[4];
        glGetIntegerv(GL_VIEWPORT, vp);
        double kx = (double)vp[2] / (g_scene_x1 - g_scene_x0), ky = (double)vp[3] / ENG_SCREEN_H;
        int x0 = vp[0] + (int)((cminx - g_scene_x0) * kx + 0.5);
        int x1 = vp[0] + (int)((cmaxx + 1 - g_scene_x0) * kx + 0.5);
        int y0 = vp[1] + (int)((ENG_SCREEN_H - 1 - cmaxy) * ky + 0.5);
        int y1 = vp[1] + (int)((ENG_SCREEN_H - cminy) * ky + 0.5);
        glEnable(GL_SCISSOR_TEST);
        glScissor(x0, y0, x1 - x0, y1 - y0);
    }

    GLuint tex = 0;
    float bsu = 1.0f, bsv = 1.0f;
    double _tb = eng_now();
    if (!solid) {
    /* su/sv are the fraction of the allocated texture the bake actually
     * fills. The bake is one texel per SOURCE texel and the allocation is
     * rounded up to a power of two for reuse, so the texture coordinates
     * have to be scaled to the used corner -- otherwise every quad samples
     * a stretched copy and the 16x16 tile pattern drifts across it. */
    /* How many texels this quad can actually show: its on-screen extent,
     * clipped to the clip window, in WINDOW pixels. A quad whose UV range
     * exceeds TEX_BAKE_MAX (the stage/title cards, 300-450 texels) used to
     * be decimated to 256 regardless of how big it is on screen, which is
     * what made the cards look low-res next to MAME. The bake takes the
     * next cap up (256/512/1024) that covers this. */
    {
        int x0 = 1 << 30, x1 = -(1 << 30), y0 = 1 << 30, y1 = -(1 << 30);
        for (int i = 0; i < q->nrv; i++) {
            int x = q->rv[i].sx16 >> 4, y = q->rv[i].sy16 >> 4;
            if (x < x0) x0 = x;  if (x > x1) x1 = x;
            if (y < y0) y0 = y;  if (y > y1) y1 = y;
        }
        if (x0 < cminx) x0 = cminx;  if (x1 > cmaxx) x1 = cmaxx;
        if (y0 < cminy) y0 = cminy;  if (y1 > cmaxy) y1 = cmaxy;
        GLint vp[4];
        glGetIntegerv(GL_VIEWPORT, vp);
        double kx = (double)vp[2] / (g_scene_x1 - g_scene_x0), ky = (double)vp[3] / ENG_SCREEN_H;
        int w = (int)((x1 - x0 + 1) * kx), h = (int)((y1 - y0 + 1) * ky);
        g_tex_bake_cap_req = w > h ? w : h;
    }
    tex = bake_quad_texture(min_u, min_v, range_u, range_v,
                                   q->texbank, pal_group, q->cmode, &bsu, &bsv);
    g_tex_bake_cap_req = 256;
    }
    g_perf_bake += eng_now() - _tb;
    /* THE BAKED RECTANGLE, NOT THE QUAD'S OWN UV BOX. bake_quad_texture
     * widens any range under 16 texels to a centred 16 (min - 8 for a single
     * texel) and bakes THAT; the texture coordinates were computed against
     * the unexpanded box, so a quad whose four UVs are all one texel sampled
     * the texel 8 to the upper-left of it instead. That is CLAUDE.md's "two
     * solid yellow squares in every framedump frame": the results-page trail
     * dots (code 977, all four UVs (15,15), pen 255 of group 14 = WHITE)
     * drew yellow, and the end marker (978, group 15 pen 255 = YELLOW) drew
     * off-white -- measured on MAME's own polygon dump through --framedump.
     * The legacy GL path (below) already mirrors the expansion. */
    int bmin_u = min_u, bmin_v = min_v, brange_u = range_u, brange_v = range_v;
    if (brange_u < 16) { bmin_u = (bmin_u * 2 + brange_u) / 2 - 8; brange_u = 16; }
    if (brange_v < 16) { bmin_v = (bmin_v * 2 + brange_v) / 2 - 8; brange_v = 16; }
    if (bmin_u < 0) bmin_u = 0;
    if (bmin_v < 0) bmin_v = 0;
    if (g_eng_degen_uv_legacy) { bmin_u = min_u; bmin_v = min_v; brange_u = range_u; brange_v = range_v; }
    /* Build screen-space vertices with attributes, clip, then emit. */
    geo_sv sv[8], cv[32];
    int nsv = 0;
    for (int i = 0; i < q->nrv && nsv < 8; i++) {
        /* a single-texel axis samples the texel's CENTRE: all four vertices
         * carry the same coordinate, and exactly on the texel edge the
         * perspective divide lands either side of it (measured: texel 7 of
         * the bake where 8 -- the quad's own texel -- was meant). */
        float cu = (range_u == 1 && brange_u != range_u) ? 0.5f : 0.0f;
        float cv_ = (range_v == 1 && brange_v != range_v) ? 0.5f : 0.0f;
        /* TEXEL CENTRES: MAME interpolates (u + 0.5) / z (poly3d_drawquad),
         * i.e. a vertex at texel u samples the MIDDLE of texel u. */
        if (cfg->texel_centre) { if (cu == 0.0f) cu = 0.5f; if (cv_ == 0.0f) cv_ = 0.5f; }
        float tu = ((float)((int)q->rv[i].u - bmin_u) + cu) / (float)brange_u * bsu;
        float tv = ((float)((int)q->rv[i].v - bmin_v) + cv_) / (float)brange_v * bsv;
        /* PERSPECTIVE-CORRECT texturing: the hardware interpolates u*ooz /
         * v*ooz and divides per pixel. glTexCoord4f(s,t,r,q) divides s/q
         * and t/q per fragment, so s=u/z with q=1/z reproduces it. */
        float iw = (q->rv[i].z > 0) ? 1.0f / (float)q->rv[i].z : 1.0f;
        /* PER-VERTEX SHADE: reference is out = c * bri / 64, so bri 64 is
         * NEUTRAL, not 255 (pc_raster_model.py shade()). */
        float sh = 1.0f;
        if (cfg->shade) {
            /* NOT clamped to 1: values above neutral are real brightening
             * and are carried through GL_RGB_SCALE below. */
            sh = (float)q->rv[i].bri / 64.0f;
            if (sh < 0.0f) sh = 0.0f;
            if (q->rv[i].bri < g_bri_min) g_bri_min = q->rv[i].bri;
            if (q->rv[i].bri > g_bri_max) g_bri_max = q->rv[i].bri;
        }
        sv[nsv].x = q->rv[i].sx16 / 16.0f + (float)hdx;
        sv[nsv].y = q->rv[i].sy16 / 16.0f;
        sv[nsv].s = tu * iw;
        sv[nsv].t = tv * iw;
        sv[nsv].w = iw;
        sv[nsv].bri = sh;
        nsv++;
    }
    double _tc = eng_now();
    int ncv = clip_to_screen(sv, nsv, cv);
    g_perf_clip += eng_now() - _tc;
    if (ncv < 3) { if (scissored) glDisable(GL_SCISSOR_TEST); return; }

    /* OVER-BRIGHTENING. The reference is out = clamp(c * bri / 64, 0, 255),
     * so bri > 64 makes a texel BRIGHTER than itself. Plain GL_MODULATE
     * cannot: glColor clamps at 1.0, so everything above neutral collapsed
     * to neutral and the frame came out uniformly dark -- measured ~16-24
     * luminance below the reference on frames carrying bri up to 173, while
     * frames with bri==64 throughout matched exactly.
     *
     * GL_RGB_SCALE (texture_env_combine, GL 1.3) multiplies the combiner
     * output by 1, 2 or 4 AFTER the modulate, which is precisely the missing
     * headroom. Pick the smallest scale covering this quad's peak bri and
     * pre-divide the per-vertex colour by it: the product is unchanged
     * (texel * bri/64), the gouraud interpolation is still linear, and the
     * clamp now happens at the end as it does in the reference. */
    int rgb_scale = 1;
    if (cfg->shade) {
        float peak = 0.0f;
        for (int i = 0; i < ncv; i++) if (cv[i].bri > peak) peak = cv[i].bri;
        if (peak > 2.0f)      rgb_scale = 4;
        else if (peak > 1.0f) rgb_scale = 2;
    }

    double _tg = eng_now();
    /* 1.0 = every quad, as before; with write_prio_alpha, the prioverchar bit */
    const float prio_a = !cfg->write_prio_alpha ? 1.0f : ((q->cmode & 7) == 1 ? 1.0f : 0.0f);
    if (solid) {
        int n = ncv > 32 ? 32 : ncv;
        glDisable(GL_TEXTURE_2D);
        for (int i = 0; i < n; i++) {
            float k = solid_noshade ? 1.0f : cv[i].bri;
            float cr = solid_rgb[0] * k, cg = solid_rgb[1] * k, cb = solid_rgb[2] * k;
            if (cfg->fade_rgb) cfg->fade_rgb(&cr, &cg, &cb);
            qa_rgba[i*4+0] = cr; qa_rgba[i*4+1] = cg;
            qa_rgba[i*4+2] = cb; qa_rgba[i*4+3] = prio_a;
            qa_xy[i*2+0] = cv[i].x; qa_xy[i*2+1] = cv[i].y;
        }
        if (cfg->write_prio_alpha) glDisable(GL_ALPHA_TEST);
        glDrawArrays(GL_POLYGON, 0, n);
        if (cfg->write_prio_alpha) glEnable(GL_ALPHA_TEST);
        rgb_scale = 1;
    } else {
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, tex);
    if (rgb_scale != 1) {
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);
        glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_MODULATE);
        glTexEnvf(GL_TEXTURE_ENV, GL_RGB_SCALE, (float)rgb_scale);
    }
    {
        float inv = 1.0f / (float)rgb_scale;
        int n = ncv > 32 ? 32 : ncv;
        for (int i = 0; i < n; i++) {
            float cr = cv[i].bri * inv, cg = cv[i].bri * inv, cb = cv[i].bri * inv;
            if (cfg->fade_rgb) cfg->fade_rgb(&cr, &cg, &cb);
            qa_rgba[i*4+0] = cr; qa_rgba[i*4+1] = cg;
            qa_rgba[i*4+2] = cb; qa_rgba[i*4+3] = prio_a;
            qa_st[i*4+0] = cv[i].s; qa_st[i*4+1] = cv[i].t;
            qa_st[i*4+2] = 0.0f;   qa_st[i*4+3] = cv[i].w;
            qa_xy[i*2+0] = cv[i].x; qa_xy[i*2+1] = cv[i].y;
        }
        if (cfg->write_prio_alpha) glDisable(GL_ALPHA_TEST);
        glDrawArrays(GL_POLYGON, 0, n);
        if (cfg->write_prio_alpha) glEnable(GL_ALPHA_TEST);
    }
    if (rgb_scale != 1)      /* restore, or every later quad inherits the scale */
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    glDisable(GL_TEXTURE_2D);
    }
    g_perf_gl += eng_now() - _tg;

    /* ---- CZ depth fog -------------------------------------------------
     * The reference chain is per pixel:
     *     cz = min(z >> 8, 0x1fff);  ff = cztab[cz] + sdelta
     *     rgb = blend(rgb, fog_rgb, 0xff - min(ff, 0xff))
     * We do it as a second pass with per-vertex alpha, gouraud interpolated
     * by GL: result = fog*a + dst*(1-a), which is exactly blend(). A second
     * pass rather than GL_FOG keeps this on plain GL 1.1 -- GL_FOG_COORD is
     * 1.4 and would need extension plumbing for no gain here.
     * Depth test is off on this path (painter's algorithm), so the overlay
     * lands exactly on the quad just drawn. */
    if (cfg->fog && cfg->fog_quad) {
        /* BIT(cz_adjust,23) disables fog for the quad regardless of the
         * board's own gate (pc_raster_model.py raster(); namcos22_v.cpp
         * poly3d_drawquad on System 22). */
        int cz_off = (q->cz_adjust & 0x800000) != 0;
        eng_fog f;
        memset(&f, 0, sizeof f);
        f.alpha_const = -1;
        if (!cz_off && cfg->fog_quad(q, &f)) {
            float fr = f.rgb[0] / 255.0f;
            float fg = f.rgb[1] / 255.0f;
            float fb = f.rgb[2] / 255.0f;
            if (cfg->fade_rgb) cfg->fade_rgb(&fr, &fg, &fb);   /* fade applies after fog */
            int any = 0;
            for (int i = 0; i < ncv; i++) {
                int32_t zz = (cv[i].w > 0.0f) ? (int32_t)(1.0f / cv[i].w) : 1;
                if (eng_fog_alpha(&f, zz) < 255) { any = 1; break; }
            }
            if (any) {
                g_fogged_quads++;
                /* GL_ALPHA_TEST is on for texture transparency
                 * (glAlphaFunc(GL_GREATER, 0.1)). Fog alphas run 0.004-0.38,
                 * so leaving it enabled DISCARDS almost every fog fragment --
                 * the stage ran, the census counted it, and the framebuffer
                 * came out byte-identical. Turn it off for this pass. */
                glDisable(GL_ALPHA_TEST);
                glEnable(GL_BLEND);
                glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                if (cfg->write_prio_alpha) glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
                { int n = ncv > 32 ? 32 : ncv;
                  for (int i = 0; i < n; i++) {
                    int32_t zz = (cv[i].w > 0.0f) ? (int32_t)(1.0f / cv[i].w) : 1;
                    int a = eng_fog_alpha(&f, zz);
                    if (a < g_fogA_min) g_fogA_min = a;
                    if (a > g_fogA_max) g_fogA_max = a;
                    /* System 22 fogs BEFORE shading: shade(blend(t, F, a)) =
                     * t*s*(1-a) + F*s*a, i.e. the pass already drawn (t*s)
                     * blended with the fog colour SCALED BY THE VERTEX SHADE. */
                    float k = cfg->fog_before_shade ? cv[i].bri : 1.0f;
                    qa_rgba[i*4+0] = fr * k; qa_rgba[i*4+1] = fg * k;
                    qa_rgba[i*4+2] = fb * k; qa_rgba[i*4+3] = (255 - a) / 255.0f;
                    qa_xy[i*2+0] = cv[i].x; qa_xy[i*2+1] = cv[i].y;
                  }
                  /* texturing is OFF for this pass, so the texcoord array is
                   * not sampled; leave it pointing at the previous quad's. */
                  glDrawArrays(GL_POLYGON, 0, n); }
                if (cfg->write_prio_alpha) glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
                glDisable(GL_BLEND);
                glEnable(GL_ALPHA_TEST);
            }
        }
    }
    if (scissored) glDisable(GL_SCISSOR_TEST);
}


/* ---------------------------------------------------------------- widescreen: the game's own backdrop reaches the picture's edges
 * A quad that IS the game's 640-wide picture -- a full-frame viewport, the four corners at ONE depth (screen space, not a piece of the
 * world), an axis-aligned rectangle from the left edge to the right edge of the 640 frame (a menu's map with its shade, a screen-sized
 * gradient) -- shows in a wider picture only its own 640 and leaves the sides bare. Its outermost texel columns are what the game shows
 * to the edge of ITS screen, so they are repeated outward: a shade or gradient (which runs along y) reaches the picture's edges, and the
 * artwork in the middle is never stretched. Off unless the game asks (eng_draw_cfg.wide_backdrop); ENG_WIDE_BACKDROP=0 turns it off. */
static bool backdrop_rect(const geo_quad *q, int L[2], int R[2])
{
    if (q->nrv != 4 || !(q->clip[0] <= 0 && q->clip[1] >= ENG_SCREEN_W - 1)) return false;
    int xmin = 1 << 30, xmax = -(1 << 30), ymin = 1 << 30, ymax = -(1 << 30);
    for (int i = 0; i < 4; i++) {
        if (q->rv[i].z <= 0 || q->rv[i].z != q->rv[0].z) return false;               /* not one depth: a piece of the world */
        if (q->rv[i].sx16 < xmin) xmin = q->rv[i].sx16;
        if (q->rv[i].sx16 > xmax) xmax = q->rv[i].sx16;
        if (q->rv[i].sy16 < ymin) ymin = q->rv[i].sy16;
        if (q->rv[i].sy16 > ymax) ymax = q->rv[i].sy16;
    }
    if (xmin > 4 * 16 || xmin < -8 * 16 || xmax < (ENG_SCREEN_W - 1 - 4) * 16 || xmax > (ENG_SCREEN_W + 8) * 16) return false;   /* not edge to edge */
    /* The WHOLE picture, top to bottom too: a full-width strip (a title panel, a bar) has a border of its own on its side columns, and
     * repeating a border sideways is wrong; the screen-sized backdrop's edge columns are the gradient the game runs off the screen. */
    if (ymin > 4 * 16 || ymax < (ENG_SCREEN_H - 4) * 16) return false;
    int nl = 0, nr = 0;
    for (int i = 0; i < 4; i++) {
        const int x = q->rv[i].sx16;
        if (x - xmin <= 16)      { if (nl < 2) L[nl] = i; nl++; }
        else if (xmax - x <= 16) { if (nr < 2) R[nr] = i; nr++; }
        else return false;                                                            /* not an upright rectangle */
    }
    if (nl != 2 || nr != 2) return false;
    if (q->rv[L[0]].sy16 > q->rv[L[1]].sy16) { const int t = L[0]; L[0] = L[1]; L[1] = t; }
    if (q->rv[R[0]].sy16 > q->rv[R[1]].sy16) { const int t = R[0]; R[0] = R[1]; R[1] = t; }
    for (int k = 0; k < 2; k++) {
        const int dy = q->rv[L[k]].sy16 - q->rv[R[k]].sy16;
        if (dy > 16 || dy < -16) return false;
    }
    return true;
}

void eng_draw_quad(const geo_quad *q, const eng_draw_cfg *cfg)
{
    draw_quad_one(q, cfg);
    if (!cfg->wide_backdrop || g_scene_x0 > -0.5f) return;
    static int on = -1;
    if (on < 0) { const char *e = getenv("ENG_WIDE_BACKDROP"); on = !(e && *e == '0'); }
    int L[2], R[2];
    if (!on || !backdrop_rect(q, L, R)) return;
    const int out0 = (int)floorf(g_scene_x0) * 16, out1 = (int)ceilf(g_scene_x1) * 16;
    geo_quad e = *q;                                 /* left: its left edge goes out to the picture's edge, its right edge comes in to the left edge and takes its texels */
    for (int k = 0; k < 2; k++) { e.rv[R[k]] = q->rv[L[k]]; e.rv[L[k]] = q->rv[L[k]]; e.rv[L[k]].sx16 = out0; }
    draw_quad_one(&e, cfg);
    e = *q;                                          /* right: the mirror image */
    for (int k = 0; k < 2; k++) { e.rv[L[k]] = q->rv[R[k]]; e.rv[R[k]] = q->rv[R[k]]; e.rv[R[k]].sx16 = out1; }
    draw_quad_one(&e, cfg);
}
