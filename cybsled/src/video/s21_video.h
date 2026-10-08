/*
 * s21_video.h -- Cyber Sled (Namco System 21): the picture (cybsled/DESIGN.md, module C).
 *
 * Reproduces the behaviour of MAME's namcos21_c67.cpp screen_update with its parts -- the xBRG_888 palette (main + ext word),
 * the C355 zooming sprites (shared/namco_c355spr.cpp, System 21 settings: colour base 0x1000, 16 colours of 256, scroll offsets
 * (0, 0x20), 10-bit x scroll at 480 lines, transparent pen 0xFF, external priority fill), the 3D rasteriser (namcos21_3d: flat
 * quads, back-face cull, per-quad average z in a 16-bit z buffer, depth cue by palette set, double-buffered) and the mixing
 * order: backdrop pen 0xFF, sprites of priority 2, the 3D frame buffer, then priority-0 sprites (mixed against the 3D z buffer
 * when palette-ext word 0 bits 8-10 say 4, plainly when 0 or 2), then priority 3. 496 x 480, palette-indexed, then RGB.
 * Our own code, written from that behaviour (MAME is the reference and the oracle, not a source of code in this tree).
 *
 * The RAM lives in the board (module A); this module reads it through s21_video_bind(). Words are HOST-order u16 as the 68000
 * sees them. The sprite RAM writer must use s21_video_spriteram_w(), which keeps the C355's page-0 table mirrors.
 */
#ifndef S21_VIDEO_H
#define S21_VIDEO_H
#include <stdint.h>
#include <stdbool.h>

#define S21_W 496
#define S21_H 480

/* one quad as the DSP subsystem hands it to the rasteriser (namcos21_3d blit_single_quad's input): screen x/y relative to the
 * centre, z codes, and the colour word (bits 0-7 colour, 8-14 code: bit 9 clear = the upper half of a 0x200 polygon palette) */
#ifndef S21_QUAD_DEFINED            /* the same definition is in src/dsp/s21_dsp.h, under the same guard */
#define S21_QUAD_DEFINED
typedef struct { int sx[4], sy[4], z[4]; uint16_t color; } s21_quad;
#endif

/* the board's RAM, all u16 words in host order: palette 0x740000 (0x8000 words), palette ext 0x750000 (0x8000), C355 sprite
 * RAM 0x700000 (0x10000 words), sprite position 0x720000 (4), video enable 0x760000 (1) */
void s21_video_bind(const uint16_t *pal, const uint16_t *palext, const uint16_t *spr, const uint16_t *sprpos, const uint16_t *vena);
/* the 68000's write to sprite RAM (word offset 0..0xFFFF), with the C355's page-1 -> page-0 table mirrors */
void s21_video_spriteram_w(uint16_t *spr, uint32_t woff, uint16_t data, uint16_t mem_mask);

bool s21_video_init(const char *romdir);      /* the sprite tiles: cy1-obj0..7 (ROM_LOAD32_BYTE) */

/* 3D: quads go into the WORK frame buffer; swap makes it the visible one and clears the next (the DSP subsystem calls it at the
 * point MAME's namcos21_dsp_c67 calls swap_and_clear_poly_framebuffer) */
void s21_video_quad(const s21_quad *q);
void s21_video_swap(void);

/* the frame, done at the end of each video frame (MAME's screen_update). `mode` is a set of outputs:
 *   S21_OUT_SW    the exact software picture (the oracle: 3D z-buffer rasteriser + mixing) into s21_video_rgb() / _pens(), 496x480
 *   S21_OUT_GL    the frame prepared for s21_video_draw_gl(): the 2D (sprites, backdrop) mixed SYMBOLICALLY at board resolution --
 *                 per pixel what shows with no polygon there and what a polygon pixel becomes (kept, a shadow bank of its pen, a
 *                 sprite pen, or that sprite only where the polygon's z is far enough: palette-ext pri-0 z-mixing) -- and the palette
 *   S21_OUT_BOTH  both (the GL gate: the GL picture at 496x480 against the software one, same frame)
 * The software rasteriser runs only while s21_video_set_sw3d(true) (the default): the GL picture draws the quads itself. */
enum { S21_OUT_SW = 1, S21_OUT_GL = 2, S21_OUT_BOTH = 3 };
void s21_video_set_sw3d(bool on);
void s21_video_begin_frame(void);
void s21_video_end_frame(int mode);
const uint8_t *s21_video_rgb(void);            /* the last S21_OUT_SW frame, 496*480*3 */
const uint16_t *s21_video_pens(void);          /* the same as palette indices (the gate compares these too) */
bool s21_video_write_ppm(const char *path);

/* THE GL PICTURE (s21_video_gl.c), from the last S21_OUT_GL frame, into the picture rectangle (x, y, w, h: GL window coordinates,
 * bottom-left origin) of the current framebuffer, at that resolution: the quads through the shared engine rasteriser
 * (engine/quad_gl.c: flat colour, the hardware's per-quad z in a depth test) carrying their pen and z, then one GLSL pass that
 * does the board's mixing per output pixel (palette, shadows on the polygon's own pen, pri-0 z-mixing) with the 2D layer
 * sampled sharp-bilinear (whole board pixels, crisp at any size; exactly the board's pixels at 496x480). Needs GLSL 1.10 and an
 * alpha + depth buffer; false = not available (the caller shows the software picture). */
bool s21_video_draw_gl(int x, int y, int w, int h);
/* EXPERIMENTAL widescreen: a rectangle wider than 4:3 shows the quads past the board's sides (the DSPs project for 496 pixels and
 * cull what is outside the board's view, so only polygons that straddle the edge appear there); the 2D stays in the 4:3 centre */
void s21_video_gl_wide(int on);
/* Widescreen (Hor+): the GL picture's 2D gets E board columns each side; a repeating backdrop (the sky) is continued into them.
 * 0 = the board's 496 (default). Takes effect at the next s21_video_end_frame. */
void s21_video_set_wide_extra(int e);

/* diagnostics */
int s21_video_quads_last(void);                /* quads in the visible 3D frame */
int s21_video_sprites_last(void);              /* sprites in the last list */
const uint8_t *s21_video_layers(void);         /* per pixel, what drew it last: 0 backdrop, 1+pri a sprite (| 0x10: a
                                                * shadow pixel, pen 0x00 / 0x01, which takes the pen below), 8 the 3D buffer */
#endif
