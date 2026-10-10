#include <stdlib.h>
/*
 * tc2_screen.c -- the System 23 screen timing and the C361's raster interrupt, our own code (MAME's namcos23.cpp: the screen is
 * set_raw(25.6 MHz, 814, 0, 640, 525, 0, 480) -- 59.906 Hz -- and its vblank callback; c361_irq_scanline_w / c361_timer_cb / c361_vpos_r /
 * c361_vblank_r; the interrupt wiring of machine_start).
 *
 * Time is the CPU's cycle count (169.344 MHz; the lift charges MAME's per-instruction costs), turned EXACTLY into pixel clocks
 * (25.6 / 169.344 = 200 / 1323). A line is 814 pixel clocks, a frame 525 lines; lines 0-479 are visible, 480-524 the vertical blank.
 *   vblank       asserted at line 480 (Cause IP2, the CPU's IRQ0; the CTL's "vblank active" latch), released at line 0 -- or earlier by the
 *                acknowledge write 0x0D00000A, which only drops it while that latch is set
 *   C361         0x06820000 W x scroll   0x06820002 W y scroll   0x06820008 W the raster line: a ONE-SHOT interrupt (Cause IP3, IRQ1 --
 *                shared with the sub-CPU) the next time the beam reaches that line; line 0x1FF clears it instead
 *                0x0682000A R beam position: vpos * 2 | vblank; clears the C361 interrupt
 *                0x0682000C R 1 while the C361 interrupt is pending or in vblank; clears the C361 interrupt
 */
#include <stdint.h>
#include <stdio.h>
#include "lift_rt.h"
#include "tc2_host.h"

#define LINE_PIX   814ull
#define FRAME_PIX  (814ull * 525ull)
#define VBL_LINE   480ull
#define NEVER      UINT64_MAX

static uint64_t cyc;                         /* CPU cycles up to the last tc2_screen_advance */
static uint64_t off_pix;                     /* the pixel clock at cycle 0 (power-on: 0; a snapshot: the start of vblank) */
static uint64_t next_vbl_on, next_vbl_off, next_c361;
static int vbl_active;                       /* MAME's m_ctl_vbl_active */
static uint16_t c361_line = 0x1FF, c361_xscroll, c361_yscroll;
/* the text layer's ROW SCROLL (MAME update_text_rowscroll): an x-scroll write holds for the rows the beam has not reached yet (+4, MAME's
 * own fudge: "otherwise rowscrolls are 4 lines too early"); tc2_text_render applies them shifted by the y scroll */
static uint16_t rowscroll[480]; static int lastrow; static uint64_t rowscroll_frame;
static uint32_t vpos_now(int *vblank);
static uint64_t pix_at(uint64_t c);
extern uint64_t tc2_cycles_now(void);
static void update_rowscroll(void)
{
    int vb; const uint32_t line = vpos_now(&vb);
    /* A NEW PICTURE STARTS WHEN THE BEAM PASSES LINE 0 -- counted on the pixel clock, not on the frame counter and not on "the line went
     * down": the game writes the attract banner's scroll at line 480 and resets it at line 66, with the frame counter ticking between the
     * two (keyed on the counter, rows 0..66 never got the banner's scroll: it sat still in chunks); and in the demo it writes ONLY at line
     * 480, every frame (keyed on a falling line number, no write ever wrapped and rows 0..479 kept the banner's old scroll: the SCORE
     * panel was drawn off to the side) */
    static uint64_t pic = ~0ull;
    const uint64_t now_pic = pix_at(tc2_cycles_now()) / FRAME_PIX;
    if (now_pic != pic) { pic = now_pic; lastrow = 0; }
    (void)rowscroll_frame;
    const int sx = (c361_xscroll - 0x35C) & 0x3FF, y = (int)line + 4 < 480 ? (int)line + 4 : 480;
    for (int i = lastrow; i < y; i++) rowscroll[i] = (uint16_t)sx;
    if (y > lastrow) lastrow = y;
}
void tc2_text_scroll(uint16_t linexscroll[1024], uint16_t *yscroll)   /* at the frame's end: MAME apply_text_scroll */
{
    update_rowscroll();
    for (int i = 0; i < 480; i++) linexscroll[(i + (c361_yscroll & 0x3FF) + 4) & 0x3FF] = rowscroll[i];
    *yscroll = c361_yscroll;
}
uint16_t tc2_c361_xscroll(void) { return c361_xscroll; }
uint16_t tc2_c361_yscroll(void) { return c361_yscroll; }

static uint64_t pix_at(uint64_t c) { return off_pix + c * 200u / 1323u; }
static uint64_t cycles_until(uint64_t pix_target, uint64_t c)    /* the first cycle >= c whose pixel clock reaches pix_target */
{
    const uint64_t p = pix_at(c);
    if (pix_target <= p) return 0;
    uint64_t d = ((pix_target - off_pix) * 1323u + 199u) / 200u;   /* the first cycle count reaching it */
    return d > c ? d - c : 0;
}
void tc2_irq_line(uint32_t bit, int on);
void tc2_h8_vblank(int on);
int tc2_irq_pending(uint32_t bit);
static void cause(uint32_t bit, int on) { tc2_irq_line(bit, on); }

/* power-on: MAME's beam starts at line 480 -- the start of vertical blank -- not at line 0 (measured: the POST's first beam read at
 * 0x80025C60 is line 227 on MAME, 272 with the beam starting at line 0: 45 lines = 525 - 480), with vblank asserted */
void tc2_screen_reset(void)
{
    cyc = 0; off_pix = VBL_LINE * LINE_PIX; vbl_active = 1; c361_line = 0x1FF;
    next_vbl_on = off_pix + FRAME_PIX; next_vbl_off = FRAME_PIX; next_c361 = NEVER;
    cause(TC2_IP_VBLANK, 1);
}
/* a snapshot is taken at MAME's frame_done = the start of vertical blank (line 480); vblank was just asserted */
void tc2_screen_at_vblank(void)
{
    void tc2_irq_sync_from_cause(void); tc2_irq_sync_from_cause();
    cyc = 0; off_pix = VBL_LINE * LINE_PIX; vbl_active = 1; c361_line = 0x1FF;
    next_vbl_on = off_pix + FRAME_PIX; next_vbl_off = FRAME_PIX; next_c361 = NEVER;
}

/* the current cycle: the host's count plus what the running slice has used (src/tc2_cpu.c) */
extern uint64_t tc2_cycles_now(void);
void tc2_on_vblank(uint32_t frame);      /* the host: a frame is complete (MAME draws the screen here) */

static void fire_due(uint64_t p)
{
    for (;;) {
        uint64_t e = next_vbl_on; int k = 0;
        if (next_vbl_off < e) { e = next_vbl_off; k = 1; }
        if (next_c361 < e) { e = next_c361; k = 2; }
        if (e > p) return;
        if (k == 0) { vbl_active = 1; cause(TC2_IP_VBLANK, 1); tc2_h8_vblank(1); rr_frame++; next_vbl_on += FRAME_PIX; tc2_on_vblank(rr_frame); }
        else if (k == 1) { cause(TC2_IP_VBLANK, 0); tc2_h8_vblank(0); next_vbl_off += FRAME_PIX; }
        else { cause(TC2_IP_RASTER, c361_line != 0x1FF); next_c361 = NEVER; }   /* one-shot (MAME: "TC2 resets it each VBL") */
    }
}

void tc2_screen_advance(uint32_t n) { cyc += n; fire_due(pix_at(cyc)); }

uint32_t tc2_screen_until_event(void)
{
    uint64_t e = next_vbl_on; if (next_vbl_off < e) e = next_vbl_off; if (next_c361 < e) e = next_c361;
    const uint64_t d = cycles_until(e, cyc);
    return d == 0 ? 1 : d > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)d;
}

static uint32_t vpos_now(int *vblank)
{
    const uint64_t c = tc2_cycles_now();
    const uint64_t p = pix_at(c);
    fire_due(p);
    const uint32_t line = (uint32_t)((p % FRAME_PIX) / LINE_PIX);
    *vblank = line >= VBL_LINE;
    return line;
}

int tc2_screen_read(uint32_t p, int size, uint32_t *v)
{
    if (size != 2) return 0;
    int vb;
    if (p == 0x0682000Au) { const uint32_t l = vpos_now(&vb); cause(TC2_IP_RASTER, 0); *v = (l * 2) | (uint32_t)vb; return 1; }
    if (p == 0x0682000Cu) { vpos_now(&vb); *v = (tc2_irq_pending(TC2_IP_RASTER) || vb) ? 1 : 0; cause(TC2_IP_RASTER, 0); return 1; }
    return 0;
}

int tc2_screen_write(uint32_t p, int size, uint32_t v)
{
    if (size != 2) return 0;
    if (p == 0x06820000u) {
        static int lg = -1, lg_from; if (lg < 0) { const char *e = getenv("TC2_SCROLLLOG"); lg = e != NULL; lg_from = e ? atoi(e) : 0; }
        if (lg && rr_frame >= (uint32_t)lg_from && rr_frame <= (uint32_t)lg_from + 3) { int vb; fprintf(stderr, "[SCROLL] frame %u line %u%s x=%04X\n", rr_frame, vpos_now(&vb), vb ? " (vblank)" : "", (unsigned)v); }
        update_rowscroll(); c361_xscroll = (uint16_t)v; return 1; }
    if (p == 0x06820002u) { c361_yscroll = (uint16_t)v; return 1; }
    if (p == 0x06820008u) {                                       /* the raster line: armed for the next time the beam reaches it */
        c361_line = (uint16_t)(v & 0x1FF);
        const uint64_t now = pix_at(tc2_cycles_now());
        uint64_t t = now - now % FRAME_PIX + (uint64_t)c361_line * LINE_PIX;
        if (t <= now) t += FRAME_PIX;
        next_c361 = t;
        return 1;
    }
    if (p == 0x0D00000Au) { if (vbl_active) { vbl_active = 0; cause(TC2_IP_VBLANK, 0); } return 1; }   /* MAME: ctl_vbl_ack_w */
    return 0;
}

int tc2_screen_owns(uint32_t p) { return p >= 0x06820008u && p <= 0x0682000Du; }
