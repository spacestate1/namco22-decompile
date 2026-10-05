/* eng_vsync.h -- ONE frame pacer for every windowed game (header only): the GAME always runs at the board's 59.906 Hz, whatever
 * the display does; what can be chosen is how many PICTURES a second are shown (Display > Frame rate).
 *
 *   eng_vsync_init(&v, win, force)   after the GL context. AUTO: vsync only on a ~60 Hz display (59..61 Hz); elsewhere -- 120/144/
 *                                    165 Hz, or a display that reports 0 -- a timer. force = "0"/"1" (the game's <TAG>_VSYNC).
 *   eng_vsync_set_lock(&v, fps)      60: one picture per game frame, vsync where it blocks (the rule above).
 *                                    0 = Auto, THE DEFAULT: the DISPLAY's refresh rate (120 on a 120 Hz monitor: each picture twice), vsync on;
 *                                    on a 60 Hz display (or one that reports no rate) it is 60. Any other choice
 *                                    (eng_vsync_rates[]) locks the pictures to fps a second: vsync off, a timer.
 *                                    The game still runs 59.906 frames a second: below that some frames are not shown (30 = every
 *                                    other one, 24 = 3:2 pull-down), above it a picture is shown again at the extra ticks (120 = each
 *                                    twice, evenly spaced) -- re-shown from a copy of the finished picture, so no game state moves.
 *   eng_vsync_want(&v, fps)         the menu's choice, every frame (ignored while ENG_FPS forces one).
 *   eng_vsync_show(&v)               per game frame, before swapping: false = no picture this frame (do not swap).
 *   eng_vsync_capture(&v, win)       just before the frame's SDL_GL_SwapWindow (also marks the frame as shown).
 *   eng_vsync_after_frame(&v, win)   after the frame (swapped or not): the extra pictures, then the wait to the next game frame.
 *                                    AUTO trusts vsync only while it BLOCKS -- re-measured every 50 frames, because a driver set to
 *                                    "vsync off", an unmapped / occluded / minimised window, Wayland and some gamescope set-ups swap at
 *                                    once and the game would run as fast as the machine allows (GitHub #26: "the games run too fast").
 *                                    The timer sleeps to each tick, the last 1.5 ms spun; behind by > 100 ms (a stall) it resyncs
 *                                    instead of running a catch-up burst.
 *   eng_vsync_resync(&v)             after a stop (menu, pause, a long load): the clocks start from now.
 * While a lock is on, one line every 600 game frames says what was MEASURED (ENG_FPSLOG=1: in AUTO too):
 *   [FPS] locked 30: 30.00 pictures/s (600 game frames, 59.91/s)
 * ENG_FPS=<n> forces a choice without touching the cfg (tests; 0 = Auto); ENG_DISPLAY_HZ=<n> pretends the display runs at n Hz. ENG_VSYNC_CHECK=1: every re-shown picture is read back at 16 sample
 * pixels and compared with the original; the count is in the [FPS] line.
 * History: Tokyo Wars' host learnt the "does it block?" rule; Rave Racer checked only its first 60 frames; Prop Cycle had no limit
 * (vsync on unconditionally: 2.4x speed on a 144 Hz display). */
#ifndef ENG_VSYNC_H
#define ENG_VSYNC_H
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL2/SDL.h>
#include "eng_gl.h"                                 /* <GL/gl.h>, with <windows.h> first on Windows */

#define ENG_FRAME_NS 16693000ull                    /* 1 / 59.906 Hz, 25.6 MHz / 814 / 525 */

static const int eng_vsync_rates[] = { 0, 24, 30, 50, 60, 75, 90, 120, 144, 165, 240 };   /* 0 = Auto, the default (the display's rate; 60 on a 60 Hz display) */
#define ENG_VSYNC_60 60                             /* one picture per game frame */
#define ENG_VSYNC_DEFAULT 0                         /* Auto (on a 60 Hz display: 60) */
#define ENG_VSYNC_NRATES ((int)(sizeof eng_vsync_rates / sizeof eng_vsync_rates[0]))

typedef struct {
    bool vsync, auto_vsync;                          /* vsync on now; what AUTO chose at init */
    int hz, vs_frames, lock, choice;                 /* display Hz; the block check; 0 = one picture per game frame (60) else pictures/s; the menu's choice */
    uint64_t vs_t0, next_ns;                         /* the block check; the current game frame's start (its end in AUTO-vsync) */
    uint64_t tp, pnext;                              /* locked: the picture period and the next picture tick */
    bool shown;                                      /* this game frame's picture was swapped */
    unsigned tex, tw, th; int cw, ch;                /* the kept picture (a power-of-two texture; the used corner) */
    unsigned char sample[16][4]; int check;          /* ENG_VSYNC_CHECK: 16 pixels of the original; -1 unset */
    unsigned log_frames, log_pics, log_checked, log_bad; uint64_t log_t0; int log_on;
    bool forced;                                     /* ENG_FPS set: the menu's choice is ignored */
} eng_vsync;

static inline uint64_t eng_vsync_ns(void)          /* SDL's counter (works on Windows too); split so counter * 1e9 cannot overflow */
{
    const uint64_t c = SDL_GetPerformanceCounter(), f = SDL_GetPerformanceFrequency();
    return c / f * 1000000000ull + c % f * 1000000000ull / f;
}

static inline const char *eng_vsync_mode(const eng_vsync *v)
{
    static char b[64];
    if (v->lock) { snprintf(b, sizeof b, "%s %d pictures/s%s (game 59.906 Hz)", v->choice == 0 ? "Auto: the display's" : "locked at", v->lock, v->vsync ? ", vsync" : ""); return b; }
    return v->vsync ? "vsync" : "timer-paced at 59.906 Hz";
}

/* the label of a choice, for the menus */
static inline const char *eng_vsync_rate_name(int fps)
{
    static char b[48];
    if (fps == 0) return "Auto (default: the display's rate)";
    if (fps == ENG_VSYNC_60) return "60 fps (one per game frame)";
    snprintf(b, sizeof b, "%d fps", fps); return b;
}
/* the next choice after fps in direction d (+1 / -1), cycling */
static inline int eng_vsync_rate_step(int fps, int d)
{
    int i = 0;
    for (int k = 0; k < ENG_VSYNC_NRATES; k++) if (eng_vsync_rates[k] == fps) i = k;
    return eng_vsync_rates[((i + d) % ENG_VSYNC_NRATES + ENG_VSYNC_NRATES) % ENG_VSYNC_NRATES];
}
/* a saved value made safe: one of the choices, else the default (Auto) */
static inline int eng_vsync_rate_valid(int fps)
{
    for (int k = 0; k < ENG_VSYNC_NRATES; k++) if (eng_vsync_rates[k] == fps) return fps;
    return ENG_VSYNC_DEFAULT;
}

static inline void eng_vsync_resync(eng_vsync *v) { v->next_ns = v->pnext = eng_vsync_ns(); v->vs_frames = 0; }

static inline void eng_vsync_set_lock(eng_vsync *v, int fps)
{
    fps = eng_vsync_rate_valid(fps);
    if (fps == v->choice) return;
    v->choice = fps;
    int lock; bool want_vsync;
    if (fps == ENG_VSYNC_60 || (fps == 0 && (v->hz <= 0 || (v->hz >= 59 && v->hz <= 61)))) {
        lock = 0; want_vsync = v->auto_vsync;        /* 60, and Auto on a 60 Hz display: one picture per game frame, vsync while it blocks */
    } else if (fps == 0) {
        lock = v->hz; want_vsync = true;            /* Auto elsewhere: the display's own rate, vsync for a tear-free picture (the timer still paces) */
    } else {
        lock = fps; want_vsync = false;             /* a fixed rate: the timer */
    }
    v->lock = lock;
    v->tp = lock ? 1000000000ull / (uint64_t)lock : ENG_FRAME_NS;
    v->vsync = want_vsync && SDL_GL_SetSwapInterval(1) == 0;
    if (!v->vsync) SDL_GL_SetSwapInterval(0);
    v->log_t0 = 0;
    eng_vsync_resync(v);
    fprintf(stderr, "[HOST] pictures: %s\n", eng_vsync_mode(v));
}

static inline void eng_vsync_init(eng_vsync *v, SDL_Window *win, const char *force)
{
    memset(v, 0, sizeof *v);
    SDL_DisplayMode dm;
    v->hz = SDL_GetCurrentDisplayMode(SDL_GetWindowDisplayIndex(win), &dm) == 0 ? dm.refresh_rate : 0;
    if (getenv("ENG_DISPLAY_HZ")) v->hz = atoi(getenv("ENG_DISPLAY_HZ"));   /* tests: what Auto would do on another display */
    v->auto_vsync = v->hz >= 59 && v->hz <= 61;
    if (force) v->auto_vsync = atoi(force) != 0;
    v->vsync = v->auto_vsync;
    if (v->vsync && SDL_GL_SetSwapInterval(1) != 0) v->vsync = v->auto_vsync = false;
    if (!v->vsync) SDL_GL_SetSwapInterval(0);
    v->tp = ENG_FRAME_NS;
    v->choice = -1;                                  /* nothing chosen yet: the first eng_vsync_want applies */
    v->log_on = getenv("ENG_FPSLOG") != NULL;
    v->check = getenv("ENG_VSYNC_CHECK") ? -1 : 0;
    eng_vsync_resync(v);
    { const char *e = getenv("ENG_FPS"); if (e) { eng_vsync_set_lock(v, atoi(e)); v->forced = true; } }   /* tests: a lock without touching the cfg */
}

/* the menu's choice (called every frame; a no-op unless it changed) */
static inline void eng_vsync_want(eng_vsync *v, int fps) { if (!v->forced) eng_vsync_set_lock(v, fps); }

/* this game frame: is a picture due? (AUTO: always) */
static inline bool eng_vsync_show(eng_vsync *v)
{
    v->shown = false;
    if (!v->lock) return true;
    return v->pnext < v->next_ns + ENG_FRAME_NS;
}

static inline void eng_vsync_samples(eng_vsync *v, int w, int h, unsigned char out[16][4])
{
    glReadBuffer(GL_BACK);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    for (int i = 0; i < 16; i++) glReadPixels(w * (i % 4 * 2 + 1) / 8, h * (i / 4 * 2 + 1) / 8, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, out[i]);
    (void)v;
}

static inline void eng_vsync_wait(uint64_t until);
static inline void eng_vsync_capture(eng_vsync *v, SDL_Window *win)
{
    v->shown = true;
    if (v->lock && v->pnext < v->next_ns + ENG_FRAME_NS) eng_vsync_wait(v->pnext);   /* the picture goes out ON its tick: even spacing */
    if (!v->lock || v->tp >= ENG_FRAME_NS) return;  /* nothing to show twice */
    int w, h; SDL_GL_GetDrawableSize(win, &w, &h);
    if (w <= 0 || h <= 0) return;
    unsigned tw = 1, th = 1; while (tw < (unsigned)w) tw <<= 1; while (th < (unsigned)h) th <<= 1;
    GLint old; glGetIntegerv(GL_TEXTURE_BINDING_2D, &old);
    if (!v->tex) glGenTextures(1, &v->tex);
    glBindTexture(GL_TEXTURE_2D, v->tex);
    if (tw != v->tw || th != v->th) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, (GLsizei)tw, (GLsizei)th, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        v->tw = tw; v->th = th;
    }
    glReadBuffer(GL_BACK);
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, w, h);
    v->cw = w; v->ch = h;
    glBindTexture(GL_TEXTURE_2D, (GLuint)old);
    if (v->check) { eng_vsync_samples(v, w, h, v->sample); v->check = 1; }
}

/* the kept picture, drawn over the whole window (fixed function; the state it touches is saved and restored) */
static inline void eng_vsync_redraw(eng_vsync *v, SDL_Window *win)
{
    int w, h; SDL_GL_GetDrawableSize(win, &w, &h);
    glPushAttrib(GL_ALL_ATTRIB_BITS);
    GLint old; glGetIntegerv(GL_TEXTURE_BINDING_2D, &old);
    glViewport(0, 0, w, h);
    glDisable(GL_DEPTH_TEST); glDisable(GL_BLEND); glDisable(GL_ALPHA_TEST); glDisable(GL_SCISSOR_TEST);
    glDisable(GL_FOG); glDisable(GL_LIGHTING); glDisable(GL_CULL_FACE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glMatrixMode(GL_PROJECTION); glPushMatrix(); glLoadIdentity(); glOrtho(0, w, 0, h, -1, 1);
    glMatrixMode(GL_MODELVIEW); glPushMatrix(); glLoadIdentity();
    glEnable(GL_TEXTURE_2D); glBindTexture(GL_TEXTURE_2D, v->tex);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    const float u = (float)v->cw / (float)v->tw, t = (float)v->ch / (float)v->th;
    glColor4f(1, 1, 1, 1);
    glBegin(GL_QUADS);                                /* window y is bottom-up, as glCopyTexSubImage2D read it */
    glTexCoord2f(0, 0); glVertex2f(0, 0);
    glTexCoord2f(u, 0); glVertex2f((float)v->cw, 0);
    glTexCoord2f(u, t); glVertex2f((float)v->cw, (float)v->ch);
    glTexCoord2f(0, t); glVertex2f(0, (float)v->ch);
    glEnd();
    glPopMatrix(); glMatrixMode(GL_PROJECTION); glPopMatrix(); glMatrixMode(GL_MODELVIEW);
    glBindTexture(GL_TEXTURE_2D, (GLuint)old);
    glPopAttrib();
    if (v->check == 1) {                              /* ENG_VSYNC_CHECK: the re-shown picture must be the original */
        unsigned char s[16][4]; eng_vsync_samples(v, w, h, s);
        int bad = 0;
        for (int i = 0; i < 16; i++) bad |= memcmp(s[i], v->sample[i], 3) != 0;
        v->log_checked++; v->log_bad += bad;
    }
}

static inline void eng_vsync_wait(uint64_t until)
{
    const uint64_t now = eng_vsync_ns();
    if (until <= now) return;
    const uint64_t left = until - now;
    if (left > 2000000ull) SDL_Delay((Uint32)((left - 1500000ull) / 1000000ull));
    while (eng_vsync_ns() < until) ;
}

static inline void eng_vsync_log(eng_vsync *v)
{
    if (!v->lock && !v->log_on) return;
    const uint64_t now = eng_vsync_ns();
    if (!v->log_t0) { v->log_t0 = now; v->log_frames = v->log_pics = v->log_checked = v->log_bad = 0; return; }
    if (++v->log_frames < 600) return;
    const double s = (double)(now - v->log_t0) / 1e9;
    char chk[64] = "";
    if (v->check) snprintf(chk, sizeof chk, "; re-shown pictures checked %u, differing %u", v->log_checked, v->log_bad);
    fprintf(stderr, "[FPS] %s: %.2f pictures/s (%u game frames, %.2f/s)%s\n",
            v->lock ? "locked" : eng_vsync_mode(v), v->log_pics / s, v->log_frames, v->log_frames / s, chk);
    v->log_t0 = now; v->log_frames = v->log_pics = v->log_checked = v->log_bad = 0;
}

static inline void eng_vsync_after_frame(eng_vsync *v, SDL_Window *win)
{
    if (!v->lock) {                                  /* AUTO */
        if (v->shown) v->log_pics++;
        if (v->vsync) {
            if (v->vs_frames++ == 10) v->vs_t0 = eng_vsync_ns();
            if (v->vs_frames == 60) {
                const uint64_t per = (eng_vsync_ns() - v->vs_t0) / 50;
                v->vs_frames = 10; v->vs_t0 = eng_vsync_ns();
                if (per < 12000000ull) {
                    v->vsync = v->auto_vsync = false; SDL_GL_SetSwapInterval(0); v->next_ns = eng_vsync_ns();
                    fprintf(stderr, "[HOST] vsync does not block here (%.1f ms a frame): timer-paced at 59.906 Hz\n", (double)per / 1e6);
                }
            }
        }
        if (!v->vsync) {
            v->next_ns += ENG_FRAME_NS;
            const uint64_t now = eng_vsync_ns();
            if (v->next_ns > now) eng_vsync_wait(v->next_ns);
            else if (now - v->next_ns > 100000000ull) v->next_ns = now;
        }
        eng_vsync_log(v);
        return;
    }
    /* LOCKED: this game frame is [next_ns, next_ns + frame); picture ticks every tp from pnext */
    const uint64_t end = v->next_ns + ENG_FRAME_NS;
    { const uint64_t t = eng_vsync_ns();                         /* ticks missed during a slow frame are dropped, not shown in a burst */
      if (t > v->pnext + v->tp) v->pnext += (t - v->pnext) / v->tp * v->tp; }
    if (v->pnext < end) {
        if (v->shown) v->log_pics++;
        v->pnext += v->tp;                                       /* the frame's own picture took the first tick */
    }
    while (v->pnext < end) {                                     /* the extra ticks: the same picture again */
        if (v->shown && v->tex && win) {
            eng_vsync_wait(v->pnext);
            eng_vsync_redraw(v, win);
            SDL_GL_SwapWindow(win);
            v->log_pics++;
        }
        v->pnext += v->tp;
    }
    v->next_ns = end;
    const uint64_t now = eng_vsync_ns();
    if (end > now) eng_vsync_wait(end);
    else if (now - end > 100000000ull) eng_vsync_resync(v);    /* fell behind: resync, no catch-up burst */
    eng_vsync_log(v);
}

/* a frame that always swapped (callers that do not ask eng_vsync_show) */
static inline void eng_vsync_after_swap(eng_vsync *v) { v->shown = true; eng_vsync_after_frame(v, NULL); }
#endif
