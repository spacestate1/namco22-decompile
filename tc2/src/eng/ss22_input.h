/* TIME CRISIS 2'S OWN COPY of namco-2x-systems engine/ss22_input.h (copied 2026-10-05, with Time Crisis 1's light-gun support). TC2 is independent of the System 22 project: change it here. */
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
    uint8_t      mouse;                      /* SDL_BUTTON_LMASK/RMASK/MMASK... that press it (light-gun games; 0 = none) */
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
    bool wheel_motor;                        /* the cabinet's steering motor (Dirt Dash): the MCU's UART0 bytes drive a force-feedback wheel */
    bool light_gun;                          /* a LIGHT GUN cabinet: the mouse pointer (or the right stick, or the arrow keys) aims -> g_ss22_gun_x/_y (engine/ss22_board.h) */
    /* the steering torque as the 68K builds it (0 = use the motor byte alone): a work-RAM word written once a frame as a running
     * sum of `parts` terms -- the first write the first term, each later one adding the next -- clamped to +-`limit` and then
     * quantised (with a dither) into the motor byte. `centre` has a bit set for each term that is centring (a spring on the
     * wheel's position, damping on its speed); the others are the road. Tapping the sum gives the force in full resolution,
     * and the two groups their own gains (the Controls page's FFB centering / FFB road effects). */
    struct { uint32_t addr; int parts; unsigned centre; int limit; } torque;
} ss22_input_game;

void ss22_input_init(const ss22_input_game *g);         /* after the settings file is loaded: the key bindings, the pads */
const eng_ui_page *ss22_input_page(void);
void ss22_input_event(const SDL_Event *e);              /* controller hot-plug */
void ss22_input_update(void);                           /* once per emulated frame: read the keys and pads, tell the MCU */
void ss22_input_neutral(void);                          /* release everything (the menu opened): a centred wheel, no buttons, no wheel force */
/* a byte the sound MCU sent the Motor/Feedback PCB (UART0). Bit 0 = a command, bit 1 = the direction (set: toward the wheel's
 * higher A-D side), bits 2-7 = 63 - the strength with the bit order reversed; 0xFF = no force. The 68K sends one a frame, its
 * steering torque (work RAM 0xE00148) / 8, clamped to 62 -- what the FFB plugin reads out of MAME, here from the real link. */
void ss22_input_motor(uint8_t b);
/* the crosshair: where the gun is aimed in the 4:3 game picture, 0..1 each way; false = off-screen (nothing to draw) */
bool ss22_input_aim(float *nx, float *ny);
void ss22_input_rumble(uint16_t low, uint16_t high, uint32_t ms);
int  ss22_input_gun_flash(void);            /* light-gun games: the shot flash is drawn (Controls page and Display page) */
void ss22_input_set_gun_flash(int pct);  /* the flash's strength 0-100 % */
void ss22_input_flash_step(int dir);     /* F6: the next preset (100, 75, 50, 25, OFF), with a hint on screen */
void ss22_input_mouse_step(int dir);     /* F7: the next mouse-speed preset (Direct, 125, 150, 200, 300 %), with a hint on screen */
//   /* a short kick on every connected pad (engine/ss22_out.c: a gun's recoil) */
void ss22_input_close(void);                            /* stop the wheel's force and let go of it (also run at exit) */
#endif
