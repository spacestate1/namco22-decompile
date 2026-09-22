/*
 * Game logic bridge
 *
 * This translates the decompiled 68020 code into C functions that
 * operate on our memory arrays via mem_read/write or direct pointers.
 *
 * Ghidra's DAT_00XXXXXX references become mem_read/write calls.
 * Work RAM globals become direct array accesses for speed.
 */
#include <stdlib.h>
#include "propcycl.h"

void input_read_service_buttons(void);  /* game_misc.c */
int  input_process_analog_deltas(void);  /* game_system.c -- the pedal */
void input_decode_buttons(void);        /* game_misc.c */
uint32_t check_game_state_change(void); /* game_misc.c */
int32_t  coin_credit_update(void);      /* game_misc.c (undefined4) */
void coin_credit_init(void);            /* game_misc.c */
void FUN_000231ec(void);                /* game_misc.c: sprite_update */
#include "video_formats.h"
#include "font_data.h"
#include "trace.h"

/* Forward declarations for debug functions */
static void debug_check_cgram(void);
static void debug_dump_tilemap(const char* filename);

/* ========== Convenience macros ========== */

/* Work RAM direct access (big-endian) */
#define WRAM8(off)   (g_sys.work_ram[(off)])
#define WRAM16(off)  ((uint16_t)(g_sys.work_ram[(off)] << 8) | g_sys.work_ram[(off)+1])
#define WRAM32(off)  ((uint32_t)(g_sys.work_ram[(off)] << 24) | (g_sys.work_ram[(off)+1] << 16) | \
                      (g_sys.work_ram[(off)+2] << 8) | g_sys.work_ram[(off)+3])

#define WRAM_SET16(off, v) do { g_sys.work_ram[(off)] = (v) >> 8; g_sys.work_ram[(off)+1] = (v) & 0xFF; } while(0)
#define WRAM_SET32(off, v) do { \
    g_sys.work_ram[(off)]   = ((v) >> 24) & 0xFF; \
    g_sys.work_ram[(off)+1] = ((v) >> 16) & 0xFF; \
    g_sys.work_ram[(off)+2] = ((v) >> 8) & 0xFF; \
    g_sys.work_ram[(off)+3] = (v) & 0xFF; \
} while(0)

/* Work RAM offsets (address - 0xE00000) */
#define OFF_FRAME_COUNTER     0x0C98
#define OFF_VBLANK_FLAG       0x0C9C
#define OFF_DOUBLE_BUF        0x0CA0
#define OFF_HALT_FLAG         0x0CA8
#define OFF_PAUSE_FLAG        0x0CAC
#define OFF_GAME_STATE        0x0CBC
#define OFF_SUB_STATE_GP      0x0CC0
#define OFF_LED_PATTERN       0x0C90
#define OFF_LED_STATE         0x0C94

/* SYSCON register access */
#define SYSCON(off) (g_sys.syscon[(off)])

/* ROM read (big-endian) */
#define ROM32(addr) ((uint32_t)(g_sys.rom[(addr)] << 24) | (g_sys.rom[(addr)+1] << 16) | \
                     (g_sys.rom[(addr)+2] << 8) | g_sys.rom[(addr)+3])

/* ========== Ported Init Functions ========== */

/* syscon_init @ 0x00BE8C */
static void syscon_init(void) {
    SYSCON(0x09) = 0x62;
    SYSCON(0x0A) = 0x62;
    SYSCON(0x0B) = 0x57;
    SYSCON(0x0C) = 0x40;
    SYSCON(0x0D) = 0x12;
    SYSCON(0x0E) = 0x52;
    SYSCON(0x0F) = 0x72;
    SYSCON(0x10) = 0xE0;
    SYSCON(0x11) = 0x2C;
    SYSCON(0x12) = 0x50;
    SYSCON(0x13) = 0xFF;
    SYSCON(0x17) = 0x0F;
    SYSCON(0x00) = 4;    /* vblank IRQ level 4 */
    SYSCON(0x01) = 2;    /* hblank IRQ level 2 */
    SYSCON(0x02) = 3;    /* SCI IRQ level 3 */
    SYSCON(0x03) = 1;    /* unknown IRQ level 1 */
    SYSCON(0x08) = 0;
}

/* clear_work_ram @ 0x00BF14 */
static void clear_work_ram(void) {
    /* Original clears 0x7C00 dwords = 0x1F000 bytes starting at 0xE00000 */
    memset(g_sys.work_ram, 0, 0x1F000);
}

/* keycus_write_1 @ 0x021C6C */
static void keycus_write_1(void) {
    mem_write16(0x40000C, 0xA532);
}

/* keycus_write_2 @ 0x021C76 */
static void keycus_write_2(void) {
    mem_write16(0x400004, 0xCD66);
}

/* dipswitch_init @ 0x022286 */
static void dipswitch_init(void) {
    /* Read DIP switches (side effect), clear cache */
    (void)mem_read16(0x440000);
    WRAM_SET32(0x2C0C, 0);
}

/* tilemap_attr_init @ 0x0226D0 */
static void tilemap_attr_init(void) {
    mem_write16(0x8A0000, 0x035C);
    mem_write16(0x8A0002, 0x0000);
    mem_write16(0x8A0004, 0x006E);
    mem_write16(0x8A0006, 0x0000);
    mem_write16(0x8A0008, 0x01FF);
    /* Original has a delay loop of 0x3B iterations - skip */
}

/* video_mixer_regs_init @ 0x02283C */
static void video_mixer_regs_init(void) {
    uint8_t* vm = g_sys.videomix;
    vm[0x00] = 0xFF;  /* Fade R */
    vm[0x01] = 0xFF;  /* Fade G */
    vm[0x02] = 0xFF;  /* Fade B */
    vm[0x03] = 0x00;  /* Fade control */
    vm[0x04] = 0x00;  /* Poly fade R */
    vm[0x05] = 0xFF;  /* Fog R */
    vm[0x06] = 0xFF;  /* Fog G */
    vm[0x07] = 0xFF;  /* Fog B */
    vm[0x08] = 0x00;  /* Fog near */
    vm[0x09] = 0x00;  /* Fog far */
    vm[0x0A] = 0x00;  /* Fog control */
    vm[0x0B] = 0x7F;  /* Global alpha */
    vm[0x0C] = 0x00;  /* Poly fade G */
    vm[0x0D] = 0xFF;  /* Poly fade B */
    vm[0x12] = 0xFF;  /* Screen fade */
    vm[0x14] = 0x0F;  /* Mixer flags */
}

/* dsp_polygon_ram_init @ 0x022CAE (partial) */
static void dsp_init(void) {
    /* Disable DSP */
    SYSCON(0x1C) = 0;

    /* Clear DSP RAM header */
    memset(g_sys.dspram, 0, 0x80);

    /* Set initial DSP params */
    mem_write32(0xC00000, 1);
    mem_write32(0xC00008, 4);
    mem_write32(0xC00014, 1);
    mem_write32(0xC00018, 0x32);
    mem_write32(0xC0001C, 0x280);

    /* THE VIEWPORT BLOCKS. ROM dsp_init @0x022C7E calls 0x022D3A
     * (dsp_param_init) right after the polygon-RAM init; this hand-written
     * shadow of it (register row 81's class) left the call out, so until
     * the first stage start ran it no viewport had a zoom, a priority or an
     * identity view in DSP RAM -- the attract's FASTEST "PERFECT" SCORE page
     * drew its course plate with the world camera at priority 0. */
    { extern int dsp_param_init(void); dsp_param_init(); }

    /* Enable DSP */
    SYSCON(0x1C) = 1;
}

/* mcu_init @ 0x022F70 */
static void mcu_init(void) {
    /* Release MCU from reset */
    SYSCON(0x16) = 1;

    /* In real hardware, this does a handshake loop waiting for MCU.
     * We stub it: MCU is instantly "ready" */

    /* Write audio reset command */
    uint32_t audio_off = 0xA0BE82 - COMMSRAM_BASE;
    g_sys.commsram[audio_off] = 0xFF;
    g_sys.commsram[audio_off + 1] = 0xFF;

    /* Mark MCU as ready */
    uint32_t mcu_off = 0xA0BD01 - COMMSRAM_BASE;
    g_sys.commsram[mcu_off] = 0x00; /* bit 7 clear = not busy */

    /* Clear MCU ready flag in work RAM */
    WRAM_SET32(0x0CB0, 0);
}

/* globals_init @ 0x00BD7E (partial - zeros many globals) */
static void globals_init(void) {
    WRAM_SET32(0x0C98, 0);  /* frame counter */
    WRAM_SET32(0x0CA0, 0);  /* double buffer */
    WRAM_SET32(0x0CA8, 0);  /* halt */
    WRAM_SET32(0x0CAC, 0);  /* pause */
    WRAM_SET32(0x0E1C, 0);  /* system halt */
    /* Zero large ranges the original does */
    memset(g_sys.work_ram + 0x3F10, 0, 0x20);
    memset(g_sys.work_ram + 0x2C24, 0, 0x20);

    /* Set defaults the original sets */
    WRAM_SET32(0x2E3C, 1);
    WRAM_SET32(0x2C2C, 1);
    WRAM_SET32(0x2C30, 4);
}

/* sprite_ram_header_init @ 0x023022 (partial) */
static void sprite_init(void) {
    uint8_t* spr = g_sys.spriteram;
    /* Set sprite header registers */
    mem_write16(0x980000, 6);
    mem_write16(0x980002, 0);
    mem_write16(0x980004, 1);
    mem_write16(0x980006, 0x53);
    mem_write16(0x980008, 0x300);
    mem_write16(0x98000A, 0x200);
    mem_write16(0x98000C, 0x300);
    mem_write16(0x98000E, 0);
    mem_write16(0x980010, 0x20);
    mem_write16(0x980012, 0x20);
    mem_write16(0x980014, 0x280);
    mem_write16(0x980016, 0x4FF);
    mem_write16(0x980018, 0x32A);
    mem_write16(0x98001A, 0x509);

    /* THREE STATIC SPRITERAM TABLES THE GAME WRITES AT BOOT AND WE DID NOT.
     *
     * Diffing our live feed against the MAME gameplay capture
     * (dumps/gameplay/sprite_f*.bin): MAME carries 650 non-zero words, we
     * carried 58. The headers and clip values already matched; what was
     * missing were three blocks, byte-identical in every captured frame of
     * both the gameplay and the attract runs, i.e. boot-time constants:
     *
     *   0x080..0x08F  the clip pair repeated -- we wrote only the first 4 of
     *                 16 words, so clip indices 1..3 selected zeroed windows
     *   0x100..0x11F  a 32-entry dither/fade ramp (0x00000000 -> 0xFFFEFFFF,
     *                 nibbles stepping 0,8,A,E,F)
     *   0x180..0x19F  the same ramp again (verified identical)
     *   0x200..0x3FF  the TILE-LIST INDIRECTION table, an identity map stored
     *                 as packed pairs: word k = (2k)<<16 | (2k+1)
     *
     * The indirection table is the "C374 sprite-group semantics unported"
     * item: without it sprite ids resolve as raw 32x32 tiles and land in the
     * Japanese font region as yellow glyphs (register rows 26 and 35).
     * sprite_hw.c itself is exonerated -- it reproduces a real gameplay
     * capture 26818/26818 pixels exactly. */
    { unsigned i;
      static const uint32_t ramp[32] = {
        0x00000000,0x00008000,0x80008000,0x80008080,0x80808080,0x80808880,0x88808880,0x88808888,
        0x88888888,0x8888A888,0xA888A888,0xA888A8A8,0xA8A8A8A8,0xA8A8AAA8,0xAAA8AAA8,0xAAA8AAAA,
        0xAAAAAAAA,0xAAAAEAAA,0xEAAAEAAA,0xEAAAEAEA,0xEAEAEAEA,0xEAEAEEEA,0xEEEAEEEA,0xEEEAEEEE,
        0xEEEEEEEE,0xEEEEFEEE,0xFEEEFEEE,0xFEEEFEFE,0xFEFEFEFE,0xFEFEFFFE,0xFFFEFFFE,0xFFFEFFFF };
      #define SPRW(word, v) do { uint32_t _v = (v); uint32_t _o = (word) * 4u;               spr[_o+0] = (uint8_t)(_v >> 24); spr[_o+1] = (uint8_t)(_v >> 16);               spr[_o+2] = (uint8_t)(_v >> 8);  spr[_o+3] = (uint8_t)_v; } while (0)
      for (i = 0; i < 16; i += 2) { SPRW(0x80 + i, 0x028004FFu); SPRW(0x81 + i, 0x032A0509u); }
      for (i = 0; i < 32; i++)    { SPRW(0x100 + i, ramp[i]);    SPRW(0x180 + i, ramp[i]); }
      for (i = 0; i < 0x200; i++) { SPRW(0x200 + i, ((2u*i) << 16) | (2u*i + 1u)); }
      #undef SPRW
    }
}

/* task_system_init @ 0x00A7BC (partial) */
static void task_system_init(void) {
    /* Zero task pools */
    memset(g_sys.work_ram + 0x3E70, 0, 0x30);
    /* Set defaults */
    WRAM_SET32(0x3E80, 1);
    WRAM_SET32(0x3E84, 1);
    WRAM_SET32(0x3E88, 1);
    WRAM_SET32(0x3E8C, 1);
    /* Similar for other pools */
    memset(g_sys.work_ram + 0x3EB0, 0, 0x30);
    WRAM_SET32(0x3EC0, 1);
    WRAM_SET32(0x3EC4, 1);
    WRAM_SET32(0x3EC8, 1);
    WRAM_SET32(0x3ECC, 1);
    memset(g_sys.work_ram + 0x3EF0, 0, 0x30);
    WRAM_SET32(0x3F00, 1);
    WRAM_SET32(0x3F04, 1);
    WRAM_SET32(0x3F08, 1);
    WRAM_SET32(0x3F0C, 1);
    /* Task list pointers */
    WRAM_SET32(0x2E40, 0xE02E4C);
    WRAM_SET32(0x2E44, 0xE02E4C);
    WRAM_SET32(0x2E48, 0x3C000);
}

/* ========== Attract Mode / Title Screen Init ========== */

/* attract_enable_display @ 0x1C874 */
static void attract_enable_display(void) {
    g_sys.videomix[0x1B] = 0x7E;  /* enable text+sprite+polygon layers */
}

/* attract_load_palette @ 0x1C87E
 * Copies 256 RGB values from ROM 0x20994 into palette planes.
 * ROM layout: R[256] at +0, G[256] at +0x100, B[256] at +0x200
 * Palette destination: plane R at 0x82FE00, G at 0x837E00, B at 0x83FE00
 *   = palette_ram offsets 0x7600, 0xFE00 (=0x8000+0x7600), 0x17600 (=0x10000+0x7600)
 *   wait, let me recalculate: 0x82FE00 - 0x828000 = 0x7E00
 */
static void attract_load_palette(void) {
    uint32_t rom_src = 0x20994;  /* ROM address of palette data */
    uint32_t pal_r = 0x7E00;    /* 0x82FE00 - 0x828000 */
    uint32_t pal_g = 0xFE00;    /* 0x837E00 - 0x828000 */
    uint32_t pal_b = 0x17E00;   /* 0x83FE00 - 0x828000 */

    for (int i = 0; i < 256; i++) {
        if (rom_src + i < ROM_SIZE && pal_r + i < PALETTE_SIZE) {
            g_sys.palette_ram[pal_r + i] = g_sys.rom[rom_src + i];          /* R */
            g_sys.palette_ram[pal_g + i] = g_sys.rom[rom_src + 0x100 + i];  /* G */
            g_sys.palette_ram[pal_b + i] = g_sys.rom[rom_src + 0x200 + i];  /* B */
        }
    }
}

/* attract_load_char_rom @ 0x1C8AA
 * Sets tilemap attribute registers from ROM table at 0x1C8D2
 */
static void attract_load_char_rom(void) {
    /* Copy 4 16-bit values from ROM 0x1C8D2 to tilemap attrs 0x8A0000 */
    for (int i = 0; i < 4; i++) {
        uint32_t rom_off = 0x1C8D2 + i * 2;
        uint16_t val = (g_sys.rom[rom_off] << 8) | g_sys.rom[rom_off + 1];
        g_sys.tilemapattr[i * 2]     = val >> 8;
        g_sys.tilemapattr[i * 2 + 1] = val & 0xFF;
    }
}

/* attract_load_sprite_table @ 0x1C8DA
 * Copies sprite data from ROM 0x1C994 into CGRAM at offset 0x1A000
 * (0x89A000 - 0x880000 = 0x1A000)
 * Copies until reaching 0x89E000 (textram) or ROM 0x20994
 */
static void attract_load_sprite_table(void) {
    uint32_t rom_src = 0x1C994;
    uint32_t cg_dst = 0x1A000;  /* CGRAM offset */
    uint32_t max_dst = CGRAM_SIZE;  /* 0x1E000 */
    uint32_t max_src = 0x20994;

    while (cg_dst < max_dst && rom_src < max_src && rom_src < ROM_SIZE) {
        g_sys.cgram[cg_dst] = g_sys.rom[rom_src];
        cg_dst++;
        rom_src++;
    }
}

/* attract_clear_textram @ 0x1C90A
 * Fill text RAM with tile 0x03BF (blank character)
 */
static void attract_clear_textram(void) {
    for (int i = 0; i < TEXTRAM_SIZE; i += 2) {
        g_sys.textram[i]     = 0x03;
        g_sys.textram[i + 1] = 0xBF;
    }
}

/* attract_gfx_init @ 0x1C0B0 */
static void attract_gfx_init(void) {
    attract_enable_display();
    attract_load_palette();
    attract_load_char_rom();
    attract_load_sprite_table();
}

/* attract_tilemap_init / tilemap_cmd_exec (formerly a hand-written
 * shadow here, same defect class as CLAUDE.md register row 81's
 * set_background_color) REMOVED 2026-09-16. This local `static
 * attract_tilemap_init` silently shadowed the real, externally-linked
 * `attract_tilemap_init` in game_title.c for every call in this file --
 * C resolves an unqualified call to the file-local static definition
 * regardless of what else is declared with the same name elsewhere, so
 * the real one (and this session's disassembly-verified port of
 * attract_tilemap_cmd_exec/attract_tilemap_cmd_dispatch, ROM
 * 0x01C936/0x01C966) was dead code no matter how correct it was.
 *
 * This shadow's own `tilemap_cmd_exec` inner loop was ALSO measurably
 * wrong: it treated the per-row opcode-0 case as "copy N distinct tiles
 * read from ROM, then chain to another opcode read" -- but ROM
 * 0x01C978-0x01C986 is actually an RLE FILL (`move.w (a4)+,d6 ; bpl ..
 * ; move.w (a4)+,d0 ; move.w d0,(a3)+ ; dbra d6,$1c980` -- the value is
 * read ONCE and the dbra loop re-executes only the WRITE, not the read),
 * repeated for further (count,value) pairs until a negative count. It
 * happened to produce byte-identical output to the real ROM behaviour
 * for the one call site this project has found (attract_tilemap_init,
 * cmd table at ROM 0x1C0E4) purely because every block there uses
 * opcode 1 (plain literal copy), which this shadow DID implement
 * correctly -- so removing it changes nothing observable here, but
 * leaves a materially more correct implementation in place for any
 * other caller or ROM data that exercises opcode 0. */

/* Transpiled state handlers */
extern void boot_hardware_init(void);
extern uint64_t attract_tilemap_init(void);
extern void video_mixer_init(void);
extern void state_attract_init(void);
extern uint16_t state_attract_run(void);
extern void state_gameplay_init(void);
extern void state_gameplay_run(void);
extern void state_stage_start_init(void);
extern void state_stage_start_run(void);
extern void state_title_init(void);
extern void state_title_run(void);
extern void state_test_mode_init(void);
extern void state_test_mode_run(void);
extern int  state_ranking_init(void);
extern void state_ranking_run(void);
extern void state_ending_init(void);
extern void state_ending_run(void);
extern void state_bonus_init(void);
extern void state_bonus_run(void);

/* set_background_color @ 0x21AC2 */
static void set_background_color(uint8_t r, uint8_t g, uint8_t b) {
    /* Use fog color as background when no 3D is rendering */
    g_sys.videomix[0x05] = r;
    g_sys.videomix[0x06] = g;
    g_sys.videomix[0x07] = b;
}

/* State handlers are now in game_ported.c / game_deps.c (transpiled) */

/* ========== Game Init (entry_reset) ========== */

void game_init(void) {
    if (propcycl_verbose()) printf("  syscon_init...\n");
    syscon_init();

    if (propcycl_verbose()) printf("  clear_work_ram...\n");
    clear_work_ram();

    /* Set free play mode so the game can start without coins.
     * Normally loaded by eeprom_settings_init which is stubbed. */
    /* 0xE03FF4 (free play) and 0xE03FF6 (coins to CONTINUE) are two 16-bit
     * settings: settings_time_limit_defaults @ROM writes them `move.w`, and
     * every reader tests them `.w`. This used to be WRAM_SET32(0x3FF4, 1),
     * which put the 1 in 0x3FF6 and left free play itself at 0 -- invisible
     * while every reader took the whole _W slot, and the reason the continue
     * screen asked for "1 more coin" (register row 168).
     * PROPCYCL_COINPLAY=1 boots with the ROM's own coin-play default instead
     * (free play 0, continue coins BE16 @0x3673E = 2), which is what MAME's
     * pinned NVRAM holds -- use it to compare the coin screens like for like. */
    WRAM_SET16(0x3FF4, getenv("PROPCYCL_COINPLAY") ? 0 : 1);
    WRAM_SET16(0x3FF6, (int)vrd16(0x3673E));
    WRAM_SET32(0x3FF0, 4);  /* time_limit = 4 (default) */
    /* The rest of the factory settings block. eeprom_settings_init is
     * stubbed, and this is the ROM's own "blank EEPROM" path for these four
     * (settings_ranking_defaults, reads made BE16). W[0x3FFA] is the
     * play-time index (default 1 -> 80 s per level, register row 100);
     * 0x3FFA and 0x3FFE are 16-bit values at 2-mod-4 offsets, so they are
     * pinned like W[0x15FE2]. */
    { extern void settings_ranking_defaults(void); settings_ranking_defaults(); }
    /* ...and 0x3FF8, the CONTINUE-timer index (ROM 0x01AC44 `move.w $2a(a1),
     * $c8(a0)` = BE16 @0x3681E = 1 -> 900 frames, which is what MAME's
     * continue screen counts), into the backing store: it is the HIGH half of
     * an unpinned 4-aligned slot, so the W16_SET above is overwritten by the
     * first sync_wram_to_W and the screen ran 1200 frames (index 0). */
    WRAM_SET16(0x3FF8, (int)vrd16(0x3681E));
    /* THE HIGH-SCORE TABLES, by the same blank-EEPROM path: ROM 0x03179A
     * (highscore_table_reset_defaults) fills the top-ten list at 0xE04030
     * and the per-course bests at 0xE04088. Nothing called it, so the attract
     * RANKING screen read ten zero entries. MAME's pinned NVRAM holds exactly
     * these defaults (EGA 1700 .. TKP 800, bests 2000 "111"), so this is the
     * state the machine boots into. The C keeps the name and link BYTES one
     * per `_W[]` slot, and 1/2/3-mod-4 slots are either never written back or
     * rebuilt from neighbours by the sync, so the whole 0x79-byte EEPROM
     * block is pinned. */
    { extern void highscore_table_reset_defaults(void); highscore_table_reset_defaults(); }
    wsync_pin(0x4030, 0x7C);  /* high-score tables 0xE04030..0xE040AB */
    /* ...and the TODAY'S tables, their heads and the power-on copies of the
     * EEPROM block (0xE15BF0..0xE15E93), kept the same way. */
    wsync_pin(0x15BF0, 0x2A4);
    /* W[0x0CA0] -- the DSP BUFFER INDEX irq_vblank flips every frame. It
     * writes _W[] only, and sync_wram_to_W (two dozen lines later in
     * game_frame) rebuilt the slot from work RAM, which still held the OLD
     * index: the list cursor W[0x0CA4] survived (a host pointer, which the
     * sync never overwrites) and moved to the new buffer, while the index
     * snapped back. So every frame's list went into one buffer and everything
     * indexed by W[0x0CA0] -- dsp_viewport_setup, the cut-scene camera node's
     * zoom -- into the OTHER buffer's viewport blocks, which the renderer
     * never reads. Every story cut-scene was drawn at the default 30 deg
     * where the camera asks for 22.5 (register row 183). */
    wsync_pin(0x17278, 3);
    wsync_pin(0x1695E, 4);   /* the audit stats day bytes, one per slot (row 189) */   /* the name-entry record's three name bytes (story-merge) */
    if (!getenv("PROPCYCL_BUF_LEGACY")) wsync_pin(0x0CA0, 4);   /* =1 reverts, for A/B */
    /* THE SAVED RANKING. The machine keeps this block in EEPROM; here it is
     * a file (propcycl_scores.nv, see hiscore_load). Then the power-on reset
     * of the TODAY'S tables, which entry_reset calls last (ROM 0x00BD64) and
     * this hand-written boot never did. */
    { extern void hiscore_load(void); hiscore_load(); }
    { extern int object_display_init(void); object_display_init(); }
    wsync_pin(0x3FFA, 2);   /* W[0x3FFA] play-time setting index */
    wsync_pin(0x3FFE, 2);   /* W[0x3FFE] ranking enable */

    /* Credit counters + the credit-display countdown. Also never called;
     * the counters happened to read 0 from clear_work_ram(), but
     * g_credit_countdown (W[0x2C14]) is meant to start at 0x16. */
    coin_credit_init();
    /* ...and into the backing store, or the next sync_wram_to_W reads the
     * cleared work_ram back over it. W[0x2C14] is not the "countdown" its
     * Ghidra name suggests -- it is the tilemap ROW the credit banner is
     * drawn at (`text_draw_rect_blink(0x19, 0xb, W[0x2C14], 0x360, 3)`), and
     * 0x16 = 22 is exactly the row MAME's tilemap carries it on. Left at 0
     * the banner was drawn off the top of the screen. */
    WRAM_SET32(0x2C14, 0x16);

    /* Credits and the coin-armed flag are 16-bit game variables at 2-mod-4
     * offsets, which sync_wram_to_W would rebuild from their neighbours'
     * bytes every frame -- see the pin rationale in game_stubs.c. Without
     * these two the credit never survives to the next frame. */
    wsync_pin(0x2C0E, 2);   /* W[0x2C0E] credit count */
    wsync_pin(0x2C12, 2);   /* W[0x2C12] coin armed   */

    /* ANIMATION-READY FLAG, same class. `W[0xEB06]` is written
     * `(uint16_t)(...)` -- 0 or 1 -- and camera_update_main branches the
     * whole rider rig on it:
     *     EB06 == 0            -> player_animation_state_update()
     *     EB06 != 0, state 3   -> player_animation_keyframe_update()
     *     EB06 != 0, otherwise -> camera_setup_simple()   (live IK)
     * At a 2-mod-4 offset it never survived a frame: measured, it read
     * back 65535 in gameplay and 131071 in the flyover, so the "!= 0"
     * arm was taken by accident rather than by the game's intent. */
    wsync_pin(0xEB06, 2);   /* W[0xEB06] animation-ready flag */
    /* W[0x15FE2] is the course's TARGET SCORE, and it sits at a 2-mod-4
     * offset -- the case where sync_W_to_wram writes back only 4-aligned
     * slots while sync_wram_to_W reads BOTH, so the slot is rebuilt every
     * frame from its neighbours' bytes and cannot persist. Without the pin
     * balloon_system_init loads 2100 and the next frame reads 0, which puts
     * the stage-end test `W[0x15FE2] <= W[0x0E4C]` back to 0 <= 0 -- i.e.
     * instant goal, the exact defect unstubbing that function fixed. */
    wsync_pin(0x15FE2, 2);  /* W[0x15FE2] per-course target score */

    /* THE POINT-GAUGE LAMP's frame counter, the BYTE at 0xE15ED6
     * (FUN_0000e528, ROM 0x00E59C `tst.b (a2) ; addq.b #1,(a2)`). A 2-mod-4
     * slot, so without the pin the sync rebuilt it every frame from the
     * neighbouring bytes -- the low half of 0x15ED4 and the ROUTE HEADING
     * word at 0x15ED8 -- and the lamp showed frame (heading+1)&15: it
     * flickered through its three tiles all the time instead of resting on
     * frame 0 and playing only while the POINT reel rolls after a score. */
    wsync_pin(0x15ED6, 2);

    /* THE TERRAIN TILE->FACE MASK, W[0x0014..0x0313] (register row 106).
     *
     * A 256-BYTE array per layer, 3 layers (`0x0014 + layer*0x100 + row*16 +
     * col`, ROM 0x00461E `move.b ($e00014,A0,D6.l),D4`), and the `_W[]` model
     * stores it one slot per byte -- so most of it sits at non-4-aligned
     * offsets and `sync_wram_to_W` rebuilds it from neighbouring bytes every
     * frame. Measured against the machine (`tools/overnight/probe_mask.lua`
     * vs `PROPCYCL_MASKDUMP`) at cell 19: every column with `col % 4 == 2`
     * read back **0** where MAME holds the real face, 17 of the 48 rows
     * differing. The seeded tile in that cell (layer 0, row 12, col 2) is
     * face 0x1E = 30 on the machine and read 0 for us.
     *
     * `world_stage_render` seeds `terrain_cell_find_triangle`'s START FACE out
     * of this array, so a zeroed entry sent the whole neighbour walk off to a
     * wrong -- and in cell 19 an invalid, 0xFD-prefixed -- face record. That
     * is the hole in the collision surface: the walk never converged, stored a
     * degenerate triangle, resolved a height of -101234, lost to the -8191
     * bottom sentinel, and poisoned the per-layer history ring so
     * `terrain_cell_point_test` failed on the following frames too. Both
     * symptoms came from it -- the invisible wall (the look-ahead probe reads
     * "no ground" and runs the escape search) and the player's shadow jumping
     * 57,000 units (it is drawn from these same resolved probe heights). */
    wsync_pin(0x0014, 0x300);   /* terrain tile->face mask, 3 layers x 256 bytes */

    /* THE SCREEN FADE AND FOG COLOUR. All three are 16-bit registers -- the
     * M68K writes them `move.w #$ff, $e0eb1a.l` at 0x00AFE8/AFF0/AFF8 and
     * `clr.w $e0eb16.l` at 0x00B000 -- and 0xEB16, 0xEB1A and 0xEB1E all sit
     * at 2-mod-4 offsets, so the sync rebuilt them from their neighbours
     * every frame. Measured on the attract logo screen: `g_fog_r` read back
     * **196608 = 3 << 16**, which is `g_fog_mode` (W[0xEB18] = 3) bleeding in
     * from the slot below, and the videomix fog bytes came out 0,0,0 where
     * MAME's captured mix_f391 has 255,255,255. Black fog instead of white
     * darkened the whole frame: our logo-screen background was RGB(24,87,245)
     * against MAME's (69,149,250), and the darkening got worse the darker the
     * source pixel (247->251 unchanged, but 41->95). Same class as W[0xEB06]
     * and W[0x15FE2] above. W[0xEB18] and W[0xEB20] are 4-aligned and fine. */
    wsync_pin(0x1234, 0x80);    /* per-chunk visibility bytes, one slot per byte (row 160) */
    wsync_pin(0xEB16, 2);   /* W[0xEB16] screen fade level */
    wsync_pin(0xEB1A, 2);   /* W[0xEB1A] fog red   */
    wsync_pin(0xEB1E, 2);   /* W[0xEB1E] fog blue  */

    /* ...AND THE RING THOSE VALUES ARE PUBLISHED THROUGH, which the pin above
     * missed -- so the fade LEVEL was correct all along and never reached a
     * pixel. `palette_update` (ROM 0x022AC0) does not write the mixer from
     * W[0xEB16] directly; it writes it from a THREE-DEEP RING and advances the
     * index each frame, which is the hardware's own pipeline delay:
     *
     *     videomix[0x19] = W[0xEB22 + W[0xEB5E]]      <- what fog_hw.c reads
     *     ...
     *     W[0xEB22 + W[0xEB5E]] = W[0xEB16];          <- published for later
     *     W[0xEB5E] = (W[0xEB5E] + 1) % 3;
     *
     * Six 3-BYTE arrays on a 10-byte stride (0xEB22 factor, 0xEB2C fog mode,
     * 0xEB36/0xEB40/0xEB4A fog RGB, 0xEB54 alpha) plus the index at 0xEB5E.
     * TWO OF EVERY THREE BYTES land on a non-4-aligned offset, and the index
     * itself is 2-mod-4 -- so the index could never advance past 0 and the one
     * slot it did select was rebuilt from its neighbours every frame.
     *
     * Measured on the ADVANCED screen (sub 23), where the machine fades the
     * whole mode-select scene to black behind "DAY1": our W[0xEB16] ramps
     * 139 -> 19 -> 12 (MAME reads 18 at its settled frame, so the LEVEL was
     * already right), while `idx` stayed 0, the ring stayed 0,0,0 and
     * videomix[0x16..0x19] stayed 0,0,0,0 -- a screen_fade_factor of 0, i.e.
     * no fade at all. That is why the menus showed through behind DAY1, and
     * the same reason the attract logo hard-cuts instead of fading.
     * `PROPCYCL_FADELOG=1` prints that whole chain. */
    wsync_pin(0xEB22, 0x3E);  /* the palette_update publish ring + its index */

    /* SPRITE DISPLAY LIST: 4 buffers of 0x1800 at 0x4AF0..0xAAF0.
     *
     * Every link field in it sits at a 2-mod-4 offset -- the list head is
     * `0x4AF0 + buf*0x1800 + 0x16` and each record's next/prev are
     * `entry + 0x16` / `entry + 0x14` on a 0x18 stride -- so the sync
     * rebuilt them from neighbouring bytes every frame and the head read
     * back as -1. sprite_update then walked a garbage list and emitted
     * nothing: spr=0 live against spr=27170 in the recording, i.e. no
     * namco logo, no game logo, no "INSERT COIN".
     *
     * The whole region is game-owned (hand-written code only touches
     * g_sys.spriteram, which sprite_update writes), so pin it entire
     * rather than chase individual link slots. */
    wsync_pin(0x4AF0, 0x6000);

    /* THE MODE-SELECT / STAGE-SELECT INDEX. `gameplay_sub12_bonus_check_init`
     * @0x009AAE inits it with `clr.w $e00c82` and sub13 reads it with
     * `move.w (a2),$e00e10` @0x009CA4, which the exit then tests
     * `tst.w $e00e10` @0x009D60 -- 0 picks NOVICE (-> sub 0, stage select),
     * non-zero picks ADVANCED (-> sub 0x16). 0x0C82 is a 2-mod-4 offset, so
     * every write to it was erased by the sync and the menu read a neighbour
     * byte: with the stick centred MAME selects NOVICE and we selected
     * ADVANCED, landing in sub 23 and never reaching gameplay. Same class as
     * W[0xEB06], W[0x15FE2] and the terrain mask above. */
    wsync_pin(0x0C82, 2);   /* W[0x0C82] menu selection index */

    if (propcycl_verbose()) printf("  dipswitch_init...\n");
    dipswitch_init();

    if (propcycl_verbose()) printf("  tilemap_attr_init...\n");
    tilemap_attr_init();

    /* Keycus + watchdog between each step (stubbed) */
    keycus_write_1();
    keycus_write_2();

    if (propcycl_verbose()) printf("  video_mixer_init...\n");
    /* The FULL mixer init, not just the registers. The M68K's
     * `video_mixer_init` @0x022816 is six calls:
     *     0x02283C video_mixer_regs_init
     *     0x022A68 fog_params_init
     *     0x022934 palette_clear      <- fills the GAMMA tables
     *     0x022972 palette_fog_init
     *     0x0229D8 cz_ram_init
     *     0x022A16 cz_attr_init
     * and game_system.c transcribes all six faithfully. This site called
     * only the first, so the mixer's gamma LUT at videomix[0x100..0x3FF]
     * was never filled; `fog_hw.c` then set have_gamma = 0 and screenshot.c
     * skipped the final gamma stage for every live frame. The picture was
     * too dark by gamma ~2 -- fitting MAME = ours^g against the captured
     * logo frame gave g = 0.52 at every sample. */
    video_mixer_init();

    keycus_write_1();
    keycus_write_2();

    if (propcycl_verbose()) printf("  dsp_init...\n");
    dsp_init();

    keycus_write_1();
    keycus_write_2();

    if (propcycl_verbose()) printf("  mcu_init...\n");
    mcu_init();

    keycus_write_1();
    keycus_write_2();

    if (propcycl_verbose()) printf("  globals_init...\n");
    globals_init();

    if (propcycl_verbose()) printf("  sprite_init...\n"); fflush(stdout);
    sprite_init();
    if (propcycl_verbose()) printf("  sprite_init done\n"); fflush(stdout);

    if (propcycl_verbose()) printf("  task_system_init...\n");
    task_system_init();

    /* Port cgram_load_tile_block: copy tile pixel data from ROM to CGRAM
     * Tile format: 16x16x4bpp = 128 bytes per tile
     * Descriptor table at ROM 0x1C4F40, pixel data at ROM 0x238504 */
    if (propcycl_verbose()) printf("  loading CGRAM tile blocks from ROM...\n");
    /* Unwritten CGRAM must read as the TRANSPARENT pen, not as pen 0.
     * These tiles are 4bpp with 0xF meaning transparent, so a byte of 0x00
     * is two OPAQUE pen-0 pixels -- a zero-filled tile draws as a solid
     * block of palette entry 0. The text layer references tiles this
     * loader never fills, which is exactly what the solid rectangles over
     * live gameplay were. 0xFF = two transparent pixels. */
    memset(g_sys.cgram, 0xFF, CGRAM_SIZE);
    {
        /* All blocks needed for title/attract/results screens */
        static const struct { int block; int dest; } blocks[] = {
            {0xB7, 0x20},   /* ASCII font at tile 32 (CRITICAL) */
            {0xF2, 0x120},  /* title graphics at tile 288 */
            {0x1C, 0x140},  /* title border at tile 320 */
            {0xD4, 0x1A0},  /* graphics at tile 416 */
            {0xE3, 0x1C0},  /* graphics at tile 448 */
            {0xA5, 0x170},  /* scene graphics at tile 368 */
            {0x105, 0x1C},  /* system tiles at tile 28 */
            {0x106, 0x40},  /* tiles at 64 */
            {0x107, 0x80},  /* tiles at 128 */
            {0x108, 0x2C0}, /* tiles at 704 */
            {0x110, 0x18},  /* tiles at 24 */
            {0x00, 0x120},  /* base graphics at 288 */
            {0xF5, 0x260},  /* results at 608 */
            {-1, -1}
        };

        for (int bi = 0; blocks[bi].block >= 0; bi++) {
            int block_id = blocks[bi].block;
            int dest_tile = blocks[bi].dest;

            uint32_t desc = 0x1C4F40 + block_id * 16;
            if (desc + 16 > ROM_SIZE) continue;

            int32_t data_idx = (g_sys.rom[desc]<<24)|(g_sys.rom[desc+1]<<16)|
                               (g_sys.rom[desc+2]<<8)|g_sys.rom[desc+3];
            int32_t w = (g_sys.rom[desc+4]<<24)|(g_sys.rom[desc+5]<<16)|
                        (g_sys.rom[desc+6]<<8)|g_sys.rom[desc+7];
            int32_t h = (g_sys.rom[desc+8]<<24)|(g_sys.rom[desc+9]<<16)|
                        (g_sys.rom[desc+10]<<8)|g_sys.rom[desc+11];

            int total = w * h;
            if (total <= 0 || total > 500) continue;

            /* Source: pixel data ROM base + data_index * 128 bytes per tile */
            /* But actually: pixel data base is at 0x238504, and each tile
             * is stored as 0x40 words = 0x80 bytes (128 bytes). Wait,
             * the decompiled code says puVar6 is offset by * 0x40 (words).
             * In the decompiled code: puVar6 = &DAT_00238504 + data_idx * 0x40
             * which is byte offset data_idx * 0x80 = data_idx * 128.
             * And inner loop copies 0x40 words (128 bytes) per tile. */
            uint32_t src = 0x238504 + data_idx * 128;

            for (int t = 0; t < total && src + 128 <= ROM_SIZE; t++) {
                uint32_t cg_off = (uint32_t)(dest_tile + t) * 128;
                if (cg_off + 128 <= CGRAM_SIZE) {
                    memcpy(g_sys.cgram + cg_off, g_sys.rom + src, 128);
                }
                src += 128;
            }
            if (propcycl_verbose()) printf("    block 0x%02X: %d tiles -> CGRAM tile %d\n",
                   block_id, total, dest_tile);
        }
    }

    /* Load builtin bitmap font into CGRAM tiles 32-127 */
    if (propcycl_verbose()) printf("  loading builtin font into CGRAM...\n");
    memcpy(g_sys.cgram + 32 * 128, builtin_font, sizeof(builtin_font));

    /* Load attract mode graphics (palette, sprite table) */
    if (propcycl_verbose()) printf("  attract_gfx_init...\n");
    attract_gfx_init();
    attract_tilemap_init();

    /* Fix palette base mismatch: attract_load_palette puts data at
     * palette_ram offset 0x7E00, but boot_hardware_init sets videomix[0x1B]=0x7F
     * which makes the renderer read from 0x7F00. Copy palette forward by 0x100. */
    memcpy(g_sys.palette_ram + 0x7F00, g_sys.palette_ram + 0x7E00, 0x100);
    memcpy(g_sys.palette_ram + 0xFF00, g_sys.palette_ram + 0xFE00, 0x100);
    memcpy(g_sys.palette_ram + 0x17F00, g_sys.palette_ram + 0x17E00, 0x100);

    /* Write title screen text using the loaded font.
     * The font is loaded at CGRAM tile 32+ (block 0xB7).
     * Text tile codes: ASCII value maps to tile number directly
     * since the font block dest starts at tile 0x20 (=32 = ASCII space).
     *
     * Palette banks determine text color:
     *   0 = default (grey), 1 = green, 2 = red, 3 = blue
     */
    if (propcycl_verbose()) printf("  writing title screen text...\n");
    {
        /* Helper: write text string to text RAM */
        /* Palette banks: 0=grey gradient, 1=red, 2=green, 3=blue
         * Color 0=brightest, 15=black. Font pixels use high values = dark.
         * For the text to show as colored ON black background, we need
         * to write to rows BELOW the colored bands (rows 0-27 are bands).
         * The band tiles use palettes 0-3. Text on black = visible.
         *
         * Actually on real hardware: the title draws text AFTER clearing
         * the bands, on a black background. The bands are just the
         * attract_tilemap_init border. Let's clear the center and write text.
         */
        /* Set up text palettes at BOTH 0x7E00 and 0x7F00 bases
         * (attract_enable_display uses 0x7E, boot_hardware_init uses 0x7F) */
        for (int base = 0x7E00; base <= 0x7F00; base += 0x100) {
            /* Bank 4 = white text */
            int pal_off = base + 4 * 16;
            for (int c = 1; c < 16; c++) {
                g_sys.palette_ram[0x00000 + pal_off + c] = 255;
                g_sys.palette_ram[0x08000 + pal_off + c] = 255;
                g_sys.palette_ram[0x10000 + pal_off + c] = 255;
            }
            /* Bank 5 = yellow */
            pal_off = base + 5 * 16;
            for (int c = 1; c < 16; c++) {
                g_sys.palette_ram[0x00000 + pal_off + c] = 255;
                g_sys.palette_ram[0x08000 + pal_off + c] = 255;
                g_sys.palette_ram[0x10000 + pal_off + c] = 0;
            }
            /* Bank 6 = cyan */
            pal_off = base + 6 * 16;
            for (int c = 1; c < 16; c++) {
                g_sys.palette_ram[0x00000 + pal_off + c] = 0;
                g_sys.palette_ram[0x08000 + pal_off + c] = 255;
                g_sys.palette_ram[0x10000 + pal_off + c] = 255;
            }
        }

        /* Clear center area (rows 4-28, cols 1-38) to blank */
        for (int r = 0; r < 30; r++) {
            for (int c = 0; c < 40; c++) {
                int off = (r * 64 + c) * 2;
                if (off + 1 < TEXTRAM_SIZE) {
                    g_sys.textram[off] = 0x03;
                    g_sys.textram[off+1] = 0xBF;
                }
            }
        }

        struct { int row; int col; int palette; const char* text; } texts[] = {
            {2,  13, 5, "PROP CYCLE"},
            {5,   7, 4, "NAMCO SYSTEM SUPER 22"},
            {10, 11, 4, "PRESS START BUTTON"},
            {14, 16, 4, "NEW GAME"},
            {15, 16, 4, "CONTINUE"},
            {18, 14, 6, "INSERT COIN"},
            {22, 15, 4, "CREDIT  0"},
            {-1, 0, 0, NULL}
        };

        for (int t = 0; texts[t].row >= 0; t++) {
            int row = texts[t].row;
            int col = texts[t].col;
            int pal = texts[t].palette;
            const char* s = texts[t].text;
            for (int i = 0; s[i]; i++) {
                int off = (row * 64 + col + i) * 2;
                if (off + 1 < TEXTRAM_SIZE) {
                    uint16_t tile = (pal << 12) | (s[i] & 0x3FF);
                    g_sys.textram[off]     = tile >> 8;
                    g_sys.textram[off + 1] = tile & 0xFF;
                }
            }
        }
    }

    /* Initialize 3D hardware (DSP, palettes, CZ depth tables). */
    if (propcycl_verbose()) printf("  boot_hardware_init (DSP/3D setup)...\n");
    sync_wram_to_W();
    boot_hardware_init();
    sync_W_to_wram();

    /* RESTORE THE MIXER FOG COLOUR that boot_hardware_init just destroyed.
     *
     * `video_mixer_regs_init` (ROM 0x022860) sets videomix[0x05..0x07] = 0xFF,
     * and `palette_test_init` (ROM 0x03F786) clears them -- both faithfully
     * transcribed. The difference is that in the REAL game palette_test_init
     * never runs on a live path: it is reached only through
     * `boot_hardware_init` @0x03DD3C, whose only caller is
     * `title_attract_init` @0x01BB3A, and that function has **no callers at
     * all** in the ROM -- no jsr, no jmp, no pointer reference. Our game_init
     * calls boot_hardware_init by hand (for the DSP and CZ tables), which
     * drags the palette-test clear in with it.
     *
     * Measured on the attract logo screen against MAME's captured mixer state
     * (dumps/attract_start/mix_f391.bin): MAME has fog 255,255,255 and we had
     * 0,0,0. Black fog instead of white darkened the whole frame -- our
     * background read RGB(24,87,245) against MAME's (69,149,250), and the
     * error grew as pixels got darker (247->251 unchanged, 41->95 halved).
     * Every other mixer register already matched exactly: poly_fade
     * 255,255,255, bg 0,0,0, screen_fade 0,0,0 factor 0, flags 0x3.
     *
     * Only the three fog bytes are restored, deliberately: boot_hardware_init
     * also sets videomix[0x1B] = 0x7F, which the palette copy just below
     * depends on. */
    g_sys.videomix[0x05] = 0xFF;
    g_sys.videomix[0x06] = 0xFF;
    g_sys.videomix[0x07] = 0xFF;

    /* ...AND THE CZ STATE, for the same reason. boot_hardware_init also runs
     * `cz_depth_table_init` (ROM 0x03F59A, reached only from 0x03DDEE inside
     * boot_hardware_init), which on the machine never executes. It leaves
     * the CZ window holding ROM 0x3FF06's fourth block (a DESCENDING table,
     * 0x1FFF 0x1FDF ...) and czattr[4] = 0x7555, where the machine's last CZ
     * writer at boot is video_mixer_init's cz_attr_init (ROM 0x022A16): all
     * attribute registers 0 and the window the ramp 0, 21, 42, ... MAME's
     * captured bank 0 at f1500..f4800 is exactly that ramp, and the live CZ
     * fog (fog_hw.c, enabled by palette_update's czattr[4] = 0x4444) is built
     * from this window. Re-running cz_attr_init here restores the machine's
     * state; nothing between the two calls reads it. */
    { extern void cz_attr_init(void); cz_attr_init(); }

    /* Copy attract palette to 0x7F00 base (boot_hardware_init sets
     * videomix[0x1B]=0x7F which reads palettes from 0x7F00, but
     * attract_load_palette loaded at 0x7E00) */
    memcpy(g_sys.palette_ram + 0x7F00, g_sys.palette_ram + 0x7E00, 0x100);
    memcpy(g_sys.palette_ram + 0xFF00, g_sys.palette_ram + 0xFE00, 0x100);
    memcpy(g_sys.palette_ram + 0x17F00, g_sys.palette_ram + 0x17E00, 0x100);

    /* Start in attract mode (state 0). The real game boots with attract
     * playlist position W[0x0CC8]=15 (opening cinematic flyover, t[15]=2);
     * advance then cycles (seq+1)%15 through logo/demo/highscore phases.
     * Verified against the MAME L3 trace (seq 15 -> 1 -> 2). The earlier
     * hand-init forced seq=1 (logo first) which scrambled the attract
     * program order. */
    WRAM_SET32(OFF_GAME_STATE, 0);
    WRAM_SET32(0x0CC8, 15);

    sync_wram_to_W();
    if (propcycl_verbose()) printf("  init complete, game state = %d (title screen)\n", WRAM32(OFF_GAME_STATE));
}

/* ========== Main Loop (one frame) ========== */

/* Game state dispatch table (from ROM 0x3512C) */
typedef void (*state_func_t)(void);

/* VBlank handler from transpiled code — sets up DSP command buffer pointer,
 * toggles double buffer, updates keycus. Must be called each frame. */
extern void irq_vblank(void);

/* Per-frame processing functions (from transpiled code).
 * Called in the main loop before/after the state dispatch. */
extern void process_dsp_results(void);
extern void task_system_tick(void);
extern void text_layer_update(void);
extern void palette_update(void);
extern void sound_deferred_tick(void);
extern void tilemap_post_update(void);
extern void sprite_dma_kick(void);

void game_frame(void) {
    /* Wait for vblank (already signaled by main loop) */
    if (!g_sys.vblank_pending) return;
    g_sys.vblank_pending = false;

    /* Run VBlank handler: initializes DSP command pointer (W[0x0CA4]),
     * toggles double buffer, updates sprite/VICS state */
    sync_wram_to_W();
    irq_vblank();
    sync_W_to_wram();

    /* Kick watchdog (no-op) */
    SYSCON(0x14) = 0;

    /* Signal DSP start */
    mem_write32(0xC00000, 1);

    /* Read MCU inputs (stub - input.c handles this) */
    keycus_write_1();

    /* Increment frame counter */
    uint32_t fc = WRAM32(OFF_FRAME_COUNTER) + 1;
    WRAM_SET32(OFF_FRAME_COUNTER, fc);

    /* L3 trace anchor (GUARDRAILS D4): the MAME-side script taps the same
     * counter write at 0xE00C98, so both sides snapshot at this instant. */
    trace_on_frame(fc);

    /* Toggle double buffer */
    uint32_t db = WRAM32(OFF_DOUBLE_BUF) ^ 1;
    WRAM_SET32(OFF_DOUBLE_BUF, db);

    /* === Sync hand-written state → transpiled state === */
    sync_wram_to_W();

    /* --- Per-frame processing (BEFORE state dispatch) ---
     * Original main loop order from annotations.md:
     *   process_dsp_results → read_mcu_inputs → task_system_tick →
     *   text_layer_update → sprite_update → palette_update →
     *   sound_deferred_tick → [state dispatch] → tilemap_post_update →
     *   sprite_dma_kick
     */
    /* (debug DSP pointer logging removed) */
    process_dsp_results();
    /* read_mcu_inputs: the input read the comment above documents but the
     * loop never made. Without it NOTHING the player does reaches the game
     * -- input_read_service_buttons() is what copies commsram[0x7D02/0x7D04]
     * into W[0x2B80]/W[0x2BA4] and derives the press edges, so coins,
     * start, service and test were all inert regardless of what input.c
     * wrote. */
    /* Call the reader DIRECTLY rather than read_mcu_inputs(), which also
     * runs dipswitch_init() / input_process_analog_deltas() and core dumps
     * during init -- almost certainly why the whole call was left out of
     * this loop in the first place.
     *
     * read_mcu_inputs()'s other two members stay out on purpose:
     *   input_decode_buttons()   -- core dumps. It decodes a 12-bit button
     *       matrix this cabinet does not have (`auStack_c[iVar4]` is a
     *       value dereferenced as a pointer; 0x2b3e/0x2b44... are W-space
     *       addresses used as host pointers). Its only output anything
     *       reads is W[0x2B3A] bit 0x100, an ALTERNATIVE start edge --
     *       the primary W[0x2BA6] bit 0 path below works without it.
     * read_mcu_inputs_process() USED to stay out for the same reason -- its
     * copy of the eight ADC words was byte-wise, so it put 0x01 where the
     * handlebar value belongs. That copy is now 16-bit at stride 2 as the ROM
     * has it (0x0222C2), and input.c publishes the live stick into the same
     * shared-RAM words, so it is safe to run -- and it must, because it is
     * the ONLY producer of the direction flags W[0x2BD8]/W[0x2BDC] that the
     * MODE SELECT and STAGE SELECT cursors read. Without it the stick could
     * not move between menu options at all. */
    if (getenv("PROPCYCL_NO_INPUTREAD") == NULL) input_read_service_buttons();
    { extern int g_menustick; extern void read_mcu_inputs_process(void);
      if (g_menustick) read_mcu_inputs_process(); }
    /* The PEDAL, and the reason the bike could never reach speed.
     * input_process_analog_deltas differences the MCU's free-running pulse
     * counters into W[0x2BFC] / W[0x2C04]; W[0x2C04] is what
     * player_update_prev_pos turns straight into thrust. Uncalled, that
     * count stays 0, thrust sits at its constant floor, and the bike tops
     * out around 4190 against the hardware's 13940 (register row 54).
     *
     * Called directly rather than through read_mcu_inputs() for the same
     * reason its sibling above is: that wrapper also runs dipswitch_init()
     * and input_decode_buttons(), which core dump. This one touches only
     * W[] and commsram. */
    input_process_analog_deltas();

    /* PROPCYCL_LOOPDBG=1: log every state / sub-state transition with the
     * frame it happened on. The game loop is the thing being brought up --
     * attract -> coin -> play -> results -> attract -- and without this the
     * only visible symptom is "the level ended early", with no idea which
     * transition fired or when. */
    { extern int g_loop_dbg;
      static long ps = -1, pu = -1;
      if (g_loop_dbg && (g_sys.frame_count % 60) == 0)
          printf("[LOOP]   f%-5u endflag=%ld dist(W15FE2)=%ld thresh(W0E4C)=%ld timer=%ld\n",
                 g_sys.frame_count, (long)_W[0x0E18], (long)_W[0x15FE2],
                 (long)_W[0x0E4C], (long)_W[0x0E44]);
      /* Results-screen phase machine (state 3, sub >= 6): the same fields
       * tools/overnight/snap_results.lua logs out of MAME, for a diff. */
      if (g_loop_dbg && _W[0x0CBC] == 3 && _W[0x0CC0] >= 6 && (g_sys.frame_count % 30) == 0)
          printf("[RES] f%u sub=%ld e18=%ld e44=%ld e4c=%ld e50=%ld p16978=%ld p16974=%ld p1697c=%ld p16720=%ld eb16=%ld\n",
                 g_sys.frame_count, (long)_W[0x0CC0], (long)_W[0x0E18], (long)_W[0x0E44], (long)_W[0x0E4C], (long)_W[0x0E50],
                 (long)_W[0x16978], (long)_W[0x16974], (long)_W[0x1697C], (long)_W[0x16720], (long)_W[0xEB16]);
      if (g_loop_dbg) {
          long st = (long)_W[0x0CBC], su = (long)_W[0x0CC0];
          if (st != ps || su != pu) {
              printf("[LOOP] f%-5u state %ld->%ld  sub %ld->%ld   timer=%ld "
                     "endflag=%ld dist=%ld thresh=%ld\n",
                     g_sys.frame_count, ps, st, pu, su, (long)_W[0x0E44],
                     (long)_W[0x0E18], (long)_W[0x15FE2], (long)_W[0x0E4C]);
              fflush(stdout);
              ps = st; pu = su;
          }
      } }
    task_system_tick();
    text_layer_update();
    { extern int g_cmd_dump;
      if (g_cmd_dump) { static int d; if (d++ % 60 == 0) {
        int i, nz = 0; for (i = 0; i < 0x2000; i++) if (g_sys.textram[i]) nz++;
        printf("[CMD] textram non-zero words: %d / 0x2000\n", nz); } } }
    /* sprite_update: converts the W[] sprite list that sprite_draw_2d
     * builds into g_sys.spriteram (what sprite_hw.c draws) and resets the
     * per-frame count. Defined but never called -- same defect as
     * coin_credit_update. Without it the count saturated at 0xFF and the
     * sprite layer drew nothing at all. Documented order
     * (annotations.md 0xBF2A): text_layer_update -> sprite_update ->
     * palette_update. */
    FUN_000231ec();
    palette_update();
    sound_deferred_tick();

    /*
     * Game state dispatch
     */
    /* check_game_state_change(): the TEST button (W[0x2B80] & 8) and the
     * service-menu exit force state 6. Documented in the main loop
     * (annotations.md, 0xBF2A) between the frame counter and the state
     * dispatch, and simply absent here -- so TEST did nothing. */
    if (getenv("PROPCYCL_NO_COINUPDATE") == NULL) check_game_state_change();

    uint32_t state = (uint32_t)_W[OFF_GAME_STATE];
    if (g_sys.frame_count < 5 || (g_sys.frame_count % 20 == 0))
        if (propcycl_verbose()) printf("    [DBG] frame=%d state=%d sub=%d fog=(%d,%d,%d) fade=%d"
               " cr=%d armed=%d in=%04x/%04x edge=%04x/%04x adc=(%d,%d)\n",
               g_sys.frame_count, (int)state,
               (int)(short)_W[0x0CC0],
               g_sys.videomix[0x05], g_sys.videomix[0x06], g_sys.videomix[0x07],
               g_sys.videomix[0x12],
               (int)_W[0x2C0E], (int)_W[0x2C12],
               (unsigned)(_W[0x2B80] & 0xffff), (unsigned)(_W[0x2BA4] & 0xffff),
               (unsigned)(_W[0x2B82] & 0xffff), (unsigned)(_W[0x2BA6] & 0xffff),
               (int)_W[0x2BC8], (int)_W[0x2BCA]);
    if (propcycl_verbose() && (g_sys.frame_count % 60 == 0))
        printf("    [CAM] cam=(%d,%d,%d) player=(%d,%d,%d) d=(%d,%d,%d) hdg=%d pitch=%d\n",
               (int)_W[0x0CDC], (int)_W[0x0CE0], (int)_W[0x0CE4],
               (int)_W[0x0D00], (int)_W[0x0D04], (int)_W[0x0D08],
               (int)(_W[0x0D00]-_W[0x0CDC]), (int)(_W[0x0D04]-_W[0x0CE0]),
               (int)(_W[0x0D08]-_W[0x0CE4]),
               (int)(_W[0x0CEC] & 0xFFFF), (int)(_W[0x0CE8] & 0xFFFF));
    switch (state) {
    case 0:  state_attract_init(); break;
    case 1:  state_attract_run(); break;
    case 2:  state_gameplay_init(); break;
    case 3:  state_gameplay_run(); break;
    case 4:  state_stage_start_init(); break;
    case 5:  state_stage_start_run(); break;
    case 6:  state_title_init(); break;
    case 7:  state_title_run(); break;
    case 8:  state_test_mode_init(); break;
    case 9:  state_test_mode_run(); break;
    case 10: state_ranking_init(); break;
    case 11: state_ranking_run(); break;
    case 12: state_ending_init(); break;
    case 13: state_ending_run(); break;
    case 14: state_bonus_init(); break;
    case 15: state_bonus_run(); break;
    }

    /* --- Post-dispatch processing --- */
    tilemap_post_update();

    /* coin_credit_update(): converts the coin/service EDGES that
     * input_read_service_buttons() puts in W[0x2B82] into the credit count
     * W[0x2C0E], and is also what turns a Start press into a game start
     * (W[0x0CBC] = 2) once W[0x2C12] is armed.
     *
     * It was defined but called from NOWHERE -- grep the tree, this was the
     * only reference. That is the whole reason player input looked dead:
     * coins reached commsram, input_read_service_buttons() copied them into
     * W[0x2B80]/W[0x2B82] with correct edges, and then nothing consumed
     * them, so credits stayed 0 and the Start gate in state_title_run
     * (game_title.c, `W[0x2C0E] > 0`) never opened.
     * Main loop position per annotations.md 0xBF2A: after the state
     * dispatch, before sprite_dma_kick. */
    if (getenv("PROPCYCL_NO_COINUPDATE") == NULL) coin_credit_update();

    sprite_dma_kick();

    /* LIVE 2D FEED DUMP (PROPCYCL_FEEDDUMP=<dir>:<frame>).
     *
     * sprite_hw.c and text_hw.c reproduce a real gameplay capture
     * pixel-for-pixel (26818/26818 and 1647/1647), so the missing
     * "INSERT 4 COIN(S)" and the garbage HUD tiles are in what the GAME
     * writes into spriteram/cgram, not in what draws it. Writing our own
     * buffers in the capture's layout is what makes that comparable:
     *   sprite_f<N>.bin  g_sys.spriteram              (0x30000)
     *   cgram_f<N>.bin   g_sys.cgram then g_sys.textram (0x1E000+0x2000)
     *   vicsd_f<N>.bin   g_sys.vics_data              (0x10000) */
    { extern char g_feed_dir[]; extern int g_feed_frame;
      if (g_feed_dir[0] && (int)g_sys.frame_count == g_feed_frame) {
          char path[512]; FILE *f;
          snprintf(path, sizeof path, "%s/sprite_f%d.bin", g_feed_dir, g_feed_frame);
          if ((f = fopen(path, "wb"))) { fwrite(g_sys.spriteram, 1, SPRITERAM_SIZE, f); fclose(f); }
          snprintf(path, sizeof path, "%s/cgram_f%d.bin", g_feed_dir, g_feed_frame);
          if ((f = fopen(path, "wb"))) { fwrite(g_sys.cgram, 1, CGRAM_SIZE, f);
                                         fwrite(g_sys.textram, 1, TEXTRAM_SIZE, f); fclose(f); }
          snprintf(path, sizeof path, "%s/vicsd_f%d.bin", g_feed_dir, g_feed_frame);
          if ((f = fopen(path, "wb"))) { fwrite(g_sys.vics_data, 1, VICS_DATA_SIZE, f); fclose(f); }
          /* the palette too -- the sprite and text models index it directly,
           * so a tile that is byte-exact in cgram can still draw the wrong
           * colour, which is what the tutorial's red arrow turned out to be. */
          snprintf(path, sizeof path, "%s/pal_f%d.bin", g_feed_dir, g_feed_frame);
          if ((f = fopen(path, "wb"))) { fwrite(g_sys.palette_ram, 1, PALETTE_SIZE, f); fclose(f); }
          printf("[FEED] dumped live 2D feed at frame %d -> %s\n", g_feed_frame, g_feed_dir);
      } }

    /* === Sync transpiled state → hand-written state (for renderers) === */
    sync_W_to_wram();

    /* Clear DSP frame */
    mem_write32(0xC00000, 0);
}

/* Debug: dump tilemap to PPM image file */
static void debug_dump_tilemap(const char* filename) {
    FILE* f = fopen(filename, "wb");
    if (!f) return;
    /* Text layer: 16x16x4bpp tiles, 64x32 tile grid = 1024x512 pixels */
    int img_w = 1024, img_h = 512;
    fprintf(f, "P6\n%d %d\n255\n", img_w, img_h);

    for (int y = 0; y < img_h; y++) {
        for (int x = 0; x < img_w; x++) {
            /* Tile position: 16x16 pixel tiles */
            int tx = x / 16, ty = y / 16;
            int px = x % 16, py = y % 16;

            /* Read tile entry from text RAM (64 tiles per row) */
            int ram_off = (ty * 64 + tx) * 2;
            uint16_t entry = 0x03BF;
            if (ram_off + 1 < TEXTRAM_SIZE) {
                entry = (g_sys.textram[ram_off] << 8) | g_sys.textram[ram_off + 1];
            }

            int palette_bank = (entry >> 12) & 0xF;
            int flip_y = (entry >> 11) & 1;
            int flip_x = (entry >> 10) & 1;
            int tile_num = entry & 0x3FF;

            int fpx = flip_x ? (15 - px) : px;
            int fpy = flip_y ? (15 - py) : py;
            int cg_off = tile_num * 128 + fpy * 8 + fpx / 2;

            uint8_t pix_val = 0xF;
            if (cg_off >= 0 && cg_off < CGRAM_SIZE) {
                uint8_t byte_val = g_sys.cgram[cg_off];
                pix_val = (fpx & 1) ? (byte_val & 0x0F) : (byte_val >> 4);
            }

            uint8_t r = 0, gr = 0, b = 0;
            if (pix_val < 0xF) {  /* 0xF = transparent background */
                int pal_base = (g_sys.videomix[0x1B] & 0x7F) << 8;
                int pal_idx = pal_base + palette_bank * 16 + pix_val;
                if (pal_idx < 0x8000) {
                    r  = g_sys.palette_ram[0x00000 + pal_idx];
                    gr = g_sys.palette_ram[0x08000 + pal_idx];
                    b  = g_sys.palette_ram[0x10000 + pal_idx];
                }
            }

            fputc(r, f); fputc(gr, f); fputc(b, f);
        }
    }
    fclose(f);
    printf("Dumped tilemap to %s\n", filename);
}

/* Debug: check what's in CGRAM */
static void debug_check_cgram(void) {
    /* Tiles are 16x16x4bpp = 128 bytes each. Max = CGRAM_SIZE/128 */
    int max_tiles = CGRAM_SIZE / 128;
    int nonzero_tiles = 0;
    for (int t = 0; t < max_tiles; t++) {
        int has_data = 0;
        for (int b = 0; b < 128; b++) {
            if (g_sys.cgram[t * 128 + b] != 0) { has_data = 1; break; }
        }
        if (has_data) nonzero_tiles++;
    }
    printf("  CGRAM: %d / %d tiles (16x16x4bpp) have data\n", nonzero_tiles, max_tiles);

    /* Check palette planes */
    int pal_nonzero = 0;
    for (int i = 0; i < 256; i++) {
        if (g_sys.palette_ram[i] || g_sys.palette_ram[0x8000+i] || g_sys.palette_ram[0x10000+i])
            pal_nonzero++;
    }
    printf("  Palette bank 0: %d / 256 colors set\n", pal_nonzero);

    /* Check what tile codes text RAM references */
    int min_tile = 9999, max_tile = 0;
    for (int i = 0; i < TEXTRAM_SIZE; i += 2) {
        uint16_t entry = (g_sys.textram[i] << 8) | g_sys.textram[i+1];
        int code = entry & 0x3FF;
        if (entry != 0x03BF) {
            if (code < min_tile) min_tile = code;
            if (code > max_tile) max_tile = code;
        }
    }
    printf("  Text RAM references tiles %d - %d\n", min_tile, max_tile);
    printf("  Tile %d CGRAM data (128-byte): ", min_tile);
    for (int b = 0; b < 16; b++) printf("%02X ", g_sys.cgram[min_tile * 128 + b]);
    printf("\n");
    /* Also check loaded tiles */
    printf("  Tile 288 CGRAM data: ");
    for (int b = 0; b < 16; b++) printf("%02X ", g_sys.cgram[288 * 128 + b]);
    printf("\n");
}
int g_loop_dbg = 0;
