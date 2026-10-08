/*
 * s21_stubs.c -- stand-ins for the modules a build leaves out (s21_board.h), chosen at build time: -DS21_STUB_DSP, -DS21_STUB_SND,
 * -DS21_STUB_IO, -DS21_STUB_VIDEO. (Not weak symbols: a weak definition in an object would stop the linker pulling the real one
 * out of the module's static library.) The DSP stand-in STORES DSP RAM / point RAM / depth cue writes so a 68000 reads back its own.
 */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "s21_board.h"

#ifdef S21_STUB_DSP
#include "s21_dsp.h"
static uint16_t dspram[0x8000], depthcue[0x400], ptram_lat;
void (*s21_quad_out)(const s21_quad *q);
void (*s21_swap_out)(void);
bool s21_dsp_init(const char *romdir) { (void)romdir; return true; }
uint16_t s21_dsp_ram_r(uint32_t off) { return dspram[off & 0x7FFF]; }
void s21_dsp_ram_w(uint32_t off, uint16_t d, uint16_t m) { uint16_t *p = &dspram[off & 0x7FFF]; *p = (uint16_t)((*p & ~m) | (d & m)); }
void s21_dsp_ptram_ctl_w(uint16_t d, uint16_t m) { (void)d; (void)m; }
uint16_t s21_dsp_ptram_r(void) { return ptram_lat; }
void s21_dsp_ptram_w(uint16_t d, uint16_t m) { ptram_lat = (uint16_t)((ptram_lat & ~m) | (d & m)); }
uint16_t s21_dsp_depthcue_r(uint32_t off) { return depthcue[off & 0x3FF]; }
void s21_dsp_depthcue_w(uint32_t off, uint16_t d, uint16_t m) { uint16_t *p = &depthcue[off & 0x3FF]; *p = (uint16_t)((*p & ~m) | (d & m)); }
void s21_dsp_reset(int s) { (void)s; }
void s21_dsp_kick(void) {}
bool s21_dsp_run(long c) { (void)c; return true; }
const char *s21_dsp_error(void) { return NULL; }
void s21_dsp_use_oracle(void) {}
#endif

#ifdef S21_STUB_SND
#include "s21_snd.h"
int  s21_snd_init(const uint8_t *snd, const uint8_t *voi[4], uint8_t *dpram) { (void)snd; (void)voi; (void)dpram; return 1; }
void s21_snd_reset(int hold) { (void)hold; }
void s21_snd_run(int cycles) { (void)cycles; }
uint64_t s21_snd_cycles(void) { return 0; }   /* the board runs both 2.048 MHz CPUs to one board time (s21_board.c) */
int  s21_snd_mix(int16_t *out, int n) { for (int i = 0; i < n * 2; i++) out[i] = 0; return n; }
#endif

#ifdef S21_STUB_IO
#include "s21_io.h"
int  s21_io_init(const uint8_t *c68, uint8_t *dpram) { (void)c68; (void)dpram; return 1; }
void s21_io_set_inputs(const s21_inputs *in) { (void)in; }
void s21_io_get_inputs(s21_inputs *in) { s21_io_default_inputs(in); }
void s21_io_default_inputs(s21_inputs *in) { for (unsigned i = 0; i < sizeof *in; i++) ((uint8_t *)in)[i] = 0xFF; }
void s21_io_reset(int hold) { (void)hold; }
void s21_io_vblank(void) {}
void s21_io_run(int cycles) { (void)cycles; }
uint64_t s21_io_cycles(void) { return 0; }
#endif

#ifdef S21_STUB_VIDEO
void s21_video_spriteram_w(uint16_t *spr, uint32_t woff, uint16_t d, uint16_t m)
{ uint16_t *p = &spr[woff & 0xFFFF]; *p = (uint16_t)((*p & ~m) | (d & m)); }
#endif
