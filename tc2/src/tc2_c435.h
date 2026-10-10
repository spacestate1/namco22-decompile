/* tc2_c435.h -- the C435 command processor's render list (src/tc2_c435.c), what the 3D renderer draws */
#ifndef TC2_C435_H
#define TC2_C435_H
#include <stdint.h>

enum { TC2_RE_MODEL = 1, TC2_RE_IMMEDIATE = 2, TC2_RE_DIRECT = 3 };
typedef struct { uint16_t type; int n; uint32_t h, pal, zbias, i[4], u[4], v[4], x[4], y[4], z[4]; } tc2_immediate;
typedef struct {
    int type;
    uint16_t absolute_priority, model_blend_factor, light_power, light_ambient;
    int16_t tx, ty, vp_size_x, vp_size_y, vp_offset_x, vp_offset_y;
    float vp_fov;
    /* a model */
    uint16_t model, model2; float scaling; int transpose, mirror_x;
    int16_t m[9]; int32_t v[3], light_vector[3];
    /* an immediate polygon */
    tc2_immediate imm;
    /* a direct (screen-space) quad: 28 words */
    uint16_t d[28];
} tc2_render_entry;
#define TC2_RENDER_MAX 10000
typedef struct { int count; tc2_render_entry e[TC2_RENDER_MAX]; } tc2_render_list;
extern tc2_render_list tc2_render[2];
extern int tc2_render_cur;                 /* the list the C435 fills; the renderer draws the other one, then swaps */
float tc2_f24(uint32_t v);
#endif
