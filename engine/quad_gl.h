/*
 * quad_gl.h -- the engine's polygon rasteriser: geo_hw quads, painter-sorted,
 * drawn with OpenGL (engine/quad_gl.c). Prop Cycle's pipeline, shared by every
 * System 22 / Super 22 game; the board differences are eng_draw_cfg fields.
 */
#ifndef ENG_QUAD_GL_H
#define ENG_QUAD_GL_H
#include <stdint.h>
#include "geo_hw.h"

#define ENG_SCREEN_W 640
#define ENG_SCREEN_H 480

/* The 3D scene's horizontal extent in 640x480 units: (0, 640), or wider for
 * the widescreen mode (Hor+: full-frame viewports widen to fill it). */
extern float g_scene_x0, g_scene_x1;

/* Widescreen HUD (engine/hud_edges.h): how far a game moves this quad (scene units, whole), or NULL / 0 = where the game put it. A quad in
 * a sub-window viewport takes its clip window along. */
extern int (*g_eng_quad_dx)(const geo_quad *q);

/* One quad's depth fog, filled by the board.
 *   Super 22: a per-depth table -- alpha = 0xff - min(tab[z>>8] + sdelta, 0xff)
 *   System 22: one factor per quad -- alpha_const (0..255, 255 = no fog)
 * `alpha` is the blend alpha of blend(rgb, fog_rgb, alpha): 255 = unfogged. */
typedef struct {
    uint8_t        rgb[3];
    const uint8_t *tab; int sdelta;   /* per-depth table, or NULL */
    int            alpha_const;       /* used when tab == NULL */
} eng_fog;
int eng_fog_alpha(const eng_fog *f, int32_t z);

typedef struct {
    int  shade;              /* per-vertex brightness (bri/64) */
    int  fog;                /* run the fog stage */
    int  fog_before_shade;   /* System 22: shade(fog(texel)); Super 22: fog(shade(texel)) */
    int  flat_white;         /* coverage probe: every quad flat white (Prop Cycle's SEAMTEST) */
    /* System 22: write each pixel's "polygon over text" bit (cmode & 7 == 1,
     * MAME's prioverchar) into DESTINATION ALPHA, so the text layer can be
     * drawn only where the last polygon did not claim priority. Needs an
     * alpha channel, texture pen 0 opaque (g_tex_opaque), and turns the
     * alpha test off for the quad. */
    int  write_prio_alpha;
    int  texel_centre;       /* sample (u + 0.5, v + 0.5), as MAME's poly3d_drawquad does */
    /* 1 = this quad is fogged, *f filled. May be NULL (no fog on this board). */
    int  (*fog_quad)(const geo_quad *q, eng_fog *f);
    /* per-channel poly fade, applied after fog; NULL = none */
    void (*fade_rgb)(float *r, float *g, float *b);
    /* Widescreen: a quad that is the game's own screen-sized backdrop (see eng_draw_quad) continues sideways to the picture's edges. */
    int  wide_backdrop;
} eng_draw_cfg;

/* Sort far to near; ties draw in REVERSE submission order (q->order), which
 * is MAME's radix bucket behaviour. tie_emit = 1: emission order (A/B only). */
void eng_quad_sort(geo_quad *buf, int n, int tie_emit);

void eng_draw_begin(void);          /* client vertex arrays on */
void eng_draw_quad(const geo_quad *q, const eng_draw_cfg *cfg);
void eng_draw_end(void);

/* diagnostics / perf, cumulative */
extern double g_perf_bake, g_perf_gl, g_perf_clip;
extern int    g_bri_min, g_bri_max, g_fogged_quads, g_fogA_min, g_fogA_max;
extern int    g_tex_clipbox;          /* 1: key the bake on the clipped polygon (A/B) */
extern int    g_eng_degen_uv_legacy;  /* 1: old single-texel UV mapping (A/B) */
extern int    g_perf_enabled;         /* 0: eng_now() returns 0, no clock reads */
double eng_now(void);
#endif
