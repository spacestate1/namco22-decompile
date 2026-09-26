/*
 * hud_edges.c -- widescreen: the HUD goes to the edges (see hud_edges.h).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "eng_gl.h"
#include "quad_gl.h"
#include "hud_edges.h"

int g_eng_hud_e;

void eng_hud_begin(bool hud_on)
{
    static int centre = -1;
    if (centre < 0) { const char *e = getenv("ENG_HUD_CENTER"); centre = e && *e != '0'; }
    g_eng_hud_e = (hud_on && !centre && g_scene_x0 < 0.0f) ? (int)(-g_scene_x0) : 0;
}

bool eng_hud_marks_up(const uint8_t *text, const eng_hud_mark *marks, int n, int *hold)
{
    bool seen = false;
    for (int i = 0; i < n && !seen; i++) {
        const int o = (marks[i].row * 64 + marks[i].col) * 2;
        if (o >= 0 && o + 1 < 0x2000 && (uint16_t)(text[o] << 8 | text[o + 1]) == marks[i].tile) seen = true;
    }
    if (seen) *hold = 8;                       /* the updates it stays on for after the marker last showed */
    else if (*hold > 0) (*hold)--;
    return *hold > 0;
}

int eng_hud_dx(double cx)
{
    if (!g_eng_hud_e) return 0;
    const double off = cx - ENG_SCREEN_W / 2.0;
    return off < -ENG_HUD_DEADZONE ? -g_eng_hud_e : off > ENG_HUD_DEADZONE ? g_eng_hud_e : 0;
}

/* ------------------------------------------------------------------------------------------------ text layer */
#define CELL 4
#define GW (ENG_SCREEN_W / CELL)
#define GH (ENG_SCREEN_H / CELL)
#define WORD_GAP 8                                 /* cells (32 px): the widest gap between two words of one line */
#define WORD_MAXH 48                               /* px: the tallest a piece may be and still count as a line of text */
static int16_t  cell_cx[GW * GH];                  /* centre x of the piece a cell belongs to; INT16_MIN = the cell draws nothing */
static bool     have_scan;

static int uf_find(int *p, int a) { while (p[a] != a) { p[a] = p[p[a]]; a = p[a]; } return a; }

void eng_hud_text_scan(const uint8_t *occ)
{
    static int parent[GW * GH], minx[GW * GH], maxx[GW * GH];
    static uint8_t drawn[GW * GH];
    for (int cy = 0; cy < GH; cy++)
        for (int cx = 0; cx < GW; cx++) {
            uint8_t any = 0;
            for (int y = 0; y < CELL && !any; y++) {
                const uint8_t *r = occ + (size_t)(cy * CELL + y) * ENG_SCREEN_W + cx * CELL;
                for (int x = 0; x < CELL; x++) if (r[x]) { any = 1; break; }
            }
            drawn[cy * GW + cx] = any;
            parent[cy * GW + cx] = cy * GW + cx;
        }
    for (int cy = 0; cy < GH; cy++)                // join each drawn cell to its drawn neighbours on the right and below (8-connected)
        for (int cx = 0; cx < GW; cx++) {
            const int a = cy * GW + cx;
            if (!drawn[a]) continue;
            static const int nb[4][2] = { { 1, 0 }, { -1, 1 }, { 0, 1 }, { 1, 1 } };
            for (int k = 0; k < 4; k++) {
                const int nx = cx + nb[k][0], ny = cy + nb[k][1];
                if (nx < 0 || nx >= GW || ny >= GH || !drawn[ny * GW + nx]) continue;
                const int ra = uf_find(parent, a), rb = uf_find(parent, ny * GW + nx);
                if (ra != rb) parent[rb] = ra;
            }
        }
    /* The words of one line: SHORT pieces (a line of text, not a counter with its icons or a frame) whose drawn cells share a row, up to
     * WORD_GAP cells apart, are one piece -- so a centred sentence ("11 ENEMY TANKS REMAIN") is judged whole, not word by word. Only
     * short ones: a radar's frame sits 15-20 px from the counters beside it, and joining across it would make one screen-wide banner. */
    static int miny[GW * GH], maxy[GW * GH];
    static uint8_t cshort[GW * GH];
    for (int i = 0; i < GW * GH; i++) { miny[i] = 1 << 30; maxy[i] = -1; }
    for (int cy = 0; cy < GH; cy++)
        for (int cx = 0; cx < GW; cx++) {
            if (!drawn[cy * GW + cx]) continue;
            const int r = uf_find(parent, cy * GW + cx);
            if (cy < miny[r]) miny[r] = cy;
            if (cy > maxy[r]) maxy[r] = cy;
        }
    static int16_t pminy[GW * GH], pmaxy[GW * GH];      // the rows of the piece each cell belongs to, as they stand before any word is joined
    for (int a = 0; a < GW * GH; a++) {
        if (!drawn[a]) { cshort[a] = 0; continue; }
        const int r = uf_find(parent, a);
        pminy[a] = (int16_t)miny[r]; pmaxy[a] = (int16_t)maxy[r];
        cshort[a] = (maxy[r] - miny[r] + 1) * CELL <= WORD_MAXH;
    }
    for (int cy = 0; cy < GH; cy++) {
        int prev = -1;
        for (int cx = 0; cx < GW; cx++) {
            const int a = cy * GW + cx;
            if (!drawn[a]) continue;
            const int p = cy * GW + prev;
            if (prev >= 0 && cx - prev <= WORD_GAP + 1 && cshort[p] && cshort[a]) {
                /* on the same line: the two pieces overlap by 70% of the shorter one's height (a frame above the text does not) */
                const int lo = pminy[p] > pminy[a] ? pminy[p] : pminy[a], hi = pmaxy[p] < pmaxy[a] ? pmaxy[p] : pmaxy[a];
                const int hp = pmaxy[p] - pminy[p] + 1, ha = pmaxy[a] - pminy[a] + 1, hmin = hp < ha ? hp : ha;
                if ((hi - lo + 1) * 10 >= hmin * 7) {
                    const int ra = uf_find(parent, p), rb = uf_find(parent, a);
                    if (ra != rb) parent[rb] = ra;
                }
            }
            prev = cx;
        }
    }
    for (int i = 0; i < GW * GH; i++) { minx[i] = 1 << 30; maxx[i] = -1; }
    for (int cy = 0; cy < GH; cy++)
        for (int cx = 0; cx < GW; cx++) {
            const int a = cy * GW + cx;
            if (!drawn[a]) continue;
            const int r = uf_find(parent, a);
            if (cx < minx[r]) minx[r] = cx;
            if (cx > maxx[r]) maxx[r] = cx;
        }
    for (int a = 0; a < GW * GH; a++) {
        if (!drawn[a]) { cell_cx[a] = INT16_MIN; continue; }
        const int r = uf_find(parent, a);
        const int x0 = minx[r] * CELL, x1 = (maxx[r] + 1) * CELL;
        cell_cx[a] = (x1 - x0 > 400) ? ENG_SCREEN_W / 2 : (int16_t)((x0 + x1) / 2);      /* a banner across the screen stays */
    }
    have_scan = true;
}

void eng_hud_text_scan_rgba(const uint8_t *rgba)
{
    static uint8_t occ[ENG_SCREEN_W * ENG_SCREEN_H];
    for (int i = 0; i < ENG_SCREEN_W * ENG_SCREEN_H; i++) occ[i] = rgba[i * 4 + 3];
    eng_hud_text_scan(occ);
}

void eng_hud_text_draw(void)
{
    glBegin(GL_QUADS);
    for (int cy = 0; cy < GH; cy++) {
        int cx = 0;
        while (cx < GW) {
            const int16_t c = cell_cx[cy * GW + cx];
            if (c == INT16_MIN) { cx++; continue; }
            const int dx = eng_hud_dx(c);
            int e = cx + 1;                                      // a run of drawn cells with the same shift is one quad
            while (e < GW && cell_cx[cy * GW + e] != INT16_MIN && eng_hud_dx(cell_cx[cy * GW + e]) == dx) e++;
            const float x0 = (float)(cx * CELL), x1 = (float)(e * CELL), y0 = (float)(cy * CELL), y1 = (float)((cy + 1) * CELL);
            const float u0 = x0 / ENG_SCREEN_W, u1 = x1 / ENG_SCREEN_W, v0 = y0 / ENG_SCREEN_H, v1 = y1 / ENG_SCREEN_H;
            glTexCoord2f(u0, v0); glVertex2f(x0 + dx, y0);
            glTexCoord2f(u1, v0); glVertex2f(x1 + dx, y0);
            glTexCoord2f(u1, v1); glVertex2f(x1 + dx, y1);
            glTexCoord2f(u0, v1); glVertex2f(x0 + dx, y1);
            cx = e;
        }
    }
    glEnd();
}

/* ------------------------------------------------------------------------------------------------ polygons */
#define MAXWIN 16
static struct { int32_t r[4]; int dx; } win[MAXWIN];
static int nwin, hud_band = -1;
static bool hud_zero_depth;

void eng_hud_quads_scan(const geo_quad *buf, int n, int band, bool zero_depth)
{
    nwin = 0; hud_band = band; hud_zero_depth = zero_depth;
    if (!g_eng_hud_e) return;
    for (int i = 0; i < n; i++) {
        const int32_t *c = buf[i].clip;
        if (c[0] <= 0 && c[1] >= ENG_SCREEN_W - 1) continue;            // a full-frame viewport is not a window
        int k;
        for (k = 0; k < nwin; k++) if (!memcmp(win[k].r, c, sizeof win[k].r)) break;
        if (k == nwin && nwin < MAXWIN) { memcpy(win[nwin].r, c, sizeof win[0].r); win[nwin].dx = eng_hud_dx((c[0] + c[1]) / 2.0); nwin++; }
    }
}

int eng_hud_quad_dx(const geo_quad *q)
{
    if (!g_eng_hud_e) return 0;
    const int32_t *c = q->clip;
    if (!(c[0] <= 0 && c[1] >= ENG_SCREEN_W - 1)) return eng_hud_dx((c[0] + c[1]) / 2.0);       // in a sub-window: with the window
    const bool hud_piece = (hud_band >= 0 && ((q->zsort >> 21) & 7) == hud_band) || (hud_zero_depth && (q->zsort & 0x1FFFFF) == 0);
    if (!hud_piece) return 0;                                                                     // the world's
    int x0 = 1 << 30, x1 = -(1 << 30), y0 = 1 << 30, y1 = -(1 << 30);
    for (int i = 0; i < q->nrv; i++) {
        const int x = q->rv[i].sx16 >> 4, y = q->rv[i].sy16 >> 4;
        if (x < x0) x0 = x; if (x > x1) x1 = x; if (y < y0) y0 = y; if (y > y1) y1 = y;
    }
    if (x0 < -64 || x1 > ENG_SCREEN_W + 64 || y0 < -64 || y1 > ENG_SCREEN_H + 64) return 0;      // not a piece of screen-space HUD
    for (int k = 0; k < nwin; k++)                                                                 // inside a window's frame: goes with it
        if (x0 >= win[k].r[0] - 4 && x1 <= win[k].r[1] + 4 && y0 >= win[k].r[2] - 4 && y1 <= win[k].r[3] + 4) return win[k].dx;
    return eng_hud_dx((x0 + x1) / 2.0);
}
