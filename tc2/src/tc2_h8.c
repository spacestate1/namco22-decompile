/*
 * tc2_h8.c -- Time Crisis 2's TWO H8s on our host: the sub-CPU (H8/3002: the sound program, the JVS link, the clock) and the gun I/O board
 * (H8/3334, MAME's namco_tssio), each its TRANSLATED program (gen/tc2_sub.c, gen/tc2_io.c; tools/h8/h8_translate.py) with our own on-chip
 * peripherals (src/h8_periph.c) and the board around them, as MAME's namcos23.cpp and namcoio.cpp wire it:
 *
 *   sub-CPU  0x000000-0x07FFFF  its program ROM (sub.bin = tss1vera.3, word-swapped)
 *            0x080000-0x08FFFF  the RAM shared with the main CPU (the main CPU's 0x04400000; same byte order, MAME BYTE_XOR_BE on 16-bit words)
 *            0x280000-0x287FFF  the C352 (src/eng/c352.c), 16-bit registers
 *            0x300020           a word write of 0x3170 interrupts the main CPU (Cause IP3, shared with the C361); 0x300000-0x30003F otherwise nothing
 *            port 6 bit 1       the JVS sense: 1 until the I/O board says it is initialised (its port 4 bit 2)
 *            port 8 bit 1       the IRQ1 pin (two scan-line timers, tc2_h8_run); port B bit 7 vblank
 *            ports 8 / A / B    read back what was written (MAME m_sub_port8 / porta / portb)
 *            SCI0               the JVS link (RS-485, 115200 baud: an external clock of 14.7456 MHz / 8 = 16x the bit rate) to the I/O board's SCI0
 *            SCI1               clocked, to the RTC-4543 (selected by port B bit 5 AND port A bit 0) and the settings latch (port A bit 0 low)
 *            held in reset at power-on; the main CPU's write to 0x04C3FF0A starts (non-zero: resets and runs) or stops it; 0x04C3FF04 acknowledges
 *            its interrupt
 *   I/O      0x0000-0x3FFF ROM (io.bin = tssioprog.ic3), 0xC000-0xFF7F RAM, 0x6000/1 the switches, 0x6002 the outputs, 0x7000-5 the GUN
 *            (X, Y, Y+1 as low then high bytes), 0x7007 its acknowledge; port 4 bit 2 = JVS sense out, port 5 bit 2 = RS-485 direction
 * Clocks: main 169.344 MHz, sub 16.9344 MHz (exactly 1/10), I/O 14.7456 MHz (64/735 of the main's). Each H8 runs up to the main CPU's time
 * at every scheduler poll (src/tc2_main.c), in slices that end at its peripherals' next event, interrupts taken between slices.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "h8_sem.h"
#include "h8_periph.h"
#include "eng/c352.h"
#include "tc2_host.h"

static int dbg(const char *name) { return getenv(name) != NULL; }   /* dev logs: asked once each, at their first use */
#define DBG(name) ({ static int v_ = -1; if (v_ < 0) v_ = dbg(name); v_; })

void tc2sub_run(h8_cpu *c);
void tc2io_run(h8_cpu *c);
uint8_t *tc2_shared_ram(void);
void tc2_irq_source(int src, uint32_t bit, int on);
extern uint16_t tc2_inputs, g_ss22_gun_x, g_ss22_gun_y;
extern _Bool g_ss22_gun_off;

static uint8_t sub_rom[0x80000], sub_onchip[0x200], io_rom[0x4000], io_ram[0x3F80];
static h8_cpu sub, io;
static h8p subp, iop;
static int sub_running, sub_ok, io_ok;
static uint64_t line_k;                                   /* the next line boundary to process (absolute lines since power-on: tc2_h8_run) */
static c352_t c352;
static uint8_t *wave;
static uint64_t c352_done;                       /* C352 samples generated (88200 Hz: one per 1920 main cycles) */
static uint8_t sub_p8 = 0x02, sub_pa = 0, sub_pb = 0x50;
static int jvs_sense_init;                       /* the I/O board says it is initialised (its port 4 bit 2) */
static uint8_t io_out;                           /* 0x6002: the cabinet outputs (bit 0 = the gun's recoil solenoid: src/tc2_out.c) */
uint8_t tc2_h8_outputs(void) { return io_out; }
static uint64_t main_now;                        /* the main CPU's cycle at the last tc2_h8_run */
unsigned long tc2_h8_untranslated;

/* ---------------------------------------------------------------- time */
static int64_t sub_target(uint64_t m) { return (int64_t)(m / 10); }
static int64_t io_target(uint64_t m) { return (int64_t)(m * 64 / 735); }
static int64_t io_from_sub(int64_t s) { return s * 640 / 735; }      /* sub states -> I/O states (same instant) */
static int64_t sub_from_io(int64_t i) { return i * 735 / 640; }

/* ---------------------------------------------------------------- the C352 */
static void c352_catch_up(void)
{
    const uint64_t due = main_now / 1920;
    static int16_t buf[4 * 256];
    while (c352_done < due) {
        const int n = due - c352_done > 256 ? 256 : (int)(due - c352_done);
        c352_generate(&c352, buf, n);
        { void tc2_audio_push(const int16_t *, int); tc2_audio_push(buf, n); }
        c352_done += (uint64_t)n;
        { static int sl = -1; static uint64_t streak[32], total[32], maxrun[32]; static unsigned runs[32], ndone;   /* TC2_VOXSTAT=1: busy runs per voice, at exit */
          if (sl < 0) { sl = getenv("TC2_VOXSTAT") != NULL; }
          if (sl) { for (int j = 0; j < 32; j++) { if (c352.voice[j].flags & 0x8000) { streak[j] += (uint64_t)n; total[j] += (uint64_t)n; }
                       else if (streak[j]) { runs[j]++; if (streak[j] > maxrun[j]) maxrun[j] = streak[j]; streak[j] = 0; } }
                    if (++ndone % 20000 == 0) { fprintf(stderr, "[VOXSTAT] after %.0f s:", (double)c352_done / 88200); for (int j = 0; j < 32; j++) if (total[j]) fprintf(stderr, " v%d busy %.1fs runs %u max %.2fs;", j, total[j] / 88200.0, runs[j], maxrun[j] / 88200.0); fprintf(stderr, "\n"); } } }
        { static int vl = -1; static uint64_t next;                 /* TC2_VOXLOG=1: the busy voices once a second (telling music from effects) */
          if (vl < 0) vl = getenv("TC2_VOXLOG") != NULL;
          if (vl && c352_done >= next) { next = c352_done + 88200;
              for (int j = 0; j < 32; j++) { const c352_voice_t *v = &c352.voice[j];
                  if (v->flags & 0x8000) fprintf(stderr, "[VOX] t%llu v%02d bank %04X start %04X end %04X loop %04X flg %04X frq %04X vol %04X %04X cur %d %d\n", (unsigned long long)(c352_done / 88200), j, v->wave_bank, v->wave_start, v->wave_end, v->wave_loop, v->flags, v->freq, v->vol_f, v->vol_r, v->curr_vol[0], v->curr_vol[1]); } } }
    }
}

/* ---------------------------------------------------------------- the RTC-4543 (Epson): 52 bits, LSB first -- seconds, minutes, hours, weekday (4 bits),
 * day, month, year, BCD -- shifted out as the SCI clocks it (MAME rtc4543.cpp's order: the weekday's high nibble is skipped) */
static uint64_t rtc_bits; static int rtc_pos, rtc_ce;
static uint8_t bcd(int v) { return (uint8_t)((v / 10) << 4 | v % 10); }
static void rtc_latch(void)
{
    const time_t t = time(NULL); struct tm tm; localtime_r(&t, &tm);
    static const int wd[7] = { 7, 1, 2, 3, 4, 5, 6 };
    rtc_bits = (uint64_t)bcd(tm.tm_sec) | (uint64_t)bcd(tm.tm_min) << 8 | (uint64_t)bcd(tm.tm_hour) << 16 | (uint64_t)(wd[tm.tm_wday] & 15) << 24
             | (uint64_t)bcd(tm.tm_mday) << 28 | (uint64_t)bcd(tm.tm_mon + 1) << 36 | (uint64_t)bcd(tm.tm_year % 100) << 44;
    rtc_pos = 0;
}
static void rtc_select(void)
{
    const int ce = (sub_pb & 0x20) && (sub_pa & 1);
    if (ce && !rtc_ce) rtc_latch();
    rtc_ce = ce;
}

/* ---------------------------------------------------------------- the sub-CPU's board */
static uint8_t sub_port_in(void *u, int port)
{
    (void)u;
    switch (port) {
    case 6: return (uint8_t)(0xFD | (!jvs_sense_init) << 1);
    case 7: return 0;
    case 8: return sub_p8;
    case 10: return sub_pa;
    case 11: return sub_pb;
    default: return 0xFF;
    }
}
static void port_log(const char *who, int port, uint8_t data, uint8_t ddr)
{
    static int dbg = -1; static uint8_t last[2][16]; static uint8_t seen[2][16];
    if (dbg < 0) dbg = getenv("TC2_OUTLOG") != NULL;
    if (!dbg || port < 0 || port > 15) return;
    const int w = who[0] == 'i';
    if (seen[w][port] && last[w][port] == data) return;
    extern uint32_t rr_frame;
    fprintf(stderr, "[OUT] frame %u: %s port %d %02X (ddr %02X)\n", rr_frame, who, port, data, ddr);
    last[w][port] = data; seen[w][port] = 1;
}
static void sub_port_out(void *u, int port, uint8_t data, uint8_t ddr)
{
    (void)u; port_log("sub", port, data, ddr);
    if (port == 8) sub_p8 = (uint8_t)((data & ~2) | (sub_p8 & 2));
    if (port == 10) { sub_pa = data; rtc_select(); }
    if (port == 11) { sub_pb = (uint8_t)((sub_pb & 0xC0) | (data & 0x3F)); rtc_select(); }
}
static uint8_t sub_sync_rx(void *u, int ch)
{
    (void)u;
    if (ch != 1 || !rtc_ce) return 0xFF;
    uint8_t b = 0;
    for (int k = 0; k < 8; k++) {
        const int bit = rtc_pos < 52 ? (int)(rtc_bits >> rtc_pos & 1) : (int)(rtc_bits >> 51 & 1);
        b |= (uint8_t)(bit << k);
        if (rtc_pos < 52) rtc_pos++;
    }
    return b;
}
static void sub_sci_tx(void *u, int ch, uint8_t byte, int64_t at)   /* the JVS link: to the I/O board */
{
    (void)u;
    if (ch == 0) h8p_sci_receive(&iop, 0, byte, io_from_sub(at));
    if (DBG("TC2_JVSLOG")) fprintf(stderr, "[JVS] sub->io %02X frame %u line %llu\n", byte, rr_frame, (unsigned long long)line_k);
}
static void io_sci_tx(void *u, int ch, uint8_t byte, int64_t at)
{
    (void)u;
    if (ch == 0) h8p_sci_receive(&subp, 0, byte, sub_from_io(at));
    if (DBG("TC2_JVSLOG")) fprintf(stderr, "[JVS] io->sub %02X frame %u line %llu\n", byte, rr_frame, (unsigned long long)line_k);
}

static uint8_t sub_rd8(void *u, uint32_t a)
{
    (void)u;
    if (a < 0x80000) return sub_rom[a];
    if (a >= 0x80000 && a < 0x90000) return tc2_shared_ram()[a - 0x80000];
    if (a >= 0x280000 && a < 0x288000) { c352_catch_up(); const uint16_t w = c352_read(&c352, (a - 0x280000) >> 1); return (a & 1) ? (uint8_t)w : (uint8_t)(w >> 8); }
    if (a >= 0xFFFD10 && a < 0xFFFF10) return sub_onchip[a - 0xFFFD10];
    if (h8p_owns(&subp, a)) return h8p_read(&subp, a);
    return 0;
}
static uint16_t sub_rd16(void *u, uint32_t a) { return (uint16_t)(sub_rd8(u, a) << 8 | sub_rd8(u, a + 1)); }
static void sub_wr8(void *u, uint32_t a, uint8_t v)
{
    (void)u;
    if (a >= 0x80000 && a < 0x90000) { tc2_shared_ram()[a - 0x80000] = v; return; }
    if (a >= 0x280000 && a < 0x288000) { c352_catch_up(); c352_write(&c352, (a - 0x280000) >> 1, (a & 1) ? v : (uint16_t)(v << 8), (a & 1) ? 0x00FF : 0xFF00); return; }
    if (a >= 0xFFFD10 && a < 0xFFFF10) { sub_onchip[a - 0xFFFD10] = v; return; }
    if (h8p_owns(&subp, a)) { h8p_write(&subp, a, v); return; }
}
static void sub_wr16(void *u, uint32_t a, uint16_t v)
{
    if (a >= 0x280000 && a < 0x288000) { c352_catch_up(); c352_write(&c352, (a - 0x280000) >> 1, v, 0xFFFF); return; }
    if (a == 0x300020) { if (DBG("TC2_SUBIRQLOG")) fprintf(stderr, "[SUBIRQ] %04X frame %u line %llu\n", v, rr_frame, (unsigned long long)line_k); if (v == 0x3170) tc2_irq_source(1, TC2_IP_RASTER, 1); return; }   /* MAME sub_interrupt_main_w */
    if (h8p_owns(&subp, a)) { h8p_write_word(&subp, a, v); return; }
    sub_wr8(u, a, (uint8_t)(v >> 8)); sub_wr8(u, a + 1, (uint8_t)v);
}

/* ---------------------------------------------------------------- the I/O board */
#define IN_COIN 0x0001
#define IN_SERVICE 0x0004
#define IN_TEST 0x0008
#define IN_TRIGGER 0x0010
#define IN_PEDAL 0x0020
#define IN_USR_UP 0x0040
#define IN_USR_DOWN 0x0080
#define IN_USR_ENTER 0x0100
static const uint8_t io_dsw = 0xFF, io_sw = 0x03;                  /* DIP switches all off; SW2 / SW3 unpopulated (high) */
static uint8_t io_port_in(void *u, int port)
{
    (void)u;
    const int service1 = (tc2_inputs & (IN_SERVICE | IN_USR_ENTER)) != 0;
    switch (port) {
    case 4: return (uint8_t)(0x80 | (io_dsw & 3) << 5 | 0 << 4 | 1 << 3 | 1 << 2 | 1 << 1 | 1);   /* no board behind it: sense None */
    case 5: return 0xFF;
    case 6: return (uint8_t)(0x80 | 3 << 5 | ((!service1) & (io_sw >> 1 & 1)) << 4 | 3 << 2 | (io_dsw >> 2 & 1) << 1 | (io_sw & 1));
    case 9: return (uint8_t)(0x80 | 0x40 | 7 << 3 | 3 << 1 | (io_dsw >> 3 & 1));
    default: return 0xFF;
    }
}
static void io_port_out(void *u, int port, uint8_t data, uint8_t ddr)
{
    (void)u; port_log("io", port, data, ddr);
    if (port == 4) jvs_sense_init = (data >> 2) & 1;
}
static uint8_t io_switches(int n)                                  /* namcoio.cpp namco_tss_io_device::in_r with MAME's timecrs2 ports */
{
    /* JVS_PLAYER1 (timecrs2): 0x01 gun trigger, 0x02 user enter, 0x10 user up, 0x20 user down, 0x8000 foot pedal, 0x4000 link ID (0 = left/red);
     * in_r: 0x6000 = ~(enter << 7 | up << 6 | down << 5 | test << 4 | counter connected << 2 | coin), 0x6001 = ~(pedal << 1 | trigger) */
    const uint16_t in = tc2_inputs;
    if (n == 0)
        return (uint8_t)~((!!(in & IN_USR_ENTER)) << 7 | (!!(in & IN_USR_UP)) << 6 | (!!(in & IN_USR_DOWN)) << 5 | (!!(in & IN_TEST)) << 4
                        | 1 << 2 /* the coin counter is connected */ | (!!(in & IN_COIN)));
    return (uint8_t)~((!!(in & IN_PEDAL)) << 1 | (!!(in & IN_TRIGGER)));
}
static uint8_t io_gun(int off)                                      /* gun_r: X, Y, Y + 1 -- low bytes at 0..2, high bytes at 3..5 */
{
    const uint16_t x = g_ss22_gun_off ? 0 : g_ss22_gun_x, y = g_ss22_gun_off ? 0 : g_ss22_gun_y;
    const uint16_t d = (off % 3) == 0 ? x : (off % 3) == 1 ? y : (uint16_t)(y + 1);
    return off < 3 ? (uint8_t)d : (uint8_t)(d >> 8);
}
static uint8_t io_rd8(void *u, uint32_t a)
{
    (void)u;
    if (a < 0x4000) return io_rom[a];
    if (a >= 0xC000 && a < 0xFF80) return io_ram[a - 0xC000];
    if (a == 0x6000 || a == 0x6001) { uint8_t v = io_switches(a & 1); if (DBG("TC2_SWLOG") && rr_frame >= 598 && rr_frame <= 645) fprintf(stderr, "[SW] %04X = %02X frame %u\n", (unsigned)a, v, rr_frame); return v; }
    if (a >= 0x7000 && a <= 0x7005) return io_gun((int)(a - 0x7000));
    if (a == 0x7007) return 0x00;                               /* the gun's acknowledge (MAME namco_tss_io_device::gun_ack_r) */
    if (h8p_owns(&iop, a)) { uint8_t v = h8p_read(&iop, a); if (DBG("TC2_SWLOG") && (a == 0xFFB7 || a == 0xFFBB) && rr_frame >= 598 && rr_frame <= 645) fprintf(stderr, "[SW] %04X = %02X frame %u\n", (unsigned)a, v, rr_frame); return v; }
    return 0xFF;
}
static uint16_t io_rd16(void *u, uint32_t a) { return (uint16_t)(io_rd8(u, a) << 8 | io_rd8(u, a + 1)); }
static void io_wr8(void *u, uint32_t a, uint8_t v)
{
    (void)u;
    if (a >= 0xC000 && a < 0xFF80) { io_ram[a - 0xC000] = v; return; }
    if (a == 0x6002) {
        static int dbg = -1; if (dbg < 0) dbg = getenv("TC2_OUTLOG") != NULL;
        if (dbg && v != io_out) { extern uint32_t rr_frame; fprintf(stderr, "[OUT] frame %u: 0x6002 %02X -> %02X\n", rr_frame, io_out, v); }
        io_out = v; return;
    }
    if (h8p_owns(&iop, a)) h8p_write(&iop, a, v);
}
static void io_wr16(void *u, uint32_t a, uint16_t v)
{
    if (h8p_owns(&iop, a)) { h8p_write_word(&iop, a, v); return; }
    io_wr8(u, a, (uint8_t)(v >> 8)); io_wr8(u, a + 1, (uint8_t)v);
}

void h8_untranslated(h8_cpu *c)
{
    if (tc2_h8_untranslated++ < 20) fprintf(stderr, "[H8] %s: UNTRANSLATED code at %06X -- the CPU stops here\n", c == &sub ? "sub-CPU" : "I/O board", c->pc);
    c->sleeping = 1; c->budget = 0;
    c->ccr |= H8_I | H8_UI;                                        /* and stays stopped: no interrupt wakes it */
}

/* ---------------------------------------------------------------- running */
static unsigned long n_irq[2][64];
static void run_to(h8_cpu *c, h8p *p, int64_t target, void (*run)(h8_cpu *))
{
    while (c->cycles < target) {
        h8p_sync(p);
        int ui; const int v = h8p_irq(p, &ui);
        if (v >= 0) { h8p_irq_taken(p, v); h8_interrupt(c, v, ui); n_irq[c == &io][v & 63]++; }
        int64_t n = h8p_next_event(p);
        if (n > target - c->cycles) n = target - c->cycles;
        if (c->sleeping) { c->cycles += n; continue; }
        c->budget = (int32_t)n;
        run(c);
    }
    h8p_sync(p);
}

/* THE SUB-CPU'S IRQ1 (port 8 bit 1), as MAME's System 23 driver makes it: two timers from power-on (the beam at line 480, pixel 0) --
 * every scan line the pin is asserted if the beam is above line 72; and it is released first at time_until_pos(0, 32) -- line 0, PIXEL 32:
 * 45 lines and 32 pixels on -- then every 46 lines and 32 pixels (that time plus one line). The program senses the pin's edges (ISCR), so
 * it interrupts at line 0 of each frame and again whenever a release falls in lines 0-71: about 2.5 times a frame -- measured on MAME
 * (a breakpoint on the handler: 3 2 3 2 2 3 2 ... a frame), the rate the sub-CPU's delay counts (0x084052) run at.
 * Time in pixel clocks (25.6 MHz; a line is 814) = main cycles * 200 / 1323. */
static int irq1_pin;
static uint64_t px_cycle(uint64_t px) { return (px * 1323u + 199u) / 200u; }   /* the first main cycle at or after pixel clock px */
static uint64_t off_j;                                   /* the next release (j-th) */
#define OFF_PX(j) (45u * 814u + 32u + (j) * (46u * 814u + 32u))
static void irq1_set(int on)
{
    irq1_pin = on;
    sub_p8 = on ? (uint8_t)(sub_p8 & ~2) : (uint8_t)(sub_p8 | 2);
    if (sub_running) { h8p_sync(&subp); h8p_set_irq_pin(&subp, 1, on); }
}
static void run_all(uint64_t main_cycles)
{
    if (io_ok) run_to(&io, &iop, io_target(main_cycles), tc2io_run);
    if (sub_ok) {
        if (sub_running) run_to(&sub, &subp, sub_target(main_cycles), tc2sub_run);
        else { sub.cycles = sub_target(main_cycles); subp.now = sub.cycles; }
    }
}
void tc2_h8_run(uint64_t main_cycles)
{
    if (!sub_ok && !io_ok) return;
    for (;;) {
        const uint64_t on_px = line_k * 814u, off_px = OFF_PX(off_j);
        const uint64_t at_px = on_px < off_px ? on_px : off_px, at = px_cycle(at_px);
        if (at > main_cycles) break;
        main_now = at; run_all(at);
        if (on_px <= off_px) {                            /* subcpu_scanline_on_tick (first, at the same instant) */
            if ((480 + line_k) % 525 < 72) irq1_set(1);
            line_k++;
        } else { irq1_set(0); off_j++; }                  /* subcpu_scanline_off_tick */
    }
    main_now = main_cycles;
    run_all(main_cycles);
    c352_catch_up();
}

void tc2_h8_report(void)
{
    for (int k = 0; k < 2; k++) {
        const h8_cpu *c = k ? &io : &sub;
        fprintf(stderr, "[H8] %s: PC %06X CCR %02X%s, %lld states; interrupts:", k ? "I/O board" : "sub-CPU", c->pc, c->ccr, c->sleeping ? " (sleeping)" : "", (long long)c->cycles);
        for (int v = 0; v < 64; v++) if (n_irq[k][v]) fprintf(stderr, " %d:%lu", v, n_irq[k][v]);
        fprintf(stderr, "\n");
    }
}

/* the screen's vertical blank (src/tc2_screen.c): the sub-CPU's IRQ1 pin and port B bit 7 */
void tc2_h8_vblank(int on)
{
    if (!sub_ok) return;
    if (on && DBG("TC2_H8TRACE")) { static int every; if (!every) every = atoi(getenv("TC2_H8TRACE")); if (every && rr_frame % (unsigned)every == 0)
        fprintf(stderr, "[H8T] frame %u sub PC %06X ccr %02X%s io PC %04X sense %d irq 13:%lu 26:%lu 53:%lu io28:%lu\n", rr_frame, sub.pc, sub.ccr, sub_running ? "" : " (held)", io.pc, jvs_sense_init,
                n_irq[0][13], n_irq[0][26], n_irq[0][53], n_irq[1][28]); }
    sub_pb = (uint8_t)((sub_pb & 0x7F) | (on ? 0x80 : 0));   /* port B bit 7 = vblank (the IRQ1 pin is the scan-line timers, above) */
}

/* the main CPU's side: MAME mcuen_w (0x04C3FF00-0F) */
int tc2_h8_main_write(uint32_t p, int size, uint32_t v)
{
    if (p < 0x04C3FF00u || p > 0x04C3FF0Fu) return 0;
    (void)size;
    const uint32_t off = (p - 0x04C3FF00u) >> 1;
    if (off == 2) tc2_irq_source(1, TC2_IP_RASTER, 0);               /* the sub-CPU's interrupt acknowledged */
    if (off == 5 && sub_ok) {
        if (v & 0xFFFF) {                                            /* (re)start: a reset, then it runs */
            sub.cycles = sub_target(main_now);
            h8_reset(&sub); h8p_reset(&subp);
            if (irq1_pin) subp.irq_pin |= 2;                         /* the pin as it is: no edge from the reset itself */
            sub_running = 1;
            fprintf(stderr, "[H8] sub-CPU started (main frame %u)\n", rr_frame);
        } else { sub_running = 0; fprintf(stderr, "[H8] sub-CPU stopped\n"); }
    }
    return 1;
}

static int load(const char *path, uint8_t *dst, size_t n)
{
    FILE *f = fopen(path, "rb"); if (!f) return 0;
    const size_t got = fread(dst, 1, n, f); fclose(f);
    return got > 0;
}
void tc2_h8_init(const char *extracted)
{
    int tc2_rom_sub(const char *, uint8_t *, size_t), tc2_rom_io(const char *, uint8_t *, size_t);
    if (tc2_rom_sub(extracted, sub_rom, sizeof sub_rom)) {
        sub.advanced = 1; sub.amask = 0xFFFFFF; sub.rd8 = sub_rd8; sub.rd16 = sub_rd16; sub.wr8 = sub_wr8; sub.wr16 = sub_wr16;
        h8p_init(&subp, H8P_3002, &sub, NULL);
        subp.port_in = sub_port_in; subp.port_out = sub_port_out; subp.sci_tx = sub_sci_tx; subp.sci_sync_rx = sub_sync_rx;
        subp.sci[0].ext_bit = 147;                                   /* 16.9344 MHz / 115200 */
        h8p_reset(&subp);
        sub_ok = 1;
    } else fprintf(stderr, "[H8] no tss1vera.3: no sub-CPU\n");
    if (tc2_rom_io(extracted, io_rom, sizeof io_rom)) {
        io.advanced = 0; io.amask = 0xFFFF; io.rd8 = io_rd8; io.rd16 = io_rd16; io.wr8 = io_wr8; io.wr16 = io_wr16;
        h8p_init(&iop, H8P_3334, &io, NULL);
        iop.port_in = io_port_in; iop.port_out = io_port_out; iop.sci_tx = io_sci_tx;
        h8p_reset(&iop);
        h8_reset(&io); io.ccr = H8_I;
        io_ok = 1;
    } else fprintf(stderr, "[H8] no tssioprog.ic3: no I/O board\n");
    wave = calloc(1, 0x1000000);
    char p[1024];
    snprintf(p, sizeof p, "%s/tss1wavel.2c", extracted); if (!load(p, wave, 0x800000)) fprintf(stderr, "[H8] cannot read %s: the C352 is silent\n", p);
    snprintf(p, sizeof p, "%s/tss1waveh.2a", extracted); if (!load(p, wave + 0x800000, 0x800000)) fprintf(stderr, "[H8] cannot read %s\n", p);
    c352_init(&c352, wave, 0x1000000);
}
