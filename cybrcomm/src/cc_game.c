/* Cyber Commando (MAME `cybrcomm`, Japan CY1) on the shared System 22 runtime (../raverace/src) -- the ONLY thing this game says about
 * itself: which ROM chip is where, and the handful of board behaviours MAME's namcos22.cpp gives it. */
#include "rr_game.h"
#include "rr_lifted.h"
#include "rr_input.h"

/* A player's first run: every chip taken out of MAME's zips by name, size and CRC (engine/romzip.c), the list as
 * ../acedriver/tools/setup_roms.py's GAMES["cybrcomm"] plus the sound BIOS. The board carries a second copy of every texture and
 * tilemap chip (.6a .7a .8a .9a, .8c .7c): identical, not needed. */
static const char *const cc_zips[] = { "cybrcomm.zip", "namcoc74.zip" };
static const eng_rom_t cc_roms[] = {
    { "cy1prgll.4d", 0x80000, NULL, NULL },
    { "cy1prglm.2d", 0x80000, NULL, NULL },
    { "cy1prgum.8d", 0x80000, NULL, NULL },
    { "cy1prguu.6d", 0x80000, NULL, NULL },
    { "cy1data.6r", 0x20000, NULL, NULL },
    { "cyc1cg0.1a", 0x200000, NULL, NULL },
    { "cyc1cg1.2a", 0x200000, NULL, NULL },
    { "cyc1cg2.3a", 0x200000, NULL, NULL },
    { "cyc1cg3.5a", 0x200000, NULL, NULL },
    { "cyc1ccrl.1c", 0x100000, NULL, NULL },
    { "cyc1ccrh.2c", 0x80000, NULL, NULL },
    { "cyc1ptl0.5b", 0x80000, NULL, NULL },
    { "cyc1ptl1.4b", 0x80000, NULL, NULL },
    { "cyc1ptl2.3b", 0x80000, NULL, NULL },
    { "cyc1ptm0.5c", 0x80000, NULL, NULL },
    { "cyc1ptm1.4c", 0x80000, NULL, NULL },
    { "cyc1ptm2.3c", 0x80000, NULL, NULL },
    { "cyc1ptu0.5d", 0x80000, NULL, NULL },
    { "cyc1ptu1.4d", 0x80000, NULL, NULL },
    { "cyc1ptu2.3d", 0x80000, NULL, NULL },
    { "cy1wav0.10r", 0x100000, NULL, NULL },
    { "cy1wav1.10p", 0x100000, NULL, NULL },
    { "cy1wav2.10n", 0x100000, NULL, NULL },
    { "cy1wav3.10l", 0x100000, NULL, NULL },
    { "rr1gam.2d", 0x100, NULL, NULL },
    { "rr1gam.3d", 0x100, NULL, NULL },
    { "rr1gam.4d", 0x100, NULL, NULL },
    { "cy1eeprm.9e", 0x2000, NULL, NULL },
    { "c74.bin", 0x4000, NULL, NULL },        /* the sound MCU BIOS: namcoc74.zip */
};

/* the Controls page in this game's words: the driving actions are the tank controls (rr_game_t.twin_stick), the shift buttons hit
 * INPUTS bits 0x0001 / 0x0002 = the gun trigger and the missile button (on both sticks of the cabinet) */
static const char *const cc_labels[RR_ACT_N] = {
    [RR_SHIFT_DOWN] = "Gun trigger", [RR_SHIFT_UP] = "Missile", [RR_VIEW] = "View change",
    [RR_STEER_LEFT] = "Turn left (Shift: strafe)", [RR_STEER_RIGHT] = "Turn right (Shift: strafe)",
    [RR_GAS] = "Forward (both sticks)", [RR_BRAKE] = "Back (both sticks)" };
/* Cyber Sled's keys and pad buttons (the same kind of cabinet: two sticks, a trigger and a missile button on each), replacing
 * the driving defaults; E/D/S/F and I/K/J/L, each stick on its own, are fixed in rr_host.c twin_stick_inputs. */
static const struct rr_bind_default cc_binds[] = {
    { RR_SHIFT_DOWN, "Z", "a" }, { RR_SHIFT_UP, "X", "b" }, { RR_VIEW, "C", "y" },
    { RR_GAS, "Up", "dpup" }, { RR_BRAKE, "Down", "dpdown" },
    { RR_STEER_LEFT, "Left", "dpleft" }, { RR_STEER_RIGHT, "Right", "dpright" } };

static const rr_game_t cc = {
    .name = "cybrcomm", .title = "Cyber Commando", .cfg_file = "cc_controls.cfg", .nv_file = "cc_eeprom.nv", .entry = L_4000,
    .prg = { "cy1prguu.6d", "cy1prgum.8d", "cy1prglm.2d", "cy1prgll.4d" },      /* ROM_LOAD32_BYTE offsets 0, 1, 2, 3 */
    .cg = { NULL, NULL, NULL, NULL, "cyc1cg0.1a", "cyc1cg1.2a", "cyc1cg2.3a", "cyc1cg3.5a" },   /* the upper 8 MB, as Ace Driver */
    .ccrl = "cyc1ccrl.1c", .ccrh = "cyc1ccrh.2c", .tex_fixup = 1,           /* 1 MB tile map, the rest of its 2 MB slot zero (MAME) */
    .pot = { { "cyc1ptl0.5b", "cyc1ptl1.4b", "cyc1ptl2.3b" }, { "cyc1ptm0.5c", "cyc1ptm1.4c", "cyc1ptm2.3c" },
             { "cyc1ptu0.5d", "cyc1ptu1.4d", "cyc1ptu2.3d" } },
    .pot_chips = 3,
    .snd_data = "cy1data.6r",                         /* 128 KB, mirrored x4 into the 512 KB region (MAME ROM_RELOAD) */
    .wav = { { "cy1wav0.10r", 0x000000 }, { "cy1wav2.10n", 0x100000 }, { "cy1wav1.10p", 0x200000 }, { "cy1wav3.10l", 0x300000 } },
    .gamma = { "rr1gam.2d", "rr1gam.3d", "rr1gam.4d" },
    .eeprom = "cy1eeprm.9e",                          /* the set's default EEPROM image */
    .keycus_off = 1, .keycus_val = 0x0185,            /* namcos22_keycus_r: NAMCOS22_CYBER_COMMANDO, offset 1 */
    .twin_stick = 1, .act_label = cc_labels, .bind_defaults = cc_binds, .nbind_defaults = (int)(sizeof cc_binds / sizeof cc_binds[0]),          /* handle_cybrcomm_io: four 8-bit stick ADCs x 0x10 at shared RAM 0x32..0x38 */
    .steer_add = 0, .gas_add = 0, .brake_add = 0,
    .inputs_idle = 0xFFFF,                            /* INPUT_PORTS_START(cybrcomm): every bit active-low, nothing pressed */
    .steer_min = 0x470, .steer_max = 0xB70, .gas_max = 0, .brake_max = 0, .gas_delta = 0, .brake_delta = 0,
    .spin_pc = 0, .spin_op = 0,                       /* the master's busy-wait poll is not measured for this game yet */
    .zips = cc_zips, .nzips = 2, .roms = cc_roms, .nroms = (int)(sizeof cc_roms / sizeof cc_roms[0]),
    .log_file = "cybrcomm.log",
};
const rr_game_t *g_rr_game = &cc;
