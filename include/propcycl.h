/*
 * Prop Cycle (Namco System Super 22) - Reimplementation
 * Master header
 */
#ifndef PROPCYCL_H
#define PROPCYCL_H

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>

/* ========== Memory Map ========== */

/* ROM: loaded from interleaved files */
#define ROM_SIZE        0x400000    /* 4 MB */

/* Work RAM */
#define WORK_RAM_BASE   0xE00000
#define WORK_RAM_SIZE   0x040000    /* 256 KB */

/* Palette RAM */
#define PALETTE_BASE    0x828000
#define PALETTE_SIZE    0x018000    /* 96 KB: 3 planes of 0x8000 (R @ +0, G @ +0x8000, B @ +0x10000) */

/* CGRAM (character graphics / tile patterns) */
#define CGRAM_BASE      0x880000
#define CGRAM_SIZE      0x01E000

/* Text RAM (tilemap) */
#define TEXTRAM_BASE    0x89E000
#define TEXTRAM_SIZE    0x002000

/* Sprite RAM (C374) */
#define SPRITERAM_BASE  0x980000
#define SPRITERAM_SIZE  0x030000

/* VICS */
#define VICS_DATA_BASE  0x900000
#define VICS_DATA_SIZE  0x010000
#define VICS_CTRL_BASE  0x940000
#define VICS_CTRL_SIZE  0x000080

/* DSP / Polygon RAM */
#define DSPRAM_BASE     0xC00000
#define DSPRAM_SIZE     0x020000

/* Comms RAM (shared with MCU/DSP) */
#define COMMSRAM_BASE   0xA04000
#define COMMSRAM_SIZE   0x008000

/* Point RAM */
#define POINTRAM_BASE   0xF80000
#define POINTRAM_SIZE   0x020000

/* CZ RAM */
#define CZRAM_BASE      0x810000
#define CZRAM_SIZE      0x000400

/* Video mixer */
#define VIDEOMIX_BASE   0x824000
#define VIDEOMIX_SIZE   0x000400

/* Tilemap attributes */
#define TILEMAPATTR_BASE 0x8A0000
#define TILEMAPATTR_SIZE 0x000010

/* EEPROM */
#define EEPROM_BASE     0x460000
#define EEPROM_SIZE     0x004000

/* ========== Hardware Register Addresses ========== */

/* SYSCON (0x700000) */
#define SYSCON_BASE         0x700000
#define SYSCON_VBLANK_LVL   0x700000
#define SYSCON_HBLANK_LVL   0x700001
#define SYSCON_SCI_LVL      0x700002
#define SYSCON_UNK_LVL      0x700003
#define SYSCON_VBLANK_ACK   0x700004
#define SYSCON_HBLANK_ACK   0x700005
#define SYSCON_SCI_ACK      0x700006
#define SYSCON_UNK_ACK      0x700007
#define SYSCON_WATCHDOG     0x700014
#define SYSCON_MCU_RESET    0x700016
#define SYSCON_DSP_CTRL     0x70001C

/* I/O */
#define KEYCUS_BASE         0x400000
#define CHIPSEL_REG         0x800000
#define DIPSWITCH_REG       0x440000
#define CPULEDS_REG         0x430000
#define PORTBIT_REG         0x450008

/* ========== Global State ========== */

typedef struct {
    /* ROM (read-only after load) */
    uint8_t rom[ROM_SIZE];

    /* RAM regions */
    uint8_t work_ram[WORK_RAM_SIZE];
    uint8_t palette_ram[PALETTE_SIZE];
    uint8_t cgram[CGRAM_SIZE];
    uint8_t textram[TEXTRAM_SIZE];
    uint8_t spriteram[SPRITERAM_SIZE];
    uint8_t vics_data[VICS_DATA_SIZE];
    uint8_t vics_ctrl[VICS_CTRL_SIZE];
    uint8_t dspram[DSPRAM_SIZE];
    uint8_t commsram[COMMSRAM_SIZE];
    uint8_t pointram[POINTRAM_SIZE];
    uint8_t czram[CZRAM_SIZE];
    uint8_t videomix[VIDEOMIX_SIZE];
    uint8_t tilemapattr[TILEMAPATTR_SIZE];
    uint8_t eeprom[EEPROM_SIZE];

    /* SYSCON registers */
    uint8_t syscon[0x20];

    /* Keycus state */
    uint16_t keycus_rng;

    /* Chipselect */
    uint32_t chipselect;

    /* Frame state */
    bool vblank_pending;
    uint32_t frame_count;
} SystemState;

extern SystemState g_sys;

/* DSP RAM holds HOST-NATIVE 32-bit words (the display list is written that
 * way and the renderer reads it that way). The master's per-viewport blocks
 * sit at byte 0x10000 + buf*0x8000 + vp*0x80 -- 0xC10000 on the board, ROM
 * 0x022D3A -- with the camera trig pairs at +0x04.., the zoom word at +0x38
 * (half field of view in 1/64 degree: 0x780 = 30 deg = focal 554.25) and the
 * absolute priority at +0x50. Stores into it must be 32-bit at that stride;
 * the transpile's `g_sys.dspram[C + (buf*0x2000 + vp*0x20)] = v` is a BYTE
 * store at a quarter of it (Ghidra's int* index taken as a byte index). */
static inline void dsp_w32(uint32_t off, int32_t v)
{
    if (off + 3 < DSPRAM_SIZE) *(int32_t *)&g_sys.dspram[off & ~3u] = v;
}
static inline int32_t dsp_r32(uint32_t off)
{
    return (off + 3 < DSPRAM_SIZE) ? *(const int32_t *)&g_sys.dspram[off & ~3u] : 0;
}

/* ========== Memory Access (HAL) ========== */

uint8_t  mem_read8(uint32_t addr);
uint16_t mem_read16(uint32_t addr);
uint32_t mem_read32(uint32_t addr);
void     mem_write8(uint32_t addr, uint8_t val);
void     mem_write16(uint32_t addr, uint16_t val);
void     mem_write32(uint32_t addr, uint32_t val);

/* Direct RAM access (for game logic that uses pointers) */
void*    mem_ptr(uint32_t addr);

/* Unified M68K virtual address type + safe read/write helpers.
 * Use vaddr_t in any function signature that carries an address —
 * never short/int/char — to block the 64-bit-truncation bug class
 * from the transpiled Ghidra output. See include/vaddr.h. */
#include "vaddr.h"

/* Transitional native-byte-order ROM helpers (rom_nr / rom_nw family).
 * Used by the mechanical rewrite in tools/rewrite_ghidra.py to replace
 * raw T-pointer derefs off &R[addr] with bounds-checked calls while
 * preserving the original native-byte-order semantics. Each call site
 * is a candidate for eventual migration to vrd (big-endian) after
 * verifying the downstream consumer. See include/rom_native.h. */
#include "rom_native.h"

/* ========== Asset Data (loaded from ROMs) ========== */

/* Point ROM: 3-plane signed 24-bit, interleaved to int32 array.
 * 3 chips per plane × 512KB each = 1.5M entries total. */
#define POINTROM_SIZE   (512 * 1024 * 3)  /* 1572864 entries */
extern int32_t  g_pointrom[POINTROM_SIZE];
extern uint32_t g_pointrom_count;

/* Texture tiles: 8 x 2MB = 16MB, 16x16x8bpp = 256 bytes per tile */
#define TEXTURE_TILE_SIZE   256
#define TEXTURE_TOTAL_SIZE  (0x200000 * 8)  /* 16 MB */
extern uint8_t* g_texture_data;             /* malloc'd 16MB */

/* Texture tilemap: UV → tile index (2.5MB, 16-bit LE entries) */
#define TEXTUREMAP_SIZE (0x280000)
extern uint8_t* g_texture_tilemap;          /* malloc'd 2.5MB */

/* Sprite tiles: 32x32x8bpp = 1024 bytes per tile, 4MB total */
#define SPRITE_TILE_SIZE    1024
#define SPRITE_TOTAL_SIZE   (0x200000 * 2)  /* 4 MB */
extern uint8_t* g_sprite_tiles;             /* malloc'd 4MB */

/* ========== ROM Loader ========== */

bool rom_load_all(const char* dir);

/* ========== Renderer ========== */

/* 2D */
void renderer2d_init(void);
void renderer2d_draw_tilemap(void);
void renderer2d_draw_sprites(void);
void renderer2d_composite(void);

/* 3D */
void renderer3d_init(void);
int  renderer3d_live_2d_ok(void);  /* exact sprite/text stages have live state */
const uint8_t *renderer3d_planar_palette(void);  /* good palette, planar layout */
void renderer3d_load_palette(const char *rom_dir);  /* load palette from ROM/MAME dump */
void palette_mark_written(int pen);   /* a game palette writer touched this pen */
void renderer3d_process_dsp_commands(void);
void renderer3d_render_frame(void);
void renderer3d_set_viewer_model(int model_id);  /* model viewer mode */
void renderer3d_toggle_level_view(void);          /* full level view mode */
void renderer3d_move_camera(float dx, float dy, float dz);
extern int g_level_view_mode;

/* ========== Input ========== */

void input_init(void);
void input_poll(void);  /* maps SDL events → MCU shared RAM */

/* ========== Game Logic ========== */

void game_init(void);   /* calls entry_reset chain */
void game_frame(void);  /* one frame of main_loop */

/* ========== Work RAM Sync ========== */
/*
 * The project has two work RAM representations:
 *   g_sys.work_ram[] - uint8_t byte array, big-endian (used by hand-written code + renderers)
 *   _W[]             - int32_t per byte offset, native endian (used by transpiled code)
 *
 * These sync functions convert between them at frame boundaries.
 */
/* A sound id the decompiler lost at a call site. sound_play() plays
 * NOTHING for it -- an unprototyped `sound_play()` passes whatever is in
 * the register, and a random id names a real row of the sound table, which
 * the player hears as a sample retriggering endlessly. */
#define SND_ID_UNRECOVERED (-1)
/* PROPCYCL_SNDTRIG: log a sound request at the entry of a sound routine
 * (game_core.c), in the format tools/overnight/probe_sound_triggers.lua
 * prints from MAME. */
void snd_trig(const char *fn, int id, int par);
void snd_trig_contact(int kind, int p2, int spd);

extern intptr_t _W[];
/* Mark _W[] slots as authoritative: sync_wram_to_W will not clobber them. */
void wsync_pin(uint32_t off, uint32_t len);

/* ---- STUB REPORTING ------------------------------------------------------
 *
 * A stub that silently returns hides the gap it leaves. `STUB_HIT` makes one
 * fire ONCE, on stderr, with its ROM address, so a stub that is actually on a
 * live path announces itself instead of being discovered months later by its
 * symptom. Use it only where the ROM has REAL CODE: 22 of this tree's
 * stub-shaped functions are a bare `RTS` (0x4E75) in the retail game and are
 * correct as they stand -- reporting those would be pure noise.
 *
 * PROPCYCL_NO_STUBWARN=1 silences it. `PROPCYCL_STUBWARN=all` reports every
 * hit rather than the first.
 */
void stub_hit_report(const char *fn, unsigned rom_addr, const char *note);
#define STUB_HIT(rom_addr, note) \
    do { static int _stub_seen; extern int g_stub_warn_all; \
         if (!_stub_seen || g_stub_warn_all) { _stub_seen = 1; \
             stub_hit_report(__func__, (rom_addr), (note)); } } while (0)


/* Per-frame diagnostics are OFF unless PROPCYCL_VERBOSE=1. At 60 fps the
 * per-frame logs emit thousands of lines a minute and make the terminal
 * unusable -- and unreadable logs are worse than no logs. One-time and
 * error messages are always printed. */
int propcycl_verbose(void);
void sync_wram_to_W(void);   /* g_sys.work_ram → _W (call before transpiled code) */
void sync_W_to_wram(void);   /* _W → g_sys.work_ram (call after transpiled code) */

/* ========== Screen Constants ========== */
#define SCREEN_WIDTH    640
#define SCREEN_HEIGHT   480
#define GAME_WIDTH      640
#define GAME_HEIGHT     480

#endif /* PROPCYCL_H */
