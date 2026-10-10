/* ss22_board.h -- TC2's stand-in for what the copied input module (src/eng/ss22_input.c) used from the Super 22 board: the gun's position as the
 * cabinet reports it (Time Crisis 2's ranges, MAME's JVS_SCREEN_POSITION_INPUT_X1/Y1: X 91..824, Y 38..285 -- src/tc2_input.c), and the
 * work-RAM watch a force-feedback wheel uses (no wheel on this cabinet: never set). */
#ifndef TC2_SS22_BOARD_SHIM_H
#define TC2_SS22_BOARD_SHIM_H
#include <stdint.h>
#include <stdbool.h>
extern uint16_t g_ss22_gun_x, g_ss22_gun_y;
extern bool     g_ss22_gun_off;
extern uint32_t g_ss22_wram_watch_off;
extern void (*g_ss22_wram_watch)(uint32_t v, int size);
#define TC2_GUN_X0 91
#define TC2_GUN_XW 733
#define TC2_GUN_Y0 38
#define TC2_GUN_YH 247
#endif
