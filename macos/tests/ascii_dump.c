/* ascii_dump.c — focused FFP render dump: one textured quad, ASCII pixels
 * (R G B Y = colour code legs, alpha shown by lower/upper case) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define W 320
#define H 240
#include "glmc.h"

static uint8_t fb[H * W * 4];

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    glmc_init();
    glmc_context_attach_window(NULL, NULL, NULL, W, H, 1.0f);

    /* 2x2 test texture: (0,0)red (1,0)green (0,1)blue (1,1)yellow */
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
    glmc_Enable(GL_TEXTURE_2D);
    /* vertex pattern: (0,0) (320,0) (320,240) (0,240) full-screen quad      */
    glmc_Begin(GL_QUADS);
        glmc_Color4f(1, 1, 1, 1);
        glmc_TexCoord2f(0, 0); glmc_Vertex2f(0, 0);
        glmc_TexCoord2f(1, 0); glmc_Vertex2f(W, 0);
        glmc_TexCoord2f(1, 1); glmc_Vertex2f(W, H);
        glmc_TexCoord2f(0, 1); glmc_Vertex2f(0, H);
    glmc_End();
    glmc_Finish();

    memset(fb, 0x55, sizeof fb);
    glmc_ReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, fb);
    printf("transform dump (cols = x, rows down the SCREEN):\n");
    for (int j = 0; j < 24; j++) {
        int y = H * j / 24;              /* === readback row index          */
        for (int i = 0; i < 32; i++) {
            int x = W * i / 32 + 2;
            const uint8_t *p = fb + ((size_t)y * W + x) * 4;
            char c;
            if (p[3] == 0)            c = '.';
            else if (p[0] > 200 && p[1] > 200) c = 'Y';
            else if (p[0] > 200)      c = 'R';
            else if (p[1] > 200)      c = 'G';
            else if (p[2] > 200)      c = 'B';
            else                      c = '#';
            putchar(c);
        }
        printf("  row-j=%d y=%d\n", j, y);
    }
    return 0;
}
