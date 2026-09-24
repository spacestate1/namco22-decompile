#ifndef RR_DSP_H
#define RR_DSP_H
#include <stdint.h>
#include <stdbool.h>

/* the master DSP (C71 = TMS320C25 + Namco BIOS) and the point ROM it reads */
extern int32_t  *g_pointrom;          /* signed24 words (MAME init_tables) */
extern uint32_t  g_pointrom_words;

bool rr_dsp_init(const char *rom_dir);
void rr_dsp_control(uint8_t v);       /* syscon 0x1A */
void rr_dsp_run(long steps);          /* advance the master (no-op while held in reset) */
void rr_dsp_vblank(void);             /* INT0 when DSP IRQs are on */
void rr_dsp_serial(void);             /* RINT/XINT, MAME's 100 Hz dsp_serial pulse */
bool rr_dsp_render_done(void);        /* pdp_begin since the last call (clears) */
uint16_t rr_dsp_pdp_base(void);
bool rr_dsp_slave_active(void);
void rr_dsp_debug(char *buf, int n);
int32_t rr_dsp_pointram_read(uint32_t a);

#endif
