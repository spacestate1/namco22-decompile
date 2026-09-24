/*
 * render_target.c — the chosen RESOLUTION as an internal render size.
 *
 * Escape -> Display -> Resolution used to resize the WINDOW. That is not what
 * a resolution setting means: the window stays whatever size it is, and the
 * game renders at the chosen pixel count and is scaled up (or down) to fill
 * it, the way a monitor scales a low resolution. So the frame is drawn into
 * an offscreen framebuffer of the chosen size and blitted to the window at
 * the end, keeping the resolution's shape (bars) unless the aspect setting is
 * "Stretch to window".
 *
 * Headless runs never come here: their fixed 640x480 frame is what every gate
 * reads, and it must not change. If the driver has no framebuffer objects the
 * whole thing falls back to drawing straight into the window, as before.
 */
#include <SDL2/SDL.h>
#include <GL/gl.h>
#include <stdio.h>
#include "render_target.h"

#ifndef GL_FRAMEBUFFER
#define GL_FRAMEBUFFER          0x8D40
#define GL_READ_FRAMEBUFFER     0x8CA8
#define GL_DRAW_FRAMEBUFFER     0x8CA9
#define GL_RENDERBUFFER         0x8D41
#define GL_COLOR_ATTACHMENT0    0x8CE0
#define GL_DEPTH_ATTACHMENT     0x8D00
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#endif
#ifndef GL_DEPTH_COMPONENT24
#define GL_DEPTH_COMPONENT24    0x81A6
#endif

typedef void   (APIENTRY *pfn_gen)(GLsizei, GLuint *);
typedef void   (APIENTRY *pfn_del)(GLsizei, const GLuint *);
typedef void   (APIENTRY *pfn_bind)(GLenum, GLuint);
typedef void   (APIENTRY *pfn_rbstore)(GLenum, GLenum, GLsizei, GLsizei);
typedef void   (APIENTRY *pfn_fbrb)(GLenum, GLenum, GLenum, GLuint);
typedef GLenum (APIENTRY *pfn_status)(GLenum);
typedef void   (APIENTRY *pfn_blit)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);

static pfn_gen gen_fb, gen_rb;
static pfn_del del_fb, del_rb;
static pfn_bind bind_fb, bind_rb;
static pfn_rbstore rb_storage;
static pfn_fbrb fb_rb;
static pfn_status fb_status;
static pfn_blit blit_fb;

static int loaded;          /* 0 not tried, 1 usable, -1 unavailable */
static GLuint fbo, rb_color, rb_depth;
static int fb_w, fb_h;      /* the allocated size */
static int active;          /* this frame draws into the FBO */

static void *proc(const char *core, const char *ext) {
    void *p = SDL_GL_GetProcAddress(core);
    return p ? p : SDL_GL_GetProcAddress(ext);
}

static int load(void) {
    if (loaded) return loaded > 0;
    gen_fb     = (pfn_gen)proc("glGenFramebuffers", "glGenFramebuffersEXT");
    del_fb     = (pfn_del)proc("glDeleteFramebuffers", "glDeleteFramebuffersEXT");
    bind_fb    = (pfn_bind)proc("glBindFramebuffer", "glBindFramebufferEXT");
    gen_rb     = (pfn_gen)proc("glGenRenderbuffers", "glGenRenderbuffersEXT");
    del_rb     = (pfn_del)proc("glDeleteRenderbuffers", "glDeleteRenderbuffersEXT");
    bind_rb    = (pfn_bind)proc("glBindRenderbuffer", "glBindRenderbufferEXT");
    rb_storage = (pfn_rbstore)proc("glRenderbufferStorage", "glRenderbufferStorageEXT");
    fb_rb      = (pfn_fbrb)proc("glFramebufferRenderbuffer", "glFramebufferRenderbufferEXT");
    fb_status  = (pfn_status)proc("glCheckFramebufferStatus", "glCheckFramebufferStatusEXT");
    blit_fb    = (pfn_blit)proc("glBlitFramebuffer", "glBlitFramebufferEXT");
    loaded = (gen_fb && del_fb && bind_fb && gen_rb && del_rb && bind_rb &&
              rb_storage && fb_rb && fb_status && blit_fb) ? 1 : -1;
    if (loaded < 0)
        fprintf(stderr, "[DISPLAY] no framebuffer objects: resolution follows the window\n");
    return loaded > 0;
}

static int ensure(int w, int h) {
    if (fbo && fb_w == w && fb_h == h) return 1;
    if (fbo) { del_fb(1, &fbo); del_rb(1, &rb_color); del_rb(1, &rb_depth); fbo = rb_color = rb_depth = 0; }
    gen_fb(1, &fbo); gen_rb(1, &rb_color); gen_rb(1, &rb_depth);
    bind_rb(GL_RENDERBUFFER, rb_color);
    rb_storage(GL_RENDERBUFFER, GL_RGBA8, w, h);
    bind_rb(GL_RENDERBUFFER, rb_depth);
    rb_storage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w, h);
    bind_rb(GL_RENDERBUFFER, 0);
    bind_fb(GL_FRAMEBUFFER, fbo);
    fb_rb(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb_color);
    fb_rb(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, rb_depth);
    const GLenum st = fb_status(GL_FRAMEBUFFER);
    bind_fb(GL_FRAMEBUFFER, 0);
    if (st != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "[DISPLAY] %dx%d render target incomplete (0x%x): drawing to the window\n", w, h, st);
        del_fb(1, &fbo); del_rb(1, &rb_color); del_rb(1, &rb_depth); fbo = rb_color = rb_depth = 0;
        return 0;
    }
    fb_w = w; fb_h = h;
    fprintf(stderr, "[DISPLAY] render target %dx%d\n", w, h);
    return 1;
}

void rt_begin(SDL_Window *win, int res_w, int res_h, int *out_w, int *out_h) {
    int dw, dh;
    SDL_GL_GetDrawableSize(win, &dw, &dh);
    if (dw < 1 || dh < 1) SDL_GetWindowSize(win, &dw, &dh);
    active = 0;
    /* native, or already the window's own size: no scaling to do */
    if (res_w > 0 && res_h > 0 && (res_w != dw || res_h != dh) && load() && ensure(res_w, res_h)) {
        bind_fb(GL_FRAMEBUFFER, fbo);
        active = 1;
        *out_w = res_w; *out_h = res_h;
        return;
    }
    if (loaded > 0) bind_fb(GL_FRAMEBUFFER, 0);
    *out_w = dw; *out_h = dh;
}

void rt_end(SDL_Window *win, int stretch) {
    if (!active) return;
    active = 0;
    int dw, dh;
    SDL_GL_GetDrawableSize(win, &dw, &dh);
    if (dw < 1 || dh < 1) SDL_GetWindowSize(win, &dw, &dh);
    int x = 0, y = 0, w = dw, h = dh;
    if (!stretch) {                     /* keep the resolution's shape, bars around it */
        const double ar = (double)fb_w / fb_h;
        w = dw; h = (int)(dw / ar + 0.5);
        if (h > dh) { h = dh; w = (int)(dh * ar + 0.5); }
        x = (dw - w) / 2; y = (dh - h) / 2;
    }
    bind_fb(GL_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST);        /* the blit and the clear both honour it */
    glViewport(0, 0, dw, dh);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    bind_fb(GL_READ_FRAMEBUFFER, fbo);
    bind_fb(GL_DRAW_FRAMEBUFFER, 0);
    /* exact multiples stay sharp; anything else is filtered, like a scaler */
    const GLenum filt = (w % fb_w == 0 && h % fb_h == 0) ? GL_NEAREST : GL_LINEAR;
    blit_fb(0, 0, fb_w, fb_h, x, y, x + w, y + h, GL_COLOR_BUFFER_BIT, filt);
    bind_fb(GL_FRAMEBUFFER, 0);        /* readbacks and the menu go to the window */
}
