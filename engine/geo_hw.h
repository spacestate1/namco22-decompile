/*
 * geo_hw.h — the System 22 / Super 22 geometry stage (see engine/geo_hw.c).
 *
 * Screen coordinates come out in 1/16 pixel, exactly as the hardware and
 * the oracle (pc_geo_fixed.py) produce them. Divide by 16.0 for float.
 */
#ifndef GEO_HW_H
#define GEO_HW_H

#include <stdint.h>

typedef struct {
    int32_t  sx16, sy16;   /* screen position, 1/16 pixel */
    int32_t  z;            /* view-space depth */
    uint32_t u, v;         /* 12-bit texture coords */
    int      bri;          /* per-vertex brightness */
    int      valid;        /* 0 = behind the eye, needs z-clip */
    /* Same u/v/bri carried at UF=16 fraction bits, i.e. BEFORE the >>UF
     * truncation. The oracle's golden stream compares these, so keeping
     * them is what lets tools/frame_gate.py check a whole frame rather
     * than one model. */
    int32_t  uf, vf, bf;
} geo_vert;

typedef struct {
    /* The oracle keeps BOTH, and so must this: `v` is the raw per-vertex
     * projection (its scr16, valid=0 when behind the eye) and is what the
     * gate compares; `rv` is the near-plane-clipped POLYGON that actually
     * gets drawn (3-6 verts). Collapsing them into one broke the gate. */
    geo_vert v[4];
    geo_vert rv[10];
    int      nrv;
    /* DRAW polygon, only when the clipped polygon reaches past the screen
     * guard band: rv clipped in 3D against the four guard planes as well, so
     * no coordinate saturates. `rv` stays the oracle-exact near-clipped polygon
     * the gates compare; the live rasteriser swaps dv in (register row 187). */
    geo_vert dv[10];
    int      ndv;
    uint32_t color;      /* PALETTE SELECTOR: (color>>8)&0x7F picks a
                          * 256-entry group. Not RGB -- painting it as
                          * RGB is what made every quad flat olive. */
    int      texbank, cmode;
    int      cz_type;    /* (flags >> 10) & 3 -- selects the czram bank for
                          * depth fog via czattr[6]. Needed by the fog stage;
                          * see FogState.quad_fog in pc_raster_model.py. */
    int32_t  zsort;
    int32_t  cz_adjust;    /* bit 23 set = fog OFF for this quad
                            * (pc_raster_model.py: BIT(cz_adjust,23)) */
    int32_t  flags_raw;    /* packet flags & 0xffffff (golden stream field) */
    int32_t  cz_value;     /* min(0x1fffff, zmax) >> 8  (golden stream field) */
    int      behind;       /* at least one vertex behind the eye */
    int      order;        /* emission index; tiebreak for the stable sort */
    int      pick_code;    /* the object this quad came from, for the picker.
                            * Not part of the hardware model and not compared
                            * by any gate -- it rides along so the sort keeps
                            * quad and owner together. */
    /* Scene clip window in screen pixels (l, r, t, b), from the viewport
     * record. The rasteriser rejects everything outside it; without it,
     * geometry the hardware never shows gets drawn. */
    int32_t  clip[4];
    /* The quad's WHOLE UV box (min_u, max_u, min_v, max_v) from its four ROM
     * UVs, whether or not a vertex is behind the eye. Not part of the hardware
     * model and not compared by any gate (like pick_code). The live
     * rasteriser keys its baked texture on this rather than on the clipped
     * polygon, whose interpolated UVs move every frame and made every
     * near-clipped quad a new texture every frame. */
    uint16_t uvbox[4];
    /* System 22 object flags from the 0x10 (233002) record, bits 21-23 of its
     * third word: non-zero draws the quad UNTEXTURED in one palette pen
     * (namcos22_v.cpp poly3d_drawquad). Super 22 never sets them. */
    int      objectflags;
    int      direct;       /* a quad the master sent straight to the renderer */
} geo_quad;

typedef struct {
    int32_t m[3][3];       /* Q15 rotation: view * object */
    int32_t t[3];          /* translation, view space */
    int32_t zoom_mant;     /* focal length mantissa (1920 = the game's 0x780) */
    int     zoom_shift;    /* right shift paired with the mantissa */
    int32_t vx, vy;        /* viewport centre offset from (320,240) */
    float   cl, cr, cu, cd; /* scene clip window: dspfloat(src[7..10])*zoom-0.5 */
    int     have_clip;
    int32_t objectshift;
    int32_t cz_adjust;
    int32_t absolute_priority;
    int     cullflip;
    /* Lighting (GeoFixed.register_normals_fixed). The normals dot the
     * light against the VIEW matrix, not the combined object*view one in
     * m[][], so the view has to be carried separately. */
    int32_t viewq[3][3];   /* view matrix alone, Q15, reflection applied */
    int32_t light[3];      /* light vector, Q15 */
    int32_t ambient, power;
    int     objectflags;   /* System 22 only, see geo_quad */
} geo_view;

typedef struct {
    int emitted, part_clip;
    int rej_behind, rej_cull, rej_pktlen, rej_chunk;
    int32_t zmin_seen, zmax_seen;
} geo_stats;
extern geo_stats g_geo_stats;
extern unsigned  g_eng_frame;   /* set by the game each frame; diagnostics only */
extern int       g_bbox_cur;    /* object code being walked, -1 = none */

typedef void (*geo_quad_cb)(const geo_quad *q, void *user);

void geo_hw_set_view(const geo_view *v);
void geo_hw_object(int32_t code, geo_quad_cb cb, void *user);
void geo_hw_zoom_from_dspfloat(uint32_t word, int32_t *mant, int *shift);

#endif /* GEO_HW_H */
