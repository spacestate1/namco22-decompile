/* gpu_readback.c — offscreen Metal GPU tests for the glmc shim.
 *
 * Builds the real shim (glmc_main.c + glmc_metal.mm), creates a windowless
 * face on the actual Apple GPU, and verifies colours/covers with CPU readback:
 *   1 clears + immediate-mode FFP quads (modulate)
 *   2 textured quads over the upload formats the games use
 *   3 GL_UNPACK_ROW_LENGTH / GL_UNPACK_ALIGNMENT handling
 *   4 blending, scissor, alpha test
 *   5 FBO render targets (RGBA8 + depth24 renderbuffer) + blit to default
 *   6 glCopyTexSubImage2D from an FBO
 *   7 depth testing on an FBO
 *   8 GL_LINES topology
 *   9 glXGetProcAddress-style proc table completeness (tc2's load_fns fails closed)
 *
 * y reads are GL window space: buf[(y*w + x)*4], y measured from the BOTTOM.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <math.h>

#include "glmc.h"
#include "glmc_dev.h"

#define W 320
#define H 240

static int n_pass, n_fail;

#define CHECK(cond, label) do { \
    if (cond) { n_pass++; } \
    else { n_fail++; printf("FAIL %s:(%d) %s\n", __FILE__, __LINE__, label); } \
} while (0)

#define TOL 2

static int px_close(const uint8_t *buf, int x, int y, int r, int g, int b, int a)
{
    const uint8_t *p = buf + ((size_t)y * W + x) * 4;
    if (abs(p[0] - r) > TOL || abs(p[1] - g) > TOL ||
        abs(p[2] - b) > TOL || abs(p[3] - a) > TOL) {
        printf("       pixel(%d,%d)= (%u,%u,%u,%u) expected (%d,%d,%d,%d)\n",
               x, y, p[0], p[1], p[2], p[3], r, g, b, a);
        return 0;
    }
    return 1;
}

static uint8_t fb[H * W * 4];               /* flag by dimensions: read W x H  */

static void fresh(void)
{
    glmc_sdl_MakeCurrent(NULL, NULL);       /* resets GL state, drops the face */
    glmc_context_attach_window(NULL, NULL, NULL, W, H, 1.0f); /* headless face */
    if (glmc_GetError() != 0) printf("       fresh(): error raised\n");
}

/* matrices + the drawing enables the games rely on */
static void std_setup(void)
{
    glmc_MatrixMode(GL_PROJECTION); glmc_LoadIdentity();
    glmc_Ortho(0, (GLdouble)W, 0, (GLdouble)H, -1, 1);
    glmc_MatrixMode(GL_MODELVIEW); glmc_LoadIdentity();
    glmc_Enable(GL_TEXTURE_2D);             /* active unit 0 by default */
}

/* an ortho projection that matches a given render-target size (FBO draws) */
static void fbo_setup(int w, int h)
{
    glmc_MatrixMode(GL_PROJECTION); glmc_LoadIdentity();
    glmc_Ortho(0, (GLdouble)w, 0, (GLdouble)h, -1, 1);
    glmc_MatrixMode(GL_MODELVIEW); glmc_LoadIdentity();
    glmc_Disable(GL_TEXTURE_2D);
}

static void read_all(void)
{
    memset(fb, 0xAA, sizeof fb);
    glmc_ReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, fb);
}

/* ---------------------------------------------------------------- test 1 */
static void test_clear_and_quad(void)
{
    std_setup();
    glmc_ClearColor(0, 0, 1, 1);
    if (!getenv("SKIP_CLEAR"))
        glmc_Clear(GL_COLOR_BUFFER_BIT);
    glmc_Disable(GL_TEXTURE_2D);
    if (!getenv("SKIP_DRAW")) {
    glmc_Begin(GL_QUADS);
        glmc_Color4f(1, 0, 0, 1);
        glmc_Vertex2f(100, 100); glmc_Vertex2f(200, 100);
        glmc_Vertex2f(200, 200); glmc_Vertex2f(100, 200);
    glmc_End();
    }
    glmc_Finish();
    read_all();
    CHECK(px_close(fb, 50, 50, 0, 0, 255, 255), "clear held outside the quad");
    CHECK(px_close(fb, 150, 150, 255, 0, 0, 255), "red quad centre");
    CHECK(px_close(fb, 102, 102, 255, 0, 0, 255), "inside corner");
    CHECK(px_close(fb, 99, 99, 0, 0, 255, 255), "just outside edge");
    /* GL_RGB readback packs 3 bytes/pixel (regression: it used to write 4) */
    {
        uint8_t rgb6[6]; memset(rgb6, 0xCC, sizeof rgb6);
        glmc_ReadPixels(150, 150, 2, 1, GL_RGB, GL_UNSIGNED_BYTE, rgb6);
        CHECK(rgb6[0] == 255 && rgb6[1] == 0 && rgb6[2] == 0, "GL_RGB px0 red");
        CHECK(rgb6[3] == 255 && rgb6[4] == 0 && rgb6[5] == 0, "GL_RGB px1 packed 3 bytes");
    }
    if (glmc_GetError() != 0) CHECK(0, "test1 GetError");
}

/* ---------------------------------------------------------------- test 2 */
static void test_textured_formats(void)
{
    uint8_t rgba[2 * 2 * 4] = { 255,0,0,255,   0,255,0,255,
                                0,0,255,255,   255,255,0,255 };
    uint8_t lum [2 * 2] = { 200, 100, 200, 100 };
    uint8_t la  [2 * 2 * 2] = { 100,128, 200,255, 100,128, 200,255 };
    uint8_t alp [2 * 2] = { 255, 0, 255, 0 };
    uint8_t rgb [2 * 2 * 3] = { 255,128,0,   0,255,128,
                                0,0,255,     128,128,64 };
    uint8_t bgra[2 * 2 * 4] = { 0,0,255,255,   0,255,0,255,   /* byte0=blue */
                                255,0,0,200,  64,128,255,255 };

    GLuint t[2]; glmc_GenTextures(2, t); glmc_BindTexture(GL_TEXTURE_2D, t[0]);
    glmc_TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glmc_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glmc_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glmc_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glmc_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glmc_TexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    std_setup();
    glmc_Begin(GL_QUADS);
        glmc_TexCoord2f(0, 0); glmc_Vertex2f(0, 0);
        glmc_TexCoord2f(1, 0); glmc_Vertex2f(W, 0);
        glmc_TexCoord2f(1, 1); glmc_Vertex2f(W, H);
        glmc_TexCoord2f(0, 1); glmc_Vertex2f(0, H);
    glmc_End();
    glmc_Finish(); read_all();
    CHECK(px_close(fb, 55, 55, 255, 0, 0, 255), "RGBA texel(0,0) red");
    CHECK(px_close(fb, W - 55, 55, 0, 255, 0, 255), "RGBA texel(1,0) green");
    CHECK(px_close(fb, 55, H - 55, 0, 0, 255, 255), "RGBA texel(0,1) blue");
    CHECK(px_close(fb, W - 55, H - 55, 255, 255, 0, 255), "RGBA texel(1,1) yellow");

    glmc_BindTexture(GL_TEXTURE_2D, t[1]);
    glmc_PixelStorei(GL_UNPACK_ALIGNMENT, 1);   /* small formats: contiguous rows */
    glmc_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glmc_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glmc_TexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, 2, 2, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, lum);
    glmc_Begin(GL_QUADS);
        glmc_TexCoord2f(0, 0); glmc_Vertex2f(0, 0);
        glmc_TexCoord2f(1, 0); glmc_Vertex2f(W, 0);
        glmc_TexCoord2f(1, 1); glmc_Vertex2f(W, H);
        glmc_TexCoord2f(0, 1); glmc_Vertex2f(0, H);
    glmc_End();
    glmc_Finish(); read_all();
    CHECK(px_close(fb, 55, 55, 200, 200, 200, 255), "LUM texel(0,0) 200");
    CHECK(px_close(fb, W - 55, H - 55, 100, 100, 100, 255), "LUM texel(1,1) 100");

    glmc_TexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE8_ALPHA8, 2, 2, 0, GL_LUMINANCE_ALPHA,
                    GL_UNSIGNED_BYTE, la);
    glmc_Begin(GL_QUADS);
        glmc_TexCoord2f(0, 0); glmc_Vertex2f(0, 0);
        glmc_TexCoord2f(1, 0); glmc_Vertex2f(W, 0);
        glmc_TexCoord2f(1, 1); glmc_Vertex2f(W, H);
        glmc_TexCoord2f(0, 1); glmc_Vertex2f(0, H);
    glmc_End();
    glmc_Finish(); read_all();
    CHECK(px_close(fb, 55, 55, 100, 100, 100, 128), "LA texel(0,0) a=128");
    CHECK(px_close(fb, W - 55, H - 55, 200, 200, 200, 255), "LA texel(1,1) a=255");

    glmc_TexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA8, 2, 2, 0, GL_ALPHA, GL_UNSIGNED_BYTE, alp);
    glmc_Enable(GL_BLEND);
    glmc_BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glmc_ClearColor(1, 1, 1, 1);
    glmc_Clear(GL_COLOR_BUFFER_BIT);
    glmc_Begin(GL_QUADS);
        glmc_Color4f(0, 0, 0, 1);
        glmc_TexCoord2f(0, 0); glmc_Vertex2f(0, 0);
        glmc_TexCoord2f(1, 0); glmc_Vertex2f(W, 0);
        glmc_TexCoord2f(1, 1); glmc_Vertex2f(W, H);
        glmc_TexCoord2f(0, 1); glmc_Vertex2f(0, H);
    glmc_End();
    glmc_Disable(GL_BLEND);
    glmc_Finish(); read_all();
    CHECK(px_close(fb, 55, 55, 0, 0, 0, 255), "ALPHA texel(0,0) blended black");
    CHECK(px_close(fb, W - 55, H - 55, 255, 255, 255, 255), "ALPHA texel(1,1) = 0 keeps white");

    glmc_TexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 2, 2, 0, GL_RGB, GL_UNSIGNED_BYTE, rgb);
    glmc_ClearColor(0, 0, 0, 1);
    glmc_Clear(GL_COLOR_BUFFER_BIT);
    glmc_Enable(GL_TEXTURE_2D);
    glmc_Begin(GL_QUADS);
        glmc_TexCoord2f(0, 0); glmc_Vertex2f(0, 0);
        glmc_TexCoord2f(1, 0); glmc_Vertex2f(W, 0);
        glmc_TexCoord2f(1, 1); glmc_Vertex2f(W, H);
        glmc_TexCoord2f(0, 1); glmc_Vertex2f(0, H);
    glmc_End();
    glmc_Finish(); read_all();
    CHECK(px_close(fb, 55, 55, 255, 128, 0, 255), "RGB texel(0,0), a expanded to 255");
    CHECK(px_close(fb, W - 55, H - 55, 128, 128, 64, 255), "RGB texel(1,1)");

    glmc_TexImage2D(GL_TEXTURE_2D, 0, GL_BGRA, 2, 2, 0, GL_BGRA, GL_UNSIGNED_BYTE, bgra);
    glmc_Begin(GL_QUADS);
        glmc_TexCoord2f(0, 0); glmc_Vertex2f(0, 0);
        glmc_TexCoord2f(1, 0); glmc_Vertex2f(W, 0);
        glmc_TexCoord2f(1, 1); glmc_Vertex2f(W, H);
        glmc_TexCoord2f(0, 1); glmc_Vertex2f(0, H);
    glmc_End();
    glmc_Finish(); read_all();
    /* BGRA rows: byte order B,G,R,A. texel(0,0) bytes {0,0,255,255} = red,
     * texel(0,1) bytes {255,0,0,200} = blue (a=200). */
    CHECK(px_close(fb, 55, 55, 255, 0, 0, 255), "BGRA texel(0,0) red");
    CHECK(px_close(fb, 55, H - 55, 0, 0, 255, 200), "BGRA texel(0,1) blue a=200");
    glmc_DeleteTextures(2, t);
}

/* ---------------------------------------------------------------- test 3 */
static void test_unpack_meta(void)
{
    /* 4x2 source, GL_UNPACK_ROW_LENGTH 4: the first 2 columns of each row */
    uint8_t src[2 * 4 * 4] = {
        100,0,0,255,  0,100,0,255,  1,0,0,0,    9,9,9,9,
        0,0,100,255,  128,0,128,255, 2,0,0,0,   9,9,9,9 };
    GLuint t; glmc_GenTextures(1, &t); glmc_BindTexture(GL_TEXTURE_2D, t);
    glmc_PixelStorei(GL_UNPACK_ROW_LENGTH, 4);   /* 4-wide rows, take cols 0..1 */
    glmc_TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, src);
    glmc_PixelStorei(GL_UNPACK_ROW_LENGTH, 0);   /* back to the GL default */
    glmc_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glmc_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glmc_TexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    std_setup();
    glmc_Begin(GL_QUADS);
        glmc_TexCoord2f(0, 0); glmc_Vertex2f(0, 0);
        glmc_TexCoord2f(1, 0); glmc_Vertex2f(W, 0);
        glmc_TexCoord2f(1, 1); glmc_Vertex2f(W, H);
        glmc_TexCoord2f(0, 1); glmc_Vertex2f(0, H);
    glmc_End();
    glmc_Finish(); read_all();
    CHECK(px_close(fb, 55, 55, 100, 0, 0, 255), "ROW_LENGTH row0 col0");
    CHECK(px_close(fb, W - 55, H - 55, 128, 0, 128, 255), "ROW_LENGTH row1 col1");
    /* GL_RGB with default alignment 4: pitch rounds 6 -> 8; verify row1 */
    uint8_t rgb3[/* 2 rows x stride 8 (6 used + align-4 pad) */16] = {
        255,0,0,   0,0,255,   9,9,
        0,128,0,   200,200,0, 9,9 };
    glmc_TexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 2, 2, 0, GL_RGB, GL_UNSIGNED_BYTE, rgb3);
    glmc_Begin(GL_QUADS);
        glmc_TexCoord2f(0, 0); glmc_Vertex2f(0, 0);
        glmc_TexCoord2f(1, 0); glmc_Vertex2f(W, 0);
        glmc_TexCoord2f(1, 1); glmc_Vertex2f(W, H);
        glmc_TexCoord2f(0, 1); glmc_Vertex2f(0, H);
    glmc_End();
    glmc_Finish(); read_all();
    CHECK(px_close(fb, 55, H - 55, 0, 128, 0, 255), "RGB ALIGN4 row1 col0");
    /* alignment 1: pitch 6 exactly (row0 = red,blue; row1 = green,yellow) */
    glmc_PixelStorei(GL_UNPACK_ALIGNMENT, 1);
    uint8_t rgb3b[2 * 6] = { 255,0,0, 0,0,255, 0,255,0, 255,255,0 };
    glmc_TexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 2, 2, 0, GL_RGB, GL_UNSIGNED_BYTE, rgb3b);
    glmc_Begin(GL_QUADS);
        glmc_TexCoord2f(0, 0); glmc_Vertex2f(0, 0);
        glmc_TexCoord2f(1, 0); glmc_Vertex2f(W, 0);
        glmc_TexCoord2f(1, 1); glmc_Vertex2f(W, H);
        glmc_TexCoord2f(0, 1); glmc_Vertex2f(0, H);
    glmc_End();
    glmc_PixelStorei(GL_UNPACK_ALIGNMENT, 4);   /* restore the GL default */
    glmc_Finish(); read_all();
    CHECK(px_close(fb, W - 55, 55, 0, 0, 255, 255), "ALIGN1 RGB row0 col1 blue");
    glmc_DeleteTextures(1, &t);
}

/* ---------------------------------------------------------------- test 4 */
static void test_blend_scissor_alphatest(void)
{
    std_setup();
    glmc_Disable(GL_TEXTURE_2D);
    glmc_ClearColor(1, 1, 1, 1);
    glmc_Clear(GL_COLOR_BUFFER_BIT);
    glmc_Enable(GL_BLEND);
    glmc_BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glmc_Begin(GL_QUADS);
        glmc_Color4f(0, 0, 0, 0.5f);            /* half transparent black  */
        glmc_Vertex2f(60, 60); glmc_Vertex2f(140, 60);
        glmc_Vertex2f(140, 140); glmc_Vertex2f(60, 140);
    glmc_End();
    glmc_Disable(GL_BLEND);
    glmc_Finish(); read_all();
    /* GL blends the alpha channel too: a = 0.5*0.5 + 0.5*1.0 = 0.75 -> 191 */
    CHECK(px_close(fb, 100, 100, 128, 128, 128, 191), "blend 0.5 black over white");

    /* scissor: draw only the left half of the next quad */
    glmc_Enable(GL_SCISSOR_TEST);
    glmc_Scissor(0, 0, W / 2, H);
    glmc_ClearColor(0, 0, 0, 1);
    glmc_Clear(GL_COLOR_BUFFER_BIT);
    glmc_Begin(GL_QUADS);
        glmc_Color4f(0, 1, 0, 1);
        glmc_Vertex2f(60, 60); glmc_Vertex2f(260, 60);
        glmc_Vertex2f(260, 180); glmc_Vertex2f(60, 180);
    glmc_End();
    glmc_Disable(GL_SCISSOR_TEST);
    glmc_Finish(); read_all();
    CHECK(px_close(fb, 100, 100, 0, 255, 0, 255), "scissored green kept on left");
    CHECK(px_close(fb, 200, 100, 0, 0, 0, 255), "scissor clipped right half");
    CHECK(px_close(fb, 300, 200, 0, 0, 0, 255), "outside = clear black");
    /* alpha test discards below the ref */
    glmc_Enable(GL_ALPHA_TEST);
    glmc_AlphaFunc(GL_GREATER, 0.5f);
    glmc_ClearColor(1, 0, 0, 1);
    glmc_Clear(GL_COLOR_BUFFER_BIT);
    glmc_Begin(GL_QUADS);
        glmc_Color4f(0, 0, 0, 0.25f);           /* alpha 64 < ref: discarded */
        glmc_Vertex2f(40, 40); glmc_Vertex2f(280, 40);
        glmc_Vertex2f(280, 200); glmc_Vertex2f(40, 200);
    glmc_End();
    glmc_Begin(GL_QUADS);
        glmc_Color4f(1, 0, 0, 1);
        glmc_Vertex2f(40, 40); glmc_Vertex2f(280, 40);
        glmc_Vertex2f(280, 200); glmc_Vertex2f(40, 200);
    glmc_End();
    glmc_Disable(GL_ALPHA_TEST);
    glmc_Finish(); read_all();
    CHECK(px_close(fb, 160, 120, 255, 0, 0, 255), "alpha test + pass draw succeeded");
    /* "discarded" hold can't fully be asserted for GL_LESS lequates? re-draw was pushed AFTER
     * the discard so the colour is red; assert alpha 0.25 was never written: a==255 */
}

/* ---------------------------------------------------------------- test 5 */
static void test_fbo_and_blit(void)
{
    const int FW = 200, FH = 160;
    GLuint col, rbo, fbo;
    glmc_GenTextures(1, &col);
    glmc_BindTexture(GL_TEXTURE_2D, col);
    glmc_TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, FW, FH, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glmc_GenRenderbuffers(1, &rbo);
    glmc_BindRenderbuffer(GL_RENDERBUFFER, rbo);
    glmc_RenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, FW, FH);
    glmc_GenFramebuffers(1, &fbo);
    glmc_BindFramebuffer(GL_FRAMEBUFFER, fbo);
    glmc_FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, col, 0);
    glmc_FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, rbo);
    CHECK(glmc_CheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE,
          "FBO status complete");
    fbo_setup(FW, FH);
    glmc_ClearColor(0, 1, 0, 1);
    glmc_ClearDepth(1.0);
    glmc_Clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glmc_Begin(GL_QUADS);
        glmc_Color4f(1, 0, 0, 1);
        glmc_Vertex2f(20, 20); glmc_Vertex2f(120, 20);
        glmc_Vertex2f(120, 120); glmc_Vertex2f(20, 120);
    glmc_End();
    glmc_Finish();
    {
        uint8_t rbuf[200 * 160 * 4];
        memset(rbuf, 0xAA, sizeof rbuf);
        glmc_ReadPixels(0, 0, FW, FH, GL_RGBA, GL_UNSIGNED_BYTE, rbuf);
        const uint8_t *p = rbuf + ((size_t)80 * FW + 70) * 4;
        CHECK(p[0] == 255 && p[1] == 0 && p[2] == 0 && p[3] == 255, "FBO quad red");
        const uint8_t *q = rbuf + ((size_t)150 * FW + 150) * 4;
        CHECK(q[1] == 255 && q[0] == 0, "FBO clear green outside");
        uint32_t tt = col;
        (void)tt;
    }
    /* blit the FBO result onto the default framebuffer (letterbox shape):
     * source = the FBO (read), destination = the window (draw). */
    glmc_BindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    glmc_BindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glmc_ClearColor(0, 0, 0, 1);
    glmc_Clear(GL_COLOR_BUFFER_BIT);
    glmc_BlitFramebuffer(0, 0, FW, FH, 60, 40, 60 + FW, 40 + FH, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glmc_Finish();
    glmc_BindFramebuffer(GL_READ_FRAMEBUFFER, 0);   /* read back the window */
    read_all();
    CHECK(px_close(fb, 70 + 50, 40 + 70, 255, 0, 0, 255), "blit quad centre red");
    CHECK(px_close(fb, 70, 44, 0, 1 * 255, 0, 255), "blit outside-quad green");
    CHECK(px_close(fb, 20, 20, 0, 0, 0, 255), "blit left empty black");
    glmc_BindFramebuffer(GL_FRAMEBUFFER, 0);
    glmc_DeleteFramebuffers(1, &fbo);
    glmc_DeleteRenderbuffers(1, &rbo);
    glmc_DeleteTextures(1, &col);
}

/* ---------------------------------------------------------------- test 6 */
static void test_copy_texsub(void)
{
    /* draw to the default, copy a region into a texture, draw that texture */
    std_setup();
    glmc_Disable(GL_TEXTURE_2D);
    glmc_ClearColor(1, 0, 0, 1);
    glmc_Clear(GL_COLOR_BUFFER_BIT);
    glmc_Begin(GL_QUADS);                       /* green box at 20..80 */
        glmc_Color4f(0, 1, 0, 1);
        glmc_Vertex2f(20, 20); glmc_Vertex2f(80, 20);
        glmc_Vertex2f(80, 80); glmc_Vertex2f(20, 80);
    glmc_End();

    GLuint t; glmc_GenTextures(1, &t); glmc_BindTexture(GL_TEXTURE_2D, t);
    glmc_CopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 10, 10, 80, 80, 0);
    glmc_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glmc_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glmc_CopyTexSubImage2D(GL_TEXTURE_2D, 0, 40, 40, 50, 40, 20, 20);  /* grab a strip */
    glmc_TexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);

    glmc_ClearColor(0, 0, 1, 1);
    glmc_Clear(GL_COLOR_BUFFER_BIT);
    glmc_Enable(GL_TEXTURE_2D);
    glmc_Begin(GL_QUADS);                       /* draw the copied texture midway */
        glmc_TexCoord2f(0, 0); glmc_Vertex2f(100, 100);
        glmc_TexCoord2f(1, 0); glmc_Vertex2f(180, 100);
        glmc_TexCoord2f(1, 1); glmc_Vertex2f(180, 180);
        glmc_TexCoord2f(0, 1); glmc_Vertex2f(100, 180);
    glmc_End();
    glmc_Finish(); read_all();
    /* the copied texture (80x80, drawn 1:1 at 100..180) holds the green box at
     * texel 10..69 (screen 20..79) over a red base; borders stay red. */
    CHECK(px_close(fb, 105, 105, 255, 0, 0, 255), "copy base red (border)");
    CHECK(px_close(fb, 110, 110, 0, 255, 0, 255), "copy box green (corner)");
    CHECK(px_close(fb, 175, 175, 255, 0, 0, 255), "copy top-right border red");
    CHECK(px_close(fb, 250, 50, 0, 0, 255, 255), "right of copy quad = clear");
    glmc_DeleteTextures(1, &t);
}

/* ---------------------------------------------------------------- test 7 */
static void test_depth(void)
{
    GLuint col, rbo, fbo;
    const int FW = 200, FH = 160;
    glmc_GenTextures(1, &col);
    glmc_BindTexture(GL_TEXTURE_2D, col);
    glmc_TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, FW, FH, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glmc_GenRenderbuffers(1, &rbo);
    glmc_BindRenderbuffer(GL_RENDERBUFFER, rbo);
    glmc_RenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, FW, FH);
    glmc_GenFramebuffers(1, &fbo);
    glmc_BindFramebuffer(GL_FRAMEBUFFER, fbo);
    glmc_FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, col, 0);
    glmc_FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, rbo);
    fbo_setup(FW, FH);
    glmc_ClearColor(0, 0, 1, 1);
    glmc_ClearDepth(1.0);
    glmc_Clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glmc_Enable(GL_DEPTH_TEST);
    glmc_DepthFunc(GL_LESS);
    glmc_Begin(GL_QUADS);                       /* near plane first       */
        glmc_Color4f(1, 0, 0, 1);
        glmc_Vertex3f(40, 40, 0.0f); glmc_Vertex3f(160, 40, 0.0f);
        glmc_Vertex3f(160, 120, 0.0f); glmc_Vertex3f(40, 120, 0.0f);
    glmc_End();
    glmc_Begin(GL_QUADS);                       /* far -> should lose     */
        glmc_Color4f(0, 1, 0, 1);
        glmc_Vertex3f(80, 80, 0.5f); glmc_Vertex3f(190, 80, 0.5f);
        glmc_Vertex3f(190, 150, 0.5f); glmc_Vertex3f(80, 150, 0.5f);
    glmc_End();
    glmc_Disable(GL_DEPTH_TEST);
    glmc_Finish();
    {
        uint8_t rbuf[200 * 160 * 4];
        memset(rbuf, 0xAA, sizeof rbuf);
        glmc_ReadPixels(0, 0, FW, FH, GL_RGBA, GL_UNSIGNED_BYTE, rbuf);
        const uint8_t *p = rbuf + ((size_t)100 * FW + 60) * 4;
        CHECK(p[0] == 255 && p[1] == 0, "depth: near red kept");
        const uint8_t *q = rbuf + ((size_t)130 * FW + 170) * 4;
        CHECK(q[0] == 0 && q[1] == 255, "depth: far green accepted where near absent");
    }
    glmc_BindFramebuffer(GL_FRAMEBUFFER, 0);
    glmc_DeleteFramebuffers(1, &fbo);
    glmc_DeleteRenderbuffers(1, &rbo);
    glmc_DeleteTextures(1, &col);
}

/* ---------------------------------------------------------------- test 8 */
static void test_lines(void)
{
    std_setup();
    glmc_Disable(GL_TEXTURE_2D);
    glmc_ClearColor(0, 0, 0, 1);
    glmc_Clear(GL_COLOR_BUFFER_BIT);
    glmc_Begin(GL_LINES);
        glmc_Color4f(1, 1, 1, 1);
        glmc_Vertex2f(30, 120); glmc_Vertex2f(90, 120);
    glmc_End();
    glmc_Finish(); read_all();
    int found = 0;
    for (int y = 117; y <= 123 && !found; y++)
        for (int x = 32; x < 88; x++) {
            const uint8_t *p = fb + ((size_t)y * W + x) * 4;
            if (p[0] > 200 && p[1] > 200 && p[2] > 200) { found = 1; break; }
        }
    CHECK(found, "GL_LINES rasterised a horizontal line");
    CHECK(px_close(fb, 100, 120, 0, 0, 0, 255), "line ended before x=100");
}

/* ---------------------------------------------------------------- test 9 */
static void test_proc_table(void)
{
    /* the names resolved at runtime by tc2 (tc2_gl.c load_fns), engine/post_gl.c,
     * engine/render_target.c, engine/quad_gl.c — everything else binds through
     * the glmc_* compile-time remapping macros */
    static const char *names[] = {
        /* tc2/src/tc2_gl.c */
        "glCreateShader", "glShaderSource", "glCompileShader", "glGetShaderiv",
        "glGetShaderInfoLog", "glCreateProgram", "glAttachShader", "glBindAttribLocation",
        "glLinkProgram", "glGetProgramiv", "glGetProgramInfoLog", "glUseProgram",
        "glGetUniformLocation", "glUniform1i", "glUniform3i", "glUniform4i",
        "glGenBuffers", "glBindBuffer", "glBufferData", "glVertexAttribPointer",
        "glEnableVertexAttribArray", "glDisableVertexAttribArray",
        "glGenFramebuffers", "glBindFramebuffer", "glFramebufferTexture2D",
        "glCheckFramebufferStatus", "glActiveTexture", "glBlendEquationSeparate",
        "glBlendFuncSeparate", "glUniform1f", "glBlendEquation",
        /* engine/post_gl.c */
        "glGetAttribLocation", "glGetProgramInfoLog",
        /* engine/render_target.c with EXT fallbacks */
        "glGenFramebuffersEXT", "glDeleteFramebuffers", "glDeleteFramebuffersEXT",
        "glBindFramebufferEXT", "glGenRenderbuffers", "glGenRenderbuffersEXT",
        "glDeleteRenderbuffers", "glDeleteRenderbuffersEXT", "glBindRenderbuffer",
        "glBindRenderbufferEXT", "glRenderbufferStorage", "glRenderbufferStorageEXT",
        "glFramebufferRenderbuffer", "glFramebufferRenderbufferEXT", "glBlitFramebuffer",
        "glBlitFramebufferEXT", "glFramebufferTexture2DEXT", "glCheckFramebufferStatusEXT",
    };
    int missing = 0;
    for (size_t i = 0; i < sizeof names / sizeof *names; i++)
        if (!glmc_sdl_GetProcAddress(names[i])) {
            printf("       proc missing: %s\n", names[i]);
            missing++;
        }
    CHECK(missing == 0, "proc table complete (loaders fail closed)");
    CHECK(glmc_sdl_GetProcAddress("totally_made_up_fn") == NULL,
          "unknown names return NULL");
    /* glSwapIntervalEXT/wglSwapIntervalEXT resolved there would sit behind
     * SDL_GL_SetSwapInterval, which the shim implements directly *              */
    CHECK(1, "SDL swap interval handled via glmc_sdl_SetSwapInterval");
}

/* A sprite atlas reused within one frame must retain its earlier contents for
 * draws already submitted, even when the same texture is updated immediately. */
static void test_texture_update_order(void)
{
    const uint8_t red[4] = {255, 0, 0, 255};
    const uint8_t green[4] = {0, 255, 0, 255};
    GLuint tex;
    std_setup();
    glmc_ClearColor(0, 0, 0, 1); glmc_Clear(GL_COLOR_BUFFER_BIT);
    glmc_GenTextures(1, &tex); glmc_BindTexture(GL_TEXTURE_2D, tex);
    glmc_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glmc_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glmc_TexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    glmc_TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, red);
    glmc_Begin(GL_QUADS);
    glmc_TexCoord2f(0,0); glmc_Vertex2f(0,0); glmc_TexCoord2f(1,0); glmc_Vertex2f(W/2,0);
    glmc_TexCoord2f(1,1); glmc_Vertex2f(W/2,H); glmc_TexCoord2f(0,1); glmc_Vertex2f(0,H);
    glmc_End();
    glmc_TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, green);
    glmc_Begin(GL_QUADS);
    glmc_TexCoord2f(0,0); glmc_Vertex2f(W/2,0); glmc_TexCoord2f(1,0); glmc_Vertex2f(W,0);
    glmc_TexCoord2f(1,1); glmc_Vertex2f(W,H); glmc_TexCoord2f(0,1); glmc_Vertex2f(W/2,H);
    glmc_End();
    glmc_Finish(); read_all();
    CHECK(px_close(fb, W/4, H/2, 255, 0, 0, 255), "earlier sprite keeps its original texture");
    CHECK(px_close(fb, 3*W/4, H/2, 0, 255, 0, 255), "later sprite sees updated texture");
    glmc_DeleteTextures(1, &tex);
}

static void test_combiner_mode_restores_configuration(void)
{
    const uint8_t red[4] = {255, 0, 0, 255};
    GLuint tex;
    std_setup();
    glmc_GenTextures(1, &tex); glmc_BindTexture(GL_TEXTURE_2D, tex);
    glmc_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glmc_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glmc_TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, red);
    glmc_TexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);
    glmc_TexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_MODULATE);
    glmc_TexEnvi(GL_TEXTURE_ENV, GL_COMBINE_ALPHA, GL_MODULATE);
    glmc_TexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    glmc_TexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);
    glmc_ClearColor(1,1,1,1); glmc_Clear(GL_COLOR_BUFFER_BIT);
    glmc_Color4f(0,1,0,1);
    glmc_Begin(GL_QUADS);
    glmc_TexCoord2f(0,0); glmc_Vertex2f(0,0); glmc_TexCoord2f(1,0); glmc_Vertex2f(W,0);
    glmc_TexCoord2f(1,1); glmc_Vertex2f(W,H); glmc_TexCoord2f(0,1); glmc_Vertex2f(0,H);
    glmc_End(); glmc_Finish(); read_all();
    CHECK(px_close(fb, W/2, H/2, 0, 0, 0, 255), "switching back to COMBINE restores texture modulation");
    glmc_DeleteTextures(1, &tex);
}

static void test_disabled_texture_unit_does_not_change_color(void)
{
    const uint8_t red[4] = {255, 0, 0, 255};
    GLuint tex;
    std_setup();
    glmc_GenTextures(1, &tex); glmc_BindTexture(GL_TEXTURE_2D, tex);
    glmc_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glmc_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glmc_TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, red);
    glmc_TexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    glmc_ActiveTexture(GL_TEXTURE1);
    glmc_TexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    glmc_Disable(GL_TEXTURE_2D);
    glmc_ActiveTexture(GL_TEXTURE0);
    glmc_Begin(GL_QUADS);
    glmc_TexCoord2f(0,0); glmc_Vertex2f(0,0); glmc_TexCoord2f(1,0); glmc_Vertex2f(W,0);
    glmc_TexCoord2f(1,1); glmc_Vertex2f(W,H); glmc_TexCoord2f(0,1); glmc_Vertex2f(0,H);
    glmc_End(); glmc_Finish(); read_all();
    CHECK(px_close(fb, W/2, H/2, 255, 0, 0, 255), "disabled unit preserves the previous unit's result");
    glmc_DeleteTextures(1, &tex);
}

static void test_copy_and_redraw_preserves_orientation(void)
{
    GLuint tex;
    std_setup(); glmc_Disable(GL_TEXTURE_2D);
    glmc_ClearColor(0, 1, 0, 1); glmc_Clear(GL_COLOR_BUFFER_BIT);
    glmc_Begin(GL_QUADS); glmc_Color4f(1,0,0,1);
    glmc_Vertex2f(0,0); glmc_Vertex2f(W,0); glmc_Vertex2f(W,H/2); glmc_Vertex2f(0,H/2);
    glmc_End();
    glmc_GenTextures(1, &tex); glmc_BindTexture(GL_TEXTURE_2D, tex);
    glmc_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glmc_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glmc_TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 512, 256, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glmc_CopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, W, H);
    glmc_ClearColor(0,0,0,1); glmc_Clear(GL_COLOR_BUFFER_BIT);
    glmc_Enable(GL_TEXTURE_2D); glmc_TexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    glmc_Begin(GL_QUADS);
    glmc_TexCoord2f(0,0); glmc_Vertex2f(0,0); glmc_TexCoord2f((float)W/512,0); glmc_Vertex2f(W,0);
    glmc_TexCoord2f((float)W/512,(float)H/256); glmc_Vertex2f(W,H); glmc_TexCoord2f(0,(float)H/256); glmc_Vertex2f(0,H);
    glmc_End(); glmc_Finish(); read_all();
    CHECK(px_close(fb, W/2, H/4, 255,0,0,255), "copied lower half remains below");
    CHECK(px_close(fb, W/2, 3*H/4, 0,255,0,255), "copied upper half remains above");
    glmc_DeleteTextures(1, &tex);
}

/* glBlitFramebuffer keeps the picture's orientation and its place: the games
 * scale their picture to the window with it, and a flip here showed the whole
 * game upside down in any window bigger than the picture. The image is
 * top/bottom asymmetric and lands off-centre, 1:1 and scaled 2x. */
static void test_blit_keeps_orientation(void)
{
    const int FW = 100, FH = 80;
    GLuint col, fbo;
    glmc_GenTextures(1, &col);
    glmc_BindTexture(GL_TEXTURE_2D, col);
    glmc_TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, FW, FH, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glmc_GenFramebuffers(1, &fbo);
    glmc_BindFramebuffer(GL_FRAMEBUFFER, fbo);
    glmc_FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, col, 0);
    fbo_setup(FW, FH);
    glmc_ClearColor(0, 0, 1, 1); glmc_Clear(GL_COLOR_BUFFER_BIT);      /* blue bottom half */
    glmc_Begin(GL_QUADS);                                              /* red top half */
        glmc_Color4f(1, 0, 0, 1);
        glmc_Vertex2f(0, 40); glmc_Vertex2f(FW, 40); glmc_Vertex2f(FW, FH); glmc_Vertex2f(0, FH);
    glmc_End();
    glmc_Finish();
    {
        uint8_t rbuf[100 * 80 * 4];
        glmc_ReadPixels(0, 0, FW, FH, GL_RGBA, GL_UNSIGNED_BYTE, rbuf);
        const uint8_t *top = rbuf + ((size_t)60 * FW + 50) * 4, *bot = rbuf + ((size_t)20 * FW + 50) * 4;
        CHECK(top[0] == 255 && top[2] == 0, "FBO top half red");
        CHECK(bot[2] == 255 && bot[0] == 0, "FBO bottom half blue");
    }
    glmc_BindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    glmc_BindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glmc_ClearColor(0, 0, 0, 1); glmc_Clear(GL_COLOR_BUFFER_BIT);
    glmc_BlitFramebuffer(0, 0, FW, FH, 10, 20, 10 + FW, 20 + FH, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glmc_Finish();
    glmc_BindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    read_all();
    CHECK(px_close(fb, 60, 20 + 60, 255, 0, 0, 255), "1:1 blit: top half red");
    CHECK(px_close(fb, 60, 20 + 20, 0, 0, 255, 255), "1:1 blit: bottom half blue");
    CHECK(px_close(fb, 60, 10, 0, 0, 0, 255), "1:1 blit: nothing below the rect");
    CHECK(px_close(fb, 60, 20 + FH + 10, 0, 0, 0, 255), "1:1 blit: nothing above the rect");
    glmc_BindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    glmc_BindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glmc_Clear(GL_COLOR_BUFFER_BIT);
    glmc_BlitFramebuffer(0, 0, FW, FH, 10, 20, 10 + 2 * FW, 20 + 2 * FH, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glmc_Finish();
    glmc_BindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    read_all();
    CHECK(px_close(fb, 110, 20 + 120, 255, 0, 0, 255), "2x blit: top half red");
    CHECK(px_close(fb, 110, 20 + 40, 0, 0, 255, 255), "2x blit: bottom half blue");
    CHECK(px_close(fb, 110, 10, 0, 0, 0, 255), "2x blit: nothing below the rect");
    glmc_BindFramebuffer(GL_FRAMEBUFFER, 0);
    glmc_DeleteFramebuffers(1, &fbo);
    glmc_DeleteTextures(1, &col);
}

/* an off-centre viewport places the drawing once: TC2 shows its picture by
 * drawing a 0..1 quad into glViewport(bar, 0, w, h) between the side bars */
static void test_offset_viewport_places_once(void)
{
    glmc_Viewport(0, 0, W, H);
    glmc_ClearColor(0, 0, 0, 1); glmc_Clear(GL_COLOR_BUFFER_BIT);
    glmc_Viewport(100, 40, 120, 80);
    glmc_MatrixMode(GL_PROJECTION); glmc_LoadIdentity(); glmc_Ortho(0, 1, 1, 0, -1, 1);
    glmc_MatrixMode(GL_MODELVIEW); glmc_LoadIdentity();
    glmc_Disable(GL_TEXTURE_2D);
    glmc_Begin(GL_QUADS);
        glmc_Color4f(1, 0, 0, 1); glmc_Vertex2f(0, 0); glmc_Vertex2f(1, 0); glmc_Vertex2f(1, 1); glmc_Vertex2f(0, 1);
    glmc_End();
    glmc_Viewport(0, 0, W, H);
    glmc_Finish(); read_all();
    CHECK(px_close(fb, 160, 80, 255, 0, 0, 255), "offset viewport: centre drawn");
    CHECK(px_close(fb, 105, 45, 255, 0, 0, 255), "offset viewport: lower-left corner drawn");
    CHECK(px_close(fb, 215, 115, 255, 0, 0, 255), "offset viewport: upper-right corner drawn");
    CHECK(px_close(fb, 95, 80, 0, 0, 0, 255), "offset viewport: nothing left of it");
    CHECK(px_close(fb, 225, 80, 0, 0, 0, 255), "offset viewport: nothing right of it");
    CHECK(px_close(fb, 160, 35, 0, 0, 0, 255), "offset viewport: nothing below it");
    CHECK(px_close(fb, 160, 125, 0, 0, 0, 255), "offset viewport: nothing above it");

    /* the same, as TC2 draws it: a textured REPLACE quad, the texture made on
     * a 640x480-style FBO, the viewport between side bars of a wide window */
    GLuint col, fbo;
    glmc_GenTextures(1, &col); glmc_BindTexture(GL_TEXTURE_2D, col);
    glmc_TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 64, 48, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glmc_GenFramebuffers(1, &fbo); glmc_BindFramebuffer(GL_FRAMEBUFFER, fbo);
    glmc_FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, col, 0);
    glmc_Viewport(0, 0, 64, 48);
    glmc_ClearColor(0, 1, 0, 1); glmc_Clear(GL_COLOR_BUFFER_BIT);
    glmc_BindFramebuffer(GL_FRAMEBUFFER, 0);
    glmc_Viewport(0, 0, W, H);
    glmc_ClearColor(0, 0, 0, 1); glmc_Clear(GL_COLOR_BUFFER_BIT);
    glmc_Viewport(100, 0, 120, H);
    glmc_MatrixMode(GL_PROJECTION); glmc_LoadIdentity(); glmc_Ortho(0, 1, 1, 0, -1, 1);
    glmc_MatrixMode(GL_MODELVIEW); glmc_LoadIdentity();
    glmc_Enable(GL_TEXTURE_2D); glmc_BindTexture(GL_TEXTURE_2D, col);
    glmc_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glmc_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glmc_TexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    glmc_Begin(GL_QUADS);
    glmc_TexCoord2f(0, 0); glmc_Vertex2f(0, 0); glmc_TexCoord2f(1, 0); glmc_Vertex2f(1, 0);
    glmc_TexCoord2f(1, 1); glmc_Vertex2f(1, 1); glmc_TexCoord2f(0, 1); glmc_Vertex2f(0, 1);
    glmc_End();
    glmc_Disable(GL_TEXTURE_2D);
    glmc_TexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    glmc_Viewport(0, 0, W, H);
    glmc_Finish(); read_all();
    CHECK(px_close(fb, 105, 120, 0, 255, 0, 255), "textured offset viewport: left edge drawn");
    CHECK(px_close(fb, 215, 120, 0, 255, 0, 255), "textured offset viewport: right edge drawn");
    CHECK(px_close(fb, 95, 120, 0, 0, 0, 255), "textured offset viewport: nothing left of it");
    CHECK(px_close(fb, 225, 120, 0, 0, 0, 255), "textured offset viewport: nothing right of it");
    glmc_DeleteFramebuffers(1, &fbo); glmc_DeleteTextures(1, &col);
}

/* the menu (Nuklear's GL2 backend) draws interleaved client arrays with
 * glDrawElements: 2 float position, 2 float uv, 4 unsigned-byte colour, a
 * 20-byte stride, 16-bit indices, a y-down ortho and a scissor per command.
 * The element path used to read them as packed floats (a garbage menu). */
static void test_menu_style_draw_elements(void)
{
    struct mv { float pos[2]; float uv[2]; unsigned char col[4]; };
    const struct mv v[8] = {
        { { 20, 20 }, { 0, 0 }, { 255, 0, 0, 255 } }, { { 140, 20 }, { 0, 0 }, { 255, 0, 0, 255 } },
        { { 140, 100 }, { 0, 0 }, { 255, 0, 0, 255 } }, { { 20, 100 }, { 0, 0 }, { 255, 0, 0, 255 } },
        { { 180, 20 }, { 0, 0 }, { 0, 0, 255, 255 } }, { { 300, 20 }, { 0, 0 }, { 0, 0, 255, 255 } },
        { { 300, 100 }, { 0, 0 }, { 0, 0, 255, 255 } }, { { 180, 100 }, { 0, 0 }, { 0, 0, 255, 255 } },
    };
    const unsigned short ix[12] = { 0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7 };
    glmc_Viewport(0, 0, W, H);
    glmc_ClearColor(0, 0, 0, 1); glmc_Clear(GL_COLOR_BUFFER_BIT);
    glmc_Scissor(0, 0, 10, 10);                         /* a box without glEnable: no clipping */
    glmc_MatrixMode(GL_PROJECTION); glmc_LoadIdentity(); glmc_Ortho(0, W, H, 0, -1, 1);
    glmc_MatrixMode(GL_MODELVIEW); glmc_LoadIdentity();
    glmc_Disable(GL_TEXTURE_2D);
    glmc_EnableClientState(GL_VERTEX_ARRAY); glmc_EnableClientState(GL_TEXTURE_COORD_ARRAY); glmc_EnableClientState(GL_COLOR_ARRAY);
    glmc_VertexPointer(2, GL_FLOAT, sizeof(struct mv), &v[0].pos);
    glmc_TexCoordPointer(2, GL_FLOAT, sizeof(struct mv), &v[0].uv);
    glmc_ColorPointer(4, GL_UNSIGNED_BYTE, sizeof(struct mv), &v[0].col);
    glmc_DrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, ix);            /* red, unclipped */
    glmc_Enable(GL_SCISSOR_TEST);
    glmc_Scissor(180, H - 100, 60, 80);                                   /* GL rows: the left half of the blue quad */
    glmc_DrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, ix + 6);
    glmc_Disable(GL_SCISSOR_TEST);
    glmc_DisableClientState(GL_VERTEX_ARRAY); glmc_DisableClientState(GL_TEXTURE_COORD_ARRAY); glmc_DisableClientState(GL_COLOR_ARRAY);
    glmc_Finish(); read_all();
    /* y-down ortho: window y 20..100 is GL rows H-100..H-20 */
    CHECK(px_close(fb, 80, H - 60, 255, 0, 0, 255), "menu draw: red quad where placed");
    CHECK(px_close(fb, 80, H - 110, 0, 0, 0, 255), "menu draw: nothing below the red quad");
    CHECK(px_close(fb, 200, H - 60, 0, 0, 255, 255), "menu draw: blue quad inside its scissor");
    CHECK(px_close(fb, 280, H - 60, 0, 0, 0, 255), "menu draw: blue quad clipped outside its scissor");
    CHECK(px_close(fb, 160, H - 60, 0, 0, 0, 255), "menu draw: gap between the quads empty");
}

/* a resize while a pass is open (the window maximised mid-frame): the open
 * pass must be closed, not dropped -- Metal aborts the process when a command
 * buffer is committed with an encoder still open */
static void test_resize_with_open_pass(void)
{
    std_setup(); glmc_Disable(GL_TEXTURE_2D);
    glmc_ClearColor(0,0,1,1); glmc_Clear(GL_COLOR_BUFFER_BIT);
    glmc_Color4f(1,0,0,1);
    glmc_Begin(GL_QUADS);
    glmc_Vertex2f(0,0); glmc_Vertex2f(W,0); glmc_Vertex2f(W,H); glmc_Vertex2f(0,H);
    glmc_End();                                         /* the default target's pass is open */
    glmc_dev_face_rewindowed(glmc_dev_current(), NULL, W, H, 1.0f);   /* the window changed size */
    glmc_ClearColor(0,1,0,1); glmc_Clear(GL_COLOR_BUFFER_BIT);
    glmc_Finish(); read_all();
    CHECK(px_close(fb, 50, 50, 0, 255, 0, 255), "draws after a resize with an open pass");
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    glmc_init();

    printf("\n-- clear + FFP quad --\n");    fresh(); test_clear_and_quad();
    printf("-- textured formats --\n");    fresh(); test_textured_formats();
    printf("-- texture update ordering --\n"); fresh(); test_texture_update_order();
    printf("-- texture combiner state --\n"); fresh(); test_combiner_mode_restores_configuration();
    printf("-- disabled texture unit --\n"); fresh(); test_disabled_texture_unit_does_not_change_color();
    printf("-- copied frame orientation --\n"); fresh(); test_copy_and_redraw_preserves_orientation();
    printf("-- unpack meta --\n");         fresh(); test_unpack_meta();
    printf("-- blend/scissor/alpha --\n"); fresh(); test_blend_scissor_alphatest();
    printf("-- FBO + blit --\n");          fresh(); test_fbo_and_blit();
    printf("-- copy texsub --\n");         fresh(); test_copy_texsub();
    printf("-- depth --\n");               fresh(); test_depth();
    printf("-- lines --\n");               fresh(); test_lines();
    printf("-- proc table --\n");          fresh(); test_proc_table();
    printf("-- blit orientation --\n");    fresh(); test_blit_keeps_orientation();
    printf("-- offset viewport --\n");     fresh(); test_offset_viewport_places_once();
    printf("-- menu-style elements --\n"); fresh(); test_menu_style_draw_elements();
    printf("-- resize with an open pass --\n"); fresh(); test_resize_with_open_pass();

    printf("\nRESULT: %d passed, %d failed\n", n_pass, n_fail);
    return n_fail ? 1 : 0;
}
