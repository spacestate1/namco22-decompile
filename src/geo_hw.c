/*
 * geo_hw.c — the SS22 geometry stage, ported from a validated oracle.
 *
 * WHY THIS EXISTS
 * ---------------
 * renderer_3d.c projects with an invented OpenGL camera (a hardcoded 90
 * degree FOV). The hardware has no field-of-view angle at all: the DSP
 * transforms vertices with a Q15 matrix and projects with an integer
 * divide against a focal length ("zoom"). Comparing the decompile's
 * attract screen against MAME's shows the two share nothing — MAME draws
 * a sky dome, clouds and the flyer; the decompile draws one enormous
 * mis-scaled quad. No amount of frustum tuning fixes that, because the
 * whole transform stage is missing rather than mistuned.
 *
 * THE ORACLE
 * ----------
 * This is a direct port of tools/pc_geo_fixed.py from the Prop Cycle
 * MiSTer project, which is gated byte-exact against MAME's own output on
 * four captured frames (sim: `make geo-run`, frames 1500/2400/3600/4800).
 * Every arithmetic step below mirrors it deliberately, including the
 * truncating divide and the 1/16-pixel screen coordinates. Where this
 * file and that Python disagree, the Python is right.
 *
 * Reference for the projection, namcos22_v.cpp (~line 296):
 *     cx = 320 + vx;  cy = 240 + vy
 *     screen_x = cx + (X * zoom) / Z
 *     screen_y = cy - (Y * zoom) / Z
 *
 * NOT PORTED YET (deliberate, and each is a visible gap):
 *   - lighting / per-vertex bri beyond the flat and gouraud cases
 *   - point-RAM objects (code 0x5)
 *   - the 0x8008 / 0x800A matrix sections, which the master-DSP HLE
 *     (tools/pc_master_model.py) also lists as unmodelled
 */

#include <stdint.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <string.h>
#include "propcycl.h"
#include "geo_hw.h"

/* ---- point ROM ---------------------------------------------------------
 * g_pointrom is already sign-extended 24-bit at load time (rom_loader.c
 * signed24()), which matches the oracle's pt.read(). */
static inline int32_t pt_read(uint32_t addr)
{
    if (addr < g_pointrom_count) return g_pointrom[addr];
    return 0;
}

/* Truncating signed division. C already truncates toward zero, which is
 * what the oracle's sdiv() reimplements because Python's // floors. */
static inline int32_t sdiv(int64_t a, int64_t b)
{
    if (b == 0) return 0;
    return (int32_t)(a / b);
}

/* Floor division / arithmetic shift, matching Python's // and >>, which
 * floor for negatives where C truncates. */
static inline int64_t fdiv64(int64_t a, int64_t b)
{
    int64_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) q--;
    return q;
}
static inline int64_t fshr64(int64_t v, int n) { return v >> n; }

/* The oracle is Python, whose ints are unbounded; ours are not. On
 * near-degenerate clip edges the lerp parameter t gets very large and
 * (b-a)*t overflows int64 -- silently, producing a mid-range screen
 * coordinate where the oracle produces a huge one that then saturates.
 * tools/frame_gate.py caught this as sx16/sy16 deltas on exactly the
 * partially-clipped quads, with the oracle at INT32_MAX/MIN and us at
 * arbitrary values. Doing the wide steps in __int128 restores parity.
 *
 * Shifts must FLOOR (Python >>) and the divide must TRUNCATE (the oracle
 * defines sdiv with C semantics); __int128 gives both natively. */
static inline int64_t mulshr128(int64_t a, int64_t b, int n)
{
    return (int64_t)(((__int128)a * (__int128)b) >> n);
}

/* base16 +/- ((v * mant) << 4 >> shift) / z, saturated to int32. */
static inline int32_t proj_sat(int64_t base16, int64_t v, int32_t mant,
                               int shift, int64_t z, int negate)
{
    /* Shift through unsigned: v*mant is routinely negative and shifting a
     * negative value left is undefined behaviour (UBSan flags it). The
     * two's-complement bit pattern is identical, so the result is
     * unchanged -- this is a correctness-of-language fix, not a maths one. */
    __int128 num = (__int128)((unsigned __int128)((__int128)v * (__int128)mant) << 4);
    num >>= shift;                       /* arithmetic: floors, like Python */
    __int128 q = z ? num / (__int128)z : 0;   /* truncating, like sdiv() */
    __int128 r = (__int128)base16 + (negate ? -q : q);
    if (r >  (__int128)INT32_MAX) return INT32_MAX;
    if (r <  (__int128)INT32_MIN) return INT32_MIN;
    return (int32_t)r;
}

static inline int32_t sat32(int64_t v)
{
    if (v >  (int64_t)INT32_MAX) return INT32_MAX;
    if (v <  (int64_t)INT32_MIN) return INT32_MIN;
    return (int32_t)v;
}

static inline int32_t sext(uint32_t v, int bits)
{
    /* MASK FIRST. Without the mask this sign-extends the low `bits` but
     * lets everything above them through, so sext(polyshift,18) leaked the
     * ap field (0x1C0000) straight into zsort -- caught by tools/geo_gate.py
     * as a zsort delta of exactly 0x1C0000 while every vertex matched. */
    uint32_t m = 1u << (bits - 1);
    v &= (1u << bits) - 1u;
    return (int32_t)((v ^ m) - m);
}

/* ---- view state -------------------------------------------------------- */
static geo_view g_view;

/* ---- per-object lighting (GeoFixed.register_normals_fixed) --------------
 * Normals packets inside an object publish per-vertex lit values that take
 * precedence over both the gouraud and flat brightness fields. Leaving them
 * unmodelled showed up in tools/frame_gate.py as b_f mismatches on exactly
 * the objects that carry them (76 vertices at frame 1500). */
#define LIT_MAX 512
static int32_t g_lit[LIT_MAX];
static int     g_lit_n, g_lit_idx;

static void register_normals(uint32_t addr)
{
    const int32_t (*v)[3] = g_view.viewq;
    const int32_t (*m)[3] = g_view.m;
    int32_t cl[3];
    for (int c = 0; c < 3; c++)
        cl[c] = (int32_t)(((int64_t)g_view.light[0] * v[0][c] +
                           (int64_t)g_view.light[1] * v[1][c] +
                           (int64_t)g_view.light[2] * v[2][c]) >> 15);
    for (int i = 0; i < 4; i++) {
        int32_t n[3], nt[3];
        for (int k = 0; k < 3; k++)
            n[k] = sext((uint32_t)pt_read(addr + i * 3 + k) & 0xffff, 16);
        for (int c = 0; c < 3; c++)
            nt[c] = (int32_t)(((int64_t)n[0] * m[0][c] + (int64_t)n[1] * m[1][c] +
                               (int64_t)n[2] * m[2][c]) >> 15);
        int64_t dot = (int64_t)nt[0] * cl[0] + (int64_t)nt[1] * cl[1] +
                      (int64_t)nt[2] * cl[2];
        if (dot < 0) dot = 0;
        if (g_lit_n < LIT_MAX)
            g_lit[g_lit_n++] = g_view.ambient +
                               (int32_t)(((int64_t)g_view.power * dot) >> 30);
    }
}

/* Reject census. A frame that draws nothing has many possible causes and
 * the screen cannot tell them apart; these can. */
geo_stats g_geo_stats;

int g_zord_ap = 0, g_zord_os = 0;   /* last view's priority fields, for PROPCYCL_ZORD */

void geo_hw_set_view(const geo_view *v) {
    g_zord_ap = v->absolute_priority; g_zord_os = v->objectshift; g_view = *v; }

void geo_hw_zoom_from_dspfloat(uint32_t word, int32_t *mant, int *shift)
{
    *mant  = sext(word & 0xffff, 16);
    int exp = (word >> 16) & 0x3f;
    int s = 0x2e - exp;
    *shift = s < 0 ? 0 : s;
}

/* ---- one quad ----------------------------------------------------------
 * Direct port of GeoFixed.quad_fixed(). `addr` is the PACKET DATA base,
 * not the raw object address — getting that base wrong is exactly the
 * defect TEXTURE_PIPELINE_BUGS.md records as Bug 2, and it is why the
 * existing renderer draws the wrong geometry.
 */
static void quad_fixed(int32_t color, uint32_t addr, int32_t polyshift,
                       int32_t flags, int32_t packetformat,
                       geo_quad_cb cb, void *user)
{
    const int32_t (*m)[3] = g_view.m;
    const int32_t *t = g_view.t;

    int32_t vx[4], vy[4], vz[4];
    for (int i = 0; i < 4; i++) {
        int32_t x = pt_read(0x8 + i * 3 + addr);
        int32_t y = pt_read(0x9 + i * 3 + addr);
        int32_t z = pt_read(0xa + i * 3 + addr);
        vx[i] = (int32_t)(((int64_t)x * m[0][0] + (int64_t)y * m[1][0] +
                           (int64_t)z * m[2][0]) >> 15) + t[0];
        vy[i] = (int32_t)(((int64_t)x * m[0][1] + (int64_t)y * m[1][1] +
                           (int64_t)z * m[2][1]) >> 15) + t[1];
        vz[i] = (int32_t)(((int64_t)x * m[0][2] + (int64_t)y * m[1][2] +
                           (int64_t)z * m[2][2]) >> 15) + t[2];
    }

    int32_t zmax = vz[0], zmin = vz[0];
    for (int i = 1; i < 4; i++) {
        if (vz[i] > zmax) zmax = vz[i];
        if (vz[i] < zmin) zmin = vz[i];
    }
    if (zmax < 0) { g_geo_stats.rej_behind++; return; }   /* wholly behind */

    /* backface cull, two cross products, verbatim from the oracle */
    if (flags & 0x20) {
        int64_t c1 = (int64_t)vx[2] * ((int64_t)vz[0] * vy[1] - (int64_t)vy[0] * vz[1])
                   + (int64_t)vy[2] * ((int64_t)vx[0] * vz[1] - (int64_t)vz[0] * vx[1])
                   + (int64_t)vz[2] * ((int64_t)vy[0] * vx[1] - (int64_t)vx[0] * vy[1]);
        int64_t c2 = (int64_t)vx[0] * ((int64_t)vz[2] * vy[3] - (int64_t)vy[2] * vz[3])
                   + (int64_t)vy[0] * ((int64_t)vx[2] * vz[3] - (int64_t)vz[2] * vx[3])
                   + (int64_t)vz[0] * ((int64_t)vy[2] * vx[3] - (int64_t)vx[2] * vy[3]);
        if (( g_view.cullflip && c1 <= 0 && c2 <= 0) ||
            (!g_view.cullflip && c1 >= 0 && c2 >= 0))
        { g_geo_stats.rej_cull++;
          /* PROPCYCL_CULLDUMP=<code>: name every back-face-culled quad of one
           * object, with its projected corners -- a hole INSIDE one model is
           * either a quad culled here or one lost to the near clip. Diagnostic
           * only; the oracle path is unchanged. */
          { static int cd = -2; extern int g_bbox_cur;
            if (cd == -2) { const char *e = getenv("PROPCYCL_CULLDUMP"); cd = e ? atoi(e) : -1; }
            if (cd >= 0 && g_bbox_cur == cd) {
                fprintf(stderr, "[CULL] f%u code %d c1=%lld c2=%lld", g_sys.frame_count, cd,
                        (long long)c1, (long long)c2);
                for (int i = 0; i < 4; i++)
                    if (vz[i] > 0)
                        fprintf(stderr, " (%.3f,%.3f,%d)",
                                proj_sat((int64_t)(320 + g_view.vx) * 16, vx[i], g_view.zoom_mant,
                                         g_view.zoom_shift, vz[i], 0) / 16.0,
                                proj_sat((int64_t)(240 + g_view.vy) * 16, vy[i], g_view.zoom_mant,
                                         g_view.zoom_shift, vz[i], 1) / 16.0, vz[i]);
                fprintf(stderr, "\n");
            } }
          return; }
    }

    if (zmin < 0) zmin = 0;
    int32_t zalg = flags & 0x300;
    int32_t zsort = (zalg == 0x000) ? zmin
                  : (zalg == 0x100) ? zmax
                                    : ((zmin + zmax) >> 1);
    if (zsort > 0x1fffff) zsort = 0x1fffff;
    int ap = g_view.absolute_priority & 7;
    if (polyshift & 0x200000) zsort = polyshift & 0x1fffff;
    else { zsort += sext(polyshift, 18); ap += (polyshift & 0x1c0000) >> 18; }
    if (g_view.objectshift & 0x200000) zsort = g_view.objectshift & 0x1fffff;
    else { zsort += sext(g_view.objectshift, 18);
           ap += (g_view.objectshift & 0x1c0000) >> 18; }
    if (zsort < 0) zsort = 0;
    if (zsort > 0x1fffff) zsort = 0x1fffff;
    zsort |= (ap & 7) << 21;

    /* projection: 1/16-pixel screen coordinates */
    const int32_t mant = g_view.zoom_mant;
    const int shift = g_view.zoom_shift;
    const int32_t cx = 320 + g_view.vx;
    const int32_t cy = 240 + g_view.vy;

    geo_quad q;
    memset(&q, 0, sizeof q);
    q.color = (uint32_t)color;
    q.zsort = zsort;
    q.behind = 0;

    uint32_t uv_raw[8];
    for (int k = 0; k < 8; k++) uv_raw[k] = (uint32_t)pt_read(k + addr) & 0xffffff;

    /* Per-vertex brightness, resolved ONCE per quad: the lit path advances
     * a shared index, so evaluating it twice (raw pass + clip pass) would
     * consume the table at double rate. */
    int briv[4];
    for (int i = 0; i < 4; i++) {
        if (g_lit_n > 0) {
            int idx = g_lit_idx++;
            if (g_lit_n > 4) idx >>= 2;
            briv[i] = g_lit[((idx % g_lit_n) + g_lit_n) % g_lit_n];
        } else if (packetformat & 0x40) {
            briv[i] = (int)((uv_raw[i] >> 16) & 0xff);   /* gouraud */
        } else {
            briv[i] = (int)((color >> 16) & 0xff);       /* flat */
        }
    }
    q.cmode   = (uv_raw[0] >> 12) & 0xf;   /* oracle: same fields */
    q.texbank = (uv_raw[1] >> 12) & 0xf;
    q.cz_type = (flags >> 10) & 3;         /* pc_geo_fixed.py line 290 */
    q.flags_raw = flags & 0xffffff;
    q.cz_adjust = g_view.cz_adjust;
    if (g_view.have_clip) {
        /* pc_raster_model.py: clip = (cx+vl, cx-vr-1, cy+vu, cy-vd-1),
         * then int() truncation. C casts truncate the same way. */
        q.clip[0] = (int32_t)((float)cx + g_view.cl);
        q.clip[1] = (int32_t)((float)cx - g_view.cr - 1.0f);
        q.clip[2] = (int32_t)((float)cy + g_view.cu);
        q.clip[3] = (int32_t)((float)cy - g_view.cd - 1.0f);
    } else {
        q.clip[0] = 0; q.clip[1] = 639; q.clip[2] = 0; q.clip[3] = 479;
    }
    {   /* cz_value = min(0x1fffff, zmax) >> 8, computed from the PRE-clamp
         * zmax (the oracle uses the raw zmax, not the zsort-clamped one). */
        int32_t zc = zmax; if (zc > 0x1fffff) zc = 0x1fffff;
        q.cz_value = zc >> 8;
    }

    /* raw per-vertex projection -- parity with the oracle's scr16 */
    for (int i = 0; i < 4; i++) {
        if (vz[i] <= 0) { q.v[i].valid = 0; q.behind = 1; continue; }
        q.v[i].sx16 = proj_sat((int64_t)cx * 16, vx[i], mant, shift, vz[i], 0);
        q.v[i].sy16 = proj_sat((int64_t)cy * 16, vy[i], mant, shift, vz[i], 1);
        q.v[i].z    = vz[i];
        q.v[i].u    = uv_raw[2 * i]     & 0xfff;
        q.v[i].v    = uv_raw[2 * i + 1] & 0xfff;
        q.v[i].bri  = briv[i];
        q.v[i].valid = 1;
    }

    double pre_x[6], pre_y[6];   /* the near-clipped polygon, before projection */
    int n_pre = 0;
    /* ---- near-plane clip, then project ---------------------------------
     * Port of the oracle's clip lerp (UF=16 attribute fraction bits,
     * TF=24 parameter bits). Dropping partially-clipped quads whole is what
     * tore black bands through the sky; interpolating them is what the
     * hardware does.
     *
     * Python's // and >> floor; C truncates toward zero. fdiv/fshr below
     * keep the arithmetic identical for negative operands. */
    {
        const int UF = 16, TF = 24;
        int64_t px_[4], py_[4], pz_[4], pu_[4], pv_[4], pb_[4];
        for (int i = 0; i < 4; i++) {
            px_[i] = vx[i]; py_[i] = vy[i]; pz_[i] = vz[i];
            pu_[i] = (int64_t)(uv_raw[2 * i]     & 0xfff) << UF;
            pv_[i] = (int64_t)(uv_raw[2 * i + 1] & 0xfff) << UF;
            pb_[i] = (int64_t)briv[i] << UF;
        }
        int64_t cx_[6], cy_[6], cz_[6], cu_[6], cv_[6], cb_[6];
        int n = 0;
        for (int i = 0; i < 4 && n < 6; i++) {
            int j = (i + 1) & 3;
            int ain = pz_[i] >= 1, bin = pz_[j] >= 1;
            if (ain) {
                cx_[n]=px_[i]; cy_[n]=py_[i]; cz_[n]=pz_[i];
                cu_[n]=pu_[i]; cv_[n]=pv_[i]; cb_[n]=pb_[i]; n++;
            }
            if (ain != bin && n < 6) {
                int64_t den = pz_[j] - pz_[i];
                /* named lerp_t, not t: `t` up-scope is the VIEW TRANSLATION
                 * vector, and shadowing it here invites a later edit to
                 * reach for the wrong one. */
                /* (1 - pz) is negative whenever the vertex is in front of
                 * the near plane, so shift through unsigned (see proj_sat). */
                int64_t lerp_t = den ? fdiv64((int64_t)((uint64_t)(1 - pz_[i]) << TF),
                                              den) : 0;
                cx_[n]=px_[i]+mulshr128(px_[j]-px_[i],lerp_t,TF);
                cy_[n]=py_[i]+mulshr128(py_[j]-py_[i],lerp_t,TF);
                cu_[n]=pu_[i]+mulshr128(pu_[j]-pu_[i],lerp_t,TF);
                cv_[n]=pv_[i]+mulshr128(pv_[j]-pv_[i],lerp_t,TF);
                cb_[n]=pb_[i]+mulshr128(pb_[j]-pb_[i],lerp_t,TF);
                cz_[n]=1;                 /* on the plane by definition */
                n++;
            }
        }
        q.nrv = (n >= 3) ? n : 0;
        n_pre = n;
        for (int i = 0; i < n && i < 6; i++) { pre_x[i] = (double)cx_[i]; pre_y[i] = (double)cy_[i]; }
        for (int i = 0; i < n && i < 6; i++) {
            q.rv[i].sx16 = proj_sat((int64_t)cx * 16, cx_[i], mant, shift, cz_[i], 0);
            q.rv[i].sy16 = proj_sat((int64_t)cy * 16, cy_[i], mant, shift, cz_[i], 1);
            q.rv[i].z    = (int32_t)cz_[i];
            q.rv[i].u    = (uint32_t)(cu_[i] >> UF) & 0xfff;
            q.rv[i].v    = (uint32_t)(cv_[i] >> UF) & 0xfff;
            q.rv[i].bri  = (int)(cb_[i] >> UF);
            q.rv[i].uf   = (int32_t)cu_[i];
            q.rv[i].vf   = (int32_t)cv_[i];
            q.rv[i].bf   = (int32_t)cb_[i];
            q.rv[i].valid = 1;
        }
    }
    /* ---- guard-band clip (register row 187) ------------------------------
     * A vertex put ON the near plane (z = 1) projects to billions of 1/16 px,
     * and proj_sat clamps x and y to +-2^31 INDEPENDENTLY -- which bends the
     * polygon's edges. On SOLITAR the cloud-floor sheet (code 112) has quads
     * with one vertex in front of the eye and three behind; banking turned
     * their clamped corners into a flat wedge across the sky. MAME rasterises
     * in float and clips to the screen, so the shape survives. Here the
     * near-clipped polygon is clipped in 3D against four planes a guard band
     * outside the screen (exact: every attribute is linear in 3D), and that
     * polygon is what gets DRAWN. rv is untouched, so the gates still compare
     * the oracle's numbers. */
    q.ndv = 0;
    if (q.nrv >= 3) {
        const int64_t G16 = 4096 * 16;        /* 4096 px either side of centre */
        int need = 0;
        for (int i = 0; i < q.nrv; i++) {
            int64_t dx = (int64_t)q.rv[i].sx16 - (int64_t)cx * 16;
            int64_t dy = (int64_t)q.rv[i].sy16 - (int64_t)cy * 16;
            if (dx > G16 || dx < -G16 || dy > G16 || dy < -G16) { need = 1; break; }
        }
        if (need) {
            /* screen offset = X * K / Z with K = mant * 16 / 2^shift */
            double K = (double)mant * 16.0 / ldexp(1.0, shift);
            double P[16][6], Q[16][6];
            int np = 0;
            {
                const int UF = 16;
                /* rebuild the near-clipped polygon in float from rv's inputs:
                 * re-derive X, Y from the projected coords would lose them, so
                 * use z and the exact inverse of the projection. */
                for (int i = 0; i < q.nrv; i++) {
                    double Z = (double)q.rv[i].z;
                    double dx = ((double)q.rv[i].sx16 - (double)cx * 16.0);
                    double dy = -((double)q.rv[i].sy16 - (double)cy * 16.0);
                    P[np][0] = dx * Z / K; P[np][1] = dy * Z / K; P[np][2] = Z;
                    P[np][3] = q.rv[i].uf; P[np][4] = q.rv[i].vf; P[np][5] = q.rv[i].bf;
                    np++;
                }
                (void)UF;
            }
            /* saturated vertices lose their true X/Y above; recover them from
             * the pre-projection polygon instead when available */
            if (np == n_pre) {
                for (int i = 0; i < np; i++) { P[i][0] = pre_x[i]; P[i][1] = pre_y[i]; }
            }
            for (int pl = 0; pl < 4 && np >= 3; pl++) {
                int nq = 0;
                for (int i = 0; i < np; i++) {
                    double *a = P[i], *b = P[(i + 1) % np];
                    double da, db, ga = G16 * a[2], gb = G16 * b[2];
                    switch (pl) {
                    case 0: da =  a[0] * K - ga; db =  b[0] * K - gb; break;
                    case 1: da = -a[0] * K - ga; db = -b[0] * K - gb; break;
                    case 2: da =  a[1] * K - ga; db =  b[1] * K - gb; break;
                    default:da = -a[1] * K - ga; db = -b[1] * K - gb; break;
                    }
                    if (da <= 0 && nq < 16) { memcpy(Q[nq++], a, sizeof Q[0]); }
                    if ((da <= 0) != (db <= 0) && nq < 16) {
                        double t = da / (da - db);
                        for (int c = 0; c < 6; c++) Q[nq][c] = a[c] + (b[c] - a[c]) * t;
                        nq++;
                    }
                }
                memcpy(P, Q, sizeof(double) * 6 * nq); np = nq;
            }
            if (np >= 3 && np <= 10) {
                for (int i = 0; i < np; i++) {
                    geo_vert *d = &q.dv[i];
                    double Z = P[i][2] < 1.0 ? 1.0 : P[i][2];
                    d->sx16 = (int32_t)llround((double)cx * 16.0 + P[i][0] * K / Z);
                    d->sy16 = (int32_t)llround((double)cy * 16.0 - P[i][1] * K / Z);
                    d->z  = (int32_t)llround(Z);
                    d->uf = (int32_t)llround(P[i][3]);
                    d->vf = (int32_t)llround(P[i][4]);
                    d->bf = (int32_t)llround(P[i][5]);
                    d->u = (uint32_t)(d->uf >> 16) & 0xfff;
                    d->v = (uint32_t)(d->vf >> 16) & 0xfff;
                    d->bri = d->bf >> 16;
                    d->valid = 1;
                }
                q.ndv = np;
            } else if (np < 3) {
                q.ndv = -1;                     /* entirely outside the guard band */
            }
        }
    }
    if (q.behind) g_geo_stats.part_clip++;
    g_geo_stats.emitted++;
    if (g_geo_stats.zmin_seen == 0 || zmin < g_geo_stats.zmin_seen)
        g_geo_stats.zmin_seen = zmin;
    if (zmax > g_geo_stats.zmax_seen) g_geo_stats.zmax_seen = zmax;
    cb(&q, user);
}

/* ---- packet walk (GeoFixed.blit_quads_fixed) --------------------------- */
static void blit_quads(uint32_t addr, uint32_t length,
                       geo_quad_cb cb, void *user)
{
    uint32_t finish = addr + length;
    while (addr < finish) {
        int32_t packetlength = pt_read(addr); addr += 1;
        int32_t packetformat = pt_read(addr + 0);
        if (packetlength == 0x17) {
            quad_fixed(pt_read(addr + 2), addr + 3, 0,
                       pt_read(addr + 1), packetformat, cb, user);
        } else if (packetlength == 0x18) {
            quad_fixed(pt_read(addr + 2), addr + 4, pt_read(addr + 3),
                       pt_read(addr + 1), packetformat, cb, user);
        } else if (packetlength == 0x10) {
            g_lit_n = 0; g_lit_idx = 0;
            register_normals(addr + 4);
        } else if (packetlength == 0x0d) {
            register_normals(addr + 1);
        } else {
            g_geo_stats.rej_pktlen++;
            return;
        }
        addr += (uint32_t)packetlength;
        if ((packetformat & 0x800000) && addr != finish) return;
    }
}

/* ---- object walk (GeoFixed.blit_polyobject_fixed) ---------------------- */
void geo_hw_object(int32_t code, geo_quad_cb cb, void *user)
{
    if (code == 0x5) return;                  /* point-RAM object: not ported */
    g_lit_n = 0; g_lit_idx = 0;               /* lit_fx is per object */
    uint32_t list_addr = (uint32_t)pt_read(code);
    for (int guard = 0; guard < 4096; guard++) {
        int32_t object_addr = pt_read(list_addr); list_addr += 1;
        if (object_addr < 0) break;           /* -1 terminator (non-pointram) */
        uint32_t chunklength = (uint32_t)pt_read(object_addr);
        object_addr += 1;
        if (chunklength > 0x100) { g_geo_stats.rej_chunk++; break; }
        blit_quads((uint32_t)object_addr, chunklength, cb, user);
    }
}
