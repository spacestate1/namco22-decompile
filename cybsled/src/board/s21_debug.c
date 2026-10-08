/*
 * s21_debug.c -- the DEVELOPER TEST MODE left in both 68000 programs of Cyber Sled (PLAN.md "Module I -- debug screens").
 *
 * Both programs still carry it whole, but the release unhooked it at the two dispatches that reached it:
 *   master 0x2056..0x206C   the test-mode loop: d0 = (DSW byte $A00026 & 0x10) >> 2 ; jsr ([0x20CA + d0]) -- the table holds
 *                           [0x4008, 0x4008]: both DSW5 settings run the OPERATOR test mode (0x4008). The developer test mode's
 *                           own dispatcher, 0x2BB0 (pages through 0x2BC8 on 0x100C14), sits right after it and nothing names it.
 *   slave  0x1BE2..0x1BFA   the per-frame mode dispatch: jsr ([0x1C5C + (shared 0x900014 & 15) * 4]). The developer pages are the
 *                           table just before it, 0x1C24 (8 entries: menu, frame buffer object test, polygon object test, switch
 *                           test, adjuster, sound test, vehicle test, colour check), which nothing indexes.
 * With the developer mode on (s21_debug_enable), the lifted programs' entry hooks (s21_cpuN_hooks, s21_cpu_glue.c rd_hook) run
 *   master: 0x2BB0 where the test-mode loop enters 0x4008 -- i.e. what a DSW5 slot naming 0x2BB0 would run;
 *   slave:  0x1C24[i] where the mode dispatch enters 0x1C5C[i], i = the mode the master's developer page writes (0..7), once the
 *           master has entered 0x2BB0 and while the board is in test mode (shared 0x900010 == 2).
 * Nothing in the ROM image is changed and nothing is jumped to from outside a CPU: the substitution happens as the game itself
 * calls the routine, on that CPU's own coroutine, and the replacement's rts returns to the same caller.
 */
#include <stdio.h>
#include "s21_board.h"
#include "s21_cpu.h"
#include "s21_debug.h"

static int dbg_on, dev_entered;

static uint32_t srom32(uint32_t a) { return (uint32_t)g_s21.srom[a >> 1] << 16 | g_s21.srom[(a >> 1) + 1]; }

void s21_debug_enable(int on)
{
    dbg_on = on;
    if (!on) dev_entered = 0;
    s21_cpu0_hooks(on);
    s21_cpu1_hooks(on);
}
int s21_debug_enabled(void) { return dbg_on; }
int s21_debug_dev_entered(void) { return dev_entered; }

int s21_dbg_sub(int cpu, uint32_t entry, uint32_t *alt)
{
    if (!dbg_on) return 0;
    if (cpu == 0) {
        if (entry != 0x4008) return 0;                                  /* only the test-mode loop's jsr (a0) at 0x206C enters 0x4008 */
        if (!dev_entered) fprintf(stderr, "[DEBUG] developer test mode: master 0x2BB0 (frame %u)\n", g_s21.frame);
        dev_entered = 1;
        *alt = 0x2BB0;
        return 1;
    }
    if (!dev_entered || g_s21.shared[0x10 >> 1] != 2) return 0;
    const unsigned i = g_s21.shared[0x14 >> 1] & 15;
    if (i >= 8 || entry != srom32(0x1C5C + i * 4)) return 0;
    *alt = srom32(0x1C24 + i * 4);
    return 1;
}
