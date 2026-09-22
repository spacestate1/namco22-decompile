/*
 * vaddr.h - Unified M68K virtual-address type + safe read/write helpers.
 *
 * Purpose: eliminate the two classes of transpilation bugs documented in
 * CLAUDE.md:318 by giving every M68K address a proper 32-bit type that
 * does not silently truncate when passed through function signatures,
 * and routing every dereference through the existing memory HAL instead
 * of casting raw integers to host pointers.
 *
 *   vaddr_t        = uint32_t, wide enough to carry any M68K address.
 *                    Use in signatures wherever Ghidra wrote 'short' or
 *                    'int' for something that is actually an address.
 *
 *   vrd8/16/32     = unsigned big-endian read at a vaddr_t.
 *   vrd8s/16s/32s  = signed variant (matches the (int16_t)*(uint16_t *)
 *                    Ghidra idiom).
 *   vwr8/16/32     = big-endian write.
 *   vptr(a)        = backing host pointer for bulk copies; NULL if unmapped.
 *
 * These wrap mem_read / mem_write / mem_ptr (memory.c), which already
 * handle region dispatch (ROM / work_ram / dspram / palette / ...) and
 * byte order.
 *
 * Rewrite patterns (hostile -> fixed):
 *
 *   (int16_t *)(0xB0938 + off) deref          ->  vrd16s(0xB0938 + off)
 *   (int *)(&R[0x15C328] + W[x]*4) deref      ->  vrd32s(0x15C328 + W[x]*4)
 *   (char *)(W[0x2908] + W[0x0990]*4) deref   ->  vrd8 (W[0x2908] + W[0x0990]*4)
 *   scene_node_render((short)ptr)             ->  scene_node_render((vaddr_t)ptr)
 *
 * The last one is the important signature change: stop storing pointers
 * in 'short' or 'int' parameters. Replace them with vaddr_t throughout
 * the call chain. Width is preserved and bugs become compile errors.
 */
#ifndef VADDR_H
#define VADDR_H

#include <stdint.h>
#include <stdbool.h>

/* Forward declarations -- full ones live in propcycl.h, but we re-declare
 * here so vaddr.h can be included standalone by the rewrite tool. */
uint8_t  mem_read8 (uint32_t addr);
uint16_t mem_read16(uint32_t addr);
uint32_t mem_read32(uint32_t addr);
void     mem_write8 (uint32_t addr, uint8_t  v);
void     mem_write16(uint32_t addr, uint16_t v);
void     mem_write32(uint32_t addr, uint32_t v);
void    *mem_ptr   (uint32_t addr);

/* Unified M68K virtual address. Never pass this through a narrower
 * type -- no int/short/char parameters for addresses. */
typedef uint32_t vaddr_t;

#define VADDR_NULL ((vaddr_t)0)

/* Region predicates (match resolve_addr() in memory.c). Useful for
 * debug asserts when you want to fail loudly on miscategorised addresses. */
static inline bool vaddr_is_rom  (vaddr_t a) { return a < 0x400000u; }
static inline bool vaddr_is_ram  (vaddr_t a) { return a >= 0xE00000u && a < 0xE40000u; }
static inline bool vaddr_is_dsp  (vaddr_t a) { return a >= 0xC00000u && a < 0xC20000u; }
static inline bool vaddr_is_point(vaddr_t a) { return a >= 0xF80000u && a < 0xFA0000u; }

/* ---- Reads (big-endian, matching the 68020 bus) ---- */

static inline uint8_t  vrd8 (vaddr_t a) { return mem_read8 (a); }
static inline uint16_t vrd16(vaddr_t a) { return mem_read16(a); }
static inline uint32_t vrd32(vaddr_t a) { return mem_read32(a); }

static inline int8_t   vrd8s (vaddr_t a) { return (int8_t) mem_read8 (a); }
static inline int16_t  vrd16s(vaddr_t a) { return (int16_t)mem_read16(a); }
static inline int32_t  vrd32s(vaddr_t a) { return (int32_t)mem_read32(a); }

/* ---- Writes ---- */

static inline void vwr8 (vaddr_t a, uint8_t  v) { mem_write8 (a, v); }
static inline void vwr16(vaddr_t a, uint16_t v) { mem_write16(a, v); }
static inline void vwr32(vaddr_t a, uint32_t v) { mem_write32(a, v); }

/* ---- Direct host pointer ----
 * For bulk-copy / DMA-like patterns that legitimately need a contiguous
 * buffer (e.g. memcpy from a ROM table). Returns NULL when unmapped,
 * so callers must guard the result. Do NOT use the pointer to work
 * around the typed helpers -- that reintroduces the original bug. */
static inline void *vptr(vaddr_t a) { return mem_ptr(a); }


/* ---- half-word access into a 32-bit _W[] slot -------------------------
 *
 * THE SINGLE MOST COMMON ENDIANNESS BUG IN THIS TREE.
 *
 * Ghidra emits `*(short *)(int_ptr + N)` for a 16-bit field inside a
 * 32-bit word. On the big-endian M68K that reads the TOP half. Converted
 * naively to the _W[] model it becomes `(short)W[slot]`, which takes the
 * BOTTOM half -- a different field, usually a plausible-looking small
 * number, so it fails silently.
 *
 * Measured example (player animation nodes, ROM 0x12ACAC):
 *     w1 = 0xFFFF0001   hi = -1 (parent link)   lo = 1
 * The high half selects the emission branch in scene_node_render; the low
 * half is always 1. Reading the low half sent every node down the wrong
 * branch and the rider never rendered.
 *
 * Use these instead of a cast, always:
 *     W_HI16(s)  the field at byte offset +0 of the word  (M68K *(short*)p)
 *     W_LO16(s)  the field at byte offset +2 of the word  (M68K *(short*)(p)+1 in BYTES)
 */
#define W_HI16(slot)  ((int16_t)((uint32_t)(_W[(slot)]) >> 16))
#define W_LO16(slot)  ((int16_t)((uint32_t)(_W[(slot)]) & 0xFFFF))
/* Half-word STORES into a _W slot. M68K `move.w #v,(a0+N)` writes the HIGH
 * half of the 32-bit word at N when N is 4-aligned and the LOW half when
 * N is 2-mod-4; keep the other half intact. Never store a 16-bit literal
 * into the whole slot -- see the parent-link bug in bicycle_ik_solve. */
#define W_SET_HI16(slot, v) (W[slot] = (intptr_t)(int32_t)(((uint32_t)(uint16_t)(v) << 16) | ((uint32_t)W[slot] & 0xffffu)))
#define W_SET_LO16(slot, v) (W[slot] = (intptr_t)(int32_t)(((uint32_t)W[slot] & 0xffff0000u) | (uint32_t)(uint16_t)(v)))


/* ---- 16-bit ARRAY inside the _W[] model -------------------------------
 *
 * A few work-RAM tables are int16_t arrays, not int32_t ones. The M68K
 * addresses element i of the table based at WRAM byte offset B as
 * `(B, dn.l*2)` with a .w access -- e.g. `tst.w $e0478c(d0.l*2)` at ROM
 * 0x011580, `addq.w #1,$e0478c(d0.l*2)` at 0x01165C.
 *
 * _W[] holds the 32-bit word at each 4-aligned byte offset, so element i
 * lives in the HIGH half of the slot at ((B + i*2) & ~3) when that offset
 * is 4-aligned and in the LOW half when it is 2-mod-4. Indexing such a
 * table as W[B + i] (stride 1) or W[B + i*4] (stride 4) is wrong twice
 * over: it reads a whole slot where the machine reads a half, and it
 * spreads the array over 2x or 4x its real extent, so it collides with
 * whatever work RAM follows.
 *
 * Unlike a bare 2-mod-4 slot these never address one, so sync_wram_to_W
 * cannot eat them and no wsync_pin is needed.                          */
#define W_A16(B, i)  ((((unsigned)((B) + (i)*2) & 2u)                  \
                        ? W_LO16(((unsigned)((B) + (i)*2) & ~3u))       \
                        : W_HI16(((unsigned)((B) + (i)*2) & ~3u))))
#define W_A16_SET(B, i, v)  ((((unsigned)((B) + (i)*2) & 2u)            \
                        ? W_SET_LO16(((unsigned)((B) + (i)*2) & ~3u), (v)) \
                        : W_SET_HI16(((unsigned)((B) + (i)*2) & ~3u), (v))))

/* ONE 16-bit work-RAM field at byte offset OFF, whichever half of its slot it
 * lives in: `move.w $e15f3e.l,d0` is W16(0x15F3E), which is W_LO16(0x15F3C).
 * Use this for a bare `.w` access to an absolute WRAM address -- the whole-slot
 * forms W[OFF] and `W[OFF] = v` are wrong for it both ways: a 4-aligned write
 * lands in the LOW half (the NEIGHBOURING field), and a 2-mod-4 slot is never
 * written back to work RAM and is rebuilt from its neighbours' bytes by
 * sync_wram_to_W every frame. */
#define W16(off)          W_A16((off), 0)
#define W16_SET(off, v)   W_A16_SET((off), 0, (v))

/* ---- text tilemap (g_sys.textram / cgram tail) -----------------------------
 * A tile word is a BIG-ENDIAN u16: [15:12] palette [11] flipy [10] flipx
 * [9:0] tile code (text_hw.c reads it that way, and so does the hardware).
 * Every transpiled writer stored it through a host-native `uint16_t *`, so
 * each character landed byte-swapped -- "FREE PLAY" came out as 4600 5200
 * 4500 ... where the machine has E043 E052 E045 ..., i.e. the character in
 * the high byte and the palette nibble destroyed. Use these instead of a
 * plain store. */
/* THE MCU COMMAND BLOCK IS 16-BIT AND BIG-ENDIAN.
 * g_sys.commsram is uint8_t[], so `commsram[0x010E] = <16-bit>` truncates to
 * one byte. ROM 0x0103A8 / 0x010520 / 0x0335A0 write these with `move.w`, and
 * MAME's own 68K puts 0x1400..0x27C8 in 0x010E where ours could only ever
 * hold 0..0xFF. That is the whole sound command stream the M37710 consumes. */
static inline void comms_w16(uint8_t *comms, unsigned off, unsigned v) {
    comms[off]     = (uint8_t)(v >> 8);
    comms[off + 1] = (uint8_t)(v & 0xFF);
}
/* `divs.l` AS THE 68K DOES IT. A zero divisor raises the zero-divide trap,
 * and this ROM's vector 5 (-> 0x00C0B4) is a bare `rte`: the division is
 * skipped and the destination keeps the DIVIDEND. Overflow (INT_MIN / -1)
 * likewise leaves the operand unaffected. C raises SIGFPE for both, so any
 * divisor the game computes -- a cos that reads 0 at 90 degrees, a distance
 * that can be 0 -- has to go through this (register row 180). */
static inline int32_t m68k_divs(int32_t num, int32_t den) {
    if (den == 0 || (num == INT32_MIN && den == -1)) return num;
    return num / den;
}

static inline unsigned comms_r16(const uint8_t *comms, unsigned off) {
    return ((unsigned)comms[off] << 8) | comms[off + 1];
}

static inline void tram_w16(void *p, unsigned v) {
    uint8_t *b = (uint8_t *)p;
    b[0] = (uint8_t)(v >> 8);
    b[1] = (uint8_t)v;
}
static inline unsigned tram_r16(const void *p) {
    const uint8_t *b = (const uint8_t *)p;
    return ((unsigned)b[0] << 8) | b[1];
}

#endif /* VADDR_H */
