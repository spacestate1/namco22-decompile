/* eng_gl.h -- <GL/gl.h> plus the few post-1.1 constants the engine uses.
 * MinGW-w64's gl.h (the Windows build) stops at OpenGL 1.1; the functions
 * themselves are resolved at run time (gl_dyn.c, render_target.c, post_gl.c). */
#ifndef ENG_GL_H
#define ENG_GL_H
#ifdef _WIN32
#include <windows.h>
#endif
#include <GL/gl.h>
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif
#ifndef GL_BGRA
#define GL_BGRA 0x80E1
#endif
#ifndef GL_COMBINE
#define GL_COMBINE     0x8570
#define GL_COMBINE_RGB 0x8571
#define GL_RGB_SCALE   0x8573
#endif
#endif
