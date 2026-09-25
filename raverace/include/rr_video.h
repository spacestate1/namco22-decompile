#ifndef RR_VIDEO_H
#define RR_VIDEO_H
/* THE SOFTWARE ORACLE (src/rr_video.c): a pixel-exact C port of MAME's
 * namcos22_v.cpp non-super path. A TEST ORACLE only (HARD RULE 2 in
 * ../CLAUDE.md): built into the dev binaries (RR_ORACLE: rr_oracle, rr_trace,
 * rr_sndoracle), never into the game, which draws with the shared engine. */
#include <stdint.h>
#include <stdbool.h>

extern uint32_t g_frame_rgb[640 * 480];        /* the last frame, 0x00RRGGBB */

bool rr_video_init(const char *rom_dir);
void rr_video_frame(bool slave_active);        /* MAME screen_update_namcos22 */
bool rr_video_write_ppm(const char *path);      /* the current output, at its own size */
/* Render size (default 640x480 = the board's picture, exactly). Wider than
 * 4:3 is widescreen. Takes effect at the next rr_video_frame(). */
void rr_video_set_size(int w, int h);
const uint32_t *rr_video_output(int *w, int *h); /* the last frame at the render size */
bool rr_video_render_dump(const char *dir, int frame, const char *out_ppm);

#endif
