/* ascii_tex.c — textured RGBA 2x2 quad across the screen: texel orientation */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define W 320
#define H 240
#include "glmc.h"

static uint8_t fb[H * W * 4];

static void dump(void)
{
    const uint8_t *q;
    q = fb + (0 * W + 1) * 4;
    printf("raw(1,0) : %3u %3u %3u %3u\n", q[0], q[1], q[2], q[3]);
    q = fb + (120 * W + 1) * 4;
    printf("raw(1,120): %3u %3u %3u %3u\n", q[0], q[1], q[2], q[3]);
    q = fb + (238 * W + 1) * 4;
    printf("raw(1,238): %3u %3u %3u %3u\n", q[0], q[1], q[2], q[3]);
    q = fb + (0 * W + W - 2) * 4;
    printf("raw(W-2,0): %3u %3u %3u %3u\n", q[0], q[1], q[2], q[3]);
    for (int j = 0; j < 24; j++) {
        int y = H * j / 24;
        for (int i = 0; i < 32; i++) {
            int x = W * i / 32 + 2;
            const uint8_t *p = fb + ((size_t)y * W + x) * 4;
            char c;
            if (p[0] > 200 && p[1] > 200 && p[2] < 100) c = 'Y';   /* yellow */
            else if (p[0] > 200 && p[1] < 100 && p[2] < 100) c = 'R';
            else if (p[1] > 200 && p[0] < 100) c = 'G';
            else if (p[2] > 200 && p[0] < 100 && p[1] < 100) c = 'B';
            else if (p[0] > 200 && p[1] > 200 && p[2] > 200) c = 'W';
            else c = '.';
            putchar(c);
        }
        printf(" ygd=%d\n", y);
    }
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    glmc_init();
    glmc_context_attach_window(NULL, NULL, NULL, W, H, 1.0f);
    uint8_t rgba[2 * 2 * 4] = { 255,0,0,255,   0,255,0,255,
                                0,0,255,255,   255,255,0,255 };
    GLuint t; glmc_GenTextures(1, &t); glmc_BindTexture(GL_TEXTURE_2D, t);
    glmc_TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glmc_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glmc_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glmc_TexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    glmc_MatrixMode(GL_PROJECTION); glmc_LoadIdentity();
    glmc_Ortho(0, (GLdouble)W, 0, (GLdouble)H, -1, 1);
    glmc_MatrixMode(GL_MODELVIEW); glmc_LoadIdentity();
    glmc_Enable(GL_TEXTURE_2D);
    glmc_Color4f(0.0f, 0.9f, 0.0f, 1.0f);
    glmc_Begin(GL_QUADS);
        glmc_TexCoord2f(0, 0); glmc_Vertex2f(0, 0);
        glmc_Color4f(1.0f, 0.0f, 0.0f, 1.0f);
        glmc_TexCoord2f(2, 0); glmc_Vertex2f(W, 0);
        glmc_Color4f(0.0f, 0.0f, 1.0f, 1.0f);
        glmc_TexCoord2f(2, 2); glmc_Vertex2f(W, H);
        glmc_Color4f(1.0f, 1.0f, 0.0f, 1.0f);
        glmc_TexCoord2f(0, 2); glmc_Vertex2f(0, H);
    glmc_End();
    glmc_Finish();
    memset(fb, 0x11, sizeof fb);
    glmc_ReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, fb);
    dump();
    /* row 0 (bottom) should be R G R G ... and top row B Y B Y ... */
    return 0;
}
