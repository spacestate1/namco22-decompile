#define _GNU_SOURCE
/*
 * 3D Renderer - DSP polygon command interpreter + textured model rendering
 *
 * Reads DSP command buffer written by game code (game_dsp3d.c),
 * then renders 3D objects from point ROM with texture lookup.
 *
 * DSP command protocol (from Ghidra decompiled game code):
 *   0x8000 (6 words):  place object, relative to camera
 *   0x8002 (13 words): place object with rotation matrix
 *   0x8008 (15 words): set transform context (matrix + position)
 *   0x8009 (4 words):  set render parameters (CZ/fog type)
 *   0x800a (6 words):  render model at position
 *   0x8010 (2-4 words): end frame / end list
 *
 * Point ROM polygon format (chunkLength-based parsing):
 *   model_id -> pointrom[model_id] = addr1 (object list)
 *   pointrom[addr1+N] = object addresses (-1 terminated)
 *   Each object: word[0] = chunkLength, then sequential packets.
 *   Each packet: word[0] = packetLength, then data words.
 *   Quad packet data (pktLen >= 0x14):
 *     [0-1]   flags, color
 *     [2-3]   texture info
 *     [4-11]  UV coords: 4 vertices x (U, V), each & 0xFFF
 *     [12-23] XYZ coords: 4 vertices x (X, Y, Z)
 *
 * Texture lookup (2-stage tilemap from pr1ccrl.3d + pr1ccrh.1d):
 *     tilemap_index = ((v & 0xFFF0) << 4) | ((u & 0xFF0) >> 4)
 *     tile = tilemap[index] (16-bit LE)
 *     attr = tilemap_attr[index] (4-bit: rotate, flipX, flipY, high_bit)
 *     pen = tex_tiles[tile * 256 + transformed_local_offset]
 */
#include "quad_gl.h"
#include "slave_list.h"
#include <time.h>
#include "propcycl.h"
static int g_txt_dirty = 1;
double g_perf_pdp, g_perf_flush, g_perf_txt, g_perf_txr, g_perf_txc, g_perf_txu;
/* Deterministic text-cache counters. The wall-clock `text = render` figure is
 * useless on this box (unrelated jobs at 350%+ CPU); the MISS COUNT is exact and
 * is what the cache fix is actually about -- one miss = one full 640x480 repaint
 * plus a 307200-iteration alpha count. */
long g_txt_calls, g_txt_miss;
int g_texthash_fade;
double g_perf_fogp;   /* flush sub-phases; bake/gl/clip are the engine's (quad_gl.h) */
extern double g_bake_texels;   /* engine/tex_bake.c */
/* THE PHASE TIMERS COST 10% OF RUNTIME WHEN NOBODY IS MEASURING.
 *
 * `perf record` on a plain run -- no PROPCYCL_PERF set -- put
 * __vdso_clock_gettime at 10.3% of all samples, third hottest in the whole
 * program. There are 15 call sites and one of them is INSIDE the per-quad
 * bake loop, so a 1789-quad frame made ~3600 clock_gettime calls, ~215k a
 * second at 60 Hz, purely to accumulate totals that were never printed.
 *
 * Gated on the flag now. `g_perf_enabled` is set once from main.c -- never
 * call getenv() in a frame loop, that is register row 40.
 *
 * MEASURED NEUTRAL, AND THE 10% WAS A MISREAD. Wall clock over 1500 frames
 * is 7.32 s with the timers off against 7.33 s with them on -- no difference.
 * perf attributed 10% to __vdso_clock_gettime, but resolving the callers
 * shows they come overwhelmingly from SDL and libnvidia-eglcore, not from
 * here, and a vdso clock read costs no syscall. Kept because paying for
 * instrumentation nobody is reading is still wrong, NOT because it is a
 * speedup: do not cite it as one. A flat profile attributes the SYMBOL, not
 * the caller -- check who is calling before optimising it. */
/* g_perf_enabled now lives with the engine's timer (engine/quad_gl.c). */
static double rperf(void) { return eng_now(); }

#include <GL/gl.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>

/* From renderer_texture.c — UV-bbox texture baker with FNV-1a-hashed cache.
 * cmode (color depth, 4 bits) is decoded inside the baker per the spec in
 * decompiled/annotations.md "Texture cmode". */
extern int g_tex_opaque;   /* renderer_texture.c: 1 = no pen-0 keying */
/* g_tex_clipbox: the engine's (quad_gl.h) */      /* PROPCYCL_TEX_CLIPBOX=1: key the bake on the clipped polygon again */
/* WIDESCREEN (Hor+). The scene is 640x480 in its own coordinates; on a wider
 * window main.c sets these to (-E, 640 + E) so the 3D world gets extra room
 * at the sides without moving or rescaling anything in 0..640. The camera's
 * projection is untouched (centre x stays 320), so it is the same view with
 * more of the world on each side. The 2D text/sprite layers and the HUD stay
 * in the 4:3 centre, where the game drew them. 0/640 = original 4:3. */
/* g_scene_x0/x1: the engine's (quad_gl.h) */
/* HUD TO THE CORNERS in widescreen: the gauges slide out by the extra width
 * (left ones left, right ones right; anything within 60 px of the centre, the
 * pendulum and its arrow plate, stays). Only during the countdown and
 * gameplay (state 3, sub 5 / 3) -- every other screen, the results board
 * included, is drawn exactly as the game laid it out. PROPCYCL_WIDE_HUD_CENTER=1
 * keeps the HUD in the 4:3 centre. */
int g_wide_hud_center = 0;
static int hud_corners_shift(double centre_off)
{
    extern intptr_t _W[];
    if (g_scene_x0 >= 0.0f || g_wide_hud_center) return 0;
    if ((int)_W[0x0CBC] != 3) return 0;
    int sub = (int)_W[0x0CC0];
    if (sub != 3 && sub != 5) return 0;
    int E = (int)(-g_scene_x0);
    return centre_off < -60.0 ? -E : centre_off > 60.0 ? E : 0;
}
extern int tex_frame_hits, tex_frame_misses, tex_cache_evictions;
extern int tex_reallocs, tex_subimages;
GLuint bake_quad_texture(int min_u, int min_v, int range_u, int range_v,
                         int texbank, int pal_group, int cmode,
                         float *out_su, float *out_sv);

/* ========== Texture Lookup ========== */

/* Get 4-bit tile attribute from packed attribute ROM (pr1ccrh.1d).
 * Two attributes per byte: even index = low nibble, odd = high nibble. */
static int get_tile_attr(int tilemap_index) {
    if (!g_texture_tilemap) return 0;
    /* Attributes are stored in the second part of the tilemap buffer,
     * starting at offset 0x200000 (after pr1ccrl.3d). Packed 2 per byte. */
    int byte_idx = tilemap_index >> 1;
    int attr_offset = 0x200000 + byte_idx;
    if (attr_offset >= (int)TEXTUREMAP_SIZE) return 0;
    uint8_t byte_val = g_texture_tilemap[attr_offset];
    /* HIGH nibble on EVEN index -- pc_raster_model.py's init_tables, which
     * is gated byte-exact against MAME. See CLAUDE.md's pipeline section. */
    if (tilemap_index & 1)
        return byte_val & 0xF;
    return (byte_val >> 4) & 0xF;
}

/* Full texture lookup: UV -> tile index -> attributes -> pixel pen value.
 * Matches MAME hardware behavior and verified Python reference renderer. */
static uint8_t texture_pen_lookup(int u, int v, int texbank) {
    if (!g_texture_data || !g_texture_tilemap) return 0;

    u &= 0xFFF;
    v = (v & 0xFFF) | (texbank * 0x1000);  /* apply texture bank offset */

    /* Stage 1: tilemap index from UV (256-column grid, banked) */
    int tilemap_index = ((v & 0xFFF0) << 4) | ((u & 0xFF0) >> 4);

    /* Stage 2: read 16-bit tile index (little-endian from pr1ccrl.3d) */
    int byte_off = tilemap_index * 2;
    if (byte_off + 1 >= 0x200000) return 0;  /* pr1ccrl.3d is 2MB */
    uint32_t tile = g_texture_tilemap[byte_off] | (g_texture_tilemap[byte_off + 1] << 8);

    /* Stage 3: read tile attributes */
    int attr = get_tile_attr(tilemap_index);

    /* Attribute bit 0: extends tile index to 17 bits */
    if (attr & 0x1)
        tile |= 0x10000;

    /* Stage 4: transform local pixel coordinates within tile */
    int local_x = u & 0xF;
    int local_y = v & 0xF;

    /* FLIPS FIRST, TRANSPOSE LAST -- the reference builds its swizzle table
     * `if a&4: ix=15-ix; if a&2: iy=15-iy; if a&8: ix,iy=iy,ix`. Transposing
     * first is a different transform whenever it combines with exactly one
     * flip (attr 0xA and 0xC). */
    if (attr & 0x4) /* Flip X */
        local_x = 15 - local_x;
    if (attr & 0x2) /* Flip Y */
        local_y = 15 - local_y;
    if (attr & 0x8) { /* transpose */
        int tmp = local_x;
        local_x = local_y;
        local_y = tmp;
    }

    /* Stage 5: read 8bpp pixel from tile data */
    uint32_t pixel_offset = tile * 256 + local_y * 16 + local_x;
    if (pixel_offset >= TEXTURE_TOTAL_SIZE) return 0;
    return g_texture_data[pixel_offset];
}

/* Look up RGB color for a pen value using the correct palette group.
 * palette_ram is planar: R at [0x00000+idx], G at [0x08000+idx], B at [0x10000+idx].
 * pal_group (0-127) selects which 256-entry palette to use.
 * Returns 0 if pen is transparent (pen == 0). */
static uint32_t pen_to_rgb(uint8_t pen, int pal_group) {
    if (pen == 0) return 0;  /* transparent */
    int idx = (pal_group & 0x7F) * 256 + pen;
    uint8_t r = g_sys.palette_ram[0x00000 + idx];
    uint8_t g = g_sys.palette_ram[0x08000 + idx];
    uint8_t b = g_sys.palette_ram[0x10000 + idx];
    return (r << 16) | (g << 8) | b;
}

/* Grayscale fallback when palette_ram is not populated */
static float pen_to_brightness(uint8_t pen) {
    if (pen == 0) return 0.0f;
    float t = (float)pen / 255.0f;
    return 0.15f + 0.85f * sqrtf(t);
}

/* Check if palette_ram has been initialized (palette_fog_init has run) */
static int palette_initialized(void) {
    /* Group 1, pen 1 should be non-zero after palette_fog_init */
    return g_sys.palette_ram[256 + 1] != 0 ||
           g_sys.palette_ram[0x8000 + 256 + 1] != 0 ||
           g_sys.palette_ram[0x10000 + 256 + 1] != 0;
}

/* ========== Camera State ========== */

static float cam_x, cam_y, cam_z;
static float cam_zoom;

/* Current transform for 0x8008 command chain */
static float cur_rot[3][2];  /* 3 axis sin/cos pairs */
static float cur_pos[3];     /* XYZ position */
/* Raw Q15 rotation words from the last 0x8008, in the order the record
 * carries them ([3..8] = three pairs). placement_matrix() wants Q15 ints,
 * not the floats cur_rot holds for the retired GL path. */
static int32_t cur_rot_q15[6];
static int      cur_rot_valid = 0;
/* 0=off  1=(sin,cos) as-is  2=swap to (cos,sin).
 * Now 1. Every 0x8008 producer writes (sin, cos) -- the order the ROM and
 * MAME's own command buffer carry -- since scene_node_render and
 * camera_dsp_terrain_render were brought in line with dsp_cmd_object_transform
 * (register row 95). The swap that used to live here was compensating for
 * ONE producer and breaking the other, which is what left the demo rider
 * ~130 deg off the bike. PROPCYCL_ROT8008=2 restores it for A/B. */
int             g_rot8008 = 1;
int             g_root15  = 1;   /* 15-word 0x8008 straight to 0x800a is a ROOT too (see the 0x800a case) */
/* Root-node rotation: the 11-word 0x8008 that precedes a run of children.
 * A child's 0x8008 carries a LOCAL offset that has to be rotated into world
 * space before it is added to the root's camera-relative position. */
static int32_t root_rot_q15[6];
static int      root_rot_valid = 0;
int             g_asm_mode = 5;   /* 5 = slot table, R_child.M_parent (measured); 0 = flat */
int             g_nodes_at = -1;
/* 0x8008 TRANSFORM ACCUMULATOR (g_asm_mode 3).
 *
 * scene_node_render serialises an articulated rig as a flat run of 0x8008
 * records and the DSP keeps the running transform:
 *   11-word 0x8008 (hi4 == 0, a ROOT): resets it, and its own 0x800a
 *       carries the camera-relative base position
 *   15-word 0x8008 (hi4 < 0, a CHILD): carries a LOCAL offset in its
 *       parent's frame, which concatenates
 * so   t += M * L ;  M = M * R   walking the run, and each 0x800a draws at
 * base + t with matrix M. Without this every part was drawn at its own
 * local offset from the nearest root -- which is why code 163, four links
 * down the chain with a local offset of (0,0,0), landed exactly ON the
 * body instead of 785,916,-205 away from it. */
static int32_t acc_M[3][3] = {{0x7FFF,0,0},{0,0x7FFF,0},{0,0,0x7FFF}};
static int32_t acc_t[3] = {0,0,0};

/* TRANSFORM SLOT TABLE (g_asm_mode 5/6) -- how the master composes a TREE
 * from a flat command stream.
 *
 * Every animation node carries a small unique id in field 0x1e (0..26 for
 * the rider rig). scene_node_render puts the PARENT's id and the node's
 * own id into the 0x8009 that follows a child's 0x8008, and the node's own
 * id into every 0x800a:
 *     child:  0x8008(R, L)  0x8009(3, parent_id, own_id)  0x800a(model, own_id, base)
 *     root :  0x8008(R)                                    0x800a(model, own_id, base)
 * i.e.  T[own] = T[parent] o (R, L)   and   draw at base + T[own].t with T[own].M.
 *
 * Siblings (nodes 1-6 all have parent 0) therefore compose against the SAME
 * parent, which a sequential accumulator got wrong -- measured, chaining
 * scored median 2093-2360 against the recording where flat was 925. */
#define TSLOTS 64
/* TWO accumulated matrices per slot, from the SAME record angles:
 *   M -- Euler XYZ, what the vertices are drawn with. Measured against the
 *        recording per part: hips/shins/feet 1-2 deg, spine 2-3, median 3.
 *   P -- Euler ZXY, used ONLY to rotate a child's local offset into the
 *        parent's frame. Measured: median position error 19 units with
 *        ZXY-composed offsets; every translation rule tried with the XYZ
 *        matrix (L.M, L.M^T, child's M, its transpose, both compose
 *        orders) scored 576-1285.
 * Neither order satisfies both, so this is what the data supports until
 * the master's own program (pc_tms_interp.py) is ported and settles it. */
static struct { int32_t M[3][3]; int32_t P[3][3]; int32_t t[3]; int valid; } g_tslot[TSLOTS];
static int32_t pend_R[3][3], pend_RP[3][3], pend_L[3];
static int     pend_valid = 0, pend_root = 0;
static int32_t pend_scale = 0x7FFF;
/* THE 0x8008 RECORD IS A PER-SLOT OP LIST, and 0x8009 [a,b,c] COMPOSES SLOTS.
 *
 * Read off every producer in the ROM (2026-09-22, the count-down marshal):
 *   [0x8008, slot, op..., -1] with op 0 = translate (x,y,z), op 1 = rotate
 *   (three sin/cos pairs + a flags word), op 6 = a 3x3 (nine words); the
 *   ops apply in order and the result is written to T[slot].
 *     scene_node_render child   [3, 1 R f, 0 L]         15 words
 *     scene_node_render root    [own, 1 R f]            11 words
 *     bonus_model_dsp_setup     [4, 0 (0,0x249,0), 1 R f]  15 words -- op 0 FIRST
 *     scene_node_render_scaled  [4, 6 diag(s)]          13 words
 *   0x8009 [a, b, c] is T[c] = T[a] then T[b] (row vectors). The rig's
 *   [3, parent, own] is exactly "own = local then parent"; the count-down
 *   board's [3, 4, 3] + [3, 11, 21] is "local then SCALE, then parent", and
 *   dsp_emit_articulated_model's [4, 11, 4] hangs the board's frame on its
 *   parent for the plates hud_draw_3d_sprite then attaches with [3, 4, 3].
 * The walker used to know only the rig subset: it read the root's op-0-first
 * record's translation as rotation pairs, and it composed 0x8009 against the
 * LAST 0x8008 rather than slot a, so the board picked up the marshal root's
 * transform and the plates never reached it. PROPCYCL_SLOT_LEGACY=1 restores
 * the old walk. */
int     g_slot_legacy = 0;
static int pend_nmat_slot = -1;
/* The 13-word 0x8008 SCALE record's diagonal, -1 when none is pending.
 * See the note in the 0x8008 case. */
static int32_t pend_nscale = -1;
static int32_t pend_nmat[9];
static int     pend_nmat_valid = 0;
/* Q15 uniform scale applied to the drawn object matrix; 0x7FFF = 1.0.
 * Fed by the 13-word 0x8008 scale record when it is diagonal. */
int32_t g_obj_scale = 0x7FFF;

/* THE 13-WORD 0x8008 IS A FULL 3x3, NOT A SCALAR.
 *
 * It was read as `pend_nscale = word[3]` -- one word of nine -- because the
 * only producer examined when that branch was written was
 * `scene_node_render_scaled`, which emits the DIAGONAL diag(s,s,s) (register
 * row 89, measured on a frame where s was 0). `hud_draw_wing_element`
 * (game_hud.c, ROM 0x015F30) writes the same record type with a GENERAL
 * matrix:
 *
 *     [3] dx   [4] 0   [5] slope_x      (W[0x166B4])
 *     [6] dy   [7] 0   [8] slope_y      (W[0x166B8])
 *     [9] dz  [10] 0  [11] slope_z      (W[0x166BC])
 *
 * -- the basis that lays the PLAYER'S SHADOW flat on the terrain it is
 * falling on. Keeping only word[3] threw the whole slope basis away and then
 * applied an offset term as a uniform scale, so the shadow drew as a full
 * size unflattened wedge whose size swung every frame.
 *
 * Applying all nine is backward compatible with row 89: for a diagonal
 * diag(s,s,s) the matrix product is exactly s*M, which is what the scalar
 * path computed, and an all-zero N still collapses every vertex to one point
 * (MAME's bbox for codes 119/120 is x[36..36] y[240..240]).
 *
 * PROPCYCL_NO_NMAT=1 restores the scalar reading for A/B.
 * PROPCYCL_NMAT_ORDER=1 composes M.N instead of N.M. */
int32_t g_obj_nmat[9];
int     g_obj_nmat_valid = 0;
int     g_no_nmat = 0;
/* PROPCYCL_NMAT_REPLACE=1: the 13-word matrix REPLACES the object rotation
 * instead of composing with the preceding 11-word 0x8008's. The record is a
 * complete 3x3 -- it already carries the ground basis AND the offsets -- so
 * composing may double-rotate. */
int     g_nmat_replace = 0;
int     g_nmat_compose = 0;
int     g_nmat_nohalf  = 0;
/* 2 = column-major, N.M. MEASURED, not assumed: over 644 emitted shadow
 * records on course 0, against MAME's own code-144 bbox (x[318..454] on every
 * sampled frame of dumps/gameplay, i.e. width 136 constant):
 *
 *   layout        width p25/med/p75    max width   wild frames
 *   row-major     107 / 113 / 148        196848        29
 *   column-major  117 / 131 / 131          3285        20
 *
 * Column-major clusters on MAME's constant width and is two orders of
 * magnitude better bounded. N.M vs M.N (2 vs 3) the data cannot separate --
 * they agree to ~1% on every statistic -- so 2 is kept because the object's
 * own basis is the innermost transform. */
int     g_nmat_order = 2;
int g_mat_dump = 0;
int g_ik_dump = 0;
int g_rig_dump = 0;
int g_sprcall = 0;
int g_cglog = 0;
char g_feed_dir[256] = {0};
int  g_feed_frame = -1;
int g_rig_calls = 0;   /* PROPCYCL_MATDUMP=1: per-0x800a object matrix + scale */  /* 0=add raw  1=rotate by child's R  2=rotate by root's R */
static int cur_priority;
/* Sentinel model id meaning "define the transform slot, draw nothing".
 * No point-ROM object uses it. */
#define RIG_XFORM_ONLY (-2)
int g_cmd_dump = 0;
int g_draw_mat = 0;   /* PROPCYCL_DRAWMAT=1 */
int g_hud_screen_off = 0;  /* PROPCYCL_NO_HUDFIX=1 */
/* THE SECOND WORD OF EVERY 0x8000 / 0x8001 / 0x8002 HEADER IS A VIEWPORT
 * INDEX, not a priority. Measured in MAME's own CPU command list (the poly
 * dumps carry it at word 0x4100/0x6100): every HUD entry sits under
 * `0x8001, 1` and everything in the world under `..., 0`, and the master's
 * output list switches 0x15 viewport records exactly where that word
 * changes -- HUD first in gameplay, world first in the attract demo. It is
 * the first argument of dsp_cmd_place_object_rotated_abs / dsp_cmd_set_camera
 * (FUN_0000e37e passes 1 for the reels, the sky emitter passes 1, the world
 * passes 0). The 0x8008 record's [1] word is something else (0/1/3/4 across
 * the rig) and does not change the viewport. */
static int cur_viewport = 0;
static int cur_hdr = 0x8000;   /* the last mode header (0x8000/1/2): see hud_screen_space */
int g_hud_by_model = 0;   /* PROPCYCL_HUD_BY_MODEL=1: the old 610..637 model-id rule, for A/B */
int g_hud_norot = 0;      /* PROPCYCL_HUD_NOROT=1: drop the HUD entries' own rotation again */
int g_hud_flat = 0;       /* PROPCYCL_HUD_FLAT=1: the old flat, fully-lit HUD stand-in */
int g_pr_calls = 0;
int g_cmd_word = 0;
int g_cmd_mode = 0;
int cur_priority_pub = 0;

/* ========== Helpers ========== */

static float dspfixed_to_float(int32_t val) {
    return (float)val / 32768.0f;
}

/* Read a 24-bit value from point ROM */
static int32_t point_read(uint32_t addr) {
    if (addr < g_pointrom_count)
        return g_pointrom[addr];
    return 0;
}

/* DSP command buffer base — set each frame from double-buffer state */
static uint32_t cmd_buf_base = 0;
/* How many words the CPU actually wrote into cmd_buf_base THIS frame, from
 * its own cursor W[0x0CA4]. **-1 = unknown, walk the whole buffer; 0 means the
 * CPU wrote NOTHING this frame and the correct picture is NOTHING.**
 *
 * Those two were conflated (0 meant both) and the test was `offset >
 * cmd_buf_base`, so a cursor sitting exactly AT the base -- the CPU wrote
 * nothing -- fell through to the whole-buffer walk. A user reported it as "when
 * it says day 1 the rider models come back up, make sure things are getting
 * cleared properly", and that is exactly what it was: on the ADVANCED/DAY1
 * screen (state 3 sub 23) the cursor reads 0x10400, the base, on every frame,
 * and the walker then read the entire stale buffer -- the CONTROLS tutorial had
 * left an 18-record rider chain in its tail, under the shorter mode-select list
 * that later frames had overwritten the prefix with. Measured: f1800 (sub 13)
 * 10 objects, f1805 (sub 23) 28 -- the extra 18 being codes 119-136 and 163,
 * the bike+rider, which MAME's own polygon dump for that screen does not
 * contain. Nothing emits them: `camera_dsp_terrain_render`, `scene_node_render`
 * and `camera_update_main` are all confirmed never to run there, and every one
 * of the 60 list writes on that frame comes from `dsp_cmd_set_camera`.
 * PROPCYCL_NO_EMPTYLIST=1 restores the old conflation for A/B.
 *
 * The display list is what the CPU wrote this frame, not whatever is left in
 * the buffer. Nothing clears the buffer between frames and several attract
 * phases do not terminate their list -- `dsp_cmd_set_camera` (ROM 0x021FC0)
 * just advances the cursor and returns -- so a phase that emits a short list
 * left the walker running on into the PREVIOUS frame's commands.
 *
 * Measured: through the whole logo phase (live f302-600) our emitted
 * placements are BYTE-IDENTICAL frame to frame -- f320 and f500 match exactly,
 * 26 placements, while MAME emits [0, 108] -- because we were re-rendering the
 * last cinematic-flyover frame for 300 frames. Register row 24.
 * PROPCYCL_NO_CURSOR_BOUND=1 reverts for A/B. */
static int cmd_buf_words = -1;
int g_no_cursor_bound = 0;
int g_no_emptylist = 0;
int g_raw_objshift = 0;
int g_hud_vp1only  = 0;
int g_hard_8010 = 0;
int g_max_data = 0;
int g_walk_exit = 0;

/* THE PER-OBJECT ZSORT BIAS, carried by the `0x8010, 3, <shift>, -1` marker.
 * This renderer is painter's-algorithm with no depth buffer, so an object
 * standing ON terrain has no way to win the sort on depth alone -- its quads
 * and the ground's straddle the same zsort and the order flips as the camera
 * moves. The hardware's answer is this bias, and the game uses it exactly
 * there: measured in dumps/gameplay/poly_f3340.bin, the signpost (code 828)
 * carries -8960, the spectators 274/332 carry -8704/-9216, scenery 843/844
 * carry -5120/-3840, and the distant building 970 carries +10240 (pushed
 * BACK). Everything else is 0.
 *
 * `scenery_object_render` emits the marker on CHANGE only, latching the live
 * value in W[0x4704] -- so this must persist across records exactly as that
 * latch does, and the 2-word `0x8010, -1` form is how the game clears it back
 * to 0 (which is why every captured frame ends at 0).
 *
 * framedump.c has read this since it was written; the LIVE walker parsed the
 * marker and threw the value away, so every live placement sorted at raw
 * depth. That asymmetry is why all six frame gates passed while the live
 * signpost flickered against the ground it stands on.
 * PROPCYCL_NO_OBJSHIFT=1 reverts for A/B. */
static int32_t g_obj_shift = 0;
int g_no_objshift = 0;

/* Forward declarations */
static void render_level_grid(void);

/* Read native 32-bit from DSP command buffer */
/* THE MASTER DSP'S RAM IS 24 BITS WIDE (register row 191). The CPU writes
 * 32-bit longs into it, the top byte is simply not stored, and the master reads
 * a signed 24-bit word -- which is why MAME's DSP-RAM reads come back with the
 * top byte forced (tools/l3 notes). The game RELIES on it: the ending's
 * floating island sinks by `lsr.l #4` of a speed that goes negative (ROM
 * 0x02BFFC/0x02C054), so on the machine its Y runs 0x0000A000 -> 0xF000A008 ->
 * 0xE000A015 ... (MAME, counters 1141..1143) and only the low 24 bits --
 * 0xA000, 0xA008, 0xA015 -- ever reach the geometry. Reading the whole 32 bits
 * threw the island 268 million units away and the terrain, tower and islands
 * of ending phase 1 vanished for the last 420 frames of it.
 * PROPCYCL_DSP32=1 restores the 32-bit read for A/B. */
int g_dsp32 = 0;
/* PAUSE 360 (pause_world.c): a second display list, walked after the
 * frozen one while paused, and a filter so the second walk draws only
 * placements the first did not. Keyed on (code, camera-relative position),
 * which is identical between the two because the camera is the same. */
int g_p360_new, g_p360_dup;
static const int32_t *g_pdp_words;   /* non-NULL: walk this instead of DSP RAM */
static int g_pdp_nwords;
static int g_seen_mode;              /* 0 off, 1 record, 2 skip what was recorded */
#define SEEN_SLOTS 16384
/* Two tables. The FROZEN frame claims a POSITION, whatever model sits there:
 * scenery comes in near/far detail versions at the same spot, and the extra
 * walk (other headings put a chunk in another LOD band) must not draw the
 * second one over the game's own. Inside the extra walk the MODEL is part of
 * the key too, because several parts of one object share its origin (a
 * windmill's tower and sails). */
static uint64_t seen_pos[SEEN_SLOTS], seen_full[SEEN_SLOTS];
/* 1 = k was already in tab; inserts it when `add` */
static int seen_probe(uint64_t *tab, uint64_t k, int add) {
    k |= 1;                                              /* 0 marks an empty slot */
    const uint64_t hh = k * 0x9E3779B97F4A7C15ull;
    for (unsigned i = (unsigned)(hh >> 50) % SEEN_SLOTS, n = 0; n < SEEN_SLOTS; n++, i = (i + 1) % SEEN_SLOTS) {
        if (tab[i] == k) return 1;
        if (tab[i] == 0) { if (add) tab[i] = k; return 0; }
    }
    return 0;
}
/* Returns 1 if this placement must be skipped. */
static int seen_check_add(int code, float px, float py, float pz) {
    const uint64_t pos = ((uint64_t)(uint32_t)(int32_t)px << 32)
                       ^ ((uint64_t)(uint32_t)(int32_t)py << 16) ^ (uint64_t)(uint32_t)(int32_t)pz;
    if (g_seen_mode == 1) { seen_probe(seen_pos, pos, 1); return 0; }
    if (seen_probe(seen_pos, pos, 0)) return 1;          /* the frozen frame has something here */
    return seen_probe(seen_full, pos ^ ((uint64_t)(uint32_t)code << 48), 1);
}

static int32_t cmdram_read32(int word_index) {
    if (g_pdp_words) {
        if (word_index < 0 || word_index >= g_pdp_nwords) return 0;
        int32_t v = g_pdp_words[word_index];
        return g_dsp32 ? v : (int32_t)((uint32_t)v << 8) >> 8;
    }
    uint32_t byte_off = cmd_buf_base + (uint32_t)word_index * 4;
    if (byte_off + 3 >= DSPRAM_SIZE) return 0;
    int32_t val;
    memcpy(&val, g_sys.dspram + byte_off, 4);
    if (!g_dsp32) val = (int32_t)((uint32_t)val << 8) >> 8;
    return val;
}

/* ========== Model Rendering ========== */

/* Render a single polygon object from point ROM using chunkLength-based parsing.
 * Extracts UV coords from each packet and samples texture for face color. */
static int g_frame_poly_count = 0;
#define MAX_FRAME_POLYS 5000  /* polygon budget per frame to prevent freezes */


/* ===================== hardware geometry path (geo_hw.c) =================
 * ON BY DEFAULT (PROPCYCL_GEO_HW=0 reverts to the superseded GL projection
 * for A/B). Kept alongside the original GL path rather than replacing it,
 * so the two can be A/B'd on the same frame -- a control, not a leap of
 * faith.
 *
 * Quads arrive ALREADY PROJECTED in 1/16-pixel screen coordinates, so
 * this draws them in an ortho 2D pass. No z-clip yet: quads with any
 * vertex behind the eye are dropped whole (the oracle interpolates them
 * instead, so silhouettes at the screen edge will be wrong until that
 * lands).
 */
#include "geo_hw.h"
#include "fog_hw.h"
#include "sprite_hw.h"
#include "text_hw.h"
#include "ui_menu.h"
void framedump_render(geo_quad_cb cb, void *user);
void framedump_render_words(const uint32_t *words, geo_quad_cb cb, void *user);
#include "master_dsp.h"
const sprite_state *framedump_current_sprites(void);
const text_state   *framedump_current_text(void);

int framedump_is_active(void);
static sprite_state g_live_spr;
static text_state   g_live_txt;
static int          g_live_ok;

static void live_2d_refresh(void)
{
    g_live_ok = 0;
    /* Gate on the REPLAY, not on whether it carried sprites: a capture with
     * no 2D data would otherwise fall through to live memory, which during
     * a replay is stale (the game loop is skipped) and draws garbage. */
    if (framedump_is_active()) return;
    if (!fog_load_live()) return;
    if (!sprite_load_live(&g_live_spr)) return;
    text_load_live(&g_live_txt);
    g_live_ok = 1;
}


/* True when the pixel-exact sprite/text stages have live state for this
 * frame, i.e. the legacy renderer_2d layer is redundant. */
int renderer3d_live_2d_ok(void) { return g_live_ok; }

/* |t| census for comparing the live coordinate scale against the
 * recording's primitive records. */
static double g_dist2[8192]; static int g_dist_id[8192]; static int g_dist_n;
void geohw_note_dist2(double d2, int id) {
    if (g_dist_n < 8192) { g_dist_id[g_dist_n] = id; g_dist2[g_dist_n++] = d2; }
}
static int dcmp(const void *a, const void *b) {
    double x = *(const double*)a, y = *(const double*)b;
    return x < y ? -1 : x > y ? 1 : 0;
}
void geohw_report_dist(void) {
    if (!g_dist_n || !propcycl_verbose()) { g_dist_n = 0; return; }
    /* extremes with their model ids, before sorting destroys the pairing */
    { int lo = 0, hi = 0;
      for (int i = 1; i < g_dist_n; i++) {
          if (g_dist2[i] < g_dist2[lo]) lo = i;
          if (g_dist2[i] > g_dist2[hi]) hi = i;
      }
      printf("    [DIST] nearest model=%d d=%ld | farthest model=%d d=%ld\n",
             g_dist_id[lo], (long)sqrt(g_dist2[lo]),
             g_dist_id[hi], (long)sqrt(g_dist2[hi]));
      /* how many placements are absurdly far (> 4x the whole 8x16 grid)? */
      int nfar = 0; for (int i = 0; i < g_dist_n; i++) if (sqrt(g_dist2[i]) > 6291456.0) nfar++;
      printf("    [DIST] placements beyond 4x grid: %d/%d\n", nfar, g_dist_n);
      /* which models are the far ones? histogram the top offenders */
      { int ids[24], cnt[24], nid = 0;
        for (int i = 0; i < g_dist_n; i++) {
            if (sqrt(g_dist2[i]) <= 6291456.0) continue;
            int k = 0; for (; k < nid; k++) if (ids[k] == g_dist_id[i]) break;
            if (k == nid && nid < 24) { ids[nid] = g_dist_id[i]; cnt[nid] = 0; nid++; }
            if (k < 24) cnt[k]++;
        }
        printf("    [DIST] far models:");
        for (int k = 0; k < nid; k++) printf(" %d(x%d)", ids[k], cnt[k]);
        printf("%s\n", nid >= 24 ? " ..." : ""); } }
    qsort(g_dist2, g_dist_n, sizeof(double), dcmp);
    #define Q(f) ((long)(sqrt(g_dist2[(int)((g_dist_n-1)*(f))])))
    printf("    [DIST] n=%d min=%ld p25=%ld median=%ld p75=%ld max=%ld\n",
           g_dist_n, Q(0.0), Q(0.25), Q(0.5), Q(0.75), Q(1.0));
    #undef Q
    g_dist_n = 0;
}

int g_only_code = -1;
/* 0x8002/0x8001 entry pairs are SIN-FIRST -- 0 means "no swap".
 *
 * This shipped at 1 (swap to cos-first) on the strength of one measurement:
 * code 874's Y pair read (11980, 30503), which is (cos .3656, sin .9309)
 * against MAME's own object matrix for 874. The pair really was backwards --
 * but in the PRODUCER, not the wire format. objects_render_master /
 * objects_render_attract_mode took the HIGH half of the ROM's two 32-bit
 * trig reads instead of the low half; see the note at game_objects.c's
 * W[0x16668] assignment. Two independent ROM emitters settle the format:
 *   scenery_object_render @0x013AC0 writes the X and Z pairs as the
 *     literals (0, 0x7FFF) -- identity read sin-first, 90 deg read cos-first
 *   dsp_cmd_place_object_rotated @0x022008 writes 0x20B004 (sin) before
 *     0x20B006 (cos)
 * and the swap is what removed the clouds from the title screen: it turned
 * the sky's identity X pair (0, 0x7FFF) into a 90 deg rotation, so the
 * composed matrix came out Ry.Rx(90) where MAME's record is a pure Ry.
 * PROPCYCL_ROT8002=1 restores the swap for A/B. */
int g_rot8002 = 0;
/* The 0x8002/0x8001 11-word entry's FLAGS word selects the Euler composition
 * order: bit 1 set -> Z.Y.X (Rz.Ry.Rx), everything else -> Z.X.Y (Rz.Rx.Ry).
 * Pinned per flags value with the CPU's own angle words against MAME's
 * master records (tools/overnight/euler_order_sweep.py, after fixing its
 * 0x8010-marker walk -- see below):
 *   flags=1  the balloon propeller (record 775 = model 706+0x45, emitted by
 *            balloon_render_and_hit_check right after the flags=0 balloon
 *            body): 232/232 CPU entries fit Rx(spin).Ry(face) = ZXY with
 *            z=0, 0/232 fit the old Z.Y.X, and the fitted X angle steps
 *            -5.63 deg/frame = the emitter's `W[0x0C8C] * -0x400` spin,
 *            tracked continuously through the full circle over 51
 *            balloon+propeller record pairs. With Z.Y.X the spin was
 *            applied in the wrong frame and the propeller precessed all
 *            over the balloon instead of turning in place.
 *   flags=1  the gameplay sky sphere (record 871 = model 802+0x45, x=0,
 *            z=90 deg): fits Rz(90).Ry -- ZXY and ZYX coincide when x=0,
 *            so the sky is untouched; Rx.Ry.Rz (XYZ) would tilt it.
 *   flags=2  the attract flyover sky (code 108, x and y non-zero): fits
 *            Ry.Rx = Z.Y.X, 471/471, and REJECTS ZXY -- row 151's
 *            measurement, which is why the default was Z.Y.X.
 *   flags=4  the animated-object placements (e.g. record 845 = model
 *            776+0x45, the animated balloon family, entries with all three
 *            angles non-zero): ZXY 1294/1294, ZYX only 662/1294 -- the
 *            banking tilt was mis-composed too.
 *   flags=0  no multi-axis entry observed; follows the same bit-1 rule.
 * Row 151 concluded "Z.Y.X fits every code" because its CPU-list parser
 * (a) skipped 2 words past the `0x8010, 3, <shift>, -1` marker where the
 * record is 4, and (b) stopped at the first mid-list `0x8010, -1` -- the
 * balloon rig sits right after one -- so no flags=1/4 multi-axis entry
 * ever entered its sample. Both fixed in euler_8002_gate.py; with them
 * the sweep itself reports 775 = {XYZ, XZY, ZXY}, 845 = {ZXY}.
 * ZXY over XZY (775 and 871 cannot separate them -- no x,z-both-nonzero
 * flags=1 entry -- but 845 can and does: XZY 662/1294): the game's own
 * routine is rotate_euler_zxy_optimized, and ZXY won row 101's rider-rig
 * sweep. The 0x8008 rig path is unaffected (g_euler_8008 = XZY, its own
 * measurement).
 * PROPCYCL_NO_8002_FLAGORD=1 restores the old single-order walk for A/B. */
int g_8002_flagord = 1;
int g_8002_dbg = 0;
int g_vdump = 0;
int g_bbox_code = -1, g_bx0, g_bx1, g_by0, g_by1, g_bn;

/* PROPCYCL_ZORD=1 -- per-object z-order census for one frame.
 * Painter's algorithm draws large zsort FIRST and small zsort LAST, so the
 * object with the SMALLEST zsort is the one painted on top. This is the
 * measurement that settles "is the sky covering the flyover?" (register
 * row 23) instead of reasoning about it. */
#define ZORD_MAX 256
static struct { int code, n; int32_t zmin, zmax; int sx0, sx1, sy0, sy1; int ap, os; } g_zord[ZORD_MAX];
static int g_zord_n = 0;
int g_zord_on = 0;

static void zord_note(int code, const geo_quad *q)
{
    int i, k;
    for (i = 0; i < g_zord_n; i++) if (g_zord[i].code == code) break;
    if (i == g_zord_n) {
        if (g_zord_n >= ZORD_MAX) return;
        g_zord_n++;
        g_zord[i].code = code; g_zord[i].n = 0;
        g_zord[i].zmin = INT32_MAX; g_zord[i].zmax = INT32_MIN;
        g_zord[i].sx0 = g_zord[i].sy0 = INT32_MAX;
        g_zord[i].sx1 = g_zord[i].sy1 = INT32_MIN;
    }
    { extern int g_zord_ap, g_zord_os; g_zord[i].ap = g_zord_ap; g_zord[i].os = g_zord_os; }
    g_zord[i].n++;
    if (q->zsort < g_zord[i].zmin) g_zord[i].zmin = q->zsort;
    if (q->zsort > g_zord[i].zmax) g_zord[i].zmax = q->zsort;
    for (k = 0; k < q->nrv; k++) {
        int sx = q->rv[k].sx16 >> 4, sy = q->rv[k].sy16 >> 4;
        if (sx < g_zord[i].sx0) g_zord[i].sx0 = sx;
        if (sx > g_zord[i].sx1) g_zord[i].sx1 = sx;
        if (sy < g_zord[i].sy0) g_zord[i].sy0 = sy;
        if (sy > g_zord[i].sy1) g_zord[i].sy1 = sy;
    }
}

static void zord_report(void)
{
    int i, j;
    if (!g_zord_on || !g_zord_n) return;
    /* sort by zmin ASCENDING: the top of this list is painted LAST, i.e. on top */
    for (i = 0; i < g_zord_n; i++)
        for (j = i + 1; j < g_zord_n; j++)
            if (g_zord[j].zmin < g_zord[i].zmin) {
                typeof(g_zord[0]) t = g_zord[i]; g_zord[i] = g_zord[j]; g_zord[j] = t; }
    printf("[ZORD] painted last (on top) first:\n");
    printf("[ZORD] %-6s %-6s %-4s %-9s %-9s %-9s %s\n",
           "code", "quads", "ap", "depth", "view_ap", "objshift", "screen bbox");
    for (i = 0; i < g_zord_n; i++)
        printf("[ZORD] %-6d %-6d %-4d %-9d %-9d %-9d x[%d..%d] y[%d..%d]\n",
               g_zord[i].code, g_zord[i].n,
               g_zord[i].zmin >> 21, g_zord[i].zmin & 0x1FFFFF,
               g_zord[i].ap, g_zord[i].os,
               g_zord[i].sx0, g_zord[i].sx1, g_zord[i].sy0, g_zord[i].sy1);
    g_zord_n = 0;
}

/* ---- placement + camera dump for tools/attract_diff.py --------------------
 *
 * PROPCYCL_DIST_DUMP=<path>            output file
 * PROPCYCL_DIST_FRAME=<n>              one frame   (legacy)
 * PROPCYCL_DIST_FROM=<a> _TO=<b>       a frame RANGE, inclusive
 *
 * Format, one record per line:
 *   F <frame> <game_frame_ctr W[0x0C98]>
 *   C <m00 m01 m02 m10 m11 m12 m20 m21 m22> <camx camy camz> <hdg pitch roll>
 *   P <cpu_model_id> <tx> <ty> <tz>
 * The C line is the live view matrix in Q15 (before the 2.14 halving), i.e.
 * directly comparable with the viewq in a recording's viewport record. The
 * P translations are camera-relative, like a recording's primitive records.
 */
FILE *g_dist_fp;

/* ---- PROPCYCL_BLINKLOG: name whatever vanishes for a moment -------------
 *
 * "There is a waterfall in the first level, it disappears now and then."
 * A defect like that is position- and input-gated, and a headless run cannot
 * reproduce it (over 1201 steered gameplay frames the terrain chunks show ZERO
 * dropouts and the only real flicker is rider parts 75/76, register row 114's
 * residual). So instead of guessing at which model it is, this watches the
 * drawn set live and prints every object that was on screen, vanished for a
 * few frames, and came back -- with its model id, so it can be rendered and
 * identified.
 *
 *   PROPCYCL_BLINKLOG=1            report to stderr
 *   PROPCYCL_BLINKLOG=<path>       append to a file
 *   PROPCYCL_BLINKGAP=<n>          longest gap still counted as a blink (default 8)
 *   PROPCYCL_BLINKMIN=<n>          frames it must have been present first (default 10)
 *
 * It only reports a model that had been drawn steadily first, so objects
 * legitimately entering and leaving view are not flagged. An ANIMATION CYCLE
 * looks like a blink too -- models that alternate on a fixed period (779/780
 * every 4 frames, 327/332 every 32) are the game's own frame sets, not a bug --
 * so the report carries the gap length and the period between gaps. */
#define BLINK_MAX_MODEL 4096
static FILE *g_blink_fp;
static int   g_blink_on, g_blink_gap = 8, g_blink_min = 10;
static uint32_t g_blink_last[BLINK_MAX_MODEL];   /* last frame drawn */
static uint32_t g_blink_since[BLINK_MAX_MODEL];  /* first frame of the current run */
static uint32_t g_blink_prevgap[BLINK_MAX_MODEL];
static uint32_t g_blink_frame;

void blinklog_init(void)
{
    const char *e = getenv("PROPCYCL_BLINKLOG");
    if (!e || !*e || !strcmp(e, "0")) return;
    g_blink_on = 1;
    if (!strcmp(e, "1")) g_blink_fp = stderr;
    else { g_blink_fp = fopen(e, "w"); if (!g_blink_fp) g_blink_fp = stderr; }
    { const char *g = getenv("PROPCYCL_BLINKGAP"); if (g) g_blink_gap = atoi(g); }
    { const char *m = getenv("PROPCYCL_BLINKMIN"); if (m) g_blink_min = atoi(m); }
    fprintf(g_blink_fp, "# blink log: gap<=%d frames, after >=%d frames present\n",
            g_blink_gap, g_blink_min);
}

void blinklog_note(int code)
{
    if (!g_blink_on || code < 0 || code >= BLINK_MAX_MODEL) return;
    uint32_t f = g_blink_frame;
    uint32_t last = g_blink_last[code];
    if (last && last < f - 1 && (f - last - 1) <= (uint32_t)g_blink_gap) {
        uint32_t run = last - g_blink_since[code] + 1;
        if (run >= (uint32_t)g_blink_min) {
            uint32_t period = g_blink_prevgap[code] ? (f - g_blink_prevgap[code]) : 0;
            fprintf(g_blink_fp,
                    "BLINK f%-7u model %-5d gone %u frame%s  (was present %u"
                    " frames; %u since previous blink)\n",
                    f, code, f - last - 1, (f - last - 1) == 1 ? "" : "s",
                    run, period);
            fflush(g_blink_fp);
        }
        g_blink_prevgap[code] = f;
        g_blink_since[code] = f;
    } else if (!last || last < f - 1) {
        g_blink_since[code] = f;      /* a fresh appearance, not a blink */
    }
    g_blink_last[code] = f;
}

/* WAS IT NOT EMITTED, OR EMITTED AND THEN CULLED?
 *
 * blinklog_note() fires when an object reaches the geometry stage, so an
 * object whose quads are ALL culled or clipped away still counts as present --
 * which is exactly the case that looks like "it disappeared" on screen while
 * the display list is unchanged. Counting the quads that actually survive into
 * the draw buffer separates the two, and that distinction is the whole
 * diagnosis: a missing EMISSION is a game-logic bug, a missing DRAW is a
 * geometry/cull/clip one, and they are fixed in completely different places. */
static int      g_blink_quads[BLINK_MAX_MODEL];
static uint32_t g_blink_hadq[BLINK_MAX_MODEL];   /* last frame with >0 quads */
static int      g_blink_prevq[BLINK_MAX_MODEL];  /* its quad count last frame */

void blinklog_quad(int code)
{
    if (!g_blink_on || code < 0 || code >= BLINK_MAX_MODEL) return;
    g_blink_quads[code]++;
}

/* Per-object reject attribution, so a CULLED/PARTIAL line can name the CAUSE.
 * "behind" = every vertex behind the near plane; "cull" = backface, i.e. the
 * winding says we are looking at its back; "clip" = it survived but crossed
 * the near plane and went through the clip lerp. A big flat sheet that only
 * sometimes draws is a different bug under each of those three. */
static int g_blink_rb[BLINK_MAX_MODEL], g_blink_rc[BLINK_MAX_MODEL],
           g_blink_rk[BLINK_MAX_MODEL];
/* Anchor the log to what the player saw. Without this, correlating "the water
 * went black just now" with a log of thousands of frames means matching a
 * screenshot filename against a frame number by hand. P and F12 both drop a
 * MARK line, so the moment is findable with a grep. */
void blinklog_mark(const char *what, unsigned frame)
{
    if (!g_blink_on || !g_blink_fp) return;
    fprintf(g_blink_fp, "----- MARK f%-7u %s -----\n", frame, what ? what : "");
    fflush(g_blink_fp);
}

void blinklog_reason(int code, int behind, int cull, int clip)
{
    if (!g_blink_on || code < 0 || code >= BLINK_MAX_MODEL) return;
    g_blink_rb[code] += behind; g_blink_rc[code] += cull; g_blink_rk[code] += clip;
}

/* ---- PROPCYCL_WATCH=<lo>-<hi>: watch a GROUP, not a model ---------------
 *
 * An animated object is a CYCLE of models -- the water surface is codes
 * 395-402, one of which is emitted each frame -- so every member of it
 * legitimately blinks, and a per-model BLINK line says nothing. The question
 * that matters is whether the GROUP went silent: did any member get emitted,
 * and did any of them draw a quad. Those are three different states with
 * three different causes, and this reports the transition between them with
 * the player's position, so the answer is a place as well as a frame. */
static int g_watch_lo = -1, g_watch_hi = -1, g_watch_prev = -1;
/* Last frame's group state, kept so the P key can report it. The per-frame
 * quad counts are cleared at the end of every frame, so asking "what is the
 * water doing right now" during event handling would always read zero. */
static int g_watch_last_emit, g_watch_last_quads; static unsigned g_watch_last_f;
/* A full snapshot of the previous frame's drawn objects, so a pause can dump
 * WHAT WAS ON SCREEN. Pausing once with a thing visible and once without, and
 * diffing the two lists, names the missing object outright -- which beats
 * guessing which code the player means by "the water". */
static int      g_snap_quads[BLINK_MAX_MODEL];
static uint8_t  g_snap_emit[BLINK_MAX_MODEL];
static unsigned g_snap_frame;
void watchlog_init(const char *spec)
{
    if (!spec) return;
    int a, b;
    if (sscanf(spec, "%d-%d", &a, &b) == 2)      { g_watch_lo = a; g_watch_hi = b; }
    else if (sscanf(spec, "%d", &a) == 1)        { g_watch_lo = g_watch_hi = a; }
    /* Say so. A watch that silently failed to arm looks exactly like a watch
     * that saw nothing, and the difference is the whole diagnosis. */
    if (g_watch_lo >= 0)
        fprintf(stderr, "[WATCH] armed for codes %d..%d%s\n", g_watch_lo, g_watch_hi,
                getenv("PROPCYCL_BLINKLOG") ? ""
                     : "  -- BUT PROPCYCL_BLINKLOG IS NOT SET, so nothing will be written");
    else
        fprintf(stderr, "[WATCH] PROPCYCL_WATCH=\"%s\" not understood; use lo-hi, e.g. 395-402\n", spec);
}

static void watchlog_frame(unsigned pf)
{
    extern intptr_t _W[];
    if (g_watch_lo < 0 || !g_blink_fp) return;
    int emitted = 0, quads = 0;
    for (int m = g_watch_lo; m <= g_watch_hi && m < BLINK_MAX_MODEL; m++) {
        if (m < 0) continue;
        if (g_blink_last[m] == pf) emitted++;
        quads += g_blink_quads[m];
    }
    g_watch_last_emit = emitted; g_watch_last_quads = quads; g_watch_last_f = pf;
    int state = !emitted ? 0 : (!quads ? 1 : 2);   /* 0 gone, 1 emitted-not-drawn, 2 drawn */
    if (state != g_watch_prev) {
        static const char *name[3] = {
            "GONE      - not one member reached the geometry stage (GAME LOGIC)",
            "NO QUADS  - emitted, every quad culled or clipped (RENDERER)",
            "drawing" };
        fprintf(g_blink_fp,
                "WATCH f%-7u %d..%d -> %s   [%d emitted, %d quads]"
                "  player (%ld,%ld,%ld) cell %ld\n",
                pf, g_watch_lo, g_watch_hi, name[state], emitted, quads,
                (long)(int32_t)_W[0x0D00], (long)(int32_t)_W[0x0D04],
                (long)(int32_t)_W[0x0D08],
                (long)(((int32_t)_W[0x0D00] / 0x18000) +
                       ((int32_t)_W[0x0D08] / 0x18000) * 8));
        fflush(g_blink_fp);
        g_watch_prev = state;
    }
}

/* Called by the P key: state the watched group's condition outright, on the
 * frame the player is looking at. A transition log still needs the reader to
 * work out which side of a transition the pause landed on; this does not. */
void blinklog_dump_codes(void);
void watchlog_report_now(void)
{
    if (g_watch_lo < 0) return;
    const char *verdict =
        !g_watch_last_emit ? "NOT EMITTED -- the game never put it in the display list (GAME LOGIC)"
      : !g_watch_last_quads ? "EMITTED, 0 QUADS DRAWN -- all culled or clipped (RENDERER)"
      : "drawing normally";
    printf("      watch   codes %d..%d on frame %u: %s  [%d emitted, %d quads]\n",
           g_watch_lo, g_watch_hi, g_watch_last_f, verdict,
           g_watch_last_emit, g_watch_last_quads);
    if (g_blink_fp) {
        fprintf(g_blink_fp, "WATCH-AT-PAUSE f%-7u %d..%d: %s  [%d emitted, %d quads]\n",
                g_watch_last_f, g_watch_lo, g_watch_hi, verdict,
                g_watch_last_emit, g_watch_last_quads);
        fflush(g_blink_fp);
    }
}

/* The whole frame, as code:quads. Two of these -- one with the thing on
 * screen, one without -- diff to exactly the object that went missing. */
void blinklog_dump_codes(void)
{
    if (!g_blink_fp || !g_snap_frame) return;
    int n = 0;
    fprintf(g_blink_fp, "CODES f%-7u ", g_snap_frame);
    for (int m = 0; m < BLINK_MAX_MODEL; m++) {
        if (!g_snap_emit[m] && !g_snap_quads[m]) continue;
        fprintf(g_blink_fp, "%d:%d ", m, g_snap_quads[m]);
        n++;
    }
    fprintf(g_blink_fp, " (%d objects)\n", n);
    fflush(g_blink_fp);
}

void blinklog_frame(unsigned f)
{
    if (g_blink_on && g_blink_frame) watchlog_frame(g_blink_frame);
    if (g_blink_on && g_blink_frame) {
        uint32_t pf = g_blink_frame;
        for (int m = 0; m < BLINK_MAX_MODEL; m++) {
            if (g_blink_last[m] != pf) continue;          /* not emitted last frame */
            if (g_blink_quads[m] > 0) {
                /* PARTIAL LOSS. A terrain chunk is one object of many quads,
                 * and the water is a SUBSET of its faces -- so a chunk that
                 * loses half its geometry still reports as present and still
                 * "draws quads". Neither BLINK nor CULLED can see that, which
                 * is exactly the case a user hit: water visibly gone, log
                 * empty. Report a sharp drop in an object's own quad count. */
                int prev = g_blink_prevq[m];
                if (prev >= 8 && g_blink_quads[m] * 2 < prev) {
                    fprintf(g_blink_fp,
                            "PARTIAL f%-7u model %-5d quads %d -> %d"
                            "  (%d%% of its geometry gone)"
                            "  [behind %d cull %d nearclip %d]\n",
                            pf, m, prev, g_blink_quads[m],
                            100 - (100 * g_blink_quads[m]) / prev,
                            g_blink_rb[m], g_blink_rc[m], g_blink_rk[m]);
                    fflush(g_blink_fp);
                }
                g_blink_prevq[m] = g_blink_quads[m];
                g_blink_hadq[m] = pf; continue;
            }
            g_blink_prevq[m] = 0;
            /* emitted, drew nothing -- report the transition only */
            if (g_blink_hadq[m] == pf - 1 && g_blink_hadq[m]) {
                fprintf(g_blink_fp,
                        "CULLED f%-7u model %-5d emitted but 0 quads drawn"
                        "  (was drawing until f%u)  [behind %d cull %d nearclip %d]\n",
                        pf, m, g_blink_hadq[m],
                        g_blink_rb[m], g_blink_rc[m], g_blink_rk[m]);
                fflush(g_blink_fp);
            }
        }
        /* snapshot before the clear -- this is what a pause reports */
        for (int m = 0; m < BLINK_MAX_MODEL; m++) {
            g_snap_quads[m] = g_blink_quads[m];
            g_snap_emit[m]  = (g_blink_last[m] == pf);
        }
        g_snap_frame = pf;
        memset(g_blink_quads, 0, sizeof g_blink_quads);
        memset(g_blink_rb, 0, sizeof g_blink_rb);
        memset(g_blink_rc, 0, sizeof g_blink_rc);
        memset(g_blink_rk, 0, sizeof g_blink_rk);
    }
    g_blink_frame = f + 1;
}

static int g_dist_from = -1, g_dist_to = -1, g_dist_tried;
static void geohw_view_matrix(int32_t m[3][3]);

static void dist_dump_open(void) {
    if (g_dist_tried) return;
    g_dist_tried = 1;
    const char *e = getenv("PROPCYCL_DIST_DUMP");
    if (!e) return;
    const char *f1 = getenv("PROPCYCL_DIST_FROM"), *f2 = getenv("PROPCYCL_DIST_TO");
    const char *fr = getenv("PROPCYCL_DIST_FRAME");
    if (f1 && f2) { g_dist_from = atoi(f1); g_dist_to = atoi(f2); }
    else { g_dist_from = g_dist_to = fr ? atoi(fr) : 420; }
    g_dist_fp = fopen(e, "w");
}
int dist_dump_active(void) {
    dist_dump_open();
    if (!g_dist_fp) return 0;
    int f = (int)g_sys.frame_count;
    if (f > g_dist_to) { fclose(g_dist_fp); g_dist_fp = NULL; return 0; }
    return f >= g_dist_from;
}
/* called once per rendered frame, before any placements */
static void dist_dump_frame_header(void) {
    if (!dist_dump_active()) return;
    extern intptr_t _W[];
    int32_t m[3][3]; geohw_view_matrix(m);
    fprintf(g_dist_fp, "F %d %ld\n", (int)g_sys.frame_count, (long)_W[0x0C98]);
    /* extra fields (appended, so the first 15 keep their meaning):
     *   adc_x adc_y  cam_target_hdg  player_x player_y player_z  player_hdg
     *   speed_term(W[0x12C8])  speed(W[0x0D48])
     * The last two are what camera_update divides by: its smoothing divisor
     * is (hi16(W[0x12C8]) + 0x10), so a run's follow rate is only
     * interpretable next to them (mismatch-register row 18). */
    fprintf(g_dist_fp, "C %d %d %d %d %d %d %d %d %d %ld %ld %ld %ld %ld %ld"
            " %ld %ld %ld %ld %ld %ld %ld %ld %ld\n",
            m[0][0],m[0][1],m[0][2], m[1][0],m[1][1],m[1][2], m[2][0],m[2][1],m[2][2],
            (long)_W[0x0CDC], (long)_W[0x0CE0], (long)_W[0x0CE4],
            (long)(_W[0x0CEC] & 0xFFFF), (long)(_W[0x0CE8] & 0xFFFF), (long)(_W[0x0CF0] & 0xFFFF),
            (long)_W[0x2BC8], (long)_W[0x2BCA], (long)(_W[0x0DAC] & 0xFFFF),
            (long)_W[0x0D00], (long)_W[0x0D04], (long)_W[0x0D08], (long)(_W[0x0D10] & 0xFFFF),
            (long)_W[0x12C8], (long)_W[0x0D48]);
}
static void geohw_quad_cb(const geo_quad *q, void *user);

/* ---- free-look course map ----------------------------------------------
 * Renders a whole course's terrain straight from the point ROM, so any
 * course can be inspected without needing a capture of it. Chunk layout is
 * the documented one: 128 chunks in an 8x16 grid, offset = grid_z*8+grid_x,
 * world position (grid_x, 0, grid_z) * GRID_CELL, model = base + offset.
 *
 * Uses the SAME geometry stage as the game (geo_hw), so what you orbit is
 * the gated-exact geometry, not a separate approximation. */
#define MAP_GRID_CELL 0x18000
/* The grid is 8x16 cells of 0x18000 => 786k x 1572k units. At mant 1920 a
 * half-width of 393k needs z ~2.5M just to reach the screen edge, and the
 * near rank sits half the depth closer than that -- so the whole map only
 * frames at roughly 3x the naive distance. */
#define MAP_BASE_DIST 3000000.0f
static const int map_terrain_base[4] = { 0x495, 0x515, 0x595, 0x615 };

/* Backdrop for the map viewer: a dark purple-to-black vertical gradient,
 * drawn before the geometry. Flat black made it hard to tell "nothing is
 * there" from "the view is pointing at nothing". */
static void geohw_map_backdrop(void)
{
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_ALPHA_TEST);
    glDisable(GL_BLEND);
    glBegin(GL_QUADS);
    glColor4f(0.13f, 0.06f, 0.20f, 1.0f);   /* top: dark purple */
    glVertex2f(g_scene_x0, 0);
    glVertex2f(g_scene_x1, 0);
    glColor4f(0.02f, 0.01f, 0.04f, 1.0f);   /* bottom: near black */
    glVertex2f(g_scene_x1, SCREEN_HEIGHT);
    glVertex2f(g_scene_x0, SCREEN_HEIGHT);
    glEnd();
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
    glEnable(GL_ALPHA_TEST);
}

static void geohw_draw_map(void)
{
    geohw_map_backdrop();

    /* The map viewer is NOT a game frame: it must not inherit the capture's
     * fog/fade. framedump_render() is skipped in map mode, so g_fog freezes
     * at whichever frame was showing when the map opened -- and if that
     * frame was mid screen-fade the fullscreen fade quad paints the whole
     * view white, permanently. It also costs a second blended pass per
     * quad, which is most of why the map felt slow.
     *
     * Gated at the two stages rather than by clearing g_fog_valid, so the
     * capture's fog state survives intact for when the map is closed. */
    float fm[3][3], fc[3], zoom;
    ui_map_camera(fm, fc, &zoom);

    int32_t m[3][3];
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++) {
            float v = fm[r][c] * 32767.0f;
            if (v >  32767.0f) v =  32767.0f;
            if (v < -32767.0f) v = -32767.0f;
            m[r][c] = (int32_t)v;
        }

    /* Orbit about the grid centre, offset by the user's pan. */
    float cx = fc[0] + 4.0f  * MAP_GRID_CELL;
    float cy = fc[1];
    float cz = fc[2] + 8.0f  * MAP_GRID_CELL;
    int32_t dist = (int32_t)(MAP_BASE_DIST * zoom);

    int base = map_terrain_base[ui_map_course() & 3];
    for (int gz = 0; gz < 16; gz++) {
        for (int gx = 0; gx < 8; gx++) {
            int32_t p[3] = { (int32_t)(gx * MAP_GRID_CELL - cx),
                             (int32_t)(0            - cy),
                             (int32_t)(gz * MAP_GRID_CELL - cz) };
            geo_view gv;
            memset(&gv, 0, sizeof gv);
            memcpy(gv.m, m, sizeof m);
            for (int c = 0; c < 3; c++)
                gv.t[c] = (int32_t)(((int64_t)p[0] * m[0][c] +
                                     (int64_t)p[1] * m[1][c] +
                                     (int64_t)p[2] * m[2][c]) >> 15);
            gv.t[2] += dist;              /* push the scene in front of the eye */
            /* Centre it vertically. Pitching down rotates the ground plane
             * up the screen; screen_y = cy - (Y*mant)/Z, so lowering Y by
             * (offset_px * Z / mant) brings it back to the middle. */
            gv.t[1] -= (int32_t)((float)dist * 0.10f);
            gv.zoom_mant = 1920; gv.zoom_shift = 0;
            gv.vx = 0; gv.vy = 0;
            gv.absolute_priority = 0; gv.objectshift = 0;
            /* Flat, fully-lit shading: with no light vector the normals
             * dot to zero and every lit quad would come out black. 64 is
             * the neutral brightness (out = c * bri / 64). */
            gv.ambient = 64; gv.power = 0;
            gv.light[0] = gv.light[1] = gv.light[2] = 0;
            memcpy(gv.viewq, m, sizeof m);
            geo_hw_set_view(&gv);
            geo_hw_object(base + gz * 8 + gx, geohw_quad_cb, NULL);
        }
    }
}

/* ---- C374 sprite layer -------------------------------------------------
 * Drawn AFTER the screen-fade quad, deliberately: sprite_render already
 * applies the fade per pixel inside its own chain (poly3d_drawsprite), so
 * putting it before the quad would fade the layer twice. Gamma is applied
 * once over the whole composed frame at read-back, as the hardware does at
 * scanout, so it is not applied here. */
static GLuint spr_tex;
static uint8_t *spr_rgba, *spr_prio;
static int spr_px;

/* Text tilemap, drawn last: it is the topmost layer (HUD text, credits).
 * Rendered in compose mode -- transparent where it draws nothing, no gamma
 * (the composed frame is gamma-corrected once at read-back), and its alpha
 * left for GL to blend against everything beneath. */
static GLuint txt_tex;
static uint8_t *txt_rgba;
static int txt_px;

static void geohw_draw_text(void)
{
    const text_state *ts = framedump_current_text();
    if (!ts && g_live_ok && g_live_txt.valid) ts = &g_live_txt;
    txt_px = 0;
    if (ui_map_active()) return;      /* map view is geometry only */
    { static int off = -1;
      if (off < 0) { const char *e = getenv("PROPCYCL_NO_TEXT"); off = (e && *e != '0'); }
      if (off) return; }
    if (!ts) return;
    if (!txt_rgba) { txt_rgba = malloc((size_t)SPR_W * SPR_H * 4); if (!txt_rgba) return; }

    /* RE-RENDER ONLY WHEN THE INPUTS CHANGE.
     *
     * text_render paints the whole 640x480 layer and was being called every
     * frame regardless: measured 2.163s of a 6.71s render over 2000 frames,
     * i.e. the text layer cost nearly as much as all the 3D geometry. Its
     * output is a pure function of (cgram incl. the tilemap tail, palette,
     * tilemapattr, and the fog fields the model applies), so hash those and
     * reuse the uploaded texture when they are unchanged.
     *
     * The alpha-count pass that used to follow was 307200 iterations every
     * frame (0.395s) to produce a number only used for an early-out and the
     * verbose census; text_render now reports it via the cached value. */
    {
        static uint64_t last_hash;  static int have_last;
        static long     last_px;
        uint64_t h = 1469598103934665603ULL;
        #define MIX(p, n) do { const uint8_t *_b=(const uint8_t*)(p);             for (size_t _i=0; _i<(size_t)(n); _i++) { h ^= _b[_i]; h *= 1099511628211ULL; } } while (0)
        if (ts->cgram) MIX(ts->cgram, 0x20000);
        if (ts->pal)   MIX(ts->pal,   0x18000);
        MIX(ts->attr, sizeof ts->attr);
        MIX(g_fog.poly_fade, sizeof g_fog.poly_fade);
        MIX(g_fog.gamma, sizeof g_fog.gamma);
        MIX(&g_fog.mixer_flags, sizeof g_fog.mixer_flags);
        /* THE SCREEN FADE ONLY REACHES THE OUTPUT WHEN IT IS ENABLED, and
         * hashing it unconditionally cost a full re-render every frame the
         * moment the fade started working.
         *
         * text_hw.c:91 gates it exactly this way --
         *     fade_en = (mixer_flags & 2) && screen_fade_factor
         * -- so with the factor at 0 (all of normal gameplay) these fields
         * cannot change a single pixel, and mixing them only served to
         * invalidate the cache. Before register row 152 the factor was stuck
         * at 0 forever, so this was free; making the fade work turned it into
         * a 640x480 re-render plus the 307200-iteration alpha count on every
         * frame of every fade. Measured on the same 12000-frame arm, text
         * render went 0.795s -> 5.107s while the deterministic render
         * counters went DOWN (texels 3559M -> 3430M), which is what said the
         * cost was the cache and not the scene.
         *
         * fade_en itself is mixed so the frame it turns on or off still
         * invalidates; while it is on the layer re-renders every frame, which
         * is correct and is what a fade is. */
        { extern int g_texthash_fade;   /* =1 reverts to hashing the fade always */
          int fade_en = g_texthash_fade
                      || (((g_fog.mixer_flags & 2) != 0) && g_fog.screen_fade_factor);
          MIX(&fade_en, sizeof fade_en);
          if (fade_en) {
              MIX(g_fog.screen_fade, sizeof g_fog.screen_fade);
              MIX(&g_fog.screen_fade_factor, sizeof g_fog.screen_fade_factor);
          } }
        #undef MIX
        g_txt_calls++;
        if (have_last && h == last_hash) {
            txt_px = last_px;                 /* unchanged: reuse txt_tex */
            if (!txt_px) return;
        } else {
            g_txt_miss++;
            { double _t=rperf(); text_render(ts, &g_fog, txt_rgba, 0); g_perf_txr += rperf()-_t; }
            { double _t=rperf();
              for (long i = 0; i < (long)SPR_W * SPR_H; i++) if (txt_rgba[i*4+3]) txt_px++;
              g_perf_txc += rperf()-_t; }
            last_hash = h; have_last = 1; last_px = txt_px;
            g_txt_dirty = 1;
            if (!txt_px) return;
        }
    }

    if (!txt_tex) {
        glGenTextures(1, &txt_tex);
        glBindTexture(GL_TEXTURE_2D, txt_tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, SPR_W, SPR_H, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    } else {
        glBindTexture(GL_TEXTURE_2D, txt_tex);
    }
    { double _t=rperf();
      glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, SPR_W, SPR_H,
                    GL_RGBA, GL_UNSIGNED_BYTE, txt_rgba);
      g_perf_txu += rperf()-_t; }
    glEnable(GL_TEXTURE_2D);
    glDisable(GL_ALPHA_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
    glBegin(GL_QUADS);
    glTexCoord2f(0,0); glVertex2f(0, 0);
    glTexCoord2f(1,0); glVertex2f(SCREEN_WIDTH, 0);
    glTexCoord2f(1,1); glVertex2f(SCREEN_WIDTH, SCREEN_HEIGHT);
    glTexCoord2f(0,1); glVertex2f(0, SCREEN_HEIGHT);
    glEnd();
    glDisable(GL_BLEND);
    glEnable(GL_ALPHA_TEST);
    glDisable(GL_TEXTURE_2D);
}

/* When not replaying a capture, the 2D state comes from live emulated
 * memory instead of dump files -- same renderers, different source. This is
 * what lets the actual game boot with sprites and text on screen. */
/* One sprite, drawn in its turn in the merged z order.
 *
 * A single 640x480 texture is allocated once and each sprite's bounding box
 * is sub-imaged into its top-left corner, so there is no per-sprite texture
 * allocation. Sprites are few (tens per frame), unlike quads. */
static GLuint spr_item_tex;
static uint8_t *spr_item_buf;

static void geohw_draw_sprite_item(const sprite_state *st, const sprite_item *it)
{
    if (!spr_item_buf) {
        spr_item_buf = malloc((size_t)SPR_W * SPR_H * 4);
        if (!spr_item_buf) return;
    }
    sprite_render_item(st, &g_fog, it, spr_item_buf, 1);
    spr_px += it->w * it->h;

    if (!spr_item_tex) {
        glGenTextures(1, &spr_item_tex);
        glBindTexture(GL_TEXTURE_2D, spr_item_tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, SPR_W, SPR_H, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    } else {
        glBindTexture(GL_TEXTURE_2D, spr_item_tex);
    }
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, it->w, it->h,
                    GL_RGBA, GL_UNSIGNED_BYTE, spr_item_buf);

    float su = (float)it->w / SPR_W, sv = (float)it->h / SPR_H;
    /* The gauge HOUSINGS are sprites, not polygons (the polygons are only
     * the rolling reels): TIME's at x 0 w 209 and its knob at 144, POINT's at
     * 432 w 208 and its knob at 370, the pendulum at 269 -- all layer 5 (key
     * 0xA00000..0xBFFFFF, the layer plus a small z_depth) and starting in the
     * top band. They follow their side to the widescreen corner; the
     * pendulum, centred, stays (hud_corners_shift's 60 px margin). */
    float dx = ((it->z >> 21) == 5u && it->y0 < 64)
             ? (float)hud_corners_shift(it->x0 + it->w / 2.0 - SCREEN_WIDTH / 2.0) : 0.0f;
    glEnable(GL_TEXTURE_2D);
    glDisable(GL_ALPHA_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
    glBegin(GL_QUADS);
    glTexCoord2f(0,  0);  glVertex2f(it->x0 + dx,          it->y0);
    glTexCoord2f(su, 0);  glVertex2f(it->x0 + it->w + dx,  it->y0);
    glTexCoord2f(su, sv); glVertex2f(it->x0 + it->w + dx,  it->y0 + it->h);
    glTexCoord2f(0,  sv); glVertex2f(it->x0 + dx,          it->y0 + it->h);
    glEnd();
    glDisable(GL_BLEND);
    glEnable(GL_ALPHA_TEST);
    glDisable(GL_TEXTURE_2D);
}


/* Background colour for the framedump path. The reference fills the frame
 * with the mixer's bg (nthbyte 0x08..0x0a) before drawing, and our
 * videomix[] is not populated from a capture, so without this the clear
 * colour is unrelated to the frame being replayed. */
int renderer3d_framedump_bg(uint8_t *r, uint8_t *g, uint8_t *b)
{
    if (!g_fog_valid) return 0;
    *r = g_fog.bg[0]; *g = g_fog.bg[1]; *b = g_fog.bg[2];
    return 1;
}

static int geohw_enabled(void)
{
    /* ON by default: the ported hardware geometry stage is the path of
     * record (launch.sh sets it for every real use, the loop gate runs it,
     * and it is the only path measured against MAME). The superseded GL
     * projection it replaced renders course 1 (Wind Woods) as untextured
     * flat-colour geometry with a screen-filling red quad and black sky --
     * PROPCYCL_GEO_HW=0 keeps it available for A/B only. */
    static int v = -1;
    if (v < 0) { const char *e = getenv("PROPCYCL_GEO_HW"); v = e ? (*e != '0') : 1; }
    return v;
}

int geohw_quads_drawn;

/* Per-vertex shade (pc_raster_model.py shade()). On by default -- it is a
 * missing pipeline stage, not an effect. PROPCYCL_GEO_NOSHADE=1 restores
 * the old flat full-brightness draw for A/B comparison. */
static int geohw_shade_enabled(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("PROPCYCL_GEO_NOSHADE"); v = !(e && *e != '0'); }
    return v;
}

/* CZ depth fog + poly/screen fade. PROPCYCL_GEO_NOFOG=1 disables the whole
 * group for A/B against the old texel->pen->palette-only output. */
static int geohw_fog_enabled(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("PROPCYCL_GEO_NOFOG"); v = !(e && *e != '0'); }
    return v;
}

/* poly_fade is a per-channel scale, applied by MAME AFTER fog. We cannot
 * post-multiply a GL fragment, so we scale BOTH the textured pass and the
 * fog colour by it. Because fade is linear and fog is a lerp,
 *     fade(a)*f + fade(b)*(1-f) == fade(a*f + b*(1-f))
 * so scaling both inputs is exactly equivalent to scaling the output. */
static void geohw_fade_rgb(float *r, float *g, float *b)
{
    if (geohw_fog_enabled() && g_fog_valid && !ui_map_active() &&
        g_fog.poly_fade_enabled) {
        *r *= g_fog.poly_fade[0] / 256.0f;
        *g *= g_fog.poly_fade[1] / 256.0f;
        *b *= g_fog.poly_fade[2] / 256.0f;
    }
}

/* Painter's algorithm. The hardware sorts by zsort and draws far to near;
 * drawing in emission order let the sky dome paint over the clouds, which
 * is why they vanished the moment z-clipping let the dome draw in full. */
#define GEOHW_MAX_QUADS 4096
static geo_quad geohw_buf[GEOHW_MAX_QUADS];
static int geohw_nbuf;

int g_tie_emit = 0;

static void geohw_draw_one(const geo_quad *q);

int g_guardband_off = 0;   /* PROPCYCL_GUARDBAND_LEGACY=1 (main.c) */
static int getenv_guardband_off(void) { return g_guardband_off; }
static void geohw_quad_cb(const geo_quad *q, void *user)
{
    /* PROPCYCL_BBOX=<object code>: screen-space bbox of that object, so the
     * live path and a --framedump of the recording can be compared for
     * apparent SIZE (same object, same |t|, same focal => same pixels). */
    { extern int g_bbox_code; extern int g_bbox_cur;
      if (g_bbox_code >= 0 && g_bbox_cur == g_bbox_code) {
          extern int g_bx0,g_bx1,g_by0,g_by1,g_bn; int k;
          for (k = 0; k < 4; k++) {
              int sx = q->v[k].sx16 >> 4, sy = q->v[k].sy16 >> 4;
              if (!q->v[k].valid) continue;
              if (g_bn == 0 || sx < g_bx0) g_bx0 = sx;
              if (g_bn == 0 || sx > g_bx1) g_bx1 = sx;
              if (g_bn == 0 || sy < g_by0) g_by0 = sy;
              if (g_bn == 0 || sy > g_by1) g_by1 = sy;
              g_bn++;
          }
      } }
    (void)user;
    if (q->nrv < 3) return;
    /* The guard-band-clipped draw polygon (geo_hw.c, register row 187): drawn
     * in place of rv when the clipped polygon would saturate. */
    if (q->ndv != 0 && !getenv_guardband_off()) {
        static geo_quad gq;
        if (q->ndv < 0) return;
        gq = *q;
        memcpy(gq.rv, q->dv, sizeof(geo_vert) * (size_t)q->ndv);
        gq.nrv = q->ndv;
        q = &gq;
    }
    /* PROPCYCL_ONLY_CODE=<n> draws ONLY that object code. "Is that red roof
     * really code 874, or a terrain face that happens to be red?" is not a
     * question to answer by looking; this answers it. */
    { extern int g_only_code, g_bbox_cur;
      if (g_only_code >= 0 && g_bbox_cur != g_only_code) return; }
    /* Rig viewer: drop everything that is not part of the bike + rider. */
    { extern int g_rig_view, g_rig_only, g_bbox_cur; extern int rigview_is_rig(int);
      if (g_rig_view && g_rig_only && !rigview_is_rig(g_bbox_cur)) return; }
    /* PROPCYCL_VDUMP=<n>: raw projected vertices of the ONLY_CODE object for
     * the first n quads of a frame. The bbox alone cannot tell "off screen"
     * from "near-plane clip blew the numbers up". */
    { extern int g_vdump, g_only_code, g_bbox_cur; static int seen; static unsigned lastf;
      if (g_vdump > 0 && g_only_code >= 0 && g_bbox_cur == g_only_code) {
          if (lastf != g_sys.frame_count) { lastf = g_sys.frame_count; seen = 0; }
          if (seen < g_vdump) { int k; seen++;
              printf("[VD] f%u q%d nv=%d nrv=%d:", g_sys.frame_count, seen, 4, q->nrv);
              for (k = 0; k < 4; k++)
                  printf("  (%d,%d z=%d v=%d)", q->v[k].sx16 >> 4, q->v[k].sy16 >> 4,
                         q->v[k].z, q->v[k].valid);
              printf("\n"); } } }
    { extern int g_zord_on, g_bbox_cur;
      if (g_zord_on) zord_note(g_bbox_cur, q); }
    if (geohw_nbuf < GEOHW_MAX_QUADS) {
        extern int g_bbox_cur;
        geohw_buf[geohw_nbuf] = *q;
        geohw_buf[geohw_nbuf].order = geohw_nbuf;   /* for the stable sort */
        geohw_buf[geohw_nbuf].pick_code = g_bbox_cur;
        geohw_nbuf++;
        { extern void blinklog_quad(int); extern int g_bbox_cur;
          blinklog_quad(g_bbox_cur); }
    }
}

/* ---------------------------------------------------------------------------
 * RIG VIEWER HOOKS  (src/rig_viewer.c)
 *
 * A standalone viewer for the bike + rider, so the articulated model can be
 * judged by eye without the rest of the scene in the way. It drives the
 * ENGINE's own pipeline -- the node chain loaded from the ROM tables,
 * scene_node_render's 0x8008/0x8009/0x800a wire format, the slot-table
 * composition and geo_hw's projection -- and only filters and magnifies the
 * result. Nothing here reimplements the rig, which is the whole point: a bug
 * in the engine has to show up in the viewer.
 *
 * EVERY hook below is dead while g_rig_view == 0, and only src/rig_viewer.c
 * ever sets it. propcycl's own output is unaffected, bit for bit.
 * ------------------------------------------------------------------------- */
int   g_rig_view = 0;          /* 1 = the rig viewer is driving */
int   g_rig_only = 1;          /* keep only codes in [g_rig_lo, g_rig_hi] */
int   g_rig_lo   = 110;        /* flyover variant set 113-136,163 ... */
int   g_rig_hi   = 190;        /* ... and the gameplay set 138-161,186 */
int   g_rig_fit  = 1;          /* auto-frame the rig in the window */
float g_rig_zoom = 1.0f;       /* user magnification on top of the fit */
float g_rig_panx = 0.0f, g_rig_pany = 0.0f;   /* pan, in screen pixels */
int   g_rig_nquads = 0, g_rig_ncodes = 0;     /* reported back to the viewer */
int   g_rig_show_bike = 1, g_rig_show_rider = 1;
float g_rig_yaw = 0.0f, g_rig_pitch = 0.0f;   /* orbit, degrees, about the rig root */
static int32_t g_rig_pivot[3]; static unsigned g_rig_pivot_frame = ~0u;

/* The rig's object codes. Two variant sets exist: the flyover draws the
 * -0x19 set (113-136 + 163) and the demo / gameplay the normal one
 * (138-161 + 186). Anything else in 110-190 is scenery -- codes 180-185
 * sat a course-length away in the demo and shrank the auto-fit to a speck. */
int rigview_is_rig(int c)
{
    int bike  = (c >= 113 && c <= 118) || (c >= 138 && c <= 145);
    int rider = (c >= 119 && c <= 136) || c == 163 || (c >= 146 && c <= 161) || c == 186;
    return (bike && g_rig_show_bike) || (rider && g_rig_show_rider);
}

/* Orbit: a rigid rotation of the whole rig about its root, in VIEW space,
 * applied after the engine has composed everything. Row-vector convention
 * throughout this file (v . R_obj . R_view), so m' = m . R and
 * t' = (t - pivot) . R + pivot. The pivot is the first rig object of the
 * frame, which is always the root. */
static void q15_mul3(const int32_t a[3][3], const int32_t b[3][3], int32_t o[3][3]);
static void rigview_orbit(geo_view *gv, int code)
{
    extern int g_rig_view;
    if (!g_rig_view || !rigview_is_rig(code)) return;
    if (g_rig_pivot_frame != g_sys.frame_count) {
        g_rig_pivot_frame = g_sys.frame_count;
        g_rig_pivot[0] = gv->t[0]; g_rig_pivot[1] = gv->t[1]; g_rig_pivot[2] = gv->t[2];
    }
    if (g_rig_yaw == 0.0f && g_rig_pitch == 0.0f) return;
    double ya = g_rig_yaw * M_PI / 180.0, pa = g_rig_pitch * M_PI / 180.0;
    int32_t cy = (int32_t)(cos(ya) * 32767), sy = (int32_t)(sin(ya) * 32767);
    int32_t cp = (int32_t)(cos(pa) * 32767), sp = (int32_t)(sin(pa) * 32767);
    const int32_t Ry[3][3] = {{cy,0,-sy},{0,0x7FFF,0},{sy,0,cy}};
    const int32_t Rx[3][3] = {{0x7FFF,0,0},{0,cp,sp},{0,-sp,cp}};
    int32_t R[3][3], nm[3][3];
    q15_mul3(Ry, Rx, R);
    q15_mul3(gv->m, R, nm);     memcpy(gv->m, nm, sizeof nm);
    q15_mul3(gv->viewq, R, nm); memcpy(gv->viewq, nm, sizeof nm);
    int64_t v[3] = { gv->t[0] - g_rig_pivot[0], gv->t[1] - g_rig_pivot[1], gv->t[2] - g_rig_pivot[2] };
    for (int j = 0; j < 3; j++) {
        int64_t a = 0;
        for (int k = 0; k < 3; k++) a += v[k] * R[k][j];
        gv->t[j] = g_rig_pivot[j] + (int32_t)(a >> 15);
    }
}

/* ---- PAUSE CAMERA ---------------------------------------------------------
 * While the game is paused (P, or Start on a pad in gameplay) the player can
 * orbit and zoom around the rider; unpausing snaps back to the game's own view
 * (main.c resets these). The same rigid view-space rotation as rigview_orbit,
 * but applied to the WHOLE WORLD viewport so the scene turns with the rider:
 *   t' = (t - pivot) . R + pivot + (0, 0, dist),  m' = m . R
 * The pivot is the rider's root, i.e. the first rig object of the previous
 * render pass -- identical while paused, since the display list is frozen.
 * The SKY is attached to the eye, not the scene, so it is rotated about the
 * eye (t' = t . R) and not dollied. HUD / screen-space viewports are left
 * alone. Nothing here runs unless g_pausecam_on, so unpaused output is
 * unchanged bit for bit. The game only emits what its own camera can see, so
 * turning far enough shows the edge of the loaded terrain -- that is the
 * game's culling, not a bug in this view. */
int   g_pausecam_on = 0;
float g_pausecam_yaw = 0.0f, g_pausecam_pitch = 0.0f;   /* degrees */
float g_pausecam_dist = 0.0f;                           /* view units, + = further away */
static int32_t pc_pivot[3] = { 0, 1024, 9400 };         /* the game's camera->bike offset */
static int32_t pc_pivot_next[3];
static int     pc_have_next;

/* The orbit, as a Q15 row-vector matrix (v' = v . R) in VIEW space.
 *
 * YAW turns about the WORLD'S VERTICAL through the rider, and PITCH tilts
 * about the LEVEL axis across the screen -- not about the game camera's own
 * axes. The game camera banks with the bike (W[0x0CF0], register row 5) and
 * looks down a little, so its up axis is tilted: yawing about it (as this
 * did first) swung the world around a slanted axis, and a bank at the moment
 * of pausing turned into a nose-up/nose-down tilt a quarter-turn later. The
 * game's own bank and pitch are kept at yaw = pitch = 0, i.e. pausing still
 * shows exactly the game's frame.
 *
 * World up in view space is row 1 of the view matrix (view = world . viewq).
 * Built in column form (x' = A x) and transposed at the end. */
static void pausecam_matrix(const geo_view *gv, int32_t R[3][3])
{
    double u[3] = { gv->viewq[1][0], gv->viewq[1][1], gv->viewq[1][2] };
    double n = sqrt(u[0]*u[0] + u[1]*u[1] + u[2]*u[2]);
    if (n < 1.0) { u[0] = 0; u[1] = 1; u[2] = 0; n = 1; }
    for (int i = 0; i < 3; i++) u[i] /= n;
    /* h = up x forward(0,0,1): horizontal, across the screen; +X when level */
    double h[3] = { u[1], -u[0], 0.0 };
    n = sqrt(h[0]*h[0] + h[1]*h[1]);
    if (n < 1e-3) { h[0] = 1; h[1] = 0; } else { h[0] /= n; h[1] /= n; }
    const double ya = g_pausecam_yaw * M_PI / 180.0, pa = g_pausecam_pitch * M_PI / 180.0;
    double Y[3][3], P[3][3], A[3][3];
    /* Rodrigues: rotation by angle a about unit axis k */
    #define ROT(M, k, a) do { const double c_ = cos(a), s_ = sin(a), t_ = 1 - c_;            \
        M[0][0] = t_*k[0]*k[0] + c_;      M[0][1] = t_*k[0]*k[1] - s_*k[2]; M[0][2] = t_*k[0]*k[2] + s_*k[1]; \
        M[1][0] = t_*k[0]*k[1] + s_*k[2]; M[1][1] = t_*k[1]*k[1] + c_;      M[1][2] = t_*k[1]*k[2] - s_*k[0]; \
        M[2][0] = t_*k[0]*k[2] - s_*k[1]; M[2][1] = t_*k[1]*k[2] + s_*k[0]; M[2][2] = t_*k[2]*k[2] + c_; } while (0)
    ROT(Y, u, ya);
    ROT(P, h, pa);
    #undef ROT
    for (int i = 0; i < 3; i++)             /* A = P . Y: yaw first, then pitch */
        for (int j = 0; j < 3; j++)
            A[i][j] = P[i][0]*Y[0][j] + P[i][1]*Y[1][j] + P[i][2]*Y[2][j];
    for (int i = 0; i < 3; i++)             /* row-vector form is the transpose */
        for (int j = 0; j < 3; j++)
            R[i][j] = (int32_t)lround(A[j][i] * 32767.0);
}

static int hud_screen_space(int code);
static void pausecam_apply(geo_view *gv, int code)
{
    if (cur_viewport != 0 || hud_screen_space(code - 0x45)) return;
    /* Pivot on the BIKE ROOT (138 in gameplay, 113 in the flyover set) when it
     * is drawn; otherwise on the first rig part. Not simply "the first rig
     * part": the range also holds the hidden wing pair 144/145, collapsed to a
     * point ~1000 px off screen (register row 95). */
    if (rigview_is_rig(code)) {
        const int root = (code == 138 || code == 113);
        if (pc_have_next < 2 && (root || !pc_have_next)) {
            pc_have_next = root ? 2 : 1;
            memcpy(pc_pivot_next, gv->t, sizeof pc_pivot_next);
        }
    }
    if (!g_pausecam_on) return;
    int32_t R[3][3], nm[3][3];
    pausecam_matrix(gv, R);
    q15_mul3(gv->m, R, nm);     memcpy(gv->m, nm, sizeof nm);
    q15_mul3(gv->viewq, R, nm); memcpy(gv->viewq, nm, sizeof nm);
    const int sky = (code == 108);
    int64_t v[3];
    for (int j = 0; j < 3; j++) v[j] = gv->t[j] - (sky ? 0 : pc_pivot[j]);
    for (int j = 0; j < 3; j++) {
        int64_t a = 0;
        for (int k = 0; k < 3; k++) a += v[k] * R[k][j];
        gv->t[j] = (int32_t)(a >> 15) + (sky ? 0 : pc_pivot[j]);
    }
    /* Zooming in stops short of the rider whatever the camera distance is. */
    if (!sky) {
        float d = g_pausecam_dist, lim = -(float)(pc_pivot[2] - 2000);
        if (pc_pivot[2] > 2000 && d < lim) d = lim;
        gv->t[2] += (int32_t)d;
    }
}

static int cmp_i32(const void *a, const void *b)
{
    int32_t x = *(const int32_t *)a, y = *(const int32_t *)b;
    return (x > y) - (x < y);
}

/* Frame the collected quads and apply the user's zoom/pan, in SCREEN space,
 * after geo_hw has finished. The engine's geometry is untouched; this only
 * decides how much of the window the result occupies. */
static void rigview_transform(void)
{
    int i, k;

    /* PERCENTILE framing, not a plain bounding box. The rig legitimately
     * contains parts the game has HIDDEN by collapsing them to a single point
     * far off screen -- in the attract demo codes 144/145 are 223 quads all
     * sitting at y=4644 -- and near-plane clipping can push a vertex out to
     * +-2^27. Either one makes a min/max box enormous and shrinks the model to
     * a speck (which is exactly what the first version of this did). Taking
     * the 2nd..98th percentile of the vertex coordinates ignores both. */
    static int32_t xs[GEOHW_MAX_QUADS * 6], ys[GEOHW_MAX_QUADS * 6];
    int n = 0;
    for (i = 0; i < geohw_nbuf; i++) {
        const geo_quad *q = &geohw_buf[i];
        /* Skip HIDDEN parts. The game collapses a part it does not want drawn
         * to a single point via a zero scale in the 13-word 0x8008 record
         * (register row 89) -- in the attract demo codes 144/145 are 223 quads
         * of the 481, all at y=4637, i.e. 46% of every vertex. They carry no
         * picture, but they dragged the framing box 4400 pixels tall and shrank
         * the rig to a 2-pixel speck. A percentile does not save you from a
         * cluster that large; the degeneracy test does. */
        int32_t qx0 = q->rv[0].sx16, qx1 = qx0, qy0 = q->rv[0].sy16, qy1 = qy0;
        for (k = 1; k < q->nrv; k++) {
            if (q->rv[k].sx16 < qx0) qx0 = q->rv[k].sx16;
            if (q->rv[k].sx16 > qx1) qx1 = q->rv[k].sx16;
            if (q->rv[k].sy16 < qy0) qy0 = q->rv[k].sy16;
            if (q->rv[k].sy16 > qy1) qy1 = q->rv[k].sy16;
        }
        if (qx1 - qx0 < 16 && qy1 - qy0 < 16) continue;    /* under one pixel */
        /* Ignore geometry the game itself puts far OUTSIDE the window. The rig
         * is camera-attached and always near the centre; at demo f900 the
         * (hidden) wing pair 144/145 draws 1000 px below the screen and would
         * otherwise stretch the fit until the rider was a speck. The bound is
         * generous (+-1.5 screens) so an orbit never clips a real part. */
        { int32_t cx = (qx0 + qx1) / 2, cy = (qy0 + qy1) / 2;
          if (cx < -SCREEN_WIDTH * 16 || cx > SCREEN_WIDTH * 32 ||
              cy < -SCREEN_HEIGHT * 16 || cy > SCREEN_HEIGHT * 32) continue; }
        for (k = 0; k < q->nrv && n < (int)(sizeof xs / sizeof xs[0]); k++) {
            /* A near-plane clip can throw a vertex to +-2^27; ignore those too. */
            if (q->rv[k].sx16 < -(1 << 22) || q->rv[k].sx16 > (1 << 22)) continue;
            if (q->rv[k].sy16 < -(1 << 22) || q->rv[k].sy16 > (1 << 22)) continue;
            xs[n] = q->rv[k].sx16;
            ys[n] = q->rv[k].sy16;
            n++;
        }
    }
    g_rig_nquads = geohw_nbuf;
    if (!n) return;
    qsort(xs, n, sizeof xs[0], cmp_i32);
    qsort(ys, n, sizeof ys[0], cmp_i32);
    int lo = (n * 2) / 100, hi = n - 1 - (n * 2) / 100;
    if (hi <= lo) { lo = 0; hi = n - 1; }
    int32_t x0 = xs[lo], x1 = xs[hi], y0 = ys[lo], y1 = ys[hi];

    /* 1/16-pixel units throughout. Fit the longer axis to 70% of the window
     * so a limb swinging out of the bounding box does not leave the screen. */
    float cx = (float)(x0 + x1) * 0.5f, cy = (float)(y0 + y1) * 0.5f;
    float w  = (float)(x1 - x0),        h  = (float)(y1 - y0);
    float fit = 1.0f;
    if (g_rig_fit) {
        float fw = (w > 1.0f) ? (SCREEN_WIDTH  * 16.0f * 0.70f) / w : 1.0f;
        float fh = (h > 1.0f) ? (SCREEN_HEIGHT * 16.0f * 0.70f) / h : 1.0f;
        fit = (fw < fh) ? fw : fh;
    }
    float sc = fit * g_rig_zoom;
    float ax = SCREEN_WIDTH  * 8.0f + g_rig_panx * 16.0f;   /* window centre */
    float ay = SCREEN_HEIGHT * 8.0f + g_rig_pany * 16.0f;

    if (propcycl_verbose())
        printf("  [RIGFIT] quads=%d verts=%d pct-box x[%d..%d] y[%d..%d] (px x[%d..%d] y[%d..%d])"
               " fit=%.3f sc=%.3f\n",
               geohw_nbuf, n, x0, x1, y0, y1, x0/16, x1/16, y0/16, y1/16, fit, sc);

    for (i = 0; i < geohw_nbuf; i++) {
        geo_quad *q = &geohw_buf[i];
        for (k = 0; k < 4; k++) {
            q->v[k].sx16 = (int32_t)(ax + ((float)q->v[k].sx16 - cx) * sc);
            q->v[k].sy16 = (int32_t)(ay + ((float)q->v[k].sy16 - cy) * sc);
        }
        for (k = 0; k < q->nrv; k++) {
            q->rv[k].sx16 = (int32_t)(ax + ((float)q->rv[k].sx16 - cx) * sc);
            q->rv[k].sy16 = (int32_t)(ay + ((float)q->rv[k].sy16 - cy) * sc);
        }
        /* The scene clip window came from the viewport record and would now
         * cut the magnified rig; the viewer wants the whole model. */
        q->clip[0] = 0; q->clip[1] = SCREEN_WIDTH;
        q->clip[2] = 0; q->clip[3] = SCREEN_HEIGHT;
    }
}


/* ---- drawn-object list, for the menu's picker -------------------------- */
/* ---- billboard picker: on-screen boxes recovered this render call ------ */
typedef struct { int tile, x0, y0, w, h; } billboard_box;
#define BILLBOARD_BOX_MAX 64
static billboard_box g_billboard_box[BILLBOARD_BOX_MAX];
static int           g_billboard_box_n;
static int           g_billboard_pick = -1;   /* index into g_billboard_box, or -1 */

int  render_billboard_list_count(void) { return g_billboard_box_n; }
void render_billboard_list_get(int i, int *tile, int *x0, int *y0, int *w, int *h) {
    if (i < 0 || i >= g_billboard_box_n) {
        if (tile) *tile=-1;
        if (x0) *x0=0; if (y0) *y0=0; if (w) *w=0; if (h) *h=0;
        return;
    }
    billboard_box *b = &g_billboard_box[i];
    if (tile) *tile=b->tile;
    if (x0) *x0=b->x0; if (y0) *y0=b->y0; if (w) *w=b->w; if (h) *h=b->h;
}
void render_billboard_pick_set(int idx) { g_billboard_pick = idx; }

/* ---- banner picker: text tilemap blocks, screen rect computed live ----- */
static int g_banner_pick = -1;   /* index into this frame's banner_calls list, or -1 */
void render_banner_pick_set(int idx) { g_banner_pick = idx; }

typedef struct { int code, quads; } objlist_ent;
#define OBJLIST_MAX 512
static objlist_ent g_objlist[OBJLIST_MAX];
static int         g_objlist_n;
int g_pick_code = -1;            /* set by the UI; -1 = nothing highlighted */

static int objlist_cmp(const void *a, const void *b)
{ return ((const objlist_ent *)a)->code - ((const objlist_ent *)b)->code; }

static void objlist_build(void)
{
    g_objlist_n = 0;
    for (int i = 0; i < geohw_nbuf; i++) {
        int c = geohw_buf[i].pick_code, j;
        for (j = 0; j < g_objlist_n; j++)
            if (g_objlist[j].code == c) { g_objlist[j].quads++; break; }
        if (j == g_objlist_n && g_objlist_n < OBJLIST_MAX)
            g_objlist[g_objlist_n++] = (objlist_ent){ c, 1 };
    }
    qsort(g_objlist, g_objlist_n, sizeof g_objlist[0], objlist_cmp);
}

int  render_objlist_count(void) { return g_objlist_n; }
void render_objlist_get(int i, int *code, int *quads)
{
    if (i < 0 || i >= g_objlist_n) { if (code) *code = -1; if (quads) *quads = 0; return; }
    if (code)  *code  = g_objlist[i].code;
    if (quads) *quads = g_objlist[i].quads;
}
void render_pick_set(int code) { g_pick_code = code; }
int  render_pick_get(void)     { return g_pick_code; }

/* ---- z-fighting detector --------------------------------------------------
 * This renderer is painter's-algorithm (sort by zsort, no depth test --
 * matching the real hardware and the oracle), so the classic GPU
 * depth-buffer-precision z-fight cannot occur. The analogous defect here is
 * two quads from DIFFERENT objects whose zsort keys are so close that a
 * one-unit difference in either (camera motion, a sub-pixel projection
 * change, anything that nudges the 24-bit key by a handful of counts) could
 * flip their draw order between frames -- which reads on screen exactly like
 * z-fighting does on hardware with a depth buffer: a flicker where either
 * surface can "win". Quads are sorted by zsort, so a near-tie is always
 * adjacent (or a few slots away, when more than two quads cluster at one
 * depth); this only has to look at each quad's next few neighbours, not
 * every pair. Screen-space bbox overlap is required too -- two near-tied
 * quads that do not share any pixels cannot flicker into each other. */
static void zfight_report(void)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("PROPCYCL_ZFIGHT"); on = (e && *e != '0'); }
    if (!on) return;
    for (int i = 0; i < geohw_nbuf; i++) {
        const geo_quad *a = &geohw_buf[i];
        if (a->nrv < 3) continue;
        int32_t ax0=999999, ax1=-999999, ay0=999999, ay1=-999999;
        for (int k = 0; k < a->nrv; k++) {
            int32_t x = a->rv[k].sx16, y = a->rv[k].sy16;
            if (x<ax0) ax0=x; if (x>ax1) ax1=x;
            if (y<ay0) ay0=y; if (y>ay1) ay1=y;
        }
        int64_t areaA = (int64_t)(ax1-ax0) * (ay1-ay0);
        if (areaA < 16*16*16*16) continue;    /* degenerate/near-zero-size quad (row 89's class) */
        /* Adjacent parts of the SAME articulated model (rider/bike -- codes
         * a handful apart, e.g. 113/119-127) legitimately touch at joints;
         * that is not a fighting risk, it is correct modelling. Require the
         * codes to be far enough apart that this is plausibly two
         * UNRELATED objects, not two bones of one skeleton. */
        for (int j = i + 1; j < geohw_nbuf && j < i + 6; j++) {
            const geo_quad *b = &geohw_buf[j];
            if (b->nrv < 3) continue;
            int codedist = a->pick_code - b->pick_code;
            if (codedist < 0) codedist = -codedist;
            if (codedist < 40) continue;    /* likely the same rig */
            int32_t dz = b->zsort - a->zsort;
            if (dz < 0) dz = -dz;
            if (dz > 8) continue;         /* not a near-tie */
            int32_t bx0=999999, bx1=-999999, by0=999999, by1=-999999;
            for (int k = 0; k < b->nrv; k++) {
                int32_t x = b->rv[k].sx16, y = b->rv[k].sy16;
                if (x<bx0) bx0=x; if (x>bx1) bx1=x;
                if (y<by0) by0=y; if (y>by1) by1=y;
            }
            int64_t areaB = (int64_t)(bx1-bx0) * (by1-by0);
            if (areaB < 16*16*16*16) continue;
            /* substantial overlap, not just touching at an edge */
            int32_t ox0 = ax0>bx0?ax0:bx0, ox1 = ax1<bx1?ax1:bx1;
            int32_t oy0 = ay0>by0?ay0:by0, oy1 = ay1<by1?ay1:by1;
            if (ox1 <= ox0 || oy1 <= oy0) continue;
            int64_t overlap = (int64_t)(ox1-ox0) * (oy1-oy0);
            int64_t smaller = areaA < areaB ? areaA : areaB;
            if (overlap * 4 < smaller) continue;   /* < 25% of the smaller quad's area */
            /* Both boxes must actually land on the visible 640x480 screen
             * (in 1/16-pixel units) -- otherwise this is a near-plane-clip
             * artifact (a partially-clipped quad's raw box can explode to
             * millions of units, CLAUDE.md's "polygons exploding" note) or
             * geometry that is off-screen anyway and so can never visibly
             * flicker regardless of how close the two z keys are. */
            if (ox0 < 0 || oy0 < 0 || ox1 > SCREEN_WIDTH*16 || oy1 > SCREEN_HEIGHT*16) continue;
            printf("[ZFIGHT] f=%d code_a=%d code_b=%d dz=%d za=%d zb=%d "
                   "boxA=[%d..%d,%d..%d] boxB=[%d..%d,%d..%d] overlap_pct=%d\n",
                   (int)g_sys.frame_count, a->pick_code, b->pick_code, (int)dz,
                   (int)a->zsort, (int)b->zsort,
                   (int)(ax0/16), (int)(ax1/16), (int)(ay0/16), (int)(ay1/16),
                   (int)(bx0/16), (int)(bx1/16), (int)(by0/16), (int)(by1/16),
                   (int)(overlap * 100 / smaller));
        }
    }
}

static void geohw_flush(void)
{
    { extern int g_rig_view; if (g_rig_view) rigview_transform(); }
    if (pc_have_next) { memcpy(pc_pivot, pc_pivot_next, sizeof pc_pivot); pc_have_next = 0; }
    eng_quad_sort(geohw_buf, geohw_nbuf, g_tie_emit);
    zord_report();
    zfight_report();
    g_fogged_quads = 0; g_fogA_min = 999; g_fogA_max = -999;
    spr_px = 0;

    /* MERGED Z ORDER, not layers. MAME queues sprites and polygons into the
     * SAME radix tree (namcos22_v.cpp:1679 and :1086) keyed on a 24-bit z,
     * and render_scene walks it far-to-near in one pass. Drawing sprites as
     * an overlay afterwards put the HUD plate on top of the score digits,
     * which are polygons; merging lets the digits show through. */
    static sprite_item items[1024];
    int ni = 0;
    const sprite_state *sst = NULL;
    { static int off = -1;
      if (off < 0) { const char *e = getenv("PROPCYCL_NO_SPRITES"); off = (e && *e != '0'); }
      /* Not in map mode: the map is a geometry inspector, and the sprite
       * guard that used to cover this lives on geohw_draw_sprites() -- which
       * became dead code when sprites moved into this merged z-order walk.
       * Result: the HUD and banners kept drawing over the map, and their
       * ~200k pixels a frame were most of why it felt slow. */
      if (!off && !ui_map_active()) {
          sst = framedump_current_sprites();
          if (!sst && g_live_ok) sst = &g_live_spr;
          if (sst) ni = sprite_collect(sst, &g_fog, items,
                                       (int)(sizeof items / sizeof items[0]));
          if (getenv("PROPCYCL_SPRC")) { static int n;
            if (n++ % 120 == 0)
              printf("    [SPRC] f=%d sst=%p live_ok=%d ni=%d spr_n=%d\n",
                     (int)g_sys.frame_count, (void*)sst, g_live_ok, ni,
                     sst ? sst->spr_n : -1); }
      } }

    /* ---- billboard picker: recover exact on-screen boxes -------------------
     * game_core.c tags and records each sprite_3d_project_and_draw call by
     * its z key (z_depth + layer*0x200000, the same value sprite_draw_2d
     * writes into the spriteram record); sprite_item.z is that SAME key
     * after the record round-trips through spriteram and sprite_collect's
     * decode, so matching by z recovers which collected item is a billboard
     * without touching sprite_hw.c (kept byte-for-byte faithful to the
     * MiSTer model) at all. Rebuilt every render call, like objlist_build --
     * stable while paused because the display list (and so items[]) is. */
    { extern int billboard_calls_count(void);
      extern void billboard_calls_get(int, int*, uint32_t*, int*, int*);
      int nb = billboard_calls_count();
      /* Claim each collected item at most once, and disambiguate same-z
       * candidates (the tagging caller always passes z_depth = 0, so z is
       * really just the layer) by the sprite's own screen position. */
      static uint8_t claimed[1024];   /* sized with items[] above */
      memset(claimed, 0, (size_t)(ni > 0 ? ni : 0));
      g_billboard_box_n = 0;
      for (int bi = 0; bi < nb && g_billboard_box_n < BILLBOARD_BOX_MAX; bi++) {
          int tile, bsx, bsy; uint32_t z;
          billboard_calls_get(bi, &tile, &z, &bsx, &bsy);
          int best = -1, bestd = 1 << 30;
          for (int si2 = 0; si2 < ni; si2++) {
              if (claimed[si2] || items[si2].z != z) continue;
              /* sprite_collect's box is the CLIPPED bbox, so it will not
               * equal (sx,sy) exactly -- take the nearest unclaimed one. */
              int dx = items[si2].x0 - bsx, dy = items[si2].y0 - bsy;
              int d = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
              if (d < bestd) { bestd = d; best = si2; }
          }
          { int si2 = best;
            if (si2 >= 0) {
              claimed[si2] = 1;
              g_billboard_box[g_billboard_box_n].tile = tile;
              g_billboard_box[g_billboard_box_n].x0 = items[si2].x0;
              g_billboard_box[g_billboard_box_n].y0 = items[si2].y0;
              g_billboard_box[g_billboard_box_n].w  = items[si2].w;
              g_billboard_box[g_billboard_box_n].h  = items[si2].h;
              g_billboard_box_n++;
            } }
      } }

    int qi = 0, si = 0;
    eng_draw_begin();
    while (qi < geohw_nbuf || si < ni) {
        uint32_t qz = (qi < geohw_nbuf)
                      ? (uint32_t)(geohw_buf[qi].zsort & 0xFFFFFF) : 0;
        uint32_t sz = (si < ni) ? items[si].z : 0;
        if (si < ni && (qi >= geohw_nbuf || sz >= qz))
            geohw_draw_sprite_item(sst, &items[si++]);
        else
            geohw_draw_one(&geohw_buf[qi++]);
    }
    eng_draw_end();

    /* ---- the object picker ------------------------------------------------
     * Snapshot what was drawn, and paint the selected object bright, so the
     * question "which of these codes is the water?" is answered by POINTING
     * at it instead of guessing. The buffer is rebuilt identically every frame
     * while paused (the display list is frozen), so the list is stable to
     * hover over. */
    objlist_build();
    if (g_pick_code >= 0) {
        glDisable(GL_TEXTURE_2D);
        glDisable(GL_ALPHA_TEST);        /* the fog pass's hazard, same care */
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        /* pulse, so a highlight over a similarly-coloured object still reads */
        float a = 0.35f + 0.25f * (float)((g_sys.frame_count >> 3) & 1);
        glColor4f(1.0f, 0.0f, 1.0f, a);
        for (int hi = 0; hi < geohw_nbuf; hi++) {
            const geo_quad *hq = &geohw_buf[hi];
            if (hq->pick_code != g_pick_code || hq->nrv < 3) continue;
            glBegin(GL_POLYGON);
            for (int k = 0; k < hq->nrv; k++)
                glVertex2f(hq->rv[k].sx16 / 16.0f, hq->rv[k].sy16 / 16.0f);
            glEnd();
        }
        glDisable(GL_BLEND);
        glEnable(GL_ALPHA_TEST);
    }

    /* ---- billboard picker highlight: a screen-space box, since these are
     * 2D sprites (particle billboards, row 134/137/138), not 3D geometry. */
    if (g_billboard_pick >= 0 && g_billboard_pick < g_billboard_box_n) {
        billboard_box *b = &g_billboard_box[g_billboard_pick];
        glDisable(GL_TEXTURE_2D);
        glDisable(GL_ALPHA_TEST);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        float a = 0.35f + 0.25f * (float)((g_sys.frame_count >> 3) & 1);
        glColor4f(1.0f, 0.0f, 1.0f, a);
        glBegin(GL_QUADS);
        glVertex2f((float)b->x0,        (float)b->y0);
        glVertex2f((float)(b->x0+b->w), (float)b->y0);
        glVertex2f((float)(b->x0+b->w), (float)(b->y0+b->h));
        glVertex2f((float)b->x0,        (float)(b->y0+b->h));
        glEnd();
        glColor4f(1.0f, 0.0f, 1.0f, 1.0f);
        glLineWidth(2.0f);
        glBegin(GL_LINE_LOOP);
        glVertex2f((float)b->x0,        (float)b->y0);
        glVertex2f((float)(b->x0+b->w), (float)b->y0);
        glVertex2f((float)(b->x0+b->w), (float)(b->y0+b->h));
        glVertex2f((float)b->x0,        (float)(b->y0+b->h));
        glEnd();
        glDisable(GL_BLEND);
        glEnable(GL_ALPHA_TEST);
    }

    /* ---- banner picker highlight: a text-tilemap block's screen rect,
     * computed from (col,row,w,h) with the SAME scroll formula text_hw.c's
     * text_render uses (16px tiles, sx/sy from tilemapattr), so no
     * cross-referencing is needed -- unlike sprites, the position is a pure
     * function of data this file already has live access to. */
    if (g_banner_pick >= 0) {
        extern int banner_calls_count(void);
        extern void banner_calls_get(int, int*, int*, int*, int*, int*, int*);
        int n = banner_calls_count();
        if (g_banner_pick < n) {
            int col, row, w, h, base, pal;
            banner_calls_get(g_banner_pick, &col, &row, &w, &h, &base, &pal);
            int sx = (int)(((g_sys.tilemapattr[0] << 8) | g_sys.tilemapattr[1]) - 0x35C) & 0x3FF;
            int sy = (int)(((g_sys.tilemapattr[2] << 8) | g_sys.tilemapattr[3])) & 0x3FF;
            /* tile (col,row) -> screen pixel: inverse of text_hw.c's
             * tx=(x+sx)&0x3FF, trow=ty>>4 -- take the principal (unwrapped)
             * solution; a banner scrolled off past the 1024px wrap is not
             * a case any real banner call hits. */
            int px = col * 16 - sx, py = row * 16 - sy;
            if (px < -512) px += 1024;
            if (px > 512) px -= 1024;
            if (py < -512) py += 1024;
            if (py > 512) py -= 1024;
            int pw = w * 16, ph = h * 16;
            glDisable(GL_TEXTURE_2D);
            glDisable(GL_ALPHA_TEST);
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            float a = 0.35f + 0.25f * (float)((g_sys.frame_count >> 3) & 1);
            glColor4f(0.1f, 1.0f, 1.0f, a);
            glBegin(GL_QUADS);
            glVertex2f((float)px,      (float)py);
            glVertex2f((float)(px+pw), (float)py);
            glVertex2f((float)(px+pw), (float)(py+ph));
            glVertex2f((float)px,      (float)(py+ph));
            glEnd();
            glColor4f(0.1f, 1.0f, 1.0f, 1.0f);
            glLineWidth(2.0f);
            glBegin(GL_LINE_LOOP);
            glVertex2f((float)px,      (float)py);
            glVertex2f((float)(px+pw), (float)py);
            glVertex2f((float)(px+pw), (float)(py+ph));
            glVertex2f((float)px,      (float)(py+ph));
            glEnd();
            glDisable(GL_BLEND);
            glEnable(GL_ALPHA_TEST);
        }
    }

    /* Cached: getenv() inside the frame loop segfaults here (register row
     * 40), and this runs once per rendered frame. */
    { static int on = -1;
      if (on < 0) { const char *e = getenv("PROPCYCL_PICKERDBG"); on = (e && *e != '0'); }
      if (on) {
        extern int banner_calls_count(void);
        static int n;
        if (n++ % 30 == 0)
            printf("[PICKERDBG] f=%d billboards=%d banners=%d\n",
                   (int)g_sys.frame_count, g_billboard_box_n, banner_calls_count());
    } }
    geohw_quads_drawn = geohw_nbuf;
    geohw_nbuf = 0;

    /* ---- screen fade --------------------------------------------------
     * blend(rgb, screen_fade, 0xff - factor) over EVERY pixel, background
     * included -- so it is a full-screen composite, not a per-quad stage.
     * Expanding the reference blend:
     *     out = (rgb*(255-factor) + fade*(1+factor)) >> 8
     * so the fade colour's weight is (factor+1)/256, which is exactly the
     * src alpha for standard GL alpha blending. */
    if (geohw_fog_enabled() && g_fog_valid && !ui_map_active() &&
        (g_fog.mixer_flags & 1) && g_fog.screen_fade_factor) {
        float a = (g_fog.screen_fade_factor + 1) / 256.0f;
        glDisable(GL_TEXTURE_2D);
        glDisable(GL_ALPHA_TEST);      /* same hazard as the fog pass */
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glColor4f(g_fog.screen_fade[0] / 255.0f, g_fog.screen_fade[1] / 255.0f,
                  g_fog.screen_fade[2] / 255.0f, a);
        glBegin(GL_QUADS);
        glVertex2f(g_scene_x0, 0); glVertex2f(g_scene_x1, 0);
        glVertex2f(g_scene_x1, SCREEN_HEIGHT); glVertex2f(g_scene_x0, SCREEN_HEIGHT);
        glEnd();
        glDisable(GL_BLEND);
        glEnable(GL_ALPHA_TEST);
    }
}

/* PROPCYCL_SEAMTEST=<code>[,<code>...]: a CRACK detector. Every quad is drawn
 * flat white with no texture, alpha test or fog, the listed object codes
 * (the backdrop: the sky 108, a sea-level sheet) are skipped, and main.c
 * clears to magenta -- so a magenta pixel with geometry on both sides of it
 * is a pixel NO polygon covered, i.e. a crack, whatever caused it. Colour
 * alone cannot tell a seam from real blue content (roofs, water); coverage
 * can. */
int g_seamtest;                    /* main.c */
int g_seam_legacy;                 /* PROPCYCL_SEAM_LEGACY, main.c */
static int g_seam_lo[16], g_seam_hi[16], g_seam_nskip;
void seamtest_parse(const char *e)          /* "108,672-717,871": codes or ranges */
{
    g_seamtest = 1; g_seam_nskip = 0;
    while (e && *e && g_seam_nskip < 16) {
        char *end; long lo = strtol(e, &end, 10), hi = lo;
        if (*end == '-') hi = strtol(end + 1, &end, 10);
        g_seam_lo[g_seam_nskip] = (int)lo; g_seam_hi[g_seam_nskip++] = (int)hi;
        e = strchr(e, ','); if (e) e++;
    }
}

/* Prop Cycle's fog for one quad: the Super 22 CZ tables (fog_hw.c). */
static int pc_fog_quad(const geo_quad *q, eng_fog *f)
{
    const uint8_t *tab = NULL; int sdelta = 0;
    if (!fog_quad(q->color, q->cz_type, &tab, &sdelta)) return 0;
    memcpy(f->rgb, g_fog.fog_rgb, 3);
    f->tab = tab; f->sdelta = sdelta;
    return 1;
}

/* One quad through the engine's rasteriser (engine/quad_gl.c), with Prop
 * Cycle's board settings: Super 22 fog applied AFTER shading, the mixer's
 * poly fade, and the SEAMTEST coverage probe. */
static void geohw_draw_one(const geo_quad *q)
{
    if (g_seamtest) {
        for (int k = 0; k < g_seam_nskip; k++)
            if (q->pick_code >= g_seam_lo[k] && q->pick_code <= g_seam_hi[k]) return;
        if (q->nrv < 3) return;
        { static FILE *qf; static int qinit;
          if (!qinit) { const char *e = getenv("PROPCYCL_SEAMDUMP"); qinit = 1;
                        if (e) qf = fopen(e, "w"); }
          if (qf) { fprintf(qf, "Q %u %d %d", g_sys.frame_count, q->pick_code, q->nrv);
                    for (int k = 0; k < q->nrv; k++)
                        fprintf(qf, " %d %d %d", (int)q->rv[k].sx16, (int)q->rv[k].sy16, (int)q->rv[k].z);
                    fprintf(qf, "\n"); } }
    }
    eng_draw_cfg cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.shade = geohw_shade_enabled();
    cfg.fog = geohw_fog_enabled() && g_fog_valid && !ui_map_active();
    cfg.fog_before_shade = 0;               /* Super 22 */
    cfg.flat_white = g_seamtest;
    cfg.fog_quad = pc_fog_quad;
    cfg.fade_rgb = geohw_fade_rgb;
    { static int tc = -1; if (tc < 0) { const char *e = getenv("PROPCYCL_TEXEL_CENTRE"); tc = e ? atoi(e) : 1; }
      cfg.texel_centre = tc; }
    eng_draw_quad(q, &cfg);
}

/* Q15 sin/cos for the game's 16-bit angle unit (65536 = 360 degrees). */
static void q15_sincos(int32_t ang, int32_t *s, int32_t *c)
{
    double a = (double)(int16_t)ang * (2.0 * 3.14159265358979 / 65536.0);
    *s = (int32_t)lrint(sin(a) * 32767.0);
    *c = (int32_t)lrint(cos(a) * 32767.0);
}

/* Camera yaw (around Y) then pitch (around X), in the oracle's m[src][dst]
 * convention: vx = (x*m[0][0] + y*m[1][0] + z*m[2][0]) >> 15. */
static void geohw_view_matrix(int32_t m[3][3])
{
    extern intptr_t _W[];
    int32_t sh, ch, sp, cp, sr, cr;
    q15_sincos((int32_t)(_W[0x0CEC] & 0xFFFF), &sh, &ch);   /* heading */
    q15_sincos((int32_t)(_W[0x0CE8] & 0xFFFF), &sp, &cp);   /* pitch   */
    q15_sincos((int32_t)(_W[0x0CF0] & 0xFFFF), &sr, &cr);   /* ROLL    */

    /* yaw */
    int32_t y[3][3] = {{ ch, 0, -sh }, { 0, 0x7FFF, 0 }, { sh, 0, ch }};
    /* pitch */
    int32_t p[3][3] = {{ 0x7FFF, 0, 0 }, { 0, cp, sp }, { 0, -sp, cp }};
    /* ROLL about the view axis. This was MISSING: the camera used yaw and
     * pitch only, so the horizon never banked and the world appeared not
     * to rotate correctly once the bike was moving and leaning.
     *
     * The game tracks roll in W[0x0CF0] (camera_update interpolates it
     * alongside pitch/heading, and dsp_viewport_setup writes its sin/cos),
     * and the recording's own view matrix is NOT a pure yaw+pitch matrix
     * -- poly_f901's viewq has m[1][0] = 615, which a yaw*pitch product
     * cannot produce. rotate_euler_zxy_optimized applies roll first, then
     * pitch, then heading (hence "zxy"). */
    int32_t r[3][3] = {{ cr, sr, 0 }, { -sr, cr, 0 }, { 0, 0, 0x7FFF }};
    int32_t yp[3][3];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) {
            int64_t acc = 0;
            for (int k = 0; k < 3; k++) acc += (int64_t)y[i][k] * p[k][j];
            yp[i][j] = (int32_t)(acc >> 15);
        }
    { const char *e = getenv("PROPCYCL_NO_ROLL");
      if (e && *e != '0') { memcpy(m, yp, sizeof yp); return; } }
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) {
            int64_t acc = 0;
            for (int k = 0; k < 3; k++) acc += (int64_t)yp[i][k] * r[k][j];
            m[i][j] = (int32_t)(acc >> 15);
        }
}


/* Q15 axis rotations in the oracle's m[src][dst] convention (row-vector:
 * v' = v . M, i.e. vx = x*m[0][0] + y*m[1][0] + z*m[2][0]). */
static void q15_mul3(const int32_t a[3][3], const int32_t b[3][3], int32_t o[3][3])
{
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) {
            int64_t acc = 0;
            for (int k = 0; k < 3; k++) acc += (int64_t)a[i][k] * b[k][j];
            o[i][j] = (int32_t)(acc >> 15);
        }
}

/* Placement rotation from the three (sin,cos) pairs the game writes.
 *
 * The pairs come straight from the ROM trig table -- game_dsp3d.c's
 * rotate_euler_zxy_optimized documents it: 0x20B004 = sin, 0x20B006 = cos,
 * Q15. dsp_cmd_place_object_rotated stores them as (s,c) per axis.
 *
 * The ORDER is not derivable from anything validated: the master-DSP HLE
 * (pc_master_model.py) reads these six words and emits [None]*9 for the
 * matrix, so the oracle has no opinion. PROPCYCL_GEO_EULER selects it and
 * the reject census decides -- a wrong order leaves the backface cull
 * rejecting most quads, which is a direct signal rather than a judgement
 * about how the picture looks.
 */
/* Which Euler order a caller wants. TWO emitters, TWO conventions:
 *   ORD_8008 -- the articulated-rig path (0x8008 slot table). MEASURED XYZ
 *               (see g_euler_8008 below): whole-tree orientation median 2 deg.
 *   ORD_8002 -- 0x8002 rotated placements (sky dome, scenery, the inline
 *               13-word form). MEASURED ZYX -- see below. */
enum { ORD_YXZ = 0, ORD_ZXY = 1, ORD_XYZ = 2, ORD_ZYX = 3, ORD_XZY = 4, ORD_YZX = 5 };
int g_euler_8002 = ORD_ZYX;   /* PROPCYCL_GEO_EULER overrides       */
/* ZYX, MEASURED against the master's own output over EVERY capture in the
 * tree (2026-09-17): 1920 dumps, 2451 multi-axis 0x8001/0x8002 entries whose
 * CPU words could be paired with the primitive record the master emitted for
 * the same code at the same translation. Composing the six words as
 * Rz.Ry.Rx reproduces that matrix on 2376 of 2451 (97%), worst element error
 * 0.047. It is the ONLY order that fits all three codes that appear:
 *
 *     code        ZYX        ZXY        XZY        YXZ
 *     108  sky   471/471     0/471      0/471    471/471
 *     447        542/542   542/542      0/542      0/542
 *     871 plate 1363/1438  1363/1438  1363/1438    56/1438
 *
 * (the 75 code-871 entries no order fits are ones where the translation
 * pairing picked the wrong record, not an order question -- every order
 * misses them equally.)
 *
 * THIS SUPERSEDES THE ZXY CONCLUSION OF REGISTER ROW 101, AND THE REASON IT
 * WAS WRONG IS A MEASUREMENT TRAP WORTH KEEPING. euler_8002_gate.py reports
 * only the BEST fit per entry and tallies that, and its `best.sort()` breaks
 * a tie alphabetically -- so on the gameplay and gameplay_steer captures,
 * where ZXY and ZYX both fit EVERY entry, ZXY won the tally purely on the
 * letter Y < Z. Row 101's "100/100 fit ZXY" was true and still is; what it
 * could not say is that 100/100 also fit ZYX. The two are only distinguished
 * by an entry with X and Y both non-zero and Z zero -- and the one such entry
 * in the game is the SKY DOME in the attract cinematic flyover, which is in
 * dumps/attract_start and was not among the captures row 101 swept.
 *
 * The user-visible symptom of getting this wrong is exactly the "black wedge"
 * the pre-row-101 comment here described, and it was never the stale
 * pre-row-87 measurement that comment dismissed it as: under ZXY the flyover
 * sky is rotated ~90 degrees about the wrong axis, so its rim cuts a diagonal
 * across the frame and the clouds sit at an angle instead of filling the
 * screen. Ground truth is unambiguous -- MAME's own CPU words for code 108
 * at attract_start/poly_f180 are [15800, 28706, -23170, 23170, 0, 32767] and
 * ours are [15800, 28712, -23170, 23161, 0, 32767], identical bar trig-table
 * rounding, so the producer was never the problem. The master composes those
 * words into [0.707 -0.341 0.6194 / 0 0.876 0.4822 / -0.7071 -0.341 0.6194],
 * which is Rz.Ry.Rx and is not Rz.Rx.Ry.
 *
 * Sweep it yourself before changing this again -- print EVERY order's error
 * for every entry, never just the winner:
 *     python3 tools/overnight/euler_order_sweep.py dumps/  * /poly_f*.bin
 */
int g_euler_8008 = ORD_XZY;   /* PROPCYCL_GEO_EULER_8008 overrides */
/* THE ORDER THE CHILD-OFFSET MATRIX `P` IS COMPOSED IN.
 *
 * P was hard-coded ORD_ZXY on the reasoning that "no single order
 * satisfies both orientation and position". That was measured BEFORE
 * register rows 87 and 95 fixed the producers' sin/cos pair order, which
 * changed what the six words in a 0x8008 record mean -- so it is no
 * longer true. Re-measured against MAME on every parent->child pair in
 * the flyover rig (predicted child offset L . R_P(parent) vs the
 * recording's own child-minus-parent), summed error over the 7 pairs:
 *
 *     ZXY  276.2 units      XZY  45.5 units
 *
 * XZY wins or ties on every pair individually, and on the right
 * hip->shin (models 89->90, flyover codes 133->134) it is 223.0 -> 1.5.
 * That node is the discriminating case: it is one of the few with BOTH
 * a non-zero X and a non-zero Z angle, so it is the only one where
 * Rz.Rx and Rx.Rz differ -- its mirror (86->87) has Z = 0 and is
 * order-blind, which is why one leg was right and the other was not.
 * PROPCYCL_GEO_EULER_P=<0..5> forces a specific order for A/B; -1 (the
 * default) means "the same order as the orientation matrix". */
int g_euler_p = -1;           /* PROPCYCL_GEO_EULER_P overrides */
/* XZY -- the one order that satisfies all three measurements at once
 * (offline, whole rig, vs the MAME recording at f180):
 *                 root ABSOLUTE   children RELATIVE   position
 *     XZY             1.6 deg          1.9 deg          17
 *     ZXY             1.6              6.1 (L hip 57, feet 176)   19
 *     XYZ            66.8 (rolls the whole rig 90 deg)  1.9   1048
 *     ZYX / YXZ / YZX   -- 57+ on the children or 66.8 on the root
 * The relative-to-body metrics alone cannot see the root roll; XYZ was
 * shipped briefly on their strength and the rider lay on his side. Always
 * check the root's absolute orientation as well. */
/* XYZ, not ZXY, for the rig -- MEASURED on the whole 18-joint tree, each
 * node's orientation against the MAME recording (deg, median / mean):
 *     XYZ   2 / 11     <- this: hips 1, shins 1-2, feet 1-2, spine 2
 *     ZXY   6 / 40        left hip 57, both feet 176
 *     XZY   6 / 19        feet 51
 *     ZYX  40 / 56,  YXZ 6 / 30,  YZX 40 / 52
 * The earlier position-only sweep that picked ZXY never tried XYZ, and
 * its anchor node has a zero X angle, which makes several orders
 * indistinguishable there. The name rotate_euler_zxy_optimized describes
 * the CPU-side routine's loop order, not this record's consumption. */

static void placement_matrix_ord(int32_t s1, int32_t c1, int32_t s2, int32_t c2,
                                 int32_t s3, int32_t c3, int32_t m[3][3], int ord);
static void placement_matrix(int32_t s1, int32_t c1, int32_t s2, int32_t c2,
                             int32_t s3, int32_t c3, int32_t m[3][3])
{   placement_matrix_ord(s1, c1, s2, c2, s3, c3, m, g_euler_8002); }

static void placement_matrix_ord(int32_t s1, int32_t c1, int32_t s2, int32_t c2,
                                 int32_t s3, int32_t c3, int32_t m[3][3], int ord)
{
    const int32_t Rx[3][3] = {{0x7FFF,0,0},{0,c1,s1},{0,-s1,c1}};
    const int32_t Ry[3][3] = {{c2,0,-s2},{0,0x7FFF,0},{s2,0,c2}};
    const int32_t Rz[3][3] = {{c3,s3,0},{-s3,c3,0},{0,0,0x7FFF}};
    /* DEFAULT IS ZXY -- measured, not assumed.
     *
     * The game's own routine is rotate_euler_zxy_optimized, and an offline
     * sweep of the rider rig's 12-node subtree against the MAME recording
     * (frame 180, anchored on the body) gives, per Euler order:
     *     ZXY  mean 22  max 39   <- this
     *     XZY  mean 24  max 50   (ties: node 0 has no X angle)
     *     ZYX  mean 137
     *     YXZ  mean 854..1188    <- the previous default
     * Read ONCE: getenv()+atoi() inside the frame loop faults because the
     * environment block gets overwritten at runtime (see the register). */
    int32_t t[3][3];
    switch (ord) {
    case ORD_ZXY: q15_mul3(Rz, Rx, t); q15_mul3(t, Ry, m); break;
    case ORD_XYZ: q15_mul3(Rx, Ry, t); q15_mul3(t, Rz, m); break;
    case ORD_ZYX: q15_mul3(Rz, Ry, t); q15_mul3(t, Rx, m); break;
    case ORD_XZY: q15_mul3(Rx, Rz, t); q15_mul3(t, Ry, m); break;
    case ORD_YZX: q15_mul3(Ry, Rz, t); q15_mul3(t, Rx, m); break;
    default:      q15_mul3(Ry, Rx, t); q15_mul3(t, Rz, m); break;  /* YXZ */
    }
}

/* When non-NULL, this matrix is used INSTEAD of building one from `rot`.
 * The 0x8008 accumulation path (g_asm_mode 3) already has a composed
 * matrix and must not re-derive one from a single node's angles. */
static const int32_t (*g_prebuilt_rot)[3] = NULL;
/* Euler order for a `rot` handed to render_object_hw_rot, when it is not a
 * prebuilt matrix. -1 means "the 0x8002 placement order" (g_euler_8002).
 *
 * A 0x800A's rotation comes from its 0x8008 transform record, and the rig
 * builds every 0x8008 matrix with g_euler_8008 (XZY). The fallback below used
 * placement_matrix(), which is hard-wired to g_euler_8002 (YXZ) -- so a part
 * drawn through the fallback got a DIFFERENT orientation convention from the
 * parts drawn through the slot table, out of the very same angles. Measured on
 * the gameplay body: 31.1 deg through the rig, 145.8 deg through the fallback,
 * 115.4 deg apart. Everything parented to it was then assembled onto a body
 * pose that is not the one on screen.
 *
 * Also set per-entry by the 0x8002 walker from the entry's flags word
 * (bit 1 -> ZYX, else ZXY -- see g_8002_flagord); the walker saves and
 * restores it around the call. */
static int g_rot_ord_override = -1;

/* The cockpit / HUD group -- selected by VIEWPORT INDEX, not by model id.
 *
 * MAME draws whatever the CPU placed under header word 1 with an identity
 * view and the 409.59 focal (its own 0x15 viewport record), regardless of
 * model. The old rule here was "CPU models 610..637", fitted to one capture;
 * the game's HUD emitters actually use models 603..646 (TIME reel faces
 * 0x25b+d / 0x265+d, POINT reel faces 0x273+d / 0x27d+d -- see game_misc.c
 * ~5240 and FUN_0000e37e) and the sky dome 802 is placed under viewport 1 in
 * gameplay too. The faces outside 610..637 are the ones shown for a few
 * frames while a counter rolls, so they took the WORLD camera and flew off
 * into the sky whenever the timer or the score changed -- "I see the HUD
 * randomly out in the sky sometimes". Measured over every frame of
 * dumps/gameplay, gameplay_steer, attract_start and demo_flight: codes
 * 672..715 (models 603..646) plus 871 (802) under the 409.59 viewport,
 * nothing else.
 *
 * This is a MEASURED stand-in for a stage the master DSP owns; when the
 * master port lands the viewport record itself carries all of it. */
/* THE ENDING'S TWO VIEWPORTS (gameplay sub 15) ARE THE OTHER WAY ROUND.
 *
 * dsp_cmd_emit_object_mode_8000/8002 (ROM 0x026072/0x0260B4) put everything
 * under viewport 3 exactly when 0xE00CC0 == 15 -- the ending -- and the
 * ending's own camera (camera_set_from_array + dsp_viewport_setup(3, 2))
 * drives it. MAME's records at ending counters 3400..6000
 * (tools/overnight/snap_ending.lua, polygon RAM decoded):
 *   vp0  identity view, zoom 554.25, full screen, ap 7   -- the credits'
 *        little standing figures, emitted under `0x8001, 0`
 *   vp3  the camera's view, zoom 372.39, vx -105 vy -49, clip 430 x 322,
 *        ap 4                                        -- the 3D window
 * so in the ending vp0 is screen space and vp3 is the world, where in
 * gameplay vp0 is the world and vp1/vp3 the HUD. The window comes from the
 * CPU's viewport words viewport_params_write (ROM 0x02AF9C) stores: half
 * width 0xE172F8, half height 0xE172F6, the slide 0xE172F4. zoom =
 * half_width * sqrt(3) (320 -> 554.25, 215 -> 372.39: a 60 degree FOV
 * across the window, like the full screen), vx = the +0x54 word, vy = the
 * NEGATED +0x58 word (MAME: -49 where the CPU writes 49).
 * PROPCYCL_NO_ENDING_VP=1 restores the gameplay rule for A/B. */
static int ending_vp(void)
{
    static int off = -1;
    if (off < 0) { const char *e = getenv("PROPCYCL_NO_ENDING_VP"); off = (e && *e != '0'); }
    return !off && (int32_t)_W[0x0CC0] == 0xF;
}

static int16_t rd_w16(uint32_t o)
{
    uint32_t sl = (uint32_t)_W[o & ~3u];
    return (o & 2) ? (int16_t)(sl & 0xFFFF) : (int16_t)(sl >> 16);
}

/* vp3's window in the ending, from the CPU's viewport words (see above). */
static void ending_vp3_params(geo_view *gv)
{
    int hw = rd_w16(0x172F8), hh = rd_w16(0x172F6), slide = rd_w16(0x172F4);
    int dx = 0x140 - hw, dy = 0xF0 - hh;
    double z = (double)hw * 1.7320508075688772;
    int shift = 0; int32_t mant;
    if (hw <= 0 || hh <= 0) return;
    while (shift < 15 && z * (double)(1 << (shift + 1)) <= 32767.0) shift++;
    mant = (int32_t)(z * (double)(1 << shift));
    gv->zoom_mant = mant; gv->zoom_shift = shift;
    gv->vx = (int32_t)(int16_t)((((dx * slide) * 2) >> 8) - dx);
    gv->vy = -(int32_t)(int16_t)(((dy * 0x1A0) >> 8) - dy);
    gv->cl = gv->cr = -((float)hw + 0.5f);
    gv->cu = gv->cd = -((float)hh + 0.5f);
    gv->have_clip = 1;
    gv->absolute_priority = 4;
}

static int hud_screen_space(int code)
{
    if (g_hud_screen_off) return 0;      /* PROPCYCL_NO_HUDFIX=1, for A/B */
    /* ...and in the ending an 0x8001 entry is in CAMERA space whatever its
     * viewport. The bad ending (stage10, ROM 0x02EE3A) emits its lightning
     * and the column of light as `0x8001, 3` entries at (v, -v, 0x18000)
     * etc., and MAME's own records for them (snap_story_badend.lua
     * PCS_DUMPK, bad-ending locals 165/200/245/290/305) carry t equal to the
     * CPU's words UNROTATED and M = the entry's own Rz with no view folded
     * in -- while the 0x8002 sparks beside them, in the same viewport, carry
     * the camera. In every gameplay/attract capture 0x8001 appears only under
     * the HUD viewport 1, so this is consistent with those too, but it is
     * applied to the ending only. PROPCYCL_END8001_WORLD=1 restores the old
     * treatment for A/B. */
    if (ending_vp()) {
        static int legacy = -1;
        if (legacy < 0) { const char *e = getenv("PROPCYCL_END8001_WORLD"); legacy = (e && *e && *e != '0'); }
        return cur_viewport == 0 || (!legacy && cur_hdr == 0x8001);
    }
    if (g_hud_by_model) return code >= 610 && code <= 637;   /* the old rule */
    (void)code;
    /* VIEWPORT 3 IS SCREEN SPACE TOO -- the ADVANCED-mode day counter.
     *
     * Register row 100 established that the second header word is a VIEWPORT
     * INDEX and that index 1 is the screen-space HUD, measured over
     * dumps/gameplay, gameplay_steer, attract_start and demo_flight. Every one
     * of those is NOVICE: ADVANCED mode was unreachable until register row 149
     * made the stick able to pick it, so index 3 appears in no capture in this
     * tree and the rule was written as `== 1` for want of a counter-example.
     *
     * ROM 0x00E864 is `pea $3.w` -- `FUN_0000e832` emits the five digit plaques
     * of the ADVANCED day/score counter (models 0x347+d, codes 908-917) under
     * viewport 3, and our transcription of it already matched the ROM exactly.
     * Falling through to the world camera is what a user reported as "908 and
     * 909 are out in the background when they should be static just below the
     * score counter": measured, they drew at x[-520..-248], off the left edge,
     * with ap 6 / view_ap 7 (the world) where the HUD gauges carry 5 / 6.
     *
     * A census of our own display list says this is a TARGETED change and not a
     * widening of row 100's rule: on an ADVANCED gameplay frame, viewport 0
     * carries the world (sky 108, terrain 1251+, props), viewport 1 carries
     * exactly row 100's HUD set (677, 687-697, 706, 717, 871), and viewport 3
     * carries 908 and 909 AND NOTHING ELSE. PROPCYCL_HUD_VP1ONLY=1 restores
     * `== 1` for A/B. */
    /* THE VIEW IS PER VIEWPORT, IN THE VIEWPORT'S OWN BLOCK (2026-09-22).
     * dsp_viewport_setup (ROM 0x022D3A) writes the camera's three rotation
     * pairs into words +0x04..+0x18 of the block of the viewport it is
     * given -- viewport 0 every frame, and a cut-scene's chosen viewport --
     * and dsp_param_init (run at every stage and scene transition) fills
     * every block with the identity pattern (0, 0x7FFF) x 3. A viewport
     * whose block still holds that pattern has never been given a camera:
     * the master draws it with an identity view, i.e. in screen space. The
     * rule "1 or 3" was that fact seen from two captures; the attract's
     * FASTEST "PERFECT" SCORE page draws its course plate under viewport 2
     * and the stage select its lifted plate under viewport 4, and both flew
     * off with the world camera. PROPCYCL_VP_VIEW_LEGACY=1 restores 1-or-3. */
    { extern int g_vp_view_legacy;
      if (!g_vp_view_legacy && cur_viewport > 0 && cur_viewport < 8 && cmd_buf_base >= 0x400) {
          uint32_t b = cmd_buf_base - 0x400 + (uint32_t)cur_viewport * 0x80;
          int untouched = 1, k;
          for (k = 0; k < 3; k++)
              if (dsp_r32(b + 0x04 + k * 8) != 0 || dsp_r32(b + 0x08 + k * 8) != 0x7FFF) untouched = 0;
          return untouched;
      } }
    { extern int g_hud_vp1only;
      return g_hud_vp1only ? (cur_viewport == 1)
                           : (cur_viewport == 1 || cur_viewport == 3); }
}
int g_vp_view_legacy;   /* PROPCYCL_VP_VIEW_LEGACY, main.c */

/* THE MASTER'S VIEWPORT TABLE -- viewports 2 and 4..7.
 *
 * The CPU initialises one 0x20-word block per viewport index at DSP RAM
 * 0xC10000 + buf*0x8000 + vp*0x80 (ROM 0x022D3A, a d2 = buffer / d3 =
 * viewport double loop): camera sin/cos pairs = identity, lighting
 * (0xC8, 0x14, light 0/0x5A82/0xA57E), zoom word 0x780, centre (0x140, 0xF0)
 * and, at word 0x14, the ABSOLUTE PRIORITY `moveq #7,d1 ; sub.l d3,d1` =
 * 7 - viewport. Dumped out of MAME on the results page (story and NOVICE,
 * tools/overnight/snap_results_story.lua): vp0..7 carry 7,6,5,4,3,2,1,0 and
 * every block is still the identity camera with zoom 0x780 except vp1
 * (0x980, the HUD's 409.59). The master's 0x15 records agree: vp2 comes out
 * with view_ap 5, zoom 0x294548 (554.25) and an identity view; vp5 with
 * view_ap 2, same zoom, identity.
 *
 * The only thing that ever writes a camera into a block is
 * dsp_viewport_setup, reached from dsp_cmd_set_matrix_and_render(W[0x169F0])
 * -- viewport 0 in gameplay and 3 in the ending -- so viewports 2 and 4..7
 * keep the identity view. The live renderer gave them the WORLD camera and
 * the world's priority 7. On the results page that sorted the stamp
 * (0x341, viewport 5, z 1472) and the minimap dots (0x38C..0x391,
 * viewport 2, z 17952) BEHIND the board (viewport 0, z 848, same priority,
 * nearer): the COMPLETED/FAILED stamp and the whole map summary were
 * emitted every frame and never visible (register row 167's open tail).
 * PROPCYCL_VP_LEGACY=1 restores the old treatment for A/B. */
static int plain_viewport(void)
{
    static int legacy = -1;
    if (legacy < 0) { const char *e = getenv("PROPCYCL_VP_LEGACY");
                      legacy = (e && *e && *e != '0'); }
    if (legacy) return 0;
    return cur_viewport == 2 || (cur_viewport >= 4 && cur_viewport <= 7);
}

/* VIEWPORT 2 AS A SUB-WINDOW: the NAME ENTRY magnifier.
 *
 * The CPU describes each viewport to the master DSP in a 32-word record at
 * DSP RAM 0xC10000 + buffer*0x8000 + viewport*0x80 (dsp_param_init, ROM
 * 0x022D3A, writes the defaults: word 14 = 0x780, 15/16 = 320/240, 17..19 =
 * 0, 20 = 7 - viewport). name_entry_render (ROM 0x02A852) re-aims viewport 2
 * every frame: words 15/16 = 0x19 (a 25-pixel half-size) and 18/19 = the
 * lens position negated. MAME's own 0x15 viewport record for it
 * (tools/overnight/snap_nameentry.lua, poly_ne300.bin) gives the master's
 * rule exactly:
 *     zoom = w15 / tan(w14/64 degrees)       25/tan(30) = 43.30  (vp0: 554.25)
 *     clip  l = -(w18+w15)  r = w18-w15  u = w19-w16  d = -(w19+w16)
 *           (the geo stage's window, cx+l .. cx-r-1, cy+u .. cy-d-1)
 *     centre offset vx = vy = 0, absolute priority w20 = 5
 * i.e. the panel drawn again ten times closer at a twelfth of the focal
 * length, 1.25x magnified, and scissored to a 50-pixel square under the
 * ring sprite. Only a SUB-window record takes this path (w15 < 320 or
 * w16 < 240): viewport 2 is also used full-screen by the menus, which keep
 * the world treatment they had. PROPCYCL_NO_LENSVP=1 disables it for A/B. */
static int lens_viewport(geo_view *gv)
{
    static int off = -1;
    if (off < 0) off = getenv("PROPCYCL_NO_LENSVP") != NULL;
    if (off) return 0;
    /* The record of the buffer the CPU built THIS frame, addressed the way
     * the game addresses it (0xC10000 + W[0x0CA0]*0x8000): this engine's
     * list cursor and W[0x0CA0] are not kept in step (game_logic toggles
     * W[0x0CA0] on its own), so cmd_buf_base is not a safe way to find it. */
    /* The block of the list being drawn: cmd_buf_base - 0x400, like every
     * other viewport-record read here. The old reason for W[0x0CA0] (the list
     * cursor and the index drifting apart) was register row 183, fixed by
     * pinning W[0x0CA0] -- after which W[0x0CA0] names the OTHER buffer at
     * render time and the lens read last frame's record (row 188). */
    const uint8_t *r;
    if (cmd_buf_base < 0x400) return 0;
    r = &g_sys.dspram[cmd_buf_base - 0x400 + 2 * 0x80];
    int32_t w13, w14, w15, w16, w18, w19;
    memcpy(&w13, r + 0x34, 4); memcpy(&w14, r + 0x38, 4);
    memcpy(&w15, r + 0x3c, 4); memcpy(&w16, r + 0x40, 4);
    memcpy(&w18, r + 0x48, 4); memcpy(&w19, r + 0x4c, 4);
    (void)w13;
    { static unsigned lf; if (getenv("PROPCYCL_LENSDBG") && lf != g_sys.frame_count) { lf = g_sys.frame_count;
        fprintf(stderr, "[LENS] f%u base=%X w14=%d w15=%d w16=%d w18=%d w19=%d\n", g_sys.frame_count, cmd_buf_base, w14, w15, w16, w18, w19); } }
    if (w15 <= 0 || w16 <= 0 || w15 > 320 || w16 > 240) return 0;
    if (w15 == 320 && w16 == 240) return 0;
    if (w18 < -640 || w18 > 640 || w19 < -480 || w19 > 480) return 0;
    if (w14 < 64 || w14 > 64 * 89) w14 = 0x780;
    {   double z = (double)w15 / tan((double)w14 / 64.0 * M_PI / 180.0);
        int sh = 0;
        while (sh < 15 && z * 2.0 < 32767.0) { z *= 2.0; sh++; }
        gv->zoom_mant = (int32_t)(z + 0.5); gv->zoom_shift = sh; }
    gv->vx = gv->vy = 0;
    gv->cl = (float)(-(w18 + w15)) - 0.5f;
    gv->cr = (float)(w18 - w15) - 0.5f;
    gv->cu = (float)(w19 - w16) - 0.5f;
    gv->cd = (float)(-(w19 + w16)) - 0.5f;
    gv->have_clip = 1;
    gv->absolute_priority = 5;
    return 1;
}

static void render_object_hw_rot(int code, float px, float py, float pz,
                                 const int32_t *rot)
{
    if (code <= 0 || code >= (int)g_pointrom_count) return;
    if (g_seen_mode) {
        extern int g_p360_new, g_p360_dup;
        if (seen_check_add(code, px, py, pz)) { g_p360_dup++; return; }
        if (g_seen_mode == 2) g_p360_new++;
    }
    /* Reject placements outside any sane world bound.
     *
     * The 8x16 terrain grid is 8*0x18000 x 16*0x18000 units, so nothing
     * legitimate sits millions of units out. animated_objects_tick walks
     * 16 slots with `uint8_t *p = &W[0x4178]` and byte offsets +0x48/+0x58
     * -- but W is intptr_t[], so those offsets land 9 and 11 SLOTS away
     * instead of inside a 0x58-byte struct. The activity test therefore
     * reads garbage, every unspawned slot reads as active, and 16 objects
     * get emitted at (0, 134025648, 0) -> |t| = 284,545,592.
     *
     * They are ~284 million units out, i.e. sub-pixel and invisible, so
     * this costs nothing visually -- it stops the wasted transform work
     * and keeps the [DIST] census meaningful. Fixing the slot addressing
     * properly is the real repair; see FAILED_APPROACHES / GUARDRAILS on
     * the (&W[base])[N] class. */
    { const double LIMIT = 16.0 * 0x18000 * 8.0;   /* 8x the grid extent */
      double ax = fabs(px), ay = fabs(py), az = fabs(pz);
      if (ax > LIMIT || ay > LIMIT || az > LIMIT) {
          static int nrej;
          if (nrej++ == 0 && propcycl_verbose())
              printf("  [GUARD] placement out of world bounds (model %d at "
                     "%.0f,%.0f,%.0f) -- skipped; see renderer_3d.c\n",
                     code, px, py, pz);
          return;
      } }

    { const char *e = getenv("PROPCYCL_TRACE_MODEL");
      if (e && code == atoi(e)) {
          static int n;
          if (n++ < 6)
              printf("    [TRACEM] code=%d raw px,py,pz = (%.0f,%.0f,%.0f) rot=%s\n",
                     code, px, py, pz, rot ? "yes" : "no");
      } }
    geo_view gv;
    memset(&gv, 0, sizeof gv);
    gv.objectshift = g_no_objshift ? 0 : g_obj_shift;   /* see g_obj_shift */
    /* PROPCYCL_GEO_IDENTITY=1: bisect the camera matrix out of the picture.
     * The gate proves the cull logic is right under identity (28/28 vs the
     * oracle), so if identity culls at a normal rate in-game and the real
     * camera matrix culls 80%, the matrix is the fault. */
    { const char *e = getenv("PROPCYCL_GEO_IDENTITY");
      if (e && *e != '0') { memset(gv.m, 0, sizeof gv.m);
          gv.m[0][0] = gv.m[1][1] = gv.m[2][2] = 0x7FFF; }
      else geohw_view_matrix(gv.m); }

    /* Lighting for the live path.
     *
     * geo_hw implements the per-vertex lighting stage exactly (it is part
     * of what makes the frame gate pass 6/6), but the live path used to
     * hand it an all-zero geo_view, so every quad rendered unlit while the
     * replay came out shaded. These are the values MAME's DSP actually
     * emits in the viewport record (decoded from poly_f1500.bin):
     *
     *     ambient 0x14 = 20, power 0xc8 = 200
     *     light   Q15 (0, 23170, -23170) = (0, 0.7071, -0.7071)
     *
     * i.e. a unit directional light 45 degrees from above and behind.
     * They are constants here rather than derived, because the master DSP
     * computes the record and that stage does not exist yet -- so this is
     * an approximation, but a measured one, and strictly closer than the
     * zeros it replaces. When the master stage lands these come from the
     * record like everything else.
     *
     * viewq is the VIEW matrix alone: the normals dot the light against
     * the view, not against the object*view product built below. */
    memcpy(gv.viewq, gv.m, sizeof gv.viewq);
    gv.light[0] = 0; gv.light[1] = 23170; gv.light[2] = -23170;
    gv.ambient = 20; gv.power = 200;
    { const char *e = getenv("PROPCYCL_NO_LIGHT");
      if (e && *e != '0') { gv.ambient = gv.power = 0;
          gv.light[0] = gv.light[1] = gv.light[2] = 0;
          memset(gv.viewq, 0, sizeof gv.viewq); } }
    {   /* Is the camera actually identity here, or is geohw_view_matrix
         * reading the wrong work-RAM words? Identity and non-identity runs
         * produced byte-identical output, which has exactly these two
         * explanations and the screen cannot tell them apart. */
        extern intptr_t _W[];
        static int d = 0;
        if (d < 3) { d++;
            if (propcycl_verbose()) printf("  [CAMDBG] heading=%ld pitch=%ld  m=[%d %d %d | %d %d %d | %d %d %d] t=%d,%d,%d\n",
                   (long)(_W[0x0CEC] & 0xFFFF), (long)(_W[0x0CE8] & 0xFFFF),
                   gv.m[0][0],gv.m[0][1],gv.m[0][2], gv.m[1][0],gv.m[1][1],gv.m[1][2],
                   gv.m[2][0],gv.m[2][1],gv.m[2][2], (int)px,(int)py,(int)pz);
        }
    }
    { static int rd = 0; if (rd < 4) { rd++;
        if (!propcycl_verbose()) { } else if (rot) printf("  [ROTDBG] rot=%d,%d %d,%d %d,%d\n",
                        rot[0],rot[1],rot[2],rot[3],rot[4],rot[5]);
        else     printf("  [ROTDBG] rot=NULL (model %d)\n", code); } }
    /* THE COCKPIT/HUD GROUP HAS NO OBJECT ROTATION.
     *
     * Ground truth, dumps/gameplay/poly_f3340.bin: codes 693, 694, 697 and
     * 706 all carry an EXACTLY identity object matrix -- 16383/0/0, 0/16383/0,
     * 0/0/16383 in 2.14 -- in every frame of the capture. The digit "flip"
     * is a MODEL SWAP, not a rotation; the reel faces are separate point-ROM
     * objects and the game changes the model id.
     *
     * We were composing the record's rotation words onto them anyway, and it
     * is not identity under either reading: cos-first gives m[1][1] = 0 and
     * sin-first a 124 deg turn about Y. Either way the reels were drawn
     * edge-on or backwards, which is why the counters looked skewed and
     * their animation looked wrong. The translation was already special-cased
     * for exactly this group a few lines above; the rotation was not. */
    if (hud_screen_space(code)) {
        /* KEEP THE ENTRY'S OWN ROTATION. The note below was right about the
         * view and wrong about the reels: MAME's HUD records are identity
         * only while a counter is still. In dumps/gameplay code 689 carries
         * 17 distinct matrices and 693 eight, all rotations about X, and the
         * CPU list shows why -- FUN_0000e37e rolls the POINT faces with
         * rot_x = (score & 7) * -0x1000 and the TIME emitter rolls its faces
         * with rot_x = frac << 11. Dropping rot drew every rolling face flat.
         * What was actually wrong in row 69 was the pair order at the
         * producer (row 87), fixed since. PROPCYCL_HUD_NOROT=1 restores the
         * drop for A/B. */
        if (g_hud_norot) { rot = NULL; g_prebuilt_rot = NULL; }
        /* and the VIEW is identity for this group too -- in a gameplay
         * capture the 0x0a view record is identity and the HUD's own object
         * matrix is identity, so the matrix these are drawn with is identity
         * outright. gv.m still held the camera rotation, which is why the
         * reels kept turning with the camera. The translation was already
         * taken off gv.viewq for the same reason. */
        /* 0x7FFE, not 0x7FFF: the HUD viewport record carries its identity
         * view as 007FFE on the diagonal (words 12..20 of the 0x15 record at
         * 0x304 in every gameplay dump), and the master pushes the
         * translation through it too -- MAME's record for code 706 lands at
         * z = 543 from the CPU's 544 (544 * 32766 >> 15). With 0x7FFF and a
         * raw t our POINT housing projected one pixel high at the top edge
         * (y 57 against the record's 58); with the record's own value the
         * four vertices are identical. */
        memset(gv.m, 0, sizeof gv.m);
        gv.m[0][0] = gv.m[1][1] = gv.m[2][2] = 0x7FFE;
        memcpy(gv.viewq, gv.m, sizeof gv.viewq);
    } else if (plain_viewport()) {
        /* identity camera for a viewport the game never points a camera at
         * -- see plain_viewport(). MAME's records carry 0x7FFE / 0x7FFC. */
        memset(gv.m, 0, sizeof gv.m);
        gv.m[0][0] = gv.m[1][1] = gv.m[2][2] = 0x7FFE;
        memcpy(gv.viewq, gv.m, sizeof gv.viewq);
    }
    /* object rotation first, then the view: v . (Robj . Rview) */
    if (rot || g_prebuilt_rot) {
        int32_t ro[3][3], comb[3][3];
        if (g_prebuilt_rot) memcpy(ro, g_prebuilt_rot, sizeof ro);
        else placement_matrix_ord(rot[0], rot[1], rot[2], rot[3], rot[4], rot[5], ro,
                                  g_rot_ord_override >= 0 ? g_rot_ord_override : g_euler_8002);
        q15_mul3(ro, gv.m, comb);
        memcpy(gv.m, comb, sizeof comb);
        { static int md = 0; if (md < 2) { md++;
            if (propcycl_verbose()) printf("  [MATDBG] composed m=[%d %d %d | %d %d %d | %d %d %d]\n",
                   gv.m[0][0],gv.m[0][1],gv.m[0][2],
                   gv.m[1][0],gv.m[1][1],gv.m[1][2],
                   gv.m[2][0],gv.m[2][1],gv.m[2][2]); } }
    }
    /* PROPCYCL_DRAWMAT=1: the matrix the object is ACTUALLY DRAWN with, after
     * the object/rig rotation has been composed onto the view. The dist-dump
     * P line records gv.m BEFORE this compose, and the [MAT] probe records the
     * rig's internal slot table -- neither is what reaches the screen, which
     * is how the gameplay rig gate stayed green over a visibly wrong picture. */
    { extern int g_draw_mat;
      if (g_draw_mat) printf("[DRAW] code=%d m=%d %d %d %d %d %d %d %d %d t=%d %d %d\n", code,
              gv.m[0][0],gv.m[0][1],gv.m[0][2], gv.m[1][0],gv.m[1][1],gv.m[1][2],
              gv.m[2][0],gv.m[2][1],gv.m[2][2], (int)px,(int)py,(int)pz); }
    /* t is the position IN VIEW SPACE. The game has already subtracted the
     * camera position (dsp_cmd_place_object does `param_3 - W[0x0CDC]`), so
     * what remains is the camera ROTATION -- applied with the same
     * m[src][dst] convention the vertex transform uses. Passing the raw
     * camera-relative position here put every object in front of the eye at
     * z ~ 0 and produced one screen-filling quad. */
    {
        /* USE THE VIEW MATRIX ALONE (gv.viewq), NOT gv.m.
         *
         * gv.m has already had the object's own rotation composed into it
         * by the block above, so translating through it rotated each
         * object's POSITION by its own orientation -- an object that spins
         * would orbit the origin instead of spinning in place. gv.viewq is
         * copied from gv.m before that compose and is exactly the view.
         *
         * This only ever bit rotated placements (rot != NULL), which is why
         * it survived: the unrotated 0x8000/0x800a paths take gv.m == view
         * anyway. It surfaced when the flyover's 0x800a records started
         * applying their 0x8008 rotation -- turning it on made the rider's
         * part positions WORSE (median error vs the recording 1224 -> 5764)
         * until the translation was taken off the composed matrix. */
        int32_t pv[3] = { (int32_t)px, (int32_t)py, (int32_t)pz };
        if (0) {   /* HUD: gv.viewq is the record's 0x7FFE identity now, so the generic loop below applies */
            /* THE COCKPIT/HUD GROUP IS ALREADY IN SCREEN SPACE.
             *
             * These placements are emitted with their final view-space
             * coordinates and must NOT be rotated by the camera. Ground
             * truth, dumps/gameplay, every frame of the capture:
             *
             *   code 706 (a score digit) t=[314,243,544]  M = IDENTITY
             *   code 138 (the bike)      t=[8781,...]     M rotated
             *
             * and t for the HUD codes is CONSTANT across all 320 frames
             * while the camera turns. Our emitted list already carries
             * exactly those numbers -- raw words at the HUD group read
             * [637, 314, 243, 544] and [625, 274, 243, 544], byte-identical
             * to MAME. Running them through gv.viewq like world geometry is
             * what swung them off the plates: measured, our HUD translation
             * wandered (-297,270,540) -> (439,354,302) over one flight
             * where MAME's never moves, and PROPCYCL_ZORD put the digits at
             * x[837..1443] y[-554..-281] on a 640x480 screen.
             *
             * So: identity view for this group. */
            gv.t[0] = pv[0]; gv.t[1] = pv[1]; gv.t[2] = pv[2];
        } else if (g_seam_legacy) {   /* PROPCYCL_SEAM_LEGACY=1: the old full-precision t */
            for (int j = 0; j < 3; j++) {
                int64_t acc = 0;
                for (int k = 0; k < 3; k++) acc += (int64_t)pv[k] * gv.viewq[k][j];
                gv.t[j] = (int32_t)(acc >> 15);
            }
        } else {   /* every viewport, the HUD included: t . viewq */
            /* THE TRANSLATION GOES THROUGH THE SAME MATRIX AS THE VERTICES.
             *
             * The vertices are transformed by gv.m, which below becomes the
             * view HALVED to 2.14 (`gv.m >>= 1`) -- a point-ROM unit is half a
             * world unit -- and the halving drops each element's lowest bit.
             * Pushing t through the FULL-precision view instead left the two
             * disagreeing by that bit times the object's extent: up to 3
             * units for a terrain chunk, whose corners sit 0x18000 world
             * units from its centre (98304 / 32768 = 3). A corner two chunks
             * share is transformed once from each chunk's centre, so the two
             * copies landed up to ~6 units apart and GL's pixel-centre rule
             * left the sky showing through the join -- the blue seam lines
             * between terrain chunks (register row 179). Measured at Wind
             * Woods cells 17/25: the shared corner projected to (64.75,-71.0)
             * z 8669 from one chunk and (64.44,-70.94) z 8666 from the other.
             *
             * With t through viewq>>1 (<<1 to stay in world units), both
             * copies are the same exact value 2P . (viewq>>1) >> 15 up to two
             * integer floors, i.e. within one unit. MAME has no seam because
             * its renderer transforms in FLOAT; its records carry the same
             * odd view entries (11% of them) that the fixed-point halving
             * truncates. geo_hw.c is untouched, so the framedump path and
             * every frame gate are byte-identical. */
            for (int j = 0; j < 3; j++) {
                int64_t acc = 0;
                for (int k = 0; k < 3; k++) acc += (int64_t)pv[k] * (gv.viewq[k][j] >> 1);
                gv.t[j] = (int32_t)(acc >> 14);
            }
        }
    }
    /* LIVE-PATH focal length -- now the HARDWARE value, not an empirical one.
     *
     * MAME's viewport record carries zoom mantissa 17736 with exponent
     * 0x29, i.e. 17736 >> 5 = 554.25. That is exactly 320/tan(30deg): a
     * 60 degree horizontal FOV. --framedump has always used it, read from
     * the record itself.
     *
     * This used to be 1920 (the 0x780 that dsp_viewport_setup writes),
     * described in-code as necessary because "554 collapses the scene to a
     * few pixels". That is not what happens -- measured, 554.25 simply
     * opens the view up from a ~19 degree telephoto crop to the real 60
     * degree frame, which is what produced the huge near-geometry wedges
     * across the live view. 1920 is the CPU-side viewport word, not the
     * focal length the hardware projects with.
     *
     * PROPCYCL_LIVE_ZOOM=<mant>[,<shift>] still overrides, for comparing. */
    /* ABSOLUTE PRIORITY -- the reason the flyover drew only sky (row 23).
     *
     * geo_hw folds priority into the sort key as `zsort |= (ap & 7) << 21`,
     * where `ap = (view.absolute_priority + polyshift_bits) & 7`. The live
     * path left absolute_priority at 0, so:
     *
     *            depth     view_ap  polyshift  ap   zsort      painted
     *   sky 108  104104    0        0          0    104104     LAST  <- on top
     *   bike 113  43510    0        7          7    14723574   first
     *
     * i.e. the sky sorted NEAREST and painted over the whole screen, hiding
     * the bike and rider. The depths were already right (the recording has
     * 104386 and 42869 for the same two objects, within 1%) -- only the
     * priority was missing.
     *
     * Measured from the recording with PROPCYCL_ZORD=1: every object in the
     * flyover carries view_ap = 7, and in the demo 177 of 210 do (the
     * exceptions are the cockpit overlay group 679-708 and sky dome 871,
     * which the master puts at 6). With 7 the same two objects become:
     *
     *   sky 108  ap = 7+0 = 7  -> zsort 14784232  -> painted FIRST (behind)
     *   bike 113 ap = 7+7 = 14 & 7 = 6 -> 12626422 -> painted after (on top)
     *
     * which reproduces the recording's ordering exactly.
     *
     * Like the zoom/ambient/light constants above this is a MEASURED stand-in
     * for a field the master DSP computes; when that stage lands it comes
     * from the record. PROPCYCL_LIVE_AP=<n> overrides for A/B. */
    gv.absolute_priority = 7;
    if (plain_viewport()) gv.absolute_priority = 7 - cur_viewport;  /* ROM 0x022E42 */
    { const char *e = getenv("PROPCYCL_LIVE_AP"); if (e) gv.absolute_priority = atoi(e); }

    gv.zoom_mant = 17736; gv.zoom_shift = 5;
    if (hud_screen_space(code) && !ending_vp()) {
        /* THE HUD HAS ITS OWN FOCAL LENGTH, and it is measured, not tuned.
         * Every gameplay dump carries TWO 0x15 viewport records:
         *   vp0 @0x0304  zoom word 0x286666  mant 26214 shift 6 -> 409.59
         *   vp1 @0x066F  zoom word 0x294548  mant 17736 shift 5 -> 554.25
         * The world is drawn at 554.25 and the cockpit group at 409.59.
         * At 554.25 a score digit at (314,243,544) projects to (640,-8) --
         * exactly off the top-right corner, which is where ours sat with
         * the view fix alone; at 409.59 it lands at (556,57), inside the
         * POINT gauge. */
        gv.zoom_mant = 26214; gv.zoom_shift = 6;
        /* And the HUD's own absolute priority. Measured off the same
         * capture with PROPCYCL_ZORD: MAME's HUD records carry ap=5
         * (view_ap 6) where the live stand-in gives everything 7. */
        gv.absolute_priority = 6;
        /* FLAT, FULLY-LIT for the cockpit group. Its quads face the camera,
         * so the world's light vector (0, 23170, -23170) dots to nearly
         * zero across them and the reels came out dark brown where MAME's
         * are bright gold. 64 is the neutral brightness (out = c*bri/64),
         * the same constant the model-viewer path uses for the same
         * reason. The HUD viewport record differs from the world's in its
         * flags word too (0xBB0003 against 0x3B0003) while carrying the
         * same ambient/power, which is consistent with a lighting-mode
         * bit -- another field that comes from the record once the master
         * port lands. */
        if (g_hud_flat) {
            gv.ambient = 64; gv.power = 0;
            gv.light[0] = gv.light[1] = gv.light[2] = 0;
        }
        /* ...but MAME's two viewport records carry IDENTICAL lighting
         * (ambient 0x14, power 0xC8, light (0, 0x5A82, 0xA57E) in words
         * 1..4 of both), so the HUD is lit like the world, with an identity
         * matrix. The flat stand-in above predates the identity-view fix
         * (row 69) that made the normals right; it is now off by default
         * and PROPCYCL_HUD_FLAT=1 brings it back for A/B. */
    }
    if (cur_viewport == 2) lens_viewport(&gv);   /* the name-entry magnifier */
    /* PROPCYCL_LIVE_ZOOM=<mant>[,<shift>]: test the focal length against
     * MAME's viewport record, which gives 17736 >> 5 = 554.25 -- exactly
     * 320/tan(30deg), a 60 degree horizontal FOV. 1920 is a ~19 degree
     * telephoto: it magnifies everything and is a candidate cause of the
     * huge near-geometry wedges across the live view. */
    /* OBJECT MATRIX IS 2.14, NOT Q15.
     *
     * The master's emitted primitive records carry the object matrix in
     * 2.14 fixed point -- 0x4000 = 1.0 (pc_master_model.py: "M = identity
     * 2.14 (0x4000 diag)"). geo_hw consumes it as Q15, so a record's
     * identity comes out as 0.5 and the hardware draws objects at that
     * scale; the frame gate passes 1323/1323 that way, so it is correct.
     *
     * The live path builds its matrix from Q15 trig (0x7FFF = 1.0) and so
     * was drawing EVERYTHING AT TWICE THE SIZE. The translation is not
     * affected (gv.t is already computed above), so this is a pure object
     * scale, which is exactly what was wrong.
     *
     * Measured, object 141 (a bike part) at the same |t| (~8800) and the
     * same focal (554.25):
     *     recording  screen w = 48
     *     live Q15   screen w = 99   <- 2x
     *     live 2.14  screen w = 48   <- matches
     *
     * PROPCYCL_LIVE_MSCALE overrides for A/B. */
    { int r, c; int nmat_done = 0;
      /* THE 0x8008 SCALE RECORD's diagonal, applied here so it composes with
       * the object rotation and not with the translation (gv.t is already
       * derived from gv.viewq above). 0x7FFF is the identity. */
      if (g_obj_nmat_valid && !g_nmat_compose) {
          /* THE 13-WORD 0x8008 IS THE OBJECT MATRIX ITSELF.
           *
           * Ground truth is the master's OWN OUTPUT. `dumps/gameplay`'s 0x0d
           * primitive record for code 144 (the shadow) carries
           *
           *     M = [[349, -734, -962],     <- the CPU's three OFFSETS
           *          [  0,    0,    0 ],    <- ZERO
           *          [959,  -36,  347 ]]    <- the CPU's three SLOPES
           *
           * and `hud_draw_wing_element` (ROM 0x015BB2, disassembled) writes
           * its nine words as three triples (offset_i, 0, slope_i), i.e. the
           * zeros land at words 1/4/7. Those become a zero ROW only when the
           * words are read COLUMN-MAJOR -- that is what picks the layout, not
           * a bbox fit.
           *
           * Equally important: M is the record's matrix UNCHANGED. The master
           * does not compose it with the preceding 11-word 0x8008's rotation,
           * so this REPLACES the object rotation rather than multiplying onto
           * it. The view is then applied exactly as the gate-exact framedump
           * path does it (framedump.c: `gv.m = m . viewq >> 15`).
           *
           * No Q15->2.14 halving afterwards: these words are already in the
           * master's output scale, which is what the frame gates consume.
           *
           * PROPCYCL_NMAT_COMPOSE=1 restores multiplying onto gv.m,
           * PROPCYCL_NO_NMAT=1 the original scalar reading. */
          int32_t N[3][3], out[3][3]; int k;
          for (r = 0; r < 3; r++) for (c = 0; c < 3; c++)
              N[r][c] = (g_nmat_order & 2) ? g_obj_nmat[c * 3 + r]
                                           : g_obj_nmat[r * 3 + c];
          for (r = 0; r < 3; r++) for (c = 0; c < 3; c++) {
              int64_t acc = 0;
              for (k = 0; k < 3; k++) acc += (int64_t)N[r][k] * gv.viewq[k][c];
              out[r][c] = (int32_t)(acc >> 15);
          }
          for (r = 0; r < 3; r++) for (c = 0; c < 3; c++) gv.m[r][c] = out[r][c];
          /* MEASURED: the halving is still required. Skipping it put the
           * shadow at exactly 2x MAME's width (274 against 136) -- the same
           * Q15 -> 2.14 factor the register records for every other object.
           * PROPCYCL_NMAT_NOHALF=1 skips it, for A/B. */
          nmat_done = g_nmat_nohalf;
      }
      else if (g_obj_nmat_valid) {
          int32_t N[3][3], out[3][3]; int k;
          for (r = 0; r < 3; r++) for (c = 0; c < 3; c++)
              N[r][c] = (g_nmat_order & 2) ? g_obj_nmat[c * 3 + r]
                                           : g_obj_nmat[r * 3 + c];
          for (r = 0; r < 3; r++) for (c = 0; c < 3; c++) {
              int64_t acc = 0;
              for (k = 0; k < 3; k++)
                  acc += (g_nmat_order & 1) ? (int64_t)gv.m[r][k] * N[k][c]
                                            : (int64_t)N[r][k] * gv.m[k][c];
              out[r][c] = (int32_t)(acc >> 15);
          }
          for (r = 0; r < 3; r++) for (c = 0; c < 3; c++) gv.m[r][c] = out[r][c];
      }
      else if (g_obj_scale != 0x7FFF)
          for (r = 0; r < 3; r++) for (c = 0; c < 3; c++)
              gv.m[r][c] = (int32_t)(((int64_t)gv.m[r][c] * g_obj_scale) >> 15);
      if (!nmat_done)
          for (r = 0; r < 3; r++) for (c = 0; c < 3; c++)
              gv.m[r][c] >>= 1;                  /* Q15 -> 2.14 */
      { static int msdone; static double ms; static int mshave;
        if (!msdone) { msdone = 1; const char *e = getenv("PROPCYCL_LIVE_MSCALE");
                       if (e) { ms = atof(e); mshave = 1; } }
        if (mshave) for (r = 0; r < 3; r++) for (c = 0; c < 3; c++)
            gv.m[r][c] = (int32_t)(gv.m[r][c] * 2.0 * ms); } }
    /* THE FOCAL LENGTH COMES FROM THE VIEWPORT'S OWN BLOCK. The CPU keeps one
     * 0x80-byte block per viewport below each command list (dsp_w32 in
     * propcycl.h) whose zoom word at +0x38 is the HALF FIELD OF VIEW in 1/64
     * degree; the master turns it into the 0x15 record's focal length
     * 320 / tan(word/64 deg), a mantissa normalised into [0x4000, 0x8000).
     * Measured against MAME's records: 0x780 (30 deg) -> 17736>>5 = 554.25,
     * the world; 0x980 (38 deg) -> 26214>>6 = 409.59, the HUD -- the two
     * constants above -- and the story intermission's animated camera, which
     * the 0x7FFE node sets to a 22.5 deg word, -> 24722>>5 = 772.6, which the
     * machine's own record for that frame carries exactly. With the constants
     * alone every cut-scene shot was drawn wide-angle. A block that holds no
     * plausible word (never initialised) keeps the constant.
     * PROPCYCL_VP_ZOOM_LEGACY=1 restores the constants for A/B. */
    /* THE ABSOLUTE PRIORITY IS PER VIEWPORT TOO, in the same block: word
     * +0x50, which dsp_param_init sets to 7 - viewport (ROM 0x022E42
     * `moveq #7,d1 ; sub.l d3,d1`, its only writer in the whole ROM). The
     * two constants above are exactly its values for viewports 0 and 1 --
     * but the STAGE SELECT draws its plates under viewport 3 (priority 4)
     * and the SELECTED plate under viewport 4 (priority 3), which the
     * constants gave 6 and 7: the lifted plate sorted BEHIND its own brown
     * backing (model 0x358), a solid square that followed the cursor.
     * PROPCYCL_VP_AP_LEGACY=1 restores the constants for A/B. */
    { extern int g_vp_ap_legacy;
      if (!g_vp_ap_legacy && cmd_buf_base >= 0x400 && cur_viewport >= 0 && cur_viewport < 8) {
          uint32_t b = cmd_buf_base - 0x400 + (uint32_t)cur_viewport * 0x80;
          int32_t a = dsp_r32(b + 0x50), zw = dsp_r32(b + 0x38);
          /* only from an initialised block (the zoom word is in range) */
          if (a >= 0 && a <= 7 && zw >= 0x100 && zw <= 0x1F00) gv.absolute_priority = a;
      } }
    { extern int g_vp_zoom_legacy;
      if (!g_vp_zoom_legacy && cmd_buf_base >= 0x400 && cur_viewport >= 0 && cur_viewport < 8) {
          int32_t w = dsp_r32(cmd_buf_base - 0x400 + (uint32_t)cur_viewport * 0x80 + 0x38);
          { extern int g_stagedbg; static unsigned zlast;
            if (g_stagedbg && (int32_t)_W[0x0CC0] == 27 && g_sys.frame_count != zlast && g_sys.frame_count % 15 == 0) {
                zlast = g_sys.frame_count;
                printf("[ZOOM] f%u vp=%d base=%05X word=%04X (%s) blk0=%04X blk1=%04X CA0=%ld dsp10=%d CAC=%ld\n", g_sys.frame_count, cur_viewport,
                       (unsigned)cmd_buf_base, (unsigned)w, (w >= 0x100 && w <= 0x1F00) ? "used" : "IGNORED",
                       (unsigned)dsp_r32(0x10038), (unsigned)dsp_r32(0x18038), (long)_W[0x0CA0],
                       (int)g_sys.dspram[0x10], (long)_W[0x0CAC]);
            } }
          if (w >= 0x100 && w <= 0x1F00) {
              double z = 320.0 / tan((double)w / 64.0 * M_PI / 180.0);
              int sh = 0;
              while (sh < 15 && z * (double)(1 << (sh + 1)) < 32768.0) sh++;
              gv.zoom_mant = (int32_t)lround(z * (double)(1 << sh));
              gv.zoom_shift = sh;
          }
      } }
    { static int zdone, zm, zsh;
      if (!zdone) { zdone = 1; const char *e = getenv("PROPCYCL_LIVE_ZOOM");
                    if (e) sscanf(e, "%d,%d", &zm, &zsh); }
      if (zm) { gv.zoom_mant = zm; gv.zoom_shift = zsh; } }
    gv.vx = 0; gv.vy = 0;
    if (ending_vp() && cur_viewport == 3) ending_vp3_params(&gv);
    /* DIAGNOSTIC ONLY (PROPCYCL_GEO_CULLFLIP=1): if inverting the cull
     * sense rescues the missing quads, the view matrix handedness is
     * wrong and THAT is what to fix -- flipping this flag would only
     * paper over a sign error that also affects the projection. */
    { const char *e = getenv("PROPCYCL_GEO_CULLFLIP"); gv.cullflip = (e && *e != '0'); }
    /* Distance census: |t| per placement, so the live scale can be compared
     * directly against the recording's records (PROPCYCL_VERBOSE). */
    { extern void geohw_note_dist2(double,int);
      extern void blinklog_note(int);
      double dx=gv.t[0], dy=gv.t[1], dz=gv.t[2];
      geohw_note_dist2((dx*dx+dy*dy+dz*dz), code);
      /* NOTE THE RECORD CODE, NOT THE CPU MODEL ID. blinklog_quad() counts
       * quads under `code + 0x45` (that is what geo_hw_object is handed and
       * what g_bbox_cur carries), so noting the presence under the raw id
       * put the two halves of the CULLED/PARTIAL test on different objects
       * and neither could ever fire correctly -- the few that did were
       * coincidences where some other placement's id aliased. */
      blinklog_note(code + 0x45);
      /* Placement census line -- see dist_dump_open() for the format.
       * PROPCYCL_DIST_MAT=1 appends the composed matrix (view*object, the
       * same thing a MAME GAMEPLAY record carries -- in gameplay the 0x0a
       * view record is identity and the master folds the view into every
       * object matrix). That makes yaw(m) directly comparable between the
       * two sides, which is what tools/overnight/camera_lag_gate.py needs
       * for row 18. Appended, so existing P-line parsers are unaffected. */
      if (dist_dump_active()) {
          static int matdone, wantmat;
          if (!matdone) { matdone = 1; const char *e = getenv("PROPCYCL_DIST_MAT");
                          wantmat = (e && *e != '0'); }
          if (wantmat)
              fprintf(g_dist_fp, "P %d %d %d %d %d %d %d %d %d %d %d %d %d\n", code,
                      gv.t[0], gv.t[1], gv.t[2],
                      gv.m[0][0], gv.m[0][1], gv.m[0][2],
                      gv.m[1][0], gv.m[1][1], gv.m[1][2],
                      gv.m[2][0], gv.m[2][1], gv.m[2][2]);
          else
              fprintf(g_dist_fp, "P %d %d %d %d\n", code, gv.t[0], gv.t[1], gv.t[2]);
      } }
    rigview_orbit(&gv, code + 0x45);       /* rig viewer only; no-op otherwise */
    pausecam_apply(&gv, code + 0x45);      /* pause camera; no-op unless paused */
    geo_hw_set_view(&gv);
    /* The point-ROM object code is the CPU list's model id PLUS 0x45.
     * pc_master_model.py documents the emitted record as
     * "placement -> code = model + 0x45", and MAME notes references below
     * 0x45 are special commands rather than objects. Passing the raw id
     * made model 39 (0x27) walk to nothing: emitted 0, cull 0, which is
     * why the placement rotation appeared to have no effect at all. */
    { extern int g_cmd_dump; extern int cur_priority_pub;
      extern int g_cmd_word;
      extern int g_cmd_mode;
      if (g_cmd_dump) printf("[CMD] render code=%d prio=%d rot=%s mode=0x%04x @word %d\n",
                             code + 0x45, cur_priority_pub, rot ? "y" : "n", g_cmd_mode, g_cmd_word); }
    { int e0 = g_geo_stats.emitted, c0 = g_geo_stats.rej_cull;
      int b0 = g_geo_stats.rej_behind, k0 = g_geo_stats.part_clip;
      g_bbox_cur = code + 0x45;
      const int hud_nb0 = geohw_nbuf;
      geo_hw_object(code + 0x45, geohw_quad_cb, NULL);
      /* Widescreen HUD to the corners (hud_corners_shift): decided per OBJECT
       * from where its polygons actually landed -- not from its origin, which
       * for several housing parts is the screen centre with the geometry off
       * to one side (that tore the gauges apart). An object straddling the
       * centre line (the pendulum pole) and the arrow plate hanging from it
       * (code 871) stay; everything else moves with its side as one piece. */
      if (g_scene_x0 < 0.0f && code + 0x45 != 871 && geohw_nbuf > hud_nb0 &&
          hud_screen_space(code)) {
          int32_t x0 = INT32_MAX, x1 = INT32_MIN;
          for (int qi = hud_nb0; qi < geohw_nbuf; qi++)
              for (int k = 0; k < geohw_buf[qi].nrv; k++) {
                  int32_t x = geohw_buf[qi].rv[k].sx16;
                  if (x < x0) x0 = x;  if (x > x1) x1 = x;
              }
          const int32_t mid = (SCREEN_WIDTH / 2) * 16;
          const int straddle = (x0 < mid && x1 > mid);
          /* An object that does not cross the centre moves as one piece. One
           * that does -- the crossbar carrying BOTH gauges' inner knobs and
           * red buttons plus the pendulum mount -- is split per polygon:
           * left of centre goes left, right goes right, and only what is
           * within 40 px of the centre line (the pole and its bob) stays. */
          int whole = straddle ? 0
                    : hud_corners_shift((x0 + x1) / 32.0 - SCREEN_WIDTH / 2.0) * 16;
          const int E16 = hud_corners_shift(1.0e9) * 16;   /* 0 unless active */
          if (whole || (straddle && E16))
              for (int qi = hud_nb0; qi < geohw_nbuf; qi++) {
                  geo_quad *hq = &geohw_buf[qi];
                  int dx = whole;
                  if (straddle) {
                      int64_t cxs = 0;
                      for (int k = 0; k < hq->nrv; k++) cxs += hq->rv[k].sx16;
                      double off = (hq->nrv ? (double)cxs / hq->nrv : mid) / 16.0 - SCREEN_WIDTH / 2.0;
                      dx = off < -40.0 ? -E16 : off > 40.0 ? E16 : 0;
                  }
                  if (!dx) continue;
                  for (int k = 0; k < 4; k++)       hq->v[k].sx16  += dx;
                  for (int k = 0; k < hq->nrv; k++) hq->rv[k].sx16 += dx;
                  for (int k = 0; k < hq->ndv; k++) hq->dv[k].sx16 += dx;
              }
      }
      /* WHY did it lose geometry? The blinklog can say an object drew
       * nothing, or lost most of its quads, but "emitted 0 quads" has three
       * completely different causes with three completely different fixes:
       * wholly behind the camera, backface-culled, or lost in the near-plane
       * clip. Attribute the frame's rejects to THIS object while its counters
       * are still local. */
      { extern void blinklog_reason(int, int, int, int);
        blinklog_reason(code + 0x45,
                        g_geo_stats.rej_behind - b0,
                        g_geo_stats.rej_cull   - c0,
                        g_geo_stats.part_clip  - k0); }
      g_bbox_cur = -1;
      static int pd = 0;
      if (pd < 8) { pd++;
          if (propcycl_verbose()) printf("  [MODEL] id=%d rot=%s emitted+=%d cull+=%d\n", code,
                 rot ? "yes" : "no",
                 g_geo_stats.emitted - e0, g_geo_stats.rej_cull - c0); } }
}


/* PROPCYCL_FINDCODE=<cpu model>: the first 12 times that model is drawn, print
 * the list mode and the words around its entry -- the neighbouring entries
 * name the emitter, which a code census cannot. */
int g_vp_zoom_legacy;   /* PROPCYCL_VP_ZOOM_LEGACY, main.c */
int g_vp_ap_legacy;     /* PROPCYCL_VP_AP_LEGACY, main.c */
int g_findcode = -1;    /* PROPCYCL_FINDCODE, main.c */
static void findcode_dump(int model_id, int index, int mode)
{
    static int n;
    int want = g_findcode;
    if (want < 0 || model_id != want || n >= 12) return;
    n++;
    fprintf(stderr, "[FIND] f%u model %d at word %d mode %04X course %ld:",
            (unsigned)g_sys.frame_count, model_id, index, mode, (long)_W[0x0E0C]);
    for (int k = index - 16; k < index + 12; k++)
        fprintf(stderr, "%s%d", k == index ? " [" : " ", (int)cmdram_read32(k));
    fputc('\n', stderr);
}
static void render_object_hw(int code, float px, float py, float pz)
{ render_object_hw_rot(code, px, py, pz, NULL); }

static void render_object(int code, float pos_x, float pos_y, float pos_z) {
    if (code <= 0 || code >= (int)g_pointrom_count)
        return;
    if (g_frame_poly_count >= MAX_FRAME_POLYS)
        return;

    /* SLICE 8B model-usage tracker: when PROPCYCL_TRACK_MODELS=<path> is
     * set, append each unique (state, sub_state, model_id) tuple to that
     * file. Used by tools/probe_menu_states.sh to map catalog meshes to
     * the menu/attract/title states they appear in. */
    {
        static FILE *track_fp = NULL;
        static int track_init = 0;
        static uint8_t seen[16 * 64 * 4096] = {0}; /* state(4) × sub(6) × model_id(12) */
        if (!track_init) {
            track_init = 1;
            const char *p = getenv("PROPCYCL_TRACK_MODELS");
            if (p) track_fp = fopen(p, "w");
            if (track_fp) {
                fprintf(track_fp, "frame\tstate\tsub_state\tmodel_id\tx\ty\tz\n");
                fflush(track_fp);
            }
        }
        if (track_fp && code > 0 && code < 4096) {
            extern intptr_t _W[];
            int state = (int)_W[0x0CBC] & 0xF;
            int sub_state = (int)_W[0x0CC0] & 0x3F;
            int sub_title = (int)_W[0x3FB4] & 0x3F;
            (void)sub_state;
            int key = state * 64 * 4096 + sub_title * 4096 + code;
            if (!seen[key]) {
                seen[key] = 1;
                fprintf(track_fp, "%u\t%d\t%d\t0x%X\t%.0f\t%.0f\t%.0f\n",
                        g_sys.frame_count, state, sub_title, code,
                        pos_x, pos_y, pos_z);
                fflush(track_fp);
            }
        }
    }

    int addr1 = point_read(code);
    if (addr1 < 0 || addr1 >= (int)g_pointrom_count)
        return;

    int total_polys = 0;
    static int obj_dbg = 0;

    (void)obj_dbg;

    /* Walk object list (terminated by -1) */
    for (int obj = 0; obj < 100; obj++) {  /* cap at 100 sub-objects */
        if (g_frame_poly_count >= MAX_FRAME_POLYS) break;
        if (addr1 + obj >= (int)g_pointrom_count) break;
        int addr2 = point_read(addr1 + obj);
        if (addr2 < 0) break;
        if (addr2 >= (int)g_pointrom_count) break;

        /* word[0] = chunkLength: total data words for all packets */
        int chunk_len = point_read(addr2);
        if (chunk_len <= 0 || chunk_len > 2000) continue;

        int pos = addr2 + 1;      /* skip chunkLength */
        int finish = pos + chunk_len;

        /* Walk packets within chunk */
        while (pos < finish && pos + 1 < (int)g_pointrom_count && total_polys < 2000) {
            int pkt_len = point_read(pos);
            pos++;  /* advance past packetLength; base = first data word */
            int base = pos;

            if (pkt_len <= 0 || pkt_len > 0x20) break;
            if (base + pkt_len > (int)g_pointrom_count) break;

            /* Only process quads: need UV (8 words) + XYZ (12 words) + header (4) = 24 min */
            if (pkt_len >= 0x14) {
                /* Extract palette group from texture info word[2] */
                int tex0 = point_read(base + 2) & 0xFFFFFF;
                int pal_group = (tex0 >> 8) & 0x7F;

                /* Extract texture bank from upper bits of first vertex V */
                int texbank = (point_read(base + 5) >> 12) & 0xF;

                /* Extract color-depth mode (cmode) from upper bits of first U.
                 * 0/1/8/9 = 8bpp passthrough, 2/3/10/11 = 4bpp, 4-7/12-15 = 2bpp.
                 * Per-quad; controls how each tile fetch byte is split into
                 * sub-pens by bake_quad_texture. */
                int cmode = (point_read(base + 4) >> 12) & 0xF;

                /* Extract UV coordinates (4 vertices at offsets [4..11]) */
                int uv[4][2];
                for (int v = 0; v < 4; v++) {
                    uv[v][0] = point_read(base + 4 + v * 2) & 0xFFF;
                    uv[v][1] = point_read(base + 5 + v * 2) & 0xFFF;
                }

                /* Extract XYZ coordinates (4 vertices at offsets [12..23]) */
                float verts[4][3];
                float max_coord = 0;
                for (int v = 0; v < 4; v++) {
                    int vi = base + 12 + v * 3;
                    float vx = (float)point_read(vi);
                    float vy = (float)point_read(vi + 1);
                    float vz = (float)point_read(vi + 2);
                    verts[v][0] = vx + pos_x;
                    verts[v][1] = vy + pos_y;
                    verts[v][2] = vz + pos_z;
                    if (fabsf(vx) > max_coord) max_coord = fabsf(vx);
                    if (fabsf(vy) > max_coord) max_coord = fabsf(vy);
                    if (fabsf(vz) > max_coord) max_coord = fabsf(vz);
                }

                /* Skip quads with degenerate/garbage vertices (matches Python exporter) */
                if (max_coord < 1 || max_coord > 500000) {
                    pos += pkt_len;
                    continue;
                }

                /* Compute face normal for basic lighting */
                float ax = verts[1][0] - verts[0][0];
                float ay = verts[1][1] - verts[0][1];
                float az = verts[1][2] - verts[0][2];
                float bx = verts[2][0] - verts[0][0];
                float by = verts[2][1] - verts[0][1];
                float bz = verts[2][2] - verts[0][2];
                float nx = ay * bz - az * by;
                float ny = az * bx - ax * bz;
                float nz = ax * by - ay * bx;
                float nlen = sqrtf(nx*nx + ny*ny + nz*nz);
                if (nlen > 0.0f) { nx /= nlen; ny /= nlen; nz /= nlen; }
                float ndotl = nx * 0.35f + ny * 0.55f + nz * 0.5f;
                float light = 0.35f + 0.65f * fabsf(ndotl);

                if (palette_initialized()) {
                    /* Per-quad baked-texture rendering — samples the namcos22
                     * tilemap pipeline through bake_quad_texture (cached in
                     * renderer_texture.c) and binds the resulting GL texture.
                     * This is the same pattern course_viewer.c uses; keeps
                     * full poster art for layer-2 billboards (menus, attract
                     * logos, stage signs, instruction panels). */
                    int u_min = uv[0][0], u_max = uv[0][0];
                    int v_min = uv[0][1], v_max = uv[0][1];
                    for (int i = 1; i < 4; i++) {
                        if (uv[i][0] < u_min) u_min = uv[i][0];
                        if (uv[i][0] > u_max) u_max = uv[i][0];
                        if (uv[i][1] < v_min) v_min = uv[i][1];
                        if (uv[i][1] > v_max) v_max = uv[i][1];
                    }
                    int ru = u_max - u_min; if (ru < 1) ru = 1;
                    int rv = v_max - v_min; if (rv < 1) rv = 1;

                    /* bake_quad_texture mirrors the same degenerate-UV
                     * expansion (range < 16 → centered 16). Recreate the
                     * adjusted bbox locally so the texture coords map onto
                     * the same baked rectangle. */
                    int bu = u_min, bv = v_min, bru = ru, brv = rv;
                    if (bru < 16) { bu = (u_min + u_max) / 2 - 8; bru = 16; }
                    if (brv < 16) { bv = (v_min + v_max) / 2 - 8; brv = 16; }
                    if (bu < 0) bu = 0;
                    if (bv < 0) bv = 0;

                    float qsu = 1.0f, qsv = 1.0f;
                    GLuint qtex = bake_quad_texture(u_min, v_min, ru, rv,
                                                    texbank, pal_group, cmode,
                                                    &qsu, &qsv);

                    glEnable(GL_TEXTURE_2D);
                    glBindTexture(GL_TEXTURE_2D, qtex);
                    glColor3f(light, light, light);  /* GL_MODULATE tints by light */
                    glBegin(GL_QUADS);
                    for (int v = 0; v < 4; v++) {
                        /* The engine's modelview applies `glScalef(1, -1, -1)`
                         * to convert the game's left-handed Y-down coords to
                         * GL right-handed Y-up. That flips screen-Y, so a
                         * poster's "bottom" (high game-Y) ends up rendering
                         * at the TOP of the screen. We compensate by flipping
                         * V in the texture coord so the texture reads upright
                         * on screen. U is left as-is (X axis isn't scaled). */
                        float tu = (float)(uv[v][0] - bu) / (float)bru * qsu;
                        float tv = (1.0f - (float)(uv[v][1] - bv) / (float)brv) * qsv;
                        glTexCoord2f(tu, tv);
                        glVertex3f(verts[v][0], verts[v][1], verts[v][2]);
                    }
                    glEnd();
                    glDisable(GL_TEXTURE_2D);
                } else {
                    /* Fallback: grayscale flat shading until palette is loaded */
                    int cu = (uv[0][0] + uv[1][0] + uv[2][0] + uv[3][0]) / 4;
                    int cv = (uv[0][1] + uv[1][1] + uv[2][1] + uv[3][1]) / 4;
                    uint8_t pen = texture_pen_lookup(cu, cv, texbank);
                    float shade = pen_to_brightness(pen) * light;
                    if (pen == 0) shade = 0.12f;
                    glColor3f(shade, shade, shade);
                    glBegin(GL_QUADS);
                    for (int v = 0; v < 4; v++)
                        glVertex3f(verts[v][0], verts[v][1], verts[v][2]);
                    glEnd();
                }

                total_polys++;
                g_frame_poly_count++;
            }

            pos += pkt_len;
        }
    }

    (void)total_polys;
}

/* ========== DSP Command Processing ========== */

/*
 * Process DSP commands from the double-buffered command area.
 *
 * The command stream uses a mode-based protocol:
 *
 * MODE HEADERS (2 words each — set current object placement mode):
 *   0x8000 [priority]  — Object mode: data entries are [model_id, x, y, z] (4 words)
 *                         Used for terrain chunks and simple objects.
 *   0x8002 [priority]  — Rotated object mode header (when word[2] >= 0x8000)
 *
 * INLINE COMMANDS (fixed size):
 *   0x8002 (13 words)  — Place object with rotation (when word[2] < 0x8000 = model_id)
 *     [0]=0x8002 [1]=priority [2]=model_id [3..5]=position
 *     [6..11]=sin/cos pairs (x,y,z rotation) [12]=flags
 *   0x8008 (15 words)  — Set transform context
 *     [0]=0x8008 [1]=priority [2]=mode [3..8]=3x2 rotation (sin/cos)
 *     [9]=scale [10]=0 [11..13]=position [14]=sentinel
 *   0x8009 (4 words)   — Object render parameters
 *     [0]=0x8009 [1]=priority [2]=cz_adjust [3]=cz_type
 *   0x800a (6 words)   — Render model (uses current transform from 0x8008)
 *     [0]=0x800a [1]=model_id [2]=cz_type [3..5]=position
 *   0x8010 (2-4 words) — End frame/list
 *     [0]=0x8010 [1]=-1  (end frame)
 *     [0]=0x8010 [1]=3 [2]=next [3]=-1  (end list segment)
 *
 * DATA ENTRIES (in current mode, word < 0x8000):
 *   Mode 0x8000: [model_id, x, y, z] — 4 words per entry
 *   Mode 0x8002: [model_id, x, y, z, sx, cx, sy, cy, sz, cz, flags] — 11 words per entry
 */
static void process_pdp_commands(void) {
    int index = 0;
    int max_words = 0x8000 / 4;
    /* Stop at the CPU's own write cursor -- see cmd_buf_words. */
    { extern int g_no_cursor_bound, g_no_emptylist;
      int lo = g_no_emptylist ? 1 : 0;          /* 0 is a real length */
      if (!g_no_cursor_bound && cmd_buf_words >= lo && cmd_buf_words < max_words)
          max_words = cmd_buf_words;
      if (g_pdp_words) max_words = g_pdp_nwords; }
    int cmds_found = 0;
    int models_rendered = 0;
    int data_entries = 0;
    int current_mode = 0x8000;  cur_hdr = 0x8000;  /* default mode */
    cur_viewport = 0;           /* the world, until a header says otherwise */
    /* PER FRAME, like cur_viewport and the transform slot table. The oracle
     * clears objectshift on the 0x15 VIEWPORT record (GeoModel.handle_bb0003),
     * which is the first record of every frame's list, so a frame starts
     * unbiased. Latching it across frames instead would leak the last
     * object's bias -- the signpost's -8960, or building 970's +10240 --
     * onto the WHOLE of the next scene whenever a phase ends its list
     * without the clearing marker, which register row 24 records some
     * phases doing. That is register row 23's failure mode. */
    g_obj_shift = 0;
    g_frame_poly_count = 0;  /* reset polygon budget each frame */
    /* Cap on data entries. 200 was arbitrary and it TRUNCATED THE LIST: the
     * attract demo's terrain+scenery run past it and the HUD, appended last
     * at word ~1056, was never reached -- all ten HUD codes 693..706 were
     * dropped. The walk is now bounded by the CPU's write cursor, which is
     * the real limit, so this only needs to be large enough not to bite.
     * PROPCYCL_MAXDATA=<n> overrides for A/B. */
    int max_data = 4096;
    { extern int g_max_data; if (g_max_data > 0) max_data = g_max_data; }

    { extern int g_cmd_dump; extern int g_pr_calls;
      if (g_cmd_dump) printf("[CMD] --- frame --- (player_render calls last frame: %d)\n", g_pr_calls);
      g_pr_calls = 0; }
    { extern int g_rig_dump, g_rig_calls;
      if (g_rig_dump) { static int n;
        if (n++ % 60 == 0) printf("[RIG] scene_node_render calls last frame: %d\n", g_rig_calls); }
      g_rig_calls = 0; }
    /* The transform slot table is PER FRAME. Left alone it carried over,
     * and the six secondary roots of the rider rig -- emitted in child
     * form with parent == self -- accumulated their own offset every
     * frame (T[17].t += L, 150 frames deep = thousands of units) until
     * the placement bounds guard rejected them: 10 of 27 parts silently
     * vanished. */
    for (int si = 0; si < TSLOTS; si++) g_tslot[si].valid = 0;
    pend_nscale = -1;
    pend_nmat_valid = 0; pend_nmat_slot = -1;
    pend_valid = 0; pend_root = 0;
    while (index < max_words) {
        { extern int g_cmd_word, g_cmd_mode; g_cmd_word = index; g_cmd_mode = current_mode; }
        int32_t word = cmdram_read32(index);

        /* Terminate on 0 or -1 sentinel. Zeros in rotation data are consumed
         * as part of 11-word entries, so they don't trigger this check. */
        /* A word below 0x8000 at a record boundary is a DATA ENTRY, and its
         * length comes from current_mode -- including model id 0, which is a
         * real entry the game emits (MAME's own records carry code 0 twice at
         * f691). This used to `break` on 0 or -1, which ended the list at the
         * first model-0 entry: measured, the walk stopped at word 698 of 1303
         * with 45 of 103 models drawn, so the HUD (appended last at word
         * ~1056), the balloons, the sky dome and the START platform were all
         * unreachable. Register rows 74/78.
         *
         * Skipping such a word instead of consuming it is just as wrong: at
         * w739 the entry is `0, -40233, -23182, -3006, 32767, 0, ...` -- an
         * 11-word 0x8002 entry -- and stepping over the 0 desynchronised the
         * walk by one, which is where the junk codes 32836 (= 0x7FFF + 0x45,
         * a Q15 rotation word read as a model id) came from.
         *
         * So: no special case at all. The entry paths below already refuse to
         * DRAW a model id outside the point ROM while still consuming the
         * right number of words, and the walk is bounded by the CPU's write
         * cursor (cmd_buf_words), which is what stops it running into the
         * previous frame. */

        if (word >= 0x8000) {
            /* Command opcode */
            switch (word) {
            case 0x8000: {
                /* Mode header: [0x8000, priority] — 2 words.
                 * Data entries: [model_id, x, y, z] — 4 words each. */
                current_mode = 0x8000; cur_hdr = 0x8000;
                cur_priority = cmdram_read32(index + 1); cur_priority_pub = cur_priority;
                cur_viewport = cur_priority;
                { extern int g_cmd_dump; if (g_cmd_dump)
                    printf("[CMD] HEADER 0x8000 prio=%d @word %d\n", cur_priority, index); }
                cmds_found++;
                index += 2;
                break;
            }

            case 0x8001: {
                /* Mode header: [0x8001, priority] — 2 words.
                 * Data entries include rotation: [model_id, x, y, z, sx, cx, sy, cy, sz, cz, flags] — 11 words. */
                current_mode = 0x8001; cur_hdr = 0x8001;
                cur_priority = cmdram_read32(index + 1); cur_priority_pub = cur_priority;
                cur_viewport = cur_priority;
                { extern int g_cmd_dump; if (g_cmd_dump)
                    printf("[CMD] HEADER 0x8001 prio=%d @word %d\n", cur_priority, index); }
                cmds_found++;
                index += 2;
                break;
            }

            case 0x8002: {
                /* Could be 13-word inline command OR 2-word mode header.
                 * Disambiguate: if word[2] is a valid model_id (< 0x8000), it's inline. */
                int32_t w2 = cmdram_read32(index + 2);
                /* IMPORTANT: consuming this as a 13-word inline command must
                 * ALSO enter mode 0x8002, because that is what it actually
                 * is -- objects_render_master writes the 2-word header
                 * `0x8002, 0` and then scenery_render_all_lod() /
                 * animated_objects_render_all() append 11-word entries. Both
                 * readings eat 13 words for the FIRST entry, so the old code
                 * looked right, but it left `mode` unchanged and every
                 * following entry was parsed with the previous mode's entry
                 * size. The list desynchronised into 62 bogus copies of
                 * model 55 at (0,-187168,-1), 20 of model 54, and a phantom
                 * "mode de15" -- 16% of all placements per frame. */
                if (w2 > 0 && w2 < 0x8000 && index + 13 <= max_words) {
                    current_mode = 0x8002; cur_hdr = 0x8002;
                    cur_priority = cmdram_read32(index + 1); cur_priority_pub = cur_priority;
                    cur_viewport = cur_priority;
                    /* 13-word inline: [0x8002, pri, model, x, y, z, 6×rot, flags] */
                    int32_t model_id = w2;
                    float px = (float)cmdram_read32(index + 3);
                    float py = (float)cmdram_read32(index + 4);
                    float pz = (float)cmdram_read32(index + 5);
                    /* rot at [6..11] = three Q15 (sin,cos) pairs, flags at [12].
                     * This is the branch attract actually takes -- a buffer
                     * dump shows 8002,pri,model=0x27,x,y,z,FFFFE892,FFFF822B,...
                     * so the rotation is real here and was being discarded. */
                    int32_t rot[6];
                    for (int rk = 0; rk < 6; rk++)
                        rot[rk] = cmdram_read32(index + 6 + rk);
                    /* COS-FIRST -- see the note in the 11-word entry branch.
                     * These are the same six words either way: the ROM writes
                     * a 2-word `0x8002, pri` header and an 11-word entry, and
                     * this 13-word reading eats header+entry, so rot still
                     * lands at index+6. Read sin-first, the platform's
                     * identity X pair (32767, 0) became a 90 deg rotation
                     * about X and tipped it off the bottom of the screen. */
                    { extern int g_rot8002; if (g_rot8002)
                        for (int rk = 0; rk < 6; rk += 2) {
                            int32_t sw = rot[rk]; rot[rk] = rot[rk+1]; rot[rk+1] = sw; } }
                    if (model_id > 0 && model_id < (int)g_pointrom_count) {
                        if (geohw_enabled()) {
                            /* flags at [12] -- bit 1 picks Z.Y.X, everything
                             * else Z.X.Y; see g_8002_flagord. */
                            extern int g_8002_flagord;
                            int saved_ord = g_rot_ord_override;
                            if (g_8002_flagord)
                                g_rot_ord_override =
                                    (cmdram_read32(index + 12) & 2) ? ORD_ZYX : ORD_ZXY;
                            findcode_dump(model_id, index, 0x8002);
                            render_object_hw_rot(model_id, px, py, pz, rot);
                            g_rot_ord_override = saved_ord;
                        }
                        else render_object(model_id, px, py, pz);
                        models_rendered++;
                    }
                    cmds_found++;
                    index += 13;
                } else {
                    /* 2-word mode header */
                    current_mode = 0x8002; cur_hdr = 0x8002;
                    cur_priority = cmdram_read32(index + 1); cur_priority_pub = cur_priority;
                    cur_viewport = cur_priority;
                    cmds_found++;
                    index += 2;
                }
                break;
            }

            case 0x8008: {
                /* THE 13-WORD SCALE RECORD -- a THIRD length, and the reason
                 * two large black polygonal blobs sat beside the bike in the
                 * cinematic flyover.
                 *
                 * `scene_node_render_scaled` (game_dsp3d.c) writes a diagonal
                 * 3x3 ahead of the node's own rotation record:
                 *     [0x8008, prio, 6, s,0,0, 0,s,0, 0,0,s, -1]
                 * `[2] == 6` is the discriminator -- every other emitter writes
                 * 1 -- and the terminator sits at [0x0c], so the record is 13
                 * words. The 11-or-15 rule could not express that, consumed 15,
                 * and desynchronised the walk by two words, so the scale never
                 * reached the object.
                 *
                 * **s == 0 is the game's HIDE mechanism**, and MAME's own
                 * records prove it: at `dumps/attract_start/poly_f150.bin`
                 * codes 119 and 120 carry `m = [0,0,0, 0,0,0, 0,0,0]` with the
                 * body's translation, so every vertex collapses to one point
                 * (MAME's screen bbox for both is exactly x[36..36] y[240..240]).
                 * We drew them at full size: 88 quads spanning
                 * x[-8608291..7512070]. Measured in our own buffer at f120,
                 * words 616 and 639, both records carry s = 0. */
                if (cmdram_read32(index + 2) == 6 &&
                    index + 0x0c < max_words && cmdram_read32(index + 0x0c) == -1) {
                    pend_nscale = cmdram_read32(index + 3);
                    /* All nine, not just [3] -- see g_obj_nmat above. */
                    for (int k = 0; k < 9; k++)
                        pend_nmat[k] = cmdram_read32(index + 3 + k);
                    pend_nmat_valid = 1;
                    /* ...and it is a slot write like any other 0x8008 (see
                     * the op-list note at pend_scale). Column-major, the
                     * layout g_nmat_order measured for the shadow. */
                    { int sl = cmdram_read32(index + 1);
                      pend_nmat_slot = sl;
                      if (!g_slot_legacy && (g_asm_mode == 5 || g_asm_mode == 6) && sl >= 0 && sl < TSLOTS) {
                          for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) {
                              int32_t v = (g_nmat_order & 2) ? pend_nmat[c * 3 + r] : pend_nmat[r * 3 + c];
                              g_tslot[sl].M[r][c] = v; g_tslot[sl].P[r][c] = v; }
                          g_tslot[sl].t[0] = g_tslot[sl].t[1] = g_tslot[sl].t[2] = 0;
                          g_tslot[sl].valid = 1;
                      } }
                    cmds_found++;
                    index += 13;
                    break;
                }
                /* OP 0 FIRST: [0x8008, slot, 0, x, y, z, 1, pairs, flags, -1]
                 * -- bonus_model_dsp_setup's count-down root (ROM 0x016E26).
                 * Translate, then rotate: M = R, t = T.R. */
                if (!g_slot_legacy && (g_asm_mode == 5 || g_asm_mode == 6) &&
                    cmdram_read32(index + 2) == 0 && index + 0x0e < max_words &&
                    cmdram_read32(index + 6) == 1 && cmdram_read32(index + 0x0e) == -1) {
                    int sl = cmdram_read32(index + 1);
                    int32_t T[3] = { cmdram_read32(index + 3), cmdram_read32(index + 4), cmdram_read32(index + 5) };
                    for (int i = 0; i < 3; i++) {
                        cur_rot_q15[i*2+0] = (int16_t)(cmdram_read32(index + 7 + i * 2) & 0xFFFF);
                        cur_rot_q15[i*2+1] = (int16_t)(cmdram_read32(index + 8 + i * 2) & 0xFFFF);
                    }
                    cur_rot_valid = 1;
                    cur_priority = sl; cur_priority_pub = cur_priority;
                    placement_matrix_ord(cur_rot_q15[0], cur_rot_q15[1], cur_rot_q15[2],
                                         cur_rot_q15[3], cur_rot_q15[4], cur_rot_q15[5], pend_R,  g_euler_8008);
                    placement_matrix_ord(cur_rot_q15[0], cur_rot_q15[1], cur_rot_q15[2],
                                         cur_rot_q15[3], cur_rot_q15[4], cur_rot_q15[5], pend_RP,
                                         g_euler_p < 0 ? g_euler_8008 : g_euler_p);
                    for (int j = 0; j < 3; j++) {
                        int64_t a = 0;
                        for (int k = 0; k < 3; k++) a += (int64_t)T[k] * pend_RP[k][j];
                        pend_L[j] = (int32_t)(a >> 15);
                    }
                    pend_root = 0; pend_valid = 1;
                    cur_pos[0] = (float)pend_L[0]; cur_pos[1] = (float)pend_L[1]; cur_pos[2] = (float)pend_L[2];
                    if (sl >= 0 && sl < TSLOTS) {
                        memcpy(g_tslot[sl].M, pend_R, sizeof pend_R);
                        memcpy(g_tslot[sl].P, pend_RP, sizeof pend_RP);
                        memcpy(g_tslot[sl].t, pend_L, sizeof pend_L);
                        g_tslot[sl].valid = 1;
                    }
                    cmds_found++;
                    index += 15;
                    break;
                }
                /* Set transform context, 15 words */
                cur_priority = cmdram_read32(index + 1); cur_priority_pub = cur_priority;
                for (int i = 0; i < 3; i++) {
                    cur_rot[i][0] = dspfixed_to_float(cmdram_read32(index + 3 + i * 2));
                    cur_rot[i][1] = dspfixed_to_float(cmdram_read32(index + 4 + i * 2));
                }
                /* Keep the RAW Q15 pair so 0x800a can actually apply it.
                 * placement_matrix(s1,c1,s2,c2,s3,c3) wants sin first; the
                 * two emitters disagree on the record's order --
                 * scene_node_render writes (cos,sin) [0x20B002 then
                 * 0x20B004] while dsp_cmd_object_transform writes
                 * (sin,cos). g_rot8008 selects, so the order is measured
                 * against the recording rather than assumed. */
                for (int i = 0; i < 3; i++) {
                    /* LOW 16 BITS ONLY. On the hardware each pair word is
                     * written by a 32-bit move.l off a 2-byte-strided table,
                     * so the 24-bit DSP word carries a slice of the NEIGHBOURING
                     * entry in its top byte (MAME's capture at fc 730: 299D0F /
                     * 0F5133 -- consecutive words share a byte). The master
                     * consumes the low 16 bits. For a clean sign-extended word
                     * this is a no-op; for the packed form that
                     * player_animation_state_update writes (vrd32) it is the
                     * difference between sin and a number 400x too large. */
                    int32_t a = (int16_t)(cmdram_read32(index + 3 + i * 2) & 0xFFFF);
                    int32_t b = (int16_t)(cmdram_read32(index + 4 + i * 2) & 0xFFFF);
                    if (g_rot8008 == 2) { cur_rot_q15[i*2+0] = b; cur_rot_q15[i*2+1] = a; }
                    else                { cur_rot_q15[i*2+0] = a; cur_rot_q15[i*2+1] = b; }
                }
                cur_rot_valid = 1;
                /* RECORD LENGTH: TEST THE TERMINATOR SLOT, DO NOT SCAN FOR -1.
                 *
                 * The two forms put their -1 at a FIXED index -- 0x0a for the
                 * 11-word root (scene_node_render's root branch) and 0x0e for
                 * the 15-word child (its child branch, dsp_cmd_object_transform
                 * and camera_dsp_terrain_render). Scanning for the first -1
                 * instead misreads any record whose DATA contains -1, and the
                 * local offset is signed world coordinates, so it does.
                 *
                 * Measured case: a rig record at word 326 with local offset
                 * (x,y,z) = (-1, 872, -49). The scan stopped at index 11 (the
                 * x), declared the 11-word form, and the parser then read the
                 * y as a MODEL ID -- emitting a phantom placement of model 872
                 * (code 941, an empty point-ROM object) every frame, and
                 * resuming the walk at the wrong offset so every following
                 * record was parsed against the wrong base. */
                { int t;
                  if (index + 0x0a < max_words && cmdram_read32(index + 0x0a) == -1) t = 0x0a;
                  else t = 0x0e;
                  if (t < 14) {   /* the 11-word ROOT form: remember its rotation */
                      memcpy(root_rot_q15, cur_rot_q15, sizeof root_rot_q15);
                      root_rot_valid = 1; }
                  pend_scale = cmdram_read32(index + 9);
                  if (g_asm_mode == 5 || g_asm_mode == 6) {
                      placement_matrix_ord(cur_rot_q15[0], cur_rot_q15[1], cur_rot_q15[2],
                                           cur_rot_q15[3], cur_rot_q15[4], cur_rot_q15[5], pend_R,  g_euler_8008);
                      placement_matrix_ord(cur_rot_q15[0], cur_rot_q15[1], cur_rot_q15[2],
                                           cur_rot_q15[3], cur_rot_q15[4], cur_rot_q15[5], pend_RP,
                                           g_euler_p < 0 ? g_euler_8008 : g_euler_p);
                      if (t < 14) { pend_L[0] = pend_L[1] = pend_L[2] = 0; pend_root = 1; }
                      else { pend_L[0] = cmdram_read32(index + 11);
                             pend_L[1] = cmdram_read32(index + 12);
                             pend_L[2] = cmdram_read32(index + 13); pend_root = 0; }
                      pend_valid = 1;
                      /* the record writes its own slot (op-list note above) */
                      { int sl = cmdram_read32(index + 1);
                        if (!g_slot_legacy && sl >= 0 && sl < TSLOTS) {
                            memcpy(g_tslot[sl].M, pend_R, sizeof pend_R);
                            memcpy(g_tslot[sl].P, pend_RP, sizeof pend_RP);
                            memcpy(g_tslot[sl].t, pend_L, sizeof pend_L);
                            g_tslot[sl].valid = 1;
                        } }
                  }
                  if (g_asm_mode == 3 || g_asm_mode == 4) {
                      int32_t R[3][3];
                      placement_matrix(cur_rot_q15[0], cur_rot_q15[1], cur_rot_q15[2],
                                       cur_rot_q15[3], cur_rot_q15[4], cur_rot_q15[5], R);
                      if (t < 14) {                    /* ROOT: reset the run */
                          memcpy(acc_M, R, sizeof acc_M);
                          acc_t[0] = acc_t[1] = acc_t[2] = 0;
                      } else {                         /* CHILD: concatenate */
                          int32_t L[3] = { cmdram_read32(index + 11),
                                           cmdram_read32(index + 12),
                                           cmdram_read32(index + 13) };
                          for (int j = 0; j < 3; j++) {
                              int64_t a = 0;
                              for (int k = 0; k < 3; k++) a += (int64_t)L[k] * acc_M[k][j];
                              acc_t[j] += (int32_t)(a >> 15);
                          }
                          int32_t nm[3][3];
                          /* Row-vector convention (v . R_obj . R_view), so a
                           * child's frame is R_child . M_parent. ASM=4 keeps
                           * the other order for A/B. */
                          if (g_asm_mode == 4) q15_mul3(acc_M, R, nm);
                          else                 q15_mul3(R, acc_M, nm);
                          memcpy(acc_M, nm, sizeof acc_M);
                      }
                  } }
                /* ONLY THE 15-WORD FORM CARRIES A POSITION.
                 *
                 * scene_node_render emits two shapes and they are NOT the
                 * same record:
                 *   child (hi4 <  0): 15 words, -1 at [0x0e],
                 *                     local offset at [0x0b..0x0d]
                 *   root  (hi4 == 0): 11 words, -1 at [0x0a],
                 *                     NO position -- the following 0x800a
                 *                     carries the camera-relative one
                 *
                 * Reading [11..13] unconditionally meant that for a ROOT
                 * record we read the NEXT COMMAND as a position: word 11 is
                 * the literal opcode 0x800A = 32778, word 12 the model id,
                 * word 13 the priority. That constant then got added to
                 * every following 0x800a placement -- which is exactly the
                 * +32775 that put code 113 (the aircraft body) off-screen
                 * right and codes 119/120 at z+32774 (register row 31).
                 *
                 * `term` is the index of the -1 found by the scan below. */
                {
                    /* Same fixed-slot rule as the length test above -- scanning
                     * for the first -1 misreads any record whose local offset
                     * legitimately contains -1. */
                    int term;
                    if (index + 0x0a < max_words && cmdram_read32(index + 0x0a) == -1) term = 0x0a;
                    else term = 0x0e;
                    if (term >= 14) {
                        cur_pos[0] = (float)cmdram_read32(index + 11);
                        cur_pos[1] = (float)cmdram_read32(index + 12);
                        cur_pos[2] = (float)cmdram_read32(index + 13);
                    } else {
                        cur_pos[0] = cur_pos[1] = cur_pos[2] = 0.0f;
                    }
                }
                cmds_found++;
                /* 0x8008 IS VARIABLE LENGTH, TERMINATED BY -1.
                 *
                 * Two emitters write it with different payloads:
                 *   scene_node_render        15 words, -1 at [0x0e]
                 *   dsp_cmd_object_transform 11 words, -1 at [0x0a]
                 * Skipping a fixed 15 desynchronised the parse on the
                 * shorter form: the attract flyover's list holds
                 * `0x800a model=0x2c` (code 113) right after an 11-word
                 * 0x8008, and every one of the 25 bike/rider placements
                 * was swallowed -- the cinematic drew only the sky
                 * (mismatch-register row 13). Scan to the -1 instead. */
                /* Advance by the TERMINATOR SLOT, not by the first -1.
                 * Scanning desynchronises the whole walk whenever a record's
                 * local offset legitimately contains -1: measured on a rig
                 * record at word 326 with offset (-1, 872, -49), the scan
                 * stopped at k=11 and advanced 12 instead of 15, landing the
                 * parser on the offset's y and reading it as a MODEL ID -- a
                 * phantom placement of model 872 (point-ROM 941, empty) 327
                 * times a run, with the 0x8009 opcode consumed as its z. */
                index += (index + 0x0a < max_words &&
                          cmdram_read32(index + 0x0a) == -1) ? 11 : 15;
                break;
            }

            case 0x8009: {
                /* [0x8009, a, b, c]: T[c] = T[a] then T[b] -- see the op-list
                 * note at pend_scale. The rig's [3, parent, own] is the case
                 * the legacy branch below was fitted on. */
                if (!g_slot_legacy && (g_asm_mode == 5 || g_asm_mode == 6)) {
                    int a = cmdram_read32(index + 1), b = cmdram_read32(index + 2), c = cmdram_read32(index + 3);
                    /* A 13-word matrix drawn from its OWN slot ([4,3,3] after
                     * [0x8008, 3, 6, N] -- the player's shadow) keeps the
                     * measured replace semantics: M is N unchanged. */
                    if (pend_nmat_valid && c == pend_nmat_slot) { pend_valid = 0; cmds_found++; index += 4; break; }
                    if (a >= 0 && a < TSLOTS && b >= 0 && b < TSLOTS && c >= 0 && c < TSLOTS) {
                        int32_t XM[3][3], XP[3][3], xt[3], PM[3][3], PP[3][3], pt[3];
                        /* MEASURED rig quirk: a child whose parent lookup
                         * lands on itself hangs off SLOT 0 (the six rider
                         * sub-roots land 11-21 units from the recording that
                         * way, 463-1544 against identity). */
                        int pe = (a == 3 && b == c && b != 3) ? 0 : b;
                        if (g_tslot[a].valid) { memcpy(XM, g_tslot[a].M, sizeof XM); memcpy(XP, g_tslot[a].P, sizeof XP); memcpy(xt, g_tslot[a].t, sizeof xt); }
                        else { memset(XM, 0, sizeof XM); memset(XP, 0, sizeof XP);
                               XM[0][0] = XM[1][1] = XM[2][2] = 0x7FFF; XP[0][0] = XP[1][1] = XP[2][2] = 0x7FFF;
                               xt[0] = xt[1] = xt[2] = 0; }
                        if (g_tslot[pe].valid) { memcpy(PM, g_tslot[pe].M, sizeof PM); memcpy(PP, g_tslot[pe].P, sizeof PP); memcpy(pt, g_tslot[pe].t, sizeof pt); }
                        else { memset(PM, 0, sizeof PM); memset(PP, 0, sizeof PP);
                               PM[0][0] = PM[1][1] = PM[2][2] = 0x7FFF; PP[0][0] = PP[1][1] = PP[2][2] = 0x7FFF;
                               pt[0] = pt[1] = pt[2] = 0; }
                        for (int j = 0; j < 3; j++) {
                            int64_t acc = 0;
                            for (int k = 0; k < 3; k++) acc += (int64_t)xt[k] * PP[k][j];
                            g_tslot[c].t[j] = pt[j] + (int32_t)(acc >> 15);
                        }
                        if (g_asm_mode == 5) { q15_mul3(XM, PM, g_tslot[c].M); q15_mul3(XP, PP, g_tslot[c].P); }
                        else                 { q15_mul3(PM, XM, g_tslot[c].M); q15_mul3(PP, XP, g_tslot[c].P); }
                        g_tslot[c].valid = 1;
                        /* a 13-word matrix composed into ANOTHER slot is used
                         * up there, not applied again to the next object */
                        if (pend_nmat_valid && (a == pend_nmat_slot || b == pend_nmat_slot)) {
                            pend_nmat_valid = 0; pend_nscale = -1; }
                    }
                    pend_valid = 0;
                    cmds_found++;
                    index += 4;
                    break;
                }
                /* [0x8009, 3, parent_slot, own_slot] after a child's 0x8008:
                 * T[own] = T[parent] o pending.  (Documented elsewhere as
                 * "cz_adjust, cz_type" -- for the articulated rig it is the
                 * parent/child slot pair; see the slot-table comment.) */
                if ((g_asm_mode == 5 || g_asm_mode == 6) && pend_valid) {
                    int ps = cmdram_read32(index + 2), os = cmdram_read32(index + 3);
                    if (ps >= 0 && ps < TSLOTS && os >= 0 && os < TSLOTS) {
                        int32_t PM[3][3]; int32_t PP[3][3]; int32_t pt[3];
                        /* ps == os is a secondary ROOT written through the
                         * child path (scene_node_render's parent lookup with
                         * hi4 == 0 lands on the node itself). MEASURED: such a
                         * node hangs off SLOT 0, the primary root -- against
                         * slot 0 the six rider sub-roots land 11-21 units from
                         * the recording; against identity 463-1544. */
                        int pe = (ps == os) ? 0 : ps;
                        if (g_tslot[pe].valid) { memcpy(PM, g_tslot[pe].M, sizeof PM); memcpy(PP, g_tslot[pe].P, sizeof PP); memcpy(pt, g_tslot[pe].t, sizeof pt); }
                        else { memset(PM, 0, sizeof PM); memset(PP, 0, sizeof PP);
                               PM[0][0] = PM[1][1] = PM[2][2] = 0x7FFF; PP[0][0] = PP[1][1] = PP[2][2] = 0x7FFF;
                               pt[0] = pt[1] = pt[2] = 0; }
                        /* t_own = t_parent + L . P_parent  (ZXY chain, row-vector) */
                        for (int j = 0; j < 3; j++) {
                            int64_t a = 0;
                            for (int k = 0; k < 3; k++) a += (int64_t)pend_L[k] * PP[k][j];
                            g_tslot[os].t[j] = pt[j] + (int32_t)(a >> 15);
                        }
                        if (g_asm_mode == 5) { q15_mul3(pend_R, PM, g_tslot[os].M); q15_mul3(pend_RP, PP, g_tslot[os].P); }
                        else                 { q15_mul3(PM, pend_R, g_tslot[os].M); q15_mul3(PP, pend_RP, g_tslot[os].P); }
                        g_tslot[os].valid = 1;
                    }
                    pend_valid = 0;
                }
                cmds_found++;
                index += 4;
                break;
            }

            case 0x800a: {
                /* Render model, 6 words — uses position from 0x8008 transform */
                int32_t model_id = cmdram_read32(index + 1);
                /* A child's cur_pos is a LOCAL offset in its parent's frame;
                 * the 0x800a carries the ROOT's camera-relative position.
                 * g_asm_mode selects how the two combine, so the rule is
                 * measured against the recording rather than assumed. */
                float off[3] = { cur_pos[0], cur_pos[1], cur_pos[2] };
                if (g_asm_mode && (cur_rot_valid || root_rot_valid)) {
                    const int32_t *rr = (g_asm_mode == 2 && root_rot_valid)
                                        ? root_rot_q15 : cur_rot_q15;
                    int32_t rm[3][3];
                    placement_matrix(rr[0], rr[1], rr[2], rr[3], rr[4], rr[5], rm);
                    int32_t v[3] = { (int32_t)off[0], (int32_t)off[1], (int32_t)off[2] };
                    for (int j = 0; j < 3; j++) {
                        int64_t acc = 0;
                        for (int k = 0; k < 3; k++) acc += (int64_t)v[k] * rm[k][j];
                        off[j] = (float)(int32_t)(acc >> 15);
                    }
                }
                if (g_asm_mode == 3 || g_asm_mode == 4) {
                    off[0] = (float)acc_t[0]; off[1] = (float)acc_t[1]; off[2] = (float)acc_t[2];
                }
                if (g_asm_mode == 5 || g_asm_mode == 6) {
                    int sl = cmdram_read32(index + 2);
                    if (sl >= 0 && sl < TSLOTS) {
                        /* A ROOT is any 0x8008 that goes STRAIGHT to 0x800a with
                         * no 0x8009 between -- in EITHER length. The 11-word form
                         * (scene_node_render, the flyover) was handled; the 15-word
                         * form was not, and MAME's own command buffer at fc 730
                         * shows the demo bike root is exactly that: a 15-word
                         * 0x8008 with L = 0, then 0x800a for slot 0, then a child
                         * whose 0x8009 names parent slot 0. Without this, slot 0
                         * was never valid in the demo and every child composed
                         * against IDENTITY -- unrotated offsets, unrotated parts:
                         * the rider detached from the bike, with a translation
                         * error (628 units) that no pair-order setting moved.
                         * PROPCYCL_ROOT15=0 restores the old behaviour for A/B. */
                        { extern int g_root15;
                        if (pend_valid && (pend_root || g_root15)) {
                            memcpy(g_tslot[sl].M, pend_R,  sizeof pend_R);
                            memcpy(g_tslot[sl].P, pend_RP, sizeof pend_RP);
                            if (pend_root) { g_tslot[sl].t[0] = g_tslot[sl].t[1] = g_tslot[sl].t[2] = 0; }
                            else           { g_tslot[sl].t[0] = pend_L[0]; g_tslot[sl].t[1] = pend_L[1]; g_tslot[sl].t[2] = pend_L[2]; }
                            g_tslot[sl].valid = 1; pend_valid = 0;
                        } }
                        if (g_tslot[sl].valid) {
                            off[0] = (float)g_tslot[sl].t[0]; off[1] = (float)g_tslot[sl].t[1]; off[2] = (float)g_tslot[sl].t[2];
                            g_prebuilt_rot = (const int32_t (*)[3])g_tslot[sl].M;
                        }
                        if (g_mat_dump) {
                            const int32_t (*M)[3] = g_tslot[sl].valid ? (const int32_t (*)[3])g_tslot[sl].M : NULL;
                            printf("[MAT] code=%d slot=%d scale=%d M=%d %d %d %d %d %d %d %d %d t=%d %d %d\n",
                                   model_id + 0x45, sl, pend_scale,
                                   M?M[0][0]:0,M?M[0][1]:0,M?M[0][2]:0, M?M[1][0]:0,M?M[1][1]:0,M?M[1][2]:0, M?M[2][0]:0,M?M[2][1]:0,M?M[2][2]:0,
                                   (int)off[0],(int)off[1],(int)off[2]);
                        }
                    }
                }
                float px = (float)cmdram_read32(index + 3) + off[0];
                float py = (float)cmdram_read32(index + 4) + off[1];
                float pz = (float)cmdram_read32(index + 5) + off[2];
                if (model_id > 0 && model_id < (int)g_pointrom_count) {
                    /* APPLY THE 0x8008 ROTATION.
                     *
                     * This used to render unrotated, which is why the
                     * flyover's rider was laid out in its REST POSE: our
                     * camera-relative part offsets came out perfectly
                     * symmetric (codes 117/118 at +-1541, 115/116 at +-31,
                     * 123 and 126 identical) where the recording's are all
                     * asymmetric -- a rotated assembly cannot stay
                     * symmetric in camera space. Only 2 of 25 parts were
                     * within 200 units of MAME.
                     *
                     * FAILED_APPROACHES 1.11 records an earlier attempt
                     * that produced non-deterministic output; that one went
                     * through atan2+glRotatef / glMultMatrixf on the
                     * retired GL path. This goes through the SAME
                     * placement_matrix + q15_mul3 the 0x8002 branch already
                     * uses, which has no GL state and is deterministic. */
                    if (model_id == RIG_XFORM_ONLY) {
                        /* TRANSFORM-ONLY ROOT. The slot table above has just
                         * been validated from this record, which is the whole
                         * reason it is emitted; drawing it as well would put a
                         * second copy of the bike body on screen, because in
                         * gameplay player_render already emits models
                         * 0x45..0x4a itself. See game_dsp3d.c's note at the
                         * player_model_set_pose call. */
                        g_prebuilt_rot = NULL;
                    }
                    else if (geohw_enabled()) {
                        if (g_asm_mode == 3 || g_asm_mode == 4) g_prebuilt_rot = (const int32_t (*)[3])acc_M;
                        /* this rotation came from a 0x8008 -- use the 0x8008 order */
                        g_rot_ord_override = g_euler_8008;
                        /* A zero here is not "skip": the master really does
                         * compose the zero diagonal and emit the record, so the
                         * placement still exists and every vertex collapses to
                         * one point -- MAME's own bbox for codes 119/120 is
                         * exactly x[36..36] y[240..240]. Going through the
                         * normal path keeps the record in the distance dump, so
                         * the frame-locked attract diff still counts it. */
                        g_obj_scale = (pend_nscale >= 0) ? pend_nscale : 0x7FFF;
                        if (pend_nmat_valid && !g_no_nmat) {
                            for (int k = 0; k < 9; k++) g_obj_nmat[k] = pend_nmat[k];
                            g_obj_nmat_valid = 1;
                            g_obj_scale = 0x7FFF;   /* the matrix carries it */
                        }
                        findcode_dump(model_id, index, 0x800A);
                        render_object_hw_rot(model_id, px, py, pz,
                                             (g_rot8008 && cur_rot_valid) ? cur_rot_q15 : NULL);
                        g_obj_nmat_valid = 0;
                        g_obj_scale = 0x7FFF;
                        g_rot_ord_override = -1;
                        g_prebuilt_rot = NULL;
                    }
                    else render_object(model_id, px, py, pz);
                    models_rendered++;
                }
                pend_nscale = -1;   /* applies to this node's record only */
                pend_nmat_valid = 0;
                cmds_found++;
                index += 6;
                break;
            }

            case 0x8010: {
                /* End frame / end list SEGMENT.
                 *
                 * `0x8010, -1` is not an unconditional end of list. The game
                 * writes it mid-list and keeps appending: in the attract demo
                 * `balloon_render_and_hit_check` ends with
                 *     *p = 0x8010; p[1] = -1; W[0x0CA4] = p + 2;
                 * and `attract_advance_sequence` then calls FUN_0000e016,
                 * which appends the whole HUD (models 0x270..0x27d = codes
                 * 693..706) AFTER it. Stopping here dropped all ten HUD codes
                 * -- MAME's f691 carries them and we emitted none, which is
                 * why the attract gauges were empty while gameplay's were fine.
                 *
                 * Safe to continue ONLY because the walk is now bounded by the
                 * CPU's write cursor (see cmd_buf_words); without that bound,
                 * continuing would run into the previous frame's stale
                 * commands. So when the length is unknown, still stop.
                 * PROPCYCL_HARD_8010=1 restores the old hard stop for A/B. */
                int32_t sub = cmdram_read32(index + 1);
                if (sub == -1 || sub == (int32_t)0xFFFFFFFF) {
                    extern int g_hard_8010;
                    if (g_hard_8010 || cmd_buf_words <= 0) { g_walk_exit = 1; goto done; }
                    /* Segment end -- keep walking, and do NOT touch
                     * current_mode: the entries after the marker are still in
                     * the mode that was in force before it. Resetting it to
                     * 0x8000 made the walker read 11-word rotated entries as
                     * 4-word ones, which is where the junk codes 32836
                     * (= 0x7FFF + 0x45, a Q15 rotation word read as a model)
                     * came from. The real period there is 15 words:
                     * `0x8010, 3, 0, -1` plus an 11-word entry. */
                    /* Clears the object shift -- see g_obj_shift. */
                    g_obj_shift = 0;
                    index += 2;
                } else if (sub == 0 && index + 2 < max_words &&
                           cmdram_read32(index + 2) == -1) {
                    /* `0x8010, 0, -1` -- THREE words. The ending's credits
                     * layer (ending_credits_render, ROM 0x02FD8E) opens its
                     * screen-space block with `0x8001, 0, 0x8010, 0, -1`, and
                     * MAME's CPU list at ending counter 3400 carries exactly
                     * that. Read as the 4-word `0x8010, 3, <shift>, -1` it
                     * took the -1 as a shift (0x3FFFF, the far plane) and the
                     * next record's first word as the terminator, which
                     * desynchronised the walk and swallowed the `0x8000, 3`
                     * header that follows -- the whole 3D window drew as
                     * viewport 0 with every object parked at the back. The 0
                     * is a no-argument sub-op; it leaves no bias. */
                    g_obj_shift = 0;
                    index += 3;
                } else {
                    /* `0x8010, 3, <shift>, -1` -- the per-object zsort bias.
                     *
                     * MASK TO 18 BITS. The word the CPU writes is a plain
                     * signed value straight out of the scenery record's +0x1c
                     * field (ROM 0xAF0D8 + type*0x20): 0, -8960, -8704, -9216,
                     * ... stored full-width, so -8960 is 0xFFFFDD00. But the
                     * CONSUMER decodes a 22-BIT FIELD (geo_hw.c):
                     *
                     *   bit 21      set zsort ABSOLUTELY
                     *   bits 20:18  priority-band adjust
                     *   bits 17:0   the signed bias
                     *
                     * so the sign bits of a full-width negative land on the
                     * absolute flag AND the priority bits. Unmasked, -7680
                     * (0xFFFFE200) has bit 21 set and was read as "set zsort =
                     * 0x1FE200 absolutely" -- 2,089,472, the far plane -- so
                     * the object was painted FIRST and the terrain covered it.
                     * A user reported exactly that: "if I select level two Wind
                     * Woods I don't see the start platform." Course 1's
                     * platform (record type 57, point-ROM model 872 -- course 0
                     * uses 874) carries -7680 where course 0's carries 0, which
                     * is why only course 0 looked right.
                     *
                     * 18 bits is what the MASTER does, and register row 139
                     * already recorded the evidence without drawing this
                     * conclusion: it measured MAME's captured word for code 845
                     * as `0x03c9f0 = -13840`, and 0xFFFFC9F0 & 0x3FFFF is
                     * exactly 0x3C9F0. Checked against the ROM: all 70 distinct
                     * +0x1c values over 256 records lie in -65536..35296, well
                     * inside signed 18 bits, so the mask is lossless.
                     *
                     * `framedump.c` masks to 24 bits and is right to -- it
                     * reads the master's ALREADY-masked word out of polygonram,
                     * while the live walker reads the CPU's raw one. That
                     * asymmetry is why the frame gates never saw this, exactly
                     * as row 139 describes for the value being dropped
                     * entirely. PROPCYCL_RAW_OBJSHIFT=1 restores the unmasked
                     * read for A/B. */
                    { extern int g_raw_objshift;
                      int32_t sh = cmdram_read32(index + 2);
                      g_obj_shift = g_raw_objshift ? sh : (int32_t)(sh & 0x3FFFF); }
                    index += 4;
                }
                cmds_found++;
                break;
            }

            default:
                /* Unknown command >= 0x8000 — skip */
                index++;
                break;
            }
        } else {
            /* Data entry: word < 0x8000, interpret based on current mode */
            if (current_mode == 0x8000 && index + 4 <= max_words) {
                /* [model_id, x, y, z] — 4 words */
                int32_t model_id = word;
                float px = (float)cmdram_read32(index + 1);
                float py = (float)cmdram_read32(index + 2);
                float pz = (float)cmdram_read32(index + 3);

                if (model_id > 0 && model_id < (int)g_pointrom_count) {
                    findcode_dump(model_id, index, current_mode);
                    if (geohw_enabled()) render_object_hw(model_id, px, py, pz);
                    else render_object(model_id, px, py, pz);
                    models_rendered++;
                }
                data_entries++;
                if (data_entries > max_data) { g_walk_exit = 2; goto done; }
                index += 4;
            } else if ((current_mode == 0x8001 || current_mode == 0x8002) && index + 11 <= max_words) {
                /* [model_id, x, y, z, sx, cx, sy, cy, sz, cz, flags] — 11 words */
                int32_t model_id = word;
                float px = (float)cmdram_read32(index + 1);
                float py = (float)cmdram_read32(index + 2);
                float pz = (float)cmdram_read32(index + 3);
                /* rotation at [4..9] = three Q15 pairs, flags at [10].
                 *
                 * THE PAIRS ARE COS-FIRST, and reading them sin-first is why
                 * the starting platform (code 874) never appeared. The ROM's
                 * scenery emitter at 0x013AC8 writes W[0x16668] then
                 * W[0x1666C], and objects_render_master loads those from the
                 * 0x20B002 (cos) and 0x20B004 (sin) lanes in that order --
                 * cos first. Measured in our own buffer for 874: the X pair
                 * is (32767, 0) and the Z pair (32767, 0), which is a zero
                 * angle read cos-first and a NINETY DEGREE one read
                 * sin-first. That 90 deg about X tips the platform's height
                 * into depth: object 874 is a pole spanning y 0..89822, and
                 * its projected view z came out 217..11228 where MAME's own
                 * record gives 6886..19087, so the whole thing landed off
                 * the bottom of the screen (bbox y[1009..22M]).
                 * The Y pair (11980, 30503) = cos 0.3656 / sin 0.9309 then
                 * matches MAME's own object matrix for 874 exactly.
                 *
                 * This is the same cos-first convention register row 36
                 * measured for the 0x8008 records (PROPCYCL_ROT8008=2); it
                 * had never been checked for the 0x8002 data entries, which
                 * is register row 42. PROPCYCL_ROT8002=0 reverts for A/B. */
                int32_t rot[6];
                for (int rk = 0; rk < 6; rk++) rot[rk] = cmdram_read32(index + 4 + rk);
                { extern int g_rot8002; if (g_rot8002)
                    for (int rk = 0; rk < 6; rk += 2) {
                        int32_t sw = rot[rk]; rot[rk] = rot[rk+1]; rot[rk+1] = sw; } }
                { extern int g_8002_dbg;
                  if (g_8002_dbg) { int32_t fl = cmdram_read32(index + 10);
                    if (fl != 0) fprintf(stderr, "[8002] f=%d model=%d flags=%d\n",
                                         g_sys.frame_count, model_id, (int)fl); } }
                if (model_id > 0 && model_id < (int)g_pointrom_count) {
                    if (geohw_enabled()) {
                        /* flags at [10] -- bit 1 picks Z.Y.X, everything else
                         * Z.X.Y. This is the branch the balloon rig actually
                         * walks through: balloon_render_and_hit_check writes
                         * its `0x8010, 3, <shift>, -1` marker and then the
                         * balloon (flags=0) + propeller (flags=1) entries
                         * inline, so the propeller's spin reaches here.
                         * See g_8002_flagord. */
                        extern int g_8002_flagord;
                        int saved_ord = g_rot_ord_override;
                        if (g_8002_flagord)
                            g_rot_ord_override =
                                (cmdram_read32(index + 10) & 2) ? ORD_ZYX : ORD_ZXY;
                        findcode_dump(model_id, index, current_mode);
                        render_object_hw_rot(model_id, px, py, pz, rot);
                        g_rot_ord_override = saved_ord;
                    }
                    else render_object(model_id, px, py, pz);
                    models_rendered++;
                }
                data_entries++;
                if (data_entries > max_data) { g_walk_exit = 3; goto done; }
                index += 11;
            } else {
                index++;
            }
        }
    }
done:
    { extern int g_cmd_dump;
      if (g_cmd_dump) printf("[WALK] exit=%d at word %d of %d (cursor words %d) "
                             "cmds=%d models=%d data=%d\n",
                             g_walk_exit, index, max_words, cmd_buf_words,
                             cmds_found, models_rendered, data_entries); }

    static int pdp_dbg = 0;
    if (pdp_dbg < 30 || (g_sys.frame_count % 60 == 0)) {
        if (propcycl_verbose()) printf("  [3D] frame=%d: %d cmds, %d data, %d models from buf 0x%X (mode=0x%X)\n",
               g_sys.frame_count, cmds_found, data_entries, models_rendered,
               cmd_buf_base, current_mode);
        pdp_dbg++;
    }
}

/* ========== Model Viewer Mode ========== */

/* When > 0, bypasses DSP commands and renders this model directly */
static int viewer_model_id = 0;
static float viewer_rot_y = 0.0f;

/* Compute bounding sphere of a model for auto-framing */
static void compute_model_bounds(int code, float *cx, float *cy, float *cz, float *radius) {
    *cx = *cy = *cz = 0; *radius = 1.0f;
    if (code <= 0 || code >= (int)g_pointrom_count) return;
    int addr1 = point_read(code);
    if (addr1 < 0 || addr1 >= (int)g_pointrom_count) return;

    float minv[3] = {1e18f, 1e18f, 1e18f};
    float maxv[3] = {-1e18f, -1e18f, -1e18f};
    int nverts = 0;

    for (int obj = 0; obj < 500; obj++) {
        if (addr1 + obj >= (int)g_pointrom_count) break;
        int addr2 = point_read(addr1 + obj);
        if (addr2 < 0) break;
        if (addr2 >= (int)g_pointrom_count) break;
        int chunk_len = point_read(addr2);
        if (chunk_len <= 0 || chunk_len > 10000) continue;
        int pos = addr2 + 1, finish = pos + chunk_len;
        while (pos < finish && pos + 1 < (int)g_pointrom_count) {
            int pkt_len = point_read(pos); pos++;
            if (pkt_len <= 0 || pkt_len > 0x20) break;
            if (pkt_len >= 0x14) {
                for (int v = 0; v < 4; v++) {
                    int vi = pos + 12 + v * 3;
                    if (vi + 2 < (int)g_pointrom_count) {
                        float px = (float)point_read(vi);
                        float py = (float)point_read(vi+1);
                        float pz = (float)point_read(vi+2);
                        if (px < minv[0]) minv[0] = px; if (px > maxv[0]) maxv[0] = px;
                        if (py < minv[1]) minv[1] = py; if (py > maxv[1]) maxv[1] = py;
                        if (pz < minv[2]) minv[2] = pz; if (pz > maxv[2]) maxv[2] = pz;
                        nverts++;
                    }
                }
            }
            pos += pkt_len;
        }
    }
    if (nverts == 0) return;
    *cx = (minv[0] + maxv[0]) * 0.5f;
    *cy = (minv[1] + maxv[1]) * 0.5f;
    *cz = (minv[2] + maxv[2]) * 0.5f;
    float dx = maxv[0] - minv[0], dy = maxv[1] - minv[1], dz = maxv[2] - minv[2];
    *radius = dx; if (dy > *radius) *radius = dy; if (dz > *radius) *radius = dz;
    if (*radius < 1.0f) *radius = 1.0f;
}

static void render_viewer_model(void) {
    float cx, cy, cz, radius;
    compute_model_bounds(viewer_model_id, &cx, &cy, &cz, &radius);

    /* Auto-camera: frame the model nicely */
    float cam_dist = radius * 2.5f;

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    float aspect = (float)SCREEN_WIDTH / SCREEN_HEIGHT;
    float near_p = cam_dist * 0.01f;
    float far_p = cam_dist * 100.0f;
    float top = near_p * tanf(45.0f * 3.14159f / 360.0f);
    float right_p = top * aspect;
    glFrustum(-right_p, right_p, -top, top, near_p, far_p);

    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glTranslatef(0, 0, -cam_dist);
    glRotatef(-20.0f, 1, 0, 0);  /* slight tilt down */
    glRotatef(viewer_rot_y, 0, 1, 0);
    glTranslatef(-cx, -cy, -cz);

    render_object(viewer_model_id, 0, 0, 0);
    glDisable(GL_DEPTH_TEST);

    viewer_rot_y += 0.5f;  /* slow auto-rotate */
}

void renderer3d_set_viewer_model(int model_id) {
    viewer_model_id = model_id;
    viewer_rot_y = 30.0f;
    printf("[3D-VIEWER] Viewing model 0x%X (%d)\n", model_id, model_id);
}

/* ========== Public API ========== */

void renderer3d_init(void) {
    cam_x = cam_y = cam_z = 0;
    cam_zoom = 1.0f;
    cmd_buf_base = 0x10400;
    cur_pos[0] = cur_pos[1] = cur_pos[2] = 0;
    memset(cur_rot, 0, sizeof(cur_rot));
}

void renderer3d_process_dsp_commands(void) {
    process_pdp_commands();
}

void renderer3d_render_frame(void) {
    { extern int g_bn; g_bn = 0; }   /* per-frame bbox reset */
    { extern void blinklog_frame(unsigned); blinklog_frame(g_sys.frame_count); }
    dist_dump_frame_header();
    /* Model viewer mode: bypass DSP commands, render directly */
    if (viewer_model_id > 0) {
        render_viewer_model();
        return;
    }

    /* Normal mode: read DSP command buffer.
     * The game writes commands via W[0x0CA4] which points into g_sys.dspram.
     * Compute which buffer the game wrote to by checking where W[0x0CA4] points.
     * This is more reliable than dspram[0x10] which may not toggle if irq_vblank
     * conditions aren't met (W[0x0CAC] != 0, halt flags, etc). */
    {
        extern intptr_t _W[];
        uintptr_t write_ptr = (uintptr_t)_W[0x0CA4];
        uintptr_t dsp_base = (uintptr_t)g_sys.dspram;

        static int buf_dbg = 0;
        if (buf_dbg < 2) {
            if (propcycl_verbose()) printf("  [BUF] W[0x0CA4]=0x%lX dspram_base=0x%lX diff=%ld dspram[0x10]=%d W[0x0CA0]=%ld W[0x0CAC]=%ld\n",
                   (unsigned long)write_ptr, (unsigned long)dsp_base,
                   (long)(write_ptr - dsp_base),
                   (int)g_sys.dspram[0x10], (long)_W[0x0CA0], (long)_W[0x0CAC]);
            /* Dump first 8 words of each buffer directly */
            int32_t w[8];
            for (int bi = 0; bi < 2; bi++) {
                uint32_t base = 0x10400 + bi * 0x8000;
                for (int wi = 0; wi < 8; wi++) {
                    memcpy(&w[wi], g_sys.dspram + base + wi * 4, 4);
                }
                if (propcycl_verbose()) printf("  [BUF] buf%d(@0x%X): %08X %08X %08X %08X %08X %08X %08X %08X\n",
                       bi, base, w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
            }
            buf_dbg++;
        }

        cmd_buf_words = -1;                     /* -1 = cursor unusable */
        if (write_ptr >= dsp_base && write_ptr < dsp_base + DSPRAM_SIZE) {
            uint32_t offset = (uint32_t)(write_ptr - dsp_base);
            cmd_buf_base = (offset >= 0x18400) ? 0x18400 : 0x10400;
            /* `>=`, not `>`: a cursor AT the base is a list of length ZERO,
             * which is a real and common state (any screen that draws no 3D of
             * its own), not a missing measurement. */
            if (offset >= cmd_buf_base && offset - cmd_buf_base <= 0x8000)
                cmd_buf_words = (int)((offset - cmd_buf_base) / 4);
        } else {
            cmd_buf_base = 0x10400 + (uint32_t)(g_sys.dspram[0x10] & 1) * 0x8000;
        }
    }
    g_sys.dspram[0x0004] = 0;

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);

    /* Texture-environment state for bake_quad_texture: MODULATE the texture
     * by the per-vertex glColor (used for lighting), ALPHA_TEST drops pen=0
     * (transparent) texels so we don't paint the corners outside the actual
     * sprite/poster shape. */
    glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    glEnable(GL_ALPHA_TEST);
    glAlphaFunc(GL_GREATER, 0.1f);

    /* Set up perspective — DSP command positions are in game units (hundreds range).
     * Y axis is inverted (down = positive in game, up = positive in OpenGL). */
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    if (geohw_enabled()) {
        /* geo_hw emits 1/16-pixel SCREEN coordinates -- the perspective
         * divide already happened in fixed point, exactly as the hardware
         * does it. So this pass is a plain 2D blit, and the modelview must
         * be identity: any camera transform here would apply the rotation
         * a second time. */
        glOrtho(g_scene_x0, g_scene_x1, SCREEN_HEIGHT, 0, -1, 1);
        /* NO DEPTH TEST on this path. geo_hw sorts by zsort and draws far
         * to near (painter's algorithm, exactly as the hardware and the
         * reference rasteriser do), and every quad is emitted at z=0 in
         * screen space. With GL_LESS still enabled from the legacy path,
         * the FIRST quad at each pixel won and everything nearer was
         * rejected -- the rider, balloons and HUD vanished behind the
         * terrain, and far geometry showed through the sky. Frames with no
         * overlap (f480) looked perfect throughout, which is what hid it. */
        glDisable(GL_DEPTH_TEST);
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
        glDisable(GL_TEXTURE_2D);
        /* hardware polygon path: no pen-0 keying (see g_tex_opaque) */
        g_tex_opaque = 1;
        live_2d_refresh();          /* no-op while replaying a capture */
        tex_frame_hits = tex_frame_misses = 0;
        geohw_quads_drawn = 0; geohw_nbuf = 0;
        g_bri_min = 9999; g_bri_max = -9999;
        memset(&g_geo_stats, 0, sizeof g_geo_stats);
        { double _t = rperf();
          static int _fd = -1;   /* getenv ONCE: it was called every frame in
                                  * the hot path, and register row 40 makes a
                                  * frame-loop getenv a fault hazard too. */
          if (_fd < 0) { const char *e = getenv("PROPCYCL_FRAMEDUMP"); _fd = (e != NULL); }
          if (ui_map_active())  geohw_draw_map();
          else if (_fd)         framedump_render(geohw_quad_cb, NULL);
          else if (master_dsp_output())
              /* the real master DSP built this frame's scene: walk its
               * record list exactly as a MAME capture is walked */
              framedump_render_words(master_dsp_output(), geohw_quad_cb, NULL);
          else {
              extern const int32_t *pause_world_words(int *);
              extern void pause_world_reset(void);
              int n = 0; const int32_t *pw = NULL;
              if (g_pausecam_on) pw = pause_world_words(&n); else pause_world_reset();
              if (pw) { memset(seen_pos, 0, sizeof seen_pos); memset(seen_full, 0, sizeof seen_full); g_seen_mode = 1; }
              process_pdp_commands();
              if (pw) {                          /* the rest of the world, paused only */
                  g_pdp_words = pw; g_pdp_nwords = n; g_seen_mode = 2;
                  process_pdp_commands();
                  g_pdp_words = NULL; g_seen_mode = 0;
                  { static int said; if (!said) { said = 1;
                    fprintf(stderr, "[PAUSE360] extra walk: %d new placements, %d already drawn\n",
                            g_p360_new, g_p360_dup); } }
                  g_p360_new = g_p360_dup = 0;
              }
          }
          g_perf_pdp += rperf() - _t; }
        { double _t = rperf(); geohw_flush(); g_perf_flush += rperf() - _t; }
        { double _t = rperf(); geohw_draw_text(); g_perf_txt += rperf() - _t; }
        {
            static int dbg = 0;
            if (dbg < 20 || (g_sys.frame_count % 60 == 0)) {
                if (propcycl_verbose()) printf("  [GEOHW] f=%d drawn=%d emitted=%d partclip=%d | rej behind=%d cull=%d pktlen=%d chunk=%d | z %d..%d | bri %d..%d | fogged=%d alpha %d..%d | tex hit=%d miss=%d evict=%d | spr=%d txt=%d | realloc=%d sub=%d\n",
                       g_sys.frame_count, geohw_quads_drawn,
                       g_geo_stats.emitted, g_geo_stats.part_clip,
                       g_geo_stats.rej_behind, g_geo_stats.rej_cull,
                       g_geo_stats.rej_pktlen, g_geo_stats.rej_chunk,
                       g_geo_stats.zmin_seen, g_geo_stats.zmax_seen,
                       g_bri_min, g_bri_max, g_fogged_quads,
                       g_fogA_min, g_fogA_max,
                       tex_frame_hits, tex_frame_misses, tex_cache_evictions,
                       spr_px, txt_px, tex_reallocs, tex_subimages);
                geohw_report_dist();
                dbg++;
            }
        }
        glDisable(GL_DEPTH_TEST);
        return;
    }
    float aspect = (float)SCREEN_WIDTH / SCREEN_HEIGHT;
    float near_p = 1.0f;
    float far_p = 500000.0f;
    float fov_deg = 90.0f;
    float top = near_p * tanf(fov_deg * 3.14159f / 360.0f);
    float right = top * aspect;
    glFrustum(-right, right, -top, top, near_p, far_p);

    /* Camera — read heading/pitch from game state, apply as view rotation.
     * Terrain positions (0x8000 mode) are camera-relative world offsets,
     * so we need to rotate them by the inverse camera orientation. */
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    {
        extern intptr_t _W[];
        int16_t heading = (int16_t)(_W[0x0CEC] & 0xFFFF);
        int16_t pitch   = (int16_t)(_W[0x0CE8] & 0xFFFF);
        float heading_deg = heading * (360.0f / 65536.0f);
        float pitch_deg   = pitch * (360.0f / 65536.0f);

        float heading_deg_f = heading_deg;
        float pitch_deg_f = pitch_deg;

        static int cam_dbg2 = 0;
        if (cam_dbg2 < 2 || (g_sys.frame_count % 100 == 0)) {
            printf("  [CAM] heading=%d (%.1f°) pitch=%d (%.1f°) cam=(%ld,%ld,%ld) player=(%ld,%ld,%ld)\n",
                   heading, heading_deg_f, pitch, pitch_deg_f,
                   (long)_W[0x0CDC], (long)_W[0x0CE0], (long)_W[0x0CE4],
                   (long)_W[0x0D00], (long)_W[0x0D04], (long)_W[0x0D08]);
            cam_dbg2++;
        }

        /* Terrain positions from terrain_chunk_visibility are already camera-relative
         * (the game subtracts camera XZ before writing to DSP buffer).
         * We need to apply camera heading rotation + pitch + Y flip + debug
         * camera offset. OpenGL right-multiplies, so the LAST glRotate in
         * source order is applied FIRST to the vertex. The order below makes
         * the world rotate around camera-Y by heading first, then around
         * camera-X by pitch — standard yaw-then-pitch FPS camera order. */
        glScalef(1.0f, -1.0f, -1.0f);  /* flip Y (game Y-down) and Z (into screen) */
        glTranslatef(-cam_x, -cam_y, -cam_z);  /* debug camera offset (WASD) */
        glRotatef(pitch_deg_f, 1, 0, 0);  /* camera pitch (around X) */
        glRotatef(heading_deg_f, 0, 1, 0);  /* camera heading (around Y) */
    }

    /* Process and render 3D commands */
    process_pdp_commands();

    /* If level view is on, also render the full terrain grid */
    if (g_level_view_mode) {
        render_level_grid();
    }

    glDisable(GL_DEPTH_TEST);
}

/* ========== Level View Mode ========== */

int g_level_view_mode = 0;
static float level_cam_x = 0, level_cam_y = -50000, level_cam_z = 0;
static float level_cam_heading = 0;

void renderer3d_move_camera(float dx, float dy, float dz) {
    cam_x += dx;
    cam_y += dy;
    cam_z += dz;
}

void renderer3d_toggle_level_view(void) {
    g_level_view_mode = !g_level_view_mode;
    printf("[LEVEL] Level view %s\n", g_level_view_mode ? "ON" : "OFF");
}

static void render_level_grid(void) {
    extern intptr_t _W[];
    int base = (int)_W[0x12B4];  /* g_terrain_base_addr */

    if (base <= 0) {
        /* Not initialized yet — terrain_chunk_render hasn't run */
        static int warn = 0;
        if (!warn) { printf("[LEVEL] terrain base not set (W[0x12B4]=%d), press 'g' first\n", base); warn = 1; }
        return;
    }

    /* Render 8x16 terrain grid — each cell is 0x18000 game units apart */
    int rendered = 0;
    for (int z = 0; z < 16; z++) {
        for (int x = 0; x < 8; x++) {
            int model_id = base + z * 8 + x;
            if (model_id <= 0 || model_id >= (int)g_pointrom_count) continue;
            /* Check if model exists in point ROM */
            int addr1 = point_read(model_id);
            if (addr1 < 0 || addr1 >= (int)g_pointrom_count) continue;

            float px = (float)(x * 0x18000);
            float py = 0;
            float pz = (float)(z * 0x18000);
            render_object(model_id, px, py, pz);
            rendered++;
        }
    }

    static int once = 0;
    if (!once) {
        printf("[LEVEL] Rendered %d terrain chunks (base=%d)\n", rendered, base);
        once = 1;
    }
}
