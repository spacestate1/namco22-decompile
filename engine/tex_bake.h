/* tex_bake.h -- per-quad texture bake + cache (engine/tex_bake.c). */
#ifndef ENG_TEX_BAKE_H
#define ENG_TEX_BAKE_H
#include <stddef.h>
#include <stdint.h>
#include "eng_gl.h"

void renderer_texture_init(void);

/* Two-stage tilemap texel fetch: returns the 8-bit pen at (u,v). */
uint8_t texture_pen_lookup(int u, int v, int texbank);

/* Bake or fetch a cached GL texture for one quad's UV bounding box.
 * texbank: 0-15 (V word bits [15:12]); pal_group: 0-127; cmode: 0-15 (U word
 * bits [15:12]). *out_su / *out_sv: the used fraction of the allocation, by
 * which the caller scales its texture coordinates. */
GLuint bake_quad_texture(int min_u, int min_v, int range_u, int range_v,
                         int texbank, int pal_group, int cmode,
                         float *out_su, float *out_sv);

extern int    g_tex_opaque;       /* 1 = no pen-0 keying (hardware polygon path) */
extern int    g_tex_bake_cap_req; /* the quad's on-screen extent, window pixels */
extern int    g_tex_fixedcap, g_tex_pow2sample, g_tex_pow2alloc, g_tex_fifo, g_tex_orphan;
extern size_t tex_cache_budget;
extern int    tex_frame_hits, tex_frame_misses, tex_cache_evictions;
extern int    tex_reallocs, tex_subimages;
extern double g_bake_texels;
#endif
