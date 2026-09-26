/*
 * input.c — SDL2 input into the paths the game actually reads.
 *
 * WHERE THE GAME READS INPUT (traced, not guessed)
 * ------------------------------------------------
 * Buttons and coins arrive through the MCU shared RAM, and the game copies
 * them itself every frame (input_read_service_buttons, game_misc.c):
 *
 *     W[0x2B80] = commsram[0x7D02];  W[0x2B82] = new & ~old   (edge)
 *     W[0x2BA4] = commsram[0x7D04];  W[0x2BA6] = new & ~old   (edge)
 *
 * So writing W[0x2B80] / W[0x2BA4] directly -- which this file used to do --
 * is DEAD: the game overwrites both from commsram on the next frame. Coin
 * insertion never worked for exactly that reason. Everything button-shaped
 * therefore goes into commsram and reaches the game through its own code.
 *
 * The analog values are the opposite case: on hardware the MCU supplies
 * them, and here this file is that source, so those go in directly.
 *
 * They go into _W[] and NOT into work_ram. Writing work_ram cannot express
 * them: the two 16-bit values sit 2 bytes apart, and sync_wram_to_W turns
 * a 4-byte-aligned offset into ONE 32-bit slot -- so a BE16 pair at
 * 0x2BC8/0x2BCA read back as _W[0x2BC8] = 0x01FF01FF (both halves merged)
 * instead of 0x1FF, and the game's `W[0x2BC8] - W[0x3FD0]` deflection was
 * nonsense. The sync layer's PIN mechanism (wsync_pin, game_stubs.c) is
 * what makes the direct write stick: the four slots are registered in
 * input_init() so sync_wram_to_W leaves them alone. Without the pin they
 * are a fixpoint -- the sync runs before anything reads them and resets
 * them to last frame's work_ram bytes.
 *
 * The CENTRES W[0x3FD0]/W[0x3FD2] are set here too. The game's own
 * analog_center_read_from_mcu() is reached only from the stubbed
 * eeprom_settings_init(), and copies BYTE-wise (`W[0x3FD0+i] =
 * commsram[0x7D0A+i]`), which yields 0x01 rather than 0x1FF. Left to
 * itself the centre reads 0 and the bike sits permanently hard over.
 *
 * BIT ASSIGNMENTS, from MAME's INPUT_PORTS_START(propcycl) (namcos22.cpp):
 *     0x0001 COIN1   0x0004 SERVICE1   0x0008 TEST   0x0100 START1
 * The port is IP_ACTIVE_LOW at the connector, but the MCU firmware inverts
 * it before shared RAM -- the game tests `W[0x2B80] & 8` for test mode and
 * computes edges as `new & ~old`, i.e. a press must SET a bit. So the
 * values written here are ACTIVE HIGH. The low byte of INPUTS lands at
 * 0x7D02 and the high byte (START1) at 0x7D04.
 *
 * ANALOG RANGE, from the same port definitions:
 *     ADC.0 handlebar X: centre 0x1FF, range 0x0BF..0x33F, PORT_REVERSE
 *     ADC.1 handlebar Y: centre 0x1FF, range 0x0BF..0x33F
 * The old code used centre 0x200 and range 0x080..0x380; the 0x200 is the
 * documented L3 divergence at WRAM 0x02BC8 (tools/l3/masks/attract.mask),
 * where MAME reports 0x1FF.
 *
 * THE PEDAL IS NOT AN ANALOG AXIS -- it is a PULSE ENCODER, and it is now
 * mapped as one. MAME models it as a 1-bit optical sensor whose PULSE RATE
 * encodes speed (propcycl_state::pedal_update): it clocks MCU timer A3 and
 * the MCU firmware counts interrupts. Writing a level into an ADC channel
 * cannot reproduce that, which is why this was left unmapped for so long.
 *
 * The game-side read HAS now been located. input_process_analog_deltas
 * (game_system.c, M68K 0x0223B8) reads a FREE-RUNNING 16-bit counter from
 * MCU shared RAM at 0x7D1A + axis*2 and takes its per-frame difference into
 * W[0x2BFC + axis*8]. Axis 1 lands in W[0x2C04], which
 * player_update_prev_pos turns straight into thrust:
 *
 *     W[0x0D80] = W[0x2C04] + 1;  W[0x0D80] = W[0x0D80] * 0x4a >> 4;  ...
 *     W[0x0D48] = W[0x0D8C] + ((W[0x0D80] + W[0x0D48]) - W[0x0D84]);
 *
 * So this file's job is to be the MCU: advance that counter at the rate a
 * real pedal would pulse. pedal_counter_update() below uses MAME's own
 * interval curve so the top speed comes out where the hardware's does
 * rather than at an invented constant -- see the note on it.
 */
#include "propcycl.h"
#include "vaddr.h"
#include <SDL2/SDL.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>
#include "ui_menu.h"
#include "pedal_enc.h"        /* an exercise bike's encoder / a Peloton as the pedal */

/* MCU shared RAM, relative to commsram base 0xA04000 */
#define MCU_CMD      (0xBD00 - 0x4000)   /* command from CPU        */
#define MCU_STATUS   (0xBD01 - 0x4000)   /* status to CPU           */
#define MCU_INPUTS_L (0xBD02 - 0x4000)   /* INPUTS low  byte        */
#define MCU_INPUTS_H (0xBD04 - 0x4000)   /* INPUTS high byte        */
#define MCU_ANALOG   (0xBD0A - 0x4000)   /* 8-byte calibration block */
#define MCU_PEDAL    (0xBD1A - 0x4000)   /* 2 free-running pulse counters */

/* INPUTS bits, active HIGH on this side of the MCU (see header) */
#define IN_COIN1    0x0001
#define IN_SERVICE1 0x0004
#define IN_TEST     0x0008
#define IN_START1   0x0100

/* MAME: PORT_BIT(0x3ff, 0x1ff, ...) PORT_MINMAX(0x0bf, 0x33f) */
#define ADC_CENTER  0x1FF
#define ADC_MIN     0x0BF
#define ADC_MAX     0x33F

static int16_t analog_x = ADC_CENTER;
static int16_t analog_y = ADC_CENTER;

/* ---- gamepads ----------------------------------------------------------
 * SDL_GameController maps any pad it recognises (Xbox, DualShock, 8BitDo,
 * ...) onto one virtual layout, so this needs no per-device handling. Pads
 * are opened at init and on hotplug. The stick feeds the SAME analog range
 * as the keyboard (centre 0x1FF, 0x0BF..0x33F) rather than a second path,
 * so remapping and calibration stay in one place. */
#define MAX_PADS 4
static SDL_GameController *pads[MAX_PADS];

/* ---- raw joysticks ----------------------------------------------------
 * Anything SDL does NOT recognise as a gamepad -- a wheel, handlebar rig,
 * arcade stick, flight stick -- is opened as a plain joystick and mapped by
 * axis/button NUMBER from propcycl_controls.cfg:
 *   joy_steer=<n>[ invert]      handlebar left/right
 *   joy_lean=<n>[ invert]       handlebar up/down
 *   joy_pedal=<n>[ invert][ half]  pedal (full -32768..32767 travel, or half = 0..32767)
 *   joy_coin / joy_start / joy_service / joy_test = <button>
 * Find the numbers with `./build/propcycl --joytest`. -1 = unmapped. Extra
 * gamepad layouts load from a gamecontrollerdb.txt beside the binary. */
typedef struct { int axis; int invert, half; } joyaxis_t;
static joyaxis_t joy_steer = { 0, 0, 0 }, joy_lean = { 1, 0, 0 }, joy_pedal = { -1, 0, 0 };
static int joy_btn[ACT_COUNT] = { -1, -1, -1, -1, -1, -1, -1, -1, -1 };
static SDL_Joystick *raws[MAX_PADS];
static SDL_JoystickID pad_id[MAX_PADS], raw_id[MAX_PADS];

static void pad_open_all(void) {
    for (int i = 0; i < SDL_NumJoysticks(); i++) {
        SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(i);
        int slot = -1, open = 0;
        for (int k = 0; k < MAX_PADS; k++) {
            if ((pads[k] && pad_id[k] == id) || (raws[k] && raw_id[k] == id)) open = 1;
            if (!pads[k] && !raws[k] && slot < 0) slot = k;
        }
        if (open || slot < 0) continue;
        if (SDL_IsGameController(i)) {
            if ((pads[slot] = SDL_GameControllerOpen(i))) {
                pad_id[slot] = id;
                printf("  [PAD] %d: %s\n", slot, SDL_GameControllerName(pads[slot]));
            }
        } else if ((raws[slot] = SDL_JoystickOpen(i))) {
            raw_id[slot] = id;
            printf("  [JOY] %d: %s (%d axes, %d buttons) -- mapped by number, see --joytest\n", slot,
                   SDL_JoystickName(raws[slot]), SDL_JoystickNumAxes(raws[slot]), SDL_JoystickNumButtons(raws[slot]));
        }
    }
}

void input_pad_event(const SDL_Event *e) {
    pedal_enc_event(e);          /* encoder A/B buttons or counter axis; no-op unless configured */
    /* JOYDEVICE events fire for gamepads AND raw joysticks, so one pair of
     * cases covers both; a remove closes only the device that left (by
     * instance id -- the old code closed and re-opened every pad). */
    if (e->type == SDL_JOYDEVICEREMOVED) {
        for (int i = 0; i < MAX_PADS; i++) {
            if (pads[i] && pad_id[i] == e->jdevice.which) {
                printf("  [PAD] %d removed\n", i); SDL_GameControllerClose(pads[i]); pads[i] = NULL; }
            if (raws[i] && raw_id[i] == e->jdevice.which) {
                printf("  [JOY] %d removed\n", i); SDL_JoystickClose(raws[i]); raws[i] = NULL; }
        }
    } else if (e->type == SDL_JOYDEVICEADDED) {
        pad_open_all();          /* skips devices already open */
    }
}

static int raw_button(int act) {
    if (joy_btn[act] < 0) return 0;
    for (int i = 0; i < MAX_PADS; i++)
        if (raws[i] && SDL_JoystickGetButton(raws[i], joy_btn[act])) return 1;
    return 0;
}
/* a raw axis as -32767..32767 (steer/lean, small deadzone for wheels) */
static int raw_axis(const joyaxis_t *ax) {
    if (ax->axis < 0) return 0;
    int best = 0;
    for (int i = 0; i < MAX_PADS; i++) {
        if (!raws[i]) continue;
        int v = SDL_JoystickGetAxis(raws[i], ax->axis);
        if (ax->invert) v = -v;
        if (v > -1500 && v < 1500) v = 0;
        if (abs(v) > abs(best)) best = v;
    }
    return best < -32767 ? -32767 : best;
}
/* a raw pedal as 0..32767 */
static int raw_pedal(void) {
    if (joy_pedal.axis < 0) return 0;
    int best = 0;
    for (int i = 0; i < MAX_PADS; i++) {
        if (!raws[i]) continue;
        int r = SDL_JoystickGetAxis(raws[i], joy_pedal.axis);
        double f = joy_pedal.half ? (r < 0 ? 0.0 : r / 32767.0) : (r + 32768) / 65535.0;
        if (joy_pedal.invert) f = 1.0 - f;
        int v = f > 0.03 ? (int)((f - 0.03) / 0.97 * 32767) : 0;
        if (v > best) best = v;
    }
    return best;
}

/* propcycl_controls.cfg hooks (ui_menu.c): 1 if the line was ours */
int input_joy_cfg(const char *key, const char *val) {
    if (pedal_enc_cfg(key, val)) return 1;      /* enc_* : the external pedal */
    joyaxis_t *ax = !strcmp(key, "joy_steer") ? &joy_steer : !strcmp(key, "joy_lean") ? &joy_lean
                  : !strcmp(key, "joy_pedal") ? &joy_pedal : NULL;
    if (ax) {
        ax->axis = atoi(val); ax->invert = strstr(val, "invert") != NULL; ax->half = strstr(val, "half") != NULL;
        return 1;
    }
    static const char *bn[ACT_COUNT] = { "joy_coin", "joy_start", "joy_service", "joy_test" };
    for (int a = 0; a < ACT_COUNT; a++)
        if (bn[a] && !strcmp(key, bn[a])) { joy_btn[a] = atoi(val); return 1; }
    return 0;
}
void input_joy_cfg_save(FILE *f) {
    const joyaxis_t *ax[3] = { &joy_steer, &joy_lean, &joy_pedal };
    const char *nm[3] = { "joy_steer", "joy_lean", "joy_pedal" };
    fprintf(f, "# raw joysticks (wheels/sticks SDL does not know as a gamepad): axis[ invert][ half], -1 = off; see --joytest\n");
    for (int i = 0; i < 3; i++)
        fprintf(f, "%s=%d%s%s\n", nm[i], ax[i]->axis, ax[i]->invert ? " invert" : "", ax[i]->half ? " half" : "");
    fprintf(f, "joy_coin=%d\njoy_start=%d\njoy_service=%d\njoy_test=%d\n",
            joy_btn[ACT_COIN], joy_btn[ACT_START], joy_btn[ACT_SERVICE], joy_btn[ACT_TEST]);
    pedal_enc_cfg_save(f);       /* or Save would drop the enc_* keys */
}

/* --joytest: list every device and print axis/button/hat changes live */
int input_joytest(void) {
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (SDL_Init(SDL_INIT_GAMECONTROLLER) != 0) { fprintf(stderr, "SDL: %s\n", SDL_GetError()); return 1; }
    SDL_GameControllerAddMappingsFromFile("gamecontrollerdb.txt");
    SDL_Delay(200); SDL_PumpEvents();
    printf("%d device(s). Move every axis and press every button; Ctrl-C to stop.\n", SDL_NumJoysticks());
    for (int i = 0; i < SDL_NumJoysticks() && i < 16; i++) {
        SDL_Joystick *j = SDL_JoystickOpen(i);
        printf("  %d: %s -- %s, %d axes, %d buttons, %d hats\n", i, SDL_JoystickName(j),
               SDL_IsGameController(i) ? "GAMEPAD (standard layout, no mapping needed)" : "RAW joystick (map by number)",
               SDL_JoystickNumAxes(j), SDL_JoystickNumButtons(j), SDL_JoystickNumHats(j));
    }
    fflush(stdout);
    SDL_Event e;
    while (SDL_WaitEvent(&e)) {
        if (e.type == SDL_QUIT) break;
        if (e.type == SDL_JOYAXISMOTION && (e.jaxis.value > 4000 || e.jaxis.value < -4000 || (e.jaxis.value > -300 && e.jaxis.value < 300)))
            printf("dev %d  axis %d = %6d\n", e.jaxis.which, e.jaxis.axis, e.jaxis.value);
        if (e.type == SDL_JOYBUTTONDOWN) printf("dev %d  button %d down\n", e.jbutton.which, e.jbutton.button);
        if (e.type == SDL_JOYHATMOTION) printf("dev %d  hat %d = %d\n", e.jhat.which, e.jhat.hat, e.jhat.value);
        fflush(stdout);
    }
    SDL_Quit();
    return 0;
}

/* Any connected pad contributes; first non-neutral wins for the axes. */
static int pad_button(SDL_GameControllerButton b) {
    for (int i = 0; i < MAX_PADS; i++)
        if (pads[i] && SDL_GameControllerGetButton(pads[i], b)) return 1;
    return 0;
}
static int pad_axis(SDL_GameControllerAxis a) {
    for (int i = 0; i < MAX_PADS; i++) {
        if (!pads[i]) continue;
        int v = SDL_GameControllerGetAxis(pads[i], a);
        /* deadzone, then rescale what is left to the full range so the output
         * starts at 0 at the deadzone edge instead of jumping to ~24% */
        if (v > 8000)  return  (int)(((long)(v - 8000) * 32767) / (32767 - 8000));
        if (v < -8000) return -(int)(((long)(-v - 8000) * 32767) / (32768 - 8000));
    }
    return 0;
}

/* A coin switch is momentary. Holding the key must not read as a coin held
 * down forever, and the game's edge detect (new & ~old) needs the bit to
 * fall again before the next coin registers. */
#define COIN_PULSE_FRAMES 4
static int coin_pulse;
static int coin_key_prev;

static void put_be16(uint8_t *p, uint16_t v) { p[0] = v >> 8; p[1] = v & 0xFF; }

/* ---- the pedal ---------------------------------------------------------
 * We are standing in for the MCU, so we publish the same thing it does: a
 * free-running 16-bit pulse counter the game differences each frame.
 *
 * The RATE comes from MAME's model rather than a tuned constant
 * (namcos22.cpp propcycl_state::pedal_update):
 *
 *     interval_usec = 750 + 100000 / |level|        level = 1..127
 *
 * At full level that is 1537us => ~651 pulses/s => ~10.9 per 60Hz frame.
 * Cross-checked against the game's own arithmetic: thrust balances drag at
 * W[0x0D80] == W[0x0D48]^2 >> 22, and the hardware's measured top speed of
 * 13940 (register row 54) needs W[0x0D80] ~ 46, i.e.
 * ((P + 1) * 0x4a >> 4) + 1 == 46 => P ~ 9. Two independent routes to the
 * same ~10 pulses/frame, which is what says the curve is right.
 *
 * LEVEL is how hard the rider is pedalling: it ramps while the key is held
 * and decays when it is not, so releasing coasts to a stop through the
 * game's own drag term instead of stopping dead. */
#define PEDAL_LEVEL_MAX   127
#define PEDAL_RAMP_UP     8      /* per frame while held   */
#define PEDAL_RAMP_DOWN   6      /* per frame while not    */

int g_stagedbg = 0;
int g_lampdbg = 0;   /* PROPCYCL_LAMPLOG=1: the POINT-gauge lamp, per frame (FUN_0000e528) */
int g_fadelog  = 0;   /* PROPCYCL_FADELOG=1 -- the screen-fade chain */
int g_train_y  = (-2147483647-1);  /* PROPCYCL_TRAIN_Y */
int g_aorec;                       /* PROPCYCL_AOREC: animated-object slots 8..14 */
int g_listwho  = 0;   /* PROPCYCL_LISTWHO=<sub> -- who writes the display list */
int g_edgefix  = 1;   /* PROPCYCL_NO_EDGEFIX=1 -- row 149's intersection, for A/B */
int g_menustick = 1;   /* PROPCYCL_NO_MENUSTICK=1 reverts register row 149 */
static int      pedal_level;      /* 0..PEDAL_LEVEL_MAX */
static uint16_t pedal_counter;    /* free-running, exactly like the MCU's */
static uint32_t pedal_frac;       /* 16.16 remainder of a partial pulse  */

/* THE COUNTER MUST ADVANCE ONCE PER SIMULATED FRAME, NOT ONCE PER POLL.
 *
 * `pedal_counter` stands in for the MCU's free-running pulse counter, and the
 * game differences it in `input_process_analog_deltas` -- which runs inside
 * `game_frame()`. `input_poll()` runs unconditionally in the main loop, so
 * whenever `game_frame()` is skipped (the P key holds it, and so does
 * anything else that stops the simulation) the counter kept running with no
 * consumer, and the WHOLE accumulated difference was delivered as one frame's
 * thrust the moment the game resumed.
 *
 * Measured, from a player's own recording (`flight_rec.c`): one frame in 1327
 * reported **31612 pulses** where the per-frame maximum is ~11 -- 2916 frames,
 * 48.6 s, of accumulation. It drove the speed to **151066** against the
 * hardware's measured top of 13940 (register row 54) and moved the bike
 * **11920 units in one frame** where it normally moves 380. Five frames of
 * that carried it ~46000 units -- half a grid cell -- straight through the
 * terrain, and it ended buried at y=40932 with the pitch snapped from -1674
 * to 66963. The player reported this as "hitting a wall in the tunnel"; the
 * tunnel had nothing to do with it.
 *
 * Splitting the two halves fixes it at the source and needs no clamp in the
 * transpiled ROM code: `input_pedal_sample()` tracks the LEVEL every poll (so
 * the ramp still follows the key), and `input_pedal_step()` advances and
 * publishes the counter exactly once per simulated frame, which is what the
 * hardware does -- on the real machine the 68K differences the MCU's counter
 * every frame and never stops. The delta is then bounded by construction.
 *
 * This is also why a teleport is so destructive here: at 11920 units/frame
 * the bike moves further in one step than the collision look-ahead reaches
 * (the escape-probe radius table tops out at 31744, and the per-frame sweep
 * is not tested at all), so it passes through solid terrain without any of
 * the six probes ever seeing it. */
static int      pedal_pressed_now;
static int      pedal_analog_now;

void input_pedal_sample(int pressed, int analog)
{
    pedal_pressed_now = pressed;
    pedal_analog_now  = analog;
}

/* THE TEST SWITCH IS A TOGGLE, like MAME's (PORT_SERVICE = "Service Mode": press once = on, again = off). It used to be "on while F2 is
 * held", which a pad (the Steam Deck) could not do at all. File > Test mode has it too, and File > Service button pulses the service
 * button for a few SIMULATED frames (input_pedal_step runs once per simulated frame, so a pulse never elapses while the menu holds the game). */
static int test_latch, service_pulse;
int  input_test_on(void) { return test_latch; }
void input_set_test(int on) { test_latch = on; fprintf(stderr, "[INPUT] test switch %s\n", on ? "ON" : "OFF"); }
void input_service_pulse(void) { service_pulse = 12; }

void input_pedal_step(void)
{
    if (service_pulse > 0) service_pulse--;
    int analog = pedal_analog_now, pressed = pedal_pressed_now;
    /* An analog trigger sets the level directly; a key ramps it. */
    if (analog > 0) {
        pedal_level = (analog > PEDAL_LEVEL_MAX) ? PEDAL_LEVEL_MAX : analog;
    } else if (pressed) {
        pedal_level += PEDAL_RAMP_UP;
        if (pedal_level > PEDAL_LEVEL_MAX) pedal_level = PEDAL_LEVEL_MAX;
    } else {
        pedal_level -= PEDAL_RAMP_DOWN;
        if (pedal_level < 0) pedal_level = 0;
    }

    if (pedal_level > 0) {
        /* pulses this frame = 1e6 / (750 + 100000/level) / 60, in 16.16 */
        uint32_t interval = 750u + 100000u / (uint32_t)pedal_level;   /* usec */
        uint32_t per_frame_q16 = (uint32_t)((1000000ULL << 16) / (interval * 60ULL));
        pedal_frac += per_frame_q16;
        pedal_counter = (uint16_t)(pedal_counter + (pedal_frac >> 16));
        pedal_frac &= 0xFFFF;
    }

    /* Publish it where input_process_analog_deltas reads it: axis 1, i.e.
     * 0x7D1A + 1*2. Big-endian, because the game reads it with move.w. */
    put_be16(&g_sys.commsram[MCU_PEDAL + 2], pedal_counter);

    { static int dbg = -1;
      if (dbg < 0) { const char *e = getenv("PROPCYCL_PEDALDBG"); dbg = (e && *e != '0'); }
      if (dbg && (g_sys.frame_count % 60) == 0)
          printf("[PEDAL] f%u level=%d ctr=%u W[0x2C04]=%ld W[0x0D80]=%ld W[0x0D48]=%ld enc_rpm=%.1f\n",
                 g_sys.frame_count, pedal_level, pedal_counter,
                 (long)_W[0x2C04], (long)_W[0x0D80], (long)_W[0x0D48], pedal_enc_rpm()); }
}

/* Force the handlebar to a raw ADC pair for one frame -- the headless stand-in
 * for a hand on the bars. Writes BOTH the shared-RAM words the game reads and
 * the W[] slots input.c owns, so it works whether or not the MCU copy runs. */
void input_force_analog(int x, int y)
{
    analog_x = (int16_t)x; analog_y = (int16_t)y;
    put_be16(&g_sys.commsram[MCU_ANALOG + 0], (uint16_t)x);
    put_be16(&g_sys.commsram[MCU_ANALOG + 2], (uint16_t)y);
    _W[0x2BC8] = x; _W[0x2BCA] = y;
}

void input_init(void) {
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) == 0) {
        if (SDL_GameControllerAddMappingsFromFile("gamecontrollerdb.txt") > 0)
            printf("  [PAD] extra pad mappings from gamecontrollerdb.txt\n");
        pad_open_all();
    }

    /* The ADC pair and its calibration centres are produced here and
     * nowhere else, so _W[] must hold them: without the pin, sync_wram_to_W
     * resets them from last frame's work_ram before the game reads them and
     * the handlebar sits dead at whatever it booted with. */
    wsync_pin(0x2BC8, 4);      /* handlebar X, Y */
    wsync_pin(0x3FD0, 4);      /* X centre, Y centre */

    uint8_t *comms = g_sys.commsram;
    comms[MCU_STATUS]   = 0x00;
    comms[MCU_INPUTS_L] = 0x00;
    comms[MCU_INPUTS_H] = 0x00;

    /* Calibration block in MCU shared RAM. The game's own reader is only
     * reached from stubbed init and decodes it byte-wise, so input_poll()
     * also writes W[0x3FD0]/W[0x3FD2] directly; this keeps the shared-RAM
     * copy correct for anything that does read it, and it must match the
     * centre the axes rest at or the bike drifts. */
    /* 0x7D0A and 0x7D0C are the LIVE handlebar channels, not centres: ROM
     * 0x0222C2 copies eight words from here into W[0x2BC8].. and the game
     * reads W[0x2BC8]/W[0x2BCA] as the stick. Publishing the live values
     * here is what lets `read_mcu_inputs_process` derive its own direction
     * flags -- the ones the menu cursors key on -- instead of us synthesising
     * them. The CENTRES are a separate thing and live in W[0x3FD0]/0x3FD2. */
    put_be16(&comms[MCU_ANALOG + 0], (uint16_t)analog_x);   /* X, live */
    put_be16(&comms[MCU_ANALOG + 2], (uint16_t)analog_y);   /* Y, live */
    put_be16(&comms[MCU_ANALOG + 4], ADC_CENTER);
    put_be16(&comms[MCU_ANALOG + 6], ADC_CENTER);
}

/* MAME's own port definition for this game (namcos22.cpp, INPUT_PORTS_START
 * (propcycl)) is the reference and it is in this repo:
 *
 *   PORT_BIT( 0x3ff, 0x1ff, IPT_AD_STICK_X ) PORT_MINMAX(0x0bf, 0x33f)
 *         PORT_SENSITIVITY(100) PORT_KEYDELTA(40) PORT_REVERSE
 *
 * PORT_KEYDELTA is counts PER FRAME, and MAME's PORT_KEYDELTA sets the centre
 * delta with it, so both the push and the spring-back are 40. This was 12 and
 * 6, from a comment reading "PORT_KEYDELTA(40) scaled to 60Hz" -- but keydelta
 * is already per frame, so there was nothing to scale and the stick moved at
 * a THIRD of the machine's rate.
 *
 * It is most visible on the MENUS, which is where a user reported it ("the
 * selection screen tilt is not sensitive enough, I have to really tilt"): the
 * cursor keys off `W[0x2BC8]` crossing `W[0x3FD0] +- 0x80` (ROM 0x0222DA), and
 * 128 counts at 12/frame is ELEVEN FRAMES of holding the key before the menu
 * sees anything, against four on the machine. PROPCYCL_KEYDELTA=<n> overrides
 * for A/B; the scripted-input paths (TEST_STEER, TEST_ANALOG, the autopilot)
 * set the axis directly and never come through here, so no gate moves. */
int g_keydelta = 40;                     /* PORT_KEYDELTA(40) -- MAME's value */

static void axis_update(int16_t *v, int neg, int pos) {
    const int STEP = g_keydelta, RETURN = g_keydelta;
    if (neg && !pos)      *v -= STEP;
    else if (pos && !neg) *v += STEP;
    else {                                /* spring back to centre */
        if (*v < ADC_CENTER) *v += (ADC_CENTER - *v < RETURN) ? (ADC_CENTER - *v) : RETURN;
        if (*v > ADC_CENTER) *v -= (*v - ADC_CENTER < RETURN) ? (*v - ADC_CENTER) : RETURN;
    }
    if (*v < ADC_MIN) *v = ADC_MIN;
    if (*v > ADC_MAX) *v = ADC_MAX;
}

/* Gameplay proper: state 3, sub-state 3. Where the pad's Start means pause. */
int input_in_gameplay(void)
{
    return (int)_W[0x0CBC] == 3 && (int)_W[0x0CC0] == 3;
}

/* PAUSE CAMERA controls (renderer_3d.c pausecam_apply), called once per
 * main-loop iteration while paused. Keyboard: the steer/lean keys (arrows)
 * orbit, +/- (or PageUp/PageDown) zoom. Pad: left stick orbits, right stick
 * Y and the triggers zoom. main.c zeroes all three on unpause, which is the
 * snap back to the game's view. */
extern float g_pausecam_yaw, g_pausecam_pitch, g_pausecam_dist;
void input_pausecam_update(void)
{
    const uint8_t *keys = SDL_GetKeyboardState(NULL);
    /* Rates per SECOND of wall clock: the main loop is paced only by vsync,
     * so a per-iteration step would spin faster on a high-refresh monitor. */
    static uint64_t last;
    uint64_t now = SDL_GetTicks64();
    float dt = last ? (now - last) / 1000.0f : 1.0f / 60.0f;
    last = now;
    if (dt > 0.1f) dt = 1.0f / 60.0f;  /* first call of a new pause, or a stall: one normal step, no jump */
    const float ORBIT = 120.0f * dt;   /* degrees at full input */
    const float DOLLY = 15000.0f * dt; /* view units at full input */
    float yaw = 0, pitch = 0, dolly = 0;
    if (keys[ui_binding[ACT_LEFT]])  yaw   -= 1;
    if (keys[ui_binding[ACT_RIGHT]]) yaw   += 1;
    if (keys[ui_binding[ACT_UP]])    pitch -= 1;     /* up raises the camera, looking down */
    if (keys[ui_binding[ACT_DOWN]])  pitch += 1;
    if (keys[SDL_SCANCODE_EQUALS] || keys[SDL_SCANCODE_KP_PLUS]  || keys[SDL_SCANCODE_PAGEUP])   dolly -= 1;
    if (keys[SDL_SCANCODE_MINUS]  || keys[SDL_SCANCODE_KP_MINUS] || keys[SDL_SCANCODE_PAGEDOWN]) dolly += 1;
    yaw   += pad_axis(SDL_CONTROLLER_AXIS_LEFTX)  / 32767.0f;
    pitch += pad_axis(SDL_CONTROLLER_AXIS_LEFTY)  / 32767.0f;   /* stick up (negative) raises it */
    dolly += pad_axis(SDL_CONTROLLER_AXIS_RIGHTY) / 32767.0f;
    for (int i = 0; i < MAX_PADS; i++) {
        if (!pads[i]) continue;
        int lt = SDL_GameControllerGetAxis(pads[i], SDL_CONTROLLER_AXIS_TRIGGERLEFT);
        int rt = SDL_GameControllerGetAxis(pads[i], SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
        if (lt > 3000) dolly += lt / 32767.0f;     /* LT: back out */
        if (rt > 3000) dolly -= rt / 32767.0f;     /* RT: move in */
    }
    g_pausecam_yaw += yaw * ORBIT;
    if (g_pausecam_yaw >  180.0f) g_pausecam_yaw -= 360.0f;
    if (g_pausecam_yaw < -180.0f) g_pausecam_yaw += 360.0f;
    g_pausecam_pitch += pitch * ORBIT;
    if (g_pausecam_pitch >  85.0f) g_pausecam_pitch =  85.0f;
    if (g_pausecam_pitch < -85.0f) g_pausecam_pitch = -85.0f;
    g_pausecam_dist += dolly * DOLLY;
    if (g_pausecam_dist < -9000.0f)  g_pausecam_dist = -9000.0f;   /* the renderer also clamps to the rider's depth */
    if (g_pausecam_dist >  60000.0f) g_pausecam_dist =  60000.0f;
}

void input_poll(void) {
    /* Serial I/O for an external pedal runs even with the menu open, so the
     * port never backs up and a Peloton keeps being polled. */
    pedal_enc_poll();
    const uint8_t *keys = SDL_GetKeyboardState(NULL);
    /* Menu open: swallow game input so menu typing never reaches the game. */
    if (ui_is_open()) {
        uint8_t *c = g_sys.commsram;
        c[MCU_INPUTS_L] = 0; c[MCU_INPUTS_H] = 0;
        return;
    }

    /* Bindings come from the Controls menu (ui_binding), so a remap takes
     * effect immediately and persists via propcycl_controls.cfg. */
    /* Handlebar. ADC.0 is PORT_REVERSE on hardware, so pressing "right"
     * moves the raw value DOWN. */
    int ax = pad_axis(SDL_CONTROLLER_AXIS_LEFTX);
    int ay = pad_axis(SDL_CONTROLLER_AXIS_LEFTY);
    { int rx = raw_axis(&joy_steer), ry = raw_axis(&joy_lean);
      if (abs(rx) > abs(ax)) ax = rx;
      if (abs(ry) > abs(ay)) ay = ry; }
    /* Each axis on its own: a stick axis outside its deadzone sets that axis
     * absolutely (like the real potentiometer); otherwise the keys drive it
     * and it springs back. Both axes used to share one test, so releasing
     * lean while still steering left lean frozen at its last reading (~24%). */
    int span = (ADC_MAX - ADC_MIN) / 2;
    if (ax) analog_x = (int16_t)(ADC_CENTER - (ax * span) / 32767);
    else    axis_update(&analog_x, keys[ui_binding[ACT_RIGHT]], keys[ui_binding[ACT_LEFT]]);
    if (ay) analog_y = (int16_t)(ADC_CENTER + (ay * span) / 32767);
    else    axis_update(&analog_y, keys[ui_binding[ACT_UP]],    keys[ui_binding[ACT_DOWN]]);
    if (analog_x < ADC_MIN) analog_x = ADC_MIN;
    if (analog_x > ADC_MAX) analog_x = ADC_MAX;
    if (analog_y < ADC_MIN) analog_y = ADC_MIN;
    if (analog_y > ADC_MAX) analog_y = ADC_MAX;

    /* PROPCYCL_TEST_COIN=<frame>: inject one coin at that frame, so the
     * path can be verified headlessly (no keyboard in a screenshot run). */
    /* A comma list (PROPCYCL_TEST_COIN=120,180,240,300) injects one coin at
     * each frame -- coin play (PROPCYCL_COINPLAY=1) needs several coins for a
     * credit, as MAME's pinned NVRAM does. */
    int test_coin = 0;
    { static long at[16]; static int nat = -1;
      if (nat < 0) { const char *e = getenv("PROPCYCL_TEST_COIN"); char *q;
                     nat = 0;
                     while (e && *e && nat < 16) {
                         at[nat++] = strtol(e, &q, 10);
                         if (*q != ',') break;
                         e = q + 1;
                     } }
      for (int i = 0; i < nat; i++)
          if (at[i] >= 0 && (long)g_sys.frame_count == at[i]) test_coin = 1; }

    /* Coin: one pulse per key press, not per frame held. */
    int coin_key = keys[ui_binding[ACT_COIN]] || test_coin ||
                   pad_button(SDL_CONTROLLER_BUTTON_BACK) || raw_button(ACT_COIN);
    if (coin_key && !coin_key_prev) coin_pulse = COIN_PULSE_FRAMES;
    coin_key_prev = coin_key;
    if (coin_pulse > 0) coin_pulse--;

    uint16_t inputs = 0;
    if (coin_pulse > 0)                    inputs |= IN_COIN1;
    if (keys[ui_binding[ACT_SERVICE]] ||
        pad_button(SDL_CONTROLLER_BUTTON_LEFTSHOULDER) ||
        raw_button(ACT_SERVICE) || service_pulse > 0)    inputs |= IN_SERVICE1;
    { static int test_key_prev;                          /* a press flips the Test switch */
      const int test_key = keys[ui_binding[ACT_TEST]] || raw_button(ACT_TEST);
      if (test_key && !test_key_prev) input_set_test(!test_latch);
      test_key_prev = test_key; }
    if (test_latch) inputs |= IN_TEST;
    /* NOT key 1 by default: main.c binds 1-4 to "force stage start", so
     * mapping START1 there too fired both actions from one press. */
    /* PROPCYCL_TEST_START=<frame>: press Start for 4 frames from that frame,
     * the Start-button counterpart of PROPCYCL_TEST_COIN. Without it the
     * coin -> title -> Start -> mode-select path cannot be walked in a
     * headless run at all, so the only thing testable about the menu was
     * how it looks once forced up, not whether it is REACHABLE. */
    int test_start = 0;
    { static long at = -2;
      if (at == -2) { const char *e = getenv("PROPCYCL_TEST_START");
                      at = e ? atol(e) : -1; }
      if (at >= 0 && (long)g_sys.frame_count >= at
                  && (long)g_sys.frame_count < at + 4) test_start = 1; }

    /* Escape -> Levels: START pressed through this same path at the stage
     * select, so the choice goes through the game's own edge detection. */
    { extern int level_select_input_tick(void);
      if (level_select_input_tick()) test_start = 1; }

    /* PROPCYCL_TEST_CONTINUE=1: on the story-mode CONTINUE screen (state 3
     * sub 29) feed a coin and then Start on a 60-frame cycle, the schedule
     * tools/overnight/snap_story.lua (PCS_CONT=1) drives into MAME, so the
     * continue can be TAKEN headlessly and the two runs compared. */
    { static int want = -1;
      if (want < 0) { const char *e = getenv("PROPCYCL_TEST_CONTINUE");
                      want = (e && *e && *e != '0') ? 1 : 0; }
      if (want && (int32_t)vrd32(0xE00000 + 0x0CBC) == 3
               && (int32_t)vrd32(0xE00000 + 0x0CC0) == 29) {
          long ph = (long)g_sys.frame_count % 60;
          if (ph < 6 && coin_pulse == 0) coin_pulse = COIN_PULSE_FRAMES;
          if (ph >= 20 && ph < 30) test_start = 1;
          if (coin_pulse > 0) inputs |= IN_COIN1;
      } }

    /* In gameplay the pad's Start is PAUSE (main.c), so it must not also be
     * the cabinet START there; A still is. */
    if (keys[ui_binding[ACT_START]] || test_start ||
        (pad_button(SDL_CONTROLLER_BUTTON_START) && !input_in_gameplay()) ||
        pad_button(SDL_CONTROLLER_BUTTON_A) ||
        raw_button(ACT_START))                           inputs |= IN_START1;

    /* Buttons go through the MCU shared RAM; the game copies them into
     * W[0x2B80]/W[0x2BA4] itself and derives the edges. */
    uint8_t *comms = g_sys.commsram;
    comms[MCU_INPUTS_L] = (uint8_t)(inputs & 0xFF);
    comms[MCU_INPUTS_H] = (uint8_t)(inputs >> 8);

    /* PROPCYCL_TEST_ANALOG=<x>,<y>: force a raw ADC pair, so the analog
     * path can be verified headlessly the same way PROPCYCL_TEST_COIN
     * verifies the coin path. Values are raw ADC (centre 511). */
    { static int done; static int tx = -1, ty = -1;
      if (!done) { done = 1;
          const char *e = getenv("PROPCYCL_TEST_ANALOG");
          if (e) sscanf(e, "%d,%d", &tx, &ty); }
      /* Gameplay only, for register row 149's reason: a held deflection
       * left running through the now-live menus picks options on the way
       * past. PROPCYCL_TEST_MENUSTICK is the flag for testing the menus. */
      if ((int32_t)_W[0x0CBC] == 3 && (int32_t)_W[0x0CC0] == 3) {
          if (tx >= 0) analog_x = (int16_t)tx;
          if (ty >= 0) analog_y = (int16_t)ty;
      } }

    /* PROPCYCL_TEST_MODE=<0|1>: pick NOVICE (0) or ADVANCED (1) at the MODE
     * SELECT screen, the same way TEST_STAGE picks a course -- by setting the
     * cursor W[0x0C82] during state 3 sub 12/13. ADVANCED leads to sub 22/23,
     * a screen that was unreachable until register row 149 and so had never
     * been looked at; this is how to get to it deterministically. */
    { static int done; static int want = -1;
      if (!done) { done = 1; const char *e = getenv("PROPCYCL_TEST_MODE");
                   if (e) want = atoi(e); }
      if (want >= 0) {
          long st = (long)vrd32(0xE00000 + 0x0CBC), sub = (long)vrd32(0xE00000 + 0x0CC0);
          if (st == 3 && (sub == 12 || sub == 13)) _W[0x0C82] = want;
      } }

    /* PROPCYCL_TEST_PASS=1: CLEAR every stage by holding the score over the
     * stage quota during gameplay -- score 0xE00E4C := quota 0xE00E60 + 500,
     * both 32-bit (`add.l`/`move.l` in the ROM). The same poke
     * tools/overnight/snap_story.lua (PCS_PASS=1) makes into MAME, so a
     * story run reaches the intermissions and the ending the same way on both
     * sides. Written to work RAM as well as _W[]: input_poll runs before the
     * frame's sync_wram_to_W, which would otherwise put the stale value back.
     * PROPCYCL_TEST_TIMER=<frames> also sets the stage timer 0xE00E48 on the
     * first gameplay frame, to shorten a run (MAME ignores the same poke). */
    { static int pass = -1; static long tmr = -2; static int armed;
      if (pass < 0) { const char *e = getenv("PROPCYCL_TEST_PASS");
                      pass = (e && *e && *e != '0') ? 1 : 0;
                      e = getenv("PROPCYCL_TEST_TIMER"); tmr = e ? atol(e) : -1; }
      long st = (long)(int32_t)vrd32(0xE00000 + 0x0CBC), sub = (long)(int32_t)vrd32(0xE00000 + 0x0CC0);
      if (st == 3 && sub == 3) {
          if (pass) {
              int32_t v = (int32_t)vrd32(0xE00000 + 0x0E60); if (v < 0) v = 0; v += 500;
              _W[0x0E4C] = v;
              g_sys.work_ram[0x0E4C] = (uint8_t)(v >> 24); g_sys.work_ram[0x0E4D] = (uint8_t)(v >> 16);
              g_sys.work_ram[0x0E4E] = (uint8_t)(v >> 8);  g_sys.work_ram[0x0E4F] = (uint8_t)v;
          }
          if (tmr >= 0 && !armed) {
              armed = 1; int32_t v = (int32_t)tmr; _W[0x0E48] = v;
              g_sys.work_ram[0x0E48] = (uint8_t)(v >> 24); g_sys.work_ram[0x0E49] = (uint8_t)(v >> 16);
              g_sys.work_ram[0x0E4A] = (uint8_t)(v >> 8);  g_sys.work_ram[0x0E4B] = (uint8_t)v;
          }
      } else armed = 0; }

    /* PROPCYCL_TEST_GOTO=<sub>@<frame>: at that frame, if the game is in
     * state 3, set the gameplay sub-state 0xE00CC0 to <sub> -- a generic jump
     * for reaching a story screen without the screens before it. Whatever
     * those screens would have set up is NOT set up; say so when a result
     * depends on it. */
    { static long at = -2, to = -1;
      if (at == -2) { const char *e = getenv("PROPCYCL_TEST_GOTO");
                      at = -1; if (e) { long s, f; if (sscanf(e, "%ld@%ld", &s, &f) == 2) { to = s; at = f; } } }
      if (at >= 0 && (long)g_sys.frame_count == at && (int32_t)vrd32(0xE00000 + 0x0CBC) == 3) {
          _W[0x0CC0] = to; g_sys.work_ram[0x0CC0] = 0; g_sys.work_ram[0x0CC1] = 0;
          g_sys.work_ram[0x0CC2] = (uint8_t)(to >> 8); g_sys.work_ram[0x0CC3] = (uint8_t)to;
          printf("[TEST] f%ld: sub-state -> %ld\n", at, to);
      } }

    /* PROPCYCL_TEST_ENDING=<frame>: from that frame on, the first gameplay
     * frame (state 3 sub 3) jumps straight to the ENDING (sub 14) with the
     * normal-ending selector 0xE00E18 = 1 and the story flag 0xE00E10 = 1 --
     * how MAME's final stage enters it (snap_story.lua: course 3, sub 3 ->
     * 14 with E18 = 1). Use with --autostart 3 so course 3 is what is
     * loaded. A shortcut for testing the ending, not a path the game has. */
    { static long at = -2; static int done;
      if (at == -2) { const char *e = getenv("PROPCYCL_TEST_ENDING"); at = e ? atol(e) : -1; }
      if (at >= 0 && !done && (long)g_sys.frame_count >= at
          && (int32_t)vrd32(0xE00000 + 0x0CBC) == 3 && (int32_t)vrd32(0xE00000 + 0x0CC0) == 3) {
          done = 1;
          _W[0x0CC0] = 14; g_sys.work_ram[0x0CC0] = 0; g_sys.work_ram[0x0CC1] = 0;
          g_sys.work_ram[0x0CC2] = 0; g_sys.work_ram[0x0CC3] = 14;
          _W[0x0E18] = 1;  g_sys.work_ram[0x0E18] = 0; g_sys.work_ram[0x0E19] = 0;
          g_sys.work_ram[0x0E1A] = 0; g_sys.work_ram[0x0E1B] = 1;
          g_sys.work_ram[0x0E10] = 0; g_sys.work_ram[0x0E11] = 1;   /* 16-bit, high half */
          _W[0x0E10] = (intptr_t)(int32_t)(((uint32_t)g_sys.work_ram[0x0E10] << 24) | ((uint32_t)g_sys.work_ram[0x0E11] << 16)
                     | ((uint32_t)g_sys.work_ram[0x0E12] << 8) | g_sys.work_ram[0x0E13]);
          printf("[TEST] f%ld: jumping to the ending (sub 14)\n", (long)g_sys.frame_count);
      } }

    /* PROPCYCL_TEST_STAGE=<n>: pick course n at the STAGE SELECT screen,
     * headlessly, by setting the menu cursor W[0x0C82] (a pinned 16-bit slot)
     * during state 3 sub 0/1. The course itself still comes from the game's
     * own table lookup at that point (FUN_00008c82).
     *
     * This does NOT go through the stick, deliberately -- it is the direct
     * way to pick a course in a harness, and it stays because it is
     * unambiguous.
     *
     * The note that used to sit here said the stick COULD NOT move the
     * cursor, and named the cause exactly: read_mcu_inputs_process copied
     * the eight ADC words from comms 0x7D0A as BYTES where ROM 0x0222C2 is
     * `move.w (a0,d2.l*2),(a3,d2.l*2)` -- 16-bit, stride 2 -- so W[0x2BC8]
     * held the ADC's high byte when the direction bits were derived. It
     * deferred the repair because it reaches the steering path. **That is
     * now fixed -- register row 149** -- and the stick moves both menus. */
    { static int done; static int want = -1;
      if (!done) { done = 1; const char *e = getenv("PROPCYCL_TEST_STAGE");
                   if (e) want = atoi(e); }
      if (want >= 0) {
          long st = (long)vrd32(0xE00000 + 0x0CBC), sub = (long)vrd32(0xE00000 + 0x0CC0);
          if (st == 3 && (sub == 0 || sub == 1)) _W[0x0C82] = want;
      } }

    /* PROPCYCL_TEST_STEER=<period>,<amp>[,<start>]: a scripted steering
     * SQUARE WAVE on the handlebar axis, identical to the one
     * tools/overnight/dump_gameplay_steer.lua drives into MAME.
     *
     * Row 18 asks how hard the camera trails the bike. camera_update
     * smooths by (hi16(W[0x12C8]) + 0x10), and a first-order follow can
     * only be fitted where the target MOVES -- a steady turn settles to a
     * constant lag and carries no information at all. Hence a square wave:
     * the reversals are the transients the fit reads the divisor out of.
     * The existing dumps/gameplay capture holds the stick at centre, so its
     * camera heading is constant to 0.01 deg over all 320 frames. */
    { static int done; static long per = -1, amp = 0, start = 0;
      if (!done) { done = 1;
          const char *e = getenv("PROPCYCL_TEST_STEER");
          if (e) { long a = 0, b = 0, c = 0;
                   int n = sscanf(e, "%ld,%ld,%ld", &a, &b, &c);
                   if (n >= 2 && a > 1) { per = a; amp = b; start = (n >= 3) ? c : 0; } } }
      /* GAMEPLAY ONLY. Until register row 149 the MODE SELECT and STAGE
       * SELECT cursors could not be moved by the stick at all, so a scripted
       * wave ran harmlessly through the menus. Now that they respond, a wave
       * left running picks options on the way past -- measured: with the
       * steer profile applied during the menus the run never reaches state 3
       * sub 3 at all, and props_gate went INCONCLUSIVE for want of a
       * gameplay frame. The menus want a CENTRED stick in every harness that
       * is not deliberately testing them; PROPCYCL_TEST_MENUSTICK is the one
       * that is. */
      if (per > 1 && (int32_t)_W[0x0CBC] == 3 && (int32_t)_W[0x0CC0] == 3) {
          long f = (long)g_sys.frame_count - start;
          if (f >= 0) {
              long half = per / 2;
              analog_x = (int16_t)(ADC_CENTER + (((f / half) & 1) ? -amp : amp));
              if (analog_x < ADC_MIN) analog_x = ADC_MIN;
              if (analog_x > ADC_MAX) analog_x = ADC_MAX;
          }
      } }

    /* PROPCYCL_TEST_AUTOPILOT=<sx>,<sy>[,<gain>]: fly at the nearest unpopped
     * balloon, headlessly. The scripted square wave never pops one (score
     * stayed 0 over four 3000-frame profiles), and the balloon-pop path is
     * the one the user reported ("pop one balloon -> PERFECT"), so a repro
     * needs an input that actually reaches a balloon. Reads the course's
     * balloon records straight out of the ROM (W[0x16008] + j*0x20, x/y/z
     * BE longs at +4/+8/+c, exactly what balloon_render_and_hit_check
     * feeds balloon_proximity_test) and the popped flags W_A16(0x478C, j -
     * W[0x15FE4]). Heading convention measured off the start of course 0:
     * dx = -sin(h) * speed, dz = cos(h) * speed. sx/sy are the stick signs
     * (+-1) -- try both if the first run turns away. */
    { static int done; static int ap_sx = 0, ap_sy = 0; static double ap_gain = 1.0;
      if (!done) { done = 1; const char *e = getenv("PROPCYCL_TEST_AUTOPILOT");
          if (e) { int n = sscanf(e, "%d,%d,%lf", &ap_sx, &ap_sy, &ap_gain); if (n < 2) ap_sy = 0; } }
      if (ap_sx && _W[0x0CBC] == 3 && _W[0x0CC0] == 3) {
          long px = (long)(int32_t)_W[0x0D00], py = (long)(int32_t)_W[0x0D04], pz = (long)(int32_t)_W[0x0D08];
          uint32_t base = (uint32_t)_W[0x16008]; int first = (int)(int16_t)_W[0x15FE4];
          long best = -1, bx = 0, by = 0, bz = 0; int bj = -1;
          for (int i = 0; i < 64; i++) {
              if (W_A16(0x478C, i) != 0) continue;             /* popped (or counting down) */
              uint32_t rec = base + (uint32_t)(first + i) * 0x20;
              long x = (int32_t)vrd32s(rec + 4), y = (int32_t)vrd32s(rec + 8), z = (int32_t)vrd32s(rec + 12);
              if (x < 0 || x > 3000000 || z < 0 || z > 3000000 || y < -100000 || y > 400000) continue;
              long dx = x - px, dz = z - pz, dy = y - py;
              long d2 = dx * dx + dz * dz + dy * dy;
              if (best < 0 || d2 < best) { best = d2; bx = x; by = y; bz = z; bj = first + i; }
          }
          if (bj >= 0) {
              double h  = (double)(_W[0x0D10] & 0xffff) * (2.0 * M_PI / 65536.0);
              double ht = atan2(-(double)(bx - px), (double)(bz - pz));
              double err = ht - h;
              while (err >  M_PI) err -= 2.0 * M_PI;
              while (err < -M_PI) err += 2.0 * M_PI;
              double sx = err / (M_PI / 8.0) * 320.0 * ap_gain;          /* 22.5 deg saturates */
              double sy = (double)(by - py) / 4000.0 * 320.0 * ap_gain;   /* 4000 units saturates */
              if (sx >  320) sx =  320; if (sx < -320) sx = -320;
              if (sy >  320) sy =  320; if (sy < -320) sy = -320;
              analog_x = (int16_t)(ADC_CENTER + ap_sx * (int)sx);
              analog_y = (int16_t)(ADC_CENTER + ap_sy * (int)sy);
              if (analog_x < ADC_MIN) analog_x = ADC_MIN; if (analog_x > ADC_MAX) analog_x = ADC_MAX;
              if (analog_y < ADC_MIN) analog_y = ADC_MIN; if (analog_y > ADC_MAX) analog_y = ADC_MAX;
              if (g_sys.frame_count % 60 == 0)
                  printf("[AUTO] f%u target j=%d at (%ld,%ld,%ld) dist=%.0f err=%.1fdeg dy=%ld adc=(%d,%d) score=%ld left=%ld\n",
                         g_sys.frame_count, bj, bx, by, bz, sqrt((double)best), err * 180.0 / M_PI, by - py,
                         analog_x, analog_y, (long)_W[0x0E4C], (long)W16(0xE68));
          }
      } }

    /* PEDAL. Keyboard ramps the level; a pad TRIGGER sets it directly, which
     * is the closest thing a controller has to pedalling harder. */
    { int trig = 0;
      for (int i = 0; i < MAX_PADS; i++) {
          if (!pads[i]) continue;
          int v = SDL_GameControllerGetAxis(pads[i], SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
          if (v > trig) trig = v;
      }
      { int rp = raw_pedal(); if (rp > trig) trig = rp; }
      int analog = trig > 3000 ? (trig * PEDAL_LEVEL_MAX) / 32767 : 0;

      /* A real crank: an encoder or a Peloton's cadence, already mapped onto
       * the same 0..127 level (pedal_enc.h). Like a pad trigger it sets the
       * level directly, so while it is turning it takes over from the key ramp
       * (and the higher of trigger and bike wins); at 0 -- idle, unplugged, or
       * not configured -- the keyboard is in charge exactly as before. */
      { int enc = pedal_enc_level(); if (enc > analog) analog = enc; }

      /* PROPCYCL_TEST_PEDAL=<level 1..127>: hold the pedal at a fixed level
       * for headless runs, the same way TEST_COIN and TEST_ANALOG stand in
       * for the keyboard. Read once -- getenv in the frame loop faults
       * (register row 40). */
      { static int done; static int lvl = 0;
        if (!done) { done = 1; const char *e = getenv("PROPCYCL_TEST_PEDAL");
                     if (e) lvl = atoi(e); }
        if (lvl > 0) analog = lvl; }

      input_pedal_sample(keys[ui_binding[ACT_PEDAL]], analog); }

    /* Analog + calibration centres straight into _W[]: see the header. */
    _W[0x2BC8] = analog_x;
    _W[0x2BCA] = analog_y;
    _W[0x3FD0] = ADC_CENTER;
    _W[0x3FD2] = ADC_CENTER;
}
