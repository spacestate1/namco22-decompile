/*
 * eng_display.h -- the display choices every game's Display menu offers, and what they mean (engine/eng_display.c).
 *
 *   Widescreen   the picture's shape follows the window (Hor+): the 3D scene is drawn wider than 4:3 (ss22_draw widens
 *                full-frame viewports through g_scene_x0/x1) and the window itself takes a 16:9 shape; the text and
 *                sprite layers stay in the 4:3 centre, where the game drew them.
 *   Window mode  windowed / fullscreen (desktop) / fullscreen (exclusive: the monitor switches to the chosen resolution).
 *   Window size  1x..4x the board's 640x480 (16:9 of the same height with widescreen).
 *   Resolution   the RENDER size (the engine draws the frame at that many pixels and the picture is scaled to the window);
 *                the height counts, the width follows the shape. 640x480 is the board's own picture; 0x0 = native (window).
 *   Aspect       the picture's shape when widescreen is off: stretch, 4:3, 8:7, 16:9.
 *   Scaling      smooth (linear), sharp (nearest) or integer (the largest whole multiple of the render size).
 * Every change is applied at once and saved to the game's cfg file (engine/eng_cfg.c). Prop Cycle's and Rave Racer's own hosts
 * carry the same logic (raverace/src/rr_host.c is where it was first written); this is the copy the next game uses.
 */
#ifndef ENG_DISPLAY_H
#define ENG_DISPLAY_H
#include <SDL2/SDL.h>
#include <stdbool.h>

typedef struct {
    int wide;                    /* 0/1 */
    int winmode;                 /* 0 windowed, 1 fullscreen (desktop), 2 fullscreen (exclusive) */
    int scale;                   /* 1..4 */
    int res_w, res_h;            /* render size; res_h <= 0 = native */
    int aspect;                  /* 0 stretch, 1 4:3, 2 8:7, 3 16:9 */
    int scaling;                 /* 0 smooth, 1 sharp, 2 integer */
    int volume;                  /* 0..100 */
} eng_display_t;
extern eng_display_t g_eng_disp;

/* Before the window exists: load cfg_path (engine/eng_cfg.c) into g_eng_disp. scale > 0 overrides the saved window size,
 * fullscreen forces fullscreen (desktop). */
void eng_disp_load(const char *cfg_path, int scale, bool fullscreen);
/* After the window exists: the exclusive mode, a size too big for this display. */
void eng_disp_attach(SDL_Window *win);
/* The window's size for scale k (a 16:9 window with widescreen on). */
int  eng_disp_win_w(int k);
int  eng_disp_win_h(int k);
/* The render size for the window as it is now. */
void eng_disp_render_size(int *w, int *h);
/* The picture's rectangle in drawable pixels (x, y from the top-left) for a render of bw x bh. */
SDL_Rect eng_disp_picture_rect(int bw, int bh);
bool eng_disp_sharp(void);                 /* the filter rt_end_rect wants: nearest (sharp / integer) or linear */
void eng_disp_toggle_fullscreen(void);     /* F11: windowed <-> fullscreen (desktop) */

void eng_disp_set_wide(int on);
void eng_disp_set_winmode(int m);
void eng_disp_set_scale(int k);
void eng_disp_set_res(int w, int h);
void eng_disp_set_aspect(int a);
void eng_disp_set_scaling(int s);
void eng_disp_set_volume(int pct);         /* also calls the hook below */
void eng_disp_set_volume_hook(void (*fn)(int pct));

int  eng_disp_res_count(void);
void eng_disp_res_get(int i, int *w, int *h);
void eng_disp_res_label(int w, int h, char *out, size_t n);
const char *eng_disp_winmode_name(int m);
const char *eng_disp_aspect_name(int a);
const char *eng_disp_scaling_name(int s);
#endif
