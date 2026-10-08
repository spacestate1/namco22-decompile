/*
 * s21_board.h -- the Cyber Sled (System 21) board: memory maps of the two 68000s, the two C148 interrupt controllers, shared RAM,
 * DPRAM, NVRAM, palette / sprite RAM storage, video enable, the reset lines, vblank and the slice scheduler (cybsled/DESIGN.md,
 * module A). Behaviour written from MAME's namcos21_c67.cpp / namco_c148.cpp (reference only, not copied).
 *
 * Every word RAM is a HOST-ORDER u16 array as the 68000 sees it (s21_video_bind takes them as they are). The DPRAM is bytes (the
 * 68000 sees the low byte of each word at 0xA00000, the 6809 at 0x7000, the C68 through its dp callbacks).
 */
#ifndef S21_BOARD_H
#define S21_BOARD_H
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint16_t mrom[0x80000], srom[0x40000], data[0x80000], edata[0x80000];   /* master 1 MB, slave 512 KB, data 1 MB, edata 1 MB */
    uint16_t mram[0x8000], sram[0x20000];                                   /* master 64 KB, slave 256 KB work RAM */
    uint16_t shared[0x8000];                                                /* 0x900000 shared RAM 64 KB */
    uint8_t  dpram[0x800];                                                  /* 0xA00000 (low bytes) / 6809 0x7000 / C68 */
    uint8_t  nvram[0x2000];                                                 /* 0x180000 odd lane (master only) */
    uint16_t pal[0x8000], palext[0x8000];                                   /* 0x740000 / 0x750000 */
    uint16_t spr[0x10000], sprpos[4], vena;                                 /* 0x700000 C355 / 0x720000 / 0x760000 */
    uint16_t c139ram[0x2000];                                               /* 0xB00000 */
    uint32_t frame;                                                         /* video frames run */
    int      slave_reset, snd_reset;                                        /* reset lines, 1 = held */
    uint64_t ins[2];                                                        /* 68000 instructions executed */
} s21_board_t;
extern s21_board_t g_s21;

bool s21_init(const char *romdir);
void s21_board_reset(void);                   /* the operator's restart between frames: CPUs from reset, the NVRAM kept */           /* load the ROMs and the default NVRAM (cybsled.nv), machine reset */
void s21_run_frame(void);                    /* one video frame (~59.94 Hz): both 68000s in slices, the hooks below, vblank */
bool s21_nvram_load(const char *path);
bool s21_nvram_save(const char *path);
void s21_set_only(int cpu);                  /* dev: run only this 68000 (-1 = both), e.g. for a one-CPU trace gate */

/* Scheduler settings (defaults: 19000 instructions per CPU per frame -- MAME's attract measures ~19.2k for the master --, in 200
 * slices; env S21_IPF / S21_SLICES override). Time is counted in 68000 instructions; the other chips get 12.288 MHz cycles. */
extern int s21_ipf, s21_slices;

/* ---- the other modules: their own headers are the interface (src/dsp/s21_dsp.h, src/snd/s21_snd.h, src/io/s21_io.h); s21_board.c
 * calls them, s21_stubs.c gives weak stand-ins with the same signatures for a build without a module (DSP RAM / point RAM /
 * depth cue then just STORE what is written). Clocks: s21_dsp_run takes 10 MHz master-DSP cycles, s21_snd_run 6809 cycles
 * (2.048 MHz), s21_io_run M37450 bus cycles (2.048 MHz). ---- */
void     s21_board_quad_hooks(void (*quad)(const void *q), void (*swap)(void));   /* the DSPs' finished quads -> the picture */

/* the C355 sprite RAM writer (module C keeps its page-0 mirrors) */
void     s21_video_spriteram_w(uint16_t *spr, uint32_t woff, uint16_t data, uint16_t mem_mask);
#endif
