/*
 * rom_decode.c - Typed ROM data decoders. See include/rom_decode.h.
 *
 * All reads go through vrd_/mem_read_ so region routing + big-endian
 * handling are one place, not duplicated per call site. No raw ROM
 * address is ever dereferenced as a host pointer in this file.
 */
#include "propcycl.h"
#include "rom_decode.h"

/* ==================================================================
 *  Per-course metadata (decoded from M68K dispatches)
 * ==================================================================*/

/* Terrain model-id bases. Same values as game_terrain.c:course_terrain_base;
 * duplicated here as the single source of truth for new code. */
static const int32_t COURSE_TERRAIN_BASE[4] = {
    0x495, 0x515, 0x595, 0x615,
};

/* Per-course terrain bitmask bases. ROM addresses. */
static const uint32_t COURSE_TERRAIN_BITMASK[4] = {
    0x8B280, 0x8BA80, 0x8C280, 0x8CA80,
};

/* Per-course stage geometry pointers. Decoded from M68K dispatch at
 * ROM 0x6BDA (tools/decode_terrain_dispatch.py). Same values as
 * game_terrain.c:course_world_props. */
static const uint32_t COURSE_STAGE_GEOM[4][5] = {
    { 0x0CB410, 0x101CDC, 0x0B971C, 0x0B991C, 0x118550 },  /* course 0 */
    { 0x0E2848, 0x10B29C, 0x0B979C, 0x0B999C, 0x11BDA4 },  /* course 1 */
    { 0x0F5B38, 0x112F68, 0x0B981C, 0x0B9A1C, 0x11F928 },  /* course 2 */
    { 0x0FADEC, 0x11505C, 0x0B989C, 0x0B9A9C, 0x120BF4 },  /* course 3 */
};

/* Zone descriptor table base. 32 pointers at 4-byte stride. */
#define ZONE_TABLE_BASE       0x8B200u
#define ZONE_DESCRIPTOR_BYTES 20u   /* 5 x int32 BE */

/* ==================================================================
 *  Terrain (live)
 * ==================================================================*/

bool rom_terrain_descriptor(int zone, int i, RomChunkDescriptor *out) {
    if (!out) return false;
    if (zone < 0 || zone >= 32) return false;
    if (i < 0 || i >= ROM_TERRAIN_DESCRIPTORS_PER_ZONE) return false;

    uint32_t zone_ptr_addr = ZONE_TABLE_BASE + (uint32_t)zone * 4u;
    if (zone_ptr_addr + 4u > ROM_SIZE) return false;

    /* Read the 32-bit BE ROM pointer at 0x8B200 + zone*4.
     * This is a ROM-relative address that points to that zone's chunk list. */
    uint32_t chunk_list = mem_read32(zone_ptr_addr);
    uint32_t entry      = chunk_list + (uint32_t)i * ZONE_DESCRIPTOR_BYTES;
    if (entry + ZONE_DESCRIPTOR_BYTES > ROM_SIZE) return false;

    out->offset  = (int32_t)mem_read32(entry + 0u);
    out->grid_dx = (int32_t)mem_read32(entry + 4u);
    out->grid_dz = (int32_t)mem_read32(entry + 8u);
    out->local_x = (int32_t)mem_read32(entry + 12u);
    out->local_z = (int32_t)mem_read32(entry + 16u);
    return true;
}

uint8_t rom_terrain_bitmask_byte(int course, int cam_cell, int grid_z) {
    if (course < 0 || course >= 4) return 0;
    if (cam_cell < 0 || cam_cell >= 128) return 0;
    if (grid_z  < 0 || grid_z  >= 16)  return 0;

    /* Each cam_cell row is 0x10 bytes, one byte per grid_z. */
    uint32_t addr = COURSE_TERRAIN_BITMASK[course]
                  + (uint32_t)cam_cell * 0x10u
                  + (uint32_t)grid_z;
    if (addr >= ROM_SIZE) return 0;
    return mem_read8(addr);
}

int32_t rom_terrain_base(int course) {
    if (course < 0 || course >= 4) return -1;
    return COURSE_TERRAIN_BASE[course];
}

/* ==================================================================
 *  Stage geometry (live — returns addresses, not deref'd)
 * ==================================================================*/

bool rom_stage_geometry(int course, RomStageGeometry *out) {
    if (!out) return false;
    if (course < 0 || course >= 4) return false;
    out->geom_cell_table = COURSE_STAGE_GEOM[course][0];
    out->cell_descriptor = COURSE_STAGE_GEOM[course][1];
    out->height_map      = COURSE_STAGE_GEOM[course][2];
    out->object_mask     = COURSE_STAGE_GEOM[course][3];
    out->collision_zones = COURSE_STAGE_GEOM[course][4];
    return true;
}

/* ==================================================================
 *  Stubs — format not yet reversed
 *
 *  Each returns 0 entries so callers can loop over them without any
 *  special-case guarding. Replace the bodies once the format is known.
 * ==================================================================*/

int rom_terrain_props(int course, RomPropEntry *out, int max) {
    (void)course; (void)out; (void)max;
    /* TODO: reverse dispatch at ROM 0x4F78. Entry point has the same
     * shape as world_props_init_dispatch (ROM 0x6BDA) — a 4-entry jump
     * table of per-course emission blocks. Approach:
     *   1) Extend tools/decode_terrain_dispatch.py with a second pass
     *      that parses 0x4F78 using the same opcode recognition.
     *   2) Each block writes a set of W[] slots giving ROM base +
     *      count for a per-course prop table.
     *   3) The prop table itself is probably an array of 8- or 12-byte
     *      entries (model_id u16, x/y/z i32, flags u16). Infer from
     *      stride by cross-referencing with game_terrain.c:571
     *      (terrain_props_dispatch) which iterates these. */
    return 0;
}

int rom_balloon_spawn_table(RomBalloonSpawn *out, int max) {
    (void)out; (void)max;
    /* TODO: reverse balloon spawn format. Starting points:
     *   - balloon_system_init (game_objects.c) is currently stubbed;
     *     its original body reads a pointer from W[] + course offset
     *     and iterates a table of balloon records.
     *   - Model IDs 0x2CE..0x307 = balloon meshes; the spawn table
     *     should reference these indirectly. Grep ROM for u16 values
     *     in [0x2CE..0x307] around the pose tables near 0x2D000.
     *   - MAME trace: enable attract-mode balloons, dump first N
     *     balloon positions per frame, then search ROM for those
     *     positions to locate the table base. */
    return 0;
}

int rom_animated_object_spawn(int course, RomAnimatedSpawn *out, int max) {
    (void)course; (void)out; (void)max;
    /* TODO: reverse animated object spawn format. Known anchors:
     *   - animated_objects_spawn_batch (game_objects.c) computes
     *       rom_off = (*param_1) * 0x1C + 0xB2B78
     *     so the record stride is 0x1C bytes. Per-course count lives
     *     at a separate W[] slot filled by a sister dispatch.
     *   - First u16 of the record is model_id. Next fields probably
     *     include position (3 x i32), rotation group (u16), flags.
     *   - Decode 0xB2B78 + course_offset * 0x1C * max_count, compare
     *     against MAME live-object state in attract mode. */
    return 0;
}
