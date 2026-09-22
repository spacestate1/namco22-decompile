/*
 * arcade_menu.c -- recreated arcade post-coin mode/course select.
 *
 * WHY THIS IS A RECREATION AND NOT A PORT
 * ---------------------------------------
 * state_stage_start_run @0x018534 dispatches through a 32-bit pointer table
 * at ROM 0x3523C:  move.l $E00CCC,D0 ; lsl.l #2,D0 ; jsr ([$3523C,D0.l]).
 * Six of its twelve slots point at bare `rts` (0x4E75) -- slots 0/1, 4/5 and
 * 8/9 -- and the six that hold real code are io_test, stage_start_debug and
 * debug_io_monitor. So the menu really is absent from THIS ROM revision;
 * unlike terrain_props_dispatch, there is no undiscovered code behind the
 * table. Everything below is therefore a reconstruction, and is labelled as
 * such rather than presented as decompiled behaviour.
 *
 * THE ASSETS: ROM 0x37990 IS THE NAME-ENTRY ALPHABET, NOT POSTERS
 * ---------------------------------------------------------------
 * An earlier revision of this file treated entries in that table as course
 * "posters" and picked indices 24 and 26 for NOVICE / ADVANCED. They are
 * letters: rendering them shows gold characters on stone tiles, and index 24
 * is "Y", 26 is "0". On screen that produced two ~30px glyphs at the edges,
 * which is what "the menu does not work" looked like.
 *
 * Verified by rendering the models: index 0..25 = A..Z, 26..35 = 0..9,
 * 36 = a symbol. The table entry is a CPU model id (0x396 + index); the
 * renderer adds 0x45 to reach the point-ROM object, so index 0 draws
 * point-ROM 0x3DB. The game's own user of this table is the name-entry
 * letter wheel (game_ending.c:1465), which is where the placement
 * convention below comes from rather than from a guess.
 *
 * SCALING
 * -------
 * The previous revision sized posters with "90 degrees FOV" arithmetic. The
 * hardware has no field of view at all -- it divides by a focal length
 * (FAILED_APPROACHES.md 1.1):
 *
 *     screen_x = 320 + (X * zoom) / Z
 *
 * so a tile of extent E at depth Z is E*zoom/Z pixels wide, and the only
 * two numbers that matter are Z and the x spacing. Both are calibrated
 * against the measured on-screen size with PROPCYCL_ZORD, not derived from
 * an invented frustum -- see MENU_Z / MENU_ADV below.
 *
 * Recreated state machine (kept in the original W[]-slot idiom):
 *   W[MENU_PHASE]     0 = idle, 1 = mode-init, 2 = mode-run,
 *                     3 = course-init, 4 = course-run, exit when 0
 *   W[MENU_SEL]       current cursor index inside the active phase
 *   W[MENU_FRAME]     phase-internal frame counter (animations)
 *   W[MENU_LOCK]      0 = picking, >0 = lock-in animation timer
 *   W[MENU_MODE]      mode chosen on phase 2 (0 = NOVICE, 1 = ADVANCED)
 */

#include "propcycl.h"
#include "rom_native.h"

extern intptr_t _W[];
#define W _W
#define R g_sys.rom

extern void dsp_cmd_set_camera(int32_t p1, int32_t p2, int32_t p3, int32_t p4,
                                int32_t p5, int32_t p6, int32_t p7, int32_t p8,
                                int32_t p9);

/* ---- W[] slots (free range above the documented map) ---- */
#define MENU_PHASE   0x16A40
#define MENU_SEL     0x16A44
#define MENU_FRAME   0x16A48
#define MENU_LOCK    0x16A4C
#define MENU_MODE    0x16A50

/* ---- ROM model-ID table — see ROM 0x37990 (model_id = 0x396 + index) ---- */
#define MODEL_TABLE_BASE 0x37990

/* Table index for a character. 0..25 = A..Z, 26..35 = 0..9 (verified by
 * rendering the models). Anything else -> -1, drawn as a gap. */
static int glyph_index(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a';
    if (c >= '0' && c <= '9') return 26 + (c - '0');
    return -1;
}

/* Read a model_id from the sequential ROM table. Stored as BE32. */
static int32_t menu_model_id(int idx) {
    return (int32_t)mem_read32(MODEL_TABLE_BASE + idx * 4);
}

/* Placement, calibrated with PROPCYCL_ZORD against the measured bbox.
 *
 * A letter tile's local extent is 164 game units (models/model_03DB.obj),
 * and screen size is extent*zoom/Z, so Z alone sets the glyph size and the
 * x step sets the gap. MENU_ADV is deliberately larger than the glyph so
 * letters do not overlap -- the game's own name-entry wheel uses a step of
 * 0x53 with the letters on an ARC, where the depth difference separates
 * them; a flat menu row has no such help. */
#define GLYPH_EXTENT 164
/* Calibrated, not guessed: at Z=2600 a glyph measured 24 px wide with
 * PROPCYCL_ZORD (code 1001 bbox x[226..250]). Screen size scales as 1/Z, so
 * Z=1450 gives 24*2600/1450 = 43 px, and the x step is scaled by the same
 * ratio to keep a small gap rather than the sparse spread 210 produced. */
#define MENU_Z       1450      /* unselected depth  -> ~43 px glyphs    */
#define MENU_Z_SEL   1250      /* selected: nearer, so it scales UP ~16% */
#define MENU_ADV      130      /* x step between glyph centres          */
#define MENU_ROW_DY   200      /* y step between rows -> ~67 px         */
/* The block measured low, overlapping the title layer's "PRESS THE START
 * BUTTON" at y~370. At this depth 130 units = 43 px, so 1 px = 3.0 units.
 * The row step and lift are sized for the WORST case, the four-row course
 * list: 3*83 = 249 px tall, centred at y~200, i.e. clear of both the top
 * edge and the title text. Positive Y is up -- see render_option_rows. */
#define MENU_BASE_Y   180

/* Spell one word centred on x=0, at depth z. Returns nothing; each glyph is
 * an inline 0x8002 placement, the same command the name-entry wheel uses. */
static void draw_word(const char *s, int y, int z)
{
    int n = 0;
    for (const char *p = s; *p; p++) n++;
    /* centre the row: first glyph sits half a row-width to the left */
    int x = -((n - 1) * MENU_ADV) / 2;
    for (const char *p = s; *p; p++, x += MENU_ADV) {
        int gi = glyph_index(*p);
        if (gi < 0) continue;                 /* space -> gap */
        dsp_cmd_set_camera(2, menu_model_id(gi), x, y, z, 0, 0, 0, 0);
    }
}

/* One menu row per option, the selected one pulled toward the camera so it
 * draws larger -- that size change IS the selection cue, exactly as the
 * poster-pop was meant to be. */
static void render_option_rows(int sel, int n, const char *const *labels)
{
    for (int i = 0; i < n; i++) {
        /* NEGATED: the projection is screen_y = cy - (Y*zoom)/Z, so +Y is UP.
         * Without this the first option drew at the BOTTOM of the list. */
        int y = MENU_BASE_Y - (i - (n - 1) / 2) * MENU_ROW_DY;
        draw_word(labels[i], y, (i == sel) ? MENU_Z_SEL : MENU_Z);
    }
}

/* Cursor index changes — left/right edge inputs (matches name_entry / bonus_check). */
static void cursor_step(int n_options) {
    /* W[0x2BDC] is the dpad-edge bitmask (bit 1 = LEFT, bit 0 = RIGHT) */
    if ((W[0x2BDC] & 2) != 0 || (W[0x2B3C] & 0x20) != 0) {
        int s = (int)W[MENU_SEL] - 1;
        if (s < 0) s = n_options - 1;
        W[MENU_SEL] = s;
    }
    if ((W[0x2BDC] & 1) != 0 || (W[0x2B3C] & 0x10) != 0) {
        int s = (int)W[MENU_SEL] + 1;
        if (s >= n_options) s = 0;
        W[MENU_SEL] = s;
    }
}

/* PROPCYCL_TEST_MENU_PICK=<n>: choose option n and confirm it after 40
 * frames, so the whole coin -> menu -> stage-start path can be exercised in
 * a headless run. Same purpose as PROPCYCL_TEST_COIN / TEST_ANALOG: there
 * is no keyboard in a --screenshot run, so without it the only thing that
 * can be verified is how the menu LOOKS, not that picking an option works. */
int g_menu_pick = -1;

/* Confirm = Start (W[0x2B3A]&0x100) or A button (W[0x2BA6]&1). */
static int confirm_pressed(void) {
    if (g_menu_pick >= 0 && (int)W[MENU_FRAME] >= 40) return 1;
    return ((W[0x2BA6] & 0x01) != 0) || ((W[0x2B3A] & 0x100) != 0);
}

/* Public step — runs once per frame from state_title_run while menu is active. */
void arcade_menu_step(void) {
    int phase = (int)W[MENU_PHASE];
    {
      static int dbg = 0;
      if (dbg < 5) {
        printf("  [MENU] step phase=%d sel=%d frame=%d lock=%d\n",
               phase, (int)W[MENU_SEL], (int)W[MENU_FRAME], (int)W[MENU_LOCK]);
        dbg++;
      }
    }
    if (phase == 0) return;

    W[MENU_FRAME] = W[MENU_FRAME] + 1;

    switch (phase) {
    case 1: /* mode-select init */
        W[MENU_SEL]   = 0;
        W[MENU_FRAME] = 0;
        W[MENU_LOCK]  = 0;
        W[MENU_PHASE] = 2;
        /* fall-through to first frame of mode-run */

    case 2: { /* mode-select run */
        if (W[MENU_LOCK] == 0) {
            cursor_step(2);
            if (confirm_pressed()) {
                W[MENU_LOCK] = 1;
                W[MENU_MODE] = (int)W[MENU_SEL];
            }
        } else {
            W[MENU_LOCK] = W[MENU_LOCK] + 1;
            if (W[MENU_LOCK] > 30) {
                if (W[MENU_MODE] == 0) {
                    /* NOVICE → course-select */
                    W[MENU_PHASE] = 3;
                } else {
                    /* ADVANCED → start full 4-stage story at course 0 */
                    W[0x0E0C] = 0;
                    W16_SET(0x3FF4, 0);
                    W[MENU_PHASE] = 0;
                    W[0x0CBC] = 4; /* → state_stage_start_init */
                }
            }
        }
        static const char *const modes[2] = { "NOVICE", "ADVANCED" };
        render_option_rows((int)W[MENU_SEL], 2, modes);
        break;
    }

    case 3: /* course-select init (NOVICE only) */
        W[MENU_SEL]  = 0;
        W[MENU_LOCK] = 0;
        W[MENU_PHASE] = 4;
        /* fall-through */

    case 4: { /* course-select run */
        if (g_menu_pick >= 0) W[MENU_SEL] = g_menu_pick % 4;
        if (W[MENU_LOCK] == 0) {
            cursor_step(4);
            if (confirm_pressed()) {
                W[MENU_LOCK] = 1;
            }
        } else {
            W[MENU_LOCK] = W[MENU_LOCK] + 1;
            if (W[MENU_LOCK] > 30) {
                W[0x0E0C] = (int)W[MENU_SEL]; /* selected course 0..2 */
                W16_SET(0x3FF4, 1);                /* NOVICE = 1-stage flag */
                W[MENU_PHASE] = 0;
                W[0x0CBC] = 4;                /* → state_stage_start_init */
            }
        }
        static const char *const courses[4] = { "COURSE 1", "COURSE 2",
                                                "COURSE 3", "COURSE 4" };
        render_option_rows((int)W[MENU_SEL], 4, courses);
        break;
    }

    default:
        W[MENU_PHASE] = 0;
        break;
    }
}

/* Trigger from title-state press-Start (with credits). Idempotent. */
void arcade_menu_trigger(void) {
    if (W[MENU_PHASE] == 0) {
        W[MENU_PHASE] = 1;
    }
}

/* Convenience predicate for state_title_run: is the menu currently driving? */
int arcade_menu_active(void) {
    return W[MENU_PHASE] != 0;
}
