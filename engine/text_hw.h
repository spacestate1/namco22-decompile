/*
 * text_hw.h — Super 22 text tilemap layer (engine/text_hw.c).
 *
 * Port of tools/render_text.py from the Prop Cycle MiSTer project, which
 * reproduces this layer pixel-exact against MAME (4120/4120 on its title
 * screen). 64x64 tiles of 16x16 px, 4bpp packed chars, transparent pen 0xF.
 */
#ifndef TEXT_HW_H
#define TEXT_HW_H

#include <stdint.h>
#include "fog_hw.h"

typedef struct {
    const uint8_t *cgram; /* 0x20000; textram is the tail 8 KB */
    int       owns_cg;
    const uint8_t *pal;   /* raw planar palette, shared with sprite_state */
    int       owns_pal;
    uint16_t  attr[8];    /* tilemapattr, big-endian u16 */
    int       valid;
} text_state;

int  text_load_frame_into(text_state *st, const char *dir, int frame,
                          const uint8_t *share_pal, const uint8_t *share_cg);
void text_free(text_state *st);
/* Point a text_state at a board's RAM (engine/text_hw.c): CGRAM with the text RAM as its tail, planar palette, host-order
 * tilemap attribute words. */
int  text_load_regs(text_state *st, const uint8_t *cg, const uint8_t *pal, const uint16_t attr[8]);

/* gate_mode = 1: fill the mixer background, apply the final gamma, and
 *   blend text alpha against the layer itself -- what render_text.py does,
 *   so the output can be diffed against it.
 * gate_mode = 0: transparent where the layer draws nothing, no gamma (the
 *   composed frame is gamma-corrected once at read-back), and alpha left in
 *   the alpha channel for the caller to blend against the real frame. */
void text_render(const text_state *st, const fog_state *fog,
                 uint8_t *rgba, int gate_mode);

#endif /* TEXT_HW_H */
