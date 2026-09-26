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

/* ------------------------------------------------------------------ prepared state */
static text_state      tst;
static sprite_state    sst;
static bool            have_spr;
static sprite_item     items[1024];
static int             ni;
static uint8_t         cgbuf[0x20000];
static bool            text_off, spr_off;
static int             frames;

void ss22_prepare(const ss22_regs *r)
{
    if (!frames) {
        const char *e = getenv("ENG_NO_TEXT");    text_off = e && *e != '0';
        e = getenv("ENG_NO_SPRITES");             spr_off  = e && *e != '0';
    }
    frames++;
    g_eng_frame++;

    fog_load_regs(r->mixer, r->czattr, r->czram);
    eng_palette_from_planar(r->pal, 0x8000);

    /* the text model indexes cg[0x1E000:0x20000] as textram, and a board keeps the two regions apart */
    memcpy(cgbuf, r->cgram, 0x1E000);
    memcpy(cgbuf + 0x1E000, r->textram, 0x2000);
    text_load_regs(&tst, cgbuf, r->pal, r->tilemapattr);

    ni = 0; have_spr = false;
    if (!spr_off && g_sprite_tiles && r->spriteram &&
        sprite_load_regs(&sst, r->spriteram, r->spriteram_size, r->vics, r->vics_size, r->vics_ctl, r->pal)) {
        have_spr = true;
        ni = sprite_collect(&sst, &g_fog, items, (int)(sizeof items / sizeof items[0]));
    }

    qn = 0; qorder = 0;
    if (r->walk) {
        eng_list_cfg cfg = { ENG_LIST_HEAD_SS22, 0, NULL, NULL, NULL, NULL };
        eng_walk_list(r->poly_word, &cfg, push_quad, NULL);
        eng_quad_sort(qbuf, qn, 0);
    }
}

int ss22_quads(void)   { return qn; }
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
    if (!spr_tex) tex_alloc(&spr_tex); else glBindTexture(GL_TEXTURE_2D, spr_tex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, it->w, it->h, GL_RGBA, GL_UNSIGNED_BYTE, spr_buf);
    const float su = (float)it->w / SPR_W, sv = (float)it->h / SPR_H;
    glEnable(GL_TEXTURE_2D);
    glDisable(GL_ALPHA_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
    glBegin(GL_QUADS);
    glTexCoord2f(0,  0);  glVertex2f((float)it->x0,          (float)it->y0);
    glTexCoord2f(su, 0);  glVertex2f((float)(it->x0 + it->w), (float)it->y0);
    glTexCoord2f(su, sv); glVertex2f((float)(it->x0 + it->w), (float)(it->y0 + it->h));
    glTexCoord2f(0,  sv); glVertex2f((float)it->x0,          (float)(it->y0 + it->h));
    glEnd();
    glDisable(GL_BLEND);
    glEnable(GL_ALPHA_TEST);
    glDisable(GL_TEXTURE_2D);
}

/* the text tilemap, drawn last: transparent where it draws nothing, its alpha left for GL to blend. text_render is a
 * pure function of the RAM it reads, so it re-renders only when a hash of those inputs changes. */
static void draw_text(void)
{
    if (text_off || !tst.valid) return;
    if (!txt_buf && !(txt_buf = malloc((size_t)SPR_W * SPR_H * 4))) return;
    static uint64_t last_hash; static bool have_last; static long last_px;
    uint64_t h = 1469598103934665603ULL;
#define MIX(p, n) do { const uint8_t *_b = (const uint8_t *)(p); for (size_t _i = 0; _i < (size_t)(n); _i++) { h ^= _b[_i]; h *= 1099511628211ULL; } } while (0)
    MIX(tst.cgram, 0x20000); MIX(tst.pal, 0x18000); MIX(tst.attr, sizeof tst.attr);
    MIX(g_fog.poly_fade, sizeof g_fog.poly_fade); MIX(g_fog.gamma, sizeof g_fog.gamma);
    MIX(&g_fog.mixer_flags, 1); MIX(&g_fog.text_palbase, 1); MIX(&g_fog.text_alpha, 1); MIX(&g_fog.text_alpha_lo, 1);
    MIX(&g_fog.text_alpha_hi, 1); MIX(&g_fog.text_alpha_mask, 1);
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
        last_hash = h; have_last = true; last_px = px;
        if (txt_tex) { glBindTexture(GL_TEXTURE_2D, txt_tex); glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, SPR_W, SPR_H, GL_RGBA, GL_UNSIGNED_BYTE, txt_buf); }
    }
    if (!px) return;
    if (!txt_tex) {
        tex_alloc(&txt_tex);
        glBindTexture(GL_TEXTURE_2D, txt_tex);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, SPR_W, SPR_H, GL_RGBA, GL_UNSIGNED_BYTE, txt_buf);
    } else glBindTexture(GL_TEXTURE_2D, txt_tex);
    glEnable(GL_TEXTURE_2D);
    glDisable(GL_ALPHA_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
    glBegin(GL_QUADS);
    glTexCoord2f(0, 0); glVertex2f(0, 0);
    glTexCoord2f(1, 0); glVertex2f(NW, 0);
    glTexCoord2f(1, 1); glVertex2f(NW, NH);
    glTexCoord2f(0, 1); glVertex2f(0, NH);
    glEnd();
    glDisable(GL_BLEND);
    glEnable(GL_ALPHA_TEST);
    glDisable(GL_TEXTURE_2D);
}

void ss22_draw(int vw, int vh)
{
    if (!g_eng_pointrom) return;
    if (!gl_ready) { renderer_texture_init(); gl_ready = true; }
    g_tex_opaque = 1;                       /* the polygon path has no transparent pen */

    /* the scene's width: wider than 4:3 widens full-frame viewports (Hor+) */
    const double aspect = (double)vw / vh;
    if (aspect > 4.0 / 3.0 + 1e-3) {
        const float E = (float)((NH * aspect - NW) / 2.0);
        g_scene_x0 = -E; g_scene_x1 = NW + E;
    } else { g_scene_x0 = 0.0f; g_scene_x1 = NW; }

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
