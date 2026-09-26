/*
 * eng_ui.h -- the menu bar every game's Escape menu is (engine/eng_ui.c): Nuklear, as in Prop Cycle.
 *
 * A bar across the top of the window with one title per page, the chosen page dropping down under its title, one row per setting:
 *   File     Resume / Restart / Exit
 *   Display  Widescreen, Window mode, Window size, Resolution (a row AND a list), Aspect ratio, Scaling   (engine/eng_display.c)
 *   Audio    Volume (a slider, Mute / 25 / 50 / 100)
 *   ...      the game's own pages (Tokyo Wars: Controls), added with eng_ui_add_page()
 * It works from the mouse, the KEYBOARD or a PAD (a popup bar needs a click to open, so this is a tabbed strip instead):
 *   Up/Down move between rows (above the first row: the title strip)     Left/Right change the value (on the strip: page)
 *   Enter/Space select / toggle / step      Tab, PgUp/PgDn, pad LB/RB switch page      Esc (pad B / Start / R3) close
 * The game pauses while the menu is open (the host stops feeding the emulation) and gets no keys.
 * Every change goes through eng_display / eng_cfg, so it is applied at once and saved.
 *
 * One translation unit defines Nuklear's implementation (third_party/nuklear.h is the tree's one copy): a binary links either
 * this or Prop Cycle's src/ui_menu.c, never both.
 */
#ifndef ENG_UI_H
#define ENG_UI_H
#include <SDL2/SDL.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct eng_ui_page {
    const char *name;                                    /* the title on the bar */
    float drop_w;                                        /* the dropdown's width (0 = 340) */
    float label_w;                                       /* the label column's width (0 = 120) */
    float row_h;                                         /* row height (0 = 26) */
    int  (*nrows)(void);
    bool (*has_value)(int row);                          /* takes Left/Right (a value) rather than being an action; NULL = all */
    bool (*enabled)(int row);                            /* NULL = all */
    void (*text)(int row, char *label, size_t ln, char *value, size_t vn);
    void (*change)(int row, int dir);                    /* dir 0 = Enter / click, -1 / +1 = Left / Right */
    void (*notes)(void (*line)(const char *fmt, ...));   /* lines under the rows; NULL = none */
} eng_ui_page;

bool eng_ui_init(SDL_Window *win, const char *title);    /* after the GL context; false = no menu (the game runs without) */
void eng_ui_shutdown(void);
void eng_ui_add_page(const eng_ui_page *p);              /* after the standard three, in the order added */
bool eng_ui_is_open(void);
void eng_ui_set_open(bool on);
bool eng_ui_quit_requested(void);                        /* File > Exit */
bool eng_ui_restart_requested(void);                     /* File > Restart: the host re-launches the program */
void eng_ui_input_begin(void);                           /* around the host's SDL_PollEvent loop */
void eng_ui_input_end(void);
bool eng_ui_event(SDL_Event *e);                         /* true = the menu consumed it (it is open) */
void eng_ui_draw(bool *quit);
void eng_ui_set_hint(const char *text, int frames);       /* a one-line hint at the bottom of the window for `frames` frames while the menu is closed */                            /* over the picture, into the window, before the swap */
/* the next key press is handed to cb (SDL_SCANCODE_UNKNOWN if cancelled with Esc): a key-binding row's "press a key" */
void eng_ui_capture_key(void (*cb)(SDL_Scancode sc, void *u), void *u);
bool eng_ui_capturing(void);
void eng_ui_goto(int page, int row);                     /* tests */
void eng_ui_nav(char k);                                 /* tests: u d l r o(k) b(ack) n(ext page) p(revious page) */
#endif
