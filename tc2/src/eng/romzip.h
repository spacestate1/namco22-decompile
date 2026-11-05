/* TIME CRISIS 2'S OWN COPY of namco-2x-systems engine/romzip.{c,h} (copied 2026-10-05). TC2 is independent of the System 22 project: change it here. */
/*
 * romzip.h -- unpack a game's ROMs straight out of MAME's zips (engine/romzip.c).
 *
 * So a game can be set up by dropping its zip(s) beside it: on Windows there is no Python for tools/setup_roms.py. At startup,
 * if the ROM folder is incomplete, the game calls eng_romzip_autosetup() with ITS chip list (name and size): every chip is taken
 * by name and size, checked against the zip's own CRC, and written into the ROM folder. A chip is taken from its expected path (the top
 * level, or the `zname` path for a chip a MAME set keeps under a clone's sub-folder) first, then from the top level, then under the same name in
 * any folder (a split set, a folder zipped whole, a zip made by hand) -- and, in a table that carries CRCs, by CRC32 + size whatever its name.
 * Every copy of each zip is read (the AppImage's folder, the current folder, roms/, the program's folder) until the set is complete, then any
 * other "<set>*.zip" / "namco*.zip" there (a clone's or a device's zip); a chip already unpacked is never replaced. ZIP64 archives are read. A chip can also name an `alt`: a different MAME version renamed it
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

#define ENG_ROM_HAS_CRC 1    /* this copy's table carries CRCs: romzip.c matches a chip by CRC32 + size whatever its name */
typedef struct { const char *name; uint32_t size; const char *zname; const char *alt; uint32_t crc; uint32_t crc_alt; void (*from_alt)(uint8_t *data, uint32_t n); } eng_rom_t;   /* crc: MAME's CRC32 of the chip (0 = not checked) -- TC2's copy */
    /* zname: the entry's PATH in the zip when it is not top-level ("dirtdasha/dt2vera.1"); NULL = top-level `name`.
       alt: an alternate top-level NAME a different MAME set uses for this same chip; NULL = none.
       crc_alt: a second dump that is just as good (0 = none) -- Angler King's I/O program: MAME's older sets carry asc3_io-c1.ic14
       (b6627a0e), current ones asc3_io-c.ic14 (2f272a7b) in namco_fca10.zip; the code is byte-identical, only the board's JVS name
       string differs, and the game refuses the newer name ("Please connect FCA-1 PCB."), so:
       from_alt: turns the crc_alt dump into the crc one in place; the result must then have CRC `crc` or the file is not taken. NULL = keep
       the crc_alt dump as it is. */

/* the first chip missing from dir (or of the wrong size), NULL when complete */
const char *eng_romzip_missing(const char *dir, const eng_rom_t *roms, int n);
/* every chip this zip holds into dest_dir: the number taken, -1 when the zip cannot be read */
int eng_romzip_extract(const char *zip_path, const char *dest_dir, const eng_rom_t *roms, int n, char *err, size_t errlen);
/* look for each zip (cwd, roms/, exe_dir, exe_dir/roms), unpack it, and check the folder is now complete. A name in `zips` ending in '*'
   is a PREFIX ("Angler King*"): any zip there starting with it is read too -- a download that bundles the set's zips in one zip.
   Zips inside a zip are read as well (one level deep). */
bool eng_romzip_autosetup(const char *rom_dir, const char *exe_dir, const char *const *zips, int nzips,
                          const eng_rom_t *roms, int n, char *err, size_t errlen);
#endif
