#ifndef RR_ROMZIP_H
#define RR_ROMZIP_H
#include <stdbool.h>
#include <stddef.h>
/* The first required chip missing from dir (or of the wrong size); NULL = complete. */
const char *rr_romzip_missing(const char *dir);
/* Unpack every Rave Racer chip one MAME zip holds into dest_dir (name, size and
 * CRC checked): the number taken, -1 when the zip cannot be read. */
int rr_romzip_extract(const char *zip_path, const char *dest_dir, char *err, size_t errlen);
/* Find raverace.zip, namcoc74.zip and namcoc71.zip (c71.bin, when raverace.zip lacks it; current folder, roms/, the program's folder
 * and its roms/) and unpack them into rom_dir; true when rom_dir is then complete. */
bool rr_romzip_autosetup(const char *rom_dir, const char *exe_dir, char *err, size_t errlen);
#endif
