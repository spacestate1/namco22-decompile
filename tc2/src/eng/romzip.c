/* TIME CRISIS 2'S OWN COPY of namco-2x-systems engine/romzip.{c,h} (copied 2026-10-05). TC2 is independent of the System 22 project: change it here. */
/* romzip.c -- see romzip.h. Rave Racer's src/rr_romzip.c with the chip list and the zip names supplied by the game. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <sys/stat.h>
#include <dirent.h>
#include <zlib.h>
#include "romzip.h"

static long file_size(const char *path)
{
    FILE *f = fopen(path, "rb");
    long n;
    if (!f) return -1;
    fseek(f, 0, SEEK_END); n = ftell(f); fclose(f);
    return n;
}

/* the first required chip missing from dir (or its size wrong), NULL when complete */
const char *eng_romzip_missing(const char *dir, const eng_rom_t *k_roms, int NROMS)
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
static uint64_t rd64(const uint8_t *p) { return (uint64_t)rd32(p) | (uint64_t)rd32(p + 4) << 32; }

/* WHY a chip that WAS in a zip was not taken, kept PER CHIP (GitHub #45: with one shared slot, a chip rejected early was later reported
 * as "no file of that name" once another chip's rejection had overwritten it), so "still missing X" says what is actually wrong with X. */
#define WHY_MAX 256
static char g_why[WHY_MAX][240];
#define WHY(i, ...) do { if ((i) < WHY_MAX) snprintf(g_why[i], sizeof g_why[i], __VA_ARGS__); } while (0)

static bool path_is(const char *name, size_t len, const char *want)      /* the zip entry `name` equals the (lower-case) path `want`, ignoring case */
{
    size_t n = strlen(want);
    if (n != len) return false;
    for (size_t k = 0; k < n; k++) if (tolower((unsigned char)name[k]) != tolower((unsigned char)want[k])) return false;
    return true;
}

/* Which chip is this zip entry, and how well does its place in the zip fit: rank 3 = the chip's CRC32 and size (whatever the entry is called:
 * a renamed chip or a hand-made zip -- only for a table that carries CRCs), 2 = the path the chip is expected at (or its `alt` spelling
 * anywhere -- a renamed chip is exactly as good a match as the name we expect), 1 = the top level, 0 = the same file name in some other folder.
 * A MAME set keeps a program set under its own name (dirtdasha/dt2vera.1), but a split set, a zip somebody made by hand (a folder zipped
 * whole: timecrs2/tss3verb.1), one packed with backslashes, or one that wraps the set in a folder does not -- and the Japanese sets carry
 * differently NAMED chips, so the name alone never picks a wrong program. The top level always wins over a sub-folder, so a merged set's
 * clone folders are only used when the parent's own copy is absent. -1 = not a chip this game needs. */
static int rom_index(const char *name, size_t len, uint32_t ecrc, uint32_t esize, const eng_rom_t *k_roms, int NROMS, int *rank)
{
    (void)ecrc; (void)esize;
#ifdef ENG_ROM_HAS_CRC
    for (int i = 0; i < NROMS; i++)
        if (k_roms[i].size == esize && ((k_roms[i].crc && k_roms[i].crc == ecrc) || (k_roms[i].crc_alt && k_roms[i].crc_alt == ecrc))) { *rank = 3; return i; }
#endif
    size_t b = len;
    while (b > 0 && name[b - 1] != '/' && name[b - 1] != '\\') b--;
    int hit = -1;
    for (int i = 0; i < NROMS; i++) {
        const char *want = k_roms[i].zname ? k_roms[i].zname : k_roms[i].name;     /* the entry's full path */
        if (path_is(name, len, want)) { *rank = 2; return i; }
        if (k_roms[i].alt && path_is(name, len, k_roms[i].alt)) { *rank = 2; return i; }
        if (k_roms[i].zname && path_is(name, len, k_roms[i].name)) { *rank = 1; return i; }
        if (hit < 0 && b > 0 && (path_is(name + b, len - b, k_roms[i].name) || (k_roms[i].alt && path_is(name + b, len - b, k_roms[i].alt)))) hit = i;
    }
    if (hit >= 0) *rank = 0;
    return hit;
}

/* Unpack every chip this zip holds into dest_dir: the number taken, -1 when the zip cannot be read. (Several zips make one set, so a
 * missing chip is not an error here -- eng_romzip_autosetup checks the folder afterwards.) A chip already in dest_dir at its right size is
 * left alone, so a second zip (a stale copy, a clone's zip) never replaces what the first one supplied. Reads plain and ZIP64 archives
 * (`zip -fz`, 7-Zip and RomVault write ZIP64 records), entries with data descriptors, Store and Deflate. */
static int extract_mem(const uint8_t *z, long zn, const char *zip_path, const char *dest_dir, const eng_rom_t *k_roms, int NROMS, char *err, size_t errlen, int depth);
static bool ends_zip(const char *name, size_t len)
{
    return len > 4 && name[len - 4] == '.' && tolower((unsigned char)name[len - 3]) == 'z' && tolower((unsigned char)name[len - 2]) == 'i' && tolower((unsigned char)name[len - 1]) == 'p';
}
int eng_romzip_extract(const char *zip_path, const char *dest_dir, const eng_rom_t *k_roms, int NROMS, char *err, size_t errlen)
{
    FILE *f = fopen(zip_path, "rb");
    if (!f) { snprintf(err, errlen, "cannot open %s", zip_path); return -1; }
    fseek(f, 0, SEEK_END); const long zn = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *z = zn >= 22 ? malloc((size_t)zn) : NULL;
    int ok = -1;
    if (!z || fread(z, 1, (size_t)zn, f) != (size_t)zn) snprintf(err, errlen, "cannot read %s", zip_path);
    else ok = extract_mem(z, zn, zip_path, dest_dir, k_roms, NROMS, err, errlen, 0);
    free(z);
    fclose(f);
    return ok;
}
/* the same for a zip already in memory; depth 0 = a file on disk, 1 = a zip found inside it (not opened further) */
static int extract_mem(const uint8_t *z, long zn, const char *zip_path, const char *dest_dir, const eng_rom_t *k_roms, int NROMS, char *err, size_t errlen, int depth)
{
    uint8_t *out = NULL;
    int ok = -1;
    int *got = calloc((size_t)NROMS, sizeof *got), *best = malloc((size_t)NROMS * sizeof *best);
    if (!got || !best) { free(got); free(best); return -1; }
    for (int i = 0; i < NROMS; i++) {
        char p[1024];
        snprintf(p, sizeof p, "%s/%s", dest_dir, k_roms[i].name);
        best[i] = file_size(p) == (long)k_roms[i].size ? 99 : -1;     /* already there: nothing in this zip replaces it */
    }
    const char *zbase = zip_path;
    for (const char *c = zip_path; *c; c++) if (*c == '/' || *c == '\\') zbase = c + 1;
    if (zn < 22) { snprintf(err, errlen, "%s is not a zip file (or is damaged)", zip_path); goto done; }

    /* End of central directory: the last "PK\5\6" within 64 KB of the end; ZIP64 when its fields are saturated and a locator precedes it. */
    long eocd = -1;
    for (long i = zn - 22; i >= 0 && i >= zn - 22 - 0x10000; i--)
        if (rd32(z + i) == 0x06054b50) { eocd = i; break; }
    if (eocd < 0) { snprintf(err, errlen, "%s is not a zip file (or is damaged)", zip_path); goto done; }
    uint64_t cd_n = rd16(z + eocd + 10), cd_off = rd32(z + eocd + 16);
    if (eocd >= 20 && rd32(z + eocd - 20) == 0x07064b50) {
        uint64_t z64 = rd64(z + eocd - 20 + 8);
        if (z64 + 56 <= (uint64_t)zn && rd32(z + z64) == 0x06064b50) { cd_n = rd64(z + z64 + 32); cd_off = rd64(z + z64 + 48); }
    }

    mkdir(dest_dir, 0755);      /* EEXIST is fine; win_compat maps it on Windows */
    uint64_t p = cd_off;
    for (uint64_t e = 0; e < cd_n; e++) {
        if (p + 46 > (uint64_t)zn || rd32(z + p) != 0x02014b50) break;
        const uint64_t hdr = p;
        const uint16_t method = rd16(z + p + 10), gflags = rd16(z + p + 8);
        uint64_t csize = rd32(z + p + 20), usize = rd32(z + p + 24), lho = rd32(z + p + 42);
        const uint16_t nlen = rd16(z + p + 28), xlen = rd16(z + p + 30), clen = rd16(z + p + 32);
        const uint32_t ecrc = rd32(z + hdr + 16);
        const char *name = (const char *)(z + p + 46);
        if (p + 46 + nlen + xlen > (uint64_t)zn) break;
        for (const uint8_t *x = z + p + 46 + nlen, *xe = x + xlen; x + 4 <= xe; x += 4 + rd16(x + 2)) {   /* ZIP64 extended information */
            if (rd16(x) != 0x0001) continue;
            const uint8_t *v = x + 4, *ve = x + 4 + rd16(x + 2);
            if (usize == 0xFFFFFFFFu && v + 8 <= ve) { usize = rd64(v); v += 8; }
            if (csize == 0xFFFFFFFFu && v + 8 <= ve) { csize = rd64(v); v += 8; }
            if (lho == 0xFFFFFFFFu && v + 8 <= ve) { lho = rd64(v); v += 8; }
            break;
        }
        p += 46 + (uint64_t)nlen + xlen + clen;
        if (nlen && (name[nlen - 1] == '/' || name[nlen - 1] == '\\')) continue;     /* a folder */
        int rank = -1;
        int idx = rom_index(name, nlen, ecrc, (uint32_t)usize, k_roms, NROMS, &rank);
        if (idx < 0 && depth == 0 && ends_zip(name, nlen) && usize >= 22 && usize < ((uint64_t)1 << 31) && !(gflags & 1)
            && lho + 30 <= (uint64_t)zn && rd32(z + lho) == 0x04034b50) {        /* a zip in the zip (a download bundling aking.zip + namco_fca10.zip) */
            const uint64_t data = lho + 30 + rd16(z + lho + 26) + rd16(z + lho + 28);
            uint8_t *inner = data + csize <= (uint64_t)zn ? malloc(usize) : NULL;
            int fine = 0;
            if (inner && method == 0 && csize == usize) { memcpy(inner, z + data, usize); fine = 1; }
            else if (inner && method == 8) {
                z_stream s; memset(&s, 0, sizeof s);
                if (inflateInit2(&s, -MAX_WBITS) == Z_OK) {
                    s.next_in = (Bytef *)(z + data); s.avail_in = (uInt)csize; s.next_out = inner; s.avail_out = (uInt)usize;
                    fine = inflate(&s, Z_FINISH) == Z_STREAM_END && s.total_out == usize;
                    inflateEnd(&s);
                }
            }
            if (fine && crc32(0L, inner, (uInt)usize) == ecrc) {
                char inner_name[600]; snprintf(inner_name, sizeof inner_name, "%s/%.*s", zip_path, (int)nlen, name);
                printf("  reading %.*s inside %s\n", (int)nlen, name, zbase);
                char e2[300] = "";
                if (extract_mem(inner, (long)usize, inner_name, dest_dir, k_roms, NROMS, e2, sizeof e2, 1) < 0 && (!strncmp(e2, "cannot write", 12) || !strncmp(e2, "out of memory", 13))) {
                    snprintf(err, errlen, "%s", e2); free(inner); goto done;
                }
                for (int i = 0; i < NROMS; i++) {      /* what the inner zip supplied is now on disk */
                    char pp[1024]; snprintf(pp, sizeof pp, "%s/%s", dest_dir, k_roms[i].name);
                    if (file_size(pp) == (long)k_roms[i].size) { if (best[i] < 99) got[i] = 1; best[i] = 99; }
                }
            }
            free(inner);
            continue;
        }
        if (idx < 0 || rank <= best[idx]) continue;                  /* not ours, or a better-placed copy is already taken */
        if (usize != k_roms[idx].size) {
            WHY(idx, "\"%.*s\" in %s is %llu bytes, not the %u bytes this game uses (a different or damaged dump)", (int)nlen, name, zbase,
                (unsigned long long)usize, k_roms[idx].size);
            continue;
        }
        if (gflags & 1) { WHY(idx, "\"%.*s\" in %s is encrypted (password-protected zip)", (int)nlen, name, zbase); continue; }
        if (lho + 30 > (uint64_t)zn || rd32(z + lho) != 0x04034b50) { WHY(idx, "\"%.*s\" in %s: the zip is damaged (bad local header)", (int)nlen, name, zbase); continue; }
        const uint64_t data = lho + 30 + rd16(z + lho + 26) + rd16(z + lho + 28);
        if (data + csize > (uint64_t)zn) { WHY(idx, "\"%.*s\" in %s: the zip is truncated (an incomplete download or copy?)", (int)nlen, name, zbase); continue; }

        free(out);
        out = malloc(usize ? usize : 1);
        if (!out) { snprintf(err, errlen, "out of memory"); goto done; }
        if (method == 0 && csize == usize) {
            memcpy(out, z + data, usize);
        } else if (method == 8) {
            z_stream s; memset(&s, 0, sizeof s);
            if (inflateInit2(&s, -MAX_WBITS) != Z_OK) continue;
            s.next_in = (Bytef *)(z + data); s.avail_in = (uInt)csize;
            s.next_out = out;     s.avail_out = (uInt)usize;
            int r = inflate(&s, Z_FINISH);
            inflateEnd(&s);
            if (r != Z_STREAM_END || s.total_out != usize) {
                WHY(idx, "\"%.*s\" in %s cannot be decompressed (the zip is damaged)", (int)nlen, name, zbase);
                continue;
            }
        } else {
            WHY(idx, "\"%.*s\" in %s is compressed with method %u%s, which this program cannot read (re-zip it with ordinary Deflate or Store)",
                (int)nlen, name, zbase, (unsigned)method, method == 9 ? " (Deflate64, 7-Zip's \"Ultra\")" : method == 14 ? " (LZMA)" : method == 12 ? " (BZip2)" : "");
            continue;
        }
        if (crc32(0L, out, (uInt)usize) != ecrc) {
            WHY(idx, "\"%.*s\" in %s fails its checksum (the zip is damaged)", (int)nlen, name, zbase);
            continue;
        }
#ifdef ENG_ROM_HAS_CRC
        if (k_roms[idx].crc && ecrc != k_roms[idx].crc && !(k_roms[idx].crc_alt && ecrc == k_roms[idx].crc_alt)) {   /* a sound zip, but not the dump this game was made from */
            WHY(idx, "\"%.*s\" in %s is not the expected dump (CRC32 %08X, MAME's is %08X): a different version or a bad dump", (int)nlen, name, zbase,
                (unsigned)ecrc, (unsigned)k_roms[idx].crc);
            continue;
        }
        if (k_roms[idx].crc_alt && ecrc == k_roms[idx].crc_alt && ecrc != k_roms[idx].crc && k_roms[idx].from_alt) {   /* the other dump: made into ours */
            k_roms[idx].from_alt(out, (uint32_t)usize);
            if (crc32(0L, out, (uInt)usize) != k_roms[idx].crc) {
                WHY(idx, "\"%.*s\" in %s could not be converted to the dump this game uses (CRC32 %08X)", (int)nlen, name, zbase, (unsigned)k_roms[idx].crc);
                continue;
            }
            printf("  %.*s: the CRC32 %08X dump, converted to %08X (%s)\n", (int)nlen, name, (unsigned)ecrc, (unsigned)k_roms[idx].crc, k_roms[idx].name);
        }
#endif

        char path[1024];
        snprintf(path, sizeof path, "%s/%s", dest_dir, k_roms[idx].name);
        FILE *o = fopen(path, "wb");
        if (!o || fwrite(out, 1, usize, o) != usize) {
            if (o) fclose(o);
            snprintf(err, errlen, "cannot write %s", path);
            goto done;
        }
        fclose(o);
        got[idx] = 1; best[idx] = rank;
    }

    ok = 0;
    for (int i = 0; i < NROMS; i++) ok += got[i];
done:
    free(got);
    free(best);
    free(out);
    return ok;
}

/* Where a player's zips are looked for: for an AppImage, whose program folder is inside the read-only image, the folder the .AppImage itself
 * is in ($APPIMAGE) and its roms/ (GitHub #45, #8); then the current folder, roms/ there, the program's folder and its roms/. */
static int search_dirs(const char *exe_dir, char dirs[6][1024])
{
    int n = 0;
    const char *ai = getenv("APPIMAGE");      /* first: the zip the player put beside the AppImage beats an older copy the launcher made */
    if (ai && *ai) {
        char d[1000]; snprintf(d, sizeof d, "%s", ai);
        char *s = strrchr(d, '/');
        if (s) { *s = 0; snprintf(dirs[n++], 1024, "%s", *d ? d : "/"); snprintf(dirs[n++], 1024, "%s/roms", d); }
    }
    snprintf(dirs[n++], 1024, ".");
    snprintf(dirs[n++], 1024, "roms");
    if (exe_dir && *exe_dir) { snprintf(dirs[n++], 1024, "%.1000s", exe_dir); snprintf(dirs[n++], 1024, "%.1000s/roms", exe_dir); }
    return n;
}

static bool same_file(const char *a, const char *b)
{
    struct stat sa, sb;
    if (stat(a, &sa) || stat(b, &sb)) return false;
#ifdef _WIN32
    return sa.st_size == sb.st_size && sa.st_mtime == sb.st_mtime && !strcmp(a, b);
#else
    return sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
#endif
}

static bool name_ieq_zip(const char *fname, const char *stem)    /* fname is "<stem>*.zip", any case */
{
    size_t fl = strlen(fname), sl = strlen(stem);
    if (fl < 4 || fl < sl + 4) return false;
    for (size_t k = 0; k < sl; k++) if (tolower((unsigned char)fname[k]) != tolower((unsigned char)stem[k])) return false;
    const char *x = fname + fl - 4;
    return x[0] == '.' && tolower((unsigned char)x[1]) == 'z' && tolower((unsigned char)x[2]) == 'i' && tolower((unsigned char)x[3]) == 'p';
}

bool eng_romzip_autosetup(const char *rom_dir, const char *exe_dir, const char *const *zips, int nzips,
                          const eng_rom_t *roms, int n, char *err, size_t errlen)
{
    char dirs[6][1024], path[1024], read_list[600] = "";
    char done_paths[24][1024];
    int ndone = 0, found = 0;
    const int nd = search_dirs(exe_dir, dirs);
    memset(g_why, 0, sizeof g_why);

    /* every copy of every named zip, in every place (not only the first: a stale copy in roms/ must not hide a good one beside the game),
     * until the set is complete; then any other zip of the same set in those places (a clone's or a device's zip under another name,
     * timecrs2*.zip, dirtdash*.zip, namco*.zip) -- each chip still checked by name or CRC, size and the zip's own CRC. */
    for (int pass = 0; pass < 2 && eng_romzip_missing(rom_dir, roms, n); pass++)
        for (int d = 0; d < nd && eng_romzip_missing(rom_dir, roms, n); d++) {
            char cand[64][256]; int nc = 0;
            if (pass == 0) { for (int zi = 0; zi < nzips && nc < 64; zi++) if (!strchr(zips[zi], '*')) snprintf(cand[nc++], 256, "%s", zips[zi]); }
            else {
                char stem[64]; snprintf(stem, sizeof stem, "%s", zips[0]);
                char *dot = strrchr(stem, '.'); if (dot) *dot = 0;
                DIR *dh = opendir(dirs[d]);
                if (!dh) continue;
                for (struct dirent *de; (de = readdir(dh)) && nc < 64;) {
                    bool want = name_ieq_zip(de->d_name, stem) || name_ieq_zip(de->d_name, "namco");
                    for (int zi = 0; zi < nzips && !want; zi++) {          /* "Angler King*": a prefix */
                        const char *st = strchr(zips[zi], '*');
                        if (!st) continue;
                        char pre[128]; snprintf(pre, sizeof pre, "%.*s", (int)(st - zips[zi]), zips[zi]);
                        want = name_ieq_zip(de->d_name, pre);
                    }
                    if (want) snprintf(cand[nc++], 256, "%s", de->d_name);
                }
                closedir(dh);
            }
            for (int c = 0; c < nc && eng_romzip_missing(rom_dir, roms, n); c++) {
                snprintf(path, sizeof path, "%s/%s", dirs[d], cand[c]);
                if (file_size(path) <= 0) continue;
                bool seen = false;
                for (int k = 0; k < ndone; k++) if (same_file(done_paths[k], path)) seen = true;
                if (seen || ndone >= 24) continue;
                snprintf(done_paths[ndone++], 1024, "%s", path);
                found++;
                size_t rl = strlen(read_list);
                char shown[1024]; snprintf(shown, sizeof shown, "%s", path);
#ifndef _WIN32
                { char *rp = realpath(path, NULL); if (rp) { snprintf(shown, sizeof shown, "%s", rp); free(rp); } }
#endif
                if (rl + strlen(shown) + 4 < sizeof read_list) snprintf(read_list + rl, sizeof read_list - rl, "%s%s", rl ? ", " : "", shown);
                printf("Unpacking ROMs from %s into %s ...\n", path, rom_dir);
                char e2[300] = "";
                if (eng_romzip_extract(path, rom_dir, roms, n, e2, sizeof e2) < 0) {
                    if (!strncmp(e2, "cannot write", 12) || !strncmp(e2, "out of memory", 13)) { snprintf(err, errlen, "%s", e2); return false; }
                    printf("  (%s -- skipped)\n", e2);       /* a damaged or non-zip file: another copy may still do */
                }
            }
        }
    const char *miss = eng_romzip_missing(rom_dir, roms, n);
    if (!miss) { printf("ROMs unpacked.\n"); return true; }
    int mi = 0;
    while (mi < n && strcmp(roms[mi].name, miss)) mi++;
    if (!found) {
        char where[400] = "";
        for (int d = 0; d < nd; d++) { size_t l = strlen(where); snprintf(where + l, sizeof where - l, "%s%s", d ? ", " : "", dirs[d]); }
        snprintf(err, errlen, "no %s found (looked in: %s)", zips[0], where);
    } else if (mi < WHY_MAX && g_why[mi][0]) snprintf(err, errlen, "%s cannot be used: %s", miss, g_why[mi]);
    else {
        char belongs[160] = "at the top level of the zip";
        if (roms[mi].zname) snprintf(belongs, sizeof belongs, "at %s, or anywhere else in the zip", roms[mi].zname);
        snprintf(err, errlen, "still missing %s -- no file of that name%s in %s (it belongs %s); if your MAME set keeps it in a separate zip "
                 "(a split or non-merged set without devices), put that zip in the roms folder too (docs/ROM_CHECKSUMS.md lists the files this game uses)",
                 miss,
#ifdef ENG_ROM_HAS_CRC
                 roms[mi].crc ? " or checksum" : "",
#else
                 "",
#endif
                 read_list, belongs);
    }
    return false;
}
