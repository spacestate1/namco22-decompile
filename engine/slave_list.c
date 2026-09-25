/*
 * slave_list.c -- the SLAVE DSP's job: walk the master DSP's display list in
 * polygon RAM and hand every primitive to the geometry stage (geo_hw.c).
 *
 * One walker for both boards (namcos22_v.cpp simulate_slavedsp). Record
 * layout, from tools/pc_geo_fixed.py (byte-exact vs MAME):
 *   len 0x15  bb0003  viewport: zoom = dspfloat(src[6]),
 *                     view[row][col] = q15(src[0x0c + col*3 + row])
 *   len 0x10  233002  object shift / cz adjust (System 22: + object flags)
 *   len 0x0a  300000  view transform: view[row][col] = q15(src[1+col*3+row])
 *   len 0x0d  200002  primitive (code 0x5 or >= 0x45):
 *                     m[row][col] = q15(src[1+col*3+row])
 *                     t = sext24(src[0xa..0xc])
 *                     combined = (m . view) >> 15, t' = (t . view) >> 15
 * Moved here from src/framedump.c unchanged, apart from the board settings.
 */
#include <stdint.h>
#include <string.h>
#include "eng.h"
#include "geo_hw.h"
#include "slave_list.h"

static int32_t sext_n(uint32_t v, int bits)
{
    uint32_t m = 1u << (bits - 1);
    v &= (1u << bits) - 1u;
    return (int32_t)((v ^ m) - m);
}

static int32_t q15v(uint32_t v) { return sext_n(v & 0xffff, 16); }

/* namcos22_v.cpp:699 dspfloat, as a float (the clip window needs the real
 * value, not the mantissa/shift pair the projection uses). */
static float dspfloatf(uint32_t v)
{
    float mant = (float)sext_n(v & 0xffff, 16);
    int exp = (int)((v >> 16) & 0x3f);
    while (exp < 0x2e) { mant /= 2.0f; exp++; }
    return mant;
}

/* GeoFixed._reflectq: mirror the view by negating a COLUMN of the matrix. */
static void reflectq(int32_t m[3][3], int reflection)
{
    if (reflection & 0x10) for (int r = 0; r < 3; r++) m[r][0] = -m[r][0];
    if (reflection & 0x20) for (int r = 0; r < 3; r++) m[r][1] = -m[r][1];
}

int eng_walk_list(eng_word_fn pw, const eng_list_cfg *cfg, geo_quad_cb cb, void *user)
{
    /* ---- camera state, persistent across records ---- */
    int32_t viewq[3][3] = {{0x7FFF,0,0},{0,0x7FFF,0},{0,0,0x7FFF}};
    int have_view = 0;                 /* oracle: `if not hasattr(self,"viewq")` */
    int32_t zoom_mant = 0x7FFF; int zoom_shift = 15;
    int32_t vx = 0, vy = 0;
    int32_t absolute_priority = 0, objectshift = 0, cz_adjust = 0;
    int reflection = 0, cullflip = 0;
    int32_t amb_fx = 0, pow_fx = 0, light_fx[3] = {0, 0, 0};
    float cl = 0, cr = 0, cu = 0, cd = 0; int have_clip = 0;

    /* Fixed list head (simulate_slavedsp): Super 22 starts at 0x304, System 22
     * at 0x2FF. Deriving it from a pointer word and skipping an FFFE prologue
     * (what this once did) is not what the hardware does. */
    int src = cfg->head;
    int objectflags = 0;

    int guard = 0, prims = 0;
    while (guard++ < 4096) {
        uint32_t code = pw(src) & 0xFFFF;
        uint32_t len  = pw(src + 1) & 0xFFFF;
        const int p = src + 2;                        /* payload base */

        if (len == 0x15) {                            /* bb0003: viewport */
            absolute_priority = (int32_t)((pw(p + 3) >> 16) & 0xffff);
            vx = sext_n((pw(p + 5) >> 16) & 0xffff, 12);
            vy = sext_n( pw(p + 5)        & 0xffff, 12);
            uint32_t z = pw(p + 6);
            zoom_mant = sext_n(z & 0xffff, 16);
            { int e = (int)((z >> 16) & 0x3f); int sh = 0x2e - e;
              zoom_shift = sh < 0 ? 0 : sh; }
            amb_fx = (int32_t)((pw(p + 1) >> 16) & 0xffff);
            pow_fx = (int32_t)( pw(p + 1)        & 0xffff);
            for (int k = 0; k < 3; k++) light_fx[k] = q15v(pw(p + 2 + k));
            {   /* scene clip window, and the reflection swaps that go with it */
                float zf = dspfloatf(z);
                cl = dspfloatf(pw(p + 8)) * zf - 0.5f;   /* vl = src[8] */
                cr = dspfloatf(pw(p + 7)) * zf - 0.5f;   /* vr = src[7] */
                cu = dspfloatf(pw(p + 9)) * zf - 0.5f;
                cd = dspfloatf(pw(p + 10)) * zf - 0.5f;
            }
            reflection = (int)((pw(p + 2) >> 16) & 0x30);
            cullflip   = (reflection == 0x10 || reflection == 0x20);
            if (reflection & 0x10) { float t2 = cl; cl = cr; cr = t2; }
            if (reflection & 0x20) { float t2 = cu; cu = cd; cd = t2; }
            have_clip = 1;
            for (int col = 0; col < 3; col++)
                for (int row = 0; row < 3; row++)
                    viewq[row][col] = q15v(pw(p + 0x0c + col * 3 + row));
            reflectq(viewq, reflection);
            have_view = 1;
            cz_adjust = objectshift = 0;   /* viewport resets the shift group */
            objectflags = 0;
        } else if (len == 0x10) {                     /* 233002: object shift */
            cz_adjust   = (int32_t)(pw(p + 1) & 0xffffff);
            objectshift = (int32_t)(pw(p + 2) & 0xffffff);
            if (cfg->s22_objectflags) objectflags = (int)((pw(p + 3) >> 21) & 7);
        } else if (len == 0x0a) {                     /* 300000: view xform */
            for (int col = 0; col < 3; col++)
                for (int row = 0; row < 3; row++)
                    viewq[row][col] = q15v(pw(p + 1 + col * 3 + row));
            reflectq(viewq, reflection);
            have_view = 1;
        } else if (len == 0x0d) {                     /* 200002: primitive */
            if ((code == 0x5 || code >= 0x45) && have_view) {
                int32_t m[3][3], t[3];
                for (int col = 0; col < 3; col++)
                    for (int row = 0; row < 3; row++)
                        m[row][col] = q15v(pw(p + 1 + col * 3 + row));
                for (int k = 0; k < 3; k++)
                    t[k] = sext_n(pw(p + 0x0a + k), 24);

                geo_view gv;
                memset(&gv, 0, sizeof gv);
                for (int r = 0; r < 3; r++)
                    for (int c = 0; c < 3; c++)
                        gv.m[r][c] = (int32_t)(((int64_t)m[r][0] * viewq[0][c] +
                                                (int64_t)m[r][1] * viewq[1][c] +
                                                (int64_t)m[r][2] * viewq[2][c]) >> 15);
                for (int c = 0; c < 3; c++)
                    gv.t[c] = (int32_t)(((int64_t)t[0] * viewq[0][c] +
                                         (int64_t)t[1] * viewq[1][c] +
                                         (int64_t)t[2] * viewq[2][c]) >> 15);
                gv.zoom_mant = zoom_mant; gv.zoom_shift = zoom_shift;
                gv.vx = vx; gv.vy = vy;
                gv.objectshift = objectshift;
                gv.cz_adjust = cz_adjust;
                gv.cl = cl; gv.cr = cr; gv.cu = cu; gv.cd = cd;
                gv.have_clip = have_clip;
                gv.absolute_priority = absolute_priority;
                gv.cullflip = cullflip;
                memcpy(gv.viewq, viewq, sizeof viewq);
                gv.light[0] = light_fx[0]; gv.light[1] = light_fx[1];
                gv.light[2] = light_fx[2];
                gv.ambient = amb_fx; gv.power = pow_fx;
                gv.objectflags = objectflags;
                geo_hw_set_view(&gv);
                g_bbox_cur = (int)code;
                geo_hw_object((int32_t)code, cb, user);
                g_bbox_cur = -1;
                objectflags &= ~2;             /* blit_polyobject: per object */
                prims++;
            }
        } else {
            break;                                    /* unknown length */
        }

        /* The next record must follow IMMEDIATELY; anything else ends the
         * list (oracle: `if nxt != index + length + 2: break`). */
        int nxt = (int)(pw(src + len + 3) & 0x7FFF);
        if (nxt != src + (int)len + 4) break;
        src = nxt;
    }
    if (cfg->out_zoom_mant) { *cfg->out_zoom_mant = zoom_mant; *cfg->out_zoom_shift = zoom_shift;
                              *cfg->out_vx = vx; *cfg->out_vy = vy; }
    return prims;
}
