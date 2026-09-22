/*
 * Standalone Course Viewer
 *
 * Replicates the game's terrain_chunk_visibility() dispatch so the level is
 * rendered the way the engine actually renders it — only the ~60 chunks
 * visible from the current camera position+heading are emitted each frame,
 * picked from ROM 0x8B200's 32 heading-zone descriptor tables. No cell-wide
 * overlap from running a "union of everything" pass.
 *
 * Build: done by CMakeLists.txt alongside model_viewer.
 *
 * Usage:
 *   ./course_viewer [rom_dir] [course 0..3]
 *   (defaults: extracted / course 0)
 *
 * TOP-DOWN OVERVIEW only — camera high above the grid, looking straight
 * down. Shows every non-placeholder terrain chunk, no culling.
 * (Fly-camera mode is deprecated; code is kept but SPACE toggle removed.)
 *
 * Controls:
 *   1-4        select course
 *   Mouse wheel  zoom in/out
 *   Shift+wheel or +/- change chunk spacing; 0 resets to 1.0x
 *   WASD       pan
 *   Middle-mouse drag  rotate the top-down view
 *   Left-click a chunk  inspect (prints model id + grid coords)
 *   Left-click LIGHT+  brighten scene
 *   G          toggle grid overlay
 *   K          cycle sky: OFF → gradient → model 0x30D → model 0x30E
 *   P          print current viewer state
 *   Esc / Ctrl-C quit
 */
#include <SDL2/SDL.h>
#include <GL/gl.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ========== Common ROM / Texture / Renderer (shared with model_viewer) ========== */
/* We link against the same translation unit pattern — reproduce it here to keep
 * the viewer self-contained; ~420 lines of shared code copied from model_viewer.c. */

#define WIN_W            1024
#define WIN_H            768
#define ROM_SIZE         0x400000
#define POINTROM_SIZE    (512 * 1024 * 3)
#define TEXTURE_TOTAL_SIZE (0x200000 * 8)
#define TEXTUREMAP_SIZE  (0x280000)
#define PALETTE_GROUPS   128
#define PALETTE_ENTRIES  256

static uint8_t  g_rom[ROM_SIZE];
static int32_t  g_pointrom[POINTROM_SIZE];
static uint32_t g_pointrom_count;
static uint8_t *g_texture_data;
static uint8_t *g_texture_tilemap;
static uint8_t  g_palette[PALETTE_GROUPS][PALETTE_ENTRIES][3];

static bool load_file(const char *path, uint8_t *buf, size_t size) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "Cannot open: %s\n", path); return false; }
    size_t rd = fread(buf, 1, size, f);
    fclose(f);
    return rd == size;
}
static bool load_file_at(const char *dir, const char *name, uint8_t *buf, size_t off, size_t size) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    return load_file(path, buf + off, size);
}
static int32_t signed24(uint32_t v) {
    return (v & 0x800000) ? (int32_t)(v | 0xFF000000) : (int32_t)v;
}
static uint32_t rom_be32(uint32_t off) {
    if (off + 4 > ROM_SIZE) return 0;
    return ((uint32_t)g_rom[off]   << 24) |
           ((uint32_t)g_rom[off+1] << 16) |
           ((uint32_t)g_rom[off+2] <<  8) |
            (uint32_t)g_rom[off+3];
}
static int32_t  rom_s32(uint32_t off) { return (int32_t)rom_be32(off); }

static bool load_roms(const char *dir) {
    /* Program ROM */
    uint8_t *r1=malloc(0x100000), *r2=malloc(0x100000), *r3=malloc(0x100000), *r4=malloc(0x100000);
    if (!r1||!r2||!r3||!r4) return false;
    bool ok = true;
    ok = ok && load_file_at(dir,"pr2ver-a.1",r1,0,0x100000);
    ok = ok && load_file_at(dir,"pr2ver-a.2",r2,0,0x100000);
    ok = ok && load_file_at(dir,"pr2ver-a.3",r3,0,0x100000);
    ok = ok && load_file_at(dir,"pr2ver-a.4",r4,0,0x100000);
    if (ok) {
        for (size_t i = 0; i < 0x100000; i++) {
            g_rom[i*4+0]=r4[i]; g_rom[i*4+1]=r3[i];
            g_rom[i*4+2]=r2[i]; g_rom[i*4+3]=r1[i];
        }
    }
    free(r1); free(r2); free(r3); free(r4);
    if (!ok) return false;

    /* Point ROM */
    const size_t CHIP = 0x80000, PLANE = CHIP*3;
    uint8_t *lo=calloc(PLANE,1), *mi=calloc(PLANE,1), *hi=calloc(PLANE,1);
    if (!lo||!mi||!hi) return false;
    const char *ptrl[]={"pr1ptrl0.18k","pr1ptrl1.16k","pr1ptrl2.15k"};
    const char *ptrm[]={"pr1ptrm0.18j","pr1ptrm1.16j","pr1ptrm2.15j"};
    const char *ptru[]={"pr1ptru0.18f","pr1ptru1.16f","pr1ptru2.15f"};
    for (int i=0;i<3;i++) {
        ok = ok && load_file_at(dir,ptrl[i],lo,CHIP*i,CHIP);
        ok = ok && load_file_at(dir,ptrm[i],mi,CHIP*i,CHIP);
        ok = ok && load_file_at(dir,ptru[i],hi,CHIP*i,CHIP);
    }
    if (ok) {
        g_pointrom_count = PLANE;
        for (size_t i = 0; i < g_pointrom_count; i++)
            g_pointrom[i] = signed24(((uint32_t)hi[i]<<16)|((uint32_t)mi[i]<<8)|lo[i]);
    }
    free(lo); free(mi); free(hi);
    if (!ok) return false;

    /* Texture tiles */
    g_texture_data = calloc(TEXTURE_TOTAL_SIZE, 1);
    const char *cg[] = {"pr1cg0.12b","pr1cg1.10d","pr1cg2.12d","pr1cg3.13d",
                        "pr1cg4.14d","pr1cg5.16d","pr1cg6.18a","pr1cg7.15a"};
    for (int i=0;i<8;i++)
        ok = ok && load_file_at(dir, cg[i], g_texture_data, 0x200000*i, 0x200000);

    /* Texture tilemap */
    g_texture_tilemap = calloc(TEXTUREMAP_SIZE, 1);
    ok = ok && load_file_at(dir,"pr1ccrl.3d", g_texture_tilemap, 0, 0x200000);
    ok = ok && load_file_at(dir,"pr1ccrh.1d", g_texture_tilemap, 0x200000, 0x80000);

    /* Palette: MAME runtime first, fall back to ROM static */
    char path[512];
    snprintf(path, sizeof(path), "%s/palette_mame_runtime.bin", dir);
    FILE *mf = fopen(path, "rb");
    int loaded_mame = 0;
    if (mf) {
        uint8_t buf[128*256*3];
        if (fread(buf, 1, sizeof(buf), mf) == sizeof(buf)) {
            for (int g = 0; g < 128; g++)
                for (int p = 0; p < 256; p++) {
                    int off = (g*256+p)*3;
                    g_palette[g][p][0]=buf[off];
                    g_palette[g][p][1]=buf[off+1];
                    g_palette[g][p][2]=buf[off+2];
                }
            loaded_mame = 1;
        }
        fclose(mf);
    }
    if (!loaded_mame) {
        for (int p = 0; p < 256; p++) {
            int s = 0xB5EC0 + p*3;
            g_palette[0][p][0]=g_rom[s]; g_palette[0][p][1]=g_rom[s+1]; g_palette[0][p][2]=g_rom[s+2];
        }
        for (int g = 0; g < 127; g++) {
            int b = 0x21B204 + g*768;
            for (int p = 0; p < 256; p++) {
                g_palette[g+1][p][0]=g_rom[b+p];
                g_palette[g+1][p][1]=g_rom[b+256+p];
                g_palette[g+1][p][2]=g_rom[b+512+p];
            }
        }
    }
    printf("  Palette: %s\n", loaded_mame ? "MAME runtime" : "ROM static");
    return ok;
}

/* Texture pipeline (identical to model_viewer) */
static int get_tile_attr(int ti) {
    int attr_off = 0x200000 + (ti >> 1);
    if (attr_off >= (int)TEXTUREMAP_SIZE) return 0;
    uint8_t b = g_texture_tilemap[attr_off];
/* THE NIBBLE ORDER IS HIGH-FIRST. pc_raster_model.py, which is gated
 * byte-exact against MAME, builds its attribute table as
 *     for b in ccrh: append(b >> 4); append(b & 0xf)
 * i.e. attr[2k] is the HIGH nibble and attr[2k+1] the LOW one. This had it
 * the other way round, so every tile whose two nibbles differ -- 1.3% of
 * them -- was decoded with its NEIGHBOUR's attribute and came out flipped
 * or transposed against its neighbours. That is the "hard lines where the
 * tiling is not working" on terrain chunks. */
    return (ti & 1) ? (b & 0xF) : ((b >> 4) & 0xF);
}
static uint8_t tex_pen_lookup(int u, int v, int texbank) {
    u &= 0xFFF; v = (v & 0xFFF) | (texbank * 0x1000);
    int ti = ((v & 0xFFF0) << 4) | ((u & 0xFF0) >> 4);
    int byte_off = ti * 2;
    if (byte_off + 1 >= 0x200000) return 0;
    uint32_t tile = g_texture_tilemap[byte_off] | (g_texture_tilemap[byte_off+1] << 8);
    int attr = get_tile_attr(ti);
    if (attr & 1) tile |= 0x10000;
    int lx = u & 0xF, ly = v & 0xF;
/* FLIPS FIRST, SWAP LAST. pc_raster_model.py's swizzle table is built
 * `if a&4: ix=15-ix; if a&2: iy=15-iy; if a&8: ix,iy = iy,ix`. Swapping
 * first, as this did, gives a DIFFERENT transform whenever the swap is
 * combined with exactly one flip (attr 0xA and 0xC, 3404 tiles): the
 * reference yields (y, 15-x) where this yielded (15-y, x). */
    if (attr & 4) lx = 15 - lx;
    if (attr & 2) ly = 15 - ly;
    if (attr & 8) { int t=lx; lx=ly; ly=t; }
    uint32_t off = tile * 256 + ly * 16 + lx;
    return (off < TEXTURE_TOTAL_SIZE) ? g_texture_data[off] : 0;
}
static uint32_t pen_to_rgb(uint8_t pen, int pal_group) {
    if (pen == 0) return 0;
    pal_group &= 0x7F;
    return (g_palette[pal_group][pen][0] << 16) |
           (g_palette[pal_group][pen][1] << 8)  |
            g_palette[pal_group][pen][2];
}

/* Texture cache */
#define TEX_CACHE_SIZE 8192
#define TEX_CACHE_MASK (TEX_CACHE_SIZE - 1)
typedef struct {
    uint16_t min_u, min_v, range_u, range_v;
    uint8_t texbank, pal_group, occupied, _pad;
    GLuint gl_tex;
} TexCacheEntry;
static TexCacheEntry tex_cache[TEX_CACHE_SIZE];
static uint8_t tex_pixels[256*256*4];

static GLuint bake_texture(int min_u, int min_v, int range_u, int range_v,
                           int texbank, int pal_group) {
    if (range_u < 16) { min_u = (min_u + min_u + range_u)/2 - 8; range_u = 16; }
    if (range_v < 16) { min_v = (min_v + min_v + range_v)/2 - 8; range_v = 16; }
    if (min_u < 0) min_u = 0; if (min_v < 0) min_v = 0;

    uint32_t h = 2166136261u;
    h = (h ^ (uint32_t)min_u)   * 16777619u;
    h = (h ^ (uint32_t)min_v)   * 16777619u;
    h = (h ^ (uint32_t)range_u) * 16777619u;
    h = (h ^ (uint32_t)range_v) * 16777619u;
    h = (h ^ (uint32_t)texbank) * 16777619u;
    h = (h ^ (uint32_t)pal_group) * 16777619u;
    for (int probe = 0; probe < 16; probe++) {
        TexCacheEntry *e = &tex_cache[(h+probe) & TEX_CACHE_MASK];
        if (!e->occupied) break;
        if (e->min_u==min_u && e->min_v==min_v && e->range_u==range_u &&
            e->range_v==range_v && e->texbank==texbank && e->pal_group==pal_group)
            return e->gl_tex;
    }

    /* Cap PER AXIS, and past the cap decimate by an INTEGER step -- see
     * register row 142. Scaling both axes by the smaller ratio threw away
     * resolution on the axis that did not need it, and `px*range_u/tw` then
     * dropped source texels UNEVENLY, differently inside each 16x16 tile,
     * which is what reads as the tile pattern not lining up. Every texel is
     * now either 1:1 (under the cap) or every step'th, so each tile
     * decimates identically. This viewer allocates exactly tw x th with no
     * power-of-two rounding, so texture coordinates stay 0..1 and the whole
     * of the main renderer's out_su/out_sv machinery is unnecessary here. */
    int step_u = (range_u + 255) / 256; if (step_u < 1) step_u = 1;
    int step_v = (range_v + 255) / 256; if (step_v < 1) step_v = 1;
    int tw = (range_u + step_u - 1) / step_u; if (tw < 8) tw = 8; if (tw > 256) tw = 256;
    int th = (range_v + step_v - 1) / step_v; if (th < 8) th = 8; if (th > 256) th = 256;
    for (int py = 0; py < th; py++)
        for (int px = 0; px < tw; px++) {
            int tu = min_u + px*step_u;
            int tv = min_v + py*step_v;
            uint8_t pen = tex_pen_lookup(tu, tv, texbank);
            uint32_t rgb = pen_to_rgb(pen, pal_group);
            int idx = (py*tw+px)*4;
            if (rgb == 0) {
                tex_pixels[idx]=tex_pixels[idx+1]=tex_pixels[idx+2]=0; tex_pixels[idx+3]=0;
            } else {
                tex_pixels[idx]=(rgb>>16)&0xFF; tex_pixels[idx+1]=(rgb>>8)&0xFF;
                tex_pixels[idx+2]=rgb&0xFF; tex_pixels[idx+3]=255;
            }
        }
    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    /* CLAMP, NOT THE DEFAULT REPEAT -- texcoords reach exactly 1.0 at the
     * far edge of each quad, which for GL_NEAREST is texel index tw, and
     * GL_REPEAT wraps that to texel 0 on the OPPOSITE side. That paints a
     * hard wrong line along every quad boundary, which on a terrain chunk
     * reads as the tiling being broken. */
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tw, th, 0, GL_RGBA, GL_UNSIGNED_BYTE, tex_pixels);

    for (int probe = 0; probe < 16; probe++) {
        TexCacheEntry *e = &tex_cache[(h+probe) & TEX_CACHE_MASK];
        if (!e->occupied) {
            e->min_u=min_u; e->min_v=min_v; e->range_u=range_u; e->range_v=range_v;
            e->texbank=texbank; e->pal_group=pal_group; e->gl_tex=tex; e->occupied=1;
            break;
        }
    }
    return tex;
}

/* Global light-brightness multiplier (bumped by the onscreen LIGHT+ button). */
static float g_light_boost = 1.0f;

/* Render a point-ROM model at absolute world position (ox, oy, oz) in GAME
 * units. Caller is responsible for the GL modelview matrix. Coordinates are
 * negated on X so the scene comes out right-handed under glFrustum. */
static int32_t pt(uint32_t addr) {
    return (addr < g_pointrom_count) ? g_pointrom[addr] : 0;
}
static int render_model_at(int code, float ox, float oy, float oz) {
    if (code <= 0 || code >= (int)g_pointrom_count) return 0;
    int addr1 = pt(code);
    if (addr1 < 0 || addr1 >= (int)g_pointrom_count) return 0;
    int total = 0;
    for (int obj = 0; obj < 500; obj++) {
        if (addr1+obj >= (int)g_pointrom_count) break;
        int a2 = pt(addr1+obj);
        if (a2 < 0 || a2 >= (int)g_pointrom_count) break;
        int cl = pt(a2);
        if (cl <= 0 || cl > 10000) continue;
        int pos = a2+1, fin = pos+cl;
        while (pos < fin && pos+1 < (int)g_pointrom_count) {
            int pl = pt(pos); pos++;
            int base = pos;
            if (pl <= 0 || pl > 0x20) break;
            if (base+pl > (int)g_pointrom_count) break;
            if (pl >= 0x14) {
                int tex0 = pt(base+2) & 0xFFFFFF;
                int pal_group = (tex0 >> 8) & 0x7F;
                int texbank = (pt(base+5) >> 12) & 0xF;
                int uv[4][2]; float verts[4][3]; float max_c = 0;
                for (int v = 0; v < 4; v++) {
                    uv[v][0] = pt(base+4+v*2) & 0xFFF;
                    uv[v][1] = pt(base+5+v*2) & 0xFFF;
                }
                for (int v = 0; v < 4; v++) {
                    int vi = base+12+v*3;
                    float vx=(float)pt(vi), vy=(float)pt(vi+1), vz=(float)pt(vi+2);
                    verts[v][0] = -(vx+ox); verts[v][1] = vy+oy; verts[v][2] = vz+oz;
                    if (fabsf(vx)>max_c) max_c=fabsf(vx);
                    if (fabsf(vy)>max_c) max_c=fabsf(vy);
                    if (fabsf(vz)>max_c) max_c=fabsf(vz);
                }
                if (max_c < 1 || max_c > 500000) { pos += pl; continue; }
                float ax=verts[1][0]-verts[0][0], ay=verts[1][1]-verts[0][1], az=verts[1][2]-verts[0][2];
                float bx=verts[2][0]-verts[0][0], by=verts[2][1]-verts[0][1], bz=verts[2][2]-verts[0][2];
                float nx=ay*bz-az*by, ny=az*bx-ax*bz, nz=ax*by-ay*bx;
                float nlen=sqrtf(nx*nx+ny*ny+nz*nz);
                if (nlen>0) { nx/=nlen; ny/=nlen; nz/=nlen; }
                float light = (0.45f + 0.55f * fabsf(nx*0.35f + ny*0.55f + nz*0.5f)) * g_light_boost;
                if (light > 1.0f) light = 1.0f;

                int u_min=uv[0][0], u_max=uv[0][0], v_min=uv[0][1], v_max=uv[0][1];
                for (int i=1;i<4;i++) {
                    if (uv[i][0]<u_min) u_min=uv[i][0]; if (uv[i][0]>u_max) u_max=uv[i][0];
                    if (uv[i][1]<v_min) v_min=uv[i][1]; if (uv[i][1]>v_max) v_max=uv[i][1];
                }
                int ru=u_max-u_min; if (ru<1) ru=1;
                int rv=v_max-v_min; if (rv<1) rv=1;
                GLuint qtex = bake_texture(u_min, v_min, ru, rv, texbank, pal_group);
                int bu=u_min, bv=v_min, bru=ru, brv=rv;
                if (bru<16) { bu=(u_min+u_max)/2-8; bru=16; }
                if (brv<16) { bv=(v_min+v_max)/2-8; brv=16; }
                if (bu<0) bu=0; if (bv<0) bv=0;

                glEnable(GL_TEXTURE_2D);
                glBindTexture(GL_TEXTURE_2D, qtex);
                glColor3f(light, light, light);
                glBegin(GL_QUADS);
                for (int v=0;v<4;v++) {
                    float tu = (float)(uv[v][0]-bu)/(float)bru;
                    float tv = (float)(uv[v][1]-bv)/(float)brv;
                    glTexCoord2f(tu, tv);
                    glVertex3f(verts[v][0], verts[v][1], verts[v][2]);
                }
                glEnd();
                glDisable(GL_TEXTURE_2D);
                total++;
            }
            pos += pl;
        }
    }
    return total;
}

/* ========== Terrain dispatch (mirrors game_terrain.c:terrain_chunk_visibility) ========== */

static const int COURSE_TERRAIN_BASE[4]    = { 0x495, 0x515, 0x595, 0x615 };
static const int COURSE_TERRAIN_BITMASK[4] = { 0x8B280, 0x8BA80, 0x8C280, 0x8CA80 };
#define CELL_SIZE 0x18000
#define ZONE_TABLE_BASE 0x8B200
#define DESCRIPTOR_STRIDE 20
#define DESCRIPTORS_PER_ZONE 60

/* Clamp camera grid to valid 8x16 range for the bitmask lookup — outside the
 * grid we'd read garbage. We still run the zone dispatch but with a safe
 * cell index. */
typedef struct { int chunks_emitted; int chunks_rejected; int zone_used; } ChunkStats;

static int render_terrain_for_camera(int course, int32_t cam_x, int32_t cam_z,
                                     uint32_t heading, ChunkStats *stats,
                                     int cull_with_bitmask) {
    int base = COURSE_TERRAIN_BASE[course];
    uint32_t bitmask_ptr = COURSE_TERRAIN_BITMASK[course];

    int cam_grid_x  = cam_x / CELL_SIZE;
    int cam_grid_z  = cam_z / CELL_SIZE;
    int cam_sub_x   = cam_x - cam_grid_x * CELL_SIZE;
    int cam_sub_z   = cam_z - cam_grid_z * CELL_SIZE;
    int cell_idx    = cam_grid_x + cam_grid_z * 8;
    int cell_valid  = (cam_grid_x >= 0 && cam_grid_x < 8 &&
                       cam_grid_z >= 0 && cam_grid_z < 16);

    int zone = ((int)(heading + 0x400) & 0xFFFF) >> 11;
    if (zone < 0) zone = 0; if (zone > 31) zone = 31;
    stats->zone_used = zone;
    uint32_t desc_base = rom_be32(ZONE_TABLE_BASE + zone * 4);

    int total_quads = 0;
    stats->chunks_emitted = 0;
    stats->chunks_rejected = 0;
    for (int i = 0; i < DESCRIPTORS_PER_ZONE; i++) {
        uint32_t entry = desc_base + i * DESCRIPTOR_STRIDE;
        if (entry + DESCRIPTOR_STRIDE > ROM_SIZE) break;
        int32_t off   = rom_s32(entry + 0);
        int32_t dx    = rom_s32(entry + 4);
        int32_t dz    = rom_s32(entry + 8);
        int32_t lx    = rom_s32(entry + 12);
        int32_t lz    = rom_s32(entry + 16);
        int wgx = cam_grid_x + dx;
        int wgz = cam_grid_z + dz;
        if (wgx < 0 || wgx >= 8 || wgz < 0 || wgz >= 16) { stats->chunks_rejected++; continue; }
        if (off < 0 || off > 127) { stats->chunks_rejected++; continue; }
        /* Bitmask gate (same formula as terrain_chunk_visibility). Toggled
         * off by the 'C' key: with culling disabled, we render everything
         * the zone table lists that fits in the grid — no per-camera-cell
         * visibility test, so nothing that's actually in view gets dropped. */
        if (cull_with_bitmask && cell_valid) {
            uint32_t bm_addr = bitmask_ptr + cell_idx * 0x10 + wgz;
            if (bm_addr < ROM_SIZE) {
                uint8_t mask_byte = g_rom[bm_addr];
                if (!(mask_byte & (0x80 >> (wgx & 7)))) {
                    stats->chunks_rejected++; continue;
                }
            }
        }
        /* Absolute world position: chunk rides at its cell index regardless
         * of camera sub-position, matching
         * world_x = cam_grid_x * CELL + local_x (the descriptor provides
         * local_x = chunk_absolute - cam_grid*CELL, so adding cam_grid*CELL
         * back gives us the chunk's absolute world position). */
        float wx = (float)(cam_grid_x * CELL_SIZE + lx);
        float wz = (float)(cam_grid_z * CELL_SIZE + lz);
        total_quads += render_model_at(base + off, wx, 0, wz);
        stats->chunks_emitted++;
    }
    return total_quads;
}

/* ========== Sky ==========
 * Modes:
 *  0 — off
 *  1 — blue gradient box that follows the camera
 *  2 — point-ROM model 0x30D (game's primary sky sphere, scaled up, depth-off)
 *  3 — point-ROM model 0x30E (alternate sky variant)
 */
static void draw_sky(int mode, float cam_x, float cam_y, float cam_z) {
    if (mode == 0) return;
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    if (mode == 1) {
        /* Gradient cube centered on the camera, drawn without texture */
        glDisable(GL_TEXTURE_2D);
        float r = 2000000.0f;   /* big enough to be outside all terrain */
        float cx = cam_x, cy = cam_y, cz = cam_z;
        /* Vertex colors: top = sky blue, bottom = horizon tan */
        float top[3]    = { 0.55f, 0.70f, 0.95f };
        float mid[3]    = { 0.80f, 0.82f, 0.86f };
        float bottom[3] = { 0.32f, 0.28f, 0.22f };
        /* Cylinder-ish quads for the four sides, plus top and bottom */
        glBegin(GL_QUADS);
        float faces[4][2][2] = {
            { {-r, -r}, { r, -r} },  /* front */
            { { r, -r}, { r,  r} },  /* right */
            { { r,  r}, {-r,  r} },  /* back */
            { {-r,  r}, {-r, -r} },  /* left */
        };
        for (int i = 0; i < 4; i++) {
            float x0 = faces[i][0][0], z0 = faces[i][0][1];
            float x1 = faces[i][1][0], z1 = faces[i][1][1];
            glColor3fv(bottom); glVertex3f(-(cx+x0), cy-r, cz+z0);
            glColor3fv(bottom); glVertex3f(-(cx+x1), cy-r, cz+z1);
            glColor3fv(mid);    glVertex3f(-(cx+x1), cy,   cz+z1);
            glColor3fv(mid);    glVertex3f(-(cx+x0), cy,   cz+z0);

            glColor3fv(mid);    glVertex3f(-(cx+x0), cy,   cz+z0);
            glColor3fv(mid);    glVertex3f(-(cx+x1), cy,   cz+z1);
            glColor3fv(top);    glVertex3f(-(cx+x1), cy+r, cz+z1);
            glColor3fv(top);    glVertex3f(-(cx+x0), cy+r, cz+z0);
        }
        /* Top cap */
        glColor3fv(top);
        glVertex3f(-(cx-r), cy+r, cz-r);
        glVertex3f(-(cx+r), cy+r, cz-r);
        glVertex3f(-(cx+r), cy+r, cz+r);
        glVertex3f(-(cx-r), cy+r, cz+r);
        glEnd();
        glColor3f(1,1,1);
    } else {
        /* Sky mesh from point ROM, rendered at camera with big scale */
        int model_id = (mode == 2) ? 0x30D : 0x30E;
        glMatrixMode(GL_MODELVIEW);
        glPushMatrix();
        /* Move the sky model to track the camera; its local coords are small
         * (~1000 units), so multiply by a scale that pushes it to horizon. */
        float sky_scale = 3000.0f;   /* game sky sphere is tiny in local units */
        /* Translate to camera, then scale */
        glTranslatef(-cam_x, cam_y, cam_z);  /* match render_model_at's X-flip */
        glScalef(sky_scale, sky_scale, sky_scale);
        /* render_model_at still negates X internally; to cancel the double
         * negate we apply another X flip here. Net result: sky uses same
         * handedness as the rest. */
        render_model_at(model_id, 0, 0, 0);
        glPopMatrix();
    }
    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);
}

/* ========== Full overview (no visibility culling) ==========
 * Iterates all 128 grid cells, skipping models whose geometry is just a
 * tiny placeholder (same heuristic as tools/assemble_full_course.py). Used
 * in top-down overview mode. */
static int is_placeholder_chunk(int model_id) {
    if (model_id <= 0 || model_id >= (int)g_pointrom_count) return 1;
    int a1 = pt(model_id);
    if (a1 <= 0 || a1 >= (int)g_pointrom_count) return 1;
    float mn[3]={1e18f,1e18f,1e18f}, mx[3]={-1e18f,-1e18f,-1e18f};
    int nv = 0;
    for (int obj = 0; obj < 500; obj++) {
        if (a1+obj >= (int)g_pointrom_count) break;
        int a2 = pt(a1+obj);
        if (a2 < 0 || a2 >= (int)g_pointrom_count) break;
        int cl = pt(a2);
        if (cl <= 0 || cl > 10000) continue;
        int pos = a2+1, fin = pos+cl;
        while (pos < fin && pos+1 < (int)g_pointrom_count) {
            int pl = pt(pos); pos++;
            if (pl <= 0 || pl > 0x20) break;
            if (pl >= 0x14) {
                for (int v = 0; v < 4; v++) {
                    int vi = pos+11+v*3;
                    if (vi+2 >= (int)g_pointrom_count) continue;
                    float x=(float)pt(vi), y=(float)pt(vi+1), z=(float)pt(vi+2);
                    if (x<mn[0])mn[0]=x; if (x>mx[0])mx[0]=x;
                    if (y<mn[1])mn[1]=y; if (y>mx[1])mx[1]=y;
                    if (z<mn[2])mn[2]=z; if (z>mx[2])mx[2]=z;
                    nv++;
                }
            }
            pos += pl;
        }
    }
    if (!nv) return 1;
    float max_dim = mx[0]-mn[0];
    if (mx[1]-mn[1] > max_dim) max_dim = mx[1]-mn[1];
    if (mx[2]-mn[2] > max_dim) max_dim = mx[2]-mn[2];
    return max_dim < 500.0f;   /* tiny 216×216 origin quads = placeholder */
}

static int render_course_overview(int course, int *chunks_out,
                                  float spacing_x, float spacing_z) {
    /* spacing_x / spacing_z = multiplier on CELL_SIZE for chunk placement.
     * 1.0 = game-accurate (chunks can overlap because chunk geometry often
     * extends multiple cells beyond its own center). >1.0 = pull chunks
     * apart so you can see each clearly. */
    int base = COURSE_TERRAIN_BASE[course];
    int total_quads = 0, chunks = 0;
    for (int z = 0; z < 16; z++) {
        for (int x = 0; x < 8; x++) {
            int model_id = base + z * 8 + x;
            if (is_placeholder_chunk(model_id)) continue;
            float wx = ((float)x + 0.5f) * CELL_SIZE * spacing_x;
            float wz = ((float)z + 0.5f) * CELL_SIZE * spacing_z;
            total_quads += render_model_at(model_id, wx, 0, wz);
            chunks++;
        }
    }
    if (chunks_out) *chunks_out = chunks;
    return total_quads;
}

/* Pick pass: render every chunk in a unique solid color (no textures, no
 * lighting) so a glReadPixels on the back buffer tells us which chunk is
 * under the cursor. Color = 0xFF0000 | (chunk_index+1), so pixel (0,0,0) =
 * background (miss), and index is recovered from R<<16|G<<8|B - 1. */
typedef struct { int model_id; int grid_x, grid_z; float wx, wz; } PickEntry;
static PickEntry pick_table[128];
static int       pick_table_count = 0;

static void render_pick_pass(int course, float spacing_x, float spacing_z) {
    int base = COURSE_TERRAIN_BASE[course];
    pick_table_count = 0;
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_ALPHA_TEST);
    for (int z = 0; z < 16; z++) {
        for (int x = 0; x < 8; x++) {
            int model_id = base + z * 8 + x;
            if (is_placeholder_chunk(model_id)) continue;
            float wx = ((float)x + 0.5f) * CELL_SIZE * spacing_x;
            float wz = ((float)z + 0.5f) * CELL_SIZE * spacing_z;
            int idx = pick_table_count + 1;   /* 1..N, reserve 0 = miss */
            pick_table[pick_table_count].model_id = model_id;
            pick_table[pick_table_count].grid_x = x;
            pick_table[pick_table_count].grid_z = z;
            pick_table[pick_table_count].wx = wx;
            pick_table[pick_table_count].wz = wz;
            pick_table_count++;
            /* Encode idx in RGB. With N<=128 we only need 8 bits but using
             * 16 gives us headroom for future combined-courses pick. */
            float r = ((idx >> 16) & 0xFF) / 255.0f;
            float g = ((idx >>  8) & 0xFF) / 255.0f;
            float b = ( idx        & 0xFF) / 255.0f;
            glColor3f(r, g, b);
            /* render_model_at uses its own glEnable(TEXTURE_2D); we bypass
             * that by calling a quad-only helper. Reuse render_model_at for
             * simplicity — textures still get bound but glTexEnv GL_MODULATE
             * multiplies texture color with glColor, which wrecks picking.
             * So we render flat-shaded quads directly here. */
            render_model_at(model_id, wx, 0, wz);
        }
    }
    glEnable(GL_ALPHA_TEST);
}

/* Read pixel at (mx, screen_height - my) and decode to chunk index (or -1). */
static int pick_read_pixel(int mx, int screen_h_minus_my) {
    uint8_t pix[3];
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(mx, screen_h_minus_my, 1, 1, GL_RGB, GL_UNSIGNED_BYTE, pix);
    int enc = (pix[0] << 16) | (pix[1] << 8) | pix[2];
    if (enc == 0) return -1;
    int idx = enc - 1;
    if (idx < 0 || idx >= pick_table_count) return -1;
    return idx;
}

/* ========== 6x8 bitmap font (basic ASCII only) ==========
 * Each glyph is 8 rows of 6-bit data packed into a byte. Bit 5 = leftmost
 * column. We only include printable ASCII 0x20-0x7E. */
static const uint8_t font6x8[96][8] = {
  /*   */ {0,0,0,0,0,0,0,0}, /* ! */ {0x20,0x20,0x20,0x20,0x20,0x00,0x20,0x00},
  /* " */ {0x50,0x50,0x50,0x00,0x00,0x00,0x00,0x00}, /* # */ {0x50,0x50,0xF8,0x50,0xF8,0x50,0x50,0x00},
  /* $ */ {0x20,0x78,0xA0,0x70,0x28,0xF0,0x20,0x00}, /* % */ {0xC8,0xC8,0x10,0x20,0x40,0x98,0x98,0x00},
  /* & */ {0x60,0x90,0xA0,0x40,0xA8,0x90,0x68,0x00}, /* ' */ {0x20,0x20,0x40,0x00,0x00,0x00,0x00,0x00},
  /* ( */ {0x10,0x20,0x40,0x40,0x40,0x20,0x10,0x00}, /* ) */ {0x40,0x20,0x10,0x10,0x10,0x20,0x40,0x00},
  /* * */ {0x00,0x20,0xA8,0x70,0xA8,0x20,0x00,0x00}, /* + */ {0x00,0x20,0x20,0xF8,0x20,0x20,0x00,0x00},
  /* , */ {0x00,0x00,0x00,0x00,0x00,0x20,0x20,0x40}, /* - */ {0x00,0x00,0x00,0xF8,0x00,0x00,0x00,0x00},
  /* . */ {0x00,0x00,0x00,0x00,0x00,0x00,0x20,0x00}, /* / */ {0x00,0x08,0x10,0x20,0x40,0x80,0x00,0x00},
  /* 0 */ {0x70,0x88,0x98,0xA8,0xC8,0x88,0x70,0x00}, /* 1 */ {0x20,0x60,0xA0,0x20,0x20,0x20,0xF8,0x00},
  /* 2 */ {0x70,0x88,0x08,0x10,0x20,0x40,0xF8,0x00}, /* 3 */ {0x70,0x88,0x08,0x30,0x08,0x88,0x70,0x00},
  /* 4 */ {0x10,0x30,0x50,0x90,0xF8,0x10,0x10,0x00}, /* 5 */ {0xF8,0x80,0xF0,0x08,0x08,0x88,0x70,0x00},
  /* 6 */ {0x30,0x40,0x80,0xF0,0x88,0x88,0x70,0x00}, /* 7 */ {0xF8,0x08,0x10,0x20,0x40,0x40,0x40,0x00},
  /* 8 */ {0x70,0x88,0x88,0x70,0x88,0x88,0x70,0x00}, /* 9 */ {0x70,0x88,0x88,0x78,0x08,0x10,0x60,0x00},
  /* : */ {0x00,0x20,0x00,0x00,0x00,0x20,0x00,0x00}, /* ; */ {0x00,0x20,0x00,0x00,0x00,0x20,0x20,0x40},
  /* < */ {0x10,0x20,0x40,0x80,0x40,0x20,0x10,0x00}, /* = */ {0x00,0x00,0xF8,0x00,0xF8,0x00,0x00,0x00},
  /* > */ {0x40,0x20,0x10,0x08,0x10,0x20,0x40,0x00}, /* ? */ {0x70,0x88,0x08,0x10,0x20,0x00,0x20,0x00},
  /* @ */ {0x70,0x88,0xA8,0xB8,0xB0,0x80,0x70,0x00}, /* A */ {0x70,0x88,0x88,0xF8,0x88,0x88,0x88,0x00},
  /* B */ {0xF0,0x88,0x88,0xF0,0x88,0x88,0xF0,0x00}, /* C */ {0x70,0x88,0x80,0x80,0x80,0x88,0x70,0x00},
  /* D */ {0xE0,0x90,0x88,0x88,0x88,0x90,0xE0,0x00}, /* E */ {0xF8,0x80,0x80,0xF0,0x80,0x80,0xF8,0x00},
  /* F */ {0xF8,0x80,0x80,0xF0,0x80,0x80,0x80,0x00}, /* G */ {0x70,0x88,0x80,0xB8,0x88,0x88,0x70,0x00},
  /* H */ {0x88,0x88,0x88,0xF8,0x88,0x88,0x88,0x00}, /* I */ {0x70,0x20,0x20,0x20,0x20,0x20,0x70,0x00},
  /* J */ {0x38,0x10,0x10,0x10,0x10,0x90,0x60,0x00}, /* K */ {0x88,0x90,0xA0,0xC0,0xA0,0x90,0x88,0x00},
  /* L */ {0x80,0x80,0x80,0x80,0x80,0x80,0xF8,0x00}, /* M */ {0x88,0xD8,0xA8,0x88,0x88,0x88,0x88,0x00},
  /* N */ {0x88,0xC8,0xA8,0x98,0x88,0x88,0x88,0x00}, /* O */ {0x70,0x88,0x88,0x88,0x88,0x88,0x70,0x00},
  /* P */ {0xF0,0x88,0x88,0xF0,0x80,0x80,0x80,0x00}, /* Q */ {0x70,0x88,0x88,0x88,0xA8,0x90,0x68,0x00},
  /* R */ {0xF0,0x88,0x88,0xF0,0xA0,0x90,0x88,0x00}, /* S */ {0x70,0x88,0x80,0x70,0x08,0x88,0x70,0x00},
  /* T */ {0xF8,0x20,0x20,0x20,0x20,0x20,0x20,0x00}, /* U */ {0x88,0x88,0x88,0x88,0x88,0x88,0x70,0x00},
  /* V */ {0x88,0x88,0x88,0x88,0x88,0x50,0x20,0x00}, /* W */ {0x88,0x88,0x88,0x88,0xA8,0xA8,0x50,0x00},
  /* X */ {0x88,0x88,0x50,0x20,0x50,0x88,0x88,0x00}, /* Y */ {0x88,0x88,0x50,0x20,0x20,0x20,0x20,0x00},
  /* Z */ {0xF8,0x08,0x10,0x20,0x40,0x80,0xF8,0x00}, /* [ */ {0x70,0x40,0x40,0x40,0x40,0x40,0x70,0x00},
  /* \ */ {0x00,0x80,0x40,0x20,0x10,0x08,0x00,0x00}, /* ] */ {0x70,0x10,0x10,0x10,0x10,0x10,0x70,0x00},
  /* ^ */ {0x20,0x50,0x88,0x00,0x00,0x00,0x00,0x00}, /* _ */ {0x00,0x00,0x00,0x00,0x00,0x00,0xF8,0x00},
  /* ` */ {0x40,0x20,0x10,0x00,0x00,0x00,0x00,0x00}, /* a */ {0x00,0x00,0x70,0x08,0x78,0x88,0x78,0x00},
  /* b */ {0x80,0x80,0xF0,0x88,0x88,0x88,0xF0,0x00}, /* c */ {0x00,0x00,0x70,0x88,0x80,0x88,0x70,0x00},
  /* d */ {0x08,0x08,0x78,0x88,0x88,0x88,0x78,0x00}, /* e */ {0x00,0x00,0x70,0x88,0xF8,0x80,0x70,0x00},
  /* f */ {0x30,0x48,0x40,0xE0,0x40,0x40,0x40,0x00}, /* g */ {0x00,0x00,0x78,0x88,0x88,0x78,0x08,0x70},
  /* h */ {0x80,0x80,0xF0,0x88,0x88,0x88,0x88,0x00}, /* i */ {0x20,0x00,0x60,0x20,0x20,0x20,0x70,0x00},
  /* j */ {0x10,0x00,0x30,0x10,0x10,0x10,0x90,0x60}, /* k */ {0x80,0x80,0x90,0xA0,0xC0,0xA0,0x90,0x00},
  /* l */ {0x60,0x20,0x20,0x20,0x20,0x20,0x70,0x00}, /* m */ {0x00,0x00,0xD0,0xA8,0xA8,0xA8,0xA8,0x00},
  /* n */ {0x00,0x00,0xB0,0xC8,0x88,0x88,0x88,0x00}, /* o */ {0x00,0x00,0x70,0x88,0x88,0x88,0x70,0x00},
  /* p */ {0x00,0x00,0xF0,0x88,0x88,0xF0,0x80,0x80}, /* q */ {0x00,0x00,0x78,0x88,0x88,0x78,0x08,0x08},
  /* r */ {0x00,0x00,0xB0,0xC8,0x80,0x80,0x80,0x00}, /* s */ {0x00,0x00,0x78,0x80,0x70,0x08,0xF0,0x00},
  /* t */ {0x40,0x40,0xE0,0x40,0x40,0x48,0x30,0x00}, /* u */ {0x00,0x00,0x88,0x88,0x88,0x88,0x78,0x00},
  /* v */ {0x00,0x00,0x88,0x88,0x88,0x50,0x20,0x00}, /* w */ {0x00,0x00,0x88,0xA8,0xA8,0xA8,0x50,0x00},
  /* x */ {0x00,0x00,0x88,0x50,0x20,0x50,0x88,0x00}, /* y */ {0x00,0x00,0x88,0x88,0x88,0x78,0x08,0x70},
  /* z */ {0x00,0x00,0xF8,0x10,0x20,0x40,0xF8,0x00}, /* { */ {0x10,0x20,0x20,0x40,0x20,0x20,0x10,0x00},
  /* | */ {0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x00}, /* } */ {0x40,0x20,0x20,0x10,0x20,0x20,0x40,0x00},
  /* ~ */ {0x48,0xA8,0x90,0x00,0x00,0x00,0x00,0x00}, /* DEL */ {0,0,0,0,0,0,0,0},
};

static void draw_text(float x, float y, const char *s, float r, float g, float b) {
    glColor3f(r, g, b);
    glBegin(GL_QUADS);
    for (const char *p = s; *p; p++) {
        int c = (unsigned char)*p;
        if (c < 0x20 || c > 0x7E) { x += 6; continue; }
        const uint8_t *glyph = font6x8[c - 0x20];
        for (int row = 0; row < 8; row++) {
            uint8_t bits = glyph[row];
            for (int col = 0; col < 6; col++) {
                if (bits & (0x80 >> col)) {
                    float px = x + col, py = y + row;
                    glVertex2f(px,     py);
                    glVertex2f(px + 1, py);
                    glVertex2f(px + 1, py + 1);
                    glVertex2f(px,     py + 1);
                }
            }
        }
        x += 6;
    }
    glEnd();
}

/* Footer: 24-px strip at bottom of window, text reads current selection. */
static void draw_footer(int win_w, int win_h, const char *msg) {
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    /* Pixel-space ortho with Y growing downward */
    glOrtho(0, win_w, win_h, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_ALPHA_TEST);
    const int FOOTER_H = 24;
    /* Background strip */
    glColor4f(0.05f, 0.05f, 0.10f, 1.0f);
    glBegin(GL_QUADS);
    glVertex2f(0, win_h - FOOTER_H);
    glVertex2f(win_w, win_h - FOOTER_H);
    glVertex2f(win_w, win_h);
    glVertex2f(0, win_h);
    glEnd();
    /* Top border line */
    glColor3f(0.4f, 0.6f, 0.9f);
    glBegin(GL_LINES);
    glVertex2f(0, win_h - FOOTER_H);
    glVertex2f(win_w, win_h - FOOTER_H);
    glEnd();
    /* Text — scale up font by glScalef so it's readable (8px → 16px) */
    glPushMatrix();
    glTranslatef(8, win_h - FOOTER_H + 4, 0);
    glScalef(2.0f, 2.0f, 1.0f);
    draw_text(0, 0, msg, 1.0f, 1.0f, 1.0f);
    glPopMatrix();
    glEnable(GL_ALPHA_TEST);
    glEnable(GL_DEPTH_TEST);
    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
}

/* ========== LIGHT+ button (pixel-space overlay, top-right) ========== */
#define LIGHT_BTN_W 120
#define LIGHT_BTN_H 32
#define LIGHT_BTN_MARGIN 10

static bool light_button_hit(int win_w, int mx, int my) {
    int bx = win_w - LIGHT_BTN_W - LIGHT_BTN_MARGIN;
    int by = LIGHT_BTN_MARGIN;
    return mx >= bx && mx < bx + LIGHT_BTN_W && my >= by && my < by + LIGHT_BTN_H;
}

static void draw_light_button(int win_w, int win_h, float boost) {
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrtho(0, win_w, win_h, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_ALPHA_TEST);

    int bx = win_w - LIGHT_BTN_W - LIGHT_BTN_MARGIN;
    int by = LIGHT_BTN_MARGIN;
    /* Fill */
    glColor4f(0.15f, 0.22f, 0.35f, 1.0f);
    glBegin(GL_QUADS);
    glVertex2f(bx, by);
    glVertex2f(bx + LIGHT_BTN_W, by);
    glVertex2f(bx + LIGHT_BTN_W, by + LIGHT_BTN_H);
    glVertex2f(bx, by + LIGHT_BTN_H);
    glEnd();
    /* Border */
    glColor3f(0.5f, 0.75f, 0.95f);
    glBegin(GL_LINE_LOOP);
    glVertex2f(bx + 0.5f, by + 0.5f);
    glVertex2f(bx + LIGHT_BTN_W - 0.5f, by + 0.5f);
    glVertex2f(bx + LIGHT_BTN_W - 0.5f, by + LIGHT_BTN_H - 0.5f);
    glVertex2f(bx + 0.5f, by + LIGHT_BTN_H - 0.5f);
    glEnd();
    /* Label: "LIGHT+ 1.00" (6x8 font scaled 2x) */
    char label[32];
    snprintf(label, sizeof(label), "LIGHT+ %.2f", boost);
    glPushMatrix();
    glTranslatef(bx + 8, by + 8, 0);
    glScalef(2.0f, 2.0f, 1.0f);
    draw_text(0, 0, label, 1.0f, 1.0f, 1.0f);
    glPopMatrix();

    glEnable(GL_ALPHA_TEST);
    glEnable(GL_DEPTH_TEST);
    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
}

/* ========== Grid overlay ========== */
static void draw_grid_overlay(int course) {
    int base = COURSE_TERRAIN_BASE[course];
    uint32_t bm = COURSE_TERRAIN_BITMASK[course];
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_DEPTH_TEST);
    glColor3f(0.25f, 0.25f, 0.25f);
    glBegin(GL_LINES);
    /* Draw 8x16 grid outline at y=0 */
    for (int z = 0; z <= 16; z++) {
        glVertex3f(-0.0f*CELL_SIZE, 0, (float)z*CELL_SIZE);
        glVertex3f(-8.0f*CELL_SIZE, 0, (float)z*CELL_SIZE);
    }
    for (int x = 0; x <= 8; x++) {
        glVertex3f(-(float)x*CELL_SIZE, 0, 0);
        glVertex3f(-(float)x*CELL_SIZE, 0, 16.0f*CELL_SIZE);
    }
    glEnd();
    glEnable(GL_DEPTH_TEST);
    (void)base; (void)bm;
}

/* ========== Main ========== */

int main(int argc, char *argv[]) {
    const char *rom_dir = "extracted";
    int course = 0;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') continue;
        if (i == 1) rom_dir = argv[i];
        else course = (int)strtol(argv[i], NULL, 0);
    }
    if (course < 0 || course > 3) course = 0;

    printf("Course Viewer — course %d, ROMs from %s\n", course, rom_dir);
    if (!load_roms(rom_dir)) { fprintf(stderr, "ROM load failed\n"); return 1; }

    if (SDL_Init(SDL_INIT_VIDEO) < 0) { fprintf(stderr, "SDL: %s\n", SDL_GetError()); return 1; }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
    SDL_Window *win = SDL_CreateWindow("Prop Cycle Course Viewer",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, WIN_W, WIN_H,
        SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN);
    SDL_GLContext ctx = SDL_GL_CreateContext(win);
    SDL_GL_SetSwapInterval(1);

    /* Fly-camera state (active when view_mode==1). Starts at default mid-grid. */
    int32_t cam_x = 4 * CELL_SIZE;
    int32_t cam_z = 2 * CELL_SIZE;
    int32_t cam_y = 200000;
    uint32_t heading = 0;
    float fly_pitch = 20.0f;   /* middle-mouse drag adjusts this (fly mode) */

    /* Middle-mouse drag state (fly mode = rotate camera) */
    bool mmb_down = false;
    int  mmb_last_x = 0, mmb_last_y = 0;

    /* View mode: 0 = top-down overview of the whole level,
     *            1 = fly camera + zone visibility dispatch (DEPRECATED).
     * SPACE toggle disabled; fly-mode code kept in place for future restore. */
    int view_mode = 0;

    bool grid_on   = true;
    bool freeze    = false;
    bool cull_bitmask = true;   /* C toggles (fly mode only) */
    int  sky_mode  = 1;          /* K cycles 0..3, default gradient */
    int  zone_override = -1;
    float overview_zoom = 1.0f;  /* mouse wheel in overview mode: smaller = closer */
    float overview_pan_x = 0.0f, overview_pan_z = 0.0f;
    /* Orbit camera for middle-mouse drag (3D-app style).
     *   yaw   — rotation around world Y (horizontal drag)
     *   pitch — tilt from top-down (90°) toward horizon (0°) (vertical drag) */
    float overview_yaw_deg   = 0.0f;
    float overview_pitch_deg = 90.0f;  /* start fully top-down */
    /* Spacing: 1.0 = game-accurate (chunks overlap because geometry extends
     * past cell boundaries). 2.016 = visually separated (matches the value
     * baked into tools/assemble_full_course.py and the GLB exports). Adjust
     * live with +/- or Shift+wheel. */
    float overview_spacing = 2.024f;

    /* Mouse pick state: clicking in overview highlights and prints the chunk
     * under the cursor. -1 = nothing selected. */
    int pending_pick_x = -1, pending_pick_y = -1;
    int selected_idx = -1;
    char footer_msg[256] = "click a chunk in overview mode to inspect it";

    glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    glEnable(GL_ALPHA_TEST);
    glAlphaFunc(GL_GREATER, 0.1f);

    int running = 1;
    Uint32 last_ticks = SDL_GetTicks();
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) running = 0;
            if (ev.type == SDL_MOUSEBUTTONDOWN &&
                ev.button.button == SDL_BUTTON_LEFT) {
                if (light_button_hit(WIN_W, ev.button.x, ev.button.y)) {
                    g_light_boost += 0.15f;
                    if (g_light_boost > 3.0f) g_light_boost = 3.0f;
                    printf("[LIGHT] boost=%.2f\n", g_light_boost);
                } else if (view_mode == 0) {
                    /* Defer the pick readback until after the scene is rendered */
                    pending_pick_x = ev.button.x;
                    pending_pick_y = ev.button.y;
                }
            }
            if (ev.type == SDL_MOUSEBUTTONDOWN &&
                ev.button.button == SDL_BUTTON_MIDDLE) {
                mmb_down = true;
                mmb_last_x = ev.button.x;
                mmb_last_y = ev.button.y;
            }
            if (ev.type == SDL_MOUSEBUTTONUP &&
                ev.button.button == SDL_BUTTON_MIDDLE) {
                mmb_down = false;
            }
            if (ev.type == SDL_MOUSEMOTION && mmb_down) {
                int dx = ev.motion.x - mmb_last_x;
                int dy = ev.motion.y - mmb_last_y;
                if (view_mode == 1) {
                    /* Fly-mode: dx -> heading, dy -> pitch (~128 u/px).
                     * 1 px ≈ 0.7° heading, 1 px ≈ 0.4° pitch. */
                    heading = (heading + (uint32_t)(dx * 128)) & 0xFFFF;
                    fly_pitch += dy * 0.4f;
                    if (fly_pitch < -89.0f) fly_pitch = -89.0f;
                    if (fly_pitch >  89.0f) fly_pitch =  89.0f;
                } else {
                    /* Overview orbit camera: horizontal drag = yaw around Y,
                     * vertical drag = pitch (tilt between top-down and horizon).
                     * Clamp pitch to top hemisphere to avoid flipping. */
                    overview_yaw_deg   += dx * 0.4f;
                    overview_pitch_deg -= dy * 0.4f;
                    if (overview_yaw_deg >  360.0f) overview_yaw_deg -= 360.0f;
                    if (overview_yaw_deg < -360.0f) overview_yaw_deg += 360.0f;
                    if (overview_pitch_deg <  1.0f) overview_pitch_deg =  1.0f;
                    if (overview_pitch_deg > 90.0f) overview_pitch_deg = 90.0f;
                }
                mmb_last_x = ev.motion.x;
                mmb_last_y = ev.motion.y;
            }
            if (ev.type == SDL_MOUSEWHEEL && view_mode == 0) {
                const Uint8 *k = SDL_GetKeyboardState(NULL);
                int ticks = (ev.wheel.y < 0 ? -ev.wheel.y : ev.wheel.y);
                if (k[SDL_SCANCODE_LSHIFT]) {
                    float factor = (ev.wheel.y > 0) ? 1.10f : 0.91f;
                    for (int n = ticks; n > 0; n--) overview_spacing *= factor;
                    if (overview_spacing < 1.0f) overview_spacing = 1.0f;
                    if (overview_spacing > 6.0f) overview_spacing = 6.0f;
                    printf("[SPACING] %.3fx  (cell_stride = %.0f game units = %.3f * CELL_SIZE)\n",
                           overview_spacing, CELL_SIZE * overview_spacing, overview_spacing);
                } else {
                    /* Smooth zoom: larger step when far (zoom >= 1), smaller
                     * step when close so zooming in doesn't jump. pct goes
                     * from 7% at zoom=0 to 15% at zoom=1+. */
                    float z = overview_zoom > 1.0f ? 1.0f : overview_zoom;
                    float pct = 0.07f + 0.08f * z;
                    float factor = (ev.wheel.y > 0) ? (1.0f - pct) : (1.0f + pct);
                    for (int n = ticks; n > 0; n--) overview_zoom *= factor;
                    if (overview_zoom < 0.05f) overview_zoom = 0.05f;
                    if (overview_zoom > 8.0f)  overview_zoom = 8.0f;
                    printf("[ZOOM]    %.3fx\n", overview_zoom);
                }
            }
            if (ev.type == SDL_KEYDOWN) {
                switch (ev.key.keysym.sym) {
                case SDLK_ESCAPE: running = 0; break;
                case SDLK_1: course = 0; break;
                case SDLK_2: course = 1; break;
                case SDLK_3: course = 2; break;
                case SDLK_4: course = 3; break;
                /* SDLK_SPACE toggle removed: fly mode deprecated for now.
                 * Restore by reinstating: case SDLK_SPACE: view_mode = 1 - view_mode; break; */
                case SDLK_EQUALS: case SDLK_KP_PLUS:
                    overview_spacing *= 1.15f;
                    if (overview_spacing > 6.0f) overview_spacing = 6.0f;
                    printf("[SPACING] %.3fx  (cell_stride = %.0f game units = %.3f * CELL_SIZE)\n",
                           overview_spacing, CELL_SIZE * overview_spacing, overview_spacing);
                    break;
                case SDLK_MINUS: case SDLK_KP_MINUS:
                    overview_spacing *= 0.87f;
                    if (overview_spacing < 1.0f) overview_spacing = 1.0f;
                    printf("[SPACING] %.3fx  (cell_stride = %.0f game units = %.3f * CELL_SIZE)\n",
                           overview_spacing, CELL_SIZE * overview_spacing, overview_spacing);
                    break;
                case SDLK_0:
                    overview_spacing = 1.0f;
                    printf("[SPACING] reset to 1.000x\n");
                    break;
                case SDLK_p:
                    /* Dump current settings in a copy-pasteable form */
                    printf("[STATE] course=%d view_mode=%s zoom=%.3f spacing=%.3f "
                           "pan=(%.0f,%.0f) cam=(%d,%d,%d) heading=%u sky=%d cull=%d\n",
                           course, view_mode ? "fly" : "overview",
                           overview_zoom, overview_spacing,
                           overview_pan_x, overview_pan_z,
                           cam_x, cam_y, cam_z, heading, sky_mode, cull_bitmask);
                    break;
                case SDLK_f: freeze = !freeze; break;
                case SDLK_g: grid_on = !grid_on; break;
                case SDLK_c: cull_bitmask = !cull_bitmask; break;
                case SDLK_k: sky_mode = (sky_mode + 1) % 4; break;
                case SDLK_z:
                    zone_override = (zone_override < 0) ? 0 : (zone_override + 1) % 32;
                    if (zone_override == 0) zone_override = -1;
                    break;
                default: break;
                }
            }
        }
        const Uint8 *keys = SDL_GetKeyboardState(NULL);
        Uint32 now = SDL_GetTicks();
        float dt = (now - last_ticks) / 1000.0f;
        last_ticks = now;
        float spd = (keys[SDL_SCANCODE_LSHIFT] ? 200000.0f : 20000.0f) * dt;
        float turn = (keys[SDL_SCANCODE_LSHIFT] ? 65536.0f : 16384.0f) * dt;

        if (view_mode == 1) {
            /* Fly camera input */
            float hf = (float)(heading) * (2.0f * 3.14159265f / 65536.0f);
            float fwd_x = sinf(hf), fwd_z = cosf(hf);
            float rt_x  = cosf(hf), rt_z = -sinf(hf);
            if (keys[SDL_SCANCODE_W]) { cam_x += (int32_t)(fwd_x * spd); cam_z += (int32_t)(fwd_z * spd); }
            if (keys[SDL_SCANCODE_S]) { cam_x -= (int32_t)(fwd_x * spd); cam_z -= (int32_t)(fwd_z * spd); }
            if (keys[SDL_SCANCODE_A]) { cam_x += (int32_t)(rt_x  * spd); cam_z += (int32_t)(rt_z  * spd); }
            if (keys[SDL_SCANCODE_D]) { cam_x -= (int32_t)(rt_x  * spd); cam_z -= (int32_t)(rt_z  * spd); }
            if (keys[SDL_SCANCODE_Q]) cam_y -= (int32_t)spd;
            if (keys[SDL_SCANCODE_E]) cam_y += (int32_t)spd;
            if (keys[SDL_SCANCODE_LEFT])  heading = (heading - (uint32_t)turn) & 0xFFFF;
            if (keys[SDL_SCANCODE_RIGHT]) heading = (heading + (uint32_t)turn) & 0xFFFF;
        }

        /* Camera for visibility dispatch: either live or frozen */
        static int32_t frozen_x, frozen_z; static uint32_t frozen_heading;
        int32_t vis_x, vis_z; uint32_t vis_heading;
        if (freeze) { vis_x = frozen_x; vis_z = frozen_z; vis_heading = frozen_heading; }
        else       { vis_x = cam_x;   vis_z = cam_z;   vis_heading = heading;
                     frozen_x = cam_x; frozen_z = cam_z; frozen_heading = heading; }
        if (zone_override >= 0) {
            vis_heading = (uint32_t)(zone_override * 2048 - 0x400 + 1024) & 0xFFFF;
        }

        glClearColor(0.08f, 0.09f, 0.14f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LESS);

        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        float aspect = 1024.0f/768.0f;
        float near_p = 1000.0f, far_p = 8000000.0f;
        float top = near_p * tanf(45.0f * 3.14159f / 360.0f);
        float right_p = top * aspect;
        glFrustum(-right_p, right_p, -top, top, near_p, far_p);

        ChunkStats stats = {0};
        int total_quads = 0;
        int overview_chunks = 0;
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();

        if (view_mode == 0) {
            /* Top-down overview: look straight down at the center of the 8x16
             * grid. overview_zoom scales the camera height (mouse wheel).
             * WASD panning also works here — drags the view along the grid. */
            /* Pan speed: constant in world-units at zoom=1, but when you
             * zoom in (zoom < 1) we *boost* the speed instead of shrinking
             * it, so close-up exploration stays fast. max(zoom, 1) floors
             * the multiplier at 1.0 (so zoomed-in pans at full speed), and
             * the 1/zoom boost when zoom<1 keeps screen-pixel motion fast. */
            /* Gentler pan speed at close zoom: sqrt(1/zoom) caps the boost so
             * zoom=0.1 gives ~3.2x instead of 10x (keeps panning responsive
             * without being jumpy in tight views). */
            float zoom_factor = (overview_zoom < 1.0f)
                                  ? sqrtf(1.0f / overview_zoom)
                                  :  overview_zoom;
            float pan_speed = 4.0f * CELL_SIZE * dt *
                              (keys[SDL_SCANCODE_LSHIFT] ? 10.0f : 1.0f) *
                              zoom_factor;
            /* Camera-relative WASD: always forward/back/left/right on screen,
             * regardless of yaw. Pan is along the ground plane (XZ), projected
             * from the camera's screen-up and screen-right vectors. Vertices
             * are X-flipped by render_model_at, so signs on the game-space
             * X component are negated. Reduces to the previous behavior at
             * yaw=0. */
            float yaw_rad = overview_yaw_deg * (3.14159265f / 180.0f);
            float sy = sinf(yaw_rad), cy = cosf(yaw_rad);
            if (keys[SDL_SCANCODE_W]) {
                overview_pan_x +=  sy * pan_speed;
                overview_pan_z += -cy * pan_speed;
            }
            if (keys[SDL_SCANCODE_S]) {
                overview_pan_x += -sy * pan_speed;
                overview_pan_z +=  cy * pan_speed;
            }
            if (keys[SDL_SCANCODE_A]) {
                overview_pan_x +=  cy * pan_speed;
                overview_pan_z +=  sy * pan_speed;
            }
            if (keys[SDL_SCANCODE_D]) {
                overview_pan_x += -cy * pan_speed;
                overview_pan_z += -sy * pan_speed;
            }

            /* Orbit camera — target = grid center + pan, distance = zoom * extent.
             * At yaw=0, pitch=90 this reproduces the original top-down view
             * (verified: glRotatef(+90, X) * glTranslate(cx, -dist, -cz)).
             * pitch goes from 90 (straight down) to ~1 (nearly horizontal).
             *
             * View matrix: Tr(0,0,-dist) * Rx(+pitch) * Ry(-yaw) * Tr(+target_x, 0, -target_z).
             * (+target_x because vertices have X flipped by render_model_at.)
             * draw_grid_overlay(), the pick pass, and the sky all inherit
             * this matrix, so they rotate/tilt with the view. */
            float target_x = 4.0f * CELL_SIZE * overview_spacing + overview_pan_x;
            float target_z = 8.0f * CELL_SIZE * overview_spacing + overview_pan_z;
            float dist     = 2.2f * 16.0f * CELL_SIZE * overview_zoom * overview_spacing;
            float center_x = target_x, center_z = target_z, look_y = dist;  /* for sky/grid hints */
            glTranslatef(0.0f, 0.0f, -dist);
            glRotatef(overview_pitch_deg, 1, 0, 0);
            glRotatef(-overview_yaw_deg,  0, 1, 0);
            glTranslatef(target_x, 0.0f, -target_z);

            /* If a click is pending, do a hidden pick pass first. Clear to
             * black, render each chunk in a unique solid color, read the pixel
             * under the cursor, then clear and re-render the normal scene. */
            if (pending_pick_x >= 0) {
                glClearColor(0, 0, 0, 1);
                glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
                render_pick_pass(course, overview_spacing, overview_spacing);
                int idx = pick_read_pixel(pending_pick_x, WIN_H - pending_pick_y);
                if (idx >= 0) {
                    selected_idx = idx;
                    PickEntry *e = &pick_table[idx];
                    snprintf(footer_msg, sizeof(footer_msg),
                             "selected: model 0x%X (%d)  grid (%d, %d)  "
                             "world (%.0f, 0, %.0f)  course %d",
                             e->model_id, e->model_id, e->grid_x, e->grid_z,
                             e->wx, e->wz, course);
                    printf("[PICK] %s\n", footer_msg);
                } else {
                    selected_idx = -1;
                    snprintf(footer_msg, sizeof(footer_msg),
                             "clicked empty space (no chunk under cursor)");
                }
                pending_pick_x = pending_pick_y = -1;
                /* Clear for the real render pass */
                glClearColor(0.08f, 0.09f, 0.14f, 1.0f);
                glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            }

            draw_sky(sky_mode, center_x, look_y, center_z);
            total_quads = render_course_overview(course, &overview_chunks,
                                                 overview_spacing, overview_spacing);

            /* Highlight: red wireframe bbox around the selected chunk. */
            if (selected_idx >= 0 && selected_idx < pick_table_count) {
                PickEntry *e = &pick_table[selected_idx];
                float half = 0.5f * CELL_SIZE * overview_spacing;
                float x0 = e->wx - half, x1 = e->wx + half;
                float z0 = e->wz - half, z1 = e->wz + half;
                glDisable(GL_TEXTURE_2D);
                glDisable(GL_DEPTH_TEST);
                glLineWidth(3.0f);
                glColor3f(1.0f, 0.3f, 0.3f);
                glBegin(GL_LINE_LOOP);
                glVertex3f(-x0, 0, z0);
                glVertex3f(-x1, 0, z0);
                glVertex3f(-x1, 0, z1);
                glVertex3f(-x0, 0, z1);
                glEnd();
                glLineWidth(1.0f);
                glEnable(GL_DEPTH_TEST);
            }
        } else {
            /* Fly camera: rotate world by -heading, translate to -camera. */
            float heading_deg = -(float)heading * (360.0f / 65536.0f);
            glRotatef(fly_pitch, 1, 0, 0);
            glRotatef(heading_deg, 0, 1, 0);
            glTranslatef((float)cam_x, -(float)cam_y, -(float)cam_z);
            draw_sky(sky_mode, (float)cam_x, (float)cam_y, (float)cam_z);
            total_quads = render_terrain_for_camera(course, vis_x, vis_z, vis_heading,
                                                    &stats, cull_bitmask);
        }

        if (grid_on) draw_grid_overlay(course);

        glDisable(GL_DEPTH_TEST);
        char title[256];
        const char *sky_name[] = {"off", "gradient", "sky0x30D", "sky0x30E"};
        if (view_mode == 0) {
            snprintf(title, sizeof(title),
                     "Course %d  OVERVIEW  chunks=%d  quads=%d  zoom=%.2fx  "
                     "spacing=%.2fx  sky=%s  "
                     "[wheel=zoom  Shift+wheel or +/-=spacing  0=reset  WASD=pan  MMB=rotate]",
                     course, overview_chunks, total_quads, overview_zoom,
                     overview_spacing, sky_name[sky_mode]);
        } else {
            snprintf(title, sizeof(title),
                     "Course %d  FLY zone %d%s  cam=(%.0f,%.0f,%.0f) h=%u  "
                     "chunks=%d/%d  quads=%d  cull=%s  sky=%s%s",
                     course, stats.zone_used,
                     zone_override >= 0 ? "*" : "",
                     (float)cam_x, (float)cam_y, (float)cam_z, heading,
                     stats.chunks_emitted, stats.chunks_emitted+stats.chunks_rejected,
                     total_quads,
                     cull_bitmask ? "on" : "off",
                     sky_name[sky_mode],
                     freeze ? " [FROZEN]" : "");
        }
        SDL_SetWindowTitle(win, title);
        draw_footer(WIN_W, WIN_H, footer_msg);
        draw_light_button(WIN_W, WIN_H, g_light_boost);
        SDL_GL_SwapWindow(win);
    }

    SDL_GL_DeleteContext(ctx);
    SDL_DestroyWindow(win);
    SDL_Quit();
    free(g_texture_data);
    free(g_texture_tilemap);
    return 0;
}
