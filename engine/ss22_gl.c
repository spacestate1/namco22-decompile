/*
 * ss22_gl.c -- a Super System 22 frame through the shared engine (see ss22_gl.h).
 *
 * Order of a frame, as Prop Cycle's renderer does it and MAME's namcos22_v.cpp defines it:
 *   1. clear to the mixer's background colour
 *   2. quads and sprites in ONE far-to-near walk (zsort / sprite z), each quad through the engine's rasteriser with
 *      CZ fog applied after shading and the mixer's poly fade; each sprite rendered to its bounding box and blended
 *   3. the screen fade over everything drawn so far (mixer flag bit 0)
 *   4. the text tilemap over that
 *   5. the mixer's gamma tables over the whole frame
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL2/SDL.h>
#include "eng_gl.h"
#include "eng.h"
#include "geo_hw.h"
#include "slave_list.h"
#include "quad_gl.h"
#include "tex_bake.h"
#include "post_gl.h"
#include "fog_hw.h"
#include "text_hw.h"
#include "sprite_hw.h"
#include "ss22_gl.h"
#include "hud_edges.h"

#define NW ENG_SCREEN_W
#define NH ENG_SCREEN_H

/* ------------------------------------------------------------------ the quads */
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

/* Super 22 fog for one quad: the CZ tables (fog_hw.c), colour = the mixer's fog colour */
static int ss22_fog_quad(const geo_quad *q, eng_fog *f)
{
    const uint8_t *tab = NULL; int sdelta = 0;
    if (!fog_quad(q->color, q->cz_type, &tab, &sdelta)) return 0;
    memcpy(f->rgb, g_fog.fog_rgb, 3);
    f->tab = tab; f->sdelta = sdelta;
    return 1;
}

/* the mixer's poly fade, per channel, after fog */
static void ss22_fade_rgb(float *r, float *g, float *b)
{
    if (g_fog_valid && g_fog.poly_fade_enabled) {
        *r *= g_fog.poly_fade[0] / 256.0f;
        *g *= g_fog.poly_fade[1] / 256.0f;
        *b *= g_fog.poly_fade[2] / 256.0f;
    }
}

static void ss22_qhist_report(void);
static void ss22_cliplog_report(void);

/* ------------------------------------------------------------------ prepared state */
static text_state      tst;
static sprite_state    sst;
static bool            have_spr;
static sprite_item     items[1024];
static int             ni;
static uint8_t         cgbuf[0x20000];
static bool            text_off, spr_off;
static uint8_t         prio_mask[SPR_W * SPR_H];   /* pixels a sprite drawn over the text layer covers this frame */
static bool            prio_any;
static const uint16_t *spot_src;             /* the text layer's spot RAM this frame, for the cache key */
static bool            spot_on;
static int             frames;
static bool            hud_on;               /* the game is showing its race HUD (widescreen moves it to the edges) */
static const eng_hud_cfg *hud_cfg;           /* the game's HUD description, set by its board host (NULL = the HUD stays put) */

void ss22_gl_set_hud(const eng_hud_cfg *h) { hud_cfg = h; }

void ss22_prepare(const ss22_regs *r)
{
    if (!frames) {
        const char *e = getenv("ENG_NO_TEXT");    text_off = e && *e != '0';
        e = getenv("ENG_NO_SPRITES");             spr_off  = e && *e != '0';
    }
    frames++;
    g_eng_frame++;
    text_set_spot(r->spotram, r->spot_enabled);
    spot_src = r->spotram; spot_on = r->spotram && r->spot_enabled;

    fog_load_regs(r->mixer, r->czattr, r->czram);
    eng_palette_from_planar(r->pal, 0x8000);

    /* the text model indexes cg[0x1E000:0x20000] as textram, and a board keeps the two regions apart */
    memcpy(cgbuf, r->cgram, 0x1E000);
    memcpy(cgbuf + 0x1E000, r->textram, 0x2000);
    text_load_regs(&tst, cgbuf, r->pal, r->tilemapattr);

    { static int hold;                                                /* widescreen: is the race HUD up? */
      hud_on = hud_cfg && hud_cfg->marks && eng_hud_marks_up(r->textram, hud_cfg->marks, hud_cfg->n_marks, &hold); }

    ni = 0; have_spr = false;
    if (!spr_off && g_sprite_tiles && r->spriteram &&
        sprite_load_regs(&sst, r->spriteram, r->spriteram_size, r->vics, r->vics_size, r->vics_ctl, r->pal)) {
        have_spr = true;
        ni = sprite_collect(&sst, &g_fog, items, (int)(sizeof items / sizeof items[0]));
        static int sprlog = -1;                      /* ENG_SPRLOG=<n>: the sprite list of update n, one line each (finding a HUD's parts) */
        if (sprlog < 0) { const char *e = getenv("ENG_SPRLOG"); sprlog = e ? atoi(e) : 0; }
        if (sprlog && frames == sprlog)
            for (int i = 0; i < ni; i++)
                fprintf(stderr, "[SPR] %3d z %06X layer %u  x %4d y %4d  %3d x %3d  idx %d  tile %d%s\n", i, items[i].z, items[i].z >> 21, items[i].x0, items[i].y0,
                        items[i].w, items[i].h, items[i].idx, items[i].tile, items[i].prioverchar ? "  prio" : "");
    }

    qn = 0; qorder = 0;
    if (r->walk) {
        eng_list_cfg cfg = { ENG_LIST_HEAD_SS22, 0, NULL, NULL, NULL, NULL };
        eng_walk_list(r->poly_word, &cfg, push_quad, NULL);
        eng_quad_sort(qbuf, qn, 0);
    }
    ss22_qhist_report();
    ss22_cliplog_report();
}

int ss22_quads(void)   { return qn; }

/* ENG_QHIST=<n>: where the game's polygons land horizontally in update n (screen x of each quad's left edge and right edge, 128 px bins from
 * -1024 to 1664) -- how far past the 4:3 window the game's own culling lets the picture go */
static void ss22_cliplog_report(void)                /* ENG_CLIPLOG=<n>: the distinct viewport clip windows (l r t b) of update n and their quad counts */
{
    static int want = -1;
    if (want < 0) { const char *e = getenv("ENG_CLIPLOG"); want = e ? atoi(e) : 0; }
    if (!want || frames != want) return;
    int32_t seen[64][4]; int cnt[64], ns = 0;
    for (int i = 0; i < qn; i++) {
        int k;
        for (k = 0; k < ns; k++) if (!memcmp(seen[k], qbuf[i].clip, sizeof seen[k])) break;
        if (k == ns && ns < 64) { memcpy(seen[ns], qbuf[i].clip, sizeof seen[0]); cnt[ns++] = 0; }
        if (k < 64) cnt[k]++;
    }
    for (int k = 0; k < ns; k++) fprintf(stderr, "[CLIP] %4d..%4d x %4d..%4d  %d quads\n", seen[k][0], seen[k][1], seen[k][2], seen[k][3], cnt[k]);
}

static void ss22_qhist_report(void)
{
    static int want = -1;
    if (want < 0) { const char *e = getenv("ENG_QHIST"); want = e ? atoi(e) : 0; }
    if (!want || frames != want) return;
    int lo[24] = { 0 }, hi[24] = { 0 };
    for (int i = 0; i < qn; i++) {
        int mn = 1 << 30, mx = -(1 << 30);
        for (int k = 0; k < qbuf[i].nrv; k++) { int x = qbuf[i].rv[k].sx16 >> 4; if (x < mn) mn = x; if (x > mx) mx = x; }
        int a = (mn + 1024) / 128, b = (mx + 1024) / 128;
        if (a < 0) a = 0; if (a > 23) a = 23; if (b < 0) b = 0; if (b > 23) b = 23;
        lo[a]++; hi[b]++;
    }
    for (int i = 0; i < qn; i++) {                       /* few quads (a menu screen): each one in full; else the wide screen-space ones (one depth, 300+ px) */
        int mn = 1 << 30, mx = -(1 << 30);
        for (int k = 0; k < qbuf[i].nrv; k++) { const int x = qbuf[i].rv[k].sx16 >> 4; if (x < mn) mn = x; if (x > mx) mx = x; }
        const bool ss = qbuf[i].nrv == 4 && qbuf[i].rv[0].z == qbuf[i].rv[1].z && qbuf[i].rv[1].z == qbuf[i].rv[2].z && qbuf[i].rv[2].z == qbuf[i].rv[3].z && mx - mn >= 300;
        if (qn > 12 && !ss) continue;
        {
            int mn = 1 << 30, mx = -(1 << 30), my0 = 1 << 30, my1 = -(1 << 30);
            for (int k = 0; k < qbuf[i].nrv; k++) {
                const int x = qbuf[i].rv[k].sx16 >> 4, y = qbuf[i].rv[k].sy16 >> 4;
                if (x < mn) mn = x; if (x > mx) mx = x; if (y < my0) my0 = y; if (y > my1) my1 = y;
            }
            fprintf(stderr, "[QUAD] %d  x %d..%d  y %d..%d  zsort %06X  clip %d..%d  nrv %d  z0..z3 %d %d %d %d  uv %d..%d x %d..%d  bank %d pal %d\n", i, mn, mx, my0, my1,
                    qbuf[i].zsort & 0xffffff, qbuf[i].clip[0], qbuf[i].clip[1], qbuf[i].nrv, (int)qbuf[i].v[0].z, (int)qbuf[i].v[1].z, (int)qbuf[i].v[2].z, (int)qbuf[i].v[3].z,
                    qbuf[i].uvbox[0], qbuf[i].uvbox[1], qbuf[i].uvbox[2], qbuf[i].uvbox[3], qbuf[i].texbank, (qbuf[i].color >> 8) & 0x7F);
            for (int k = 0; k < qbuf[i].nrv; k++)
                fprintf(stderr, "[QUAD]    v%d  sx %d sy %d  u %u v %u  bri %d\n", k, qbuf[i].rv[k].sx16 >> 4, qbuf[i].rv[k].sy16 >> 4, qbuf[i].rv[k].u, qbuf[i].rv[k].v, qbuf[i].rv[k].bri);
        }
    }
    fprintf(stderr, "[QHIST] %d quads; bins of 128 px from -1024 (left edge | right edge)\n", qn);
    for (int i = 0; i < 24; i++) fprintf(stderr, "[QHIST] %5d..%5d  %5d %5d\n", i * 128 - 1024, i * 128 - 897, lo[i], hi[i]);
}
int ss22_sprites(void) { return ni; }

/* ------------------------------------------------------------------ drawing */
static bool    gl_ready;
static GLuint  spr_tex, txt_tex;
static uint8_t *spr_buf, *txt_buf;

static void tex_alloc(GLuint *t)
{
    glGenTextures(1, t);
    glBindTexture(GL_TEXTURE_2D, *t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, SPR_W, SPR_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
}

/* one sprite in its turn of the merged z order: rendered into the corner of one 640x480 texture, sub-imaged, blended */
static void draw_sprite(const sprite_item *it)
{
    if (!spr_buf && !(spr_buf = malloc((size_t)SPR_W * SPR_H * 4))) return;
    sprite_render_item(&sst, &g_fog, it, spr_buf, 1);
    if (it->prioverchar) {                    /* the text layer shows nothing under this sprite's pixels */
        for (int y = 0; y < it->h; y++)
            for (int x = 0; x < it->w; x++)
                if (spr_buf[((size_t)y * it->w + x) * 4 + 3]) prio_mask[(size_t)(it->y0 + y) * SPR_W + it->x0 + x] = 1;
        prio_any = true;
    }
    if (!spr_tex) tex_alloc(&spr_tex); else glBindTexture(GL_TEXTURE_2D, spr_tex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, it->w, it->h, GL_RGBA, GL_UNSIGNED_BYTE, spr_buf);
    const float su = (float)it->w / SPR_W, sv = (float)it->h / SPR_H;
    glEnable(GL_TEXTURE_2D);
    glDisable(GL_ALPHA_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
    /* widescreen: a HUD sprite goes out to its side (a sprite that is deep in the world, a billboard, stays where the 3D puts it) */
    const int dx = (g_eng_hud_e && hud_cfg && (int)(it->z & 0x1FFFFF) <= hud_cfg->sprite_zmax) ? eng_hud_dx(it->x0 + it->w / 2.0) : 0;
    glBegin(GL_QUADS);
    glTexCoord2f(0,  0);  glVertex2f((float)(it->x0 + dx),          (float)it->y0);
    glTexCoord2f(su, 0);  glVertex2f((float)(it->x0 + it->w + dx), (float)it->y0);
    glTexCoord2f(su, sv); glVertex2f((float)(it->x0 + it->w + dx), (float)(it->y0 + it->h));
    glTexCoord2f(0,  sv); glVertex2f((float)(it->x0 + dx),          (float)(it->y0 + it->h));
    glEnd();
    glDisable(GL_BLEND);
    glEnable(GL_ALPHA_TEST);
    glDisable(GL_TEXTURE_2D);
}

/* the text tilemap, drawn last: transparent where it draws nothing, its alpha left for GL to blend. text_render is a
 * pure function of the RAM it reads, so it re-renders only when a hash of those inputs changes. */
/* WIDESCREEN: a full-screen SHADE in the text layer (Dirt Dash's jungle: black, translucent, darkest at the top and bottom -- MAME's text spot/alpha) is
 * laid out for 640 px and, drawn as it is, stops at the 4:3 edges: a dark rectangle in the middle of a wider picture. Its outermost columns -- the first
 * from each side where most rows are that kind of pixel (translucent and dark; HUD text is opaque) -- are repeated outward to the picture's edges, row by
 * row, so the vignette carries on. Nothing else in the layer is ever repeated. ENG_TEXT_EXTEND=0 turns it off. */
/* A pixel of a full-screen shade: translucent and dark (HUD text is opaque). */
static bool is_shade(const uint8_t *q) { return q[3] && q[3] < 255 && q[0] <= 24 && q[1] <= 24 && q[2] <= 24; }

/* WIDESCREEN, a shade under a moving HUD. The HUD's text is moved out piece by piece, and a piece is a connected run of drawn pixels -- a full-screen
 * shade is drawn pixels too, and joined every label of the layer into one screen-wide "banner" that (rightly) stays put: the sprites' digits went to the
 * corners and their labels stayed in the middle. So while the HUD moves, a layer with a shade is drawn in two passes: the SHADE alone, where it was (with
 * the holes under the text filled from the shade beside them, or the text's old places would show as bright silhouettes), then the TEXT alone, piece
 * by piece, at its side. The piece scan sees the text only. */
static uint8_t *shd_buf, *only_buf;                 /* the shade (holes filled), and everything else */
static GLuint   shd_tex;
static bool     shade_split;                        /* this layer holds a full-screen shade and the HUD may move */

static void text_hud_scan(const uint8_t *rgba)
{
    static uint8_t occ[SPR_W * SPR_H];
    long nshade = 0;
    for (long i = 0; i < (long)SPR_W * SPR_H; i++) { const uint8_t *q = rgba + i * 4; const bool sh = is_shade(q); nshade += sh; occ[i] = q[3] && !sh; }
    eng_hud_text_scan(occ);
    shade_split = nshade > (long)SPR_W * SPR_H / 8;
    if (!shade_split) return;
    if (!shd_buf) shd_buf = malloc((size_t)SPR_W * SPR_H * 4);
    if (!only_buf) only_buf = malloc((size_t)SPR_W * SPR_H * 4);
    if (!shd_buf || !only_buf) { shade_split = false; return; }
    for (int y = 0; y < SPR_H; y++) {
        const uint8_t *row = rgba + (size_t)y * SPR_W * 4;
        uint8_t *sr = shd_buf + (size_t)y * SPR_W * 4, *tr = only_buf + (size_t)y * SPR_W * 4;
        for (int x = 0; x < SPR_W; x++) {
            const uint8_t *q = row + (size_t)x * 4;
            if (is_shade(q)) { memcpy(sr + (size_t)x * 4, q, 4); memset(tr + (size_t)x * 4, 0, 4); }
            else             { memcpy(tr + (size_t)x * 4, q, 4); memset(sr + (size_t)x * 4, 0, 4); }
        }
        for (int x = 0; x < SPR_W; x++) {               /* the shade behind a text pixel: what its neighbours in the row have */
            const uint8_t *q = row + (size_t)x * 4;
            if (!q[3] || is_shade(q)) continue;
            for (int d = 1; d <= 48; d++) {
                const uint8_t *l = x - d >= 0 ? row + (size_t)(x - d) * 4 : NULL, *r = x + d < SPR_W ? row + (size_t)(x + d) * 4 : NULL;
                if (l && is_shade(l)) { memcpy(sr + (size_t)x * 4, l, 4); break; }
                if (r && is_shade(r)) { memcpy(sr + (size_t)x * 4, r, 4); break; }
            }
        }
    }
}

static uint8_t ext_pix[2 * SPR_H * 4];        /* texel 0 = the left shade column, texel 1 = the right one, one row each */
static int     ext_xl = -1, ext_xr = -1;      /* the source columns (-1 = that side has no shade) */
static GLuint  ext_tex;
static bool    ext_dirty;

static void text_shade_scan(const uint8_t *rgba)
{
    static int cov[SPR_W];
    memset(cov, 0, sizeof cov);
    for (int y = 0; y < SPR_H; y++) {
        const uint8_t *row = rgba + (size_t)y * SPR_W * 4;
        for (int x = 0; x < SPR_W; x++) {
            const uint8_t *q = row + (size_t)x * 4;
            if (is_shade(q)) cov[x]++;
        }
    }
    ext_xl = ext_xr = -1;
    for (int x = 0; x < SPR_W / 2; x++) if (cov[x] >= SPR_H / 2) { ext_xl = x; break; }
    for (int x = SPR_W - 1; x >= SPR_W / 2; x--) if (cov[x] >= SPR_H / 2) { ext_xr = x; break; }
    memset(ext_pix, 0, sizeof ext_pix);
    for (int y = 0; y < SPR_H; y++)
        for (int side = 0; side < 2; side++) {
            const int x = side ? ext_xr : ext_xl;
            if (x < 0) continue;
            const uint8_t *q = rgba + ((size_t)y * SPR_W + (size_t)x) * 4;
            if (is_shade(q)) memcpy(ext_pix + ((size_t)y * 2 + (size_t)side) * 4, q, 4);
        }
    ext_dirty = true;
}

static void text_shade_draw(void)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("ENG_TEXT_EXTEND"); on = !(e && *e == '0'); }
    if (!on || g_scene_x0 > -0.5f || (ext_xl < 0 && ext_xr < 0)) return;
    if (!ext_tex) { tex_alloc(&ext_tex); ext_dirty = true; }
    glBindTexture(GL_TEXTURE_2D, ext_tex);
    if (ext_dirty) { glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 2, SPR_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, ext_pix); ext_dirty = false; }
    glBegin(GL_QUADS);
    if (ext_xl >= 0) {                                   /* from the picture's left edge to the source column: the column's own row, at the texel's centre */
        glTexCoord2f(0.25f, 0); glVertex2f(g_scene_x0, 0);  glTexCoord2f(0.25f, 0); glVertex2f((float)ext_xl, 0);
        glTexCoord2f(0.25f, 1); glVertex2f((float)ext_xl, NH); glTexCoord2f(0.25f, 1); glVertex2f(g_scene_x0, NH);
    }
    if (ext_xr >= 0) {
        glTexCoord2f(0.75f, 0); glVertex2f((float)(ext_xr + 1), 0); glTexCoord2f(0.75f, 0); glVertex2f(g_scene_x1, 0);
        glTexCoord2f(0.75f, 1); glVertex2f(g_scene_x1, NH); glTexCoord2f(0.75f, 1); glVertex2f((float)(ext_xr + 1), NH);
    }
    glEnd();
    glBindTexture(GL_TEXTURE_2D, txt_tex);
}

static void draw_text(void)
{
    if (text_off || !tst.valid) return;
    if (!txt_buf && !(txt_buf = malloc((size_t)SPR_W * SPR_H * 4))) return;
    static uint64_t last_hash; static bool have_last, txt_dirty, tex_masked; static long last_px;
    uint64_t h = 1469598103934665603ULL;
#define MIX(p, n) do { const uint8_t *_b = (const uint8_t *)(p); for (size_t _i = 0; _i < (size_t)(n); _i++) { h ^= _b[_i]; h *= 1099511628211ULL; } } while (0)
    MIX(tst.cgram, 0x20000); MIX(tst.pal, 0x18000); MIX(tst.attr, sizeof tst.attr);
    MIX(g_fog.poly_fade, sizeof g_fog.poly_fade); MIX(g_fog.gamma, sizeof g_fog.gamma);
    MIX(&g_fog.mixer_flags, 1); MIX(&g_fog.text_palbase, 1); MIX(&g_fog.text_alpha, 1); MIX(&g_fog.text_alpha_lo, 1);
    MIX(&g_fog.text_alpha_hi, 1); MIX(&g_fog.text_alpha_mask, 1); MIX(&g_fog.spot_factor, sizeof g_fog.spot_factor);
    MIX(&spot_on, sizeof spot_on); if (spot_on && spot_src) MIX(spot_src, 0x1000);
    { int fade_en = ((g_fog.mixer_flags & 2) != 0) && g_fog.screen_fade_factor;   /* text_hw.c gates the fade the same way */
      MIX(&fade_en, sizeof fade_en);
      if (fade_en) { MIX(g_fog.screen_fade, 3); MIX(&g_fog.screen_fade_factor, 1); } }
#undef MIX
    long px;
    if (have_last && h == last_hash) px = last_px;
    else {
        text_render(&tst, &g_fog, txt_buf, 0);
        px = 0;
        for (long i = 0; i < (long)SPR_W * SPR_H; i++) if (txt_buf[i * 4 + 3]) px++;
        last_hash = h; have_last = true; last_px = px; txt_dirty = true;
        if (hud_cfg && hud_cfg->marks) text_hud_scan(txt_buf);                 /* the layer's pieces (the text; a shade apart), for widescreen */
        else shade_split = false;
        text_shade_scan(txt_buf);                                              /* a full-screen shade in the layer, for widescreen */
    }
    { static int want = -1;                              /* ENG_TXTEDGE=<n>: the text layer's outermost pixel columns of update n (what a full-screen shade looks like) */
      if (want < 0) { const char *e = getenv("ENG_TXTEDGE"); want = e ? atoi(e) : 0; }
      if (want && frames == want) {
          for (int y = 0; y < SPR_H; y += 40) {
              const uint8_t *l = txt_buf + ((size_t)y * SPR_W) * 4, *m = txt_buf + ((size_t)y * SPR_W + SPR_W / 2) * 4, *r = txt_buf + ((size_t)y * SPR_W + SPR_W - 1) * 4;
              fprintf(stderr, "[TXTEDGE] y %3d  left %3d %3d %3d a%3d   mid %3d %3d %3d a%3d   right %3d %3d %3d a%3d\n", y, l[0], l[1], l[2], l[3], m[0], m[1], m[2], m[3], r[0], r[1], r[2], r[3]);
          }
          long opaque = 0, shade = 0;
          for (int y = 0; y < SPR_H; y++) for (int x = 0; x < SPR_W; x += SPR_W - 1) { const uint8_t a8 = txt_buf[((size_t)y * SPR_W + x) * 4 + 3]; if (a8 == 255) opaque++; else if (a8) shade++; }
          fprintf(stderr, "[TXTEDGE] edge columns: %ld opaque, %ld translucent of %d\n", opaque, shade, 2 * SPR_H);
      } }
    if (!px) return;
    if (!txt_tex) { tex_alloc(&txt_tex); txt_dirty = true; }
    glBindTexture(GL_TEXTURE_2D, txt_tex);
    const bool split = shade_split && g_eng_hud_e != 0;             /* a moving HUD over a shade: the text alone here, the shade in its own texture */
    static bool tex_split;
    if (txt_dirty || prio_any || tex_masked || split != tex_split) {          /* upload: the text as rendered, less what a sprite over the text layer covers */
        static uint8_t *masked;
        const uint8_t *src = split ? only_buf : txt_buf, *src2 = split ? shd_buf : NULL;
        if (prio_any) {
            if (!masked && !(masked = malloc((size_t)SPR_W * SPR_H * 4))) masked = NULL;
            if (masked) {
                memcpy(masked, src, (size_t)SPR_W * SPR_H * 4);
                for (long i = 0; i < (long)SPR_W * SPR_H; i++) if (prio_mask[i]) masked[i * 4 + 3] = 0;
                src = masked;
            }
            /* the shade gets NO such hole while the HUD moves: it stays put, and the sprite (and the text piece its hole belongs to) has gone out to its side --
             * a hole left at the old place shows the unshaded picture through it (the tacho's hub, as a bright disc) */
        }
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, SPR_W, SPR_H, GL_RGBA, GL_UNSIGNED_BYTE, src);
        if (src2) {
            if (!shd_tex) tex_alloc(&shd_tex);
            glBindTexture(GL_TEXTURE_2D, shd_tex);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, SPR_W, SPR_H, GL_RGBA, GL_UNSIGNED_BYTE, src2);
            glBindTexture(GL_TEXTURE_2D, txt_tex);
        }
        tex_masked = prio_any; tex_split = split; txt_dirty = false;
    }
    glEnable(GL_TEXTURE_2D);
    glDisable(GL_ALPHA_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
    if (g_eng_hud_e) {
        if (split) {                                      /* the shade where it was (and out to the picture's edges), then the text piece by piece */
            glBindTexture(GL_TEXTURE_2D, shd_tex);
            glBegin(GL_QUADS);
            glTexCoord2f(0, 0); glVertex2f(0, 0);
            glTexCoord2f(1, 0); glVertex2f(NW, 0);
            glTexCoord2f(1, 1); glVertex2f(NW, NH);
            glTexCoord2f(0, 1); glVertex2f(0, NH);
            glEnd();
            text_shade_draw();                            /* leaves txt_tex bound */
        }
        eng_hud_text_draw();                              /* widescreen HUD: each piece of the layer at its side */
    } else {
    glBegin(GL_QUADS);
    glTexCoord2f(0, 0); glVertex2f(0, 0);
    glTexCoord2f(1, 0); glVertex2f(NW, 0);
    glTexCoord2f(1, 1); glVertex2f(NW, NH);
    glTexCoord2f(0, 1); glVertex2f(0, NH);
    glEnd();
    }
    if (!split) text_shade_draw();                       /* widescreen: a full-screen shade reaches the picture's edges */
    glDisable(GL_BLEND);
    glEnable(GL_ALPHA_TEST);
    glDisable(GL_TEXTURE_2D);
}

void ss22_draw(int vw, int vh)
{
    if (!g_eng_pointrom) return;
    if (!gl_ready) { renderer_texture_init(); gl_ready = true; }
    g_tex_opaque = 1;                       /* the polygon path has no transparent pen */
    if (prio_any) { memset(prio_mask, 0, sizeof prio_mask); prio_any = false; }

    /* the scene's width: wider than 4:3 widens full-frame viewports (Hor+) */
    const double aspect = (double)vw / vh;
    if (aspect > 4.0 / 3.0 + 1e-3) {
        const float E = (float)((NH * aspect - NW) / 2.0);
        g_scene_x0 = -E; g_scene_x1 = NW + E;
    } else { g_scene_x0 = 0.0f; g_scene_x1 = NW; }

    eng_hud_begin(hud_on);                  /* widescreen: how far the HUD goes out (0 = it stays) */
    g_eng_quad_dx = eng_hud_quad_dx;
    eng_hud_quads_scan(qbuf, qn, hud_cfg ? hud_cfg->quad_band : -1, hud_cfg && hud_cfg->quad_zero_depth);

    glViewport(0, 0, vw, vh);
    glMatrixMode(GL_PROJECTION); glLoadIdentity();
    glOrtho(g_scene_x0, g_scene_x1, NH, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE); glDisable(GL_SCISSOR_TEST); glDisable(GL_BLEND);
    glDisable(GL_TEXTURE_2D);
    glEnable(GL_ALPHA_TEST); glAlphaFunc(GL_GREATER, 0.1f);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    /* the mixer's background, before the gamma; what no layer covers */
    glClearColor(g_fog.bg[0] / 255.0f, g_fog.bg[1] / 255.0f, g_fog.bg[2] / 255.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    eng_draw_cfg cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.shade = 1;
    cfg.fog = g_fog_valid;
    cfg.fog_before_shade = 0;               /* Super 22 */
    cfg.fog_quad = ss22_fog_quad;
    cfg.fade_rgb = ss22_fade_rgb;
    cfg.wide_backdrop = 1;                   /* a menu's screen-sized backdrop reaches the wide picture's edges (engine/quad_gl.c) */
    { static int tc = -1; if (tc < 0) { const char *e = getenv("ENG_TEXEL_CENTRE"); tc = e ? atoi(e) : 1; } cfg.texel_centre = tc; }

    /* MERGED Z ORDER, not layers: MAME queues sprites and polygons into the same radix tree keyed on a 24-bit z and
     * walks it far to near, so a sprite can sit behind a polygon (the HUD plate behind the score digits) */
    int qi = 0, si = 0;
    eng_draw_begin();
    while (qi < qn || si < ni) {
        const uint32_t qz = qi < qn ? (uint32_t)(qbuf[qi].zsort & 0xFFFFFF) : 0;
        const uint32_t sz = si < ni ? items[si].z : 0;
        if (si < ni && (qi >= qn || sz >= qz)) { eng_draw_end(); draw_sprite(&items[si++]); eng_draw_begin(); }
        else eng_draw_quad(&qbuf[qi++], &cfg);
    }
    eng_draw_end();

    /* the screen fade: blend(rgb, fade, 0xff - factor) over every pixel, background included, i.e. the fade colour at
     * alpha (factor + 1) / 256 */
    if (g_fog_valid && (g_fog.mixer_flags & 1) && g_fog.screen_fade_factor) {
        const float a = (g_fog.screen_fade_factor + 1) / 256.0f;
        glDisable(GL_TEXTURE_2D); glDisable(GL_ALPHA_TEST);
        glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glColor4f(g_fog.screen_fade[0] / 255.0f, g_fog.screen_fade[1] / 255.0f, g_fog.screen_fade[2] / 255.0f, a);
        glBegin(GL_QUADS);
        glVertex2f(g_scene_x0, 0); glVertex2f(g_scene_x1, 0); glVertex2f(g_scene_x1, NH); glVertex2f(g_scene_x0, NH);
        glEnd();
        glDisable(GL_BLEND); glEnable(GL_ALPHA_TEST);
    }

    draw_text();

    /* the mixer's gamma over the whole frame, once, as the hardware does at scanout */
    if (g_fog_valid && g_fog.have_gamma) eng_post_lut(g_fog.gamma, vw, vh);
}

/* ------------------------------------------------------------------ GL context helpers */
void eng_gl_context_attributes(void)
{
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
}

static SDL_Window   *hl_win;
static SDL_GLContext hl_ctx;

bool eng_gl_open_headless(int w, int h)
{
    SDL_SetHint("SDL_VIDEODRIVER", "offscreen");
    setenv("SDL_VIDEODRIVER", "offscreen", 1);         /* SDL < 2.0.22 reads only the environment */
    if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
        SDL_SetHint("SDL_VIDEODRIVER", ""); setenv("SDL_VIDEODRIVER", "", 1);
        if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) { fprintf(stderr, "[GL] no video: %s\n", SDL_GetError()); return false; }
    }
    eng_gl_context_attributes();
    hl_win = SDL_CreateWindow("engine", 0, 0, w, h, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
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

bool eng_gl_write_ppm(const char *path, int vw, int vh)
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
