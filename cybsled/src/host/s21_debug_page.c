/*
 * s21_debug_page.c -- the menu's Debug page: the developer TEST MODE left in Cyber Sled's ROM (src/board/s21_debug.c, PLAN.md
 * "Module I -- debug screens"), launched the game's own way:
 *   1. the Test switch OFF until the master has left any test mode (shared 0x900010 != 2) -- the game's own reset;
 *   2. the developer dispatch on (s21_debug_enable), the Test switch ON: the master resets into its test-mode loop, which now runs
 *      the developer menu (0x2BB0) and the slave its pages (0x1C24);
 *   3. for a page: once the menu is up (master page 0x100C14 == 0, slave mode 0), the menu's cursor (shared 0x902200) is set to
 *      the row and the Gun pressed for 6 frames -- the menu's own "select" (0x2C34: rising edge of Gun; it then clears the page's
 *      parameters at 0x2C82 and sets the page from the table at 0x2C72).
 * "Return to game": the Test switch OFF (the program's own way out of its test mode: 0x2088), then the developer dispatch off once
 * the master has left the test loop. The page holds the Test switch while a screen runs; the Controls page's own switch is turned
 * off at launch so it is off afterwards.
 * Headless (tests): CS_DEBUG=<row>@<frame>[,r@<frame>] -- row 0 = the menu, 1..7 = the table below; r = return to game.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "s21_board.h"
#include "s21_debug.h"
#include "s21_io.h"
#include "s21_host.h"

/* the menu rows (slave strings 0x1E1C..0x1ED8; master cursor -> page table 0x2C72 = 1,2,3,4,5,6,6,7) */
static const struct { const char *name, *desc; int cursor; } screens[] = {
    { "Developer test menu",        "TEST MODE MENU: both levers forward/back move the cursor, Gun selects", -1 },
    { "Frame buffer object test",   "C355 sprite viewer: link no, offset, position, size, palette, bank", 0 },
    { "Polygon object test",        "3D object viewer: Gun/Missile = object no +/-, levers move the row / rotate", 1 },
    { "Switch test (state & value)", "A/D values and switch bits; both triggers back to the menu", 2 },
    { "Adjuster",                   "3-2-1 countdown, then the levers' positions become their CENTRES (NVRAM)", 3 },
    { "Sound test",                 "voice no, BGM no, sound stop; EXIT + Gun", 4 },
    { "Vehicle test (not used)",    "the menu's own 'NOT USED': the title backdrop; Gun exits", 5 },
    { "Colour check",               "the palette as a grid; Gun exits", 7 },
};
#define NSCR ((int)(sizeof screens / sizeof screens[0]))

/* the two developer DIP switches the master still reads every frame of its main loop (entry_reset, 0x12B8..0x1324):
 *   DSW2 (DSW bit 1): 0x10C004 = 1 -> the frame's game work is skipped (0x1354 bne 0x14BC): the game FREEZES;
 *   DSW7 (DSW bit 6): a rising edge of DPRAM byte 0 bit 3 toggles a pause (0x10C002), bit 2 lets ONE frame run (0x10C006). The C68
 *   puts cabinet port MCUB bits 5 and 3 there (measured: MCUB 0x20 -> bit 3, 0x08 -> bit 2) -- inputs MAME does not map. */
static int dip_freeze, dip_pause, pulse_pause, pulse_step;

enum { IDLE, LEAVE, ENTER, SELECT, ACTIVE, EXIT };
static int st = IDLE, target, cnt, outside;
static const char *state_name[] = { "idle", "leaving test mode", "entering the developer menu", "selecting", "running", "returning" };

static void launch(int r)
{
    if (r < 0 || r >= NSCR) return;
    s21_input_set_test(0);
    target = r; st = LEAVE; cnt = 0; outside = 0;
    fprintf(stderr, "[DEBUG] launch: %s (frame %u)\n", screens[r].name, g_s21.frame);
}
static void leave(void)
{
    if (st == IDLE) return;
    st = EXIT; outside = 0;
    fprintf(stderr, "[DEBUG] return to game (frame %u)\n", g_s21.frame);
}

void s21_debug_frame(s21_inputs *in)
{
    { static int init; static int f_l = -1, f_r = -1, row;            /* CS_DEBUG=<row>@<frame>[,r@<frame>] (tests) */
      if (!init) { init = 1; const char *e = getenv("CS_DEBUG"); if (e) { sscanf(e, "%d@%d", &row, &f_l); const char *c = strchr(e, ','); if (c) sscanf(c, ",r@%d", &f_r); } }
      if ((int)g_s21.frame == f_l) launch(row);
      if ((int)g_s21.frame == f_r) leave(); }
    if (dip_freeze) in->dsw &= (uint8_t)~0x02;
    if (dip_pause) in->dsw &= (uint8_t)~0x40;
    if (pulse_pause > 0) { if (pulse_pause <= 4) in->mcub &= (uint8_t)~0x20; pulse_pause--; }   /* 2 frames released, then 4 held */
    if (pulse_step > 0) { if (pulse_step <= 4) in->mcub &= (uint8_t)~0x08; pulse_step--; }
    const int intest = g_s21.shared[0x10 >> 1] == 2;
    switch (st) {
    case IDLE: return;
    case LEAVE:                                         /* the Test switch off until the master is out of any test mode */
        in->dsw |= 1;
        outside = intest ? 0 : outside + 1;
        if (outside >= 2) { s21_debug_enable(1); st = ENTER; cnt = 0; }
        return;
    case ENTER:
        in->dsw &= (uint8_t)~1;
        if (s21_debug_dev_entered() && g_s21.mram[0xC14 >> 1] == 0 && g_s21.shared[0x14 >> 1] == 0) {
            if (screens[target].cursor < 0) { st = ACTIVE; fprintf(stderr, "[DEBUG] developer menu up (frame %u)\n", g_s21.frame); }
            else { g_s21.shared[0x2200 >> 1] = (uint16_t)screens[target].cursor; st = SELECT; cnt = 0; }
        }
        return;
    case SELECT:                                        /* the menu's own select: Gun, 6 frames */
        in->dsw &= (uint8_t)~1;
        if (cnt < 6) in->mcuh &= (uint8_t)~0x20;
        if (++cnt >= 8) { st = ACTIVE; fprintf(stderr, "[DEBUG] %s up (frame %u, page %u)\n", screens[target].name, g_s21.frame, g_s21.mram[0xC14 >> 1]); }
        return;
    case ACTIVE:
        in->dsw &= (uint8_t)~1;
        return;
    case EXIT:                                          /* the Test switch off; the developer dispatch off once out of the loop */
        in->dsw |= 1;
        outside = intest ? 0 : outside + 1;
        if (outside >= 2) { s21_debug_enable(0); st = IDLE; fprintf(stderr, "[DEBUG] back in the game (frame %u)\n", g_s21.frame); }
        return;
    }
}

/* the page: one row per screen, "Return to game", then the developer DIP switches */
enum { R_RET = NSCR, R_FREEZE, R_PAUSEDIP, R_PAUSE, R_STEP, R_N };
static int nrows(void) { return R_N; }
static bool has_value(int r) { return r == R_FREEZE || r == R_PAUSEDIP; }
static bool enabled(int r) { return r < NSCR || r == R_FREEZE || r == R_PAUSEDIP || (r == R_RET && st != IDLE) || ((r == R_PAUSE || r == R_STEP) && dip_pause); }
static void text(int r, char *l, size_t ln, char *v, size_t vn)
{
    if (r < NSCR) { snprintf(l, ln, "%s", screens[r].name); snprintf(v, vn, "%s", (st != IDLE && target == r) ? state_name[st] : ""); }
    else if (r == R_RET) { snprintf(l, ln, "Return to game"); snprintf(v, vn, "%s", st == IDLE ? "" : "Test switch off: the game resets"); }
    else if (r == R_FREEZE) { snprintf(l, ln, "DSW2: freeze"); snprintf(v, vn, "%s", dip_freeze ? "ON (the game stops)" : "OFF"); }
    else if (r == R_PAUSEDIP) { snprintf(l, ln, "DSW7: pause / frame step"); snprintf(v, vn, "%s", dip_pause ? "ON" : "OFF"); }
    else if (r == R_PAUSE) { snprintf(l, ln, "  Pause / resume"); snprintf(v, vn, "%s", g_s21.mram[0xC002 >> 1] ? "paused" : "running"); }
    else { snprintf(l, ln, "  Step one frame"); snprintf(v, vn, "(while paused)"); }
}
static void change(int r, int dir)
{
    if (r == R_FREEZE) { dip_freeze = !dip_freeze; return; }
    if (r == R_PAUSEDIP) { dip_pause = !dip_pause; return; }
    if (dir) return;
    if (r < NSCR) launch(r);
    else if (r == R_RET) leave();
    else if (r == R_PAUSE) pulse_pause = 6;
    else if (r == R_STEP) pulse_step = 6;
}
static void notes(void (*line)(const char *fmt, ...))
{
    line("Developer screens left in the ROM; the release reaches none of them.");
    for (int i = 0; i < NSCR; i++) line("%s: %s", screens[i].name, screens[i].desc);
    line("In the menu: both levers forward / back move the cursor, Gun selects. EXIT + Gun (or Gun, or both triggers) back to it.");
    line("DSW2 / DSW7: the board's developer DIP switches, read by the game every frame (pause input: cabinet MCUB bits 5 / 3).");
}
static const eng_ui_page page = { "Debug", 560, 230, 0, nrows, has_value, enabled, text, change, notes };
const eng_ui_page *s21_debug_page(void) { return &page; }
