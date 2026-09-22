/*
 * rom_zip.c -- unpack the ROMs straight out of MAME's propcycl.zip.
 *
 * So the game can be set up by dropping propcycl.zip beside it: on Windows
 * there is no Python for tools/setup_roms.py, and on Linux it saves a step.
 * At startup, if the ROM folder is incomplete, main() calls
 * rom_zip_autosetup(), which looks for propcycl.zip, checks every required
 * chip by name and size, and inflates them into the ROM folder.
 *
 * A zip is a list of files described by a central directory at its end.
 * Only top-level entries are taken: MAME's archive also carries the Japanese
 * program set under propcyclj/, which is a different program.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <sys/stat.h>
#include <zlib.h>
#include "rom_zip.h"

static const struct { const char *name; uint32_t size; } k_roms[] = {
    {"pr2ver-a.1", 0x100000}, {"pr2ver-a.2", 0x100000},
    {"pr2ver-a.3", 0x100000}, {"pr2ver-a.4", 0x100000},
    {"pr1ptrl0.18k", 0x80000}, {"pr1ptrl1.16k", 0x80000}, {"pr1ptrl2.15k", 0x80000},
    {"pr1ptrm0.18j", 0x80000}, {"pr1ptrm1.16j", 0x80000}, {"pr1ptrm2.15j", 0x80000},
    {"pr1ptru0.18f", 0x80000}, {"pr1ptru1.16f", 0x80000}, {"pr1ptru2.15f", 0x80000},
    {"pr1cg0.12b", 0x200000}, {"pr1cg1.10d", 0x200000}, {"pr1cg2.12d", 0x200000},
    {"pr1cg3.13d", 0x200000}, {"pr1cg4.14d", 0x200000}, {"pr1cg5.16d", 0x200000},
    {"pr1cg6.18a", 0x200000}, {"pr1cg7.15a", 0x200000},
    {"pr1ccrl.3d", 0x200000}, {"pr1ccrh.1d", 0x80000},
    {"pr1scg0.12f", 0x200000}, {"pr1scg1.10f", 0x200000},
    {"pr1data.8k", 0x80000},
    {"pr1wavea.2l", 0x400000}, {"pr1waveb.1l", 0x400000},
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

bool rom_dir_complete(const char *dir)
{
    char p[1024];
    for (int i = 0; i < NROMS; i++) {
        snprintf(p, sizeof p, "%s/%s", dir, k_roms[i].name);
        if (file_size(p) != (long)k_roms[i].size) return false;
    }
    return true;
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

bool rom_zip_extract(const char *zip_path, const char *dest_dir, char *err, size_t errlen)
{
    FILE *f = fopen(zip_path, "rb");
    uint8_t *z = NULL, *out = NULL;
    long zn;
    bool ok = false;
    int got[NROMS] = {0};
    if (!f) { snprintf(err, errlen, "cannot open %s", zip_path); return false; }
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

    for (int i = 0; i < NROMS; i++)
        if (!got[i]) {
            snprintf(err, errlen, "%s is not the Prop Cycle ROM set (missing or bad: %s)",
                     zip_path, k_roms[i].name);
            goto done;
        }
    ok = true;
done:
    free(out);
    free(z);
    fclose(f);
    return ok;
}

bool rom_zip_autosetup(const char *rom_dir, const char *exe_dir, char *err, size_t errlen)
{
    char cand[4][1024];
    int n = 0;
    snprintf(cand[n++], sizeof cand[0], "propcycl.zip");
    snprintf(cand[n++], sizeof cand[0], "roms/propcycl.zip");
    if (exe_dir && *exe_dir) {
        snprintf(cand[n++], sizeof cand[0], "%s/propcycl.zip", exe_dir);
        snprintf(cand[n++], sizeof cand[0], "%s/roms/propcycl.zip", exe_dir);
    }
    snprintf(err, errlen, "no propcycl.zip found");
    for (int i = 0; i < n; i++) {
        if (file_size(cand[i]) <= 0) continue;
        printf("Unpacking ROMs from %s into %s ...\n", cand[i], rom_dir);
        if (rom_zip_extract(cand[i], rom_dir, err, errlen)) {
            printf("ROMs unpacked.\n");
            return true;
        }
        fprintf(stderr, "ROM setup: %s\n", err);
        return false;
    }
    return false;
}
