/*
 * fog_hw.h — SS22 CZ depth fog + poly/screen fade (see src/fog_hw.c).
 *
 * The stages our renderer used to stop short of. MAME's pixel chain is
 *
 *     texel -> pen -> palette -> shade(bri) -> CZ fog -> poly_fade
 *                                                     -> screen_fade -> gamma
 *
 * and we implemented only the first three. Ported from the validated
 * reference model (pc_raster_model.py FogState), which mirrors
 * namcos22_v.cpp recalc_czram / drawquad.
 */
#ifndef FOG_HW_H
#define FOG_HW_H

#include <stdint.h>

typedef struct {
    uint16_t czattr[8];          /* 0x810000, 8 x u16 big-endian */
    uint8_t  recalc[4][0x2000];  /* per-bank czram -> fog factor tables */

    uint8_t  poly_fade[3];       /* mixer 0x00..0x02 */
    int      poly_fade_enabled;  /* mixer 0x00..0x02 != ff ff ff */
    uint8_t  fog_rgb[3];         /* mixer 0x05..0x07 */
    uint8_t  bg[3];              /* mixer 0x08..0x0a */
    uint8_t  screen_fade[3];     /* mixer 0x16..0x18 */
    uint8_t  screen_fade_factor; /* mixer 0x19 */
    uint8_t  mixer_flags;        /* mixer 0x1a, bit0 gates screen fade */
    uint8_t  poly_alpha_color;   /* mixer 0x0f - sprite alpha gate */
    uint8_t  poly_alpha_pen;     /* mixer 0x10 - sprite alpha pen */
    /* text layer (render_text.py): alpha window + palette base */
    uint8_t  text_alpha_lo;      /* mixer 0x12 */
    uint8_t  text_alpha_hi;      /* mixer 0x13 */
    uint8_t  text_alpha_mask;    /* mixer 0x14, low nibble */
    uint8_t  text_alpha;         /* mixer 0x15 */
    uint8_t  text_palbase;       /* mixer 0x1b */
    /* Final-stage gamma: 3 x 256-byte LUTs at mixer 0x100/0x200/0x300,
     * applied to the WHOLE frame (background included) after every other
     * stage. Omitting it left a systematic per-pixel colour error --
     * 99.9% of pixels differing from the reference at a mean of ~60/255. */
    uint8_t  gamma[3][256];
    int      have_gamma;
} fog_state;

/* Apply the final gamma to one RGB triple in place. No-op without data. */
void fog_apply_gamma(uint8_t *r, uint8_t *g, uint8_t *b);

extern fog_state g_fog;
extern int       g_fog_valid;   /* 0 = no fog data; renderer must skip the stage */

/* Load czattr/czram0-3/mix for one frame from a capture directory.
 * Returns 1 on success. Sets g_fog_valid. */
int  fog_load_frame(const char *dir, int frame);
/* Same, into a caller-owned state (used to preload a whole sequence so
 * playback does no file I/O per frame). Does not touch g_fog/g_fog_valid. */
int  fog_load_frame_into(fog_state *fs, const char *dir, int frame);
void fog_invalidate(void);
/* Build g_fog from LIVE emulated memory (g_sys.videomix + g_sys.czram)
 * instead of a capture. Returns 1 on success. */
int  fog_load_live(void);

/* Per-quad fog decision (FogState.quad_fog). Pass the RAW colour word;
 * the gate is on bit 15 (the flag above the 7-bit palette group).
 * Returns 1 when this quad is fogged, and fills *table and *sdelta. */
int  fog_quad(uint32_t color, int cz_type, const uint8_t **table, int *sdelta);

/* Fog factor for a view-space depth, matching the per-pixel chain:
 *   cz = min(z >> 8, 0x1fff);  ff = table[cz] + sdelta
 * Returns the blend alpha in 0..255 (0 = fully fogged, 255 = unfogged),
 * i.e. the `alpha` for blend(rgb, fog_rgb, alpha). */
int  fog_alpha_for_z(const uint8_t *table, int sdelta, int32_t z);

#endif /* FOG_HW_H */
