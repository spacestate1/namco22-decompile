/* GL/gl.h — macOS Metal shim: provides the GL names the engine includes
 * (engine/eng_gl.h and tc2/src/eng/eng_gl.h only include <GL/gl.h>), backed by
 * Metal. No OpenGL framework is loaded or linked. */
#ifndef GLMC_GL_H_WRAPPER
#define GLMC_GL_H_WRAPPER

#include "../glmc.h"

/* the windows get a CAMetalLayer view (SDL_Metal_CreateView), not an OpenGL
 * surface: an SDL_WINDOW_OPENGL window makes SDL load OpenGL.framework. Every
 * call site includes SDL.h first; the guard keeps SDL's own enum intact. */
#ifdef SDL_video_h_
#define SDL_WINDOW_OPENGL SDL_WINDOW_METAL
#endif

#ifndef GLMC_PRE_INCLUDED
#define GLMC_PRE_INCLUDED
/* the call-site remappings in case glmc_pre.h was not pre-included */
#define glEnable(a)            glmc_Enable(a)
#define glDisable(a)           glmc_Disable(a)
#define glIsEnabled(a)         glmc_IsEnabled(a)
#define glEnablei(a,b)         glmc_Enablei(a,b)
#define glDisablei(a,b)        glmc_Disablei(a,b)
#define glGetString(a)         glmc_glGetString(a)
#define glGetIntegerv(a,b)     glmc_GetIntegerv(a,b)
#define glGetError()           glmc_GetError()
#define glGetStringi(a,b)      glmc_glGetStringi(a,b)
#define glClear(a)             glmc_Clear(a)
#define glClearColor(a,b,c,d)  glmc_ClearColor(a,b,c,d)
#define glClearDepth(a)        glmc_ClearDepth(a)
#define glViewport(a,b,c,d)    glmc_Viewport(a,b,c,d)
#define glScissor(a,b,c,d)     glmc_Scissor(a,b,c,d)
#define glDepthFunc(a)         glmc_DepthFunc(a)
#define glDepthMask(a)         glmc_DepthMask(a)
#define glBlendFunc(a,b)       glmc_BlendFunc(a,b)
#define glBlendFuncSeparate(a,b,c,d) glmc_BlendFuncSeparate(a,b,c,d)
#define glBlendEquation(a)     glmc_BlendEquation(a)
#define glBlendEquationSeparate(a,b) glmc_BlendEquationSeparate(a,b)
#define glAlphaFunc(a,b)       glmc_AlphaFunc(a,b)
#define glColorMask(a,b,c,d)   glmc_ColorMask(a,b,c,d)
#define glLineWidth(a)         glmc_LineWidth(a)
#define glPixelStorei(a,b)     glmc_PixelStorei(a,b)
#define glPixelTransferi(a,b)  glmc_PixelTransferi(a,b)
#define glPixelMapfv(a,b,c)    glmc_PixelMapfv(a,b,c)
#define glTexImage2D           glmc_TexImage2D
#define glTexSubImage2D        glmc_TexSubImage2D
#define glCopyTexImage2D       glmc_CopyTexImage2D
#define glCopyTexSubImage2D    glmc_CopyTexSubImage2D
#define glTexParameteri        glmc_TexParameteri
#define glTexParameterf        glmc_TexParameterf
#define glTexEnvi              glmc_TexEnvi
#define glTexEnvf              glmc_TexEnvf
#define glTexEnvfv             glmc_TexEnvfv
#define glActiveTexture        glmc_ActiveTexture
#define glBindTexture          glmc_BindTexture
#define glGenTextures          glmc_GenTextures
#define glDeleteTextures       glmc_DeleteTextures
#define glIsTexture            glmc_IsTexture
#define glGenRenderbuffers     glmc_GenRenderbuffers
#define glGenRenderbuffersEXT  glmc_GenRenderbuffers
#define glDeleteRenderbuffers  glmc_DeleteRenderbuffers
#define glDeleteRenderbuffersEXT glmc_DeleteRenderbuffers
#define glBindRenderbuffer     glmc_BindRenderbuffer
#define glBindRenderbufferEXT  glmc_BindRenderbuffer
#define glRenderbufferStorage  glmc_RenderbufferStorage
#define glRenderbufferStorageEXT glmc_RenderbufferStorage
#define glFramebufferRenderbuffer glmc_FramebufferRenderbuffer
#define glFramebufferRenderbufferEXT glmc_FramebufferRenderbuffer
#define glGenFramebuffers      glmc_GenFramebuffers
#define glGenFramebuffersEXT   glmc_GenFramebuffers
#define glDeleteFramebuffers   glmc_DeleteFramebuffers
#define glDeleteFramebuffersEXT glmc_DeleteFramebuffers
#define glBindFramebuffer      glmc_BindFramebuffer
#define glBindFramebufferEXT   glmc_BindFramebuffer
#define glFramebufferTexture2D glmc_FramebufferTexture2D
#define glFramebufferTexture2DEXT glmc_FramebufferTexture2D
#define glCheckFramebufferStatus glmc_CheckFramebufferStatus
#define glCheckFramebufferStatusEXT glmc_CheckFramebufferStatus
#define glBlitFramebuffer      glmc_BlitFramebuffer
#define glBlitFramebufferEXT   glmc_BlitFramebuffer
#define glReadBuffer           glmc_ReadBuffer
#define glDrawBuffer           glmc_DrawBuffer
#define glReadPixels           glmc_ReadPixels
#define glMatrixMode           glmc_MatrixMode
#define glLoadIdentity         glmc_LoadIdentity
#define glPushMatrix           glmc_PushMatrix
#define glPopMatrix            glmc_PopMatrix
#define glOrtho                glmc_Ortho
#define glFrustum              glmc_Frustum
#define glTranslatef           glmc_Translatef
#define glScaled               glmc_Scaled
#define glTranslatef           glmc_Translatef
#define glScalef               glmc_Scalef
#define glRotatef              glmc_Rotatef
#define glTranslated           glmc_Translated
#define glRotated              glmc_Rotated
#define glMultMatrixf          glmc_MultMatrixf
#define glMultMatrixd          glmc_MultMatrixd
#define glColor3f              glmc_Color3f
#define glColor4f              glmc_Color4f
#define glColor3f              glmc_Color3f
#define glColor4f              glmc_Color4f
#define glVertex2f             glmc_Vertex2f
#define glVertex3f             glmc_Vertex3f
#define glVertex4f             glmc_Vertex4f
#define glTexCoord2f           glmc_TexCoord2f
#define glTexCoord3f           glmc_TexCoord3f
#define glTexCoord4f           glmc_TexCoord4f
#define glBegin                glmc_Begin
#define glEnd()                glmc_End()
#define glShadeModel           glmc_ShadeModel
#define glEnableClientState    glmc_EnableClientState
#define glDisableClientState   glmc_DisableClientState
#define glVertexPointer        glmc_VertexPointer
#define glColorPointer         glmc_ColorPointer
#define glTexCoordPointer      glmc_TexCoordPointer
#define glDrawArrays           glmc_DrawArrays
#define glDrawElements         glmc_DrawElements
#define glCreateShader         glmc_CreateShaderName
#define glShaderSource         glmc_ShaderSource
#define glCompileShader        glmc_CompileShader
#define glGetShaderiv          glmc_GetShaderiv
#define glGetShaderInfoLog     glmc_GetShaderInfoLog
#define glCreateProgram        glmc_CreateProgramName
#define glAttachShader         glmc_AttachShader
#define glBindAttribLocation   glmc_BindAttribLocation
#define glLinkProgram          glmc_LinkProgram
#define glGetProgramiv         glmc_GetProgramiv
#define glGetProgramInfoLog    glmc_GetProgramInfoLog
#define glUseProgram           glmc_UseProgram
#define glGetUniformLocation   glmc_GetUniformLocation
#define glGetAttribLocation    glmc_GetAttribLocation
#define glUniform1i            glmc_Uniform1i
#define glUniform3i            glmc_Uniform3i
#define glUniform4i            glmc_Uniform4i
#define glUniform1f            glmc_Uniform1f
#define glGenBuffers           glmc_GenBuffers
#define glDeleteBuffers        glmc_DeleteBuffers
#define glBindBuffer           glmc_BindBuffer
#define glBufferData           glmc_BufferData
#define glEnableVertexAttribArray glmc_EnableVertexAttribArray
#define glDisableVertexAttribArray glmc_DisableVertexAttribArray
#define glVertexAttribPointer  glmc_VertexAttribPointer
#define glPushAttrib           glmc_PushAttrib
#define glPopAttrib()          glmc_PopAttrib()
#define glFinish()             glmc_Finish()
#define glFlush()              glmc_Flush()

/* SDL GL entry points, replaced with the Metal equivalents */
void *glmc_sdl_gl_from_name(const char *name);
#define SDL_GL_GetProcAddress(n)        glmc_sdl_GetProcAddress(n)
#define SDL_GL_CreateContext(w)         glmc_sdl_CreateContext((void *)(w))
#define SDL_GL_MakeCurrent(w,c)         glmc_sdl_MakeCurrent((void *)(w),(void *)(c))
#define SDL_GL_SwapWindow(w)            glmc_sdl_SwapWindow((void *)(w))
#define SDL_GL_UnloadLibrary()          glmc_sdl_UnloadLibrary()
#define SDL_GL_LoadLibrary(p)           glmc_sdl_LoadLibrary(p)
#define SDL_GL_SetSwapInterval(i)       glmc_sdl_SetSwapInterval(i)
#define SDL_GL_GetSwapInterval()        glmc_sdl_GetSwapInterval()
#define SDL_GL_GetDrawableSize(w,a,b)   glmc_sdl_GetDrawableSize((void *)(w),(a),(b))
#define SDL_GL_DeleteContext(c)         glmc_sdl_DeleteContext((void *)(c))
#define SDL_GL_SetAttribute(a,b)        glmc_sdl_SetAttribute(a,b)
#endif /* GLMC_PRE_INCLUDED */

#endif /* GLMC_GL_H_WRAPPER */
