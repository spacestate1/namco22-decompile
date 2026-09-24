/* render_target.h — render at the chosen resolution, scale to the window. */
#ifndef RENDER_TARGET_H
#define RENDER_TARGET_H
#include <SDL2/SDL.h>

/* Bind the offscreen target of res_w x res_h (0 x 0 = native: draw straight
 * into the window). *out_w / *out_h get the size actually being drawn. */
void rt_begin(SDL_Window *win, int res_w, int res_h, int *out_w, int *out_h);
/* Scale the target into the window (aspect kept unless stretch) and leave
 * the window's framebuffer bound. A no-op when rt_begin drew directly. */
void rt_end(SDL_Window *win, int stretch);

#endif
