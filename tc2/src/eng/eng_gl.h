/* TIME CRISIS 2'S OWN COPY of namco-2x-systems engine/eng_gl.h (copied 2026-10-05). TC2 is independent of the System 22 project: change it here. */
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
/* GL 1.3 multitexture + combiners (the single-pass fog in quad_gl.c) */
#ifndef GL_VERSION_1_3
#define GL_TEXTURE0        0x84C0
#define GL_TEXTURE1        0x84C1
#define GL_COMBINE_ALPHA   0x8572
#define GL_INTERPOLATE     0x8575
#define GL_CONSTANT        0x8576
#define GL_PRIMARY_COLOR   0x8577
#define GL_PREVIOUS        0x8578
#define GL_SOURCE0_RGB     0x8580
#define GL_SOURCE1_RGB     0x8581
#define GL_SOURCE2_RGB     0x8582
#define GL_SOURCE0_ALPHA   0x8588
#define GL_SOURCE1_ALPHA   0x8589
#define GL_SOURCE2_ALPHA   0x858A
#define GL_OPERAND0_RGB    0x8590
#define GL_OPERAND1_RGB    0x8591
#define GL_OPERAND2_RGB    0x8592
#define GL_OPERAND0_ALPHA  0x8598
#define GL_OPERAND1_ALPHA  0x8599
#define GL_OPERAND2_ALPHA  0x859A
#endif
#endif
