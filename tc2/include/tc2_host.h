/* tc2_host.h -- what the lifted program's runtime (src/tc2_cpu.c) needs from the Time Crisis 2 host (src/tc2_mem.c, src/tc2_main.c) */
#ifndef TC2_HOST_H
#define TC2_HOST_H
#include <stdint.h>
/* Ghidra's MIPS register space (decompiled/pcode.txt "R" lines): 64-bit registers, the address in the low half */
#define TC2_R_SP      0xE8
#define TC2_R_COUNT   0x2048
#define TC2_R_COMPARE 0x2058
#define TC2_R_STATUS  0x2060
#define TC2_R_CAUSE   0x2068
#define TC2_R_EPC     0x2070
#define TC2_R_ERROREPC 0x20F0              /* CP0 30 */
#define TC2_IP_VBLANK 0x400              /* Cause.IP2 = the CPU's Int0: vertical blank (handler table 0x802D3060 slot 0x0C -> 0x800007B8) */
#define TC2_IP_RASTER 0x800              /* Cause.IP3 = the C361 raster interrupt (src/tc2_screen.c; shared with the sub-CPU): handler 0x80000824 -> 0x80002A78, which reads the beam at 0x0682000A (that clears it) */
#define TC2_SLICE     1000                 /* instructions between scheduler polls */
#define TC2_CLOCK     169344000u           /* the R4650 pipeline clock; cycles as MAME charges them (tools/lift/lift.py MIPS_EXTRA) */
void tc2_host_step(uint32_t instructions);  /* time passed: devices, frames */
uint32_t tc2_until_event(void);            /* instructions to the next interrupt event: the scheduler polls right there */
void tc2_trace_mark(const char *what, uint32_t v);
void tc2_on_trap(void);
void tc2_on_exit(void);
void tc2_advance_count(uint32_t instructions);
void tc2_poll_irq(void);
void tc2_irq_line(uint32_t bit, int on);   /* drive one of the CPU's interrupt pins: Cause.IP2..IP7 (src/tc2_cpu.c) */
int  tc2_irq_pending(uint32_t bit);                  /* take a pending, enabled interrupt now (src/tc2_cpu.c) */
extern uint32_t rr_frame, rr_n_traps, rr_pc;
extern uint64_t rr_n_irq;
#endif
