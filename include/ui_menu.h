/*
 * ui_menu.h — Nuklear menu bar (Escape toggles it).
 *
 * Escape used to quit outright. It now opens a menu bar across the top with
 * Display / Maps / Controls / Quit; Escape again closes it. Quitting is a
 * menu item so the old muscle memory cannot destroy a session by accident.
 */
#ifndef UI_MENU_H
#define UI_MENU_H

#include <SDL2/SDL.h>
#include <stdbool.h>

/* ---- control remapping -------------------------------------------------
 * Actions the player can bind. input.c reads the resolved scancodes, so a
 * remap takes effect immediately and survives via ui_controls_save(). */
typedef enum {
    ACT_COIN, ACT_START, ACT_SERVICE, ACT_TEST,
    ACT_LEFT, ACT_RIGHT, ACT_UP, ACT_DOWN,
    ACT_PEDAL,
    ACT_COUNT
} ui_action;

extern SDL_Scancode ui_binding[ACT_COUNT];
const char *ui_action_name(ui_action a);
void ui_controls_defaults(void);
int  ui_controls_save(void);      /* -> propcycl_controls.cfg */
int  ui_controls_load(void);

/* ---- map viewer --------------------------------------------------------
 * A freely orbitable view of one course's terrain, built from the point ROM
 * rather than from a capture, so any course can be inspected without one. */
bool ui_map_active(void);
int  ui_map_course(void);
void ui_map_input(const Uint8 *keys, float dt);
void ui_map_mouse(int dx, int dy, int buttons, int wheel);
void ui_map_camera(float m[3][3], float t[3], float *zoom);

/* ---- lifecycle --------------------------------------------------------- */
/* Target aspect ratio, or 0 to stretch to the window. The renderer
 * letterboxes/pillarboxes its 640x480 output to match. */
float ui_aspect(void);
/* The chosen internal render resolution, or 0 x 0 for native (the window's
 * own pixel size). The window is NOT resized to it; render_target.c scales. */
void ui_render_res(int *w, int *h);
void ui_init(SDL_Window *win);
void ui_shutdown(void);
bool ui_is_open(void);
void ui_toggle(void);
/* Nuklear buffers input between these two calls and only commits it at
 * ui_input_end(). Without the pair the context never settles: hover and
 * active state flip every frame and the menu visibly flickers. Bracket the
 * SDL_PollEvent loop with them. */
void ui_input_begin(void);
void ui_input_end(void);
/* Returns true if the UI consumed the event. */
bool ui_handle_event(SDL_Event *e);
/* Draw the menu; call last, after the scene. Sets *quit when Quit is used. */
void ui_draw(SDL_Window *win, bool *quit);
/* File -> Restart was chosen: main() re-launches the program on exit. */
bool ui_restart_requested(void);

void ui_set_hint(const char *text, int frames);   /* a hint line at the bottom of the window while the menu is closed */
#endif /* UI_MENU_H */
