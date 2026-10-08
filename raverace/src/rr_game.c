/* Rave Racer's table: what this runtime did before it was made per-game (byte-identical by construction). */
#include "rr_game.h"
#include "rr_lifted.h"

static const rr_game_t rave = {
    .name = "raverace", .title = "Rave Racer", .log_file = "raveracer.log", .cfg_file = "rr_controls.cfg", .nv_file = "rr_eeprom.nv", .entry = L_4000, .autosetup = 1,
    .prg = { "rv2_prguub.6d", "rv2_prgumb.8d", "rv2_prglmb.2d", "rv2_prgllb.4d" },
    .cg = { "rv1cg0.1a", "rv1cg1.1c", "rv1cg2.1d", "rv1cg3.1e", "rv1cg4.1f", "rv1cg5.1j", "rv1cg6.1k", "rv1cg7.1n" },
    .ccrl = "rv1ccrl.5a", .ccrh = "rv1ccrh.5c",
    .pot = { { "rv1potl0.5b", "rv1potl1.4b", "rv1potl2.3b", "rv1potl3.2b" },
             { "rv1potm0.5c", "rv1potm1.4c", "rv1potm2.3c", "rv1potm3.2c" },
             { "rv1potu0.5d", "rv1potu1.4d", "rv1potu2.3d", "rv1potu3.2d" } },
    .pot_chips = 4,
    .snd_data = "rv1data.6r",
    .wav = { { "rv1wav0.10r", 0x000000 }, { "rv1wav2.10n", 0x100000 }, { "rv1wav1.10p", 0x200000 }, { "rv1wav3.10l", 0x300000 } },
    .gamma = { "rr1gam.2d", "rr1gam.3d", "rr1gam.4d" },
    .eeprom = "rv1eeprm.9e",
    .keycus_off = -1,
    .steer_add = 32, .gas_add = 992, .brake_add = 3008,
    .inputs_idle = 0xFEFF, .steer_min = 0x280, .steer_max = 0xD80, .gas_max = 0x610, .brake_max = 0x610, .gas_delta = 160, .brake_delta = 160,
    .spin_pc = 0x452D, .spin_op = 0x2000,
};
const rr_game_t *g_rr_game = &rave;
