#define _POSIX_C_SOURCE 200809L
/*
 * Work RAM array and sync layer
 *
 * Bridges the two memory representations:
 *   g_sys.work_ram[] - uint8_t big-endian byte array (hand-written code, renderers)
 *   _W[]             - intptr_t native-endian per byte offset (transpiled code)
 *
 * _W uses intptr_t so it can hold both integer values AND pointers on 64-bit.
 */
#include "propcycl.h"
#include <stdlib.h>
#include <time.h>

/* Work RAM array */
intptr_t _W[WORK_RAM_SIZE];

/*
 * Pinned slots: _W[] is authoritative here, so sync_wram_to_W must not
 * overwrite them from work_ram.
 *
 * WHY THIS EXISTS. sync_W_to_wram writes back ONLY 4-byte-aligned slots,
 * while sync_wram_to_W reads back BOTH 4- and 2-byte-aligned ones. Two
 * kinds of value cannot survive that asymmetry:
 *
 *  1. A 16-bit game variable at a 2-mod-4 offset. Nothing ever writes its
 *     bytes out, so every frame _W[i] is rebuilt from its NEIGHBOURS'
 *     bytes. W[0x2C0E] (credits) and W[0x2C12] (coin-armed) are both such
 *     offsets: coin_credit_update's increment was erased before the next
 *     frame could read it, and state_attract_init's W[0x2C12] = 1 read back
 *     as 0. That is why player input looked dead even once the coin
 *     arrived in W[0x2B82] as a correct edge.
 *
 *  2. A slot whose only producer is hand-written code running OUTSIDE
 *     game_frame() -- input.c writing the ADC pair from input_poll(). Such
 *     a slot is a fixpoint at its initial value: sync_wram_to_W runs before
 *     anything reads it and resets it to last frame's work_ram bytes, then
 *     sync_W_to_wram writes that same stale value back out. The
 *     hand-written write is erased every frame, never observed.
 *
 * Pinning is deliberately per-slot rather than a blanket rule. Rebuilding a
 * 2-aligned slot from neighbouring bytes is NOT always wrong -- it is how
 * the transpiled code's nested indexing reads a 16-bit view that lands mid
 * variable, e.g. `W[0x28FC + W[0x096C]]` in terrain_cell_history_store.
 * Suppressing that wholesale corrupts the index and segfaults, so only
 * slots that genuinely need to persist are listed.
 *
 * This does not repair the underlying _W[] model; GUARDRAILS.md removes it.
 */
static uint8_t wram_pinned[WORK_RAM_SIZE];

void wsync_pin(uint32_t off, uint32_t len) {
    for (uint32_t i = off; i < off + len && i < WORK_RAM_SIZE; i++)
        wram_pinned[i] = 1;
}

/* PROPCYCL_LEGACY_SYNC=1 ignores the pins, for bisecting against this
 * layer. */
static int legacy_sync(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("PROPCYCL_LEGACY_SYNC"); v = (e && *e != '0'); }
    return v;
}

/*
 * sync_wram_to_W: Copy g_sys.work_ram → _W[]
 * Only syncs 4-byte aligned offsets. The transpiled code accesses most
 * state at 4-byte aligned offsets; 2-byte aligned accesses read the
 * same bytes (just shifted).
 */
double g_perf_w2r, g_perf_r2w; long g_perf_frames;
static double perf_now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec+t.tv_nsec*1e-9; }
void sync_wram_to_W(void) {
    double _t0 = perf_now();
    uint8_t *ram = g_sys.work_ram;
    const int legacy = legacy_sync();

    /* NOTE: apart from the pins this loop must stay unconditional. It is a
     * lossless round trip with sync_W_to_wram, but it is ALSO an implicit
     * "truncate every slot to sign-extended 32 bits" pass that the
     * transpiled code depends on: skipping unchanged slots let 64-bit
     * values persist in slots used as ROM indices and segfaulted in
     * FUN_0000eaaa (`(&R[0x354D4])[W[0x15EE4]]`). */
    /* ONE PASS, BOTH ALIGNMENTS. This used to be two full sweeps of the
     * 256 KB work RAM -- offsets 0,4,8,... then 2,6,10,... -- which walks the
     * same bytes twice and touches the 2 MB _W[] array twice. Measured at
     * 1.20s of a 11.2s 2000-frame run (sync_W_to_wram, which makes ONE pass
     * over the same data, cost 0.38s). Folding them together reads
     * ram[i..i+5] once per step and halves the loop overhead. Semantics are
     * unchanged: the same slots get the same values, and the 2-mod-4 pass
     * still runs -- it is what lets a 16-bit read land mid-slot. */
    for (uint32_t i = 0; i <= WORK_RAM_SIZE - 4; i += 4) {
        if (legacy || !wram_pinned[i]) {
            intptr_t val = (intptr_t)(int32_t)(
                ((uint32_t)ram[i] << 24) | ((uint32_t)ram[i+1] << 16) |
                ((uint32_t)ram[i+2] << 8) | (uint32_t)ram[i+3]);
            if ((_W[i] >> 32) == 0 || (_W[i] >> 32) == -1) _W[i] = val;
        }
        uint32_t j = i + 2;
        if (j <= WORK_RAM_SIZE - 4 && (legacy || !wram_pinned[j])) {
            intptr_t val = (intptr_t)(int32_t)(
                ((uint32_t)ram[j] << 24) | ((uint32_t)ram[j+1] << 16) |
                ((uint32_t)ram[j+2] << 8) | (uint32_t)ram[j+3]);
            if ((_W[j] >> 32) == 0 || (_W[j] >> 32) == -1) _W[j] = val;
        }
    }
    g_perf_w2r += perf_now() - _t0;
}

/*
 * sync_W_to_wram: Copy _W[] → g_sys.work_ram
 * Only syncs 4-byte aligned offsets to avoid overlapping writes.
 * 2-byte values are NOT synced back separately — they share bytes
 * with the 4-byte values and would clobber them.
 */
void sync_W_to_wram(void) {
    double _t0 = perf_now();
    uint8_t *ram = g_sys.work_ram;

    for (uint32_t i = 0; i <= WORK_RAM_SIZE - 4; i += 4) {
        int32_t v = (int32_t)_W[i];
        ram[i]   = (v >> 24) & 0xFF;
        ram[i+1] = (v >> 16) & 0xFF;
        ram[i+2] = (v >> 8) & 0xFF;
        ram[i+3] = v & 0xFF;
    }
    /* Do NOT sync 2-byte offsets back — they overlap with 4-byte values
     * and would overwrite correct data with stale values. */
    g_perf_r2w += perf_now() - _t0;
}
/* ---------------------------------------------------------------------------
 * EEPROM: DELIBERATE OVERRIDES, not gaps.
 *
 * Unlike the RTS stubs elsewhere in this tree, all three of these are REAL
 * M68K code in the ROM -- verified by reading pr2ver-a.*:
 *
 *     eeprom_sector_format      @0x04A436   first word 0x23FC (move.l #imm,abs)
 *     eeprom_wait_not_busy      @0x049B92   first word 0x3039 (move.w abs,d0)
 *     eeprom_default_data_init  @0x04A0D4   first word 0x41F9 (lea abs,a0)
 *
 * They drive the serial EEPROM that holds coinage, bookkeeping and high
 * scores. This build forces FREE PLAY (eeprom_settings_init is stubbed in
 * game_logic.c), so the device is never exercised and nothing reads back
 * what these would have written. Returning 0 means "succeeded / not busy",
 * which is what every caller checks for.
 *
 * To implement: model the serial EEPROM and back it with an NVRAM file --
 * tools/l3/pin/nvram already holds a pinned image for the differential
 * harness, so that is the reference content. Until then, changing these to
 * anything but 0 will hang the callers that poll eeprom_wait_not_busy.
 *
 * The same applies to eeprom_sync_all @0x019EEE, eeprom_write_block
 * @0x019F1A and eeprom_command_handler @0x0489F8 in game_eeprom.c.
 * ------------------------------------------------------------------------ */
int eeprom_sector_format()     { return 0; }
int eeprom_wait_not_busy()     { return 0; }
int eeprom_default_data_init() { return 0; }


/* ---- stub reporting (see STUB_HIT in include/propcycl.h) ---- */
int g_stub_warn_all = 0;
static int g_stub_warn_off = -1;

void stub_hit_report(const char *fn, unsigned rom_addr, const char *note)
{
    if (g_stub_warn_off < 0)
        g_stub_warn_off = getenv("PROPCYCL_NO_STUBWARN") ? 1 : 0;
    if (g_stub_warn_off) return;
    fprintf(stderr, "[STUB] %s @0x%06X called -- %s\n",
            fn, rom_addr, note ? note : "not implemented");
}
