/*
 * rr_scene.h -- System 22 video FRONT END (src/rr_scene.c): the board rules
 * that decide what a screen update draws, shared by the renderer in the game
 * (the engine's OpenGL pipeline, src/rr_gl.c) and the software oracle
 * (src/rr_video.c, a MAME port kept for gates only).
 *
 *   pdp_begin / render_refresh  MAME's render_frame_active: whether this
 *                               update keeps the master's display list
 *   direct polys                quads the master sends straight to the
 *                               render device (port 0xC, 0x1C words each)
 */
#ifndef RR_SCENE_H
#define RR_SCENE_H
#include <stdint.h>
#include <stdbool.h>

void rr_scene_pdp_begin(bool in_vblank);        /* master port 2 read */
void rr_scene_render_refresh(void);             /* master port 8 write */
void rr_scene_direct_poly(const uint16_t *src); /* master port 0xC, 0x1C words */

/* Call once per screen update: advances the frame and returns whether the
 * master's display list is to be walked (slave DSP on and the list current). */
bool rr_scene_frame(bool slave_active);

/* The direct polys queued since the last rr_scene_consume(), in arrival order. */
int  rr_scene_direct_count(void);
const uint16_t *rr_scene_direct(int i);         /* 0x1C words */
void rr_scene_consume(void);

/* Force the next rr_scene_frame() to walk the list (the renderer gate, which
 * draws a captured state with no CPU running). */
void rr_scene_force_walk(void);

/* Load a MAME video-state capture (tools/mame/dump_video.lua) into g_rr. */
bool rr_scene_load_capture(const char *dir, int frame);
#endif
