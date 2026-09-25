/*
 * rr_video.c -- System 22 video, a C port of MAME's namcos22_v.cpp (the
 * non-super paths only), kept as close to the original as C allows so that a
 * pixel difference against MAME is a porting bug, not a design difference.
 *
 *  simulate_slavedsp   the slave DSP's job: walk the master's display list
 *                      at polygon RAM 0x2FF (viewport 0x15, options 0x10,
 *                      view 0x0a, primitive 0x0d records) and turn point-ROM
 *                      objects into quads
 *  draw_direct_poly    quads the master sends straight to the render device
 *  scene               MAME's radix tree == sort by zsort, farthest first;
 *                      equal zsort draws the LAST submitted first
 *  poly3d_drawquad     z-clip, project, fog/shade setup; poly.h's
 *                      render_triangle rules (round_coordinate, +0.5 centres)
 *  renderscanline_poly texture (tilemap + attribute + 16x16x8 tile), palette,
 *                      per-poly fog, shading AFTER fog (System 22 order)
 *  text layer + mix    16x16 4bpp tiles from CG RAM, pen 0xF transparent,
 *                      priority 2 over polys unless the poly set prioverchar;
 *                      shadow pens, global fade, gamma PROMs
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <limits.h>
#include "rr_mem.h"
#include "rr_dsp.h"
#include "rr_video.h"
#include "rr_scene.h"

#define NW 640                         /* the board's own picture */
#define NH 480

uint32_t g_frame_rgb[NW * NH];         /* 0x00RRGGBB, final (gamma applied) */

/* THE OUTPUT SIZE. The hardware draws 640x480 and so does this, exactly, by
 * default -- every gate and --shots run. A windowed player may ask for another
 * RENDER size (rr_video_set_size): the same scene is rasterised at that many
 * pixels, scale vs = H / 480 on both axes, and when W is wider than 4:3 the
 * extra width is WIDESCREEN (vE native pixels added each side): viewports that
 * cover the whole screen widen to fill it, everything else keeps its place in
 * the 4:3 centre, and the text layer stays in the centre too. The game still
 * only emits what its own 4:3 camera sees, so the side strips can show late
 * pop-in. At 640x480 the original code path runs untouched. */
static int W = NW, H = NH;
static double vs = 1.0, vE = 0.0;
static bool vnative = true;
static int want_w = NW, want_h = NH;
static uint32_t *out = g_frame_rgb;
static int16_t *tmap_x, *tmap_y;       /* output pixel -> native text-layer pixel, -1 = none */

/* ------------------------------------------------------------ ROM data ---- */
static uint8_t  *ttdata;               /* 16x16x8 texture tiles, 0x10000 tiles */
static uint16_t *ttmap;                /* texture tilemap, 0x100000 entries   */
static uint8_t  *ttattr;               /* unpacked 4-bit attributes           */
static uint8_t   tt_ayx_to_pixel[16 * 16 * 16];
static uint8_t   gamma_prom[0x300];

static uint8_t *slurp(const char *dir, const char *name, size_t n)
{
    char p[1024];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    FILE *f = fopen(p, "rb");
    if (!f) { fprintf(stderr, "[VID] cannot open %s\n", p); return NULL; }
    uint8_t *b = malloc(n);
    size_t got = fread(b, 1, n, f);
    fclose(f);
    if (got != n) { fprintf(stderr, "[VID] short %s\n", p); free(b); return NULL; }
    return b;
}

void rr_video_set_size(int w, int h);
bool rr_video_init(const char *dir)
{
    /* RR_RENDER_SIZE=<w>x<h>: render at another size headlessly (--shots) */
    { const char *e = getenv("RR_RENDER_SIZE"); int w, h;
      if (e && sscanf(e, "%dx%d", &w, &h) == 2) rr_video_set_size(w, h); }
    static const char *cg[8] = { "rv1cg0.1a", "rv1cg1.1c", "rv1cg2.1d", "rv1cg3.1e",
                                 "rv1cg4.1f", "rv1cg5.1j", "rv1cg6.1k", "rv1cg7.1n" };
    ttdata = malloc(0x1000000);
    for (int i = 0; i < 8; i++) {
        uint8_t *b = slurp(dir, cg[i], 0x200000);
        if (!b) return false;
        memcpy(ttdata + i * 0x200000, b, 0x200000);
        free(b);
    }
    uint8_t *l = slurp(dir, "rv1ccrl.5a", 0x200000), *h = slurp(dir, "rv1ccrh.5c", 0x80000);
    if (!l || !h) return false;
    ttmap = malloc(0x100000 * sizeof *ttmap);
    for (int i = 0; i < 0x100000; i++) ttmap[i] = (uint16_t)(l[2 * i] | l[2 * i + 1] << 8);   /* ROM_REGION16_LE */
    ttattr = malloc(0x100000);
    for (int i = 0; i < 0x80000; i++) { ttattr[2 * i] = h[i] >> 4; ttattr[2 * i + 1] = h[i] & 0xF; }
    free(l); free(h);
    for (int attr = 0; attr < 16; attr++)
        for (int y = 0; y < 16; y++)
            for (int x = 0; x < 16; x++) {
                int ix = x, iy = y;
                if (attr & 4) ix = 15 - ix;
                if (attr & 2) iy = 15 - iy;
                if (attr & 8) { int t = ix; ix = iy; iy = t; }
                tt_ayx_to_pixel[attr << 8 | y << 4 | x] = (uint8_t)(iy << 4 | ix);
            }
    static const char *gp[3] = { "rr1gam.2d", "rr1gam.3d", "rr1gam.4d" };
    for (int i = 0; i < 3; i++) {
        uint8_t *b = slurp(dir, gp[i], 0x100);
        if (!b) return false;
        memcpy(gamma_prom + i * 0x100, b, 0x100); free(b);
    }
    return true;
}

/* -------------------------------------------------------- MAME helpers ---- */
static inline int32_t signed12(int32_t v) { return (int32_t)((uint32_t)v << 20) >> 20; }
static inline int32_t signed18(int32_t v) { return (int32_t)((uint32_t)v << 14) >> 14; }
static inline int32_t signed24(int32_t v) { return (int32_t)((uint32_t)v << 8) >> 8; }
static inline float dspfixed_to_nativefloat(int16_t v) { return v / (float)0x7fff; }
static float dspfloat_to_nativefloat(uint32_t val)
{
    const int16_t mantissa = (int16_t)val;
    float result = (float)mantissa;
    int exponent = val >> 16 & 0x3f;
    while (exponent < 0x2e) { result /= 2.0f; exponent++; }
    return result;
}
static inline int32_t point_read(uint32_t offs)
{
    offs &= 0xffffff;
    if (offs < g_pointrom_words) return g_pointrom[offs];
    return rr_dsp_pointram_read(offs);
}
static inline uint8_t mixer_b(int n) { return g_rr.mixer[n & (RR_MIXER_SIZE - 1)]; }

/* palette: planar R/G/B, 0x8000 pens each, big-endian bytes as written */
static uint32_t pen_rgb(int pen)
{
    pen &= 0x7fff;
    return (uint32_t)g_rr.pal[pen] << 16 | (uint32_t)g_rr.pal[pen + 0x8000] << 8 | g_rr.pal[pen + 0x10000];
}

/* rgbaint_t, the three operations the S22 path uses */
typedef struct { int32_t r, g, b; } rgbi;
static inline rgbi rgb_set(uint32_t c) { rgbi v = { (int32_t)(c >> 16 & 0xff), (int32_t)(c >> 8 & 0xff), (int32_t)(c & 0xff) }; return v; }
static inline void rgb_blend(rgbi *a, rgbi o, int factor)
{
    const int32_t s1 = factor, s2 = 256 - s1;
    a->r = (a->r * s1 + o.r * s2) >> 8;
    a->g = (a->g * s1 + o.g * s2) >> 8;
    a->b = (a->b * s1 + o.b * s2) >> 8;
}
static inline int32_t clamp8(int32_t v) { return v < 0 ? 0 : v > 255 ? 255 : v; }
static inline void rgb_scale_imm_clamp(rgbi *a, int32_t s)
{
    a->r = clamp8((a->r * s) >> 8); a->g = clamp8((a->g * s) >> 8); a->b = clamp8((a->b * s) >> 8);
}
static inline void rgb_scale_clamp(rgbi *a, rgbi s)
{
    a->r = clamp8((a->r * s.r) >> 8); a->g = clamp8((a->g * s.g) >> 8); a->b = clamp8((a->b * s.b) >> 8);
}
static inline uint32_t rgb_pack(rgbi v) { return (uint32_t)v.r << 16 | (uint32_t)v.g << 8 | (uint32_t)v.b; }

/* ------------------------------------------------------- scene nodes ---- */
typedef struct { float x, y, z; int u, v, bri; } polyvertex;
typedef struct {
    uint32_t zsort, seq;
    int vx, vy, vu, vd, vl, vr;
    int texturebank, color, cmode, cz_value, cz_type, cz_adjust, objectflags;
    bool direct;
    polyvertex v[4];
} quadnode;

static quadnode *nodes;
static uint32_t n_nodes, cap_nodes, node_seq;

static quadnode *new_quad(uint32_t zsort)
{
    if (n_nodes == cap_nodes) {
        cap_nodes = cap_nodes ? cap_nodes * 2 : 8192;
        nodes = realloc(nodes, cap_nodes * sizeof *nodes);
    }
    quadnode *q = &nodes[n_nodes++];
    memset(q, 0, sizeof *q);
    q->zsort = zsort & 0xffffff;
    q->seq = node_seq++;
    return q;
}

/* --------------------------------------------------- slave DSP state ---- */
static float    viewmatrix[4][4];
static int      cz_adjust, objectflags, absolute_priority, objectshift;
static uint8_t  reflection;
static bool     cullflip;
static uint8_t  LitSurfaceInfo[0x80];
static unsigned LitSurfaceCount, LitSurfaceIndex;
static int      camera_vx, camera_vy, camera_vu, camera_vd, camera_vl, camera_vr;
static float    camera_zoom, camera_lx, camera_ly, camera_lz;
static int      camera_ambient, camera_power;

static void matrix3d_multiply(float a[4][4], float b[4][4])
{
    float result[4][4];
    for (int row = 0; row < 4; row++)
        for (int col = 0; col < 4; col++) {
            float sum = 0.0f;
            for (int i = 0; i < 4; i++) sum += a[row][i] * b[i][col];
            result[row][col] = sum;
        }
    memcpy(a, result, sizeof result);
}
static void matrix3d_identity(float m[4][4])
{
    for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) m[r][c] = r == c ? 1.0f : 0.0f;
}
static void matrix3d_apply_reflection(float m[4][4])
{
    if (!reflection) return;
    float r[4][4];
    matrix3d_identity(r);
    if (reflection & 0x10) r[0][0] = -1.0f;
    if (reflection & 0x20) r[1][1] = -1.0f;
    matrix3d_multiply(m, r);
}
static void transform_point(float *vx, float *vy, float *vz, float m[4][4])
{
    float x = *vx, y = *vy, z = *vz;
    *vx = m[0][0]*x + m[1][0]*y + m[2][0]*z + m[3][0];
    *vy = m[0][1]*x + m[1][1]*y + m[2][1]*z + m[3][1];
    *vz = m[0][2]*x + m[1][2]*y + m[2][2]*z + m[3][2];
}
static void transform_normal(float *nx, float *ny, float *nz, float m[4][4])
{
    float x = *nx, y = *ny, z = *nz;
    *nx = m[0][0]*x + m[1][0]*y + m[2][0]*z;
    *ny = m[0][1]*x + m[1][1]*y + m[2][1]*z;
    *nz = m[0][2]*x + m[1][2]*y + m[2][2]*z;
}
static void register_normals(int addr, float m[4][4])
{
    for (int i = 0; i < 4; i++) {
        float nx = dspfixed_to_nativefloat((int16_t)point_read(addr + i * 3 + 0));
        float ny = dspfixed_to_nativefloat((int16_t)point_read(addr + i * 3 + 1));
        float nz = dspfixed_to_nativefloat((int16_t)point_read(addr + i * 3 + 2));
        transform_normal(&nx, &ny, &nz, m);
        float cx = camera_lx, cy = camera_ly, cz = camera_lz;
        transform_normal(&cx, &cy, &cz, viewmatrix);
        float dot = nx*cx + ny*cy + nz*cz;
        if (dot < 0.0f) dot = 0.0f;
        if (LitSurfaceCount < 0x80)
            LitSurfaceInfo[LitSurfaceCount++] = (uint8_t)(camera_ambient + camera_power * dot);
    }
}

static void blit_single_quad(uint32_t color, uint32_t addr, float m[4][4], int polyshift, int flags, int packetformat)
{
    polyvertex v[4];
    for (int i = 0; i < 4; i++) {
        polyvertex *pv = &v[i];
        pv->x = (float)point_read(0x8 + i * 3 + addr);
        pv->y = (float)point_read(0x9 + i * 3 + addr);
        pv->z = (float)point_read(0xa + i * 3 + addr);
        transform_point(&pv->x, &pv->y, &pv->z, m);
    }
    float zmin = 0.0f, zmax = 0.0f;
    for (int i = 0; i < 4; i++) {
        if (i == 0 || v[i].z > zmax) zmax = v[i].z;
        if (i == 0 || v[i].z < zmin) zmin = v[i].z;
    }
    if (zmax < 0.0f) return;
    if (flags & 0x20) {
        float c1 = (v[2].x*((v[0].z*v[1].y)-(v[0].y*v[1].z))) + (v[2].y*((v[0].x*v[1].z)-(v[0].z*v[1].x))) +
                   (v[2].z*((v[0].y*v[1].x)-(v[0].x*v[1].y)));
        float c2 = (v[0].x*((v[2].z*v[3].y)-(v[2].y*v[3].z))) + (v[0].y*((v[2].x*v[3].z)-(v[2].z*v[3].x))) +
                   (v[0].z*((v[2].y*v[3].x)-(v[2].x*v[3].y)));
        if ((cullflip && c1 <= 0.0f && c2 <= 0.0f) || (!cullflip && c1 >= 0.0f && c2 >= 0.0f)) return;
    }
    int zsort;
    if (zmin < 0.0f) zmin = 0.0f;
    switch (flags & 0x300) {
    case 0x000: zsort = (int)(zmin + 0.5f); break;
    case 0x100: zsort = (int)(zmax + 0.5f); break;
    default:    zsort = (int)((zmin + zmax) / 2.0f + 0.5f); break;
    }
    if (zsort > 0x1fffff) zsort = 0x1fffff;
    int ap = absolute_priority & 7;
    if (polyshift & 0x200000) zsort = polyshift & 0x1fffff;
    else { zsort += signed18(polyshift); ap += (polyshift & 0x1c0000) >> 18; }
    if (objectshift & 0x200000) zsort = objectshift & 0x1fffff;
    else { zsort += signed18(objectshift); ap += (objectshift & 0x1c0000) >> 18; }
    if (zsort < 0) zsort = 0;
    if (zsort > 0x1fffff) zsort = 0x1fffff;
    ap &= 7;
    zsort |= ap << 21;
    if (zmax < 0.0f) zmax = 0.0f;
    if (zmax > (float)0x1fffff) zmax = (float)0x1fffff;
    int cz_value = (int)(zmax + 0.5f);

    for (int i = 0; i < 4; i++) {
        int bri;
        v[i].u = point_read(0 + i * 2 + addr);
        v[i].v = point_read(1 + i * 2 + addr);
        if (LitSurfaceCount > 0) {
            unsigned index = LitSurfaceIndex++;
            if (LitSurfaceCount > 4) index >>= 2;
            index %= LitSurfaceCount;
            bri = LitSurfaceInfo[index];
        } else if (packetformat & 0x40)
            bri = point_read(i + addr) >> 16 & 0xff;
        else
            bri = color >> 16 & 0xff;
        v[i].bri = bri;
    }
    quadnode *q = new_quad((uint32_t)zsort);
    q->cmode = v[0].u >> 12 & 0xf;
    q->texturebank = v[0].v >> 12 & 0xf;
    q->color = color >> 8 & 0xff;
    q->cz_value = cz_value >> 8;
    q->cz_type = flags >> 10 & 3;
    q->cz_adjust = cz_adjust;
    q->objectflags = objectflags;
    for (int i = 0; i < 4; i++) {
        polyvertex *p = &q->v[i];
        p->x = v[i].x * camera_zoom;
        p->y = v[i].y * camera_zoom;
        p->z = v[i].z;
        p->u = v[i].u & 0xfff;
        p->v = v[i].v & 0xfff;
        p->bri = v[i].bri;
    }
    q->direct = false;
    q->vx = camera_vx; q->vy = camera_vy;
    q->vu = camera_vu; q->vd = camera_vd; q->vl = camera_vl; q->vr = camera_vr;
}

static void blit_quads(int addr, int len, float m[4][4])
{
    const int finish = addr + len;
    while (addr < finish) {
        const int packetlength = point_read(addr++);
        const int packetformat = point_read(addr + 0);
        int flags, color, bias;
        switch (packetlength) {
        case 0x17:
            flags = point_read(addr + 1); color = point_read(addr + 2); bias = 0;
            blit_single_quad((uint32_t)color, addr + 3, m, bias, flags, packetformat);
            break;
        case 0x18:
            flags = point_read(addr + 1); color = point_read(addr + 2); bias = point_read(addr + 3);
            blit_single_quad((uint32_t)color, addr + 4, m, bias, flags, packetformat);
            break;
        case 0x10:
            LitSurfaceCount = 0; LitSurfaceIndex = 0;
            register_normals(addr + 4, m);
            break;
        case 0x0d:
            register_normals(addr + 1, m);
            break;
        default:
            return;
        }
        addr += packetlength;
        if ((packetformat & 0x800000) && addr != finish) return;
    }
}

static void blit_polyobject(int code, float m[4][4])
{
    const bool pointram = code == 0x5;
    int list_addr = pointram ? 0xf00000 : point_read(code);
    LitSurfaceCount = 0; LitSurfaceIndex = 0;
    for (int guard = 0; guard < 0x10000; guard++) {
        int object_addr = point_read(list_addr++);
        if (object_addr < 0) {
            if (object_addr == -1) break;
            else if (!pointram) break;
            object_addr &= 0x00ffffff;
        }
        const uint32_t chunklength = (uint32_t)point_read(object_addr++);
        if (chunklength > 0x100) break;
        blit_quads(object_addr, (int)chunklength, m);
    }
    objectflags &= ~2;
}

static void handle_bb0003(const int32_t *src)
{
    camera_ambient = src[0x1] >> 16 & 0xffff;
    camera_power = src[0x1] & 0xffff;
    camera_lx = dspfixed_to_nativefloat((int16_t)src[0x2]);
    camera_ly = dspfixed_to_nativefloat((int16_t)src[0x3]);
    camera_lz = dspfixed_to_nativefloat((int16_t)src[0x4]);
    absolute_priority = src[0x3] >> 16 & 0xffff;
    camera_vx = signed12(src[0x5] >> 16);
    camera_vy = signed12(src[0x5] & 0xffff);
    camera_zoom = dspfloat_to_nativefloat((uint32_t)src[0x6]);
    camera_vr = (int)(dspfloat_to_nativefloat((uint32_t)src[0x7]) * camera_zoom - 0.5f);
    camera_vl = (int)(dspfloat_to_nativefloat((uint32_t)src[0x8]) * camera_zoom - 0.5f);
    camera_vu = (int)(dspfloat_to_nativefloat((uint32_t)src[0x9]) * camera_zoom - 0.5f);
    camera_vd = (int)(dspfloat_to_nativefloat((uint32_t)src[0xa]) * camera_zoom - 0.5f);
    reflection = src[0x2] >> 16 & 0x30;
    cullflip = reflection == 0x10 || reflection == 0x20;
    if (reflection & 0x10) { int t = camera_vl; camera_vl = camera_vr; camera_vr = t; }
    if (reflection & 0x20) { int t = camera_vu; camera_vu = camera_vd; camera_vd = t; }
    viewmatrix[0][0] = dspfixed_to_nativefloat((int16_t)src[0x0c]);
    viewmatrix[1][0] = dspfixed_to_nativefloat((int16_t)src[0x0d]);
    viewmatrix[2][0] = dspfixed_to_nativefloat((int16_t)src[0x0e]);
    viewmatrix[0][1] = dspfixed_to_nativefloat((int16_t)src[0x0f]);
    viewmatrix[1][1] = dspfixed_to_nativefloat((int16_t)src[0x10]);
    viewmatrix[2][1] = dspfixed_to_nativefloat((int16_t)src[0x11]);
    viewmatrix[0][2] = dspfixed_to_nativefloat((int16_t)src[0x12]);
    viewmatrix[1][2] = dspfixed_to_nativefloat((int16_t)src[0x13]);
    viewmatrix[2][2] = dspfixed_to_nativefloat((int16_t)src[0x14]);
    matrix3d_apply_reflection(viewmatrix);
    cz_adjust = 0; objectshift = 0; objectflags = 0;
}

static void handle_200002(const int32_t *src, int code)
{
    if (code == 0x5 || code >= 0x45) {
        float m[4][4];
        matrix3d_identity(m);
        m[0][0] = dspfixed_to_nativefloat((int16_t)src[0x1]);
        m[1][0] = dspfixed_to_nativefloat((int16_t)src[0x2]);
        m[2][0] = dspfixed_to_nativefloat((int16_t)src[0x3]);
        m[0][1] = dspfixed_to_nativefloat((int16_t)src[0x4]);
        m[1][1] = dspfixed_to_nativefloat((int16_t)src[0x5]);
        m[2][1] = dspfixed_to_nativefloat((int16_t)src[0x6]);
        m[0][2] = dspfixed_to_nativefloat((int16_t)src[0x7]);
        m[1][2] = dspfixed_to_nativefloat((int16_t)src[0x8]);
        m[2][2] = dspfixed_to_nativefloat((int16_t)src[0x9]);
        m[3][0] = (float)signed24(src[0xa]);
        m[3][1] = (float)signed24(src[0xb]);
        m[3][2] = (float)signed24(src[0xc]);
        matrix3d_multiply(m, viewmatrix);
        blit_polyobject(code, m);
    }
}

static void handle_300000(const int32_t *src)
{
    viewmatrix[0][0] = dspfixed_to_nativefloat((int16_t)src[1]);
    viewmatrix[1][0] = dspfixed_to_nativefloat((int16_t)src[2]);
    viewmatrix[2][0] = dspfixed_to_nativefloat((int16_t)src[3]);
    viewmatrix[0][1] = dspfixed_to_nativefloat((int16_t)src[4]);
    viewmatrix[1][1] = dspfixed_to_nativefloat((int16_t)src[5]);
    viewmatrix[2][1] = dspfixed_to_nativefloat((int16_t)src[6]);
    viewmatrix[0][2] = dspfixed_to_nativefloat((int16_t)src[7]);
    viewmatrix[1][2] = dspfixed_to_nativefloat((int16_t)src[8]);
    viewmatrix[2][2] = dspfixed_to_nativefloat((int16_t)src[9]);
    matrix3d_apply_reflection(viewmatrix);
}

static void handle_233002(const int32_t *src)
{
    cz_adjust = src[1] & 0xffffff;
    objectshift = src[2] & 0xffffff;
    objectflags = src[3] >> 21 & 7;
}

static void simulate_slavedsp(void)
{
    const int32_t *base = (const int32_t *)g_rr.poly;
    const int32_t *src = base + 0x300 - 1;
    for (int guard = 0; guard < 0x8000; guard++) {
        const uint16_t code = (uint16_t)*src++;
        const uint16_t len = (uint16_t)*src++;
        const int32_t index = (int32_t)(src - base);
        if (index + len >= 0x7fff) return;
        switch (len) {
        case 0x15: handle_bb0003(src); break;
        case 0x10: handle_233002(src); break;
        case 0x0a: handle_300000(src); break;
        case 0x0d: handle_200002(src, code); break;
        default: return;
        }
        src += len;
        src++;                                   /* 0xffff GOTO */
        const uint16_t next = (uint16_t)(*src++ & 0x7fff);
        if (next != index + len + 1 + 1) return; /* end of list: goto self */
        if (next == 0x7fff) return;
    }
}

/* master port 0xC: one 0x1C-word record per direct quad */
static void direct_poly(const uint16_t *src)
{
    const uint32_t zsort = ((uint32_t)(src[1] & 0xfff) << 12) | (src[0] & 0xfff);
    quadnode *q = new_quad(zsort);
    q->cmode = (src[0 + 4] & 0xf000) >> 12;
    q->texturebank = (src[1 + 4] & 0xf000) >> 12;
    q->color = (src[2] & 0xff00) >> 8;
    q->cz_value = src[3] >> 2 & 0x1fff;
    q->cz_type = src[3] & 3;
    src += 4;
    for (int i = 0; i < 4; i++) {
        polyvertex *p = &q->v[i];
        p->u = src[0] & 0x0fff;
        p->v = src[1] & 0x0fff;
        const int mantissa = src[5];
        int exponent = src[4] & 0x3f;
        if (mantissa) {
            p->z = (float)mantissa;
            while (exponent < 0x2e) { p->z /= 2.00f; exponent++; }
        } else p->z = (float)0x10000;
        p->x = (float)(int16_t)src[2];
        p->y = (float)-(int16_t)src[3];
        p->bri = src[4] >> 8;
        src += 6;
    }
    q->direct = true;
    q->vx = 0; q->vy = 0; q->vu = -240; q->vd = -240; q->vl = -320; q->vr = -320;
}

/* ------------------------------------------------------ rasterizer ---- */
typedef struct { double x, y, p[4]; } vtx;
typedef struct {
    int texture_enabled, shade_enabled, bn, cmode, prioverchar, fogfactor;
    int penbase;                      /* palette index of pen 0 */
    rgbi fogcolor;
} objdata;

static uint32_t *dest;                /* frame buffer before mixing */
static uint8_t  *primap;

static int32_t round_coordinate(double value)
{
    if (value >= (double)INT_MAX) return INT_MAX;
    const double ipart = floor(value);
    if (ipart < (double)INT_MIN) return INT_MIN;
    const double fpart = value - ipart;
    return (int32_t)ipart + (fpart > 0.5 ? 1 : 0);
}

static void scanline_poly(int y, int startx, int stopx, const float pstart[4], const float pdx[4], const objdata *e)
{
    float z = pstart[0], u = pstart[1], v = pstart[2], i = pstart[3];
    const float dz = pdx[0], du = pdx[1], dv = pdx[2], di = pdx[3];
    const int bn = e->bn * 0x1000;
    int penbase = e->penbase, penmask = 0xff, penshift = 0, pen = 0;
    const int fogfactor = 0xff - e->fogfactor;
    if (e->cmode & 4) { penbase += 0xec + ((e->cmode & 8) << 1); penmask = 0x03; penshift = 2 * (~e->cmode & 3); }
    else if (e->cmode & 2) { penbase += 0xe0 + ((e->cmode & 8) << 1); penmask = 0x0f; penshift = 4 * (~e->cmode & 1); }
    uint32_t *d = dest + y * W;
    uint8_t *pm = primap + y * W;
    for (int x = startx; x < stopx; x++) {
        const float ooz = 1.0f / z;
        if (e->texture_enabled) {
            const int tx = (int)(u * ooz) & 0xfff;
            const int ty = ((int)(v * ooz) & 0xfff) | bn;
            const int to = (ty << 4 & 0xfff00) | (tx >> 4);
            pen = ttdata[(ttmap[to] << 8) | tt_ayx_to_pixel[(ttattr[to] << 8) | (ty << 4 & 0xf0) | (tx & 0xf)]];
        }
        rgbi rgb = rgb_set(pen_rgb(penbase + (pen >> penshift & penmask)));
        if (fogfactor != 0xff) rgb_blend(&rgb, e->fogcolor, fogfactor);
        if (e->shade_enabled) {
            const int shade = (int)(i * ooz);
            rgb_scale_imm_clamp(&rgb, shade << 2);
        }
        d[x] = rgb_pack(rgb);
        pm[x] = (uint8_t)((pm[x] & ~1) | e->prioverchar);
        u += du; v += dv; i += di; z += dz;
    }
}

typedef struct { int minx, maxx, miny, maxy; } clip_t;

/* A triangle, set up once and then scanned band by band. The per-scanline
 * values are computed from these absolute quantities (never stepped from the
 * previous row), so a band renders exactly the pixels the serial loop did. */
typedef struct {
    const objdata *e;
    int32_t y0, y1;                   /* [y0, y1) after the clip rectangle */
    int minx, maxx;
    vtx v1, v2;                       /* sorted by y (v3 only enters via the slopes) */
    double dxdy_v1v2, dxdy_v1v3, dxdy_v2v3;
    double ps[4], pdx[4], pdy[4];
} tri_t;

static bool tri_setup(tri_t *T, const clip_t *cr, const objdata *e, const vtx *v1, const vtx *v2, const vtx *v3)
{
    const vtx *t;
    if (v2->y < v1->y) { t = v1; v1 = v2; v2 = t; }
    if (v3->y < v2->y) { t = v2; v2 = v3; v3 = t; if (v2->y < v1->y) { t = v1; v1 = v2; v2 = t; } }
    int32_t v1y = round_coordinate(v1->y), v3y = round_coordinate(v3->y);
    int32_t v1yclip = v1y > cr->miny ? v1y : cr->miny;
    int32_t v3yclip = v3y < cr->maxy + 1 ? v3y : cr->maxy + 1;
    if (v3yclip - v1yclip <= 0) return false;
    T->e = e; T->y0 = v1yclip; T->y1 = v3yclip; T->minx = cr->minx; T->maxx = cr->maxx;
    T->v1 = *v1; T->v2 = *v2;
    T->dxdy_v1v2 = (v2->y == v1->y) ? 0.0 : (v2->x - v1->x) / (v2->y - v1->y);
    T->dxdy_v1v3 = (v3->y == v1->y) ? 0.0 : (v3->x - v1->x) / (v3->y - v1->y);
    T->dxdy_v2v3 = (v3->y == v2->y) ? 0.0 : (v3->x - v2->x) / (v3->y - v2->y);
    double a00 = v2->y - v3->y, a01 = v3->x - v2->x, a02 = v2->x*v3->y - v3->x*v2->y;
    double a10 = v3->y - v1->y, a11 = v1->x - v3->x, a12 = v3->x*v1->y - v1->x*v3->y;
    double a20 = v1->y - v2->y, a21 = v2->x - v1->x, a22 = v1->x*v2->y - v2->x*v1->y;
    double det = a02 + a12 + a22;
    if (fabs(det) < 0.00001) {
        for (int k = 0; k < 4; k++) { T->pdx[k] = 0; T->pdy[k] = 0; T->ps[k] = v1->p[k]; }
    } else {
        double idet = 1.0 / det;
        for (int k = 0; k < 4; k++) {
            T->pdx[k] = idet * (v1->p[k]*a00 + v2->p[k]*a10 + v3->p[k]*a20);
            T->pdy[k] = idet * (v1->p[k]*a01 + v2->p[k]*a11 + v3->p[k]*a21);
            T->ps[k]  = idet * (v1->p[k]*a02 + v2->p[k]*a12 + v3->p[k]*a22);
        }
    }
    return true;
}

static void tri_scan(const tri_t *T, int32_t ya, int32_t yb)
{
    const vtx *v1 = &T->v1, *v2 = &T->v2;
    if (ya < T->y0) ya = T->y0;
    if (yb > T->y1) yb = T->y1;
    for (int32_t y = ya; y < yb; y++) {
        double fully = (double)y + 0.5;
        double startx = v1->x + (fully - v1->y) * T->dxdy_v1v3;
        double stopx = fully < v2->y ? v1->x + (fully - v1->y) * T->dxdy_v1v2 : v2->x + (fully - v2->y) * T->dxdy_v2v3;
        int32_t istartx = round_coordinate(startx), istopx = round_coordinate(stopx);
        if (istartx > istopx) { int32_t s = istartx; istartx = istopx; istopx = s; }
        if (istartx < T->minx) istartx = T->minx;
        if (istopx > T->maxx + 1) istopx = T->maxx + 1;
        if (istartx >= istopx) continue;
        double fullstartx = (double)istartx + 0.5;
        float st[4], dx[4];
        for (int k = 0; k < 4; k++) { st[k] = (float)(T->ps[k] + fullstartx * T->pdx[k] + fully * T->pdy[k]); dx[k] = (float)T->pdx[k]; }
        scanline_poly(y, istartx, istopx, st, dx, T->e);
    }
}

static int zclip_if_less(int numverts, const vtx *v, vtx *outv, double clipval)
{
    bool prevclipped = v[numverts - 1].p[0] < clipval;
    vtx *nextout = outv;
    for (int vn = 0; vn < numverts; vn++) {
        bool thisclipped = v[vn].p[0] < clipval;
        if (thisclipped != prevclipped) {
            const vtx *a = &v[vn == 0 ? numverts - 1 : vn - 1], *b = &v[vn];
            double frac = (clipval - a->p[0]) / (b->p[0] - a->p[0]);
            nextout->x = a->x + frac * (b->x - a->x);
            nextout->y = a->y + frac * (b->y - a->y);
            for (int k = 0; k < 4; k++) nextout->p[k] = a->p[k] + frac * (b->p[k] - a->p[k]);
            nextout++;
        }
        if (!thisclipped) *nextout++ = v[vn];
        prevclipped = thisclipped;
    }
    return (int)(nextout - outv);
}

/* per-quad setup output, indexed by the quad's position in the sorted list */
static objdata *q_obj;
static tri_t   *q_tri;                /* 4 slots per quad (a clipped quad is at most a hexagon: 4 triangles) */
static uint8_t *q_ntri;
static uint32_t q_cap;

static void quad_setup(uint32_t idx)
{
    const quadnode *n = &nodes[idx];
    q_ntri[idx] = 0;
    vtx v[4], clipv[6];
    int clipverts;
    const int cx = 320 + n->vx, cy = 240 + n->vy;
    clip_t cr = { cx + n->vl, cx - n->vr - 1, cy + n->vu, cy - n->vd - 1 };
    if (cr.minx < 0) cr.minx = 0;
    if (cr.miny < 0) cr.miny = 0;
    if (cr.maxx > NW - 1) cr.maxx = NW - 1;
    if (cr.maxy > NH - 1) cr.maxy = NH - 1;
    if (cr.minx > cr.maxx || cr.miny > cr.maxy) return;
    /* another output size: the same window in output pixels. A viewport that
     * covers the whole 640 width covers the whole (possibly wider) output. */
    const double pcx = vnative ? cx : (cx + vE) * vs, pcy = vnative ? cy : cy * vs, ps = vnative ? 1.0 : vs;
    if (!vnative) {
        clip_t o;
        if (cr.minx == 0 && cr.maxx == NW - 1) { o.minx = 0; o.maxx = W - 1; }
        else { o.minx = (int)floor((cr.minx + vE) * vs); o.maxx = (int)ceil((cr.maxx + 1 + vE) * vs) - 1; }
        o.miny = (int)floor(cr.miny * vs); o.maxy = (int)ceil((cr.maxy + 1) * vs) - 1;
        if (o.minx < 0) o.minx = 0;
        if (o.miny < 0) o.miny = 0;
        if (o.maxx > W - 1) o.maxx = W - 1;
        if (o.maxy > H - 1) o.maxy = H - 1;
        if (o.minx > o.maxx || o.miny > o.maxy) return;
        cr = o;
    }

    if (!n->direct) {
        for (int k = 0; k < 4; k++) {
            v[k].x = n->v[k].x; v[k].y = n->v[k].y;
            v[k].p[0] = n->v[k].z; v[k].p[1] = n->v[k].u; v[k].p[2] = n->v[k].v; v[k].p[3] = n->v[k].bri;
        }
        clipverts = zclip_if_less(4, v, clipv, 0.00001);
        if (clipverts < 3) return;
        for (int k = 0; k < clipverts; k++) {
            const double ooz = 1.0 / clipv[k].p[0];
            clipv[k].x = vnative ? cx + clipv[k].x * ooz : pcx + clipv[k].x * ooz * ps;
            clipv[k].y = vnative ? cy - clipv[k].y * ooz : pcy - clipv[k].y * ooz * ps;
            clipv[k].p[0] = ooz;
            clipv[k].p[1] = (clipv[k].p[1] + 0.5) * ooz;
            clipv[k].p[2] = (clipv[k].p[2] + 0.5) * ooz;
            clipv[k].p[3] = (clipv[k].p[3] + 0.5) * ooz;
        }
    } else {
        clipverts = 4;
        for (int k = 0; k < 4; k++) {
            const double ooz = n->v[k].z;
            clipv[k].x = vnative ? cx + n->v[k].x : pcx + n->v[k].x * ps;
            clipv[k].y = vnative ? cy - n->v[k].y : pcy - n->v[k].y * ps;
            clipv[k].p[0] = ooz;
            clipv[k].p[1] = (n->v[k].u + 0.5) * ooz;
            clipv[k].p[2] = (n->v[k].v + 0.5) * ooz;
            clipv[k].p[3] = (n->v[k].bri + 0.5) * ooz;
        }
    }

    objdata *ep = &q_obj[idx];
#define e (*ep)
    memset(&e, 0, sizeof e);
    e.shade_enabled = 1;
    e.texture_enabled = 1;
    e.penbase = (n->color & 0x7f) << 8;
    e.bn = n->texturebank;
    e.cmode = n->cmode;
    e.prioverchar = (n->cmode & 7) == 1 ? 1 : 0;
    if (!(n->color & 0x80)) {                        /* System 22 poly fog */
        const int cz_type = n->cz_type;
        const int cz_color = cz_type & mixer_b(0x84 + cz_type);
        e.fogcolor.r = mixer_b(0x100 + cz_color);
        e.fogcolor.g = mixer_b(0x180 + cz_color);
        e.fogcolor.b = mixer_b(0x200 + cz_color);
        e.fogfactor = g_rr.czram[(cz_type << 13 | n->cz_value) & (RR_CZRAM_SIZE - 1)];
    }
    if (n->objectflags) {
        e.texture_enabled = 0;
        e.cmode = 0;
        if (n->objectflags & 6) { e.penbase = n->cz_adjust & 0x7fff; e.shade_enabled = 0; }
        else e.penbase += (n->cz_adjust >> 16 & 0x7f) & (n->color | 0x1f);
    }
    if (n->cz_adjust & 0x800000) e.fogfactor = 0;

    for (int k = 2; k < clipverts && q_ntri[idx] < 4; k++)
        if (tri_setup(&q_tri[idx * 4 + q_ntri[idx]], &cr, ep, &clipv[0], &clipv[k - 1], &clipv[k])) q_ntri[idx]++;
#undef e
}

static int cmp_node(const void *a, const void *b)
{
    const quadnode *x = a, *y = b;
    if (x->zsort != y->zsort) return x->zsort < y->zsort ? 1 : -1;     /* far first */
    return x->seq < y->seq ? 1 : -1;                                   /* then the newest */
}

/* ------------------------------------------------------- text layer ---- */
static uint8_t text_pen_n[NW * NH], *text_pen = text_pen_n;

static void draw_text_layer(int text_palbase, int ya, int yb)
{
    const uint16_t a0 = (uint16_t)(g_rr.tilemapattr[0] << 8 | g_rr.tilemapattr[1]);
    const uint16_t a1 = (uint16_t)(g_rr.tilemapattr[2] << 8 | g_rr.tilemapattr[3]);
    const int sx = (a0 - 0x35c) & 0x3ff, sy = a1 & 0x3ff;
    for (int y = ya; y < yb; y++) {
        const int ny = vnative ? y : tmap_y[y];
        if (ny < 0) continue;
        const int ty = (ny + sy) & 0x3ff, trow = ty >> 4, cy = ty & 15;
        for (int x = 0; x < W; x++) {
            const int nx = vnative ? x : tmap_x[x];
            if (nx < 0) continue;
            const int tx = (nx + sx) & 0x3ff;
            const int ti = (trow * 64 + (tx >> 4)) * 2;
            const uint16_t d = (uint16_t)(g_rr.text[ti] << 8 | g_rr.text[ti + 1]);
            const int code = d & 0x3ff;
            int cx = tx & 15;
            if (d & 0x400) cx = 15 - cx;
            const int ccy = (d & 0x800) ? 15 - cy : cy;
            const uint32_t off = (uint32_t)code * 128 + (uint32_t)ccy * 8 + (uint32_t)(cx >> 1);
            const uint8_t byte = off < 0x1e000 ? g_rr.cgram[off] : g_rr.text[off - 0x1e000];
            const int pix = (cx & 1) ? (byte & 0xf) : (byte >> 4);
            if (pix == 0xf) continue;
            text_pen[y * W + x] = (uint8_t)((d >> 12) << 4 | pix);
            primap[y * W + x] = (uint8_t)((primap[y * W + x] & 3) | 2);
        }
    }
    (void)text_palbase;
}

/* ------------------------------------------------------ frame output ---- */

/* ---------------------------------------------------- worker threads ----
 * A fixed pool runs one job at a time: items are claimed from an atomic counter
 * and the calling thread works too. RR_THREADS=1 renders serially (A/B). */
#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>

typedef void (*job_fn)(int item);
static int n_workers;                       /* extra threads besides the caller */
static pthread_t workers[31];
static pthread_mutex_t job_mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t job_cv = PTHREAD_COND_INITIALIZER, done_cv = PTHREAD_COND_INITIALIZER;
static job_fn job; static int job_items;
static atomic_int job_next, job_busy;
static unsigned job_gen;

static void job_drain(void)
{
    for (int i; (i = atomic_fetch_add(&job_next, 1)) < job_items; ) job(i);
}

static void *worker_main(void *arg)
{
    (void)arg;
    unsigned seen = 0;
    for (;;) {
        pthread_mutex_lock(&job_mx);
        while (job_gen == seen) pthread_cond_wait(&job_cv, &job_mx);
        seen = job_gen;
        pthread_mutex_unlock(&job_mx);
        job_drain();
        if (atomic_fetch_sub(&job_busy, 1) == 1) {
            pthread_mutex_lock(&job_mx); pthread_cond_signal(&done_cv); pthread_mutex_unlock(&job_mx);
        }
    }
    return NULL;
}

static void pool_init(void)
{
    static bool done;
    if (done) return;
    done = true;
    long nc = sysconf(_SC_NPROCESSORS_ONLN);
    int want = nc > 1 ? (int)nc - 1 : 0;                   /* leave a core for the emulator threads */
    const char *e = getenv("RR_THREADS");
    if (e) want = atoi(e) - 1;
    if (want < 0) want = 0;
    if (want > 31) want = 31;
    for (int i = 0; i < want; i++)
        if (pthread_create(&workers[n_workers], NULL, worker_main, NULL) == 0) n_workers++;
}

static void run_job(job_fn f, int items)
{
    if (n_workers == 0 || items <= 1) { for (int i = 0; i < items; i++) f(i); return; }
    pthread_mutex_lock(&job_mx);
    job = f; job_items = items;
    atomic_store(&job_next, 0);
    atomic_store(&job_busy, n_workers + 1);
    job_gen++;
    pthread_cond_broadcast(&job_cv);
    pthread_mutex_unlock(&job_mx);
    job_drain();
    if (atomic_fetch_sub(&job_busy, 1) != 1) {
        pthread_mutex_lock(&job_mx);
        while (atomic_load(&job_busy) != 0) pthread_cond_wait(&done_cv, &job_mx);
        pthread_mutex_unlock(&job_mx);
    }
}

/* ------------------------------------------------------ the frame, banded */
#define BAND 16                                 /* rows per band: 30 bands, load-balanced by the pool */
#define QCHUNK 64                               /* quads per setup item */
static uint32_t fb_n[NW * NH], *fb = fb_n;
static uint8_t pri_n[NW * NH], *pri = pri_n;
static uint32_t frame_bg;
static int f_text_palbase, f_mixer_flags, f_fade_r, f_fade_g, f_fade_b;

static void job_setup(int item)
{
    uint32_t a = (uint32_t)item * QCHUNK, b = a + QCHUNK;
    if (b > n_nodes) b = n_nodes;
    for (uint32_t k = a; k < b; k++) quad_setup(k);
}

static void job_band(int item)
{
    const int ya = item * BAND, yb = ya + BAND < H ? ya + BAND : H;
    memset(pri + ya * W, 0, (size_t)(yb - ya) * W);
    for (int i = ya * W; i < yb * W; i++) fb[i] = frame_bg;
    for (uint32_t k = 0; k < n_nodes; k++)                    /* painter's order: far to near */
        for (int j = 0; j < q_ntri[k]; j++) {
            const tri_t *T = &q_tri[k * 4 + j];
            if (T->y1 > ya && T->y0 < yb) tri_scan(T, ya, yb);
        }
    draw_text_layer(f_text_palbase, ya, yb);

    /* namcos22_mix_text_layer */
    const int text_palbase = f_text_palbase, mixer_flags = f_mixer_flags;
    const int fade_r = f_fade_r, fade_g = f_fade_g, fade_b = f_fade_b;
    const bool fade_enabled = fade_r != 0x100 || fade_g != 0x100 || fade_b != 0x100;
    const uint32_t fade_r_add = fade_r > 0x100 ? 1u << 16 : 0, fade_g_add = fade_g > 0x100 ? 1u << 8 : 0, fade_b_add = fade_b > 0x100 ? 1u : 0;
    const bool fade_white = fade_r_add || fade_g_add || fade_b_add;
    const bool shadow_enabled = (mixer_flags >> 8) & 1;
    const rgbi fade_color = { fade_r, fade_g, fade_b };
    const rgbi rgb_mix[3] = {
        { mixer_b(0x08), mixer_b(0x09), mixer_b(0x0a) },
        { mixer_b(0x0b), mixer_b(0x0c), mixer_b(0x0d) },
        { mixer_b(0x0e), mixer_b(0x0f), mixer_b(0x10) } };
    for (int i = ya * W; i < yb * W; i++) {
        uint32_t pixel = fb[i];
        if (pri[i] == 2) {
            const int pen = text_palbase + text_pen[i];      /* palette offset + tile colour/pixel */
            const uint8_t p8 = (uint8_t)text_pen[i];
            if (shadow_enabled && p8 >= 0xfc && p8 <= 0xfe) {
                rgbi rgb = rgb_set(pixel);
                rgb_scale_clamp(&rgb, rgb_mix[p8 - 0xfc]);
                pixel = rgb_pack(rgb);
            } else pixel = pen_rgb(pen);
        }
        if (fade_enabled) {
            if (fade_white) {
                if (!(pixel & 0xff0000)) pixel += fade_r_add;
                if (!(pixel & 0x00ff00)) pixel += fade_g_add;
                if (!(pixel & 0x0000ff)) pixel += fade_b_add;
            }
            rgbi rgb = rgb_set(pixel);
            rgb_scale_clamp(&rgb, fade_color);
            pixel = rgb_pack(rgb);
        }
        out[i] = (uint32_t)gamma_prom[pixel >> 16 & 0xff] << 16 | (uint32_t)gamma_prom[0x100 + (pixel >> 8 & 0xff)] << 8 |
                         gamma_prom[0x200 + (pixel & 0xff)];
    }
}

void rr_video_set_size(int w, int h)
{
    if (w < 64 || h < 48 || (long)w * h > 7680L * 4320) { w = NW; h = NH; }
    want_w = w; want_h = h;
}

const uint32_t *rr_video_output(int *w, int *h) { *w = W; *h = H; return out; }

static void apply_size(void)
{
    if (want_w == W && want_h == H) return;
    const bool nat = want_w == NW && want_h == NH;
    if (!vnative) { free(fb); free(pri); free(text_pen); free(out); free(tmap_x); free(tmap_y); }
    fb = fb_n; pri = pri_n; text_pen = text_pen_n; out = g_frame_rgb; tmap_x = tmap_y = NULL;
    W = NW; H = NH; vs = 1.0; vE = 0.0; vnative = true;
    if (nat) { fprintf(stderr, "[VID] render size 640x480 (native)\n"); return; }
    const size_t n = (size_t)want_w * want_h;
    uint32_t *f = malloc(n * 4), *o = malloc(n * 4);
    uint8_t *p = malloc(n), *t = malloc(n);
    int16_t *mx = malloc((size_t)want_w * sizeof *mx), *my = malloc((size_t)want_h * sizeof *my);
    if (!f || !o || !p || !t || !mx || !my) {
        free(f); free(o); free(p); free(t); free(mx); free(my);
        fprintf(stderr, "[VID] no memory for %dx%d, staying at 640x480\n", want_w, want_h);
        want_w = NW; want_h = NH; return;
    }
    fb = f; out = o; pri = p; text_pen = t; tmap_x = mx; tmap_y = my;
    W = want_w; H = want_h; vnative = false;
    vs = H / (double)NH;
    vE = (W / vs - NW) / 2.0;                       /* > 0: widescreen margin, native pixels */
    for (int x = 0; x < W; x++) { const int nx = (int)floor((x + 0.5) / vs - vE); tmap_x[x] = (int16_t)(nx >= 0 && nx < NW ? nx : -1); }
    for (int y = 0; y < H; y++) { const int ny = (int)floor((y + 0.5) / vs);       tmap_y[y] = (int16_t)(ny < NH ? ny : -1); }
    memset(out, 0, n * 4);
    fprintf(stderr, "[VID] render size %dx%d (scale %.3f, %+.1f px each side)\n", W, H, vs, vE);
}

void rr_video_frame(bool slave_active)
{
    pool_init();
    apply_size();
    dest = fb; primap = pri;
    /* render_frame_active (rr_scene.c) */
    const bool walk = rr_scene_frame(slave_active);
    /* the master's direct polys arrived during the frame, before the list */
    for (int i = 0; i < rr_scene_direct_count(); i++) direct_poly(rr_scene_direct(i));
    rr_scene_consume();

    /* update_mixer (System 22) */
    f_mixer_flags = mixer_b(0x00) << 8 | mixer_b(0x01);
    const int bg_palbase = mixer_b(0x04) << 8 & 0x7f00;
    f_text_palbase = mixer_b(0x07) << 8 & 0x7f00;
    f_fade_r = mixer_b(0x11) << 8 | mixer_b(0x12);
    f_fade_g = mixer_b(0x13) << 8 | mixer_b(0x14);
    f_fade_b = mixer_b(0x15) << 8 | mixer_b(0x16);
    frame_bg = pen_rgb(bg_palbase | 0xff);

    if (walk) simulate_slavedsp();
    qsort(nodes, n_nodes, sizeof *nodes, cmp_node);
    if (n_nodes > q_cap) {
        q_cap = cap_nodes;
        q_obj = realloc(q_obj, q_cap * sizeof *q_obj);
        q_tri = realloc(q_tri, q_cap * 4 * sizeof *q_tri);
        q_ntri = realloc(q_ntri, q_cap);
    }
    run_job(job_setup, (int)((n_nodes + QCHUNK - 1) / QCHUNK));
    run_job(job_band, (H + BAND - 1) / BAND);
    n_nodes = 0;
}

bool rr_video_write_ppm(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (int i = 0; i < W * H; i++) {
        uint8_t b[3] = { (uint8_t)(out[i] >> 16), (uint8_t)(out[i] >> 8), (uint8_t)out[i] };
        fwrite(b, 1, 3, f);
    }
    fclose(f);
    return true;
}

/* ---- renderer gate: draw a MAME-captured video state (tools/mame/dump_video.lua) ---- */
bool rr_video_render_dump(const char *dir, int f, const char *out_ppm)
{
    if (!rr_scene_load_capture(dir, f)) return false;
    rr_scene_force_walk();
    rr_video_frame(true);
    return rr_video_write_ppm(out_ppm);
}
