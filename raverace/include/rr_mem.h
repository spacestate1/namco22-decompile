/*
 * rr_mem.h -- Rave Racer's ONE memory model (pc-reverse GUARDRAILS.md R1-R5).
 *
 * Every 68020 memory access in generated code is a vrd / vwr call at the
 * ORIGINAL 32-bit address, big-endian, with the width the instruction used.
 * There is no second representation and no sync layer: aliasing and partial
 * accesses are correct by construction because they hit the same bytes.
 * Addresses travel as vaddr_t (uint32_t) and are never host pointers.
 *
 * Unmapped accesses and writes to ROM are LOGGED (first hit per page) and
 * counted, never silently absorbed.
 */
#ifndef RR_MEM_H
#define RR_MEM_H
#include <stdint.h>
#include <stdbool.h>

typedef uint32_t vaddr_t;

/* Region sizes (MAME namcos22_am). */
#define RR_ROM_SIZE      0x200000u     /* 0x00000000 program ROM            */
#define RR_WRAM_SIZE     0x20000u      /* 0x10000000 work RAM (mirr 0x18..) */
#define RR_SCI_SIZE      0x4000u       /* 0x20010000 C139 SCI buffer        */
#define RR_EEPROM_SIZE   0x2000u       /* 0x58000000 EEPROM                 */
#define RR_SHARED_SIZE   0x8000u       /* 0x60004000 C74 MCU shared RAM     */
#define RR_POLY_WORDS    0x8000u       /* 0x70000000 polygon RAM, 32-bit x 0x8000 (24 used) */
#define RR_CZRAM_SIZE    0x8000u       /* 0x90010000 depth-cue LUT          */
#define RR_MIXER_SIZE    0x8000u       /* 0x90020000 C305 mixer             */
#define RR_PAL_SIZE      0x18000u      /* 0x90028000 palette                */
#define RR_CGRAM_SIZE    0x1E000u      /* 0x90080000 tilemap PCG            */
#define RR_TEXT_SIZE     0x2000u       /* 0x9009E000 tilemap                */

typedef struct rr_sys {
    uint8_t  rom[RR_ROM_SIZE];
    uint8_t  wram[RR_WRAM_SIZE];
    uint8_t  sci[RR_SCI_SIZE];
    uint8_t  eeprom[RR_EEPROM_SIZE];
    uint8_t  shared[RR_SHARED_SIZE];   /* 68K view, big-endian words        */
    uint32_t poly[RR_POLY_WORDS];      /* MAME's stored value: signed24     */
    uint8_t  czram[RR_CZRAM_SIZE];
    uint8_t  mixer[RR_MIXER_SIZE];
    uint8_t  pal[RR_PAL_SIZE];
    uint8_t  cgram[RR_CGRAM_SIZE];
    uint8_t  text[RR_TEXT_SIZE];
    uint8_t  syscon[0x20];
    uint8_t  tilemapattr[0x10];
    uint8_t  sci_reg[0x10];
    uint16_t keycus[8];
    uint32_t dsw;                      /* 0x50000000 read                   */
    uint32_t portbit;                  /* 0x50000008                        */
    /* diagnostics */
    uint32_t n_unmapped, n_romwrite;
} rr_sys_t;

extern rr_sys_t g_rr;

uint32_t rr_read(vaddr_t a, int size);           /* size 1, 2 or 4 */
void     rr_write(vaddr_t a, int size, uint32_t v);

static inline uint32_t vrd8 (vaddr_t a) { return rr_read(a, 1); }
static inline uint32_t vrd16(vaddr_t a) { return rr_read(a, 2); }
static inline uint32_t vrd32(vaddr_t a) { return rr_read(a, 4); }
static inline int32_t  vrd8s (vaddr_t a) { return (int8_t)rr_read(a, 1); }
static inline int32_t  vrd16s(vaddr_t a) { return (int16_t)rr_read(a, 2); }
static inline int32_t  vrd32s(vaddr_t a) { return (int32_t)rr_read(a, 4); }
static inline void vwr8 (vaddr_t a, uint32_t v) { rr_write(a, 1, v); }
static inline void vwr16(vaddr_t a, uint32_t v) { rr_write(a, 2, v); }
static inline void vwr32(vaddr_t a, uint32_t v) { rr_write(a, 4, v); }

/* Hardware hooks the engine installs (syscon, sci, keycus, mixer ...).
 * Return true if the access was handled. */
typedef bool (*rr_io_read_fn)(vaddr_t a, int size, uint32_t *out);
typedef bool (*rr_io_write_fn)(vaddr_t a, int size, uint32_t v);
void rr_set_io_hooks(rr_io_read_fn r, rr_io_write_fn w);

bool rr_load_program(const char *rom_dir);       /* the four rv2_prg* chips */

#endif
