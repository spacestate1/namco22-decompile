/*
 * rr_scene.c -- System 22 video front end (see include/rr_scene.h).
 * The render_frame_active rule is namcos22_state::pdp_begin_r /
 * render_frame_active in MAME's namcos22.cpp: the list is kept for a screen
 * update when the master began its frame during the vblank before it (the
 * frame counter it recorded is ahead), and dropped once the master has since
 * asked for a render refresh.
 */
#include <stdio.h>
#include <string.h>
#include "rr_mem.h"
#include "rr_scene.h"

#define DIRECT_MAX 4096
static uint16_t direct_q[DIRECT_MAX][0x1C];
static int      direct_n;
static bool     pdp_render_done, render_refresh;
static uint64_t pdp_frame, cur_frame;

void rr_scene_pdp_begin(bool in_vblank) { pdp_frame = cur_frame + (in_vblank ? 1 : 0); pdp_render_done = true; }
void rr_scene_render_refresh(void)      { render_refresh = true; }

void rr_scene_direct_poly(const uint16_t *src)
{
    if (direct_n < DIRECT_MAX) memcpy(direct_q[direct_n++], src, sizeof direct_q[0]);
}

bool rr_scene_frame(bool slave_active)
{
    cur_frame++;
    if (cur_frame > pdp_frame && render_refresh) pdp_render_done = false;
    render_refresh = false;
    return pdp_render_done && slave_active;
}

int  rr_scene_direct_count(void)   { return direct_n; }
const uint16_t *rr_scene_direct(int i) { return direct_q[i]; }
void rr_scene_consume(void)        { direct_n = 0; }

void rr_scene_force_walk(void) { pdp_render_done = true; render_refresh = false; pdp_frame = cur_frame + 1; }

/* ---- a MAME video-state capture (tools/mame/dump_video.lua) into g_rr: the
 * renderer gate draws MAME's exact state with no CPU running ---- */
static bool load(const char *dir, const char *what, int f, uint8_t *dst, size_t n)
{
    char p[1024];
    snprintf(p, sizeof p, "%s/%s_f%d.bin", dir, what, f);
    FILE *fp = fopen(p, "rb");
    if (!fp) { fprintf(stderr, "[SCENE] no %s\n", p); return false; }
    size_t got = fread(dst, 1, n, fp);
    fclose(fp);
    return got == n;
}

bool rr_scene_load_capture(const char *dir, int f)
{
    static uint8_t poly[0x20000], cg[0x20000], tattr[0x10];
    if (!load(dir, "poly", f, poly, sizeof poly) || !load(dir, "pal", f, g_rr.pal, RR_PAL_SIZE) ||
        !load(dir, "mixer", f, g_rr.mixer, RR_MIXER_SIZE) || !load(dir, "czram", f, g_rr.czram, RR_CZRAM_SIZE) ||
        !load(dir, "cg", f, cg, sizeof cg) || !load(dir, "tattr", f, tattr, sizeof tattr))
        return false;
    for (int i = 0; i < 0x8000; i++) {
        uint32_t w = (uint32_t)poly[4*i] << 24 | (uint32_t)poly[4*i+1] << 16 | (uint32_t)poly[4*i+2] << 8 | poly[4*i+3];
        g_rr.poly[i] = (uint32_t)((int32_t)(w << 8) >> 8);   /* signed24, as MAME stores it */
    }
    memcpy(g_rr.cgram, cg, RR_CGRAM_SIZE);
    memcpy(g_rr.text, cg + RR_CGRAM_SIZE, RR_TEXT_SIZE);
    memcpy(g_rr.tilemapattr, tattr, sizeof tattr);
    return true;
}

