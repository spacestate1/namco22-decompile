/*
 * rr_input.c -- control bindings, and the input RECORDER / REPLAY.
 *
 * Bindings: rr_controls.cfg beside the binary's working directory, lines of
 *   action = key[, key...]        e.g.  gas = Up, W
 * with SDL key names (SDL_GetScancodeFromName), and pad_<action> = button with
 * SDL game-controller button names (a b x y back start leftshoulder ...).
 * A missing file means the defaults below; `rr --write-controls` writes them.
 *
 * Recorder: one line per frame on which the cabinet inputs CHANGED --
 *   F <frame> <inputs hex> <steer hex> <gas hex> <brake hex>
 * The emulation is deterministic (keycus LCG seeded, threads only render), so
 * replaying a recording reproduces the run exactly: the tool for "it went wrong
 * when I drove there" and for scripted input tests.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <SDL.h>
#include "rr_hw.h"
#include "rr_input.h"

rr_bind_t g_bind[RR_ACT_N];
int g_steer_speed = 160, g_steer_return = 160;      /* MAME PORT_KEYDELTA(160) */
int g_cfg_freeplay = -1;
int g_cfg_volume = 100;
int g_cfg_fullscreen = 0, g_cfg_scale = 2, g_cfg_scaling = 0;
/* display, matching Prop Cycle's Display menu: window mode 0 windowed / 1 desktop
 * fullscreen / 2 exclusive (-1 = not set: follow `fullscreen`); the RENDER size
 * (0x0 = native, the window's own pixels; 640x480 = the board, the default);
 * widescreen; and the picture aspect when widescreen is off (0 stretch, 1 4:3,
 * 2 8:7, 3 16:9) */
int g_cfg_winmode = -1, g_cfg_res_w = 640, g_cfg_res_h = 480, g_cfg_wide = 0, g_cfg_aspect = 1;   /* scaling: 0 smooth, 1 sharp, 2 integer */            /* free_play = 0|1 in rr_controls.cfg; -1 = not set */
int g_pad_deadzone = 8000;          /* of 32767; a real Xbox One pad here rests at 3019 */

/* Raw joysticks: devices SDL does not know as a gamepad (wheels, pedals,
 * arcade sticks). Axes and buttons are mapped by number -- find the numbers
 * with `rr --joytest`. An axis spec is "<n>[ invert][ half]": half = a pedal
 * that uses only 0..32767; otherwise the full -32768..32767 travel maps to
 * 0..max (pedals usually rest at one end). -1 = unmapped. */
rr_joyaxis_t g_joy_steer = { 0, false, false }, g_joy_gas = { -1, false, false }, g_joy_brake = { -1, false, false };
int g_joy_button[RR_ACT_N];

static const char *act_name[RR_ACT_N] = {
    "coin1", "coin2", "service", "test", "shift_down", "shift_up", "view",
    "steer_left", "steer_right", "gas", "brake",
    "screenshot", "pause", "record", "quit" };

static void set_default(int a, const char *keys, const char *pad)
{
    rr_bind_t *b = &g_bind[a];
    memset(b, 0, sizeof *b);
    char tmp[128]; snprintf(tmp, sizeof tmp, "%s", keys);
    for (char *t = strtok(tmp, ","); t && b->nkeys < RR_MAXKEYS; t = strtok(NULL, ",")) {
        while (isspace((unsigned char)*t)) t++;
        SDL_Scancode sc = SDL_GetScancodeFromName(t);
        if (sc != SDL_SCANCODE_UNKNOWN) b->keys[b->nkeys++] = sc;
        else fprintf(stderr, "[INPUT] unknown key '%s' for %s\n", t, act_name[a]);
    }
    b->pad = pad ? SDL_GameControllerGetButtonFromString(pad) : SDL_CONTROLLER_BUTTON_INVALID;
}

static void defaults(void)
{
    for (int a = 0; a < RR_ACT_N; a++) g_joy_button[a] = -1;
    g_joy_steer = (rr_joyaxis_t){ 0, false, false };
    g_joy_gas = g_joy_brake = (rr_joyaxis_t){ -1, false, false };
    set_default(RR_COIN1, "5", "back");
    set_default(RR_COIN2, "6", NULL);
    set_default(RR_SERVICE, "9", NULL);
    set_default(RR_TEST, "F2", NULL);
    set_default(RR_SHIFT_DOWN, "A", "leftshoulder");
    set_default(RR_SHIFT_UP, "S", "rightshoulder");
    set_default(RR_VIEW, "V", "y");
    set_default(RR_STEER_LEFT, "Left", "dpleft");
    set_default(RR_STEER_RIGHT, "Right", "dpright");
    set_default(RR_GAS, "X, Up", "a");
    set_default(RR_BRAKE, "Z, Down", "b");
    set_default(RR_SCREENSHOT, "F12", NULL);
    set_default(RR_PAUSE, "P", NULL);
    set_default(RR_RECORD, "F9", NULL);
    set_default(RR_QUIT, "Escape", NULL);
}

static char *trim(char *s)
{
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

void rr_input_load(const char *path)
{
    defaults();
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char *h = strchr(line, '#'); if (h) *h = 0;
        char *eq = strchr(line, '='); if (!eq) continue;
        *eq = 0;
        char *k = trim(line), *v = trim(eq + 1);
        if (!strcmp(k, "steer_speed")) { g_steer_speed = atoi(v); continue; }
        if (!strcmp(k, "steer_return")) { g_steer_return = atoi(v); continue; }
        if (!strcmp(k, "pad_deadzone")) { g_pad_deadzone = atoi(v); continue; }
        if (!strcmp(k, "free_play")) { g_cfg_freeplay = atoi(v) ? 1 : 0; continue; }
        if (!strcmp(k, "volume")) { int x = atoi(v); g_cfg_volume = x < 0 ? 0 : x > 100 ? 100 : x; continue; }
        if (!strcmp(k, "fullscreen")) { g_cfg_fullscreen = atoi(v) ? 1 : 0; continue; }
        if (!strcmp(k, "window_mode")) { int x = atoi(v); if (x >= 0 && x <= 2) g_cfg_winmode = x; continue; }
        if (!strcmp(k, "resolution")) { int w, h;
            if (!strcmp(v, "native")) { g_cfg_res_w = g_cfg_res_h = 0; }
            else if (sscanf(v, "%dx%d", &w, &h) == 2 && w >= 320 && h >= 240) { g_cfg_res_w = w; g_cfg_res_h = h; }
            continue; }
        if (!strcmp(k, "widescreen")) { g_cfg_wide = atoi(v) ? 1 : 0; continue; }
        if (!strcmp(k, "aspect")) {
            g_cfg_aspect = !strcmp(v, "stretch") ? 0 : !strcmp(v, "8:7") ? 2 : !strcmp(v, "16:9") ? 3 : 1;
            continue; }
        if (!strcmp(k, "window_scale")) { int x = atoi(v); if (x >= 1 && x <= 4) g_cfg_scale = x; continue; }
        if (!strcmp(k, "scaling")) {
            g_cfg_scaling = !strcmp(v, "sharp") ? 1 : !strcmp(v, "integer") ? 2 : 0;
            continue; }
        if (!strcmp(k, "joy_steer") || !strcmp(k, "joy_gas") || !strcmp(k, "joy_brake")) {
            rr_joyaxis_t *ax = k[4] == 's' ? &g_joy_steer : k[4] == 'g' ? &g_joy_gas : &g_joy_brake;
            ax->axis = atoi(v); ax->invert = strstr(v, "invert") != NULL; ax->half = strstr(v, "half") != NULL;
            continue;
        }
        if (!strncmp(k, "joy_", 4)) {
            int a = -1;
            for (int i = 0; i < RR_ACT_N; i++) if (!strcmp(k + 4, act_name[i])) a = i;
            if (a < 0) fprintf(stderr, "[INPUT] %s: unknown action '%s'\n", path, k);
            else g_joy_button[a] = atoi(v);
            continue;
        }
        bool pad = !strncmp(k, "pad_", 4);
        const char *an = pad ? k + 4 : k;
        int a = -1;
        for (int i = 0; i < RR_ACT_N; i++) if (!strcmp(an, act_name[i])) a = i;
        if (a < 0) { fprintf(stderr, "[INPUT] %s: unknown action '%s'\n", path, k); continue; }
        if (pad) {
            g_bind[a].pad = SDL_GameControllerGetButtonFromString(v);
            if (g_bind[a].pad == SDL_CONTROLLER_BUTTON_INVALID && *v) fprintf(stderr, "[INPUT] unknown pad button '%s'\n", v);
        } else {
            SDL_GameControllerButton keep = g_bind[a].pad;
            set_default(a, v, NULL);
            g_bind[a].pad = keep;
        }
    }
    fclose(f);
    fprintf(stderr, "[INPUT] bindings from %s\n", path);
}

bool rr_input_write(const char *path)
{
    defaults();
    FILE *f = fopen(path, "w");
    if (!f) return false;
    fprintf(f, "# Rave Racer controls. Keys: SDL key names, comma-separated.\n"
               "# pad_<action>: SDL game-controller button (a b x y back start guide\n"
               "# leftshoulder rightshoulder leftstick rightstick dpup dpdown dpleft dpright).\n"
               "# The left stick steers and the triggers are gas (right) / brake (left).\n");
    fprintf(f, "steer_speed = %d    # keyboard steering, ADC counts per frame (MAME KEYDELTA 160)\n", g_steer_speed);
    fprintf(f, "steer_return = %d   # re-centring rate with no key held\n", g_steer_return);
    fprintf(f, "pad_deadzone = %d  # of 32767\n", g_pad_deadzone);
    fprintf(f, "fullscreen = 0      # F11 / Alt+Enter toggle\nwindow_scale = 2    # 1..4 x 640x480\n"
               "window_mode = 0     # 0 windowed | 1 fullscreen (desktop) | 2 fullscreen (exclusive)\n"
               "resolution = 640x480  # render size: 640x480 (the board) | native (window) | WxH\n"
               "widescreen = 0      # 1: fill a wide window with more track at the sides\n"
               "aspect = 4:3        # widescreen off: stretch | 4:3 | 8:7 | 16:9\n"
               "scaling = smooth    # smooth | sharp | integer\nvolume = 100        # master volume, percent\n\n");
    fprintf(f, "# Raw joysticks (wheels, pedals, arcade sticks -- anything SDL does not list\n"
               "# as a gamepad). Find axis/button numbers with: ./build/rr --joytest\n"
               "# axis spec: <n>[ invert][ half]   (half = pedal reads 0..32767 only; -1 = off)\n"
               "joy_steer = 0\njoy_gas = -1\njoy_brake = -1\n"
               "# joy_<action> = <button number>, e.g. joy_coin1 = 6, joy_shift_up = 5\n\n");
    for (int a = 0; a < RR_ACT_N; a++) {
        fprintf(f, "%s = ", act_name[a]);
        for (int i = 0; i < g_bind[a].nkeys; i++) fprintf(f, "%s%s", i ? ", " : "", SDL_GetScancodeName(g_bind[a].keys[i]));
        fprintf(f, "\n");
        if (g_bind[a].pad != SDL_CONTROLLER_BUTTON_INVALID)
            fprintf(f, "pad_%s = %s\n", act_name[a], SDL_GameControllerGetStringForButton(g_bind[a].pad));
    }
    fclose(f);
    return true;
}

/* set one "key = value" line in the config, keeping every other line as the
 * user wrote it (so saving a menu option never rewrites their bindings) */
bool rr_input_set_option(const char *path, const char *key, const char *val)
{
    char buf[16384]; size_t n = 0;
    FILE *f = fopen(path, "r");
    bool done = false;
    if (f) {
        char line[256];
        while (fgets(line, sizeof line, f) && n + 300 < sizeof buf) {
            char *p = line; while (isspace((unsigned char)*p)) p++;
            size_t kl = strlen(key);
            if (!strncmp(p, key, kl) && (isspace((unsigned char)p[kl]) || p[kl] == '=')) {
                n += (size_t)snprintf(buf + n, sizeof buf - n, "%s = %s\n", key, val); done = true;
            } else n += (size_t)snprintf(buf + n, sizeof buf - n, "%s", line);
        }
        fclose(f);
    }
    if (!done) n += (size_t)snprintf(buf + n, sizeof buf - n, "%s = %s\n", key, val);
    if (!(f = fopen(path, "w"))) return false;
    fwrite(buf, 1, n, f);
    fclose(f);
    return true;
}

/* ---------------------------------------------------------- recorder ---- */
static FILE *rec_f, *rep_f;
static uint32_t rep_next_frame = UINT32_MAX;
static unsigned rep_in, rep_st, rep_gas, rep_br;
static uint16_t last[4];
static bool rec_first;

static void rep_read(void)
{
    char line[160];
    rep_next_frame = UINT32_MAX;
    while (rep_f && fgets(line, sizeof line, rep_f))          /* skip '#' comments and blank lines */
        if (sscanf(line, " F %u %x %x %x %x", &rep_next_frame, &rep_in, &rep_st, &rep_gas, &rep_br) == 5) return;
        else rep_next_frame = UINT32_MAX;
}

bool rr_input_replay_open(const char *path)
{
    rep_f = fopen(path, "r");
    if (!rep_f) { fprintf(stderr, "[INPUT] cannot open %s\n", path); return false; }
    rep_read();
    if (rep_next_frame == UINT32_MAX) { fprintf(stderr, "[INPUT] %s holds no input events\n", path); return false; }
    fprintf(stderr, "[INPUT] replaying %s\n", path);
    return true;
}
bool rr_input_replaying(void) { return rep_f != NULL; }

bool rr_input_record_start(const char *path)
{
    if (rec_f) return true;
    rec_f = fopen(path, "w");
    if (!rec_f) { fprintf(stderr, "[INPUT] cannot write %s\n", path); return false; }
    fprintf(rec_f, "# Rave Racer input recording: F frame inputs steer gas brake (hex), on change\n");
    rec_first = true;
    fprintf(stderr, "[INPUT] recording to %s\n", path);
    return true;
}
void rr_input_record_stop(void)
{
    if (rec_f) { fclose(rec_f); rec_f = NULL; fprintf(stderr, "[INPUT] recording stopped\n"); }
}
bool rr_input_recording(void) { return rec_f != NULL; }

/* once per frame, after the host has polled: replay overrides, recorder logs */
void rr_input_frame(uint32_t frame)
{
    if (rep_f) {
        while (rep_next_frame <= frame) {
            g_hw.inputs = (uint16_t)rep_in; g_hw.steer = (uint16_t)rep_st;
            g_hw.gas = (uint16_t)rep_gas; g_hw.brake = (uint16_t)rep_br;
            rep_read();
        }
    }
    if (rec_f) {
        uint16_t cur[4] = { g_hw.inputs, g_hw.steer, g_hw.gas, g_hw.brake };
        if (rec_first || memcmp(cur, last, sizeof cur)) {
            fprintf(rec_f, "F %u %04X %03X %03X %03X\n", frame, cur[0], cur[1], cur[2], cur[3]);
            memcpy(last, cur, sizeof cur);
            rec_first = false;
        }
    }
}

/* the menu's Controls page: an action's name, and binding one key to it
 * (replaces the action's keys, keeps its pad button, and saves the line) */
const char *rr_input_action_name(int a) { return a >= 0 && a < RR_ACT_N ? act_name[a] : "?"; }
void rr_input_bind_key(int a, SDL_Scancode sc)
{
    if (a < 0 || a >= RR_ACT_N || sc == SDL_SCANCODE_UNKNOWN) return;
    g_bind[a].keys[0] = sc; g_bind[a].nkeys = 1;
    rr_input_set_option("rr_controls.cfg", act_name[a], SDL_GetScancodeName(sc));
    fprintf(stderr, "[INPUT] %s = %s (saved to rr_controls.cfg)\n", act_name[a], SDL_GetScancodeName(sc));
}
