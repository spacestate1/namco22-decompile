/*
 * post_gl.c -- the final per-channel colour stage over the whole frame: the
 * mixer's gamma LUTs (Super 22) or the gamma PROMs plus the global fade
 * (System 22). Both are one 256-entry table per channel applied to the
 * COMPOSED picture, background and text included, which is how the hardware
 * does it -- it cannot be folded into the palette, because shading and fog
 * blend before it.
 *
 * GLSL 1.10 (GL 2.0): the frame is copied to a texture and drawn back through
 * a 256x1 RGB table. Without shaders it falls back to GL's pixel map on the
 * copy -- exact, but on NVIDIA that path is a CPU round trip and costs a
 * whole frame.
 */
#include <stdio.h>
#include <string.h>
#include <SDL2/SDL.h>
#include "eng_gl.h"
#include "post_gl.h"

#ifndef GL_FRAGMENT_SHADER
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER   0x8B31
#define GL_COMPILE_STATUS  0x8B81
#define GL_LINK_STATUS     0x8B82
#endif
#ifndef GL_TEXTURE0
#define GL_TEXTURE0 0x84C0
#define GL_TEXTURE1 0x84C1
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
typedef void   (APIENTRY *pfn_active_texture)(GLenum);

static pfn_create_shader  p_create_shader;
static pfn_shader_source  p_shader_source;
static pfn_uint           p_compile, p_link, p_use;
static pfn_get_iv         p_get_shader_iv, p_get_program_iv;
static pfn_get_log        p_get_shader_log;
static pfn_create_program p_create_program;
static pfn_attach         p_attach;
static pfn_uniform_loc    p_uniform_loc;
static pfn_uniform1i      p_uniform1i;
static pfn_active_texture p_active_texture;

static int    state;             /* 0 untried, 1 shader, -1 pixel-map fallback */
static GLuint prog, frame_tex, lut_tex;
static int    frame_w, frame_h;

static const char *FS =
    "uniform sampler2D frame;\n"
    "uniform sampler2D lut;\n"
    "void main() {\n"
    "    vec3 c = texture2D(frame, gl_TexCoord[0].st).rgb;\n"
    "    /* texel centres of the 256-wide table: (v * 255 + 0.5) / 256 */\n"
    "    vec3 k = c * (255.0 / 256.0) + 0.5 / 256.0;\n"
    "    gl_FragColor = vec4(texture2D(lut, vec2(k.r, 0.5)).r,\n"
    "                        texture2D(lut, vec2(k.g, 0.5)).g,\n"
    "                        texture2D(lut, vec2(k.b, 0.5)).b, 1.0);\n"
    "}\n";

static int init_shader(void)
{
#define LOAD(v, T, n) v = (T)SDL_GL_GetProcAddress(n); if (!v) return 0
    LOAD(p_create_shader, pfn_create_shader, "glCreateShader");
    LOAD(p_shader_source, pfn_shader_source, "glShaderSource");
    LOAD(p_compile, pfn_uint, "glCompileShader");
    LOAD(p_get_shader_iv, pfn_get_iv, "glGetShaderiv");
    LOAD(p_get_shader_log, pfn_get_log, "glGetShaderInfoLog");
    LOAD(p_create_program, pfn_create_program, "glCreateProgram");
    LOAD(p_attach, pfn_attach, "glAttachShader");
    LOAD(p_link, pfn_uint, "glLinkProgram");
    LOAD(p_get_program_iv, pfn_get_iv, "glGetProgramiv");
    LOAD(p_use, pfn_uint, "glUseProgram");
    LOAD(p_uniform_loc, pfn_uniform_loc, "glGetUniformLocation");
    LOAD(p_uniform1i, pfn_uniform1i, "glUniform1i");
    LOAD(p_active_texture, pfn_active_texture, "glActiveTexture");
#undef LOAD
    GLuint fs = p_create_shader(GL_FRAGMENT_SHADER);
    p_shader_source(fs, 1, &FS, NULL);
    p_compile(fs);
    GLint ok = 0;
    p_get_shader_iv(fs, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512]; p_get_shader_log(fs, sizeof log, NULL, log);
        fprintf(stderr, "[POST] shader: %s\n", log);
        return 0;
    }
    prog = p_create_program();
    p_attach(prog, fs);
    p_link(prog);
    p_get_program_iv(prog, GL_LINK_STATUS, &ok);
    if (!ok) return 0;
    p_use(prog);
    p_uniform1i(p_uniform_loc(prog, "frame"), 0);
    p_uniform1i(p_uniform_loc(prog, "lut"), 1);
    p_use(0);
    glGenTextures(1, &lut_tex);
    glBindTexture(GL_TEXTURE_2D, lut_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 256, 1, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL);
    return 1;
}

static void full_quad(void)
{
    glMatrixMode(GL_PROJECTION); glPushMatrix(); glLoadIdentity(); glOrtho(0, 1, 0, 1, -1, 1);
    glMatrixMode(GL_MODELVIEW);  glPushMatrix(); glLoadIdentity();
    glColor4f(1, 1, 1, 1);
    glBegin(GL_QUADS);
    glTexCoord2f(0, 0); glVertex2f(0, 0);
    glTexCoord2f(1, 0); glVertex2f(1, 0);
    glTexCoord2f(1, 1); glVertex2f(1, 1);
    glTexCoord2f(0, 1); glVertex2f(0, 1);
    glEnd();
    glMatrixMode(GL_PROJECTION); glPopMatrix();
    glMatrixMode(GL_MODELVIEW);  glPopMatrix();
}

void eng_post_lut(const uint8_t lut[3][256], int vw, int vh)
{
    if (state == 0) {
        state = init_shader() ? 1 : -1;
        fprintf(stderr, "[POST] final colour stage: %s\n", state > 0 ? "GLSL" : "pixel map (slow)");
    }
    glDisable(GL_BLEND); glDisable(GL_ALPHA_TEST); glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, vw, vh);
    if (!frame_tex || frame_w != vw || frame_h != vh) {
        if (!frame_tex) glGenTextures(1, &frame_tex);
        glBindTexture(GL_TEXTURE_2D, frame_tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, vw, vh, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        frame_w = vw; frame_h = vh;
    }
    if (state > 0) {
        uint8_t rgb[256][3];
        for (int v = 0; v < 256; v++) { rgb[v][0] = lut[0][v]; rgb[v][1] = lut[1][v]; rgb[v][2] = lut[2][v]; }
        p_active_texture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, lut_tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 1, GL_RGB, GL_UNSIGNED_BYTE, rgb);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        p_active_texture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, frame_tex);
        glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, vw, vh);
        p_use(prog);
        full_quad();
        p_use(0);
        return;
    }
    /* fallback: the pixel map applies during the copy */
    GLfloat map[3][256];
    for (int c = 0; c < 3; c++) for (int v = 0; v < 256; v++) map[c][v] = lut[c][v] / 255.0f;
    glPixelMapfv(GL_PIXEL_MAP_R_TO_R, 256, map[0]);
    glPixelMapfv(GL_PIXEL_MAP_G_TO_G, 256, map[1]);
    glPixelMapfv(GL_PIXEL_MAP_B_TO_B, 256, map[2]);
    glPixelTransferi(GL_MAP_COLOR, GL_TRUE);
    glBindTexture(GL_TEXTURE_2D, frame_tex);
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, vw, vh);
    glPixelTransferi(GL_MAP_COLOR, GL_FALSE);
    glEnable(GL_TEXTURE_2D);
    full_quad();
    glDisable(GL_TEXTURE_2D);
}
