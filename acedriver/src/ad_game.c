/* Ace Driver: Racing Evolution (MAME `acedrive`, World AD2) on the shared System 22 runtime (../raverace/src) -- the ONLY thing this
 * game says about itself: which ROM chip is where, and the handful of board behaviours MAME's namcos22.cpp gives it. */
#include "rr_game.h"
#include "rr_lifted.h"

/* Developer screens left in the program: nothing reaches them (no absolute, PC-relative or table reference in the ROM); found
 * 2026-10-06 by their strings, run in MAME first (tools/mame/jump_to.lua with the Test switch held) and seeded into Ghidra
 * (ghidra/seeds/debug.calls). Each loops while the Test switch is on and leaves through 0x97E8 when it goes off. */
static const rr_debug_t ace_debug[] = {
    { "Object viewer",              0x5EFC, "A 3D model by CODE: position POSIX/Y/Z, rotation THX/Y/Z, polygon POLY." },
    { "Object viewer (sky)",        0x5F64, "The same over the sky backdrop, with the model's FLAG word." },
    { "Object viewer (offsets)",    0x5FEC, "A model with X/Z, offset OX/OY/OZ and rotation THX/THY." },
    { "Sprite viewer",              0x6070, "A sprite by BN: position, SIZE, colour mode CMODE, CZ, palette." },
    { "Character viewer",           0x60CE, "Every text-layer character: BASE / OFFSET / ATR over a hex grid." },
    { "Results screen test",        0x610C, "The race results / ranking screen on its own." },
};

/* A player's first run: every chip taken out of MAME's zips by name, size and CRC (engine/romzip.c), the list as
 * tools/setup_roms.py's (GAMES["acedrive"]) plus the sound BIOS. */
static const char *const ace_zips[] = { "acedrive.zip", "namcoc74.zip" };      /* (the DSP BIOS, c71.bin, is built into the engine) */
static const eng_rom_t ace_roms[] = {
    { "ad2_prgll.4d", 0x80000, NULL, NULL },
    { "ad2_prglm.2d", 0x80000, NULL, NULL },
    { "ad2_prgum.8d", 0x80000, NULL, NULL },
    { "ad2_prguu.6d", 0x80000, NULL, NULL },
    { "ad1data.6r", 0x80000, NULL, NULL },
    { "ad1cg0.1a", 0x200000, NULL, NULL },
    { "ad1cg1.2a", 0x200000, NULL, NULL },
    { "ad1cg2.3a", 0x200000, NULL, NULL },
    { "ad1cg3.5a", 0x200000, NULL, NULL },
    { "ad1ccrl.1c", 0x200000, NULL, NULL },
    { "ad1ccrh.2c", 0x80000, NULL, NULL },
    { "ad1potl0.5b", 0x80000, NULL, NULL },
    { "ad1potl1.4b", 0x80000, NULL, NULL },
    { "ad1potm0.5c", 0x80000, NULL, NULL },
    { "ad1potm1.4c", 0x80000, NULL, NULL },
    { "ad1potu0.5d", 0x80000, NULL, NULL },
    { "ad1potu1.4d", 0x80000, NULL, NULL },
    { "ad1wave0.10r", 0x100000, NULL, NULL },
    { "ad1wave1.10p", 0x100000, NULL, NULL },
    { "ad1wave2.10n", 0x100000, NULL, NULL },
    { "ad1wave3.10l", 0x100000, NULL, NULL },
    { "rr1gam.2d", 0x100, NULL, NULL },
    { "rr1gam.3d", 0x100, NULL, NULL },
    { "rr1gam.4d", 0x100, NULL, NULL },
    { "c74.bin", 0x4000, NULL, NULL },        /* the sound MCU BIOS: namcoc74.zip */
};

static const rr_game_t ace = {
    .name = "acedrive", .title = "Ace Driver", .cfg_file = "ad_controls.cfg", .nv_file = "ad_eeprom.nv", .entry = L_440,
    .prg = { "ad2_prguu.6d", "ad2_prgum.8d", "ad2_prglm.2d", "ad2_prgll.4d" },      /* ROM_LOAD32_BYTE offsets 0, 1, 2, 3 */
    .cg = { NULL, NULL, NULL, NULL, "ad1cg0.1a", "ad1cg1.2a", "ad1cg2.3a", "ad1cg3.5a" },   /* the four texture chips sit in the UPPER 8 MB */
    .ccrl = "ad1ccrl.1c", .ccrh = "ad1ccrh.2c", .tex_fixup = 1,             /* MAME init_tables: NAMCOS22_ACE_DRIVER */
    .pot = { { "ad1potl0.5b", "ad1potl1.4b" }, { "ad1potm0.5c", "ad1potm1.4c" }, { "ad1potu0.5d", "ad1potu1.4d" } },
    .pot_chips = 2,
    .snd_data = "ad1data.6r",
    .wav = { { "ad1wave0.10r", 0x000000 }, { "ad1wave2.10n", 0x100000 }, { "ad1wave1.10p", 0x200000 }, { "ad1wave3.10l", 0x300000 } },
    .gamma = { "rr1gam.2d", "rr1gam.3d", "rr1gam.4d" },
    .eeprom = NULL,                                   /* no default image in the set: the game initialises a blank EEPROM */
    .keycus_off = 3, .keycus_val = 0x0173,            /* namcos22_keycus_r: NAMCOS22_ACE_DRIVER, offset 3 */
    .steer_add = 2048, .gas_add = 992, .brake_add = 3008,   /* handle_driving_io: Ace Driver / Victory Lap steer + 2048 */
    .inputs_idle = 0xFFFF,                            /* INPUT_PORTS_START(acedrive): no cabinet field, every unused bit active-low 1; Motion-Stop 0x8000 idle */
    .steer_min = 0x200, .steer_max = 0xE00,           /* ADC.0 PORT_MINMAX(0x200, 0xe00) */
    .gas_max = 0x480, .brake_max = 0x240,             /* ADC.1 (0, 0x480), ADC.2 (0, 0x240) */
    .gas_delta = 120, .brake_delta = 80,              /* PORT_KEYDELTA(120) / (80) */
    .spin_pc = 0, .spin_op = 0,
    .debug = ace_debug, .ndebug = (int)(sizeof ace_debug / sizeof ace_debug[0]),
    .task_sp = 0x10000030, .task_sp_empty = 0x10000030,   /* A6-0x7FD0: the coroutine task stack (0x4C2C resets it to its own address) */
    .task_slot = 0x10000038,
    .zips = ace_zips, .nzips = 2, .roms = ace_roms, .nroms = (int)(sizeof ace_roms / sizeof ace_roms[0]),
    .log_file = "acedriver.log",                         /* A6-0x7FC8: the mode task's resume slot (0x4C36 / 0x4C48) */                       /* the master's busy-wait poll is not measured for this game yet */
};
const rr_game_t *g_rr_game = &ace;
