/*
 * ss22_host.h -- a Super System 22 game in a window (engine/ss22_host.c): the OpenGL context, the menu bar, the display modes,
 * F12 screenshots, vsync or a timer at the board's 59.906 Hz, the sound card. Shared by every Super 22 game whose 68K is a lifted
 * program (Tokyo Wars, Dirt Dash); the game supplies its name, its settings file and its cabinet controls as an ss22_host_game.
 */
#ifndef ENG_SS22_HOST_H
#define ENG_SS22_HOST_H
#include <stdbool.h>
#include <SDL2/SDL.h>
#include "eng_ui.h"

typedef struct {
    const char *title;                       /* the window and the menu bar: "Tokyo Wars" */
    const char *cfg_file;                    /* the settings file: "tw_controls.cfg" */
    const char *tag;                         /* "TW": the test environment variables <tag>_VSYNC, _HOSTDBG, _WINSHOT, _MENU_AT, _MENU_KEYS, _MENU_PRESS */
    const char *shot_prefix;                 /* "tw": screenshots/<prefix>_<date>.ppm, <prefix>_window_<n>.ppm */
    void (*input_init)(void);                /* the cabinet's keys and pads, after the settings are loaded */
    const eng_ui_page *(*input_page)(void);  /* the menu's Controls page */
    void (*input_event)(const SDL_Event *e); /* every SDL event, first: controller hot-plug */
    void (*input_update)(void);              /* once per emulated frame while the game runs */
    void (*input_neutral)(void);             /* the menu opened: a centred wheel and no buttons */
    void (*snd_set_output)(bool live);       /* the sound board's output goes to the sound card */
} ss22_host_game;

bool ss22_host_open(const ss22_host_game *g, int scale, bool fullscreen);   /* a real window; scale <= 0 = the saved window size */
bool ss22_host_open_headless(void);                                          /* an offscreen GL context, for --shots and --render-dump */
bool ss22_host_restart_requested(void);                                      /* File > Restart: re-launch the program after the clean-up */
bool ss22_host_active(void);                                                 /* a window is open (its keyboard drives the cabinet) */
bool ss22_host_frame(void);                                                  /* once per emulated frame, after the video is prepared: events, draw, present, pace; false = quit */
void ss22_host_shot(const char *path);                                       /* the prepared frame, 640x480, to a PPM (any GL context) */
void ss22_host_close(void);
#endif
