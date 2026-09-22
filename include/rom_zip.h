#ifndef ROM_ZIP_H
#define ROM_ZIP_H
#include <stdbool.h>
#include <stddef.h>

/* Every chip the game needs is present in dir with the right size. */
bool rom_dir_complete(const char *dir);
/* Unpack the Prop Cycle chips from a MAME propcycl.zip into dest_dir,
 * checking name, size and CRC of each. On failure err says why. */
bool rom_zip_extract(const char *zip_path, const char *dest_dir, char *err, size_t errlen);
/* Look for propcycl.zip in the usual places (current folder, roms/, the
 * program's own folder) and unpack it into rom_dir. */
bool rom_zip_autosetup(const char *rom_dir, const char *exe_dir, char *err, size_t errlen);
#endif
