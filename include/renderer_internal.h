/*
 * renderer_internal.h — Shared declarations for the 3D renderer subsystem.
 *
 * Included by renderer_3d.c and renderer_texture.c only.
 * Not part of the public API (use propcycl.h for that).
 *
 * -------------------------------------------------------------------
 * COORDINATE SYSTEM
 * -------------------------------------------------------------------
 * The Namco Super 22 hardware uses a LEFT-HANDED coordinate system:
 *   +X  = right
 *   +Y  = downward  (toward underground; player Y ≈ 73389 = below surface)
 *   +Z  = forward   (into the screen)
 *
 * OpenGL uses right-handed (Y-up, Z-toward-viewer). We bridge with:
 *   glScalef(1.0f, -1.0f, -1.0f)   — flips Y (down→up) and Z (forward→backward)
 *
 * After this scale: game (X, Y, Z) → GL (X, -Y, -Z). Positive game Z (forward)
 * becomes negative GL Z (in front of camera). Positive game Y (downward) becomes
 * negative GL Y (below camera center). This matches standard GL perspective.
 *
 * -------------------------------------------------------------------
 * GAME UNITS
 * -------------------------------------------------------------------
 * Distances are in raw game units. Reference scale:
 *   Player start:     X=192509, Y=73389,  Z=245090
 *   Terrain cell:     98304 units wide (0x18000)
 *   Gate arch depth:  ~848 units in front of camera
 *
 * -------------------------------------------------------------------
 * DSP COMMAND BUFFER PROTOCOL
 * -------------------------------------------------------------------
 * The game (M68K) writes 3D scene commands to g_sys.dspram double-buffer
 * (buffer 0 at byte offset 0x10400, buffer 1 at 0x18400). Each word = 4 bytes.
 *
 * MODE HEADERS (set current object placement mode, 2 words each):
 *   [0x8000, priority]  — terrain/simple object mode; entries are [model, x, y, z]
 *   [0x8001, priority]  — rotated object mode; entries are [model, x, y, z, sx,cx, sy,cy, sz,cz, flags]
 *   [0x8002, priority]  — same as 0x8001 when word[2] >= 0x8000
 *
 * INLINE COMMANDS (fixed word count, execute immediately):
 *   0x8002 (13 words)  — place rotated object at camera-frame position
 *                         word[2] < 0x8000 disambiguates from mode header
 *                         [0x8002, pri, model_id, x, y, z, sx,cx, sy,cy, sz,cz, flags]
 *                         Positions are absolute (dsp_cmd_set_camera does NOT subtract
 *                         the game camera W[0x0CDC/0x0CE0/0x0CE4] — see game_core.c)
 *   0x8008 (15 words)  — set transform context (matrix + position for 0x800a)
 *   0x8009 (4 words)   — render parameters (cz_adjust, cz_type)
 *   0x800a (6 words)   — render model using current 0x8008 context
 *   0x8010 (2-4 words) — end frame ([0x8010, -1]) or end list segment
 *
 * DATA ENTRIES (word < 0x8000, consumed while in current mode):
 *   Mode 0x8000: [model_id, x, y, z]                         — 4 words
 *   Mode 0x8001/0x8002: [model_id, x, y, z, sx,cx, sy,cy, sz,cz, flags]  — 11 words
 *
 * HEADING ROTATION:
 *   Terrain positions (0x8000 mode) are written as world-axis offsets
 *   (game subtracts camera X and Z before writing), so the renderer must
 *   undo the camera heading rotation with glRotatef(heading_deg, 0,1,0).
 *   0x8002 inline objects from dsp_cmd_set_camera are camera-space positions
 *   and do NOT need this rotation, so we undo it with glRotatef(-heading_deg).
 */
#ifndef RENDERER_INTERNAL_H
#define RENDERER_INTERNAL_H

#include "propcycl.h"
#include "eng_gl.h"

/* -------------------------------------------------------------------
 * Texture Cache
 * Bakes UV bounding boxes from the point ROM tilemap into GL textures.
 * Key: (min_u, min_v, range_u, range_v, texbank, pal_group)
 * Textures are immutable (ROM data) so the cache persists across frames.
 * ------------------------------------------------------------------- */

#include "tex_bake.h"   /* the engine's bake: renderer_texture_init, bake_quad_texture */

/* Per-frame texture hit/miss counters (printed every 300 frames). */

/* -------------------------------------------------------------------
 * Model Rendering
 * Walks the point ROM object/packet tree for one model and draws it.
 * ------------------------------------------------------------------- */

/* Budget: max polygons per frame before culling remaining models. */
extern int g_frame_poly_count;
#define MAX_FRAME_POLYS 5000

/* Render one model from point ROM at the given world-space position.
 * Reads point ROM packet data, samples textures, and emits GL quads.
 * pos_x/y/z are added to each vertex; no GL matrix push/pop here. */
void render_model(int model_id, float pos_x, float pos_y, float pos_z);

/* -------------------------------------------------------------------
 * DSP Command Buffer
 * ------------------------------------------------------------------- */

/* Base byte offset into g_sys.dspram for the active command buffer.
 * Updated each frame by detecting which buffer the game is writing to. */
extern uint32_t cmd_buf_base;

/* Read a 32-bit word from the DSP command buffer (native endian). */
int32_t cmdram_read32(int word_index);

/* Current camera heading in degrees, set each frame from W[0x0CEC].
 * Used by process_dsp_cmdbuf to undo heading rotation for 0x8002 inline. */
extern float g_cam_heading_deg;

/* Current 0x8008 transform context (set by 0x8008 command, consumed by 0x800a). */
extern float cur_rot[3][2];  /* 3 axes, each [sin, cos] */
extern float cur_pos[3];     /* position x/y/z */

/* Process all DSP commands in the active buffer for this frame.
 * Calls render_model() for each placed object. Static to renderer_3d.c;
 * the public entry point is renderer3d_process_dsp_commands(). */

#endif /* RENDERER_INTERNAL_H */
