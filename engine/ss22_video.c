/*
 * ss22_video.c -- a Super 22 game's video state as the shared engine's Super 22 compositor wants it (engine/ss22_gl.h).
 * Prop Cycle does the same from its own memory (src/renderer_3d.c, src/pc_live2d.c); both are the same board family, so
 * nothing here draws.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ss22_game.h"
#include "eng.h"
#include "ss22_gl.h"
#include "sprite_hw.h"
#include "frame_rule.h"

static bool from_capture;                 /* --render-dump: no CPU or DSP is running, the capture says what is on screen */
void ss22_video_pdp_begin(void)      { eng_pdp_begin(g_ss22_in_vblank); }
void ss22_video_render_refresh(void) { eng_render_refresh(); }

static uint32_t poly_word(int i) { return g_ss22.poly[i & 0x7FFF]; }

bool ss22_video_init(const char *dir)
{
    const ss22_video_cfg *c = &g_ss22_game->video;
    ss22_gl_set_hud(&c->hud);                /* widescreen: the compositor learns this game's HUD (Prop Cycle, which links it too, never does) */
    if (!eng_load_texture_roms(dir, c->cg, c->ccrl, c->ccrh)) return false;

    const size_t chip = 0x200000, region = c->sprite_region;
    uint8_t *tiles = malloc(region);
    if (!tiles) return false;
    memset(tiles, c->sprite_fill, region);
    for (int i = 0; i < c->n_scg; i++) {
        char p[1024]; snprintf(p, sizeof p, "%s/%s", dir, c->scg[i]);
        FILE *f = fopen(p, "rb");
        if (!f || fread(tiles + i * chip, 1, chip, f) != chip) {
            fprintf(stderr, "[VIDEO] cannot read %s -- no sprites\n", p);
            if (f) fclose(f);
            free(tiles);
            tiles = NULL;
            break;
        }
        fclose(f);
    }
    g_sprite_tiles = tiles; g_sprite_tiles_size = tiles ? region : 0;
    g_sprite_vics_count_in_list = c->vics_count_in_list;

    g_eng_pointrom = g_ss22_pointrom; g_eng_pointrom_n = g_ss22_pointrom_words;
    return g_eng_pointrom != NULL;
}

void ss22_video_prepare(void)
{
    ss22_regs r;
    memset(&r, 0, sizeof r);
    r.walk = eng_frame_walks(from_capture || ss22_dsp_slave_active());
    r.poly_word = poly_word;
    r.pal = g_ss22.pal;
    r.mixer = g_ss22.mixer;
    for (int i = 0; i < 8; i++) { r.czattr[i] = g_ss22.czattr[i]; r.tilemapattr[i] = g_ss22.tilemapattr[i]; }
    for (int b = 0; b < 4; b++) r.czram[b] = g_ss22.czram[b];
    r.cgram = g_ss22.cgram; r.textram = g_ss22.text;
    r.spotram = g_ss22.spotram;
    r.spot_enabled = (g_ss22.spot_enable & 1) && (g_ss22.chipselect & 0xC000);
    r.spriteram = g_ss22.sprite; r.spriteram_size = sizeof g_ss22.sprite;
    r.vics = g_ss22.vics;        r.vics_size = sizeof g_ss22.vics;
    for (int i = 0; i < 0x20; i++) r.vics_ctl[i] = g_ss22.vics_ctl[i];
    ss22_prepare(&r);
}

/* ---- MAME's captured state (tools/mame/vid_dump.lua) ---------------------------------------------- */
static bool slurp(const char *dir, const char *name, int frame, void *dst, size_t n)
{
    char p[1024]; snprintf(p, sizeof p, "%s/%s_f%d.bin", dir, name, frame);
    FILE *f = fopen(p, "rb");
    if (!f) { fprintf(stderr, "[VIDEO] cannot open %s\n", p); return false; }
    size_t got = fread(dst, 1, n, f);
    fclose(f);
    if (got != n) { fprintf(stderr, "[VIDEO] short read %s (%zu of %zu)\n", p, got, n); return false; }
    return true;
}

bool ss22_video_load_dump(const char *dir, int frame)
{
    static uint8_t raw[SS22_POLY_WORDS * 4];
    from_capture = true;
    /* the capture has no CPU: draw its list, unless the frame is one MAME drew WITHOUT the master's list (its render_frame_active
     * dropped it: a black 3D layer over the 2D layers) -- <tag>_RENDER_NOLIST=1 says so, see tools/vid_gate.sh */
    { char v[64]; snprintf(v, sizeof v, "%s_RENDER_NOLIST", g_ss22_game->tag);
      const char *nl = getenv(v); if (!(nl && *nl == '1')) eng_frame_force_walk(); }
    if (!slurp(dir, "polyraw", frame, raw, sizeof raw)) return false;
    for (uint32_t i = 0; i < SS22_POLY_WORDS; i++)         /* the share's own words, big-endian, upper bytes intact */
        g_ss22.poly[i] = (uint32_t)raw[i * 4] << 24 | raw[i * 4 + 1] << 16 | raw[i * 4 + 2] << 8 | raw[i * 4 + 3];
    uint8_t b16[16], czb[512];
    bool ok = slurp(dir, "pal", frame, g_ss22.pal, sizeof g_ss22.pal) && slurp(dir, "mix", frame, g_ss22.mixer, sizeof g_ss22.mixer) &&
              slurp(dir, "cgram", frame, g_ss22.cgram, sizeof g_ss22.cgram) && slurp(dir, "text", frame, g_ss22.text, sizeof g_ss22.text) &&
              slurp(dir, "spr", frame, g_ss22.sprite, sizeof g_ss22.sprite) && slurp(dir, "vics", frame, g_ss22.vics, sizeof g_ss22.vics);
    if (!ok) return false;
    if (!slurp(dir, "czattr", frame, b16, 16)) return false;
    for (int i = 0; i < 8; i++) g_ss22.czattr[i] = (uint16_t)(b16[i * 2] << 8 | b16[i * 2 + 1]);
    if (!slurp(dir, "tmattr", frame, b16, 16)) return false;
    for (int i = 0; i < 8; i++) g_ss22.tilemapattr[i] = (uint16_t)(b16[i * 2] << 8 | b16[i * 2 + 1]);
    for (int b = 0; b < 4; b++) {
        char nm[16]; snprintf(nm, sizeof nm, "czram%d", b);
        if (!slurp(dir, nm, frame, czb, sizeof czb)) return false;
        for (int i = 0; i < 256; i++) g_ss22.czram[b][i] = (uint16_t)(czb[i * 2] << 8 | czb[i * 2 + 1]);
    }
    { uint8_t sp[0x1000];                                  /* the spot RAM (tools/mame/vid_dump.lua); the capture does not record its enable */
      if (slurp(dir, "spot", frame, sp, sizeof sp)) {
          for (int i = 0; i < 0x800; i++) g_ss22.spotram[i] = (uint16_t)(sp[i * 2] << 8 | sp[i * 2 + 1]);
          if (g_ss22_game->video.spot_in_dumps) { g_ss22.spot_enable = 1; g_ss22.chipselect = 0xC000; }
      } }
    uint8_t vc[128];
    if (!slurp(dir, "vicsctl", frame, vc, sizeof vc)) return false;
    for (int i = 0; i < 0x20; i++) g_ss22.vics_ctl[i] = (uint32_t)vc[i * 4] << 24 | vc[i * 4 + 1] << 16 | vc[i * 4 + 2] << 8 | vc[i * 4 + 3];
    return true;
}
