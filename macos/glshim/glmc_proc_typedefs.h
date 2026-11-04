/* glmc_proc_typedefs.h — the PFN function-pointer types tc2's GPU renderer
 * (and render_target/post_gl on Windows) look up with SDL_GL_GetProcAddress,
 * matching the OS's naming; each returns the corresponding Metal-backed
 * implementation. */
#ifndef GLMC_PROC_TYPEDEFS_H
#define GLMC_PROC_TYPEDEFS_H
typedef unsigned int (*PFNGLCREATESHADERPROC)(unsigned int);
typedef void (*PFNGLSHADERSOURCEPROC)(unsigned int, int, const char *const *, const int *);
typedef void (*PFNGLCOMPILESHADERPROC)(unsigned int);
typedef void (*PFNGLGETSHADERIVPROC)(unsigned int, unsigned int, int *);
typedef void (*PFNGLGETSHADERINFOLOGPROC)(unsigned int, int, int *, char *);
typedef unsigned int (*PFNGLCREATEPROGRAMPROC)(void);
typedef void (*PFNGLATTACHSHADERPROC)(unsigned int, unsigned int);
typedef void (*PFNGLBINDATTRIBLOCATIONPROC)(unsigned int, unsigned int, const char *);
typedef void (*PFNGLLINKPROGRAMPROC)(unsigned int);
typedef void (*PFNGLGETPROGRAMIVPROC)(unsigned int, unsigned int, int *);
typedef void (*PFNGLGETPROGRAMINFOLOGPROC)(unsigned int, int, int *, char *);
typedef void (*PFNGLUSEPROGRAMPROC)(unsigned int);
typedef int (*PFNGLGETUNIFORMLOCATIONPROC)(unsigned int, const char *);
typedef void (*PFNGLUNIFORM1IPROC)(int, int);
typedef void (*PFNGLUNIFORM3IPROC)(int, int, int, int);
typedef void (*PFNGLUNIFORM4IPROC)(int, int, int, int, int);
typedef void (*PFNGLUNIFORM1FPROC)(int, float);
typedef void (*PFNGLGENBUFFERSPROC)(int, unsigned int *);
typedef void (*PFNGLBINDBUFFERPROC)(unsigned int, unsigned int);
typedef void (*PFNGLBUFFERDATAPROC)(unsigned int, long, const void *, unsigned int);
typedef void (*PFNGLVERTEXATTRIBPOINTERPROC)(unsigned int, int, unsigned int, unsigned char, int, const void *);
typedef void (*PFNGLENABLEVERTEXATTRIBARRAYPROC)(unsigned int);
typedef void (*PFNGLDISABLEVERTEXATTRIBARRAYPROC)(unsigned int);
typedef void (*PFNGLGENFRAMEBUFFERSPROC)(int, unsigned int *);
typedef void (*PFNGLBINDFRAMEBUFFERPROC)(unsigned int, unsigned int);
typedef void (*PFNGLFRAMEBUFFERTEXTURE2DPROC)(unsigned int, unsigned int, unsigned int, unsigned int, int);
typedef unsigned int (*PFNGLCHECKFRAMEBUFFERSTATUSPROC)(unsigned int);
typedef void (*PFNGLACTIVETEXTUREPROC)(unsigned int);
typedef void (*PFNGLBLENDEQUATIONSEPARATEPROC)(unsigned int, unsigned int);
typedef void (*PFNGLBLENDFUNCSEPARATEPROC)(unsigned int, unsigned int, unsigned int, unsigned int);
typedef void (*PFNGLBLENDEQUATIONPROC)(unsigned int);
#endif
