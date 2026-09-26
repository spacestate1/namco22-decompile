#ifndef TW_ROM_H
#define TW_ROM_H
#include <stdbool.h>
#include <stddef.h>

/* Tokyo Wars' ROM chips (src/tw_rom.c). tw_rom_missing: the first chip missing from dir (or of the wrong size), NULL when the
 * folder is complete. tw_rom_setup: unpack tokyowar.zip (found in the current folder, roms/, or beside the program) into rom_dir. */
const char *tw_rom_missing(const char *dir);
bool tw_rom_setup(const char *rom_dir, const char *exe_dir, char *err, size_t errlen);
#endif
