/* ss22_input.c -- see ss22_input.h. The same code for every Super 22 game; the game's table is an ss22_input_game. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ss22_input.h"
#include "eng_cfg.h"
#include "eng_ui.h"

#define DEADZONE 8000
#define MAX_ACTIONS 16

static const ss22_input_game *game;
static SDL_Scancode bound[MAX_ACTIONS];
static int rebinding = -1;                       /* the action waiting for its key, or -1 */
static bool test_latch, test_prev[MAX_ACTIONS];  /* the Test switch (a toggle) and each action's last state for its press edge */
static int  service_frames;                      /* the Service button pressed from the menu: held this many more frames */

/* ---- the key bindings: one rebindable key and one fixed alternate per action ---------------------------------- */
static void bindings_load(void)
{
    for (int a = 0; a < game->n; a++) {
        bound[a] = game->actions[a].def;
        char k[48]; snprintf(k, sizeof k, "key_%s", game->actions[a].key);
        const char *v = eng_cfg_get(k);
        if (v && *v) { const SDL_Scancode sc = SDL_GetScancodeFromName(v); if (sc != SDL_SCANCODE_UNKNOWN) bound[a] = sc; }
    }
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

static void on_key(SDL_Scancode sc, void *u)
{
    const int a = (int)(intptr_t)u;
    rebinding = -1;
    if (sc != SDL_SCANCODE_UNKNOWN) binding_set(a, sc);
}
/* the page's first rows are the cabinet switches (when the game has them), then "Reset to defaults", then one row per action */
static int  nsw(void) { return (game->test_bit ? 1 : 0) + (game->service_bit ? 1 : 0); }
static int  pg_n(void) { return nsw() + 1 + game->n; }
static bool pg_val(int r) { (void)r; return false; }
static void pg_text(int r, char *l, size_t ln, char *v, size_t vn)
{
    *v = 0;
    if (game->test_bit && r == 0) { snprintf(l, ln, "Test mode"); snprintf(v, vn, "%s", test_latch ? "ON" : "OFF"); return; }
    if (game->service_bit && r == (game->test_bit ? 1 : 0)) { snprintf(l, ln, "Service button"); snprintf(v, vn, "press"); return; }
    r -= nsw();
    if (r == 0) { snprintf(l, ln, "Reset to defaults"); return; }
    const int a = r - 1;
    snprintf(l, ln, "%s", game->actions[a].label);
    const char *k = rebinding == a ? "press a key..." : SDL_GetScancodeName(bound[a]);
    snprintf(v, vn, "%s", (k && *k) ? k : "(none)");
}
static void pg_change(int r, int dir)
{
    if (dir != 0) return;
    if (game->test_bit && r == 0) { test_latch = !test_latch; fprintf(stderr, "[INPUT] test switch %s\n", test_latch ? "ON" : "OFF"); eng_ui_set_open(false); return; }
    if (game->service_bit && r == (game->test_bit ? 1 : 0)) { service_frames = 12; eng_ui_set_open(false); return; }
    r -= nsw();
    if (r == 0) { for (int a = 0; a < game->n; a++) if (bound[a] != game->actions[a].def) binding_set(a, game->actions[a].def); return; }
    rebinding = r - 1;
    eng_ui_capture_key(on_key, (void *)(intptr_t)(r - 1));
}
static void pg_notes(void (*line)(const char *fmt, ...))
{
    for (int i = 0; i < 3; i++) if (game->notes[i]) line("%s", game->notes[i]);
}
static const eng_ui_page controls_page = { "Controls", 400, 140, 22, pg_n, pg_val, NULL, pg_text, pg_change, pg_notes };
const eng_ui_page *ss22_input_page(void) { return &controls_page; }

static SDL_GameController *pads[4];
static unsigned wheel = 0x200, pedal[2];

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

void ss22_input_init(const ss22_input_game *g)
{
    game = g;
    bindings_load();
    SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER);
    pad_scan();
}

void ss22_input_event(const SDL_Event *e)
{
    if (e->type == SDL_CONTROLLERDEVICEADDED || e->type == SDL_CONTROLLERDEVICEREMOVED) pad_scan();
}

static unsigned ramp(unsigned v, unsigned target, unsigned step)
{
    if (v < target) return v + step > target ? target : v + step;
    if (v > target) return v < target + step ? target : v - step;
    return v;
}

void ss22_input_update(void)
{
    const uint8_t *k = SDL_GetKeyboardState(NULL);
    uint16_t p = 0;
    int left = 0, right = 0, pk[2] = { 0, 0 };
    for (int a = 0; a < game->n; a++) {
        const ss22_action *ac = &game->actions[a];
        const bool held = key_held(k, a);
        if (ac->bit && ac->bit == game->test_bit) {          /* the Test switch: a press flips it (the pad buttons of the action too, below) */
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
    const unsigned wt = (unsigned)(0x200 + dir * game->wheel_key_span);
    wheel = ramp(wheel, wt, (unsigned)game->wheel_step);
    for (int i = 0; i < 2; i++) pedal[i] = ramp(pedal[i], pk[i] ? (unsigned)game->pedal_max[i] : 0, (unsigned)game->pedal_step);

    /* a pad out of its deadzone sets the axis directly, and wins over the keys */
    bool stick = false;
    for (int i = 0; i < 4; i++) {
        SDL_GameController *c = pads[i];
        if (!c) continue;
        const int lx = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTX);
        if (lx > DEADZONE || lx < -DEADZONE) {
            /* rescaled to START at the deadzone's edge: the wheel used to jump from centre to a quarter of full lock the moment the stick left it */
            const double t = ((lx < 0 ? -lx : lx) - DEADZONE) / (32767.0 - DEADZONE);
            wheel = (unsigned)(0x200 + (lx < 0 ? -1 : 1) * (int)(t * game->wheel_key_span));
            stick = true;
        }
        const int rt = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERRIGHT), lt = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERLEFT);
        if (rt > 2000) pedal[0] = (unsigned)((double)rt * game->pedal_max[0] / 32767);
        if (lt > 2000) pedal[1] = (unsigned)((double)lt * game->pedal_max[1] / 32767);
        for (int a = 0; a < game->n; a++) {
            const ss22_action *ac = &game->actions[a];
            if (!ac->bit || !ac->pad || ac->bit == game->test_bit) continue;
            for (int b = 0; b < SDL_CONTROLLER_BUTTON_MAX; b++)
                if ((ac->pad >> b & 1u) && SDL_GameControllerGetButton(c, (SDL_GameControllerButton)b)) { p |= ac->bit; break; }
        }
    }
    { static bool stick_drove;                           /* the stick came back into its deadzone: the wheel is centred at once (left to the keys' ramp it crept back at */
      if (stick) stick_drove = true;                     /* PORT_KEYDELTA counts a frame, ~0.35 s, and the car kept steering after the stick was released) */
      else if (stick_drove) { stick_drove = false; if (dir == 0) wheel = 0x200; } }
    if ((int)wheel < game->wheel_min) wheel = (unsigned)game->wheel_min;
    if ((int)wheel > game->wheel_max) wheel = (unsigned)game->wheel_max;
    for (int i = 0; i < 2; i++) if ((int)pedal[i] > game->pedal_max[i]) pedal[i] = (unsigned)game->pedal_max[i];
    if (game->test_bit && test_latch) p |= game->test_bit;
    if (game->service_bit && service_frames > 0) { p |= game->service_bit; service_frames--; }
    game->send(p, wheel, pedal[0], pedal[1]);
}

void ss22_input_neutral(void)
{
    wheel = 0x200; pedal[0] = pedal[1] = 0;
    game->send(game->test_bit && test_latch ? game->test_bit : 0, wheel, pedal[0], pedal[1]);      /* the Test switch stays where it is */
}
