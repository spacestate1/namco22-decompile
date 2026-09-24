/*
 * rr_romzip.c -- unpack the Rave Racer ROMs straight out of MAME's zips.
 *
 * So the game can be set up by dropping raverace.zip and namcoc74.zip (the
 * sound CPU's BIOS, c74.bin) beside it: on Windows there is no Python for
 * tools/setup_roms.py. At startup, if the ROM folder is incomplete, main()
 * calls rr_romzip_autosetup(), which looks for both zips, takes every chip it
 * needs by name, size and CRC, and writes them into the ROM folder.
 * Only top-level entries are taken: raverace.zip also carries the Japanese
 * program sets under raveracej/ and raveraceja/. The zip reader is Prop
 * Cycle's (../src/rom_zip.c); the chip list is tools/setup_roms.py's.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <sys/stat.h>
#include <zlib.h>
#include "rr_romzip.h"

static const struct { const char *name; uint32_t size; int opt; } k_roms[] = {
    {"c71.bin", 0x2000},
    {"c74.bin", 0x4000},
    {"rr1gam.2d", 0x100},
    {"rr1gam.3d", 0x100},
    {"rr1gam.4d", 0x100},
    {"rv1ccrh.5c", 0x80000},
    {"rv1ccrl.5a", 0x200000},
    {"rv1cg0.1a", 0x200000},
    {"rv1cg1.1c", 0x200000},
    {"rv1cg2.1d", 0x200000},
    {"rv1cg3.1e", 0x200000},
    {"rv1cg4.1f", 0x200000},
    {"rv1cg5.1j", 0x200000},
    {"rv1cg6.1k", 0x200000},
    {"rv1cg7.1n", 0x200000},
    {"rv1data.6r", 0x80000},
    {"rv1eeprm.9e", 0x2000},
    {"rv1potl0.5b", 0x80000},
    {"rv1potl1.4b", 0x80000},
    {"rv1potl2.3b", 0x80000},
    {"rv1potl3.2b", 0x80000},
    {"rv1potm0.5c", 0x80000},
    {"rv1potm1.4c", 0x80000},
    {"rv1potm2.3c", 0x80000},
    {"rv1potm3.2c", 0x80000},
    {"rv1potu0.5d", 0x80000},
    {"rv1potu1.4d", 0x80000},
    {"rv1potu2.3d", 0x80000},
    {"rv1potu3.2d", 0x80000},
    {"rv1wav0.10r", 0x100000},
    {"rv1wav1.10p", 0x100000},
    {"rv1wav2.10n", 0x100000},
    {"rv1wav3.10l", 0x100000},
    {"rv2_prgllb.4d", 0x80000},
    {"rv2_prglmb.2d", 0x80000},
    {"rv2_prgumb.8d", 0x80000},
    {"rv2_prguub.6d", 0x80000},
};
#define NROMS ((int)(sizeof k_roms / sizeof k_roms[0]))

static long file_size(const char *path)
{
    FILE *f = fopen(path, "rb");
    long n;
    if (!f) return -1;
    fseek(f, 0, SEEK_END); n = ftell(f); fclose(f);
    return n;
}

/* the first required chip missing from dir (or its size wrong), NULL when complete */
const char *rr_romzip_missing(const char *dir)
{
    char p[1024];
    for (int i = 0; i < NROMS; i++) {
        snprintf(p, sizeof p, "%s/%s", dir, k_roms[i].name);
        if (file_size(p) != (long)k_roms[i].size) return k_roms[i].name;
    }
    return NULL;
}

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static int rom_index(const char *name, size_t len)
{
    for (int i = 0; i < NROMS; i++) {
        size_t n = strlen(k_roms[i].name);
        if (n != len) continue;
        size_t k = 0;
        while (k < n && tolower((unsigned char)name[k]) == k_roms[i].name[k]) k++;
        if (k == n) return i;
    }
    return -1;
}

/* Unpack every chip this zip holds into dest_dir: the number taken, -1 when the
 * zip cannot be read. (Two zips make one set, so a missing chip is not an error
 * here -- rr_romzip_autosetup checks the folder afterwards.) */
int rr_romzip_extract(const char *zip_path, const char *dest_dir, char *err, size_t errlen)
{
    FILE *f = fopen(zip_path, "rb");
    uint8_t *z = NULL, *out = NULL;
    long zn;
    int ok = -1;
    int got[NROMS] = {0};
    if (!f) { snprintf(err, errlen, "cannot open %s", zip_path); return -1; }
    fseek(f, 0, SEEK_END); zn = ftell(f); fseek(f, 0, SEEK_SET);
    if (zn < 22 || !(z = malloc((size_t)zn)) || fread(z, 1, (size_t)zn, f) != (size_t)zn) {
        snprintf(err, errlen, "cannot read %s", zip_path);
        goto done;
    }

    /* End of central directory: the last "PK\5\6" within 64 KB of the end. */
    long eocd = -1;
    for (long i = zn - 22; i >= 0 && i >= zn - 22 - 0x10000; i--)
        if (rd32(z + i) == 0x06054b50) { eocd = i; break; }
    if (eocd < 0) { snprintf(err, errlen, "%s is not a zip file", zip_path); goto done; }
    uint32_t cd_n = rd16(z + eocd + 10), cd_off = rd32(z + eocd + 16);

    mkdir(dest_dir, 0755);      /* EEXIST is fine; win_compat maps it on Windows */
    uint32_t p = cd_off;
    for (uint32_t e = 0; e < cd_n; e++) {
        if ((long)p + 46 > zn || rd32(z + p) != 0x02014b50) break;
        uint32_t hdr = p;
        uint16_t method = rd16(z + p + 10);
        uint32_t csize = rd32(z + p + 20), usize = rd32(z + p + 24);
        uint16_t nlen = rd16(z + p + 28), xlen = rd16(z + p + 30), clen = rd16(z + p + 32);
        uint32_t lho = rd32(z + p + 42);
        const char *name = (const char *)(z + p + 46);
        int idx = memchr(name, '/', nlen) ? -1 : rom_index(name, nlen);
        p += 46 + nlen + xlen + clen;
        if (idx < 0 || usize != k_roms[idx].size) continue;
        if ((long)lho + 30 > zn || rd32(z + lho) != 0x04034b50) continue;
        uint32_t data = lho + 30 + rd16(z + lho + 26) + rd16(z + lho + 28);
        if ((long)data + csize > zn) continue;

        free(out);
        out = malloc(usize);
        if (!out) { snprintf(err, errlen, "out of memory"); goto done; }
        if (method == 0 && csize == usize) {
            memcpy(out, z + data, usize);
        } else if (method == 8) {
            z_stream s; memset(&s, 0, sizeof s);
            if (inflateInit2(&s, -MAX_WBITS) != Z_OK) continue;
            s.next_in = z + data; s.avail_in = csize;
            s.next_out = out;     s.avail_out = usize;
            int r = inflate(&s, Z_FINISH);
            inflateEnd(&s);
            if (r != Z_STREAM_END || s.total_out != usize) continue;
        } else {
            continue;
        }
        if (crc32(0L, out, usize) != rd32(z + hdr + 16)) continue;

        char path[1024];
        snprintf(path, sizeof path, "%s/%s", dest_dir, k_roms[idx].name);
        FILE *o = fopen(path, "wb");
        if (!o || fwrite(out, 1, usize, o) != usize) {
            if (o) fclose(o);
            snprintf(err, errlen, "cannot write %s", path);
            goto done;
        }
        fclose(o);
        got[idx] = 1;
    }

    ok = 0;
    for (int i = 0; i < NROMS; i++) ok += got[i];
done:
    free(out);
    free(z);
    fclose(f);
    return ok;
}

static int find_zip(const char *name, const char *exe_dir, char *out, size_t n)
{
    const char *where[4] = { "", "roms/", exe_dir, exe_dir };
    for (int i = 0; i < 4; i++) {
        if (i >= 2 && !(exe_dir && *exe_dir)) break;
        snprintf(out, n, i == 3 ? "%s/roms/%s" : i == 2 ? "%s/%s" : "%s%s",
                 i >= 2 ? exe_dir : where[i], name);
        if (file_size(out) > 0) return 1;
    }
    return 0;
}

bool rr_romzip_autosetup(const char *rom_dir, const char *exe_dir, char *err, size_t errlen)
{
    static const char *zips[] = { "raverace.zip", "namcoc74.zip" };
    char path[1024];
    int found = 0;
    for (int z = 0; z < 2; z++) {
        if (!find_zip(zips[z], exe_dir, path, sizeof path)) continue;
        found++;
        printf("Unpacking ROMs from %s into %s ...\n", path, rom_dir);
        if (rr_romzip_extract(path, rom_dir, err, errlen) < 0) return false;
    }
    const char *miss = rr_romzip_missing(rom_dir);
    if (!miss) { printf("ROMs unpacked.\n"); return true; }
    if (!found) snprintf(err, errlen, "no raverace.zip / namcoc74.zip found");
    else snprintf(err, errlen, "still missing %s -- raverace.zip and namcoc74.zip (the MAME sets) are both needed", miss);
    return false;
}
