/*
 * s21_video_gl.c -- Cyber Sled (System 21): the picture drawn through the SHARED GL engine at the render resolution
 * (cybsled/PLAN.md "Module G"). Two passes into the picture rectangle of the current framebuffer:
 *
 *  1. THE 3D: the visible frame's quads through engine/quad_gl.c (eng_draw_cfg.flat_quad + depth_test, the System 21 settings):
 *     untextured, one colour per quad, a GL_LESS depth test on the hardware's per-quad AVERAGE z (window depth = z / 65536, the
 *     depth buffer cleared to 0x8000 / 65536 like the board's z buffer), drawn in the DSPs' order so equal z keeps the first quad
 *     as the board does. The "colour" is not a colour: R,G = the quad's pen after the depth cue, B,A = its z, so every output
 *     pixel knows which polygon pen and which z the hardware's buffers would hold there.
 *     Placement: the board's rasteriser covers pixel (x, y) when xl(y) < x + 1 <= xr(y) with the edges taken at the row's TOP
 *     (namcos21_3d's renderscanline), i.e. it samples at (x + 1, y); GL samples at the centre (x + .5, y + .5) with
 *     left-inclusive edges. The projection moves the quads by (-0.5 + 1/128, +0.5 - 1/256) board pixels, which makes the two
 *     the same test except for edges passing within about that of a sample -- at ANY resolution, because it is a fixed offset in
 *     board coordinates. The y offset must not be smaller than 1/256: GL snaps vertices to 1/256 pixel, a smaller offset lands
 *     back ON the sample and the GPU's tie rule then drops every quad's top row (measured: 99.165 % -> 99.93 % identical).
 *     CS_GLEPS=ex,ey overrides them (the sweep is in PLAN.md Module G).
 *  2. THE MIXER: that buffer is copied to a texture and one GLSL 1.10 pass computes each output pixel: per board pixel the 2D
 *     says what shows with no polygon there and what happens to a polygon pixel (s21_video.c prepare_gl()); this pass applies
 *     it to the polygon pen and z found at the output pixel, looks the pen up in the palette, and samples the 2D SHARP-BILINEAR
 *     (the four board pixels around, each mixed against this output pixel's polygon, weights sharpened to a one-output-pixel
 *     transition): every board pixel comes out the same size at any scale, exact at 496x480 and at whole multiples.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <SDL2/SDL.h>
#include "eng_gl.h"
#include "geo_hw.h"
#include "quad_gl.h"
#include "s21_video_int.h"

#ifndef GL_FRAGMENT_SHADER
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_COMPILE_STATUS  0x8B81
#define GL_LINK_STATUS     0x8B82
#endif
#ifndef GL_TEXTURE2
#define GL_TEXTURE2 0x84C2
#endif
typedef char GLch;
typedef GLuint (APIENTRY *pfn_create_shader)(GLenum);
typedef void   (APIENTRY *pfn_shader_source)(GLuint, GLsizei, const GLch *const *, const GLint *);
typedef void   (APIENTRY *pfn_uint)(GLuint);
typedef void   (APIENTRY *pfn_get_iv)(GLuint, GLenum, GLint *);
typedef void   (APIENTRY *pfn_get_log)(GLuint, GLsizei, GLsizei *, GLch *);
typedef GLuint (APIENTRY *pfn_create_program)(void);
typedef void   (APIENTRY *pfn_attach)(GLuint, GLuint);
typedef GLint  (APIENTRY *pfn_uniform_loc)(GLuint, const GLch *);
typedef void   (APIENTRY *pfn_uniform1i)(GLint, GLint);
typedef void   (APIENTRY *pfn_uniform1f)(GLint, GLfloat);
typedef void   (APIENTRY *pfn_uniform2f)(GLint, GLfloat, GLfloat);
typedef void   (APIENTRY *pfn_uniform1fv)(GLint, GLsizei, const GLfloat *);
typedef void   (APIENTRY *pfn_active_texture)(GLenum);
static pfn_create_shader p_create_shader; static pfn_shader_source p_shader_source; static pfn_uint p_compile, p_link, p_use;
static pfn_get_iv p_shader_iv, p_program_iv; static pfn_get_log p_shader_log, p_program_log; static pfn_create_program p_create_program;
static pfn_attach p_attach; static pfn_uniform_loc p_loc; static pfn_uniform1i p_u1i; static pfn_uniform1f p_u1f;
static pfn_uniform2f p_u2f; static pfn_uniform1fv p_u1fv; static pfn_active_texture p_active;

/* the mixer, per output pixel (GLSL 1.10 has no integer bit operations: pens are whole numbers in floats, exact below 2^24) */
static const char *FS =
    "uniform sampler2D t3d, t2d, pal;\n"
    "uniform vec2 org, psz, ksh;\n"            /* picture origin + size (window pixels), sharpening = window pixels per board pixel */
    "uniform float bx0, bxw, tew, tw;\n"      /* the board x the picture spans: bx0 .. bx0 + bxw; the 2D texture is tw wide, the board from column tew */
    "uniform float pri[16];\n"
    "float by(float c) { return floor(c * 255.0 + 0.5); }\n"
    "vec3 palc(float p) { return texture2D(pal, vec2((mod(p, 256.0) + 0.5) / 256.0, (floor(p / 256.0) + 0.5) / 128.0)).rgb; }\n"
    "float bank(float b, float x) { return b + mod(x, 8192.0); }\n"
    /* namcos21_c67 screen_update, pri1 = 4: kept where ((s & 0x5000) && pri[s >> 8 & 15] <= z) || s < 0x1000 */
    "bool zpass(float s, float z) {\n"
    "    float b12 = mod(floor(s / 4096.0), 2.0), b14 = mod(floor(s / 16384.0), 2.0);\n"
    "    if (b12 + b14 > 0.0 && pri[int(mod(floor(s / 256.0), 16.0))] <= z) return true;\n"
    "    return s < 4096.0;\n"
    "}\n"
    "vec3 texel(vec2 tc, float P, float Z) {\n"
    "    vec4 a = (tc.x < 0.0 || tc.x > 1.0) ? vec4(1.0, 0.0, 1.0, 1.0) : texture2D(t2d, tc);\n"   /* beside the board's 2D: backdrop, keep */
    "    if (P == 0.0) return palc(by(a.r) + 256.0 * by(a.g));\n"
    "    float op = by(a.b) + 256.0 * by(a.a), x = P, s;\n"
    "    if (op < 32768.0) x = op;\n"
    "    else if (op < 65531.0) { s = op - 32768.0; if (zpass(s, Z)) x = s; }\n"
    "    else if (op == 65534.0) x = bank(16384.0, P);\n"
    "    else if (op == 65533.0) x = bank(24576.0, P);\n"
    "    else if (op == 65532.0) { s = bank(16384.0, P); if (zpass(s, Z)) x = s; }\n"
    "    else if (op == 65531.0) { s = bank(24576.0, P); if (zpass(s, Z)) x = s; }\n"
    "    return palc(x);\n"
    "}\n"
    "void main() {\n"
    "    vec2 fc = gl_FragCoord.xy - org;\n"
    "    vec4 d = texture2D(t3d, fc / psz);\n"
    "    float P = by(d.r) + 256.0 * by(d.g), Z = by(d.b) + 256.0 * by(d.a);\n"
    "    vec2 u = vec2(bx0 + fc.x / psz.x * bxw, (psz.y - fc.y) / psz.y * 480.0) - 0.5;\n"
    "    vec2 t0 = floor(u), f = clamp((u - t0 - 0.5) * ksh + 0.5, 0.0, 1.0);\n"
    "    vec2 tc = (t0 + vec2(0.5 + tew, 0.5)) / vec2(tw, 480.0), dx = vec2(1.0 / tw, 0.0), dy = vec2(0.0, 1.0 / 480.0);\n"
    "    vec3 c = texel(tc, P, Z);\n"
    "    if (f.x > 0.0) c = mix(c, texel(tc + dx, P, Z), f.x);\n"
    "    if (f.y > 0.0) {\n"
    "        vec3 c2 = texel(tc + dy, P, Z);\n"
    "        if (f.x > 0.0) c2 = mix(c2, texel(tc + dx + dy, P, Z), f.x);\n"
    "        c = mix(c, c2, f.y);\n"
    "    }\n"
    "    gl_FragColor = vec4(c, 1.0);\n"
    "}\n";

static int state;                     /* 0 untried, 1 ready, -1 unavailable */
static GLuint prog, t3d, t2d, tpal;
static int t3d_w, t3d_h;
static GLint u_org, u_psz, u_ksh, u_pri, u_bx0, u_bxw, u_tew, u_tw;
static int t2d_w = S21_W;
static int wide_on;                   /* s21_video_gl_wide(): the 3D drawn past the 4:3 sides */
void s21_video_gl_wide(int on) { wide_on = on; }

static GLuint mktex(void)
{
    GLuint t; glGenTextures(1, &t); glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return t;
}
static int init(void)
{
#define LOAD(v, T, n) v = (T)SDL_GL_GetProcAddress(n); if (!v) { fprintf(stderr, "[S21GL] no %s\n", n); return 0; }
    LOAD(p_create_shader, pfn_create_shader, "glCreateShader"); LOAD(p_shader_source, pfn_shader_source, "glShaderSource");
    LOAD(p_compile, pfn_uint, "glCompileShader"); LOAD(p_shader_iv, pfn_get_iv, "glGetShaderiv");
    LOAD(p_shader_log, pfn_get_log, "glGetShaderInfoLog"); LOAD(p_program_log, pfn_get_log, "glGetProgramInfoLog");
    LOAD(p_create_program, pfn_create_program, "glCreateProgram"); LOAD(p_attach, pfn_attach, "glAttachShader");
    LOAD(p_link, pfn_uint, "glLinkProgram"); LOAD(p_program_iv, pfn_get_iv, "glGetProgramiv"); LOAD(p_use, pfn_uint, "glUseProgram");
    LOAD(p_loc, pfn_uniform_loc, "glGetUniformLocation"); LOAD(p_u1i, pfn_uniform1i, "glUniform1i");
    LOAD(p_u1f, pfn_uniform1f, "glUniform1f"); LOAD(p_u2f, pfn_uniform2f, "glUniform2f"); LOAD(p_u1fv, pfn_uniform1fv, "glUniform1fv");
    LOAD(p_active, pfn_active_texture, "glActiveTexture");
#undef LOAD
    GLint bits = 0;
    glGetIntegerv(GL_ALPHA_BITS, &bits);
    if (bits < 8) { fprintf(stderr, "[S21GL] the framebuffer has %d alpha bits (8 needed)\n", bits); return 0; }
    glGetIntegerv(GL_DEPTH_BITS, &bits);
    if (bits < 16) { fprintf(stderr, "[S21GL] the framebuffer has %d depth bits (16 needed)\n", bits); return 0; }
    GLuint fs = p_create_shader(GL_FRAGMENT_SHADER);
    p_shader_source(fs, 1, &FS, NULL);
    p_compile(fs);
    GLint ok = 0; char log[1024];
    p_shader_iv(fs, GL_COMPILE_STATUS, &ok);
    if (!ok) { p_shader_log(fs, sizeof log, NULL, log); fprintf(stderr, "[S21GL] mixer shader: %s\n", log); return 0; }
    prog = p_create_program(); p_attach(prog, fs); p_link(prog);
    p_program_iv(prog, GL_LINK_STATUS, &ok);
    if (!ok) { p_program_log(prog, sizeof log, NULL, log); fprintf(stderr, "[S21GL] mixer link: %s\n", log); return 0; }
    p_use(prog);
    p_u1i(p_loc(prog, "t3d"), 0); p_u1i(p_loc(prog, "t2d"), 1); p_u1i(p_loc(prog, "pal"), 2);
    u_org = p_loc(prog, "org"); u_psz = p_loc(prog, "psz"); u_ksh = p_loc(prog, "ksh"); u_pri = p_loc(prog, "pri"); u_bx0 = p_loc(prog, "bx0"); u_bxw = p_loc(prog, "bxw"); u_tew = p_loc(prog, "tew"); u_tw = p_loc(prog, "tw");
    p_use(0);
    t3d = mktex(); t2d = mktex(); tpal = mktex();
    glBindTexture(GL_TEXTURE_2D, t2d); glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, S21_W, S21_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glBindTexture(GL_TEXTURE_2D, tpal); glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 256, 128, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL);
    glBindTexture(GL_TEXTURE_2D, 0);
    return 1;
}

/* the engine hooks: the quad's pen + z as its "colour", z as its depth */
static int flat_cb(const geo_quad *q, float c[4])
{
    const unsigned p = q->color, z = (unsigned)q->zsort;
    c[0] = (float)(p & 0xff) / 255.0f; c[1] = (float)(p >> 8 & 0xff) / 255.0f;
    c[2] = (float)(z & 0xff) / 255.0f; c[3] = (float)(z >> 8 & 0xff) / 255.0f;
    return 1;
}
static float depth_cb(const geo_quad *q) { return 1.0f - 2.0f * (float)q->zsort / 65536.0f; }   /* glOrtho(.., -1, 1): window z = zsort/65536 */

static geo_quad gq;                   /* one quad at a time: the flat path reads rv[0..3].sx16/sy16, nrv, color, zsort */
long g_s21gl_quads;                   /* quads drawn (cumulative) */
long g_s21gl_wide_fullq;              /* widescreen: full-board quads widened to the picture (cumulative) */

bool s21_video_draw_gl(int x, int y, int w, int h)
{
    if (state == 0) { state = init() ? 1 : -1; fprintf(stderr, "[S21GL] GL picture: %s\n", state > 0 ? "on (engine quads + GLSL mixer)" : "UNAVAILABLE"); }
    if (state < 0 || w < 1 || h < 1) return false;
    glViewport(x, y, w, h);
    glEnable(GL_SCISSOR_TEST); glScissor(x, y, w, h);
    glClearColor(0, 0, 0, 0); glClearDepth(0x8000 / 65536.0); glDepthMask(GL_TRUE);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);
    glClearDepth(1.0);
    if (!s21v_video_on()) {               /* video enable bit 6 clear: black */
        glClearColor(0, 0, 0, 1); glEnable(GL_SCISSOR_TEST); glClear(GL_COLOR_BUFFER_BIT); glDisable(GL_SCISSOR_TEST);
        return true;
    }
    /* ---- pass 1: the quads through the engine ---- */
    glMatrixMode(GL_PROJECTION); glLoadIdentity();
    static double ex = -1, ey;
    if (ex < 0) { ex = 1.0 / 128; ey = 1.0 / 256; const char *e = getenv("CS_GLEPS"); if (e) sscanf(e, "%lf,%lf", &ex, &ey); }
    /* the board's 496 x 480 fill a 4:3 picture h * 4/3 wide; a wider rectangle (widescreen) shows more board x either side */
    const double bxw = wide_on ? S21_W * (double)w / (h * 4.0 / 3.0) : S21_W, bx0 = (S21_W - bxw) / 2;
    glOrtho(bx0 + 0.5 - ex, bx0 + bxw + 0.5 - ex, S21_H - 0.5 + ey, -0.5 + ey, -1.0, 1.0);
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    glMatrixMode(GL_TEXTURE); glLoadIdentity(); glMatrixMode(GL_MODELVIEW);
    glDisable(GL_TEXTURE_2D); glDisable(GL_BLEND); glDisable(GL_ALPHA_TEST); glDisable(GL_CULL_FACE); glDisable(GL_DITHER);
    glShadeModel(GL_FLAT);
    eng_draw_cfg cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.flat_quad = flat_cb;
    cfg.depth_test = 1;
    cfg.quad_depth = depth_cb;
    int n; const uint16_t *col; const int *zs;
    const s21_quad *q = s21v_vis_quads(&n, &col, &zs);
    eng_draw_begin();
    gq.nrv = 4;
    /* Widescreen: a quad that covers the WHOLE board at one depth -- an upright rectangle past all four screen edges, e.g. the hit
     * flash (a direct quad, -256..256 square at z 0) -- is the game's full-screen effect, sized for the 496 x 480 board: its left and
     * right edges go out to the wide picture's (the counterpart of engine/quad_gl.c's wide_backdrop for a screen-sized quad). */
    const int wx = wide_on ? (int)ceil(bxw / 2.0) + 1 : 0;
    for (int k = 0; k < n; k++) {
        int qx[4];
        for (int i = 0; i < 4; i++) qx[i] = q[k].sx[i];
        if (wx > S21_W / 2 && q[k].z[0] == q[k].z[1] && q[k].z[0] == q[k].z[2] && q[k].z[0] == q[k].z[3]) {
            int xa = qx[0], xb = qx[0], ya = q[k].sy[0], yb = q[k].sy[0], up = 1;
            for (int i = 1; i < 4; i++) { if (qx[i] < xa) xa = qx[i]; if (qx[i] > xb) xb = qx[i]; if (q[k].sy[i] < ya) ya = q[k].sy[i]; if (q[k].sy[i] > yb) yb = q[k].sy[i]; }
            for (int i = 0; i < 4; i++) if ((qx[i] != xa && qx[i] != xb) || (q[k].sy[i] != ya && q[k].sy[i] != yb)) up = 0;
            if (up && xa <= -S21_W / 2 && xb >= S21_W / 2 - 1 && ya <= -S21_H / 2 && yb >= S21_H / 2 - 1) {
                for (int i = 0; i < 4; i++) qx[i] = qx[i] == xa ? (xa < -wx ? xa : -wx) : (xb > wx ? xb : wx);
                g_s21gl_wide_fullq++;
            }
        }
        for (int i = 0; i < 4; i++) { gq.rv[i].sx16 = (S21_W / 2 + qx[i]) * 16; gq.rv[i].sy16 = (S21_H / 2 + q[k].sy[i]) * 16; }
        gq.color = col[k]; gq.zsort = zs[k];
        eng_draw_quad(&gq, &cfg);
    }
    eng_draw_end();
    g_s21gl_quads += n;
    glShadeModel(GL_SMOOTH); glDisable(GL_ALPHA_TEST); glEnable(GL_DITHER);
    /* ---- pass 2: the mixer ---- */
    p_active(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, t3d);
    if (w != t3d_w || h != t3d_h) { glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL); t3d_w = w; t3d_h = h; }
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, x, y, w, h);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    p_active(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, t2d);
    const int w2d = s21v_gl2d_w();                    /* 496, or 496 + 2E in widescreen (s21_video.c) */
    if (w2d != t2d_w) { glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w2d, S21_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL); t2d_w = w2d; }
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w2d, S21_H, GL_RGBA, GL_UNSIGNED_BYTE, s21v_gl2d());
    p_active(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, tpal);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 128, GL_RGB, GL_UNSIGNED_BYTE, s21v_palrgb());
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    p_use(prog);
    GLint vp[4]; glGetIntegerv(GL_VIEWPORT, vp);
    p_u2f(u_org, (float)x, (float)y); p_u2f(u_psz, (float)w, (float)h);
    p_u1f(u_bx0, (float)bx0); p_u1f(u_bxw, (float)bxw);
    p_u1f(u_tew, (float)((w2d - S21_W) / 2)); p_u1f(u_tw, (float)w2d);
    float kx = (float)(w / bxw), ky = (float)h / S21_H;
    p_u2f(u_ksh, kx < 1 ? 1 : kx, ky < 1 ? 1 : ky);
    float pt[16]; const uint16_t *pr = s21v_pritab();
    for (int i = 0; i < 16; i++) pt[i] = pr[i];
    p_u1fv(u_pri, 16, pt);
    glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0, 1, 0, 1, -1, 1);
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    glDisable(GL_DEPTH_TEST);
    glBegin(GL_QUADS); glVertex2f(0, 0); glVertex2f(1, 0); glVertex2f(1, 1); glVertex2f(0, 1); glEnd();
    p_use(0);
    p_active(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, 0);
    p_active(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, 0);
    p_active(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, 0);
    (void)vp;
    return true;
}
