/*
 * ss22_board.h -- the Namco Super System 22 board as the lifted 68EC020 sees it (engine/ss22_board.c): the memory map
 * (MAME namcos22s_am) and the devices on it -- keycus, syscon (the IRQ lines, the DSP and sound-MCU enables), the DSW, the two serial
 * bit ports, the EEPROM. Every access in a game's lifted program is an rr_read / rr_write at the ORIGINAL 24-bit address,
 * big-endian, with the width the instruction used. The map is the Super 22 one -- keycus 0x400000, syscon 0x700000, video 0x8xxxxx,
 * polygon RAM 0xC00000, work RAM 0xE00000 -- not Rave Racer's System 22 one (32-bit addresses).
 *
 * Device registers keep MAME's HANDLER WIDTH: a 32-bit access to a 16-bit device is two handler calls, high half first (the keycus
 * draws one random value per call, and the trace comparison replays them in that order).
 *
 * What is a game's: the program image (its ROM chips' layout), the default EEPROM, and the keycus's one answer (ss22_board_cfg).
 * Tokyo Wars and Dirt Dash each had a copy of this file, byte for byte but for those.
 */
#ifndef ENG_SS22_BOARD_H
#define ENG_SS22_BOARD_H
#include <stdint.h>
#include <stdbool.h>

#define SS22_ROM_SIZE    0x400000u
#define SS22_WRAM_SIZE   0x40000u
#define SS22_SCI_SIZE    0x4000u
#define SS22_EEPROM_SIZE 0x2000u
#define SS22_SHARED_SIZE 0x8000u          /* 0xA04000, 16-bit words shared with the sound MCU */
#define SS22_POLY_WORDS  0x8000u          /* 0xC00000, 32-bit words, 24 connected */
#define SS22_CGRAM_SIZE  0x1E000u         /* 0x880000; the last 0x2000 of the range is text RAM */
#define SS22_TEXT_SIZE   0x2000u          /* 0x89E000 */
#define SS22_PAL_SIZE    0x18000u         /* 0x828000 */
#define SS22_MIXER_SIZE  0x400u           /* 0x824000 */
#define SS22_VICS_SIZE   0x10000u         /* 0x900000 */
#define SS22_SPRITE_SIZE 0x30000u         /* 0x980000 (C374) */

typedef struct ss22_sys {
    uint8_t  rom[SS22_ROM_SIZE];
    uint8_t  wram[SS22_WRAM_SIZE];
    uint8_t  sci[SS22_SCI_SIZE];
    uint8_t  eeprom[SS22_EEPROM_SIZE];
    uint8_t  shared[SS22_SHARED_SIZE];    /* 68K view, big-endian words */
    uint32_t poly[SS22_POLY_WORDS];       /* MAME's stored value: signed24 */
    uint8_t  cgram[SS22_CGRAM_SIZE];
    uint8_t  text[SS22_TEXT_SIZE];
    uint8_t  pal[SS22_PAL_SIZE];
    uint8_t  mixer[SS22_MIXER_SIZE];
    uint8_t  vics[SS22_VICS_SIZE];
    uint8_t  sprite[SS22_SPRITE_SIZE];
    uint16_t czattr[8];
    uint16_t czram[4][256];               /* namcos22s_czram: 4 banks of 256 words */
    uint16_t spotram[0x800];
    uint16_t tilemapattr[8];
    uint32_t vics_ctl[0x20];
    uint8_t  syscon[0x20];
    uint16_t spot_addr, spot_enable;
    uint32_t chipselect;
    uint32_t n_unmapped, n_romwrite;
} ss22_sys_t;

extern ss22_sys_t g_ss22;

typedef struct ss22_hw {
    uint32_t irq_enabled, irq_state;      /* one bit per syscon line 0..3 */
    bool     mcu_run;                     /* syscon 0x16 */
    uint8_t  dsp_ctrl;                    /* syscon 0x1C */
    uint32_t dsw;
    uint16_t portbits[2];
    uint16_t keycus_rng; uint32_t lcg;
} ss22_hw_t;

extern ss22_hw_t g_hw;

typedef struct {
    const char *tag;                                  /* "TW": messages ([TWMEM]) and the test variables <tag>_MBOXLOG, <tag>_KEYCUS_FILE */
    uint32_t keycus_unit, keycus_value;               /* the protection chip's one answer: a read of this 16-bit unit returns the value */
    bool (*load_program)(const char *rom_dir);        /* the 68K's image into g_ss22.rom (the chips' layout is the game's) */
    bool (*load_eeprom)(const char *rom_dir);         /* the EEPROM's starting contents (the set's defaults, or blank) */
} ss22_board_cfg;

void ss22_board_use(const ss22_board_cfg *cfg);     /* first: which game this is */
bool ss22_load_program(const char *rom_dir);
bool ss22_hw_init(const char *rom_dir);
void ss22_hw_vblank(void);                          /* the frame's vblank: raise syscon line 0 if the game enabled it */
void ss22_hw_scanline(void);                        /* the scanline IRQ (line 1) */
int  ss22_hw_irq_level(void);                       /* the highest pending, enabled IRQ as a 68K level (0 = none) */
void ss22_hw_keycus_force(uint32_t v);              /* trace oracle: the next keycus read answers with MAME's value */

uint32_t rr_read(uint32_t a, int size);           /* the lifted-code ABI: size 1, 2 or 4 */
void     rr_write(uint32_t a, int size, uint32_t v);

/* syscon 0x1C and 0x16 reach the game's DSP and sound board through these (set by them at init) */
extern void (*g_ss22_dsp_control)(uint8_t v);
extern void (*g_ss22_snd_set_run)(bool run);

/* Interactive runs keep the machine's EEPROM (options, records) in a file beside the game, as MAME's nvram does: loaded at boot when it holds
 * exactly one image, rewritten (temp file + rename) whenever the game changed it. Headless runs never touch it. */
void ss22_eeprom_persist(const char *path);
void ss22_eeprom_save(void);

/* <tag>_MBOXLOG=<file>: every 68K write into the sound mailbox (shared RAM 0..0x1FF): "frame offset size value" -- the 68K -> sound-MCU
 * command stream, for diffing against tools/mame/mbox68k.lua */
#endif
