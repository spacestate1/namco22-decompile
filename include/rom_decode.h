/*
 * rom_decode.h - Typed ROM data decoders.
 *
 * Purpose: replace "chase the pointer through Ghidra" with "call a
 * function that returns a struct". Every decoder reads directly from
 * g_sys.rom, does the 24-bit/32-bit big-endian unpack once, and returns
 * typed values. No raw ROM-address-as-pointer, no W[] pointer chasing.
 *
 * All decoders are pure reads against immutable ROM, so they are safe
 * to call from anywhere, cheap enough to call per-frame, and require
 * no initialization beyond the ROM being loaded.
 *
 * Status per decoder (see src/rom_decode.c for detail):
 *   rom_terrain_descriptor       - LIVE  (ROM 0x8B200 zone table)
 *   rom_terrain_bitmask_byte     - LIVE  (four per-course tables)
 *   rom_stage_geometry           - LIVE  (W[0x2908..0x2918] slot bases)
 *   rom_terrain_props            - STUB  (needs 0x4F78 dispatch reversed)
 *   rom_balloon_spawn_table      - STUB  (format not yet reversed)
 *   rom_animated_object_spawn    - STUB  (format not yet reversed)
 *
 * STUBs return empty tables so callers can use them without crashing
 * and get a correctness improvement over the "don't call it at all"
 * status quo. Each stub has a concrete TODO pointing at the data to
 * investigate in MAME.
 */
#ifndef ROM_DECODE_H
#define ROM_DECODE_H

#include <stdint.h>
#include <stdbool.h>

/* ==================================================================
 *  Terrain (live)
 * ==================================================================*/

/* One entry from a zone's chunk descriptor list. 5 x int32 BE in ROM,
 * 20-byte stride. See CLAUDE.md:94 and terrain_chunk_visibility.  */
typedef struct {
    int32_t offset;    /* chunk offset added to terrain base to form model_id */
    int32_t grid_dx;   /* chunk grid X relative to camera grid */
    int32_t grid_dz;   /* chunk grid Z relative to camera grid */
    int32_t local_x;   /* chunk-local X offset within its grid cell */
    int32_t local_z;   /* chunk-local Z offset within its grid cell */
} RomChunkDescriptor;

/* Read descriptor `i` (0..59) for `zone` (0..31).
 * Returns false if zone or index is out of range, or if the ROM address
 * would be past ROM_SIZE. Never dereferences a raw ROM address. */
bool rom_terrain_descriptor(int zone, int i, RomChunkDescriptor *out);

/* Number of descriptors per zone (fixed: 60 per game's loop). */
#define ROM_TERRAIN_DESCRIPTORS_PER_ZONE 60

/* Per-cell visibility bitmask byte.
 *   course   0..3
 *   cam_cell 0..127 (cam_grid_x + cam_grid_z * 8)
 *   grid_z   0..15
 * Returns 0 on out-of-range. Bit (0x80 >> (grid_x & 7)) in the byte
 * is set if the chunk at that column is visible. */
uint8_t rom_terrain_bitmask_byte(int course, int cam_cell, int grid_z);

/* Terrain model-id base for a course (1173, 1301, 1429, 1557).
 * Returns -1 on bad course. Wraps course_terrain_base[] so there is
 * one source of truth. */
int32_t rom_terrain_base(int course);

/* ==================================================================
 *  Stage geometry pointers (live)
 * ==================================================================*/

/* The five W[0x2908..0x2918] slot bases per course. Decoded from the
 * M68K dispatch at ROM 0x6BDA. CLAUDE.md:107 has the full table. */
typedef struct {
    uint32_t geom_cell_table;   /* W[0x2908] */
    uint32_t cell_descriptor;   /* W[0x290C] */
    uint32_t height_map;        /* W[0x2910] */
    uint32_t object_mask;       /* W[0x2914] */
    uint32_t collision_zones;   /* W[0x2918] */
} RomStageGeometry;

/* Returns false on bad course. out is populated with ROM addresses,
 * NOT dereferenced — the caller is expected to use vrd* helpers to
 * read through them, which keeps the 64-bit-pointer class of bugs
 * from reappearing here. */
bool rom_stage_geometry(int course, RomStageGeometry *out);

/* ==================================================================
 *  Stubs — format not yet reversed
 * ==================================================================*/

/* Prop entry emitted by terrain_props_dispatch. Shape is a guess;
 * stubs return count=0 so consumers just skip without crashing. */
typedef struct {
    uint16_t model_id;
    int32_t  x, y, z;
    uint8_t  group;
    uint8_t  flags;
} RomPropEntry;

/* TODO: reverse dispatch at ROM 0x4F78. See CLAUDE.md "Stubbed" table. */
int rom_terrain_props(int course, RomPropEntry *out, int max);

/* Balloon spawn table — feeds balloon_system_init. TODO: reverse. */
typedef struct {
    uint16_t model_id;
    int32_t  x, y, z;
    uint16_t palette_group;
    uint8_t  color_index;   /* 0..7 matching balloon color ranges */
    uint8_t  flags;
} RomBalloonSpawn;

int rom_balloon_spawn_table(RomBalloonSpawn *out, int max);

/* Animated object spawn table — feeds animated_objects_spawn_batch.
 * Format partially visible at ROM 0xB2B78 + course * 0x1C (CLAUDE.md). */
typedef struct {
    uint16_t model_id;
    int32_t  x, y, z;
    uint16_t anim_group;
    uint8_t  flags;
    uint8_t  _pad;
} RomAnimatedSpawn;

int rom_animated_object_spawn(int course, RomAnimatedSpawn *out, int max);

#endif /* ROM_DECODE_H */
