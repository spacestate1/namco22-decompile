/*
 * romzip.h -- unpack a game's ROMs straight out of MAME's zips (engine/romzip.c).
 *
 * So a game can be set up by dropping its zip(s) beside it: on Windows there is no Python for tools/setup_roms.py. At startup,
 * if the ROM folder is incomplete, the game calls eng_romzip_autosetup() with ITS chip list (name and size): every chip is taken
 * by name and size, checked against the zip's own CRC, and written into the ROM folder. Only top-level zip entries are taken (a
 * MAME set also carries its Japanese program sets under sub-folders). The same code as Rave Racer's src/rr_romzip.c, with the
 * chip list and the zip names supplied by the game; Rave Racer keeps its own copy for now.
 */
#ifndef ENG_ROMZIP_H
#define ENG_ROMZIP_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct { const char *name; uint32_t size; } eng_rom_t;

/* the first chip missing from dir (or of the wrong size), NULL when complete */
const char *eng_romzip_missing(const char *dir, const eng_rom_t *roms, int n);
/* every chip this zip holds into dest_dir: the number taken, -1 when the zip cannot be read */
int eng_romzip_extract(const char *zip_path, const char *dest_dir, const eng_rom_t *roms, int n, char *err, size_t errlen);
/* look for each zip (cwd, roms/, exe_dir, exe_dir/roms), unpack it, and check the folder is now complete */
bool eng_romzip_autosetup(const char *rom_dir, const char *exe_dir, const char *const *zips, int nzips,
                          const eng_rom_t *roms, int n, char *err, size_t errlen);
#endif
