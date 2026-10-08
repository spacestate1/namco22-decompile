/* s21_host.h -- Cyber Sled's window side (src/host): the picture handed to engine/ss22_host.c, the cabinet controls. */
#ifndef S21_HOST_H
#define S21_HOST_H
#include <stdint.h>
#include <SDL2/SDL.h>
#include "eng_ui.h"
#define S21_PIC_W 496
#define S21_PIC_H 480
const uint8_t *s21_host_picture(void);        /* the software frame (RGB24 496x480), NULL = none yet */
int  s21_host_use_gl(void);                   /* 1: show the GL picture (default), 0: the software one (CS_PICTURE=sw) */
void s21_host_gl_unavailable(void);           /* the GL picture failed: the software one from now on */
int  s21_wide_extra(int fw, int fh);          /* s21_glutil.c: Hor+ widescreen -- board pixels shown each side of the 496 (0: none) */
void s21_view_for(int fw, int fh);            /* set the DSP board's view for a picture fw x fh, before the frame is emulated */
/* first run (cs21_roms.c): the ROM folder to use -- romdir, or roms/ when the chips lie there loose -- after unpacking MAME's zips into
 * romdir if chips are missing; NULL = still missing (said on stderr, and in a message box when window) */
const char *cs21_rom_setup(const char *romdir, int window);
/* controls (s21_input.c): keyboard + SDL game controllers -> the C68's ports (s21_io_set_inputs) */
void s21_input_init(void);
const eng_ui_page *s21_input_page(void);
void s21_input_event(const SDL_Event *e);
void s21_input_update(void);
void s21_input_neutral(void);
void s21_input_set_test(int on);              /* the Test switch (the Controls page's toggle) */
/* the Debug page (s21_debug_page.c): the developer test mode left in the ROM (PLAN.md Module I) */
struct s21_inputs;
const eng_ui_page *s21_debug_page(void);
void s21_debug_frame(struct s21_inputs *in);  /* every frame, last thing before s21_io_set_inputs: holds the Test switch / presses Gun */
#endif
