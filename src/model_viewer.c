/*
 * Standalone 3D Model Viewer
 * Loads ROMs directly, renders models with per-quad baked textures.
 * No game state machine — just ROM data + OpenGL.
 *
 * Usage: ./model_viewer [rom_dir] [start_model_hex]
 * Keys: M/Right = next model, N/Left = prev model, Q/Esc = quit
 *        WASD = rotate, Shift = fast
 */
#include <SDL2/SDL.h>
#include <GL/gl.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ========== ROM Data ========== */

#define ROM_SIZE        0x400000
#define POINTROM_SIZE   (512 * 1024 * 3)
#define TEXTURE_TOTAL_SIZE (0x200000 * 8)
#define TEXTUREMAP_SIZE (0x280000)
#define PALETTE_GROUPS  128
#define PALETTE_ENTRIES 256

static uint8_t  g_rom[ROM_SIZE];
static int32_t  g_pointrom[POINTROM_SIZE];
static uint32_t g_pointrom_count;
static uint8_t *g_texture_data;
static uint8_t *g_texture_tilemap;

/* Palette: 128 groups x 256 entries x 3 bytes (RGB) */
static uint8_t g_palette[PALETTE_GROUPS][PALETTE_ENTRIES][3];

/* ========== ROM Loading ========== */

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

static int32_t signed24(uint32_t v) { return (v & 0x800000) ? (int32_t)(v | 0xFF000000) : (int32_t)v; }

static bool load_roms(const char *dir) {
    /* Program ROM (for palette data) */
    uint8_t *r1=malloc(0x100000), *r2=malloc(0x100000), *r3=malloc(0x100000), *r4=malloc(0x100000);
    if (!r1||!r2||!r3||!r4) return false;
    bool ok = true;
    ok = ok && load_file_at(dir,"pr2ver-a.1",r1,0,0x100000);
    ok = ok && load_file_at(dir,"pr2ver-a.2",r2,0,0x100000);
    ok = ok && load_file_at(dir,"pr2ver-a.3",r3,0,0x100000);
    ok = ok && load_file_at(dir,"pr2ver-a.4",r4,0,0x100000);
    if (ok) {
        size_t i;
        for (i = 0; i < 0x100000; i++) {
            g_rom[i*4+0]=r4[i]; g_rom[i*4+1]=r3[i]; g_rom[i*4+2]=r2[i]; g_rom[i*4+3]=r1[i];
        }
    }
    free(r1); free(r2); free(r3); free(r4);
    if (!ok) return false;

    /* Point ROM */
    size_t CHIP = 0x80000, PLANE = CHIP*3;
    uint8_t *lo=calloc(PLANE,1), *mi=calloc(PLANE,1), *hi=calloc(PLANE,1);
    if (!lo||!mi||!hi) return false;
    ok = ok && load_file_at(dir,"pr1ptrl0.18k",lo,CHIP*0,CHIP);
    ok = ok && load_file_at(dir,"pr1ptrl1.16k",lo,CHIP*1,CHIP);
    ok = ok && load_file_at(dir,"pr1ptrl2.15k",lo,CHIP*2,CHIP);
    ok = ok && load_file_at(dir,"pr1ptrm0.18j",mi,CHIP*0,CHIP);
    ok = ok && load_file_at(dir,"pr1ptrm1.16j",mi,CHIP*1,CHIP);
    ok = ok && load_file_at(dir,"pr1ptrm2.15j",mi,CHIP*2,CHIP);
    ok = ok && load_file_at(dir,"pr1ptru0.18f",hi,CHIP*0,CHIP);
    ok = ok && load_file_at(dir,"pr1ptru1.16f",hi,CHIP*1,CHIP);
    ok = ok && load_file_at(dir,"pr1ptru2.15f",hi,CHIP*2,CHIP);
    if (ok) {
        size_t i;
        g_pointrom_count = PLANE;
        for (i = 0; i < g_pointrom_count; i++)
            g_pointrom[i] = signed24(((uint32_t)hi[i]<<16)|((uint32_t)mi[i]<<8)|lo[i]);
        printf("  Point ROM: %u entries\n", g_pointrom_count);
    }
    free(lo); free(mi); free(hi);

    /* Texture tiles */
    g_texture_data = calloc(TEXTURE_TOTAL_SIZE, 1);
    if (!g_texture_data) return false;
    {
        const char *cg[] = {"pr1cg0.12b","pr1cg1.10d","pr1cg2.12d","pr1cg3.13d",
                            "pr1cg4.14d","pr1cg5.16d","pr1cg6.18a","pr1cg7.15a"};
        int i;
        for (i = 0; i < 8; i++)
            ok = ok && load_file_at(dir, cg[i], g_texture_data, 0x200000*i, 0x200000);
    }

    /* Texture tilemap */
    g_texture_tilemap = calloc(TEXTUREMAP_SIZE, 1);
    if (!g_texture_tilemap) return false;
    ok = ok && load_file_at(dir,"pr1ccrl.3d", g_texture_tilemap, 0, 0x200000);
    ok = ok && load_file_at(dir,"pr1ccrh.1d", g_texture_tilemap, 0x200000, 0x80000);

    /* Palette: try MAME runtime dump first, fall back to ROM static */
    {
        char path[512];
        FILE *mf;
        int loaded_mame = 0;
        snprintf(path, sizeof(path), "%s/palette_mame_runtime.bin", dir);
        mf = fopen(path, "rb");
        if (mf) {
            uint8_t buf[128*256*3];
            if (fread(buf, 1, sizeof(buf), mf) == sizeof(buf)) {
                int g, p;
                for (g = 0; g < 128; g++)
                    for (p = 0; p < 256; p++) {
                        int off = (g*256+p)*3;
                        g_palette[g][p][0] = buf[off];
                        g_palette[g][p][1] = buf[off+1];
                        g_palette[g][p][2] = buf[off+2];
                    }
                loaded_mame = 1;
                printf("  Palette: MAME runtime dump\n");
            }
            fclose(mf);
        }
        if (!loaded_mame) {
            /* Group 0: packed RGB at 0xB5EC0 */
            int p;
            for (p = 0; p < 256; p++) {
                int s = 0xB5EC0 + p*3;
                g_palette[0][p][0]=g_rom[s]; g_palette[0][p][1]=g_rom[s+1]; g_palette[0][p][2]=g_rom[s+2];
            }
            /* Groups 1-127: planar at 0x21B204 */
            int g;
            for (g = 0; g < 127; g++) {
                int b = 0x21B204 + g*768;
                for (p = 0; p < 256; p++) {
                    g_palette[g+1][p][0]=g_rom[b+p];
                    g_palette[g+1][p][1]=g_rom[b+256+p];
                    g_palette[g+1][p][2]=g_rom[b+512+p];
                }
            }
            printf("  Palette: ROM static\n");
        }
    }
    return ok;
}

/* ========== Texture Pipeline ========== */

static int get_tile_attr(int ti) {
    int byte_idx = ti >> 1;
    int attr_off = 0x200000 + byte_idx;
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
    u &= 0xFFF;
    v = (v & 0xFFF) | (texbank * 0x1000);
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
           (g_palette[pal_group][pen][1] << 8) |
            g_palette[pal_group][pen][2];
}

/* ========== Texture Cache ========== */

#define TEX_CACHE_SIZE 4096
#define TEX_CACHE_MASK (TEX_CACHE_SIZE - 1)

typedef struct {
    uint16_t min_u, min_v, range_u, range_v;
    uint8_t texbank, pal_group, occupied, _pad;
    GLuint gl_tex;
} TexCacheEntry;

static TexCacheEntry tex_cache[TEX_CACHE_SIZE];
static int tex_cache_total = 0;
static uint8_t tex_pixels[256*256*4];

static uint32_t tex_hash(int mu, int mv, int ru, int rv, int tb, int pg) {
    uint32_t h = 2166136261u;
    h ^= (uint32_t)mu; h *= 16777619u;
    h ^= (uint32_t)mv; h *= 16777619u;
    h ^= (uint32_t)ru; h *= 16777619u;
    h ^= (uint32_t)rv; h *= 16777619u;
    h ^= (uint32_t)tb; h *= 16777619u;
    h ^= (uint32_t)pg; h *= 16777619u;
    return h;
}

/* MV_OPAQUE=1: do not key black texels to transparent.
 * MV_NOLIGHT=1: drop the viewer's invented per-face shading.
 * Both are viewer-only inventions the game does not do -- see the note in
 * render_model(). Read once; getenv in a draw loop is register row 40. */
static int mv_opaque = -1, mv_nolight = -1, mv_repeat = 0, mv_uvtest = 0;
static void mv_flags(void) {
    if (mv_opaque < 0) {
        const char *e = getenv("MV_OPAQUE");   mv_opaque  = (e && *e && *e != '0');
        e = getenv("MV_NOLIGHT");              mv_nolight = (e && *e && *e != '0');
        e = getenv("MV_REPEAT");               mv_repeat  = (e && *e && *e != '0');
        e = getenv("MV_UVTEST");               mv_uvtest  = (e && *e && *e != '0');
    }
}

static GLuint bake_texture(int min_u, int min_v, int range_u, int range_v,
                           int texbank, int pal_group) {
    int probe;
    if (range_u < 16) { min_u = (min_u + min_u + range_u)/2 - 8; range_u = 16; }
    if (range_v < 16) { min_v = (min_v + min_v + range_v)/2 - 8; range_v = 16; }
    if (min_u < 0) min_u = 0;
    if (min_v < 0) min_v = 0;

    uint32_t h = tex_hash(min_u, min_v, range_u, range_v, texbank, pal_group);
    for (probe = 0; probe < 16; probe++) {
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

    int px, py;
    for (py = 0; py < th; py++) {
        for (px = 0; px < tw; px++) {
            int tu = min_u + px*step_u;
            int tv = min_v + py*step_v;
            uint8_t pen = tex_pen_lookup(tu, tv, texbank);
            uint32_t rgb = pen_to_rgb(pen, pal_group);
            int idx = (py*tw+px)*4;
            if (mv_uvtest) {
                /* Encode the ABSOLUTE tilemap coordinate this texel stands
                 * for, so a rendered pixel's colour says which (u,v) it
                 * sampled. Compared against the same encoding from a
                 * per-pixel reference, this isolates the MAPPING from the
                 * texture content, the palette and the lighting. */
                tex_pixels[idx]   = (uint8_t)(tu & 0xFF);
                tex_pixels[idx+1] = (uint8_t)(tv & 0xFF);
                tex_pixels[idx+2] = 0;
                tex_pixels[idx+3] = 255;
                continue;
            }
            if (rgb == 0 && !mv_opaque) {
                tex_pixels[idx]=tex_pixels[idx+1]=tex_pixels[idx+2]=0; tex_pixels[idx+3]=0;
            } else {
                tex_pixels[idx]=(rgb>>16)&0xFF; tex_pixels[idx+1]=(rgb>>8)&0xFF;
                tex_pixels[idx+2]=rgb&0xFF; tex_pixels[idx+3]=255;
            }
        }
    }

    /* MV_DUMPTEX=<dir>: write every baked texture as a PPM named by its own
     * parameters, so the BAKE can be diffed against direct tilemap sampling
     * without any camera or rasteriser in the way. */
    {
        const char *dd = getenv("MV_DUMPTEX");
        if (dd && *dd) {
            char fn[512];
            snprintf(fn, sizeof fn, "%s/bake_u%d_v%d_ru%d_rv%d_b%d_p%d.ppm",
                     dd, min_u, min_v, range_u, range_v, texbank, pal_group);
            FILE *f = fopen(fn, "wb");
            if (f) {
                fprintf(f, "P6\n%d %d\n255\n", tw, th);
                for (int yy = 0; yy < th; yy++)
                    for (int xx = 0; xx < tw; xx++) {
                        int o = (yy*tw+xx)*4;
                        fwrite(&tex_pixels[o], 1, 3, f);
                    }
                fclose(f);
            }
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
    {   /* MV_REPEAT=1 restores the old (broken) GL_REPEAT, for A/B. */
        GLint wrap = mv_repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap);
    }
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tw, th, 0, GL_RGBA, GL_UNSIGNED_BYTE, tex_pixels);

    for (probe = 0; probe < 16; probe++) {
        TexCacheEntry *e = &tex_cache[(h+probe) & TEX_CACHE_MASK];
        if (!e->occupied) {
            e->min_u=min_u; e->min_v=min_v; e->range_u=range_u; e->range_v=range_v;
            e->texbank=texbank; e->pal_group=pal_group; e->gl_tex=tex; e->occupied=1;
            tex_cache_total++;
            break;
        }
    }
    return tex;
}

/* ========== Model Rendering ========== */

static int32_t pt(uint32_t addr) {
    return (addr < g_pointrom_count) ? g_pointrom[addr] : 0;
}

static int render_model(int code, float ox, float oy, float oz) {
    int total = 0, obj;
    mv_flags();
    if (code <= 0 || code >= (int)g_pointrom_count) return 0;
    int addr1 = pt(code);
    if (addr1 < 0 || addr1 >= (int)g_pointrom_count) return 0;

    for (obj = 0; obj < 500; obj++) {
        int addr2, chunk_len, pos, finish;
        if (addr1+obj >= (int)g_pointrom_count) break;
        addr2 = pt(addr1+obj);
        if (addr2 < 0 || addr2 >= (int)g_pointrom_count) break;
        chunk_len = pt(addr2);
        if (chunk_len <= 0 || chunk_len > 10000) continue;
        pos = addr2+1; finish = pos+chunk_len;

        while (pos < finish && pos+1 < (int)g_pointrom_count) {
            int pkt_len = pt(pos); pos++;
            int base = pos;
            if (pkt_len <= 0 || pkt_len > 0x20) break;
            if (base+pkt_len > (int)g_pointrom_count) break;

            if (pkt_len >= 0x14) {
                int tex0 = pt(base+2) & 0xFFFFFF;
                int pal_group = (tex0 >> 8) & 0x7F;
                int texbank = (pt(base+5) >> 12) & 0xF;

                int uv[4][2];
                float verts[4][3];
                float max_c = 0;
                int v;

                for (v = 0; v < 4; v++) {
                    uv[v][0] = pt(base+4+v*2) & 0xFFF;
                    uv[v][1] = pt(base+5+v*2) & 0xFFF;
                }
                for (v = 0; v < 4; v++) {
                    int vi = base+12+v*3;
                    float vx=(float)pt(vi), vy=(float)pt(vi+1), vz=(float)pt(vi+2);
                    verts[v][0] = -(vx+ox);  /* negate X for correct handedness */
                    verts[v][1] = vy+oy;
                    verts[v][2] = vz+oz;
                    if (fabsf(vx)>max_c) max_c=fabsf(vx);
                    if (fabsf(vy)>max_c) max_c=fabsf(vy);
                    if (fabsf(vz)>max_c) max_c=fabsf(vz);
                }
                if (max_c < 1 || max_c > 500000) { pos += pkt_len; continue; }

                /* Face normal for lighting */
                float ax=verts[1][0]-verts[0][0], ay=verts[1][1]-verts[0][1], az=verts[1][2]-verts[0][2];
                float bx=verts[2][0]-verts[0][0], by=verts[2][1]-verts[0][1], bz=verts[2][2]-verts[0][2];
                float nx=ay*bz-az*by, ny=az*bx-ax*bz, nz=ax*by-ay*bx;
                float nlen=sqrtf(nx*nx+ny*ny+nz*nz);
                if (nlen>0) { nx/=nlen; ny/=nlen; nz/=nlen; }
                /* THE VIEWER INVENTS THIS SHADING -- the hardware does not
                 * light a model this way, and on a terrain chunk (many small
                 * quads at slightly different angles) it paints a hard seam
                 * at every quad edge, which reads as the texture being
                 * broken up. MV_NOLIGHT=1 turns it off. */
                float light = mv_nolight ? 1.0f
                            : 0.35f + 0.65f * fabsf(nx*0.35f + ny*0.55f + nz*0.5f);

                /* UV bounding box */
                int u_min=uv[0][0], u_max=uv[0][0], v_min=uv[0][1], v_max=uv[0][1];
                int i;
                for (i=1;i<4;i++) {
                    if (uv[i][0]<u_min) u_min=uv[i][0]; if (uv[i][0]>u_max) u_max=uv[i][0];
                    if (uv[i][1]<v_min) v_min=uv[i][1]; if (uv[i][1]>v_max) v_max=uv[i][1];
                }
                int ru=u_max-u_min; if (ru<1) ru=1;
                int rv=v_max-v_min; if (rv<1) rv=1;

                GLuint qtex = bake_texture(u_min, v_min, ru, rv, texbank, pal_group);

                /* Recompute after degenerate expansion */
                int bu=u_min, bv=v_min, bru=ru, brv=rv;
                if (bru<16) { bu=(u_min+u_max)/2-8; bru=16; }
                if (brv<16) { bv=(v_min+v_max)/2-8; brv=16; }
                if (bu<0) bu=0; if (bv<0) bv=0;

                glEnable(GL_TEXTURE_2D);
                glBindTexture(GL_TEXTURE_2D, qtex);
                glColor3f(light, light, light);
                glBegin(GL_QUADS);
                for (v=0;v<4;v++) {
                    float tu = (float)(uv[v][0]-bu)/(float)bru;
                    float tv = (float)(uv[v][1]-bv)/(float)brv;
                    glTexCoord2f(tu, tv);
                    glVertex3f(verts[v][0], verts[v][1], verts[v][2]);
                }
                glEnd();
                glDisable(GL_TEXTURE_2D);
                total++;
            }
            pos += pkt_len;
        }
    }
    return total;
}

/* ========== Bounding Box ========== */

static void model_bounds(int code, float *cx, float *cy, float *cz, float *radius) {
    int obj;
    float mn[3]={1e18f,1e18f,1e18f}, mx[3]={-1e18f,-1e18f,-1e18f};
    int nverts = 0;
    *cx=*cy=*cz=0; *radius=1;
    if (code<=0||code>=(int)g_pointrom_count) return;
    int a1 = pt(code);
    if (a1<0||a1>=(int)g_pointrom_count) return;

    for (obj=0;obj<500;obj++) {
        int a2,cl,pos,fin;
        if (a1+obj>=(int)g_pointrom_count) break;
        a2=pt(a1+obj); if(a2<0) break; if(a2>=(int)g_pointrom_count) break;
        cl=pt(a2); if(cl<=0||cl>10000) continue;
        pos=a2+1; fin=pos+cl;
        while (pos<fin && pos+1<(int)g_pointrom_count) {
            int pl=pt(pos); pos++;
            if (pl<=0||pl>0x20) break;
            if (pl>=0x14) {
                int v;
                for (v=0;v<4;v++) {
                    int vi=pos+12+v*3;
                    if (vi+2<(int)g_pointrom_count) {
                        /* NEGATE X, exactly as render_model() does when it
                         * builds the drawn vertices. It did not, so the centre
                         * this returns was the centre of the UN-mirrored model
                         * and `glTranslatef(-cx,..)` mis-centred every model by
                         * twice its x centre -- invisible at the old framing
                         * because everything was too far away to notice, and
                         * the reason a zoomed model slid off into a corner. */
                        float p0=-(float)pt(vi),p1=(float)pt(vi+1),p2=(float)pt(vi+2);
                        if(p0<mn[0])mn[0]=p0; if(p0>mx[0])mx[0]=p0;
                        if(p1<mn[1])mn[1]=p1; if(p1>mx[1])mx[1]=p1;
                        if(p2<mn[2])mn[2]=p2; if(p2>mx[2])mx[2]=p2;
                        nverts++;
                    }
                }
            }
            pos+=pl;
        }
    }
    if (!nverts) return;
    *cx=(mn[0]+mx[0])*0.5f; *cy=(mn[1]+mx[1])*0.5f; *cz=(mn[2]+mx[2])*0.5f;
    /* BOUNDING-SPHERE radius -- half the diagonal, not the largest single
     * extent. The old value was the full width, and `cam_dist = radius*2.5`
     * then pushed the camera 2.5 WIDTHS back: a terrain chunk (very wide,
     * very flat, seen at a shallow pitch) shrank to a sliver a few dozen
     * pixels tall, and all its texture detail aliased into noise. That is
     * what "1311 looks garbled" was -- the texture is fine, it was just
     * minified into nothing. */
    float d0=mx[0]-mn[0], d1=mx[1]-mn[1], d2=mx[2]-mn[2];
    *radius = 0.5f * sqrtf(d0*d0 + d1*d1 + d2*d2);
    if (*radius<1) *radius=1;
}

/* ========== Find Valid Models ========== */

static int valid_models[8192];
static int valid_count = 0;

static void scan_models(void) {
    int mid;
    valid_count = 0;
    for (mid = 1; mid < (int)g_pointrom_count && mid < 8192; mid++) {
        int a1 = pt(mid);
        if (a1 <= 0 || a1 >= (int)g_pointrom_count) continue;
        int a2 = pt(a1);
        if (a2 <= 0 || a2 >= (int)g_pointrom_count) continue;
        int cl = pt(a2);
        if (cl <= 0 || cl > 10000) continue;
        /* Has at least one quad? */
        int pos = a2+1, fin = pos+cl, has_quad = 0;
        while (pos < fin && pos+1 < (int)g_pointrom_count) {
            int pl = pt(pos); pos++;
            if (pl <= 0 || pl > 0x20) break;
            if (pl >= 0x14) { has_quad = 1; break; }
            pos += pl;
        }
        if (has_quad) valid_models[valid_count++] = mid;
    }
    printf("  Found %d valid models\n", valid_count);
}

/* Index of the first valid model at or after model_id, or -1 if there is
 * none. It used to `return 0` in that case, which is indistinguishable from
 * a real hit on the first model -- ask for a model that does not exist and
 * you silently got model 1 with nothing said about it. */
static int find_model_index(int model_id) {
    int i;
    for (i = 0; i < valid_count; i++)
        if (valid_models[i] >= model_id) return i;
    return -1;
}

/* Jump to a model id, saying plainly what happened. Not every id in the
 * point ROM is a drawable model -- scan_models() keeps only those with at
 * least one quad -- so an exact hit is not guaranteed and the caller should
 * be told when it snapped somewhere else. */
static int goto_model(int want, int fallback) {
    int idx = find_model_index(want);
    if (idx < 0) {
        printf("  no model at or after %d (highest is %d) -- staying put\n",
               want, valid_count ? valid_models[valid_count-1] : 0);
        return fallback;
    }
    if (valid_models[idx] != want)
        printf("  model %d is not a drawable model; showing %d instead\n",
               want, valid_models[idx]);
    return idx;
}

/* ========== Main ========== */

/* Write the framebuffer as a PPM, so what the viewer draws can be looked at
 * and diffed without a human watching a window. */
static void save_ppm(const char *path, int w, int h) {
    unsigned char *px = (unsigned char *)malloc((size_t)w * h * 3);
    if (!px) return;
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, px);
    FILE *f = fopen(path, "wb");
    if (f) {
        fprintf(f, "P6\n%d %d\n255\n", w, h);
        for (int y = h - 1; y >= 0; y--)          /* GL is bottom-up */
            fwrite(px + (size_t)y * w * 3, 1, (size_t)w * 3, f);
        fclose(f);
        printf("  wrote %s (%dx%d)\n", path, w, h);
    }
    free(px);
}

int main(int argc, char *argv[]) {
    const char *rom_dir = "extracted";
    const char *shot_path = NULL;
    int shot_frame = 2;          /* let the first frame settle */
    int start_model = 0x2CF;
    float start_zoom = 1.0f, start_yaw = 30.0f, start_pitch = -20.0f;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--zoom") == 0 && i + 1 < argc) {
            start_zoom = (float)atof(argv[++i]);
            if (start_zoom < 0.05f) start_zoom = 0.05f;
        } else if (strcmp(argv[i], "--yaw") == 0 && i + 1 < argc) {
            start_yaw = (float)atof(argv[++i]);
        } else if (strcmp(argv[i], "--pitch") == 0 && i + 1 < argc) {
            start_pitch = (float)atof(argv[++i]);
        } else if (strcmp(argv[i], "--shot") == 0 && i + 1 < argc) {
            shot_path = argv[++i];
            if (i + 1 < argc && argv[i+1][0] != '-' &&
                argv[i+1][0] >= '0' && argv[i+1][0] <= '9')
                shot_frame = atoi(argv[++i]);
        } else if (argv[i][0] != '-') {
            if (rom_dir == NULL || i == 1) rom_dir = argv[i];
            else start_model = (int)strtol(argv[i], NULL, 0);
        }
    }

    printf("Model Viewer — loading ROMs from %s\n", rom_dir);
    if (!load_roms(rom_dir)) { fprintf(stderr, "ROM load failed\n"); return 1; }

    scan_models();
    int cur_idx = goto_model(start_model, 0);
    printf("  type a model NUMBER and press Enter to jump to it"
           " (Backspace edits, Esc cancels); M/N or the arrow keys step\n");

    if (SDL_Init(SDL_INIT_VIDEO) < 0) { fprintf(stderr, "SDL: %s\n", SDL_GetError()); return 1; }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);

    SDL_Window *win = SDL_CreateWindow("Prop Cycle Model Viewer",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 800, 600,
        SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN);
    SDL_GLContext ctx = SDL_GL_CreateContext(win);
    SDL_GL_SetSwapInterval(1);

    float rot_y = start_yaw, rot_x = start_pitch;
    int running = 1;
    int need_print = 1;
    /* TYPE-A-NUMBER-AND-ENTER jump. This viewer has no text rendering, so
     * the partially typed id is echoed in the window title bar. */
    char jump_buf[8] = {0};
    int  jump_len = 0, title_dirty = 1;
    float zoom = start_zoom, pan_x = 0.0f, pan_y = 0.0f;

    glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    glEnable(GL_ALPHA_TEST);
    glAlphaFunc(GL_GREATER, 0.1f);

    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) running = 0;
            if (ev.type == SDL_MOUSEWHEEL) {
                if (ev.wheel.y > 0) zoom *= 1.15f;
                else if (ev.wheel.y < 0) zoom /= 1.15f;
                if (zoom > 200.0f) zoom = 200.0f;
                if (zoom < 0.05f) zoom = 0.05f;
            }
            if (ev.type == SDL_KEYDOWN) {
                SDL_Keycode k = ev.key.keysym.sym;
                int digit = -1;
                if (k >= SDLK_0 && k <= SDLK_9)            digit = k - SDLK_0;
                else if (k >= SDLK_KP_1 && k <= SDLK_KP_9) digit = k - SDLK_KP_1 + 1;
                else if (k == SDLK_KP_0)                   digit = 0;

                if (digit >= 0) {
                    if (jump_len < (int)sizeof(jump_buf) - 1) {
                        jump_buf[jump_len++] = (char)('0' + digit);
                        jump_buf[jump_len] = '\0';
                        title_dirty = 1;
                    }
                    continue;
                }
                if (k == SDLK_BACKSPACE) {
                    if (jump_len) jump_buf[--jump_len] = '\0';
                    title_dirty = 1;
                    continue;
                }
                if (k == SDLK_RETURN || k == SDLK_KP_ENTER) {
                    if (jump_len) {
                        cur_idx = goto_model(atoi(jump_buf), cur_idx);
                        jump_len = 0; jump_buf[0] = '\0';
                        need_print = 1; title_dirty = 1;
                    }
                    continue;
                }
                if (k == SDLK_ESCAPE && jump_len) {   /* cancel, do NOT quit */
                    jump_len = 0; jump_buf[0] = '\0';
                    title_dirty = 1;
                    continue;
                }

                switch (k) {
                case SDLK_ESCAPE: case SDLK_q: running = 0; break;
                case SDLK_RIGHT: case SDLK_m: case SDLK_PERIOD:
                    if (cur_idx < valid_count-1) cur_idx++;
                    else cur_idx = 0;
                    need_print = 1;
                    break;
                case SDLK_LEFT: case SDLK_n: case SDLK_COMMA:
                    if (cur_idx > 0) cur_idx--;
                    else cur_idx = valid_count-1;
                    need_print = 1;
                    break;
                case SDLK_PAGEUP:
                    cur_idx = (cur_idx + 10) % valid_count;
                    need_print = 1;
                    break;
                case SDLK_PAGEDOWN:
                    cur_idx = (cur_idx - 10 + valid_count) % valid_count;
                    need_print = 1;
                    break;
                /* ZOOM and PAN -- the viewer had neither, so a model that
                 * framed badly could not be inspected at all. */
                case SDLK_EQUALS: case SDLK_PLUS: case SDLK_KP_PLUS:
                    zoom *= 1.25f; if (zoom > 200.0f) zoom = 200.0f; break;
                case SDLK_MINUS: case SDLK_KP_MINUS:
                    zoom /= 1.25f; if (zoom < 0.05f) zoom = 0.05f; break;
                case SDLK_r:
                    zoom = 1.0f; pan_x = pan_y = 0.0f;
                    rot_y = 30.0f; rot_x = -20.0f;
                    printf("  view reset\n"); break;
                case SDLK_UP:    pan_y -= 0.08f; break;
                case SDLK_DOWN:  pan_y += 0.08f; break;
                default: break;
                }
            }
        }

        /* Continuous rotation with arrow keys held */
        const Uint8 *keys = SDL_GetKeyboardState(NULL);
        float rspd = keys[SDL_SCANCODE_LSHIFT] ? 3.0f : 0.8f;
        if (keys[SDL_SCANCODE_A]) rot_y -= rspd;
        if (keys[SDL_SCANCODE_D]) rot_y += rspd;
        if (keys[SDL_SCANCODE_W]) rot_x -= rspd;
        if (keys[SDL_SCANCODE_S]) rot_x += rspd;

        int model_id = valid_models[cur_idx];
        if (need_print) title_dirty = 1;
        if (title_dirty) {
            char title[128];
            if (jump_len)
                snprintf(title, sizeof title,
                         "Prop Cycle Model Viewer  --  go to model: %s_", jump_buf);
            else
                snprintf(title, sizeof title,
                         "Prop Cycle Model Viewer  --  model %d (0x%X)  [%d/%d]",
                         model_id, model_id, cur_idx + 1, valid_count);
            SDL_SetWindowTitle(win, title);
            title_dirty = 0;
        }
        if (need_print) {
            printf("Model 0x%03X (%d) [%d/%d]\n", model_id, model_id, cur_idx+1, valid_count);
            need_print = 0;
        }

        float cx, cy, cz, radius;
        model_bounds(model_id, &cx, &cy, &cz, &radius);
        /* Distance at which a sphere of this radius exactly fills the 45 deg
         * vertical FOV, then divided by the user's zoom. */
        float cam_dist = radius / sinf(45.0f * 3.14159265f / 360.0f) / zoom;

        glClearColor(0.12f, 0.12f, 0.18f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LESS);

        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        float aspect = 800.0f / 600.0f;
        float near_p = cam_dist * 0.01f, far_p = cam_dist * 100.0f;
        float top = near_p * tanf(45.0f * 3.14159f / 360.0f);
        float right_p = top * aspect;
        glFrustum(-right_p, right_p, -top, top, near_p, far_p);

        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
        glTranslatef(pan_x * radius, -pan_y * radius, -cam_dist);
        glRotatef(rot_x, 1, 0, 0);
        glRotatef(rot_y, 0, 1, 0);
        glTranslatef(-cx, -cy, -cz);

        int nquads = render_model(model_id, 0, 0, 0);
        (void)nquads;

        glDisable(GL_DEPTH_TEST);

        /* HUD text via window title */
        {
            char title[128];
            snprintf(title, sizeof(title), "Model 0x%03X (%d) — %d quads — [%d/%d] — M/N cycle, WASD rotate",
                     model_id, model_id, nquads, cur_idx+1, valid_count);
            SDL_SetWindowTitle(win, title);
        }

        SDL_GL_SwapWindow(win);

        if (shot_path && --shot_frame <= 0) {
            save_ppm(shot_path, 800, 600);
            running = 0;
        }
        rot_y += 0.3f;  /* slow auto-rotate */
    }

    SDL_GL_DeleteContext(ctx);
    SDL_DestroyWindow(win);
    SDL_Quit();
    free(g_texture_data);
    free(g_texture_tilemap);
    return 0;
}
