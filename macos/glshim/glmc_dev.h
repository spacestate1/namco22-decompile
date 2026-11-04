/* glmc_dev.h — the device layer (glmc_metal.mm) as seen by the GL state
 * machine (glmc_main.c). The face implements every real GPU operation on
 * Metal; the GL state machine decides what to draw and why, keeps the call
 * shapes the games were written against, and converts pixel/vertex data into
 * the exact layouts listed here. Coordinates: all rects are in framebuffer
 * pixels with y measured from the BOTTOM (GL window space), so the face maps
 * to Metal directly. */
#ifndef GLMC_DEV_H
#define GLMC_DEV_H

#include <stdint.h>
#include "glmc.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- pixel storage formats --------------------------------------- */
enum {
    GLMC_PX_RGBA8 = 0,      /* MTLPixelFormatRGBA8Unorm (also GL_RGB, GL_LUMINANCE expanded) */
    GLMC_PX_BGRA8,          /* MTLPixelFormatBGRA8Unorm (GL_BGRA uploads, e.g. tc2 palette) */
    GLMC_PX_R8,             /* MTLPixelFormatR8Unorm (GL_LUMINANCE, tc2 t_rom) */
    GLMC_PX_RG8,            /* MTLPixelFormatRG8Unorm (GL_LUMINANCE_ALPHA) */
    GLMC_PX_DEPTH32F,       /* MTLPixelFormatDepth32Float (GL depth attachment) */
    GLMC_PX_RGB8            /* readback-only shape: 3 bytes/px, expanded here */
};

/* semantics of the R8/RG8 stores in the tc2 MSL: what GL would have sampled.
 * luminance: c = (l, l, l, 1). luminance_alpha: c = (l, l, l, a). Since MSL
 * gets (l, 0, 0, 1) or (l, a, 0, 1), helpers glmc_lum/glmc_lalpha rebuild the
 * GL fetch result — see glmc_msl.h. */

/* ---------- program ids -------------------------------------------------- */
enum {
    GLMC_PROG_FFP = 0,      /* fixed function: env combiners, matrices, arrays */
    GLMC_PROG_POST1,        /* engine/post_gl.c FS (frame, lut) over the FFP vertex path */
    GLMC_PROG_TC2_POLY,     /* tc2/src/tc2_gl.c vs_poly/fs_poly */
    GLMC_PROG_TC2_TEXT,     /* tc2 fs_text with vs_text (a_pos only) */
    GLMC_PROG_TC2_GAMMA,    /* tc2 fs_gamma with vs_text */
    GLMC_PROG_COUNT
};

/* topology the face understands (the C layer converts everything else) */
enum {
    GLMC_TOPO_TRI = 0,      /* triangle list, optional u16 indices */
    GLMC_TOPO_TRI_FAN,      /* tc2 fs_gamma? not used; kept for completeness */
    GLMC_TOPO_LINE_LIST,    /* 2-vertex line pairs (engine line drawing) */
    GLMC_TOPO_COUNT
};

/* ---------- draw request ------------------------------------------------- */
typedef struct glmcd_blend {
    int    on;
    int    f_srgb, f_drgb, f_salpha, f_dalpha;   /* GL blend factor codes */
    int    eq_rgb, eq_a;                          /* GL blend equation codes */
} glmcd_blend;

typedef struct glmcd_pipekey {
    uint16_t prog;              /* GLMC_PROG_* */
    uint16_t topo;              /* GLMC_TOPO_* (fan only as a hint) */
    glmcd_blend blend;
    uint8_t  depth_on, depth_write;
    uint16_t depth_func;        /* full GL compare enum (0x0200..0x0207) */
    uint8_t  af_on, af_func;    /* alpha test func (small code 0..8) */
    uint8_t  mask_r, mask_g, mask_b, mask_a;
    uint8_t  scissor;           /* 1 = a scissor rect is live (set per encoder) */
    uint8_t  fragdepth;         /* program may write gl_FragDepth (tc2 pre-pass) */
} glmcd_pipekey;

typedef struct glmcd_samp {     /* sampling state for one texture node */
    uint32_t tex;               /* device texture id (0 = none) */
    int      minf, magf, wraps, wrapt;   /* GL filter/wrap codes */
} glmcd_samp;

typedef struct glmcd_vlayout {  /* how the vertex blob maps to MSL attributes */
    int nattr;                  /* 3 (FFP) / 1 (tc2 text) / 5 (tc2 poly) */
    int slot[5];                /* MSL attribute index of attr k */
    int fmt[5];                 /* MTLVertexFormat code (see usages) */
    int off[5];                 /* byte offset in each vertex */
    int stride;                 /* bytes per vertex */
} glmcd_vlayout;

/* ---------- uniforms (byte layouts must match the MSL in glmc_msl.h) ----- */
/* FFP: one uniform block per draw. The 28 ints after the two env colours are
 * E[0..6] in the MSL, 7 int4 rows:
 *   E[0] = unit0 (rgb op, alpha op, rgb scale code, 0)
 *   E[1] = unit0 rgb sources (src0, opnd0, src1, opnd1)
 *   E[2] = unit0 rgb src2 + operand, alpha sources (s2, o2, as0, as1)
 *   E[3..5] = unit1, same shapes
 *   E[6] = (unit0 texture on, unit1 texture on, alpha-test code, alpha-ref*255)
 * Scale code is log2(GL_RGB_SCALE). Source/opnd codes are the FFP_* codes in
 * the MSL. 208 bytes total, as MSL naturally lays it out. */
typedef struct glmcd_ffp_u {
    float mvp[16];              /* GL modelview*projection (column-major) */
    float envc0[4], envc1[4];   /* GL_TEXTURE_ENV_COLOR of unit 0 / 1 */
    int   e[28];
    int   texfmt[4];            /* GLMC_PX_* of bound unit 0 / 1 (rest 0) */
} glmcd_ffp_u;

/* engine/post_gl.c FS: frame (unit 0), lut (unit 1); also its sampler mapping */
typedef struct glmcd_post1_u {
    int frame_unit, lut_unit, pad0, pad1;
} glmcd_post1_u;

/* tc2 fs_poly — uniform ints packed as the MSL reads them */
typedef struct glmcd_tc2poly_u {
    int ex, scale, h, prio;
    int pfade, ff, alpha, alpha_pen;
    int near_on, depth_on, pad0, pad1;
    int pcol[3], pcol_pad;
    int fcol[3], fcol_pad;
    float keep;
    float keep_pad[3];
} glmcd_tc2poly_u;

/* tc2 fs_text */
typedef struct glmcd_tc2text_u {
    int ex, scale, h, pass;
    int fade, ff, alpha, amask;
    int c12, c13, pad0, pad1;
    int fcol[3], fcol_pad;
} glmcd_tc2text_u;

/* tc2 fs_gamma */
typedef struct glmcd_tc2gamma_u {
    int pad;
} glmcd_tc2gamma_u;

/* ---------- per-context draw entry point --------------------------------- */
typedef struct glmcd_drawreq {
    const glmcd_pipekey *pk;
    const void *uniforms;       /* bytes of the program's uniform struct */
    int          uniform_len;
    const void *verts;
    int          vert_bytes;
    const void *idx;            /* NULL = no index buffer */
    int          idx_bytes;     /* bytes (u16 indices) */
    int          first, count;  /* vertices (or indices) */
    glmcd_samp   samp[8];       /* sampler nodes per shader slot */
    int          nsamp;
} glmcd_drawreq;

/* ---------- face --------------------------------------------------------- */
typedef struct glmcd_face glmcd_face;

/* texture ids live in the face; 0 is never returned */
uint32_t glmc_tex_new(glmcd_face *f);
void     glmc_tex_del(glmcd_face *f, uint32_t id);
void     glmc_tex_storage(glmcd_face *f, uint32_t id, int fmt, int w, int h);
void     glmc_tex_upload(glmcd_face *f, uint32_t id, int x, int y, int w, int h,
                         const void *data, int pitch_bytes);
/* width/height of the stored image (0 if none yet) */
void     glmc_tex_size(glmcd_face *f, uint32_t id, int *w, int *h, int *fmt);

/* framebuffer objects */
uint32_t glmc_fbo_new(void);
void     glmc_fbo_del(uint32_t id);
void     glmc_fbo_attach(uint32_t fbo, int slot, uint32_t tex);  /* slot 0 color, 1 depth */
int      glmc_fbo_status(uint32_t fbo);

/* binding and clearing */
void     glmc_dev_bind(glmcd_face *f);                            /* make active for this thread */
void     glmc_dev_fbo(glmcd_face *f, uint32_t fbo);               /* 0 = default (the window) */
void     glmc_dev_read_fbo(glmcd_face *f, uint32_t fbo);          /* GL_READ_FRAMEBUFFER binding */
void     glmc_dev_fbo_size(glmcd_face *f, uint32_t fbo, int *w, int *h);
void     glmc_dev_clear(glmcd_face *f, int mask /*GLMC CLR bits*/,
                        float r, float g, float b, float a, double depth);
enum { GLMC_CLR_COLOR = 1, GLMC_CLR_DEPTH = 2 };

void     glmc_dev_viewport(glmcd_face *f, int x, int y, int w, int h, int fbo_w, int fbo_h);
void     glmc_dev_scissor(glmcd_face *f, int on, int x, int y, int w, int h, int fbo_w, int fbo_h);

void     glmc_dev_draw(glmcd_face *f, const glmcd_drawreq *req);

/* glCopyTexSubImage2D / glCopyTexImage2D equivalent: GPU-to-GPU copy from the
 * current READ framebuffer (the callee resolves it) */
void     glmc_dev_copy_tex(glmcd_face *f, uint32_t dst_tex, int dst_fmt,
                           int dx, int dy, int sx, int sy, int w, int h);

/* glBlitFramebuffer equivalent (color only; the callee guarantees masks) */
void     glmc_dev_blit(glmcd_face *f, int sx0, int sy0, int sx1, int sy1,
                       int dx0, int dy0, int dx1, int dy1, int linear);

/* CPU readback: pixel count w*h*4 RGBA8 unless px_fmt says otherwise */
void     glmc_dev_read(glmcd_face *f, int x, int y, int w, int h,
                       void *out, int out_fmt);

/* present the default target to the window, flush pending work */
void     glmc_dev_present(glmcd_face *f);
void     glmc_dev_flush(glmcd_face *f);
/* the pixel size of the window's drawable (also drives the default target) */
void     glmc_dev_window_size(glmcd_face *f, int *w, int *h);

/* one face redirection per GL "context" */
glmcd_face *glmc_dev_face_new(void *sdl_window /*SDL_Window* or NULL for none*/,
                              int w, int h, float scale);
void     glmc_dev_face_free(glmcd_face *face);
glmcd_face *glmc_dev_current(void);                        /* the bound face (tests) */
void     glmc_dev_face_rewindowed(glmcd_face *face, void *sdl_window,
                                  int w, int h, float scale);
/* follow the window's current Metal drawable size (no-op unless it changed) */
void     glmc_dev_face_refit(glmcd_face *face, void *sdl_window);
void     glmc_dev_set_swap_interval(glmcd_face *face, int v);

/* SDL-side helpers implemented in the face (Metal views) */
void    *glmc_sdl_metal_view(void *sdl_window);            /* SDL_Metal_CreateView */
void    *glmc_sdl_metal_layer(void *metal_view);           /* SDL_Metal_GetLayer  */
void     glmc_sdl_metal_destroy(void *metal_view);
void     glmc_sdl_metal_size(void *sdl_window_or_layer, int *w, int *h);

/* the screen's refresh while the game fills it (fullscreen or the filled
 * window): on != 0 switches the window's display to the fastest refresh at
 * the same resolution that cap divides (144 -> 120 Hz for 120), for this app
 * only; on == 0 restores it. Returns the display's refresh afterwards
 * (0: unknown). Called by eng_display.c. */
int      glmc_display_match_refresh(void *sdl_window, int on, int cap);

/* error surface: the face records why a GL call needs support and refuses */
void     glmc_dev_note(const char *msg);                    /* loud, once */

#ifdef __cplusplus
}
#endif
#endif /* GLMC_DEV_H */
