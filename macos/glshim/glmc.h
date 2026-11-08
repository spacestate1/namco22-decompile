/* glmc.h — the Metal-rendered GL-compatible interface the macOS port uses.
 *
 * The upstream games draw with classic OpenGL 1.x/2.x fixed function plus a
 * small GLSL 1.30 pipeline. On macOS we do not link or load OpenGL at all:
 * every one of these calls is implemented by a Metal backend
 * (glmc_metal.mm / glmc_main.c), with the same names, order and semantics the
 * games rely on (painter's order, texture env combiners, scissor, blends,
 * framebuffers, and the four known GLSL programs hand-ported to Metal Shading
 * Language; see glmc_msl.h). Anything unsupported fails loudly — there are no
 * quietly-stubbed rendering paths.
 */
#ifndef GLMC_H
#define GLMC_H

#include <stddef.h>

typedef unsigned int   GLenum;
typedef unsigned int   GLuint;
typedef int            GLint;
typedef int            GLsizei;
typedef unsigned char  GLboolean;
typedef unsigned char  GLubyte;
typedef unsigned char  GLbyte;
typedef unsigned short GLushort;
typedef short          GLshort;
typedef float          GLfloat;
typedef float          GLclampf;
typedef double         GLdouble;
typedef void           GLvoid;
typedef char           GLchar;
typedef unsigned int   GLbitfield;
typedef long           GLsizeiptr;
#define APIENTRY
#define APIENTRYP *
#define GLAPI extern

#define GL_FALSE 0
#define GL_TRUE  1

#include "glmc_gl1.h"       /* GL 1.1 enums + types (our file, never Apple's) */
#include "glmc_gl3.h"       /* enums used by the engine's FBO/pipeline code  */

/* function declarations ---------------------------------------------------- */
#ifdef __cplusplus
extern "C" {
#endif
void   glmc_init(void);
void   glmc_context_attach_window(void *sdl_window, void *metal_view, void *metal_layer, int w, int h, float scale);

GLenum glmc_GetError(void);
const GLubyte *glmc_glGetString(GLenum name);
void   glmc_GetIntegerv(GLenum pname, GLint *v);
void   glmc_ClearColor(GLclampf r, GLclampf g, GLclampf b, GLclampf a);
void   glmc_ClearDepth(GLdouble d);
void   glmc_Clear(GLbitfield mask);
void   glmc_Enable(GLenum cap);
void   glmc_Disable(GLenum cap);
void   glmc_glIsEnabled(GLenum cap);
void   glmc_glEnablei(GLenum cap, GLuint idx);
void   glmc_glDisablei(GLenum cap, GLuint idx);
GLboolean glmc_IsEnabled(GLenum cap);
void   glmc_Viewport(GLint x, GLint y, GLsizei w, GLsizei h);
void   glmc_Scissor(GLint x, GLint y, GLsizei w, GLsizei h);
void   glmc_DepthFunc(GLenum);
void   glmc_DepthMask(GLboolean);
void   glmc_BlendFunc(GLenum sf, GLenum df);
void   glmc_BlendFuncSeparate(GLenum s0, GLenum d0, GLenum s1, GLenum d1);
void   glmc_BlendEquation(GLenum);
void   glmc_BlendEquationSeparate(GLenum, GLenum);
void   glmc_AlphaFunc(GLenum, GLclampf);
void   glmc_PolygonMode(GLenum, GLenum);
void   glmc_ColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a);
void   glmc_LineWidth(GLfloat);
void   glmc_PixelStorei(GLenum, GLint);
void   glmc_PixelTransferi(GLenum, GLint);
void   glmc_PixelMapfv(GLenum map, GLsizei n, const GLfloat *v);

void   glmc_TexImage2D(GLenum target, GLint level, GLint internal, GLsizei w, GLsizei h, GLint border, GLenum fmt, GLenum type, const void *data);
void   glmc_TexSubImage2D(GLenum target, GLint level, GLint xo, GLint yo, GLsizei w, GLsizei h, GLenum fmt, GLenum type, const void *data);
void   glmc_CopyTexSubImage2D(GLenum target, GLint level, GLint xo, GLint yo, GLint sx, GLint sy, GLsizei w, GLsizei h);
void   glmc_TexParameteri(GLenum, GLenum, GLint);
void   glmc_TexParameterf(GLenum, GLenum, GLfloat);
void   glmc_TexEnvi(GLenum, GLenum, GLint);
void   glmc_TexEnvf(GLenum, GLenum, GLfloat);
void   glmc_TexEnvfv(GLenum, GLenum, const GLfloat *);
void   glmc_ActiveTexture(GLenum);
void   glmc_BindTexture(GLenum, GLuint);
void   glmc_GenTextures(GLsizei, GLuint *);
void   glmc_DeleteTextures(GLsizei, const GLuint *);
void   glmc_GenRenderbuffers(GLsizei, GLuint *);
void   glmc_DeleteRenderbuffers(GLsizei, const GLuint *);
void   glmc_BindRenderbuffer(GLenum, GLuint);
void   glmc_RenderbufferStorage(GLenum, GLenum, GLsizei, GLsizei);
void   glmc_FramebufferRenderbuffer(GLenum, GLenum, GLenum, GLuint);
void   glmc_GenFramebuffers(GLsizei, GLuint *);
void   glmc_DeleteFramebuffers(GLsizei, const GLuint *);
void   glmc_BindFramebuffer(GLenum, GLuint);
void   glmc_FramebufferTexture2D(GLenum, GLenum, GLenum, GLuint, GLint);
GLenum glmc_CheckFramebufferStatus(GLenum);
void   glmc_BlitFramebuffer(GLint sx0, GLint sy0, GLint sx1, GLint sy1, GLint dx0, GLint dy0, GLint dx1, GLint dy1, GLbitfield mask, GLenum filt);
void   glmc_ReadBuffer(GLenum);
void   glmc_DrawBuffer(GLenum);
void   glmc_ReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum fmt, GLenum type, void *data);
void   glmc_CopyTexImage2D(GLenum target, GLint level, GLenum internal_fmt, GLint x, GLint y, GLsizei w, GLsizei h, GLint border);

void   glmc_MatrixMode(GLenum);
void   glmc_LoadIdentity(void);
void   glmc_PushMatrix(void);
void   glmc_PopMatrix(void);
void   glmc_Ortho(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f);
void   glmc_Frustum(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f);
void   glmc_Translatef(GLfloat, GLfloat, GLfloat);
void   glmc_Scalef(GLfloat, GLfloat, GLfloat);
void   glmc_Rotatef(GLfloat, GLfloat, GLfloat, GLfloat);
void   glmc_Translated(GLdouble, GLdouble, GLdouble);
void   glmc_Scaled(GLdouble, GLdouble, GLdouble);
void   glmc_Rotated(GLdouble, GLdouble, GLdouble, GLdouble);
void   glmc_MultMatrixf(const GLfloat *);
void   glmc_MultMatrixd(const GLdouble *);
void   glmc_Color3f(float, float, float);
void   glmc_Color4f(float, float, float, float);
void   glmc_Vertex2f(float, float);
void   glmc_Vertex3f(float, float, float);
void   glmc_Vertex4f(float, float, float, float);
void   glmc_TexCoord2f(GLfloat, GLfloat);
void   glmc_TexCoord3f(GLfloat, GLfloat, GLfloat);
void   glmc_TexCoord4f(GLfloat, GLfloat, GLfloat, GLfloat);
void   glmc_Begin(GLenum);
void   glmc_End(void);
void   glmc_ShadeModel(GLenum);

void   glmc_EnableClientState(GLenum);
void   glmc_DisableClientState(GLenum);
void   glmc_VertexPointer(GLint size, GLenum type, GLsizei stride, const void *ptr);
void   glmc_ColorPointer(GLint size, GLenum type, GLsizei stride, const void *ptr);
void   glmc_TexCoordPointer(GLint size, GLenum type, GLsizei stride, const void *ptr);
void   glmc_DrawArrays(GLenum mode, GLint first, GLsizei count);
void   glmc_DrawElements(GLenum mode, GLsizei count, GLenum type, const void *idx);

GLuint glmc_CreateShaderName(GLenum kind);   /* glCreateShader: returns a name */
void   glmc_ShaderSource(GLuint shader, GLsizei n, const GLchar *const *src, const GLint *len);
void   glmc_CompileShader(GLuint);
void   glmc_GetShaderiv(GLuint, GLenum, GLint *);
void   glmc_GetShaderInfoLog(GLuint, GLsizei, GLsizei *, char *);
GLuint glmc_CreateProgramName(void);         /* glCreateProgram: returns a name */
void   glmc_AttachShader(GLuint, GLuint);
void   glmc_BindAttribLocation(GLuint, GLuint, const char *);
void   glmc_LinkProgram(GLuint);
void   glmc_GetProgramiv(GLuint, GLenum, GLint *);
void   glmc_GetProgramInfoLog(GLuint, GLsizei, GLsizei *, char *);
void   glmc_UseProgram(GLuint);
GLint  glmc_GetUniformLocation(GLuint, const char *);
GLint  glmc_GetAttribLocation(GLuint, const char *);
void   glmc_Uniform1i(GLint, GLint);
void   glmc_Uniform3i(GLint, GLint, GLint, GLint);
void   glmc_Uniform4i(GLint, GLint, GLint, GLint, GLint);
void   glmc_Uniform1f(GLint, GLfloat);
void   glmc_GenBuffers(GLsizei, GLuint *);
void   glmc_DeleteBuffers(GLsizei, const GLuint *);
void   glmc_BindBuffer(GLenum, GLuint);
void   glmc_BufferData(GLenum, GLsizeiptr, const void *, GLenum);
void   glmc_EnableVertexAttribArray(GLuint);
void   glmc_DisableVertexAttribArray(GLuint);
void   glmc_VertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean norm, GLsizei stride, const void *ptr);
void   glmc_PushAttrib(GLbitfield);
void   glmc_PopAttrib(void);
void   glmc_Finish(void);
void   glmc_Flush(void);
GLboolean glmc_IsTexture(GLuint);

/* SDL GL shim (Metal equivalence; MSL is the GLSL of the game) ------------ */
int           glmc_sdl_SetAttribute(GLenum a, int v);
void         *glmc_sdl_CreateContext(void *win);
int           glmc_sdl_MakeCurrent(void *win, void *ctx);
void         *glmc_sdl_GetProcAddress(const char *name);
void          glmc_sdl_SwapWindow(void *win);
void          glmc_sdl_UnloadLibrary(void);
int           glmc_sdl_LoadLibrary(const char *path);
int           glmc_sdl_SetSwapInterval(int i);
int           glmc_sdl_GetSwapInterval(void);
void          glmc_sdl_GetDrawableSize(void *win, int *w, int *h);
void          glmc_sdl_DeleteContext(void *ctx);
#ifdef __cplusplus
}
#endif

#endif /* GLMC_H */
