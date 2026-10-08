/* rr_game.h -- what differs between the System 22 games this runtime hosts (HARD RULE 3: one runtime, tweaks per game).
 * Rave Racer's table is the default (src/rr_game.c); another game (acedriver/) supplies its own table instead of that file. */
#ifndef RR_GAME_H
#define RR_GAME_H
#include <stddef.h>
#include <stdint.h>
#include "romzip.h"

typedef struct { const char *file; uint32_t at; } rr_wave_t;
/* A developer screen left in the program (Ace Driver's object / sprite / character viewers). The program runs its modes as
 * coroutines: the main loop resumes the mode task each frame through the address saved in its slot, the mode code yields back
 * (Ace Driver 0x4C48 / 0x4C6E; its own setter 0x4C36 writes the slot). The menu's Debug page launches a screen the same way:
 * at a frame boundary in the MAIN context (the task stack empty) it writes `entry` into the mode task's slot and turns the Test
 * switch ON (these screens loop while it is on); the main loop then starts the screen, and turning the Test switch OFF is the
 * screen's own exit back into the game (rr_main.c rr_debug_launch). */
typedef struct { const char *name; uint32_t entry; const char *help; } rr_debug_t;

typedef struct {
    const char *name;                       /* "raverace", "acedrive" ... */
    const char *title;                      /* the window title and message boxes: "Rave Racer", "Ace Driver" */
    const char *cfg_file;                   /* the controls / menu settings file beside the binary (rr_controls.cfg, ad_controls.cfg) */
    const char *nv_file;                    /* the persisted EEPROM of a windowed session (rr_eeprom.nv, ad_eeprom.nv): never shared between games */
    int autosetup;                          /* 1: src/rr_romzip.c unpacks this game's zips on first run (its chip table is Rave Racer's); 0: the ROM files must be in extracted/ */
    void (*entry)(void);                    /* the lifted reset routine (the 68020 program's reset PC): never returns */
    const char *prg[4];                     /* the four 68020 program chips: MAME ROM_LOAD32_BYTE offsets 0..3 (uu, um, lm, ll) */
    const char *cg[8];                      /* texture chips, 2 MB each at slot i (NULL = the slot is empty: Ace Driver has slots 4..7 only) */
    const char *ccrl, *ccrh;                /* texture tilemap + attributes */
    const char *pot[3][4];                  /* point ROM: low / mid / upper byte planes, pot_chips chips of 512 KB each (NULL past that) */
    int pot_chips;                          /* 4 (Rave Racer), 2 (Ace Driver), 3 (Victory Lap) */
    const char *snd_data;                   /* the sound MCU's program + data ROM */
    rr_wave_t wav[4];                       /* C352 wave ROMs and where each sits */
    const char *gamma[3];                   /* gamma PROMs */
    const char *eeprom;                     /* default EEPROM image, or NULL (blank: the game initialises it) */
    int keycus_off; uint16_t keycus_val;    /* MAME namcos22_keycus_r: this word offset returns keycus_val, every other read is random; -1 = all random */
    int steer_add, gas_add, brake_add;      /* MAME handle_driving_io: the offsets added to the axes in shared RAM 0x32..0x36 */
    uint16_t inputs_idle;                   /* the INPUTS word with nothing pressed (active-low bits 1; Rave Racer's cabinet field 0x2100 = Standard -> 0xFEFF) */
    int steer_min, steer_max;               /* MAME ADC.0 PORT_MINMAX (centre 0x800) */
    int gas_max, brake_max;                 /* MAME ADC.1 / ADC.2 PORT_MINMAX upper bound */
    int gas_delta, brake_delta;             /* MAME PORT_KEYDELTA of the pedals (ADC counts per frame from a key) */
    int tex_fixup;                          /* 1: MAME's System 22 texture tilemap fix-up (eng_texture_tilemap_sys22_fixup): Ace Driver, Ridge Racer */
    uint16_t spin_pc, spin_op;              /* the master DSP program's busy-wait poll (0 = none: nothing is fast-forwarded) */
    const rr_debug_t *debug; int ndebug;    /* developer screens for the Debug menu page (none: the page is hidden) */
    uint32_t task_sp, task_sp_empty;        /* where the coroutine task stack pointer lives, and its value when empty (main context) */
    uint32_t task_slot;                     /* the mode task's resume slot the Debug page writes */
    /* A player's first run: the chips taken out of these MAME zips by the ENGINE's table-driven unpacker (engine/romzip.c) when
     * the ROM folder is incomplete. NULL roms = Rave Racer's own src/rr_romzip.c (autosetup). */
    const char *const *zips; int nzips;
    const eng_rom_t *roms; int nroms;
    const char *log_file;                   /* Windows: stdout/stderr beside the .exe (raveracer.log, acedriver.log) */
    /* TWIN STICKS instead of wheel / gas / brake (Cyber Commando): the I/O answer is MAME's handle_cybrcomm_io -- the four 8-bit stick
     * ADCs x 0x10 at shared RAM 0x32 / 0x34 / 0x36 / 0x38 (right Y, left Y, right X, left X; centre 0x7F, range 0x47..0xB7, forward =
     * low). Keys drive both sticks as "tank" controls through the existing actions (gas = forward, brake = back, steer = turn: the
     * sticks pushed opposite ways); a gamepad's two sticks are the cabinet's two sticks. 0 = the driving model (every other game). */
    int twin_stick;
    const char *const *act_label;           /* per-action names for the Controls page (NULL entries / NULL table = the action's own name) */
    const struct rr_bind_default { int act; const char *keys, *pad; } *bind_defaults;   /* default keys / pad button replacing the
                                             * driving defaults for these actions (keys: SDL names, comma-separated; pad NULL = none) */
    int nbind_defaults;
} rr_game_t;

extern const rr_game_t *g_rr_game;
/* rr_main.c: launch g_rr_game->debug[i] at the next frame boundary (outside interrupts) */
void rr_debug_launch(int i);
#endif
