#ifndef TW_HOST_H
#define TW_HOST_H
#include <stdbool.h>

/* The window, the sound card and the pacing (src/tw_host.c). */
bool tw_host_open(int scale, bool fullscreen);     /* a real window: OpenGL, menu, audio, controls; scale <= 0 = the saved window size */
bool tw_host_open_headless(void);                  /* an offscreen GL context, for --shots and --render-dump */
bool tw_host_restart_requested(void);              /* File > Restart: re-launch the program after the clean-up */
bool tw_host_active(void);                         /* a window is open (its keyboard drives the cabinet) */
bool tw_host_frame(void);                          /* once per emulated frame, after tw_video_prepare(): events, draw, present, pace; false = quit */
void tw_host_shot(const char *path);               /* the prepared frame, 640x480, to a PPM (any GL context) */
void tw_host_close(void);
#endif
