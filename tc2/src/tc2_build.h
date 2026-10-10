/* tc2_build.h -- ONE FRAME, BUILT: what both picture back ends draw.
 *   The geometry stage (src/tc2_render.c, on the worker thread) turns a frame snapshot (src/tc2_frame.h) into screen-space polygons in the
 *   hardware's draw order (z-sort key, far to near) plus the C361 text layer as a pen map. Then either
 *     - the GPU back end (src/tc2_gl.c): one draw call over all polygons in that order, texturing / shading / fade / alpha in a shader, the
 *       text layer and the polygon-over-text priority in two more passes -- at any internal resolution; or
 *     - the software back end (src/tc2_render.c): the same at 640 x 480 on the CPU -- the fallback, and the oracle the GPU is gated against.
 */
#ifndef TC2_BUILD_H
#define TC2_BUILD_H
#include <stdint.h>
#include "tc2_frame.h"

typedef struct { float x, y, p[4]; } tc2_pvert;       /* screen x / y (projected, before the viewport offset); p[0] = 1/z, p[1..3] = u, v, shade times 1/z */
typedef struct {
    tc2_pvert pv[8]; int n;
    uint32_t zkey, index;
    int stencil, cmode, tbase, color_base, blend, prioverchar, alpha_enabled;
    int vp_size_x, vp_size_y, vp_offset_x, vp_offset_y;
    int entry, model;                                  /* the render-list entry it came from and its model (-1 = immediate / direct) */
} tc2_poly;
/* the C404's per-frame values every polygon uses */
typedef struct { int pr, pg, pb, alpha_color, alpha_pen, alpha, fr, fg, fb, ffactor, fflags, layers; uint32_t bg; } tc2_fx;

#define TC2_MAX_POLYS 30000
typedef struct {
    tc2_frame_state fs;                                /* the snapshot */
    tc2_fx fx;
    tc2_poly polys[TC2_MAX_POLYS]; int npoly;
    uint32_t order[TC2_MAX_POLYS];                     /* indices into polys[], in draw order */
    uint16_t mix[640 * 480];                           /* the text layer: pen | 0x8000 where opaque, 0 where transparent */
    uint16_t band[480];                                /* widescreen: a text row opaque all the way across (a letterbox band): its commonest
                                                        * pen | 0x8000 -- the side strips continue it; 0 = not a band */
} tc2_built;

/* the newest built frame (the window's renderer reads it on the main thread); NULL before the first */
const tc2_built *tc2_built_latest(void);
const tc2_built *tc2_built_latest_wait(void);          /* waits for the frame that just ended */

/* the GPU back end (src/tc2_gl.c) */
int  tc2_gl_init(void);                                /* after the window's GL context exists: 0 = not available (the software picture is used) */
int  tc2_gl_ok(void);
void tc2_gl_set_scale(int s);                          /* the internal resolution: 640 * s x 480 * s */
unsigned tc2_gl_draw(const tc2_built *b);              /* draws b; returns the GL texture holding the picture */
int  tc2_gl_scale(void);
void tc2_gl_set_wide(int e);                           /* widescreen: the picture is (640 + 2e) x 480 board pixels, the 3D wider (Hor+), the text in the 4:3 centre */
int  tc2_gl_wide(void);
int  tc2_gl_read_full(uint8_t *rgb, int cap);           /* the whole target, RGB, top row first: returns its width */
void tc2_gl_read(uint32_t *rgba);                      /* the last picture at 640 x 480 (0x00RRGGBB, top row first) -- screenshots, the gate */
/* the software back end, on a built frame (the gate) */
void tc2_soft_raster(const tc2_built *b, uint32_t *rgba);
#endif
