/*
 * tc_game.c -- Time Crisis (Namco 1995/96, SUPER System 22, MAME `timecris` = World TS2 Ver.B) as a table for the shared Super 22 host
 * (engine/ss22_run.c, ss22_board.c, ss22_dsp.c, ss22_snd.c, ss22_video.c, ss22_input.c, ss22_host.c). Everything here is what is Time Crisis':
 * its ROM chips, how its program image is laid out, the light gun on the memory map, the cabinet's controls and the scripted play a test
 * run feeds them. The program itself is gen/tc_lifted.c (tools/namco22/lift.py); its master DSP program is translated at build time
 * (gen/tc_c25.c, tools/gen/c25.cov). See PLAN.md.
 *
 * The ROM chip list is tools/setup_roms.py's CHIPS (MAME's ROM_START(timecris)). The zip names the four program chips ts2ver-b.N where MAME's
 * current names are ts2verb.N: the table uses MAME's and the zip path names the file inside the set.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ss22_game.h"
#include "lift_cpu.h"
#include "tc_lifted.h"

bool tc_c25_exec(c71_t *d, int pc);          /* gen/tc_c25.c */

static const eng_rom_t k_roms[] = {
    {"ts2verb.1", 0x100000, "ts2ver-b.1"}, {"ts2verb.2", 0x100000, "ts2ver-b.2"}, {"ts2verb.3", 0x100000, "ts2ver-b.3"}, {"ts2verb.4", 0x100000, "ts2ver-b.4"},
    {"ts1data.8k", 0x80000},
    {"ts1scg0.12f", 0x200000}, {"ts1scg1.10f", 0x200000}, {"ts1scg2.8f", 0x200000}, {"ts1scg3.7f", 0x200000}, {"ts1scg4.5f", 0x200000}, {"ts1scg5.3f", 0x200000},
    {"ts1cg0.8d", 0x200000}, {"ts1cg1.10d", 0x200000}, {"ts1cg2.12d", 0x200000}, {"ts1cg3.13d", 0x200000}, {"ts1cg4.14d", 0x200000},
    {"ts1cg5.16d", 0x200000}, {"ts1cg6.18d", 0x200000},
    {"ts1ccrl.3d", 0x200000}, {"ts1ccrh.1d", 0x80000},
    {"ts1ptrl0.18k", 0x80000}, {"ts1ptrl1.16k", 0x80000}, {"ts1ptrl2.15k", 0x80000},
    {"ts1ptrm0.18j", 0x80000}, {"ts1ptrm1.16j", 0x80000}, {"ts1ptrm2.15j", 0x80000},
    {"ts1ptru0.18f", 0x80000}, {"ts1ptru1.16f", 0x80000}, {"ts1ptru2.15f", 0x80000},
    {"ts1wavea.2l", 0x400000}, {"ts1waveb.1l", 0x200000},
};
#define NROMS ((int)(sizeof k_roms / sizeof k_roms[0]))

/* MAME ROM_LOAD32_BYTE: ts2verb.1 -> byte 3 of every long, .2 -> 2, .3 -> 1, .4 -> 0 (four 1 MB chips) */
static bool tc_load_program(const char *dir)
{
    static const char *const lane[4] = { "ts2verb.4", "ts2verb.3", "ts2verb.2", "ts2verb.1" };      /* byte 0 .. 3 */
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

/* no default EEPROM image in the set: MAME starts from a blank 2864 (all 0xFF) and the game formats it */
static bool tc_load_eeprom(const char *dir)
{
    (void)dir;
    const char *e = getenv("TC_EEPROM_FILL");
    memset(g_ss22.eeprom, e ? (int)strtol(e, NULL, 0) : 0xFF, SS22_EEPROM_SIZE);
    return true;
}

/* keycus: Time Crisis is in none of namcos22_keycus_r's cases, so every read is random (a unit that is never asked: 0xFFFF); the light gun is the extra device */
static const ss22_board_cfg tc_board = { "TC", 0xFFFF, 0, tc_load_program, tc_load_eeprom, ss22_gun_read };

/* the cabinet: coin, service button, the operator's test switch, the gun's TRIGGER (also starts the game and confirms the menus) and the FOOT PEDAL
 * (takes cover / comes out to attack, reloads). The gun itself is the pointer (engine/ss22_board.c: g_ss22_gun_x / _y), not an axis here. */
#define IN_COIN    0x0001
#define IN_SERVICE 0x0004
#define IN_TEST    0x0008
#define IN_TRIGGER 0x0010
#define IN_PEDAL   0x0020
static const ss22_action actions[] = {
    { "Insert coin",  "coin",    SDL_SCANCODE_5,      SDL_SCANCODE_6,       IN_COIN,    0, SS22_PAD(SDL_CONTROLLER_BUTTON_BACK) },
    { "Gun trigger",  "trigger", SDL_SCANCODE_SPACE,  SDL_SCANCODE_RETURN,  IN_TRIGGER, 0, SS22_PAD(SDL_CONTROLLER_BUTTON_A) | SS22_PAD(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER), SDL_BUTTON_LMASK },
    { "Foot pedal",   "pedal",   SDL_SCANCODE_Z,      SDL_SCANCODE_X,       IN_PEDAL,   0, SS22_PAD(SDL_CONTROLLER_BUTTON_B) | SS22_PAD(SDL_CONTROLLER_BUTTON_LEFTSHOULDER), SDL_BUTTON_RMASK | SDL_BUTTON_MMASK },
    { "Service",      "service", SDL_SCANCODE_9,      SDL_SCANCODE_UNKNOWN, IN_SERVICE, 0, 0 },
    { "Test",         "test",    SDL_SCANCODE_F2,     SDL_SCANCODE_UNKNOWN, IN_TEST,    0, 0 },
};
static const ss22_input_game input = {
    actions, (int)(sizeof actions / sizeof *actions),
    0x001, 0x3FF, 0x1FF, 12,
    { 0, 0 }, 0,
    { "Enter, then press the new key (Esc cancels)", "Mouse / absolute-mouse gun aims: left = trigger, right/middle = pedal; screen edge, R or side button = off-screen (reload). F8 = gun border", "Pad: either stick or the D-pad aims, A/RB trigger, B/LB pedal, Back coin. Arrow keys aim too; 5 = coin" },
    ss22_snd_inputs,
    IN_TEST, IN_SERVICE,
    false,                                  /* no steering motor (the gun recoil is the aux PCB: not emulated) */
    true,                                   /* the light gun: the pointer aims (engine/ss22_input.c) */
};

static const ss22_press_name presses[] = {
    {"coin", IN_COIN}, {"trigger", IN_TRIGGER}, {"pedal", IN_PEDAL}, {"service", IN_SERVICE}, {"test", IN_TEST} };

/* TC_AUTOPLAY=test: the operator menu (test mode) walked at random, the way Dirt Dash's DD_AUTOPLAY=test does. The cabinet switch on from frame 300 (off for 120
 * frames every 4000 to restart the menu), then every 12..31 frames a new action held: a shot inside the screen (the menu's UP) or outside (DOWN), the pedal
 * (ENTER), service, coin. TC_TEST_SEED picks the walk. */
static void autoplay_test(long n, uint16_t *p)
{
    static unsigned long long seed; static int init, left; static uint16_t held; static int off;
    if (!init) { const char *e = getenv("TC_TEST_SEED"); seed = e ? strtoull(e, NULL, 0) : 12345; init = 1; }
    const long t = n - 300;
    if (t >= 0 && !(t >= 4000 && t % 4000 < 120)) *p |= IN_TEST;
    if (n > 400) {
        if (left > 0) left--;
        else {
            #define RND(k) (seed = (seed * 1103515245ull + 12345ull) % 2147483648ull, (unsigned)((seed / 65536) % (k)))
            const unsigned r = RND(100);
            held = 0; off = 0;
            if      (r < 25) held = IN_TRIGGER;                 /* inside: UP */
            else if (r < 45) { held = IN_TRIGGER; off = 1; }    /* outside: DOWN */
            else if (r < 70) held = IN_PEDAL;                   /* ENTER */
            else if (r < 82) held = IN_SERVICE;
            else if (r < 88) held = IN_COIN;
            left = 12 + (int)RND(20);
            #undef RND
        }
        *p |= held; g_ss22_gun_off = off != 0;
        g_ss22_gun_x = 68 + 313; g_ss22_gun_y = 43 + 120;
    }
}

/* tools/mame/cov_trace_play.lua, frame for frame (the gun itself is not scripted here: it stays at the ports' default, the middle of the screen) */
static void autoplay(long n, uint16_t *p, unsigned *wheel, unsigned *pedal1, unsigned *pedal2)
{
    (void)wheel; (void)pedal1; (void)pedal2;
    static int seed = -1, tst = -1; if (seed < 0) { const char *e = getenv("TC_SEED"); seed = e ? atoi(e) : 0; }
    if (tst < 0) { const char *e = getenv("TC_AUTOPLAY"); tst = e && !strcmp(e, "test"); }
    if (tst) { autoplay_test(n, p); return; }
    const long m = n % 7000;
    if ((m >= 300 && m < 306) || (m >= 340 && m < 346) || (m >= 380 && m < 386)) *p |= IN_COIN;     /* three coins: CREDIT n/3 */
    if (seed == 0) {
        if (n > 600 && n % 45 < 4) *p |= IN_TRIGGER;
        if (n > 1500 && n % 300 < 90) *p |= IN_PEDAL;
        return;
    }
    /* TC_SEED=k: a player's input -- the gun sweeps and jumps about (off-screen sometimes), trigger and pedal in random bursts */
    if (n > 600) {
        uint32_t h = (uint32_t)(n / 12) * 2654435761u ^ (uint32_t)seed * 40503u; h ^= h >> 15; h *= 2246822519u; h ^= h >> 13;
        g_ss22_gun_off = (h & 15) == 0;
        g_ss22_gun_x = (uint16_t)(68 + (h >> 4) % 626); g_ss22_gun_y = (uint16_t)(43 + (h >> 14) % 241);
        if ((h >> 3 & 3) == 0 && n % 12 < 5) *p |= IN_TRIGGER;
        if ((h >> 9 & 7) == 0) *p |= IN_PEDAL;
    }
}

/* TEST SWITCHES, to reach the later levels (environment variables, read once). Work RAM, found by dumping it every second
 * through a played session (--dump-every 60):
 *   0xE00796  the area timer, in frames (TIME 26.83 on screen = 1609); 0 = time over
 *   0xE00733  the mode: 0 booting, 2 the attract demo (which runs on the same timer), 3 a game
 *   0xE00791  lives left (the LIFE icons: set to 1, the HUD shows one)
 * TC_INF_TIME=1: in a game, the timer is topped up to 60 s whenever it falls under 10 s.
 * TC_INF_LIFE=1: in a game, a life lost is given back (up to the most this game has had, so an operator's 5 stays 5).
 * Never before frame 600: the power-on RAM test checks this memory, and a write during it fails the boot (WORK RAM NG). */
#define TC_W_TIMER 0x0796
#define TC_W_MODE  0x0733
#define TC_W_LIVES 0x0791
static uint16_t wram16(uint32_t a) { return (uint16_t)(g_ss22.wram[a] << 8 | g_ss22.wram[a + 1]); }
static void wram16_set(uint32_t a, uint16_t v) { g_ss22.wram[a] = (uint8_t)(v >> 8); g_ss22.wram[a + 1] = (uint8_t)v; }
static void tc_frame(long n)
{
    static int inf_time = -1, inf_life, most_lives;
    static int lives_log = -1, last_mode = -1, last_lives = -1;
    if (inf_time < 0) { inf_time = getenv("TC_INF_TIME") != NULL; inf_life = getenv("TC_INF_LIFE") != NULL; }
    if (lives_log < 0) lives_log = getenv("TC_LIVESLOG") != NULL;            /* TC_LIVESLOG=1: the mode and lives byte on every change */
    if (lives_log && n >= 600 && (g_ss22.wram[TC_W_MODE] != last_mode || g_ss22.wram[TC_W_LIVES] != last_lives)) {
        last_mode = g_ss22.wram[TC_W_MODE]; last_lives = g_ss22.wram[TC_W_LIVES];
        fprintf(stderr, "[LIVES] frame %ld mode %d lives %d\n", n, last_mode, last_lives);
    }
    if (n < 600 || g_ss22.wram[TC_W_MODE] != 3) { most_lives = 0; return; }
    if (inf_time) { const uint16_t t = wram16(TC_W_TIMER); if (t > 0 && t < 600) wram16_set(TC_W_TIMER, 3600); }
    if (inf_life) {
        const int l = g_ss22.wram[TC_W_LIVES];
        if (l > most_lives && l <= 9) most_lives = l;
        if (l < most_lives) g_ss22.wram[TC_W_LIVES] = (uint8_t)most_lives;
    }
}

/* THE STAGE SELECT (--stage 1|2|3): the game's own TIMED GAME, where the player picks a stage and has
 * unlimited lives, chosen on the cabinet's inputs -- three coins (a game costs three), a shot at TIMED GAME on SELECT GAME MODE, a shot at
 * the stage's box on the stage screen -- then the gun is the player's. Frame n counts from the script's start; from power-on it waits for the
 * boot first. The aim points are the boxes' centres, in the gun ports' units (X 68..694, Y 43..284 across the screen). */
#define AIM_TIMED_X 381
#define AIM_TIMED_Y 213
#define AIM_STAGE_Y 147
static const uint16_t aim_stage_x[3] = { 193, 382, 569 };
static bool start(const char *name, long n, uint16_t *p, unsigned *wheel, unsigned *pedal1, unsigned *pedal2)
{
    (void)wheel; (void)pedal1; (void)pedal2;
    const int st = name[0] >= '1' && name[0] <= '3' && !name[1] ? name[0] - '1' : -1;
    if (st < 0) return false;
    if (n < 0) return true;                                                  /* the name is good */
    static long t0, last = -1;
    if (last < 0 || n < last) t0 = n + (rr_frame < 300 ? 300 - (long)rr_frame : 0);   /* a new run; from power-on, after the boot (by frame ~180) */
    last = n;
    const long t = n - t0;
    if (t < 0) return true;
    if (t >= 900) return false;
    g_ss22_gun_off = false;
    if ((t < 6) || (t >= 40 && t < 46) || (t >= 80 && t < 86)) *p |= IN_COIN;
    if (t < 330) { g_ss22_gun_x = AIM_TIMED_X; g_ss22_gun_y = AIM_TIMED_Y; }  /* SELECT GAME MODE comes up ~120 frames after the first coin */
    else { g_ss22_gun_x = aim_stage_x[st]; g_ss22_gun_y = AIM_STAGE_Y; }     /* the stage screen, ~240 frames after that */
    if (t >= 140 && t % 40 < 4) *p |= IN_TRIGGER;                             /* a shot between the boxes hits nothing */
    return true;
}

static const eng_hud_mark hud_marks_none[] = { { 0, 0, 0 } };

static const ss22_game game = {
    .name = "Time Crisis", .tag = "TC", .lname = "tc", .logname = "timecris.log", .zip = "timecris.zip", .recoil_mask = 0x0002,   /* MAME: "Time Crisis: 1 = gun solenoid" (mcuout1) */
    .out_gain = 2.4,                         /* not measured for this game yet (Dirt Dash 2.4, Tokyo Wars 1.4) */
    .prune_shadow = true,                         /* the test-mode watchdog restarts the program from inside an IRQ handler (jmp $110D0) */
    .snd_poll_sync = true,                        /* SUBCPU START WAIT: the MCU pulses its handshake bit inside one slice */
    .board = &tc_board,
    .dsp = { { {"ts1ptrl0.18k", "ts1ptrl1.16k", "ts1ptrl2.15k"},
               {"ts1ptrm0.18j", "ts1ptrm1.16j", "ts1ptrm2.15j"},
               {"ts1ptru0.18f", "ts1ptru1.16f", "ts1ptru2.15f"} }, 3, tc_c25_exec },        /* three planes of three chips (Prop Cycle's shape); no busy-wait poll measured yet */
    .snd = { "ts1data.8k", { "ts1wavea.2l", "ts1waveb.1l" }, { 0, 0x800000 }, 0x1000000, 0xFFFF, { 1, 2 } },   /* MAME's c352 region: ts1wavea.2l at 0, ts1waveb.1l at 0x800000 */
    .video = { { "ts1cg0.8d", "ts1cg1.10d", "ts1cg2.12d", "ts1cg3.13d", "ts1cg4.14d", "ts1cg5.16d", "ts1cg6.18d", NULL },
               "ts1ccrl.3d", "ts1ccrh.1d",
               { "ts1scg0.12f", "ts1scg1.10f", "ts1scg2.8f", "ts1scg3.7f", "ts1scg4.5f", "ts1scg5.3f" }, 6, 0x1000000, 0xFF,
               false, false,                /* MAME: a 16 MB sprite region, ROMREGION_ERASEFF, six chips from 0 */
               .hud = { hud_marks_none, 0, 0x10, -1 } },
    .input = &input,
    .roms = k_roms, .n_roms = NROMS,
    .entry = L_110D0,
    .polls_per_frame = 67000,               /* measured on MAME's register trace at T3 */
    .presses = presses, .n_presses = (int)(sizeof presses / sizeof *presses),
    .autoplay = autoplay,
    .start = start, .start_names = "1, 2, 3",
    .frame = tc_frame,
};

int main(int argc, char **argv) { return ss22_main(argc, argv, &game); }
