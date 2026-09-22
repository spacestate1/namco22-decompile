/*
 * ea68k.h - 68K EFFECTIVE ADDRESSES for code ported instruction by
 * instruction from the ROM (the story ending, ROM 0x02A000..0x030000).
 *
 * The ROM passes ADDRESSES around: a ROM table (0x038134), a work-RAM block
 * (0xE17294), or the address of a stack local (-$c(a6)). The transpiled C
 * turned each of those into a HOST pointer and dereferenced it natively,
 * which is wrong three ways at once: a ROM table is big-endian, a work-RAM
 * slot in the _W[] model is 8 bytes wide, and `&R[...]` is a byte array.
 *
 * An ea_t is the ROM's own value, with one extension for stack locals:
 *   - a 68K address as the ROM holds it (fits in 32 bits, possibly sign-
 *     extended), dispatched by region: ROM < 0x400000 (big-endian),
 *     work RAM 0xE00000..0xE3FFFF (through _W[]), else the memory bus;
 *   - a HOST pointer to an int32_t array (a C local standing in for a
 *     68K stack local), recognised by its upper 32 bits being neither
 *     0 nor all-ones -- no 68K address and no sign-extended value has that.
 * NEVER pass `&W[x]` (use EA_W(x)) or `&R[x]` (use the address x).
 *
 * Work-RAM rules follow the rest of the tree: a 32-bit field at a 4-aligned
 * offset is the whole slot W[off]; a 16-bit field is W16(off) (whichever half
 * of its slot it lives in).
 */
#ifndef EA68K_H
#define EA68K_H

#include <stdint.h>
#include "propcycl.h"
#include "vaddr.h"

extern intptr_t _W[];

typedef intptr_t ea_t;

#define EA_W(off)  ((ea_t)(0xE00000u + (uint32_t)(off)))

static inline int ea_is_host(ea_t a)
{
    uint64_t hi = (uint64_t)a >> 32;
    return hi != 0 && hi != 0xFFFFFFFFu;
}

/* The transpiled idioms `&W[x]` and `&R[x]` (host pointers into _W[] and
 * into the ROM byte array) -> the 68K address they stand for. */
static inline ea_t ea_norm(ea_t a)
{
    if (!ea_is_host(a)) return a;
    if ((intptr_t *)a >= _W && (intptr_t *)a < _W + 0x40000)
        return EA_W((uint32_t)((intptr_t *)a - _W));
    if ((const uint8_t *)a >= g_sys.rom && (const uint8_t *)a < g_sys.rom + ROM_SIZE)
        return (ea_t)((const uint8_t *)a - g_sys.rom);
    return a;
}

/* W16 / W16_SET without the including file's `W` macro (same semantics). */
static inline int16_t ea_w16(uint32_t o)
{
    uint32_t s = (uint32_t)_W[o & ~3u];
    return (o & 2) ? (int16_t)(s & 0xFFFFu) : (int16_t)(s >> 16);
}
static inline void ea_w16_set(uint32_t o, int16_t v)
{
    uint32_t b = o & ~3u, s = (uint32_t)_W[b];
    s = (o & 2) ? ((s & 0xFFFF0000u) | (uint16_t)v) : (((uint32_t)(uint16_t)v << 16) | (s & 0xFFFFu));
    _W[b] = (intptr_t)(int32_t)s;
}

static inline int ea_wram(ea_t a, uint32_t *off)
{
    uint32_t v = (uint32_t)a;
    if (ea_is_host(a) || v < 0xE00000u || v >= 0xE40000u) return 0;
    *off = v - 0xE00000u;
    return 1;
}

static inline int32_t ea_rd32(ea_t a)
{
    uint32_t o;
    if (ea_is_host(a)) return *(const int32_t *)a;
    if (ea_wram(a, &o)) {
        if ((o & 3) == 0) return (int32_t)_W[o];
        return (int32_t)(((uint32_t)(uint16_t)ea_w16(o) << 16) | (uint16_t)ea_w16(o + 2));
    }
    return (int32_t)vrd32((uint32_t)a);
}

static inline int16_t ea_rd16(ea_t a)
{
    uint32_t o;
    if (ea_is_host(a)) return (int16_t)*(const int32_t *)a;   /* locals are longs */
    if (ea_wram(a, &o)) return ea_w16(o);
    return vrd16s((uint32_t)a);
}

static inline uint8_t ea_rd8(ea_t a)
{
    uint32_t o;
    if (ea_wram(a, &o))
        return (uint8_t)((uint32_t)_W[o & ~3u] >> ((3 - (o & 3)) * 8));
    return vrd8((uint32_t)a);
}

static inline void ea_wr32(ea_t a, int32_t v)
{
    uint32_t o;
    if (ea_is_host(a)) { *(int32_t *)a = v; return; }
    if (ea_wram(a, &o)) {
        if ((o & 3) == 0) { _W[o] = (intptr_t)v; return; }
        ea_w16_set(o, (int16_t)(v >> 16)); ea_w16_set(o + 2, (int16_t)v); return;
    }
    vwr32((uint32_t)a, (uint32_t)v);
}

static inline void ea_wr16(ea_t a, int16_t v)
{
    uint32_t o;
    if (ea_is_host(a)) { *(int32_t *)a = v; return; }
    if (ea_wram(a, &o)) { ea_w16_set(o, v); return; }
    vwr16((uint32_t)a, (uint16_t)v);
}

/* The DSP display-list cursor W[0x0CA4] is a HOST pointer into g_sys.dspram
 * (native int32 words -- what renderer_3d.c reads). dl_put stores one word
 * and advances, dropping it rather than writing outside dspram (register
 * row 19's guard). */
int32_t *dl_put(int32_t *p, int32_t v);

/* Ported ending helpers whose pointer parameters are 68K effective
 * addresses (see above). Prototyped here so the ported callers pass a full
 * ea_t; the transpiled files' K&R declarations stay compatible. */
void camera_set_from_array(ea_t src, int flag);
void scene_objects_draw_list(ea_t list, ea_t pos);
void ending_camera_shake_init(ea_t table, int n);
void ending_camera_shake_update(void);
uint32_t *render_gate_or_ring(ea_t table, ea_t pos, uint32_t *cursor);
void render_bridge_structure(void);
int  ending_ferris_wheel_draw(int y0, int y1, int t);
int  math_lerp_int(int a, int b, int t0, int t1, int t);
void dsp_cmd_emit_object_mode_8000(void);
void dsp_cmd_emit_object_mode_8002(void);
void ending_sky_draw_alt(void);
void dsp_viewport_setup(int vp, int param);
void ending_object_strip_draw(ea_t list, int32_t frame, int32_t bias);
void ending_object_strip_draw_anim(ea_t list, int32_t frame, int32_t bias);
void ending_player_physics_sim(void);
int  ending_frame_render(void);
void ending_player_reset(void);

#endif /* EA68K_H */
