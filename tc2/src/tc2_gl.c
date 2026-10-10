/*
 * tc2_gl.c -- THE GPU PICTURE: a built frame (src/tc2_build.h) drawn with OpenGL, organised like an ordinary PC renderer.
 *
 *   ASSETS, uploaded once (the ROM is data, nothing more):
 *     the texture tiles (tss1cg*, 32 MB of 8-bit pens)  -> an 8192 x 4096 single-channel texture
 *     the decoded tile map (tss1ccrl/h: tile, flips)     -> a 2048 x 1024 RGB texture
 *   PER FRAME, small uploads: the palette (32768 pens), the C412 stencil SRAM, the text layer's pen map, and ONE vertex buffer holding every
 *   polygon of the frame as triangles in the hardware's draw order (z-sort key, far to near).
 *   The target is stored TOP ROW FIRST (window y = screen y), so GL's fill rule breaks pixel-centre ties the way the hardware's scan does.
 *   PASSES, into an off-screen target at 640 x 480 times the internal-resolution scale:
 *     1. clear to the C404 background colour
 *     2. the text layer's pixels (all of them sit under the polygons at first)
 *     3. the polygons: ONE draw call. GL draws the primitives of a call in order, so blending against what is already there reproduces the
 *        hardware's painter's order exactly, without a depth buffer. The fragment shader does what the board's pixel pipeline does: the
 *        tile map -> tile -> pen lookup per pixel (no texture baking), the 2/4/8-bit colour modes, the stencil, Gouraud shade, the polygon
 *        colour fade, the screen fade, alpha by colour / by pen and the 50% blend.
 *     4. the same triangles into a small priority target (MAME's priority map: which text pixels a polygon lets through again)
 *     5. the text pixels the priority target marks, over the polygons
 *   The software back end (src/tc2_render.c tc2_soft_raster) draws the same built frame at 640 x 480 on the CPU: it is the fallback for a
 *   machine without GLSL 1.30 and the oracle this file is gated against (TC2_GPU_GATE, src/tc2_main.c).
 *
 * Needs OpenGL 3.0 / GLSL 1.30 (in a compatibility context) and 8192-texel textures: every Intel GPU since Sandy Bridge (HD 2000 / 3000).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_opengl.h>
#include "tc2_build.h"

/* the post-1.1 entry points, from the driver at run time (the same on Linux and Windows) */
#define GLFN(T, n) static T p_##n;
#define GLFNS \
    GLFN(PFNGLCREATESHADERPROC, glCreateShader) GLFN(PFNGLSHADERSOURCEPROC, glShaderSource) GLFN(PFNGLCOMPILESHADERPROC, glCompileShader) \
    GLFN(PFNGLGETSHADERIVPROC, glGetShaderiv) GLFN(PFNGLGETSHADERINFOLOGPROC, glGetShaderInfoLog) GLFN(PFNGLCREATEPROGRAMPROC, glCreateProgram) \
    GLFN(PFNGLATTACHSHADERPROC, glAttachShader) GLFN(PFNGLBINDATTRIBLOCATIONPROC, glBindAttribLocation) GLFN(PFNGLLINKPROGRAMPROC, glLinkProgram) \
    GLFN(PFNGLGETPROGRAMIVPROC, glGetProgramiv) GLFN(PFNGLGETPROGRAMINFOLOGPROC, glGetProgramInfoLog) GLFN(PFNGLUSEPROGRAMPROC, glUseProgram) \
    GLFN(PFNGLGETUNIFORMLOCATIONPROC, glGetUniformLocation) GLFN(PFNGLUNIFORM1IPROC, glUniform1i) GLFN(PFNGLUNIFORM3IPROC, glUniform3i) \
    GLFN(PFNGLUNIFORM4IPROC, glUniform4i) GLFN(PFNGLGENBUFFERSPROC, glGenBuffers) GLFN(PFNGLBINDBUFFERPROC, glBindBuffer) \
    GLFN(PFNGLBUFFERDATAPROC, glBufferData) GLFN(PFNGLVERTEXATTRIBPOINTERPROC, glVertexAttribPointer) \
    GLFN(PFNGLENABLEVERTEXATTRIBARRAYPROC, glEnableVertexAttribArray) GLFN(PFNGLDISABLEVERTEXATTRIBARRAYPROC, glDisableVertexAttribArray) \
    GLFN(PFNGLGENFRAMEBUFFERSPROC, glGenFramebuffers) GLFN(PFNGLBINDFRAMEBUFFERPROC, glBindFramebuffer) \
    GLFN(PFNGLFRAMEBUFFERTEXTURE2DPROC, glFramebufferTexture2D) GLFN(PFNGLCHECKFRAMEBUFFERSTATUSPROC, glCheckFramebufferStatus) \
    GLFN(PFNGLACTIVETEXTUREPROC, glActiveTexture) GLFN(PFNGLBLENDEQUATIONSEPARATEPROC, glBlendEquationSeparate) \
    GLFN(PFNGLBLENDFUNCSEPARATEPROC, glBlendFuncSeparate) GLFN(PFNGLUNIFORM1FPROC, glUniform1f) GLFN(PFNGLBLENDEQUATIONPROC, glBlendEquation)
GLFNS
#undef GLFN
static int load_fns(void)
{
    int ok = 1;
#define GLFN(T, n) if (!(p_##n = (T)SDL_GL_GetProcAddress(#n))) { fprintf(stderr, "[GPU] no %s\n", #n); ok = 0; }
    GLFNS
#undef GLFN
    return ok;
}

/* the ROM data, owned by src/tc2_render.c */
void tc2_render_assets(const uint8_t **texrom, const uint32_t **tm_decoded, const uint8_t **tattr);

static const char *vs_poly =
    "#version 130\n"
    "in vec4 a_pos; in vec3 a_uvs; in vec4 a_par; in vec4 a_clip; in vec2 a_ext;\n"
    "out vec3 v_uvs; flat out vec4 v_par; flat out vec4 v_clip; flat out vec2 v_ext;\n"
    "void main() { gl_Position = a_pos; v_uvs = a_uvs; v_par = a_par; v_clip = a_clip; v_ext = a_ext; }\n";
static const char *fs_poly =
    "#version 130\n"
    "uniform sampler2D t_rom, t_map, t_pal, t_sten, t_near, t_nearz;\n"
    "uniform int u_ex, u_scale, u_h, u_prio, u_pfade, u_ff, u_alpha, u_alpha_pen, u_near, u_depth;\n"
    "uniform float u_keep;\n"
    "uniform ivec3 u_pcol, u_fcol;\n"
    "in vec3 v_uvs; flat in vec4 v_par; flat in vec4 v_clip; flat in vec2 v_ext;\n"
    "uint b8(vec4 t, int c) { return uint(t[c] * 255.0 + 0.5); }\n"
    "void main() {\n"
    "  ivec2 px = ivec2(int(gl_FragCoord.x) / u_scale - u_ex, int(gl_FragCoord.y) / u_scale);\n"
    "  if (px.x < int(v_clip.x) || px.x >= int(v_clip.z) || px.y < int(v_clip.y) || px.y >= int(v_clip.w)) discard;\n"
    "  int fl = int(v_par.w);\n"
    "  uint tx = uint(int(v_uvs.x)), ty = uint(int(v_uvs.y));\n"
    "  if ((fl & 1) != 0) {\n"                                            /* the stencil: a 0 bit in the C412 SRAM hides the pixel */
    "    uint offs = ((ty << 6u) | (tx >> 4u)) & 0x1FFFFu;\n"
    "    vec4 s = texelFetch(t_sten, ivec2(int(offs & 511u), int(offs >> 9u)), 0);\n"
    "    uint w = b8(s, 0) | (b8(s, 3) << 8u);\n"
    "    if (((w >> ((tx & 15u) ^ 15u)) & 1u) == 0u) discard;\n"
    "  }\n"
    "  ty += uint(v_par.z);\n"
    "  float oz = gl_FragCoord.w;\n"                                      /* 1/z: what the software renderer interpolates */
    "  if (u_near != 0) {\n"                                              /* EXACT DEPTH pre-pass: the nearest opaque pixel, its object and band */
    "    if ((fl & 2) != 0 || (u_alpha != 255 && (fl & 4) != 0)) discard;\n"
    "    gl_FragDepth = clamp(0.5 + log2(max(oz, 1e-30)) / 256.0, 0.0, 1.0);\n"
    "    int e = int(v_ext.x);\n"
    "    gl_FragColor = vec4(float(e & 255) / 255.0, float((e >> 8) & 255) / 255.0, v_ext.y / 255.0, 1.0); return;\n"
    "  }\n"
    "  if (u_depth != 0) {\n"                                             /* hidden if ANOTHER object of this band is clearly nearer here */
    "    ivec2 fp = ivec2(gl_FragCoord.xy); vec4 nn = texelFetch(t_near, fp, 0);\n"
    "    if (nn.a > 0.5) {\n"
    "      int ne = int(nn.r * 255.0 + 0.5) | (int(nn.g * 255.0 + 0.5) << 8), nb = int(nn.b * 255.0 + 0.5);\n"
    "      if (ne != int(v_ext.x) && nb == int(v_ext.y) && oz < exp2((texelFetch(t_nearz, fp, 0).r - 0.5) * 256.0) * u_keep) discard;\n"
    "    }\n"
    "  }\n"
    "  int cmode = int(v_par.y); uint pbase = uint(v_par.x); uint pmask = 0xFFu; uint pshift = 0u;\n"
    "  if ((cmode & 4) != 0) { pbase += 0xECu + uint((cmode & 8) << 1); pmask = 3u; pshift = uint(2 * (~cmode & 3)); }\n"
    "  else if ((cmode & 2) != 0) { pbase += 0xE0u + uint((cmode & 8) << 1); pmask = 15u; pshift = uint(4 * (~cmode & 1)); }\n"
    "  uint id = ((tx >> 4u) & 0xFFu) | ((ty << 4u) & 0x1FFF00u);\n"   /* the tile map: tile number and its flips */
    "  vec4 m = texelFetch(t_map, ivec2(int(id & 2047u), int(id >> 11u)), 0);\n"
    "  uint mb = b8(m, 2); uint tile = b8(m, 0) | (b8(m, 1) << 8u) | ((mb & 1u) << 16u); uint a = mb >> 1u;\n"
    "  if ((a & 1u) != 0u) ty = ~ty;\n"
    "  if ((a & 2u) != 0u) tx = ~tx;\n"
    "  if ((a & 4u) != 0u) { uint t = tx; tx = ty; ty = t; }\n"
    "  uint ri = ((tile << 8u) | ((ty << 4u) & 0xF0u) | (tx & 0x0Fu)) & 0x1FFFFFFu;\n"
    "  uint pen = b8(texelFetch(t_rom, ivec2(int(ri & 8191u), int(ri >> 13u)), 0), 0);\n"
    "  if (u_prio != 0) {\n"                                               /* the priority target: R text-or-7, G any polygon, A last was 7 */
    "    int pv = (fl >> 3) & 7;\n"
    "    gl_FragColor = vec4(pv == 7 ? 1.0 : 0.0, pv != 0 ? 1.0 : 0.0, 0.0, pv == 7 ? 1.0 : 0.0); return;\n"
    "  }\n"
    "  uint ci = pbase + ((pen >> pshift) & pmask);\n"
    "  vec4 c = texelFetch(t_pal, ivec2(int(ci & 255u), int((ci >> 8u) & 127u)), 0);\n"
    "  ivec3 col = ivec3(c.rgb * 255.0 + 0.5);\n"
    "  int sh = clamp(int(v_uvs.z), 0, 63);\n"
    "  col = (col * sh) >> 6;\n"
    "  if (u_pfade != 0) col = (col * u_pcol) >> 8;\n"
    "  if (u_ff != 255) col = (col * u_ff + u_fcol * (256 - u_ff)) >> 8;\n"
    "  bool ua = u_alpha != 255 && ((fl & 4) != 0 || int(pen) == u_alpha_pen);\n"
    "  float al = ua ? float(u_alpha) / 256.0 : ((fl & 2) != 0 ? 0.5 : 1.0);\n"
    "  gl_FragColor = vec4(vec3(col) / 255.0, al);\n"
    "}\n";
static const char *vs_text =
    "#version 130\n"
    "in vec4 a_pos;\n"
    "void main() { gl_Position = a_pos; }\n";
static const char *fs_text =
    "#version 130\n"
    "uniform sampler2D t_mix, t_pal, t_prio, t_band;\n"
    "uniform int u_ex, u_scale, u_h, u_pass, u_fade, u_ff, u_alpha, u_amask, u_c12, u_c13;\n"
    "uniform ivec3 u_fcol;\n"
    "void main() {\n"
    "  ivec2 px = ivec2(int(gl_FragCoord.x) / u_scale - u_ex, int(gl_FragCoord.y) / u_scale);\n"
    "  uint mv;\n"
    "  if (px.x < 0 || px.x >= 640) {\n"      /* widescreen: a text row opaque all the way across (the cut-scenes' letterbox) continues
                                                   into the side strips in its commonest pen (b->band); anything else stays in the 4:3 centre */
    "    vec4 a = texelFetch(t_band, ivec2(px.y, 0), 0);\n"
    "    mv = uint(a.r * 255.0 + 0.5) | (uint(a.a * 255.0 + 0.5) << 8u);\n"
    "  } else {\n"
    "    vec4 m = texelFetch(t_mix, ivec2(px.x, px.y), 0);\n"
    "    mv = uint(m.r * 255.0 + 0.5) | (uint(m.a * 255.0 + 0.5) << 8u);\n"
    "  }\n"
    "  if ((mv & 0x8000u) == 0u) discard;\n"
    "  if (u_pass == 0) { gl_FragColor = vec4(1.0, 0.0, 0.0, 0.0); return; }\n"       /* the priority target starts as 'text here' */
    "  if (u_pass == 2) { vec4 p = texelFetch(t_prio, ivec2(gl_FragCoord.xy), 0); if (!(p.r > 0.5 && p.g > 0.5 && p.a < 0.5)) discard; }\n"
    "  uint pen = mv & 0x7FFFu;\n"
    "  ivec3 col = ivec3(texelFetch(t_pal, ivec2(int(pen & 255u), int(pen >> 8u)), 0).rgb * 255.0 + 0.5);\n"
    "  if (u_fade != 0) col = (col * u_ff + u_fcol * (256 - u_ff)) >> 8;\n"
    "  bool ua = u_alpha != 255 && (int(pen & 15u) == u_amask || (int(pen) >= u_c12 && int(pen) <= u_c13));\n"
    "  gl_FragColor = vec4(vec3(col) / 255.0, ua ? float(u_alpha) / 256.0 : 1.0);\n"
    "}\n";

static const char *fs_gamma =
    "#version 130\n"
    "uniform sampler2D t_src, t_lut;\n"
    "void main() {\n"
    "  vec3 c = texelFetch(t_src, ivec2(gl_FragCoord.xy), 0).rgb;\n"
    "  ivec3 i = ivec3(c * 255.0 + 0.5);\n"
    "  gl_FragColor = vec4(texelFetch(t_lut, ivec2(i.r, 0), 0).r, texelFetch(t_lut, ivec2(i.g, 0), 0).g, texelFetch(t_lut, ivec2(i.b, 0), 0).b, 1.0);\n"
    "}\n";
static int ok, scale = 1, ex, fbw, fbh;                  /* ex: widescreen margin each side, in board pixels (0 = 4:3) */
static GLuint prog_poly, prog_text, prog_gamma, vbo, quad_vbo, t_out, fbo_out, t_lut;
static GLint ug[2];
static GLuint t_band, t_rom, t_map, t_pal, t_sten, t_mix, t_col, t_prio, fbo_col, fbo_prio;
static GLint up[20], ut[16];
static GLuint t_near, t_nearz, fbo_near;          /* the exact-depth pre-pass target: nearest object / band (colour), its 1/z (depth) */
enum { A_POS, A_UVS, A_PAR, A_CLIP, A_EXT };
#define VFLOATS 17
static float *verts; static size_t vcap;

static GLuint shader(GLenum kind, const char *src)
{
    const GLuint s = p_glCreateShader(kind);
    p_glShaderSource(s, 1, &src, NULL); p_glCompileShader(s);
    GLint good = 0; p_glGetShaderiv(s, GL_COMPILE_STATUS, &good);
    if (!good) { char log[2048]; p_glGetShaderInfoLog(s, sizeof log, NULL, log); fprintf(stderr, "[GPU] shader: %s\n", log); return 0; }
    return s;
}
static GLuint program(const char *vs, const char *fs)
{
    const GLuint v = shader(GL_VERTEX_SHADER, vs), f = shader(GL_FRAGMENT_SHADER, fs);
    if (!v || !f) return 0;
    const GLuint p = p_glCreateProgram();
    p_glAttachShader(p, v); p_glAttachShader(p, f);
    p_glBindAttribLocation(p, A_POS, "a_pos"); p_glBindAttribLocation(p, A_UVS, "a_uvs");
    p_glBindAttribLocation(p, A_PAR, "a_par"); p_glBindAttribLocation(p, A_CLIP, "a_clip"); p_glBindAttribLocation(p, A_EXT, "a_ext");
    p_glLinkProgram(p);
    GLint good = 0; p_glGetProgramiv(p, GL_LINK_STATUS, &good);
    if (!good) { char log[2048]; p_glGetProgramInfoLog(p, sizeof log, NULL, log); fprintf(stderr, "[GPU] link: %s\n", log); return 0; }
    return p;
}
static GLuint texture(GLint ifmt, int w, int h, GLenum fmt, const void *data)
{
    GLuint t; glGenTextures(1, &t); glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, ifmt, w, h, 0, fmt, GL_UNSIGNED_BYTE, data);
    return t;
}
static int targets(void)
{
    fbw = (640 + 2 * ex) * scale; fbh = 480 * scale;
    if (!t_col) { glGenTextures(1, &t_col); glGenTextures(1, &t_prio); glGenTextures(1, &t_out);
                  p_glGenFramebuffers(1, &fbo_col); p_glGenFramebuffers(1, &fbo_prio); p_glGenFramebuffers(1, &fbo_out); }
    const GLuint tt[3] = { t_col, t_prio, t_out }, ff[3] = { fbo_col, fbo_prio, fbo_out };
    for (int i = 0; i < 3; i++) {
        glBindTexture(GL_TEXTURE_2D, tt[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, fbw, fbh, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        p_glBindFramebuffer(GL_FRAMEBUFFER, ff[i]);
        p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tt[i], 0);
        if (p_glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) { fprintf(stderr, "[GPU] render target %dx%d incomplete\n", fbw, fbh); p_glBindFramebuffer(GL_FRAMEBUFFER, 0); return 0; }
    }
    if (!t_near) { glGenTextures(1, &t_near); glGenTextures(1, &t_nearz); p_glGenFramebuffers(1, &fbo_near); }
    glBindTexture(GL_TEXTURE_2D, t_near);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, fbw, fbh, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glBindTexture(GL_TEXTURE_2D, t_nearz);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, 0x884C /* GL_TEXTURE_COMPARE_MODE */, 0 /* GL_NONE: sample the value */);
    glTexImage2D(GL_TEXTURE_2D, 0, 0x8CAC /* GL_DEPTH_COMPONENT32F */, fbw, fbh, 0, GL_DEPTH_COMPONENT, GL_FLOAT, NULL);
    p_glBindFramebuffer(GL_FRAMEBUFFER, fbo_near);
    p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t_near, 0);
    p_glFramebufferTexture2D(GL_FRAMEBUFFER, 0x8D00 /* GL_DEPTH_ATTACHMENT */, GL_TEXTURE_2D, t_nearz, 0);
    if (p_glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) { fprintf(stderr, "[GPU] exact-depth target incomplete: depth off\n"); fbo_near = 0; }
    p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return 1;
}

int tc2_gl_ok(void) { return ok; }
int tc2_gl_scale(void) { return scale; }
int tc2_gl_wide(void) { return ex; }
/* widescreen: the picture is (640 + 2 * e) x 480 board pixels; e = 0 is the board's 4:3 */
void tc2_gl_set_wide(int e)
{
    if (e < 0) e = 0;
    if (e > 400) e = 400;
    if (e == ex) return;
    ex = e;
    if (ok && !targets()) { ex = 0; targets(); }
    fprintf(stderr, "[GPU] picture %d x %d board pixels (%s)\n", 640 + 2 * ex, 480, ex ? "widescreen" : "4:3");
}
void tc2_gl_set_scale(int s)
{
    if (s < 1) s = 1;
    if (s > 4) s = 4;
    if (s == scale) return;
    scale = s;
    if (ok && !targets()) { scale = 1; targets(); }
    fprintf(stderr, "[GPU] internal resolution %d x %d\n", fbw, fbh);
}

int tc2_gl_init(void)
{
    ok = 0;
    const char *e = getenv("TC2_GPU");
    if (e && *e == '0') { fprintf(stderr, "[GPU] off (TC2_GPU=0): the software picture\n"); return 0; }
    const char *sl = (const char *)glGetString(GL_SHADING_LANGUAGE_VERSION);
    int maj = 0, min = 0; if (sl) sscanf(sl, "%d.%d", &maj, &min);
    GLint maxtex = 0; glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxtex);
    if (maj * 100 + min < 130 || maxtex < 8192) {
        fprintf(stderr, "[GPU] needs GLSL 1.30 and 8192-texel textures (have GLSL %s, %d): the software picture\n", sl ? sl : "?", maxtex);
        return 0;
    }
    if (!load_fns()) return 0;
    if (!(prog_poly = program(vs_poly, fs_poly)) || !(prog_text = program(vs_text, fs_text)) || !(prog_gamma = program(vs_text, fs_gamma))) return 0;
    ug[0] = p_glGetUniformLocation(prog_gamma, "t_src"); ug[1] = p_glGetUniformLocation(prog_gamma, "t_lut");
    static const char *pn[] = { "t_rom", "t_map", "t_pal", "t_sten", "u_scale", "u_h", "u_prio", "u_pfade", "u_ff", "u_alpha", "u_alpha_pen", "u_pcol", "u_fcol", "u_ex",
                                "t_near", "t_nearz", "u_near", "u_depth", "u_keep" };
    for (int i = 0; i < 19; i++) up[i] = p_glGetUniformLocation(prog_poly, pn[i]);
    static const char *tn[] = { "t_mix", "t_pal", "t_prio", "u_scale", "u_h", "u_pass", "u_fade", "u_ff", "u_alpha", "u_amask", "u_c12", "u_c13", "u_fcol", "u_ex", "t_band" };
    for (int i = 0; i < 15; i++) ut[i] = p_glGetUniformLocation(prog_text, tn[i]);

    const uint8_t *texrom; const uint32_t *tm; const uint8_t *tattr;
    tc2_render_assets(&texrom, &tm, &tattr);
    if (!texrom) { fprintf(stderr, "[GPU] no ROM data\n"); return 0; }
    t_rom = texture(GL_LUMINANCE8, 8192, 4096, GL_LUMINANCE, texrom);
    uint8_t *map = malloc(0x200000u * 3);
    for (uint32_t id = 0; id < 0x200000u; id++) {
        const uint32_t tile = tm[id] >> 8;
        map[3 * id] = (uint8_t)tile; map[3 * id + 1] = (uint8_t)(tile >> 8); map[3 * id + 2] = (uint8_t)((tile >> 16 & 1) | tattr[id] << 1);
    }
    t_map = texture(GL_RGB8, 2048, 1024, GL_RGB, map);
    free(map);
    t_pal = texture(GL_RGBA8, 256, 128, GL_BGRA, NULL);
    t_sten = texture(GL_LUMINANCE8_ALPHA8, 512, 256, GL_LUMINANCE_ALPHA, NULL);
    t_mix = texture(GL_LUMINANCE8_ALPHA8, 640, 480, GL_LUMINANCE_ALPHA, NULL);
    t_band = texture(GL_LUMINANCE8_ALPHA8, 480, 1, GL_LUMINANCE_ALPHA, NULL);
    t_lut = texture(GL_RGB8, 256, 1, GL_RGB, NULL);
    p_glGenBuffers(1, &vbo); p_glGenBuffers(1, &quad_vbo);
    static const float quad[] = { -1, -1, 0, 1,  1, -1, 0, 1,  1, 1, 0, 1,  -1, -1, 0, 1,  1, 1, 0, 1,  -1, 1, 0, 1 };
    p_glBindBuffer(GL_ARRAY_BUFFER, quad_vbo); p_glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
    p_glBindBuffer(GL_ARRAY_BUFFER, 0);
    { const char *s = getenv("TC2_RES"); if (s) scale = atoi(s) < 1 ? 1 : atoi(s) > 4 ? 4 : atoi(s); }
    { const char *s = getenv("TC2_WIDE"); if (s) { const float a = (float)atof(s); ex = a > 1.34f ? (int)(480 * a / 2 + 0.5f) - 320 : 0; } }   /* tests: TC2_WIDE=1.7778 */
    if (!targets()) return 0;
    GLenum err = glGetError();
    if (err != GL_NO_ERROR) { fprintf(stderr, "[GPU] GL error %04X during setup: the software picture\n", err); return 0; }
    ok = 1;
    fprintf(stderr, "[GPU] the picture is drawn on the GPU (%s, GLSL %s), internal resolution %d x %d\n", (const char *)glGetString(GL_RENDERER), sl, fbw, fbh);
    return 1;
}

/* the frame's polygons as triangles, in draw order: per vertex its clip position (x*w, y*w, 0, w with w = z, so GL interpolates u, v and
 * the shade perspective-correct exactly as the hardware's u/z, v/z, i/z), u v shade, and the polygon's parameters and viewport clip */
static int fill_vertices(const tc2_built *b)
{
    size_t need = 0;
    for (int i = 0; i < b->npoly; i++) need += (size_t)(b->polys[i].n - 2) * 3;
    if (need * VFLOATS > vcap) { vcap = need * VFLOATS * 2 + 4096; verts = realloc(verts, vcap * sizeof *verts); }
    float *o = verts;
    for (int i = 0; i < b->npoly; i++) {
        const tc2_poly *p = &b->polys[b->order[i]];
        const int cl = (320 - p->vp_size_x) + p->vp_offset_x, cr = (320 + p->vp_size_x) + p->vp_offset_x;
        const int ct = (240 - p->vp_size_y) - p->vp_offset_y, cb = (240 + p->vp_size_y) - p->vp_offset_y;
        /* widescreen (Hor+): a viewport that fills the 4:3 screen fills the wide one -- same camera, more world at the sides */
        const int full = ex && cl <= 0 && cr >= 640;
        const int ccl = full ? -ex : cl, ccr = full ? 640 + ex : cr;
        const float flags = (float)(p->stencil | p->blend << 1 | p->alpha_enabled << 2 | (p->prioverchar & 7) << 3);
        for (int k = 1; k + 1 < p->n; k++) {
            const int tri[3] = { 0, k, k + 1 };
            for (int j = 0; j < 3; j++) {
                const tc2_pvert *v = &p->pv[tri[j]];
                const float ooz = v->p[0] != 0 ? v->p[0] : 1e-30f, w = 1.0f / ooz;
                const float sx = v->x + (float)cl, sy = v->y + (float)ct;
                *o++ = ((sx + (float)ex) / (320.0f + (float)ex) - 1.0f) * w; *o++ = (sy / 240.0f - 1.0f) * w; *o++ = 0; *o++ = w;
                *o++ = v->p[1] * w; *o++ = v->p[2] * w; *o++ = v->p[3] * w;
                *o++ = (float)p->color_base; *o++ = (float)p->cmode; *o++ = (float)p->tbase; *o++ = flags;
                *o++ = (float)ccl; *o++ = (float)ct; *o++ = (float)ccr; *o++ = (float)cb;
                *o++ = (float)((p->entry & 0x3FFF) + 1); *o++ = (float)(p->zkey >> 21);
            }
        }
    }
    return (int)need;
}
int tc2_depth_on(void); float tc2_depth_keep(void);   /* src/tc2_render.c: the exact-depth setting */
static void bind_tex(int unit, GLuint t) { p_glActiveTexture(GL_TEXTURE0 + unit); glBindTexture(GL_TEXTURE_2D, t); }
static void draw_quad(void)
{
    p_glBindBuffer(GL_ARRAY_BUFFER, quad_vbo);
    p_glEnableVertexAttribArray(A_POS); p_glVertexAttribPointer(A_POS, 4, GL_FLOAT, GL_FALSE, 0, NULL);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    p_glDisableVertexAttribArray(A_POS);
}
static void text_pass(const tc2_built *b, int pass)
{
    const tc2_frame_state *fs = &b->fs;
#define C4(w) (fs->c404[2 * (w) + 1])
    p_glUseProgram(prog_text);
    p_glUniform1i(ut[0], 0); p_glUniform1i(ut[1], 1); p_glUniform1i(ut[2], 2);
    p_glUniform1i(ut[3], scale); p_glUniform1i(ut[4], fbh); p_glUniform1i(ut[5], pass);
    p_glUniform1i(ut[6], (C4(0x1A) & 2) && C4(0x19)); p_glUniform1i(ut[7], 0xFF - C4(0x19));
    p_glUniform1i(ut[8], 0xFF - C4(0x15)); p_glUniform1i(ut[9], C4(0x14)); p_glUniform1i(ut[10], C4(0x12)); p_glUniform1i(ut[11], C4(0x13));
    p_glUniform3i(ut[12], C4(0x16), C4(0x17), C4(0x18)); p_glUniform1i(ut[13], ex); p_glUniform1i(ut[14], 3);
#undef C4
    bind_tex(0, t_mix); bind_tex(1, t_pal); bind_tex(2, t_prio); bind_tex(3, t_band);
    draw_quad();
}

unsigned tc2_gl_draw(const tc2_built *b)
{
    if (!ok || !b) return t_out;
    GLint vp[4]; glGetIntegerv(GL_VIEWPORT, vp);
    const tc2_fx *f = &b->fx;
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    bind_tex(0, t_pal); glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 128, GL_BGRA, GL_UNSIGNED_BYTE, b->fs.pens);
    bind_tex(0, t_sten); glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 512, 256, GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, b->fs.sram);
    if (f->layers & 4) {
        bind_tex(0, t_mix); glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 640, 480, GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, b->mix);
        bind_tex(0, t_band); glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 480, 1, GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, b->band);
    }
    glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE); glDisable(GL_SCISSOR_TEST); glDisable(GL_TEXTURE_2D); glDisable(GL_ALPHA_TEST);
    glViewport(0, 0, fbw, fbh);
    /* 1. the background */
    p_glBindFramebuffer(GL_FRAMEBUFFER, fbo_col);
    glClearColor((f->bg >> 16 & 0xFF) / 255.0f, (f->bg >> 8 & 0xFF) / 255.0f, (f->bg & 0xFF) / 255.0f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    p_glBindFramebuffer(GL_FRAMEBUFFER, fbo_prio);
    glClearColor(0, 0, 0, 0); glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_BLEND);
    /* 2. the text layer, under everything (and 'text here' into the priority target) */
    if (f->layers & 4) {
        p_glBlendEquation(GL_FUNC_ADD); glBlendFunc(GL_ONE, GL_ZERO);
        text_pass(b, 0);
        p_glBindFramebuffer(GL_FRAMEBUFFER, fbo_col);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        text_pass(b, 1);
    }
    /* 3. + 4. the polygons, one draw call each into the picture and into the priority target */
    const int nv = fill_vertices(b);
    if (nv > 0) {
        p_glBindBuffer(GL_ARRAY_BUFFER, vbo);
        p_glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(nv * VFLOATS * sizeof(float)), NULL, GL_STREAM_DRAW);       /* orphan: no stall */
        p_glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(nv * VFLOATS * sizeof(float)), verts, GL_STREAM_DRAW);
        const GLsizei st = VFLOATS * sizeof(float);
        p_glEnableVertexAttribArray(A_POS); p_glVertexAttribPointer(A_POS, 4, GL_FLOAT, GL_FALSE, st, (void *)0);
        p_glEnableVertexAttribArray(A_UVS); p_glVertexAttribPointer(A_UVS, 3, GL_FLOAT, GL_FALSE, st, (void *)(4 * sizeof(float)));
        p_glEnableVertexAttribArray(A_PAR); p_glVertexAttribPointer(A_PAR, 4, GL_FLOAT, GL_FALSE, st, (void *)(7 * sizeof(float)));
        p_glEnableVertexAttribArray(A_CLIP); p_glVertexAttribPointer(A_CLIP, 4, GL_FLOAT, GL_FALSE, st, (void *)(11 * sizeof(float)));
        p_glEnableVertexAttribArray(A_EXT); p_glVertexAttribPointer(A_EXT, 2, GL_FLOAT, GL_FALSE, st, (void *)(15 * sizeof(float)));
        p_glUseProgram(prog_poly);
        p_glUniform1i(up[0], 0); p_glUniform1i(up[1], 1); p_glUniform1i(up[2], 2); p_glUniform1i(up[3], 3);
        p_glUniform1i(up[4], scale); p_glUniform1i(up[5], fbh);
        p_glUniform1i(up[7], f->pr || f->pg || f->pb); p_glUniform1i(up[8], (f->fflags & 1) ? 0xFF - f->ffactor : 0xFF);
        p_glUniform1i(up[9], 0xFF - f->alpha); p_glUniform1i(up[10], f->alpha_pen);
        p_glUniform3i(up[11], f->pr, f->pg, f->pb); p_glUniform3i(up[12], f->fr, f->fg, f->fb); p_glUniform1i(up[13], ex);
        bind_tex(0, t_rom); bind_tex(1, t_map); bind_tex(2, t_pal); bind_tex(3, t_sten);
        p_glUniform1i(up[14], 4); p_glUniform1i(up[15], 5); p_glUniform1i(up[16], 0);
        const int depth = tc2_depth_on() && fbo_near;
        p_glUniform1i(up[17], 0); p_glUniform1f(up[18], tc2_depth_keep());
        if (depth) {                                     /* EXACT DEPTH pre-pass: nearest opaque object / band / 1/z per pixel */
            bind_tex(4, 0); bind_tex(5, 0);
            p_glBindFramebuffer(GL_FRAMEBUFFER, fbo_near);
            glClearColor(0, 0, 0, 0); glClearDepth(0.0); glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            glDisable(GL_BLEND); glEnable(GL_DEPTH_TEST); glDepthFunc(GL_GREATER); glDepthMask(GL_TRUE);
            p_glUniform1i(up[6], 0); p_glUniform1i(up[16], 1);
            glDrawArrays(GL_TRIANGLES, 0, nv);
            glDisable(GL_DEPTH_TEST); glDepthFunc(GL_LESS); glClearDepth(1.0); glEnable(GL_BLEND);
            p_glUniform1i(up[16], 0); p_glUniform1i(up[17], 1);
            bind_tex(4, t_near); bind_tex(5, t_nearz);
        }
        p_glBindFramebuffer(GL_FRAMEBUFFER, fbo_col);
        p_glUniform1i(up[6], 0);
        p_glBlendEquation(GL_FUNC_ADD); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDrawArrays(GL_TRIANGLES, 0, nv);
        if (f->layers & 4) {
            p_glBindFramebuffer(GL_FRAMEBUFFER, fbo_prio);
            p_glUniform1i(up[6], 1);
            p_glBlendEquationSeparate(GL_MAX, GL_FUNC_ADD); p_glBlendFuncSeparate(GL_ONE, GL_ONE, GL_ONE, GL_ZERO);
            glDrawArrays(GL_TRIANGLES, 0, nv);
            p_glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
        }
        for (int a = A_POS; a <= A_EXT; a++) p_glDisableVertexAttribArray((GLuint)a);
        bind_tex(4, 0); bind_tex(5, 0); p_glActiveTexture(GL_TEXTURE0);
    }
    /* 5. the text the polygons let through again */
    if (f->layers & 4) {
        p_glBindFramebuffer(GL_FRAMEBUFFER, fbo_col);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        text_pass(b, 2);
    }
    /* 6. the C404's GAMMA (the game's own R / G / B curves), into the picture the window shows */
    {
        uint8_t lut[256 * 3];
        for (int i = 0; i < 256; i++) { lut[3 * i] = b->fs.gamma[0][i]; lut[3 * i + 1] = b->fs.gamma[1][i]; lut[3 * i + 2] = b->fs.gamma[2][i]; }
        bind_tex(0, t_lut); glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 1, GL_RGB, GL_UNSIGNED_BYTE, lut);
        glDisable(GL_BLEND);
        p_glBindFramebuffer(GL_FRAMEBUFFER, fbo_out);
        p_glUseProgram(prog_gamma); p_glUniform1i(ug[0], 0); p_glUniform1i(ug[1], 1);
        bind_tex(0, t_col); bind_tex(1, t_lut);
        draw_quad();
    }
    /* back to the state the window and the menu expect */
    glDisable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    p_glUseProgram(0); p_glBindBuffer(GL_ARRAY_BUFFER, 0); p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
    for (int u = 3; u >= 0; u--) bind_tex(u, 0);
    glViewport(vp[0], vp[1], vp[2], vp[3]);
    glClearColor(0, 0, 0, 1);
    return t_out;
}

/* the last picture at 640 x 480, top row first (every scale-th pixel when the internal resolution is higher) */
void tc2_gl_read(uint32_t *rgba)
{
    if (!ok) { memset(rgba, 0, 640 * 480 * 4); return; }
    static uint8_t *buf; static size_t cap;
    const size_t n = (size_t)fbw * fbh * 4;
    if (n > cap) { buf = realloc(buf, n); cap = n; }
    p_glBindFramebuffer(GL_FRAMEBUFFER, fbo_out);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, fbw, fbh, GL_RGBA, GL_UNSIGNED_BYTE, buf);
    p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
    for (int y = 0; y < 480; y++)
        for (int x = 0; x < 640; x++) {
            const uint8_t *c = buf + 4 * ((size_t)(y * scale + scale / 2) * fbw + (size_t)((x + ex) * scale + scale / 2));
            rgba[y * 640 + x] = (uint32_t)c[0] << 16 | (uint32_t)c[1] << 8 | c[2];
        }
}
/* the whole target (fbw x fbh, RGB, top row first) -- the gate's full-resolution shot */
int tc2_gl_read_full(uint8_t *rgb, int cap)
{
    if (!ok || cap < fbw * fbh * 3) return 0;
    p_glBindFramebuffer(GL_FRAMEBUFFER, fbo_out);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, fbw, fbh, GL_RGB, GL_UNSIGNED_BYTE, rgb);
    p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return fbw;
}
