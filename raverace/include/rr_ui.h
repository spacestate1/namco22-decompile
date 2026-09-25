/* rr_ui.h -- the Nuklear menu bar (Escape), and the host setters it drives. */
#ifndef RR_UI_H
#define RR_UI_H
#include <stdbool.h>
#include <SDL.h>

bool rr_ui_init(SDL_Window *win);            /* the window has a current OpenGL context */
void rr_ui_shutdown(void);
bool rr_ui_is_open(void);
void rr_ui_set_open(bool on);
/* Nuklear buffers input between these two; bracket the SDL_PollEvent loop */
void rr_ui_input_begin(void);
void rr_ui_input_end(void);
bool rr_ui_event(SDL_Event *e);          /* true = the menu took it */
void rr_ui_draw(bool *quit);             /* after the game picture, before present */
bool rr_ui_quit_requested(void);
/* headless tests: drive the menu without an input device */
enum { RR_UI_UP, RR_UI_DOWN, RR_UI_LEFT, RR_UI_RIGHT, RR_UI_OK, RR_UI_BACK, RR_UI_TABPREV, RR_UI_TABNEXT };
void rr_ui_test_nav(int k);
void rr_ui_test_goto(int tab, int row);  /* tab: 0 File 1 Display 2 Audio 3 Controls 4 Record */

/* rr_host.c: every display/audio/option change, applied and saved */
void rr_host_set_winmode(int m);         /* 0 windowed, 1 desktop fullscreen, 2 exclusive */
void rr_host_set_scale(int k);           /* windowed size, 1..4 x 640x480 */
void rr_host_set_res(int w, int h);      /* render size; 0x0 = native */
void rr_host_set_wide(int on);
void rr_host_set_draw(int level);           /* draw distance 0 original .. 3 maximum */
const char *rr_host_draw_name(int level);
void rr_host_set_aspect(int a);          /* 0 stretch, 1 4:3, 2 8:7, 3 16:9 */
void rr_host_set_scaling(int s);         /* 0 smooth, 1 sharp, 2 integer */
void rr_host_set_volume(int percent);
void rr_host_set_freeplay(bool on);
void rr_host_toggle_record(void);
int  rr_host_res_count(void);
void rr_host_res_get(int i, int *w, int *h);
void rr_host_render_size(int *w, int *h);

#endif
