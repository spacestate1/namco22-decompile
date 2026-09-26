/*
 * ss22_gl.h -- a Super System 22 frame through the shared engine, from the board's RAM (engine/ss22_gl.c).
 *
 * The composition Prop Cycle's renderer does (src/renderer_3d.c geohw_flush + geohw_draw_text), lifted out of that file
 * so a game that is not Prop Cycle gets it without a copy: the master DSP's display list walked and painter-sorted with
 * the C374 sprites merged into the SAME z order (MAME queues both into one radix tree), Super 22 per-pixel CZ fog
 * applied after shading, the mixer's poly fade, the screen fade, the text tilemap on top, and the mixer's gamma over
 * the whole frame. Pixel-exactness of each layer is the shared modules' (fog_hw, sprite_hw, text_hw, quad_gl); what a
 * game supplies is where its RAM is.
 *
 * Everything is 640x480 in scene units; a wider window widens full-frame viewports (Hor+) through g_scene_x0/x1.
 */
#ifndef ENG_SS22_GL_H
#define ENG_SS22_GL_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "hud_edges.h"

typedef struct {
    bool            walk;                   /* this update keeps the master's list (frame_rule.h); false = no 3D, sprites and text only */
    uint32_t (*poly_word)(int index);        /* polygon RAM word (MAME's stored value, signed 24), index & 0x7FFF */
    const uint8_t  *pal;                     /* palette RAM as planar R, G, B planes of 0x8000 bytes */
    const uint8_t  *mixer;                   /* the video mixer, 0x400 bytes (0x824000) */
    uint16_t        czattr[8];               /* 0x810000, host-order words */
    const uint16_t *czram[4];                /* the four CZ banks, 256 host-order words each */
    const uint8_t  *cgram, *textram;         /* character RAM (0x1E000) and the text RAM behind it (0x2000) */
    uint16_t        tilemapattr[8];          /* host-order words */
    const uint8_t  *spriteram;  size_t spriteram_size;   /* C374, big-endian bytes */
    const uint8_t  *vics;       size_t vics_size;        /* VICS data, big-endian bytes */
    uint32_t        vics_ctl[0x20];          /* VICS control, host-order words */
    const uint16_t *spotram;                 /* the spot RAM: 0x800 host-order words (text_hw.c) */
    bool            spot_enabled;            /* (spot enable & 1) && (chipselect & 0xC000) */
} ss22_regs;

/* WIDESCREEN: the game's HUD description (engine/hud_edges.h); NULL or no marks = the HUD stays where the game put it. The pointer must
 * outlive the frames drawn. A board host calls it once; a caller that never does (Prop Cycle links this file) gets no HUD move. */
void ss22_gl_set_hud(const eng_hud_cfg *h);

/* Once per screen update: latch the state, walk the display list, sort, collect the sprites. The walk decides what the
 * update keeps, so it must happen exactly once per update. */
void ss22_prepare(const ss22_regs *r);
/* Draw the prepared frame into the current framebuffer, vw x vh pixels (any number of times after one prepare). */
void ss22_draw(int vw, int vh);
int  ss22_quads(void);                       /* quads in the prepared frame */
int  ss22_sprites(void);                     /* sprites in the prepared frame */

/* An OpenGL 2.1 compatibility context, the way the engine wants it (alpha channel, double buffered). */
void eng_gl_context_attributes(void);
/* An offscreen w x h context for headless runs (SDL's offscreen driver); false if there is no GL at all. */
bool eng_gl_open_headless(int w, int h);
/* Read the current framebuffer back as a binary PPM. */
bool eng_gl_write_ppm(const char *path, int vw, int vh);
#endif
