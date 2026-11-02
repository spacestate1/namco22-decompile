/* crt_gl.h -- the Scud Race CRT over the presented picture (engine/crt_gl.c).
 *
 * Three Display rows, Scud's own: Television Off / Trinitron / Arcade, Curvature Off / On, Brightness 0.50..1.50.
 * Saved as crt_tv, crt_curvature, crt_gain (thousandths, 1000 = as the game); the default is off. ENG_CRT=0|1|2 and
 * ENG_CRT_CURVE=0|1 override the saved choice for a run (tests), without saving it.
 */
#ifndef ENG_CRT_GL_H
#define ENG_CRT_GL_H
#include <SDL2/SDL.h>
#include <stdbool.h>
#include <stdlib.h>

#define ENG_CRT_GAIN_MIN  500
#define ENG_CRT_GAIN_MAX  1500
#define ENG_CRT_GAIN_STEP 50
#define ENG_CRT_GAIN_DEF  1000

static inline const char *eng_crt_tv_name(int tv) { return tv == 1 ? "Trinitron" : tv == 2 ? "Arcade" : "Off"; }
static inline int eng_crt_tv_valid(int tv) { return tv >= 0 && tv <= 2 ? tv : 0; }
static inline int eng_crt_gain_valid(int m) { return m < ENG_CRT_GAIN_MIN ? ENG_CRT_GAIN_MIN : m > ENG_CRT_GAIN_MAX ? ENG_CRT_GAIN_MAX : m; }
/* the test overrides, after the settings are loaded */
static inline void eng_crt_env(int *tv, int *curve)
{
    const char *e = getenv("ENG_CRT");
    if (e && *e) *tv = eng_crt_tv_valid(atoi(e));
    e = getenv("ENG_CRT_CURVE");
    if (e && *e) *curve = atoi(e) != 0;
}

/* Present the frame drawn since rt_begin into the window rectangle (x, y top-left, w x h drawable pixels) through the
 * CRT (tv 1/2) or the brightness-only blit (tv 0, gain != 1000); sharp = nearest when scaling the frame to that size.
 * Leaves the window's framebuffer bound, like rt_end_rect. false = it did nothing (CRT off at 1.00, or unavailable):
 * the caller presents the plain picture with rt_end_rect. */
bool eng_crt_present(SDL_Window *win, int x, int y, int w, int h, int sharp, int tv, int curve, int gain_milli);
#endif
