/*
 * rr_gl.c -- Rave Racer's picture through the SHARED ENGINE (../engine/):
 * the same geometry stage, display-list walker, texture bake and OpenGL
 * rasteriser Prop Cycle draws with. What is System 22 about it is here, as
 * board settings and the parts only this board has:
 *
 *   list head 0x2FF, object flags in the 0x10 record, point RAM objects
 *   depth fog: ONE factor per quad (czram[cz_type<<13 | cz_value]) in a colour
 *              per cz_type (mixer 0x100/0x180/0x200), applied BEFORE shading
 *   direct polys the master sends straight to the renderer
 *   text layer over the polygons unless the last polygon on the pixel set
 *              prioverchar (cmode & 7 == 1), shadow pens 0xFC-0xFE
 *   global fade (mixer 0x11-0x16) and the three gamma PROMs, per channel,
 *              over the whole composed frame
 *
 * The reference for every rule is namcos22_v.cpp's non-super path, which
 * src/rr_video.c ports pixel for pixel; that file is the TEST ORACLE this
 * one is measured against (--render-dump with --gl vs without).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL.h>
#include "eng_gl.h"
#include "eng.h"
#include "geo_hw.h"
#include "slave_list.h"
#include "quad_gl.h"
#include "hud_edges.h"
#include "tex_bake.h"
#include "post_gl.h"
#include "rr_mem.h"
#include "rr_dsp.h"
#include "rr_scene.h"
#include "rr_gl.h"

#define NW 640
#define NH 480

static uint8_t gamma_prom[3][256];
static bool    assets_ok, gl_ready;

/* ---------------------------------------------------------------- assets */
static int32_t pointram_read(uint32_t a) { return rr_dsp_pointram_read(a & 0xffffff); }

bool rr_gl_init(const char *dir)
{
    static const char *const cg[8] = { "rv1cg0.1a", "rv1cg1.1c", "rv1cg2.1d", "rv1cg3.1e",
                                       "rv1cg4.1f", "rv1cg5.1j", "rv1cg6.1k", "rv1cg7.1n" };
    if (!eng_load_texture_roms(dir, cg, "rv1ccrl.5a", "rv1ccrh.5c")) return false;
    static const char *gp[3] = { "rr1gam.2d", "rr1gam.3d", "rr1gam.4d" };
    for (int i = 0; i < 3; i++) {
        char p[1024]; snprintf(p, sizeof p, "%s/%s", dir, gp[i]);
        FILE *f = fopen(p, "rb");
        if (!f || fread(gamma_prom[i], 1, 256, f) != 256) { if (f) fclose(f); fprintf(stderr, "[GL] no %s\n", p); return false; }
        fclose(f);
    }
    g_eng_pointrom   = g_pointrom;           /* rr_dsp_init loaded it */
    g_eng_pointrom_n = g_pointrom_words;
    g_eng_pointram   = pointram_read;
    g_tex_opaque = 1;                        /* the polygon path has no transparent pen */
    assets_ok = g_eng_pointrom != NULL;
    return assets_ok;
}

/* -------------------------------------------------------- board helpers */
static inline uint8_t mixer_b(int n) { return g_rr.mixer[n & (RR_MIXER_SIZE - 1)]; }
static inline void pen_rgb(int pen, uint8_t o[3])
{
    pen &= 0x7fff;
    o[0] = g_rr.pal[pen]; o[1] = g_rr.pal[pen + 0x8000]; o[2] = g_rr.pal[pen + 0x10000];
}

/* System 22 poly fog (namcos22_v.cpp poly3d_drawquad, !m_is_ss22):
 * a colour byte with bit 7 clear is fogged by one factor for the whole quad. */
static int s22_fog_quad(const geo_quad *q, eng_fog *f)
{
    const int color = (int)((q->color >> 8) & 0xff);
    if (color & 0x80) return 0;
    const int cz_type = q->cz_type & 3;
    const int cz_color = cz_type & mixer_b(0x84 + cz_type);
    const int ff = g_rr.czram[((cz_type << 13) | (q->cz_value & 0x1fff)) & (RR_CZRAM_SIZE - 1)];
    if (!ff) return 0;
    f->rgb[0] = mixer_b(0x100 + cz_color);
    f->rgb[1] = mixer_b(0x180 + cz_color);
    f->rgb[2] = mixer_b(0x200 + cz_color);
    f->tab = NULL;
    f->alpha_const = 0xff - ff;              /* blend(rgb, fog, 0xff - ff) */
    return 1;
}

/* ------------------------------------------------------------ the quads */
static geo_quad *qbuf;
static int       qn, qcap, qorder;

static void push_quad(const geo_quad *q, void *user)
{
    (void)user;
    if (qn == qcap) {
        int nc = qcap ? qcap * 2 : 8192;
        geo_quad *nb = realloc(qbuf, (size_t)nc * sizeof *qbuf);
        if (!nb) return;
        qbuf = nb; qcap = nc;
    }
    qbuf[qn] = *q;
    qbuf[qn].order = qorder++;
    qn++;
}

static uint32_t poly_word(int i) { return g_rr.poly[i & 0x7fff]; }

/* A direct poly (namcos22_v.cpp draw_direct_poly): four screen-space vertices
 * with a per-vertex 1/z already worked out by the master. */
static void direct_quad(const uint16_t *src)
{
    geo_quad q;
    memset(&q, 0, sizeof q);
    q.zsort   = (int32_t)(((uint32_t)(src[1] & 0xfff) << 12) | (src[0] & 0xfff));
    q.cmode   = (src[4] & 0xf000) >> 12;
    q.texbank = (src[5] & 0xf000) >> 12;
    q.color   = (uint32_t)(src[2] & 0xff00);          /* palette byte in bits 15:8 */
    q.cz_value = (src[3] >> 2) & 0x1fff;
    q.cz_type  = src[3] & 3;
    q.clip[0] = 0; q.clip[1] = NW - 1; q.clip[2] = 0; q.clip[3] = NH - 1;
    q.direct = 1;
    q.nrv = 4;
    int u0 = 0xfff, u1 = 0, v0 = 0xfff, v1 = 0;
    const uint16_t *s = src + 4;
    for (int i = 0; i < 4; i++, s += 6) {
        geo_vert *v = &q.rv[i];
        v->u = s[0] & 0x0fff; v->v = s[1] & 0x0fff;
        float ooz = (float)0x10000;
        if (s[5]) { ooz = (float)s[5]; for (int e = s[4] & 0x3f; e < 0x2e; e++) ooz /= 2.0f; }
        /* x/y are screen offsets from the centre; z carries the depth 1/ooz */
        v->sx16 = (NW / 2 + (int16_t)s[2]) * 16;
        v->sy16 = (NH / 2 + (int16_t)s[3]) * 16;
        double z = ooz > 0.0f ? 1.0 / ooz : 1.0;
        v->z = z < 1.0 ? 1 : z > 2e9 ? 2000000000 : (int32_t)z;
        v->bri = s[4] >> 8;
        v->valid = 1;
        if ((int)v->u < u0) u0 = v->u;  if ((int)v->u > u1) u1 = v->u;
        if ((int)v->v < v0) v0 = v->v;  if ((int)v->v > v1) v1 = v->v;
        q.v[i] = *v;
    }
    q.uvbox[0] = (uint16_t)u0; q.uvbox[1] = (uint16_t)u1;
    q.uvbox[2] = (uint16_t)v0; q.uvbox[3] = (uint16_t)v1;
    push_quad(&q, NULL);
}

/* ------------------------------------------------------------ text layer
 * namcos22_v.cpp draw_text_layer + namcos22_mix_text_layer: 64x64 tiles of
 * 16x16 4bpp from CG RAM, pen 0xF transparent, scrolled by tilemapattr.
 * Two images: the text's own colours (alpha = drawn), and the SHADOW pens
 * 0xFC-0xFE as per-channel multipliers of what is under them. */
static uint8_t txt_rgba[NW * NH * 4], shd_rgba[NW * NH * 4];
static GLuint  txt_tex, shd_tex;

static bool build_text(int text_palbase, bool shadow_enabled, bool *any_shadow)
{
    const uint16_t a0 = (uint16_t)(g_rr.tilemapattr[0] << 8 | g_rr.tilemapattr[1]);
    const uint16_t a1 = (uint16_t)(g_rr.tilemapattr[2] << 8 | g_rr.tilemapattr[3]);
    const int sx = (a0 - 0x35c) & 0x3ff, sy = a1 & 0x3ff;
    uint8_t mix[3][3];
    for (int k = 0; k < 3; k++) for (int c = 0; c < 3; c++) mix[k][c] = mixer_b(0x08 + k * 3 + c);
    bool any = false; *any_shadow = false;
    static uint8_t occ[NW * NH];                       /* where either layer draws: the widescreen HUD's pieces are found in it */
    memset(occ, 0, sizeof occ);
    memset(shd_rgba, 0xff, sizeof shd_rgba);
    for (int y = 0; y < NH; y++) {
        const int ty = (y + sy) & 0x3ff, trow = ty >> 4, cy = ty & 15;
        uint8_t *d = txt_rgba + (size_t)y * NW * 4, *sd = shd_rgba + (size_t)y * NW * 4;
        for (int x = 0; x < NW; x++, d += 4, sd += 4) {
            d[3] = 0;
            const int tx = (x + sx) & 0x3ff;
            const int ti = (trow * 64 + (tx >> 4)) * 2;
            const uint16_t w = (uint16_t)(g_rr.text[ti] << 8 | g_rr.text[ti + 1]);
            const int code = w & 0x3ff;
            int cx = tx & 15;
            if (w & 0x400) cx = 15 - cx;
            const int ccy = (w & 0x800) ? 15 - cy : cy;
            const uint32_t off = (uint32_t)code * 128 + (uint32_t)ccy * 8 + (uint32_t)(cx >> 1);
            const uint8_t byte = off < 0x1e000 ? g_rr.cgram[off] : g_rr.text[off - 0x1e000];
            const int pix = (cx & 1) ? (byte & 0xf) : (byte >> 4);
            if (pix == 0xf) continue;
            const uint8_t p8 = (uint8_t)((w >> 12) << 4 | pix);
            if (shadow_enabled && p8 >= 0xfc && p8 <= 0xfe) {
                sd[0] = mix[p8 - 0xfc][0]; sd[1] = mix[p8 - 0xfc][1]; sd[2] = mix[p8 - 0xfc][2];
                sd[3] = 255; *any_shadow = true;
                occ[(size_t)y * NW + x] = 1;
                continue;
            }
            pen_rgb(text_palbase + p8, d);
            d[3] = 255;
            occ[(size_t)y * NW + x] = 1;
            any = true;
        }
    }
    if (g_eng_hud_e) eng_hud_text_scan(occ);
    return any;
}

static void upload(GLuint *tex, const uint8_t *px, int w, int h)
{
    if (!*tex) {
        glGenTextures(1, tex);
        glBindTexture(GL_TEXTURE_2D, *tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    } else {
        glBindTexture(GL_TEXTURE_2D, *tex);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    }
}

static void draw_layer(GLuint tex)
{
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, tex);
    glColor4f(1, 1, 1, 1);
    if (g_eng_hud_e) eng_hud_text_draw();               /* widescreen HUD: each piece of the layer at its side (engine/hud_edges.h) */
    else {
    glBegin(GL_QUADS);
    glTexCoord2f(0, 0); glVertex2f(0, 0);
    glTexCoord2f(1, 0); glVertex2f(NW, 0);
    glTexCoord2f(1, 1); glVertex2f(NW, NH);
    glTexCoord2f(0, 1); glVertex2f(0, NH);
    glEnd();
    }
    glDisable(GL_TEXTURE_2D);
}

/* ------------------------------------------------ fade + gamma, per channel
 * Both are per-channel functions of the composed pixel, so together they are
 * one lookup table per channel: MAME's order is fade (with the white-fade
 * floor of 1 on channels faded above 0x100), then the gamma PROM. Applied to
 * the whole frame as a copy through GL's pixel map. */
static void post_lut(int vw, int vh)
{
    const int fr = mixer_b(0x11) << 8 | mixer_b(0x12);
    const int fg = mixer_b(0x13) << 8 | mixer_b(0x14);
    const int fb = mixer_b(0x15) << 8 | mixer_b(0x16);
    const int fade[3] = { fr, fg, fb };
    const bool fade_enabled = fr != 0x100 || fg != 0x100 || fb != 0x100;
    const bool fade_white = fr > 0x100 || fg > 0x100 || fb > 0x100;
    uint8_t lut[3][256];
    for (int c = 0; c < 3; c++)
        for (int v = 0; v < 256; v++) {
            int x = v;
            if (fade_enabled) {
                if (fade_white && fade[c] > 0x100 && x == 0) x = 1;
                x = (x * fade[c]) >> 8;
                if (x > 255) x = 255;
            }
            lut[c][v] = gamma_prom[c][x];
        }
    eng_post_lut(lut, vw, vh);
}

/* ----------------------------------------------------------- the frame
 * rr_gl_prepare runs once per SCREEN UPDATE (the list walk decides what the
 * update keeps, and must happen exactly once); rr_gl_draw may run any number
 * of times after it -- a paused window redraws the same frame. */
static int frame_mixer_flags, frame_bg_palbase, frame_text_palbase;

/* widescreen: the race HUD is up while the gear ladder's first row is on screen (text cells row 22, cols 2 and 3). That row holds F0C0 /
 * F0C1 while gear 1 is NOT selected and 40D2 / 40D3 while it is (measured over a whole race: the two states never show on any other
 * screen), so both are marks -- with only the first, the HUD sat in the 4:3 centre through the start countdown and in gear 1. The map's
 * dots and the mirror's frame are polygons in the full-frame viewport at priority band 0 (the mirror's own picture is a sub-window
 * viewport). */
static const eng_hud_mark hud_marks[] = { { 22, 2, 0xF0C0 }, { 22, 3, 0xF0C1 }, { 22, 2, 0x40D2 }, { 22, 3, 0x40D3 } };
static bool hud_on;

void rr_gl_prepare(bool slave_active)
{
    if (!assets_ok) return;
    g_eng_frame++;
    { static int hold; hud_on = eng_hud_marks_up(g_rr.text, hud_marks, (int)(sizeof hud_marks / sizeof *hud_marks), &hold); }
    /* update_mixer (System 22), latched with the frame */
    frame_mixer_flags  = mixer_b(0x00) << 8 | mixer_b(0x01);
    frame_bg_palbase   = mixer_b(0x04) << 8 & 0x7f00;
    frame_text_palbase = mixer_b(0x07) << 8 & 0x7f00;

    /* direct polys first (they arrived during the frame), then the list */
    const bool walk = rr_scene_frame(slave_active);
    qn = 0; qorder = 0;
    for (int i = 0; i < rr_scene_direct_count(); i++) direct_quad(rr_scene_direct(i));
    rr_scene_consume();
    if (walk) {
        eng_list_cfg cfg = { ENG_LIST_HEAD_S22, 1, NULL, NULL, NULL, NULL };
        eng_walk_list(poly_word, &cfg, push_quad, NULL);
    }
    eng_quad_sort(qbuf, qn, 0);
    { static int want = -1, n;                          /* RR_CLIPLOG=<n>: the distinct viewport clip windows of screen update n */
      if (want < 0) { const char *e = getenv("RR_CLIPLOG"); want = e ? atoi(e) : 0; }
      if (want && n + 1 == want) {                       /* per priority band: how many quads and where (the HUD's polygons are the small bands) */
          int ap_n[8] = { 0 }, ax0[8], ax1[8], ay0[8], ay1[8];
          for (int b = 0; b < 8; b++) { ax0[b] = 1 << 30; ax1[b] = -(1 << 30); ay0[b] = 1 << 30; ay1[b] = -(1 << 30); }
          for (int i = 0; i < qn; i++) {
              const int b = (qbuf[i].zsort >> 21) & 7;
              ap_n[b]++;
              for (int k = 0; k < qbuf[i].nrv; k++) {
                  int x = qbuf[i].rv[k].sx16 >> 4, y = qbuf[i].rv[k].sy16 >> 4;
                  if (x < ax0[b]) ax0[b] = x; if (x > ax1[b]) ax1[b] = x;
                  if (y < ay0[b]) ay0[b] = y; if (y > ay1[b]) ay1[b] = y;
              }
          }
          for (int i = 0; i < qn; i++) {
              if (((qbuf[i].zsort >> 21) & 7) != 0 || qbuf[i].clip[1] < 639) continue;
              int x0 = 1 << 30, x1 = -(1 << 30), y0 = 1 << 30, y1 = -(1 << 30);
              for (int k = 0; k < qbuf[i].nrv; k++) {
                  int x = qbuf[i].rv[k].sx16 >> 4, y = qbuf[i].rv[k].sy16 >> 4;
                  if (x < x0) x0 = x; if (x > x1) x1 = x; if (y < y0) y0 = y; if (y > y1) y1 = y;
              }
              fprintf(stderr, "[HUDQ] pick %d  direct %d  x %d..%d  y %d..%d  zsort %06X nrv %d\n", qbuf[i].pick_code, qbuf[i].direct, x0, x1, y0, y1, qbuf[i].zsort & 0xffffff, qbuf[i].nrv);
          }
          for (int b = 0; b < 8; b++) if (ap_n[b]) fprintf(stderr, "[AP] band %d: %d quads  x %d..%d  y %d..%d\n", b, ap_n[b], ax0[b], ax1[b], ay0[b], ay1[b]);
          { int bx0, by0, bx1, by1;                          /* RR_CLIPLOG_BOX=x0,y0,x1,y1: every quad that lies wholly inside that screen box */
            const char *e = getenv("RR_CLIPLOG_BOX");
            if (e && sscanf(e, "%d,%d,%d,%d", &bx0, &by0, &bx1, &by1) == 4)
                for (int i = 0; i < qn; i++) {
                    int x0 = 1 << 30, x1 = -(1 << 30), y0 = 1 << 30, y1 = -(1 << 30);
                    for (int k = 0; k < qbuf[i].nrv; k++) {
                        int x = qbuf[i].rv[k].sx16 >> 4, y = qbuf[i].rv[k].sy16 >> 4;
                        if (x < x0) x0 = x; if (x > x1) x1 = x; if (y < y0) y0 = y; if (y > y1) y1 = y;
                    }
                    if (qbuf[i].nrv && x0 >= bx0 && x1 <= bx1 && y0 >= by0 && y1 <= by1)
                        fprintf(stderr, "[BOXQ] pick %d  direct %d  band %d  clip %d..%d  x %d..%d  y %d..%d  zsort %06X nrv %d\n", qbuf[i].pick_code, qbuf[i].direct,
                                (qbuf[i].zsort >> 21) & 7, qbuf[i].clip[0], qbuf[i].clip[1], x0, x1, y0, y1, qbuf[i].zsort & 0xffffff, qbuf[i].nrv);
                } }
      }
      if (want && ++n == want) {
          int32_t seen[64][4]; int cnt[64], ns = 0;
          for (int i = 0; i < qn; i++) {
              int k;
              for (k = 0; k < ns; k++) if (!memcmp(seen[k], qbuf[i].clip, sizeof seen[k])) break;
              if (k == ns && ns < 64) { memcpy(seen[ns], qbuf[i].clip, sizeof seen[0]); cnt[ns++] = 0; }
              if (k < 64) cnt[k]++;
          }
          for (int k = 0; k < ns; k++) fprintf(stderr, "[CLIP] %4d..%4d x %4d..%4d  %d quads\n", seen[k][0], seen[k][1], seen[k][2], seen[k][3], cnt[k]);
      } }
}

int rr_gl_quads(void) { return qn; }

void rr_gl_draw(int vw, int vh)
{
    if (!assets_ok) return;
    if (!gl_ready) { renderer_texture_init(); gl_ready = true; }

    /* the scene's width: wider than 4:3 widens full-frame viewports (Hor+) */
    const double aspect = (double)vw / vh;
    if (aspect > 4.0 / 3.0 + 1e-3) {
        const float E = (float)((NH * aspect - NW) / 2.0);
        g_scene_x0 = -E; g_scene_x1 = NW + E;
    } else { g_scene_x0 = 0.0f; g_scene_x1 = NW; }

    eng_palette_from_planar(g_rr.pal, 0x8000);

    eng_hud_begin(hud_on);                              /* widescreen: how far the HUD goes out (0 = it stays) */
    g_eng_quad_dx = eng_hud_quad_dx;
    eng_hud_quads_scan(qbuf, qn, 0, true);                /* band 0 = the direct polys (map dots, mirror frame); depth 0 = the tacho needle */

    glViewport(0, 0, vw, vh);
    glMatrixMode(GL_PROJECTION); glLoadIdentity();
    glOrtho(g_scene_x0, g_scene_x1, NH, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE); glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glEnable(GL_ALPHA_TEST); glAlphaFunc(GL_GREATER, 0.1f);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    uint8_t bg[3]; pen_rgb(frame_bg_palbase | 0xff, bg);
    glClearColor(bg[0] / 255.0f, bg[1] / 255.0f, bg[2] / 255.0f, 0.0f);   /* alpha 0: no prio */
    glClear(GL_COLOR_BUFFER_BIT);

    eng_draw_cfg dc;
    memset(&dc, 0, sizeof dc);
    dc.shade = 1;
    dc.fog = 1;
    dc.fog_before_shade = 1;
    dc.fog_quad = s22_fog_quad;
    dc.write_prio_alpha = 1;
    { static int tc = -1; if (tc < 0) { const char *e = getenv("RR_TEXEL_CENTRE"); tc = e ? atoi(e) : 1; }
      dc.texel_centre = tc; }
    eng_draw_begin();
    for (int i = 0; i < qn; i++) eng_draw_quad(&qbuf[i], &dc);
    eng_draw_end();

    /* text: over the polygons wherever the last one drawn did not set
     * prioverchar (destination alpha 0). Transparent texels are discarded by
     * the alpha test, so the polygons stay where the layer draws nothing. */
    bool any_shadow;
    bool any_text = build_text(frame_text_palbase, (frame_mixer_flags >> 8) & 1, &any_shadow);
    { static int no = -1; if (no < 0) { const char *e = getenv("RR_NO_TEXT"); no = e && *e == '1'; }     /* dev: the picture without the text layer */
      if (no) { any_text = false; any_shadow = false; } }
    glEnable(GL_BLEND);
    if (any_shadow) {                                   /* rgb *= mix/256 */
        upload(&shd_tex, shd_rgba, NW, NH);
        glDisable(GL_ALPHA_TEST);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
        glBlendFunc(GL_ZERO, GL_SRC_COLOR);
        draw_layer(shd_tex);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glEnable(GL_ALPHA_TEST);
    }
    if (any_text) {
        upload(&txt_tex, txt_rgba, NW, NH);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
        glBlendFunc(GL_ONE_MINUS_DST_ALPHA, GL_DST_ALPHA);
        draw_layer(txt_tex);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    }
    glDisable(GL_BLEND);
    glDisable(GL_ALPHA_TEST);
    { static int nolut = -1; if (nolut < 0) { const char *e = getenv("RR_NO_LUT"); nolut = e && *e == '1'; }
      if (!nolut) post_lut(vw, vh); }
}

bool rr_gl_write_ppm(const char *path, int vw, int vh)
{
    uint8_t *px = malloc((size_t)vw * vh * 3);
    if (!px) return false;
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, vw, vh, GL_RGB, GL_UNSIGNED_BYTE, px);
    FILE *f = fopen(path, "wb");
    if (!f) { free(px); return false; }
    fprintf(f, "P6\n%d %d\n255\n", vw, vh);
    for (int y = vh - 1; y >= 0; y--) fwrite(px + (size_t)y * vw * 3, 1, (size_t)vw * 3, f);
    fclose(f);
    free(px);
    return true;
}

/* ------------------------------------------------ a headless GL context */
static SDL_Window   *hl_win;
static SDL_GLContext hl_ctx;

bool rr_gl_open_headless(int w, int h)
{
    SDL_SetHint("SDL_VIDEODRIVER", "offscreen");
    setenv("SDL_VIDEODRIVER", "offscreen", 1);         /* SDL < 2.0.22 reads only the environment */
    if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
        SDL_SetHint("SDL_VIDEODRIVER", ""); setenv("SDL_VIDEODRIVER", "", 1);
        if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) { fprintf(stderr, "[GL] no video: %s\n", SDL_GetError()); return false; }
    }
    rr_gl_context_attributes();
    hl_win = SDL_CreateWindow("rr", 0, 0, w, h, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (!hl_win) { fprintf(stderr, "[GL] no window: %s\n", SDL_GetError()); return false; }
#ifdef _WIN32
    { extern SDL_GLContext eng_gl_create_win(SDL_Window **, const char **); const char *miss = NULL;
      hl_ctx = eng_gl_create_win(&hl_win, &miss); }
#else
    hl_ctx = SDL_GL_CreateContext(hl_win);
#endif
    if (!hl_ctx) { fprintf(stderr, "[GL] no context: %s\n", SDL_GetError()); return false; }
    fprintf(stderr, "[GL] headless: %s\n", (const char *)glGetString(GL_RENDERER));
    return true;
}

void rr_gl_context_attributes(void)
{
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);            /* the prioverchar bit */
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
}
