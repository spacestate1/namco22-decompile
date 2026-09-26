/*
 * gl_dyn.c -- Windows only: OpenGL through pointers resolved at run time.
 * The shared engine's (engine/): both games' Windows builds use it.
 *
 * The Windows build does not link opengl32. Every OpenGL 1.1 call in the
 * engine compiles (MinGW-w64's GL/gl.h, dllimport) to an indirect call through
 * a pointer named __imp_glXxx -- normally filled by the loader from
 * opengl32.dll's import table. This file DEFINES those pointers and fills them
 * from whichever OpenGL library SDL has loaded (SDL_GL_GetProcAddress), so
 * main.c can switch to the bundled Mesa (mesa/opengl32.dll, software llvmpipe)
 * when Windows offers no real driver -- a virtual machine, Remote Desktop, no
 * GPU driver installed -- without any change to the renderer.
 *
 * A new OpenGL function used anywhere shows up as an undefined __imp_glXxx at
 * link time: add it to the list below. Functions past 1.1 are loaded by their
 * own code (render_target.c) and need nothing here.
 */
#ifdef _WIN32
#include <stdbool.h>
#include <stdio.h>
#include <SDL.h>
#include <windows.h>
#include <string.h>
#include <stdlib.h>
#include <wchar.h>

#define GLF(n) void *__imp_##n;
#define GL_FUNCS \
    GLF(glAlphaFunc) \
    GLF(glBegin) \
    GLF(glBindTexture) \
    GLF(glBlendFunc) \
    GLF(glClear) \
    GLF(glClearColor) \
    GLF(glColor3f) \
    GLF(glColor4f) \
    GLF(glColorMask) \
    GLF(glColorPointer) \
    GLF(glCopyTexSubImage2D) \
    GLF(glDeleteTextures) \
    GLF(glDepthFunc) \
    GLF(glDisable) \
    GLF(glDisableClientState) \
    GLF(glDrawArrays) \
    GLF(glDrawElements) \
    GLF(glEnable) \
    GLF(glEnableClientState) \
    GLF(glEnd) \
    GLF(glFinish) \
    GLF(glFrustum) \
    GLF(glGenTextures) \
    GLF(glGetError) \
    GLF(glGetIntegerv) \
    GLF(glGetString) \
    GLF(glLineWidth) \
    GLF(glLoadIdentity) \
    GLF(glMatrixMode) \
    GLF(glOrtho) \
    GLF(glPixelMapfv) \
    GLF(glPixelStorei) \
    GLF(glPixelTransferi) \
    GLF(glPopAttrib) \
    GLF(glPopMatrix) \
    GLF(glPushAttrib) \
    GLF(glPushMatrix) \
    GLF(glReadBuffer) \
    GLF(glReadPixels) \
    GLF(glRotatef) \
    GLF(glScalef) \
    GLF(glScissor) \
    GLF(glTexCoord2f) \
    GLF(glTexCoordPointer) \
    GLF(glTexEnvf) \
    GLF(glTexEnvi) \
    GLF(glTexImage2D) \
    GLF(glTexParameteri) \
    GLF(glTexSubImage2D) \
    GLF(glTranslatef) \
    GLF(glVertex2f) \
    GLF(glVertex3f) \
    GLF(glVertexPointer) \
    GLF(glViewport)
GL_FUNCS
#undef GLF

/* Fill every pointer from the current context's library -- module: the full
 * path of the OpenGL DLL in use (NULL: the system's opengl32.dll); false (and
 * the first missing name in *missing) if any is absent. */
/* OpenGL 1.1 entry points are EXPORTS of opengl32.dll (the system's, or Mesa's
 * of the same name once main.c has switched to it); wglGetProcAddress -- what
 * SDL tries first -- returns NULL or junk (1, 2, 3, -1) for them on some
 * drivers, so the module comes first. */
static void *gl_proc(const char *module, const char *name)
{
    HMODULE m = GetModuleHandleA(module ? module : "opengl32.dll");   /* a full path picks that one */
    void *p = m ? (void *)GetProcAddress(m, name) : NULL;
    return p ? p : SDL_GL_GetProcAddress(name);
}

bool gl_dyn_resolve(const char *module, const char **missing)
{
#define GLF(n) if (!(__imp_##n = gl_proc(module, #n))) { if (missing) *missing = #n; return false; }
    GL_FUNCS
#undef GLF
    return true;
}

/* A GL context for *win with the system's OpenGL resolved -- or, when there is
 * no usable one (no context, or only "GDI Generic", the GL 1.1 stub of a VM,
 * Remote Desktop or a PC without its GPU driver), the window re-created on the
 * bundled Mesa (mesa\opengl32.dll beside the .exe, llvmpipe). The window is
 * re-made with the same title, size and flags. NULL if neither works;
 * *missing names a GL function the library lacked. NAMCO22_FORCE_MESA=1 forces
 * Mesa. (Prop Cycle's main.c carries the same logic inline.) */
SDL_GLContext eng_gl_create_win(SDL_Window **win, const char **missing)
{
    SDL_GLContext ctx = SDL_GL_CreateContext(*win);
    bool usable = ctx && gl_dyn_resolve(NULL, missing);
    if (usable) {
        typedef const unsigned char *(APIENTRY *gs_t)(unsigned int);
        gs_t gs = (gs_t)__imp_glGetString;
        const char *ren = (const char *)gs(0x1F01);      /* GL_RENDERER */
        usable = ren && !strstr(ren, "GDI Generic") && !getenv("NAMCO22_FORCE_MESA");
    }
    if (usable) return ctx;
    const char *dll = "mesa\\opengl32.dll";
    FILE *t = fopen(dll, "rb");
    if (!t) return ctx;                                  /* nothing better: keep what there is */
    fclose(t);
    char title[256]; int w, h, x, y; Uint32 flags = SDL_GetWindowFlags(*win);
    snprintf(title, sizeof title, "%s", SDL_GetWindowTitle(*win));
    SDL_GetWindowSize(*win, &w, &h); SDL_GetWindowPosition(*win, &x, &y);
    fprintf(stderr, "[GL] no usable OpenGL driver: switching to the bundled Mesa (%s)\n", dll);
    if (ctx) SDL_GL_DeleteContext(ctx);
    SDL_DestroyWindow(*win); *win = NULL;
    SDL_GL_UnloadLibrary();
    wchar_t wdir[32768];
    DWORD wn = GetFullPathNameW(L"mesa", 32768, wdir, NULL);
    if (wn > 0 && wn < 32768) SetDllDirectoryW(wdir);   /* its libgallium_wgl.dll sits beside it */
    if (SDL_GL_LoadLibrary(dll) != 0) { fprintf(stderr, "[GL] cannot load %s: %s\n", dll, SDL_GetError()); return NULL; }
    *win = SDL_CreateWindow(title, x, y, w, h, flags & (SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE |
                            SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_FULLSCREEN_DESKTOP | SDL_WINDOW_HIDDEN));
    if (!*win) return NULL;
    ctx = SDL_GL_CreateContext(*win);
    if (ctx && !gl_dyn_resolve(dll, missing)) { SDL_GL_DeleteContext(ctx); ctx = NULL; }
    return ctx;
}
#endif
