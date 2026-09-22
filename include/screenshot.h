/*
 * screenshot.h -- capture the LIVE GL framebuffer to a timestamped PNG.
 *
 * main.c's save_screenshot() is the headless one-shot: PPM, and it reads a
 * fixed SCREEN_WIDTH x SCREEN_HEIGHT block from the origin, which is only
 * correct because headless runs at win_scale 1.  The interactive window
 * renders at 2x, so that same read would capture the bottom-left QUARTER.
 * This reads the real drawable size, so it is correct at any window scale.
 */
#ifndef SCREENSHOT_H
#define SCREENSHOT_H

#include <SDL2/SDL.h>

/* Grab the current front/back buffer and write <dir>/shot_<stamp>_f<frame>.png.
 * Creates <dir> if needed.  Returns the path written (a static buffer, valid
 * until the next call) or NULL on failure. */
const char *screenshot_capture(SDL_Window *win, const char *dir, unsigned frame);

#endif
