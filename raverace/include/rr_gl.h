/*
 * rr_gl.h -- Rave Racer's picture through the shared engine's OpenGL
 * pipeline (src/rr_gl.c). The software renderer (rr_video.c) is the oracle.
 */
#ifndef RR_GL_H
#define RR_GL_H
#include <stdbool.h>

bool rr_gl_init(const char *rom_dir);          /* texture ROMs, gamma PROMs, point data */
void rr_gl_context_attributes(void);           /* SDL_GL attributes, before creating the context */
bool rr_gl_open_headless(int w, int h);        /* an offscreen w x h context for --gl runs */
void rr_gl_prepare(bool slave_active);         /* once per screen update: walk + sort */
void rr_gl_draw(int vw, int vh);               /* draw the prepared frame, vw x vh pixels */
bool rr_gl_write_ppm(const char *path, int vw, int vh);
int  rr_gl_quads(void);                        /* quads in the prepared frame */
#endif
