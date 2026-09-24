#ifndef RR_HW_H
#define RR_HW_H
#include <stdint.h>
#include <stdbool.h>

typedef struct rr_hw {
    uint32_t irq_enabled, irq_state;   /* bit per syscon line 0..4 */
    bool     mcu_run;                  /* syscon 0x18 */
    uint8_t  dsp_ctrl;                 /* syscon 0x1A */
    uint32_t dsw, cpuleds;
    uint16_t portbits[2];
    uint16_t keycus_rng; uint32_t lcg;
    /* inputs as MAME's ports hold them (before the per-game offsets) */
    uint16_t inputs, steer, gas, brake;
    int      old_coin, credits1, credits2;
} rr_hw_t;

extern rr_hw_t g_hw;

bool rr_hw_init(const char *rom_dir);
void rr_hw_vblank(void);
int  rr_hw_irq_level(void);
void rr_hw_set_freeplay(bool on);     /* the game's own COIN OPTIONS -> FREE PLAY */
bool rr_hw_freeplay(void);

#endif
