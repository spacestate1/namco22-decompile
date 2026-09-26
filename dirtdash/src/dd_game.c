/*
 * dd_game.c -- Dirt Dash (Namco 1995, Super System 22, MAME `dirtdash` family, World DT2 Ver.A = `dirtdasha`) as a table for the shared
 * Super 22 host (engine/ss22_run.c, ss22_board.c, ss22_dsp.c, ss22_snd.c, ss22_video.c, ss22_input.c, ss22_host.c). Everything here is
 * what is Dirt Dash's: its ROM chips, how its program image is laid out, the keycus answer, the sound board's wiring, the cabinet's
 * controls and the scripted play a test run feeds them. The program itself is gen/dd_lifted.c (tools/namco22/lift.py); its master DSP and
 * sound programs are translated at build time (gen/dd_c25.c, gen/dd_snd_driver.c).
 *
 * The ROM chip list is tools/setup_roms.py's REQUIRED (World, DT2 Ver.A): names and sizes, checked before anything is written.
 * dirtdash.zip is the whole set (the sound BIOS c71.bin included); the program is its dirtdasha/ sub-folder (World DT2 Ver.A), NOT
 * dirtdashj/ (Japan).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "ss22_game.h"
#include "dd_lifted.h"

bool dd_c25_exec(c71_t *d, int pc);          /* gen/dd_c25.c */

static const eng_rom_t k_roms[] = {
    {"c71.bin", 0x2000},
    {"dt1ccrh.1d", 0x80000},
    {"dt1ccrl.3d", 0x200000},
    {"dt1cg0.8d", 0x200000},
    {"dt1cg1.10d", 0x200000},
    {"dt1cg2.12d", 0x200000},
    {"dt1cg3.13d", 0x200000},
    {"dt1cg4.14d", 0x200000},
    {"dt1cg5.16d", 0x200000},
    {"dt1cg6.18d", 0x200000},
    {"dt1cg7.19d", 0x200000},
    {"dt1dataa.8k", 0x80000},
    {"dt1ptrl0.18k", 0x80000},
    {"dt1ptrl1.16k", 0x80000},
    {"dt1ptrl2.15k", 0x80000},
    {"dt1ptrm0.18j", 0x80000},
    {"dt1ptrm1.16j", 0x80000},
    {"dt1ptrm2.15j", 0x80000},
    {"dt1ptru0.18f", 0x80000},
    {"dt1ptru1.16f", 0x80000},
    {"dt1ptru2.15f", 0x80000},
    {"dt1scg0.12f", 0x200000},
    {"dt1scg1.10f", 0x200000},
    {"dt1wavea.2l", 0x400000},
    {"dt1waveb.1l", 0x400000},
    {"dt2vera.1", 0x200000, "dirtdasha/dt2vera.1"},          /* World DT2 Ver.A: MAME's dirtdasha, a sub-folder of dirtdash.zip */
    {"dt2vera.2", 0x200000, "dirtdasha/dt2vera.2"},
};
#define NROMS ((int)(sizeof k_roms / sizeof k_roms[0]))

/* ---- ROM images --------------------------------------------------------------------------- */
static bool dd_load_program(const char *dir)
{
    /* MAME ROM_LOAD32_WORD_SWAP: dt2vera.2 -> the high 16-bit word of every long, dt2vera.1 -> the low word, the two bytes of each
     * file's words swapped (the chips are little-endian 16-bit, the 68020 big-endian) */
    static const char *const half[2] = { "dt2vera.2", "dt2vera.1" };
    for (int h = 0; h < 2; h++) {
        char p[1024]; snprintf(p, sizeof p, "%s/%s", dir, half[h]);
        FILE *f = fopen(p, "rb");
        if (!f) { fprintf(stderr, "cannot open %s\n", p); return false; }
        for (uint32_t i = 0; i < SS22_ROM_SIZE / 4; i++) {
            int lo = fgetc(f), hi = fgetc(f);
            if (lo == EOF || hi == EOF) { fclose(f); fprintf(stderr, "short %s\n", p); return false; }
            g_ss22.rom[i * 4 + (uint32_t)h * 2]     = (uint8_t)hi;
            g_ss22.rom[i * 4 + (uint32_t)h * 2 + 1] = (uint8_t)lo;
        }
        fclose(f);
    }
    return true;
}

/* Dirt Dash's set has no default EEPROM image (Tokyo Wars' has one): MAME starts a game with no nvram file from a blank 2864, and the
 * game formats it itself. What "blank" reads as is DD_EEPROM_FILL (default 0xFF), checked against MAME's own reads by the T3 trace. */
static bool dd_load_eeprom(const char *dir)
{
    (void)dir;
    const char *e = getenv("DD_EEPROM_FILL");
    memset(g_ss22.eeprom, e ? (int)strtol(e, NULL, 0) : 0xFF, SS22_EEPROM_SIZE);
    return true;
}


static const ss22_board_cfg dd_board = { "DD", 0, 0x01A2, dd_load_program, dd_load_eeprom };      /* keycus: unit 0 answers 0x01A2 (NAMCOS22_DIRT_DASH) */

/* the cabinet: coin, service, test, View Change (also confirms the menus), Shift Up / Shift Down, Motion-Stop, the steering wheel (ADC 0,
 * 1..0x3FF), the gas pedal (ADC 1, 0..0x140) and the brake pedal (ADC 2, 0..0x100); keys ramp like MAME's KEYDELTA (12 on the wheel, 40 on the pedals) */
#define IN_COIN    0x0001
#define IN_SERVICE 0x0004
#define IN_TEST    0x0008
#define IN_VIEW    0x0010        /* View Change (also confirms the stage / car menus, with the gas pedal) */
#define IN_SHIFTUP 0x0020
#define IN_SHIFTDN 0x0040
#define IN_MOTION  0x0080        /* Motion-Stop (the deluxe cabinet's motion cut-out) */
static const ss22_action actions[] = {
    { "Insert coin",           "coin",        SDL_SCANCODE_5,      SDL_SCANCODE_6,        IN_COIN,    0,                   SS22_PAD(SDL_CONTROLLER_BUTTON_BACK) },
    { "View / select",         "view",        SDL_SCANCODE_C,      SDL_SCANCODE_RETURN,   IN_VIEW,    0,                   SS22_PAD(SDL_CONTROLLER_BUTTON_START) | SS22_PAD(SDL_CONTROLLER_BUTTON_A) },
    { "Service",               "service",     SDL_SCANCODE_9,      SDL_SCANCODE_UNKNOWN,  IN_SERVICE, 0,                   0 },
    { "Test",                  "test",        SDL_SCANCODE_F2,     SDL_SCANCODE_UNKNOWN,  IN_TEST,    0,                   0 },
    { "Wheel left",            "wheel_left",  SDL_SCANCODE_LEFT,   SDL_SCANCODE_A,        0,          SS22_AX_WHEEL_LEFT,  0 },
    { "Wheel right",           "wheel_right", SDL_SCANCODE_RIGHT,  SDL_SCANCODE_D,        0,          SS22_AX_WHEEL_RIGHT, 0 },
    { "Gas pedal",             "gas",         SDL_SCANCODE_X,      SDL_SCANCODE_UP,       0,          SS22_AX_PEDAL1,      0 },
    { "Brake pedal",           "brake",       SDL_SCANCODE_Z,      SDL_SCANCODE_DOWN,     0,          SS22_AX_PEDAL2,      0 },
    { "Shift up",              "shift_up",    SDL_SCANCODE_E,      SDL_SCANCODE_LCTRL,    IN_SHIFTUP, 0,                   SS22_PAD(SDL_CONTROLLER_BUTTON_Y) | SS22_PAD(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) },
    { "Shift down",            "shift_down",  SDL_SCANCODE_Q,      SDL_SCANCODE_LALT,     IN_SHIFTDN, 0,                   SS22_PAD(SDL_CONTROLLER_BUTTON_X) | SS22_PAD(SDL_CONTROLLER_BUTTON_LEFTSHOULDER) },
    { "Motion stop",           "motion_stop", SDL_SCANCODE_M,      SDL_SCANCODE_UNKNOWN,  IN_MOTION,  0,                   SS22_PAD(SDL_CONTROLLER_BUTTON_B) },
};
static const ss22_input_game input = {
    actions, (int)(sizeof actions / sizeof *actions),
    0x001, 0x3FF, 0x1FF, 12,
    { 0x140, 0x100 }, 40,
    { "Enter, then press the new key (Esc cancels)", "Alternates: 6, Enter, Up/Down, A D, Ctrl, Alt", "Pad: stick, RT gas, LT brake, X/Y shift, A" },
    ss22_snd_inputs,
    IN_TEST, IN_SERVICE,
};

/* widescreen: the race HUD is up while the TIME / POSITION labels are on screen (text cells row 1: col 2 and col 31). Its digits and the map's
 * route are sprites at depth 0..2 (the world's smoke and dust are deeper); the map and the tacho needle are polygons in sub-window viewports. */
static const eng_hud_mark hud_marks[] = { { 1, 2, 0x01CE }, { 1, 31, 0x01C0 } };

static const ss22_press_name presses[] = {
    {"coin", IN_COIN}, {"view", IN_VIEW}, {"service", IN_SERVICE}, {"test", IN_TEST},
    {"up", IN_SHIFTUP}, {"down", IN_SHIFTDN}, {"motion", IN_MOTION} };

/* DD_AUTOPLAY=test: the operator menu (test mode) walked at random -- tools/mame/cov_trace_test.lua, the script MAME's test-mode coverage came from:
 * the cabinet switch on at frame 300 (off for 120 frames every 4000 to restart the menu), then every 12..31 frames a new action held: gas
 * (enter), wheel left/right (choose), shift up/down (change a value), view, service, coin, motion-stop, brake. DD_TEST_SEED picks the walk. */
static void autoplay_test(long n, uint16_t *p, unsigned *wheel, unsigned *gas, unsigned *brake)
{
    static unsigned long long seed; static int init, left; static uint16_t held; static unsigned hw = 512, hg, hb;
    if (!init) { const char *e = getenv("DD_TEST_SEED"); seed = e ? strtoull(e, NULL, 0) : 12345; init = 1; }
    const long t = n - 300;
    if (t >= 0 && !(t >= 4000 && t % 4000 < 120)) *p |= IN_TEST;
    if (n > 400) {
        if (left > 0) left--;
        else {
            unsigned r;
            #define RND(k) (seed = (seed * 1103515245ull + 12345ull) % 2147483648ull, (unsigned)((seed / 65536) % (k)))
            held = 0; hw = 512; hg = 0; hb = 0;
            r = RND(100);
            if      (r < 22) hg = 300;
            else if (r < 40) hw = 512 - 220;
            else if (r < 58) hw = 512 + 220;
            else if (r < 70) held = IN_SHIFTUP;
            else if (r < 80) held = IN_SHIFTDN;
            else if (r < 84) held = IN_VIEW;
            else if (r < 88) held = IN_SERVICE;
            else if (r < 91) held = IN_COIN;
            else if (r < 94) held = IN_MOTION;
            else if (r < 97) hb = 200;
            left = 12 + (int)RND(20);
            #undef RND
        }
    }
    *p |= held; *wheel = hw; *gas = hg; *brake = hb;
}

#define START_DECIDE 620          /* frame the gas pedal first decides (the stage), then again every START_PRESS_EVERY frames (car, transmission) */
#define START_PRESS_EVERY 200
#define START_PRESS_LEN 40
#define START_HOLD 720            /* the wheel stays at the stage's position until here (the cursor follows it: letting go before the stage is decided moves it back) */
#define START_END 900             /* the script is over at this frame (the car is decided, the race is about to start) */
/* --stage NAME: the start of the game played for you. The stage select's cursor follows the wheel's ABSOLUTE position and cycles through the five stages
 * (measured with the wheel held: 100 jungle, 300 mountain, 420 snow, 512 city, 620 hill, then it repeats), and the stage is decided with the gas pedal
 * (the panel says SELECT wheel, DECIDE gas), the car and the transmission the same way. So: two coins, the wheel centred until the select screen is up,
 * held at the stage's position, then the gas pedal to decide -- and the cabinet is the player's again. */
static const struct { const char *name; unsigned wheel; } stages[] = { {"city", 512}, {"jungle", 100}, {"hill", 620}, {"mountain", 300}, {"snow", 420} };
static bool start(const char *name, long n, uint16_t *p, unsigned *wheel, unsigned *gas, unsigned *brake)
{
    (void)brake;
    int st = -1;
    for (int i = 0; i < 5; i++) if (!strcasecmp(name, stages[i].name)) st = i;
    if (st < 0) return false;
    if (n < 0) return true;                                                  /* the name is good */
    if (n >= START_END) return false;
    if ((n >= 300 && n < 306) || (n >= 340 && n < 346)) *p |= IN_COIN;      /* two coins: a game costs two credits */
    *wheel = n < 400 ? 512u : n < START_HOLD ? stages[st].wheel : 512u;
    if (n >= START_DECIDE && (n - START_DECIDE) % START_PRESS_EVERY < START_PRESS_LEN) *gas = 320;
    return true;
}

/* tools/mame/cov_trace_play.lua, frame for frame */
static void autoplay(long n, uint16_t *p, unsigned *wheel, unsigned *gas, unsigned *brake)
{
    const long m = n % 3600;
    static int test = -1;
    if (test < 0) { const char *e = getenv("DD_AUTOPLAY"); test = e && !strcmp(e, "test"); }
    if (test) { autoplay_test(n, p, wheel, gas, brake); return; }
    if ((m >= 300 && m < 306) || (m >= 340 && m < 346)) *p |= IN_COIN;      /* two coins: a game costs two credits */
    if (n >= 500 && n % 120 < 6) *p |= IN_VIEW;                              /* confirms the stage and the car */
    if (n > 1500) *gas = 320;
    *wheel = (unsigned)(512 + (((n / 60) % 2 == 0) ? -180 : 180));
    /* DD_DRIVE=<a>[:<b>]: a fixed test drive instead of the weaving script -- gas full from frame 1300, the wheel at 512+a until frame 2100 and 512+b after
     * (b defaults to a). For comparing how a stage handles, and for MAME to be given the same inputs. */
    static long da = -99999, db;
    if (da == -99999) { const char *e = getenv("DD_DRIVE"); da = 0; db = 0; if (e) { char *q; da = strtol(e, &q, 10); db = *q == ':' ? strtol(q + 1, NULL, 10) : da; } else da = -99999 - 1; }
    if (da != -99999 - 1) {
        if (n >= 900) *p = 0;                                            /* no View pulses in the race: the camera stays put */
        *wheel = (unsigned)(512 + (n < 2100 ? da : db));
        *gas = n >= 1300 ? 320u : 0u;
        *brake = 0;
        return;
    }
    if (n > 2000 && n % 400 < 40) *brake = 256;
    if (n > 1800 && n % 170 < 5) *p |= IN_SHIFTUP;
    if (n > 1800 && n % 230 < 5) *p |= IN_SHIFTDN;
    if (n > 2200 && n % 900 < 30) *p |= IN_MOTION;
}

static const ss22_game game = {
    .name = "Dirt Dash", .tag = "DD", .lname = "dd", .logname = "dirtdash.log", .zip = "dirtdash.zip",
    .board = &dd_board,
    .dsp = { { {"dt1ptrl0.18k", "dt1ptrl1.16k", "dt1ptrl2.15k"},
               {"dt1ptrm0.18j", "dt1ptrm1.16j", "dt1ptrm2.15j"},
               {"dt1ptru0.18f", "dt1ptru1.16f", "dt1ptru2.15f"} }, 3, dd_c25_exec },     /* the region is three planes of three chips (Prop Cycle's shape) */
    .snd = { "dt1dataa.8k", { "dt1wavea.2l", "dt1waveb.1l" }, { 0, 0x800000 }, 0x1000000, 0xFDFF, { 1, 2 } },   /* MAME's c352 region: dt1wavea.2l at 0, dt1waveb.1l at 0x800000 */
    .video = { { "dt1cg0.8d", "dt1cg1.10d", "dt1cg2.12d", "dt1cg3.13d", "dt1cg4.14d", "dt1cg5.16d", "dt1cg6.18d", "dt1cg7.19d" },
               "dt1ccrl.3d", "dt1ccrh.1d", { "dt1scg0.12f", "dt1scg1.10f" }, 2, 0x1000000, 0xFF,
               true, true,     /* MAME: a 16 MB region, ROMREGION_ERASEFF, the two chips at 0 and 0x200000; the spot is on in its captures */
               .hud = { hud_marks, 2, 0x10, -1 } },
    .input = &input,
    .roms = k_roms, .n_roms = NROMS,
    .entry = L_1074,
    .polls_per_frame = 56000,
    .presses = presses, .n_presses = (int)(sizeof presses / sizeof *presses),
    .autoplay = autoplay,
    .start = start, .start_names = "city, jungle, hill, mountain, snow",
    .pedal_full = { 0x140, 0x100 },
};

int main(int argc, char **argv) { return ss22_main(argc, argv, &game); }
