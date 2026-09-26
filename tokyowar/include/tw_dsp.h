#ifndef TW_DSP_H
#define TW_DSP_H
#include <stdint.h>
#include <stdbool.h>

/* the master DSP (C71 = TMS320C25 + Namco BIOS) of Tokyo Wars, and the point ROM it reads */
extern int32_t  *g_tw_pointrom;         /* signed24 words (MAME init_tables) */
extern uint32_t  g_tw_pointrom_words;

bool tw_dsp_init(const char *rom_dir);
void tw_dsp_control(uint8_t v);         /* syscon 0x1C: 0 reset, 1 run + DSP IRQs, 0xFF master alone (code upload) */
void tw_dsp_run(long steps);            /* advance the master (no-op while held in reset) */
void tw_dsp_vblank(void);               /* INT0 when the DSP IRQs are on */
void tw_dsp_serial(void);               /* RINT/XINT: MAME's 100 Hz dsp_serial pulse */
bool tw_dsp_active(void);
bool tw_dsp_slave_active(void);         /* the game started the slave (syscon/port 7): the list is to be drawn */
bool tw_dsp_faulted(void);
void tw_dsp_debug(char *buf, int n);
uint32_t tw_dsp_pdp_begins(void);       /* port-2 reads so far: the frames' display lists */
#endif
