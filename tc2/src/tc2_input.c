/*
 * tc2_input.c -- Time Crisis 2's cabinet on a keyboard, mouse, absolute-mouse light gun and pads: the copied input module (src/eng/ss22_input.c,
 * Time Crisis 1's light-gun support with it: the pointer / either stick / the arrow keys aim, the screen edge, R or a side button is
 * off-screen = reload, trigger and pedal on keys, mouse buttons and pad buttons, all rebindable on the menu's Controls page, the gun border
 * F8, the pad's rumble for the recoil) with the controls MAME's timecrs2 and crszone ports share (JVS_PLAYER1 trigger / pedal / the operator's
 * user service up/down/enter, JVS_SYSTEM test, coin); the gun's range is the game's (include/tc2_game.h: TC2 X 91..824 Y 38..285, Crisis Zone
 * X 0x40..0x33F Y 0x20..0xFF). Crisis Zone uses this file unchanged: same keys, mouse / absolute-mouse guns, pads, recoil.
 * What the cabinet reads goes through the I/O board (the H8/3334 JVS program, to be translated): tc2_inputs holds it until then.
 */
#include <stdint.h>
#include <stdbool.h>
#include <SDL2/SDL.h>
#include "eng/ss22_input.h"
#include "eng/ss22_board.h"

uint16_t g_ss22_gun_x, g_ss22_gun_y;           /* the game's port range (include/tc2_game.h): centred at start-up below */
__attribute__((constructor)) static void gun_centre(void) { g_ss22_gun_x = (uint16_t)(TC2_GUN_X0 + TC2_GUN_XW / 2); g_ss22_gun_y = (uint16_t)(TC2_GUN_Y0 + TC2_GUN_YH / 2); }
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

/* ANGLER KING (tc2_game.io_asca3: the FCA-1 = ASCA-3 board, src/tc2_h8.c): the rod is AIMED like the gun (pointer, either stick, the arrow keys:
 * the gun's 0..1023 range becomes Rod Yaw / Rod Pitch, MAME 0x0100..0xFFFF; the arm follows the rod, its yaw reversed as MAME's port is), the
 * cast / confirm button is the gun's trigger, the REEL is a pedal axis (a held key ramps it, a pad trigger sets it) that winds the rotary
 * encoder by up to 16 counts a frame (MAME's dial: PORT_KEYDELTA 8). Up / Down select are the operator keys plus the pad's shoulders. */
extern uint16_t tc2_analog_in[8];
static const ss22_action ak_actions[] = {
    { "Insert coin",   "coin",    SDL_SCANCODE_5,     SDL_SCANCODE_6,       IN_COIN,      0, SS22_PAD(SDL_CONTROLLER_BUTTON_BACK) },
    { "Cast / enter",  "trigger", SDL_SCANCODE_SPACE, SDL_SCANCODE_RETURN,  IN_TRIGGER,   0, SS22_PAD(SDL_CONTROLLER_BUTTON_A) | SS22_PAD(SDL_CONTROLLER_BUTTON_START), SDL_BUTTON_LMASK },
    { "Reel",          "reel",    SDL_SCANCODE_R,     SDL_SCANCODE_LSHIFT,  0, SS22_AX_PEDAL1, SS22_PAD(SDL_CONTROLLER_BUTTON_B) },
    { "Select up",     "op_up",   SDL_SCANCODE_PAGEUP,   SDL_SCANCODE_W,    IN_USR_UP,    0, SS22_PAD(SDL_CONTROLLER_BUTTON_LEFTSHOULDER) },
    { "Select down",   "op_down", SDL_SCANCODE_PAGEDOWN, SDL_SCANCODE_S,    IN_USR_DOWN,  0, SS22_PAD(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) },
    { "Service",       "service", SDL_SCANCODE_9,     SDL_SCANCODE_UNKNOWN, IN_SERVICE,   0, 0 },
    { "Test",          "test",    SDL_SCANCODE_F2,    SDL_SCANCODE_UNKNOWN, IN_TEST,      0, 0 },
};
static void ak_send_inputs(uint16_t pressed, unsigned wheel, unsigned reel, unsigned p2)
{
    (void)wheel; (void)p2;
    tc2_inputs = pressed;
    const uint32_t x = g_ss22_gun_off ? 512 : g_ss22_gun_x, y = g_ss22_gun_off ? 512 : g_ss22_gun_y;   /* 0..1023 (ak_game.c's gun range) */
    const uint16_t yaw = (uint16_t)(0x0100 + x * 0xFEFFu / 1023), pitch = (uint16_t)(0x0100 + (1023 - y) * 0xFEFFu / 1023);
    tc2_analog_in[2] = yaw; tc2_analog_in[3] = pitch;                     /* Rod Yaw / Rod Pitch */
    tc2_analog_in[0] = (uint16_t)(0x10000 - yaw); tc2_analog_in[1] = pitch;   /* Arm Yaw (PORT_REVERSE) / Arm Pitch */
    void tc2_reel_wind(int); tc2_reel_wind((int)(reel / 4));            /* the reel: 0..64 -> 0..16 counts a frame (src/tc2_h8.c) */
}
const ss22_input_game ak_input_game = {
    ak_actions, (int)(sizeof ak_actions / sizeof *ak_actions),
    0x001, 0x3FF, 0x1FF, 12,
    { 64, 0 }, 8,
    { "Enter, then press the new key (Esc cancels)",
      "Mouse / arrow keys / either stick aim the ROD; left button, Space or pad A = cast / enter. Hold R (or the pad's right trigger) to REEL",
      "Select: PageUp / PageDown or W / S, the pad's shoulders. 5 = coin, F2 = test" },
    ak_send_inputs,
    IN_TEST, IN_SERVICE,
    false,
    true,                                     /* the pointer aims the rod */
};

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
