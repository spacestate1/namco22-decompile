/*
 * rom_native.h - Transitional native-byte-order ROM access helpers.
 *
 * Purpose: Replace raw `*(T *)(&R[addr])` derefs with a bounds-checked,
 * alias-safe helper that PRESERVES the existing semantics (native byte
 * order on the host). These are intended as a migration step, not a
 * destination.
 *
 * Why not just use vrd16/vrd32 (big-endian)?
 *   The ROM buffer g_sys.rom[] is BE-stored (verified by the SP/PC
 *   check in rom_loader.c:67). Transpiled Ghidra output dereferences
 *   it with native-order casts, which on x86_64 reads LE and therefore
 *   byte-swaps every word. Swapping everything to vrd* (BE) all at once
 *   changes the values downstream code sees, breaks init, and crashes.
 *
 *   The fix is per-site migration: each rom_nr*() call is a candidate
 *   to flip to vrd*() after its consumer is verified to expect the
 *   big-endian value. Grep for rom_nr to find remaining sites.
 *
 * Byte order: these helpers read EXACTLY the bytes at g_sys.rom[a..a+N]
 * in the host's native order. On x86_64 that is little-endian, which
 * matches what `*(T *)(&g_sys.rom[a])` produces -- so the rewrite is
 * semantics-preserving.
 *
 * Bounds: out-of-range reads return 0 instead of dereferencing past
 * the end of g_sys.rom[].
 */
#ifndef ROM_NATIVE_H
#define ROM_NATIVE_H

#include <stdint.h>
#include <string.h>
#include "propcycl.h"   /* g_sys, ROM_SIZE */

static inline uint8_t  rom_nr8  (uint32_t a) { return (a < ROM_SIZE) ? g_sys.rom[a] : 0; }
static inline int8_t   rom_nr8s (uint32_t a) { return (a < ROM_SIZE) ? (int8_t)g_sys.rom[a] : 0; }

static inline uint16_t rom_nr16 (uint32_t a) {
    uint16_t v = 0;
    if (a + 2 <= ROM_SIZE) memcpy(&v, &g_sys.rom[a], 2);
    return v;
}
static inline int16_t  rom_nr16s(uint32_t a) {
    int16_t v = 0;
    if (a + 2 <= ROM_SIZE) memcpy(&v, &g_sys.rom[a], 2);
    return v;
}

static inline uint32_t rom_nr32 (uint32_t a) {
    uint32_t v = 0;
    if (a + 4 <= ROM_SIZE) memcpy(&v, &g_sys.rom[a], 4);
    return v;
}
static inline int32_t  rom_nr32s(uint32_t a) {
    int32_t v = 0;
    if (a + 4 <= ROM_SIZE) memcpy(&v, &g_sys.rom[a], 4);
    return v;
}

/* Writes (rare for ROM; included for symmetry). */
static inline void rom_nw8 (uint32_t a, uint8_t  v) { if (a <  ROM_SIZE) g_sys.rom[a] = v; }
static inline void rom_nw16(uint32_t a, uint16_t v) { if (a + 2 <= ROM_SIZE) memcpy(&g_sys.rom[a], &v, 2); }
static inline void rom_nw32(uint32_t a, uint32_t v) { if (a + 4 <= ROM_SIZE) memcpy(&g_sys.rom[a], &v, 4); }

#endif /* ROM_NATIVE_H */
