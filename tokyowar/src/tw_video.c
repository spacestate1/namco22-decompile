/*
 * tw_video.c -- Tokyo Wars' video state as the shared engine's Super 22 compositor wants it (engine/ss22_gl.h).
 * Prop Cycle does the same from its own memory (src/renderer_3d.c, src/pc_live2d.c); both are the same board family, so
 * nothing here draws.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tw_mem.h"
#include "tw_dsp.h"
#include "tw_video.h"
#include "eng.h"
#include "ss22_gl.h"
#include "sprite_hw.h"
#include "frame_rule.h"

bool g_tw_in_vblank;
static bool from_capture;                 /* --render-dump: no CPU or DSP is running, the capture says what is on screen */
void tw_video_pdp_begin(void)      { eng_pdp_begin(g_tw_in_vblank); }
void tw_video_render_refresh(void) { eng_render_refresh(); }

static uint32_t poly_word(int i) { return g_tw.poly[i & 0x7FFF]; }

bool tw_video_init(const char *dir)
{
    static const char *const cg[8] = { "tw1cg0.8d", "tw1cg1.10d", "tw1cg2.12d", "tw1cg3.13d",
                                       "tw1cg4.14d", "tw1cg5.16d", "tw1cg6.18d", "tw1cg7.19d" };
    if (!eng_load_texture_roms(dir, cg, "tw1ccrl.3d", "tw1ccrh.1d")) return false;

    static const char *const scg[4] = { "tw1scg0.12f", "tw1scg1.10f", "tw1scg2.8f", "tw1scg3.7f" };
    const size_t chip = 0x200000;
    uint8_t *tiles = calloc(4, chip);
    if (!tiles) return false;
    for (int i = 0; i < 4; i++) {
        char p[1024]; snprintf(p, sizeof p, "%s/%s", dir, scg[i]);
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
    g_sprite_tiles = tiles; g_sprite_tiles_size = tiles ? 4 * chip : 0;

    g_eng_pointrom = g_tw_pointrom; g_eng_pointrom_n = g_tw_pointrom_words;
    return g_eng_pointrom != NULL;
}

void tw_video_prepare(void)
{
    ss22_regs r;
    memset(&r, 0, sizeof r);
    r.walk = eng_frame_walks(from_capture || tw_dsp_slave_active());
    r.poly_word = poly_word;
    r.pal = g_tw.pal;
    r.mixer = g_tw.mixer;
    for (int i = 0; i < 8; i++) { r.czattr[i] = g_tw.czattr[i]; r.tilemapattr[i] = g_tw.tilemapattr[i]; }
    for (int b = 0; b < 4; b++) r.czram[b] = g_tw.czram[b];
    r.cgram = g_tw.cgram; r.textram = g_tw.text;
    r.spriteram = g_tw.sprite; r.spriteram_size = sizeof g_tw.sprite;
    r.vics = g_tw.vics;        r.vics_size = sizeof g_tw.vics;
    for (int i = 0; i < 0x20; i++) r.vics_ctl[i] = g_tw.vics_ctl[i];
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

bool tw_video_load_dump(const char *dir, int frame)
{
    static uint8_t raw[TW_POLY_WORDS * 4];
    from_capture = true;
    /* the capture has no CPU: draw its list, unless the frame is one MAME drew WITHOUT the master's list (its render_frame_active
     * dropped it: a black 3D layer over the 2D layers) -- TW_RENDER_NOLIST=1 says so, see tools/vid_gate.sh */
    { const char *nl = getenv("TW_RENDER_NOLIST"); if (!(nl && *nl == '1')) eng_frame_force_walk(); }
    if (!slurp(dir, "polyraw", frame, raw, sizeof raw)) return false;
    for (uint32_t i = 0; i < TW_POLY_WORDS; i++)         /* the share's own words, big-endian, upper bytes intact */
        g_tw.poly[i] = (uint32_t)raw[i * 4] << 24 | raw[i * 4 + 1] << 16 | raw[i * 4 + 2] << 8 | raw[i * 4 + 3];
    uint8_t b16[16], czb[512];
    bool ok = slurp(dir, "pal", frame, g_tw.pal, sizeof g_tw.pal) && slurp(dir, "mix", frame, g_tw.mixer, sizeof g_tw.mixer) &&
              slurp(dir, "cgram", frame, g_tw.cgram, sizeof g_tw.cgram) && slurp(dir, "text", frame, g_tw.text, sizeof g_tw.text) &&
              slurp(dir, "spr", frame, g_tw.sprite, sizeof g_tw.sprite) && slurp(dir, "vics", frame, g_tw.vics, sizeof g_tw.vics);
    if (!ok) return false;
    if (!slurp(dir, "czattr", frame, b16, 16)) return false;
    for (int i = 0; i < 8; i++) g_tw.czattr[i] = (uint16_t)(b16[i * 2] << 8 | b16[i * 2 + 1]);
    if (!slurp(dir, "tmattr", frame, b16, 16)) return false;
    for (int i = 0; i < 8; i++) g_tw.tilemapattr[i] = (uint16_t)(b16[i * 2] << 8 | b16[i * 2 + 1]);
    for (int b = 0; b < 4; b++) {
        char nm[16]; snprintf(nm, sizeof nm, "czram%d", b);
        if (!slurp(dir, nm, frame, czb, sizeof czb)) return false;
        for (int i = 0; i < 256; i++) g_tw.czram[b][i] = (uint16_t)(czb[i * 2] << 8 | czb[i * 2 + 1]);
    }
    uint8_t vc[128];
    if (!slurp(dir, "vicsctl", frame, vc, sizeof vc)) return false;
    for (int i = 0; i < 0x20; i++) g_tw.vics_ctl[i] = (uint32_t)vc[i * 4] << 24 | vc[i * 4 + 1] << 16 | vc[i * 4 + 2] << 8 | vc[i * 4 + 3];
    return true;
}
