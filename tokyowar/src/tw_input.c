#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tw_input.h"
#include "tw_snd.h"
#include "eng_cfg.h"

#define KEYDELTA 20                              /* PORT_KEYDELTA(20): counts per frame */
#define DEADZONE 8000

/* ---- the key bindings: one rebindable key and one fixed alternate per action ---------------------------------- */
enum { A_COIN, A_START, A_SERVICE, A_TEST, A_LEFT, A_RIGHT, A_FWD, A_BWD, A_LTRIG, A_RTRIG, A_N };
static const char *act_label[A_N] = { "Insert coin", "Start", "Service", "Test", "Wheel left", "Wheel right",
                                      "Forward pedal", "Backward pedal", "Left trigger", "Right trigger" };
static const char *act_key[A_N] = { "coin", "start", "service", "test", "wheel_left", "wheel_right",
                                    "pedal_forward", "pedal_backward", "trigger_left", "trigger_right" };
static const SDL_Scancode def1[A_N] = { SDL_SCANCODE_5, SDL_SCANCODE_RETURN, SDL_SCANCODE_9, SDL_SCANCODE_F2, SDL_SCANCODE_LEFT,
                                        SDL_SCANCODE_RIGHT, SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, SDL_SCANCODE_Z, SDL_SCANCODE_X };
static const SDL_Scancode alt[A_N]  = { SDL_SCANCODE_6, SDL_SCANCODE_KP_ENTER, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_A,
                                        SDL_SCANCODE_D, SDL_SCANCODE_W, SDL_SCANCODE_S, SDL_SCANCODE_LCTRL, SDL_SCANCODE_LALT };
static SDL_Scancode bound[A_N];
static int rebinding = -1;                       /* the action waiting for its key, or -1 */

static void bindings_load(void)
{
    for (int a = 0; a < A_N; a++) {
        bound[a] = def1[a];
        char k[48]; snprintf(k, sizeof k, "key_%s", act_key[a]);
        const char *v = eng_cfg_get(k);
        if (v && *v) { const SDL_Scancode sc = SDL_GetScancodeFromName(v); if (sc != SDL_SCANCODE_UNKNOWN) bound[a] = sc; }
    }
}
static void binding_set(int a, SDL_Scancode sc)
{
    bound[a] = sc;
    char k[48]; snprintf(k, sizeof k, "key_%s", act_key[a]);
    eng_cfg_set(k, SDL_GetScancodeName(sc));
    fprintf(stderr, "[INPUT] %s = %s (saved)\n", act_label[a], SDL_GetScancodeName(sc));
}
static bool key_held(const uint8_t *k, int a) { return k[bound[a]] || (alt[a] != SDL_SCANCODE_UNKNOWN && k[alt[a]]); }

static void on_key(SDL_Scancode sc, void *u)
{
    const int a = (int)(intptr_t)u;
    rebinding = -1;
    if (sc != SDL_SCANCODE_UNKNOWN) binding_set(a, sc);
}
static int  pg_n(void) { return 1 + A_N; }
static bool pg_val(int r) { (void)r; return false; }
static void pg_text(int r, char *l, size_t ln, char *v, size_t vn)
{
    if (r == 0) { snprintf(l, ln, "Reset to defaults"); *v = 0; return; }
    const int a = r - 1;
    snprintf(l, ln, "%s", act_label[a]);
    const char *k = rebinding == a ? "press a key..." : SDL_GetScancodeName(bound[a]);
    snprintf(v, vn, "%s", (k && *k) ? k : "(none)");
}
static void pg_change(int r, int dir)
{
    if (dir != 0) return;
    if (r == 0) { for (int a = 0; a < A_N; a++) if (bound[a] != def1[a]) binding_set(a, def1[a]); return; }
    rebinding = r - 1;
    eng_ui_capture_key(on_key, (void *)(intptr_t)(r - 1));
}
static void pg_notes(void (*line)(const char *fmt, ...))
{
    line("Enter, then press the new key (Esc cancels)");
    line("Alternates: 6, keypad Enter, A D W S, Ctrl, Alt");
    line("Pad: stick steers, triggers = pedals");
}
static const eng_ui_page controls_page = { "Controls", 400, 140, 22, pg_n, pg_val, NULL, pg_text, pg_change, pg_notes };
const eng_ui_page *tw_input_page(void) { return &controls_page; }

static SDL_GameController *pads[4];
static unsigned wheel = 0x200, fwd, bwd;

static void pad_scan(void)
{
    for (int i = 0; i < 4; i++) if (pads[i] && !SDL_GameControllerGetAttached(pads[i])) { SDL_GameControllerClose(pads[i]); pads[i] = NULL; }
    for (int j = 0, n = SDL_NumJoysticks(); j < n; j++) {
        if (!SDL_IsGameController(j)) continue;
        SDL_GameController *c = SDL_GameControllerOpen(j);
        if (!c) continue;
        int have = 0;
        for (int i = 0; i < 4; i++) if (pads[i] == c) have = 1;
        if (have) continue;
        for (int i = 0; i < 4; i++) if (!pads[i]) { pads[i] = c; fprintf(stderr, "[INPUT] pad: %s\n", SDL_GameControllerName(c)); break; }
    }
}

void tw_input_init(void)
{
    bindings_load();
    SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER);
    pad_scan();
}

void tw_input_event(const SDL_Event *e)
{
    if (e->type == SDL_CONTROLLERDEVICEADDED || e->type == SDL_CONTROLLERDEVICEREMOVED) pad_scan();
}

static unsigned ramp(unsigned v, unsigned target, unsigned step)
{
    if (v < target) return v + step > target ? target : v + step;
    if (v > target) return v < target + step ? target : v - step;
    return v;
}

void tw_input_update(void)
{
    const uint8_t *k = SDL_GetKeyboardState(NULL);
    uint16_t p = 0;
    if (key_held(k, A_COIN)) p |= TW_IN_COIN;
    if (key_held(k, A_SERVICE)) p |= TW_IN_SERVICE;
    if (key_held(k, A_TEST)) p |= TW_IN_TEST;
    if (key_held(k, A_START)) p |= TW_IN_START;
    if (key_held(k, A_LTRIG)) p |= TW_IN_LTRIG;
    if (key_held(k, A_RTRIG)) p |= TW_IN_RTRIG;

    int dir = (key_held(k, A_RIGHT) ? 1 : 0) - (key_held(k, A_LEFT) ? 1 : 0);
    unsigned wt = 0x200 + dir * 0x100, ft = key_held(k, A_FWD) ? 0x100 : 0, bt = key_held(k, A_BWD) ? 0x100 : 0;
    wheel = ramp(wheel, wt, KEYDELTA); fwd = ramp(fwd, ft, KEYDELTA); bwd = ramp(bwd, bt, KEYDELTA);

    /* a pad out of its deadzone sets the axis directly, and wins over the keys */
    for (int i = 0; i < 4; i++) {
        SDL_GameController *c = pads[i];
        if (!c) continue;
        const int lx = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTX);
        if (lx > DEADZONE || lx < -DEADZONE) wheel = (unsigned)(0x200 + (int)((double)lx * 0x100 / 32767));
        const int rt = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERRIGHT), lt = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERLEFT);
        if (rt > 2000) fwd = (unsigned)((double)rt * 0x100 / 32767);
        if (lt > 2000) bwd = (unsigned)((double)lt * 0x100 / 32767);
        if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_START) || SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_A) || SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_Y)) p |= TW_IN_START;
        if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_BACK)) p |= TW_IN_COIN;
        if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_X) || SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_LEFTSHOULDER)) p |= TW_IN_LTRIG;
        if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_B) || SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) p |= TW_IN_RTRIG;
    }
    if (wheel < 0x100) wheel = 0x100;
    if (wheel > 0x300) wheel = 0x300;
    if (fwd > 0x100) fwd = 0x100;
    if (bwd > 0x100) bwd = 0x100;
    tw_snd_inputs(p, wheel, fwd, bwd);
}

void tw_input_neutral(void)
{
    wheel = 0x200; fwd = 0; bwd = 0;
    tw_snd_inputs(0, wheel, fwd, bwd);
}
