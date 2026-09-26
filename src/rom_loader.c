/*
 * ROM loader - reads all Prop Cycle ROM files
 *
 * Program ROM: 4x1MB byte-interleaved → 4MB
 * Point ROM:   9 files (3 planes × 3 chips) → signed 24-bit array
 * Texture:     8x2MB sequential → 16MB tile data
 * Tex tilemap: ccrl(2MB) + ccrh(512KB) → 2.5MB UV lookup
 * Sprite:      2x2MB sequential → 4MB tile data
 */
#include "propcycl.h"
#include "sprite_hw.h"
#include <stdlib.h>

/* Asset storage */
int32_t   g_pointrom[POINTROM_SIZE];
uint32_t  g_pointrom_count;

static bool load_file(const char* path, uint8_t* buf, size_t size) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "  Cannot open: %s\n", path);
        return false;
    }
    size_t rd = fread(buf, 1, size, f);
    fclose(f);
    if (rd != size) {
        fprintf(stderr, "  Short read: %s (%zu/%zu)\n", path, rd, size);
        return false;
    }
    return true;
}

static bool load_file_at(const char* dir, const char* name, uint8_t* buf, size_t offset, size_t size) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    return load_file(path, buf + offset, size);
}

/* Sign-extend 24-bit to 32-bit */
static int32_t signed24(uint32_t val) {
    if (val & 0x800000) return (int32_t)(val | 0xFF000000);
    return (int32_t)val;
}

/* ========== Program ROM ========== */

static bool load_program_rom(const char* dir) {
    const size_t CHIP = 0x100000;
    uint8_t* r1 = malloc(CHIP), *r2 = malloc(CHIP), *r3 = malloc(CHIP), *r4 = malloc(CHIP);
    if (!r1 || !r2 || !r3 || !r4) return false;

    bool ok = true;
    ok = ok && load_file_at(dir, "pr2ver-a.1", r1, 0, CHIP);
    ok = ok && load_file_at(dir, "pr2ver-a.2", r2, 0, CHIP);
    ok = ok && load_file_at(dir, "pr2ver-a.3", r3, 0, CHIP);
    ok = ok && load_file_at(dir, "pr2ver-a.4", r4, 0, CHIP);

    if (ok) {
        for (size_t i = 0; i < CHIP; i++) {
            g_sys.rom[i*4+0] = r4[i];
            g_sys.rom[i*4+1] = r3[i];
            g_sys.rom[i*4+2] = r2[i];
            g_sys.rom[i*4+3] = r1[i];
        }
        uint32_t sp = (g_sys.rom[0]<<24)|(g_sys.rom[1]<<16)|(g_sys.rom[2]<<8)|g_sys.rom[3];
        uint32_t pc = (g_sys.rom[4]<<24)|(g_sys.rom[5]<<16)|(g_sys.rom[6]<<8)|g_sys.rom[7];
        printf("  Program ROM: SP=0x%08X PC=0x%08X %s\n", sp, pc,
               (sp == 0xE20000 && pc == 0xBC4C) ? "OK" : "MISMATCH!");
    }
    free(r1); free(r2); free(r3); free(r4);
    return ok;
}

/* ========== Point ROM (3D geometry) ========== */
/*
 * 9 files: pr1ptrl0-2 (low byte), pr1ptrm0-2 (mid), pr1ptru0-2 (high)
 * Each set of 3 = one 512KB plane, total 3 planes × 1.5MB = 4.5MB
 * Interleave: pointrom[i] = signed24(high[i]<<16 | mid[i]<<8 | low[i])
 */
static bool load_point_rom(const char* dir) {
    const size_t PLANE_CHIP = 0x80000;  /* 512KB per chip */
    const size_t PLANE_SIZE = PLANE_CHIP * 3;  /* 3 chips per plane = 1.5MB */

    uint8_t* low  = calloc(PLANE_SIZE, 1);
    uint8_t* mid  = calloc(PLANE_SIZE, 1);
    uint8_t* high = calloc(PLANE_SIZE, 1);
    if (!low || !mid || !high) return false;

    bool ok = true;
    /* Low plane: ptrl0, ptrl1, ptrl2 */
    ok = ok && load_file_at(dir, "pr1ptrl0.18k", low,  PLANE_CHIP*0, PLANE_CHIP);
    ok = ok && load_file_at(dir, "pr1ptrl1.16k", low,  PLANE_CHIP*1, PLANE_CHIP);
    ok = ok && load_file_at(dir, "pr1ptrl2.15k", low,  PLANE_CHIP*2, PLANE_CHIP);
    /* Mid plane: ptrm0, ptrm1, ptrm2 */
    ok = ok && load_file_at(dir, "pr1ptrm0.18j", mid,  PLANE_CHIP*0, PLANE_CHIP);
    ok = ok && load_file_at(dir, "pr1ptrm1.16j", mid,  PLANE_CHIP*1, PLANE_CHIP);
    ok = ok && load_file_at(dir, "pr1ptrm2.15j", mid,  PLANE_CHIP*2, PLANE_CHIP);
    /* High plane: ptru0, ptru1, ptru2 */
    ok = ok && load_file_at(dir, "pr1ptru0.18f", high, PLANE_CHIP*0, PLANE_CHIP);
    ok = ok && load_file_at(dir, "pr1ptru1.16f", high, PLANE_CHIP*1, PLANE_CHIP);
    ok = ok && load_file_at(dir, "pr1ptru2.15f", high, PLANE_CHIP*2, PLANE_CHIP);

    if (ok) {
        g_pointrom_count = PLANE_SIZE;
        if (g_pointrom_count > POINTROM_SIZE)
            g_pointrom_count = POINTROM_SIZE;
        for (size_t i = 0; i < g_pointrom_count; i++) {
            g_pointrom[i] = signed24(((uint32_t)high[i] << 16) |
                                     ((uint32_t)mid[i] << 8) |
                                     ((uint32_t)low[i]));
        }
        printf("  Point ROM: %u entries (%.1f MB)\n",
               g_pointrom_count, g_pointrom_count * 4.0 / (1024*1024));
        g_eng_pointrom = g_pointrom;            /* the shared geometry stage reads it */
        g_eng_pointrom_n = g_pointrom_count;
    }
    free(low); free(mid); free(high);
    return ok;
}

/* ========== Texture ROM (16x16x8bpp tiles) ========== */
/*
 * 8 sequential files: pr1cg0-7, each 2MB
 * Concatenated into 16MB buffer
 * Each tile = 256 bytes (16×16 pixels, 8bpp)
 * Total tiles = 16MB / 256 = 65536
 */
static bool load_texture_rom(const char* dir) {
    static const char *const cg[8] = {
        "pr1cg0.12b", "pr1cg1.10d", "pr1cg2.12d", "pr1cg3.13d",
        "pr1cg4.14d", "pr1cg5.16d", "pr1cg6.18a", "pr1cg7.15a"
    };
    if (!eng_load_texture_roms(dir, cg, "pr1ccrl.3d", "pr1ccrh.1d")) return false;
    printf("  Texture ROM: %d tiles (16 MB)\n", TEXTURE_TOTAL_SIZE / TEXTURE_TILE_SIZE);
    printf("  Texture tilemap: 2.5 MB\n");
    return true;
}

static bool load_texture_tilemap(const char* dir) { (void)dir; return true; }  /* loaded with the tiles */

/* ========== Sprite ROM (32x32x8bpp tiles) ========== */
/*
 * pr1scg0.12f (2MB) + pr1scg1.10f (2MB) = 4MB
 * Each tile = 1024 bytes (32×32 pixels, 8bpp)
 * Total tiles = 4MB / 1024 = 4096
 */
static bool load_sprite_rom(const char* dir) {
    g_sprite_tiles = calloc(SPRITE_TOTAL_SIZE, 1);
    if (!g_sprite_tiles) return false;
    g_sprite_tiles_size = SPRITE_TOTAL_SIZE;

    bool ok = true;
    ok = ok && load_file_at(dir, "pr1scg0.12f", g_sprite_tiles, 0, 0x200000);
    ok = ok && load_file_at(dir, "pr1scg1.10f", g_sprite_tiles, 0x200000, 0x200000);
    if (ok) {
        printf("  Sprite ROM: %d tiles (4 MB)\n", SPRITE_TOTAL_SIZE / SPRITE_TILE_SIZE);
    }
    return ok;
}

/* ========== Load Everything ========== */

/*
 * Map ROM data at the original M68K address range (0x000000-0x3FFFFF).
 *
 * The transpiled code stores raw M68K ROM addresses in variables and
 * dereferences them as pointers. On the original hardware, ROM was
 * memory-mapped at 0x000000. On x86-64, these addresses are unmapped
 * and cause segfaults. By mapping the ROM data at address 0, all ~319
 * raw pointer dereferences in the transpiled code work correctly.
 */
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif
static bool map_rom_at_zero(void) {
#ifdef _WIN32
    /* Same idea as the mmap below: put the ROM image at its own 68K
     * addresses. Windows' lowest allocatable address is also 0x10000. */
    {
        size_t off = 0x10000, sz = ROM_SIZE - off;
        void *p = VirtualAlloc((void *)off, sz, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!p) {
            fprintf(stderr, "WARNING: Cannot map ROM at 0x%zX -- transpiled code may crash\n", off);
            return false;
        }
        memcpy(p, g_sys.rom + off, sz);
        return true;
    }
#else
    /*
     * Map ROM data at the M68K address range so transpiled code's raw
     * pointer dereferences work. The kernel's mmap_min_addr is typically
     * 0x10000 (65536), so we map starting there. This covers addresses
     * 0x10000-0x3FFFFF which includes all ROM table lookups used by the
     * game logic. Addresses 0-0xFFFF (vector table) are rarely dereferenced
     * as data pointers.
     */
    size_t map_offset = 0x10000;
    size_t map_size = ROM_SIZE - map_offset;
    void *mapped = mmap((void*)map_offset, map_size,
                        PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
                        -1, 0);
    if (mapped == MAP_FAILED) {
        /* Last resort: try MAP_FIXED (overwrites existing mappings) */
        mapped = mmap((void*)map_offset, map_size,
                      PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED,
                      -1, 0);
    }
    if (mapped == MAP_FAILED) {
        fprintf(stderr, "WARNING: Cannot map ROM at 0x%zX — transpiled code may crash\n",
                map_offset);
        return false;
    }
    /* Copy ROM data as-is (big-endian). Transpiled code that reads via
     * *(int*) or *(short*) will get byte-swapped values on x86 — these
     * must be handled with __builtin_bswap32/16 at the access sites.
     * The mmap provides valid addresses; endianness fixes are per-site. */
    memcpy(mapped, g_sys.rom + map_offset, map_size);
    printf("  ROM mapped at %p-%p (M68K address space, big-endian)\n",
           mapped, (char*)mapped + map_size - 1);
    return true;
#endif
}

bool rom_load_all(const char* dir) {
    printf("Loading ROMs from: %s\n", dir);

    if (!load_program_rom(dir)) {
        fprintf(stderr, "FATAL: Program ROM load failed\n");
        return false;
    }

    /* Map ROM at M68K address 0 for transpiled code compatibility.
     * PROPCYCL_NO_ROMMAP=1 skips it (tests what happens when the mapping
     * cannot be made, as on Windows when the stack sits in that range). */
    if (!getenv("PROPCYCL_NO_ROMMAP")) map_rom_at_zero();

    /* These are optional for initial boot - game can run without 3D */
    bool ok3d = true;
    ok3d = load_point_rom(dir) && ok3d;
    ok3d = load_texture_rom(dir) && ok3d;
    ok3d = load_texture_tilemap(dir) && ok3d;
    ok3d = load_sprite_rom(dir) && ok3d;

    if (!ok3d) {
        fprintf(stderr, "WARNING: Some asset ROMs failed to load, 3D will be unavailable\n");
    }

    printf("ROM loading complete\n");
    return true;
}
