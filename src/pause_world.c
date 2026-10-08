/*
 * pause_world.c — while PAUSED, the world in every direction.
 *
 * The game only emits the terrain chunks and scenery its own camera can see:
 * terrain_chunk_visibility picks one of 32 heading-zone chunk lists (ROM
 * 0x8B200), scenery and props hang off that list, and the LOD bands go by
 * list position. So when the pause camera (renderer_3d.c pausecam_apply)
 * turns round, the frozen display list simply has nothing behind or beside
 * the rider. That is the game's culling, and it stays exactly as it is in
 * play.
 *
 * While paused -- the simulation is frozen, so this runs once per pause --
 * the game's OWN emitters are run again for the four quarter headings
 * against a SAVED copy of the game state, the words they write are kept,
 * and the state is put back byte for byte. The renderer then walks that
 * extra list after the frozen one and draws only what the frozen frame does
 * not already contain. Nothing here runs unless paused, and the game never
 * sees any of it: _W[], work RAM, DSP RAM and the sound mailbox (the scenery
 * sound sources write it) are all restored. PROPCYCL_PAUSE360=0 turns it off.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include "propcycl.h"

extern intptr_t _W[];
#define W _W

void terrain_chunk_visibility(uint32_t, int, uint32_t, int);
void terrain_props_dispatch(void);
uint32_t objects_render_master(void);
void balloon_render_and_hit_check(void);

static int32_t *words;
static int nwords, cap, built;

void pause_world_reset(void) { built = 0; nwords = 0; }

static int append(const int32_t *src, int n) {
    if (nwords + n > cap) {
        int nc = cap ? cap : 16384;
        while (nc < nwords + n) nc *= 2;
        int32_t *nb = realloc(words, (size_t)nc * sizeof *nb);
        if (!nb) return 0;
        words = nb; cap = nc;
    }
    memcpy(words + nwords, src, (size_t)n * sizeof *src);
    nwords += n;
    return 1;
}

/* Run the game's own emitters once per heading in hs[] against a saved copy of the state, keep the words
 * they write, and put the state back byte for byte after each pass. Returns 0 when the frame is not one to
 * fill (not gameplay / the intro orbit, or the cursor is unusable). */
static int walk(const int32_t *hs, int nh, const char *tag, int turn_camera) {
    /* Gameplay and the intro orbit only: the menu screens run in state 3
     * too, and must not grow a landscape behind them. */
    const int state = (int)W[0x0CBC], sub = (int)W[0x0CC0];
    if (state != 3 || (sub != 3 && sub != 5)) return 0;

    const uintptr_t dsp = (uintptr_t)g_sys.dspram, cur = (uintptr_t)W[0x0CA4];
    if (cur < dsp || cur >= dsp + DSPRAM_SIZE) return 0;
    /* the buffer this frame's list is NOT in is free: the game rewrites it next frame */
    const uint32_t other = (cur - dsp >= 0x18400) ? 0x10400 : 0x18400;
    const uint32_t room = 0x8000 - 0x400;          /* list area of one buffer, bytes */

    static intptr_t *sW;
    static uint8_t *sWram, *sDsp, *sComm;
    if (!sW) {
        sW = malloc(WORK_RAM_SIZE * sizeof *sW);
        sWram = malloc(WORK_RAM_SIZE);
        sDsp = malloc(DSPRAM_SIZE);
        sComm = malloc(COMMSRAM_SIZE);
        if (!sW || !sWram || !sDsp || !sComm) { fprintf(stderr, "[PAUSE360] out of memory\n"); sW = NULL; return 0; }
    }
    memcpy(sW, _W, WORK_RAM_SIZE * sizeof *sW);
    memcpy(sWram, g_sys.work_ram, WORK_RAM_SIZE);
    memcpy(sDsp, g_sys.dspram, DSPRAM_SIZE);
    memcpy(sComm, g_sys.commsram, COMMSRAM_SIZE);

    for (int k = 0; k < nh; k++) {
        const int32_t h = hs[k] & 0xffff;
        int32_t *base = (int32_t *)(g_sys.dspram + other);
        W[0x0CA4] = (intptr_t)base;
        if (turn_camera) W[0x0CEC] = h;             /* the pause camera turns: billboards face this way too. Widescreen does NOT
                                                     * turn the camera -- only the chunk list is chosen for the turned heading --
                                                     * or billboards (a balloon's value digits) move and slip past the
                                                     * renderer's duplicate filter: a "100" balloon read "1100" */
        terrain_chunk_visibility((uint32_t)W[0x0CDC], (int)W[0x0CE0], (uint32_t)W[0x0CE4], h);
        terrain_props_dispatch();
        objects_render_master();
        balloon_render_and_hit_check();     /* its hit test changes nothing: the state is put back below */
        const intptr_t n = ((int32_t *)W[0x0CA4]) - base;
        if (n > 0 && (uint32_t)n * 4 <= room) append(base, (int)n);
        else if (n > 0) fprintf(stderr, "[%s] heading %d: %ld words overran the buffer, skipped\n", tag, h, (long)n);
        /* every pass starts from the saved state, and the game gets it back untouched */
        memcpy(_W, sW, WORK_RAM_SIZE * sizeof *sW);
        memcpy(g_sys.work_ram, sWram, WORK_RAM_SIZE);
        memcpy(g_sys.dspram, sDsp, DSPRAM_SIZE);
        memcpy(g_sys.commsram, sComm, COMMSRAM_SIZE);
    }
    return 1;
}

static void build(void) {
    nwords = 0;
    { const char *e = getenv("PROPCYCL_PAUSE360"); if (e && *e == '0') return; }
    const int32_t h0 = (int32_t)W[0x0CEC];
    const int32_t hs[4] = { h0, h0 + 0x4000, h0 + 0x8000, h0 + 0xC000 };
    if (walk(hs, 4, "PAUSE360", 1)) fprintf(stderr, "[PAUSE360] %d words from 4 headings\n", nwords);
}

/* WIDESCREEN FILL. The game emits the terrain chunks, props and scenery its own 4:3 camera can see (32 heading-zone
 * chunk lists, LOD by list position), so in widescreen the side strips show the edge of that: scenery popping in as
 * it crosses into the 4:3 view. Each frame, the same emitters run again for the camera heading turned by +-dh -- the
 * extra half-angle the wide frame shows -- and the renderer draws only placements this frame does not already have.
 * The game state is put back byte for byte, so play is unchanged; with the frame at 4:3 nothing runs.
 * extra = the scene's extra width at each side in 640x480 units, zoom = the world viewport's focal length. */
const int32_t *pause_world_wide_words(int *n, float extra, float zoom) {
    *n = 0; nwords = 0;
    static int off = -1;
    if (off < 0) { const char *e = getenv("PROPCYCL_WIDE_FILL"); off = e && *e == '0'; }
    if (off || extra < 1.0f || zoom <= 0.0f) return NULL;
    /* the angle from the 4:3 edge to the wide edge, in 1/65536 turns, plus half a heading zone (0x800) so a
     * chunk list chosen for the turned heading still overlaps the edge */
    const double a = atan2(320.0 + extra, zoom) - atan2(320.0, zoom);
    const int32_t dh = (int32_t)(a * 65536.0 / (2.0 * 3.14159265358979) + 0.5) + 0x400;
    const int32_t h0 = (int32_t)W[0x0CEC];
    const int32_t hs[2] = { h0 - dh, h0 + dh };
    static int turn = -1;                       /* PROPCYCL_WIDEFILL_TURN=1: the first version's behaviour, for A/B only */
    if (turn < 0) { const char *e = getenv("PROPCYCL_WIDEFILL_TURN"); turn = e && *e == '1'; }
    if (!walk(hs, 2, "WIDEFILL", turn)) return NULL;
    *n = nwords;
    return nwords ? words : NULL;
}

const int32_t *pause_world_words(int *n) {
    if (!built) { build(); built = 1; }
    *n = nwords;
    return nwords ? words : NULL;
}
