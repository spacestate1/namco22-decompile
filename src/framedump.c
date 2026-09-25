/*
 * framedump.c — render a CAPTURED MAME frame through our geometry stage.
 *
 * WHY
 * ---
 * Comparing our live attract against MAME's is not a measurement: the
 * attract is not deterministic here, and the same frame number gave
 * drawn=61/behind=9, then 32/38, then 17/53 across runs. Every "does it
 * look closer?" judgement made against it was therefore unanswerable.
 *
 * This drives the pipeline from MAME's own polygon-RAM dump instead, so
 * the input is fixed and the output can be diffed pixel-for-pixel against
 * MAME's capture of that same frame. Same idea as tools/geo_gate.py, one
 * level up: turn "looks right" into a number.
 *
 * The dump contains the records the MASTER DSP emitted, so it also removes
 * every unknown we do not model -- the camera, the zoom, the per-object
 * matrices are all in the data rather than reconstructed.
 *
 * Record layout, from tools/pc_geo_fixed.py (byte-exact vs MAME):
 *   len 0x15  bb0003  viewport: zoom = dspfloat(src[6]),
 *                     view[row][col] = q15(src[0x0c + col*3 + row])
 *   len 0x0a  300000  view transform: view[row][col] = q15(src[1+col*3+row])
 *   len 0x0d  200002  primitive (code 0x5 or >= 0x45):
 *                     m[row][col] = q15(src[1+col*3+row])
 *                     t = sext24(src[0xa..0xc])
 *                     combined = (m . view) >> 15, t' = (t . view) >> 15
 *
 * SEQUENCE MODE
 * -------------
 * --framedump accepts a DIRECTORY as well as a single .bin. Given a
 * directory it plays every poly_f<N>.bin in it, in frame order, one dump
 * per rendered frame -- i.e. the captured attract runs as an animation
 * rather than a still. Dumps are loaded lazily (128 KB each), so a long
 * capture costs disk, not RAM.
 *
 * PROPCYCL_FRAMEDUMP_HOLD=N holds each dump for N rendered frames
 * (default 1). This matters because MAME captures are usually SPARSE --
 * the in-tree set is 6 frames spread over the whole attract, and at one
 * dump per frame it flashes past in 100 ms. Generate a contiguous range
 * with tools/dump_attract_range.sh if you want true motion.
 *
 * Single-file behaviour is unchanged: --geotest, tools/geo_gate.py and
 * launch-attract.sh all still get exactly one deterministic frame.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include "propcycl.h"
#include "geo_hw.h"
#include "slave_list.h"
#include "fog_hw.h"
#include "sprite_hw.h"
#include "text_hw.h"

static uint32_t *g_poly;          /* 0x8000 words, 24-bit values */
static int       g_poly_words;

/* ---- sequence playback state (empty in single-file mode) ------------- */
#define FD_SEQ_MAX 4096
static char *g_seq[FD_SEQ_MAX];   /* paths, frame-number order */
static int   g_seq_frame[FD_SEQ_MAX];
static int   g_seq_n;             /* 0 = single-file mode */
static int   g_seq_i = -1;        /* index of the dump currently loaded */
static int   g_hold = 1;          /* rendered frames per dump */
static int   g_hold_ctr;
static char  g_seq_dir[1024];

/* ---- preloaded sequence -------------------------------------------------
 * Playback used to re-read SEVEN files per rendered frame (the 128 KB poly
 * dump plus czattr, czram0-3 and mix) and malloc/free the poly buffer each
 * time. That cost about 60% of frame time -- 76 fps against 187 for a
 * single dump held in memory. Captures are small next to RAM (a 900-frame
 * demo flight is ~150 MB resident), so the whole sequence is loaded once
 * up front and playback becomes pointer swaps. Falls back to the old lazy
 * path if a capture would exceed the budget. */
#define FD_PRELOAD_BUDGET ((size_t)2048 * 1024 * 1024)
static uint32_t **g_pre_poly;     /* [i] -> words for dump i */
static int       *g_pre_words;
static fog_state *g_pre_fog;
static uint8_t   *g_pre_fog_ok;
static sprite_state *g_pre_spr;      /* 2D layer state, one per dump */
static text_state   *g_pre_txt;
static int        g_preloaded;

/* Current frame's sprite state, for the renderer. NULL when the capture
 * carries no 2D data (older captures) or in single-file mode. */
static sprite_state g_cur_spr;
static int          g_cur_spr_ok;
const sprite_state *framedump_current_sprites(void)
{
    if (g_preloaded && g_pre_spr && g_seq_i >= 0)
        return g_pre_spr[g_seq_i].valid ? &g_pre_spr[g_seq_i] : NULL;
    return g_cur_spr_ok ? &g_cur_spr : NULL;
}

static text_state g_cur_txt;
static int        g_cur_txt_ok;
/* True once a capture is being replayed, regardless of whether that capture
 * happens to carry 2D data. The renderer must not fall back to live memory
 * during a replay: the game loop is skipped there, so live RAM holds
 * whatever init left behind and renders as garbage glyphs. */
int framedump_is_active(void) { return g_seq_n > 0 || g_poly != NULL; }

const text_state *framedump_current_text(void)
{
    if (g_preloaded && g_pre_txt && g_seq_i >= 0)
        return g_pre_txt[g_seq_i].valid ? &g_pre_txt[g_seq_i] : NULL;
    return g_cur_txt_ok ? &g_cur_txt : NULL;
}

/* Set only for the duration of framedump_render_words(): the LIVE master
 * DSP's polygon RAM (master_dsp.c), walked with the same record walker. */
static const uint32_t *g_live_words;

static uint32_t pw(int i)
{
    i &= 0x7FFF;
    if (g_live_words) return g_live_words[i];
    return (i < g_poly_words) ? g_poly[i] : 0;
}

static void walk_records(geo_quad_cb cb, void *user);
static uint32_t pw_idx(int i) { return pw(i); }

/* Walk a live polygon RAM (24-bit words, 0x8000 of them) produced by the
 * master DSP. No capture state is touched: fog, fades and the 2D layers stay
 * on their live sources. */
void framedump_render_words(const uint32_t *words, geo_quad_cb cb, void *user)
{
    g_live_words = words;
    walk_records(cb, user);
    g_live_words = NULL;
}

static int load_one(const char *path, int quiet)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "framedump: cannot open %s\n", path); return 0; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    int words = (int)(n / 4);
    uint32_t *buf = calloc(words ? words : 1, sizeof *buf);
    if (!buf) { fclose(f); return 0; }
    for (int i = 0; i < words; i++) {
        unsigned char b[4];
        if (fread(b, 1, 4, f) != 4) break;
        buf[i] = ((uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 |
                  (uint32_t)b[2] << 8  | b[3]) & 0xFFFFFF;
    }
    fclose(f);
    if (!g_preloaded) free(g_poly);   /* preloaded buffers are owned by the array */
    g_poly = buf;
    g_poly_words = words;
    if (!quiet) printf("framedump: %s, %d words\n", path, words);
    return 1;
}

/* poly_f<N>.bin -> N, or -1 if the name doesn't match. */
static int dump_frame_no(const char *name)
{
    if (strncmp(name, "poly_f", 6) != 0) return -1;
    const char *p = name + 6;
    if (*p < '0' || *p > '9') return -1;
    char *end;
    long v = strtol(p, &end, 10);
    if (strcmp(end, ".bin") != 0) return -1;
    return (int)v;
}

static int seq_cmp(const void *a, const void *b)
{
    int ia = *(const int *)a, ib = *(const int *)b;
    return (g_seq_frame[ia] > g_seq_frame[ib]) - (g_seq_frame[ia] < g_seq_frame[ib]);
}

static int load_sequence(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) { fprintf(stderr, "framedump: cannot open dir %s\n", dir); return 0; }
    struct dirent *e;
    int order[FD_SEQ_MAX];
    while ((e = readdir(d)) && g_seq_n < FD_SEQ_MAX) {
        int fn = dump_frame_no(e->d_name);
        if (fn < 0) continue;
        size_t len = strlen(dir) + 1 + strlen(e->d_name) + 1;
        char *full = malloc(len);
        if (!full) break;
        snprintf(full, len, "%s/%s", dir, e->d_name);
        g_seq[g_seq_n] = full;
        g_seq_frame[g_seq_n] = fn;
        order[g_seq_n] = g_seq_n;
        g_seq_n++;
    }
    closedir(d);
    if (g_seq_n == 0) {
        fprintf(stderr, "framedump: no poly_f*.bin in %s\n", dir);
        return 0;
    }
    /* sort indices by frame number, then permute both arrays */
    qsort(order, g_seq_n, sizeof order[0], seq_cmp);
    char *np[FD_SEQ_MAX]; int nf[FD_SEQ_MAX];
    for (int i = 0; i < g_seq_n; i++) { np[i] = g_seq[order[i]]; nf[i] = g_seq_frame[order[i]]; }
    for (int i = 0; i < g_seq_n; i++) { g_seq[i] = np[i]; g_seq_frame[i] = nf[i]; }

    const char *h = getenv("PROPCYCL_FRAMEDUMP_HOLD");
    if (h) { int v = atoi(h); if (v > 0) g_hold = v; }

    printf("framedump: sequence of %d dumps from %s (frames %d..%d), "
           "hold=%d frame%s each\n", g_seq_n, dir,
           g_seq_frame[0], g_seq_frame[g_seq_n - 1], g_hold,
           g_hold == 1 ? "" : "s");
    /* Sparse means GAPPY, not few: 10 consecutive frames are motion, while
     * the 6 in-tree gate dumps span 4680 frames and are a slideshow. Judge
     * by span vs count, or a dense short capture gets held needlessly. */
    {
        int span = g_seq_frame[g_seq_n - 1] - g_seq_frame[0] + 1;
        if (span > g_seq_n * 4)
            printf("framedump: NOTE sparse capture -- %d dumps spanning %d "
                   "frames is a slideshow, not motion. Capture a contiguous "
                   "range with tools/dump_attract_range.sh\n", g_seq_n, span);
    }
    g_seq_i = 0; g_hold_ctr = 0;
    snprintf(g_seq_dir, sizeof g_seq_dir, "%s", dir);

    /* Preload, if it fits the budget. */
    /* poly + fog + (spriteram 0x30000 + vics_data 0x10000); the palette is
     * shared across frames so it is not multiplied in here. */
    size_t est = (size_t)g_seq_n * (0x8000 * sizeof(uint32_t) + sizeof(fog_state)
                                    + 0x30000 + 0x10000);
    if (est <= FD_PRELOAD_BUDGET) {
        g_pre_poly   = calloc(g_seq_n, sizeof *g_pre_poly);
        g_pre_words  = calloc(g_seq_n, sizeof *g_pre_words);
        g_pre_fog    = calloc(g_seq_n, sizeof *g_pre_fog);
        g_pre_fog_ok = calloc(g_seq_n, 1);
        g_pre_spr    = calloc(g_seq_n, sizeof *g_pre_spr);
        g_pre_txt    = calloc(g_seq_n, sizeof *g_pre_txt);
        if (g_pre_poly && g_pre_words && g_pre_fog && g_pre_fog_ok && g_pre_spr && g_pre_txt) {
            int ok = 1;
            for (int i = 0; i < g_seq_n; i++) {
                if (!load_one(g_seq[i], 1)) { ok = 0; break; }
                g_pre_poly[i] = g_poly; g_pre_words[i] = g_poly_words;
                g_poly = NULL; g_poly_words = 0;   /* ownership moves */
                g_pre_fog_ok[i] = (uint8_t)fog_load_frame_into(
                    &g_pre_fog[i], g_seq_dir, g_seq_frame[i]);
                /* 2D state. The palette is identical frame to frame, so
                 * hand each load the previous one to share instead of
                 * carrying ~86 MB of duplicates across a long capture. */
                sprite_load_frame_into(&g_pre_spr[i], g_seq_dir, g_seq_frame[i],
                                       i ? g_pre_spr[i-1].pal : NULL);
                text_load_frame_into(&g_pre_txt[i], g_seq_dir, g_seq_frame[i],
                                     g_pre_spr[i].pal,
                                     i ? g_pre_txt[i-1].cgram : NULL);
            }
            if (!ok) {
                /* A mid-preload failure used to just fall through to the
                 * lazy path, orphaning every buffer allocated so far. */
                for (int j = 0; j < g_seq_n; j++) {
                    free(g_pre_poly[j]); g_pre_poly[j] = NULL;
                    sprite_free(&g_pre_spr[j]);
                    text_free(&g_pre_txt[j]);
                }
                free(g_pre_poly); free(g_pre_words); free(g_pre_fog);
                free(g_pre_fog_ok); free(g_pre_spr); free(g_pre_txt);
                g_pre_poly = NULL; g_pre_words = NULL; g_pre_fog = NULL;
                g_pre_fog_ok = NULL; g_pre_spr = NULL; g_pre_txt = NULL;
            }
            if (ok) {
                g_preloaded = 1;
                printf("framedump: preloaded %d dumps (%.0f MB) -- no file I/O "
                       "during playback\n", g_seq_n, est / (1024.0 * 1024.0));
            }
        }
        if (!g_preloaded)
            printf("framedump: preload failed, falling back to per-frame reads\n");
    } else {
        printf("framedump: sequence too large to preload (%.0f MB) -- "
               "reading per frame\n", est / (1024.0 * 1024.0));
    }

    if (g_preloaded) {
        g_poly = g_pre_poly[0]; g_poly_words = g_pre_words[0];
        g_fog_valid = g_pre_fog_ok[0];
        if (g_fog_valid) g_fog = g_pre_fog[0];
        printf("framedump: 2D layer %s / text %s\n",
               g_pre_spr[0].valid ? "sprites+VICS" : "absent",
               g_pre_txt[0].valid ? "loaded" : "absent");
    } else {
        if (!load_one(g_seq[0], 1)) return 0;
        if (!fog_load_frame(g_seq_dir, g_seq_frame[0]))
            printf("framedump: no fog data in %s -- CZ fog / fades disabled "
                   "(recapture with tools/dump_attract_range.sh to get them)\n", dir);
    }
    return 1;
}

int framedump_load(const char *path)
{
    struct stat st;
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode))
        return load_sequence(path);
    if (!load_one(path, 0)) return 0;

    /* Single .bin: recover <dir> and <frame> from ".../poly_f<N>.bin" so a
     * lone dump picks up its fog state too. Older captures have no fog
     * files; that is not an error, the stage just stays off. */
    {
        char dir[1024];
        snprintf(dir, sizeof dir, "%s", path);
        char *slash = strrchr(dir, '/');
        const char *base = slash ? slash + 1 : dir;
        /* same size as dir[]: a truncated basename fails dump_frame_no and
         * the fog / 2D state then goes silently missing */
        char basecopy[sizeof dir];
        snprintf(basecopy, sizeof basecopy, "%s", base);
        if (slash) *slash = '\0'; else snprintf(dir, sizeof dir, ".");
        int fn = dump_frame_no(basecopy);
        if (fn >= 0 && !fog_load_frame(dir, fn))
            printf("framedump: no fog data beside %s -- CZ fog / fades "
                   "disabled\n", basecopy);
        if (fn >= 0) {
            g_cur_spr_ok = sprite_load_frame_into(&g_cur_spr, dir, fn, NULL);
            g_cur_txt_ok = text_load_frame_into(&g_cur_txt, dir, fn,
                                                g_cur_spr_ok ? g_cur_spr.pal : NULL,
                                                NULL);
        }
    }
    return 1;
}

/* Walk the record list and hand each primitive to the geometry stage.
 *
 * Mirrors GeoModel.run() + GeoFixed's handlers (pc_geo_model.py /
 * pc_geo_fixed.py), which are byte-exact against MAME. The previous
 * version handled three of the four record types and dropped three pieces
 * of camera state; tools/frame_gate.py caught it as:
 *   - zsort wrong in bits 23:21 on EVERY quad, because absolute_priority
 *     was never read from the viewport record and objectshift never read
 *     at all -- the whole length-0x10 record type was missing. Those bits
 *     are the priority band, so the draw order was wrong.
 *   - no reflection handling, so scenes the hardware mirrors came out
 *     un-mirrored ("things look backwards").
 */
void framedump_render(geo_quad_cb cb, void *user)
{
    /* Sequence mode: advance one dump per rendered frame (held g_hold
     * frames each), looping at the end. Called exactly once per frame
     * from renderer_3d.c, which is what makes this the right place. */
    if (g_seq_n > 0) {
        if (++g_hold_ctr > g_hold) {
            g_hold_ctr = 1;
            g_seq_i = (g_seq_i + 1) % g_seq_n;
            if (g_preloaded) {
                g_poly = g_pre_poly[g_seq_i];
                g_poly_words = g_pre_words[g_seq_i];
                g_fog_valid = g_pre_fog_ok[g_seq_i];
                if (g_fog_valid) g_fog = g_pre_fog[g_seq_i];
            } else {
                if (!load_one(g_seq[g_seq_i], 1)) return;
                fog_load_frame(g_seq_dir, g_seq_frame[g_seq_i]);
            }
            if (propcycl_verbose())
                printf("  [FD] dump %d/%d  frame %d  (%s)\n", g_seq_i + 1,
                       g_seq_n, g_seq_frame[g_seq_i], g_seq[g_seq_i]);
        }
    }

    walk_records(cb, user);
}

static void walk_records(geo_quad_cb cb, void *user)
{
    int32_t zoom_mant = 0; int zoom_shift = 0; int32_t vx = 0, vy = 0;
    eng_list_cfg cfg = { ENG_LIST_HEAD_SS22, 0, &zoom_mant, &zoom_shift, &vx, &vy };
    int prims = eng_walk_list(pw_idx, &cfg, cb, user);
    /* once per DUMP, not per rendered frame -- in single-file mode the
     * harness redraws the same dump every frame and the repeat drowned the
     * per-frame log; in sequence mode each dump is genuinely new. */
    static int said = -2;          /* -2, not -1: single-file mode leaves
                                    * g_seq_i at -1, and matching it here
                                    * would suppress the line entirely */
    if (said != g_seq_i && (propcycl_verbose() || g_seq_n == 0)) {
        said = g_seq_i;
        printf("framedump: %d primitives, zoom=%d>>%d vx=%d vy=%d\n",
               prims, zoom_mant, zoom_shift, vx, vy);
    }
}
