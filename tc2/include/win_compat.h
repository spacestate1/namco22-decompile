/* TC2's own copy of the System 22 project's win_compat.h (+ the 'near' fix): POSIX pieces mapped onto Windows, force-included by
 * CMakeLists.txt in a Windows build only. */
#ifndef WIN_COMPAT_H
#define WIN_COMPAT_H
#ifdef _WIN32

#include <errno.h>
#define WIN32_LEAN_AND_MEAN      /* keep winsock.h out of windows.h so winsock2.h can follow */
#include <windows.h>
#include <winsock2.h>            /* rr_net.c's UDP socket (link ws2_32) */
#include <ws2tcpip.h>            /* getaddrinfo */
#include <stdlib.h>
#include <time.h>
#include <direct.h>
#include <io.h>
#include <sys/stat.h>

/* The engine only uses CLOCK_MONOTONIC. Use QPC rather than requiring
 * MinGW's optional clock_gettime implementation / winpthreads at link time.
 * Query the frequency locally: no shared lazy-initialisation data race. */
#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
#endif
static inline int win_clock_gettime(int clock_id, struct timespec *ts)
{
    LARGE_INTEGER frequency, counter;
    if (clock_id != CLOCK_MONOTONIC) {
        errno = EINVAL;
        return -1;
    }
    if (!QueryPerformanceFrequency(&frequency) ||
        !QueryPerformanceCounter(&counter)) {
        ts->tv_sec = 0;
        ts->tv_nsec = 0;
        errno = EIO;
        return -1;
    }
    ts->tv_sec = (time_t)(counter.QuadPart / frequency.QuadPart);
    ts->tv_nsec = (long)((counter.QuadPart % frequency.QuadPart) *
                         1000000000LL / frequency.QuadPart);
    return 0;
}
#define clock_gettime win_clock_gettime

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

/* the Win32 headers define the obsolete keywords near / far; the lifted code may use them as names */
#ifdef near
#undef near
#endif
#ifdef far
#undef far
#endif

#endif /* _WIN32 */
#endif /* WIN_COMPAT_H */
