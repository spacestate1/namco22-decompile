/* Regression: the recorded sub-CPU instruction at 0x5942 must execute. */
#include <stdio.h>
#include "h8_sem.h"

static int missing;
void h8_untranslated(h8_cpu *cpu) { (void)cpu; missing = 1; }
void tc2sub_run(h8_cpu *cpu);

int main(void) {
    h8_cpu cpu = {0};
    cpu.pc = 0x5942;
    cpu.advanced = 1;
    cpu.amask = 0xffffff;
    cpu.er[4] = 0x1234ffff;
    cpu.budget = 1; /* one instruction, before the following memory write */
    tc2sub_run(&cpu);
    if (missing || cpu.pc != 0x5946 || cpu.er[4] != 0x12340000 || !(cpu.ccr & H8_Z)) {
        fprintf(stderr, "recorded instruction unavailable or wrong: missing=%d pc=%06x er4=%08x ccr=%02x\n", missing, cpu.pc, cpu.er[4], cpu.ccr);
        return 1;
    }
    puts("recorded H8 instruction executes correctly");
    return 0;
}
