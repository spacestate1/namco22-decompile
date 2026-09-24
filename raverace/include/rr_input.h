#ifndef RR_INPUT_H
#define RR_INPUT_H
#include <stdint.h>
#include <stdbool.h>
#include <SDL.h>

enum { RR_COIN1, RR_COIN2, RR_SERVICE, RR_TEST, RR_SHIFT_DOWN, RR_SHIFT_UP, RR_VIEW,
       RR_STEER_LEFT, RR_STEER_RIGHT, RR_GAS, RR_BRAKE,
       RR_SCREENSHOT, RR_PAUSE, RR_RECORD, RR_QUIT, RR_ACT_N };

#define RR_MAXKEYS 4
typedef struct { SDL_Scancode keys[RR_MAXKEYS]; int nkeys; SDL_GameControllerButton pad; } rr_bind_t;
extern rr_bind_t g_bind[RR_ACT_N];
extern int g_steer_speed, g_steer_return, g_pad_deadzone, g_cfg_freeplay;
extern int g_cfg_fullscreen, g_cfg_scale, g_cfg_scaling, g_cfg_volume;
extern int g_cfg_winmode, g_cfg_res_w, g_cfg_res_h, g_cfg_wide, g_cfg_aspect;
bool rr_input_set_option(const char *path, const char *key, const char *val);
const char *rr_input_action_name(int a);
void rr_input_bind_key(int a, SDL_Scancode sc);
typedef struct { int axis; bool invert, half; } rr_joyaxis_t;
extern rr_joyaxis_t g_joy_steer, g_joy_gas, g_joy_brake;
extern int g_joy_button[RR_ACT_N];

void rr_input_load(const char *path);          /* defaults, then the file if present */
bool rr_input_write(const char *path);         /* write the defaults as a template */

bool rr_input_replay_open(const char *path);
bool rr_input_replaying(void);
bool rr_input_record_start(const char *path);
void rr_input_record_stop(void);
bool rr_input_recording(void);
void rr_input_frame(uint32_t frame);           /* apply replay / log recording */

#endif
