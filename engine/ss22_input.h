/*
 * ss22_input.h -- the cabinet's controls of a Super System 22 game on a keyboard and pads (engine/ss22_input.c): a rebindable key and a
 * fixed alternate per action, the menu's Controls page, keys that ramp like MAME's PORT_KEYDELTA, and every SDL game controller
 * (hot-plugged). What a game supplies is a table: its actions (with the INPUTS bit each presses, the pad buttons that press it and
 * whether it drives the wheel or a pedal), the axis ranges, and where the result goes (the sound MCU's ports and A-D converter).
 */
#ifndef ENG_SS22_INPUT_H
#define ENG_SS22_INPUT_H
#include <stdint.h>
#include <SDL2/SDL.h>
#include "eng_ui.h"

enum { SS22_AX_NONE = 0, SS22_AX_WHEEL_LEFT, SS22_AX_WHEEL_RIGHT, SS22_AX_PEDAL1, SS22_AX_PEDAL2 };
#define SS22_PAD(b) (1u << (b))              /* an SDL_CONTROLLER_BUTTON_* in an action's pad mask */

typedef struct {
    const char  *label;                      /* the Controls page's row */
    const char  *key;                        /* the settings file's key_<key> */
    SDL_Scancode def, alt;                   /* the default key, and the fixed alternate (SDL_SCANCODE_UNKNOWN = none) */
    uint16_t     bit;                        /* the INPUTS bit it presses (0 for an axis key) */
    int          axis;                       /* SS22_AX_*: a key that drives the wheel or a pedal */
    uint32_t     pad;                        /* SS22_PAD(...) buttons that press it */
} ss22_action;

typedef struct {
    const ss22_action *actions; int n;
    int wheel_min, wheel_max;                /* the wheel's A-D range, centre 0x200 */
    int wheel_key_span;                      /* a held wheel key aims at centre +- this */
    int wheel_step;                          /* PORT_KEYDELTA: counts a frame */
    int pedal_max[2];                        /* the two pedals' A-D maxima (the analog triggers scale to these) */
    int pedal_step;
    const char *notes[3];                    /* the Controls page's lines under the rows */
    void (*send)(uint16_t pressed, unsigned wheel, unsigned pedal1, unsigned pedal2);   /* into the MCU */
    uint16_t test_bit, service_bit;          /* the INPUTS bits of the cabinet's Test switch and Service button (0 = the game has none).
                                              * THE TEST SWITCH IS A TOGGLE like MAME's (press once = on, again = off): the action whose bit
                                              * is test_bit latches. The Controls page has both as rows, so a pad (the Steam Deck) reaches them */
} ss22_input_game;

void ss22_input_init(const ss22_input_game *g);         /* after the settings file is loaded: the key bindings, the pads */
const eng_ui_page *ss22_input_page(void);
void ss22_input_event(const SDL_Event *e);              /* controller hot-plug */
void ss22_input_update(void);                           /* once per emulated frame: read the keys and pads, tell the MCU */
void ss22_input_neutral(void);                          /* release everything (the menu opened): a centred wheel, no buttons */
#endif
