#ifndef RR_SND_BOARD_H
#define RR_SND_BOARD_H
#include <stdint.h>
#include "m37710.h"
/* the shared sound board (snd_board.c) and its one variable part: how
 * instructions get executed. Implemented by gen/snd_driver.c in the game and by
 * tools/sndoracle/sound_oracle.c in the test oracle. */
extern m37710_t g_snd_cpu;
extern uint8_t g_snd_bios[0x4000], *g_snd_data;
void snd_board_port4(void);
void snd_executor_init(void);
void snd_execute(m37710_t *c, uint64_t end_cycle);   /* run until c->cycles >= end_cycle */
#endif
