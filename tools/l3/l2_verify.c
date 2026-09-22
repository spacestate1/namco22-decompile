/*
 * L2 differential replay verifier (GUARDRAILS §5 L2).
 *
 * Parses the MAME debug.log produced by mame_l2_capture.lua (L2E/L2X
 * breakpoint printf lines), pairs entry/exit records by function and SP,
 * replays every entry into the v2 C translation, and diffs against
 * MAME's exit value — the function's real input distribution as ground
 * truth.
 *
 * Also implements the pass-one (v1) semantics inline (--v1-sim) to
 * quantify how wrong the old translations were on the same inputs:
 *   v1 math_slope_angle: native-LE table read, unscaled index, value*2
 *   v1 math_atan2_coarse: returned 0 for every quadrant (stubbed
 *     dispatch swallowed the table path)
 *   v1 math_atan2: the LIVE hand-port (game_core.c:44) — correct byte
 *     order, but base-path conditions swapped and different quadrant
 *     transforms vs the machine code (the transpiled variant in
 *     game_math/game_all deref'd the division quotient as a pointer and
 *     was never compiled).
 *
 * Build: cmake target l2_verify (links src/memory.c, src/trace.c,
 * src/v2/game_math_v2.c).
 *
 * Usage: l2_verify <rom_dir> <debug.log> [--v1-sim] [--max-fail N]
 */
#include "propcycl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* `propcycl_verbose()` is supplied per-BINARY -- main.c and rig_viewer.c each
 * define their own -- and this tool has its own main(), so it never got one.
 * game_math_v2.c calls it from v2_div0(), so l2_verify has failed to LINK
 * (undefined reference) independently of any build-flag change, which is why
 * CLAUDE.md can list it as a validation entry point while it does not build. */
int propcycl_verbose(void);
int propcycl_verbose(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("PROPCYCL_VERBOSE"); v = (e && *e != '0'); }
    return v;
}

extern uint32_t math_atan2(int32_t, int32_t);
extern uint32_t math_atan2_coarse(int32_t, int32_t);
extern uint32_t math_slope_angle(int32_t, int32_t);

/* SDL-free stand-ins for symbols memory.c/trace.c expect (none needed —
 * memory.c is self-contained, trace.c only needs g_sys from memory.c). */

static bool load_program_rom(const char* dir) {
    static const char* names[4] = {
        "pr2ver-a.4", "pr2ver-a.3", "pr2ver-a.2", "pr2ver-a.1" };
    const size_t CHIP = 0x100000;
    uint8_t* bufs[4];
    for (int i = 0; i < 4; i++) {
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", dir, names[i]);
        FILE* f = fopen(path, "rb");
        if (!f) { fprintf(stderr, "cannot open %s\n", path); return false; }
        bufs[i] = malloc(CHIP);
        if (fread(bufs[i], 1, CHIP, f) != CHIP) {
            fprintf(stderr, "short read: %s\n", path);
            fclose(f);
            return false;
        }
        fclose(f);
    }
    /* byte interleave, same as rom_loader.c: rom[i*4+0]=r4[i].. +3=r1[i] */
    for (size_t i = 0; i < CHIP; i++) {
        g_sys.rom[i*4+0] = bufs[0][i];
        g_sys.rom[i*4+1] = bufs[1][i];
        g_sys.rom[i*4+2] = bufs[2][i];
        g_sys.rom[i*4+3] = bufs[3][i];
    }
    for (int i = 0; i < 4; i++) free(bufs[i]);
    return true;
}

/* ---- v1 (pass one) semantics, for --v1-sim reporting ---- */

static uint16_t rom_le16(uint32_t a) {           /* native-LE read of BE ROM */
    return (uint16_t)(g_sys.rom[a] | (g_sys.rom[a+1] << 8));
}

static uint32_t v1_math_slope_angle(int32_t p1, int32_t p2) {
    /* game_math.c pass one: rom_nr16s(0x201002 + q) * 2 — unscaled index,
     * value doubled, little-endian. Division guarded (post-FPE-fix). */
    uint32_t uVar2 = (uint32_t)p1 << 9;
    if (p2 < 0) p2 += 7;
    uint32_t uVar1 = (uint32_t)(p2 >> 3);
    if ((int32_t)uVar1 == 0) return 0;
    if ((int32_t)(uVar1 ^ uVar2) < 0)
        return 0x8000 - ((int16_t)rom_le16(0x201002 + (int32_t)uVar2 / -(int32_t)uVar1) * 2);
    return (uint32_t)((int16_t)rom_le16(0x201002 + (int32_t)uVar2 / (int32_t)uVar1) * 2);
}

static uint32_t v1_math_atan2_coarse(int32_t p1, int32_t p2) {
    (void)p1; (void)p2;
    return 0;   /* pass one: "dispatch stub" swallowed every quadrant */
}

static uint16_t rom_be16(uint32_t a) {
    return (uint16_t)((g_sys.rom[a] << 8) | g_sys.rom[a + 1]);
}

static uint32_t v1_math_atan2(int32_t y, int32_t x) {
    /* the live hand-port from game_core.c:44 (pre-v2) */
    int quadrant = 0;
    if (y < 0) { y = -y; quadrant = 1; }
    if (x < 0) { x = -x; quadrant += 2; }
    uint32_t angle;
    if ((uint32_t)y < (uint32_t)x) {
        uint32_t ratio = x ? (((uint32_t)y << 10) / (uint32_t)x) : 0;
        if (ratio > 1023) ratio = 1023;
        angle = rom_be16(0x200000 + ratio * 2);
    } else {
        uint32_t ratio = y ? (((uint32_t)x << 10) / (uint32_t)y) : 0;
        if (ratio > 1023) ratio = 1023;
        angle = 0x4000 - rom_be16(0x200000 + ratio * 2);
    }
    switch (quadrant) {
    case 0: break;
    case 1: angle = 0x8000 - angle; break;
    case 2: angle = -angle & 0xFFFF; break;
    case 3: angle = 0x8000 + angle; break;
    }
    return angle & 0xFFFF;
}

/* ---- record parsing ---- */

typedef struct {
    char fn[8];
    uint32_t p1, p2, sp, fc;
    int has_exit;
    uint32_t ret;
} Rec;

#define MAX_PENDING 64

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <rom_dir> <debug.log> [--v1-sim] [--max-fail N]\n",
                argv[0]);
        return 2;
    }
    const char* rom_dir = argv[1];
    const char* log_path = argv[2];
    int v1_sim = 0;
    long max_fail = 10;
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "--v1-sim")) v1_sim = 1;
        else if (!strcmp(argv[i], "--max-fail") && i + 1 < argc)
            max_fail = atol(argv[++i]);
    }

    memset(&g_sys, 0, sizeof(g_sys));
    if (!load_program_rom(rom_dir)) return 1;

    FILE* f = fopen(log_path, "r");
    if (!f) { fprintf(stderr, "cannot open %s\n", log_path); return 1; }

    /* per-function pending stacks (IRQ nesting safe), keyed by SP */
    Rec pending[MAX_PENDING];
    int n_pending = 0;

    struct {
        const char* fn;
        uint32_t (*v2)(int32_t, int32_t);
        uint32_t (*v1)(int32_t, int32_t);   /* NULL = not runnable */
        long total, v2_ok, v1_ok;
        long shown;
    } stats[3] = {
        { "A870", math_atan2,        v1_math_atan2,        0,0,0,0 },
        { "A91A", math_atan2_coarse, v1_math_atan2_coarse, 0,0,0,0 },
        { "A99E", math_slope_angle,  v1_math_slope_angle,  0,0,0,0 },
    };

    char line[512];
    long pair_errors = 0;
    while (fgets(line, sizeof(line), f)) {
        char* p = strstr(line, "L2");
        if (!p) continue;
        if (p[2] == 'E') {
            Rec r;
            memset(&r, 0, sizeof(r));
            if (sscanf(p, "L2E,%7[^,],%x,%x,%x,%x",
                       r.fn, &r.p1, &r.p2, &r.sp, &r.fc) != 5)
                continue;
            if (n_pending < MAX_PENDING)
                pending[n_pending++] = r;
        }
        else if (p[2] == 'X') {
            char fn[8];
            uint32_t d0, sp;
            if (sscanf(p, "L2X,%7[^,],%x,%x", fn, &d0, &sp) != 3)
                continue;
            /* match most recent pending entry with same fn+sp */
            int idx = -1;
            for (int i = n_pending - 1; i >= 0; i--) {
                if (!strcmp(pending[i].fn, fn) && pending[i].sp == sp) {
                    idx = i;
                    break;
                }
            }
            if (idx < 0) { pair_errors++; continue; }
            Rec r = pending[idx];
            memmove(&pending[idx], &pending[idx+1],
                    (n_pending - idx - 1) * sizeof(Rec));
            n_pending--;

            for (int s = 0; s < 3; s++) {
                if (strcmp(stats[s].fn, fn)) continue;
                stats[s].total++;
                uint32_t got = stats[s].v2((int32_t)r.p1, (int32_t)r.p2);
                if (got == d0) {
                    stats[s].v2_ok++;
                }
                else if (stats[s].shown++ < max_fail) {
                    printf("MISMATCH %s fc=%u p1=%d p2=%d mame=0x%X v2=0x%X\n",
                           fn, r.fc, (int32_t)r.p1, (int32_t)r.p2, d0, got);
                }
                if (v1_sim && stats[s].v1) {
                    if (stats[s].v1((int32_t)r.p1, (int32_t)r.p2) == d0)
                        stats[s].v1_ok++;
                }
            }
        }
    }
    fclose(f);

    printf("\n=== L2 VERIFY ===\n");
    const char* names[3] = { "math_atan2", "math_atan2_coarse", "math_slope_angle" };
    int all_pass = 1;
    for (int s = 0; s < 3; s++) {
        printf("%-18s @0x%s  calls=%-6ld v2 match: %ld/%ld",
               names[s], stats[s].fn, stats[s].total,
               stats[s].v2_ok, stats[s].total);
        if (v1_sim)
            printf("   v1 match: %ld/%ld", stats[s].v1_ok, stats[s].total);
        printf("\n");
        if (stats[s].total == 0 || stats[s].v2_ok != stats[s].total)
            all_pass = 0;
    }
    if (pair_errors)
        printf("entry/exit pairing errors: %ld\n", pair_errors);
    printf(all_pass ? "RESULT: PASS (all calls match, coverage nonzero)\n"
                    : "RESULT: FAIL\n");
    return all_pass ? 0 : 1;
}
