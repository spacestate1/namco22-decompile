/* Time Crisis 2's own copy (independent of the System 22 project it was taken from, 2026-10-04). */
/*
 * lift_rt.h -- runtime for the LIFTED 68K programs of every System 22 game
 * (tools/namco22/lift.py). Shared: was raverace/include/rr_lift_rt.h.
 *
 * The generated code's ABI is the rr_* one: R[], rr_read/rr_write (the game's memory
 * map, declared here), the shadow return stack and interrupt entry (engine/lift_cpu.c).
 * Rave Racer's rr_lift_rt.h is now this file plus rr_mem.h.
 *
 * R[] is Ghidra's 68020 register space as big-endian bytes: D0 at 0x00,
 * A0 at 0x20, SP (A7) at 0x3C, the flag bytes at 0x40.., PC 0x50, SR 0x200,
 * ISP/MSP/VBR/CACR at 0x100.. -- every varnode is a BE load/store at its
 * offset, which makes sub-register writes (D0w, D0b) exact.
 */
#ifndef LIFT_RT_H
#define LIFT_RT_H
#include <stdint.h>

/* the game's memory map: every 68K access in the generated code lands here, big-endian, at the
 * original address and the width the instruction used (size 1, 2 or 4) */
uint32_t rr_read(uint32_t a, int size);
void     rr_write(uint32_t a, int size, uint32_t v);

#ifndef RR_REGSPACE
#define RR_REGSPACE 0x4100     /* Ghidra's MIPS register space ends at 0x4004 (contextreg) */
#endif
extern uint8_t R[RR_REGSPACE];
extern int32_t rr_budget;

/* the register file is BIG-ENDIAN bytes (Ghidra's register space layout); a load / store is one byte-swapped access (TC2: was byte by byte --
 * 22% of a run in perf) */
static inline uint64_t RG1(uint32_t o) { return R[o]; }
static inline uint64_t RG2(uint32_t o) { uint16_t v; __builtin_memcpy(&v, R + o, 2); return __builtin_bswap16(v); }
static inline uint64_t RG4(uint32_t o) { uint32_t v; __builtin_memcpy(&v, R + o, 4); return __builtin_bswap32(v); }
static inline uint64_t RG8(uint32_t o) { uint64_t v; __builtin_memcpy(&v, R + o, 8); return __builtin_bswap64(v); }
static inline void RS1(uint32_t o, uint64_t v) { R[o] = (uint8_t)v; }
static inline void RS2(uint32_t o, uint64_t v) { const uint16_t w = __builtin_bswap16((uint16_t)v); __builtin_memcpy(R + o, &w, 2); }
static inline void RS4(uint32_t o, uint64_t v) { const uint32_t w = __builtin_bswap32((uint32_t)v); __builtin_memcpy(R + o, &w, 4); }
static inline void RS8(uint32_t o, uint64_t v) { const uint64_t w = __builtin_bswap64(v); __builtin_memcpy(R + o, &w, 8); }
/* 80-bit extended-precision registers: 68881 opcodes Ghidra reads out of DATA regions (a 68EC020 has no FPU and the traces never execute them);
 * kept as their low 64 bits so the file compiles, not as floating point. */
static inline uint64_t RG10(uint32_t o) { return RG8(o + 2); }
static inline void RS10(uint32_t o, uint64_t v) { R[o] = 0; R[o + 1] = 0; RS8(o + 2, v); }

#include <string.h>
/* MAIN RAM INLINE (2026-10-08): rr_read / rr_write were ~12% of a run, nearly all of it main RAM; their first branch is exactly this
 * (the 28-bit physical address inside the 16 MB, big-endian), so taking it here changes nothing and skips the call + byte loop. */
extern uint8_t tc2_ram[0x1000000];
static inline uint32_t MRD1(uint32_t a) { const uint32_t p = a & 0x0FFFFFFFu; if (p < 0x1000000u) return tc2_ram[p]; return rr_read(a, 1); }
static inline uint32_t MRD2(uint32_t a) { const uint32_t p = a & 0x0FFFFFFFu; if (p + 2 <= 0x1000000u) { uint16_t v; memcpy(&v, tc2_ram + p, 2); return __builtin_bswap16(v); } return rr_read(a, 2); }
static inline uint32_t MRD4(uint32_t a) { const uint32_t p = a & 0x0FFFFFFFu; if (p + 4 <= 0x1000000u) { uint32_t v; memcpy(&v, tc2_ram + p, 4); return __builtin_bswap32(v); } return rr_read(a, 4); }
static inline uint64_t MRD8(uint32_t a) { return (uint64_t)MRD4(a) << 32 | MRD4(a + 4); }
static inline void MWR1(uint32_t a, uint64_t v) { const uint32_t p = a & 0x0FFFFFFFu; if (p < 0x1000000u) { tc2_ram[p] = (uint8_t)v; return; } rr_write(a, 1, (uint32_t)v); }
static inline void MWR2(uint32_t a, uint64_t v) { const uint32_t p = a & 0x0FFFFFFFu; if (p + 2 <= 0x1000000u) { const uint16_t w = __builtin_bswap16((uint16_t)v); memcpy(tc2_ram + p, &w, 2); return; } rr_write(a, 2, (uint32_t)v); }
static inline void MWR4(uint32_t a, uint64_t v) { const uint32_t p = a & 0x0FFFFFFFu; if (p + 4 <= 0x1000000u) { const uint32_t w = __builtin_bswap32((uint32_t)v); memcpy(tc2_ram + p, &w, 4); return; } rr_write(a, 4, (uint32_t)v); }
static inline void MWR8(uint32_t a, uint64_t v) { MWR4(a, v >> 32); MWR4(a + 4, v); }

static inline int64_t SX1(uint64_t v) { return (int8_t)v; }
/* FLOATING POINT (Ghidra's FLOAT_* p-code; the 68K games never use it, the MIPS R4650's FPU does): a varnode holds the IEEE bits, 4 bytes =
 * float, 8 = double. */
#include <string.h>
#include <math.h>
static inline float    rr_u2f(uint64_t v) { uint32_t u = (uint32_t)v; float f; memcpy(&f, &u, 4); return f; }
static inline double   rr_u2d(uint64_t v) { double d; memcpy(&d, &v, 8); return d; }
static inline uint64_t rr_f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static inline uint64_t rr_d2u(double d) { uint64_t u; memcpy(&u, &d, 8); return u; }
static inline double   rr_fv(uint64_t v, int sz) { return sz == 4 ? (double)rr_u2f(v) : rr_u2d(v); }      /* value as double (exact for float) */
static inline uint64_t rr_fb(double d, int sz) { return sz == 4 ? rr_f2u((float)d) : rr_d2u(d); }       /* double -> the size's bits */
/* THE R4650 FPU'S EXCEPTIONS (IDT RC4650 datasheet DSC 3149/3; IDT MIPS software reference manual ch. 8). FCSR (Ghidra's fcsr, R[0x1210]):
 * flags 2-6, enables 7-11, cause 12-16 (each I U O Z V), cause bit 17 = E "unimplemented operation" (no enable: it ALWAYS traps), FS bit 24.
 * Every FP arithmetic op sets the cause field to its own exceptions; an exception whose enable is set (or E) traps, otherwise its flag is
 * set. E: a denormal operand, and (with its enable off) an invalid op or a convert-to-integer overflow -- the chip leaves those to software.
 * The enables are the game's: MAME's FCSR during play is 0x01000000 (FS only; _start's 0x01000F00 is guarded by Status.CU1, and the
 * interrupt handler masks the enables around itself), so in play only E can trap -- a denormal operand, a trunc.w of a NaN / out-of-range
 * value. Its FPE path (0x80000358) hands the exception to the boot monitor's debugger: on the board an FPE FREEZES the game. So the RESULT here stays what MAME computes (host IEEE single precision: MAME's core is
 * the oracle; its only FPE is div.s by zero with Z enabled), and the CLASSIFICATION is the R4650's: FCSR's cause/flags as the chip sets them,
 * and rr_fpe() reports every op the board would have trapped on. Single precision only: the program has no double-precision op (measured). */
#define RR_FCSR 0x1210
#define FPX_I 0x01000u
#define FPX_U 0x02000u
#define FPX_O 0x04000u
#define FPX_Z 0x08000u
#define FPX_V 0x10000u
#define FPX_E 0x20000u
void rr_fpe(uint32_t cause, uint32_t fcsr, char op);   /* the runtime: an op the R4650 traps on */
static inline void rr_fp_cause(uint32_t c, char op)
{
    uint32_t f = (uint32_t)RG4(RR_FCSR);
    const int trap = (c & FPX_E) || ((c >> 5) & f & 0xF80u);
    f = (f & ~0x3F000u) | c;
    if (!trap) f |= (c >> 10) & 0x7Cu;           /* a trapped exception sets its cause, not its flag */
    RS4(RR_FCSR, f);
    if (trap) rr_fpe(c, f, op);
}
static inline int rr_fnan(uint32_t u)  { return (u & 0x7F800000u) == 0x7F800000u && (u & 0x7FFFFFu); }
static inline int rr_fsnan(uint32_t u) { return rr_fnan(u) && (u & 0x400000u); }  /* MIPS legacy NaNs: mantissa MSB SET = signalling */
static inline int rr_fden(uint32_t u)  { return !(u & 0x7F800000u) && (u & 0x7FFFFFu); }
/* U and I from the unrounded value: tiny = non-zero below 2^-126 (traps when U is enabled; otherwise a flag only if also inexact) */
static inline uint32_t rr_fp_round(int inexact, double exact, float r)
{
    uint32_t c = inexact ? FPX_I : 0;
    if (isinf(r)) return FPX_O | FPX_I;
    if (exact != 0 && fabs(exact) < 1.1754943508222875e-38) c |= FPX_U;
    return c;
}
static inline uint32_t rr_fp_cause2(uint32_t ua, uint32_t ub, char op, float r)
{
    if (rr_fden(ua) || rr_fden(ub)) return FPX_E;
    if (rr_fsnan(ua) || rr_fsnan(ub)) return FPX_V;
    if (rr_fnan(ua) || rr_fnan(ub)) return 0;                                 /* a quiet NaN propagates */
    const float x = rr_u2f(ua), y = rr_u2f(ub);
    const int ix = isinf(x), iy = isinf(y);
    switch (op) {
    case '+': case '-': {
        const double yy = op == '+' ? (double)y : -(double)y;
        if (ix && iy && (x > 0) != (yy > 0)) return FPX_V;                    /* inf - inf */
        if (ix || iy) return 0;
        const double s = (double)x + yy, bb = s - x, err = ((double)x - (s - bb)) + (yy - bb);   /* TwoSum: s + err is exact */
        return rr_fp_round(!((double)r == s && err == 0), s, r);
    }
    case '*': {
        if ((ix && y == 0) || (iy && x == 0)) return FPX_V;                   /* 0 * inf */
        if (ix || iy) return 0;
        const double p = (double)x * (double)y;                               /* exact: 24 x 24 bits */
        return rr_fp_round((double)r != p, p, r);
    }
    default: {   /* '/' */
        if ((x == 0 && y == 0) || (ix && iy)) return FPX_V;                   /* 0/0, inf/inf */
        if (y == 0) return ix ? 0 : FPX_Z;                                    /* finite / 0 (MAME raises this one too) */
        if (ix || iy) return 0;
        const double q = (double)x / (double)y;
        return rr_fp_round(!isinf(r) && (double)r * (double)y != (double)x, q, r);   /* r*y is exact (48 bits) */
    }
    }
}
static inline uint64_t FOP2(int sz, uint64_t a, uint64_t b, char op)
{
    if (sz != 4) {
        const double x = rr_u2d(a), y = rr_u2d(b);
        return rr_d2u(op == '+' ? x + y : op == '-' ? x - y : op == '*' ? x * y : x / y);
    }
    const float x = rr_u2f(a), y = rr_u2f(b);
    const float r = op == '+' ? x + y : op == '-' ? x - y : op == '*' ? x * y : x / y;
    rr_fp_cause(rr_fp_cause2((uint32_t)a, (uint32_t)b, op, r), op);
    return rr_f2u(r);
}
#define FOP2(sz, a, b, op) FOP2((sz), (a), (b), (#op)[0])
/* compares: Ghidra makes c.lt / c.le FLOAT_LESS / FLOAT_LESSEQUAL and c.eq FLOAT_EQUAL; this program has no other compare (measured: only
 * functions 0x32 c.eq, 0x3C c.lt, 0x3E c.le), so < and <= are the SIGNALLING compares (V on any NaN) and == the quiet one (V on a signalling NaN).
 * Ghidra's p-code reads fcsr BEFORE the compare and writes it back (with C, bit 23) after, so a compare's cause bits are lost again; the
 * trap check and report happen here first, and nothing reads a compare's cause bits except an FPE handler that would not return anyway. */
static inline uint64_t FCMP(int sz, uint64_t a, uint64_t b, char op)
{
    if (sz != 4) { const double x = rr_u2d(a), y = rr_u2d(b); return op == '<' ? x < y : op == 'l' ? x <= y : op == '=' ? x == y : x != y; }
    const float x = rr_u2f(a), y = rr_u2f(b);
    const uint32_t ua = (uint32_t)a, ub = (uint32_t)b;
    const int any = rr_fnan(ua) || rr_fnan(ub), sig = rr_fsnan(ua) || rr_fsnan(ub);
    uint32_t f = (uint32_t)RG4(RR_FCSR), c = (op == '<' || op == 'l') ? (any ? FPX_V : 0) : (sig ? FPX_V : 0);
    if (c) rr_fp_cause(c, 'c');
    else RS4(RR_FCSR, f & ~0x3F000u);
    return op == '<' ? x < y : op == 'l' ? x <= y : op == '=' ? x == y : x != y;
}
#define FCMP(sz, a, b, op) FCMP((sz), (a), (b), (#op)[0] == '<' && (#op)[1] == '=' ? 'l' : (#op)[0] == '!' ? '!' : (#op)[0])
static inline uint64_t FNEG(int sz, uint64_t a) { return sz == 4 ? (a ^ 0x80000000ULL) : (a ^ 0x8000000000000000ULL); }
static inline uint64_t FABS(int sz, uint64_t a) { return sz == 4 ? (a & 0x7FFFFFFFULL) : (a & 0x7FFFFFFFFFFFFFFFULL); }
static inline uint64_t FSQRT(int sz, uint64_t a)
{
    if (sz != 4) return rr_d2u(sqrt(rr_u2d(a)));
    const uint32_t u = (uint32_t)a; const float x = rr_u2f(a), r = sqrtf(x);
    uint32_t c;
    if (rr_fden(u)) c = FPX_E;
    else if (rr_fsnan(u)) c = FPX_V;
    else if (rr_fnan(u) || isinf(x) || x == 0) c = 0;
    else if (x < 0) c = FPX_V;                                                /* sqrt of a negative */
    else c = ((double)r * (double)r != (double)x) ? FPX_I : 0;
    rr_fp_cause(c, 'q');
    return rr_f2u(r);
}
static inline uint64_t FNAN(int sz, uint64_t a) { return sz == 4 ? (uint64_t)isnan(rr_u2f(a)) : (uint64_t)isnan(rr_u2d(a)); }
static inline uint64_t FCONV(int isz, uint64_t a, int osz) { return rr_fb(rr_fv(a, isz), osz); }           /* FLOAT2FLOAT (not in this program) */
static inline uint64_t FFROMI(int64_t i, int osz)                                                         /* INT2FLOAT: cvt.s.w -- inexact only */
{
    if (osz != 4) return rr_d2u((double)i);
    const float r = (float)i;
    rr_fp_cause((int64_t)r != i ? FPX_I : 0, 'w');
    return rr_f2u(r);
}
/* TRUNC (trunc.w.s): a NaN or an out-of-range value is invalid (V; with V disabled the chip leaves it to software, E). The value given is
 * MAME's: (int32_t) of the double, which x86 makes the "integer indefinite" 0x80000000 */
static inline uint64_t FTRUNC(int isz, uint64_t a, int osz)
{
    const double d = rr_fv(a, isz);
    const double hi = osz == 4 ? 2147483648.0 : 9223372036854775808.0, lo = osz == 4 ? -2147483648.0 : -9223372036854775808.0;
    if (isz == 4 && osz == 4) {
        const uint32_t u = (uint32_t)a, f = (uint32_t)RG4(RR_FCSR);
        uint32_t c;
        if (rr_fden(u)) c = FPX_E;
        else if (d != d || d >= hi || d < lo) c = (f & 0x800u) ? FPX_V : FPX_E;
        else c = (double)(int64_t)d != d ? FPX_I : 0;
        rr_fp_cause(c, 't');
    }
    if (d != d || d >= hi || d < lo) return osz == 4 ? 0x80000000ULL : 0x8000000000000000ULL;
    return osz == 4 ? (uint64_t)(uint32_t)(int32_t)d : (uint64_t)(int64_t)d;
}
#define FROUNDOP(sz, a, fn) ((sz) == 4 ? rr_f2u(fn##f(rr_u2f(a))) : rr_d2u(fn(rr_u2d(a))))

static inline int64_t SX2(uint64_t v) { return (int16_t)v; }
static inline int64_t SX4(uint64_t v) { return (int32_t)v; }
static inline int64_t SX8(uint64_t v) { return (int64_t)v; }
static inline int64_t SXN(uint64_t v, int sz) {
    return sz == 1 ? SX1(v) : sz == 2 ? SX2(v) : sz == 4 ? SX4(v) : (int64_t)v;
}
static inline uint64_t MASKN(uint64_t v, int sz) { return sz >= 8 ? v : v & ((1ULL << (8 * sz)) - 1); }

/* p-code shifts: an amount >= the operand width gives 0 (or the sign) */
static inline uint64_t SHL(uint64_t a, uint64_t n, int sz) { return n >= (uint64_t)(8 * sz) ? 0 : MASKN(a << n, sz); }
static inline uint64_t SHR(uint64_t a, uint64_t n, int sz) { return n >= (uint64_t)(8 * sz) ? 0 : MASKN(a, sz) >> n; }
static inline uint64_t SAR(uint64_t a, uint64_t n, int sz) {
    int64_t s = SXN(a, sz);
    return (uint64_t)(n >= (uint64_t)(8 * sz) ? (s < 0 ? -1 : 0) : (s >> n));
}
static inline uint64_t CARRY(uint64_t a, uint64_t b, int sz) {
    return (MASKN(a, sz) + MASKN(b, sz)) >> (8 * sz) & 1 ? 1 : (sz == 8 && MASKN(a, 8) + MASKN(b, 8) < MASKN(a, 8));
}
static inline uint64_t SCARRY(uint64_t a, uint64_t b, int sz) {
    int64_t x = SXN(a, sz), y = SXN(b, sz), r = SXN((uint64_t)(x + y), sz);
    return (x >= 0) == (y >= 0) && (r >= 0) != (x >= 0);
}
static inline uint64_t SBORROW(uint64_t a, uint64_t b, int sz) {
    int64_t x = SXN(a, sz), y = SXN(b, sz), r = SXN((uint64_t)(x - y), sz);
    return (x >= 0) != (y >= 0) && (r >= 0) != (x >= 0);
}

void rr_trap(uint32_t at, uint32_t target, const char *what);
void rr_div0(const char *what);   /* the 68K takes vector 5 (a game restarts there, or the ROM has a bare rte): counted and reported, execution goes on with 0 */
static inline uint64_t UDIVREM(uint64_t a, uint64_t b, char op) {
    if (!b) { rr_div0("unsigned divide by zero"); return 0; }
    return op == '/' ? a / b : a % b;
}
static inline int64_t SDIVREM(int64_t a, int64_t b, char op) {
    if (!b) { rr_div0("signed divide by zero"); return 0; }
    if (b == -1) return op == '/' ? -a : 0;
    return op == '/' ? a / b : a % b;
}

/* 68020 decimal arithmetic -- Ghidra's bcdAdjust leaves the byte binary, so
 * abcd/sbcd/nbcd are done here. X = C = decimal carry/borrow; Z is cleared
 * if the result is non-zero and otherwise UNCHANGED (multi-byte chains). */
#define RR_XF 0x43
#define RR_CF 0x47
#define RR_ZF 0x45
static inline uint32_t rr_abcd(uint32_t dst, uint32_t src) {
    uint32_t x = R[RR_XF] & 1;
    uint32_t lo = (dst & 0xF) + (src & 0xF) + x;
    uint32_t r = (dst & 0xF0) + (src & 0xF0);
    if (lo > 9) lo += 6;
    r += lo;
    uint32_t c = 0;
    if (r > 0x99) { r += 0x60; c = 1; }
    r &= 0xFF;
    R[RR_XF] = R[RR_CF] = (uint8_t)c;
    if (r) R[RR_ZF] = 0;
    return r;
}
static inline uint32_t rr_sbcd(uint32_t dst, uint32_t src) {
    uint32_t x = R[RR_XF] & 1;
    int lo = (int)(dst & 0xF) - (int)(src & 0xF) - (int)x;
    int hi = (int)(dst & 0xF0) - (int)(src & 0xF0);
    uint32_t c = 0;
    if (lo < 0) { lo -= 6; hi -= 0x10; }
    int r = hi + (lo & 0xF);
    if (hi < 0) { r -= 0x60; c = 1; }
    r &= 0xFF;
    R[RR_XF] = R[RR_CF] = (uint8_t)c;
    if (r) R[RR_ZF] = 0;
    return (uint32_t)r;
}

/* ---- 68020 bit-fields: bfextu / bfexts / bfffo / bftst / bfclr / bfset / bfchg / bfins (tools/namco22/lift.py emit_bitfield). Done here, by the manual's
 * rules, because Ghidra's p-code gets bfclr / bfset / bfchg on MEMORY wrong: it reuses one temporary for the whole word and for the extracted field, so the
 * instruction stores the FIELD (0..3) over the entire 32-bit word (Dirt Dash: the car's damage bits 0x55555000 became 0x00000001 at the first crash, and
 * the truck's body model was looked up at index -1).
 * A field is `w` bits (1..32) starting `off` bits below the MSB. In a REGISTER the offset is taken mod 32 and the field wraps around inside the register.
 * In MEMORY the offset is signed and counts from the MSB of the byte at ea: the field lives in the (bit + w + 7) / 8 bytes from ea + (off >> 3), and only
 * those bytes are read and written back. Flags: N = the field's top bit, Z = field == 0, V = C = 0 (X untouched); bfins takes them from the value inserted. */
static inline uint32_t rr_bf_mask(uint32_t w) { return w >= 32 ? 0xFFFFFFFFu : ((1u << w) - 1); }
static inline uint32_t rr_bf_reg_get(uint32_t v, int32_t off, uint32_t w)
{
    uint32_t o = (uint32_t)off & 31, rot = o ? (v << o) | (v >> (32 - o)) : v;
    return rot >> (32 - w);
}
static inline uint32_t rr_bf_reg_set(uint32_t v, int32_t off, uint32_t w, uint32_t fld)
{
    uint32_t o = (uint32_t)off & 31, rot = o ? (v << o) | (v >> (32 - o)) : v;
    uint32_t m = rr_bf_mask(w) << (32 - w);
    rot = (rot & ~m) | ((fld << (32 - w)) & m);
    return o ? (rot >> o) | (rot << (32 - o)) : rot;
}
static inline uint32_t rr_bf_mem_get(uint32_t ea, int32_t off, uint32_t w)
{
    uint32_t a = ea + (uint32_t)(off >> 3), bit = (uint32_t)off & 7, n = (bit + w + 7) >> 3;
    uint64_t v = 0;
    for (uint32_t i = 0; i < n; i++) v = v << 8 | MRD1(a + i);
    return (uint32_t)((v >> (n * 8 - bit - w)) & rr_bf_mask(w));
}
static inline void rr_bf_mem_set(uint32_t ea, int32_t off, uint32_t w, uint32_t fld)
{
    uint32_t a = ea + (uint32_t)(off >> 3), bit = (uint32_t)off & 7, n = (bit + w + 7) >> 3, sh = n * 8 - bit - w;
    uint64_t v = 0;
    for (uint32_t i = 0; i < n; i++) v = v << 8 | MRD1(a + i);
    uint64_t m = (uint64_t)rr_bf_mask(w) << sh;
    v = (v & ~m) | (((uint64_t)fld << sh) & m);
    for (uint32_t i = 0; i < n; i++) MWR1(a + i, (v >> ((n - 1 - i) * 8)) & 0xFF);
}
static inline void rr_bf_flags(uint32_t fld, uint32_t w)
{
    R[0x44] = (uint8_t)((fld >> (w - 1)) & 1);      /* N */
    R[0x45] = fld == 0;                              /* Z */
    R[0x46] = 0; R[0x47] = 0;                        /* V, C */
}

/* SR lives in Ghidra's flag bytes: 0x40 T, 0x41 S, 0x42 IPL (0..7), 0x43 X,
 * 0x44 N, 0x45 Z, 0x46 V, 0x47 C. Ghidra's move-from-SR p-code composes it
 * in ONE-BYTE temporaries, so IPL<<8, S<<13 and T<<15 shift out to zero and
 * only the CCR survives -- the lifter substitutes rr_get_sr() for that. */
static inline uint32_t rr_get_sr(void)
{
    return (R[0x40] & 1u) << 15 | (R[0x41] & 1u) << 13 | (R[0x42] & 7u) << 8 | (R[0x43] & 1u) << 4 |
           (R[0x44] & 1u) << 3 | (R[0x45] & 1u) << 2 | (R[0x46] & 1u) << 1 | (R[0x47] & 1u);
}
static inline void rr_set_sr(uint32_t sr)
{
    R[0x40] = sr >> 15 & 1; R[0x41] = sr >> 13 & 1; R[0x42] = sr >> 8 & 7; R[0x43] = sr >> 4 & 1;
    R[0x44] = sr >> 3 & 1; R[0x45] = sr >> 2 & 1; R[0x46] = sr >> 1 & 1; R[0x47] = sr & 1;
    RS2(0x200, sr);
}

/* The scheduler hook: every call and backward branch counts down a budget;
 * when it runs out the host delivers interrupts and steps the other chips. */
void rr_tick(void);
/* rd_poll_rec (src/rd/rd_core.c): during a check, the register file is recorded at
 * every poll -- an interrupt can only land there, and the vblank handler saves the
 * registers to WRAM, so a readable function must hold the 68K's registers at each one */
extern int rd_poll_rec;
void rd_poll_snap(void);
#define RR_POLL() do { if (rd_poll_rec) rd_poll_snap(); if (rr_budget <= 0) rr_tick(); } while (0)
/* RR_IDLE(n) (tools/lift/idle_loops.txt): at the back edge of a loop that only re-reads RAM waiting for an interrupt, count the passes
 * left in this slice at once -- n instructions each, the same budget the passes would have spent (it ends at 0 .. -(n-1), as stepping
 * them would), so the scheduler runs at the same instruction count and the interrupt lands where it did. Not in a trace build. */
#ifdef RR_TRACE
#define RR_IDLE(n) ((void)0)
#else
#define RR_IDLE(n) do { if (!rd_poll_rec && rr_budget > 3 * (n)) rr_budget -= (n) * ((rr_budget - 2 * (n) - 1) / (n)); } while (0)
/* (the last two passes before the slice ends still run for real: a device read in a pass can fire a screen event a few cycles early,
 * through the cycle-to-pixel rounding, and skipping that last pass moved the vblank work by one pass -- measured on the sound) */
#endif

void rr_call_ind(uint32_t target, uint32_t at);

/* Shadow return stack. The lifted code turns every 68K call into a C call,
 * but 68K code also uses rts as a computed jump (pea label; ...; rts) and
 * returns past several frames. Each call pushes (expected return, SP); rts
 * with a matching entry unwinds the C frames to that call site
 * (rr_after_call), anything else is a jump. rr_ret_to = where rts went. */
extern uint32_t rr_ret_to;
int  rr_call_push(uint32_t ret);      /* -> this call's shadow index */
int  rr_after_call(int j);            /* 1 = keep unwinding past this frame */
int  rr_return(uint32_t target);      /* 1 = matched a call site: C return */
void rr_rte(void);
int  rr_irq_push(void);

/* Per-instruction hook. Empty in the normal build; the trace build
 * (-DRR_TRACE) logs every instruction with the registers, in a format
 * tools/diff_trace_mame.py lines up against MAME's debugger trace. */
/* Time is counted in 68K instructions: every instruction spends one unit of
 * rr_budget; RR_POLL (calls, backward branches) hands over to the scheduler
 * once it runs out. */
#ifdef RR_TRACE
void rr_trace_ins(uint32_t pc);
extern uint32_t rr_pc;
#define RR_INS(a) (rr_pc = (a), rr_trace_ins(a), --rr_budget)
#else
extern uint32_t rr_pc;                 /* the instruction about to run: an interrupt's EPC */
#define RR_INS(a) (rr_pc = (a), --rr_budget)
#endif

#endif
