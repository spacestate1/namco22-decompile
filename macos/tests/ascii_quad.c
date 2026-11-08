/* ascii_quad.c — color-only immediate quad: dump ASCII to see the transform */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define W 320
#define H 240
#include "glmc.h"

static uint8_t fb[H * W * 4];

static void dump(void)
{
    for (int j = 0; j < 24; j++) {
        int y = H * j / 24;
        for (int i = 0; i < 32; i++) {
            int x = W * i / 32 + 2;
            const uint8_t *p = fb + ((size_t)y * W + x) * 4;
            char c;
            if (p[0] > 200 && p[1] > 200) c = 'W';
            else if (p[0] > 200)          c = 'R';
            else if (p[1] > 200)          c = 'G';
            else                          c = '.';
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
    glmc_MatrixMode(GL_PROJECTION); glmc_LoadIdentity();
    glmc_Ortho(0, (GLdouble)W, 0, (GLdouble)H, -1, 1);
    glmc_MatrixMode(GL_MODELVIEW); glmc_LoadIdentity();
    glmc_Disable(GL_TEXTURE_2D);
    glmc_ClearColor(0, 0, 0, 1);
    glmc_Clear(GL_COLOR_BUFFER_BIT);
    glmc_Begin(GL_QUADS);
        glmc_Color4f(1, 0, 0, 1);
        glmc_Vertex2f(100, 100); glmc_Vertex2f(200, 100);
        glmc_Vertex2f(200, 200); glmc_Vertex2f(100, 200);
    glmc_End();
    glmc_Finish();
    memset(fb, 0x11, sizeof fb);
    glmc_ReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, fb);
    dump();
    return 0;
}
