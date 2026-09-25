/*
 * eng.h -- the shared Namco System 22 / Super System 22 engine.
 *
 * ONE renderer, ONE texture bake, ONE C352, ONE sound-MCU peripheral model
 * for every System 22 game in this tree (Prop Cycle at the root, Rave Racer in
 * raverace/, and the games after them). A game supplies its ASSETS and its
 * BOARD SETTINGS here; nothing in engine/ knows which game is running.
 *
 * What differs between the boards is a setting, never a fork:
 *
 *                         Super System 22 (Prop Cycle)   System 22 (Rave Racer)
 *   display list head     polygon RAM 0x304               0x2FF
 *   depth fog             per pixel, CZ RAM tables,       one factor per quad (czram
 *                         applied AFTER shading           [type<<13|cz_value]), applied
 *                                                         BEFORE shading, colour per cz_type
 *   point RAM objects     (code 0x5 unused)               code 0x5, point RAM 0xF00000
 *   sprites               C374                            none
 *   final colour          mixer gamma LUTs                gamma PROMs + global fade
 */
#ifndef ENG_H
#define ENG_H

#include <stdint.h>
#include <stddef.h>

/* ---- the texture ROMs: identical layout on both boards ------------------
 * tiles:   8 chips x 2 MB, 16x16 8bpp tiles, row-major
 * tilemap: ccrl (2 MB, 16-bit little-endian tile numbers) followed by
 *          ccrh (512 KB, packed 4-bit attributes, HIGH nibble first) */
#define TEXTURE_TILE_SIZE   (16 * 16)
#define TEXTURE_TOTAL_SIZE  (0x200000 * 8)  /* 16 MB */
#define TEXTUREMAP_SIZE     (0x280000)
extern uint8_t *g_texture_data;
extern uint8_t *g_texture_tilemap;

/* Load both from a ROM directory, by chip name. Returns 0 on failure. */
int eng_load_texture_roms(const char *dir, const char *const cg[8],
                          const char *ccrl, const char *ccrh);

/* ---- point data ----------------------------------------------------------
 * Point ROM, already sign-extended to 24 bits, plus an optional reader for
 * addresses past it (System 22's point RAM at 0xF00000). */
typedef int32_t (*eng_point_ext_fn)(uint32_t addr);
extern const int32_t   *g_eng_pointrom;
extern uint32_t         g_eng_pointrom_n;
extern eng_point_ext_fn g_eng_pointram;          /* NULL: none */
static inline int32_t eng_point_read(uint32_t addr)
{
    if (addr < g_eng_pointrom_n) return g_eng_pointrom[addr];
    return g_eng_pointram ? g_eng_pointram(addr) : 0;
}

/* ---- the resolved palette the texture bake reads --------------------------
 * 128 groups x 256 pens x RGB. A game fills it (Prop Cycle from its ROM
 * snapshot, Rave Racer from live palette RAM) and bumps the group's
 * generation whenever that group's colours change, which retires every
 * texture baked from the old ones. */
#define ENG_PAL_GROUPS  128
#define ENG_PAL_ENTRIES 256
extern uint8_t  direct_palette[ENG_PAL_GROUPS][ENG_PAL_ENTRIES][3];
extern int      direct_palette_loaded;
extern uint16_t g_eng_pal_gen[ENG_PAL_GROUPS];

/* Copy a PLANAR palette (R at +0, G at +n, B at +2n, 0x8000 pens each) into
 * direct_palette, bumping the generation of each group that changed. */
void eng_palette_from_planar(const uint8_t *planar, size_t plane_stride);

#endif /* ENG_H */
