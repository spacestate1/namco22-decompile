/* test_mode.h -- Prop Cycle's operator test mode (src/test_mode.c + test_mode_io.c + test_mode_misc.c), ported from 0x01A15E..0x01C0B0.
 * Memory convention for every page: 68K addresses through tm_r* / tm_w* (work RAM = the _W[] slot model by byte offset, the same
 * storage the game and the per-frame sync use; anything else = the memory map). The page word is 0xE03FB4 (16-bit), the page's
 * cursor byte 0xE03FB6 + page/2. */
#ifndef TEST_MODE_H
#define TEST_MODE_H
#include <stdint.h>

int32_t  tm_r8(uint32_t a);              /* sign-extended */
int32_t  tm_r16(uint32_t a);             /* sign-extended */
uint32_t tm_r32(uint32_t a);
void     tm_w8(uint32_t a, int32_t v);
void     tm_w16(uint32_t a, int32_t v);
void     tm_w32(uint32_t a, uint32_t v);

int  tm_page(void);                      /* the page word 0xE03FB4 */
void tm_set_page(int n);
int  tm_cursor(void);                    /* this page's cursor byte (signed) */
void tm_set_cursor(int v);

/* the inputs, read the way their producers write them this frame */
unsigned tm_in_edge(void);               /* 0xE02BDA: stick rising edge  (1 left, 2 right, 4 up, 8 down) */
unsigned tm_in_rep(void);                /* 0xE02BDC: edge + auto-repeat */
unsigned tm_in_dir(void);                /* 0xE02BD8: stick held */
unsigned tm_in_start(void);              /* 0xE02BA6: service-byte edge (bit 0 = Start) */
unsigned tm_in_svc(void);                /* 0xE02BA4: service byte held */
unsigned tm_in_sw(void);                 /* 0xE02B80: switch byte held (bit 3 = the Test switch) */
unsigned tm_in_swedge(void);             /* 0xE02B82: its edge */
uint32_t tm_frame(void);                 /* 0xE00C98 */

/* drawing (the text layer, 64 tiles a row at 0x89E000) */
void tm_tile(int col, int row, unsigned v);
void tm_print(int col, int row, uint32_t rom_string, int pal);                 /* 0x02108C with a ROM string */
void tm_hex(int col, int row, int n, uint32_t v, int pal);                    /* 0x0210CC */
void tm_draw_decimal(int col, int row, int width, int32_t value, uint32_t attr);   /* 0x01A3C2 */
void tm_draw_time(int col, int row, int width, uint32_t value, int pal);           /* 0x01A32C */
void tm_legend(int n);                                                         /* 0x01A40E */
void tm_labels(uint32_t table, int count);                                     /* 0x01A55A */

/* the item records (see test_mode.c) */
int32_t tm_get_value(uint32_t rec);                                            /* 0x01A5CE */
void    tm_set_value(uint32_t rec, int32_t v);                                 /* 0x01A614 */
void    tm_draw_items(uint32_t rec, int count);                                /* 0x01A652 */
int     tm_page_select(int count);                                             /* 0x01A79A */
int     tm_value_adjust(uint32_t rec);                                         /* 0x01A806 */
void    tm_copy_to_work(uint32_t rom);                                         /* 0x01A8B0 */
void    tm_clear_highlights(uint32_t rec);                                     /* 0x01A8E2 */
int     tm_confirm(void);                                                      /* 0x01A8FC */

/* the pages: slot n of the table at 0x36548 (even = init, odd = run). test_mode.c has 0..6, test_mode_io.c 7..17,
 * test_mode_misc.c 18..33. */
void tm_p07(void); void tm_p08(void); void tm_p09(void); void tm_p10(void); void tm_p11(void); void tm_p12(void);
void tm_p13(void); void tm_p14(void); void tm_p15(void); void tm_p16(void); void tm_p17(void);
void tm_p18(void); void tm_p19(void); void tm_p20(void); void tm_p21(void); void tm_p22(void); void tm_p23(void);
void tm_p24(void); void tm_p25(void); void tm_p26(void); void tm_p27(void); void tm_p28(void); void tm_p29(void);
void tm_p30(void); void tm_p31(void); void tm_p32(void); void tm_p33(void);

void tm_state_title_run(void);           /* 0x01A2AE: one frame of test mode */
void tm_state_title_init(void);          /* 0x01A210: entering it */
#endif
