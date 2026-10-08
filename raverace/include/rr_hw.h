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
    uint8_t  adc[4];                   /* twin-stick games: ADC.0..3 = right Y, left Y, right X, left X (8-bit, centre 0x7F) */
    int      old_coin, credits1, credits2;
} rr_hw_t;

extern rr_hw_t g_hw;

bool rr_hw_init(const char *rom_dir);
void rr_hw_vblank(void);
void rr_hw_sci_irq(void);            /* C139 link chip: a frame arrived / a transmit finished */
int  rr_hw_irq_level(void);
void rr_hw_set_freeplay(bool on);     /* the game's own COIN OPTIONS -> FREE PLAY */
bool rr_hw_freeplay(void);
void rr_hw_drive_io(void);            /* the I/O board's answer (handle_driving_io) into shared RAM 0x30..0x3A */
void rr_hw_keycus_force(uint32_t v);   /* trace oracle: the next keycus read returns v (MAME's random answer) */
int  rr_env_init(void);                /* trace oracle (rr_env.c): RR_ENV=<file> replays what MAME's devices returned + where its interrupts landed */
int  rr_env_active(void);
void rr_hw_eeprom_persist(const char *path);   /* windowed runs: load the EEPROM from this file, save it back when it changes */
void rr_hw_eeprom_save(void);                  /* write the file if the game changed the EEPROM (no-op otherwise) */
void rr_hw_set_steering_motor(bool on);   /* the game's own OTHERS -> STEERING MOTOR (MAME's EEPROM has it OFF) */
void rr_hw_set_link_cabinet(int n);       /* cabinet number: settings group 3 byte 0 (link id & 7) */
uint8_t rr_hw_motor_byte(void);           /* the drive command the I/O board passes to the Motor/Feedback PCB */

#endif
