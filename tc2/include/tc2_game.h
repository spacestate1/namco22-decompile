/* tc2_game.h -- ONE GAME = ONE TABLE. The System 23 host in tc2/src is shared by every System 23 / Evolution 2 game (HARD RULE 3: settings and
 * small hooks, never copies). Time Crisis 2 fills it in src/tc2_game.c, Crisis Zone in ../crszone/src/csz_game.c; each binary links one.
 * Everything a game differs in that the host must know is here: the ROM files, the board (System 23 or Evolution 2), the clocks, which
 * CPU interrupt pin each device drives. */
#ifndef TC2_GAME_H
#define TC2_GAME_H
#include <stdint.h>
#include <stddef.h>
#include "eng/romzip.h"

/* the board's interrupt SOURCES (MAME namcos23.cpp MAIN_*_IRQ); each game's table says which Cause bit (IP2..IP7) a source drives */
enum { TC2_SRC_VBLANK, TC2_SRC_C361, TC2_SRC_SUB, TC2_SRC_C435, TC2_SRC_C422, TC2_SRC_C450, TC2_SRC_C451, TC2_SRC_RS232, TC2_SRC_TIMER, TC2_SRC_N };
#define TC2_IP(n) (0x400u << (n))                 /* the CPU's IRQn pin = Cause.IP(n+2); the CP0 timer is IP7 = TC2_IP(5) */

typedef struct {
    const char *title;                            /* "Time Crisis 2": windows, messages */
    const char *tag;                              /* "tc2": the cfg / backup / log file names */
    const char *window_title;                     /* "Time Crisis II" */
    int cz_fog;                                   /* 1 = draw the C412 depth fog (CZ flag + PCZ table, Super 22's model: src/tc2_render.c). Angler King on;
                                                   * TC2 asks for it too (frame 3720's forest) but stays off until checked */
    int uv_edge;                                  /* 1 = model polygons sample their texture range as written (no +0.5 texel, MAME's): a 128-texel tile
                                                   * ends at 1024 EXCLUSIVE, so +0.5 reaches the next atlas tile on the far edges -- Angler King's water seams */
    const char *subtitles;                        /* NULL, or the English subtitle table beside the game (src/tc2_subs.c): Angler King's captions are pictures */
    const char *mame_set;                         /* "timecrs2": the set name the output protocol announces (recoil hooks key on it) */
    const char *zip_help;                         /* what the player must supply, for the missing-ROM message */
    const char *const *zips; int nzips;           /* the MAME zips searched (a split set keeps a device ROM in its own zip) */
    const eng_rom_t *chips; int nchips;           /* every ROM file: name, size, CRC (src/eng/romzip.c) */

    int twin;                                     /* 1 = a TWIN-CABINET game (Time Crisis 2: Left/Red + Right/Blue on the C422 RS-422 link): the menu's
                                                   * Cabinet choice (the I/O board's Link ID) and online play (src/tc2_link.c) */
    int evo2;                                     /* 1 = System 23 EVOLUTION 2: the chips at +0x10000000, 29-bit decode, C450 DMA, IRQ status regs */
    int io_asca3;                                 /* 1 = the I/O board is an ASCA-3 / FCA-1 (MAME namco_asca_3_device; Angler King's namco_fca10 is one with its
                                                   * undumped MB90F574 held in reset): switches 0x6000-3 (ASCA-1 layout), outputs 0x6004/5, the rotary
                                                   * encoders' differences 0x6006/7, the analogue inputs on the H8's A/D (src/tc2_h8.c). 0 = Time Crisis 2's gun board */
    int super23;                                  /* 1 = SUPER System 23 (MAME 0.289 namcoss23_state: Angler King): a write to 0x0100005C acknowledges the C361
                                                   * (raster) interrupt (c435_c361_ack_w) -- the game's IP3 handler does it, it never reads 0x0682000A/C */
    uint32_t clock;                               /* the main CPU, Hz (cycles as MAME charges them) */
    uint32_t px_num, px_den;                      /* pixel clocks (25.6 MHz) per main cycle, exactly */
    uint32_t sub_div;                             /* main cycles per sub-CPU (H8/3002) state */
    uint32_t io_num, io_den;                      /* I/O board (H8/3334) states = main cycles * io_num / io_den */
    uint32_t c352_div;                            /* main cycles per C352 sample (88200 Hz) */
    uint32_t ip[TC2_SRC_N];                       /* the Cause bit each source drives (0 = not wired) */
    int irq_prompt;                               /* 1 = an interrupt a device raises mid-slice is polled at once (MAME takes it at the next instruction);
                                                   * 0 = at the end of the slice, as Time Crisis 2's gates were measured with. Crisis Zone waits on ~550
                                                   * C450 DMA interrupts a frame: a slice's delay each (up to TC2_SLICE instructions) halved its frame rate */

    /* the program images, made from the ROM files at start-up (tools/make_prog.py writes the same for the translators) */
    uint32_t boot_size;                           /* the boot ROM as the CPU sees it, ending at 0x1FFFFFFF (4 MB System 23, 8 MB Evolution 2) */
    int (*rom_boot)(const char *dir, uint8_t *dst);
    int (*rom_sub)(const char *dir, uint8_t *dst, size_t n);
    int (*rom_io)(const char *dir, uint8_t *dst, size_t n);
    uint32_t io_rom_size;                         /* how much of the I/O board's program its H8 maps */
    int no_direct_buf;                            /* 1 = the C435 direct buffer (S23 0x0CC00000) is not decoded: Crisis Zone (MAME's crszone map leaves
                                                   * 0x1CC00000 unmapped: a full-screen black quad written there covers the namco / GASHIN logos) */
    int depth_default;                            /* Display > Depth when tc2.cfg has no choice yet: 1 = "exact" (Time Crisis 2), 0 = the arcade's order.
                                                   * Crisis Zone 0: with "exact" its hotel scene (frame 2280) loses the sky (the background colour shows where
                                                   * coverage proves the sky polygons drawn) and gains black bands top and bottom -- cause not found yet (NOT
                                                   * transparent texels: System 23 has none, only the stencil, which the pre-pass honours) */
    uint16_t c417_status;                         /* the C417 status read's fixed bits (MAME: System 23 0x008E; crszone_state::c417_status_r 0) | test mode << 15 */
    uint16_t gun_x0, gun_xw, gun_y0, gun_yh;      /* the light gun's port range (MAME JVS_SCREEN_POSITION_INPUT_X1/Y1 PORT_MINMAX): first value + width */
    uint8_t io_6001_held;                         /* bits of the I/O board's 0x6001 that read as PRESSED always (MAME namco_tss_io_device::in_r: bit 3 =
                                                   * BUTTON5, which crszone's port declares active-low and unused -> pressed; "Motor test shows NG if this is not pressed") */
    uint8_t sub_p7;                               /* what the sub-CPU reads on port 7 (MAME: timecrs2 0, crszone 0x80) */
    const char *data_rom[2][2];                   /* the data ROMs: [half][high, low] byte-interleaved */
    int npt; const char *pt_rom[4][2];            /* the point ROM chip pairs [high, low] */
    const char *ccrl, *ccrh, *cg[4];              /* texture tile maps and tiles */
    const char *wave[2];                          /* the C352's samples: low, high */
    int recoil_hold;                              /* 1 = 0x6002 bit 0 stays on WHILE the gun fires (Crisis Zone's machine gun: measured on a player's
                                                   * session, on for the whole burst), 0 = a 3-frame pulse per shot (Time Crisis 2) */
    int io_analog_out;                            /* 1 = the I/O board has MAME's namco_csz1 analog output at 0x6003 (Crisis Zone): published as analog0 */
    /* the SPEECH, on its own gain (Audio > Voice volume, GitHub #46): a C352 voice in speech_voices, or a key-on from a sample bank in
     * speech_bank_lo..speech_bank_hi (hi 0 = no range), is mixed as speech. Measured per game with C352_KEYLOG on a recorded session */
    uint32_t speech_voices;
    uint8_t speech_bank_lo, speech_bank_hi;
} tc2_game_t;

extern const tc2_game_t tc2_game;                 /* the one this binary was built for */
#endif
