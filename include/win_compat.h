/* win_compat.h -- the few POSIX pieces the engine uses, mapped onto Windows.
 *
 * Force-included into every source file by CMakeLists.txt when building for
 * Windows (MinGW-w64), so the Linux build never sees it. Keep it to thin
 * mappings; anything with real behaviour belongs behind #ifdef _WIN32 at the
 * one call site that needs it.
 */
#ifndef WIN_COMPAT_H
#define WIN_COMPAT_H
#ifdef _WIN32

#include <stdlib.h>
#include <time.h>
#include <direct.h>
#include <io.h>
#include <sys/stat.h>

/* mkdir(path, mode): Windows has no permission bits. */
#define mkdir(path, mode) _mkdir(path)

/* localtime_r / setenv */
static inline struct tm *win_localtime_r(const time_t *t, struct tm *out)
{ return localtime_s(out, t) == 0 ? out : 0; }
#define localtime_r win_localtime_r
static inline int win_setenv(const char *n, const char *v, int overwrite)
{ if (!overwrite && getenv(n)) return 0; return _putenv_s(n, v); }
#define setenv win_setenv

/* Windows' GL/gl.h stops at OpenGL 1.1; these are the later enums the
 * renderer uses (all core since GL 1.2/1.3, present in every driver). */
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE   0x812F
#endif
#ifndef GL_COMBINE
#define GL_COMBINE         0x8570
#endif
#ifndef GL_COMBINE_RGB
#define GL_COMBINE_RGB     0x8571
#endif
#ifndef GL_RGB_SCALE
#define GL_RGB_SCALE       0x8573
#endif

#endif /* _WIN32 */
#endif /* WIN_COMPAT_H */
