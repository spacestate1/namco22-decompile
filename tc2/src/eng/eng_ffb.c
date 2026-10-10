/* TIME CRISIS 2'S OWN COPY of namco-2x-systems engine/eng_ffb.c (copied 2026-10-05, with Time Crisis 1's light-gun support). TC2 is independent of the System 22 project: change it here. */
/* eng_ffb.c -- see eng_ffb.h */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "eng_ffb.h"

static SDL_Haptic *haptic;
static int effect = -1;
static SDL_JoystickID dev = -1;                 /* the device last tried, so a wheel without FFB is not retried every frame */
static int applied;
static Uint8 dirtype = SDL_HAPTIC_CARTESIAN;    /* the direction encoding the device took (eng_ffb_device) */

int eng_ffb_decode(uint8_t b)
{
    int r = 0;                                  /* bits 2-7, bit order reversed */
    for (int i = 0; i < 6; i++) if (b >> (2 + i) & 1) r |= 1 << (5 - i);
    if (r == 0) return 0;                       /* 63: never sent (Dirt Dash stops at 62, Rave Racer at 60) -- RAM not written yet */
    return (b & 2) ? r - 63 : 63 - r;
}

/* SDL's direction is where a force comes FROM: along +x a positive level pushes toward the axis' negative end. The steering-axis
 * encoding is the same direction (SDL's Linux backend turns both into 0x4000), but on Windows it makes a ONE-axis DirectInput
 * effect, where a Cartesian one spans every force-feedback axis the device reports -- which a wheel with one motor may refuse
 * (a Fanatec DD base: steering and buttons fine, no force). CannonBall picks the encodings the same way. */
static SDL_HapticEffect constant(int level)
{
    SDL_HapticEffect e;
    memset(&e, 0, sizeof e);
    e.type = SDL_HAPTIC_CONSTANT;
    e.constant.direction.type = dirtype;
    e.constant.direction.dir[0] = 1;
    e.constant.length = SDL_HAPTIC_INFINITY;
    e.constant.level = (Sint16)level;
    return e;
}

static void release(void)
{
    if (haptic && effect >= 0) {                /* a wheel left holding the last force would keep pushing after we are gone */
        SDL_HapticStopEffect(haptic, effect);
        SDL_HapticDestroyEffect(haptic, effect);
    }
    if (haptic) SDL_HapticClose(haptic);
    haptic = NULL; effect = -1;
}

static Uint32 tried = 0;                        /* the candidate set that failed last (eng_ffb_device_from), 0 = none */

void eng_ffb_close(void) { release(); dev = -1; tried = 0; }

void eng_ffb_start(void)
{
    if (!SDL_WasInit(SDL_INIT_HAPTIC) && SDL_InitSubSystem(SDL_INIT_HAPTIC) != 0)
        fprintf(stderr, "[FFB] SDL haptic: %s\n", SDL_GetError());
}

bool eng_ffb_capable(SDL_Joystick *js)
{
    if (!js) return false;
    if (!SDL_WasInit(SDL_INIT_HAPTIC) && SDL_InitSubSystem(SDL_INIT_HAPTIC) != 0) return false;
    return SDL_JoystickIsHaptic(js) == 1;
}

void eng_ffb_forget(SDL_JoystickID id) { if (id == dev) eng_ffb_close(); }

bool eng_ffb_device(SDL_Joystick *js)
{
    const SDL_JoystickID id = js ? SDL_JoystickInstanceID(js) : -1;
    if (id == dev) return effect >= 0;
    release();
    dev = id;
    if (!js) return false;
    if (!SDL_WasInit(SDL_INIT_HAPTIC) && SDL_InitSubSystem(SDL_INIT_HAPTIC) != 0) {
        fprintf(stderr, "[FFB] SDL haptic: %s\n", SDL_GetError());
        return false;
    }
    const char *name = SDL_JoystickName(js) ? SDL_JoystickName(js) : "joystick";
    const bool wheel = SDL_JoystickGetType(js) == SDL_JOYSTICK_TYPE_WHEEL;
    if (!(haptic = SDL_HapticOpenFromJoystick(js))) {
        fprintf(stderr, "[FFB] %s: no force feedback\n", name);
        return false;
    }
    const unsigned q = SDL_HapticQuery(haptic);
    if (!(q & SDL_HAPTIC_CONSTANT)) {
        fprintf(stderr, "[FFB] %s: no constant force\n", name);
        release();
        return false;
    }
    if (q & SDL_HAPTIC_AUTOCENTER) SDL_HapticSetAutocenter(haptic, 0);        /* the motor is the only force, as in the cabinet */
    if (q & SDL_HAPTIC_GAIN) SDL_HapticSetGain(haptic, 100);
    /* a wheel tries the steering axis first, anything else the Cartesian x axis first; each falls back to the other */
    const Uint8 order[2] = { wheel ? SDL_HAPTIC_STEERING_AXIS : SDL_HAPTIC_CARTESIAN,
                             wheel ? SDL_HAPTIC_CARTESIAN : SDL_HAPTIC_STEERING_AXIS };
    for (int i = 0; i < 2 && effect < 0; i++) {
        dirtype = order[i];
        SDL_HapticEffect e = constant(0);
        effect = SDL_HapticNewEffect(haptic, &e);
        if (effect >= 0 && SDL_HapticRunEffect(haptic, effect, 1) != 0) { SDL_HapticDestroyEffect(haptic, effect); effect = -1; }
    }
    if (effect < 0) { fprintf(stderr, "[FFB] %s: %s\n", name, SDL_GetError()); release(); return false; }
    applied = 0;
    static bool at_exit;
    if (!at_exit) { atexit(eng_ffb_close); at_exit = true; }      /* every way out that ends in exit() */
    fprintf(stderr, "[FFB] %s drives the wheel motor\n", name);
    return true;
}

bool eng_ffb_device_from(SDL_Joystick *const *js, int n)
{
    if (n <= 0) { eng_ffb_device(NULL); tried = 0; return false; }
    Uint32 key = 2166136261u;                    /* the set, in order */
    for (int i = 0; i < n; i++) {
        const SDL_JoystickID id = SDL_JoystickInstanceID(js[i]);
        if (effect >= 0 && id == dev) return true;         /* already driving one of them */
        key = (key ^ (Uint32)(id + 1)) * 16777619u;
    }
    if (key == tried) return false;
    tried = key;
    for (int i = 0; i < n; i++) {
        dev = -2;                                /* force an attempt even if this device was tried alone before */
        if (eng_ffb_device(js[i])) { tried = 0; return true; }
    }
    return false;
}

void eng_ffb_force(int motor, int strength, bool reverse) { eng_ffb_force_f(motor / 63.0, strength, reverse); }

void eng_ffb_force_f(double f, int strength, bool reverse)
{
    if (effect < 0) return;
    /* a negative command pushes toward the higher A-D side: the axis' positive end unless reversed */
    if (f > 1.0) f = 1.0;
    if (f < -1.0) f = -1.0;
    int level = (int)(f * 32767.0 * strength / 100.0);
    if (reverse) level = -level;
    if (level == applied) return;
    SDL_HapticEffect e = constant(level);
    if (SDL_HapticUpdateEffect(haptic, effect, &e) == 0) applied = level;
}
