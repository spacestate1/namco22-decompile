/* win_startup.c -- see win_startup.h. */
#ifdef _WIN32
#include <stdio.h>
#include <io.h>
#include <direct.h>
#include <wchar.h>
#include <windows.h>
#include <SDL.h>
#include "win_startup.h"

void eng_win_startup(const char *logname)
{
    static wchar_t p[32768];
    DWORD n = GetModuleFileNameW(NULL, p, 32768);
    if (n > 0 && n < 32768) {
        wchar_t *sl = wcsrchr(p, L'\\');
        if (sl) { *sl = 0; _wchdir(p); }
    }
    if (freopen(logname, "w", stdout)) {
        setvbuf(stdout, NULL, _IONBF, 0);
        _dup2(_fileno(stdout), _fileno(stderr));
    }
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
}
#endif
