/* eng_pace.h -- the REAL frame pacing of a windowed run, in the log (header only). One line every 600 frames:
 *   [PACE] 600 frames: interval mean 16.68 ms, max 24.10, 3 over 20 ms (a missed vsync: the frame showed twice); work mean 4.20 ms, max 9.90 (emulation + render)
 * "work" is what the game and the renderer cost per frame (start of the frame to just before the swap); "interval" is what the player sees
 * (swap to swap, vsync and the timer included). Interval fine but work near 16 ms = a machine at its limit; interval bad with work small =
 * pacing / compositor; both are what a report of "not a stable 60 fps" needs. Wall clock, so quote it from an idle machine. */
#ifndef ENG_PACE_H
#define ENG_PACE_H
#include <stdio.h>
#include <stdint.h>
#include <time.h>

typedef struct { uint64_t last, work0; unsigned n, slow; double isum, imax, wsum, wmax; int skip; } eng_pace;

static inline uint64_t eng_pace_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }

/* the game stopped (menu, pause): the next interval is not a frame */
static inline void eng_pace_reset(eng_pace *p) { p->last = 0; p->work0 = 0; p->skip = 60; }

/* just before the swap: the work of this frame */
static inline void eng_pace_before_swap(eng_pace *p)
{
    if (!p->work0 || p->skip > 0) return;
    const double w = (double)(eng_pace_ns() - p->work0) / 1e6;
    p->wsum += w; if (w > p->wmax) p->wmax = w;
}

/* after the swap and the pacing: one frame is over */
static inline void eng_pace_after(eng_pace *p, const char *tag)
{
    const uint64_t t = eng_pace_ns();
    if (p->skip > 0) { p->skip--; p->last = t; p->work0 = t; return; }
    if (p->last) {
        const double iv = (double)(t - p->last) / 1e6;
        p->isum += iv; if (iv > p->imax) p->imax = iv; if (iv > 20.0) p->slow++;
        if (++p->n == 600) {
            fprintf(stderr, "[PACE] %s %u frames: interval mean %.2f ms, max %.2f, %u over 20 ms (a missed vsync); work mean %.2f ms, max %.2f\n",
                    tag, p->n, p->isum / p->n, p->imax, p->slow, p->wsum / p->n, p->wmax);
            p->n = p->slow = 0; p->isum = p->imax = p->wsum = p->wmax = 0;
        }
    }
    p->last = t; p->work0 = t;
}
#endif
