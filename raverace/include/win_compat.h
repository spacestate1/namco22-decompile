/* win_compat.h -- the few POSIX pieces Rave Racer uses, mapped onto Windows.
 * Force-included into every source file by CMakeLists.txt when building for
 * Windows (MinGW-w64); the Linux build never sees it. (Prop Cycle has its own,
 * ../include/win_compat.h.) */
#ifndef RR_WIN_COMPAT_H
#define RR_WIN_COMPAT_H
#ifdef _WIN32
#include <stdlib.h>
#include <time.h>
#include <direct.h>
#include <io.h>
#include <sys/stat.h>

/* mkdir(path, mode): Windows has no permission bits */
#define mkdir(path, mode) _mkdir(path)

/* localtime_r / setenv */
static inline struct tm *rr_win_localtime_r(const time_t *t, struct tm *out)
{ return localtime_s(out, t) == 0 ? out : 0; }
#define localtime_r rr_win_localtime_r
static inline int rr_win_setenv(const char *n, const char *v, int overwrite)
{ if (!overwrite && getenv(n)) return 0; return _putenv_s(n, v); }
#define setenv rr_win_setenv

/* sysconf(_SC_NPROCESSORS_ONLN): the renderer's thread count -- SDL knows it */
#ifndef _SC_NPROCESSORS_ONLN
#define _SC_NPROCESSORS_ONLN 84
#endif
extern int SDL_GetCPUCount(void);
static inline long rr_win_sysconf(int name) { (void)name; return SDL_GetCPUCount(); }
#define sysconf rr_win_sysconf

#endif /* _WIN32 */
#endif /* RR_WIN_COMPAT_H */
