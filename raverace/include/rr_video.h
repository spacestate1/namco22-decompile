#ifndef RR_VIDEO_H
#define RR_VIDEO_H
#include <stdint.h>
#include <stdbool.h>

extern uint32_t g_frame_rgb[640 * 480];        /* the last frame, 0x00RRGGBB */

bool rr_video_init(const char *rom_dir);
void rr_video_frame(bool slave_active);        /* MAME screen_update_namcos22 */
void rr_video_direct_poly(const uint16_t *src);/* master port 0xC, 0x1C words */
void rr_video_pdp_begin(bool in_vblank);       /* master port 2 read */
void rr_video_render_refresh(void);            /* master port 8 write */
bool rr_video_write_ppm(const char *path);      /* the current output, at its own size */
/* Render size (default 640x480 = the board's picture, exactly). Wider than
 * 4:3 is widescreen. Takes effect at the next rr_video_frame(). */
void rr_video_set_size(int w, int h);
const uint32_t *rr_video_output(int *w, int *h); /* the last frame at the render size */
bool rr_video_render_dump(const char *dir, int frame, const char *out_ppm);

#endif
