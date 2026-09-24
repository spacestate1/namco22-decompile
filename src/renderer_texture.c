/*
 * renderer_texture.c — Texture pipeline for the 3D renderer.
 *
 * Implements the Namco Super 22 texture pipeline:
 *   UV coordinates → tilemap lookup → tile pixel → palette color → GL texture
 *
 * The pipeline is two-stage (matching MAME namcos22_v.cpp renderscanline_poly):
 *   Stage 1: tilemap index = ((v_banked & 0xFFF0) << 4) | ((u & 0xFF0) >> 4)
 *   Stage 2: tile  = pr1ccrl[tilemap_index * 2] (16-bit LE)
 *            attr  = pr1ccrh[tilemap_index / 2]  (packed nibble, 4 bits)
 *            pen   = texture_tiles[tile * 256 + transformed_local_offset]
 *   Stage 3: color = palette[group * 256 + pen]
 *
 * Texture bank (texbank, from V word bits [15:12]) offsets V by texbank*0x1000.
 * Without texbank the tilemap lookup reads the wrong 256x256 tile region —
 * balloon models (ID 0x2CE-0x307) use texbank=1; terrain uses texbank=0.
 *
 * Palette groups (0-127) are loaded once from ROM or MAME runtime dump.
 * Group 0:    packed RGB at ROM 0xB5EC0 (256 × 3 bytes)
 * Groups 1-127: planar R[256] G[256] B[256] at ROM 0x21B204 (768 bytes/group)
 * Groups 0 and 116-127 differ at runtime due to fog/lighting — use MAME dump.
 */
#include "renderer_internal.h"
#include <stdlib.h>
#include <stdio.h>
#include <math.h>

/* ========== Direct Palette ========== */

/* 128 palette groups × 256 entries × RGB. Loaded once at startup from
 * MAME runtime dump (preferred) or ROM static data (fallback). */
#define PAL_GROUPS  128
#define PAL_ENTRIES 256
uint8_t direct_palette[PAL_GROUPS][PAL_ENTRIES][3];
int direct_palette_loaded = 0;

void renderer3d_load_palette(const char *rom_dir) {
    char path[512];
    FILE *f;
    int loaded_mame = 0;

    /* Try MAME runtime palette dump first (includes runtime fog/lighting) */
    snprintf(path, sizeof(path), "%s/palette_mame_runtime.bin", rom_dir);
    f = fopen(path, "rb");
    if (f) {
        uint8_t *buf = malloc(128 * 256 * 3);
        if (buf && fread(buf, 1, 128 * 256 * 3, f) == 128 * 256 * 3) {
            for (int g = 0; g < 128; g++)
                for (int p = 0; p < 256; p++) {
                    int off = (g * 256 + p) * 3;
                    direct_palette[g][p][0] = buf[off];
                    direct_palette[g][p][1] = buf[off + 1];
                    direct_palette[g][p][2] = buf[off + 2];
                }
            loaded_mame = 1;
            printf("  [3D] Palette: MAME runtime dump (%s)\n", path);
        }
        if (buf) free(buf);
        fclose(f);
    }

    if (!loaded_mame) {
        /* Group 0 is black on the machine (MAME's runtime palette). The
         * packed table at 0xB5EC0 is NOT group 0: sprite_ram_header_init
         * (ROM 0x023198) loads it into groups 116..124 -- see below. */
        memset(direct_palette[0], 0, sizeof direct_palette[0]);
        /* Groups 1-127: planar R/G/B at ROM 0x21B204, 768 bytes per group */
        for (int g = 0; g < 127; g++) {
            int b = 0x21B204 + g * 768;
            for (int p = 0; p < 256; p++) {
                if (b + 512 + p < ROM_SIZE) {
                    direct_palette[g + 1][p][0] = g_sys.rom[b + p];
                    direct_palette[g + 1][p][1] = g_sys.rom[b + 256 + p];
                    direct_palette[g + 1][p][2] = g_sys.rom[b + 512 + p];
                }
            }
        }
        /* Groups 116..124: packed RGB at 0xB5EC0, 0x300 bytes a group, count
         * the BE16 at 0xB85B4 -- what the game itself loads at boot. */
        int npk = (g_sys.rom[0xB85B4] << 8) | g_sys.rom[0xB85B5];
        for (int k = 0; k < npk && 116 + k < PAL_GROUPS; k++)
            for (int p = 0; p < PAL_ENTRIES; p++) {
                int s = 0xB5EC0 + k * 0x300 + p * 3;
                if (s + 2 < ROM_SIZE) {
                    direct_palette[116 + k][p][0] = g_sys.rom[s];
                    direct_palette[116 + k][p][1] = g_sys.rom[s + 1];
                    direct_palette[116 + k][p][2] = g_sys.rom[s + 2];
                }
            }
        printf("  [3D] Palette: ROM static (group 127, the runtime colour ramps, is filled by the game)\n");
    }
    direct_palette_loaded = 1;
}

/*
 * The same palette in the PLANAR layout the 2D stages want (R at +0,
 * G at +0x8000, B at +0x10000), built once from direct_palette.
 *
 * text_hw/sprite_hw take a raw planar block. In a framedump replay they get
 * the capture's, which is why they gate 100.00% exact. On the LIVE path
 * they were handed g_sys.palette_ram -- the palette this project documents
 * as broken and which renderer3d_load_palette() exists to bypass. The
 * result was the HUD text drawing as solid magenta blocks over gameplay.
 */
static uint8_t planar_pal[0x18000];
static int planar_pal_built;

/*
 * WHICH PENS THE GAME ITSELF HAS WRITTEN.
 *
 * Measured against MAME's palette RAM on the CONTROLS tutorial screen
 * (tools/overnight/dump_menu_feed.lua vs PROPCYCL_FEEDDUMP): the snapshot
 * above is identical to the machine's on 32659 of 32768 pens, and ALL 109 it
 * gets wrong are in group 127 -- 0x7F00..0x7FFF, the runtime colour-ramp bank
 * that cz_load_color_ramp fills per screen. That is why the tutorial's red
 * arrow drew grey: its pens are 0x7F51..0x7F5E and the snapshot carried a
 * different screen's ramp there.
 *
 * g_sys.palette_ram cannot simply replace the snapshot -- the game only loads
 * a fraction of it (551/32768 match MAME), so a blanket switch would trade 109
 * wrong pens for 32217. Instead each palette writer marks the pens it touched,
 * and those -- and only those -- come from the game. The composition can
 * therefore only ever move agreement UP, and it converges on the machine as
 * more ramp loaders are wired in.
 */
static uint8_t pal_written[0x8000];

void palette_mark_written(int pen)
{
    if (pen < 0 || pen >= 0x8000) return;
    if (!pal_written[pen]) { pal_written[pen] = 1; planar_pal_built = 0; }
    else planar_pal_built = 0;      /* value may have changed */
}

const uint8_t *renderer3d_planar_palette(void) {
    if (!direct_palette_loaded) return NULL;
    if (!planar_pal_built) {
        for (int g = 0; g < PAL_GROUPS; g++)
            for (int p = 0; p < PAL_ENTRIES; p++) {
                int idx = g * 256 + p;
                if (pal_written[idx]) {
                    planar_pal[0x00000 + idx] = g_sys.palette_ram[0x00000 + idx];
                    planar_pal[0x08000 + idx] = g_sys.palette_ram[0x08000 + idx];
                    planar_pal[0x10000 + idx] = g_sys.palette_ram[0x10000 + idx];
                } else {
                    planar_pal[0x00000 + idx] = direct_palette[g][p][0];
                    planar_pal[0x08000 + idx] = direct_palette[g][p][1];
                    planar_pal[0x10000 + idx] = direct_palette[g][p][2];
                }
            }
        planar_pal_built = 1;
    }
    return planar_pal;
}

/* ========== Texture Lookup Pipeline ========== */

/* Read 4-bit tile attribute from pr1ccrh.1d, packed 2 per byte.
 * Even tilemap_index = low nibble, odd = high nibble. */
static int get_tile_attr(int tilemap_index) {
    if (!g_texture_tilemap) return 0;
    /* pr1ccrh.1d is stored after pr1ccrl.3d (2MB) in g_texture_tilemap */
    int attr_offset = 0x200000 + (tilemap_index >> 1);
    if (attr_offset >= (int)TEXTUREMAP_SIZE) return 0;
    uint8_t byte_val = g_texture_tilemap[attr_offset];
/* THE NIBBLE ORDER IS HIGH-FIRST. pc_raster_model.py, which is gated
 * byte-exact against MAME, builds its attribute table as
 *     for b in ccrh: append(b >> 4); append(b & 0xf)
 * i.e. attr[2k] is the HIGH nibble and attr[2k+1] the LOW one. This had it
 * the other way round, so every tile whose two nibbles differ -- 1.3% of
 * them -- was decoded with its NEIGHBOUR's attribute and came out flipped
 * or transposed against its neighbours. That is the "hard lines where the
 * tiling is not working" on terrain chunks. */
    return (tilemap_index & 1) ? (byte_val & 0xF) : ((byte_val >> 4) & 0xF);
}

/* Full UV → pen value lookup matching MAME renderscanline_poly.
 * texbank offsets V into the correct 256x256 tile region (critical for
 * balloon models which live in bank 1). */
uint8_t texture_pen_lookup(int u, int v, int texbank) {
    if (!g_texture_data || !g_texture_tilemap) return 0;

    u &= 0xFFF;
    v = (v & 0xFFF) | (texbank * 0x1000);  /* bank offset into V address space */

    /* Tilemap index: V selects 16-pixel row, U selects column within row */
    int tilemap_index = ((v & 0xFFF0) << 4) | ((u & 0xFF0) >> 4);

    /* Tile index from pr1ccrl.3d (16-bit little-endian) */
    int byte_off = tilemap_index * 2;
    if (byte_off + 1 >= 0x200000) return 0;
    uint32_t tile = g_texture_tilemap[byte_off] | (g_texture_tilemap[byte_off + 1] << 8);

    /* Tile attributes from pr1ccrh.1d */
    int attr = get_tile_attr(tilemap_index);
    if (attr & 0x1) tile |= 0x10000;   /* bit 0: extends tile index to 17 bits */

    /* Transform local pixel offset within 16×16 tile */
    int local_x = u & 0xF;
    int local_y = v & 0xF;
/* FLIPS FIRST, SWAP LAST. pc_raster_model.py's swizzle table is built
 * `if a&4: ix=15-ix; if a&2: iy=15-iy; if a&8: ix,iy = iy,ix`. Swapping
 * first, as this did, gives a DIFFERENT transform whenever the swap is
 * combined with exactly one flip (attr 0xA and 0xC, 3404 tiles): the
 * reference yields (y, 15-x) where this yielded (15-y, x). */
    if (attr & 0x4) local_x = 15 - local_x;  /* flip X */
    if (attr & 0x2) local_y = 15 - local_y;  /* flip Y */
    if (attr & 0x8) { int t = local_x; local_x = local_y; local_y = t; }  /* transpose */

    /* 8bpp pixel from pr1cg0-7 (row-major 16×16 tiles) */
    uint32_t pixel_offset = tile * 256 + local_y * 16 + local_x;
    if (pixel_offset >= TEXTURE_TOTAL_SIZE) return 0;
    return g_texture_data[pixel_offset];
}

/* When set, every texel is opaque and pen 0 resolves through the palette
 * like any other pen. This is what the hardware polygon path does: the
 * reference pixel chain (pc_raster_model.py) is
 *     rgb = pal.pen(pens_base + pen)
 * with NO transparency test anywhere. Treating pen 0 -- and, worse, any
 * pen whose palette entry happens to be black -- as transparent punched
 * black holes through solid sky and terrain. Left off for the legacy /
 * sprite paths, which do rely on pen-0 keying. */
int g_tex_opaque = 0;

/* Max baked texture edge.
 *
 * Kept at 256 deliberately. 512 was measured and buys almost nothing
 * (mean error vs the reference rasteriser 11.8 -> 11.5 of 255 across the
 * six gate frames) while costing 4x VRAM per cache entry -- with 4096
 * cache slots that is gigabytes. The residual error is NOT resolution:
 * we bake a per-quad UV bounding box and let GL sample it, where the
 * reference samples the tilemap per pixel. Closing that gap properly
 * means per-pixel sampling (a shader), not a bigger bake. */
#ifndef TEX_BAKE_MAX
/* The per-quad bake cap, in tilemap texels per axis.
 *
 * RAISING THIS TO 512 WAS MEASURED AND REJECTED. With the sampling exact
 * (below), 512 is a real and sizeable accuracy win -- mean error /255 over
 * the six reference frames 4.13 -> 3.60, all of it on the two dense terrain
 * frames (f1500 7.72 -> 6.54, f4800 9.69 -> 7.68), and 1024 renders
 * BYTE-IDENTICAL to 512, so 512 covers every quad in them. It also bakes
 * almost exactly the texel count the OLD oversampling was already spending
 * (1127M vs 1125M over a 3600-frame autopilot level), which made it look
 * free.
 *
 * It is not free: big textures cost more per texel than small ones (upload
 * bandwidth and the 1 MB staging buffer, not the sample loop), so the same
 * texel count took bake 16.5 -> 32.4 s, evictions 50276 -> 64090, and
 * **763 of 3600 frames over the 16.67 ms budget, against 565 at cap 256**
 * on the same machine under the same load. Register row
 * 121 bought that 0% and it is not worth 0.5/255.
 *
 * So the cap stays 256 and quads over it are decimated by an integer step.
 * The way to actually close the remaining gap is per-pixel sampling in a
 * shader, as the raster note in CLAUDE.md has said all along -- not a
 * bigger bake. PROPCYCL_TEX_POW2SAMPLE=1 reverts the sampling change for
 * A/B; there is deliberately no flag for the cap, because it is a
 * measured loss. */
#define TEX_BAKE_MAX 256

/* THE CAP IS PER QUAD (register row 193). A texture needs no more texels per
 * axis than the screen pixels it covers, so the renderer asks for the quad's
 * on-screen extent (g_tex_bake_cap_req, rounded up to 256/512/1024 so the
 * cache does not fragment): distant terrain keeps the measured 256 and its
 * budget, while the big screen-space plates -- the stage cards and title
 * cards, 300-400 texels wide -- are baked at full resolution instead of every
 * other texel, which is what made them look low-resolution next to MAME.
 * PROPCYCL_TEX_FIXEDCAP=1 restores the flat 256. */
int g_tex_bake_cap_req = TEX_BAKE_MAX;
int g_tex_fixedcap = 0;

/* Set from PROPCYCL_TEX_POW2SAMPLE in main.c. */
int g_tex_pow2sample = 0;
int g_tex_pow2alloc = 0;   /* PROPCYCL_TEX_POW2ALLOC=1: pad storage to a power of two again */
int g_tex_fifo = 0;        /* PROPCYCL_TEX_FIFO=1: evict with the old reference-less clock */
/* PROPCYCL_TEXORPHAN=0 disables orphaning, for A/B. Default ON. */
int g_tex_orphan = 1;
#endif

/* Pen value → 24-bit packed RGB using the loaded palette.
 * Returns 0 for pen==0 unless g_tex_opaque (see above). */
static uint32_t pen_to_rgb(uint8_t pen, int pal_group) {
    if (pen == 0 && !g_tex_opaque) return 0;
    pal_group &= 0x7F;
    return ((uint32_t)direct_palette[pal_group][pen][0] << 16) |
           ((uint32_t)direct_palette[pal_group][pen][1] << 8)  |
            (uint32_t)direct_palette[pal_group][pen][2];
}

/* Decode the per-polygon cmode (color depth, 4-bit) into the
 * (palette_offset, penshift, penmask) triple used to convert one 8bpp
 * tile fetch byte into a final pen index inside a palette group.
 * Mirrors tools/tile_pipeline.py:cmode_params and the MAME source
 * src/mame/namco/namcos22_v.cpp::renderscanline_poly. See
 * decompiled/annotations.md "Texture cmode" for the full table. */
static void cmode_params(int cmode, int *out_offset, int *out_shift, int *out_mask) {
    cmode &= 0xF;
    if (cmode & 4) {
        *out_offset = 0xEC + ((cmode & 8) << 1);
        *out_mask   = 0x03;
        *out_shift  = 2 * (~cmode & 3);
    } else if (cmode & 2) {
        *out_offset = 0xE0 + ((cmode & 8) << 1);
        *out_mask   = 0x0F;
        *out_shift  = 4 * (~cmode & 1);
    } else {
        *out_offset = 0;
        *out_mask   = 0xFF;
        *out_shift  = 0;
    }
}

/* ========== Per-Quad Texture Cache ========== */

/*
 * Quads with the same UV bounding box + texbank + palette group produce
 * identical pixels regardless of world position (ROM data is immutable).
 * We bake each unique combination once and reuse the GL texture indefinitely.
 *
 * Cache key: (min_u, min_v, range_u, range_v, texbank, pal_group)
 * Collision resolution: linear probing, 16-slot window.
 * Capacity: 4096 slots. Typical: ~1650 entries, ~2600 hits/frame.
 */
/* THE TABLE, NOT THE BYTE BUDGET, WAS THE LIMIT -- and that is why raising
 * the budget made things WORSE.
 *
 * This is linear probing with a 16-slot window at both ends: a LOOKUP gives
 * up after 16 probes and reports a miss even when the entry is stored
 * further along the cluster, and an INSERT that finds no free slot in 16
 * evicts an arbitrary victim (`tex_cache[h & MASK]`). Both degrade as the
 * load factor rises, so a bigger cache keeps more entries resident, clusters
 * grow past 16, and the false-miss and collision-eviction rates climb.
 * Measured over a full 9000-frame level at 16384 slots:
 *
 *      budget   frames over 12ms   worst frame   cumulative evictions
 *       448 MB      4049 / 9000      326 ms            145k
 *      1024 MB      2389             135 ms            268k   <- MORE
 *      2048 MB      1639              43 ms            385k   <- MORE STILL
 *
 * At 2048 MB the working set wants ~40000 entries and the table has 16384,
 * so it is permanently saturated. The late part of a course is where this
 * bites: the first 3600 frames never reach it, which is why every earlier
 * measurement in this file looked healthy.
 *
 * 65536 entries is ~2.6 MB of SYSTEM ram (the entry is ~40 bytes) and keeps
 * the load factor low enough that clusters stay short. The probe window is
 * 32 to match. Neither number bounds VRAM -- TEX_CACHE_BYTE_BUDGET does
 * that, and the two must be sized together: a budget that admits more
 * entries than the table can hold is the failure above. */
#define TEX_CACHE_SIZE 65536
#define TEX_CACHE_PROBES 32
#define TEX_CACHE_MASK (TEX_CACHE_SIZE - 1)

typedef struct {
    uint32_t bytes;            /* VRAM this entry holds (for the budget) */
    uint16_t alloc_w, alloc_h; /* storage actually allocated on the object */
    uint16_t used_w, used_h;   /* the sw x sh corner the bake actually fills */
    float    su, sv;           /* texcoord scale for that corner, INCLUDING the
                                * decimation step -- the caller multiplies its
                                * texture coordinates by it, so a CACHE HIT has
                                * to report the identical number, not re-derive
                                * it from used/alloc (which drops the step) */
    uint16_t min_u, min_v, range_u, range_v;
    uint8_t  texbank, pal_group;
    uint8_t  cmode;
    uint8_t  occupied;
    uint8_t  ref;              /* used since the eviction hand last passed (second chance) */
    uint16_t cap;              /* the bake cap this entry was made at */
    GLuint   gl_texture;
} TexCacheEntry;

static TexCacheEntry tex_cache[TEX_CACHE_SIZE];
int tex_frame_hits  = 0;
int tex_frame_misses = 0;
int tex_cache_evictions = 0;
int tex_reallocs = 0;      /* glTexImage2D: allocates new storage */
int tex_subimages = 0;     /* glTexSubImage2D: reuses storage */

/* Bake sizes are quantised to powers of two so a reused texture object
 * almost always already has storage of the right size, turning a
 * glTexImage2D (reallocate) into a glTexSubImage2D (reuse). Without this
 * every bake reallocated, and the NVIDIA driver pools freed GPU
 * allocations in SYSTEM RAM (EnableSystemMemoryPools), so the churn grew
 * an unaccounted pool instead of being recycled. */
static int pow2_up(int v, int lo, int hi)
{
    int p = lo;
    while (p < v && p < hi) p <<= 1;
    return p > hi ? hi : p;
}

/* Free list of texture OBJECTS with their allocated storage size.
 *
 * Budget eviction used to glDeleteTextures, so the next bake always found
 * an empty slot and had to glTexImage2D fresh storage -- measured at 18305
 * reallocations and ZERO storage reuses over 400 frames. Recycling the
 * object here, keyed by its allocated size, turns the steady state into
 * glTexSubImage2D against storage that already exists. */
typedef struct { GLuint id; uint16_t w, h; } tex_free_ent;
static tex_free_ent tex_free[TEX_CACHE_SIZE];
static int tex_free_n;

static void tex_free_push(GLuint id, uint16_t w, uint16_t h)
{
    if (tex_free_n < TEX_CACHE_SIZE) {
        tex_free[tex_free_n].id = id;
        tex_free[tex_free_n].w = w; tex_free[tex_free_n].h = h;
        tex_free_n++;
    } else {
        glDeleteTextures(1, &id);      /* list full: genuinely drop it */
    }
}

/* Prefer an object whose storage is already exactly tw x th. */
static GLuint tex_free_pop(int tw, int th, int *had_w, int *had_h)
{
    for (int i = tex_free_n - 1; i >= 0; i--) {
        if (tex_free[i].w == tw && tex_free[i].h == th) {
            GLuint id = tex_free[i].id;
            *had_w = tex_free[i].w; *had_h = tex_free[i].h;
            tex_free[i] = tex_free[--tex_free_n];
            return id;
        }
    }
    if (tex_free_n > 0) {
        tex_free_n--;
        *had_w = tex_free[tex_free_n].w; *had_h = tex_free[tex_free_n].h;
        return tex_free[tex_free_n].id;
    }
    *had_w = *had_h = 0;
    return 0;
}

/* VRAM budget for the quad-texture cache.
 *
 * The cache is bounded by ENTRY COUNT, but entries vary from 8x8 to
 * 256x256 -- so 16384 slots is anywhere from 4 MB to 4 GB of VRAM. That
 * upper bound is not acceptable on its own, and it is worse than it looks
 * on NVIDIA systems running NVreg_PreserveVideoMemoryAllocations=1, where
 * the driver mirrors video memory into system RAM: VRAM we allocate shows
 * up as unaccounted system pages. Track bytes and evict against a budget
 * so the footprint is bounded no matter the mix of texture sizes. */
/* THE CACHE WAS THRASHING AT 192 MB, AND A RE-BAKE IS THE MOST EXPENSIVE
 * THING THIS RENDERER DOES. 192 was chosen to bound the footprint, not from
 * any measurement of what the working set needs. Measured over a 3600-frame
 * autopilot level (-O2 build), sweeping the budget:
 *
 *      MB   frames over 16.67ms   bake     evictions
 *     192        306 / 3600      10.68 s     50276
 *     320         16             3.87 s      43191
 *     448          5             3.15 s      39225
 *     576          3             3.06 s      36402
 *
 * i.e. the old default spent roughly SEVEN SECONDS of every 3600 frames
 * re-baking textures it had just thrown away. 448 MB sits past the knee and
 * is still modest enough for a 2 GB GPU; this machine's GTX 1660 has 6 GB.
 * PROPCYCL_TEXBUDGET=<MB> overrides it -- lower it on a small-VRAM card, and
 * note the eviction counter in the [PERF] line is how you tell if it is
 * thrashing again. */
#define TEX_CACHE_BUDGET_DEFAULT_MB 448
size_t tex_cache_budget = (size_t)TEX_CACHE_BUDGET_DEFAULT_MB * 1024 * 1024;
#define TEX_CACHE_BYTE_BUDGET tex_cache_budget
size_t tex_cache_bytes = 0;
int tex_cache_live = 0;
static int    tex_evict_hand  = 0;
static int tex_cache_total = 0;

/* Static pixel buffer for baking (TEX_BAKE_MAX^2 RGBA, reused each bake) */
static uint8_t tex_pixel_buf[(1024 + 1) * (1024 + 1) * 4];   /* up to the 1024 cap + guard texel */

void renderer_texture_init(void) {
    /* Delete any live textures rather than just dropping the entries --
     * calling this a second time would otherwise orphan every texture the
     * cache holds. Safe at startup: occupied is zero, so nothing is freed. */
    for (int i = 0; i < TEX_CACHE_SIZE; i++) {
        if (tex_cache[i].occupied) glDeleteTextures(1, &tex_cache[i].gl_texture);
        tex_cache[i].occupied = 0;
        tex_cache[i].alloc_w = tex_cache[i].alloc_h = 0;
    }
    for (int i = 0; i < tex_free_n; i++) glDeleteTextures(1, &tex_free[i].id);
    tex_free_n = 0;
    tex_cache_total  = 0;
    tex_frame_hits   = 0;
    tex_frame_misses = 0;
    tex_cache_bytes  = 0;
    tex_evict_hand   = 0;
}

static uint32_t tex_cache_hash(int min_u, int min_v, int range_u, int range_v,
                               int texbank, int pal_group, int cmode) {
    uint32_t h = 2166136261u;
    h ^= (uint32_t)min_u;    h *= 16777619u;
    h ^= (uint32_t)min_v;    h *= 16777619u;
    h ^= (uint32_t)range_u;  h *= 16777619u;
    h ^= (uint32_t)range_v;  h *= 16777619u;
    h ^= (uint32_t)texbank;  h *= 16777619u;
    h ^= (uint32_t)pal_group;h *= 16777619u;
    h ^= (uint32_t)cmode;    h *= 16777619u;
    h ^= (uint32_t)g_tex_opaque; h *= 16777619u;  /* keyed: changes the texels */
    return h;
}

/*
 * Bake a GL texture for one quad's UV bounding box, or return a cached one.
 *
 * We always bake the full UV bounding rectangle (no polygon clip) to avoid
 * seam artifacts where adjacent quads share edges — the geometry clips what's
 * visible. Degenerate ranges < 16 are expanded to 16 (matches Python exporter).
 * pen=0 pixels are transparent (alpha=0).
 */
GLuint bake_quad_texture(int min_u, int min_v, int range_u, int range_v,
                         int texbank, int pal_group, int cmode,
                         float *out_su, float *out_sv) {
    /* Expand degenerate UV ranges to minimum 16 texels */
    if (range_u < 16) { min_u = (min_u * 2 + range_u) / 2 - 8; range_u = 16; }
    if (range_v < 16) { min_v = (min_v * 2 + range_v) / 2 - 8; range_v = 16; }
    if (min_u < 0) min_u = 0;
    if (min_v < 0) min_v = 0;

    /* Cache lookup — cmode contributes to the key because cmode 2 vs 3
     * (and the 2bpp variants) decode the same fetch byte to different
     * sub-pens, so the same UV bbox baked under different cmodes must
     * not alias. */
    int cap = TEX_BAKE_MAX;
    if (!g_tex_fixedcap && (range_u > TEX_BAKE_MAX || range_v > TEX_BAKE_MAX)) {
        int want = g_tex_bake_cap_req;
        cap = want > 512 ? 1024 : want > 256 ? 512 : 256;
    }
    uint32_t h = tex_cache_hash(min_u, min_v, range_u, range_v, texbank, pal_group, cmode);
    h ^= (uint32_t)cap; h *= 16777619u;
    for (int probe = 0; probe < TEX_CACHE_PROBES; probe++) {
        int slot = (h + probe) & TEX_CACHE_MASK;
        TexCacheEntry *e = &tex_cache[slot];
        if (!e->occupied) break;
        if (e->min_u == min_u && e->min_v == min_v &&
            e->range_u == range_u && e->range_v == range_v &&
            e->texbank == texbank && e->pal_group == pal_group &&
            e->cmode == cmode && e->cap == cap) {
            tex_frame_hits++;
            e->ref = 1;
            if (out_su) *out_su = e->su;
            if (out_sv) *out_sv = e->sv;
            return e->gl_texture;
        }
    }
    tex_frame_misses++;

    /* Reuse an evicted slot's texture OBJECT rather than deleting and
     * regenerating one. At ~40 misses + ~40 evictions per frame this was
     * ~4800 texture create/destroy pairs per second, and the NVIDIA driver
     * pools freed allocations in system RAM (EnableSystemMemoryPools), so
     * the churn shows up as unaccounted system pages rather than being
     * recycled. Reusing the id keeps the object count flat. */
    /* Decode cmode once per bake (constant across all texels of this quad). */
    int cm_offset, cm_shift, cm_mask;
    cmode_params(cmode, &cm_offset, &cm_shift, &cm_mask);

    /* SAMPLING RATE AND ALLOCATION BUCKET ARE TWO DIFFERENT NUMBERS, and
     * this used to be one. The bake was resampled to a power of two and the
     * `px * range_u / tw` integer divide took the difference -- which does
     * NOT preserve texel phase. With range_u 192 oversampled to 256, source
     * texels 1 and 2 both land on output 0 while 3 lands on 3: texels
     * duplicated and skipped UNEVENLY, and unevenly WITHIN each 16x16 tile.
     * That is what a player sees as the tile pattern not lining up, and the
     * old comment's claim that the oversample "shifts nothing" was wrong --
     * it shifts every texel whose index is not a multiple of tw/range_u.
     *
     * Now: one baked texel per source texel, so texel N of the bake IS texel
     * min_u + N of the tilemap, and the power of two survives only as the
     * ALLOCATION size below (so a recycled object still usually has storage
     * of exactly the right size and the upload stays a glTexSubImage2D).
     * The used corner is reported through out_su/out_sv and the caller
     * scales its texture coordinates by it.
     *
     * OVER THE CAP, decimate by an INTEGER step rather than scaling by a
     * ratio. The reference samples the tilemap per pixel; we bake a bbox and
     * let GL sample it, so a downsample is detail permanently lost -- but
     * losing it EVENLY matters as much as how much is lost. range_u 380
     * scaled to 256 keeps source texels 0,1,2,4,5,7,8,..., an irregular
     * stutter that falls differently inside every tile; step 2 keeps
     * 0,2,4,6,... so every tile decimates identically and the pattern stays
     * regular. Measured on the two dense terrain frames, mean error /255:
     * f1500 7.89 -> 7.72, f4800 10.12 -> 9.69 (the other four are under the
     * cap, step 1, and unchanged).
     *
     * The cap is also applied PER AXIS. Scaling both by the smaller ratio
     * threw away detail on the axis that did not need it: a quad of 380x99
     * was baked 256x67, losing a third of its v resolution for no reason.
     *
     * Whole-suite result vs the reference rasteriser, mean error /255:
     *   120 1.12->0.79  480 1.51->1.07  1500 9.39->7.72  2400 6.00->4.76
     *   3600 1.09->0.77  4800 11.56->9.69   mean 5.11 -> 4.13 */
    /* A/B: PROPCYCL_TEX_POW2SAMPLE=1 restores the old behaviour -- sample at
     * the power-of-two allocation size, uneven phase and all -- so the two
     * can be rendered side by side from one binary. */
    int step_u = (range_u + cap - 1) / cap;
    int step_v = (range_v + cap - 1) / cap;
    if (step_u < 1) step_u = 1;
    if (step_v < 1) step_v = 1;
    int sw = (range_u + step_u - 1) / step_u;
    int sh = (range_v + step_v - 1) / step_v;
    if (sw < 1) sw = 1;
    if (sh < 1) sh = 1;
    int tw = sw, th = sh;
    if (g_tex_pow2sample) {
        /* The OLD sizing, verbatim, for A/B: one number served as both the
         * sampling rate and the allocation bucket, both axes scaled by the
         * smaller ratio, and the bake loop's `px * range / tw` divide took
         * up the slack. `sw == tw` below is what makes it oversample. */
        float scale = 1.0f;
        if (range_u > TEX_BAKE_MAX) scale = (float)TEX_BAKE_MAX / range_u;
        if (range_v > TEX_BAKE_MAX && (float)TEX_BAKE_MAX / range_v < scale)
            scale = (float)TEX_BAKE_MAX / range_v;
        tw = (int)(range_u * scale + 0.5f); if (tw < 8) tw = 8; if (tw > TEX_BAKE_MAX) tw = TEX_BAKE_MAX;
        th = (int)(range_v * scale + 0.5f); if (th < 8) th = 8; if (th > TEX_BAKE_MAX) th = TEX_BAKE_MAX;
        sw = tw; sh = th;
        step_u = step_v = 1;
    }
    /* ALLOCATION ONLY. Quantising the storage to a power of two means a
     * recycled texture object usually already has exactly this size, so the
     * upload is a glTexSubImage2D (reuse) rather than a glTexImage2D
     * (reallocate). That matters because the NVIDIA driver pools freed GPU
     * allocations in SYSTEM RAM, and the churn grew an unaccounted pool
     * instead of being recycled.
     *
     * It no longer costs any accuracy, because it no longer changes the
     * sampling rate. Measured over a 3600-frame autopilot level, this change
     * against the old shared number:
     *   texels sampled   1125M -> 608M   (-46%)
     *   glTexImage2D     11578 -> 10448  (-10%)
     *   glTexSubImage2D  40243 -> 42108  (+5%, the same work moved off the
     *                                     reallocating path)
     *   evictions        49394 -> 50276  (+1.8%, the per-axis cap's cost:
     *                                     a wide thin quad no longer has its
     *                                     short axis scaled down with the
     *                                     long one, so it buckets larger)
     * and back to back under identical machine load:
     *   bake             16.836 -> 16.956 s  (flat; these runs spread +-2.5 s)
     *   render           27.496 -> 28.035 s
     *   frames over 16.67 ms   681 -> 612 of 3600
     * (texels are 608M rather than 599M and bake 16.956 rather than 16.153
     * because of the guard texel below -- ~1.5%, and worth it)
     * (that machine had unrelated load at the time, which is why the
     * absolute over-budget count is not the 0 register row 121 records --
     * both arms were measured in the same conditions, so the DELTA is the
     * result, and the counters above are exact regardless of load).
     *
     * (An earlier note here claimed pow2 cost 2.0 -> 53.5. That was wrong:
     * the measurement had the sprite layer composited while the reference
     * is 3D-only. Use PROPCYCL_NO_SPRITES=1 when comparing to it.) */
    tw = pow2_up(sw, 8, cap);               /* allocation size, for reuse */
    th = pow2_up(sh, 8, cap);
    if (g_tex_pow2sample) { sw = tw; sh = th; }   /* old: sample AT the pow2 */
    /* EXACT STORAGE, not a power of two. The pow2 bucket existed so a
     * recycled texture object would already have storage of the right size
     * and could take a glTexSubImage2D -- but the orphaning fix (below) now
     * calls glTexImage2D on EVERY bake ("realloc N / reuse 0" in [PERF]), so
     * the padding bought nothing and cost VRAM: a 260-texel quad held 512^2,
     * ~4x what it uses. The byte budget counts the allocation, so the cache
     * was being filled with empty padding and evicting live textures to make
     * room -- 185k evictions over a 6000-frame level. The guard texel stays
     * exactly where the pow2 layout had it (present when the bucket had room,
     * absent when sw was already a power of two), so the baked texels are
     * the same; only the storage around them shrinks. */
    else if (!g_tex_pow2alloc) {
        tw = (sw < tw) ? sw + 1 : sw;
        th = (sh < th) ? sh + 1 : sh;
    }
    /* Source offset d lands on baked texel d/step, i.e. texcoord
     * d/(step*tw) -- so the caller's range_u maps to range_u/(step*tw).
     * With step 1 this is the plain sw/tw. */
    float su = g_tex_pow2sample ? 1.0f : (float)range_u / (float)(step_u * tw);
    float sv = g_tex_pow2sample ? 1.0f : (float)range_v / (float)(step_v * th);
    if (out_su) *out_su = su;
    if (out_sv) *out_sv = sv;

    /* Sample tilemap → palette → RGBA pixels.
     *
     * For non-8bpp polygons (cmode != 0), one tile fetch byte encodes
     * multiple lower-bpp pens packed side-by-side. cm_shift selects
     * which sub-pen, cm_mask isolates it, and cm_offset moves the
     * lookup into the upper sub-palette region of the palette group.
     * For cmode 0 the math collapses to the original pen passthrough. */
    /* The SAMPLED size is the work done; tw/th are only the allocation
     * bucket, and counting those reported the bake as larger than it is. */
    /* ONE GUARD TEXEL past the used corner, where there is room for it.
     *
     * The storage is tw x th and only sw x sh of it is written, so a sample
     * at exactly the quad's far edge -- texcoord == su, i.e. texel index sw
     * -- reads storage this bake never touched. That is not merely
     * undefined: texture OBJECTS are recycled, and the realloc is skipped
     * whenever the size already matches, so the padding still holds the
     * PREVIOUS quad's pixels. The result would be a one-pixel edge of an
     * unrelated texture -- exactly the seam artifact the "always fill the
     * whole UV bounding box" rule in CLAUDE.md exists to prevent.
     *
     * Baking one extra row and column of real tilemap data past the bbox
     * costs ~2*sqrt(N) texels and makes that sample land on the neighbouring
     * source texel, which is what it should have been. su/sv are unchanged:
     * they still map range_u onto sw, so the guard is only ever reached by
     * the boundary sample itself. */
    int bw = (sw < tw) ? sw + 1 : sw;
    int bh = (sh < th) ? sh + 1 : sh;
    { extern double g_bake_texels; g_bake_texels += (double)bw * bh; }
    /* THE HOT LOOP OF THE WHOLE RENDERER. Measured over a 3600-frame run:
     * 1046 MILLION texels baked, 14.0 s of a 17.8 s flush -- and in gameplay
     * the flush alone was 13.5 ms of a 15.9 ms frame, i.e. over the 16.67 ms
     * a 60 Hz frame allows. The GL calls were 1.1 s of that 17.8; the cost is
     * here, per texel.
     *
     * Three things were being redone for every one of those texels, all of
     * them constant or near-constant across the bake. None of this changes a
     * single output byte -- verified pixel-identical on five captured frames.
     *
     *  1. `px * range_u / tw` is an INTEGER DIVIDE per texel. It depends only
     *     on px, so the row of tu values is computed once (tw divides instead
     *     of tw*th), and tv once per row.
     *  2. pen_to_rgb() indexes direct_palette[group][pen][0..2] separately per
     *     texel although pal_group is fixed for the bake. Flattened to one
     *     256-entry RGBA LUT built once.
     *  3. texture_pen_lookup() redoes the tilemap index, the 16-bit tile read
     *     and the attribute nibble for every texel, but a 16x16 tile covers 16
     *     consecutive u values -- so consecutive texels in a row nearly always
     *     hit the SAME tilemap entry. Cached on the entry index. */
    {
    /* 1. tu per column, once */
    static int tu_row[1024 + 1];   /* up to the 1024 cap + guard texel */
    if (g_tex_pow2sample)
        for (int px = 0; px < bw; px++) tu_row[px] = min_u + px * range_u / sw;
    else
        for (int px = 0; px < bw; px++) tu_row[px] = min_u + px * step_u;

    /* 2. the palette group's 256 colours, pre-packed as the RGBA bytes the
     *    buffer wants (alpha 0 marks the transparent pen, exactly as the
     *    per-texel `rgb == 0 && !g_tex_opaque` test did). */
    uint8_t lut[256][4];
    for (int p = 0; p < 256; p++) {
        uint32_t rgb = pen_to_rgb((uint8_t)p, pal_group);
        if (rgb == 0 && !g_tex_opaque) { lut[p][0]=lut[p][1]=lut[p][2]=lut[p][3]=0; }
        else { lut[p][0]=(rgb>>16)&0xFF; lut[p][1]=(rgb>>8)&0xFF;
               lut[p][2]=rgb&0xFF;       lut[p][3]=255; }
    }

    const uint8_t *tmap = g_texture_tilemap, *tdata = g_texture_data;
    for (int py = 0; py < bh; py++) {
        int tv = g_tex_pow2sample ? min_v + py * range_v / sh
                                  : min_v + py * step_v;
        int v  = (tv & 0xFFF) | (texbank * 0x1000);
        int vrow = (v & 0xFFF0) << 4;            /* tilemap row base */
        int local_y0 = v & 0xF;
        uint8_t *dst = &tex_pixel_buf[(size_t)py * bw * 4];
        int last_idx = -1; uint32_t tile = 0; int attr = 0;
        for (int px = 0; px < bw; px++) {
            uint8_t fetch = 0;
            if (tmap && tdata) {
                int u = tu_row[px] & 0xFFF;
                int tilemap_index = vrow | ((u & 0xFF0) >> 4);
                /* 3. same tile as the previous texel? (16 u values share one) */
                if (tilemap_index != last_idx) {
                    last_idx = tilemap_index;
                    int byte_off = tilemap_index * 2;
                    if (byte_off + 1 < 0x200000) {
                        tile = tmap[byte_off] | (tmap[byte_off + 1] << 8);
                        int ao = 0x200000 + (tilemap_index >> 1);
                        /* HIGH nibble on EVEN index -- the same rule as
                         * get_tile_attr() above. THIS is the live copy: row
                         * 121 inlined the tilemap lookup into the hot loop,
                         * so the two must be kept in step, and fixing only
                         * the function above changes nothing at all. */
                        attr = (ao < (int)TEXTUREMAP_SIZE)
                               ? ((tilemap_index & 1) ? (tmap[ao] & 0xF)
                                                      : ((tmap[ao] >> 4) & 0xF))
                               : 0;
                        if (attr & 0x1) tile |= 0x10000;
                    } else { tile = 0; attr = 0; last_idx = -1; }
                }
                int local_x = u & 0xF, local_y = local_y0;
                if (attr & 0x4) local_x = 15 - local_x;   /* flips FIRST */
                if (attr & 0x2) local_y = 15 - local_y;
                if (attr & 0x8) { int t = local_x; local_x = local_y; local_y = t; }
                uint32_t po = tile * 256 + local_y * 16 + local_x;
                if (po < TEXTURE_TOTAL_SIZE) fetch = tdata[po];
            }
            int sub_pen = (fetch >> cm_shift) & cm_mask;
            const uint8_t *c = lut[(cm_offset + sub_pen) & 0xFF];
            dst[0] = c[0]; dst[1] = c[1]; dst[2] = c[2]; dst[3] = c[3];
            dst += 4;
        }
    }
    }

    /* Upload to GL (NEAREST filtering — pixel art, no blurring) */
    /* Find the slot first so an eviction can donate its texture object. */
    GLuint tex = 0;
    TexCacheEntry *slotp = NULL;
    for (int probe = 0; probe < TEX_CACHE_PROBES; probe++) {
        TexCacheEntry *e = &tex_cache[(h + probe) & TEX_CACHE_MASK];
        if (!e->occupied) { slotp = e; break; }
    }
    if (!slotp) {
        slotp = &tex_cache[h & TEX_CACHE_MASK];
        tex = slotp->gl_texture;          /* reuse, do NOT delete */
        tex_cache_bytes -= slotp->bytes;
        tex_cache_total--;
        tex_cache_evictions++;
        slotp->occupied = 0;
    }
    int had_w = slotp->alloc_w, had_h = slotp->alloc_h;
    if (!tex) {
        tex = tex_free_pop(tw, th, &had_w, &had_h);
        if (!tex) { glGenTextures(1, &tex); had_w = had_h = 0; }
    }
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    /* CLAMP, NOT THE DEFAULT REPEAT. Texture coordinates run to exactly the
     * far edge of the used region, and for GL_NEAREST that is texel index
     * sw -- one past the last sampled texel. Under GL_REPEAT that wraps to
     * texel 0, i.e. the OPPOSITE side of the texture, so the last row and
     * column of pixels of every quad is sampled from the wrong place. On a
     * terrain chunk, where quads tile edge to edge, that draws a hard wrong
     * line along every quad boundary. The guard texel below covers the case
     * where the pow2 allocation leaves room for it, but when sw is already
     * a power of two there is no room and only the wrap mode saves it. */
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    /* The storage is tw x th (a power of two, so a recycled object usually
     * already has it); the PIXELS are the exact sw x sh bake and go in the
     * top-left corner. The caller only ever samples that corner, because
     * out_su/out_sv scale its texture coordinates to it. */
    /* ORPHAN BEFORE UPLOADING. Writing into a texture the GPU may still be
     * reading from makes the driver SYNCHRONISE -- it has to wait for the
     * in-flight draw that references the old contents before it can touch
     * them. glTexImage2D with a NULL pointer is the standard idiom for
     * "discard what was there, give me fresh storage", which lets the
     * driver hand back a new buffer immediately instead of stalling.
     *
     * Skipping it when the size already matched was meant to save a
     * reallocation, and it does -- at the cost of the stall. A full-level
     * profile put 28.8% of the entire run inside __vdso_clock_gettime called
     * from libnvidia-eglcore, which is what a driver spinning on a fence
     * looks like. PROPCYCL_TEXORPHAN=0 restores the old behaviour for A/B. */
    if (!g_tex_orphan && had_w == tw && had_h == th) {
        tex_subimages++;
    } else {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tw, th, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        tex_reallocs++;
    }
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, bw, bh,
                    GL_RGBA, GL_UNSIGNED_BYTE, tex_pixel_buf);

    /* Store in cache, EVICTING if the probe window is full.
     *
     * This used to just give up after 16 failed probes and return the
     * texture unstored -- and nothing ever called glDeleteTextures, so
     * every such bake leaked a GL texture for the lifetime of the process.
     * Replaying a 900-frame capture bakes ~1000 quads per frame, so once
     * the table filled the leak ran at roughly a thousand textures per
     * frame and the renderer slowed to a crawl as VRAM filled. Bounded
     * now: the table owns at most TEX_CACHE_SIZE textures. */
    /* Enforce the byte budget with a SECOND-CHANCE clock hand, skipping the
     * slot we are about to fill. A plain clock with no reference bit is FIFO:
     * it evicted textures drawn on EVERY frame as readily as dead ones, and
     * each came straight back as a re-bake -- over a 6000-frame level ~170k
     * bakes against ~1.8k that were a different size tier of a cached entry
     * and 0 from a full probe window, i.e. almost all of them were this. A
     * hit (or a fresh bake) sets `ref`; the hand clears it and moves on, so
     * only textures unused for a whole sweep are evicted. Which texture is
     * evicted is the only thing this changes -- every bake is the same.
     * PROPCYCL_TEX_FIFO=1 restores the old hand. Bounded: two sweeps. */
    {
        size_t incoming = (size_t)tw * th * 4;
        int guard = 0;
        while (tex_cache_bytes + incoming > TEX_CACHE_BYTE_BUDGET &&
               guard++ < 2 * TEX_CACHE_SIZE) {
            TexCacheEntry *e = &tex_cache[tex_evict_hand];
            tex_evict_hand = (tex_evict_hand + 1) & TEX_CACHE_MASK;
            if (!e->occupied || e == slotp) continue;
            if (e->ref && !g_tex_fifo) { e->ref = 0; continue; }
            tex_free_push(e->gl_texture, e->alloc_w, e->alloc_h);
            tex_cache_bytes -= e->bytes;
            tex_cache_total--;
            tex_cache_evictions++;
            e->occupied = 0;
        }
    }
    slotp->min_u = (uint16_t)min_u;  slotp->min_v = (uint16_t)min_v;
    slotp->range_u = (uint16_t)range_u; slotp->range_v = (uint16_t)range_v;
    slotp->texbank  = (uint8_t)texbank;
    slotp->pal_group = (uint8_t)pal_group;
    slotp->cmode     = (uint8_t)(cmode & 0xF);
    slotp->cap       = (uint16_t)cap;
    slotp->gl_texture = tex;
    slotp->alloc_w = (uint16_t)tw; slotp->alloc_h = (uint16_t)th;
    slotp->used_w  = (uint16_t)sw; slotp->used_h  = (uint16_t)sh;
    slotp->su = su; slotp->sv = sv;
    slotp->bytes = (uint32_t)(tw * th * 4);
    tex_cache_bytes += slotp->bytes;
    if (!slotp->occupied) tex_cache_total++;
    slotp->occupied = 1;
    slotp->ref = 1;
    return tex;
}
