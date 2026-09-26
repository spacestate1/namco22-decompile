#ifndef TW_VIDEO_H
#define TW_VIDEO_H
#include <stdbool.h>

/* Tokyo Wars' picture through the shared engine (engine/ss22_gl.c): texture ROMs tw1cg0-7 / tw1ccrl / tw1ccrh, the sprite
 * ROM tw1scg0-3 (8 MB, 32x32 8bpp tiles), the point ROM the master DSP already loaded. What is Tokyo Wars about it is here:
 * where its video RAM lives (g_tw) and what its ROMs are called. */
void tw_video_pdp_begin(void);                        /* master DSP port 2 read (engine/frame_rule.h) */
void tw_video_render_refresh(void);                    /* master DSP port 8 write */
extern bool g_tw_in_vblank;                            /* set by the scheduler while the master answers the vblank INT0 */
bool tw_video_init(const char *rom_dir);
void tw_video_prepare(void);                          /* latch g_tw's video state and walk polygon RAM: once per frame */
bool tw_video_load_dump(const char *dir, int frame);  /* MAME's state (tools/mame/vid_dump.lua) into g_tw, for --render-dump */
#endif
