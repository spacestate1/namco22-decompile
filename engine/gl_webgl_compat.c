/*
 * gl_webgl_compat.c -- the four GL 1.x calls Emscripten's LEGACY_GL_EMULATION
 * does not provide. Browser build only (see timecris/CMakeLists.txt).
 *
 * glPushAttrib/glPopAttrib: the two call sites bracket UI drawing (the Nuklear
 * menu, third_party/nuklear_sdl_gl2.h: ENABLE|COLOR_BUFFER|TRANSFORM bits; the
 * paused-frame redraw, eng_vsync.h: ALL bits). A real attrib stack is not
 * available over WebGL, so save and restore by hand exactly the state those
 * draws change: enables, the blend function and the matrix mode.
 *
 * glPixelMapfv/glPixelTransferi: only post_gl.c's no-shader fallback for the
 * gamma LUT stage; over WebGL the GLSL path is taken, and if that fails the
 * copy simply happens without the LUT (a gamma-only difference), so these are
 * no-ops.
 */
#ifdef __EMSCRIPTEN__
#include <stdio.h>
#include "eng_gl.h"

#define ATTRIB_DEPTH 8
static struct saved {
    GLboolean blend, tex2d, cull, depth, scissor, alpha;
} g_attrib[ATTRIB_DEPTH];
static int g_attrib_n;

/* glGetIntegerv(GL_BLEND_SRC / GL_BLEND_DST / GL_MATRIX_MODE) is NOT
 * implemented by the legacy-GL emulation (INVALID_ENUM) -- restoring those
 * queried values set a GARBAGE matrix mode after every pop, and every
 * transform set up afterwards landed on the wrong matrix stack (huge
 * distorted quads that changed with frame pacing). Restore this engine's
 * canonical state instead: everything here draws with SRC_ALPHA blending
 * and leaves the matrix mode on MODELVIEW. */
void glPushAttrib(GLbitfield mask)
{
    (void)mask;
    if (g_attrib_n >= ATTRIB_DEPTH) { fprintf(stderr, "[GLCOMPAT] attrib stack full\n"); return; }
    struct saved *s = &g_attrib[g_attrib_n++];
    s->blend   = glIsEnabled(GL_BLEND);
    s->tex2d   = glIsEnabled(GL_TEXTURE_2D);
    s->cull    = glIsEnabled(GL_CULL_FACE);
    s->depth   = glIsEnabled(GL_DEPTH_TEST);
    s->scissor = glIsEnabled(GL_SCISSOR_TEST);
    s->alpha   = glIsEnabled(GL_ALPHA_TEST);
}

void glPopAttrib(void)
{
    if (g_attrib_n <= 0) return;
    const struct saved *s = &g_attrib[--g_attrib_n];
    (s->blend   ? glEnable : glDisable)(GL_BLEND);
    (s->tex2d   ? glEnable : glDisable)(GL_TEXTURE_2D);
    (s->cull    ? glEnable : glDisable)(GL_CULL_FACE);
    (s->depth   ? glEnable : glDisable)(GL_DEPTH_TEST);
    (s->scissor ? glEnable : glDisable)(GL_SCISSOR_TEST);
    (s->alpha   ? glEnable : glDisable)(GL_ALPHA_TEST);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glMatrixMode(GL_MODELVIEW);
}

void glPixelMapfv(GLenum map, GLsizei mapsize, const GLfloat *values)
{
    (void)map; (void)mapsize; (void)values;
}

void glPixelTransferi(GLenum pname, GLint param)
{
    (void)pname; (void)param;
    static int warned;
    if (!warned++) fprintf(stderr, "[GLCOMPAT] glPixelTransfer ignored (no gamma LUT on the copy path)\n");
}
#endif
