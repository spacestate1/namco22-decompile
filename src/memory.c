/*
 * Memory map and bus emulation
 * Translates MC68EC020 addresses to backing arrays
 */
#include "propcycl.h"
#include "trace.h"

SystemState g_sys;

/* KEYCUS range (0x400000-0x40001F). With a MAME-recorded replay loaded
 * (GUARDRAILS D2), reads are intercepted at full access width — before
 * the byte decomposition below — so one game-level read consumes exactly
 * one recorded bus value, mirroring MAME's per-access read tap. */
static inline bool keycus_range(uint32_t addr) {
    return addr >= 0x400000 && addr < 0x400020;
}

/* Map an address to a pointer into our backing arrays */
static uint8_t* resolve_addr(uint32_t addr, uint32_t* offset) {
    /* ROM: 0x000000 - 0x3FFFFF */
    if (addr < 0x400000) {
        *offset = addr;
        return g_sys.rom;
    }
    /* Work RAM: 0xE00000 - 0xE3FFFF */
    if (addr >= 0xE00000 && addr < 0xE40000) {
        *offset = addr - 0xE00000;
        return g_sys.work_ram;
    }
    /* Palette RAM: 0x828000 - 0x83FFFF */
    if (addr >= 0x828000 && addr < 0x840000) {
        *offset = addr - 0x828000;
        return g_sys.palette_ram;
    }
    /* CGRAM: 0x880000 - 0x89DFFF */
    if (addr >= 0x880000 && addr < 0x89E000) {
        *offset = addr - 0x880000;
        return g_sys.cgram;
    }
    /* Text RAM: 0x89E000 - 0x89FFFF */
    if (addr >= 0x89E000 && addr < 0x8A0000) {
        *offset = addr - 0x89E000;
        return g_sys.textram;
    }
    /* Tilemap attrs: 0x8A0000 - 0x8A000F */
    if (addr >= 0x8A0000 && addr < 0x8A0010) {
        *offset = addr - 0x8A0000;
        return g_sys.tilemapattr;
    }
    /* Sprite RAM: 0x980000 - 0x9AFFFF */
    if (addr >= 0x980000 && addr < 0x9B0000) {
        *offset = addr - 0x980000;
        return g_sys.spriteram;
    }
    /* VICS data: 0x900000 - 0x90FFFF */
    if (addr >= 0x900000 && addr < 0x910000) {
        *offset = addr - 0x900000;
        return g_sys.vics_data;
    }
    /* VICS ctrl: 0x940000 - 0x94007F */
    if (addr >= 0x940000 && addr < 0x940080) {
        *offset = addr - 0x940000;
        return g_sys.vics_ctrl;
    }
    /* DSP/Polygon RAM: 0xC00000 - 0xC1FFFF */
    if (addr >= 0xC00000 && addr < 0xC20000) {
        *offset = addr - 0xC00000;
        return g_sys.dspram;
    }
    /* Comms RAM: 0xA04000 - 0xA0BFFF */
    if (addr >= 0xA04000 && addr < 0xA0C000) {
        *offset = addr - 0xA04000;
        return g_sys.commsram;
    }
    /* Point RAM: 0xF80000 - 0xF9FFFF */
    if (addr >= 0xF80000 && addr < 0xFA0000) {
        *offset = addr - 0xF80000;
        return g_sys.pointram;
    }
    /* CZ RAM: 0x810000 - 0x8103FF */
    if (addr >= 0x810000 && addr < 0x810400) {
        *offset = addr - 0x810000;
        return g_sys.czram;
    }
    /* Video mixer: 0x824000 - 0x8243FF */
    if (addr >= 0x824000 && addr < 0x824400) {
        *offset = addr - 0x824000;
        return g_sys.videomix;
    }
    /* EEPROM: 0x460000 - 0x463FFF */
    if (addr >= 0x460000 && addr < 0x464000) {
        *offset = addr - 0x460000;
        return g_sys.eeprom;
    }
    return NULL;
}

/* ========== Hardware Register Handlers ========== */

static uint8_t hw_read8(uint32_t addr) {
    /* SYSCON */
    if (addr >= 0x700000 && addr < 0x700020) {
        return g_sys.syscon[addr - 0x700000];
    }
    /* KEYCUS - return random */
    if (addr >= 0x400000 && addr < 0x400020) {
        /* Game-specific: Prop Cycle expects certain values */
        g_sys.keycus_rng = (g_sys.keycus_rng * 1103515245 + 12345) & 0xFFFF;
        return (uint8_t)g_sys.keycus_rng;
    }
    /* DIP switches */
    if (addr >= 0x440000 && addr < 0x440004) {
        return 0xFF; /* all switches off = default settings */
    }
    /* Port bits */
    if (addr >= 0x450000 && addr < 0x450010) {
        return 0xFF;
    }
    return 0;
}

static void hw_write8(uint32_t addr, uint8_t val) {
    /* SYSCON */
    if (addr >= 0x700000 && addr < 0x700020) {
        uint32_t offset = addr - 0x700000;
        g_sys.syscon[offset] = val;

        switch (offset) {
        case 0x04: /* VBlank ACK */
            g_sys.vblank_pending = false;
            break;
        case 0x14: /* Watchdog - just ignore */
            break;
        case 0x16: /* MCU reset */
            if (val == 1) {
                /* MCU released from reset - set ready flag */
                /* MCU status byte at commsram offset for 0xA0BD01 */
                uint32_t mcu_status_off = 0xA0BD01 - COMMSRAM_BASE;
                if (mcu_status_off < COMMSRAM_SIZE)
                    g_sys.commsram[mcu_status_off] = 0x80;
            }
            break;
        case 0x1C: /* DSP control */
            /* 0=disable, 1=enable, 0xFF=upload */
            break;
        }
        return;
    }
    /* KEYCUS writes - ignored */
    if (addr >= 0x400000 && addr < 0x400020) return;
    /* Chipselect */
    if (addr >= 0x800000 && addr < 0x800004) {
        g_sys.chipselect = val;
        return;
    }
    /* CPU LEDs - ignored */
    if (addr >= 0x430000 && addr < 0x430004) return;
}

/* ========== Public Memory Access ========== */

uint8_t mem_read8(uint32_t addr) {
    if (keycus_range(addr) && trace_keycus_active())
        return (uint8_t)trace_keycus_read(addr);
    uint32_t offset;
    uint8_t* base = resolve_addr(addr, &offset);
    if (base) return base[offset];
    return hw_read8(addr);
}

uint16_t mem_read16(uint32_t addr) {
    if (keycus_range(addr) && trace_keycus_active())
        return (uint16_t)trace_keycus_read(addr);
    /* Big-endian (68020) */
    return ((uint16_t)mem_read8(addr) << 8) | mem_read8(addr + 1);
}

uint32_t mem_read32(uint32_t addr) {
    if (keycus_range(addr) && trace_keycus_active())
        return trace_keycus_read(addr);
    return ((uint32_t)mem_read16(addr) << 16) | mem_read16(addr + 2);
}

void mem_write8(uint32_t addr, uint8_t val) {
    uint32_t offset;
    uint8_t* base = resolve_addr(addr, &offset);
    if (base) {
        base[offset] = val;
        return;
    }
    hw_write8(addr, val);
}

void mem_write16(uint32_t addr, uint16_t val) {
    mem_write8(addr, val >> 8);
    mem_write8(addr + 1, val & 0xFF);
}

void mem_write32(uint32_t addr, uint32_t val) {
    mem_write16(addr, val >> 16);
    mem_write16(addr + 2, val & 0xFFFF);
}

/* Direct pointer for game logic that uses C pointers into memory */
void* mem_ptr(uint32_t addr) {
    uint32_t offset;
    uint8_t* base = resolve_addr(addr, &offset);
    if (base) return base + offset;
    return NULL;
}
