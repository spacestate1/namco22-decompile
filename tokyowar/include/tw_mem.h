/*
 * tw_mem.h -- Tokyo Wars' memory map (Namco Super System 22, MAME namcos22s_am).
 *
 * Every 68EC020 access in the lifted program (gen/tw_lifted.c) is an rr_read / rr_write at the
 * ORIGINAL 24-bit address, big-endian, with the width the instruction used. The map is the
 * Super 22 one -- keycus at 0x400000, syscon at 0x700000, video at 0x8xxxxx, polygon RAM at
 * 0xC00000, work RAM at 0xE00000 -- not Rave Racer's System 22 one (32-bit addresses).
 *
 * Device registers keep MAME's HANDLER WIDTH: a 32-bit access to a 16-bit device is two
 * handler calls, high half first (the keycus draws one random value per call, and the trace
 * comparison replays them in that order).
 */
#ifndef TW_MEM_H
#define TW_MEM_H
#include <stdint.h>
#include <stdbool.h>

#define TW_ROM_SIZE    0x400000u
#define TW_WRAM_SIZE   0x40000u
#define TW_SCI_SIZE    0x4000u
#define TW_EEPROM_SIZE 0x2000u
#define TW_SHARED_SIZE 0x8000u          /* 0xA04000, 16-bit words shared with the sound MCU */
#define TW_POLY_WORDS  0x8000u          /* 0xC00000, 32-bit words, 24 connected */
#define TW_CGRAM_SIZE  0x1E000u         /* 0x880000; the last 0x2000 of the range is text RAM */
#define TW_TEXT_SIZE   0x2000u          /* 0x89E000 */
#define TW_PAL_SIZE    0x18000u         /* 0x828000 */
#define TW_MIXER_SIZE  0x400u           /* 0x824000 */
#define TW_VICS_SIZE   0x10000u         /* 0x900000 */
#define TW_SPRITE_SIZE 0x30000u         /* 0x980000 (C374) */

typedef struct tw_sys {
    uint8_t  rom[TW_ROM_SIZE];
    uint8_t  wram[TW_WRAM_SIZE];
    uint8_t  sci[TW_SCI_SIZE];
    uint8_t  eeprom[TW_EEPROM_SIZE];
    uint8_t  shared[TW_SHARED_SIZE];    /* 68K view, big-endian words */
    uint32_t poly[TW_POLY_WORDS];       /* MAME's stored value: signed24 */
    uint8_t  cgram[TW_CGRAM_SIZE];
    uint8_t  text[TW_TEXT_SIZE];
    uint8_t  pal[TW_PAL_SIZE];
    uint8_t  mixer[TW_MIXER_SIZE];
    uint8_t  vics[TW_VICS_SIZE];
    uint8_t  sprite[TW_SPRITE_SIZE];
    uint16_t czattr[8];
    uint16_t czram[4][256];             /* namcos22s_czram: 4 banks of 256 words */
    uint16_t spotram[0x800];
    uint16_t tilemapattr[8];
    uint32_t vics_ctl[0x20];
    uint8_t  syscon[0x20];
    uint16_t spot_addr, spot_enable;
    uint32_t chipselect;
    uint32_t n_unmapped, n_romwrite;
} tw_sys_t;

extern tw_sys_t g_tw;

uint32_t rr_read(uint32_t a, int size);           /* the lifted-code ABI: size 1, 2 or 4 */
void     rr_write(uint32_t a, int size, uint32_t v);

bool tw_load_program(const char *rom_dir);        /* tw2ver-a.4/.3/.2/.1 -> byte lanes 0/1/2/3 */
bool tw_load_eeprom(const char *rom_dir);         /* tokyowar_defaults.nv */

/* devices (src/tw_hw.c) */
uint32_t tw_hw_keycus_r(uint32_t unit);           /* one 16-bit handler call */
uint32_t tw_hw_dsw(void);
uint32_t tw_hw_portbit_r(uint32_t unit);
void     tw_hw_portbit_w(uint32_t unit);
void     tw_hw_syscon_w(uint32_t off, uint8_t data);
int      tw_hw_irq_level(void);
/* TW_MBOXLOG=<file>: every 68K write into the sound mailbox (shared RAM 0..0x1FF): "frame offset size value" --
 * the 68K -> sound-MCU command stream, for diffing against tools/mame/mbox68k.lua */
extern int g_tw_mbox;
void tw_mbox_log(uint32_t off, int size, uint32_t v);
#endif
