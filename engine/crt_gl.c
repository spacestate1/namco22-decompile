/*
 * crt_gl.c -- the Scud Race CRT (wanszai's Scud Race PC 0.0.15, Tim Lottes' public-domain CRT math), see crt_gl.h.
 *
 * Where it sits: in place of rt_end_rect, the step that scales the frame into the picture rectangle. The frame (the
 * render target's FBO, or the window's back buffer at native size) is first scaled to the rectangle's size with the
 * Scaling filter, into a texture; then a bright pass at a quarter of that size, a 5-tap Gaussian across and down, and
 * the composite drawn into the rectangle 1:1 -- so the phosphor mask is 3 real screen pixels, nothing rescales it
 * afterwards, and the menu, crosshair and gun border the host draws next stay on top of the tube. CRT off with a
 * brightness other than 1.00: the same scaled copy drawn back times the gain.
 *
 * The values are scudrace.exe's k_crt_presets, not tuned. GLSL 1.10 (GL 2.0), loaded by name like post_gl.c; the
 * targets are texture FBOs. No shaders or FBOs: "CRT UNAVAILABLE" once, and the host shows the plain picture.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL2/SDL.h>
#include "eng_gl.h"
#include "crt_gl.h"
#include "render_target.h"

#ifndef GL_FRAGMENT_SHADER
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER   0x8B31
#define GL_COMPILE_STATUS  0x8B81
#define GL_LINK_STATUS     0x8B82
#endif
#ifndef GL_FRAMEBUFFER
#define GL_FRAMEBUFFER          0x8D40
#define GL_READ_FRAMEBUFFER     0x8CA8
#define GL_DRAW_FRAMEBUFFER     0x8CA9
#define GL_COLOR_ATTACHMENT0    0x8CE0
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#endif
#ifndef APIENTRY
#define APIENTRY
#endif
typedef char GLchar_;
typedef GLuint (APIENTRY *pfn_create_shader)(GLenum);
typedef void   (APIENTRY *pfn_shader_source)(GLuint, GLsizei, const GLchar_ *const *, const GLint *);
typedef void   (APIENTRY *pfn_uint)(GLuint);
typedef void   (APIENTRY *pfn_get_iv)(GLuint, GLenum, GLint *);
typedef void   (APIENTRY *pfn_get_log)(GLuint, GLsizei, GLsizei *, GLchar_ *);
typedef GLuint (APIENTRY *pfn_create_program)(void);
typedef void   (APIENTRY *pfn_attach)(GLuint, GLuint);
typedef GLint  (APIENTRY *pfn_uniform_loc)(GLuint, const GLchar_ *);
typedef void   (APIENTRY *pfn_uniform1i)(GLint, GLint);
typedef void   (APIENTRY *pfn_uniform1f)(GLint, GLfloat);
typedef void   (APIENTRY *pfn_uniform2f)(GLint, GLfloat, GLfloat);
typedef void   (APIENTRY *pfn_uniform4f)(GLint, GLfloat, GLfloat, GLfloat, GLfloat);
typedef void   (APIENTRY *pfn_active_texture)(GLenum);
typedef void   (APIENTRY *pfn_gen)(GLsizei, GLuint *);
typedef void   (APIENTRY *pfn_del)(GLsizei, const GLuint *);
typedef void   (APIENTRY *pfn_bind)(GLenum, GLuint);
typedef void   (APIENTRY *pfn_fbtex)(GLenum, GLenum, GLenum, GLuint, GLint);
typedef GLenum (APIENTRY *pfn_status)(GLenum);
typedef void   (APIENTRY *pfn_blit)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);

static pfn_create_shader  p_create_shader;
static pfn_shader_source  p_shader_source;
static pfn_uint           p_compile, p_link, p_use;
static pfn_get_iv         p_get_shader_iv, p_get_program_iv;
static pfn_get_log        p_get_shader_log, p_get_program_log;
static pfn_create_program p_create_program;
static pfn_attach         p_attach;
static pfn_uniform_loc    p_uniform_loc;
static pfn_uniform1i      p_uniform1i;
static pfn_uniform1f      p_uniform1f;
static pfn_uniform2f      p_uniform2f;
static pfn_uniform4f      p_uniform4f;
static pfn_active_texture p_active_texture;
static pfn_gen            p_gen_fb;
static pfn_del            p_del_fb;
static pfn_bind           p_bind_fb;
static pfn_fbtex          p_fb_tex;
static pfn_status         p_fb_status;
static pfn_blit           p_blit;

static int    state;                       /* 0 untried, 1 ready, -1 unavailable */
static GLuint prog_bloom, prog_blur, prog_crt, prog_gain;
static GLuint src_tex, src_fb, bloom_tex, bloom_fb, blur_tex, blur_fb;
static int    out_w, out_h, q_w, q_h;      /* the targets' sizes: the picture rectangle, a quarter of it */
static int    bad_w, bad_h;                /* a size whose targets failed (tried again at another size) */

/* scudrace.exe k_crt_presets: scan, beam min, beam max, mask strength, mask type (0 grille, 1 shadow), glow */
static const float k_crt_presets[3][6] = {
    { 0.00f, 0.00f, 0.00f, 0.00f, 0.00f, 0.00f },   /* OFF */
    { 0.35f, 0.90f, 1.60f, 0.30f, 0.00f, 0.20f },   /* TRINITRON (aperture grille) */
    { 0.50f, 0.80f, 1.40f, 0.45f, 1.00f, 0.35f },   /* ARCADE (shadow mask) */
};
static const char *const k_crt_names[3] = { "OFF", "TRINITRON", "ARCADE" };
#define CRT_CURVE    0.12f                 /* the Curvature row on: curvature, vignette */
#define CRT_VIGNETTE 0.25f
#define CRT_LINES    480.0f                /* System 22's visible lines: one beam per board line */

/* ---- GLSL 1.10: the skill's shaders (references/shaders.md), one fullscreen quad each ---------------------------- */
static const char *VS =
    "varying vec2 vUv;\n"
    "void main() { vUv = gl_Vertex.xy * 0.5 + 0.5; gl_Position = vec4(gl_Vertex.xy, 0.0, 1.0); }\n";
static const char *FS_BLOOM =
    "uniform sampler2D s_src; varying vec2 vUv;\n"
    "void main() {\n"
    "    vec3 c = texture2D(s_src, vUv).rgb;\n"
    "    float l = max(max(c.r, c.g), c.b);\n"
    "    float k = clamp((l - 0.40) / 0.60, 0.0, 1.0);\n"
    "    gl_FragColor = vec4(c * k, 1.0);\n"
    "}\n";
static const char *FS_BLUR =
    "uniform sampler2D s_src; uniform vec4 c_dir; varying vec2 vUv;\n"
    "void main() {\n"
    "    float w[5]; w[0]=0.227027; w[1]=0.194595; w[2]=0.121622; w[3]=0.054054; w[4]=0.016216;\n"
    "    vec3 s = texture2D(s_src, vUv).rgb * w[0];\n"
    "    for (int i = 1; i < 5; i++) {\n"
    "        vec2 o = c_dir.xy * float(i);\n"
    "        s += texture2D(s_src, vUv + o).rgb * w[i];\n"
    "        s += texture2D(s_src, vUv - o).rgb * w[i];\n"
    "    }\n"
    "    gl_FragColor = vec4(s, 1.0);\n"
    "}\n";
static const char *FS_CRT =                /* org = the rectangle's bottom-left in window pixels: the mask is the picture's own */
    "uniform sampler2D s_src; uniform sampler2D s_glow;\n"
    "uniform vec4 p0; uniform vec4 p1; uniform vec4 p2; uniform vec2 org; varying vec2 vUv;\n"
    "vec2 warp(vec2 uv, float amt) { uv = uv*2.0-1.0; vec2 o = abs(uv.yx)/vec2(3.0,4.0); uv += uv*o*o*amt; return uv*0.5+0.5; }\n"
    "float beam(float dist, float luma, float bmin, float bmax) { float w = mix(bmin,bmax,luma); float d = dist/max(w,1e-3); return exp(-2.0*d*d); }\n"
    "vec3 mask(vec2 spos, float type, float strength) {\n"
    "    float shift = (type < 0.5) ? 0.0 : (mod(spos.y, 2.0) < 1.0 ? 0.0 : 1.5);\n"
    "    float px = floor(mod(spos.x + shift, 3.0));\n"
    "    vec3 m = (px < 0.5) ? vec3(1.0,0.0,0.0) : (px < 1.5) ? vec3(0.0,1.0,0.0) : vec3(0.0,0.0,1.0);\n"
    "    return mix(vec3(1.0), m, strength);\n"
    "}\n"
    "void main() {\n"
    "    vec2 uv = vec2(vUv.x, 1.0 - vUv.y);\n"
    "    vec2 wuv = warp(uv, p1.z);\n"
    "    if (wuv.x < 0.0 || wuv.x > 1.0 || wuv.y < 0.0 || wuv.y > 1.0) { gl_FragColor = vec4(0.0,0.0,0.0,1.0); return; }\n"
    "    vec2 tuv = vec2(wuv.x, 1.0 - wuv.y);\n"
    "    vec3 src = texture2D(s_src, tuv).rgb;\n"
    "    float luma = dot(src, vec3(0.299, 0.587, 0.114));\n"
    "    float dist = fract(wuv.y * p2.y) - 0.5;\n"
    "    float b = beam(dist, luma, p0.y, p0.z);\n"
    "    vec3 col = src * mix(1.0 - p0.x, 1.0, b);\n"
    "    vec2 spos = gl_FragCoord.xy - org;\n"
    "    col *= mask(vec2(spos.x, p2.w - spos.y), p1.x, p0.w);\n"
    "    col += texture2D(s_glow, tuv).rgb * p1.y;\n"
    "    vec2 vd = uv * 2.0 - 1.0;\n"
    "    col *= clamp(1.0 - dot(vd, vd) * p1.w, 0.0, 1.0);\n"
    "    col *= p2.x;\n"
    "    gl_FragColor = vec4(clamp(col, 0.0, 1.0), 1.0);\n"
    "}\n";
static const char *FS_GAIN =
    "uniform sampler2D tex; uniform float gGain; varying vec2 vUv;\n"
    "void main() { gl_FragColor = vec4(clamp(texture2D(tex, vUv).rgb * gGain, 0.0, 1.0), 1.0); }\n";

static void *proc(const char *core, const char *ext)
{
    void *p = SDL_GL_GetProcAddress(core);
    return p || !ext ? p : SDL_GL_GetProcAddress(ext);
}

static GLuint shader(GLenum type, const char *src, const char *name)
{
    GLuint s = p_create_shader(type);
    p_shader_source(s, 1, &src, NULL);
    p_compile(s);
    GLint ok = 0;
    p_get_shader_iv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[512] = ""; p_get_shader_log(s, sizeof log, NULL, log); fprintf(stderr, "[CRT] %s: %s\n", name, log); return 0; }
    return s;
}
static GLuint program(GLuint vs, const char *fs_src, const char *name)
{
    GLuint fs = shader(GL_FRAGMENT_SHADER, fs_src, name);
    if (!vs || !fs) return 0;
    GLuint pr = p_create_program();
    p_attach(pr, vs); p_attach(pr, fs);
    p_link(pr);
    GLint ok = 0;
    p_get_program_iv(pr, GL_LINK_STATUS, &ok);
    if (!ok) { char log[512] = ""; p_get_program_log(pr, sizeof log, NULL, log); fprintf(stderr, "[CRT] %s link: %s\n", name, log); return 0; }
    return pr;
}

static const char *init_gl(void)            /* NULL = ready, else why not */
{
#define LOAD(v, T, n, e) if (!(v = (T)proc(n, e))) return n
    LOAD(p_create_shader, pfn_create_shader, "glCreateShader", NULL);
    LOAD(p_shader_source, pfn_shader_source, "glShaderSource", NULL);
    LOAD(p_compile, pfn_uint, "glCompileShader", NULL);
    LOAD(p_get_shader_iv, pfn_get_iv, "glGetShaderiv", NULL);
    LOAD(p_get_shader_log, pfn_get_log, "glGetShaderInfoLog", NULL);
    LOAD(p_create_program, pfn_create_program, "glCreateProgram", NULL);
    LOAD(p_attach, pfn_attach, "glAttachShader", NULL);
    LOAD(p_link, pfn_uint, "glLinkProgram", NULL);
    LOAD(p_get_program_iv, pfn_get_iv, "glGetProgramiv", NULL);
    LOAD(p_get_program_log, pfn_get_log, "glGetProgramInfoLog", NULL);
    LOAD(p_use, pfn_uint, "glUseProgram", NULL);
    LOAD(p_uniform_loc, pfn_uniform_loc, "glGetUniformLocation", NULL);
    LOAD(p_uniform1i, pfn_uniform1i, "glUniform1i", NULL);
    LOAD(p_uniform1f, pfn_uniform1f, "glUniform1f", NULL);
    LOAD(p_uniform2f, pfn_uniform2f, "glUniform2f", NULL);
    LOAD(p_uniform4f, pfn_uniform4f, "glUniform4f", NULL);
    LOAD(p_active_texture, pfn_active_texture, "glActiveTexture", "glActiveTextureARB");
    LOAD(p_gen_fb, pfn_gen, "glGenFramebuffers", "glGenFramebuffersEXT");
    LOAD(p_del_fb, pfn_del, "glDeleteFramebuffers", "glDeleteFramebuffersEXT");
    LOAD(p_bind_fb, pfn_bind, "glBindFramebuffer", "glBindFramebufferEXT");
    LOAD(p_fb_tex, pfn_fbtex, "glFramebufferTexture2D", "glFramebufferTexture2DEXT");
    LOAD(p_fb_status, pfn_status, "glCheckFramebufferStatus", "glCheckFramebufferStatusEXT");
    LOAD(p_blit, pfn_blit, "glBlitFramebuffer", "glBlitFramebufferEXT");
#undef LOAD
    GLuint vs = shader(GL_VERTEX_SHADER, VS, "vertex shader");
    prog_bloom = program(vs, FS_BLOOM, "crt_bloom_ps");
    prog_blur = program(vs, FS_BLUR, "crt_blur_ps");
    prog_crt = program(vs, FS_CRT, "crt_ps");
    prog_gain = program(vs, FS_GAIN, "fullscreen_ps");
    if (!prog_bloom || !prog_blur || !prog_crt || !prog_gain) return "a shader did not build";
    p_use(prog_bloom); p_uniform1i(p_uniform_loc(prog_bloom, "s_src"), 0);
    p_use(prog_blur);  p_uniform1i(p_uniform_loc(prog_blur, "s_src"), 0);
    p_use(prog_crt);   p_uniform1i(p_uniform_loc(prog_crt, "s_src"), 0); p_uniform1i(p_uniform_loc(prog_crt, "s_glow"), 1);
    p_use(prog_gain);  p_uniform1i(p_uniform_loc(prog_gain, "tex"), 0);
    p_use(0);
    return NULL;
}

/* an RGBA8 linear texture with real (zeroed) storage -- some GL 2.1 drivers reject NULL -- as an FBO's colour */
static bool target(GLuint *tex, GLuint *fb, int w, int h, void *zero)
{
    if (*fb) { p_del_fb(1, fb); *fb = 0; }
    if (*tex) { glDeleteTextures(1, tex); *tex = 0; }
    glGenTextures(1, tex);
    glBindTexture(GL_TEXTURE_2D, *tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, zero);
    glBindTexture(GL_TEXTURE_2D, 0);
    p_gen_fb(1, fb);
    p_bind_fb(GL_FRAMEBUFFER, *fb);
    p_fb_tex(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, *tex, 0);
    const GLenum st = p_fb_status(GL_FRAMEBUFFER);
    p_bind_fb(GL_FRAMEBUFFER, 0);
    return st == GL_FRAMEBUFFER_COMPLETE;
}
static bool targets(int w, int h)            /* the composite size, and a quarter of it for the glow */
{
    if (src_fb && out_w == w && out_h == h) return true;
    if (w == bad_w && h == bad_h) return false;
    const int qw = w / 4 > 0 ? w / 4 : 1, qh = h / 4 > 0 ? h / 4 : 1;
    void *zero = calloc((size_t)w * h, 4);
    bool ok = zero && target(&src_tex, &src_fb, w, h, zero) && target(&bloom_tex, &bloom_fb, qw, qh, zero)
              && target(&blur_tex, &blur_fb, qw, qh, zero);
    free(zero);
    if (!ok) {
        fprintf(stderr, "[CRT] CRT UNAVAILABLE (no %dx%d render targets): the plain picture\n", w, h);
        bad_w = w; bad_h = h; out_w = out_h = 0;
        return false;
    }
    out_w = w; out_h = h; q_w = qw; q_h = qh; bad_w = bad_h = 0;
    return true;
}

static void quad(void)
{
    glBegin(GL_QUADS);
    glVertex2f(-1, -1); glVertex2f(1, -1); glVertex2f(1, 1); glVertex2f(-1, 1);
    glEnd();
}
static void pass(GLuint fb, int vx, int vy, int vw, int vh, GLuint prog, GLuint tex0)
{
    p_bind_fb(GL_FRAMEBUFFER, fb);
    glViewport(vx, vy, vw, vh);
    p_use(prog);
    p_active_texture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, tex0);
}

/* one line per change of setup, Scud's wording, so builds compare textually */
static void log_setup(int tv, int curve, int gain, int w, int h)
{
    static int last[5] = { -1, -1, -1, -1, -1 };
    if (!tv && gain == ENG_CRT_GAIN_DEF) w = h = 0;       /* nothing drawn: the size does not matter */
    const int now[5] = { tv, curve, gain, w, h };
    if (!memcmp(now, last, sizeof now)) return;
    memcpy(last, now, sizeof now);
    if (!tv) {
        if (w) fprintf(stderr, "[CRT] CRT OFF, brightness %.2f, out %dx%d\n", gain / 1000.0, w, h);
        else fprintf(stderr, "[CRT] CRT OFF, brightness %.2f\n", gain / 1000.0);
        return;
    }
    const float *r = k_crt_presets[tv];
    fprintf(stderr, "[CRT] CRT %s (bloom %dx%d, blur H, blur V, crt_ps: scan %.2f beam %.2f-%.2f mask %s %.2f glow %.2f, "
            "curvature %.2f vignette %.2f, %d virtual lines), brightness %.2f, out %dx%d\n",
            k_crt_names[tv], q_w, q_h, r[0], r[1], r[2], r[4] < 0.5f ? "grille" : "shadow", r[3], r[5],
            curve ? CRT_CURVE : 0.0f, curve ? CRT_VIGNETTE : 0.0f, (int)CRT_LINES, gain / 1000.0, w, h);
}

bool eng_crt_present(SDL_Window *win, int x, int y, int w, int h, int sharp, int tv, int curve, int gain_milli)
{
    tv = eng_crt_tv_valid(tv);
    curve = curve != 0;
    const int gain = eng_crt_gain_valid(gain_milli > 0 ? gain_milli : ENG_CRT_GAIN_DEF);
    if (!tv && gain == ENG_CRT_GAIN_DEF) { log_setup(0, curve, gain, 0, 0); return false; }   /* off at 1.00: the plain picture */
    if (w < 4 || h < 4) return false;
    if (state == 0) {
        const char *why = init_gl();
        state = why ? -1 : 1;
        if (why) fprintf(stderr, "[CRT] CRT UNAVAILABLE (%s): the plain picture\n", why);
    }
    if (state < 0 || !targets(w, h)) return false;
    log_setup(tv, curve, gain, w, h);

    int dw, dh;
    SDL_GL_GetDrawableSize(win, &dw, &dh);
    if (dw < 1 || dh < 1) SDL_GetWindowSize(win, &dw, &dh);
    glPushAttrib(GL_ENABLE_BIT | GL_COLOR_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST); glDisable(GL_DEPTH_TEST); glDisable(GL_BLEND); glDisable(GL_ALPHA_TEST);
    glDisable(GL_CULL_FACE); glDisable(GL_TEXTURE_2D);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    /* the frame, scaled to the rectangle's size with the Scaling filter (the upscale comes before the CRT) */
    int fw, fh;
    const GLuint from = rt_take(win, &fw, &fh);
    p_bind_fb(GL_READ_FRAMEBUFFER, from);
    if (!from) glReadBuffer(GL_BACK);
    p_bind_fb(GL_DRAW_FRAMEBUFFER, src_fb);
    p_blit(0, 0, fw, fh, 0, 0, w, h, GL_COLOR_BUFFER_BIT, sharp ? GL_NEAREST : GL_LINEAR);
    p_bind_fb(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, dw, dh);                                 /* the window round the picture: black, as rt_end_rect leaves it */
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    const int gy = dh - (y + h);                              /* GL's origin is bottom-left */

    if (tv) {
        const float *r = k_crt_presets[tv];
        pass(bloom_fb, 0, 0, q_w, q_h, prog_bloom, src_tex); quad();
        pass(blur_fb, 0, 0, q_w, q_h, prog_blur, bloom_tex);
        p_uniform4f(p_uniform_loc(prog_blur, "c_dir"), 1.0f / q_w, 0.0f, 0.0f, 0.0f); quad();
        pass(bloom_fb, 0, 0, q_w, q_h, prog_blur, blur_tex);
        p_uniform4f(p_uniform_loc(prog_blur, "c_dir"), 0.0f, 1.0f / q_h, 0.0f, 0.0f); quad();
        pass(0, x, gy, w, h, prog_crt, src_tex);
        p_active_texture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, bloom_tex);
        p_uniform4f(p_uniform_loc(prog_crt, "p0"), r[0], r[1], r[2], r[3]);
        p_uniform4f(p_uniform_loc(prog_crt, "p1"), r[4], r[5], curve ? CRT_CURVE : 0.0f, curve ? CRT_VIGNETTE : 0.0f);
        p_uniform4f(p_uniform_loc(prog_crt, "p2"), gain / 1000.0f, CRT_LINES, (float)w, (float)h);
        p_uniform2f(p_uniform_loc(prog_crt, "org"), (float)x, (float)gy);
        quad();
        glBindTexture(GL_TEXTURE_2D, 0);
        p_active_texture(GL_TEXTURE0);
    } else {
        pass(0, x, gy, w, h, prog_gain, src_tex);
        p_uniform1f(p_uniform_loc(prog_gain, "gGain"), gain / 1000.0f);
        quad();
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    p_use(0);
    glPopAttrib();
    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, dw, dh);
    return true;
}
