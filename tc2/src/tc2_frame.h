/* tc2_frame.h -- what one frame's picture needs, snapshotted at the start of vertical blank so the picture can be composed on another thread
 * while the CPUs run on (src/tc2_render.c fills it, src/tc2_text.c and the polygon fill read it) */
#ifndef TC2_FRAME_H
#define TC2_FRAME_H
#include <stdint.h>
#include "tc2_c435.h"
typedef struct {
    uint8_t  c404[0x40];               /* the C404's registers 0x00-0x1F (big-endian words) */
    uint8_t  gamma[3][256];            /* the C404's GAMMA RAM: R / G / B curves at 0x200 / 0x400 / 0x600 (256 words each, low bytes) --
                                        * the last stage of the picture; MAME never applies it, so its picture is darker than the board's */
    uint32_t pens[0x8000];             /* the palette as 0x00RRGGBB */
    uint8_t  text[0x20000];            /* the C361's character RAM (0x06800000) and tile map (0x0681E000) */
    uint16_t linexscroll[1024], yscroll;
    uint16_t sram[0x20000];            /* the C412 SRAM: the stencil tiles */
    tc2_render_list list;              /* the render list to draw */
} tc2_frame_state;
void tc2_text_layer(const tc2_frame_state *fs, uint16_t *mix, uint8_t *pri);
void tc2_text_mix(const tc2_frame_state *fs, uint32_t *rgba, const uint16_t *mix, const uint8_t *pri, int prival);
#endif
