/*
 * test_mode.c -- PROP CYCLE'S OPERATOR TEST MODE (F2), ported from the 68K at 0x01A15E..0x01C0B0 (GitHub #26, 2026-10-05).
 *
 * The test mode is a page machine: state_title_run (0x01A2AE) calls the handler for the PAGE word at 0xE03FB4 through the
 * 34-entry table at 0x36548 (even = a page's init, odd = its run). Before this file the handlers were Ghidra's text with every
 * kind of transpilation fault at once, and contradicting each other about where things live:
 *   - the page word (16-bit, the HIGH half of slot 0xE03FB4) was read and written as the whole slot, and the per-page cursor
 *     BYTES at 0xE03FB6 + page/2 share that slot (pages 0..3) -- so setting the page wiped two cursors, and a cursor move
 *     changed the page;
 *   - the cursor was moved through a `char *` into the 8-byte _W[] array (somewhere else entirely) while the menu drew it from
 *     work RAM: Up/Down never moved anything;
 *   - the option ITEM RECORDS (24 bytes, see below) were read through host pointers in native byte order, and one caller
 *     passed the bare integer 0x6714 for ROM 0x36714 and the copy segfaulted -- "it closes suddenly";
 *   - wrong call arguments (the legend dispatch's case, label counts of 0 where the ROM passes 0x100).
 * This file is the whole machine again, from the disassembly, with ONE memory convention: byte offsets into the _W[] slot model
 * through W8 / W16 / whole 4-aligned slots -- the same storage the rest of the game and the per-frame sync use. Where a REAL
 * setting is shared with the game (the coin and game options' load/save), it is read and written the way the game's own
 * readers do (noted per field). Ground truth for the screens: work/gh26/mame_test/ (MAME's own test mode, page by page).
 *
 * ITEM RECORD (24 bytes; ROM tables and their work-RAM copy at 0xE0AB04):
 *   +0x00 u32  the value's address        +0x04 u32  display: 0 decimal, -1 hex digits, -2 minutes'seconds"frames, else a
 *   +0x08 s16  column  +0x0A s16  row                 table of string pointers indexed by the value
 *   +0x0C s16  minimum +0x0E s16  maximum  +0x10 s16  width (digits)  +0x12 s16  the default (drawn green when equal)
 *   +0x14 s16  the value's size: 1 signed byte, 2 signed word, 3 unsigned word, 4 long (else signed word)
 *   +0x16 s16  the item's cursor index (-1 = not selectable)
 * LABEL RECORD (12 bytes): +0 u32 string, +4 col, +6 row, +8 palette, +0xA cursor index.
 */
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "propcycl.h"
#include "vaddr.h"
#include "test_mode.h"

extern intptr_t _W[];
#define W _W

#define WRAM0 0xE00000u

/* ---- memory: a 68K address, through the same storage the game uses ---------------------------------------------------- */
static int is_wram(uint32_t a) { return a >= WRAM0 && a < WRAM0 + WORK_RAM_SIZE; }
int32_t tm_r8(uint32_t a)  { return is_wram(a) ? (int8_t)W8(a - WRAM0) : (int8_t)vrd8(a); }
/* FIELDS THE GAME KEEPS AS WHOLE SLOTS (the value in the low bits of _W[off], whatever the alignment; the 2-mod-4 ones pinned): the
 * handlebar ADCs and their centres (input.c), the sound settings (settings_audio_defaults, sound_env_apply), the DIP byte. A test-mode
 * page that shows or sets one must use the same storage as the game, or it shows 0 and its change never arrives. */
static int whole_slot(uint32_t o)
{
    switch (o) {
    case 0x2BC8: case 0x2BCA: case 0x3FD0: case 0x3FD2:
    case 0x3FE0: case 0x3FE2: case 0x3FE4: case 0x3FE6: case 0x3FE8:
    case 0x2C0C: return 1;
    }
    return 0;
}
int32_t tm_r16(uint32_t a)
{
    if (!is_wram(a)) return (int16_t)vrd16(a);
    return whole_slot(a - WRAM0) ? (int16_t)W[a - WRAM0] : (int16_t)W16(a - WRAM0);
}
uint32_t tm_r32(uint32_t a)
{
    if (!is_wram(a)) return vrd32(a);
    const uint32_t o = a - WRAM0;
    if (!(o & 3)) return (uint32_t)(int32_t)W[o];
    return ((uint32_t)(uint16_t)W16(o) << 16) | (uint16_t)W16(o + 2);
}
void tm_w8(uint32_t a, int32_t v)  { if (is_wram(a)) W8_SET(a - WRAM0, v); else vwr8(a, (uint8_t)v); }
void tm_w16(uint32_t a, int32_t v)
{
    if (!is_wram(a)) { vwr16(a, (uint16_t)v); return; }
    if (whole_slot(a - WRAM0)) W[a - WRAM0] = (int16_t)v; else W16_SET(a - WRAM0, v);
}
void tm_w32(uint32_t a, uint32_t v)
{
    if (!is_wram(a)) { vwr32(a, v); return; }
    const uint32_t o = a - WRAM0;
    if (!(o & 3)) { W[o] = (intptr_t)(int32_t)v; return; }
    W16_SET(o, v >> 16); W16_SET(o + 2, v);
}

/* the page word and its cursor byte */
#define PAGE()        ((int)(int16_t)W16(0x3FB4))
#define SET_PAGE(n)   W16_SET(0x3FB4, (n))
#define CUR_OFF(p)    (0x3FB6u + (unsigned)((p) >> 1))
int  tm_page(void) { return PAGE(); }
void tm_set_page(int n) { SET_PAGE(n); }
int  tm_cursor(void) { return (int8_t)W8(CUR_OFF(PAGE())); }
void tm_set_cursor(int v) { W8_SET(CUR_OFF(PAGE()), v); }

/* the inputs the menus test, read the way their producers write them this frame (src/game_misc.c read_mcu_inputs_process,
 * input_read_service_buttons): the stick EDGE 0x2BDA is the LOW half of slot 0x2BD8; 0x2BDC (edge + auto-repeat) is written
 * to the whole slot; 0x2BA4/0x2BA6 (service byte, its edge) and 0x2B80 (switch byte) are whole slots; 0x2B38/0x2B3A/0x2B3C
 * come from the button decoder this cabinet does not have and stay 0, as on a machine with nothing wired there. */
#define IN_EDGE()     ((uint16_t)W_LO16(0x2BD8))
#define IN_REP()      ((uint16_t)W[0x2BDC])
#define IN_START()    ((uint16_t)W[0x2BA6])
#define IN_SVC()      ((uint16_t)W[0x2BA4])
#define IN_SW()       ((uint16_t)W[0x2B80])
#define IN_B3A()      ((uint16_t)W[0x2B3A])
#define IN_B3C()      ((uint16_t)W[0x2B3C])
#define FRAMECNT()    ((uint32_t)W[0x0C98])
unsigned tm_in_edge(void)   { return IN_EDGE(); }
unsigned tm_in_rep(void)    { return IN_REP(); }
unsigned tm_in_dir(void)    { return (uint16_t)W_HI16(0x2BD8); }
unsigned tm_in_start(void)  { return IN_START(); }
unsigned tm_in_svc(void)    { return IN_SVC(); }
unsigned tm_in_sw(void)     { return IN_SW(); }
unsigned tm_in_swedge(void) { return (uint16_t)W[0x2B82]; }
uint32_t tm_frame(void)     { return FRAMECNT(); }

/* the text layer: 64 tiles of 16 bits a row at 0x89E000 */
static void tile(int col, int row, unsigned v) { vwr16(TEXTRAM_BASE + (uint32_t)(row * 0x80 + col * 2), (uint16_t)v); }

/* ---- the drawing helpers ---------------------------------------------------------------------------------------------- */
extern void text_print_string(int col, int row, char *s, int pal);       /* 0x02108C; takes a ROM offset or a host pointer */
extern void text_draw_hex_digits(int col, int row, int n, uint32_t v, int pal);   /* 0x0210CC */
static void print_rom(int col, int row, uint32_t s, int pal)
{
    if (!is_wram(s)) { text_print_string(col, row, (char *)(uintptr_t)s, pal); return; }
    for (int i = 0; i < 64 && col + i < 64; i++) {            /* a string in work RAM (the SOUND TEST's MCU message): 0x02108C, byte by byte */
        const int c = (uint8_t)tm_r8(s + (uint32_t)i);
        if (!c) break;
        tile(col + i, row, ((unsigned)pal << 12) | (unsigned)c);
    }
}
void tm_tile(int col, int row, unsigned v) { tile(col, row, v); }
void tm_print(int col, int row, uint32_t s, int pal) { print_rom(col, row, s, pal); }
void tm_hex(int col, int row, int n, uint32_t v, int pal) { text_draw_hex_digits(col, row, n, v, pal); }

/* 0x01A3C2: `width` decimal digits ending at col+width-1, value | attr per tile (digits are tile codes 0..9) */
void tm_draw_decimal(int col, int row, int width, int32_t value, uint32_t attr)
{
    for (int i = 0; i < width; i++) {
        tile(col + width - 1 - i, row, (uint16_t)((value % 10) | attr));
        value /= 10;
    }
}

/* 0x01A32C: a frame count as time -- width digits with ' after the 4th and " after the 2nd (counted from the right), the radixes
 * from the BE16 table at 0x36682; the value is first divided by 60 (frames -> seconds) */
void tm_draw_time(int col, int row, int width, uint32_t value, int pal)
{
    const uint32_t attr = (uint32_t)pal << 12;
    int pos = width > 4 ? width + 2 : width > 2 ? width + 1 : width;
    uint32_t v = value / 60;
    for (int i = 0; i < width; i++) {
        if (i == 2) tile(col + --pos, row, 0x22 + attr);        /* '"' */
        else if (i == 4) tile(col + --pos, row, 0x27 + attr);   /* '\'' */
        const uint32_t base = (uint32_t)(int16_t)vrd16(0x36682 + i * 2);
        tile(col + --pos, row, (v % base) + attr);
        v /= base;
    }
}

/* 0x01A40E: the control legend at the bottom of a page, seven forms (palette 12) */
void tm_legend(int n)
{
    switch (n) {
    case 0: print_rom(0x16, 0x1B, 0x36654, 12); /* fall through */
    case 1: print_rom(4, 0x1B, 0x3663D, 12); break;
    case 2: print_rom(1, 0x1B, 0x3A528, 12); break;
    case 3: print_rom(4, 0x19, 0x3663D, 12); print_rom(0x16, 0x19, 0x36654, 12); print_rom(4, 0x1B, 0x3661C, 12); break;
    case 4: print_rom(4, 0x17, 0x3A54E, 12); print_rom(4, 0x19, 0x36654, 12); print_rom(4, 0x1B, 0x3661C, 12); break;
    case 5: print_rom(4, 0x1B, 0x36649, 12); break;
    case 6: print_rom(2, 0x19, 0x3A567, 12); print_rom(0x16, 0x19, 0x36649, 12); print_rom(2, 0x1B, 0x3A574, 12); break;
    }
}

/* 0x01A55A: label records; the one whose index is the page's cursor blinks in palette 2 (frame bit 3) when count > 0x7F */
void tm_labels(uint32_t t, int count)
{
    const int sel = tm_cursor();
    int n = 0;
    do {
        if (++n > count) return;
        int pal = (int16_t)vrd16(t + 8);
        if ((int16_t)vrd16(t + 0xA) == sel && (FRAMECNT() & 8) && count > 0x7F) pal = 2;
        print_rom((int16_t)vrd16(t + 4), (int16_t)vrd16(t + 6), vrd32(t), pal);
        t += 0xC;
    } while (vrd32(t) != 0);
}

/* 0x01A5CE / 0x01A614 */
int32_t tm_get_value(uint32_t rec)
{
    const uint32_t p = tm_r32(rec);
    switch (tm_r16(rec + 0x14)) {
    case 1:  return tm_r8(p);
    case 3:  return (uint16_t)tm_r16(p);
    case 4:  return (int32_t)tm_r32(p);
    default: return tm_r16(p);
    }
}
void tm_set_value(uint32_t rec, int32_t v)
{
    const uint32_t p = tm_r32(rec);
    switch (tm_r16(rec + 0x14)) {
    case 1:  tm_w8(p, v); break;
    case 4:  tm_w32(p, (uint32_t)v); break;
    default: tm_w16(p, v); break;
    }
}

/* 0x01A652: the items' values. Per item a highlight word from 0xE168EC (+2 per item): green (0x4000) when the value is the
 * default, red blink (0x2000) on the cursor's item; then the value as a number, hex, time or a string from its table. */
void tm_draw_items(uint32_t rec, int count)
{
    const int sel = tm_cursor();
    uint32_t hl = WRAM0 + 0x168EC;
    int n = 0;
    do {
        if (++n > count) return;
        uint32_t attr = (uint16_t)tm_r16(hl); hl += 2;
        const int32_t v = tm_get_value(rec);
        if (v == tm_r16(rec + 0x12) && tm_r16(rec + 0x16) != -1) attr = 0x4000;
        if (tm_r16(rec + 0x16) == sel && count > 0x7F) {
            if (FRAMECNT() & 8) attr = 0x2000;
            else if (attr == 0x2000) attr = 0;
        }
        const int col = tm_r16(rec + 8), row = tm_r16(rec + 0xA), width = tm_r16(rec + 0x10);
        const uint32_t mode = tm_r32(rec + 4);
        if (mode == 0) tm_draw_decimal(col, row, width, v, attr);
        else if (mode == 0xFFFFFFFFu) {                                   /* hex digits straight into the tilemap */
            uint32_t d = (uint32_t)v;
            for (int i = 0; i < width; i++) { tile(col + width - 1 - i, row, (d & 0xF) | attr); d = (uint32_t)((int32_t)d >> 4); }
        }
        else if (mode == 0xFFFFFFFEu) tm_draw_time(col, row, width, (uint32_t)v, (int)(attr >> 12));
        else print_rom(col, row, vrd32(mode + (uint32_t)v * 4), (int)(attr >> 12));
        rec += 0x18;
    } while (tm_r32(rec) != 0);
}

/* 0x01A79A: up/down on the stick edge (bit 2 up, bit 3 down) moves the page's cursor over `count` entries, wrapping */
int tm_page_select(int count)
{
    int moved = 0;
    const uint32_t c = CUR_OFF(PAGE());
    if ((IN_EDGE() & 4) || (IN_B3A() & 0x80)) {
        int8_t v = (int8_t)(W8(c) - 1);
        if (v < 0) v = (int8_t)(count - 1);
        W8_SET(c, v); moved = 1;
    }
    if ((IN_EDGE() & 8) || (IN_B3A() & 0x40)) {
        int8_t v = (int8_t)(W8(c) + 1);
        if (v >= count) v = 0;
        W8_SET(c, v); moved = 1;
    }
    return moved;
}

/* 0x01A806: left/right (0x2BDC bit 1 / bit 0, with auto-repeat) changes the value of the item under the cursor, wrapping
 * between min and max; returns the change (+-1, +-0x11 on a wrap) or 0 */
int tm_value_adjust(uint32_t rec)
{
    int r = 0;
    const int sel = tm_cursor();
    while (tm_r16(rec + 0x16) != sel) { rec += 0x18; if (tm_r32(rec) == 0) return 0; }
    int32_t v = tm_get_value(rec);
    if ((IN_REP() & 2) || (IN_B3C() & 0x20)) {
        v--;
        if (v < tm_r16(rec + 0xC)) { v = tm_r16(rec + 0xE); r -= 0x10; }
        r--;
    }
    if ((IN_REP() & 1) || (IN_B3C() & 0x10)) {
        v++;
        if (v > tm_r16(rec + 0xE)) { v = tm_r16(rec + 0xC); r += 0x10; }
        r++;
    }
    tm_set_value(rec, v);
    return r;
}

/* 0x01A8B0: the item records (24 bytes each, through the one whose first long is 0) -> the work copy at 0xE0AB04 */
void tm_copy_to_work(uint32_t rom)
{
    uint32_t d = WRAM0 + 0xAB04;
    for (;;) {
        for (int k = 0; k < 0x18; k += 4) tm_w32(d + k, vrd32(rom + k));
        const uint32_t first = vrd32(rom);
        d += 0x18; rom += 0x18;
        if (!first) break;
    }
}
/* 0x01A8E2: one cleared highlight word per record */
void tm_clear_highlights(uint32_t rec)
{
    uint32_t hl = WRAM0 + 0x168EC;
    for (;;) {
        tm_w16(hl, 0); hl += 2;
        const uint32_t first = tm_r32(rec);
        rec += 0x18;
        if (!first) break;
    }
}
/* 0x01A8FC: Start (the service byte's edge bit 0), or the decoder's 0x800 / 0x100 */
int tm_confirm(void) { return (IN_START() & 1) || (IN_B3A() & 0x800) || (IN_B3A() & 0x100); }

/* =========================================================================================================================
 * THE PAGES
 * ========================================================================================================================= */
extern void sync_post(void);                            /* 0x02106A */
extern void eeprom_write_block(intptr_t addr, int n);   /* 0x019F1A */

/* 0x01A928: the menu's init */
static void p00_init(void) { sync_post(); tm_legend(0); SET_PAGE(1); }
/* 0x01A940: the MENU -- seven entries, Start goes to the page in the BE16 table at 0x36704 */
static void p01_menu(void)
{
    tm_page_select(7);
    tm_labels(0x36698, 0x100);
    if (tm_confirm()) SET_PAGE((int16_t)vrd16(0x36704 + tm_cursor() * 2));
}

/* ---- COIN OPTIONS (pages 2/3): game cost and continue cost, free play. Scratch 0xE1691C (long), 0xE16920 (long), 0xE16928 (w).
 * The real settings: game cost 0xE03FF0 -- the rest of the game reads it as the WHOLE slot (coin_credit_update, register row 60);
 * continue cost 0xE03FF6 and free play 0xE03FF4 as 16-bit words (W16). */
static void p02_coin_init(void)                         /* 0x01A978 */
{
    sync_post();
    tm_legend(3);
    tm_copy_to_work(0x36714);
    tm_clear_highlights(0x36714);
    if (IN_SVC() & 2) { tm_w16(WRAM0 + 0xAB16, 4); tm_w16(WRAM0 + 0xAB46, 2); }   /* DIP: the other defaults */
    tm_w32(WRAM0 + 0x1691C, (uint32_t)(int32_t)(int16_t)W[0x3FF0]);
    tm_w32(WRAM0 + 0x16920, (uint32_t)(int32_t)(int16_t)W16(0x3FF6));
    tm_w16(WRAM0 + 0x16928, W16(0x3FF4));
    SET_PAGE(3);
}
static void p03_coin_save(void)                         /* 0x01A9E8 (also on leaving test mode from this page) */
{
    W[0x3FF0] = (int16_t)tm_r32(WRAM0 + 0x1691C);
    W16_SET(0x3FF6, (int16_t)tm_r32(WRAM0 + 0x16920));
    W16_SET(0x3FF4, tm_r16(WRAM0 + 0x16928));
    eeprom_write_block(0xE03FF0, 8);
}
static void p03_coin_run(void)                          /* 0x01AA1C */
{
    const uint32_t work = WRAM0 + 0xAB04;
    tm_labels(0x36774, 0x100);
    tm_draw_items(work, 0x100);
    for (int i = 0; i < 2; i++) {                       /* "COIN(S)" / "COIN" after each cost: singular when the cost is 1 */
        const uint32_t p = tm_r32(work + (uint32_t)i * 0x18);
        const int one = tm_r32(p) == 1;
        print_rom((int16_t)vrd16(0x367EC + i * 4), (int16_t)vrd16(0x367EE + i * 4), vrd32(0x36614 + (one ? 0 : 4)), 0);
    }
    if (tm_page_select(4)) p03_coin_save();
    if (tm_value_adjust(work)) tm_w16(WRAM0 + 0x168EC + (uint32_t)(int8_t)W8(0x3FB7) * 2, 0x2000);
    if (tm_confirm() && (int8_t)W8(0x3FB7) == 3) { SET_PAGE(0); W8_SET(0x3FB7, 0); }
}

/* ---- GAME OPTIONS (pages 4/5): play time, difficulty?, score table reset, attract sound. Scratch 0xE1692A (w), 0xE16920 (long),
 * 0xE16928 (w: "reset the scores" on exit), 0xE1692C (w). The real settings 0xE03FFA, 0xE03FF8, 0xE03FFC: the game reads 0xE03FFA
 * and 0xE03FFC as WHOLE slots (game_misc.c, game_dsp3d.c, game_gameplay.c; 0xE03FFA is pinned) and 0xE03FF8 as a word. */
static void p04_game_init(void)                         /* 0x01AB2A */
{
    sync_post();
    tm_legend(1);
    tm_clear_highlights(0x367F4);
    tm_w16(WRAM0 + 0x1692A, (int16_t)W[0x3FFA]);
    tm_w32(WRAM0 + 0x16920, (uint32_t)(int32_t)(int16_t)W16(0x3FF8));
    tm_w16(WRAM0 + 0x16928, 0);
    tm_w16(WRAM0 + 0x1692C, (int16_t)W[0x3FFC]);
    SET_PAGE(5);
}
static void p05_game_save(void)                         /* 0x01AB76 */
{
    W[0x3FFA] = tm_r16(WRAM0 + 0x1692A);
    W16_SET(0x3FF8, (int16_t)tm_r32(WRAM0 + 0x16920));
    W[0x3FFC] = tm_r16(WRAM0 + 0x1692C);
    eeprom_write_block(0xE03FF8, 6);
}
static void p05_game_run(void)                          /* 0x01ABA8 */
{
    tm_draw_items(0x367F4, 0x100);
    tm_labels(0x3686C, 0x100);
    if (tm_page_select(5)) p05_game_save();
    if (tm_value_adjust(0x367F4)) tm_w16(WRAM0 + 0x168EC + (uint32_t)(int8_t)W8(0x3FB8) * 2, 0x2000);
    if (tm_confirm() && (int8_t)W8(0x3FB8) == 4) {
        if (tm_r16(WRAM0 + 0x16928)) { extern void highscore_table_reset_defaults(void); highscore_table_reset_defaults(); }   /* 0x01AC52 */
        SET_PAGE(0); W8_SET(0x3FB8, 0);
    }
}

/* 0x01AC5A: the I/O TEST menu's init */
static void p06_init(void) { sync_post(); tm_legend(0); SET_PAGE(7); }

/* =========================================================================================================================
 * THE MACHINE
 * ========================================================================================================================= */
static void (* const pages[34])(void) = {
    p00_init, p01_menu, p02_coin_init, p03_coin_run, p04_game_init, p05_game_run, p06_init, tm_p07,
    tm_p08, tm_p09, tm_p10, tm_p11, tm_p12, tm_p13, tm_p14, tm_p15,
    tm_p16, tm_p17, tm_p18, tm_p19, tm_p20, tm_p21, tm_p22, tm_p23,
    tm_p24, tm_p25, tm_p26, tm_p27, tm_p28, tm_p29, tm_p30, tm_p31,
    tm_p32, tm_p33,
};

/* 0x01A15E: leaving test mode -- mark the EEPROM, sync it, and restart the game from the top (0x00BC4C) */
static void leave(void)
{
    extern void eeprom_sync_all(void), entry_reset(void);
    W16_SET(0x3FEA, 1);
    eeprom_sync_all();
    entry_reset();
}

/* 0x01A2AE: one frame. The page's handler from the table at 0x36548; then, with the Test switch OFF (0xE02B80 bit 3), leave --
 * saving the option page that was open, folding an init page to its menu, the ROM test and the ADS pages back to 0x16 -- and
 * keep 0xE03FB0 = 0xE04010 (the clock at which test mode was last left; state_title_init compares against it). In a game
 * (0xE00E30 set) only when 0xE02B5E bit 8 is set, as on the machine. */
void tm_state_title_run(void)
{
    { static int log = -1, lp = -99, lc = -99; static unsigned li = 0xFFFF;   /* PROPCYCL_TMLOG=1: the page, its cursor and the inputs on every change */
      if (log < 0) { const char *e = getenv("PROPCYCL_TMLOG"); log = e && *e && *e != '0'; }
      const unsigned in = IN_EDGE() | IN_REP() << 4 | (IN_START() & 1) << 8 | (IN_SW() & 0xFF) << 9;
      if (log && (PAGE() != lp || tm_cursor() != lc || in != li)) {
          fprintf(stderr, "[TM] f%u page %d cursor %d  edge %X rep %X start %X sw %X svc %X\n", (unsigned)W[0x0C98], PAGE(), tm_cursor(),
                  IN_EDGE(), IN_REP(), IN_START(), IN_SW(), IN_SVC());
          lp = PAGE(); lc = tm_cursor(); li = in;
      } }
    const int pg = PAGE();
    if (pg >= 0 && pg < 34) pages[pg]();
    if (W[0x0E30] != 0 && !((uint16_t)W16(0x2B5E) & 0x100)) return;
    if (IN_SW() & 8) return;
    if (PAGE() == 3) p03_coin_save();
    if (PAGE() == 5) p05_game_save();
    SET_PAGE(PAGE() & 0xFFFE);
    if (PAGE() == 0x18 || PAGE() == 0x1E) SET_PAGE(0x16);
    tm_w32(WRAM0 + 0x3FB0, tm_r32(WRAM0 + 0x4010));
    leave();
}

/* 0x01A210: entering test mode */
void tm_state_title_init(void)
{
    extern void sync_reset(void), set_background_color(int, int, int), sound_stop_all(void), tilemap_enable_set(void),
                tilemap_scroll_set(int), cz_ram_init(void);
    sync_reset();
    set_background_color(0, 0, 0);
    SET_PAGE(PAGE() & 0xFFFE);
    W[0x0CBC] = 7;
    if (tm_r32(WRAM0 + 0x3FB0) + 600u < tm_r32(WRAM0 + 0x4010)) SET_PAGE(0);   /* more than 10 s since it was left: the menu */
    if (IN_SW() & 4) SET_PAGE(0x20);
    else if (PAGE() == 0x20) SET_PAGE(0);
    W16_SET(0x16918, 0xFFFF);
    W[0xEB16] = 0;                                     /* the fade level, read whole-slot by the game */
    W[0x2C24] = 0;
    W[0x0CAC] = 0;
    sound_stop_all();
    tilemap_enable_set();
    tilemap_scroll_set(0);
    W_SET_HI16(0xEB20, 0);
    cz_ram_init();
}
