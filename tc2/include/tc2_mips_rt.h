/* tc2_mips_rt.h -- the MIPS-only part of the lifted runtime ABI (the rest is include/lift_rt.h): the userops lift.py emits as
 * rr_mips_<name>(pc, args...) for Ghidra's CALLOTHER (CP0 moves, cache ops, the ISA-mode hint, trap). Implemented by the TC2 host. */
#ifndef TC2_MIPS_RT_H
#define TC2_MIPS_RT_H
#include <stdint.h>
void     rr_mips_setCopReg(uint32_t pc, uint64_t sel, uint64_t reg, uint64_t val);   /* mtc0 to a register Ghidra did not name (not seen) */
void     rr_mips_cop0_write(uint32_t pc, uint32_t off);   /* mtc0 stored into R[off] (Count 0x2048, Compare 0x2058, Status 0x2060, Cause 0x2068, EPC 0x2070) */
void     rr_mips_cacheOp(uint32_t pc, uint64_t op, uint64_t addr);                    /* cache */
void     rr_mips_setISAMode(uint32_t pc, uint64_t mode);                               /* jalr/jr's mips16 hint: no-op on the R4650 */
void     rr_mips_trap(uint32_t pc, uint64_t code);                                     /* break / trap */
void     rr_mips_SYNC(uint32_t pc, uint64_t stype);                                    /* sync: a memory barrier -- nothing to order in a single-threaded host */
#endif
