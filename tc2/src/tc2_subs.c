/* tc2_subs.c -- ENGLISH SUBTITLES for a game whose captions are PICTURES in the text layer (Angler King, 2026-10-25).
 *
 * Angler King writes no caption as text: it copies ready-drawn glyph images (Japanese, outlined) into the C361's character RAM from
 * tile 0x084 on and lays them out in the tile map -- the tile NUMBERS are just the next free ones, the PIXELS say which caption it is.
 * So every frame the tile map's caption tiles (code >= 0x084) are grouped into blocks (4-connected), and each block is identified by
 * an FNV-1a hash of its tiles' pixels (and their place and flips in the block, not their palette, which blinks). The table
 * (tc2_game.subtitles, e.g. aking/aking_subtitles.txt: "HASH  English text", '|' = a new line, '#' = a comment, "-" = a block that only
 * continues another one's sentence) turns a hash into a line. Everything recognised in a frame goes in ONE dark bar -- above the game's
 * caption box when one is in the lower part of the screen, else at the bottom -- over the picture (the menu's layer hook: the picture itself is not touched, and a
 * screenshot / headless shot is the game's own picture).
 *
 *   $TC2_SUBLOG=<dir>   every caption block that stays for 10 frames and is not in the table: <dir>/<hash>.pgm (its glyphs, 16 px a
 *                       tile) and a "[SUB] new" line -- how the table is filled.
 * Menu: a "Subtitles" page (on / off, saved as subtitles= in the game's cfg).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <SDL2/SDL.h>
#include "tc2_game.h"
#include "eng_ui.h"
#include "eng_nk.h"

uint8_t *tc2_vram_ptr(uint32_t);
int eng_cfg_int(const char *, int);
void eng_cfg_set_int(const char *, int);

#define MAXSUB 512
#define MAXBLK 8
static struct { uint32_t hash; char *text; } tab[MAXSUB];
static int ntab, loaded, enabled = 1;

typedef struct { uint32_t hash; int r0, c0, r1, c1; } blk;
static blk cur[MAXBLK], prev[MAXBLK]; static int ncur, nprev;
static int stable;                                       /* frames the block set has been the same */
static SDL_mutex *lock;
static blk shown[MAXBLK]; static const char *shown_text[MAXBLK]; static int nshown;   /* what the layer draws (under lock) */

static char *trim(char *s) { while (*s == ' ' || *s == '\t') s++; char *e = s + strlen(s); while (e > s && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t')) *--e = 0; return s; }
static void load(void)
{
    loaded = 1;
    if (!tc2_game.subtitles) return;
    FILE *f = fopen(tc2_game.subtitles, "r");
    if (!f) { char *b = SDL_GetBasePath(); if (b) { char p[1024]; snprintf(p, sizeof p, "%s%s", b, tc2_game.subtitles); f = fopen(p, "r"); SDL_free(b); } }
    if (!f) { fprintf(stderr, "[SUB] no %s: no subtitles\n", tc2_game.subtitles); return; }
    char line[1024];
    while (fgets(line, sizeof line, f) && ntab < MAXSUB) {
        char *s = trim(line), *end;
        if (!*s || *s == '#') continue;
        const unsigned long h = strtoul(s, &end, 16);
        if (end == s) continue;
        char *t = trim(end);
        if (!*t) continue;
        for (char *q = t; *q; q++) if (*q == '|') *q = '\n';
        tab[ntab].hash = (uint32_t)h; tab[ntab].text = strdup(t); ntab++;
    }
    fclose(f);
    fprintf(stderr, "[SUB] %d subtitles from %s\n", ntab, tc2_game.subtitles);
}
static const char *find(uint32_t h) { for (int i = 0; i < ntab; i++) if (tab[i].hash == h) return tab[i].text; return NULL; }

static inline uint16_t tm_word(const uint8_t *tm, int r, int c) { const int o = (r * 64 + c) * 2; return (uint16_t)(tm[o] << 8 | tm[o + 1]); }
static inline int is_cap(uint16_t w) { return (w & 0x3FF) >= 0x084; }

static void dump_pgm(const char *dir, const blk *b, const uint8_t *tm, const uint8_t *cg)
{
    char p[512]; snprintf(p, sizeof p, "%s/%08X.pgm", dir, b->hash);
    FILE *f = fopen(p, "wb"); if (!f) return;
    const int w = (b->c1 - b->c0 + 1) * 16, h = (b->r1 - b->r0 + 1) * 16;
    fprintf(f, "P5\n%d %d\n255\n", w, h);
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        const uint16_t t = tm_word(tm, b->r0 + y / 16, b->c0 + x / 16);
        uint8_t v = 0;
        if (is_cap(t)) {
            const int ty = (t & 0x800) ? 15 - y % 16 : y % 16, tx = (t & 0x400) ? 15 - x % 16 : x % 16;
            const uint8_t *row = cg + (size_t)(t & 0x3FF) * 128 + ty * 8;   /* 16 pixels, 4 bits each, big-endian nibbles */
            const int pen = (row[tx >> 1] >> ((tx & 1) ? 0 : 4)) & 0xF;
            v = pen == 0xF ? 0 : (uint8_t)(255 - pen * 12);
        }
        fputc(v, f);
    }
    fclose(f);
}

/* every game frame (src/tc2_main.c tc2_on_vblank): find the caption blocks */
void tc2_subs_frame(uint32_t frame)
{
    if (!tc2_game.subtitles) return;
    if (!loaded) { load(); enabled = eng_cfg_int("subtitles", 1) != 0; if (!lock) lock = SDL_CreateMutex(); }
    static const char *logdir; static int ld = -1;
    if (ld < 0) { logdir = getenv("TC2_SUBLOG"); ld = 1; }
    const uint8_t *cg = tc2_vram_ptr(0x06800000u), *tm = tc2_vram_ptr(0x0681E000u);
    static uint8_t seen[30][40];
    memset(seen, 0, sizeof seen);
    ncur = 0;
    for (int r = 0; r < 30; r++) for (int c = 0; c < 40; c++) {
        if (seen[r][c] || !is_cap(tm_word(tm, r, c))) continue;
        int st[1200][2], sp = 0; blk b = { 0, r, c, r, c };
        st[sp][0] = r; st[sp][1] = c; sp++; seen[r][c] = 1;
        while (sp) {                                      /* the block's extent */
            sp--; const int y = st[sp][0], x = st[sp][1];
            if (y < b.r0) b.r0 = y; if (y > b.r1) b.r1 = y; if (x < b.c0) b.c0 = x; if (x > b.c1) b.c1 = x;
            static const int d[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
            for (int k = 0; k < 4; k++) { const int ny = y + d[k][0], nx = x + d[k][1];
                if (ny >= 0 && ny < 30 && nx >= 0 && nx < 40 && !seen[ny][nx] && is_cap(tm_word(tm, ny, nx))) { seen[ny][nx] = 1; st[sp][0] = ny; st[sp][1] = nx; sp++; } }
        }
        uint32_t h = 2166136261u;                        /* FNV-1a over the block in raster order: place, flips, the 128 pixel bytes */
        for (int y = b.r0; y <= b.r1; y++) for (int x = b.c0; x <= b.c1; x++) {
            const uint16_t t = tm_word(tm, y, x);
            if (!is_cap(t)) continue;
            const uint8_t hdr[3] = { (uint8_t)(y - b.r0), (uint8_t)(x - b.c0), (uint8_t)(t >> 10 & 3) };
            for (int k = 0; k < 3; k++) { h ^= hdr[k]; h *= 16777619u; }
            const uint8_t *px = cg + (size_t)(t & 0x3FF) * 128;
            for (int k = 0; k < 128; k++) { h ^= px[k]; h *= 16777619u; }
        }
        b.hash = h;
        if (ncur < MAXBLK) cur[ncur++] = b;
    }
    int same = ncur == nprev;
    for (int i = 0; same && i < ncur; i++) same = cur[i].hash == prev[i].hash;
    stable = same ? stable + 1 : 0;
    memcpy(prev, cur, sizeof cur); nprev = ncur;
    if (stable == 10 && logdir) {                       /* a caption that stays: log it if the table does not have it */
        static uint32_t logged[1024]; static int nlog;
        for (int i = 0; i < ncur; i++) {
            int had = find(cur[i].hash) != NULL;
            for (int k = 0; k < nlog && !had; k++) had = logged[k] == cur[i].hash;
            if (had) continue;
            if (nlog < 1024) logged[nlog++] = cur[i].hash;
            dump_pgm(logdir, &cur[i], tm, cg);
            fprintf(stderr, "[SUB] new %08X frame %u rows %d-%d cols %d-%d\n", cur[i].hash, frame, cur[i].r0, cur[i].r1, cur[i].c0, cur[i].c1);
        }
    }
    if (lock) {
        SDL_LockMutex(lock);
        nshown = 0;
        if (stable >= 2) for (int i = 0; i < ncur; i++) { const char *t = find(cur[i].hash); if (t) { shown[nshown] = cur[i]; shown_text[nshown] = t; nshown++; } }
        SDL_UnlockMutex(lock);
    }
}

/* ---------------------------------------------------------------- drawing: a layer of the menu (over the picture, menu closed) */
bool tc2_pic_rect_win(float *x, float *y, float *w, float *h);   /* src/tc2_window.c: the game's 4:3 area in window units */
const struct nk_user_font *eng_ui_font_big(float h);
static void layer_draw(struct nk_context *ctx, int ww, int wh, bool menu_open)
{
    (void)ww; (void)wh;
    if (menu_open || !enabled || !lock) return;
    float px, py, pw, ph;
    if (!tc2_pic_rect_win(&px, &py, &pw, &ph)) return;
    const struct nk_user_font *big = eng_ui_font_big(ph / 30.0f * 1.3f);    /* about 1.3 of the game's text rows (its captions are two rows a line) */
    if (!big) return;
    /* ONE bar for everything recognised this frame (a promo line can be two blocks shown together; the tutorial's title and its
     * caption are up at once), top-to-bottom; a "-" entry continues another block and adds nothing */
    char lines[16][256]; int n = 0, low = 99;
    SDL_LockMutex(lock);
    int ord[MAXBLK]; for (int i = 0; i < nshown; i++) ord[i] = i;
    for (int i = 1; i < nshown; i++) for (int k = i; k > 0 && shown[ord[k]].r0 < shown[ord[k - 1]].r0; k--) { const int t = ord[k]; ord[k] = ord[k - 1]; ord[k - 1] = t; }
    for (int j = 0; j < nshown; j++) {
        const blk *b = &shown[ord[j]]; const char *t = shown_text[ord[j]];
        if (b->r1 >= 18 && b->r0 < low) low = b->r0;      /* a caption box in the lower part: the bar goes above it */
        if (!strcmp(t, "-")) continue;
        for (const char *s = t; *s && n < 16; ) {
            const char *e = strchr(s, '\n'); const size_t l = e ? (size_t)(e - s) : strlen(s);
            snprintf(lines[n++], sizeof lines[0], "%.*s", (int)(l < 255 ? l : 255), s);
            s = e ? e + 1 : s + l;
        }
    }
    SDL_UnlockMutex(lock);
    if (!n) return;
    const float lh = big->height + 4, tile = ph / 30.0f;
    float tw = 0;
    for (int k = 0; k < n; k++) { const float w = big->width(big->userdata, big->height, lines[k], (int)strlen(lines[k])); if (w > tw) tw = w; }
    float bw = tw + 32, bh = n * lh + 14;
    if (bw > pw - 8) bw = pw - 8;
    const float bx = px + (pw - bw) / 2;
    float by = low < 99 ? py + (low - 1) * tile - bh - 4 : py + ph - bh - 6;
    if (by < py + 2) by = py + 2;
    struct nk_style_item keep = ctx->style.window.fixed_background;
    const struct nk_vec2 pad = ctx->style.window.padding;
    ctx->style.window.fixed_background = nk_style_item_color(nk_rgba(0, 0, 0, 200));
    ctx->style.window.padding = nk_vec2(8, 6);
    nk_style_push_font(ctx, big);
    if (nk_begin(ctx, "subtitles", nk_rect(bx, by, bw, bh), NK_WINDOW_NO_SCROLLBAR | NK_WINDOW_NO_INPUT)) {
        for (int k = 0; k < n; k++) { nk_layout_row_dynamic(ctx, lh, 1); nk_label_colored(ctx, lines[k], NK_TEXT_CENTERED, nk_rgb(255, 245, 200)); }
    }
    nk_end(ctx);
    nk_style_pop_font(ctx);
    ctx->style.window.fixed_background = keep; ctx->style.window.padding = pad;
}
static const eng_ui_layer sub_layer = { layer_draw, NULL, NULL };

/* ---------------------------------------------------------------- the menu page */
static int p_nrows(void) { return 1; }
static void p_text(int row, char *l, size_t ln, char *v, size_t vn) { (void)row; snprintf(l, ln, "English subtitles"); snprintf(v, vn, "%s", enabled ? "On" : "Off"); }
static void p_change(int row, int dir) { (void)row; (void)dir; enabled = !enabled; eng_cfg_set_int("subtitles", enabled); }
static void p_notes(void (*line)(const char *fmt, ...)) { line("%d captions translated (%s).", ntab, tc2_game.subtitles ? tc2_game.subtitles : "-"); line("An English line over the Japanese captions."); }
static const eng_ui_page sub_page = { "Subtitles", 360, 160, 0, p_nrows, NULL, NULL, p_text, p_change, p_notes };

void tc2_subs_ui_init(void)                             /* src/tc2_window.c, after eng_ui_init */
{
    if (!tc2_game.subtitles) return;
    if (!loaded) { load(); enabled = eng_cfg_int("subtitles", 1) != 0; if (!lock) lock = SDL_CreateMutex(); }
    eng_ui_add_page(&sub_page);
    eng_ui_add_layer(&sub_layer);
}
