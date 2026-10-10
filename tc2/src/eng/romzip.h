/* TIME CRISIS 2'S OWN COPY of namco-2x-systems engine/romzip.{c,h} (copied 2026-10-05). TC2 is independent of the System 22 project: change it here. */
/*
 * romzip.h -- unpack a game's ROMs straight out of MAME's zips (engine/romzip.c).
 *
 * So a game can be set up by dropping its zip(s) beside it: on Windows there is no Python for tools/setup_roms.py. At startup,
 * if the ROM folder is incomplete, the game calls eng_romzip_autosetup() with ITS chip list (name and size): every chip is taken
 * by name and size, checked against the zip's own CRC, and written into the ROM folder. Only top-level zip entries are taken, unless a chip names its
 * zip path (`zname`): a MAME set also carries its other program sets under sub-folders. Such a chip is taken from that path first, then from the top
 * level, then under the same name in any folder (a split set, a zip made by hand). A chip can also name an `alt`: a different MAME version renamed it
 * (Tokyo Wars' program chips were `tw2ver-a.1`..`.4`, current MAME sets ship them as `tw2vera.1`..`.4`, same data) -- either spelling is taken and
 * always written to disk under the chip's own `name`, so nothing downstream needs to know a rename ever happened. A chip that is in the zip but
 * unusable (wrong size, damaged, an unsupported compression method) says so in the error. The same code as Rave Racer's src/rr_romzip.c, with the
 * chip list and the zip names supplied by the game; Rave Racer keeps its own copy for now.
 */
#ifndef ENG_ROMZIP_H
#define ENG_ROMZIP_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct { const char *name; uint32_t size; const char *zname; const char *alt; uint32_t crc; } eng_rom_t;   /* crc: MAME's CRC32 of the chip (0 = not checked) -- TC2's copy */
    /* zname: the entry's PATH in the zip when it is not top-level ("dirtdasha/dt2vera.1"); NULL = top-level `name`.
       alt: an alternate top-level NAME a different MAME set uses for this same chip; NULL = none. */

/* the first chip missing from dir (or of the wrong size), NULL when complete */
const char *eng_romzip_missing(const char *dir, const eng_rom_t *roms, int n);
/* every chip this zip holds into dest_dir: the number taken, -1 when the zip cannot be read */
int eng_romzip_extract(const char *zip_path, const char *dest_dir, const eng_rom_t *roms, int n, char *err, size_t errlen);
/* look for each zip (cwd, roms/, exe_dir, exe_dir/roms), unpack it, and check the folder is now complete */
bool eng_romzip_autosetup(const char *rom_dir, const char *exe_dir, const char *const *zips, int nzips,
                          const eng_rom_t *roms, int n, char *err, size_t errlen);
#endif
