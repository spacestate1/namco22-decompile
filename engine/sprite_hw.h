/*
 * sprite_hw.h — C374 sprite layer (engine/sprite_hw.c).
 *
 * Port of tools/pc_sprite_model.py from the Prop Cycle MiSTer project,
 * itself a faithful port of MAME namcos22_v.cpp
 *   draw_sprites -> draw_sprite_group -> render_sprite
 *   -> poly3d_drawsprite -> renderscanline_sprite
 *
 * This is the layer the 3D pass cannot draw: the gold TIME/POINT HUD
 * frames, the banners, the plate over the score, and the score digits
 * themselves (which live in the VICS banks, not the main sprite bank).
 */
#ifndef SPRITE_HW_H
#define SPRITE_HW_H

#include <stddef.h>
#include <stdint.h>
#include "fog_hw.h"

typedef struct {
    uint32_t *spr;        /* :spriteram share, big-endian words */
    int       spr_n;
    uint32_t *vicsd;      /* :vics_data share */
    int       vicsd_n;
    uint32_t  vicsc[0x20];/* vics control regs 0x940000..0x94007F */
    int       have_vics;
    const uint8_t *pal;   /* raw planar palette block (R/G/B at 0/0x8000/0x10000) */
    int       owns_pal;   /* 1 if pal must be freed with this state */
    int       valid;
} sprite_state;

/* Load one frame's 2D state from a capture directory. Returns 1 on success.
 * `share_pal` may point at a previously loaded palette to reuse when the
 * bytes are identical (the palette does not change frame to frame). */
int  sprite_load_frame_into(sprite_state *st, const char *dir, int frame,
                            const uint8_t *share_pal);
void sprite_free(sprite_state *st);
/* Point a sprite_state at a board's RAM (engine/sprite_hw.c): sprite RAM and VICS data as big-endian BYTES, the 32 VICS
 * control words in host order, the planar palette. Uses internal scratch buffers that are refilled each call; the state
 * must not outlive the next call. */
#define SPR_RAM_MAX  0x030000
#define SPR_VICS_MAX 0x010000
int  sprite_load_regs(sprite_state *st, const uint8_t *spriteram, size_t spriteram_size,
                      const uint8_t *vics_data, size_t vics_size, const uint32_t vics_ctl[0x20], const uint8_t *pal);

/* The sprite ROM, 32x32 8bpp tiles (1024 bytes each), and its size in bytes: the game loads them (Prop Cycle 4 MB from
 * pr1scg0-1, Tokyo Wars 8 MB from tw1scg0-3). A tile code past the end draws nothing. */
extern uint8_t *g_sprite_tiles;
extern size_t   g_sprite_tiles_size;
/* Dirt Dash: the VICS bank's sprite count is in its list (the byte at the list's page start, plus one), not in a control register */
extern int      g_sprite_vics_count_in_list;

/* Render the sprite layer into a 640x480 RGBA buffer (alpha 0 = untouched)
 * and a matching priority buffer. */
/* compose_alpha = 0: model-exact. Translucent pixels are pre-blended
 *   against the sprite canvas (black where nothing is under), which is what
 *   pc_sprite_model.py does and what tools gate against.
 * compose_alpha = 1: emit the sprite colour with its blend factor in the
 *   alpha channel instead, so the caller can blend it against the REAL
 *   framebuffer. The hardware mixer composites the layers, so a translucent
 *   plate must see the 3D beneath it -- pre-blending against black hides
 *   whatever is under it (the score digits, in propcycl's HUD). */
void sprite_render_ex(const sprite_state *st, const fog_state *fog,
                      uint8_t *rgba, uint8_t *prio, int compose_alpha);
void sprite_render(const sprite_state *st, const fog_state *fog,
                   uint8_t *rgba, uint8_t *prio);

/* ---- z-merged drawing --------------------------------------------------
 * MAME does NOT composite sprites as a layer over the polygons. Both
 * draw_sprites() and draw_polygons() queue into the SAME radix tree keyed
 * on a 24-bit z (sprites use zcoord = attr[0] & 0xFFFFFF, polygons use
 * zsort), and render_scene walks it far-to-near in one pass
 * (namcos22_v.cpp:1679 / :1086 / :681). So a sprite can be behind a
 * polygon -- which is how the score digits show through the HUD plate.
 *
 * sprite_collect() returns the frame's sprites as draw items with that z
 * and a screen bounding box; the renderer merges them into its own quad
 * sort and calls sprite_render_item() when each one's turn comes. */
typedef struct {
    uint32_t z;              /* zcoord, same space as polygon zsort */
    int      x0, y0, w, h;   /* screen bbox, clipped */
    int      idx;            /* index into the frame's sprite list */
    int      prioverchar;    /* drawn over the text layer (the text shows nothing where this sprite has a pixel) */
    int      tile;           /* the sprite's first tile number (which art it is: a game can tell a HUD's parts by it) */
} sprite_item;

int  sprite_collect(const sprite_state *st, const fog_state *fog,
                    sprite_item *out, int max_items);
/* Render one sprite into a w*h RGBA buffer whose origin is (it->x0, it->y0). */
void sprite_render_item(const sprite_state *st, const fog_state *fog,
                        const sprite_item *it, uint8_t *rgba, int compose_alpha);

#define SPR_W 640
#define SPR_H 480

#endif /* SPRITE_HW_H */
