/* ss22_input.c -- see ss22_input.h. Shared keyboard, pad and raw wheel/joystick input for Super System 22 games. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "ss22_input.h"
#include "eng_cfg.h"
#include "eng_ffb.h"
#include "eng_ui.h"
#include "ss22_board.h"

#define PAD_DEADZONE   4000     /* of 32767: an Xbox pad rests near 3000. 8000 left a quarter of the stick dead, and with the curve below the steering
                                * all came in the stick's outer half -- easing off a turn dropped it back towards the centre */
#define STICK_RATE     0x50     /* the most the stick moves the wheel in a frame (centre to full lock ~0.1 s): a stick flicked or let go
                                * jumped the wheel the whole way in one frame, and the car jerked */
#define WHEEL_DEADZONE 32      /* of 32767 (0.1% of the lock): a wheel's sensor noise. 1000 left +-4 degrees of the cabinet's 270 with no
                                * steering and no centring, the spring catching the wheel at its edge -- a notch at the centre in play */
#define MAX_ACTIONS    16
#define MAX_DEV        8
#define MAX_CAP_AXES   16

static const ss22_input_game *game;
static SDL_Scancode bound[MAX_ACTIONS];
static int rebinding = -1;                       /* action waiting for a key, button or axis, or -1 */
/* The stick's travel past the deadzone, t in 0..1, steers t^curve of full lock. A thumbstick's short throw driving the wheel linearly
 * was too sensitive to steer with ("controls too sensitive" on a pad); a curve keeps the centre fine and still reaches full lock. */
static const struct { const char *name; double curve; } steer_levels[] = {
    { "Linear", 1.0 }, { "Medium", 1.5 }, { "Smooth", 2.0 }, { "Very smooth", 2.5 } };
#define STEER_N ((int)(sizeof steer_levels / sizeof *steer_levels))
#define STEER_DEFAULT 1
static int steer_level = STEER_DEFAULT;
static bool test_latch, test_prev[MAX_ACTIONS];
static int service_frames;

/* ---- raw wheels / joysticks -------------------------------------------------
 * SDL game controllers keep their standard layout. Everything else (wheels,
 * pedal sets, arcade controls) is opened as SDL_Joystick.
 *
 * Axis settings use the same syntax as Rave Racer:
 *   joy_steer = <axis>[ invert]
 *   joy_gas   = <axis>[ invert][ positive|negative]
 *   joy_brake = <axis>[ invert][ positive|negative]
 *
 * A Controls-menu bind records the SDL GUID too, so a separate USB pedal set
 * is not confused with the wheel base, and the device's axis and button counts
 * (joy_<axis>_shape = A<axes>B<buttons>): one wheel can be several devices
 * under one GUID -- a Fanatec DD base is two "FANATEC Wheel"s (12 axes / 63
 * buttons and 8 axes / 108 buttons), and the first match need not be the one bound. "positive"/"negative" is learnt from
 * the direction in which the pedal moved while binding.
 */
typedef struct {
    int axis;
    bool invert;
    int direction;                              /* 0 auto, +1 pressed raises value, -1 lowers it */
    char guid[40];                              /* SDL GUID string; empty = first raw device */
    int shape_axes, shape_buttons;              /* the bound device's axis and button counts (0 = not recorded: the first GUID match) */
} raw_axis_bind;

typedef struct {
    SDL_Joystick *js;
    SDL_JoystickID id;
    char guid[40];
    char name[96];
} raw_dev;

typedef struct {
    bool rest_valid;
    int rest;
    int learned_direction;
} pedal_cal;

static raw_axis_bind joy_steer = { 0, false, 0, "" };
static raw_axis_bind joy_gas   = { -1, false, 0, "" };
static raw_axis_bind joy_brake = { -1, false, 0, "" };
static int joy_button[MAX_ACTIONS];
static char joy_button_guid[MAX_ACTIONS][40];
static raw_dev raws[MAX_DEV];
static pedal_cal gas_cal, brake_cal;

/* axis capture in the Controls menu: 0 steer, 1 gas, 2 brake */
static int axis_capture = -1;
static int cap_base[MAX_DEV][MAX_CAP_AXES];
static bool cap_valid[MAX_DEV][MAX_CAP_AXES];

static const char *axis_cfg_name(int k) { return k == 0 ? "joy_steer" : k == 1 ? "joy_gas" : "joy_brake"; }
static const char *guid_cfg_name(int k) { return k == 0 ? "joy_steer_guid" : k == 1 ? "joy_gas_guid" : "joy_brake_guid"; }
static const char *shape_cfg_name(int k) { return k == 0 ? "joy_steer_shape" : k == 1 ? "joy_gas_shape" : "joy_brake_shape"; }
static raw_axis_bind *axis_bind(int k) { return k == 0 ? &joy_steer : k == 1 ? &joy_gas : &joy_brake; }

static void parse_axis(raw_axis_bind *b, const char *key, int def_axis)
{
    const char *v = eng_cfg_get(key);
    b->axis = v && *v ? atoi(v) : def_axis;
    b->invert = v && strstr(v, "invert") != NULL;
    b->direction = v && strstr(v, "positive") ? +1 : v && strstr(v, "negative") ? -1 : 0;
}

static void raw_bindings_load(void)
{
    parse_axis(&joy_steer, "joy_steer", 0);
    parse_axis(&joy_gas, "joy_gas", -1);
    parse_axis(&joy_brake, "joy_brake", -1);
    for (int k = 0; k < 3; k++) {
        raw_axis_bind *b = axis_bind(k);
        const char *g = eng_cfg_get(guid_cfg_name(k));
        snprintf(b->guid, sizeof b->guid, "%s", g ? g : "");
        const char *sh = eng_cfg_get(shape_cfg_name(k));
        if (!sh || sscanf(sh, "A%dB%d", &b->shape_axes, &b->shape_buttons) != 2) b->shape_axes = b->shape_buttons = 0;
    }
    for (int a = 0; a < MAX_ACTIONS; a++) joy_button[a] = -1;
    for (int a = 0; a < game->n && a < MAX_ACTIONS; a++) {
        char k[64]; snprintf(k, sizeof k, "joy_button_%s", game->actions[a].key);
        joy_button[a] = eng_cfg_int(k, -1);
        /* Legacy button keys overlap joy_gas/joy_brake axis settings. */
        if (!eng_cfg_get(k) && game->actions[a].axis == SS22_AX_NONE) {
            snprintf(k, sizeof k, "joy_%s", game->actions[a].key);
            joy_button[a] = eng_cfg_int(k, -1);
        }
        snprintf(k, sizeof k, "joy_button_%s_guid", game->actions[a].key);
        const char *guid = eng_cfg_get(k);
        snprintf(joy_button_guid[a], sizeof joy_button_guid[a], "%s", guid ? guid : "");
    }
}

static void raw_bind_save(int kind)
{
    raw_axis_bind *b = axis_bind(kind);
    char v[64];
    snprintf(v, sizeof v, "%d%s%s", b->axis, b->invert ? " invert" : "",
             b->direction > 0 ? " positive" : b->direction < 0 ? " negative" : "");
    eng_cfg_set(axis_cfg_name(kind), v);
    eng_cfg_set(guid_cfg_name(kind), b->guid);
    snprintf(v, sizeof v, "A%dB%d", b->shape_axes, b->shape_buttons);
    eng_cfg_set(shape_cfg_name(kind), v);
}

/* ---- keyboard bindings --------------------------------------------------- */
static void bindings_load(void)
{
    for (int a = 0; a < game->n; a++) {
        bound[a] = game->actions[a].def;
        char k[48]; snprintf(k, sizeof k, "key_%s", game->actions[a].key);
        const char *v = eng_cfg_get(k);
        if (v && *v) {
            const SDL_Scancode sc = SDL_GetScancodeFromName(v);
            if (sc != SDL_SCANCODE_UNKNOWN) bound[a] = sc;
        }
    }
    steer_level = eng_cfg_int("stick_steering", STEER_DEFAULT);
    if (steer_level < 0 || steer_level >= STEER_N) steer_level = STEER_DEFAULT;
}

static void steer_set(int level)
{
    steer_level = level;
    eng_cfg_set_int("stick_steering", level);
    fprintf(stderr, "[INPUT] stick steering = %s (saved)\n", steer_levels[level].name);
}

static void binding_set(int a, SDL_Scancode sc)
{
    bound[a] = sc;
    char k[48]; snprintf(k, sizeof k, "key_%s", game->actions[a].key);
    eng_cfg_set(k, SDL_GetScancodeName(sc));
    fprintf(stderr, "[INPUT] %s = %s (saved)\n", game->actions[a].label, SDL_GetScancodeName(sc));
}

static bool key_held(const uint8_t *k, int a)
{
    const SDL_Scancode alt = game->actions[a].alt;
    return k[bound[a]] || (alt != SDL_SCANCODE_UNKNOWN && k[alt]);
}

/* ---- the wheel motor (force feedback) -------------------------------------
 * The cabinet's Motor/Feedback PCB turns each UART0 byte of the MCU into a
 * steering torque (ss22_input_motor), played on the device the steering axis
 * is bound to (engine/eng_ffb.c). Settings: ffb_strength 0-100 (0 = off) and
 * ffb_invert for a wheel whose driver pushes the other way.
 *
 * With the game's torque word (ss22_input_game.torque) the force is taken from
 * the sum the 68K builds instead of the byte: the byte is that sum clamped,
 * cut to 64 steps and dithered by a 4-frame pattern, which the cabinet's motor
 * smoothed away but a modern wheel plays as a notch where the sign flips at the
 * centre. The terms also split into centring and road, each with its own gain
 * (ffb_centering / ffb_road, 0-200 %). */
static int ffb_strength = 100, ffb_centre = 100, ffb_road = 100;
static bool ffb_invert;
static double force;                             /* the last command, -1..1: negative pushes toward the higher A-D side */

/* the torque word's writes this frame: the running sum, term by term */
static int tq_sum[8], tq_n, tq_centre, tq_roadv;
static uint32_t tq_frame, tq_seen = ~0u;
extern uint32_t rr_frame;
static void torque_write(uint32_t v, int size)
{
    if (size != 2) return;
    if (rr_frame != tq_frame || tq_n >= (int)(sizeof tq_sum / sizeof *tq_sum)) { tq_frame = rr_frame; tq_n = 0; }
    tq_sum[tq_n++] = (int16_t)v;
    if (tq_n == game->torque.parts) {            /* the whole sum: split it into its terms */
        tq_centre = 0;
        for (int i = 0; i < tq_n; i++)
            if (game->torque.centre >> i & 1) tq_centre += tq_sum[i] - (i ? tq_sum[i - 1] : 0);
        tq_roadv = tq_sum[tq_n - 1] - tq_centre;
    } else { tq_centre = tq_sum[tq_n - 1]; tq_roadv = 0; }      /* a one-write path (the game's own centring mode) */
    tq_seen = rr_frame;
}

void ss22_input_close(void) { eng_ffb_close(); }

/* ---- controller / raw-device discovery ---------------------------------- */
typedef struct { SDL_GameController *gc; SDL_JoystickID id; } pad_dev;
static pad_dev pads[MAX_DEV];

/* a short kick on every connected game pad (the gun's recoil solenoid; a Steam Deck's own motors included). A pad without rumble ignores it. */
void ss22_input_rumble(uint16_t low, uint16_t high, uint32_t ms)
{
    for (int i = 0; i < MAX_DEV; i++) if (pads[i].gc) SDL_GameControllerRumble(pads[i].gc, low, high, ms);
}

static int pad_find(SDL_JoystickID id)
{
    for (int i = 0; i < MAX_DEV; i++) if (pads[i].gc && pads[i].id == id) return i;
    return -1;
}
static int raw_find(SDL_JoystickID id)
{
    for (int i = 0; i < MAX_DEV; i++) if (raws[i].js && raws[i].id == id) return i;
    return -1;
}

static void pad_scan(void)
{
    for (int i = 0; i < MAX_DEV; i++)
        if (pads[i].gc && !SDL_GameControllerGetAttached(pads[i].gc)) {
            SDL_GameControllerClose(pads[i].gc); pads[i].gc = NULL; pads[i].id = -1;
        }

    for (int j = 0, n = SDL_NumJoysticks(); j < n; j++) {
        if (!SDL_IsGameController(j)) continue;
        const SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(j);
        if (pad_find(id) >= 0) continue;
        int slot = -1;
        for (int i = 0; i < MAX_DEV; i++) if (!pads[i].gc) { slot = i; break; }
        if (slot < 0) break;
        SDL_GameController *c = SDL_GameControllerOpen(j);
        if (!c) continue;
        pads[slot].gc = c; pads[slot].id = id;
        fprintf(stderr, "[INPUT] gamepad %d: %s\n", slot, SDL_GameControllerName(c));
    }
}

static void raw_scan(void)
{
    for (int i = 0; i < MAX_DEV; i++)
        if (raws[i].js && !SDL_JoystickGetAttached(raws[i].js)) {
            eng_ffb_forget(raws[i].id);                                  /* the haptic side goes before its joystick */
            SDL_JoystickClose(raws[i].js); memset(&raws[i], 0, sizeof raws[i]); raws[i].id = -1;
        }

    for (int j = 0, n = SDL_NumJoysticks(); j < n; j++) {
        if (SDL_IsGameController(j)) continue;
        const SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(j);
        if (raw_find(id) >= 0) continue;
        int slot = -1;
        for (int i = 0; i < MAX_DEV; i++) if (!raws[i].js) { slot = i; break; }
        if (slot < 0) break;
        SDL_Joystick *js = SDL_JoystickOpen(j);
        if (!js) continue;
        raws[slot].js = js; raws[slot].id = id;
        const SDL_JoystickGUID guid = SDL_JoystickGetGUID(js);
        SDL_JoystickGetGUIDString(guid, raws[slot].guid, (int)sizeof raws[slot].guid);
        snprintf(raws[slot].name, sizeof raws[slot].name, "%s", SDL_JoystickName(js) ? SDL_JoystickName(js) : "joystick");
        fprintf(stderr, "[INPUT] raw %d: %s [%s], %d axes, %d buttons\n", slot, raws[slot].name, raws[slot].guid,
                SDL_JoystickNumAxes(js), SDL_JoystickNumButtons(js));
    }
}

static int raw_slot_for(const raw_axis_bind *b)
{
    if (b->guid[0]) {
        for (int i = 0; i < MAX_DEV; i++)
            if (raws[i].js && !strcmp(raws[i].guid, b->guid) &&
                (!b->shape_axes || (SDL_JoystickNumAxes(raws[i].js) == b->shape_axes &&
                                    SDL_JoystickNumButtons(raws[i].js) == b->shape_buttons))) return i;
        return -1;
    }
    for (int i = 0; i < MAX_DEV; i++) if (raws[i].js) return i;
    return -1;
}

/* the steering device's haptic side (engine/eng_ffb.c opens it once per device) */
static bool ffb_device(void)
{
    /* the candidates: the devices sharing the steering binding's GUID -- the one the axis is read from first, then those SDL
     * calls haptic, then the rest. A Fanatec DD base is two "FANATEC Wheel"s under one GUID and the motor need not sit on the
     * steering axis' one; eng_ffb_device_from() opens the first that takes a force */
    SDL_Joystick *cand[MAX_DEV];
    int n = 0;
    const int first = joy_steer.axis >= 0 ? raw_slot_for(&joy_steer) : -1;
    for (int pass = 0; pass < 3 && first >= 0; pass++)
        for (int i = 0; i < MAX_DEV; i++) {
            if (!raws[i].js || (joy_steer.guid[0] && strcmp(raws[i].guid, joy_steer.guid))) continue;
            const bool capable = eng_ffb_capable(raws[i].js);
            if (pass == 0 ? i == first : pass == 1 ? i != first && capable : i != first && !capable) cand[n++] = raws[i].js;
        }
    return eng_ffb_device_from(cand, n);
}

static void ffb_apply(void)
{
    if (game->wheel_motor && ffb_device()) eng_ffb_force_f(force, ffb_strength, joy_steer.invert != ffb_invert);
}

void ss22_input_motor(uint8_t b)
{
    if (!game || !game->wheel_motor) return;
    /* the torque sum while the game builds it (this frame or the last); the byte when it does not (menus, attract) */
    if (game->torque.addr && tq_seen != ~0u && rr_frame - tq_seen <= 1) {
        double t = (tq_centre * ffb_centre + tq_roadv * ffb_road) / 100.0;
        if (t > game->torque.limit) t = game->torque.limit;
        if (t < -game->torque.limit) t = -game->torque.limit;
        force = t / game->torque.limit;
    } else force = eng_ffb_decode(b) / 63.0;
    ffb_apply();
}

/* Explicit button bindings also accept standard game controllers. */
static SDL_Joystick *button_device(SDL_JoystickID id)
{
    int d = raw_find(id);
    if (d >= 0) return raws[d].js;
    d = pad_find(id);
    return d >= 0 ? SDL_GameControllerGetJoystick(pads[d].gc) : NULL;
}

static bool button_matches(SDL_Joystick *js, int a)
{
    if (!js || joy_button[a] >= SDL_JoystickNumButtons(js)) return false;
    if (joy_button_guid[a][0]) {
        char guid[40];
        SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(js), guid, sizeof guid);
        if (strcmp(guid, joy_button_guid[a])) return false;
    }
    return SDL_JoystickGetButton(js, joy_button[a]) != 0;
}

static bool raw_button_held(int a)
{
    if (a < 0 || a >= game->n || joy_button[a] < 0) return false;
    for (int i = 0; i < MAX_DEV; i++) {
        if (button_matches(raws[i].js, a)) return true;
        if (pads[i].gc && button_matches(SDL_GameControllerGetJoystick(pads[i].gc), a)) return true;
    }
    return false;
}

/* ---- Controls menu ------------------------------------------------------- */
static int action_axis(int a)
{
    switch (game->actions[a].axis) {
    case SS22_AX_WHEEL_LEFT: case SS22_AX_WHEEL_RIGHT: return 0;
    case SS22_AX_PEDAL1: return 1;
    case SS22_AX_PEDAL2: return 2;
    default: return -1;
    }
}
static bool capture_input(const SDL_Event *e, void *u);

static void axis_capture_begin(int kind)
{
    axis_capture = kind;
    memset(cap_valid, 0, sizeof cap_valid);
    for (int d = 0; d < MAX_DEV; d++) if (raws[d].js) {
        const int na = SDL_JoystickNumAxes(raws[d].js) < MAX_CAP_AXES ? SDL_JoystickNumAxes(raws[d].js) : MAX_CAP_AXES;
        for (int a = 0; a < na; a++) {
            cap_base[d][a] = SDL_JoystickGetAxis(raws[d].js, a);
            cap_valid[d][a] = true;
        }
    }
}

static void axis_label(int kind, char *v, size_t vn)
{
    if (axis_capture == kind) {
        snprintf(v, vn, "%s", kind == 0 ? "move wheel..." : kind == 1 ? "press gas..." : "press brake...");
        return;
    }
    const raw_axis_bind *b = axis_bind(kind);
    if (b->axis < 0) { snprintf(v, vn, "(not bound)"); return; }
    snprintf(v, vn, "axis %d%s%s", b->axis, b->invert ? " (inv)" : "",
             raw_slot_for(b) < 0 && b->guid[0] ? " (disconnected)" : "");
}

static int nsw(void) { return (game->test_bit ? 1 : 0) + (game->service_bit ? 1 : 0); }
static int nffb(void) { return game->wheel_motor ? (game->torque.addr ? 4 : 2) : 0; }   /* Force feedback, FFB direction[, FFB centering, FFB road effects] */
static int pg_n(void) { return nsw() + nffb() + 2 + game->n; }    /* the switches, the FFB rows, Reset, Stick steering, the actions */
static bool pg_val(int r) { return (r >= nsw() && r < nsw() + nffb()) || r == nsw() + nffb() + 1; }

static void pg_text(int r, char *l, size_t ln, char *v, size_t vn)
{
    *v = 0;
    if (game->test_bit && r == 0) { snprintf(l, ln, "Test mode"); snprintf(v, vn, "%s", test_latch ? "ON" : "OFF"); return; }
    if (game->service_bit && r == (game->test_bit ? 1 : 0)) { snprintf(l, ln, "Service button"); snprintf(v, vn, "press"); return; }
    r -= nsw();
    if (r == 0 && nffb()) {
        snprintf(l, ln, "Force feedback");
        if (ffb_strength) snprintf(v, vn, "%d%%%s", ffb_strength, ffb_device() ? "" : " (no FFB wheel bound)");
        else snprintf(v, vn, "OFF");
        return;
    }
    if (r == 1 && nffb()) { snprintf(l, ln, "FFB direction"); snprintf(v, vn, "%s", ffb_invert ? "reversed" : "normal"); return; }
    if (r == 2 && nffb() > 2) { snprintf(l, ln, "FFB centering"); snprintf(v, vn, "%d%%", ffb_centre); return; }
    if (r == 3 && nffb() > 2) { snprintf(l, ln, "FFB road effects"); snprintf(v, vn, "%d%%", ffb_road); return; }
    r -= nffb();
    if (r == 0) { snprintf(l, ln, "Reset keyboard defaults"); return; }
    if (r == 1) { snprintf(l, ln, "Stick steering"); snprintf(v, vn, "%s", steer_levels[steer_level].name); return; }
    r -= 2;
    const int a = r;
    snprintf(l, ln, "%s", game->actions[a].label);
    if (rebinding == a) {
        snprintf(v, vn, "%s", action_axis(a) >= 0 ? "key, button or move control..." : "press key or button...");
        return;
    }
    const char *key = SDL_GetScancodeName(bound[a]);
    snprintf(v, vn, "%s", key && *key ? key : "(none)");
    if (joy_button[a] >= 0) {
        size_t used = strlen(v);
        snprintf(v + used, vn - used, " / button %d", joy_button[a]);
    }
    const int kind = action_axis(a);
    if (kind >= 0 && axis_bind(kind)->axis >= 0) {
        char label[128];
        axis_label(kind, label, sizeof label);
        size_t used = strlen(v);
        snprintf(v + used, vn - used, " / %s", label);
    }
}

static void pg_change(int r, int dir)
{
    if (r == nsw() + nffb() + 1) {                       /* Stick steering: left/right step through the levels, a press moves on (wrapping) */
        const int lv = dir < 0 ? steer_level - 1 : dir > 0 ? steer_level + 1 : (steer_level + 1) % STEER_N;
        if (lv >= 0 && lv < STEER_N) steer_set(lv);
        return;
    }
    if (dir != 0 && !pg_val(r)) return;
    if (game->test_bit && r == 0) { test_latch = !test_latch; fprintf(stderr, "[INPUT] test switch %s\n", test_latch ? "ON" : "OFF"); eng_ui_set_open(false); return; }
    if (game->service_bit && r == (game->test_bit ? 1 : 0)) { service_frames = 12; eng_ui_set_open(false); return; }
    r -= nsw();
    if (r == 0 && nffb()) {                                        /* Left/Right 10% steps, Enter cycles */
        ffb_strength = dir ? ffb_strength + 10 * dir : (ffb_strength + 10) % 110;
        if (ffb_strength < 0) ffb_strength = 0;
        if (ffb_strength > 100) ffb_strength = 100;
        eng_cfg_set_int("ffb_strength", ffb_strength);
        return;
    }
    if (r == 1 && nffb()) { ffb_invert = !ffb_invert; eng_cfg_set_int("ffb_invert", ffb_invert); return; }
    if ((r == 2 || r == 3) && nffb() > 2) {                      /* 0-200 %: Left/Right 10% steps, Enter cycles */
        int *g = r == 2 ? &ffb_centre : &ffb_road;
        *g = dir ? *g + 10 * dir : (*g + 10) % 210;
        if (*g < 0) *g = 0;
        if (*g > 200) *g = 200;
        eng_cfg_set_int(r == 2 ? "ffb_centering" : "ffb_road", *g);
        return;
    }
    r -= nffb();
    if (r == 0) {
        for (int a = 0; a < game->n; a++) if (bound[a] != game->actions[a].def) binding_set(a, game->actions[a].def);
        if (steer_level != STEER_DEFAULT) steer_set(STEER_DEFAULT);
        return;
    }
    r -= 2;
    rebinding = r;
    axis_capture_begin(action_axis(r));
    eng_ui_capture_input(capture_input, NULL);
}

static void pg_notes(void (*line)(const char *fmt, ...))
{
    line("Select an action, then press a key/button or move its control.");
    line("Esc cancels. Wheel left/right share one axis binding.");
    line("Pedal direction is learned automatically and saved.");
    if (game->wheel_motor) line("Force feedback: the cabinet's wheel motor, on the bound steering wheel.");
    for (int i = 1; i < 3; i++) if (game->notes[i]) line("%s", game->notes[i]);
}

static const eng_ui_page controls_page = { "Controls", 520, 170, 22, pg_n, pg_val, NULL, pg_text, pg_change, pg_notes };
const eng_ui_page *ss22_input_page(void) { return &controls_page; }

/* ---- input lifecycle ----------------------------------------------------- */
static unsigned wheel = 0x200, pedal[2];

void ss22_input_init(const ss22_input_game *g)
{
    game = g;
    bindings_load();
    raw_bindings_load();
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER);
    if (g->wheel_motor) {
        ffb_strength = eng_cfg_int("ffb_strength", 100);
        if (ffb_strength < 0) ffb_strength = 0;
        if (ffb_strength > 100) ffb_strength = 100;
        ffb_invert = eng_cfg_int("ffb_invert", 0) != 0;
        ffb_centre = eng_cfg_int("ffb_centering", 100);
        ffb_road = eng_cfg_int("ffb_road", 100);
        if (ffb_centre < 0) ffb_centre = 0;
        if (ffb_centre > 200) ffb_centre = 200;
        if (ffb_road < 0) ffb_road = 0;
        if (ffb_road > 200) ffb_road = 200;
        if (g->torque.addr && g->torque.parts > 0) {          /* tap the torque sum as the 68K writes it */
            g_ss22_wram_watch_off = g->torque.addr - 0xE00000u;
            g_ss22_wram_watch = torque_write;
        }
    }
    SDL_GameControllerAddMappingsFromFile("gamecontrollerdb.txt");
    pad_scan();
    raw_scan();
}

void ss22_input_event(const SDL_Event *e)
{
    if (e->type == SDL_JOYDEVICEADDED || e->type == SDL_CONTROLLERDEVICEADDED) {
        pad_scan(); raw_scan();
    } else if (e->type == SDL_JOYDEVICEREMOVED || e->type == SDL_CONTROLLERDEVICEREMOVED) {
        pad_scan(); raw_scan();
        memset(&gas_cal, 0, sizeof gas_cal); memset(&brake_cal, 0, sizeof brake_cal);
    }

}

/* UI capture consumes the binding event before it can navigate the menu. */
static bool capture_input(const SDL_Event *e, void *u)
{
    (void)u;
    if (!e || (e->type == SDL_KEYDOWN && e->key.keysym.scancode == SDL_SCANCODE_ESCAPE)) {
        rebinding = axis_capture = -1;
        return true;
    }
    if (e->type == SDL_KEYDOWN && !e->key.repeat) {
        binding_set(rebinding, e->key.keysym.scancode);
        rebinding = axis_capture = -1;
        return true;
    }
    if (e->type == SDL_JOYBUTTONDOWN) {
        SDL_Joystick *js = button_device(e->jbutton.which);
        if (!js) return false;
        joy_button[rebinding] = e->jbutton.button;
        SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(js), joy_button_guid[rebinding],
                                 sizeof joy_button_guid[rebinding]);
        char key[64];
        snprintf(key, sizeof key, "joy_button_%s", game->actions[rebinding].key);
        eng_cfg_set_int(key, joy_button[rebinding]);
        snprintf(key, sizeof key, "joy_button_%s_guid", game->actions[rebinding].key);
        eng_cfg_set(key, joy_button_guid[rebinding]);
        rebinding = axis_capture = -1;
        return true;
    }
    if (axis_capture < 0 || e->type != SDL_JOYAXISMOTION) return false;
    const int d = raw_find(e->jaxis.which);
    const int a = e->jaxis.axis;
    if (d < 0 || a < 0 || a >= MAX_CAP_AXES || !cap_valid[d][a]) return false;
    const int delta = (int)e->jaxis.value - cap_base[d][a];
    if (abs(delta) < 6000) return false;

    raw_axis_bind *b = axis_bind(axis_capture);
    b->axis = a;
    b->invert = axis_capture == 0 &&
        ((game->actions[rebinding].axis == SS22_AX_WHEEL_LEFT && delta > 0) ||
         (game->actions[rebinding].axis == SS22_AX_WHEEL_RIGHT && delta < 0));
    b->direction = axis_capture == 0 ? 0 : (delta > 0 ? +1 : -1);
    snprintf(b->guid, sizeof b->guid, "%s", raws[d].guid);
    b->shape_axes = SDL_JoystickNumAxes(raws[d].js);
    b->shape_buttons = SDL_JoystickNumButtons(raws[d].js);
    raw_bind_save(axis_capture);
    if (axis_capture == 1) memset(&gas_cal, 0, sizeof gas_cal);
    if (axis_capture == 2) memset(&brake_cal, 0, sizeof brake_cal);
    fprintf(stderr, "[INPUT] %s bound to %s axis %d%s\n",
            axis_capture == 0 ? "wheel" : axis_capture == 1 ? "gas" : "brake",
            raws[d].name, a, b->direction > 0 ? " (+)" : b->direction < 0 ? " (-)" : "");
    rebinding = axis_capture = -1;
    return true;
}

static unsigned ramp(unsigned v, unsigned target, unsigned step)
{
    if (v < target) return v + step > target ? target : v + step;
    if (v > target) return v < target + step ? target : v - step;
    return v;
}

static bool raw_wheel_value(int *out)
{
    const int d = raw_slot_for(&joy_steer);
    if (d < 0 || joy_steer.axis < 0 || joy_steer.axis >= SDL_JoystickNumAxes(raws[d].js)) return false;
    int v = SDL_JoystickGetAxis(raws[d].js, joy_steer.axis);
    if (joy_steer.invert) v = -v;
    if (v > -WHEEL_DEADZONE && v < WHEEL_DEADZONE) v = 0;
    else {
        const int s = v < 0 ? -1 : 1;
        const int m = abs(v) - WHEEL_DEADZONE;
        v = s * (int)((long long)m * 32767 / (32767 - WHEEL_DEADZONE));
    }
    if (v < -32767) v = -32767;
    if (v >  32767) v =  32767;
    *out = v;
    return true;
}

static bool raw_pedal_value(raw_axis_bind *b, pedal_cal *cal, int *out)
{
    const int d = raw_slot_for(b);
    if (d < 0 || b->axis < 0 || b->axis >= SDL_JoystickNumAxes(raws[d].js)) return false;
    int r = SDL_JoystickGetAxis(raws[d].js, b->axis);
    if (b->invert) r = -r;

    if (!cal->rest_valid) {
        cal->rest = r;
        cal->rest_valid = true;
        cal->learned_direction = b->direction;
        if (!cal->learned_direction) {
            if (r > 12000) cal->learned_direction = -1;
            else if (r < -12000) cal->learned_direction = +1;
        }
    }
    if (!cal->learned_direction) {
        const int delta = r - cal->rest;
        if (delta > 4000) cal->learned_direction = +1;
        else if (delta < -4000) cal->learned_direction = -1;
        else { *out = 0; return true; }
    }

    const int dir = cal->learned_direction;
    const int delta = dir > 0 ? r - cal->rest : cal->rest - r;
    const int range = dir > 0 ? 32767 - cal->rest : cal->rest + 32768;
    double f = range > 0 ? (double)delta / range : 0.0;
    if (f < 0.02) f = 0.0;
    else f = (f - 0.02) / 0.98;
    if (f < 0.0) f = 0.0;
    if (f > 1.0) f = 1.0;
    *out = (int)(f * 32767.0 + 0.5);
    return true;
}

/* ---- the light gun: where it points, from the mouse, the right stick or the arrow keys ------------------------------------------ */
#include "ss22_board.h"
#include "ss22_host.h"
static float aim_x = 0.5f, aim_y = 0.5f;
static bool  aim_on;                                    /* the crosshair is on the picture */
static int   aim_src;                                   /* 0 none yet, 1 mouse, 2 stick / keys */
bool ss22_input_aim(float *nx, float *ny) { if (!game || !game->light_gun || !aim_on) return false; *nx = aim_x; *ny = aim_y; return true; }
static float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
static void aim_update(const uint8_t *k)
{
    static int last_mx = -1, last_my = -1;
    int mx, my; const uint32_t mb_now = SDL_GetMouseState(&mx, &my);
    float px, py; bool inside;
    const bool have = ss22_host_pointer(&px, &py, &inside);
    if (have && (mx != last_mx || my != last_my)) { aim_src = 1; last_mx = mx; last_my = my; }
    float dx = 0, dy = 0;                               /* the stick and the arrow keys move the crosshair (a full deflection crosses the picture in ~1 s) */
    for (int i = 0; i < MAX_DEV; i++) {
        SDL_GameController *c = pads[i].gc; if (!c) continue;
        const int rx = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_RIGHTX), ry = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_RIGHTY);
        if (abs(rx) > PAD_DEADZONE) dx += rx / 32767.0f;
        if (abs(ry) > PAD_DEADZONE) dy += ry / 32767.0f;
    }
    if (k[SDL_SCANCODE_LEFT])  dx -= 1; if (k[SDL_SCANCODE_RIGHT]) dx += 1;
    if (k[SDL_SCANCODE_UP])    dy -= 1; if (k[SDL_SCANCODE_DOWN])  dy += 1;
    if (dx != 0 || dy != 0) {
        if (aim_src == 1 && have) { aim_x = clamp01(px); aim_y = clamp01(py); }   /* start from where the mouse was */
        aim_src = 2; aim_on = true;
        aim_x = clamp01(aim_x + dx * 0.018f); aim_y = clamp01(aim_y + dy * 0.024f);
    } else if (aim_src == 1) {
        aim_on = have && inside;                        /* the pointer left the picture: the gun is off-screen (a reload shot) */
        if (have) { aim_x = clamp01(px); aim_y = clamp01(py); }
    } else if (aim_src == 0) aim_on = false;
    if ((mb_now & SDL_BUTTON_X1MASK) || k[SDL_SCANCODE_R]) aim_on = false;     /* an "aim off-screen" button / key (a reload without moving the gun) */
    g_ss22_gun_off = !aim_on;
    g_ss22_gun_x = (uint16_t)(68 + aim_x * 626);        /* the cabinet's port ranges (engine/ss22_board.h) */
    g_ss22_gun_y = (uint16_t)(43 + aim_y * 241);
    { static int dbg = -1, n; if (dbg < 0) dbg = getenv("SS22_AIMDBG") != NULL;       /* SS22_AIMDBG=1: the aim, once a second */
      if (dbg && ++n % 60 == 0) fprintf(stderr, "[AIM] src %d on %d  norm %.3f,%.3f  port %u,%u  buttons 0x%X\n", aim_src, aim_on, aim_x, aim_y, g_ss22_gun_x, g_ss22_gun_y, SDL_GetMouseState(NULL, NULL)); } 
}

void ss22_input_update(void)
{
    const uint8_t *k = SDL_GetKeyboardState(NULL);
    uint16_t p = 0;
    int left = 0, right = 0, pk[2] = { 0, 0 };
    const uint32_t mb = game->light_gun ? SDL_GetMouseState(NULL, NULL) : 0;
    if (game->light_gun) aim_update(k);

    for (int a = 0; a < game->n; a++) {
        const ss22_action *ac = &game->actions[a];
        const bool held = key_held(k, a) || raw_button_held(a) || (ac->mouse && (mb & ac->mouse));
        if (ac->bit && ac->bit == game->test_bit) {
            if (held && !test_prev[a]) { test_latch = !test_latch; fprintf(stderr, "[INPUT] test switch %s\n", test_latch ? "ON" : "OFF"); }
            test_prev[a] = held;
        } else if (ac->bit && held) p |= ac->bit;
        if (held) switch (ac->axis) {
            case SS22_AX_WHEEL_LEFT: left = 1; break;
            case SS22_AX_WHEEL_RIGHT: right = 1; break;
            case SS22_AX_PEDAL1: pk[0] = 1; break;
            case SS22_AX_PEDAL2: pk[1] = 1; break;
            default: break;
        }
    }

    const int dir = right - left;
    const unsigned centre = (unsigned)((game->wheel_min + game->wheel_max) / 2);
    const unsigned wt = (unsigned)((int)centre + dir * game->wheel_key_span);
    for (int i = 0; i < 2; i++)
        pedal[i] = ramp(pedal[i], pk[i] ? (unsigned)game->pedal_max[i] : 0, (unsigned)game->pedal_step);

    /* Standard gamepads: a stick out of its deadzone steers, and wins over the keys. */
    bool stick = false;
    unsigned stick_wheel = centre;
    for (int i = 0; i < MAX_DEV; i++) {
        SDL_GameController *c = pads[i].gc;
        if (!c) continue;
        const int lx = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTX);
        if (lx > PAD_DEADZONE || lx < -PAD_DEADZONE) {
            const double t = pow(((lx < 0 ? -lx : lx) - PAD_DEADZONE) / (32767.0 - PAD_DEADZONE), steer_levels[steer_level].curve);
            stick_wheel = (unsigned)((int)centre + (lx < 0 ? -1 : 1) * (int)(t * game->wheel_key_span));
            stick = true;
        }
        const int rt = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
        const int lt = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERLEFT);
        if (rt > 2000) {
            const unsigned v = (unsigned)((double)rt * game->pedal_max[0] / 32767);
            if (v > pedal[0]) pedal[0] = v;
        }
        if (lt > 2000) {
            const unsigned v = (unsigned)((double)lt * game->pedal_max[1] / 32767);
            if (v > pedal[1]) pedal[1] = v;
        }
        for (int a = 0; a < game->n; a++) {
            const ss22_action *ac = &game->actions[a];
            if (!ac->bit || !ac->pad || ac->bit == game->test_bit) continue;
            for (int b = 0; b < SDL_CONTROLLER_BUTTON_MAX; b++)
                if ((ac->pad >> b & 1u) && SDL_GameControllerGetButton(c, (SDL_GameControllerButton)b)) { p |= ac->bit; break; }
        }
    }

    { static bool stick_drove;
      if (stick) { stick_drove = true; wheel = ramp(wheel, stick_wheel, STICK_RATE); }
      else if (stick_drove && dir == 0) {                /* let go: back to centre at the stick's pace, not the keys' (PORT_KEYDELTA counts a */
          wheel = ramp(wheel, centre, STICK_RATE);       /* frame, ~0.35 s: the car kept steering after the stick was released) */
          if (wheel == centre) stick_drove = false;
      } else { stick_drove = false; wheel = ramp(wheel, wt, (unsigned)game->wheel_step); } }

    /* A raw wheel is absolute while it is moving; when centred, keyboard/pad remain usable. */
    { static bool raw_drove;
      int rw;
      if (raw_wheel_value(&rw)) {
          if (rw != 0) {
              const int negspan = (int)centre - game->wheel_min;
              const int posspan = game->wheel_max - (int)centre;
              int w = (int)centre + (rw < 0 ? (int)((long long)rw * negspan / 32767)
                                            : (int)((long long)rw * posspan / 32767));
              if (w < game->wheel_min) w = game->wheel_min;
              if (w > game->wheel_max) w = game->wheel_max;
              wheel = (unsigned)w;
              raw_drove = true;
          } else if (raw_drove) {
              wheel = centre;
              raw_drove = false;
          }
      } else raw_drove = false;
    }

    int rp;
    if (raw_pedal_value(&joy_gas, &gas_cal, &rp)) {
        const unsigned v = (unsigned)((long long)rp * game->pedal_max[0] / 32767);
        if (v > pedal[0]) pedal[0] = v;
    }
    if (raw_pedal_value(&joy_brake, &brake_cal, &rp)) {
        const unsigned v = (unsigned)((long long)rp * game->pedal_max[1] / 32767);
        if (v > pedal[1]) pedal[1] = v;
    }

    if ((int)wheel < game->wheel_min) wheel = (unsigned)game->wheel_min;
    if ((int)wheel > game->wheel_max) wheel = (unsigned)game->wheel_max;
    for (int i = 0; i < 2; i++) if ((int)pedal[i] > game->pedal_max[i]) pedal[i] = (unsigned)game->pedal_max[i];
    if (game->test_bit && test_latch) p |= game->test_bit;
    if (game->service_bit && service_frames > 0) { p |= game->service_bit; service_frames--; }
    game->send(p, wheel, pedal[0], pedal[1]);
}

void ss22_input_neutral(void)
{
    const unsigned centre = (unsigned)((game->wheel_min + game->wheel_max) / 2);
    wheel = centre; pedal[0] = pedal[1] = 0;
    force = 0; ffb_apply();                                  /* the next byte from the game restores the force */
    game->send(game->test_bit && test_latch ? game->test_bit : 0, wheel, pedal[0], pedal[1]);
}
