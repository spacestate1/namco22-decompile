/*
 * win_startup.h -- Windows start-up (MinGW-w64 builds only; a no-op elsewhere), engine/win_startup.c.
 *
 *  - work from the program's own folder, found with the WIDE API: double-clicked or started from a shortcut, extracted/, roms/
 *    and the cfg are there; the ANSI calls cannot name a folder outside the system code page (C:\Users\Zoe\...);
 *  - a windowed program has no console: stdout and stderr go to logname beside the .exe -- the first thing to read when it does
 *    not start;
 *  - report real pixel sizes, or Windows stretches (blurs) the window by the display scaling.
 * Rave Racer's src/rr_win.c is the same code with its log name fixed.
 */
#ifndef ENG_WIN_STARTUP_H
#define ENG_WIN_STARTUP_H
#ifdef _WIN32
void eng_win_startup(const char *logname);
#else
static inline void eng_win_startup(const char *logname) { (void)logname; }
#endif
#endif
