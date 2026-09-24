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

static void build(void) {
    nwords = 0;
    { const char *e = getenv("PROPCYCL_PAUSE360"); if (e && *e == '0') return; }
    /* Gameplay and the intro orbit only: the menu screens run in state 3
     * too, and must not grow a landscape behind them. */
    const int state = (int)W[0x0CBC], sub = (int)W[0x0CC0];
    if (state != 3 || (sub != 3 && sub != 5)) return;

    const uintptr_t dsp = (uintptr_t)g_sys.dspram, cur = (uintptr_t)W[0x0CA4];
    if (cur < dsp || cur >= dsp + DSPRAM_SIZE) return;
    /* the buffer the frozen frame is NOT in is free while paused */
    const uint32_t other = (cur - dsp >= 0x18400) ? 0x10400 : 0x18400;
    const uint32_t room = 0x8000 - 0x400;          /* list area of one buffer, bytes */

    static intptr_t *sW;
    static uint8_t *sWram, *sDsp, *sComm;
    if (!sW) {
        sW = malloc(WORK_RAM_SIZE * sizeof *sW);
        sWram = malloc(WORK_RAM_SIZE);
        sDsp = malloc(DSPRAM_SIZE);
        sComm = malloc(COMMSRAM_SIZE);
        if (!sW || !sWram || !sDsp || !sComm) { fprintf(stderr, "[PAUSE360] out of memory\n"); return; }
    }
    memcpy(sW, _W, WORK_RAM_SIZE * sizeof *sW);
    memcpy(sWram, g_sys.work_ram, WORK_RAM_SIZE);
    memcpy(sDsp, g_sys.dspram, DSPRAM_SIZE);
    memcpy(sComm, g_sys.commsram, COMMSRAM_SIZE);

    const int32_t h0 = (int32_t)W[0x0CEC];
    for (int k = 0; k < 4; k++) {
        const int32_t h = (h0 + k * 0x4000) & 0xffff;
        int32_t *base = (int32_t *)(g_sys.dspram + other);
        W[0x0CA4] = (intptr_t)base;
        W[0x0CEC] = h;                              /* billboards face this way too */
        terrain_chunk_visibility((uint32_t)W[0x0CDC], (int)W[0x0CE0], (uint32_t)W[0x0CE4], h);
        terrain_props_dispatch();
        objects_render_master();
        balloon_render_and_hit_check();     /* its hit test changes nothing: the state is put back below */
        const intptr_t n = ((int32_t *)W[0x0CA4]) - base;
        if (n > 0 && (uint32_t)n * 4 <= room) append(base, (int)n);
        else if (n > 0) fprintf(stderr, "[PAUSE360] heading %d: %ld words overran the buffer, skipped\n", h, (long)n);
        /* every pass starts from the frozen state */
        memcpy(_W, sW, WORK_RAM_SIZE * sizeof *sW);
        memcpy(g_sys.work_ram, sWram, WORK_RAM_SIZE);
        memcpy(g_sys.dspram, sDsp, DSPRAM_SIZE);
        memcpy(g_sys.commsram, sComm, COMMSRAM_SIZE);
    }
    fprintf(stderr, "[PAUSE360] %d words from 4 headings\n", nwords);
}

const int32_t *pause_world_words(int *n) {
    if (!built) { build(); built = 1; }
    *n = nwords;
    return nwords ? words : NULL;
}
