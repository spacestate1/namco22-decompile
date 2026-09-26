#ifndef TW_HW_H
#define TW_HW_H
#include <stdint.h>
#include <stdbool.h>

typedef struct tw_hw {
    uint32_t irq_enabled, irq_state;   /* one bit per syscon line 0..3 */
    bool     mcu_run;                  /* syscon 0x16 */
    uint8_t  dsp_ctrl;                 /* syscon 0x1C */
    uint32_t dsw;
    uint16_t portbits[2];
    uint16_t keycus_rng; uint32_t lcg;
} tw_hw_t;

extern tw_hw_t g_hw;

bool tw_hw_init(const char *rom_dir);
void tw_hw_vblank(void);
void tw_hw_scanline(void);
#endif
