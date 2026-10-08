/*
 * tw_game.c -- Tokyo Wars (Namco 1996, Super System 22, MAME `tokyowar`, World TW2 Ver.A) as a table for the shared Super 22 host
 * (engine/ss22_run.c, ss22_board.c, ss22_dsp.c, ss22_snd.c, ss22_video.c, ss22_input.c, ss22_host.c). Everything here is what is
 * Tokyo Wars': its ROM chips, how its program image is laid out, the keycus answer, the sound board's wiring, the cabinet's controls and
 * the scripted play a test run feeds them. The program itself is gen/tw_lifted.c (tools/namco22/lift.py); its master DSP and sound
 * programs are translated at build time (gen/tw_c25.c, gen/tw_snd_driver.c).
 *
 * The ROM chip list is tools/setup_roms.py's REQUIRED: names and sizes, checked before anything is written. tokyowar.zip is the whole set
 * (the default EEPROM included; the DSP BIOS, c71.bin, is built into the engine: engine/c25/c71_bios.c); its Japanese program set under tokyowarj/ is not taken.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ss22_game.h"
#include "tw_lifted.h"

bool tw_c25_exec(c71_t *d, int pc);          /* gen/tw_c25.c */

static const eng_rom_t k_roms[] = {
    {"tokyowar_defaults.nv", 0x2000},
    {"tw1ccrh.1d", 0x80000},
    {"tw1ccrl.3d", 0x200000},
    {"tw1cg0.8d", 0x200000},
    {"tw1cg1.10d", 0x200000},
    {"tw1cg2.12d", 0x200000},
    {"tw1cg3.13d", 0x200000},
    {"tw1cg4.14d", 0x200000},
    {"tw1cg5.16d", 0x200000},
    {"tw1cg6.18d", 0x200000},
    {"tw1cg7.19d", 0x200000},
    {"tw1data.8k", 0x80000},
    {"tw1ptrl0.18k", 0x80000},
    {"tw1ptrl1.16k", 0x80000},
    {"tw1ptrl2.15k", 0x80000},
    {"tw1ptrl3.14k", 0x80000},
    {"tw1ptrm0.18j", 0x80000},
    {"tw1ptrm1.16j", 0x80000},
    {"tw1ptrm2.15j", 0x80000},
    {"tw1ptrm3.14j", 0x80000},
    {"tw1ptru0.18f", 0x80000},
    {"tw1ptru1.16f", 0x80000},
    {"tw1ptru2.15f", 0x80000},
    {"tw1ptru3.14f", 0x80000},
    {"tw1scg0.12f", 0x200000},
    {"tw1scg1.10f", 0x200000},
    {"tw1scg2.8f", 0x200000},
    {"tw1scg3.7f", 0x200000},
    {"tw1wavea.2l", 0x400000},
    /* current MAME sets (this project's own reference namcos22.cpp among them) ship these as tw2vera.1-.4, no hyphen; older
     * archived sets used tw2ver-a.1-.4. Same data either way -- `alt` takes whichever spelling the zip actually has. */
    {"tw2ver-a.1", 0x100000, NULL, "tw2vera.1"},
    {"tw2ver-a.2", 0x100000, NULL, "tw2vera.2"},
    {"tw2ver-a.3", 0x100000, NULL, "tw2vera.3"},
    {"tw2ver-a.4", 0x100000, NULL, "tw2vera.4"},
};
#define NROMS ((int)(sizeof k_roms / sizeof k_roms[0]))

/* ---- ROM images --------------------------------------------------------------------------- */
static bool tw_load_program(const char *dir)
{
    /* MAME ROM_LOAD32_BYTE: tw2ver-a.4/.3/.2/.1 -> byte 0/1/2/3 of every long */
    static const char *lane[4] = { "tw2ver-a.4", "tw2ver-a.3", "tw2ver-a.2", "tw2ver-a.1" };
    for (int l = 0; l < 4; l++) {
        char p[1024]; snprintf(p, sizeof p, "%s/%s", dir, lane[l]);
        FILE *f = fopen(p, "rb");
        if (!f) { fprintf(stderr, "cannot open %s\n", p); return false; }
        for (uint32_t i = 0; i < SS22_ROM_SIZE / 4; i++) {
            int c = fgetc(f);
            if (c == EOF) { fclose(f); fprintf(stderr, "short %s\n", p); return false; }
            g_ss22.rom[i * 4 + (uint32_t)l] = (uint8_t)c;
        }
        fclose(f);
    }
    return true;
}

static bool tw_load_eeprom(const char *dir)
{
    char p[1024]; snprintf(p, sizeof p, "%s/tokyowar_defaults.nv", dir);
    FILE *f = fopen(p, "rb");
    if (!f) { fprintf(stderr, "[TW] no %s -- EEPROM blank\n", p); return false; }
    size_t n = fread(g_ss22.eeprom, 1, SS22_EEPROM_SIZE, f);
    fclose(f);
    if (n != SS22_EEPROM_SIZE) fprintf(stderr, "[TW] short EEPROM image (%zu bytes)\n", n);
    return n == SS22_EEPROM_SIZE;
}


static const ss22_board_cfg tw_board = { "TW", 4, 0x01A8, tw_load_program, tw_load_eeprom };      /* keycus: unit 4 answers 0x01A8 (NAMCOS22_TOKYO_WARS) */

/* the cabinet: coin, service, test, start (also the view change), the two trigger buttons, the steering wheel (ADC 0, 0x100..0x300) and the
 * forward / backward pedals (ADC 2 and 3, 0..0x100), keys ramping at PORT_KEYDELTA(20) a frame */
#define IN_COIN    0x0001
#define IN_SERVICE 0x0004
#define IN_TEST    0x0008
#define IN_START   0x0010
#define IN_RTRIG   0x0020
#define IN_LTRIG   0x0040
static const ss22_action actions[] = {
    { "Insert coin",     "coin",           SDL_SCANCODE_5,      SDL_SCANCODE_6,        IN_COIN,    0,                   SS22_PAD(SDL_CONTROLLER_BUTTON_BACK) },
    { "Start",           "start",          SDL_SCANCODE_RETURN, SDL_SCANCODE_KP_ENTER, IN_START,   0,                   SS22_PAD(SDL_CONTROLLER_BUTTON_START) | SS22_PAD(SDL_CONTROLLER_BUTTON_A) | SS22_PAD(SDL_CONTROLLER_BUTTON_Y) },
    { "Service",         "service",        SDL_SCANCODE_9,      SDL_SCANCODE_UNKNOWN,  IN_SERVICE, 0,                   0 },
    { "Test",            "test",           SDL_SCANCODE_F2,     SDL_SCANCODE_UNKNOWN,  IN_TEST,    0,                   0 },
    { "Wheel left",      "wheel_left",     SDL_SCANCODE_LEFT,   SDL_SCANCODE_A,        0,          SS22_AX_WHEEL_LEFT,  0 },
    { "Wheel right",     "wheel_right",    SDL_SCANCODE_RIGHT,  SDL_SCANCODE_D,        0,          SS22_AX_WHEEL_RIGHT, 0 },
    { "Forward pedal",   "pedal_forward",  SDL_SCANCODE_UP,     SDL_SCANCODE_W,        0,          SS22_AX_PEDAL1,      0 },
    { "Backward pedal",  "pedal_backward", SDL_SCANCODE_DOWN,   SDL_SCANCODE_S,        0,          SS22_AX_PEDAL2,      0 },
    { "Left trigger",    "trigger_left",   SDL_SCANCODE_Z,      SDL_SCANCODE_LCTRL,    IN_LTRIG,   0,                   SS22_PAD(SDL_CONTROLLER_BUTTON_X) | SS22_PAD(SDL_CONTROLLER_BUTTON_LEFTSHOULDER) },
    { "Right trigger",   "trigger_right",  SDL_SCANCODE_X,      SDL_SCANCODE_LALT,     IN_RTRIG,   0,                   SS22_PAD(SDL_CONTROLLER_BUTTON_B) | SS22_PAD(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) },
};
static const ss22_input_game input = {
    actions, (int)(sizeof actions / sizeof *actions),
    0x100, 0x300, 0x100, 20,
    { 0x100, 0x100 }, 20,
    { "Enter, then press the new key (Esc cancels)", "Alternates: 6, keypad Enter, A D W S, Ctrl, Alt", "Pad: stick steers, triggers = pedals" },
    ss22_snd_inputs,
    IN_TEST, IN_SERVICE,
    .kick_wheel = true,                      /* the handle solenoid (mcuout4, recoil_mask): a jolt on a force-feedback wheel */
};

/* widescreen: the battle HUD is up while the tank-count labels are on screen (text cells row 2: col 1 on the left, col 29 on the right). Its
 * counts, icons and gauges are text tiles; the radar is a polygon in a sub-window viewport. */
static const eng_hud_mark hud_marks[] = { { 2, 1, 0x92D0 }, { 2, 29, 0x82D0 } };

static const ss22_press_name presses[] = {
    {"coin", IN_COIN}, {"start", IN_START}, {"service", IN_SERVICE}, {"test", IN_TEST}, {"ltrig", IN_LTRIG}, {"rtrig", IN_RTRIG} };

/* tools/mame/cov_trace_play.lua, frame for frame */
static void autoplay(long n, uint16_t *p, unsigned *wheel, unsigned *pedal1, unsigned *pedal2)
{
    (void)pedal2;
    if (n % 600 >= 300 && n % 600 < 306) *p |= IN_COIN;
    if (n % 300 >= 150 && n % 300 < 156 && n > 400) *p |= IN_START;
    if (n > 700) *pedal1 = 0x100;
    *wheel = ((n / 90) % 2 == 0) ? 0x100 : 0x300;
    if (n % 40 < 4) *p |= IN_LTRIG;
    if (n % 55 < 4) *p |= IN_RTRIG;
}

/* THE CABINET LINK (engine/ss22_link.h, --link ID/N). The game's link driver (FUN_00092F82 init, FUN_00093072 send, FUN_000932A0 the SCI
 * handler, FUN_0009362E the per-frame latch) takes its cabinet number from the operator setting at EEPROM byte 9 (0x460012, & 3: four cabinets,
 * FUN_00012BB6 masks it), the high word of the setting at 0xE0A00C. Online play: src/tw_net.c. */
bool tw_net_boot(void);
void tw_link_cabinet(int id)
{
    g_ss22.eeprom[8] = 0; g_ss22.eeprom[9] = (uint8_t)(id & 3);                    /* read at boot (FUN_00092F82, FUN_0000532C) */
    g_ss22.wram[0x2000] = 0; g_ss22.wram[0x2001] = (uint8_t)(id & 3);              /* 0xE02000: the driver's copy, for a renumbering after boot */
}
static uint16_t w16(uint32_t a) { return (uint16_t)(g_ss22.wram[a - 0xE00000u] << 8 | g_ss22.wram[a - 0xE00000u + 1]); }
static void tw_link_debug(uint32_t f)
{
    fprintf(stderr, "[TW] link f%u game: cab %u ring %u ownret %u ok %u alive %04X objs %04X/%04X rx %u tx %u bad %u nopkt %u\n", f,
            w16(0xE02000), w16(0xE02006), w16(0xE0200A), w16(0xE0200C), w16(0xE02010), w16(0xE02014), w16(0xE02018),
            w16(0xE02062), w16(0xE02064), w16(0xE02066), w16(0xE02068));
}

static const ss22_game game = {
    .name = "Tokyo Wars", .tag = "TW", .lname = "tw", .logname = "tokyowar.log", .zip = "tokyowar.zip",
    .out_gain = 1.4,      /* the chip runs ~4x hotter than Rave Racer's (raw median RMS 2369 against 563): x6 put 15% of the samples above the limiter's knee; 1.4 lands on Rave Racer's level (~3400 RMS) */
    .board = &tw_board,
    .dsp = { { {"tw1ptrl0.18k", "tw1ptrl1.16k", "tw1ptrl2.15k", "tw1ptrl3.14k"},
               {"tw1ptrm0.18j", "tw1ptrm1.16j", "tw1ptrm2.15j", "tw1ptrm3.14j"},
               {"tw1ptru0.18f", "tw1ptru1.16f", "tw1ptru2.15f", "tw1ptru3.14f"} }, 4, tw_c25_exec,
               .spin_pc = 0x45BB, .spin_op = 0x2000 },   /* the upload routine's LAC *0 / BNEZ poll of polygon-RAM word 0 (~34% of all retired steps) */
    .snd = { "tw1data.8k", { "tw1wavea.2l", NULL }, { 0, 0 }, 0x400000, 0xFFFF, { 2, 3 } },
    .video = { { "tw1cg0.8d", "tw1cg1.10d", "tw1cg2.12d", "tw1cg3.13d", "tw1cg4.14d", "tw1cg5.16d", "tw1cg6.18d", "tw1cg7.19d" },
               "tw1ccrl.3d", "tw1ccrh.1d", { "tw1scg0.12f", "tw1scg1.10f", "tw1scg2.8f", "tw1scg3.7f" }, 4, 4 * 0x200000, 0x00,
               .hud = { hud_marks, 2, 0x10, -1 } },
    .input = &input,
    .roms = k_roms, .n_roms = NROMS,
    .entry = L_4000,
    .polls_per_frame = 66772,           /* 68K instructions per frame: MAME's average over its first 300 frames */
    .presses = presses, .n_presses = (int)(sizeof presses / sizeof *presses),
    .autoplay = autoplay,
    .pedal_full = { 0x100, 0x100 },
    .link_cabinet = tw_link_cabinet, .link_debug = tw_link_debug,
    .recoil_mask = 0x0010,              /* mcuout4, MAME "4 = handle solenoid": rises on each cannon shot (either trigger) -> pad rumble + wheel kick */
};

int main(int argc, char **argv) { tw_net_boot(); return ss22_main(argc, argv, &game); }
