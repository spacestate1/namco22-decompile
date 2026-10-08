/*
 * s21_input.c -- Cyber Sled's cabinet on a keyboard and SDL game controllers: two sticks (the sled is driven tank-style, left and
 * right stick forward/back), Gun, Missile, Viewport, coin, start, the Test switch (a TOGGLE, like MAME's PORT_SERVICE) and the
 * Service button. Keys are MAME's defaults for cybsled (left stick E/D and S/F, right stick I/K and J/L) plus arrows/WASD-free
 * extras; a held key ramps the stick 16 counts a frame (PORT_KEYDELTA 16) and it springs back when released.
 * TODO (HARD RULE 3): rebinding rows like engine/ss22_input.c -- that layer is wheel/pedal-shaped; generalising it to N analog
 * axes is the shared fix (work/proposed_patches note), not a copy here.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "s21_host.h"
#include "s21_board.h"
#include "s21_io.h"

static SDL_GameController *pads[4];
static int test_on, test_prev;
static int axis[4] = { 0x7F, 0x7F, 0x7F, 0x7F };       /* an0 right Y, an1 left Y, an2 right X, an3 left X */

/* CS_FAKEPAD=1 (TEST): an SDL virtual game controller (Xbox 360 ids) attached at start and driven by a fixed script, so both analog
 * sticks reach the levers through the same SDL game-controller path a real pad uses (the Linux driver is the one part it skips).
 * Script, in frames: Back (coin) 240, Start 330, A x4 from 420 (picks Training), then from 2000: both sticks up 180 f, left down +
 * right up 120 f, both left 120 f, both half up 120 f, centre, D-pad up 120 f. Read the result with CS_INLOG=1. */
static int fake_idx = -1;
static SDL_Joystick *fake_js;
static void fakepad_init(void)
{
    if (!getenv("CS_FAKEPAD")) return;
#if SDL_VERSION_ATLEAST(2, 24, 0)                        /* SDL_JoystickAttachVirtualEx: SDL 2.24 (Ubuntu 22.04 has 2.0.20) */
    SDL_VirtualJoystickDesc d; SDL_zero(d);
    d.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION; d.type = SDL_JOYSTICK_TYPE_GAMECONTROLLER;
    d.naxes = SDL_CONTROLLER_AXIS_MAX; d.nbuttons = SDL_CONTROLLER_BUTTON_MAX; d.vendor_id = 0x045E; d.product_id = 0x028E;
    d.name = "Virtual X-Box 360 pad (CS_FAKEPAD)";
    fake_idx = SDL_JoystickAttachVirtualEx(&d);
    if (fake_idx >= 0) fake_js = SDL_JoystickOpen(fake_idx);
    fprintf(stderr, "[INPUT] CS_FAKEPAD: virtual pad %s\n", fake_js ? "attached" : SDL_GetError());
#else
    fprintf(stderr, "[INPUT] CS_FAKEPAD: needs SDL 2.24 or later (this build: %d.%d.%d)\n", SDL_MAJOR_VERSION, SDL_MINOR_VERSION, SDL_PATCHLEVEL);
#endif
}
static void fakepad_frame(void)
{
    if (!fake_js) return;
#if SDL_VERSION_ATLEAST(2, 24, 0)
    static int f; f++;
    int lx = 0, ly = 0, rx = 0, ry = 0, hat_up = 0;
    const int t = f - 2000;
    if (t >= 0 && t < 180) { ly = -32767; ry = -32767; }
    else if (t >= 180 && t < 300) { ly = 32767; ry = -32767; }
    else if (t >= 300 && t < 420) { lx = -32767; rx = -32767; }
    else if (t >= 420 && t < 540) { ly = -16000; ry = -16000; }
    else if (t >= 600 && t < 720) hat_up = 1;
    SDL_JoystickSetVirtualAxis(fake_js, SDL_CONTROLLER_AXIS_LEFTX, (Sint16)lx); SDL_JoystickSetVirtualAxis(fake_js, SDL_CONTROLLER_AXIS_LEFTY, (Sint16)ly);
    SDL_JoystickSetVirtualAxis(fake_js, SDL_CONTROLLER_AXIS_RIGHTX, (Sint16)rx); SDL_JoystickSetVirtualAxis(fake_js, SDL_CONTROLLER_AXIS_RIGHTY, (Sint16)ry);
    SDL_JoystickSetVirtualButton(fake_js, SDL_CONTROLLER_BUTTON_DPAD_UP, (Uint8)hat_up);
    SDL_JoystickSetVirtualButton(fake_js, SDL_CONTROLLER_BUTTON_BACK, (Uint8)(f >= 240 && f < 246));
    SDL_JoystickSetVirtualButton(fake_js, SDL_CONTROLLER_BUTTON_START, (Uint8)(f >= 330 && f < 336));
    SDL_JoystickSetVirtualButton(fake_js, SDL_CONTROLLER_BUTTON_A, (Uint8)(f >= 420 && f < 700 && (f - 420) % 72 < 6));
#endif
}
void s21_input_init(void) { SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER); fakepad_init(); }

void s21_input_event(const SDL_Event *e)
{
    if (e->type == SDL_CONTROLLERDEVICEADDED) {
        for (int i = 0; i < 4; i++) if (!pads[i]) { pads[i] = SDL_GameControllerOpen(e->cdevice.which); if (pads[i]) fprintf(stderr, "[INPUT] pad: %s\n", SDL_GameControllerName(pads[i])); break; }
    } else if (e->type == SDL_CONTROLLERDEVICEREMOVED) {
        for (int i = 0; i < 4; i++) if (pads[i] && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pads[i])) == e->cdevice.which) { SDL_GameControllerClose(pads[i]); pads[i] = NULL; }
    }
}

static int pad_btn(SDL_GameControllerButton b) { for (int i = 0; i < 4; i++) if (pads[i] && SDL_GameControllerGetButton(pads[i], b)) return 1; return 0; }
static int pad_axis(SDL_GameControllerAxis a)          /* the strongest deflection over the pads, -32768..32767 */
{
    int best = 0;
    for (int i = 0; i < 4; i++) if (pads[i]) { int v = SDL_GameControllerGetAxis(pads[i], a); if ((v < 0 ? -v : v) > (best < 0 ? -best : best)) best = v; }
    return best;
}

static void ramp(int *v, int dec, int inc)
{
    const int lo = 0x3F, hi = 0xBF, c = 0x7F;
    if (dec && !inc) *v = *v - 16 < lo ? lo : *v - 16;
    else if (inc && !dec) *v = *v + 16 > hi ? hi : *v + 16;
    else if (*v < c) *v = *v + 16 > c ? c : *v + 16;     /* springs back */
    else if (*v > c) *v = *v - 16 < c ? c : *v - 16;
}
static int stick(int keyv, SDL_GameControllerAxis a, int invert)
{
    int p = pad_axis(a);
    if (p > -6000 && p < 6000) return keyv;
    if (invert) p = -p;
    int v = 0x7F + p * 0x40 / 32768;
    return v < 0x3F ? 0x3F : v > 0xBF ? 0xBF : v;
}

static s21_inputs cur = { .mcub = 0xFF, .mcuc = 0xFF, .mcuh = 0xFF, .dsw = 0xDF, .an = { 0x7F, 0x7F, 0x7F, 0x7F, 0xFF, 0xFF, 0xFF, 0xFF }, .dial = { 0xFF, 0xFF, 0xFF, 0xFF } };

/* move an axis toward a target at the keyboard's 16 counts a frame (MAME's PORT_KEYDELTA(16) for these ports) */
static void toward(int *v, int t) { if (*v < t) *v = *v + 16 > t ? t : *v + 16; else if (*v > t) *v = *v - 16 < t ? t : *v - 16; }

void s21_input_update(void)
{
    fakepad_frame();
    SDL_GameControllerUpdate();
    const Uint8 *k = SDL_GetKeyboardState(NULL);
    /* ARROWS DRIVE THE SLED: Cyber Sled is steered like a tank with two levers (AN1 left, AN0 right; a LOWER value is forward --
     * measured: both held at 0x3F drive up to the wall ahead, both at 0xBF back away). Up/Down = both levers forward/back, Left/Right
     * = the levers opposite (turn on the spot; with Up/Down, a curve), Shift + Left/Right = both levers sideways (strafe). The pad's
     * D-pad does the same. E/D/S/F and I/K/J/L still work each lever on its own, as on MAME's default keys. */
    const int up = k[SDL_SCANCODE_UP] || pad_btn(SDL_CONTROLLER_BUTTON_DPAD_UP), dn = k[SDL_SCANCODE_DOWN] || pad_btn(SDL_CONTROLLER_BUTTON_DPAD_DOWN);
    const int lt = k[SDL_SCANCODE_LEFT] || pad_btn(SDL_CONTROLLER_BUTTON_DPAD_LEFT), rt = k[SDL_SCANCODE_RIGHT] || pad_btn(SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
    const int shift = k[SDL_SCANCODE_LSHIFT] || k[SDL_SCANCODE_RSHIFT];
    const int arrows = up || dn || lt || rt;
    if (arrows) {
        /* The menus (course / vehicle / pilot select) read only the levers' SIDEWAYS movement, a battle reads both: master work
         * RAM byte 0x100907 is 0 on the title, attract, course select, round and tutorial screens and 3 while the sled is
         * controllable (measured over a full coin -> Training run, cybsled/PLAN.md). Outside a battle Left/Right move both levers
         * sideways; in a battle they turn (and Shift + Left/Right strafes). */
        const int battle = ((g_s21.mram[0x907 >> 1] & 0xFF) != 0);       /* odd byte = the low half of the big-endian word */
        const int fwd = up - dn, turn = (shift || !battle) ? 0 : lt - rt, side = (shift || !battle) ? rt - lt : 0;
        int l = fwd - turn, r = fwd + turn;                  /* +1 forward .. -1 back, per lever */
        l = l > 1 ? 1 : l < -1 ? -1 : l; r = r > 1 ? 1 : r < -1 ? -1 : r;
        toward(&axis[1], 0x7F - 0x40 * l); toward(&axis[0], 0x7F - 0x40 * r);
        toward(&axis[3], 0x7F + 0x40 * side); toward(&axis[2], 0x7F + 0x40 * side);
    } else {
        ramp(&axis[1], k[SDL_SCANCODE_E], k[SDL_SCANCODE_D]);   /* left lever Y */
        ramp(&axis[3], k[SDL_SCANCODE_S], k[SDL_SCANCODE_F]);   /* left lever X */
        ramp(&axis[0], k[SDL_SCANCODE_I], k[SDL_SCANCODE_K]);   /* right lever Y */
        ramp(&axis[2], k[SDL_SCANCODE_J], k[SDL_SCANCODE_L]);   /* right lever X */
    }
    s21_inputs in = cur;
    in.an[1] = (uint8_t)stick(axis[1], SDL_CONTROLLER_AXIS_LEFTY, 0);
    in.an[3] = (uint8_t)stick(axis[3], SDL_CONTROLLER_AXIS_LEFTX, 0);
    in.an[0] = (uint8_t)stick(axis[0], SDL_CONTROLLER_AXIS_RIGHTY, 0);
    in.an[2] = (uint8_t)stick(axis[2], SDL_CONTROLLER_AXIS_RIGHTX, 0);
    const int gun = k[SDL_SCANCODE_LCTRL] || k[SDL_SCANCODE_Z] || pad_btn(SDL_CONTROLLER_BUTTON_A) || pad_axis(SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 16000;
    const int missile = k[SDL_SCANCODE_LALT] || k[SDL_SCANCODE_X] || pad_btn(SDL_CONTROLLER_BUTTON_B) || pad_axis(SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 16000;
    const int view = k[SDL_SCANCODE_SPACE] || k[SDL_SCANCODE_C] || pad_btn(SDL_CONTROLLER_BUTTON_Y);
    const int coin = k[SDL_SCANCODE_5] || pad_btn(SDL_CONTROLLER_BUTTON_BACK);
    const int start = k[SDL_SCANCODE_1] || k[SDL_SCANCODE_RETURN] || pad_btn(SDL_CONTROLLER_BUTTON_START);
    const int service = k[SDL_SCANCODE_9] || pad_btn(SDL_CONTROLLER_BUTTON_LEFTSHOULDER);
    const int test = k[SDL_SCANCODE_F2];
    if (test && !test_prev) { test_on = !test_on; fprintf(stderr, "[INPUT] test switch %s\n", test_on ? "ON" : "OFF"); }
    test_prev = test;
    in.mcuh = (uint8_t)(0xFF & ~(gun ? 0x20 : 0) & ~(missile ? 0x08 : 0) & ~(view ? 0x02 : 0));
    in.mcuc = (uint8_t)(0xFF & ~(coin ? 0x20 : 0) & ~(service ? 0x80 : 0));
    in.mcub = (uint8_t)(0xFF & ~(start ? 0x80 : 0));
    in.dsw = (uint8_t)((cur.dsw & ~1) | (test_on ? 0 : 1));
    { static s21_inputs prev; static int log = -1; if (log < 0) log = getenv("CS_INLOG") != NULL;   /* CS_INLOG=1: every change of the ports */
      s21_debug_frame(&in);
      if (log && memcmp(&prev, &in, sizeof in)) { fprintf(stderr, "[INPUT] b %02X c %02X h %02X dsw %02X an %02X %02X %02X %02X\n", in.mcub, in.mcuc, in.mcuh, in.dsw, in.an[0], in.an[1], in.an[2], in.an[3]); prev = in; } }
    s21_io_set_inputs(&in);
}

void s21_input_set_test(int on) { if (test_on != !!on) fprintf(stderr, "[INPUT] test switch %s\n", on ? "ON" : "OFF"); test_on = !!on; }

void s21_input_neutral(void)
{
    s21_inputs in = cur; in.dsw = (uint8_t)((cur.dsw & ~1) | (test_on ? 0 : 1));
    s21_debug_frame(&in);
    for (int i = 0; i < 4; i++) axis[i] = 0x7F;
    s21_io_set_inputs(&in);
}

/* the Controls page: the bindings (fixed for now) and the two cabinet switches, so a pad reaches them */
static const char *rows[][2] = {
    { "Drive (both levers)", "arrows, Shift+Left/Right strafe, pad D-pad" }, { "Left lever", "E/D/S/F, pad left stick" }, { "Right lever", "I/K/J/L, pad right stick" },
    { "Gun", "Ctrl / Z, pad A or RT" }, { "Missile", "Alt / X, pad B or LT" }, { "Viewport", "Space / C, pad Y" },
    { "Coin / Start", "5 / 1 or Enter, pad Back / Start" }, { "Test switch", "" }, { "Service button", "9, pad LB" },
};
static int nrows(void) { return (int)(sizeof rows / sizeof rows[0]); }
static bool has_value(int r) { return r == 7; }
static void text(int r, char *l, size_t ln, char *v, size_t vn)
{
    snprintf(l, ln, "%s", rows[r][0]);
    if (r == 7) snprintf(v, vn, "%s (F2)", test_on ? "ON" : "OFF"); else snprintf(v, vn, "%s", rows[r][1]);
}
static void change(int r, int dir) { (void)dir; if (r == 7) { test_on = !test_on; fprintf(stderr, "[INPUT] test switch %s\n", test_on ? "ON" : "OFF"); } }
static const eng_ui_page page = { "Controls", 460, 130, 0, nrows, has_value, NULL, text, change, NULL };
const eng_ui_page *s21_input_page(void) { return &page; }
