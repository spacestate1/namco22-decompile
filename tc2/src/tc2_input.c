/*
 * tc2_input.c -- Time Crisis 2's cabinet on a keyboard, mouse, absolute-mouse light gun and pads: the copied input module (src/eng/ss22_input.c,
 * Time Crisis 1's light-gun support with it: the pointer / either stick / the arrow keys aim, the screen edge, R or a side button is
 * off-screen = reload, trigger and pedal on keys, mouse buttons and pad buttons, all rebindable on the menu's Controls page, the gun border
 * F8, the pad's rumble for the recoil) with TC2's controls (MAME's timecrs2 ports: JVS_PLAYER1 trigger / pedal / the operator's user
 * service up/down/enter, JVS_SYSTEM test, coin; the gun's X 91..824, Y 38..285).
 * What the cabinet reads goes through the I/O board (the H8/3334 JVS program, to be translated): tc2_inputs holds it until then.
 */
#include <stdint.h>
#include <stdbool.h>
#include <SDL2/SDL.h>
#include "eng/ss22_input.h"
#include "eng/ss22_board.h"

uint16_t g_ss22_gun_x = TC2_GUN_X0 + TC2_GUN_XW / 2, g_ss22_gun_y = TC2_GUN_Y0 + TC2_GUN_YH / 2;
bool     g_ss22_gun_off;
uint32_t g_ss22_wram_watch_off = ~0u;
void (*g_ss22_wram_watch)(uint32_t v, int size);

#define IN_COIN      0x0001
#define IN_SERVICE   0x0004
#define IN_TEST      0x0008
#define IN_TRIGGER   0x0010
#define IN_PEDAL     0x0020
#define IN_USR_UP    0x0040
#define IN_USR_DOWN  0x0080
#define IN_USR_ENTER 0x0100
static const ss22_action actions[] = {
    { "Insert coin",   "coin",    SDL_SCANCODE_5,     SDL_SCANCODE_6,       IN_COIN,      0, SS22_PAD(SDL_CONTROLLER_BUTTON_BACK) },
    { "Gun trigger",   "trigger", SDL_SCANCODE_SPACE, SDL_SCANCODE_RETURN,  IN_TRIGGER,   0, SS22_PAD(SDL_CONTROLLER_BUTTON_A) | SS22_PAD(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER), SDL_BUTTON_LMASK },
    { "Foot pedal",    "pedal",   SDL_SCANCODE_Z,     SDL_SCANCODE_X,       IN_PEDAL,     0, SS22_PAD(SDL_CONTROLLER_BUTTON_B) | SS22_PAD(SDL_CONTROLLER_BUTTON_LEFTSHOULDER), SDL_BUTTON_RMASK | SDL_BUTTON_MMASK },
    { "Service",       "service", SDL_SCANCODE_9,     SDL_SCANCODE_UNKNOWN, IN_SERVICE,   0, 0 },
    { "Test",          "test",    SDL_SCANCODE_F2,    SDL_SCANCODE_UNKNOWN, IN_TEST,      0, 0 },
    { "Operator up",   "op_up",   SDL_SCANCODE_PAGEUP,   SDL_SCANCODE_UNKNOWN, IN_USR_UP,    0, 0 },
    { "Operator down", "op_down", SDL_SCANCODE_PAGEDOWN, SDL_SCANCODE_UNKNOWN, IN_USR_DOWN,  0, 0 },
    { "Operator enter","op_enter",SDL_SCANCODE_HOME,     SDL_SCANCODE_UNKNOWN, IN_USR_ENTER, 0, 0 },
};

uint16_t tc2_inputs;                          /* the pressed IN_* bits, for the I/O board */
static void tc2_send_inputs(uint16_t pressed, unsigned wheel, unsigned p1, unsigned p2) { (void)wheel; (void)p1; (void)p2; tc2_inputs = pressed; }

const ss22_input_game tc2_input_game = {
    actions, (int)(sizeof actions / sizeof *actions),
    0x001, 0x3FF, 0x1FF, 12,
    { 0, 0 }, 0,
    { "Enter, then press the new key (Esc cancels)",
      "Mouse / absolute-mouse gun aims: left = trigger, right/middle = pedal; screen edge, R or side button = off-screen (reload). F8 = gun border",
      "Pad: either stick or the D-pad aims, A/RB trigger, B/LB pedal, Back coin. Arrow keys aim too; 5 = coin" },
    tc2_send_inputs,
    IN_TEST, IN_SERVICE,
    false,                                    /* no steering motor */
    true,                                     /* the light gun */
};
