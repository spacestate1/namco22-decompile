/*
 * test_mode_misc.c -- Prop Cycle's operator test mode, pages 18..33 of the table at ROM 0x36548 (GitHub #26, 2026-10-05):
 * SOUND TEST (0x12/0x13), ADS DATA (0x14/0x15), OTHERS (0x16/0x17) and its sub-pages DSP PCB TEST (0x18/0x19), ADS INITIALIZE
 * (0x1A/0x1B), BACK UP MEMORY INITIALIZE (0x1C/0x1D), POINT ROM CHECK (0x1E/0x1F) and the handlebar ADJUST MODE (0x20/0x21).
 * Ported instruction by instruction from 0x01B3D6..0x01C0AE; see test_mode.c for the page machine and the memory convention.
 * Ground truth: work/gh26/mame_test/ (MAME's own test mode).
 *
 * Fields SHARED with the game keep the game's own convention (test_mode.c does the same for the coin/game options):
 *   - the sound settings 0xE03FE0/E2/E4/E6 and the song request 0xE03FE8 are read and written as WHOLE _W[] slots, as
 *     sound_env_apply (0x00F94A, game_dsp3d.c) and settings_audio_defaults (game_eeprom.c) do;
 *   - the handlebar ADC words 0xE02BC8/CA and their centres 0xE03FD0/D2 are whole, pinned slots (input.c).
 * Everything else (the test mode's scratch at 0xE168xx/0xE169xx, the record copy at 0xE0AB04, the ADS audit words) is the ROM's
 * own layout through the tm_r* / tm_w* accessors.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "propcycl.h"
#include "vaddr.h"
#include "test_mode.h"
#include "eng.h"

extern intptr_t _W[];
#define WRAM0 0xE00000u

extern void sync_post(void);                            /* 0x02106A */
extern void sound_stop_all(void);                       /* 0x00F810 */
extern int  sound_env_apply(void);                      /* 0x00F94A */
extern void eeprom_write_block(intptr_t addr, int n);   /* 0x019F1A */
extern void eeprom_sync_all(void);                      /* 0x019EEE */
extern void entry_reset(void);                          /* 0x00BC4C */
extern void game_stats_reset(void);                     /* 0x020C9C */
extern void eeprom_factory_reset(void);                 /* 0x01A0FE */
extern void eeprom_init_with_calibration(void);         /* 0x01A0BE */
extern int  scene_get_max_priority(void);               /* 0x00F36E */
extern void text_print_string(int col, int row, char *s, int pal);   /* 0x02108C */

/* a game-shared word kept as a whole _W[] slot (see the header) */
static int32_t slot_r(uint32_t a) { return (int16_t)_W[a - WRAM0]; }
static void    slot_w(uint32_t a, int32_t v) { _W[a - WRAM0] = (intptr_t)(int32_t)(int16_t)v; }

/* 0x01A15E (title_menu_select): mark the EEPROM, sync it, restart from the top */
static void leave(void)
{
    tm_w16(WRAM0 + 0x3FEA, 1);
    eeprom_sync_all();
    entry_reset();
}

/* a label (12-byte record) table like tm_labels (0x01A55A), but a string pointer in WORK RAM is read from work RAM: the SOUND
 * TEST prints the MCU's message line from 0xE0AC44 (and, with the DIP set, the record copy at 0xE0AB04) that way. */
static void labels_ram(uint32_t t, int count)
{
    const int sel = tm_cursor();
    int n = 0;
    do {
        if (++n > count) return;
        int pal = tm_r16(t + 8);
        if (tm_r16(t + 0xA) == sel && (tm_frame() & 8) && count > 0x7F) pal = 2;
        const uint32_t s = tm_r32(t);
        if (s >= WRAM0 && s < WRAM0 + WORK_RAM_SIZE) {
            char buf[129]; int k = 0;
            while (k < 128 && (buf[k] = (char)tm_r8(s + (uint32_t)k)) != 0) k++;
            buf[k] = 0;
            text_print_string(tm_r16(t + 4), tm_r16(t + 6), buf, pal);
        } else tm_print(tm_r16(t + 4), tm_r16(t + 6), s, pal);
        t += 0xC;
    } while (tm_r32(t) != 0);
}

/* ===================================================================================================================== SOUND TEST */
/* 0x01B3D6: slot 0x12. Mode (0xE16928) = DIP byte 0xE02C0C bit 4: off = volumes + a 3-digit decimal song number (records 0x36A98),
 * on = hex digits and a register poke (0x36B88). */
void tm_p18(void)
{
    const uint32_t work = WRAM0 + 0xAB04;
    sync_post();
    tm_w8(work + 0x140, 0);                                        /* the message line 0xE0AC44 */
    tm_copy_to_work(0x36714);
    tm_clear_highlights(0x36714);
    if (tm_in_svc() & 2) { tm_w16(work + 0x12, 4); tm_w16(work + 0x42, 2); }
    const int mode = (tm_r8(WRAM0 + 0x2C0C) & 0xFF) & 0x10;
    tm_w16(WRAM0 + 0x16928, mode);
    if (mode) {
        tm_copy_to_work(0x36B88);
        tm_clear_highlights(0x36B88);
        const int32_t req = slot_r(WRAM0 + 0x3FE8);
        tm_w16(WRAM0 + 0x16938, req & 0xF);
        tm_w16(WRAM0 + 0x1693A, (req >> 4) & 0xF);
        tm_w16(WRAM0 + 0x1693C, (req >> 8) & 0xF);
    } else {
        tm_copy_to_work(0x36A98);
        tm_clear_highlights(0x36A98);
        tm_w16(WRAM0 + 0x16938, 0); tm_w16(WRAM0 + 0x1693A, 0); tm_w16(WRAM0 + 0x1693C, 0);
    }
    if (tm_in_svc() & 2) { tm_w16(work + 0x12, 0x2B); tm_w16(work + 0x2A, 0x2B); }
    tm_w16(WRAM0 + 0x16934, slot_r(WRAM0 + 0x3FE0));               /* right speaker volume */
    tm_w16(WRAM0 + 0x16936, slot_r(WRAM0 + 0x3FE2));               /* left */
    tm_w16(WRAM0 + 0x1692C, 0);
    tm_w16(WRAM0 + 0x1692A, 0x89);
    tm_w32(WRAM0 + 0x16924, (uint32_t)scene_get_max_priority());   /* the highest song number */
    tm_set_page(0x13);
}

/* 0x01B4F8: slot 0x13 */
void tm_p19(void)
{
    const uint32_t work = WRAM0 + 0xAB04, a4E = WRAM0 + 0x1693A, a5E = WRAM0 + 0x1693C;
    const int mode = tm_r16(WRAM0 + 0x16928);
    labels_ram(mode ? 0x36C48 : 0x36B28, 0x100);
    tm_draw_items(work, 0x100);
    tm_legend(4);
    {   /* the MCU's message: 16-bit words from shared RAM 0xA04300, low byte first, into 0xE0AC44, blank-padded to 0x74 */
        uint32_t dst = WRAM0 + 0xAC44, src = 0xA04300;
        int n = 0;
        for (;;) {
            const unsigned w = (uint16_t)tm_r16(src); src += 2;
            tm_w8(dst++, (int)(w & 0xFF));
            if (!(w & 0xFF)) break;
            tm_w8(dst++, (int)(w >> 8));
            if (!(w >> 8)) break;
            if (++n >= 0x74) break;
        }
        dst--;
        while (n < 0x74) { tm_w8(dst++, 0x20); n++; }
        tm_w8(dst, 0);
    }
    const int r = (int16_t)tm_value_adjust(work);
    int sel = (int16_t)(int8_t)tm_r8(WRAM0 + 0x3FBF);
    if (tm_r16(WRAM0 + 0x16934) != slot_r(WRAM0 + 0x3FE0)) {
        const int32_t v = tm_r16(WRAM0 + 0x16934);
        slot_w(WRAM0 + 0x3FE0, v); slot_w(WRAM0 + 0x3FE4, v);
        sound_env_apply();
    }
    if (tm_r16(WRAM0 + 0x16936) != slot_r(WRAM0 + 0x3FE2)) {
        const int32_t v = tm_r16(WRAM0 + 0x16936);
        slot_w(WRAM0 + 0x3FE2, v); slot_w(WRAM0 + 0x3FE6, v);
        sound_env_apply();
    }
    if (r >= 0x10) {                                               /* a digit wrapped upward: carry */
        if (sel == 4) {
            tm_w16(a4E, tm_r16(a4E) + 1);
            if (tm_r16(a4E) >= (mode ? 0x10 : 10)) { tm_w16(a4E, 0); sel = 3; }
        }
        if (sel == 3) {
            tm_w16(a5E, tm_r16(a5E) + 1);
            if (tm_r16(a5E) >= (mode ? 0x10 : 10)) tm_w16(a5E, 0);
        }
    } else if (r <= -0x10) {                                       /* wrapped downward: borrow */
        if (sel == 4) {
            tm_w16(a4E, tm_r16(a4E) - 1);
            if (tm_r16(a4E) < 0) { tm_w16(a4E, mode ? 15 : 9); sel = 3; }
        }
        if (sel == 3) {
            tm_w16(a5E, tm_r16(a5E) - 1);
            if (tm_r16(a5E) < 0) tm_w16(a5E, mode ? 15 : 9);
        }
    }
    {   const int32_t hi = tm_r16(a5E), mid = tm_r16(a4E);
        const int32_t v = (mode ? (hi << 8) + (mid << 4) : hi * 100 + mid * 10) + tm_r16(WRAM0 + 0x16938);
        slot_w(WRAM0 + 0x3FE8, v); }                               /* the song number */
    if (r != 0) {
        const unsigned s = (uint16_t)sel;
        if (s >= 2 && s <= 4) { tm_w16(WRAM0 + 0x168F0, 0x2000); tm_w16(WRAM0 + 0x168F2, 0x2000); tm_w16(WRAM0 + 0x168F4, 0x2000); }
        else tm_w16(WRAM0 + 0x168EC + s * 2, 0x2000);
    }
    if (tm_page_select(mode ? 8 : 6)) eeprom_write_block(0xE03FE0, 10);
    tm_w16(WRAM0 + 0x16932, tm_r16(WRAM0 + 0x16932) - 1);          /* the song's play timer */
    if (tm_r16(WRAM0 + 0x16932) <= 0 || !((uint16_t)tm_r16(0xA04000) & 0x8000)) {
        sound_stop_all();
        tm_w16(WRAM0 + 0x16932, 0);
    }
    if (tm_confirm()) {
        if ((int8_t)tm_r8(WRAM0 + 0x3FBF) == (mode ? 7 : 5)) {    /* EXIT */
            sound_stop_all();
            tm_set_page(0);
            tm_w8(WRAM0 + 0x3FBF, 0);
            eeprom_sync_all();
        } else if (tm_r16(WRAM0 + 0x16932) > 0) {                  /* playing: STOP */
            tm_w16(WRAM0 + 0x16932, 0);
            sound_stop_all();
        } else {                                                   /* REQUEST */
            tm_w16(WRAM0 + 0x16932, 600);
            if (mode) tm_w16(0xA04100 + (uint32_t)tm_r16(WRAM0 + 0x1692C) * 2, tm_r16(WRAM0 + 0x1692A));
            if ((int32_t)tm_r32(WRAM0 + 0x16924) >= slot_r(WRAM0 + 0x3FE8))
                tm_w16(0xA04000, slot_r(WRAM0 + 0x3FE8) | 0x4000); /* the song, into the MCU's command slot 0 */
            tm_w32(WRAM0 + 0x16920, 0);
        }
    }
    tm_w32(WRAM0 + 0x16920, tm_r32(WRAM0 + 0x16920) + 1);
}

/* ======================================================================================================================= ADS DATA */
static uint32_t avg(uint32_t sum, unsigned n) { return n ? sum / n : 0; }           /* divu.l, 0 when the count is 0 */
/* 0x01B814: slot 0x14 -- the three averages of page 1/4 */
void tm_p20(void)
{
    sync_post();
    tm_legend(6);
    tm_clear_highlights(0x36CC0);
    tm_w32(WRAM0 + 0x1691C, avg(tm_r32(WRAM0 + 0x4014), (uint16_t)tm_r16(WRAM0 + 0x4024)));
    tm_w32(WRAM0 + 0x16924, avg(tm_r32(WRAM0 + 0x4018), (uint16_t)tm_r16(WRAM0 + 0x4020)));
    tm_w32(WRAM0 + 0x16920, avg(tm_r32(WRAM0 + 0x401C), (uint16_t)tm_r16(WRAM0 + 0x4022)));
    tm_set_page(0x15);
}
/* 0x01B89A: slot 0x15 -- four views (UP/DOWN), START leaves, handlebars RIGHT + START goes to the ADS reset */
void tm_p21(void)
{
    const uint32_t a3 = WRAM0 + 0x3F30;
    if (tm_page_select(4)) { sync_post(); tm_legend(6); }
    const int v = (int16_t)(int8_t)tm_r8(WRAM0 + 0x3FC0);
    tm_labels(tm_r32(0x36FD8 + (uint32_t)v * 4), 0x100);
    if (v == 1) {                                                  /* 2/4 NOVICE PLAYERS' SKILL: 3 x 6 */
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 6; j++)
                tm_draw_decimal(i * 12 + 8, j * 2 + 9, 4, (uint16_t)tm_r16(a3 + 0x1FA + (uint32_t)(i * 16 - j * 2)), 0);
    } else if (v == 2) {                                           /* 3/4 STAGE SELECT: 3 x 5, two numbers from the second */
        for (int j = 0; j < 3; j++)
            for (int i = 0; i < 5; i++) {
                const int col = i > 2 ? i * 6 + 9 : 4 + i * 8;
                tm_draw_decimal(col, j * 5 + 10, 4, (uint16_t)tm_r16(a3 + 0x1B2 + (uint32_t)(j * 20 + i * 2)), 0);
                if (i) tm_draw_decimal(col, j * 5 + 12, 4, (uint16_t)tm_r16(a3 + 0x1BC + (uint32_t)(j * 20 + i * 2)), 0);
            }
    } else if (v == 3) {                                           /* 4/4 ADVANCED PLAYERS' SKILL: 5 x 5 and the row sums */
        for (int j = 0; j < 5; j++) {
            int16_t sum = 0;
            for (int i = 0; i < 5; i++) {
                const int32_t w = (uint16_t)tm_r16(a3 + 0x180 + (uint32_t)(j * 10 + i * 2));
                sum = (int16_t)(sum + w);
                tm_draw_decimal(i * 5 + 8, j * 3 + 10, 4, w, 0);
            }
            tm_draw_decimal(0x22, j * 3 + 10, 4, sum, 0);
        }
    } else tm_draw_items(tm_r32(0x36FE8 + (uint32_t)v * 4), 0x100); /* 1/4 TIME */
    const unsigned b38 = (uint16_t)tm_r16(WRAM0 + 0x2B38), b3a = (uint16_t)tm_r16(WRAM0 + 0x2B3A);
    if (((tm_in_edge() & 1) && (tm_in_svc() & 1)) || ((tm_in_start() & 1) && (tm_in_dir() & 1)) ||
        ((b3a & 0x10) && (b38 & 0x100)) || ((b3a & 0x100) && (b38 & 0x10)) ||
        ((b3a & 0x10) && (b38 & 0x800)) || ((b3a & 0x800) && (b38 & 0x10)))
        tm_set_page(0x1A);
    else if (tm_confirm()) tm_set_page(0);
}

/* ========================================================================================================================= OTHERS */
/* 0x01BB3A (title_attract_init): VIDEO PCB TEST -- the sprite chip's registers to a plain state, then the board test at 0x03DD3C,
 * then back as if test mode were left. 0x03DD3C (boot_hardware_init) is NOT ported as the interactive screen test it is on the
 * machine (MAME: 'SCREEN#00 DISPLAY GAGE'); this engine's boot_hardware_init is only its set-up half, run once at boot, and
 * re-running it here would clear the mixer's fog colour and the CZ table (see game_init). So it is skipped, and said so. */
static void video_pcb_test(void)
{
    static const uint16_t reg[14] = { 0, 0, 0, 0, 0x2FF, 0, 0, 0x7FF, 0x20, 0x20, 0, 0x2FF, 0, 0x7FF };
    for (int i = 0; i < 14; i++) tm_w16(0x980000 + (uint32_t)i * 2, reg[i]);
    for (uint32_t a = 0x980200; a < 0x980240; a += 4) { tm_w16(a, 0); tm_w16(a + 2, 0x7FF); }
    tm_w16(WRAM0 + 0x16918, 0x17);
    { static int said; if (!said++) fprintf(stderr, "[TEST] VIDEO PCB TEST: the board test at 0x03DD3C is not ported -- skipped\n"); }
    tm_w16(WRAM0 + 0x16918, 0xFFFF);
    tm_w32(WRAM0 + 0x3FB0, tm_r32(WRAM0 + 0x4010));
    leave();
}
/* 0x01BB2A: slot 0x16 */
void tm_p22(void) { sync_post(); tm_set_page(0x17); }
/* 0x01BBE8: slot 0x17 -- the OTHERS list; START: 0 BACK UP MEMORY INITIALIZE, 1 VIDEO PCB TEST, 2 DSP PCB TEST, 3 POINT ROM CHECK,
 * 4 EXIT (the jump table at 0x01BC32) */
void tm_p23(void)
{
    tm_labels(0x36FF8, 0x100);
    tm_legend(0);
    tm_page_select(5);
    if (!tm_confirm()) return;
    switch ((int8_t)tm_r8(WRAM0 + 0x3FC1)) {
    case 0: tm_set_page(0x1C); break;
    case 1: video_pcb_test(); break;
    case 2: tm_set_page(0x18); break;
    case 3: tm_set_page(0x1E); break;
    case 4: tm_w8(WRAM0 + 0x3FC1, 0); tm_set_page(0); break;
    }
}

/* =================================================================================================================== DSP PCB TEST */
/* 0x03B114: run board test d0 with results at a0 = 0xE0AB04 (one word per test, 0 = OK; test 12 = the point ROM sums at a0+0xD2).
 * Tests 0..11 upload the master/slave DSPs' own test programs and run them (0x03B228.. 0x03B5F2) -- this engine runs the master's
 * GAME program only (and that only with PROPCYCL_MASTER=1), so those are not ported and report NG (1), said once in the log.
 * Test 12 (0x03B164) has the master DSP stream the point ROM through DSP RAM 0xC00C00 while the 68K adds the three bytes of each
 * 24-bit word into three 16-bit sums per 512K-word plane: (L, M, U) of plane 0..3 -> words 0..11. Here the sums are taken from
 * the point ROM directly -- the same bytes; they equal the ROM's expected table at 0x372BC (= each chip's byte sum). */
static void dsp_pcb_test(int idx)
{
    const uint32_t res = WRAM0 + 0xAB04;
    if (idx == 12) {
        uint16_t s[12] = { 0 };
        for (int p = 0; p < 4; p++)
            for (uint32_t i = 0; i < 0x80000; i++) {
                const uint32_t w = (uint32_t)eng_point_read((uint32_t)p * 0x80000 + i) & 0xFFFFFF;
                s[p * 3 + 0] = (uint16_t)(s[p * 3 + 0] + (w & 0xFF));
                s[p * 3 + 1] = (uint16_t)(s[p * 3 + 1] + ((w >> 8) & 0xFF));
                s[p * 3 + 2] = (uint16_t)(s[p * 3 + 2] + ((w >> 16) & 0xFF));
            }
        for (int k = 0; k < 12; k++) tm_w16(res + 0xD2 + (uint32_t)k * 2, s[k]);
        return;
    }
    { static int said; if (!said++) fprintf(stderr, "[TEST] DSP PCB TEST: tests 0-11 run the DSPs' own test programs, which this engine does not -- reported NG\n"); }
    tm_w16(res + (uint32_t)idx * 2, 1);
}
/* 0x01BCAA (title_rom_checksum_test): the test numbered by 0xE16914; 1 = NG */
static int run_test(void)
{
    tm_w16(WRAM0 + 0x16918, 0x19);
    const int idx = (int32_t)tm_r32(WRAM0 + 0x16914);
    dsp_pcb_test(idx);
    tm_w16(WRAM0 + 0x16918, 0xFFFF);
    return tm_r16(WRAM0 + 0xAB04 + (uint32_t)idx * 2) != 0;
}
/* 0x01BC66: slot 0x18 */
void tm_p24(void)
{
    sync_post();
    tm_legend(5);
    for (int i = 0; i < 12; i++) tm_w16(WRAM0 + 0x168FC + (uint32_t)i * 2, 0xFFFF);
    tm_w32(WRAM0 + 0xE1C, 1);
    tm_w16(WRAM0 + 0x1691A, 12);
    tm_w32(WRAM0 + 0x16914, 0);
    tm_set_page(0x19);
}
/* 0x01BCE8: slot 0x19 -- one test a frame, OK / NG beside each line; START leaves (as test mode does) */
void tm_p25(void)
{
    tm_labels(0x37070, 0x100);
    for (int i = 0; i < 12; i++) {
        const int r = tm_r16(WRAM0 + 0x168FC + (uint32_t)i * 2);
        if (r == 0) tm_print(0x1D, i + 7, 0x3A6AD, 0);
        else if (r == 1) tm_print(0x1D, i + 7, 0x3A6DF, 2);
    }
    if (tm_confirm()) {
        tm_set_page(0x16);
        tm_w8(WRAM0 + 0x3FC2, 0);
        tm_w32(WRAM0 + 0x3FB0, tm_r32(WRAM0 + 0x4010));
        leave();
    }
    if (tm_r16(WRAM0 + 0x1691A) != 0) {
        const int32_t i = (int32_t)tm_r32(WRAM0 + 0x16914);
        tm_print(0x1D, i + 7, 0x36663, 0);                         /* WORKING */
        const int r = run_test();
        tm_w16(WRAM0 + 0x168FC + (uint32_t)i * 2, r);
        tm_print(0x1D, i + 7, 0x3667A, 0);
        tm_w16(WRAM0 + 0x1691A, tm_r16(WRAM0 + 0x1691A) - 1);
        if (tm_r16(WRAM0 + 0x1691A) == 0) tm_w32(WRAM0 + 0x16914, 0xFFFFFFFFu);
        else tm_w32(WRAM0 + 0x16914, (uint32_t)(i + 1));
    }
}

/* ================================================================================================================ ADS INITIALIZE */
/* 0x01BDCE: slot 0x1A (cursor on NO) */
void tm_p26(void) { sync_post(); tm_legend(0); tm_w8(WRAM0 + 0x3FC3, 1); tm_set_page(0x1B); }
/* 0x01BDEE: slot 0x1B -- YES clears the audit statistics; any choice goes back to ADS DATA */
void tm_p27(void)
{
    tm_labels(0x37130, 0x100);
    tm_page_select(3);
    if (!tm_confirm()) return;
    if (tm_r8(WRAM0 + 0x3FC3) == 0) {
        game_stats_reset();
        eeprom_sync_all();
        tm_w8(WRAM0 + 0x3FC0, 0);
    }
    tm_set_page(0x14);
}

/* ===================================================================================================== BACK UP MEMORY INITIALIZE */
/* 0x01BE36: slot 0x1C (cursor on NO) */
void tm_p28(void) { sync_post(); tm_legend(0); tm_w8(WRAM0 + 0x3FC4, 1); tm_set_page(0x1D); }
/* 0x01BE56: slot 0x1D -- YES: the factory reset (the MENU's cursor kept across it); back to OTHERS */
void tm_p29(void)
{
    tm_labels(0x37178, 0x100);
    tm_page_select(3);
    if (!tm_confirm()) return;
    if (tm_r8(WRAM0 + 0x3FC4) == 0) {
        const int keep = tm_r8(WRAM0 + 0x3FB6);
        eeprom_factory_reset();
        tm_w8(WRAM0 + 0x3FB6, keep);
        tm_w8(WRAM0 + 0x3FC1, 0);
    }
    tm_set_page(0x16);
}

/* =============================================================================================================== POINT ROM CHECK */
/* 0x01BEA4: slot 0x1E -- the expected sums (ROM 0x372BC), then the check itself (WORKING while it runs) */
void tm_p30(void)
{
    sync_post();
    tm_labels(0x371CC, 0x100);
    for (int i = 0; i < 9; i++)
        tm_hex(0x11, tm_r16(0x371D2 + (uint32_t)i * 12), 4, (uint32_t)tm_r16(0x372BC + (uint32_t)i * 2), 0);
    tm_print(0x18, 6, 0x36663, 0);
    tm_w32(WRAM0 + 0x16914, 12);
    run_test();
    tm_print(0x18, 6, 0x3667A, 0);
    tm_legend(5);
    tm_set_page(0x1F);
}
/* 0x01BF4A: slot 0x1F -- each sum against its expected value: OK (palette 4) or NG! (palette 2); START leaves */
void tm_p31(void)
{
    for (int i = 0; i < 9; i++) {
        const int32_t got = tm_r16(WRAM0 + 0xABD6 + (uint32_t)i * 2);
        const int row = tm_r16(0x371CC + 6 + (uint32_t)i * 12);
        const int ok = tm_r16(0x372BC + (uint32_t)i * 2) == got;
        tm_hex(0x18, row, 4, (uint32_t)got, ok ? 4 : 2);
        tm_print(0x1E, row, ok ? 0x3A6AD : 0x3AB28, ok ? 4 : 2);
    }
    if (tm_confirm()) {
        tm_set_page(0x16);
        tm_w32(WRAM0 + 0x3FB0, tm_r32(WRAM0 + 0x4010));
        leave();
    }
}

/* ================================================================================================================== ADJUST MODE */
/* 0x01C030: slot 0x20 (entered from state_title_init when the service switch is held) */
void tm_p32(void)
{
    sync_post();
    eeprom_init_with_calibration();
    tm_clear_highlights(0x372D0);
    tm_w8(WRAM0 + 0x3FC6, 0);
    tm_set_page(0x21);
}
/* 0x01C058: slot 0x21 -- the handlebar's two ADC words, their centres and the difference; the service switch re-centres */
void tm_p33(void)
{
    tm_w16(WRAM0 + 0x16928, slot_r(WRAM0 + 0x2BCA) - slot_r(WRAM0 + 0x3FD2));
    tm_w16(WRAM0 + 0x1692A, slot_r(WRAM0 + 0x2BC8) - slot_r(WRAM0 + 0x3FD0));
    tm_labels(0x37378, 0x100);
    tm_draw_items(0x372D0, 0x100);
    if (tm_in_swedge() & 4) eeprom_init_with_calibration();
}
