/* glmc_msl.h — the Metal Shading Language programs of the macOS port.
 *
 * Programs:
 *   GLMC_MSL_FFP        fixed function: env combiners, alpha test, MVP.
 *                       (This file is also the vertex source of POST1.)
 *   GLMC_MSL_POST1      engine/post_gl.c's gamma LUT fragment shader.
 *   GLMC_MSL_TC2P_*     tc2/src/tc2_gl.c vs_poly/fs_poly (plain + exact-depth).
 *   GLMC_MSL_TC2Q_V     the tc2 full-screen quad vertex shader (a_pos only).
 *   GLMC_MSL_TC2_TEXT   tc2 fs_text.
 *   GLMC_MSL_TC2_GAMMA  tc2 fs_gamma.
 *   GLMC_MSL_BLIT       internal copy/blit/present shader.
 *
 * The translation units the .mm builds are the pieces concatenated in this
 * order (they need no further headers):
 *   FFP:    GLMC_MSL_FFP
 *   POST1:  GLMC_MSL_FFP           (vertex path) + GLMC_MSL_POST1
 *   TC2P:   GLMC_MSL_TC2P_V + GLMC_MSL_TC2P_SH0 +
 *           (PLAIN_HEAD | DEPTH_HEAD) + GLMC_MSL_TC2P_BODY + GLMC_MSL_TC2P_TAIL
 *   TC2TEXT:  GLMC_MSL_TC2Q_V + (header for the tail programs) + GLMC_MSL_TC2_TEXT
 *   TC2GAMMA: GLMC_MSL_TC2Q_V + GLMC_MSL_TC2Q_TAILHEAD + GLMC_MSL_TC2_GAMMA
 *   BLIT:   GLMC_MSL_BLIT
 *
 * Conventions inherited from the games' GL semantics:
 *   - framebuffer pixels stored y-up (GL order, row 0 = the image bottom);
 *     every vertex program negates NDC y so Metal's top-down [[position]]
 *     lands on the GL row and in.pos maps 1:1 to gl_FragCoord.
 *   - GL depth mapping (z/w + 1) / 2 is folded into the FFP vertex by keeping
 *     o.pos.z = (c.z + c.w) / 2 and letting Metal divide by o.pos.w; the tc2
 *     poly program computes real depths and skips it.
 *   - tc2's GL_BGRA palette uploads are stored MTLPixelFormatBGRA8Unorm, whose
 *     read() gives the same .rgb the GLSL saw.
 *   - GL_LUMINANCE / LUMINANCE_ALPHA textures are stored R8 / RG8; the tc2
 *     shaders read exactly the channels the GL fetch produced (see notes).
 *   - A texture unit whose enable bit is off contributes WHITE (1,1,1,1),
 *     measured on the reference implementation the games were tuned against
 *     (pin: untextured MODULATE draws carry the vertex colour unchanged).
 */
#ifndef GLMC_MSL_H
#define GLMC_MSL_H

/* ---------------- fixed function ---------------- */
static const char GLMC_MSL_FFP[] = R"MSL(
// cache-buster 2026-10-26a
#include <metal_stdlib>
using namespace metal;

struct ffp_vin
{
    float4 atr0 [[attribute(0)]];   /* position x,y,z (0) */
    float4 atr1 [[attribute(1)]];   /* colour */
    float4 atr2 [[attribute(2)]];   /* texcoord s,t,r,q */
};
struct ffp_vout
{
    float4 pos  [[position]];
    float4 col;
    float4 tex;
};
struct ffp_u
{
    float4x4 mvp;
    float4 envc0;
    float4 envc1;
    int4 e[7];      /* see glmc_dev.h: 0..5 unit rows, 6 = tex on + alpha test */
    int4 texfmt;    /* GLMC_PX_* of unit 0 (x) and unit 1 (y) */
};

static constant int FFP_MODULATE    = 0;
static constant int FFP_REPLACE     = 1;
static constant int FFP_INTERPOLATE = 2;
static constant int FFP_TEX      = 0;
static constant int FFP_PRIMARY  = 1;
static constant int FFP_CONSTANT = 2;
static constant int FFP_PREVIOUS = 3;
/* operand codes (glmc_main.c pre-encodes the GL enum) */
static constant int FFP_OP_COLOR     = 0;
static constant int FFP_OP_ALPHA     = 1;
static constant int FFP_OP_1MPERC    = 2;
static constant int FFP_OP_1MPERA    = 3;

/* one source: rgb in .rgb; in .a the alpha the GL argument table gives
 * (SRC_COLOR keeps the source's alpha, SRC_ALPHA replicates it) */
static float4 ffparg(int src, int opnd, float4 t, float4 prim, float4 konst, float4 prev)
{
    float4 v = (src == FFP_TEX)      ? t
             : (src == FFP_PRIMARY)  ? prim
             : (src == FFP_CONSTANT) ? konst
                                     : prev;
    if (opnd == FFP_OP_ALPHA) return v.aaaa;
    if (opnd == FFP_OP_1MPERA) return float4(1.0 - v.a);
    if (opnd == FFP_OP_1MPERC) return float4(1.0 - v.rgb, 1.0);
    return float4(v.rgb, v.a);
}

/* one texture unit's combiner; er = E[base..base+2] (op row, rgb rows, tail) */
static float4 ffpunit(constant int4 *er, float4 t, float4 prim, float4 konst, float4 prev)
{
    constant int4 &op = er[0];
    int4 r0 = er[1];
    int4 r1 = er[2];
    float4 a0 = ffparg(r0.x, r0.y, t, prim, konst, prev);
    float4 a1 = ffparg(r0.z, r0.w, t, prim, konst, prev);
    float4 a2 = ffparg(r1.x, r1.y, t, prim, konst, prev);
    float3 rgb;
    if (op.x == FFP_MODULATE) rgb = a0.rgb * a1.rgb;
    else if (op.x == FFP_INTERPOLATE) rgb = a0.rgb * saturate(a2.a) + a1.rgb * (1.0 - saturate(a2.a));
    else rgb = a0.rgb;                                  /* REPLACE */
    rgb *= exp2(float(op.z));
    /* alpha chain: sources r1.z / r1.w, no operands (always alpha-of-source) */
    float4 aa0 = ffparg(r1.z, 0, t, prim, konst, prev);
    float4 aa1 = ffparg(r1.w, 0, t, prim, konst, prev);
    float al;
    if (op.y == FFP_MODULATE) al = aa0.a * aa1.a;
    else al = aa0.a;                                    /* REPLACE */
    return float4(saturate(rgb), al);
}

vertex ffp_vout glmc_ffp_v(ffp_vin a [[stage_in]], constant ffp_u &u [[buffer(0)]])
{
    ffp_vout o;
    float4 c = u.mvp * float4(a.atr0.xyz, 1.0);
    o.pos = float4(c.x, -c.y, (c.z + c.w) * 0.5, c.w);   /* GL NDC y-up -> Metal clip y-down */
    o.col = a.atr1;
    o.tex = a.atr2;
    return o;
}

// cache-buster 2026-10-26a
/* rebuild the GL fetch result from the stored channel set (see glmc_dev.h):
 * R8 luminance -> (l,l,l,1), RG8 luminance_alpha -> (l,l,l,a). RGBA8/BGRA8 are
 * already the logical RGBA (the hardware swizzles BGRA). */
static float4 ffpfmt(float4 c, int fmt)
{
    if (fmt == 2) return float4(c.r, c.r, c.r, 1.0);   /* GLMC_PX_R8 */
    if (fmt == 3) return float4(c.r, c.r, c.r, c.g);   /* GLMC_PX_RG8 */
    return c;
}

fragment float4 glmc_ffp_f(ffp_vout in [[stage_in]],
                           texture2d<float> t0 [[texture(0)]], sampler s0 [[sampler(0)]],
                           texture2d<float> t1 [[texture(1)]], sampler s1 [[sampler(1)]],
                           constant ffp_u &u [[buffer(0)]])
{
    float2 uv = in.tex.xy / max(in.tex.w, 1e-30);
    float4 prev = in.col;          /* unit 0's previous = the primary colour */
    if (u.e[6].x != 0) {
        float4 t0c = ffpfmt(t0.sample(s0, uv), u.texfmt.x);
        prev = ffpunit(&u.e[0], t0c, in.col, u.envc0, prev);
    }
    if (u.e[6].y != 0) {
        float4 t1c = ffpfmt(t1.sample(s1, uv), u.texfmt.y);
        prev = ffpunit(&u.e[3], t1c, in.col, u.envc1, prev);
    }

    /* GL alpha test on the fragment's alpha; code 0 = test off */
    float r = (float)u.e[6].w / 255.0;
    bool keep;
    switch (u.e[6].z) {            /* 0 off 1 always 2< 3= 4<= 5> 6!= 7>= 8 never */
    case 1:  keep = true; break;
    case 2:  keep = prev.a < r; break;
    case 3:  keep = prev.a == r; break;
    case 4:  keep = prev.a <= r; break;
    case 5:  keep = prev.a > r; break;
    case 6:  keep = prev.a != r; break;
    case 7:  keep = prev.a >= r; break;
    case 8:  keep = false; break;
    default: keep = true; break;   /* alpha test off */
    }
    if (!keep) discard_fragment();
    if (isnan(prev.r) || isinf(prev.r)) return float4(0.0);
    if (isnan(prev.g) || isinf(prev.g)) return float4(0.0);
    if (isnan(prev.b) || isinf(prev.b)) return float4(0.0);
    if (isnan(prev.a) || isinf(prev.a)) return float4(0.0);
    return prev;
}
)MSL";

/* ---------------- TC1 gamma LUT (engine/post_gl.c FS ported) -------------- */
static const char GLMC_MSL_POST1[] = R"MSL(
struct postv { float4 pos [[position]]; float4 col; float4 tex; };
fragment float4 glmc_post_f(postv in [[stage_in]],
                            texture2d<float> t_frame [[texture(0)]], sampler s0 [[sampler(0)]],
                            texture2d<float> t_lut   [[texture(1)]], sampler s1 [[sampler(1)]])
{
    float2 uv = in.tex.xy / max(in.tex.w, 1e-30);
    float3 c = t_frame.sample(s0, uv).rgb;
    float3 k = c * (255.0 / 256.0) + 0.5 / 256.0;
    float r = t_lut.sample(s1, float2(k.r, 0.5)).r;
    float g = t_lut.sample(s1, float2(k.g, 0.5)).g;
    float b = t_lut.sample(s1, float2(k.b, 0.5)).b;
    return float4(r, g, b, 1.0);
}
)MSL";

/* ---------------- tc2 poly: GLSL 1.30, ported ----------------
 * The tc2 targets all carry (or read) real depths where needed; the plain
 * variant's fragment returns a float4 and never writes a depth, exactly as
 * the GLSL gl_FragColor paths do when depth-testing is off. The exact-depth
 * pre-pass gets the depth-output variant. */

static const char GLMC_MSL_TC2P_V[] = R"MSL(
#include <metal_stdlib>
using namespace metal;
struct tc2pv
{
    float4 a_pos  [[attribute(0)]];   /* (x*w, y*w, 0, w) */
    float4 a_uvs  [[attribute(1)]];   /* u/z, v/z, shade, 0 (padding) */
    float4 a_par  [[attribute(2)]];   /* pbase, cmode, tbase, flags */
    float4 a_clip [[attribute(3)]];
    float4 a_ext  [[attribute(4)]];   /* entry, band, padding */
};
struct tc2po
{
    float4 pos  [[position]];
    float3 uvs;
    float4 par;
    float4 clip;
    float4 ext;
};
vertex tc2po glmc_tc2p_v(tc2pv a [[stage_in]])
{
    tc2po o;
    o.pos = float4(a.a_pos.x, -a.a_pos.y, a.a_pos.z, a.a_pos.w);   /* GL NDC y-up -> Metal clip y-down */
    o.uvs = a.a_uvs.xyz; o.par = a.a_par; o.clip = a.a_clip; o.ext = float4(a.a_ext.xy, 0.0, 0.0);
    return o;
}
)MSL";

/* the uniform block and helpers, shared by the two fragment variants */
static const char GLMC_MSL_TC2P_SH0[] = R"MSL(
struct tc2pu
{
    int4 s1;    /* ex, scale, h, prio */
    int4 s2;    /* pfade, ff, alpha, alpha_pen */
    int4 s3;    /* near, depth, 0, 0 */
    int4 pcol;
    int4 fcol;
    float keep;
};
static uint b8(float4 t, int c)
{
    if (c == 0) return (uint)(t.x * 255.0 + 0.5);
    if (c == 1) return (uint)(t.y * 255.0 + 0.5);
    if (c == 2) return (uint)(t.z * 255.0 + 0.5);
    return (uint)(t.w * 255.0 + 0.5);
}
)MSL";

static const char GLMC_MSL_TC2P_BODY[] = R"MSL(
    float2 pf = float2(in.pos.x, in.pos.y);
    int2 px = int2(int(in.pos.x) / u.s1.y - u.s1.x, int(in.pos.y) / u.s1.y);
    if (px.x < int(in.clip.x) || px.x >= int(in.clip.z) ||
        px.y < int(in.clip.y) || px.y >= int(in.clip.w)) { discard_fragment(); return OUTA(float4(0.0)); }
    int fl = (int)in.par.w;
    uint tx = (uint)in.uvs.x, ty = (uint)in.uvs.y;
    if ((fl & 1) != 0) {
        uint offs = ((ty << 6u) | (tx >> 4u)) & 0x1FFFFu;
        float4 s = t_sten.read(uint2(offs & 511u, offs >> 9u));
        /* t_sten is GL_LUMINANCE8_ALPHA8 stored RG8: .r = the L byte, .g = the A byte */
        uint w = b8(s, 0) | (b8(s, 3) << 8u);
        if (((w >> ((tx & 15u) ^ 15u)) & 1u) == 0u) { discard_fragment(); return OUTA(float4(0.0)); }
    }
    ty += uint(in.par.z);
    float oz = in.pos.w;                       /* gl_FragCoord.w */
    if (u.s3.x != 0) {                          /* the exact-depth pre-pass */
        if ((fl & 2) != 0 || (u.s2.z != 255 && (fl & 4) != 0)) { discard_fragment(); return OUTA(float4(0.0)); }
        OSD(clamp(0.5 + log2(max(oz, 1e-30)) / 256.0, 0.0, 1.0));
        int e = int(in.ext.x);
        return OUTA(float4(float(e & 255) / 255.0, float((e >> 8) & 255) / 255.0,
                           in.ext.y / 255.0, 1.0));
    }
    if (u.s3.y != 0) {                          /* hidden-pixel resolve pass */
        uint2 fp = uint2(max(floor(pf), 0.0f));
        float4 nn = t_near.read(fp);
        if (nn.a > 0.5) {
            int ne = int(nn.r * 255.0 + 0.5) | (int(nn.g * 255.0 + 0.5) << 8);
            int nb = int(nn.b * 255.0 + 0.5);
            if (ne != int(in.ext.x) && nb == int(in.ext.y) &&
                oz < exp2((t_nearz.read(fp) - 0.5) * 256.0) * u.keep) { discard_fragment(); return OUTA(float4(0.0)); }
        }
    }
    int cmode = int(in.par.y);
    uint pbase = uint(in.par.x);
    uint pmask = 0xFFu, pshift = 0u;
    if ((cmode & 4) != 0) { pbase += 0xECu + uint((cmode & 8) << 1); pmask = 3u; pshift = uint(2 * (~cmode & 3)); }
    else if ((cmode & 2) != 0) { pbase += 0xE0u + uint((cmode & 8) << 1); pmask = 15u; pshift = uint(4 * (~cmode & 1)); }
    uint id = ((tx >> 4u) & 0xFFu) | ((ty << 4u) & 0x1FFF00u);
    float4 m = t_map.read(uint2(id & 2047u, id >> 11u));
    uint mb = b8(m, 2);
    uint tile = b8(m, 0) | (b8(m, 1) << 8u) | ((mb & 1u) << 16u);
    uint a = mb >> 1u;
    if ((a & 1u) != 0u) ty = ~ty;
    if ((a & 2u) != 0u) tx = ~tx;
    if ((a & 4u) != 0u) { uint t2 = tx; tx = ty; ty = t2; }
    uint ri = ((tile << 8u) | ((ty << 4u) & 0xF0u) | (tx & 0x0Fu)) & 0x1FFFFFFu;
    uint pen = b8(t_rom.read(uint2(ri & 8191u, ri >> 13u)), 0);
    if (u.s1.w != 0) {                          /* the priority target pass */
        int pv = (fl >> 3) & 7;
        return OUTA(float4(pv == 7 ? 1.0 : 0.0, pv != 0 ? 1.0 : 0.0, 0.0, pv == 7 ? 1.0 : 0.0));
    }
    uint ci = pbase + ((pen >> pshift) & pmask);
    float4 c = t_pal.read(uint2(ci & 255u, (ci >> 8u) & 127u));
    int3 col = int3(c.rgb * 255.0 + 0.5);
    int sh = clamp((int)in.uvs.z, 0, 63);
    col = (col * sh) >> 6;
    if (u.s2.x != 0) col = (col * int3(u.pcol.xyz)) >> 8;
    if (u.s2.y != 255) col = (col * u.s2.y + int3(u.fcol.xyz) * (256 - u.s2.y)) >> 8;
    bool ua = u.s2.z != 255 && ((fl & 4) != 0 || int(pen) == u.s2.w);
    float al = ua ? float(u.s2.z) / 256.0 : ((fl & 2) != 0 ? 0.5 : 1.0);
    OSD(1.0);
    return OUTA(float4(float3(col) / 255.0, al));
)MSL";

static const char GLMC_MSL_TC2P_PLAIN_HEAD[] = R"MSL(
fragment float4 glmc_tc2p_f(tc2po in [[stage_in]],
                            texture2d<float> t_rom [[texture(0)]],
                            texture2d<float> t_map [[texture(1)]],
                            texture2d<float> t_pal [[texture(2)]],
                            texture2d<float> t_sten [[texture(3)]],
                            texture2d<float> t_near [[texture(4)]],
                            depth2d<float> t_nearz [[texture(5)]],
                            constant tc2pu &u [[buffer(0)]])
{
#define OUTA(x) (float4)(x)
#define OSD(x)  ((void)0)
)MSL";

static const char GLMC_MSL_TC2P_DEPTH_HEAD[] = R"MSL(
struct tc2out { float4 col [[color(0)]]; float depth [[depth(any)]]; };
fragment tc2out glmc_tc2p_f(tc2po in [[stage_in]],
                            texture2d<float> t_rom [[texture(0)]],
                            texture2d<float> t_map [[texture(1)]],
                            texture2d<float> t_pal [[texture(2)]],
                            texture2d<float> t_sten [[texture(3)]],
                            texture2d<float> t_near [[texture(4)]],
                            depth2d<float> t_nearz [[texture(5)]],
                            constant tc2pu &u [[buffer(0)]])
{
#define OUTA(x) (tc2out){ .col = (x) }
#define OSD(x)  ((dout = (x)), (void)0)
    float dout = 1.0f;
)MSL";

static const char GLMC_MSL_TC2P_TAIL[] = "}\n";

/* ---------------- tc2 text / gamma (a_pos only) ---------------- */
static const char GLMC_MSL_TC2Q_V[] = R"MSL(
#include <metal_stdlib>
using namespace metal;
struct tc2qv { float4 a_pos [[attribute(0)]]; };
struct tc2qo { float4 pos [[position]]; };
vertex tc2qo glmc_tc2q_v(tc2qv a [[stage_in]])
{
    tc2qo o;
    o.pos = float4(a.a_pos.x, -a.a_pos.y, a.a_pos.z, a.a_pos.w);   /* GL NDC y-up -> Metal clip y-down */
    return o;
}
)MSL";

/* one fragment uniform block shape lines up with tc2qlu_u below */
static const char GLMC_MSL_TC2Q_TAILHEAD[] = R"MSL(
struct tc2tu
{
    int4 s1;    /* ex, scale, h, pass */
    int4 s2;    /* fade, ff, alpha, amask */
    int4 s3;    /* c12, c13, 0, 0 */
    int4 fcol;
};
)MSL";

static const char GLMC_MSL_TC2_TEXT[] = R"MSL(
fragment float4 glmc_tc2t_f(tc2qo in [[stage_in]],
                            texture2d<float> t_mix [[texture(0)]],
                            texture2d<float> t_pal [[texture(1)]],
                            texture2d<float> t_prio [[texture(2)]],
                            texture2d<float> t_band [[texture(3)]],
                            constant tc2tu &u [[buffer(0)]])
{
    int2 px = int2(int(in.pos.x) / u.s1.y - u.s1.x, int(in.pos.y) / u.s1.y);
    uint mv;
    if (px.x < 0 || px.x >= 640) {
        float4 bq = t_band.read(uint2(uint(max(px.y, 0)), 0));
        /* LA texture stored RG8: .r = the L byte, .g = the A byte */
        mv = (uint)(bq.r * 255.0 + 0.5) | ((uint)(bq.g * 255.0 + 0.5) << 8u);
    } else {
        float4 mq = t_mix.read(uint2(uint(px.x), uint(max(px.y, 0))));
        mv = (uint)(mq.r * 255.0 + 0.5) | ((uint)(mq.g * 255.0 + 0.5) << 8u);
    }
    if ((mv & 0x8000u) == 0u) discard_fragment();
    if (u.s1.w == 0) return float4(1.0, 0.0, 0.0, 0.0);
    if (u.s1.w == 2) {
        float4 p = t_prio.read(uint2(uint(in.pos.x), uint(in.pos.y)));
        if (!(p.r > 0.5 && p.g > 0.5 && p.a < 0.5)) discard_fragment();
    }
    uint pen = mv & 0x7FFFu;
    int3 col = int3(t_pal.read(uint2(pen & 255u, pen >> 8u)).rgb * 255.0 + 0.5);
    if (u.s2.x != 0) col = (col * u.s2.y + int3(u.fcol.xyz) * (256 - u.s2.y)) >> 8;
    bool ua = u.s2.z != 255 && ((int)(pen & 15u) == u.s2.w ||
             ((int)(pen & 255u) >= u.s3.x && (int)(pen & 255u) <= u.s3.y));
    return float4(float3(col) / 255.0, ua ? float(u.s2.z) / 256.0 : 1.0);
}
)MSL";

static const char GLMC_MSL_TC2_GAMMA[] = R"MSL(
fragment float4 glmc_tc2g_f(tc2qo in [[stage_in]],
                            texture2d<float> t_src [[texture(0)]],
                            texture2d<float> t_lut [[texture(1)]])
{
    float4 c = t_src.read(uint2(uint(in.pos.x), uint(in.pos.y)));
    int3 i = int3(c.rgb * 255.0 + 0.5);
    float r = t_lut.read(uint2(uint(i.r), 0)).r;
    float g = t_lut.read(uint2(uint(i.g), 0)).g;
    float b = t_lut.read(uint2(uint(i.b), 0)).b;
    return float4(r, g, b, 1.0);
}
)MSL";

/* ---------------- internal blit / copy / present ---------------- */
static const char GLMC_MSL_BLIT[] = R"MSL(
#include <metal_stdlib>
using namespace metal;
struct blitu
{
    float4 sc;   /* src rect x0, y0, w, h, texels of the source (GL order) */
    float4 sz;   /* src texture size, w, h */
};
struct blitout { float4 pos [[position]]; float2 uv; };
static inline float2 blit_corner(uint vid)
{
    const uint ix = (vid < 6u) ? vid : 0u;
    const float2 p[6] = { float2(0.0, 0.0), float2(1.0, 0.0), float2(1.0, 1.0),
                          float2(0.0, 0.0), float2(1.0, 1.0), float2(0.0, 1.0) };
    return p[ix];
}
vertex blitout glmc_blit_v(uint vid [[vertex_id]], constant blitu &uu [[buffer(0)]])
{
    blitout o;
    float2 c = blit_corner(vid);
    o.pos = float4(c.x * 2.0 - 1.0, c.y * 2.0 - 1.0, 0.0, 1.0);
    /* src rect is in texels, GL row order (row 0 = first); the y sign handles
     * the present flip (a negative sc.h samples the source bottom-up) */
    float2 tr = uu.sc.xy + c * uu.sc.zw;
    o.uv = tr / max(uu.sz.zw, float2(1.0, 1.0));
    return o;
}
fragment float4 glmc_blit_f(blitout ino [[stage_in]],
                            texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]])
{
    return t.sample(s, ino.uv);
}
)MSL";

#endif /* GLMC_MSL_H */
