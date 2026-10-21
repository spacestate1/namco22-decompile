/*
 * wide_cull.h -- WIDESCREEN: a game's own angle cull, widened with the picture (Hor+).
 *
 * Cyber Commando and Cyber Sled decide on the 68K which objects to send to the DSPs by their bearing from the camera:
 * |atan2(object - camera) - heading| < LIMIT, LIMIT being the arcade's horizontal half-view plus some slack for the object's size
 * (Cyber Commando 0x1D00 = 40.8 deg against a 30 deg half-view, Cyber Sled 0x2500 = 52 deg against 36.7 deg). Widescreen draws the
 * same projection wider, so objects past the 4:3 edge were culled before the side strips could show them -- scenery popping in
 * and out at the screen's sides. eng_wide_cull moves LIMIT out by exactly what the half-view gained:
 *     LIMIT' = LIMIT + atan(tan(half) * k) - half,   k = the picture's half-width / the arcade's (1 at 4:3 = the arcade's own)
 * Angles are 68K units, 0x10000 to the full turn. The result saturates below a half turn (0x7FFF: everything passes).
 * ENG_WIDE_CULL=0 (A/B tests): the arcade's limit at any width.
 */
#ifndef ENG_WIDE_CULL_H
#define ENG_WIDE_CULL_H
#include <stdint.h>
#include <stdlib.h>
#include <math.h>

static inline uint32_t eng_wide_cull(uint32_t limit, double half_deg, double k)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("ENG_WIDE_CULL"); on = !(e && *e == '0'); }
    if (!on || !(k > 1.0001)) return limit;                  /* 4:3 (or narrower, or no picture yet): the arcade's limit */
    const double pi = 3.14159265358979323846, h = half_deg * pi / 180.0;
    const double gain = atan(tan(h) * k) - h;                /* radians the half-view gained */
    const double l = limit + gain * 65536.0 / (2.0 * pi) + 0.5;
    return l >= 0x7FFF ? 0x7FFFu : (uint32_t)l;
}

#endif
