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
#include <SDL2/SDL.h>           /* SDL_GL_GetProcAddress: glActiveTexture (GL 1.3) */
#include "eng_gl.h"
#include "eng.h"
#include "geo_hw.h"
#include "tex_bake.h"
#include "quad_gl.h"

/* glActiveTexture, resolved at run time (the Windows build does not link
 * opengl32, and GL 1.3 entry points need a lookup there anyway). NULL = no
 * multitexture: the fog stays a second pass. ENG_FOG2PASS=1 forces that. */
static void (APIENTRY *p_active_texture)(GLenum);
static int mt_state = -1;                           /* -1 untried, 0 unavailable, 1 usable */
static GLuint mt_dummy_tex;                         /* unit 1's binding (its combiner never samples it) */
static void mt_resolve(void)
{
    if (mt_state >= 0) return;
    p_active_texture = (void (APIENTRY *)(GLenum))SDL_GL_GetProcAddress("glActiveTexture");
    const char *e = getenv("ENG_FOG2PASS");
    mt_state = (p_active_texture && !(e && *e == '1')) ? 1 : 0;
    if (!mt_state) return;
    p_active_texture(GL_TEXTURE1);
    glGenTextures(1, &mt_dummy_tex);
    glBindTexture(GL_TEXTURE_2D, mt_dummy_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    const uint32_t white = 0xFFFFFFFF;
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, &white);
    p_active_texture(GL_TEXTURE0);
}

float  g_scene_x0 = 0.0f, g_scene_x1 = (float)ENG_SCREEN_W;
int  (*g_eng_quad_dx)(const geo_quad *q);
double g_perf_bake, g_perf_gl, g_perf_clip;
int    g_bri_min = 9999, g_bri_max = -9999;
int    g_fogged_quads, g_fogA_min = 999, g_fogA_max = -999;
int    g_tex_clipbox = 0;
int    g_eng_degen_uv_legacy = 0;

/* tracked GL state for the quad pass: valid only between eng_draw_begin/resume and
 * eng_draw_end -- the sprite/text/post passes change state behind us, so resume
 * marks it all unknown and the next quad re-issues what it needs. */
static struct { int valid, tex_on, alpha_on, blend_on, env_scale, prio_mask, fog; GLuint tex; float frgb[3]; } qs;
long g_bb_draws, g_bb_binds;                     /* per-run GL calls, for the FTIME line */
static void qs_reset(void) { qs.valid = 0; qs.env_scale = -1; }   /* qs.fog stays truthful: unit 1 may really be on */
static void qs_tex_on(int on) { if (!qs.valid || qs.tex_on != on) { if (on) glEnable(GL_TEXTURE_2D); else glDisable(GL_TEXTURE_2D); qs.tex_on = on; } }
static void qs_bind(GLuint t) { if (!qs.valid || qs.tex != t) { glBindTexture(GL_TEXTURE_2D, t); qs.tex = t; g_bb_binds++; } }
static void qs_alpha(int on) { if (!qs.valid || qs.alpha_on != on) { if (on) glEnable(GL_ALPHA_TEST); else glDisable(GL_ALPHA_TEST); qs.alpha_on = on; } }
static void qs_blend(int on) { if (!qs.valid || qs.blend_on != on) { if (on) { glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); } else glDisable(GL_BLEND); qs.blend_on = on; } }
static void qs_prio_mask(int off) { if (!qs.valid || qs.prio_mask != off) { glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, off ? GL_FALSE : GL_TRUE); qs.prio_mask = off; } }
static int qs_sc_on; static int qs_sc[4];
static void qs_scissor(int on, const int *box)
{
    if (qs.valid && qs_sc_on == on && (!on || !memcmp(qs_sc, box, sizeof qs_sc))) return;
    if (on) { glEnable(GL_SCISSOR_TEST); glScissor(box[0], box[1], box[2], box[3]); }
    else glDisable(GL_SCISSOR_TEST);
    qs_sc_on = on; if (on) memcpy(qs_sc, box, sizeof qs_sc);
}


/* 0 = plain GL_MODULATE; 1/2/4 = COMBINE with GL_RGB_SCALE headroom (see over-brightening below) */
static void qs_env(int scale)
{
    if (qs.valid && qs.env_scale == scale) return;
    if (!scale) glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    else {
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);
        glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_MODULATE);
        glTexEnvf(GL_TEXTURE_ENV, GL_RGB_SCALE, (float)scale);
    }
    qs.env_scale = scale;
}

/* ---- single-pass fog (GL 1.3 combiners, two texture units) ---------------
 * The reference chain is per pixel: fog factor f from the CZ table, fog colour
 * F, vertex shade s, texel t:
 *     Super 22 (fog AFTER shade):  out = t*s*(1-f) + F*f
 *     System 22 (fog BEFORE shade): out = (t*(1-f) + F*f) * s
 * Both are one GL_INTERPOLATE with the fog factor in the vertex ALPHA and F in
 * the unit's constant colour; the shade is a MODULATE by the vertex colour
 * with GL_RGB_SCALE headroom, on unit 1 for System 22 (shade after the fog
 * interpolation) and on unit 0 for Super 22 (shade first, fog interpolates
 * the result toward F). The two-pass blend needed a flush per fogged quad
 * (~1,300 state breaks a frame in Dirt Dash's city); this keeps fogged quads
 * in the batch. Vertex alpha no longer carries prio here -- single-pass is
 * textured quads only, and only when write_prio_alpha is off. */
static void qs_fog(int mode, const float *frgb, int scale)
{
    /* mode 0: no fog -- unit 1 off, unit 0 as before */
    if (!mode) {
        if (qs.fog) {   /* leaving fog: unit 1 off, unit 0 rebuilt by qs_env below */
            p_active_texture(GL_TEXTURE1);
            glDisable(GL_TEXTURE_2D);
            glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
            p_active_texture(GL_TEXTURE0);
            if (qs.fog == 1) {  /* unit 0 was INTERPOLATE with a CONSTANT source: restore MODULATE's */
                glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_RGB, GL_TEXTURE);
                glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_RGB, GL_PRIMARY_COLOR);
                glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_RGB, GL_SRC_COLOR);
                glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND1_RGB, GL_SRC_COLOR);
            }
            /* both modes made unit 0's alpha REPLACE(texture) */
            glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_ALPHA, GL_MODULATE);
            glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_ALPHA, GL_TEXTURE);
            glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_ALPHA, GL_PRIMARY_COLOR);
            qs.env_scale = -1;
            qs.fog = 0;
        }
        qs_env(scale);
        return;
    }
    if (qs.valid && qs.fog == mode && !memcmp(qs.frgb, frgb, 12)) {
        if (mode == 2) qs_env(scale);   /* unit 0's shade scale may differ for the same fog colour */
        return;
    }
    if (qs.fog && qs.fog != mode) {     /* switching fog modes: rebuild from the unfogged state */
        int save = qs.valid;
        qs_fog(0, frgb, scale);
        qs.valid = save;
    }
    if (mode == 2) {            /* Super 22: shade on unit 0, fog interpolate on unit 1 */
        qs_env(scale);
        /* vertex alpha now carries the fog FACTOR, not transparency: unit 0's
         * alpha must come from the texture alone or the alpha test would
         * discard heavily fogged pixels (a fully fogged sky drew black) */
        glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_ALPHA, GL_REPLACE);
        glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_ALPHA, GL_TEXTURE);
        p_active_texture(GL_TEXTURE1);
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, mt_dummy_tex);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);
        glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_INTERPOLATE);
        glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_RGB, GL_PREVIOUS);
        glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_RGB, GL_SRC_COLOR);
        glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_RGB, GL_CONSTANT);
        glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND1_RGB, GL_SRC_COLOR);
        glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE2_RGB, GL_PRIMARY_COLOR);
        glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND2_RGB, GL_SRC_ALPHA);
        glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_ALPHA, GL_REPLACE);
        glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_ALPHA, GL_PREVIOUS);
        glTexEnvf(GL_TEXTURE_ENV, GL_RGB_SCALE, 1.0f);
        glTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, frgb);
        p_active_texture(GL_TEXTURE0);
    } else {                    /* System 22: fog interpolate on unit 0, shade on unit 1 */
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);
        glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_INTERPOLATE);
        glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_RGB, GL_TEXTURE);
        glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_RGB, GL_SRC_COLOR);
        glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_RGB, GL_CONSTANT);
        glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND1_RGB, GL_SRC_COLOR);
        glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE2_RGB, GL_PRIMARY_COLOR);
        glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND2_RGB, GL_SRC_ALPHA);
        glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_ALPHA, GL_REPLACE);
        glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_ALPHA, GL_TEXTURE);
        glTexEnvf(GL_TEXTURE_ENV, GL_RGB_SCALE, 1.0f);
        glTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, frgb);
        p_active_texture(GL_TEXTURE1);
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, mt_dummy_tex);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);
        glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_MODULATE);
        glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_RGB, GL_PREVIOUS);
        glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_RGB, GL_SRC_COLOR);
        glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_RGB, GL_PRIMARY_COLOR);
        glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND1_RGB, GL_SRC_COLOR);
        glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_ALPHA, GL_REPLACE);
        glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_ALPHA, GL_PREVIOUS);
        glTexEnvf(GL_TEXTURE_ENV, GL_RGB_SCALE, (float)scale);
        p_active_texture(GL_TEXTURE0);
        qs.env_scale = -1;      /* unit 0 is INTERPOLATE now: a later unfogged quad re-applies */
    }
    qs.fog = mode;
    memcpy(qs.frgb, frgb, 12);
}

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
typedef struct { int32_t zsort; int order; int idx; } quad_key;
static int zcmp(const void *a, const void *b)
{
    const quad_key *qa = (const quad_key *)a, *qb = (const quad_key *)b;
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

/* Sorts small keys, then moves each quad once. A geo_quad is ~1 KB and the Windows C runtime's qsort swaps elements
 * a byte at a time: sorting the quads themselves cost ~9 ms a frame there (glibc's merge sort hid it on Linux).
 * `order` is unique per quad, so the order is total and the result is the same whatever the sort. */
void eng_quad_sort(geo_quad *buf, int n, int tie_emit)
{
    static quad_key *keys;
    static int cap;
    if (n < 2) return;
    if (n > cap) {
        quad_key *k = realloc(keys, (size_t)n * sizeof *k);
        if (!k) return;
        keys = k; cap = n;
    }
    for (int i = 0; i < n; i++) { keys[i].zsort = buf[i].zsort; keys[i].order = buf[i].order; keys[i].idx = i; }
    g_sort_tie_emit = tie_emit;
    qsort(keys, (size_t)n, sizeof keys[0], zcmp);
    /* apply the permutation in place, one cycle at a time: slot j takes the quad from keys[j].idx */
    for (int i = 0; i < n; i++) {
        if (keys[i].idx == i) continue;
        geo_quad held = buf[i];
        int j = i;
        for (;;) {
            const int k = keys[j].idx;
            keys[j].idx = j;
            if (k == i) { buf[j] = held; break; }
            buf[j] = buf[k];
            j = k;
        }
    }
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
static float qa_xy[32 * 2], qa_rgba[32 * 4], qa_st[32 * 4], qa_z[32];

/* ---- batched triangles ---------------------------------------------------
 * One glDrawArrays per quad cost ~1.2-5 us of driver time each, and with the
 * bake atlas (engine/tex_bake.c) nearly every quad shares a page texture, so
 * quads collect into BB_* as fan triangles and flush once per STATE RUN. */
#define BB_MAXV 60000
static float bb_xy[BB_MAXV * 2], bb_rgba[BB_MAXV * 4], bb_st[BB_MAXV * 4];
static float *bb_xyz;                    /* depth-tested runs (eng_draw_cfg.depth_test): x, y, z -- allocated on first use */
static int bb_n, bb_valid;
static struct bbstate { GLuint tex; int env, alpha, blend, prio, tex_on, sc_on, fog; int sc[4]; float frgb[3]; int depth; } bb_run;
static int depth_applied;                /* GL_DEPTH_TEST as this file last set it (only depth-tested runs ever turn it on) */

static void bb_flush(void)
{
    if (!bb_n) return;
    /* re-bind the run's page OURSELVES: tex_bake's atlas_page_init (a cold bake
     * creating a page, e.g. from a quad that then clips away) binds the new page
     * behind our back, and commit() below binds only when the page is dirty --
     * a clean page would draw against whatever texture was last bound */
    if (bb_run.tex_on) { qs_bind(bb_run.tex); tex_bake_commit(bb_run.tex); }   /* the page's dirty bands upload once, here, not per bake */
    if (bb_run.depth) {
        glVertexPointer(3, GL_FLOAT, 0, bb_xyz);
        glDrawArrays(GL_TRIANGLES, 0, bb_n);
        glVertexPointer(2, GL_FLOAT, 0, bb_xy);
    } else
    glDrawArrays(GL_TRIANGLES, 0, bb_n);
    g_bb_draws++;
    bb_n = 0;
}

static void bb_apply(const struct bbstate *st)
{
    qs_tex_on(st->tex_on);
    if (st->tex_on) {
        qs_bind(st->tex);
        /* the atlas slot origin is folded into s,t per vertex (s + ou*q at the
         * qa_st fill in draw_quad_one), NOT the texture matrix: with the matrix
         * the slot origin was part of this run state and broke the batch on
         * nearly every quad -- ~3,600 draws and ~6,900 matrix calls a frame in
         * Dirt Dash's city, 12+ ms of driver overhead on an i5-6260U.
         * (s,t,r,q) x translate(ou,ov) divides to ou + s/q either way. */
        if (mt_state == 1) qs_fog(st->fog, st->frgb, st->env);
        else qs_env(st->env);
    } else if (qs.fog) {
        qs_fog(0, st->frgb, 0);             /* leaving fog for a flat pass: unit 1 off */
    }
    qs_alpha(st->alpha);
    qs_blend(st->blend);
    qs_prio_mask(st->prio);
    qs_scissor(st->sc_on, st->sc);
    if (st->depth != depth_applied) {
        if (st->depth) { glEnable(GL_DEPTH_TEST); glDepthFunc(GL_LESS); glDepthMask(GL_TRUE); }
        else glDisable(GL_DEPTH_TEST);
        depth_applied = st->depth;
    }
}

/* ENG_RUNSTATS=1: why runs break, printed at exit (per-field counts + hook flushes) */
static long br_tex, br_env, br_alpha, br_blend, br_prio, br_sc, br_hook, br_full, br_quads, br_fog;
static void bb_runstats(void)
{
    if (!getenv("ENG_RUNSTATS")) return;
    fprintf(stderr, "[RUNSTATS] quads %ld  breaks: tex %ld env %ld alpha %ld blend %ld prio %ld scissor %ld fog %ld hook %ld full %ld\n",
            br_quads, br_tex, br_env, br_alpha, br_blend, br_prio, br_sc, br_fog, br_hook, br_full);
}

/* qa_* hold the quad's clipped polygon: append it as fan triangles, flushing
 * when the run's state changes or the buffer is full. */
static void bb_emit(int n, const struct bbstate *st_)
{
    struct bbstate st_buf = *st_;
    static int nobatch = -1;                            /* ENG_NOBATCH=1: one draw per quad, for A/B bisection.
                                                         * Per-quad textures (PROPCYCL_TEXATLAS=0) never batch:
                                                         * every quad is its own texture, so runs are 1 anyway. */
    static int rs_once;
    if (!rs_once) { rs_once = 1; atexit(bb_runstats); }
    br_quads++;
    /* an UNFOGGED textured quad can ride a FOGGED run on the same page: vertex
     * alpha 1.0 makes the fog interpolation the identity (t*1 + F*0), so adopt
     * the run's fog state instead of breaking it -- in Dirt Dash's city the
     * distant runs are nearly all fogged. Only unfogged quads (alpha == 1.0)
     * qualify; prio mode never takes the single-pass fog path at all. */
    if (st_buf.tex_on && !st_buf.fog && bb_valid && bb_run.tex_on && bb_run.fog && st_buf.tex == bb_run.tex) {
        st_buf.fog = bb_run.fog;
        memcpy(st_buf.frgb, bb_run.frgb, sizeof st_buf.frgb);
    }
    const struct bbstate *st = &st_buf;
    if (nobatch < 0) { const char *e = getenv("ENG_NOBATCH"); nobatch = (e && *e == '1') || !tex_bake_atlas_active(); }
    if (bb_valid && memcmp(st, &bb_run, sizeof bb_run) != 0) {
        if (st->tex != bb_run.tex || st->tex_on != bb_run.tex_on) br_tex++;
        if (st->env != bb_run.env) br_env++;
        if (st->alpha != bb_run.alpha) br_alpha++;
        if (st->blend != bb_run.blend) br_blend++;
        if (st->prio != bb_run.prio) br_prio++;
        if (st->sc_on != bb_run.sc_on || memcmp(st->sc, bb_run.sc, sizeof st->sc)) br_sc++;
        if (st->fog != bb_run.fog || memcmp(st->frgb, bb_run.frgb, sizeof st->frgb)) br_fog++;
        bb_flush(); bb_valid = 0;
    }
    if (!bb_valid) { bb_run = *st; bb_apply(st); bb_valid = 1; }
    if (nobatch) bb_flush();                            /* start empty: draw just this quad below */
    if (bb_n + (n - 2) * 3 > BB_MAXV) { br_full++; bb_flush(); }
    if (st->depth && !bb_xyz && !(bb_xyz = malloc(sizeof(float) * 3 * BB_MAXV))) return;
    for (int i = 1; i + 1 < n; i++)
        for (int k = 0; k < 3; k++) {
            int s = k == 0 ? 0 : i + k - 1;              /* fan: (0, i, i+1) */
            if (st->depth) { bb_xyz[bb_n*3+0] = qa_xy[s*2+0]; bb_xyz[bb_n*3+1] = qa_xy[s*2+1]; bb_xyz[bb_n*3+2] = qa_z[s]; }
            else {
            bb_xy[bb_n*2+0] = qa_xy[s*2+0]; bb_xy[bb_n*2+1] = qa_xy[s*2+1];
            }
            memcpy(&bb_rgba[bb_n*4], &qa_rgba[s*4], 4 * sizeof(float));
            memcpy(&bb_st[bb_n*4], &qa_st[s*4], 4 * sizeof(float));
            bb_n++;
        }
    if (nobatch) { bb_flush(); bb_valid = 0; }
}

/* tex_bake's flush hook: a bake is about to write page_tex's shadow, and
 * buffered quads may reference pixels on it -- draw them first, then re-apply
 * state (atlas_page_init bound a new page behind our back). A page-conditional
 * flush (same page only) was TRIED and corrupted renders (dd/tw vid gates):
 * reverted pending a proper mechanism study. */
static void bb_bake_hook(GLuint page_tex)
{
    (void)page_tex;
    br_hook++;
    if (bb_valid) bb_flush();
    bb_valid = 0; qs_reset();
}
/* the GL pass draws the fan-triangulated expansion of qa_*: Mesa tessellates
 * GL_POLYGON per call (measured ~4-6 us/draw on a 1.8 GHz mobile CPU), so the
 * fan is expanded on our side and drawn as GL_TRIANGLES -- same triangles the
 * driver would have made, byte-identical output */

/* read once per batch, not per quad: every glGet makes a threaded GL driver (NVIDIA on Windows) wait for its worker thread */
static GLint draw_vp[4];

void eng_draw_begin(void)
{
    static int hook_once;
    if (!hook_once) { tex_bake_set_flush_hook(bb_bake_hook); hook_once = 1; }
    mt_resolve();
    glGetIntegerv(GL_VIEWPORT, draw_vp);
    eng_draw_resume();
}
void eng_draw_resume(void)
{
    qs_reset();
    bb_valid = 0;
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glVertexPointer(2, GL_FLOAT, 0, bb_xy);
    glColorPointer(4, GL_FLOAT, 0, bb_rgba);
    glTexCoordPointer(4, GL_FLOAT, 0, bb_st);
}
void eng_draw_end(void)
{
    bb_flush();
    bb_valid = 0;
    glDisableClientState(GL_VERTEX_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    /* restore the state the per-quad code used to leave behind: the sprite, text and
     * post passes (and Rave Racer's 2D) were written against it (a leftover
     * COMBINE/RGB_SCALE texenv visibly brightened Rave Racer's HUD) */
    if (qs.fog) {                           /* single-pass fog: unit 1 off, unit 0 plain */
        p_active_texture(GL_TEXTURE1);
        glDisable(GL_TEXTURE_2D);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
        p_active_texture(GL_TEXTURE0);
        glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_ALPHA, GL_MODULATE);
        glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_ALPHA, GL_TEXTURE);
        glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_ALPHA, GL_PRIMARY_COLOR);
        glTexEnvf(GL_TEXTURE_ENV, GL_RGB_SCALE, 1.0f);
        qs.fog = 0;
    }
    if (depth_applied) { glDisable(GL_DEPTH_TEST); depth_applied = 0; }
    glDisable(GL_TEXTURE_2D);
    glEnable(GL_ALPHA_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    glMatrixMode(GL_TEXTURE); glLoadIdentity(); glMatrixMode(GL_MODELVIEW);
    qs_reset();
}

static void draw_quad_one(const geo_quad *q, const eng_draw_cfg *cfg)
{
    if (cfg->flat_white) {                 /* coverage probe: flat white, no texture */
        if (q->nrv < 3) return;
        int n = q->nrv > 32 ? 32 : q->nrv;
        for (int i = 0; i < n; i++) {
            qa_rgba[i*4+0] = qa_rgba[i*4+1] = qa_rgba[i*4+2] = qa_rgba[i*4+3] = 1.0f;
            qa_xy[i*2+0] = q->rv[i].sx16 / 16.0f; qa_xy[i*2+1] = q->rv[i].sy16 / 16.0f;
        }
        bb_emit(n, &(struct bbstate){ .tex_on = 0, .alpha = 0, .blend = 0, .prio = 0, .sc_on = 0 });
        return;
    }
    if (cfg->flat_quad) {                  /* System 21: one colour per quad, optionally depth-tested (see quad_gl.h) */
        float c[4];
        if (cfg->flat_quad(q, c)) {
            if (q->nrv < 3) return;
            const int n = q->nrv > 32 ? 32 : q->nrv;
            const float z = (cfg->depth_test && cfg->quad_depth) ? cfg->quad_depth(q) : 0.0f;
            for (int i = 0; i < n; i++) {
                memcpy(&qa_rgba[i*4], c, sizeof c);
                qa_xy[i*2+0] = q->rv[i].sx16 / 16.0f; qa_xy[i*2+1] = q->rv[i].sy16 / 16.0f;
                qa_z[i] = z;
            }
            bb_emit(n, &(struct bbstate){ .tex_on = 0, .alpha = 0, .blend = 0, .prio = 0, .sc_on = 0, .depth = cfg->depth_test ? 1 : 0 });
            return;
        }
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
    int scbox[4] = { 0, 0, 0, 0 };
    if (scissored) {
        /* glScissor takes WINDOW pixels, but the scene is a 640x480 ortho
         * drawn into whatever viewport main.c set for the window (scaled,
         * letterboxed). Map the clip window through that viewport -- in
         * 640x480 scene pixels a larger window cut the ending's credits
         * window and the name-entry lens down to a corner (register row 192).
         * Headless (viewport 0,0,640,480) is unchanged. The box goes into the
         * batch's run state; it is applied when the run flushes, not here. */
        const GLint *vp = draw_vp;
        double kx = (double)vp[2] / (g_scene_x1 - g_scene_x0), ky = (double)vp[3] / ENG_SCREEN_H;
        int x0 = vp[0] + (int)((cminx - g_scene_x0) * kx + 0.5);
        int x1 = vp[0] + (int)((cmaxx + 1 - g_scene_x0) * kx + 0.5);
        int y0 = vp[1] + (int)((ENG_SCREEN_H - 1 - cmaxy) * ky + 0.5);
        int y1 = vp[1] + (int)((ENG_SCREEN_H - cminy) * ky + 0.5);
        scbox[0] = x0; scbox[1] = y0; scbox[2] = x1 - x0; scbox[3] = y1 - y0;
    }

    GLuint tex = 0;
    float bsu = 1.0f, bsv = 1.0f, bou = 0.0f, bov = 0.0f;
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
        const GLint *vp = draw_vp;
        double kx = (double)vp[2] / (g_scene_x1 - g_scene_x0), ky = (double)vp[3] / ENG_SCREEN_H;
        int w = (int)((x1 - x0 + 1) * kx), h = (int)((y1 - y0 + 1) * ky);
        g_tex_bake_cap_req = w > h ? w : h;
    }
    tex = bake_quad_texture(min_u, min_v, range_u, range_v,
                                   q->texbank, pal_group, q->cmode, &bsu, &bsv, &bou, &bov);
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
    if (ncv < 3) return;

    /* OVER-BRIGHTENING. The reference is out = clamp(c * bri / 64, 0, 255),
     * so bri > 64 makes a texel BRIGHTER than itself. Plain GL_MODULATE
     * cannot: glColor clamps at 1.0, so everything above neutral collapsed
     * to neutral and the frame came out uniformly dark -- measured ~16-24
     * luminance below the reference on frames carrying bri up to 173, while
     * frames with bri==64 throughout matched exactly.
     *
     * GL_RGB_SCALE (texture_env_combine, GL 1.3) multiplies the combiner
     * output by 1, 2 or 4 AFTER the modulate, which is precisely the missing
     * headroom. The scale is ALWAYS 4 with the colour pre-divided by 4:
     * dividing and re-multiplying by a power of two commutes with rounding
     * at ANY precision (it only moves the exponent), so this is bit-exact
     * against picking the smallest scale per quad -- and a constant scale
     * stops rgb_scale changes breaking the batch (~200 runs a frame in Dirt
     * Dash's city). bri <= 173 measured, so bri/4 <= 0.68: no vertex clamp. */
    int rgb_scale = 4;

    double _tg = eng_now();
    /* 1.0 = every quad, as before; with write_prio_alpha, the prioverchar bit */
    const float prio_a = !cfg->write_prio_alpha ? 1.0f : ((q->cmode & 7) == 1 ? 1.0f : 0.0f);
    const int base_alpha = !cfg->write_prio_alpha;      /* that mode writes the prio bit untested */

    /* SINGLE-PASS FOG setup: textured quads with the prio channel off draw the
     * CZ fog in the same call (two combiner units, see qs_fog). The vertex
     * alpha carries the fog factor (1 = unfogged), the run state the mode and
     * fog colour. Solids and prio-mode quads keep the second pass below. */
    int fogm = 0;
    float fogrgb[3] = { 0.0f, 0.0f, 0.0f };
    float fogv[32];
    eng_fog f1;
    if (!solid && base_alpha && mt_state == 1 && cfg->fog && cfg->fog_quad &&
        !(q->cz_adjust & 0x800000)) {
        memset(&f1, 0, sizeof f1);
        f1.alpha_const = -1;
        if (cfg->fog_quad(q, &f1)) {
            fogrgb[0] = f1.rgb[0] / 255.0f; fogrgb[1] = f1.rgb[1] / 255.0f; fogrgb[2] = f1.rgb[2] / 255.0f;
            if (cfg->fade_rgb) cfg->fade_rgb(&fogrgb[0], &fogrgb[1], &fogrgb[2]);   /* fade applies after fog */
            int any = 0;
            for (int i = 0; i < ncv; i++) {
                int32_t zz = (cv[i].w > 0.0f) ? (int32_t)(1.0f / cv[i].w) : 1;
                int a = eng_fog_alpha(&f1, zz);
                if (a < g_fogA_min) g_fogA_min = a;
                if (a > g_fogA_max) g_fogA_max = a;
                fogv[i] = a / 255.0f;
                if (a < 255) any = 1;
            }
            if (any) { fogm = cfg->fog_before_shade ? 1 : 2; g_fogged_quads++; }
        }
    }

    if (solid) {
        /* SOLID through the batch's CURRENT page: with the white texel at
         * (0,0) of every atlas page, a solid quad is a textured quad sampling
         * white -- tex_on stays 1, tex is the run's page, and with rgb_scale
         * a constant 4 the state usually matches the run: solid quads no
         * longer break it (~500 tex breaks a frame in Dirt Dash's city).
         * Colours go in /4 like every shaded quad; the x4 rescale is exact. */
        GLuint wtex = 0;
        if (tex_bake_atlas_active() && !cfg->fog_before_shade)      /* fog-before-shade solids keep the whole legacy path (Rave Racer) */
            wtex = (bb_valid && bb_run.tex_on) ? bb_run.tex : tex_bake_white_tex();
        if (wtex) {
            /* fog on a solid (Super 22 order only: fog-before-shade solids keep
             * the two-pass fog below) */
            if (base_alpha && mt_state == 1 && !cfg->fog_before_shade && cfg->fog && cfg->fog_quad &&
                !(q->cz_adjust & 0x800000)) {
                memset(&f1, 0, sizeof f1);
                f1.alpha_const = -1;
                if (cfg->fog_quad(q, &f1)) {
                    fogrgb[0] = f1.rgb[0] / 255.0f; fogrgb[1] = f1.rgb[1] / 255.0f; fogrgb[2] = f1.rgb[2] / 255.0f;
                    if (cfg->fade_rgb) cfg->fade_rgb(&fogrgb[0], &fogrgb[1], &fogrgb[2]);
                    int any = 0;
                    for (int i = 0; i < ncv; i++) {
                        int32_t zz = (cv[i].w > 0.0f) ? (int32_t)(1.0f / cv[i].w) : 1;
                        int a = eng_fog_alpha(&f1, zz);
                        fogv[i] = a / 255.0f;
                        if (a < 255) any = 1;
                    }
                    if (any) { fogm = 2; g_fogged_quads++; }
                }
            }
            int n = ncv > 32 ? 32 : ncv;
            for (int i = 0; i < n; i++) {
                float k = solid_noshade ? 1.0f : cv[i].bri;
                float cr = solid_rgb[0] * k, cg = solid_rgb[1] * k, cb = solid_rgb[2] * k;
                if (cfg->fade_rgb) cfg->fade_rgb(&cr, &cg, &cb);
                qa_rgba[i*4+0] = cr * 0.25f; qa_rgba[i*4+1] = cg * 0.25f;
                qa_rgba[i*4+2] = cb * 0.25f; qa_rgba[i*4+3] = fogm ? fogv[i] : prio_a;
                qa_st[i*4+0] = 0.0f; qa_st[i*4+1] = 0.0f;
                qa_st[i*4+2] = 0.0f; qa_st[i*4+3] = 1.0f;
                qa_xy[i*2+0] = cv[i].x; qa_xy[i*2+1] = cv[i].y;
            }
            bb_emit(n, &(struct bbstate){ .tex = wtex, .env = 4, .alpha = base_alpha, .blend = 0, .prio = 0,
                                          .tex_on = 1, .sc_on = scissored, .sc = { scbox[0], scbox[1], scbox[2], scbox[3] },
                                          .fog = fogm, .frgb = { fogrgb[0], fogrgb[1], fogrgb[2] } });
            g_perf_gl += eng_now() - _tg;
            goto fog_pass;      /* an unfogged-here solid still gets the two-pass fog when single-pass was unavailable */
        }
        int n = ncv > 32 ? 32 : ncv;
        for (int i = 0; i < n; i++) {
            float k = solid_noshade ? 1.0f : cv[i].bri;
            float cr = solid_rgb[0] * k, cg = solid_rgb[1] * k, cb = solid_rgb[2] * k;
            if (cfg->fade_rgb) cfg->fade_rgb(&cr, &cg, &cb);
            qa_rgba[i*4+0] = cr; qa_rgba[i*4+1] = cg;
            qa_rgba[i*4+2] = cb; qa_rgba[i*4+3] = prio_a;
            qa_xy[i*2+0] = cv[i].x; qa_xy[i*2+1] = cv[i].y;
        }
        bb_emit(n, &(struct bbstate){ .tex_on = 0, .alpha = base_alpha, .blend = 0, .prio = 0,
                                      .sc_on = scissored, .sc = { scbox[0], scbox[1], scbox[2], scbox[3] } });
    } else {
    {
        float inv = 1.0f / (float)rgb_scale;
        int n = ncv > 32 ? 32 : ncv;
        for (int i = 0; i < n; i++) {
            float cr = cv[i].bri * inv, cg = cv[i].bri * inv, cb = cv[i].bri * inv;
            if (cfg->fade_rgb) cfg->fade_rgb(&cr, &cg, &cb);
            qa_rgba[i*4+0] = cr; qa_rgba[i*4+1] = cg;
            qa_rgba[i*4+2] = cb; qa_rgba[i*4+3] = fogm ? fogv[i] : prio_a;
            /* the atlas slot origin is folded in HERE, post-clip, as s + ou*q --
             * exactly what the GL texture matrix computed per emitted vertex
             * (matrix follows clipping on the GPU), so batching no longer
             * breaks per slot. bou/bov are 0 with per-quad textures: s+0 == s. */
            qa_st[i*4+0] = cv[i].s + bou * cv[i].w; qa_st[i*4+1] = cv[i].t + bov * cv[i].w;
            qa_st[i*4+2] = 0.0f;   qa_st[i*4+3] = cv[i].w;
            qa_xy[i*2+0] = cv[i].x; qa_xy[i*2+1] = cv[i].y;
        }
        bb_emit(n, &(struct bbstate){ .tex = tex, .env = rgb_scale, .alpha = base_alpha, .blend = 0, .prio = 0,
                                      .tex_on = 1, .sc_on = scissored, .sc = { scbox[0], scbox[1], scbox[2], scbox[3] },
                                      .fog = fogm, .frgb = { fogrgb[0], fogrgb[1], fogrgb[2] } });
    }
    }
    g_perf_gl += eng_now() - _tg;

    /* ---- CZ depth fog (two-pass fallback) ----------------------------------
     * Textured quads with the prio channel off draw fog in the SAME call (the
     * single-pass path above). This second blended pass remains for solids and
     * prio-mode quads, and for everything when multitexture is unavailable
     * (ENG_FOG2PASS=1):
     *     cz = min(z >> 8, 0x1fff);  ff = cztab[cz] + sdelta
     *     rgb = blend(rgb, fog_rgb, 0xff - min(ff, 0xff))
     * result = fog*a + dst*(1-a), which is exactly blend().
     * Depth test is off on this path (painter's algorithm), so the overlay
     * lands exactly on the quad just drawn. */
fog_pass:
    if (fogm == 0 && cfg->fog && cfg->fog_quad) {
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
                  /* texturing is OFF for this pass: texcoords are not sampled */
                  bb_emit(n, &(struct bbstate){ .tex_on = 0, .alpha = 0, .blend = 1,
                                                .prio = cfg->write_prio_alpha ? 1 : 0,
                                                .sc_on = scissored, .sc = { scbox[0], scbox[1], scbox[2], scbox[3] } }); }
            }
        }
    }
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
