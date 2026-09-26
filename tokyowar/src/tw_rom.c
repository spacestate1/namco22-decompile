/*
 * tw_rom.c -- Tokyo Wars' ROM chips, and setting them up from MAME's zip.
 *
 * The chip list is tools/setup_roms.py's REQUIRED (World, TW2 Ver.A): names and sizes, checked before anything is written.
 * tokyowar.zip is the whole set (the sound BIOS c71.bin and the default EEPROM included); its Japanese program set under
 * tokyowarj/ is not taken. The zip reader is the shared engine's (engine/romzip.c).
 */
#include "romzip.h"
#include "tw_rom.h"

static const eng_rom_t k_roms[] = {
    {"c71.bin", 0x2000},
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
    {"tw2ver-a.1", 0x100000},
    {"tw2ver-a.2", 0x100000},
    {"tw2ver-a.3", 0x100000},
    {"tw2ver-a.4", 0x100000},
};
#define NROMS ((int)(sizeof k_roms / sizeof k_roms[0]))

const char *tw_rom_missing(const char *dir) { return eng_romzip_missing(dir, k_roms, NROMS); }

bool tw_rom_setup(const char *rom_dir, const char *exe_dir, char *err, size_t errlen)
{
    static const char *zips[] = { "tokyowar.zip" };
    return eng_romzip_autosetup(rom_dir, exe_dir, zips, 1, k_roms, NROMS, err, errlen);
}
