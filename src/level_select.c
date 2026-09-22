/*
 * level_select.c -- Escape -> Levels: start any stage directly, NOVICE or
 * ADVANCED (story), without playing the ones before it.
 *
 * Nothing here invents a state the machine does not reach. A request walks
 * the game's own chain and only shortcuts the parts a player would sit
 * through:
 *
 *   1. force a stage start (state 4), exactly as the 1-4 keys and
 *      --autostart do -- state_stage_start_init resets the sync, the text
 *      layer and the sound, and the chain arrives at state_gameplay_init;
 *
 *   2. state_gameplay_init calls level_select_apply() after its own story
 *      initialisation, which puts the story variables where the machine has
 *      them on that day and skips the CONTROLS tutorial and the MODE SELECT
 *      -- entering sub 0x16 (the DAY screen) for ADVANCED, as the MODE
 *      SELECT's exit does (ROM `tst.w $e00e10` -> 0x16), or sub 0 (the
 *      STAGE SELECT) for NOVICE;
 *
 *   3. at the STAGE SELECT the cursor is put on the wanted course -- the
 *      game still maps it to a course through its own table (FUN_00008c82),
 *      the same way PROPCYCL_TEST_STAGE picks one -- and START is pressed
 *      twice through the real input path: once to choose, once to skip the
 *      plate animation, which is what a player in a hurry does.
 *
 * THE STORY STATE FOR DAY d, from the stage-clear path (results case 7):
 * every clear does `E54 += score; E2C ^= 1 << course; E64++; E66++;
 * E6A = 0`, and state_gameplay_init starts them at 0xF / 0 / 0. So the
 * natural route to course c on day c is E2C = 0xF with the bits of every
 * earlier course cleared, and E64 = E66 = c. Day 4 is course 3: the STAGE
 * SELECT's `(E2C & 7) == 0` test routes to FUN_0000971a, which sets course
 * 3 itself. The accumulated total E54 is left at 0 -- a real day-2 player
 * would carry day 1's score, and nothing on the path to the stage reads it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "propcycl.h"

#define W _W

void sync_post();
void camera_state_reset();
void sound_reset_all();
short *sound_play_or_defer();

enum { LS_IDLE, LS_PENDING, LS_SELECT };

static int ls_state = LS_IDLE;
static int ls_adv;          /* 1 = ADVANCED (story), 0 = NOVICE */
static int ls_course;       /* 0..3 */
static unsigned ls_t0;      /* frame the stage select was first seen */
static unsigned ls_armed;   /* frame the request was made */
static int     ls_end;          /* 1 = the request is the ENDING */
static int     ls_end_cleared;
static int32_t ls_end_total;


/* No getenv here: this runs inside the frame loop, where register row 40
 * makes a late getenv fault. main.c reads PROPCYCL_STAGEDBG at startup. */
extern int g_stagedbg;
static int dbg(void) { return g_stagedbg; }

static const char *course_name(int c) {
    static const char *n[4] = { "CLIFF ROCK", "WIND WOODS", "INDUSTARN", "FINAL STAGE" };
    return (c >= 0 && c < 4) ? n[c] : "?";
}

const char *level_select_name(int adv, int course) {
    static char b[64];
    if (adv) snprintf(b, sizeof b, "ADVANCED day %d  %s", course + 1, course_name(course));
    else     snprintf(b, sizeof b, "NOVICE  %s", course_name(course));
    return b;
}

int level_select_active(void) { return ls_state != LS_IDLE; }

/* Step 1: force a stage start. Same writes as main.c's 1-4 keys: work RAM
 * directly, which game_frame's sync_wram_to_W carries into _W[]. */
void level_select_request(int adv, int course) {
    if (course < 0) course = 0;
    if (course > 3) course = 3;
    if (!adv && course > 2) course = 2;   /* NOVICE offers 3 stages without the debug switch */
    ls_adv = adv ? 1 : 0;
    ls_course = course;
    ls_state = LS_PENDING;
    ls_armed = g_sys.frame_count;
    uint8_t *wr = g_sys.work_ram;
    wr[0x0CBC] = 0; wr[0x0CBD] = 0; wr[0x0CBE] = 0; wr[0x0CBF] = 4;   /* state 4 */
    wr[0x0D24] = 0; wr[0x0D25] = 0; wr[0x0D26] = 0; wr[0x0D27] = 0;
    printf("[LEVEL] %s\n", level_select_name(ls_adv, ls_course));
}

/* Step 2: called by state_gameplay_init after its own initialisation.
 * Returns 1 when it has chosen the sub-state. */
int level_select_apply(void) {
    if (ls_state != LS_PENDING) return 0;
    int c = ls_course;
    if (ls_end) {                          /* Escape -> Levels -> ENDING */
        ls_end = 0;
        sync_post();
        camera_state_reset();
        sound_reset_all();
        W[0x2C12] = 0;
        W[0x0CAC] = 0;
        W16_SET(0xE10, 1);                 /* story mode */
        W[0x0E0C] = 3;                     /* SOLITAR */
        W[0x0E18] = ls_end_cleared ? 1 : 2;/* 1 cleared -> phase 0, else phase 9 */
        W[0x0E54] = ls_end_total;          /* the story total (results after END) */
        /* The story progress on ENTERING day 4 -- the same values
         * level_select_apply puts there for ADVANCED day 4. The results screen
         * after the END card applies the stage clear itself (results case 7:
         * `E2C ^= 1 << course; E64++; E66++`, E12 when E2C reaches 0), so a
         * pre-cleared mask would be flipped back and send the game to another
         * intermission. */
        W16_SET(0xE2C, 8);
        W16_SET(0xE12, 0);
        W16_SET(0xE64, 3);
        W16_SET(0xE66, 3);
        W16_SET(0xE6A, 0);
        W[0x0CC0] = 14;                    /* gameplay_sub14_init */
        ls_state = LS_IDLE;
        if (dbg()) fprintf(stderr, "[LEVEL] f%u ending: E18=%ld total=%ld\n",
                           g_sys.frame_count, (long)W[0x0E18], (long)W[0x0E54]);
        return 1;
    }
    /* What the MODE SELECT's init (sub 12) does before the player chooses:
     * sync_post, camera_state_reset, sound_reset_all and its music, id 0x0F.
     * A day-2+ player arrives from the intermission instead, which ends the
     * music (sub 27), so the music is only started for a first stage. */
    sync_post();
    camera_state_reset();
    sound_reset_all();
    /* What a real game start does on the way in (coin_credit_update, ROM
     * 0x028B1C): disarm START and clear the pause flag. Left armed -- it is
     * whenever the request comes from attract -- the START press at the stage
     * select is read as "start a NEW game" and the state machine goes back to
     * state 2. W[0x2C12] is a pinned slot, so it is written here, inside the
     * frame, not in work RAM. */
    W[0x2C12] = 0;
    W[0x0CAC] = 0;
    W16_SET(0xE10, ls_adv);
    W[0x0E0C] = c;
    if (ls_adv) {
        W16_SET(0xE2C, 0xF & ~((1 << c) - 1));
        W16_SET(0xE64, c);
        W16_SET(0xE66, c);
        W16_SET(0xE6A, 0);
        W16_SET(0xE12, 0);
        W[0x0CC0] = 0x16;              /* the DAY screen */
    } else {
        W[0x0CC0] = 0;                 /* the STAGE SELECT */
    }
    if (!ls_adv || c == 0) sound_play_or_defer(0x0F);
    ls_state = LS_SELECT;
    ls_t0 = 0;
    if (dbg()) fprintf(stderr, "[LEVEL] f%u apply adv=%d course=%d E2C=%X E66=%d sub=%ld\n",
                       g_sys.frame_count, ls_adv, c, (unsigned)W16(0xE2C) & 0xF,
                       (int)W16(0xE66), (long)W[0x0CC0]);
    return 1;
}

/* Step 3: called by input.c once per frame BEFORE game_frame. Puts the
 * STAGE SELECT cursor on the wanted course and returns 1 on the frames
 * START should be held. */
int level_select_input_tick(void) {
    if (ls_state == LS_IDLE) return 0;
    long st  = (long)(int32_t)vrd32(0xE00000 + 0x0CBC);
    long sub = (long)(int32_t)vrd32(0xE00000 + 0x0CC0);
    unsigned f = g_sys.frame_count;
    if (ls_state == LS_PENDING) {
        /* the stage start never reached state_gameplay_init */
        if (f - ls_armed > 600) ls_state = LS_IDLE;
        return 0;
    }
    if (st != 3) { ls_state = LS_IDLE; return 0; }
    if (sub == 2 || sub == 5 || sub == 3) {
        if (dbg()) fprintf(stderr, "[LEVEL] f%u stage chosen, course=%ld\n", f, (long)W[0x0E0C]);
        ls_state = LS_IDLE;
        return 0;
    }
    if (sub != 1) return 0;           /* DAY screen, final-stage screen: let them run */
    /* ADVANCED day 4 goes through FUN_0000971a/FUN_00009792, which has no
     * cursor and advances by itself. */
    if (ls_adv && ls_course == 3) return 0;
    if (ls_t0 == 0) ls_t0 = f ? f : 1;
    /* Find the course in the list FUN_00008b6a built from E2C: bytes packed
     * big-endian in slot 0x0C7C, W[0x0C80] of them. */
    int n = (int)(W[0x0C80] & 0xFFFF), idx = -1;
    for (int i = 0; i < n && i < 4; i++) {
        int b = (int)(int8_t)(((uint32_t)_W[0x0C7C] >> (8 * (3 - i))) & 0xFF);
        if (b == ls_course) { idx = i; break; }
    }
    if (idx < 0) return 0;
    if (W16(0x0C54) == 0) _W[0x0C82] = idx;
    /* The screen needs a moment to come up (W[0x0C78] counts from 0 and
     * loads its graphics at 3). Press at +40, release, press again at +60
     * to skip the confirm animation: two EDGES, the input path derives
     * W[0x2BA6] from new & ~old. */
    unsigned d = f - ls_t0;
    return (d >= 40 && d < 44) || (d >= 60 && d < 64);
}

/* ---- skip to the ENDING (Escape -> Levels, PROPCYCL_LEVEL=end:...) --------
 * The story ending is gameplay sub 14 -> 15. gameplay_sub14_init (ROM
 * 0x02FF54) chooses the good ending (phase 0) when 0xE00E18 == 1 -- the final
 * stage CLEARED -- and the bad one (phase 9, time up) otherwise, and reads
 * nothing else the earlier days leave behind except the story TOTAL 0xE00E54
 * (the results and name-entry branch after the END card) and the story flag
 * 0xE00E10. So the jump writes exactly those, the course (3, SOLITAR), and
 * state 3 / sub 14, and lets the game's own init run. `phase` >= 0 starts at
 * that phase instead (the PROPCYCL_ENDING_PHASE path, incl. ending_phase_seed
 * for the state phases 0..4 set up). The total defaults to 8995, what MAME's
 * story captures hold on entering the ending. */
int g_ending_phase_req = -1;
void level_select_request(int adv, int course);


void level_select_ending(int cleared, int phase, int32_t total)
{
    /* Through the same stage start as a stage (state 4 -> state_gameplay_init
     * -> level_select_apply), so the game's own gameplay init runs first and
     * cannot overwrite what the ending reads; apply then enters sub 14. */
    level_select_request(1, 3);
    ls_end = 1;
    ls_end_cleared = cleared ? 1 : 0;
    ls_end_total = total;
    g_ending_phase_req = (phase >= 0 && phase < 13) ? phase : -1;
    printf("[LEVEL] ENDING (%s)\n", cleared ? "cleared" : "bad, time up");
    if (g_ending_phase_req >= 0) printf("[LEVEL]   phase %d\n", g_ending_phase_req);
}

/* PROPCYCL_LEVEL=end:<clear|bad>[:phase][@frame] -- the ending. Returns 1 and
 * fills the fields when `s` names the ending. */
int level_select_parse_ending(const char *s, int *cleared, int *phase, long *frame)
{
    const char *p, *at;
    if (!s || strncmp(s, "end", 3) != 0) return 0;
    *cleared = 1; *phase = -1; *frame = 10;
    p = strchr(s, ':');
    if (p) {
        p++;
        if (!strncmp(p, "bad", 3) || *p == 'b' || *p == 'f') *cleared = 0;
        p = strchr(p, ':');
        if (p) *phase = atoi(p + 1);
    }
    at = strchr(s, '@');
    if (at) *frame = atol(at + 1);
    return 1;
}

/* PROPCYCL_LEVEL=<adv|nov>:<course>[@frame] / --level: the headless form. */
int level_select_parse(const char *s, int *adv, int *course, long *frame) {
    if (!s || !*s || !strncmp(s, "end", 3)) return 0;
    *adv = 1; *frame = 10;
    if (!strncmp(s, "adv", 3) || s[0] == 'a' || s[0] == 'A') *adv = 1;
    else if (!strncmp(s, "nov", 3) || s[0] == 'n' || s[0] == 'N') *adv = 0;
    const char *p = strchr(s, ':');
    p = p ? p + 1 : s + strcspn(s, "0123456789");
    *course = atoi(p);
    const char *at = strchr(s, '@');
    if (at) *frame = atol(at + 1);
    return 1;
}
