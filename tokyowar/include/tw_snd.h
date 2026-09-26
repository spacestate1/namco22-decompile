#ifndef TW_SND_H
#define TW_SND_H
#include <stdint.h>
#include <stdbool.h>

/*
 * Tokyo Wars' sound MCU and the C352 it drives -- the Super System 22's ONE M37710 (namcos22.cpp
 * mcu_program). The MCU runs the sound driver AND reads the cabinet: its ports 4/5 and A-D
 * channels are the coin / start / trigger switches and the wheel and pedals, and it writes them
 * into the shared RAM the 68K reads (0xA04000). Its program is tw1data.8k, translated to C at
 * build time (gen/tw_snd_driver.c, tools/gen/snd_translate.py --game tw); the fetch/decode
 * interpreter exists only in the tw_oracle test build.
 */
bool tw_snd_init(const char *rom_dir);
void tw_snd_set_run(bool run);           /* syscon 0x16: 0 holds the MCU in reset */
void tw_snd_slice(void);                 /* 1/16 of a video frame of MCU time (and of chip time) */
void tw_snd_close(void);
bool tw_snd_faulted(void);
void tw_snd_set_output(bool on);         /* the front pair to the sound card (engine/audio_out.c) as it is generated */

/* what the MCU's ports and A-D converter read (the host's cabinet) */
#define TW_IN_COIN    0x0001
#define TW_IN_SERVICE 0x0004
#define TW_IN_TEST    0x0008
#define TW_IN_START   0x0010
#define TW_IN_RTRIG   0x0020
#define TW_IN_LTRIG   0x0040
void tw_snd_inputs(uint16_t pressed, unsigned wheel, unsigned fwd, unsigned bwd);   /* pressed: bits above; A-D 10-bit */
uint16_t tw_snd_outputs(void);           /* the MCU's output latches (lamps, motors) */
void tw_snd_debug(char *buf, int n);
#endif
