/*
 * ss22_game.h -- a Super System 22 game whose 68EC020 program is a lifted C program (tools/namco22/lift.py) as ONE table
 * (engine/ss22_run.c runs it). Tokyo Wars and Dirt Dash were the same program twice, file for file; what actually differs is here:
 * the ROM chips, the keycus answer, how the program image is laid out, the sound board's wiring, the controls, the scripted play a
 * test run feeds the cabinet. HARD RULE 3: one engine, tweaks per game, never copies.
 */
#ifndef ENG_SS22_GAME_H
#define ENG_SS22_GAME_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "ss22_board.h"
#include "ss22_input.h"
#include "romzip.h"
#include "c25.h"
#include "hud_edges.h"

/* the master DSP: the point ROM (three planes of 3 or 4 chips, signed 24-bit words) and the game's translated program */
typedef struct {
    const char *pointrom[3][4];              /* plane 0 = low bytes ... plane 2 = high */
    int         chips;                       /* per plane (3 or 4) */
    bool      (*xlat)(c71_t *d, int pc);     /* gen/<game>_c25.c: the master program translated to C */
} ss22_dsp_cfg;

/* the sound board: the S22-BIOS + sound program, the wave ROMs, what the MCU reads of the cabinet */
typedef struct {
    const char *rom;                         /* the M37710's program (S22-BIOS in the top 16 KB): 0x80000 bytes */
    const char *wave[2];                     /* the C352's sample ROMs (wave[1] may be NULL) */
    uint32_t    wave_off[2];                 /* where each sits in the chip's address space */
    uint32_t    wave_size;                   /* the chip's region */
    uint16_t    inputs_idle;                 /* the INPUTS word with nothing pressed (IP_ACTIVE_LOW; bit 9 = the cabinet switch on Dirt Dash) */
    int         adc_pedal[2];                /* the A-D channels of the two pedals (channel 0 is the wheel) */
} ss22_snd_cfg;

/* the picture: where the ROM chips are and what they are called (the RAM is the board's) */
typedef struct {
    const char *cg[8];                       /* the 8 texture chips */
    const char *ccrl, *ccrh;                 /* the texture tilemap and its attributes */
    const char *scg[4];                      /* the sprite chips */
    int         n_scg;
    size_t      sprite_region;               /* the sprite ROM region: the chips, then this fill */
    uint8_t     sprite_fill;                 /* MAME's ROMREGION_ERASEFF = 0xFF */
    bool        vics_count_in_list;          /* the VICS bank's sprite count is stored in its list (Dirt Dash; MAME's per-game workaround) */
    bool        spot_in_dumps;               /* MAME's captures do not record the spot enable: treat the spot as ON in them (Dirt Dash; Tokyo Wars' table is not a spot) */
    eng_hud_cfg  hud;                        /* WIDESCREEN: which frames show the race HUD and what of the picture is HUD (engine/hud_edges.h) */
} ss22_video_cfg;

typedef struct { const char *name; uint16_t bit; } ss22_press_name;      /* --press NAME@F[+LEN] */

typedef struct ss22_game {
    const char *name;                        /* "Tokyo Wars" */
    const char *tag;                         /* "TW": messages, and the test environment variables <tag>_* */
    const char *lname;                       /* "tw": screenshots, the settings file <lname>_controls.cfg, <lname>_eeprom.nv, the log <name>.log */
    const char *logname;                     /* "tokyowar.log" (Windows) */
    const char *zip_msg;                     /* "tokyowar.zip" */
    const ss22_board_cfg *board;
    ss22_dsp_cfg   dsp;
    ss22_snd_cfg   snd;
    ss22_video_cfg video;
    const ss22_input_game *input;
    const eng_rom_t *roms; int n_roms;       /* the chips, name and size */
    const char *zip;                         /* the MAME set the game unpacks them from */
    void (*entry)(void);                     /* the 68K's reset entry in the lifted program: never returns */
    uint32_t reset_sp_addr;                  /* 0 */
    int         polls_per_frame;             /* 68K instructions a frame (the poll budget): MAME's average */
    const ss22_press_name *presses; int n_presses;
    /* --autoplay: the cabinet as tools/mame/cov_trace_play.lua feeds MAME, at emulated frame n */
    void (*autoplay)(long n, uint16_t *pressed, unsigned *wheel, unsigned *pedal1, unsigned *pedal2);
    /* --stage NAME: a short script that plays the START of the game -- coins, the stage (and car) NAME -- on the cabinet's own inputs, then gives the
     * cabinet back to the player (a window's keys and pad, or nothing). Returns false once it is over (or NAME is not one of start_names). NULL = the
     * game has none. Frame n counts from power-on. */
    bool (*start)(const char *name, long n, uint16_t *pressed, unsigned *wheel, unsigned *pedal1, unsigned *pedal2);
    const char *start_names;                 /* what --stage takes, for the help and the error: "city, jungle, ..." */
    unsigned    pedal_full[2];               /* what --pedal F holds the first pedal at */
} ss22_game;

int ss22_main(int argc, char **argv, const ss22_game *g);            /* engine/ss22_run.c */
extern const ss22_game *g_ss22_game;

/* the modules (each takes the game's table from g_ss22_game) */
bool ss22_dsp_init(const char *rom_dir);
void ss22_dsp_control(uint8_t v);        /* syscon 0x1C: 0 reset, 1 run + DSP IRQs, 0xFF master alone (code upload) */
void ss22_dsp_run(long steps);           /* advance the master (no-op while held in reset) */
void ss22_dsp_vblank(void);              /* INT0 when the DSP IRQs are on */
void ss22_dsp_serial(void);              /* RINT/XINT: MAME's 100 Hz dsp_serial pulse */
bool ss22_dsp_active(void);
bool ss22_dsp_slave_active(void);        /* the game started the slave: the list is to be drawn */
bool ss22_dsp_faulted(void);
void ss22_dsp_debug(char *buf, int n);
uint32_t ss22_dsp_pdp_begins(void);      /* port-2 reads so far: the frames' display lists */
extern int32_t  *g_ss22_pointrom;        /* signed24 words (MAME init_tables) */
extern uint32_t  g_ss22_pointrom_words;
extern bool g_ss22_in_vblank;            /* set by the scheduler while the master answers the vblank INT0 */

bool ss22_snd_init(const char *rom_dir);
void ss22_snd_set_run(bool run);         /* syscon 0x16: 0 holds the MCU in reset */
void ss22_snd_slice(void);               /* 1/16 of a video frame of MCU time (and of chip time) */
void ss22_snd_close(void);
bool ss22_snd_faulted(void);
void ss22_snd_set_output(bool on);       /* the front pair to the sound card (engine/audio_out.c) as it is generated */
void ss22_snd_inputs(uint16_t pressed, unsigned wheel, unsigned pedal1, unsigned pedal2);   /* pressed: INPUTS bits; A-D 10-bit */
uint16_t ss22_snd_outputs(void);         /* the MCU's output latches (lamps, motors) */
void ss22_snd_debug(char *buf, int n);

bool ss22_video_init(const char *rom_dir);
void ss22_video_prepare(void);           /* latch the board's video state and walk polygon RAM: once per frame */
bool ss22_video_load_dump(const char *dir, int frame);   /* MAME's state (tools/mame/vid_dump.lua) into g_ss22, for --render-dump */
void ss22_video_pdp_begin(void);         /* master DSP port 2 read (engine/frame_rule.h) */
void ss22_video_render_refresh(void);    /* master DSP port 8 write */

/* the trace oracle's replayed environment (dev builds only; the shipped game has none) */
int  ss22_env_init(const char *path);
int  ss22_env_active(void);
void ss22_env_report(void);
#endif
