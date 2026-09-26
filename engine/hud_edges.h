/*
 * hud_edges.h -- WIDESCREEN: the HUD goes to the edges (engine/hud_edges.c). Prop Cycle's rule (src/renderer_3d.c
 * hud_corners_shift), for every other game: in a wide picture the 3D scene is drawn wider (Hor+: the same projection, more of the
 * world at the sides -- nothing is stretched), and the game's HUD, which the board lays out for a 640x480 frame, slides out by the
 * extra width: what sits left of the middle goes left, what sits right of it goes right, and anything within ENG_HUD_DEADZONE of the
 * middle (a mirror, a radar, a crosshair, a banner) stays. Nothing is scaled; a piece keeps its own pixels.
 * (Its full-frame HUD polygons are found by priority band and/or by depth 0 -- see eng_hud_cfg.)
 *
 * It applies only while the game shows its race HUD (a game supplies the marker: text cells that hold a given tile only then), so
 * the title, menus and results, which are 640x480 compositions, are drawn exactly as the game laid them out.
 *
 * What moves, by layer:
 *   text layer   its ELEMENTS (eng_hud_text_scan: connected pieces of drawn pixels, decided by the centre of the whole piece)
 *   sprites      per sprite, HUD ones only (the game names the highest sprite depth that is HUD, not a billboard in the world)
 *   polygons     a sub-window viewport (radar, needle, map) with all its polygons, by the window's centre; and, where a game draws
 *                its HUD polygons in the full-frame viewport (Rave Racer's map dots), those of the game's HUD priority band, by the
 *                object's centre -- the ones that lie inside a sub-window's rectangle (its frame) go with the window
 */
#ifndef ENG_HUD_EDGES_H
#define ENG_HUD_EDGES_H
#include <stdint.h>
#include <stdbool.h>
#include "geo_hw.h"

#define ENG_HUD_DEADZONE 60              /* scene units either side of the middle that never move (Prop Cycle's 60) */

/* one HUD marker: the text cell (row, col of the 64x64 tilemap) holds this tile word only while the HUD is up */
typedef struct { int row, col; uint16_t tile; } eng_hud_mark;

/* What a game tells the engine about its HUD (a board's frame composer keeps a pointer to it). No marks = the HUD stays in the 4:3 centre. */
typedef struct {
    const eng_hud_mark *marks; int n_marks;  /* text cells that hold these tiles only while the HUD is up (any one showing = up) */
    int sprite_zmax;                         /* a sprite is HUD when its depth (z & 0x1FFFFF) is at most this; deeper ones are in the world */
    int quad_band;                           /* priority band (zsort bits 23:21) of the screen-space HUD polygons in the full-frame viewport; -1 = none */
    bool quad_zero_depth;                    /* ...and/or: a full-frame polygon whose depth (zsort & 0x1FFFFF) is 0 is one too (Rave Racer's tacho
                                              * needle: band 7, depth 0; every world polygon has a depth) */
} eng_hud_cfg;

extern int g_eng_hud_e;                  /* this frame's shift in scene units (0 = the HUD stays in the 4:3 centre) */

/* Per shown frame, once g_scene_x0 is set. hud_on: the game is showing its HUD. E = the extra width at each side (whole units, so a
 * piece keeps its texel alignment at every integer render scale). ENG_HUD_CENTER=1 keeps the HUD in the middle. */
void eng_hud_begin(bool hud_on);

/* Is the marker up? tile words are big-endian in `text` (the 64x64 tilemap's RAM, 2 bytes a cell). With HYSTERESIS: it stays on for a
 * few updates after the marker last showed, so a label that blinks does not make the layout jump. Call once per screen update. */
bool eng_hud_marks_up(const uint8_t *text, const eng_hud_mark *marks, int n, int *hold);

/* How far an element centred at scene x moves. */
int  eng_hud_dx(double cx);

/* TEXT LAYER. Give it the layer's coverage (occ: 640x480, non-zero where the layer draws a pixel -- RGBA `a` for a layer with alpha,
 * or any mask a game builds). It finds the elements: 4x4-pixel cells, drawn cells joined to their 8 neighbours and, along a row, to
 * cells up to 32 px away when both pieces are a line of text on the same line (48 px tall or less, overlapping by 70% of the shorter
 * one's height: the words of one sentence stay together; a counter with its icons, or a frame above the text, is not joined), each
 * piece decided by the centre of its bounding box (a piece wider
 * than 400 px is a banner and stays). Call when the layer's pixels change. */
void eng_hud_text_scan(const uint8_t *occ);
void eng_hud_text_scan_rgba(const uint8_t *rgba);      /* the same, from 640x480 RGBA (alpha byte) */
/* Draw the layer texture `tex` (640x480, bound by the caller through glBindTexture with the state the caller wants) cell run by cell run,
 * each piece at its shifted place. Only when g_eng_hud_e != 0 and a scan exists. */
void eng_hud_text_draw(void);

/* POLYGONS. eng_hud_quads_scan(buf, n) lists the sub-window viewports of the frame's quads; eng_hud_quad_dx(q) is how far one quad
 * moves. hud_band: the priority band (zsort bits 23:21) a game's own screen-space polygons are in, or -1 for none. Install
 * eng_hud_quad_dx as g_eng_quad_dx (engine/quad_gl.h). */
void eng_hud_quads_scan(const geo_quad *buf, int n, int hud_band, bool zero_depth);
int  eng_hud_quad_dx(const geo_quad *q);
#endif
