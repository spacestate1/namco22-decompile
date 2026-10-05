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
extern int g_cfg_ffb_strength, g_cfg_ffb_invert;
extern int g_cfg_fullscreen, g_cfg_scale, g_cfg_scaling, g_cfg_volume;
extern int g_cfg_winmode, g_cfg_res_w, g_cfg_res_h, g_cfg_wide, g_cfg_aspect, g_cfg_draw, g_cfg_fps;
extern char g_cfg_net_server[128], g_cfg_net_name[24];   /* the Online page's server and lobby name */
bool rr_input_set_option(const char *path, const char *key, const char *val);
const char *rr_input_action_name(int a);
void rr_input_bind_key(int a, SDL_Scancode sc);
typedef struct {
    int axis; bool invert, half;
    char guid[40];
    int shape_axes, shape_buttons;   /* the bound device's axes and buttons (0 = not recorded): one wheel can be several devices under one GUID */
    int direction, rest;
    bool rest_valid;
} rr_joyaxis_t;
void rr_input_capture_begin(int action);
bool rr_input_capture_event(int action, const SDL_Event *e);
void rr_input_binding_label(int action, char *text, size_t size);
bool rr_input_device_matches(SDL_Joystick *js, const char *guid);
/* the device an axis is bound to: its GUID and, when recorded, its axis and button counts -- a Fanatec DD base is two
 * "FANATEC Wheel"s under one GUID (12 axes / 63 buttons and 8 axes / 108 buttons) */
bool rr_input_axis_device(const rr_joyaxis_t *ax, SDL_Joystick *js);
bool rr_input_button_matches(int action, SDL_Joystick *js, int button);
double rr_input_pedal_value(rr_joyaxis_t *axis, int value);
extern rr_joyaxis_t g_joy_steer, g_joy_gas, g_joy_brake;
extern int g_joy_button[RR_ACT_N];
extern char g_joy_button_guid[RR_ACT_N][40];

void rr_input_load(const char *path);          /* defaults, then the file if present */
bool rr_input_write(const char *path);         /* write the defaults as a template */

bool rr_input_replay_open(const char *path);
bool rr_input_replaying(void);
bool rr_input_record_start(const char *path);
void rr_input_record_stop(void);
bool rr_input_recording(void);
void rr_input_frame(uint32_t frame);           /* apply replay / log recording */

#endif
