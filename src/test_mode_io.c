/*
 * test_mode_io.c -- Prop Cycle's operator test mode, pages 7..17 of the table at 0x36548 (GitHub #26, 2026-10-05): the I/O TEST
 * menu and its four sub-pages (SWITCH, PEDAL, MOTOR, LAMP) and the MONITOR TEST with its ten patterns. Ported from the 68K at
 * 0x01AC72..0x01B3D6 and 0x01C0B0..0x01C992 (the pattern drawers and the character set they load), instruction by instruction.
 * Memory goes through include/test_mode.h's tm_r* / tm_w* (68K addresses). Three kinds of value are read the way their
 * PRODUCER on this side stores them, not through the ROM's layout, and each has a static helper below:
 *   - the handlebar ADCs 0xE02BC8 / 0xE02BCA and their centres 0xE03FD0 / 0xE03FD2: whole _W[] slots (src/input.c writes and
 *     pins them; read_mcu_inputs_process compares them whole);
 *   - the DIP byte 0xE02C0C: a whole slot (dipswitch_init, game_eeprom.c / game_hud.c read `W[0x2C0C] & n`);
 *   - the MCU's output word 0xA0BD2E (motor bit 1, lamp bit 2): comms_r16/comms_w16, as tilemap_scroll_set does.
 * MAME ground truth: work/gh26/mame_test/ (run3, run5).
 */
#include <stdint.h>
#include "propcycl.h"
#include "vaddr.h"
#include "test_mode.h"

extern intptr_t _W[];
#define WRAM0 0xE00000u

extern void sync_post(void);                    /* 0x02106A */
extern void tilemap_enable_set(void);           /* 0x0225F8 */
extern void tilemap_scroll_set(int on);         /* 0x022670: the lamp output, 0xA0BD2E bit 2 */

static int16_t slot16(uint32_t off) { return (int16_t)_W[off]; }      /* a whole-slot 16-bit value (see the header) */
static uint8_t dip_byte(void) { return (uint8_t)_W[0x2C0C]; }

/* 0x01AC72: the DIP switches 1..8 at row 6, column 0x11 -- the digit, red when the switch is on */
static void dip_update(void)
{
    unsigned bit = 1;
    for (int d = 1; d <= 8; d++, bit <<= 1)
        tm_tile(0x11 + d - 1, 6, (unsigned)d | ((dip_byte() & bit) ? 0x2000u : 0u));
}

/* 0x01AD24: a value in hex (4 digits) and after it "OK" (0x3667F), or "NG" in palette 4 (0x3A6AD) outside min..max */
static void draw_value_with_range(int col, int row, int32_t v, int32_t lo, int32_t hi)
{
    tm_hex(col, row, 4, (uint32_t)v, 0);
    if (v < lo || v > hi) tm_print(col + 5, row, 0x3A6AD, 4);
    else tm_print(col + 5, row, 0x3667F, 0);
}
/* 0x01AD84: "ON" (0x3A50C, palette 2) or "OFF" (0x3A508) */
static void draw_onoff(int col, int row, int on)
{
    if (on) tm_print(col, row, 0x3A50C, 2);
    else tm_print(col, row, 0x3A508, 0);
}

/* the exit test the sub-pages share: handlebars UP with Start (either order of edge and hold), or the decoder's pair */
static int sub_exit(int b3x_pair)
{
    if ((tm_in_edge() & 4) && (tm_in_svc() & 1)) return 1;
    if ((tm_in_start() & 1) && (tm_in_dir() & 4)) return 1;
    return b3x_pair;
}

/* ---- slot 7, 0x01ACAE: the I/O TEST menu -- SWITCH, PEDAL, MOTOR, LAMP, EXIT. Start on entry n < 4 goes to page 8 + 2n. */
void tm_p07(void)
{
    tm_page_select(5);
    tm_labels(0x368CC, 0x100);
    dip_update();
    if (tm_confirm()) {
        const int c = tm_cursor();
        if (c < 4) tm_set_page(c * 2 + 8);
        else { tm_set_page(0); tm_w8(WRAM0 + 0x3FB9, 0); }
    }
}

/* ---- slot 8, 0x01AD02: SWITCH init -- legend 2, the cursor parked at -2 (nothing on this page is selectable) */
void tm_p08(void) { sync_post(); tm_legend(2); tm_set_page(9); tm_w8(WRAM0 + 0x3FBA, 0xFE); }

/* ---- slot 9, 0x01ADB8: SWITCH -- the two handlebar axes off their centres (OK within +-0x110), Start, Service, Test */
void tm_p09(void)
{
    tm_w16(WRAM0 + 0x16928, (int16_t)(slot16(0x2BCA) - slot16(0x3FD2)));
    tm_w16(WRAM0 + 0x1692A, (int16_t)(slot16(0x2BC8) - slot16(0x3FD0)));
    dip_update();
    draw_value_with_range(0x19, 0xC, tm_r16(WRAM0 + 0x16928), -0x110, 0x110);
    draw_value_with_range(0x19, 0xE, tm_r16(WRAM0 + 0x1692A), -0x110, 0x110);
    draw_onoff(0x19, 0x10, tm_in_svc() & 1);      /* START */
    draw_onoff(0x19, 0x12, tm_in_sw() & 1);       /* COIN */
    draw_onoff(0x19, 0x14, tm_in_sw() & 4);       /* SERVICE */
    tm_labels(0x368CC, 3);
    tm_labels(0x3692C, 0x100);
    if (sub_exit((tm_r16(WRAM0 + 0x2B3C) & 0x100) != 0)) tm_set_page(6);
}

/* ---- slot 10, 0x01AECE: PEDAL init */
void tm_p10(void)
{
    sync_post(); tm_legend(2);
    tm_w16(WRAM0 + 0x1692A, 0); tm_w16(WRAM0 + 0x16928, 0);
    tm_set_page(0xB);
}

/* ---- slot 11, 0x01AEF6: PEDAL -- the pedal counter (0xE02C0A, the MCU's pulse counter as this side's
 * input_process_analog_deltas keeps it) and a three-step check: Start to begin (state 1 stores the count), pedal, Start again
 * (state 2: the pulses counted, OK at 0x8AF or more). State word 0xE16928, result 0xE1692A, start count 0xE16924 (long). */
void tm_p11(void)
{
    const uint32_t st = WRAM0 + 0x16928, res = WRAM0 + 0x1692A, cnt = WRAM0 + 0x16924, ctr = WRAM0 + 0x2C0A;
    dip_update();
    tm_labels(0x368CC, 4);
    tm_labels(0x36974, 0x100);
    if (!(tm_frame() & 0x10)) {
        switch (tm_r16(st)) {
        case 0: tm_print(0xD, 0x12, 0x3A6B0, 0); break;
        case 1: tm_print(0xD, 0x12, 0x3A6BC, 0); tm_print(0xE, 0x14, 0x3A6CE, 0); break;
        case 2:
            if (tm_r16(res) > 0) tm_print(0xF, 0x12, 0x3A6AD, 4);
            else tm_print(0xF, 0x12, 0x3A6DF, 2);
            tm_print(0xD, 0x14, 0x3A6E2, 0);
            break;
        default: break;
        }
    } else {
        if (tm_r16(st) != 2) tm_print(0xD, 0x12, 0x36670, 0);
        tm_print(0xD, 0x14, 0x3666C, 0);
    }
    uint32_t s;
    if (tm_frame() & 8) s = 0x36674;
    else s = tm_r16(st) == 0 ? 0x3A70A : tm_r16(st) == 1 ? 0x3A6F8 : 0x3A706;
    tm_print(0xF, 0x10, s, 0);
    tm_hex(0x19, 0xE, 4, (uint16_t)tm_r16(ctr), 0);
    if (tm_confirm()) {
        tm_print(0xF, 0x10, 0x36674, 0);
        tm_print(0xD, 0x12, 0x36670, 0);
        tm_print(0xD, 0x14, 0x3666C, 0);
        const int n = tm_r16(st) + 1;
        tm_w16(st, n);
        if (n == 1) {
            tm_w16(res, 0);
            tm_w32(cnt, (uint16_t)tm_r16(ctr));
        } else if (n == 2) {
            const uint32_t d = (uint32_t)(uint16_t)tm_r16(ctr) - tm_r32(cnt);
            tm_w32(cnt, (uint16_t)d);
            tm_w16(res, (int32_t)tm_r32(cnt) >= 0x8AF ? 1 : -1);
        } else tm_w16(st, 0);
    }
    const int pair = (tm_r16(WRAM0 + 0x2B38) & 0x80) && (tm_r16(WRAM0 + 0x2B38) & 0x100);
    if (sub_exit(pair)) { tm_set_page(6); tm_w8(WRAM0 + 0x3FBB, 0); }
}

/* ---- slot 12, 0x01B150: MOTOR init */
void tm_p12(void) { sync_post(); tm_legend(0); tm_w16(WRAM0 + 0x16928, 0); tm_set_page(0xD); }

/* the ON/OFF line of the MOTOR and LAMP pages: the state at `flag`, the cursor `c` (0 = the switch, 1 = EXIT) */
static void onoff_line(int col, int row, int on, int c)
{
    int pal;
    if (on) { pal = (c != 0 || (tm_frame() & 8)) ? 2 : 0; tm_print(col, row, 0x3A50C, pal); }
    else    { pal = (c == 0 && (tm_frame() & 8)) ? 2 : 0; tm_print(col, row, 0x3A508, pal); }
}

/* ---- slot 13, 0x01B16E: MOTOR -- Start toggles the handlebar motor (MCU output 0xA0BD2E bit 1); EXIT returns */
void tm_p13(void)
{
    tm_page_select(2);
    dip_update();
    onoff_line(0x17, 0x10, tm_r16(WRAM0 + 0x16928) != 0, tm_cursor());
    tm_labels(0x368CC, 5);
    tm_labels(0x3698C, 0x100);
    if (tm_confirm()) {
        if (tm_cursor() == 0) {
            const int v = (uint16_t)tm_r16(WRAM0 + 0x16928) ^ 1;
            tm_w16(WRAM0 + 0x16928, v);
            const unsigned o = comms_r16(g_sys.commsram, 0x7D2E);
            comms_w16(g_sys.commsram, 0x7D2E, v ? (o | 2) : (o & 0xFFFD));
        } else {
            tm_set_page(6); tm_w8(WRAM0 + 0x3FBC, 0);
            tilemap_enable_set();
        }
    }
}

/* ---- slot 14, 0x01B23E: LAMP init */
void tm_p14(void) { sync_post(); tm_legend(0); tm_w16(WRAM0 + 0x1692E, 0); tm_set_page(0xF); }

/* ---- slot 15, 0x01B25C: LAMP -- Start toggles the lamp (0x022670 with 1/0); EXIT turns it off and returns */
void tm_p15(void)
{
    tm_page_select(2);
    dip_update();
    onoff_line(0x15, 0x12, tm_r16(WRAM0 + 0x1692E) != 0, tm_cursor());
    tm_labels(0x368CC, 6);
    tm_labels(0x369B0, 0x100);
    if (tm_confirm()) {
        if (tm_cursor() == 0) {
            const int v = (uint16_t)tm_r16(WRAM0 + 0x1692E) ^ 1;
            tm_w16(WRAM0 + 0x1692E, v);
            tilemap_scroll_set(v ? 1 : 0);
        } else {
            tm_set_page(6); tm_w8(WRAM0 + 0x3FBD, 0);
            tilemap_scroll_set(0);
        }
    }
}

/* ---- the MONITOR TEST's graphics: 0x01C0B0 (palette base 0x7E, the pattern palette and its character set) ---------------- */
static void gfx_init(void)
{
    tm_w8(0x82401B, 0x7E);                                             /* 0x01C874 */
    for (int i = 0; i < 0x100; i++) {                                  /* 0x01C87E: planar R/G/B of palette 0x7E */
        tm_w8(0x83FE00 + i, (uint8_t)vrd8(0x20994 + 0x200 + i));
        tm_w8(0x837E00 + i, (uint8_t)vrd8(0x20994 + 0x100 + i));
        tm_w8(0x82FE00 + i, (uint8_t)vrd8(0x20994 + i));
        palette_mark_written(0x7E00 + i);                              /* the renderer takes these pens from the game (src/pc_palette.c) */
    }
    tm_w16(0x8A0000, 0x035C); tm_w16(0x8A0002, 0x0000);               /* 0x01C8AA: the text layer's scroll / attributes */
    tm_w16(0x8A0004, 0x006E); tm_w16(0x8A0006, 0x0000);
    uint32_t src = 0x1C994, dst = 0x89A000;                            /* 0x01C8DA: 0x1C994..0x20994 -> cgram 0x89A000..0x89E000 */
    do {
        for (int k = 0; k < 0x80; k += 4) tm_w32(dst + k, vrd32(src + k));
        src += 0x80; dst += 0x80;
    } while (dst != 0x89E000 && src < 0x20994);
}

/* 0x01C90A: the text layer's 40 x 30 visible tiles to 0x3BF */
static void clear_text(void)
{
    for (int r = 0; r < 30; r++)
        for (int c = 0; c < 40; c++) tm_tile(c, r, 0x3BF);
}
/* 0x01C966: one row from a stream -- op 0: (count, value) fills until a negative count; op 1: count+1 literal words */
static void cmd_row(uint32_t dst, uint32_t src)
{
    const int op = (int16_t)vrd16(src); src += 2;
    if (op == 0) {
        for (;;) {
            const int n = (int16_t)vrd16(src); src += 2;
            if (n < 0) return;
            const uint16_t v = vrd16(src); src += 2;
            for (int i = 0; i <= n; i++) { tm_w16(dst, v); dst += 2; }
        }
    } else if (op == 1) {
        const int n = (int16_t)vrd16(src); src += 2;
        for (int i = 0; i <= n; i++) { tm_w16(dst, vrd16(src)); dst += 2; src += 2; }
    }
}
/* 0x01C936: x, y, stride, then blocks (rows-1, source) until a negative count; each block repeats its source row */
static void cmd_exec(uint32_t a)
{
    const int x = (int16_t)vrd16(a), y = (int16_t)vrd16(a + 2), stride = (int16_t)vrd16(a + 4);
    a += 6;
    uint32_t dst = TEXTRAM_BASE + (uint32_t)(int16_t)(x * 2 + (y << 7));
    for (;;) {
        const int rows = (int16_t)vrd16(a); a += 2;
        if (rows < 0) return;
        const uint32_t src = vrd32(a); a += 4;
        for (int i = 0; i <= rows; i++) { cmd_row(dst, src); dst += (uint32_t)stride; }
    }
}
/* the ten patterns (the pointer table at 0x369D4): each clears the text layer and runs its streams */
static void pattern(int n)
{
    static const uint32_t streams[10][6] = {
        { 0x1C0E4 },                                     /* 0x01C0CA */
        { 0x1C278, 0x1C286 },                            /* 0x01C254 */
        { 0x1C3C6, 0x1C3D4 },                            /* 0x01C3A2 */
        { 0x1C50A },                                     /* 0x01C4F0 */
        { 0x1C550 },                                     /* 0x01C536 */
        { 0x1C598 },                                     /* 0x01C57E */
        { 0x1C60A, 0x1C618, 0x1C626, 0x1C634, 0x1C642 }, /* 0x01C5C8 */
        { 0x1C676 },                                     /* 0x01C65C */
        { 0x1C76A },                                     /* 0x01C750 */
        { 0x1C85E },                                     /* 0x01C844 */
    };
    if (n < 0 || n > 9) return;
    clear_text();
    for (int k = 0; k < 6 && streams[n][k]; k++) cmd_exec(streams[n][k]);
}

/* ---- slot 16, 0x01B328: MONITOR TEST init */
void tm_p16(void)
{
    gfx_init();
    tm_w8(0x82401B, 0x7F);
    sync_post();
    tm_w16(WRAM0 + 0x16930, 0);
    tm_set_page(0x11);
}

/* ---- slot 17, 0x01B34C: MONITOR TEST -- eleven entries (ten patterns, EXIT). Start draws the pattern (palette base 0x7E) and
 * holds it until the next Start, which clears it (palette base back to 0x7F, sync_post). */
void tm_p17(void)
{
    if (tm_r16(WRAM0 + 0x16930)) {
        if (tm_confirm()) {
            tm_w16(WRAM0 + 0x16930, 0);
            tm_w8(0x82401B, 0x7F);
            sync_post();
        }
        return;
    }
    tm_page_select(0xB);
    tm_labels(0x369FC, 0x100);
    tm_legend(0);
    if (tm_confirm()) {
        if (tm_cursor() == 10) { tm_set_page(0); tm_w8(WRAM0 + 0x3FBE, 0); }
        else {
            tm_w16(WRAM0 + 0x16930, 1);
            tm_w8(0x82401B, 0x7E);
            pattern(tm_cursor());
        }
    }
}
