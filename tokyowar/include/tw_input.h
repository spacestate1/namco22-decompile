#ifndef TW_INPUT_H
#define TW_INPUT_H
#include <SDL2/SDL.h>
#include <stdbool.h>
#include "eng_ui.h"

/* Tokyo Wars' cabinet controls (MAME namcos22.cpp, INPUT_PORTS_START(tokyowar)): coin, service, test (the service switch),
 * start (also the view change), the two trigger buttons, the steering wheel (ADC 0, 0x100..0x300, centre 0x200) and the
 * forward / backward pedals (ADC 2 and 3, 0..0x100). The MCU reads them (tw_snd_inputs); this is the keyboard and pad side.
 *
 *   5 coin   Enter start   9 service   F2 test   Left/Right or A/D steer   Up/W forward   Down/S backward
 *   Z or Left-Ctrl left trigger   X or Left-Alt right trigger
 * Each action has a rebindable KEY (the Controls page of the menu, saved to tw_controls.cfg as key_<action>) and a fixed
 * alternate (the second key above). Pads (SDL game controllers, hot-plugged): left stick X steers, RT forward, LT backward,
 * X/LB left trigger, B/RB right trigger, A/Start start, Back coin. Keys ramp like MAME's KEYDELTA(20) per frame. */
void tw_input_init(void);
void tw_input_event(const SDL_Event *e);     /* controller hot-plug */
void tw_input_update(void);                  /* once per emulated frame: read the keys and pads, tell the MCU */
void tw_input_neutral(void);                 /* release everything (the menu opened): the MCU reads a centred wheel and no buttons */
const eng_ui_page *tw_input_page(void);      /* the menu's Controls page: the key bindings */
#endif
