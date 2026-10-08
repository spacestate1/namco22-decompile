/*
 * s21_glutil.c -- what engine/ss22_host.c needs from engine/ss22_gl.c, for a board whose picture is not a Super 22 frame:
 * ss22_draw() draws the System 21 picture (src/video: the last S21_OUT_SW frame, 496x480 RGB) over the render viewport, and the
 * three GL context helpers. HARD RULE 3 item: the helpers are a COPY of ss22_gl.c's (eng_gl_context_attributes,
 * eng_gl_open_headless, eng_gl_write_ppm) -- work/proposed_patches/a_eng_gl_util.patch moves them into engine/eng_gl_util.c so
 * both link the one copy.
 *
 * Two pictures: THE GL PICTURE (default; src/video/s21_video_gl.c: the polygons through the shared engine rasteriser at the
 * render size, the board's mixing in one GLSL pass, the 2D sharp-bilinear) and THE SOFTWARE PICTURE (CS_PICTURE=sw, or when GL
 * cannot: the oracle's exact 496x480 frame as one texture, scaled sharp-bilinear as below). Either fills the 4:3 picture.
 * WIDESCREEN (Display > Widescreen, a render size wider than 4:3; PLAN.md "Module H") is Hor+ with the GL picture: the same
 * projection (centre, focal), the 3D drawn over the whole width, the 2D kept in the 4:3 centre (s21_video_gl.c), and the DSP
 * board told to keep the vertex groups that now fall in the side strips (s21_dsp_set_view_extra: the slave's reject window grows
 * by the strip width). The software picture (the oracle) stays 496 wide and is pillarboxed.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <SDL2/SDL.h>
#include "eng_gl.h"
#include "s21_host.h"
#include "s21_video.h"

#include "eng_display.h"
void s21_dsp_set_view_extra(int px);             /* src/dsp/s21_dsp.h */

int g_eng_hud_edges_on = 1;      /* engine/hud_edges.c's Display-menu switch (eng_display.c sets it); System 21 has no HUD to move */

/* The board's 496x480 picture into the render target (vw x vh), SHARP BILINEAR: first a nearest-neighbour upscale by whole
 * numbers (kx = ceil(vw/496), ky = ceil(vh/480)) on the CPU, then one linear step down to the viewport. Every source pixel comes
 * out the same width, unlike a plain nearest stretch to a non-multiple (496 -> 640 doubled every third or fourth column and the
 * 2D text drew with uneven strokes), and without the blur of a plain linear stretch. CS_SCALE=nearest|linear for A/B. */
static GLuint tex;
static int tex_w, tex_h;
static uint8_t *up;
static void draw_sw(int px, int py, int vw, int vh);

/* Hor+: the board pixels each side of the 496 that a picture fw x fh shows (s21_video_gl.c: the 4:3 part is h * 4/3 wide and
 * spans the board's 496), 0 when it is not widescreen -- Widescreen off, a 4:3 (or narrower) picture, or the software picture. */
int s21_wide_extra(int fw, int fh)
{
    if (!g_eng_disp.wide || !s21_host_use_gl() || fw <= 0 || fh <= 0 || fw * 3 <= fh * 4 + 3) return 0;
    const double bxw = S21_PIC_W * (double)fw / (fh * 4.0 / 3.0);
    const int e = (int)((bxw - S21_PIC_W) / 2.0 + 0.999);
    return e > 0 ? e : 0;
}
/* before each emulated frame: the DSP board's view for the picture it will be drawn into */
void s21_view_for(int fw, int fh)
{
    const int e = s21_wide_extra(fw, fh);
    static int dsp_on = -1; if (dsp_on < 0) { const char *v = getenv("CS_WIDE_DSP"); dsp_on = !(v && *v == '0'); }   /* A/B: 0 = the DSPs' 4:3 view */
    static int bd_on = -1; if (bd_on < 0) { const char *v = getenv("CS_WIDE_BACKDROP"); bd_on = !(v && *v == '0'); }  /* A/B: 0 = no backdrop in the strips */
    /* the DSP's reject edge goes OUT PAST the picture's edge (48 board px), so an object turning into view is already drawn when it
     * reaches the edge instead of popping in on the picture; Widescreen off keeps the arcade's own window (CS_WIDE_MARGIN=<px>) */
    static int margin = -1; if (margin < 0) { const char *v = getenv("CS_WIDE_MARGIN"); margin = v ? atoi(v) : 48; }
    s21_dsp_set_view_extra(dsp_on && e > 0 ? e + margin : 0);
    s21_video_set_wide_extra(bd_on ? e : 0);
}

void ss22_draw(int fw, int fh)
{
    glViewport(0, 0, fw, fh);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT);
    int px = 0, py = 0, pw = fw, ph = fh;
    const int wide = s21_wide_extra(fw, fh) > 0;
    s21_video_gl_wide(wide);
    if (fw * 3 > fh * 4 + 3 && !wide) { pw = (fh * 4 + 1) / 3; px = (fw - pw) / 2; }     /* wider than 4:3 and not Hor+: pillarbox */
    if (s21_host_use_gl()) {
        if (s21_video_draw_gl(px, py, pw, ph)) {
            glViewport(0, 0, fw, fh);
            glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0, 1, 1, 0, -1, 1);
            glMatrixMode(GL_MODELVIEW); glLoadIdentity();
            glDisable(GL_DEPTH_TEST); glDisable(GL_ALPHA_TEST); glDisable(GL_BLEND);
            return;
        }
        s21_host_gl_unavailable();
    }
    draw_sw(px, py, pw, ph);
    glViewport(0, 0, fw, fh);
}
static void draw_sw(int px, int py, int vw, int vh)
{
    const uint8_t *rgb = s21_host_picture();
    glViewport(px, py, vw, vh);
    glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0, 1, 1, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    glDisable(GL_DEPTH_TEST); glDisable(GL_BLEND); glDisable(GL_ALPHA_TEST); glDisable(GL_SCISSOR_TEST); glDisable(GL_CULL_FACE);
    if (!rgb) return;
    static int mode = -1;                  /* 0 sharp bilinear, 1 nearest, 2 linear */
    if (mode < 0) { const char *e = getenv("CS_SCALE"); mode = !e ? 0 : !strcmp(e, "nearest") ? 1 : !strcmp(e, "linear") ? 2 : 0; }
    int kx = 1, ky = 1;
    if (mode == 0) {
        kx = (vw + S21_PIC_W - 1) / S21_PIC_W; ky = (vh + S21_PIC_H - 1) / S21_PIC_H;
        if (kx < 1) kx = 1; if (ky < 1) ky = 1; if (kx > 8) kx = 8; if (ky > 8) ky = 8;
        if (kx * S21_PIC_W == vw && ky * S21_PIC_H == vh) { /* an exact multiple: nearest is already exact */ }
    }
    const int tw = S21_PIC_W * kx, th = S21_PIC_H * ky;
    if (!tex) glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    const GLint filt = (mode == 1 || (kx * S21_PIC_W == vw && ky * S21_PIC_H == vh && mode == 0)) ? GL_NEAREST : GL_LINEAR;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filt); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filt);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    if (tw != tex_w || th != tex_h) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, tw, th, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL);
        tex_w = tw; tex_h = th;
        free(up); up = (kx > 1 || ky > 1) ? malloc((size_t)tw * th * 3) : NULL;
    }
    const uint8_t *src = rgb;
    if (up) {                              /* the whole-number nearest upscale */
        for (int y = 0; y < S21_PIC_H; y++) {
            uint8_t *row = up + (size_t)y * ky * tw * 3;
            const uint8_t *s = rgb + (size_t)y * S21_PIC_W * 3;
            uint8_t *d = row;
            for (int x = 0; x < S21_PIC_W; x++, s += 3)
                for (int i = 0; i < kx; i++, d += 3) { d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; }
            for (int r = 1; r < ky; r++) memcpy(row + (size_t)r * tw * 3, row, (size_t)tw * 3);
        }
        src = up;
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, tw, th, GL_RGB, GL_UNSIGNED_BYTE, src);
    glEnable(GL_TEXTURE_2D); glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE); glColor3f(1, 1, 1);
    glBegin(GL_QUADS);                     /* the board's 496x480 fills the 4:3 picture, as on the cabinet's monitor */
    glTexCoord2f(0, 0); glVertex2f(0, 0); glTexCoord2f(1, 0); glVertex2f(1, 0);
    glTexCoord2f(1, 1); glVertex2f(1, 1); glTexCoord2f(0, 1); glVertex2f(0, 1);
    glEnd();
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);   /* the menu (Nuklear's GL2 backend) modulates its font texture by the vertex colour */
    glDisable(GL_TEXTURE_2D);
}

/* ---- COPY of engine/ss22_gl.c's helpers (see above) ---- */
void eng_gl_context_attributes(void)
{
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);    /* the GL picture's depth test holds the board's 16-bit z (s21_video_gl.c) */
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
}
static SDL_Window *hl_win;
static SDL_GLContext hl_ctx;
bool eng_gl_open_headless(int w, int h)
{
#ifndef _WIN32
    SDL_SetHint("SDL_VIDEODRIVER", "offscreen");
    setenv("SDL_VIDEODRIVER", "offscreen", 1);
#endif
    if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
        SDL_SetHint("SDL_VIDEODRIVER", ""); setenv("SDL_VIDEODRIVER", "", 1);
        if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) { fprintf(stderr, "[GL] no video: %s\n", SDL_GetError()); return false; }
    }
    eng_gl_context_attributes();
    hl_win = SDL_CreateWindow("engine", 0, 0, w, h, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (!hl_win) { fprintf(stderr, "[GL] no window: %s\n", SDL_GetError()); return false; }
    hl_ctx = SDL_GL_CreateContext(hl_win);
    if (!hl_ctx) { fprintf(stderr, "[GL] no context: %s\n", SDL_GetError()); return false; }
    fprintf(stderr, "[GL] headless: %s\n", (const char *)glGetString(GL_RENDERER));
    return true;
}
bool eng_gl_write_ppm(const char *path, int vw, int vh)
{
    uint8_t *px = malloc((size_t)vw * vh * 3);
    if (!px) return false;
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, vw, vh, GL_RGB, GL_UNSIGNED_BYTE, px);
    FILE *f = fopen(path, "wb");
    if (!f) { free(px); return false; }
    fprintf(f, "P6\n%d %d\n255\n", vw, vh);
    for (int y = vh - 1; y >= 0; y--) fwrite(px + (size_t)y * vw * 3, 1, (size_t)vw * 3, f);
    fclose(f); free(px);
    return true;
}
