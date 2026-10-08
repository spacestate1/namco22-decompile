/* s21_video_int.h -- between s21_video.c (the oracle and the GL picture's CPU half) and s21_video_gl.c (the GL picture). */
#ifndef S21_VIDEO_INT_H
#define S21_VIDEO_INT_H
#include "s21_video.h"
const s21_quad *s21v_vis_quads(int *n, const uint16_t **col, const int **z);   /* the visible frame's front-facing quads + pen + avg z */
int s21v_quads_lost(void);                    /* quads past the list's capacity (cumulative; the software path still drew them) */
const uint8_t *s21v_gl2d(void);               /* 496*480 RGBA8: no-polygon pen (R,G), op on a polygon pixel (B,A) -- s21_video.c */
int s21v_gl2d_w(void);                        /* its width: 496, or 496 + 2E in widescreen (the board in the middle; s21_video_set_wide_extra) */
const uint8_t *s21v_palrgb(void);             /* 0x8000 pens as RGB8 */
const uint16_t *s21v_pritab(void);            /* the 16 z-mix thresholds */
int s21v_video_on(void);
int s21v_zmix(void);                          /* palette-ext pri1 = 4: priority-0 sprites z-mixed */
#endif
