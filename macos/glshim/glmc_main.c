/* glmc_main.c — the GL state machine behind the GL-call-shape adapter.
 *
 * Every GL name the games were written against is implemented here in GL
 * semantics — pixel environment combiners, client arrays, matrices and the
 * four known shaders — and the actual GPU work goes through the Metal face
 * (glmc_dev.h, implemented in glmc_metal.mm).
 *
 * Single-threaded; tc2 may open short-lived probe contexts before the real
 * window, so faces are bound/unbound like GL contexts.
 *
 * NOTE: the face owns the coordinate conventions (GL memory row order, the
 * viewport rect transform); everything here is GL window space only. */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#include <SDL.h>
#include "glmc.h"
#include "glmc_dev.h"

void glmc_dev_note(const char *msg);         /* loud, once (device layer)  */
int   g_read_fbo(void);                      /* defined with fbo state      */
void  glmcd_state_reset(void);               /* fresh-context GL defaults   */

#define MAT_STACK_MAX 16
#define TU_MAX 8

struct tenv {               /* a texture unit's environment               */
    int mode;               /* GL_TEXTURE_ENV_MODE (default GL_MODULATE)  */
    int comb;               /* GL_COMBINE configured                      */
    int rop, aop;           /* rgb / alpha op (GL code)                   */
    int rs0, ro0, rs1, ro1, rs2, ro2;   /* rgb sources + operands          */
    int as0, as1;           /* alpha-chain sources (s0, s1)               */
    int rscale_log2, ascale_log2;
    float co[4];            /* GL_TEXTURE_ENV_COLOR                       */
};

struct ctex {                /* a texture OBJECT (GL's own name space)     */
    int live;
    uint32_t id;            /* device texture id                          */
    int w, h, px;           /* stored shape (GLMC_PX_*)                   */
    int minf, magf, ws, wt; /* GL filter / wrap codes as set              */
};

static struct {
    glmcd_face *face;                    /* the bound face               */
    int error;

    /* matrices */
    int mode;                            /* GL_MODELVIEW / GL_PROJECTION */
    float mv[16], pr[16];
    float mvst[16][16]; int mvsp;
    float prst[16][16]; int prsp;

    /* per-draw state */
    int depth_on, depth_func, depth_write;
    int blend_on; int bf_s, bf_d, bf_sa, bf_da, eq_rgb, eq_a;
    int alpha_on; int af_code; float af_ref;
    int sc_on, scx, scy, scw, sch;
    int mask_r, mask_g, mask_b, mask_a;
    int linewidth, shade_flat;
    float clear_r, clear_g, clear_b, clear_a;
    double clear_depth;

    /* viewport (GL window space, bottom-left origin) + fbo bindings        */
    int vmaxx, vmaxy, vww, vwh;      /* glViewport as passed               */
    int fbo_cur;                     /* the DRAW binding (GL_FRAMEBUFFER)  */
    int read_fbo;                    /* the READ binding (defaults fbo_cur) */

    /* texture units */
    int act;                             /* active unit (0..TU_MAX-1)    */
    int tex_on[TU_MAX];
    uint32_t bind[TU_MAX];               /* bound texture NAME per unit  */
    struct tenv env[TU_MAX];
    struct ctex tex[4096];               /* texture name -> object       */
    int unpack_align, unpack_rowlen;

    /* the four known programs: shader objects remember their role, and
     * programs map to GLMC_PROG_*; uniforms stored per program by their
     * slot index (the render code asks for locations)                     */
    int prog;                            /* GLMC_PROG_* or -1            */
    int prog_raw;                        /* bound GL program name        */
    struct tprog {
        int live, mapped;
        char nm[24][24];
        int nu;
        int iv[24][4];
        int keepf;                       /* slot keeps a float (u_keep)  */
        float fv[8];
    } progt[8];

    /* client pointers (GL 1.1 arrays + the tc2 attribute pointers)        */
    struct vptr { const void *p; int size, type, stride; int on; } vp, cp, tp;
    struct vptr ap[8];                   /* tc2 attrib slots 0..4        */
    /* VBOs: name -> bytes (small; tc2 poly frames the whole draw list)    */
    struct vbo { int live; void *bytes; size_t len; } vbov[64];
    int vbo_n; uint32_t vbo_bind;

    /* immediate mode */
    int bmode, bvert;                    /* mode code, vertex index      */
    float bpos[4], bcol[4], btex[4];
    int bhas_col, bhas_tex, bhas_a;
    float *batch; size_t bcap;           /* 3 floats per vertex (pos)    */
    float *bcolv, *btexv;                /* 4 floats per vertex          */
    size_t bcol_cap, btex_cap;

    /* attrib stack (PushAttrib / PopAttrib)                               */
    int natt;
    struct attint {
        int depth_on, depth_func, depth_write, blend_on, bf_s, bf_d, bf_sa, bf_da,
            eq_rgb, eq_a, alpha_on, af_code;
        float af_ref;
        int sc_on, scx, scy, scw, sch, mask_r, mask_g, mask_b, mask_a;
        int act, prog_raw;
        struct tenv env[TU_MAX];
    } att[16];

    /* pack-side pixel store for ReadPixels defaults                       */
    int pack_align, pack_rowlen;
} g;

void glmc_init(void)
{
    memset(&g, 0, sizeof g);
    g.mv[0] = g.mv[5] = g.mv[10] = g.mv[15] = 1.0f;
    g.pr[0] = g.pr[5] = g.pr[10] = g.pr[15] = 1.0f;
    g.mode = GL_MODELVIEW;
    g.mask_r = g.mask_g = g.mask_b = g.mask_a = 1;
    g.depth_func = GL_LESS; g.depth_write = 1;
    g.bf_s = GL_ONE; g.bf_d = GL_ZERO; g.af_ref = 0.0f;
    g.clear_r = 0; g.clear_g = 0; g.clear_b = 0; g.clear_a = 0; g.clear_depth = 1.0;
    g.unpack_align = 4; g.pack_align = 4;
    for (int i = 0; i < TU_MAX; i++)
        g.env[i].mode = GL_MODULATE, g.env[i].rop = GL_MODULATE, g.env[i].aop = GL_MODULATE;
}

/* ------------------------------------------------------------- contexts -- */
static struct tprog *prog_by_name(int name)
{
    if (name < 1 || name > 8) return NULL;
    return &g.progt[name - 1];
}

glmcd_face *glmc_cur_face(void) { return g.face; }

void glmc_context_attach_window(void *sdl_window, void *metal_view, void *metal_layer,
                                int w, int h, float scale)
{
    (void)metal_view; (void)metal_layer;
    /* faces are stateless per-window; a fresh face is a fresh "context"    */
    glmcd_face *f = glmc_dev_face_new(sdl_window, w, h, scale);
    if (!f) {
        fprintf(stderr, "[glmc] no Metal face for a context\n");
        g.error = GL_INVALID_OPERATION;
        return;
    }
    g.face = f;
    glmcd_state_reset();
}

GLenum glmc_GetError(void) { GLenum e = (GLenum)g.error; g.error = GL_NO_ERROR; return e; }

const GLubyte *glmc_glGetString(GLenum name)
{
    switch (name) {
    case GL_RENDERER: return (const GLubyte *)"Metal (GL shape shim)";
    case GL_VENDOR:   return (const GLubyte *)"namco22-mac";
    case GL_VERSION:  return (const GLubyte *)"2.1 GLMC";
    case GL_SHADING_LANGUAGE_VERSION:
        return (const GLubyte *)"1.30";      /* the tc2 1.30-capability gate */
    default: glmc_dev_note("unhandled glGetString pname"); return (const GLubyte *)"";
    }
}

void glmc_GetIntegerv(GLenum pname, GLint *v)
{
    switch (pname) {
    case GL_MAX_TEXTURE_SIZE:      *v = 8192; return;
    case GL_MAX_TEXTURE_UNITS:     *v = TU_MAX; return;
    case GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS: *v = TU_MAX; return;
    case GL_MODELVIEW_STACK_DEPTH:  *v = g.mvsp; return;
    case GL_PROJECTION_STACK_DEPTH: *v = g.prsp; return;
    case GL_TEXTURE_BINDING_2D:
        v[0] = (GLint)g.bind[g.act]; return;
    case GL_VIEWPORT: {
        /* the stored viewport; before any glViewport the window's size     */
        if (g.vww > 0) { v[0] = g.vmaxx; v[1] = g.vmaxy; v[2] = g.vww; v[3] = g.vwh; return; }
        int w = 0, h = 0;
        if (g.face) glmc_dev_window_size(g.face, &w, &h);
        v[0] = 0; v[1] = 0; v[2] = w; v[3] = h; return;
    }
    case GL_SCISSOR_BOX:
        v[0] = g.scx; v[1] = g.scy; v[2] = g.scw; v[3] = g.sch; return;
    case GL_UNPACK_ALIGNMENT: *v = g.unpack_align; return;
    case GL_PACK_ALIGNMENT:   *v = g.pack_align; return;
    case GL_ARRAY_BUFFER_BINDING: *v = (GLint)g.vbo_bind; return;
    case GL_BLEND_SRC_RGB: *v = g.bf_s; return;
    case GL_BLEND_DST_RGB: *v = g.bf_d; return;
    case GL_BLEND_SRC_ALPHA: *v = g.bf_sa; return;
    case GL_BLEND_DST_ALPHA: *v = g.bf_da; return;
    case GL_BLEND_EQUATION_RGB: *v = g.eq_rgb; return;
    case GL_BLEND_EQUATION_ALPHA: *v = g.eq_a; return;
    case GL_ALPHA_TEST_FUNC: *v = g.alpha_on ? g.af_code : 0; return;
    case GL_CURRENT_PROGRAM: *v = g.prog_raw; return;
    case GL_FRAMEBUFFER_BINDING: *v = (GLint)g_read_fbo(); return;
    case GL_ACTIVE_UNIFORMS: case GL_ACTIVE_ATTRIBUTES: *v = 0; return;
    default:
        glmc_dev_note("unhandled glGetIntegerv pname");
        *v = 0; return;
    }
}

/* -------------------------------------------------------------- enabling -- */
void glmc_Enable(GLenum cap)
{
    switch (cap) {
    case GL_DEPTH_TEST:  g.depth_on = 1; break;
    case GL_BLEND:       g.blend_on = 1; break;
    case GL_ALPHA_TEST:  g.alpha_on = 1; break;
    case GL_SCISSOR_TEST: g.sc_on = 1; break;
    case GL_TEXTURE_2D:  g.tex_on[g.act] = 1; break;
    case GL_LINE_SMOOTH: case GL_POLYGON_SMOOTH: case GL_DITHER: case GL_MULTISAMPLE:
        break;                                   /* observed, no-ops        */
    case GL_CULL_FACE:   glmc_dev_note("GL_CULL_FACE enabled (not modelled)"); break;
    case GL_FOG: case GL_LIGHTING: case GL_COLOR_LOGIC_OP:
        glmc_dev_note("unsupported enable (ignored)"); break;
    default: break;
    }
}

void glmc_Disable(GLenum cap)
{
    switch (cap) {
    case GL_DEPTH_TEST:  g.depth_on = 0; break;
    case GL_BLEND:       g.blend_on = 0; break;
    case GL_ALPHA_TEST:  g.alpha_on = 0; break;
    case GL_SCISSOR_TEST: g.sc_on = 0; break;
    case GL_CULL_FACE: break;
    case GL_TEXTURE_2D: g.tex_on[g.act] = 0; break;
    default: break;
    }
}

void glmc_glIsEnabled(GLenum cap) { (void)cap; }
void glmc_glEnablei(GLenum cap, GLuint idx) { (void)cap; (void)idx; }
void glmc_glDisablei(GLenum cap, GLuint idx) { (void)cap; (void)idx; }

GLboolean glmc_IsEnabled(GLenum cap)
{
    switch (cap) {
    case GL_DEPTH_TEST: return (GLboolean)(g.depth_on != 0);
    case GL_BLEND:      return (GLboolean)(g.blend_on != 0);
    case GL_ALPHA_TEST: return (GLboolean)(g.alpha_on != 0);
    case GL_SCISSOR_TEST: return (GLboolean)(g.sc_on != 0);
    default: return 0;
    }
}

/* alpha-test function -> the FFP MSL code (0 off, 1 always, 2 <, 3 =, 4 <=,
 * 5 >, 6 !=, 7 >=, 8 never) */
int glmc_af_code(GLenum f)
{
    switch (f) {
    case GL_ALWAYS:  return 1;
    case GL_LESS:    return 2;
    case GL_EQUAL:   return 3;
    case GL_LEQUAL:  return 4;
    case GL_GREATER: return 5;
    case GL_NOTEQUAL: return 6;
    case GL_GEQUAL:  return 7;
    case GL_NEVER:   return 8;
    default: return 0;
    }
}

void glmc_DepthFunc(GLenum f)  { g.depth_func = f; }
void glmc_DepthMask(GLboolean on) { g.depth_write = on ? 1 : 0; }
void glmc_BlendFunc(GLenum sf, GLenum df) { glmc_BlendFuncSeparate(sf, df, sf, df); }

void glmc_BlendFuncSeparate(GLenum s0, GLenum d0, GLenum s1, GLenum d2)
{ g.bf_s = s0; g.bf_d = d0; g.bf_sa = s1; g.bf_da = d2; }

void glmc_BlendEquation(GLenum e) { glmc_BlendEquationSeparate(e, e); }

void glmc_BlendEquationSeparate(GLenum e0, GLenum e1) { g.eq_rgb = e0; g.eq_a = e1; }

void glmc_AlphaFunc(GLenum f, GLclampf r) { g.af_code = glmc_af_code(f); g.af_ref = (float)r; }

void glmc_PolygonMode(GLenum face, GLenum mode) { (void)face; (void)mode; }
void glmc_ColorMask(GLboolean r, GLboolean gr, GLboolean b, GLboolean a)
{ g.mask_r = r ? 1 : 0; g.mask_g = gr ? 1 : 0; g.mask_b = b ? 1 : 0; g.mask_a = a ? 1 : 0; }
void glmc_LineWidth(GLfloat w) { g.linewidth = (int)w; }
void glmc_ShadeModel(GLenum m) { g.shade_flat = (m == GL_FLAT); }
void glmc_PixelStorei(GLenum p, GLint v)
{
    if (p == GL_UNPACK_ALIGNMENT) g.unpack_align = v;
    if (p == GL_UNPACK_ROW_LENGTH) g.unpack_rowlen = v;
    if (p == GL_PACK_ALIGNMENT) g.pack_align = v;
    if (p == GL_PACK_ROW_LENGTH) g.pack_rowlen = v;
}
void glmc_PixelTransferi(GLenum p, GLint v)
{ (void)p; (void)v; }        /* only GL map forwards/none upstream */
void glmc_PixelMapfv(GLenum map, GLsizei n, const GLfloat *vals)
{
    (void)map; (void)n; (void)vals;
    /* the engine's rt_begin GLSL-1.10 pixel-map path never runs: the PFNs
     * always resolve. A real call here means an unaccounted draw path.      */
    glmc_dev_note("glPixelMapfv reached (unexpected: maps path is dead upstream)");
}

/* CHUNKA */

/* ----------------------------------------------------- state helpers ----- */
void glmcd_state_reset(void)
{
    g.mask_r = g.mask_g = g.mask_b = g.mask_a = 1;
    g.depth_func = GL_LESS; g.depth_write = 1;
    g.bf_s = GL_ONE; g.bf_d = GL_ZERO; g.eq_rgb = GL_FUNC_ADD; g.eq_a = GL_FUNC_ADD;
    g.mv[0] = g.mv[5] = g.mv[10] = g.mv[15] = 1.0f;
    g.pr[0] = g.pr[5] = g.pr[10] = g.pr[15] = 1.0f;
    g.mode = GL_MODELVIEW; g.mvsp = g.prsp = 0;
    g.act = 0; g.prog = 0; g.prog_raw = 0;       /* 0 = PR_FFP (GLMC_PROG_FFP) */
    g.vbo_n = 0; g.fbo_cur = 0; g.read_fbo = 0;
    g.clear_r = 0; g.clear_g = 0; g.clear_b = 0; g.clear_a = 0; g.clear_depth = 1.0;
    g.unpack_align = 4; g.pack_align = 4; g.unpack_rowlen = 0; g.pack_rowlen = 0;
    g.error = GL_NO_ERROR;
    for (int i = 0; i < TU_MAX; i++) {
        memset(&g.env[i], 0, sizeof g.env[i]);
        g.env[i].mode = g.env[i].rop = g.env[i].aop = GL_MODULATE;
        g.env[i].co[3] = 1.0f;
        g.tex_on[i] = 0; g.bind[i] = 0;
        memset(&g.ap[i], 0, sizeof g.ap[i]);
    }
    /* the GL default current attributes: colour white, texcoord 0,0,0,1      */
    g.bcol[0] = 1.0f; g.bcol[1] = 1.0f; g.bcol[2] = 1.0f; g.bcol[3] = 1.0f;
    g.btex[0] = 0.0f; g.btex[1] = 0.0f; g.btex[2] = 0.0f; g.btex[3] = 1.0f;
    memset(&g.vp, 0, sizeof g.vp);
    memset(&g.cp, 0, sizeof g.cp);
    memset(&g.tp, 0, sizeof g.tp);
}

/* ----------------------------------------------------- matrix functions -- */
static void glmcd_mat4_mult(float out[16], const float a[16], const float b[16])
/* column-major a*b (out = a * b)                                              */
{
    float t[16];
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++) {
            float s = 0.f;
            for (int k = 0; k < 4; k++) s += a[k * 4 + r] * b[c * 4 + k];
            t[c * 4 + r] = s;
        }
    memcpy(out, t, sizeof t);
}

void glmc_MatrixMode(GLenum m) { g.mode = m; }

void glmc_LoadIdentity(void)
{
    float *m = (g.mode == GL_PROJECTION) ? g.pr : g.mv;
    memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

void glmc_PushMatrix(void)
{
    if (g.mode == GL_PROJECTION) {
        if (g.prsp >= MAT_STACK_MAX) { g.error = GL_STACK_OVERFLOW; return; }
        memcpy(g.prst[g.prsp], g.pr, sizeof g.pr); g.prsp++;
    } else {
        if (g.mvsp >= MAT_STACK_MAX) { g.error = GL_STACK_OVERFLOW; return; }
        memcpy(g.mvst[g.mvsp], g.mv, sizeof g.mv); g.mvsp++;
    }
}

void glmc_PopMatrix(void)
{
    if (g.mode == GL_PROJECTION) {
        if (g.prsp <= 0) { g.error = GL_STACK_UNDERFLOW; return; }
        g.prsp--; memcpy(g.pr, g.prst[g.prsp], sizeof g.pr);
    } else {
        if (g.mvsp <= 0) { g.error = GL_STACK_UNDERFLOW; return; }
        g.mvsp--; memcpy(g.mv, g.mvst[g.mvsp], sizeof g.mv);
    }
}

static void glmcd_cur_mult(const float m[16])
{
    float *cur = (g.mode == GL_PROJECTION) ? g.pr : g.mv;
    glmcd_mat4_mult(cur, cur, m);
}

void glmc_Ortho(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f)
{
    float m[16] = { 0 };
    m[0] = (float)(2.0 / (r - l));
    m[5] = (float)(2.0 / (t - b));
    m[10] = (float)(-2.0 / (f - n));
    m[12] = (float)(-(r + l) / (r - l));
    m[13] = (float)(-(t + b) / (t - b));
    m[14] = (float)(-(f + n) / (f - n));
    m[15] = 1.0f;
    glmcd_cur_mult(m);
}

void glmc_Frustum(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f)
{
    float m[16] = { 0 };
    m[0] = (float)(2.0 * n / (r - l));
    m[5] = (float)(2.0 * n / (t - b));
    m[8] = (float)((r + l) / (r - l));
    m[9] = (float)((t + b) / (t - b));
    m[10] = (float)(-(f + n) / (f - n));
    m[11] = -1.0f;
    m[14] = (float)(-2.0 * f * n / (f - n));
    glmcd_cur_mult(m);
}

void glmc_Translatef(GLfloat x, GLfloat y, GLfloat z) { glmc_Translated(x, y, z); }
void glmc_Scalef(GLfloat x, GLfloat y, GLfloat z) { glmc_Scaled(x, y, z); }
void glmc_Rotatef(GLfloat a, GLfloat x, GLfloat y, GLfloat z) { glmc_Rotated(a, x, y, z); }

void glmc_Translated(GLdouble x, GLdouble y, GLdouble z)
{
    float m[16] = { 1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  (float)x, (float)y, (float)z, 1 };
    glmcd_cur_mult(m);
}
void glmc_Scaled(GLdouble x, GLdouble y, GLdouble z)
{
    float m[16] = { (float)x, 0, 0, 0,  0, (float)y, 0, 0,  0, 0, (float)z, 0,  0, 0, 0, 1 };
    glmcd_cur_mult(m);
}
void glmc_Rotated(GLdouble ad, GLdouble x, GLdouble y, GLdouble z)
{
    float a = (float)(ad * 3.14159265358979323846 / 180.0);
    float c = cosf(a), s = sinf(a);
    float len = (float)sqrt(x * x + y * y + z * z);
    if (len < 1e-20) return;
    float vx = (float)(x / len), vy = (float)(y / len), vz = (float)(z / len), ic = 1.0f - c;
    float m[16] = { 0 };
    m[0]  = vx * vx * ic + c;        m[4]  = vx * vy * ic - vz * s;  m[8]  = vx * vz * ic + vy * s;
    m[1]  = vy * vx * ic + vz * s;   m[5]  = vy * vy * ic + c;       m[9]  = vy * vz * ic - vx * s;
    m[2]  = vz * vx * ic - vy * s;   m[6]  = vz * vy * ic + vx * s;  m[10] = vz * vz * ic + c;
    m[15] = 1.0f;
    glmcd_cur_mult(m);
}

void glmc_MultMatrixf(const GLfloat *m) { float t[16]; for (int i = 0; i < 16; i++) t[i] = m[i]; glmcd_cur_mult(t); }
void glmc_MultMatrixd(const GLdouble *d) { float t[16]; for (int i = 0; i < 16; i++) t[i] = (float)d[i]; glmcd_cur_mult(t); }

/* CHUNKB */

/* ---------------------------------------------------------- tex objects -- */
static struct ctex *tex_obj(uint32_t name)
{
    if (name < 1 || name > 4096) return NULL;
    return &g.tex[name - 1];
}

void glmc_GenTextures(GLsizei n, GLuint *out)
{
    for (GLsizei i = 0; i < n; i++) {
        int slot = -1;
        for (int j = 0; j < 4096; j++)
            if (!g.tex[j].live) { slot = j; break; }
        if (slot < 0) { g.error = GL_OUT_OF_MEMORY; out[i] = 0; continue; }
        memset(&g.tex[slot], 0, sizeof g.tex[slot]);
        g.tex[slot].live = 1;
        g.tex[slot].minf = GL_NEAREST;          /* upstream sets its own   */
        g.tex[slot].magf = GL_LINEAR;
        g.tex[slot].ws = GL_REPEAT; g.tex[slot].wt = GL_REPEAT;
        out[i] = (GLuint)(slot + 1);
    }
}

GLboolean glmc_IsTexture(GLuint name)
{ return tex_obj(name) && tex_obj(name)->live; }

void glmc_DeleteTextures(GLsizei n, const GLuint *names)
{
    for (GLsizei i = 0; i < n; i++) {
        struct ctex *t = tex_obj(names[i]);
        if (!t || !t->live) continue;
        if (g.face) glmc_tex_del(g.face, t->id);
        memset(t, 0, sizeof *t);
        for (int u = 0; u < TU_MAX; u++) if (g.bind[u] == names[i]) g.bind[u] = 0;
    }
}

/* the upload formats the engine uploads; everything expands into the device's
 * small shape set                                                          */
static int px_from_internal(int internal, GLenum fmt, GLenum type)
{
    (void)type;
    switch (internal) {
    case GL_LUMINANCE8: case GL_LUMINANCE: return GLMC_PX_R8;
    case GL_LUMINANCE8_ALPHA8: return GLMC_PX_RG8;
    case GL_BGRA: return GLMC_PX_BGRA8;      /* BGRA-ordered rows straight in */
    case GL_DEPTH_COMPONENT32F: case GL_DEPTH_COMPONENT24: case GL_DEPTH_COMPONENT16:
        return GLMC_PX_DEPTH32F;
    case GL_RGBA: case GL_RGBA8: case GL_ALPHA8: case GL_ALPHA: case GL_RGB8:
    case GL_RGB5_A1: case GL_RGBA4:
        return GLMC_PX_RGBA8;
    default:
        if (internal == GL_RGB) return GLMC_PX_RGBA8;
        if (fmt == GL_BGRA) return GLMC_PX_BGRA8;
        return GLMC_PX_RGBA8;
    }
}

/* client-side bytes per pixel of the upload format */
static int glmcd_srcbpp(GLenum fmt)
{
    switch (fmt) {
    case GL_RGB:  return 3;
    case GL_LUMINANCE: case GL_ALPHA: return 1;
    case GL_LUMINANCE_ALPHA: return 2;
    case GL_DEPTH_COMPONENT: return 4;            /* GL_FLOAT rows */
    default: return 4;                            /* GL_RGBA, GL_BGRA */
    }
}

static uint8_t *glmcd_expbuf;                        /* the expansion scratch */
static size_t glmcd_expcap;

static int glmcd_direct(GLenum fmt, int px)
{
    if (fmt == GL_LUMINANCE && px == GLMC_PX_R8) return 1;
    if (fmt == GL_LUMINANCE_ALPHA && px == GLMC_PX_RG8) return 1;
    if (fmt == GL_BGRA && px == GLMC_PX_BGRA8) return 1;
    if (fmt == GL_RGBA && px == GLMC_PX_RGBA8) return 1;
    if (fmt == GL_DEPTH_COMPONENT && px == GLMC_PX_DEPTH32F) return 1;
    return 0;
}

static void glmcd_expbuf_get(size_t bytes)
{
    if (bytes > glmcd_expcap) {
        glmcd_expcap = bytes < 1 << 20 ? 1 << 20 : bytes;
        glmcd_expbuf = realloc(glmcd_expbuf, glmcd_expcap);
    }
}

/* the upload data as the device texture consumes it, with the pitch the
 * replaceRegion needs. Direct cases pass the caller's rows through; the rest
 * expands row by row (GL rows stay GL rows: the face's storage is GL-ordered).
 * Handles GL_UNPACK_ROW_LENGTH and GL_UNPACK_ALIGNMENT. */
static const uint8_t *glmcd_upload_prep(GLenum fmt, int w, int h, const void *data,
                                        int px, int unpack_rowlen, int unpack_align,
                                        int *pitch_out)
{
    int bpp = glmcd_srcbpp(fmt);
    int dw = (px == GLMC_PX_R8) ? 1 : (px == GLMC_PX_RG8) ? 2 : 4;
    long rowlen = unpack_rowlen > 0 ? unpack_rowlen : w;
    long stride = rowlen * bpp;
    stride = (stride + unpack_align - 1) / unpack_align * unpack_align;
    *pitch_out = (int)(w * dw);

    /* fast path: source and device byte layout match, there is no row padding
     * to skip over, and the row length is already alignment-safe */
    if (glmcd_direct(fmt, px) && rowlen == w && (long)(w * bpp) % unpack_align == 0) {
        *pitch_out = (int)(w * bpp);
        return (const uint8_t *)data;
    }

    size_t need = (size_t)w * dw * (size_t)h;
    glmcd_expbuf_get(need);
    if (!glmcd_expbuf) return NULL;
    const uint8_t *s = data;
    for (int j = 0; j < h; j++) {
        const uint8_t *srow = s + (long)j * stride;
        uint8_t *d = glmcd_expbuf + (size_t)j * (size_t)w * dw;
        for (int i = 0; i < w; i++, srow += bpp, d += dw) {
            /* direct formats are already in the device byte order: plain copy */
            if (glmcd_direct(fmt, px)) {
                memcpy(d, srow, (size_t)dw);
                continue;
            }
            switch (fmt) {
            case GL_RGB:            d[0] = srow[0]; d[1] = srow[1]; d[2] = srow[2];
                                    d[3] = 255; break;      /* the tc2 tile map */
            case GL_ALPHA:          memset(d, 0, (size_t)dw); d[3] = srow[0]; break;
            case GL_LUMINANCE:      d[0] = d[1] = d[2] = srow[0]; d[3] = 255; break;
            case GL_LUMINANCE_ALPHA: d[0] = srow[0]; d[1] = srow[1]; d[2] = 0; d[3] = srow[1]; break;
            case GL_RGBA:           d[0] = srow[0]; d[1] = srow[1]; d[2] = srow[2]; d[3] = srow[3]; break;
            case GL_BGRA:           d[0] = srow[2]; d[1] = srow[1]; d[2] = srow[0]; d[3] = srow[3]; break;
            default:                d[0] = srow[0]; d[1] = srow[1]; d[2] = srow[2]; d[3] = (dw > 3) ? srow[3] : 0; break;
            }
        }
    }
    return glmcd_expbuf;
}

void glmc_TexImage2D(GLenum target, GLint level, GLint internal, GLsizei w, GLsizei h,
                     GLint border, GLenum fmt, GLenum type, const void *data)
{
    (void)level; (void)border; (void)type;
    if (target != GL_TEXTURE_2D) { glmc_dev_note("non-2D tex image target"); return; }
    struct ctex *t = tex_obj(g.bind[g.act]);
    if (!t || !t->live) { glmc_dev_note("teximage without a bound object"); return; }
    int px = px_from_internal(internal, fmt, type);
    if (!g.face) return;
    if (!t->id) t->id = glmc_tex_new(g.face);
    glmc_tex_storage(g.face, t->id, px, w, h);
    t->w = w; t->h = h; t->px = px;
    if (!data) return;
    int pitch = 0;
    const uint8_t *bytes = glmcd_upload_prep(fmt, w, h, data, px,
                                             g.unpack_rowlen, g.unpack_align, &pitch);
    if (!bytes) { glmc_dev_note("upload data unavailable (memory)"); return; }
    glmc_tex_upload(g.face, t->id, 0, 0, w, h, bytes, pitch);
}

void glmc_TexSubImage2D(GLenum target, GLint level, GLint xo, GLint yo,
                        GLsizei w, GLsizei h, GLenum fmt, GLenum type, const void *data)
{
    (void)level; (void)type;
    if (target != GL_TEXTURE_2D) { glmc_dev_note("non-2D tex subimage target"); return; }
    struct ctex *t = tex_obj(g.bind[g.act]);
    if (!t || !t->live || !t->id || !g.face) { glmc_dev_note("texsubimage without storage"); return; }
    if (!data) return;
    int pitch = 0;
    const uint8_t *bytes = glmcd_upload_prep(fmt, w, h, data, t->px,
                                             g.unpack_rowlen, g.unpack_align, &pitch);
    if (!bytes) { glmc_dev_note("upload data unavailable (memory)"); return; }
    glmc_tex_upload(g.face, t->id, xo, yo, w, h, bytes, pitch);
}

void glmc_TexParameteri(GLenum target, GLenum pname, GLint v)
{
    struct ctex *t = tex_obj(g.bind[g.act]);
    if (!t || !t->live || target != GL_TEXTURE_2D) return;
    switch (pname) {
    case GL_TEXTURE_MIN_FILTER: t->minf = v; return;
    case GL_TEXTURE_MAG_FILTER: t->magf = v; return;
    case GL_TEXTURE_WRAP_S:     t->ws = v; return;
    case GL_TEXTURE_WRAP_T:     t->wt = v; return;
    case GL_TEXTURE_COMPARE_MODE: case GL_TEXTURE_COMPARE_FUNC: return;
    default: glmc_dev_note("unhandled glTexParameteri pname"); return;
    }
}
void glmc_TexParameterf(GLenum target, GLenum pname, GLfloat v)
{ glmc_TexParameteri(target, pname, (GLint)(v + 0.5f)); }

void glmc_TexEnvi(GLenum tgt, GLenum pname, GLint v)
{
    if (tgt != GL_TEXTURE_ENV) return;
    struct tenv *e = &g.env[g.act];
    switch (pname) {
    case GL_TEXTURE_ENV_MODE:
        e->mode = v;
        return; /* GL keeps COMBINE settings when the mode changes. */
    case GL_COMBINE_RGB:    e->rop = v; return;
    case GL_COMBINE_ALPHA:  e->aop = v; return;
    case GL_SOURCE0_RGB:    e->rs0 = v; return;
    case GL_SOURCE1_RGB:    e->rs1 = v; return;
    case GL_SOURCE2_RGB:    e->rs2 = v; return;
    case GL_OPERAND0_RGB:   e->ro0 = v; return;
    case GL_OPERAND1_RGB:   e->ro1 = v; return;
    case GL_OPERAND2_RGB:   e->ro2 = v; return;
    case GL_SOURCE0_ALPHA:
        e->as0 = v; return;
    case GL_SOURCE1_ALPHA:
        e->as1 = v; return;
    case GL_RGB_SCALE:
    case GL_ALPHA_SCALE: return;
    default: glmc_dev_note("unhandled glTexEnvi pname"); return;
    }
}

void glmc_TexEnvf(GLenum tgt, GLenum pname, GLfloat v)
{
    if (tgt != GL_TEXTURE_ENV) return;
    struct tenv *e = &g.env[g.act];
    switch (pname) {
    case GL_RGB_SCALE: {
        int k = 0; float s = v;
        while (s > 1.9f) { s *= 0.5f; k++; }
        e->rscale_log2 = k;
        if (s != 1.0f) glmc_dev_note("non-power RGB scale");
        return;
    }
    case GL_ALPHA_SCALE:
        e->ascale_log2 = (v != 1.0f);  /* nonzero flag; alpha scale unsupported */
        if (v != 1.0f) glmc_dev_note("GL_ALPHA_SCALE != 1 (unsupported)");
        return;
    case GL_TEXTURE_ENV_MODE: e->mode = (int)v; return;
    default: return;
    }
}

void glmc_TexEnvfv(GLenum tgt, GLenum pname, const GLfloat *v)
{
    if (tgt != GL_TEXTURE_ENV || pname != GL_TEXTURE_ENV_COLOR || !v) return;
    memcpy(g.env[g.act].co, v, sizeof(float) * 4);
}

void glmc_ActiveTexture(GLenum unit)
{
    int u = (int)unit - GL_TEXTURE0;
    if (unit == 0 || unit == 0x30) return;        /* legacy passthrough       */
    if (u < 0 || u >= TU_MAX) { glmc_dev_note("active texture out of range"); return; }
    g.act = u;
}

void glmc_BindTexture(GLenum target, GLuint name)
{
    if (target != GL_TEXTURE_2D) { glmc_dev_note("non-2D bind target"); return; }
    g.bind[g.act] = name;
}

/* ------------------------------------------------- texture sampling nodes -- */
static void glmcd_samp_node(glmcd_samp *s, struct ctex *t)
{
    memset(s, 0, sizeof *s);
    if (!t || !t->live || !t->id) return;
    s->tex = t->id;
    s->minf = t->minf; s->magf = t->magf;
    s->wraps = t->ws; s->wrapt = t->wt;
}

/* ----------------------------------- env combiners -> the FFP uniform --- */
/* the FFP source/operand codes in glmc_msl.h: TEX=0, PRIMARY=1, CONSTANT=2,
 * PREVIOUS=3; operand color / alpha / 1-color / 1-alpha = 0..3              */
static int env_src(int gls)
{
    switch (gls) {
    case GL_TEXTURE:       return 0;
    case GL_PRIMARY_COLOR: return 1;
    case GL_CONSTANT:      return 2;
    case GL_PREVIOUS:      return 3;
    default:
        glmc_dev_note("unsupported texenv source");
        return 3;
    }
}
static int env_opnd(int gld)
{
    switch (gld) {
    case GL_SRC_COLOR:   return 0;
    case GL_SRC_ALPHA:   return 1;
    case GL_ONE_MINUS_SRC_COLOR: return 2;
    case GL_ONE_MINUS_SRC_ALPHA: return 3;
    default:
        glmc_dev_note("unsupported texenv operand");
        return 0;
    }
}
static int env_op(int glo)
{
    switch (glo) {
    case GL_MODULATE: return 0;
    case GL_REPLACE: return 1;
    case GL_INTERPOLATE: return 2;
    case GL_ADD:
        glmc_dev_note("texenv ADD treated as REPLACE (check drawing use)");
        return 1;
    case GL_DECAL:
        glmc_dev_note("texenv DECAL treated as REPLACE (check drawing use)");
        return 1;
    default:
        glmc_dev_note("unsupported texenv op");
        return 1;
    }
}

static void glmcd_fill_env_unit(glmcd_ffp_u *u, int unit, const struct tenv *e)
{
    int base = unit * 12;
    const int combined = e->mode == GL_COMBINE;
    /* E[op row]: rgb op, alpha op, scale code, 0                       */
    u->e[base + 0] = env_op(combined ? e->rop : e->mode);
    u->e[base + 1] = env_op(combined ? e->aop : e->mode);
    u->e[base + 2] = combined ? e->rscale_log2 : 0;
    u->e[base + 3] = 0;
    /* E[s0/s1 row]: (s0, o0) + (s1, o1). The GL combiner defaults are
     *   SOURCE0_RGB = GL_TEXTURE, SOURCE1_RGB = GL_PREVIOUS,
     *   SOURCE2_RGB = GL_CONSTANT, so MODULATE -> tex*prev and
     *   REPLACE -> tex (the game's two modes).                         */
    u->e[base + 4] = env_src(combined && e->rs0 ? e->rs0 : GL_TEXTURE);
    u->e[base + 5] = env_opnd(combined && e->ro0 ? e->ro0 : GL_SRC_COLOR);
    u->e[base + 6] = env_src(combined && e->rs1 ? e->rs1 : GL_PREVIOUS);
    u->e[base + 7] = env_opnd(combined && e->ro1 ? e->ro1 : GL_SRC_COLOR);
    /* E[s2/alpha row]: (s2, o2) + alpha sources (SOURCE0_A = GL_TEXTURE,
     * SOURCE1_A = GL_PREVIOUS)                                         */
    u->e[base + 8] = env_src(combined && e->rs2 ? e->rs2 : GL_CONSTANT);
    u->e[base + 9] = env_opnd(combined && e->ro2 ? e->ro2 : GL_SRC_ALPHA);
    u->e[base + 10] = env_src(combined && e->as0 ? e->as0 : GL_TEXTURE);
    u->e[base + 11] = env_src(combined && e->as1 ? e->as1 : GL_PREVIOUS);
}

static void glmcd_fill_ffp(glmcd_ffp_u *u)
{
    glmcd_mat4_mult(u->mvp, g.pr, g.mv);
    memcpy(u->envc0, g.env[0].co, sizeof u->envc0);
    memcpy(u->envc1, g.env[1].co, sizeof u->envc1);
    memset(u->e, 0, sizeof u->e);
    glmcd_fill_env_unit(u, 0, &g.env[0]);
    glmcd_fill_env_unit(u, 1, &g.env[1]);
    /* row 6: tex enables, alpha test, ref*255                              */
    u->e[24] = g.tex_on[0] ? 1 : 0;
    u->e[25] = g.tex_on[1] ? 1 : 0;
    u->e[26] = g.alpha_on ? g.af_code : 0;
    u->e[27] = (int)(g.af_ref * 255.0f + 0.5f);
    /* the stored formats drive the R8/RG8 luminance expansion in the shader */
    struct ctex *tf0 = tex_obj(g.bind[0]);
    struct ctex *tf1 = tex_obj(g.bind[1]);
    u->texfmt[0] = (tf0 && tf0->live && tf0->id) ? tf0->px : GLMC_PX_RGBA8;
    u->texfmt[1] = (tf1 && tf1->live && tf1->id) ? tf1->px : GLMC_PX_RGBA8;
    u->texfmt[2] = u->texfmt[3] = 0;
    /* engine units use unit1's scale too; the rows carry each unit's own   */
}

/* CHUNKB2 */

/* ---------------------------- GLSL 1.30-style program machinery ---------- */

/* shader "role" sniffed from the source: which known program is this        */
enum { SH_NONE = 0, SH_TC2_VS_POLY, SH_TC2_VS_TEXT, SH_TC2_FS_POLY,
       SH_TC2_FS_TEXT, SH_TC2_FS_GAMMA, SH_POST1_FS, SH_ROLE_MAX };

struct shobj {
    int live, kind, role;
    char *src;
    size_t srclen;
};
static struct shobj gsh[24];
static int gsh_n;                    /* names issued so far            */

static struct shobj *sh_by_name(int name)
{ return (name >= 1 && name <= 24 && gsh[name - 1].live) ? &gsh[name - 1] : NULL; }

static int glmcd_role_of_source(GLenum kind, const char *src)
{
    if (!src) return SH_NONE;
    int frag = (kind == GL_FRAGMENT_SHADER);
    if (frag) {
        if (strstr(src, "t_src"))       return SH_TC2_FS_GAMMA;
        if (strstr(src, "t_mix") || strstr(src, "u_amask")) return SH_TC2_FS_TEXT;
        if (strstr(src, "t_near") || strstr(src, "u_pcol")) return SH_TC2_FS_POLY;
        if (strstr(src, "frame") && strstr(src, "lut")) return SH_POST1_FS;
    } else {
        if (strstr(src, "a_uvs") || strstr(src, "a_par")) return SH_TC2_VS_POLY;
        if (strstr(src, "a_pos"))                      return SH_TC2_VS_TEXT;
    }
    glmc_dev_note("shader source did not match a known program");
    return SH_NONE;
}

GLuint glmc_CreateShaderName(GLenum kind)
{
    for (int i = 0; i < 24; i++)
        if (!gsh[i].live) {
            memset(&gsh[i], 0, sizeof gsh[i]);
            gsh[i].live = 1; gsh[i].kind = (int)kind; gsh[i].role = SH_NONE;
            return (GLuint)(i + 1);
        }
    g.error = GL_OUT_OF_MEMORY;
    return 0;
}

void glmc_ShaderSource(GLuint shader, GLsizei n, const GLchar *const *src, const GLint *len)
{
    struct shobj *s = sh_by_name((int)shader);
    if (!s || n < 1 || !src) { g.error = GL_INVALID_VALUE; return; }
    size_t total = 0;
    for (GLsizei i = 0; i < n; i++)
        total += (len && len[i] >= 0) ? (size_t)len[i] : (src[i] ? strlen(src[i]) : 0);
    free(s->src);
    s->src = (char *)malloc(total + 1);
    if (!s->src) { g.error = GL_OUT_OF_MEMORY; return; }
    s->srclen = total;
    char *o = s->src;
    for (GLsizei i = 0; i < n; i++) {
        size_t k = (len && len[i] >= 0) ? (size_t)len[i] : (src[i] ? strlen(src[i]) : 0);
        if (k) memcpy(o, src[i], k);
        o += k;
    }
    *o = 0;
}

void glmc_CompileShader(GLuint shader)
{
    struct shobj *s = sh_by_name((int)shader);
    if (!s) { g.error = GL_INVALID_VALUE; return; }
    s->role = glmcd_role_of_source((GLenum)s->kind, s->src);
    if (s->role == SH_NONE)
        fprintf(stderr, "[glmc] shader %u: source does not match a known program\n", shader);
}

void glmc_GetShaderiv(GLuint shader, GLenum pname, GLint *v)
{
    struct shobj *s = sh_by_name((int)shader);
    if (v) *v = 0;
    if (!s || !v) { if (shader) g.error = GL_INVALID_VALUE; return; }
    if (pname == GL_COMPILE_STATUS)
        *v = (s->role != SH_NONE) ? 1 : 0;
    else if (pname == GL_SHADER_TYPE)
        *v = s->kind;
}

void glmc_GetShaderInfoLog(GLuint shader, GLsizei len, GLsizei *got, char *log)
{
    struct shobj *s = sh_by_name((int)shader);
    const char *msg = (!s || s->role == SH_NONE) ? "not one of the ported programs" : "";
    GLsizei k = (GLsizei)strlen(msg);
    if (log && len > 0) { strncpy(log, msg, (size_t)len - 1); log[len - 1] = 0; }
    if (got) *got = k;
}

/* --------------------------------------------------- program objects ----- */
/* the GLMC_PROG_* ids; PR_NONE = no usable mapping */
enum { PR_NONE = -1, PR_FFP = GLMC_PROG_FFP, PR_POST1 = GLMC_PROG_POST1,
       PR_TC2_POLY = GLMC_PROG_TC2_POLY, PR_TC2_TEXT = GLMC_PROG_TC2_TEXT,
       PR_TC2_GAMMA = GLMC_PROG_TC2_GAMMA };

/* the roles attached so far; the mapping is recomputed on every attach     */
static struct { int roles[7]; } proga[8];

static struct tprog *pr_by_name(int name)
{ return (name >= 1 && name <= 8) ? &g.progt[name - 1] : NULL; }

static void glmcd_remap(struct tprog *p, int idx)
{
    p->mapped = -1;
    int *r = proga[idx].roles;
    if (r[SH_TC2_VS_POLY] && r[SH_TC2_FS_POLY])   p->mapped = PR_TC2_POLY;
    else if (r[SH_TC2_VS_TEXT] && r[SH_TC2_FS_GAMMA]) p->mapped = PR_TC2_GAMMA;
    else if (r[SH_TC2_VS_TEXT] && r[SH_TC2_FS_TEXT])  p->mapped = PR_TC2_TEXT;
    else if (r[SH_POST1_FS])                      p->mapped = PR_POST1;
}

GLuint glmc_CreateProgramName(void)
{
    for (int i = 0; i < 8; i++)
        if (!g.progt[i].live) {
            memset(&g.progt[i], 0, sizeof g.progt[i]);
            g.progt[i].live = 1; g.progt[i].mapped = -1;
            memset(&proga[i], 0, sizeof proga[i]);
            return (GLuint)(i + 1);
        }
    g.error = GL_OUT_OF_MEMORY;
    return 0;
}

void glmc_AttachShader(GLuint prog, GLuint shader)
{
    struct tprog *p = pr_by_name((int)prog);
    struct shobj *s = sh_by_name((int)shader);
    if (!p || !s || prog < 1 || prog > 8) { g.error = GL_INVALID_VALUE; return; }
    proga[prog - 1].roles[s->role]++;
    glmcd_remap(p, (int)prog - 1);
}

void glmc_BindAttribLocation(GLuint prog, GLuint idx, const char *name)
{ (void)prog; (void)idx; (void)name; }    /* the MSL attributes are fixed */

void glmc_LinkProgram(GLuint prog)
{
    struct tprog *p = pr_by_name((int)prog);
    if (!p) { g.error = GL_INVALID_VALUE; return; }
    if (p->mapped < 0)
        glmc_dev_note("program linked without a recognised role");
}

void glmc_GetProgramiv(GLuint prog, GLenum pname, GLint *v)
{
    struct tprog *p = pr_by_name((int)prog);
    if (v) *v = (p && pname == GL_LINK_STATUS) ? (p->mapped >= 0) : 0;
}

void glmc_GetProgramInfoLog(GLuint prog, GLsizei len, GLsizei *got, char *log)
{
    struct tprog *p = pr_by_name((int)prog);
    const char *msg = (!p || p->mapped < 0) ? "program role unknown" : "";
    GLsizei k = (GLsizei)strlen(msg);
    if (log && len > 0) { strncpy(log, msg, (size_t)len - 1); log[len - 1] = 0; }
    if (got) *got = k;
}

void glmc_UseProgram(GLuint prog)
{
    if (prog == 0) { g.prog = PR_FFP; g.prog_raw = 0; return; }
    struct tprog *p = pr_by_name((int)prog);
    if (!p || p->mapped < 0) {
        glmc_dev_note("use of an unknown program (ignored)");
        g.prog = -1; g.prog_raw = 0;
        return;
    }
    g.prog = p->mapped; g.prog_raw = (int)prog;
}

GLint glmc_GetUniformLocation(GLuint prog, const char *name)
{
    struct tprog *p = pr_by_name((int)prog);
    if (!p || !name) return -1;
    for (int i = 0; i < p->nu; i++)
        if (!strcmp(p->nm[i], name)) return (GLint)i;
    if (p->nu >= 24) { glmc_dev_note("too many uniform slots"); return -1; }
    strncpy(p->nm[p->nu], name, sizeof p->nm[0] - 1);
    p->nm[p->nu][sizeof p->nm[0] - 1] = 0;
    return (GLint)p->nu++;
}

static void glmcd_stor(int loc, GLint a, GLint b, GLint c, GLint d)
{
    if (g.prog_raw < 1) return;
    struct tprog *p = &g.progt[g.prog_raw - 1];
    if (p->mapped < 0 || loc < 0 || loc >= p->nu) return;
    p->iv[loc][0] = a; p->iv[loc][1] = b; p->iv[loc][2] = c; p->iv[loc][3] = d;
}

void glmc_Uniform1i(GLint loc, GLint v) { glmcd_stor(loc, v, 0, 0, 0); }
void glmc_Uniform3i(GLint loc, GLint a, GLint b, GLint c) { glmcd_stor(loc, a, b, c, 0); }
void glmc_Uniform4i(GLint loc, GLint a, GLint b, GLint c, GLint d) { glmcd_stor(loc, a, b, c, d); }
void glmc_Uniform1f(GLint loc, GLfloat v)
{
    if (g.prog_raw < 1) return;
    struct tprog *p = &g.progt[g.prog_raw - 1];
    if (p->mapped < 0 || loc < 0 || loc >= p->nu) return;
    p->keepf = 1;
    if (loc < 8) p->fv[loc] = v;
}
GLint glmc_GetAttribLocation(GLuint prog, const char *name)
{ (void)prog; (void)name; return -1; }

/* ---- client-side arrays and buffers ------------------------------------ */
static void vptr_set(struct vptr *v, GLint size, GLenum type, GLsizei st, const void *p)
{
    v->p = p; v->size = size; v->type = (int)type; v->stride = st; v->on = 1;
}
static const unsigned char *vptr_aptr(struct vptr *v, int i)
{
    const unsigned char *base = (const unsigned char *)v->p;
    if (!base) return NULL;
    int st = v->stride;
    if (!st) st = (v->type == GL_FLOAT) ? v->size * 4 : v->size;
    return base + (size_t)i * (size_t)st;
}

/* element i of a client array as floats: its stride, GL_FLOAT or normalised
 * GL_UNSIGNED_BYTE components; missing components keep out[]'s defaults    */
static int vptr_fetch(struct vptr *v, int i, float *out)
{
    const unsigned char *e = v->on ? vptr_aptr(v, i) : NULL;
    if (!e) return 0;
    const int n = v->size < 1 ? 1 : v->size > 4 ? 4 : v->size;
    if (v->type == GL_UNSIGNED_BYTE) for (int k = 0; k < n; k++) out[k] = e[k] / 255.0f;
    else for (int k = 0; k < n; k++) { float f; memcpy(&f, e + 4 * k, 4); out[k] = f; }
    return 1;
}

void glmc_EnableClientState(GLenum cap)
{
    switch (cap) {
    case GL_VERTEX_ARRAY:     g.vp.on = 1; break;
    case GL_COLOR_ARRAY:      g.cp.on = 1; break;
    case GL_TEXTURE_COORD_ARRAY: g.tp.on = 1; break;
    default: break;
    }
}
void glmc_DisableClientState(GLenum cap)
{
    switch (cap) {
    case GL_VERTEX_ARRAY:     g.vp.on = 0; break;
    case GL_COLOR_ARRAY:      g.cp.on = 0; break;
    case GL_TEXTURE_COORD_ARRAY: g.tp.on = 0; break;
    default: break;
    }
}
void glmc_VertexPointer(GLint size, GLenum type, GLsizei stride, const void *ptr)
{ vptr_set(&g.vp, size, type, stride, ptr); }
void glmc_ColorPointer(GLint size, GLenum type, GLsizei stride, const void *ptr)
{ vptr_set(&g.cp, size, type, stride, ptr); }
void glmc_TexCoordPointer(GLint size, GLenum type, GLsizei stride, const void *ptr)
{ vptr_set(&g.tp, size, type, stride, ptr); }

void glmc_GenBuffers(GLsizei n, GLuint *out)
{
    if (!out) return;
    for (GLsizei i = 0; i < n; i++) {
        int slot = -1;
        for (int j = 0; j < 64; j++)
            if (!g.vbov[j].live) { slot = j; break; }
        if (slot < 0) { g.error = GL_OUT_OF_MEMORY; out[i] = 0; continue; }
        memset(&g.vbov[slot], 0, sizeof g.vbov[slot]);
        g.vbov[slot].live = 1;
        out[i] = (GLuint)(slot + 1);
    }
}
void glmc_DeleteBuffers(GLsizei n, const GLuint *names)
{
    for (GLsizei i = 0; i < n; i++) {
        GLuint b = names[i];
        if (b < 1 || b > 64) continue;
        free(g.vbov[b - 1].bytes);
        memset(&g.vbov[b - 1], 0, sizeof g.vbov[b - 1]);
        if (g.vbo_bind == b) g.vbo_bind = 0;
    }
}
void glmc_BindBuffer(GLenum target, GLuint name)
{ g.vbo_bind = (target == GL_ARRAY_BUFFER) ? name : 0; }
void glmc_BufferData(GLenum target, GLsizeiptr len, const void *data, GLenum usage)
{
    (void)usage;
    if (target != GL_ARRAY_BUFFER) { glmc_dev_note("buffer target other than GL_ARRAY_BUFFER"); return; }
    if (g.vbo_bind < 1 || (size_t)g.vbo_bind > sizeof g.vbov / sizeof g.vbov[0]) { g.error = GL_INVALID_VALUE; return; }
    struct vbo *b = &g.vbov[g.vbo_bind - 1];
    free(b->bytes); b->bytes = NULL;
    b->len = 0;
    if (len > 0) {
        b->bytes = malloc((size_t)len);
        if (b->bytes) {
            if (data) memcpy(b->bytes, data, (size_t)len);
            else memset(b->bytes, 0, (size_t)len);   /* the orphan trick */
            b->len = (size_t)len;
        }
    }
}

/* the tc2 attribute slots 0..4 (BindAttribLocation is a no-op: both sides
 * keep the attribute order fixed)                                         */
void glmc_EnableVertexAttribArray(GLuint i)
{ if (i < 8) g.ap[i].on = 1; }
void glmc_DisableVertexAttribArray(GLuint i)
{ if (i < 8) g.ap[i].on = 0; }
void glmc_VertexAttribPointer(GLuint i, GLint size, GLenum type, GLboolean norm,
                              GLsizei stride, const void *ptr)
{
    (void)norm;
    if (i >= 8) return;
    vptr_set(&g.ap[i], size, type, stride, ptr);
}

/* ---- immediate mode ---------------------------------------------------- */
static void batch_reserve(void)
{
    if ((size_t)(g.bvert + 1) * 3 > g.bcap) {
        size_t want = g.bcap ? g.bcap * 2 : 4096 * 3;
        g.batch = realloc(g.batch, want * sizeof(float));
        g.bcap = want;
    }
    if ((size_t)(g.bvert + 1) * 4 > g.bcol_cap) {
        size_t want = g.bcol_cap ? g.bcol_cap * 2 : 4096 * 4;
        g.bcolv = realloc(g.bcolv, want * sizeof(float));
        g.bcol_cap = want;
    }
    if ((size_t)(g.bvert + 1) * 4 > g.btex_cap) {
        size_t want = g.btex_cap ? g.btex_cap * 2 : 4096 * 4;
        g.btexv = realloc(g.btexv, want * sizeof(float));
        g.btex_cap = want;
    }
}

void glmc_Begin(GLenum mode)
{
    g.bmode = (int)mode; g.bvert = 0;
    g.bhas_col = 0; g.bhas_tex = 0;
    /* current colour / texcoord persist across Begin, as in GL               */
}
void glmc_End(void);
void glmc_Color3f(float r, float g_, float b) { g.bhas_col = 1; g.bhas_a = 1; g.bcol[0]=r; g.bcol[1]=g_; g.bcol[2]=b; g.bcol[3]=1.0f; }
void glmc_Color4f(float r, float g_, float b, float a)
{ g.bhas_col = 1; g.bhas_a = 1; g.bcol[0]=r; g.bcol[1]=g_; g.bcol[2]=b; g.bcol[3]=a; }
void glmc_TexCoord2f(GLfloat s, GLfloat t) { glmc_TexCoord4f(s, t, 0.0f, 1.0f); }
void glmc_TexCoord3f(GLfloat s, GLfloat t, GLfloat r) { glmc_TexCoord4f(s, t, r, 1.0f); }
void glmc_TexCoord4f(GLfloat s, GLfloat t, GLfloat r, GLfloat q)
{ g.bhas_tex = 1; g.btex[0]=s; g.btex[1]=t; g.btex[2]=r; g.btex[3]=q; }

void glmc_Vertex2f(float x, float y) { glmc_Vertex3f(x, y, 0.0f); }
void glmc_Vertex3f(float x, float y, float z) { glmc_Vertex4f(x, y, z, 1.0f); }
void glmc_Vertex4f(float x, float y, float z, float w)
{
    (void)w;
    batch_reserve();
    float *pos = g.batch + 3 * (size_t)g.bvert;
    pos[0] = x; pos[1] = y;
    pos[2] = (g.bmode == GL_LINES || g.bmode == GL_LINE_STRIP ||
              g.bmode == GL_LINE_LOOP) ? 0.0f : z;
    float *c = g.bcolv + 4 * (size_t)g.bvert;
    c[0] = g.bcol[0]; c[1] = g.bcol[1]; c[2] = g.bcol[2]; c[3] = g.bcol[3];
    float *tv = g.btexv + 4 * (size_t)g.bvert;
    tv[0] = g.btex[0]; tv[1] = g.btex[1]; tv[2] = g.btex[2]; tv[3] = g.btex[3];
    g.bvert++;
}

/* ---------------------------------------------------------------- drawing -- */
/* the canonical FFP vertex: [F4 pos @0 (w=1), F4 colour @16, F4 texcoord @32],
 * 48-byte stride -- float4 attributes must sit at 16-byte offsets on Metal. */
#define FFP_VSTRIDE 48

static uint8_t *glmcd_ffbuf;
static size_t glmcd_ffcap;

/* second scratch for topology reorders (glmc_End): the primary buffer
 * cannot expand in place because reads run ahead of writes              */
static uint8_t *glmcd_ffre;
static size_t glmcd_ffrecap;

static uint8_t *glmcd_ffbuf_get(size_t vertices)
{
    size_t need = vertices * FFP_VSTRIDE;
    if (need > glmcd_ffcap) {
        glmcd_ffcap = need < 65536 ? 65536 : need;
        glmcd_ffbuf = realloc(glmcd_ffbuf, glmcd_ffcap);
    }
    return glmcd_ffbuf;
}

static uint8_t *glmcd_ffre_get(size_t vertices)
{
    size_t need = vertices * FFP_VSTRIDE;
    if (need > glmcd_ffrecap) {
        glmcd_ffrecap = need < 65536 ? 65536 : need;
        glmcd_ffre = realloc(glmcd_ffre, glmcd_ffrecap);
    }
    return glmcd_ffre;
}

/* generic scratch for the tc2 repack etc. */
static uint8_t *glmcd_vre;
static size_t glmcd_vrecap;
static uint8_t *glmcd_vre_get(size_t bytes)
{
    if (bytes > glmcd_vrecap) {
        glmcd_vrecap = bytes < 65536 ? 65536 : bytes;
        glmcd_vre = realloc(glmcd_vre, glmcd_vrecap);
    }
    return glmcd_vre;
}

/* interleave client arrays (or the immediate batch) into the FFP layout,
 * expanding the topology; returns the expanded vertex count. src_* describe
 * per-vertex float reads; a NULL pointer set falls back to the batch.      */
static int glmcd_ffp_build(GLenum mode, int count,
                           const float *pos /* 2 or 3 comp */, int psz,
                           const float *col,     /* 4 comp */
                           const float *tex,     /* 4 comp */
                           uint8_t **out)
{
    (void)mode;
    uint8_t *o = glmcd_ffbuf_get((size_t)count);
    if (!o) return 0;
    for (int i = 0; i < count; i++) {
        float *v = (float *)(o + (size_t)i * FFP_VSTRIDE);
        v[0] = pos ? pos[(size_t)i * psz + 0] : 0.0f;
        v[1] = pos ? pos[(size_t)i * psz + 1] : 0.0f;
        v[2] = (psz >= 3 && pos) ? pos[(size_t)i * psz + 2] : 0.0f;
        v[3] = 1.0f;                        /* Metal reads the position as float4 */
        const float *c = col ? col + (size_t)i * 4 : NULL;
        v[4]  = c ? c[0] : 1.0f;
        v[5]  = c ? c[1] : 1.0f;
        v[6]  = c ? c[2] : 1.0f;
        v[7]  = c ? c[3] : 1.0f;
        const float *t = tex ? tex + (size_t)i * 4 : NULL;
        v[8]  = t ? t[0] : 0.0f;
        v[9]  = t ? t[1] : 0.0f;
        v[10] = t ? t[2] : 0.0f;
        v[11] = t ? t[3] : 1.0f;
    }
    *out = o;
    return count;
}

/* the per-draw pipekey, assembled from the live GL state                   */
static void glmcd_pk(glmcd_pipekey *pk, int prog, int topo, int fragdepth)
{
    memset(pk, 0, sizeof *pk);
    pk->prog = (uint16_t)prog;
    pk->topo = (uint16_t)topo;
    pk->blend.on = g.blend_on;
    pk->blend.f_srgb = g.bf_s; pk->blend.f_drgb = g.bf_d;
    pk->blend.f_salpha = g.bf_sa; pk->blend.f_dalpha = g.bf_da;
    pk->blend.eq_rgb = g.eq_rgb; pk->blend.eq_a = g.eq_a;
    pk->depth_on = g.depth_on ? 1 : 0;
    pk->depth_func = (uint16_t)g.depth_func;
    pk->depth_write = g.depth_write ? 1 : 0;
    pk->af_on = g.alpha_on ? 1 : 0;
    pk->af_func = (uint8_t)g.af_code;
    pk->mask_r = g.mask_r; pk->mask_g = g.mask_g;
    pk->mask_b = g.mask_b; pk->mask_a = g.mask_a;
    pk->scissor = 1;                 /* the face reads the live scissor state */
    pk->fragdepth = fragdepth ? 1 : 0;
}

static void glmcd_samp_node2(glmcd_samp *s, struct ctex *t)
{
    memset(s, 0, sizeof *s);
    if (!t || !t->live || !t->id) return;
    s->tex = t->id;
    s->minf = t->minf; s->magf = t->magf;
    s->wraps = t->ws; s->wrapt = t->wt;
}

/* forward the live viewport / scissor into the face for the next draw     */
static void glmcd_vps_forward(int fbo_w, int fbo_h)
{
    if (!g.face) return;
    /* an unset viewport falls back to the whole target (the games always
     * call glViewport before drawing; this is the headless/gate probe path) */
    int vx = g.vmaxx, vy = g.vmaxy, vw = g.vww, vh = g.vwh;
    if (vw <= 0 || vh <= 0) { vx = vy = 0; vw = fbo_w; vh = fbo_h; }
    glmc_dev_viewport(g.face, vx, vy, vw, vh, fbo_w, fbo_h);
    glmc_dev_scissor(g.face, g.sc_on, g.scx, g.scy, g.scw, g.sch, fbo_w, fbo_h);
}

/* the actual submission: everything inside glmc_dev_draw's contract        */
static void glmcd_submit(int prog, int topo, int fragdepth,
                         const void *uni, int ulen,
                         const void *verts, int vert_bytes,
                         const glmcd_samp *smp, int ns,
                         int first, int count)
{
    if (!g.face || count <= 0 || prog < 0) return;
    glmcd_pipekey pk;
    glmcd_pk(&pk, prog, topo, fragdepth);
    int fw = 0, fh = 0;
    glmc_dev_fbo_size(g.face, (uint32_t)g.fbo_cur, &fw, &fh);
    if (fw < 1 || fh < 1) { glmc_dev_window_size(g.face, &fw, &fh); }
    glmcd_vps_forward(fw, fh);
    glmcd_drawreq rq;
    memset(&rq, 0, sizeof rq);
    rq.pk = &pk;
    rq.uniforms = uni; rq.uniform_len = ulen;
    rq.verts = verts; rq.vert_bytes = vert_bytes;
    rq.first = first; rq.count = count;
    if (ns > 8) ns = 8;
    memcpy(rq.samp, smp, (size_t)ns * sizeof(glmcd_samp));
    rq.nsamp = ns;
    glmc_dev_draw(g.face, &rq);
}

/* -------- the tc2 uniform structs, rebuilt from the stored slots -------- */
static int glmcd_slot(struct tprog *p, const char *name)
{
    for (int i = 0; i < p->nu; i++)
        if (!strcmp(p->nm[i], name)) return i;
    return -1;
}
static int glmcd_slot_iv(struct tprog *p, const char *name, int dflt)
{
    int k = glmcd_slot(p, name);
    return (k >= 0) ? p->iv[k][0] : dflt;
}
static const int *glmcd_slot_3i(struct tprog *p, const char *name)
{
    static int dflt3[3];
    int k = glmcd_slot(p, name);
    return (k >= 0) ? p->iv[k] : dflt3;
}

static void glmcd_tc2poly_uni(struct tprog *p, glmcd_tc2poly_u *u)
{
    memset(u, 0, sizeof *u);
    u->ex = glmcd_slot_iv(p, "u_ex", 0);
    u->scale = glmcd_slot_iv(p, "u_scale", 1);
    u->h = glmcd_slot_iv(p, "u_h", 0);
    u->prio = glmcd_slot_iv(p, "u_prio", 0);
    u->pfade = glmcd_slot_iv(p, "u_pfade", 0);
    u->ff = glmcd_slot_iv(p, "u_ff", 255);
    u->alpha = glmcd_slot_iv(p, "u_alpha", 255);
    u->alpha_pen = glmcd_slot_iv(p, "u_alpha_pen", 0);
    u->near_on = glmcd_slot_iv(p, "u_near", 0);
    u->depth_on = glmcd_slot_iv(p, "u_depth", 0);
    const int *pc = glmcd_slot_3i(p, "u_pcol");
    u->pcol[0] = pc[0]; u->pcol[1] = pc[1]; u->pcol[2] = pc[2];
    const int *fc = glmcd_slot_3i(p, "u_fcol");
    u->fcol[0] = fc[0]; u->fcol[1] = fc[1]; u->fcol[2] = fc[2];
    u->keep = (glmcd_slot(p, "u_keep") >= 0 && p->keepf) ? p->fv[glmcd_slot(p, "u_keep")] : 1.0f;
}

static void glmcd_tc2text_uni(struct tprog *p, glmcd_tc2text_u *u)
{
    memset(u, 0, sizeof *u);
    u->ex = glmcd_slot_iv(p, "u_ex", 0);
    u->scale = glmcd_slot_iv(p, "u_scale", 1);
    u->h = glmcd_slot_iv(p, "u_h", 0);
    u->pass = glmcd_slot_iv(p, "u_pass", 0);
    u->fade = glmcd_slot_iv(p, "u_fade", 0);
    u->ff = glmcd_slot_iv(p, "u_ff", 255);
    u->alpha = glmcd_slot_iv(p, "u_alpha", 255);
    u->amask = glmcd_slot_iv(p, "u_amask", 0);
    u->c12 = glmcd_slot_iv(p, "u_c12", 0);
    u->c13 = glmcd_slot_iv(p, "u_c13", 0);
    const int *fc = glmcd_slot_3i(p, "u_fcol");
    u->fcol[0] = fc[0]; u->fcol[1] = fc[1]; u->fcol[2] = fc[2];
}

/* -------- the GL draw calls the games use ------------------------------- */

void glmc_End(void)
{
    if (!g.bvert) return;
    if (g.prog < 0) { g.bvert = 0; return; }        /* an unknown program     */

    if (g.prog == PR_TC2_TEXT || g.prog == PR_TC2_GAMMA) {
        /* tc2's full-screen quads: the 16-byte a_pos layout, already in
         * g.batch as 3 floats; pad to 4 (w). The vertex shaders take NDC. */
        int n = g.bvert;
        int vcap = n * 4;
        uint8_t *o = glmcd_ffbuf_get((size_t)(vcap * 4) / 4 * 0 + (size_t)vcap * 4);
        if (!o) { g.bvert = 0; return; }
        for (int i = 0; i < n; i++) {
            float *v = (float *)(o + (size_t)i * 16);
            v[0] = g.batch[(size_t)i * 3 + 0];
            v[1] = g.batch[(size_t)i * 3 + 1];
            v[2] = g.batch[(size_t)i * 3 + 2];
            v[3] = 1.0f;
        }
        struct tprog *p = &g.progt[g.prog_raw - 1];
        glmcd_samp smp[4];
        if (g.prog == PR_TC2_TEXT) {
            static const char *tn[4] = { "t_mix", "t_pal", "t_prio", "t_band" };
            for (int k = 0; k < 4; k++) {
                int slot = glmcd_slot(p, tn[k]);
                int unit = (slot >= 0) ? p->iv[slot][0] : k;
                struct ctex *t = tex_obj(g.bind[unit & 7]);
                glmcd_samp_node2(&smp[k], t);
            }
            glmcd_tc2text_u mu;
            glmcd_tc2text_uni(p, &mu);
            glmcd_submit(g.prog, GLMC_TOPO_TRI, 0, &mu, sizeof mu,
                         o, n * 16, smp, 4, 0, n);
        } else {
            static const char *gn[2] = { "t_src", "t_lut" };
            for (int k = 0; k < 2; k++) {
                int slot = glmcd_slot(p, gn[k]);
                int unit = (slot >= 0) ? p->iv[slot][0] : k;
                struct ctex *t = tex_obj(g.bind[unit & 7]);
                glmcd_samp_node2(&smp[k], t);
            }
            glmcd_tc2gamma_u mu = { 0 };
            glmcd_submit(g.prog, GLMC_TOPO_TRI, 0, &mu, sizeof mu,
                         o, n * 16, smp, 2, 0, n);
        }
        g.bvert = 0;
        return;
    }

    /* FFP / POST1: interleave the batch, expand the topology, draw         */
    int count = g.bvert;
    int topo = GLMC_TOPO_TRI;
    int nout = 0;
    uint8_t *base = glmcd_ffbuf_get((size_t)count);
    if (!base) { g.bvert = 0; return; }
    /* 3-float positions; the batch keeps GL positions (r/q of the tex 4)   */
    nout = glmcd_ffp_build(GL_TRIANGLES, count, g.batch, 3, g.bcolv, g.btexv, &base);
    if (getenv("GLMC_TRACE"))
        fprintf(stderr, "[trace] end: colv=%p texv=%p nout=%d bcol=%g %g %g %g\n",
                (void *)g.bcolv, (void *)g.btexv, nout,
                g.bcol[0], g.bcol[1], g.bcol[2], g.bcol[3]);
    /* expanded topology lives in the reorder buffer, which is sized for the
     * EXPANDED count -- never copy it back over the smaller `base`. */
    uint8_t *outbuf = base;
    if (g.bmode == GL_LINES || g.bmode == GL_LINE_STRIP || g.bmode == GL_LINE_LOOP || g.bmode == GL_QUADS) {
        int cap2;
        if (g.bmode == GL_QUADS)      cap2 = (count / 4) * 6;
        else if (g.bmode == GL_LINES) cap2 = count;
        else if (g.bmode == GL_LINE_STRIP) cap2 = (count - 1) * 2;
        else                          cap2 = count * 2;   /* GL_LINE_LOOP */
        uint8_t *o2 = glmcd_ffre_get((size_t)(cap2 > count ? cap2 : count));
        if (!o2) { g.bvert = 0; return; }
        int m = 0;
        if (g.bmode == GL_QUADS) {
            for (int q = 0; q + 3 < count; q += 4)
                for (int t = 0; t < 2; t++)
                    for (int k = 0; k < 3; k++) {
                        int s = q + (t == 0 ? k : (k == 0 ? 0 : k + 1));
                        memcpy(o2 + (size_t)m * FFP_VSTRIDE, base + (size_t)s * FFP_VSTRIDE, FFP_VSTRIDE);
                        m++;
                    }
            nout = m;
        } else if (g.bmode == GL_LINES) {
            nout = count; topo = GLMC_TOPO_LINE_LIST;
            memcpy(o2, base, (size_t)count * FFP_VSTRIDE);
        } else if (g.bmode == GL_LINE_STRIP) {
            for (int i = 0, k = 0; i + 1 < count; i++, k++) {
                memcpy(o2 + (size_t)k * 2 * FFP_VSTRIDE, base + (size_t)i * FFP_VSTRIDE, FFP_VSTRIDE);
                memcpy(o2 + (size_t)(k * 2 + 1) * FFP_VSTRIDE, base + (size_t)(i + 1) * FFP_VSTRIDE, FFP_VSTRIDE);
            }
            nout = (count - 1) * 2; topo = GLMC_TOPO_LINE_LIST;
        } else { /* GL_LINE_LOOP */
            for (int i = 0, k = 0; i < count; i++, k++) {
                memcpy(o2 + (size_t)(k * 2) * FFP_VSTRIDE, base + (size_t)i * FFP_VSTRIDE, FFP_VSTRIDE);
                memcpy(o2 + (size_t)(k * 2 + 1) * FFP_VSTRIDE,
                       base + (size_t)((i + 1) % count) * FFP_VSTRIDE, FFP_VSTRIDE);
            }
            nout = count * 2; topo = GLMC_TOPO_LINE_LIST;
        }
        outbuf = o2;
    }
    glmcd_ffp_u fu;
    glmcd_fill_ffp(&fu);
    glmcd_samp smp[2];
    glmcd_samp_node2(&smp[0], tex_obj(g.bind[0]));
    glmcd_samp_node2(&smp[1], tex_obj(g.bind[1]));
    glmcd_submit(g.prog, topo, 0, &fu, sizeof fu, outbuf, nout * FFP_VSTRIDE, smp, 2, 0, nout);
    g.bvert = 0;
}

void glmc_DrawArrays(GLenum mode, GLint first, GLsizei count)
{
    if (count <= 0 || g.prog < 0) return;
    if (g.prog == PR_TC2_POLY) {
        /* tc2's one poly call: attributes 0..4 over the bound VBO. tc2 packs
         * 17 floats per vertex; Metal needs the float4 attributes at 16-byte
         * offsets, so repack into 20 floats: pos4, uvs4(+pad), par4, clip4,
         * ext4(+pad)                                                         */
        struct tprog *p = &g.progt[g.prog_raw - 1];
        struct vbo *b = (g.vbo_bind >= 1 && g.vbo_bind <= 64) ? &g.vbov[g.vbo_bind - 1] : NULL;
        if (!b || !b->bytes) { glmc_dev_note("tc2 poly draw without a buffer"); return; }
        if (first < 0 || (size_t)count * 17 > b->len / sizeof(float) - (size_t)first * 17) {
            glmc_dev_note("tc2 poly draw range overruns the buffer"); return;
        }
        const float *in = (const float *)b->bytes + (size_t)first * 17;
        size_t need = (size_t)count * 20 * sizeof(float);
        uint8_t *rb = glmcd_vre_get(need);
        if (!rb) return;
        float *out = (float *)rb;
        for (int i = 0; i < count; i++) {
            const float *s = in + (size_t)i * 17;
            float *d = out + (size_t)i * 20;
            d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3];
            d[4] = s[4]; d[5] = s[5]; d[6] = s[6]; d[7] = 0.0f;
            d[8] = s[7]; d[9] = s[8]; d[10] = s[9]; d[11] = s[10];
            d[12] = s[11]; d[13] = s[12]; d[14] = s[13]; d[15] = s[14];
            d[16] = s[15]; d[17] = s[16]; d[18] = 0.0f; d[19] = 0.0f;
        }
        glmcd_samp smp[6];
        static const char *pn[6] = { "t_rom", "t_map", "t_pal", "t_sten", "t_near", "t_nearz" };
        for (int k = 0; k < 6; k++) {
            int slot = glmcd_slot(p, pn[k]);
            int unit = (slot >= 0) ? p->iv[slot][0] : k;
            struct ctex *t = tex_obj(g.bind[unit & 7]);
            glmcd_samp_node2(&smp[k], t);
        }
        glmcd_tc2poly_u mu;
        glmcd_tc2poly_uni(p, &mu);
        glmcd_submit(g.prog, GLMC_TOPO_TRI, mu.near_on != 0, &mu, sizeof mu,
                     rb, (int)need, smp, 6, 0, count);
        return;
    }
    /* tc2 text/gamma quads arrive as TRIANGLES over quad_vbo               */
    if (g.prog == PR_TC2_TEXT || g.prog == PR_TC2_GAMMA) {
        struct tprog *p = &g.progt[g.prog_raw - 1];
        struct vbo *b = (g.vbo_bind >= 1 && g.vbo_bind <= 64) ? &g.vbov[g.vbo_bind - 1] : NULL;
        if (!b || !b->bytes) { glmc_dev_note("tc2 quad draw without a buffer"); return; }
        glmcd_samp smp2[4];
        glmcd_samp ns2[4];
        int n6 = g.prog == PR_TC2_TEXT ? 4 : 2;
        static const char *xn[4] = { "t_src", "t_lut", "t_prio", "t_band" };
        for (int k = 0; k < n6; k++) {
            int slot = g.prog == PR_TC2_TEXT
                ? glmcd_slot(p, (k == 0 ? "t_mix" : k == 1 ? "t_pal" : k == 2 ? "t_prio" : "t_band"))
                : glmcd_slot(p, (k == 0 ? "t_src" : "t_lut"));
            (void)xn;
            int unit = (slot >= 0) ? p->iv[slot][0] : k;
            struct ctex *t = tex_obj(g.bind[unit & 7]);
            glmcd_samp_node2(&smp2[k], t);
        }
        if (g.prog == PR_TC2_TEXT) {
            glmcd_tc2text_u mu;
            glmcd_tc2text_uni(p, &mu);
            glmcd_submit(g.prog, GLMC_TOPO_TRI, 0, &mu, sizeof mu,
                         b->bytes, (int)b->len, smp2, 4, first, count);
        } else {
            glmcd_tc2gamma_u mu = { 0 };
            glmcd_submit(g.prog, GLMC_TOPO_TRI, 0, &mu, sizeof mu,
                         b->bytes, (int)b->len, smp2, 2, first, count);
        }
        return;
    }

    /* FFP / POST1 over the client arrays                                   */
    if (g.vp.on && !g.vp.p) { g.error = GL_INVALID_OPERATION; return; }
    int psz = g.vp.size >= 3 ? 3 : 2;
    const float *pos = (const float *)g.vp.p;
    const float *col = (const float *)g.cp.p;
    const float *tex = (const float *)g.tp.p;
    int topo = GLMC_TOPO_TRI;
    int nexp = count;
    (void)mode;
    if (mode == GL_LINES || mode == GL_LINE_STRIP || mode == GL_LINE_LOOP) topo = GLMC_TOPO_LINE_LIST;
    uint8_t *base;
    int n = glmcd_ffp_build(GL_TRIANGLES, count, pos, psz, col, tex, &base);
    /* tc1's depth path passes x, y, z: keep the pointer's 3rd component    */
    if (!g.bhas_tex && !g.tp.on) { }
    glmcd_ffp_u fu;
    glmcd_fill_ffp(&fu);
    glmcd_samp smp[2];
    glmcd_samp_node2(&smp[0], tex_obj(g.bind[0]));
    glmcd_samp_node2(&smp[1], tex_obj(g.bind[1]));
    glmcd_submit(g.prog, topo, 0, &fu, sizeof fu,
                 base, n * FFP_VSTRIDE, smp, 2, first, n);
}

void glmc_DrawElements(GLenum mode, GLsizei count, GLenum type, const void *idx)
{
    if (!idx || type != GL_UNSIGNED_SHORT || count <= 0) {
        if (count > 0) glmc_dev_note("unsupported DrawElements shape");
        return;
    }
    /* expand the 16-bit indices into the FFP layout, reading each client
     * array through its own stride and type (the menu's Nuklear vertices are
     * interleaved: 2 float position, 2 float uv, 4 unsigned-byte colour)    */
    const uint16_t *ix = (const uint16_t *)idx;
    uint8_t *o = glmcd_ffbuf_get((size_t)count);
    if (!o) return;
    for (int i = 0; i < count; i++) {
        const int s = ix[i];
        float v[12] = { 0, 0, 0, 1,  1, 1, 1, 1,  0, 0, 0, 1 };      /* the FFP vertex: pos4 @0, colour4 @16, texcoord4 @32 */
        vptr_fetch(&g.vp, s, &v[0]);
        if (!vptr_fetch(&g.cp, s, &v[4])) memcpy(&v[4], g.bcol, sizeof(float) * 4);   /* no colour array: the current colour */
        vptr_fetch(&g.tp, s, &v[8]);
        memcpy(o + (size_t)i * FFP_VSTRIDE, v, FFP_VSTRIDE);
    }
    int topo = GLMC_TOPO_TRI;
    if (mode == GL_LINES) topo = GLMC_TOPO_LINE_LIST;
    glmcd_ffp_u fu;
    glmcd_fill_ffp(&fu);
    glmcd_samp smp[2];
    glmcd_samp_node2(&smp[0], tex_obj(g.bind[0]));
    glmcd_samp_node2(&smp[1], tex_obj(g.bind[1]));
    glmcd_submit(g.prog, topo, 0, &fu, sizeof fu, o, count * FFP_VSTRIDE, smp, 2, 0, count);
}

/* CHUNKE — the framebuffer / pixel-path / context state the first chunk was
 * cut short on. GL semantics throughout: this file owns the names, the face
 * does the GPU work.                                                       */

int g_read_fbo(void) { return g.read_fbo; }

/* ---------------------------------------------------------- clear state -- */
void glmc_ClearColor(GLclampf r, GLclampf gc, GLclampf b, GLclampf a)
{ g.clear_r = r; g.clear_g = gc; g.clear_b = b; g.clear_a = a; }

void glmc_ClearDepth(GLdouble d) { g.clear_depth = d; }

void glmc_Clear(GLbitfield mask)
{
    if (!g.face) return;
    int m = 0;
    if (mask & GL_COLOR_BUFFER_BIT) m |= GLMC_CLR_COLOR;
    if (mask & GL_DEPTH_BUFFER_BIT) m |= GLMC_CLR_DEPTH;
    glmc_dev_clear(g.face, m, g.clear_r, g.clear_g, g.clear_b, g.clear_a, g.clear_depth);
}

void glmc_Viewport(GLint x, GLint y, GLsizei w, GLsizei h)
{ g.vmaxx = x; g.vmaxy = y; g.vww = w; g.vwh = h; }

void glmc_Scissor(GLint x, GLint y, GLsizei w, GLsizei h)
{ g.scx = x; g.scy = y; g.scw = w; g.sch = h; }               /* the box only: enable is glmc_Enable's */

void glmc_ReadBuffer(GLenum m) { (void)m; }        /* GL_BACK only: the default */
void glmc_DrawBuffer(GLenum m) { (void)m; }

/* -------------------------------------------------------- renderbuffers -- */
static struct { int live, w, h, px; uint32_t tex; } rb[256];
static int rb_bind;

void glmc_GenRenderbuffers(GLsizei n, GLuint *out)
{
    for (GLsizei i = 0; i < n; i++) {
        int slot = -1;
        for (int j = 0; j < 256; j++) if (!rb[j].live) { slot = j; break; }
        if (slot < 0) { g.error = GL_OUT_OF_MEMORY; out[i] = 0; continue; }
        memset(&rb[slot], 0, sizeof rb[slot]);
        rb[slot].live = 1;
        out[i] = (GLuint)(slot + 1);
    }
}

void glmc_DeleteRenderbuffers(GLsizei n, const GLuint *names)
{
    for (GLsizei i = 0; i < n; i++) {
        int k = (int)names[i];
        if (k < 1 || k > 256 || !rb[k - 1].live) continue;
        if (g.face && rb[k - 1].tex) glmc_tex_del(g.face, rb[k - 1].tex);
        memset(&rb[k - 1], 0, sizeof rb[k - 1]);
        if (rb_bind == k) rb_bind = 0;
    }
}

void glmc_BindRenderbuffer(GLenum target, GLuint name)
{
    if (target != GL_RENDERBUFFER) { glmc_dev_note("bind of a non-renderbuffer target"); return; }
    rb_bind = (int)name;
}

void glmc_RenderbufferStorage(GLenum target, GLenum internal, GLsizei w, GLsizei h)
{
    (void)target;                                     /* GL_RENDERBUFFER bound */
    if (rb_bind < 1 || rb_bind > 256 || !rb[rb_bind - 1].live) { g.error = GL_INVALID_OPERATION; return; }
    int ri = rb_bind - 1;
    if (!g.face) { glmc_dev_note("renderbuffer storage without a face"); return; }
    if (!rb[ri].tex) rb[ri].tex = glmc_tex_new(g.face);
    rb[ri].px = (internal == GL_DEPTH_COMPONENT24 || internal == GL_DEPTH_COMPONENT32F ||
             internal == GL_DEPTH_COMPONENT16) ? GLMC_PX_DEPTH32F
            : px_from_internal(internal, GL_RGBA, GL_UNSIGNED_BYTE);
    rb[ri].px = internal == GL_RGBA8 ? GLMC_PX_RGBA8 : rb[ri].px;
    glmc_tex_storage(g.face, rb[ri].tex, rb[ri].px, w, h);
    rb[ri].w = w; rb[ri].h = h;
}

/* --------------------------------------------------------- framebuffers -- */
void glmc_GenFramebuffers(GLsizei n, GLuint *out)
{
    for (GLsizei i = 0; i < n; i++) out[i] = glmc_fbo_new();
}

void glmc_DeleteFramebuffers(GLsizei n, const GLuint *names)
{
    for (GLsizei i = 0; i < n; i++) {
        uint32_t k = names[i];
        if (k < 1) continue;
        for (int u = 0; u < TU_MAX; u++) { (void)u; }
        if ((uint32_t)g.fbo_cur == k) glmc_BindFramebuffer(GL_FRAMEBUFFER, 0);
        if ((uint32_t)g.read_fbo == k && k != (uint32_t)g.fbo_cur) { g.read_fbo = 0; if (g.face) glmc_dev_read_fbo(g.face, 0); }
        glmc_fbo_del(k);
    }
}

void glmc_BindFramebuffer(GLenum target, GLuint name)
{
    if (!g.face) { g.fbo_cur = g.read_fbo = (int)name; return; }
    switch (target) {
    case GL_DRAW_FRAMEBUFFER:
        g.fbo_cur = (int)name;
        glmc_dev_fbo(g.face, name);
        break;
    case GL_READ_FRAMEBUFFER:
        g.read_fbo = (int)name;
        glmc_dev_read_fbo(g.face, name);
        break;
    default:      /* GL_FRAMEBUFFER: both, as the spec binds them together */
        g.fbo_cur = (int)name;
        glmc_dev_fbo(g.face, name);
        g.read_fbo = (int)name;
        glmc_dev_read_fbo(g.face, name);
        break;
    }
}

void glmc_FramebufferTexture2D(GLenum target, GLenum attachment, GLenum textgt,
                               GLuint tex, GLint level)
{
    (void)target; (void)textgt; (void)level;
    int slot = (attachment == GL_DEPTH_ATTACHMENT) ? 1
             : (attachment == GL_COLOR_ATTACHMENT0 || attachment == GL_COLOR_ATTACHMENT1) ? 0 : -1;
    if (slot < 0) { glmc_dev_note("unknown framebuffer attach slot"); return; }
    uint32_t tid = 0;
    if (tex) {
        struct ctex *t = tex_obj(tex);
        if (!t || !t->live || !t->id) { glmc_dev_note("attach of a texture without device storage"); return; }
        tid = t->id;
    }
    glmc_fbo_attach((uint32_t)g.fbo_cur, slot, tid);
}

void glmc_FramebufferRenderbuffer(GLenum target, GLenum attachment, GLenum rbtgt, GLuint rbo)
{
    (void)target; (void)rbtgt;
    int slot = (attachment == GL_DEPTH_ATTACHMENT) ? 1
             : (attachment == GL_COLOR_ATTACHMENT0) ? 0 : -1;
    if (slot < 0) { glmc_dev_note("unknown renderbuffer attach slot"); return; }
    uint32_t tid = 0;
    if (rbo >= 1 && rbo <= 256 && rb[rbo - 1].live) tid = rb[rbo - 1].tex;
    glmc_fbo_attach((uint32_t)g.fbo_cur, slot, tid);
}

GLenum glmc_CheckFramebufferStatus(GLenum target)
{
    (void)target;
    if (glmc_fbo_status((uint32_t)g.fbo_cur) == 0) return GL_FRAMEBUFFER_COMPLETE;
    return GL_FRAMEBUFFER_INCOMPLETE;
}

/* ------------------------------------------------------- pixel movement -- */
void glmc_ReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum fmt, GLenum type, void *data)
{
    if (!g.face || !data) return;
    if (type != GL_UNSIGNED_BYTE) { glmc_dev_note("ReadPixels with a non-byte type"); return; }
    if (g.pack_rowlen > 0) { glmc_dev_note("ReadPixels PACK_ROW_LENGTH"); return; }
    int out_fmt = GLMC_PX_RGBA8;
    if (fmt == GL_RGB) out_fmt = GLMC_PX_RGB8;
    else if (fmt == GL_RGBA || fmt == GL_BGRA) out_fmt = GLMC_PX_RGBA8;
    else if (fmt == GL_LUMINANCE) out_fmt = GLMC_PX_R8;
    else { glmc_dev_note("ReadPixels of an unlisted format"); return; }
    glmc_dev_read(g.face, x, y, w, h, data, out_fmt);
}

void glmc_CopyTexImage2D(GLenum target, GLint level, GLenum internal, GLint x, GLint y,
                         GLsizei w, GLsizei h, GLint border)
{
    (void)level; (void)border;
    if (target != GL_TEXTURE_2D) return;
    struct ctex *t = tex_obj(g.bind[g.act]);
    if (!t || !t->live || !g.face) { glmc_dev_note("copyteximage without a bound object"); return; }
    int px = px_from_internal(internal, GL_RGBA, GL_UNSIGNED_BYTE);
    if (!t->id) t->id = glmc_tex_new(g.face);
    glmc_tex_storage(g.face, t->id, px, w, h);
    t->w = w; t->h = h; t->px = px;
    glmc_dev_copy_tex(g.face, t->id, px, 0, 0, x, y, w, h);
}

void glmc_CopyTexSubImage2D(GLenum target, GLint level, GLint xo, GLint yo,
                            GLint x, GLint y, GLsizei w, GLsizei h)
{
    (void)level;
    if (target != GL_TEXTURE_2D) return;
    struct ctex *t = tex_obj(g.bind[g.act]);
    if (!t || !t->live || !t->id || !g.face) { glmc_dev_note("copytexsub without storage"); return; }
    glmc_dev_copy_tex(g.face, t->id, t->px, xo, yo, x, y, w, h);
}

void glmc_BlitFramebuffer(GLint sx0, GLint sy0, GLint sx1, GLint sy1,
                          GLint dx0, GLint dy0, GLint dx1, GLint dy1,
                          GLbitfield mask, GLenum filt)
{
    (void)mask;                       /* colour only; depth blits aren't used */
    if (!g.face) return;
    glmc_dev_blit(g.face, sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1, filt == GL_LINEAR);
}

/* ------------------------------------------------------- attribute stack -- */
void glmc_PushAttrib(GLbitfield mask)
{
    (void)mask;                       /* one combined snapshot, as upstream uses */
    if (g.natt >= 16) { g.error = GL_STACK_OVERFLOW; return; }
    struct attint *a = &g.att[g.natt++];
    a->depth_on = g.depth_on; a->depth_func = g.depth_func; a->depth_write = g.depth_write;
    a->blend_on = g.blend_on; a->bf_s = g.bf_s; a->bf_d = g.bf_d;
    a->bf_sa = g.bf_sa; a->bf_da = g.bf_da; a->eq_rgb = g.eq_rgb; a->eq_a = g.eq_a;
    a->alpha_on = g.alpha_on; a->af_code = g.af_code; a->af_ref = g.af_ref;
    a->sc_on = g.sc_on; a->scx = g.scx; a->scy = g.scy; a->scw = g.scw; a->sch = g.sch;
    a->mask_r = g.mask_r; a->mask_g = g.mask_g; a->mask_b = g.mask_b; a->mask_a = g.mask_a;
    a->act = g.act; a->prog_raw = g.prog_raw;
    memcpy(a->env, g.env, sizeof a->env);
}

void glmc_PopAttrib(void)
{
    if (g.natt <= 0) { g.error = GL_STACK_UNDERFLOW; return; }
    struct attint *a = &g.att[--g.natt];
    g.depth_on = a->depth_on; g.depth_func = a->depth_func; g.depth_write = a->depth_write;
    g.blend_on = a->blend_on; g.bf_s = a->bf_s; g.bf_d = a->bf_d;
    g.bf_sa = a->bf_sa; g.bf_da = a->bf_da; g.eq_rgb = a->eq_rgb; g.eq_a = a->eq_a;
    g.alpha_on = a->alpha_on; g.af_code = a->af_code; g.af_ref = a->af_ref;
    g.sc_on = a->sc_on; g.scx = a->scx; g.scy = a->scy; g.scw = a->scw; g.sch = a->sch;
    g.mask_r = a->mask_r; g.mask_g = a->mask_g; g.mask_b = a->mask_b; g.mask_a = a->mask_a;
    g.act = a->act;
    memcpy(g.env, a->env, sizeof g.env);
    g.prog_raw = a->prog_raw;
    struct tprog *p = pr_by_name(g.prog_raw);
    g.prog = (g.prog_raw >= 1 && p) ? p->mapped : PR_FFP;  /* 0 = fixed pipe  */
}

/* -------------------------------------------------------- flush, queries -- */
void glmc_Finish(void) { if (g.face) glmc_dev_flush(g.face); }
void glmc_Flush(void)  { if (g.face) glmc_dev_flush(g.face); }

const GLubyte *glmc_glGetStringi(GLenum name, GLuint idx)
{
    (void)name; (void)idx;
    return (const GLubyte *)"GLMC";
}

/* ============================ SDL shim ==================================== */
/* SDL_GL_* becomes Metal windowing: the "context" is a face bound to a
 * window; faces (and the drawable) live in the Metal device layer, and this
 * file keeps the window -> face record SDL calls through. */

static int ginit_done;
static void glmcd_ensure_init(void)
{
    if (!ginit_done) { ginit_done = 1; glmc_init(); }
}

static struct { void *win, *ctx; glmcd_face *face; } gctxs[8];

static glmcd_face *gctx_face(void *win, void *ctx)
{
    for (int i = 0; i < 8; i++)
        if (gctxs[i].face && ((ctx && gctxs[i].ctx == ctx) || (!ctx && gctxs[i].win == win)))
            return gctxs[i].face;
    return NULL;
}

int glmc_sdl_SetAttribute(GLenum a, int v) { (void)a; (void)v; return 0; }

int glmc_sdl_LoadLibrary(const char *path) { (void)path; return 0; }   /* the GPU is the device */
void glmc_sdl_UnloadLibrary(void) {}

static int g_pending_interval = 1;

void *glmc_sdl_CreateContext(void *win)     /* the SDL_GLContext token */
{
    glmcd_ensure_init();
    int lw = 0, lh = 0, dw = 0, dh = 0;
    if (win) {
        SDL_GetWindowSize((SDL_Window *)win, &lw, &lh);
        SDL_Metal_GetDrawableSize((SDL_Window *)win, &dw, &dh);
    }
    if (dw < 1 || dh < 1) { dw = lw ? lw : 640; dh = lh ? lh : 480; }
    else { lw = dw; lh = dh; }                     /* Retina: one drawable px per unit below */
    int slot = -1;
    for (int i = 0; i < 8; i++) if (!gctxs[i].face) { slot = i; break; }
    if (slot < 0) { glmc_dev_note("context table full"); return NULL; }
    float scale = lw > 0 ? (float)dw / (float)lw : 1.0f;
    glmcd_face *f = NULL;
    glmc_context_attach_window(win, NULL, NULL, dw, dh, scale);
    f = gctxs[slot].face = g.face;                 /* attach bound it */
    gctxs[slot].win = win;
    gctxs[slot].ctx = (void *)f;                   /* the SDL_GLContext token */
    glmc_dev_set_swap_interval(f, g_pending_interval);
    return (void *)f;
}

int glmc_sdl_MakeCurrent(void *win, void *ctx)
{
    glmcd_ensure_init();
    glmcd_face *f = gctx_face(win, ctx);
    if (!f && !ctx) { g.face = NULL; return 0; }
    if (!f) { g.error = GL_INVALID_OPERATION; return -1; }
    g.face = f;
    glmcd_state_reset();               /* a context switch is a fresh state */
    return 0;
}

void glmc_sdl_DeleteContext(void *ctx)
{
    if (!ctx) return;
    for (int i = 0; i < 8; i++)
        if (gctxs[i].ctx == ctx) {
            if (g.face == gctxs[i].face) { g.face = NULL; }
            if (gctxs[i].face) glmc_dev_face_free(gctxs[i].face);
            memset(&gctxs[i], 0, sizeof gctxs[i]);
            return;
        }
}

void *glmc_sdl_GetProcAddress(const char *name)
{
    if (!name) return NULL;
    /* the FBO/program names the games resolve at run time; everything else
     * already goes through the glmc_* remapping macros                     */
    if (!strcmp(name, "glCreateShader"))  return (void *)glmc_CreateShaderName;
    if (!strcmp(name, "glShaderSource"))  return (void *)glmc_ShaderSource;
    if (!strcmp(name, "glCompileShader")) return (void *)glmc_CompileShader;
    if (!strcmp(name, "glGetShaderiv"))   return (void *)glmc_GetShaderiv;
    if (!strcmp(name, "glGetShaderInfoLog")) return (void *)glmc_GetShaderInfoLog;
    if (!strcmp(name, "glCreateProgram")) return (void *)glmc_CreateProgramName;
    if (!strcmp(name, "glAttachShader"))  return (void *)glmc_AttachShader;
    if (!strcmp(name, "glBindAttribLocation")) return (void *)glmc_BindAttribLocation;
    if (!strcmp(name, "glLinkProgram"))   return (void *)glmc_LinkProgram;
    if (!strcmp(name, "glGetProgramiv"))  return (void *)glmc_GetProgramiv;
    if (!strcmp(name, "glGetProgramInfoLog")) return (void *)glmc_GetProgramInfoLog;
    if (!strcmp(name, "glUseProgram"))    return (void *)glmc_UseProgram;
    if (!strcmp(name, "glGetUniformLocation")) return (void *)glmc_GetUniformLocation;
    if (!strcmp(name, "glGetAttribLocation")) return (void *)glmc_GetAttribLocation;
    if (!strcmp(name, "glUniform1i"))     return (void *)glmc_Uniform1i;
    if (!strcmp(name, "glUniform3i"))     return (void *)glmc_Uniform3i;
    if (!strcmp(name, "glUniform4i"))     return (void *)glmc_Uniform4i;
    if (!strcmp(name, "glUniform1f"))     return (void *)glmc_Uniform1f;
    if (!strcmp(name, "glGenBuffers"))    return (void *)glmc_GenBuffers;
    if (!strcmp(name, "glDeleteBuffers")) return (void *)glmc_DeleteBuffers;
    if (!strcmp(name, "glBindBuffer"))    return (void *)glmc_BindBuffer;
    if (!strcmp(name, "glBufferData"))    return (void *)glmc_BufferData;
    if (!strcmp(name, "glVertexAttribPointer")) return (void *)glmc_VertexAttribPointer;
    if (!strcmp(name, "glEnableVertexAttribArray")) return (void *)glmc_EnableVertexAttribArray;
    if (!strcmp(name, "glDisableVertexAttribArray")) return (void *)glmc_DisableVertexAttribArray;
    if (!strcmp(name, "glGenFramebuffersEXT"))      return (void *)glmc_GenFramebuffers;
    if (!strcmp(name, "glGenFramebuffers"))         return (void *)glmc_GenFramebuffers;
    if (!strcmp(name, "glDeleteFramebuffersEXT"))   return (void *)glmc_DeleteFramebuffers;
    if (!strcmp(name, "glDeleteFramebuffers"))      return (void *)glmc_DeleteFramebuffers;
    if (!strcmp(name, "glBindFramebufferEXT"))      return (void *)glmc_BindFramebuffer;
    if (!strcmp(name, "glBindFramebuffer"))         return (void *)glmc_BindFramebuffer;
    if (!strcmp(name, "glGenRenderbuffersEXT"))     return (void *)glmc_GenRenderbuffers;
    if (!strcmp(name, "glGenRenderbuffers"))        return (void *)glmc_GenRenderbuffers;
    if (!strcmp(name, "glDeleteRenderbuffersEXT"))  return (void *)glmc_DeleteRenderbuffers;
    if (!strcmp(name, "glDeleteRenderbuffers"))     return (void *)glmc_DeleteRenderbuffers;
    if (!strcmp(name, "glBindRenderbufferEXT"))     return (void *)glmc_BindRenderbuffer;
    if (!strcmp(name, "glBindRenderbuffer"))        return (void *)glmc_BindRenderbuffer;
    if (!strcmp(name, "glRenderbufferStorageEXT"))  return (void *)glmc_RenderbufferStorage;
    if (!strcmp(name, "glRenderbufferStorage"))     return (void *)glmc_RenderbufferStorage;
    if (!strcmp(name, "glFramebufferRenderbufferEXT")) return (void *)glmc_FramebufferRenderbuffer;
    if (!strcmp(name, "glFramebufferRenderbuffer")) return (void *)glmc_FramebufferRenderbuffer;
    if (!strcmp(name, "glFramebufferTexture2DEXT")) return (void *)glmc_FramebufferTexture2D;
    if (!strcmp(name, "glFramebufferTexture2D"))    return (void *)glmc_FramebufferTexture2D;
    if (!strcmp(name, "glCheckFramebufferStatusEXT")) return (void *)glmc_CheckFramebufferStatus;
    if (!strcmp(name, "glCheckFramebufferStatus"))  return (void *)glmc_CheckFramebufferStatus;
    if (!strcmp(name, "glBlitFramebufferEXT"))      return (void *)glmc_BlitFramebuffer;
    if (!strcmp(name, "glBlitFramebuffer"))         return (void *)glmc_BlitFramebuffer;
    if (!strcmp(name, "glActiveTexture")) return (void *)glmc_ActiveTexture;
    if (!strcmp(name, "glBlendEquation")) return (void *)glmc_BlendEquation;
    if (!strcmp(name, "glBlendEquationSeparate")) return (void *)glmc_BlendEquationSeparate;
    if (!strcmp(name, "glBlendFuncSeparate")) return (void *)glmc_BlendFuncSeparate;
    return NULL;                       /* genuinely absent names fail loudly */
}

glmcd_face *glmc_dev_current(void) { return g.face; }   /* the bound face (tests) */

static void glmcd_resize_check(glmcd_face *f, void *win)
{
    if (win) glmc_dev_face_refit(f, win);
}

void glmc_sdl_SwapWindow(void *win)
{
    glmcd_face *f = gctx_face(win, NULL);
    if (!f) f = g.face;
    if (!f) return;
    glmcd_resize_check(f, win);
    glmc_dev_present(f);
}

int glmc_sdl_SetSwapInterval(int i)
{
    g_pending_interval = i;
    if (g.face) glmc_dev_set_swap_interval(g.face, i);
    return 0;
}

int glmc_sdl_GetSwapInterval(void)
{
    return g_pending_interval;         /* the face keeps its own sync flag */
}

void glmc_sdl_GetDrawableSize(void *win, int *w, int *h)
{
    int dw = 0, dh = 0;
    if (win) SDL_Metal_GetDrawableSize((SDL_Window *)win, &dw, &dh);
    if (w) *w = dw;
    if (h) *h = dh;
}



