/*
 * rr_win.c -- Windows start-up (MinGW-w64 build only).
 *
 *  - work from the program's own folder, found with the WIDE API: double-clicked
 *    or started from a shortcut, extracted/, roms/ and rr_controls.cfg are there;
 *    the ANSI calls cannot name a folder outside the system code page
 *    (C:\Users\Zoë\...), so everything after this uses relative paths;
 *  - a windowed program has no console: stdout and stderr go to raveracer.log
 *    beside the .exe -- the first thing to read when it does not start;
 *  - report real pixel sizes, or Windows stretches (blurs) the window by the
 *    display scaling.
 */
#ifdef _WIN32
#include <stdio.h>
#include <io.h>
#include <direct.h>
#include <wchar.h>
#include <windows.h>
#include <SDL.h>

void rr_win_startup(void)
{
    static wchar_t p[32768];
    DWORD n = GetModuleFileNameW(NULL, p, 32768);
    if (n > 0 && n < 32768) {
        wchar_t *sl = wcsrchr(p, L'\\');
        if (sl) { *sl = 0; _wchdir(p); }
    }
    if (freopen("raveracer.log", "w", stdout)) {
        setvbuf(stdout, NULL, _IONBF, 0);
        _dup2(_fileno(stdout), _fileno(stderr));
    }
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
}
#endif
