/*
 * m377_sem.h -- the 7700-family instruction SEMANTICS, shared by
 *   tools/sndoracle/m37710.c   the test oracle (fetches and decodes from memory), and
 *   gen/snd_driver.c           the TRANSLATED sound program (opcode and operand
 *                              bytes are build-time constants; gcc folds each
 *                              m377_exec() call down to that one instruction).
 * One source for both is what lets the translation be gated as EXACTLY equal to
 * the oracle, which is itself gated against MAME. Split out of pc-reverse's
 * validated m37710.c without changing a line of semantics.
 */
#ifndef RR_M377_SEM_H
#define RR_M377_SEM_H
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include "m37710.h"

void m377_sfr_w(m37710_t *c, uint32_t a, uint8_t v);   /* on-chip peripherals (m37710.c / snd_periph) */
#define sfr_w m377_sfr_w

/* Every bus access costs a cycle. The flat "2 per instruction" this started
 * with ran the core at ~2.5 cycles an instruction where the real part
 * averages nearer 6, which skewed every peripheral period measured in
 * cycles -- the A-D conversion finished hundreds of instructions late and
 * the PC diff against MAME could not get past it. Counting accesses is
 * cheap and lands in the right range without a per-opcode table. */
static inline uint8_t  rd8 (m37710_t *c, uint32_t a)
{
    c->cycles++;
    a &= 0xFFFFFF;
    if (a >= 0x20 && a < 0x30)          /* A-D conversion results, 8 x 16-bit */
        return (uint8_t)(c->ad_result[(a - 0x20) >> 1] >> (((a & 1) ? 8 : 0)));
    if (a < 0x80) return c->sfr[a];
    return c->read8(c->user, a);
}
static inline void     wr8 (m37710_t *c, uint32_t a, uint8_t v)
{
    c->cycles++;
    a &= 0xFFFFFF;
    if (a < 0x80) { sfr_w(c, a, v); return; }
    c->write8(c->user, a, v);
}
static inline uint16_t rd16(m37710_t *c, uint32_t a)            { return (uint16_t)(rd8(c,a) | (rd8(c,a+1) << 8)); }
static inline void     wr16(m37710_t *c, uint32_t a, uint16_t v){ wr8(c,a,(uint8_t)v); wr8(c,a+1,(uint8_t)(v>>8)); }

static inline uint8_t  fetch8 (m37710_t *c)
{
    c->fetches++;
    if (c->ops) { c->cycles++; c->pc++; return *c->ops++; }   /* translated code: operands are constants */
    uint8_t v = rd8(c, ((uint32_t)c->pg << 16) | c->pc); c->pc++; return v;
}
static inline uint16_t fetch16(m37710_t *c) { uint16_t v = fetch8(c); return (uint16_t)(v | (fetch8(c) << 8)); }

static inline bool flag(const m37710_t *c, uint16_t f) { return (c->ps & f) != 0; }
static inline void setf(m37710_t *c, uint16_t f, bool on) { if (on) c->ps |= f; else c->ps &= (uint16_t)~f; }

/* Width of A and of X/Y is selected by the m and x flags: 1 = 8-bit. */
static inline bool amode16(const m37710_t *c) { return !flag(c, M377_M); }
static inline bool imode16(const m37710_t *c) { return !flag(c, M377_X); }

static inline void set_nz(m37710_t *c, uint32_t v, bool wide)
{
    setf(c, M377_Z, (wide ? (v & 0xFFFF) : (v & 0xFF)) == 0);
    setf(c, M377_N, (v & (wide ? 0x8000 : 0x80)) != 0);
}

/* ---- stack ------------------------------------------------------------- */
static inline void push8 (m37710_t *c, uint8_t v)  { wr8(c, c->s, v); c->s--; }
static inline uint8_t pop8(m37710_t *c)            { c->s++; return rd8(c, c->s); }
static inline void push16(m37710_t *c, uint16_t v) { push8(c,(uint8_t)(v>>8)); push8(c,(uint8_t)v); }
static inline uint16_t pop16(m37710_t *c)          { uint16_t l = pop8(c); return (uint16_t)(l | (pop8(c) << 8)); }

/* ---- addressing modes. Each returns a 24-bit effective address. --------- */
static inline uint32_t ea_direct(m37710_t *c)   { return (uint32_t)((c->dpr + fetch8(c)) & 0xFFFF); }
static inline uint32_t ea_direct_x(m37710_t *c) { return (uint32_t)((c->dpr + fetch8(c) + c->x) & 0xFFFF); }
static inline uint32_t ea_direct_y(m37710_t *c) { return (uint32_t)((c->dpr + fetch8(c) + c->y) & 0xFFFF); }
static inline uint32_t ea_abs(m37710_t *c)      { uint16_t o = fetch16(c); return ((uint32_t)c->dt << 16) | o; }
static inline uint32_t ea_abs_x(m37710_t *c)    { uint16_t o = fetch16(c); return (((uint32_t)c->dt << 16) + o + c->x) & 0xFFFFFF; }
static inline uint32_t ea_abs_y(m37710_t *c)    { uint16_t o = fetch16(c); return (((uint32_t)c->dt << 16) + o + c->y) & 0xFFFFFF; }
static inline uint32_t ea_long(m37710_t *c)     { uint16_t o = fetch16(c); return ((uint32_t)fetch8(c) << 16) | o; }
static inline uint32_t ea_long_x(m37710_t *c)   { uint16_t o = fetch16(c); uint32_t b = (uint32_t)fetch8(c) << 16; return (b + o + c->x) & 0xFFFFFF; }
static inline uint32_t ea_ind_y(m37710_t *c)    { uint32_t p = ea_direct(c); uint16_t o = rd16(c,p); return (((uint32_t)c->dt << 16) + o + c->y) & 0xFFFFFF; }
/* (dp) -- direct indirect, no index. */
static inline uint32_t ea_ind(m37710_t *c)      { uint32_t p = ea_direct(c); return ((uint32_t)c->dt << 16) | rd16(c,p); }
static inline uint32_t ea_x_ind(m37710_t *c)    { uint32_t p = ea_direct_x(c); return ((uint32_t)c->dt << 16) | rd16(c,p); }
static inline uint32_t ea_indl(m37710_t *c)     { uint32_t p = ea_direct(c); return (uint32_t)rd16(c,p) | ((uint32_t)rd8(c,p+2) << 16); }
static inline uint32_t ea_indl_y(m37710_t *c)   { uint32_t b = ea_indl(c); return (b + c->y) & 0xFFFFFF; }
/* Stack relative. MAME prints the offset signed (`83 F0` -> STA -$10,S),
 * so it is taken as a signed 8-bit displacement from S. */
static inline uint32_t ea_stack(m37710_t *c)    { return (uint32_t)((c->s + (int8_t)fetch8(c)) & 0xFFFF); }
static inline uint32_t ea_stack_y(m37710_t *c)  { uint32_t p = ea_stack(c); uint16_t o = rd16(c,p); return (((uint32_t)c->dt << 16) + o + c->y) & 0xFFFFFF; }

/* Reads/writes honouring the current width flag. */
static inline uint16_t load_m(m37710_t *c, uint32_t a) { return amode16(c) ? rd16(c,a) : rd8(c,a); }
static inline void store_m(m37710_t *c, uint32_t a, uint16_t v) { if (amode16(c)) wr16(c,a,v); else wr8(c,a,(uint8_t)v); }
static inline uint16_t imm_m(m37710_t *c) { return amode16(c) ? fetch16(c) : fetch8(c); }
static inline uint16_t imm_i(m37710_t *c) { return imode16(c) ? fetch16(c) : fetch8(c); }

/* Operand fetch for the 0x89 page (MPY / DIV).
 *
 * The addressing mode is encoded in the low bits exactly as on the base
 * page, so MPY abs is `89 0D` beside ORA abs `0D`, and DIV is the same set
 * with bit 5 of the second byte set. Decoding the mode from the low nibble
 * rather than listing 30 cases keeps the two instruction families to one
 * table each. */
static inline uint32_t m89_operand(m37710_t *c, uint8_t op2)
{
    switch (op2 & 0x1F) {
    case 0x09: return imm_m(c);
    case 0x05: return load_m(c, ea_direct(c));
    case 0x15: return load_m(c, ea_direct_x(c));
    case 0x0D: return load_m(c, ea_abs(c));
    case 0x1D: return load_m(c, ea_abs_x(c));
    case 0x19: return load_m(c, ea_abs_y(c));
    case 0x01: return load_m(c, ea_x_ind(c));
    case 0x11: return load_m(c, ea_ind_y(c));
    case 0x12: return load_m(c, ea_ind(c));
    case 0x03: return load_m(c, ea_stack(c));
    case 0x07: return load_m(c, ea_indl(c));
    case 0x17: return load_m(c, ea_indl_y(c));
    default:   return 0;
    }
}

/* Accumulator access respecting m, and respecting the 0x42 prefix.
 *
 * The 7700's 0x42 page re-runs the SAME opcode table against the B
 * accumulator -- MAME's disassembler shows `42 A9` as LDB #imm, `42 9D` as
 * STB abs,X, `42 6D` as ADCB abs. So rather than duplicating every handler,
 * the prefix just points the accumulator accessors at B for one instruction.
 * The low 8 bits alias when m = 1. */
static inline uint16_t get_a(const m37710_t *c)
{
    uint16_t v = c->use_b ? c->b : c->a;
    return amode16(c) ? v : (uint16_t)(v & 0xFF);
}
/* The accumulator WITHOUT the m-width mask. TAX/TAY need it: their width is
 * the index flag's, not the accumulator's. */
static inline uint16_t raw_a(const m37710_t *c) { return c->use_b ? c->b : c->a; }

static inline void set_a(m37710_t *c, uint16_t v)
{
    uint16_t *r = c->use_b ? &c->b : &c->a;
    *r = amode16(c) ? v : (uint16_t)((*r & 0xFF00) | (v & 0xFF));
}

/* ---- ALU --------------------------------------------------------------- */
static void op_adc(m37710_t *c, uint16_t v)
{
    bool w = amode16(c); uint32_t a = get_a(c), r;
    if (flag(c, M377_D)) {          /* decimal mode -- the driver may use it */
        uint32_t lo = (a & 0x0F) + (v & 0x0F) + (flag(c,M377_C) ? 1 : 0), carry = 0;
        if (lo > 9) { lo += 6; carry = 1; }
        r = lo & 0x0F;
        for (int sh = 4; sh < (w ? 16 : 8); sh += 4) {
            uint32_t d = ((a >> sh) & 0x0F) + ((v >> sh) & 0x0F) + carry; carry = 0;
            if (d > 9) { d += 6; carry = 1; }
            r |= (d & 0x0F) << sh;
        }
        setf(c, M377_C, carry != 0);
    } else {
        r = a + v + (flag(c,M377_C) ? 1 : 0);
        setf(c, M377_C, r > (w ? 0xFFFFu : 0xFFu));
    }
    uint32_t sign = w ? 0x8000 : 0x80;
    setf(c, M377_V, (~(a ^ v) & (a ^ r) & sign) != 0);
    set_a(c, (uint16_t)r); set_nz(c, r, w);
}

static void op_sbc(m37710_t *c, uint16_t v)
{
    bool w = amode16(c); uint32_t a = get_a(c);
    uint32_t inv = (uint32_t)(~v) & (w ? 0xFFFFu : 0xFFu);
    uint32_t r = a + inv + (flag(c,M377_C) ? 1 : 0);
    setf(c, M377_C, r > (w ? 0xFFFFu : 0xFFu));
    uint32_t sign = w ? 0x8000 : 0x80;
    setf(c, M377_V, (~(a ^ inv) & (a ^ r) & sign) != 0);
    set_a(c, (uint16_t)r); set_nz(c, r, w);
}

static void op_cmp_val(m37710_t *c, uint32_t lhs, uint32_t rhs, bool w)
{
    uint32_t m = w ? 0xFFFFu : 0xFFu;
    uint32_t r = (lhs & m) - (rhs & m);
    setf(c, M377_C, (lhs & m) >= (rhs & m));
    set_nz(c, r, w);
}

/* Read-modify-write shifts and rotates on memory, width per m. */
static void rmw_shift(m37710_t *c, uint32_t a, int kind)
{
    bool w = amode16(c);
    uint32_t v = load_m(c, a), top = w ? 0x8000u : 0x80u, carry = flag(c,M377_C) ? 1u : 0u;
    switch (kind) {
    case 0: setf(c,M377_C,(v&top)!=0); v <<= 1;                       break; /* ASL */
    case 1: setf(c,M377_C,(v&1)!=0);   v >>= 1;                       break; /* LSR */
    case 2: { uint32_t nc=(v&top)!=0; v=(v<<1)|carry; setf(c,M377_C,nc!=0); } break; /* ROL */
    default:{ uint32_t nc=v&1; v=(v>>1)|(carry?top:0); setf(c,M377_C,nc!=0); }      break; /* ROR */
    }
    v &= w ? 0xFFFFu : 0xFFu;
    store_m(c, a, (uint16_t)v);
    set_nz(c, v, w);
}

/* SEB/CLB: set or clear the bits of an immediate mask in memory. Encoded
 * `04 dp imm16` / `0C abs16 imm16` (and 0x14 / 0x1C to clear) -- read off
 * MAME's own listing, e.g. `04 80 80 00  SEB #$0080, $80`. */
static void op_seb_clb(m37710_t *c, uint32_t a, bool set)
{
    uint16_t mask = imm_m(c), v = load_m(c, a);
    uint16_t r = (uint16_t)(set ? (v | mask) : (v & (uint16_t)~mask));
    store_m(c, a, r);
    setf(c, M377_Z, (amode16(c) ? (uint16_t)(v & mask) : (uint16_t)(v & mask & 0xFF)) == 0);
}

static inline void branch(m37710_t *c, bool take)
{
    int8_t d = (int8_t)fetch8(c);
    if (take) { c->pc = (uint16_t)(c->pc + d); c->cycles += 2; }
}

/* ---- init / reset ------------------------------------------------------ */

/* Execute ONE instruction whose opcode byte (after any 0x42 prefix, which the
 * caller signals with c->use_b) has been consumed. Operand bytes come from the
 * bus, or from c->ops when the caller supplies them (translated code). Returns
 * false for an opcode this core does not implement. */
#ifndef M377_EXEC_ATTR
#define M377_EXEC_ATTR
#endif
/* gen/snd_driver.c defines M377_EXEC_ATTR as always_inline, so each call with a
 * constant opcode is folded to that one instruction's code. */
static inline M377_EXEC_ATTR bool m377_exec(m37710_t *c, uint16_t op)
{
    switch (op) {
        /* --- flags ------------------------------------------------------ */
        case 0x18: setf(c, M377_C, false); break;                    /* CLC */
        case 0x38: setf(c, M377_C, true);  break;                    /* SEC */
        case 0x58: setf(c, M377_I, false); break;                    /* CLI */
        case 0x78: setf(c, M377_I, true);  break;                    /* SEI */
        case 0xD8: setf(c, M377_M, false); break;                    /* CLM -- NOT CLD */
        case 0xB8: setf(c, M377_V, false); break;                    /* CLV */
        case 0xC2: c->ps &= (uint16_t)~fetch8(c); break;             /* CLP */
        case 0xE2: c->ps |=  fetch8(c);           break;             /* SEP */

        /* --- loads / stores --------------------------------------------- */
        case 0xA9: { uint16_t v = imm_m(c); set_a(c,v); set_nz(c,v,amode16(c)); } break;
        case 0xA5: { uint16_t v = load_m(c, ea_direct(c)); set_a(c,v); set_nz(c,v,amode16(c)); } break;
        case 0xAD: { uint16_t v = load_m(c, ea_abs(c));    set_a(c,v); set_nz(c,v,amode16(c)); } break;
        case 0xAF: { uint16_t v = load_m(c, ea_long(c));   set_a(c,v); set_nz(c,v,amode16(c)); } break;
        case 0xBD: { uint16_t v = load_m(c, ea_abs_x(c));  set_a(c,v); set_nz(c,v,amode16(c)); } break;
        case 0xB9: { uint16_t v = load_m(c, ea_abs_y(c));  set_a(c,v); set_nz(c,v,amode16(c)); } break;
        case 0xB5: { uint16_t v = load_m(c, ea_direct_x(c)); set_a(c,v); set_nz(c,v,amode16(c)); } break;
        case 0xB1: { uint16_t v = load_m(c, ea_ind_y(c));  set_a(c,v); set_nz(c,v,amode16(c)); } break;
        case 0xA1: { uint16_t v = load_m(c, ea_x_ind(c));  set_a(c,v); set_nz(c,v,amode16(c)); } break;
        case 0xB7: { uint16_t v = load_m(c, ea_indl_y(c)); set_a(c,v); set_nz(c,v,amode16(c)); } break;
        case 0xBF: { uint16_t v = load_m(c, ea_long_x(c)); set_a(c,v); set_nz(c,v,amode16(c)); } break;

        case 0x85: store_m(c, ea_direct(c),   get_a(c)); break;      /* STA */
        case 0x8D: store_m(c, ea_abs(c),      get_a(c)); break;
        case 0x8F: store_m(c, ea_long(c),     get_a(c)); break;
        case 0x9D: store_m(c, ea_abs_x(c),    get_a(c)); break;
        case 0x99: store_m(c, ea_abs_y(c),    get_a(c)); break;
        case 0x95: store_m(c, ea_direct_x(c), get_a(c)); break;
        case 0x91: store_m(c, ea_ind_y(c),    get_a(c)); break;
        case 0x81: store_m(c, ea_x_ind(c),    get_a(c)); break;
        case 0x97: store_m(c, ea_indl_y(c),   get_a(c)); break;
        case 0x9F: store_m(c, ea_long_x(c),   get_a(c)); break;

        case 0xA2: { uint16_t v = imm_i(c); c->x = v; set_nz(c,v,imode16(c)); } break;   /* LDX */
        case 0xA6: { uint16_t v = imode16(c)?rd16(c,ea_direct(c)):rd8(c,ea_direct(c)); c->x=v; set_nz(c,v,imode16(c)); } break;
        case 0xAE: { uint16_t v = imode16(c)?rd16(c,ea_abs(c)):rd8(c,ea_abs(c));       c->x=v; set_nz(c,v,imode16(c)); } break;
        case 0xA0: { uint16_t v = imm_i(c); c->y = v; set_nz(c,v,imode16(c)); } break;   /* LDY */
        case 0xA4: { uint16_t v = imode16(c)?rd16(c,ea_direct(c)):rd8(c,ea_direct(c)); c->y=v; set_nz(c,v,imode16(c)); } break;
        case 0xAC: { uint16_t v = imode16(c)?rd16(c,ea_abs(c)):rd8(c,ea_abs(c));       c->y=v; set_nz(c,v,imode16(c)); } break;

        case 0x86: { uint32_t a=ea_direct(c); if(imode16(c)) wr16(c,a,c->x); else wr8(c,a,(uint8_t)c->x);} break; /* STX */
        case 0x8E: { uint32_t a=ea_abs(c);    if(imode16(c)) wr16(c,a,c->x); else wr8(c,a,(uint8_t)c->x);} break;
        case 0x84: { uint32_t a=ea_direct(c); if(imode16(c)) wr16(c,a,c->y); else wr8(c,a,(uint8_t)c->y);} break; /* STY */
        case 0x8C: { uint32_t a=ea_abs(c);    if(imode16(c)) wr16(c,a,c->y); else wr8(c,a,(uint8_t)c->y);} break;
        /* 7700 STORE-IMMEDIATE AND FLAG OPS -- NOT the 65C816 meanings.
         * Verified against MAME's own M37710 disassembler (the encodings the
         * ROM actually uses; reading them as 65C816 made 0x64 STZ, 0xF8 SED
         * and 0x89 BIT, all wrong). The immediate follows the m flag, which
         * is why a static disassembly desyncs here and a runtime core does
         * not. */
        /* LDM TAKES THE ADDRESS FIRST, THEN THE IMMEDIATE: `64 <dp> <imm(m)>`.
         * Fetching the immediate first reads the dp byte as the value and the
         * value's high byte as the address -- `64 C0 00 A0` then writes
         * 0x00C0 to 0xA0 instead of 0xA000 to 0xC0. Both orders consume the
         * SAME number of bytes, so the PC stream is unaffected and this
         * survived 71,000 instructions of PC-identical execution. It is the
         * reason a write-stream comparison is the gate that matters. */
        case 0x64: { uint32_t a = ea_direct(c);   store_m(c, a, imm_m(c)); } break; /* LDM #imm,dp */
        case 0x74: { uint32_t a = ea_direct_x(c); store_m(c, a, imm_m(c)); } break;
        case 0x9C: { uint32_t a = ea_abs(c);      store_m(c, a, imm_m(c)); } break; /* LDM #imm,abs */
        case 0x9E: { uint32_t a = ea_abs_x(c);    store_m(c, a, imm_m(c)); } break;
        case 0xF8: setf(c, M377_M, true);  break;                    /* SEM */
        case 0x96: { uint32_t a=ea_direct_y(c); if(imode16(c)) wr16(c,a,c->x); else wr8(c,a,(uint8_t)c->x);} break; /* STX dp,Y */
        case 0xB6: { uint32_t a=ea_direct_y(c); uint16_t v=imode16(c)?rd16(c,a):rd8(c,a); c->x=v; set_nz(c,v,imode16(c)); } break; /* LDX dp,Y */
        case 0xBE: { uint32_t a=ea_abs_y(c);    uint16_t v=imode16(c)?rd16(c,a):rd8(c,a); c->x=v; set_nz(c,v,imode16(c)); } break; /* LDX abs,Y */
        case 0x94: { uint32_t a=ea_direct_x(c); if(imode16(c)) wr16(c,a,c->y); else wr8(c,a,(uint8_t)c->y);} break; /* STY dp,X */
        case 0xB4: { uint32_t a=ea_direct_x(c); uint16_t v=imode16(c)?rd16(c,a):rd8(c,a); c->y=v; set_nz(c,v,imode16(c)); } break; /* LDY dp,X */
        case 0xBC: { uint32_t a=ea_abs_x(c);    uint16_t v=imode16(c)?rd16(c,a):rd8(c,a); c->y=v; set_nz(c,v,imode16(c)); } break; /* LDY abs,X */

        /* --- transfers --------------------------------------------------- */
        /* TAX/TAY take their width from the INDEX flag x, not from the
         * accumulator flag m -- with m=1 and x=0 the WHOLE 16-bit
         * accumulator transfers, which `get_a()` would have masked to 8.
         *
         * This was not academic. The driver's voice-clear routine at ROM
         * 0xE1A1 is `TBX ; STA $00,X ; STA $20,X ; STA $40,X ; ...` with
         * B = 0x8800 + n, clearing a voice's workspace in shared RAM. Masked
         * to 8 bits X came out 0, so the stores landed on the INTERNAL
         * PERIPHERAL REGISTERS instead: `STA $40,X` wrote 0 to the
         * count-start register and stopped every timer the driver had
         * running, including the Timer B0 one-shot that wakes its main loop.
         * The driver then sat in `LDA $83 ; BEQ` forever and the game was
         * silent. Measured in the running game: B=0x8800 X=0x0000 at
         * pc=0xE1A7, writing SFR[40] 0x0D -> 0x00 at frame 129. */
        case 0xAA: c->x = imode16(c) ? raw_a(c) : (uint16_t)(raw_a(c) & 0xFF);
                   set_nz(c, c->x, imode16(c)); break;                  /* TAX */
        case 0xA8: c->y = imode16(c) ? raw_a(c) : (uint16_t)(raw_a(c) & 0xFF);
                   set_nz(c, c->y, imode16(c)); break;                  /* TAY */
        case 0x8A: set_a(c,c->x);   set_nz(c,get_a(c),amode16(c)); break; /* TXA */
        case 0x98: set_a(c,c->y);   set_nz(c,get_a(c),amode16(c)); break; /* TYA */
        case 0x9B: c->y = c->x; set_nz(c,c->y,imode16(c)); break;       /* TXY */
        case 0xBB: c->x = c->y; set_nz(c,c->x,imode16(c)); break;       /* TYX */
        case 0xBA: c->x = imode16(c) ? c->s : (uint16_t)(c->s & 0xFF);
                   set_nz(c, c->x, imode16(c)); break;                  /* TSX */
        case 0x9A: c->s = c->x; break;                                  /* TXS */
        case 0x5B: c->dpr = c->a; set_nz(c,c->dpr,true); break;         /* TCD */
        case 0x7B: c->a = c->dpr; set_nz(c,c->a,true); break;           /* TDC */
        case 0x1B: c->s = c->a; break;                                  /* TCS */
        case 0x3B: c->a = c->s; set_nz(c,c->a,true); break;             /* TSC */

        /* --- inc / dec --------------------------------------------------- */
        /* 0x1A IS DEA AND 0x3A IS INA -- the reverse of the 65C816, which is
         * what this was copied from. MAME names the unprefixed bytes DEA/INA
         * and the 0x42-prefixed pair DEB/INB, consistently across all 49
         * occurrences in the ROM. */
        case 0x1A: { uint16_t v=(uint16_t)(get_a(c)-1); set_a(c,v); set_nz(c,v,amode16(c)); } break; /* DEA */
        case 0x3A: { uint16_t v=(uint16_t)(get_a(c)+1); set_a(c,v); set_nz(c,v,amode16(c)); } break; /* INA */
        case 0xE8: c->x=(uint16_t)(c->x+1); set_nz(c,c->x,imode16(c)); break;
        case 0xC8: c->y=(uint16_t)(c->y+1); set_nz(c,c->y,imode16(c)); break;
        case 0xCA: c->x=(uint16_t)(c->x-1); set_nz(c,c->x,imode16(c)); break;
        case 0x88: c->y=(uint16_t)(c->y-1); set_nz(c,c->y,imode16(c)); break;
        case 0xE6: { uint32_t a=ea_direct(c); uint16_t v=(uint16_t)(load_m(c,a)+1); store_m(c,a,v); set_nz(c,v,amode16(c)); } break;
        case 0xEE: { uint32_t a=ea_abs(c);    uint16_t v=(uint16_t)(load_m(c,a)+1); store_m(c,a,v); set_nz(c,v,amode16(c)); } break;
        case 0xC6: { uint32_t a=ea_direct(c); uint16_t v=(uint16_t)(load_m(c,a)-1); store_m(c,a,v); set_nz(c,v,amode16(c)); } break;
        case 0xCE: { uint32_t a=ea_abs(c);    uint16_t v=(uint16_t)(load_m(c,a)-1); store_m(c,a,v); set_nz(c,v,amode16(c)); } break;

        /* --- logic / arithmetic ------------------------------------------ */
        case 0x69: op_adc(c, imm_m(c)); break;
        case 0x65: op_adc(c, load_m(c, ea_direct(c))); break;
        case 0x6D: op_adc(c, load_m(c, ea_abs(c)));    break;
        case 0x7D: op_adc(c, load_m(c, ea_abs_x(c)));  break;
        case 0x79: op_adc(c, load_m(c, ea_abs_y(c)));  break;
        case 0xE9: op_sbc(c, imm_m(c)); break;
        case 0xE5: op_sbc(c, load_m(c, ea_direct(c))); break;
        case 0xED: op_sbc(c, load_m(c, ea_abs(c)));    break;
        case 0xFD: op_sbc(c, load_m(c, ea_abs_x(c)));  break;
        case 0xF9: op_sbc(c, load_m(c, ea_abs_y(c)));  break;

        case 0x29: { uint16_t v=(uint16_t)(get_a(c) & imm_m(c)); set_a(c,v); set_nz(c,v,amode16(c)); } break;
        case 0x25: { uint16_t v=(uint16_t)(get_a(c) & load_m(c,ea_direct(c))); set_a(c,v); set_nz(c,v,amode16(c)); } break;
        case 0x2D: { uint16_t v=(uint16_t)(get_a(c) & load_m(c,ea_abs(c)));    set_a(c,v); set_nz(c,v,amode16(c)); } break;
        case 0x3D: { uint16_t v=(uint16_t)(get_a(c) & load_m(c,ea_abs_x(c)));  set_a(c,v); set_nz(c,v,amode16(c)); } break;
        case 0x09: { uint16_t v=(uint16_t)(get_a(c) | imm_m(c)); set_a(c,v); set_nz(c,v,amode16(c)); } break;
        case 0x05: { uint16_t v=(uint16_t)(get_a(c) | load_m(c,ea_direct(c))); set_a(c,v); set_nz(c,v,amode16(c)); } break;
        case 0x0D: { uint16_t v=(uint16_t)(get_a(c) | load_m(c,ea_abs(c)));    set_a(c,v); set_nz(c,v,amode16(c)); } break;
        case 0x1D: { uint16_t v=(uint16_t)(get_a(c) | load_m(c,ea_abs_x(c)));  set_a(c,v); set_nz(c,v,amode16(c)); } break;
        case 0x49: { uint16_t v=(uint16_t)(get_a(c) ^ imm_m(c)); set_a(c,v); set_nz(c,v,amode16(c)); } break;
        case 0x45: { uint16_t v=(uint16_t)(get_a(c) ^ load_m(c,ea_direct(c))); set_a(c,v); set_nz(c,v,amode16(c)); } break;
        case 0x4D: { uint16_t v=(uint16_t)(get_a(c) ^ load_m(c,ea_abs(c)));    set_a(c,v); set_nz(c,v,amode16(c)); } break;

        case 0xC9: op_cmp_val(c, get_a(c), imm_m(c), amode16(c)); break;
        case 0xC5: op_cmp_val(c, get_a(c), load_m(c,ea_direct(c)), amode16(c)); break;
        case 0xCD: op_cmp_val(c, get_a(c), load_m(c,ea_abs(c)),    amode16(c)); break;
        case 0xDD: op_cmp_val(c, get_a(c), load_m(c,ea_abs_x(c)),  amode16(c)); break;
        case 0xD9: op_cmp_val(c, get_a(c), load_m(c,ea_abs_y(c)),  amode16(c)); break;
        case 0xE0: op_cmp_val(c, c->x, imm_i(c), imode16(c)); break;
        case 0xE4: op_cmp_val(c, c->x, imode16(c)?rd16(c,ea_direct(c)):rd8(c,ea_direct(c)), imode16(c)); break;
        case 0xEC: op_cmp_val(c, c->x, imode16(c)?rd16(c,ea_abs(c)):rd8(c,ea_abs(c)),       imode16(c)); break;
        case 0xC0: op_cmp_val(c, c->y, imm_i(c), imode16(c)); break;
        case 0xC4: op_cmp_val(c, c->y, imode16(c)?rd16(c,ea_direct(c)):rd8(c,ea_direct(c)), imode16(c)); break;
        case 0xCC: op_cmp_val(c, c->y, imode16(c)?rd16(c,ea_abs(c)):rd8(c,ea_abs(c)),       imode16(c)); break;

        /* --- BBS/BBC: test a bit mask in memory and branch -------------------
         * 7700-only, and NOT the 65C816's BIT at 0x24/0x2C -- mistaking them
         * cost a silent 2-byte decode where the real instruction is 4.
         * Encoding is opcode, the address, the mask (width per m), then an
         * 8-bit displacement from the END of the instruction:
         *     24 <dp>    <imm(m)> <rel8>     BBS dp
         *     34 <dp>    <imm(m)> <rel8>     BBC dp
         *     2C <abs16> <imm(m)> <rel8>     BBS abs
         *     3C <abs16> <imm(m)> <rel8>     BBC abs
         * Confirmed against MAME's RUNTIME trace, which knows m where the
         * static listing does not: `24 80 01 4F` with m=1 is four bytes, and
         * 0xC48E + 0x4F = 0xC4DD, the target MAME printed. */
        case 0x24: case 0x34: case 0x2C: case 0x3C: {
            bool is_abs = (op == 0x2C || op == 0x3C);
            bool want_set = (op == 0x24 || op == 0x2C);
            uint32_t a = is_abs ? ea_abs(c) : ea_direct(c);
            uint16_t mask = imm_m(c);
            uint16_t v = load_m(c, a);
            bool take = want_set ? ((v & mask) == mask) : ((v & mask) == 0);
            branch(c, take);
        } break;

        /* --- shifts / rotates -------------------------------------------- */
        case 0x0A: { uint32_t v=get_a(c); v<<=1; setf(c,M377_C,(v&(amode16(c)?0x10000:0x100))!=0); set_a(c,(uint16_t)v); set_nz(c,v,amode16(c)); } break;
        case 0x4A: { uint32_t v=get_a(c); setf(c,M377_C,(v&1)!=0); v>>=1; set_a(c,(uint16_t)v); set_nz(c,v,amode16(c)); } break;
        case 0x2A: { uint32_t v=((uint32_t)get_a(c)<<1)|(flag(c,M377_C)?1:0); setf(c,M377_C,(v&(amode16(c)?0x10000:0x100))!=0); set_a(c,(uint16_t)v); set_nz(c,v,amode16(c)); } break;
        case 0x6A: { uint32_t v=get_a(c); uint32_t cy=flag(c,M377_C)?(amode16(c)?0x8000:0x80):0; setf(c,M377_C,(v&1)!=0); v=(v>>1)|cy; set_a(c,(uint16_t)v); set_nz(c,v,amode16(c)); } break;

        /* --- jumps / calls ------------------------------------------------ */
        case 0x4C: c->pc = fetch16(c); break;                         /* JMP abs */
        case 0x5C: { uint16_t o=fetch16(c); c->pg=fetch8(c); c->pc=o; } break; /* JMP long */
        case 0x6C: { uint32_t p=fetch16(c); c->pc=rd16(c,p); } break; /* JMP (abs) */
        /* JSR PUSHES THE REAL RETURN ADDRESS AND RTS DOES NOT ADD ONE.
         * The 65C816 convention (push PC-1, pop and add 1) is self-consistent
         * for a matched JSR/RTS pair, so it survived 67,000 instructions here.
         * What exposes it is the driver pushing a return address by hand:
         *     C0ED  PEA $c0f3
         *     C0F0  JMP ($bef2)
         * and MAME's RTS lands on exactly 0xC0F3, not 0xC0F4. */
        case 0x20: { uint16_t t=fetch16(c); push16(c,c->pc); c->pc=t; } break; /* JSR */
        case 0x22: { uint16_t t=fetch16(c); uint8_t b=fetch8(c); push8(c,c->pg); push16(c,c->pc); c->pg=b; c->pc=t; } break;
        case 0x60: c->pc = pop16(c); break;                            /* RTS */
        case 0x6B: { uint16_t r=pop16(c); c->pg=pop8(c); c->pc=r; } break; /* RTL */
        case 0x40: { c->ps=pop16(c); c->pc=pop16(c); c->pg=pop8(c); } break; /* RTI */

        /* --- branches ------------------------------------------------------ */
        case 0x80: branch(c, true); break;
        case 0xD0: branch(c, !flag(c,M377_Z)); break;
        case 0xF0: branch(c,  flag(c,M377_Z)); break;
        case 0x90: branch(c, !flag(c,M377_C)); break;
        case 0xB0: branch(c,  flag(c,M377_C)); break;
        case 0x10: branch(c, !flag(c,M377_N)); break;
        case 0x30: branch(c,  flag(c,M377_N)); break;
        case 0x50: branch(c, !flag(c,M377_V)); break;
        case 0x70: branch(c,  flag(c,M377_V)); break;

        /* --- stack ---------------------------------------------------------- */
        /* PHA -- and PHB, since the 0x42 prefix selects the accumulator.
         * Reaching `c->a` directly here instead of through get_a() pushed A
         * where the driver meant B, and the ROM brackets its MPY with
         * PHB/PLB precisely because MPY writes its result across B:A. The
         * restore then put A's old value in B, and the voice-workspace
         * pointer that B carries became a small number -- so the next
         * `TBX ; STA $00,X` walked the INTERNAL PERIPHERAL REGISTERS again.
         * Measured: PHB saved 0x001F (A) where B held 0x6F00, and X came out
         * 0x003F. Same end symptom as the TAX width bug, different cause. */
        case 0x48: if (amode16(c)) push16(c,get_a(c)); else push8(c,(uint8_t)get_a(c)); break;
        case 0x68: { uint16_t v = amode16(c)?pop16(c):pop8(c); set_a(c,v); set_nz(c,v,amode16(c)); } break;
        case 0xDA: if (imode16(c)) push16(c,c->x); else push8(c,(uint8_t)c->x); break;
        case 0xFA: { uint16_t v = imode16(c)?pop16(c):pop8(c); c->x=v; set_nz(c,v,imode16(c)); } break;
        case 0x5A: if (imode16(c)) push16(c,c->y); else push8(c,(uint8_t)c->y); break;
        case 0x7A: { uint16_t v = imode16(c)?pop16(c):pop8(c); c->y=v; set_nz(c,v,imode16(c)); } break;
        case 0x08: push16(c, c->ps); break;                           /* PHP */
        case 0x28: c->ps = pop16(c); break;                           /* PLP */
        case 0x8B: push8(c, c->dt); break;                            /* PHB */
        case 0xAB: { c->dt = pop8(c); set_nz(c,c->dt,false); } break; /* PLB */
        case 0x0B: push16(c, c->dpr); break;                          /* PHD */
        case 0x2B: { c->dpr = pop16(c); set_nz(c,c->dpr,true); } break;
        case 0x4B: push8(c, c->pg); break;                            /* PHK */
        case 0xF4: push16(c, fetch16(c)); break;                      /* PEA */

        /* PSH/PUL: the 7700's multi-register stack instructions, an 8-bit
         * mask selecting A, B, X, Y, DPR, DT, PG, PS. This is how the sound
         * driver's interrupt handlers save and restore context, so nothing
         * past the first tick runs without them.
         *
         * A and B follow the m flag, X and Y the x flag, DPR is always 16
         * bits and DT/PG always 8. PUL restores in the reverse order AND
         * takes PS first, so the widths of the registers pulled after it are
         * decided by the status word that was just restored -- which is the
         * whole point of saving PS. (PG is pushable but not pullable; the
         * pull side has no 0x40 case.) PS is pushed as ipl:ps, which is
         * exactly our 16-bit ps, so push16/pop16 already order it right. */
        case 0xEB: {                                                  /* PSH #imm */
            uint8_t m = fetch8(c);
            if (m & 0x01) { if (amode16(c)) push16(c,c->a); else push8(c,(uint8_t)c->a); }
            if (m & 0x02) { if (amode16(c)) push16(c,c->b); else push8(c,(uint8_t)c->b); }
            if (m & 0x04) { if (imode16(c)) push16(c,c->x); else push8(c,(uint8_t)c->x); }
            if (m & 0x08) { if (imode16(c)) push16(c,c->y); else push8(c,(uint8_t)c->y); }
            if (m & 0x10) push16(c, c->dpr);
            if (m & 0x20) push8 (c, c->dt);
            if (m & 0x40) push8 (c, c->pg);
            if (m & 0x80) push16(c, c->ps);
        } break;
        case 0xFB: {                                                  /* PUL #imm */
            uint8_t m = fetch8(c);
            if (m & 0x80) c->ps  = pop16(c);
            if (m & 0x20) c->dt  = pop8(c);
            if (m & 0x10) c->dpr = pop16(c);
            if (m & 0x08) c->y = imode16(c) ? pop16(c) : pop8(c);
            if (m & 0x04) c->x = imode16(c) ? pop16(c) : pop8(c);
            if (m & 0x02) { if (amode16(c)) c->b = pop16(c); else c->b = (uint16_t)((c->b & 0xFF00) | pop8(c)); }
            if (m & 0x01) { if (amode16(c)) c->a = pop16(c); else c->a = (uint16_t)((c->a & 0xFF00) | pop8(c)); }
        } break;

        /* --- 7700 bit set/clear ---------------------------------------------- */
        case 0x04: op_seb_clb(c, ea_direct(c), true);  break;         /* SEB dp  */
        case 0x0C: op_seb_clb(c, ea_abs(c),    true);  break;         /* SEB abs */
        case 0x14: op_seb_clb(c, ea_direct(c), false); break;         /* CLB dp  */
        case 0x1C: op_seb_clb(c, ea_abs(c),    false); break;         /* CLB abs */

        /* --- the 0x89 PREFIX PAGE ---------------------------------------------
         * 0x89 is a second opcode page, not an instruction: MPY, DIV, XAB,
         * RLA and LDT all live behind it. `89 C2 <imm8>` is LDT (load the
         * data bank) -- every one of the 79 in the ROM has that fixed middle
         * byte, and the immediates are 0x00 / 0x20 / 0x21, i.e. bank numbers,
         * 0x20/0x21 being the ROM at 0x200000 where the sample tables are.
         *
         * MPY and DIV return a 32-bit result across A:B, which is why the B
         * accumulator exists on this part at all. Reading 0x89 as the 65C816's
         * BIT #imm would consume the wrong number of bytes and desync the
         * whole stream, which is what makes this a page and not a one-off. */
        case 0x89: {
            uint8_t op2 = fetch8(c);
            switch (op2) {
            case 0xC2: c->dt = fetch8(c); break;                      /* LDT #imm */
            case 0x28: { uint16_t t=c->a; c->a=c->b; c->b=t;          /* XAB */
                         set_nz(c, c->a, amode16(c)); } break;
            /* RLA #imm: rotate A left by a COUNT, flags untouched. The count
             * is an immediate of the accumulator's width, which is what makes
             * it an 0x89-page instruction rather than a shift with a mode. */
            case 0x49: {
                uint16_t cnt = imm_m(c);
                if (amode16(c)) { cnt &= 15; c->a = (uint16_t)((c->a << cnt) | (c->a >> (16 - cnt))); if (!cnt) { /* no-op */ } }
                else { cnt &= 7; uint8_t v = (uint8_t)c->a;
                       if (cnt) v = (uint8_t)((v << cnt) | (v >> (8 - cnt)));
                       c->a = (uint16_t)((c->a & 0xFF00) | v); }
            } break;
            /* MPY: A * src -> B:A, unsigned. */
            case 0x09: case 0x05: case 0x15: case 0x0D: case 0x1D: case 0x19:
            case 0x01: case 0x12: case 0x11: case 0x03: {
                uint32_t src = m89_operand(c, op2), r;
                if (amode16(c)) { r = (uint32_t)c->a * (src & 0xFFFF);
                                  c->a = (uint16_t)r; c->b = (uint16_t)(r >> 16); }
                else            { r = (uint32_t)(c->a & 0xFF) * (src & 0xFF);
                                  c->a = (uint16_t)((c->a & 0xFF00) | (r & 0xFF));
                                  c->b = (uint16_t)((c->b & 0xFF00) | ((r >> 8) & 0xFF)); }
                setf(c, M377_N, (r >> (amode16(c) ? 31 : 15)) & 1);
                setf(c, M377_Z, r == 0);
                setf(c, M377_C, false);
            } break;
            /* DIV: B:A / src -> A, remainder -> B, unsigned. */
            case 0x29: case 0x25: case 0x35: case 0x2D: case 0x3D: case 0x39:
            case 0x21: case 0x32: case 0x31: case 0x23: {
                uint32_t src = m89_operand(c, op2);
                if (amode16(c)) {
                    uint32_t num = ((uint32_t)c->b << 16) | c->a, d = src & 0xFFFF;
                    if (d == 0 || (num / d) > 0xFFFF) { setf(c, M377_V, true); }
                    else { setf(c, M377_V, false);
                           uint32_t q = num / d, rem = num % d;
                           c->a = (uint16_t)q; c->b = (uint16_t)rem;
                           setf(c, M377_N, (q >> 15) & 1); setf(c, M377_Z, q == 0); }
                } else {
                    uint32_t num = ((c->b & 0xFF) << 8) | (c->a & 0xFF), d = src & 0xFF;
                    if (d == 0 || (num / d) > 0xFF) { setf(c, M377_V, true); }
                    else { setf(c, M377_V, false);
                           uint32_t q = num / d, rem = num % d;
                           c->a = (uint16_t)((c->a & 0xFF00) | q);
                           c->b = (uint16_t)((c->b & 0xFF00) | rem);
                           setf(c, M377_N, (q >> 7) & 1); setf(c, M377_Z, q == 0); }
                }
                setf(c, M377_C, false);
            } break;
            default:
                c->unimpl_hit = true; c->unimpl_op = (uint16_t)(0x200 | op2);
                c->unimpl_pc  = (uint32_t)(c->pg << 16) | (uint16_t)(c->pc - 2);
                return true;   /* unimpl_hit is set; the caller stops */
            }
        } break;

        /* --- stack-relative --------------------------------------------------- */
        case 0x03: { uint16_t v=(uint16_t)(get_a(c) | load_m(c,ea_stack(c))); set_a(c,v); set_nz(c,v,amode16(c)); } break;
        case 0x23: { uint16_t v=(uint16_t)(get_a(c) & load_m(c,ea_stack(c))); set_a(c,v); set_nz(c,v,amode16(c)); } break;
        case 0x43: { uint16_t v=(uint16_t)(get_a(c) ^ load_m(c,ea_stack(c))); set_a(c,v); set_nz(c,v,amode16(c)); } break;
        case 0x63: op_adc(c, load_m(c, ea_stack(c))); break;
        case 0xE3: op_sbc(c, load_m(c, ea_stack(c))); break;
        case 0xA3: { uint16_t v=load_m(c,ea_stack(c)); set_a(c,v); set_nz(c,v,amode16(c)); } break;
        case 0x83: store_m(c, ea_stack(c), get_a(c)); break;
        case 0xC3: op_cmp_val(c, get_a(c), load_m(c,ea_stack(c)), amode16(c)); break;
        case 0x13: { uint16_t v=(uint16_t)(get_a(c) | load_m(c,ea_stack_y(c))); set_a(c,v); set_nz(c,v,amode16(c)); } break;
        case 0xB3: { uint16_t v=load_m(c,ea_stack_y(c)); set_a(c,v); set_nz(c,v,amode16(c)); } break;
        case 0x93: store_m(c, ea_stack_y(c), get_a(c)); break;

        /* --- remaining (dp,X)/(dp),Y/long forms -------------------------------- */
        case 0x01: { uint16_t v=(uint16_t)(get_a(c) | load_m(c,ea_x_ind(c))); set_a(c,v); set_nz(c,v,amode16(c)); } break;
        case 0x11: { uint16_t v=(uint16_t)(get_a(c) | load_m(c,ea_ind_y(c))); set_a(c,v); set_nz(c,v,amode16(c)); } break;
        case 0x15: { uint16_t v=(uint16_t)(get_a(c) | load_m(c,ea_direct_x(c))); set_a(c,v); set_nz(c,v,amode16(c)); } break;
        case 0x19: { uint16_t v=(uint16_t)(get_a(c) | load_m(c,ea_abs_y(c))); set_a(c,v); set_nz(c,v,amode16(c)); } break;
        case 0x21: { uint16_t v=(uint16_t)(get_a(c) & load_m(c,ea_x_ind(c))); set_a(c,v); set_nz(c,v,amode16(c)); } break;
        case 0x31: { uint16_t v=(uint16_t)(get_a(c) & load_m(c,ea_ind_y(c))); set_a(c,v); set_nz(c,v,amode16(c)); } break;
        case 0x35: { uint16_t v=(uint16_t)(get_a(c) & load_m(c,ea_direct_x(c))); set_a(c,v); set_nz(c,v,amode16(c)); } break;
        case 0x39: { uint16_t v=(uint16_t)(get_a(c) & load_m(c,ea_abs_y(c))); set_a(c,v); set_nz(c,v,amode16(c)); } break;
        case 0x41: { uint16_t v=(uint16_t)(get_a(c) ^ load_m(c,ea_x_ind(c))); set_a(c,v); set_nz(c,v,amode16(c)); } break;
        case 0x51: { uint16_t v=(uint16_t)(get_a(c) ^ load_m(c,ea_ind_y(c))); set_a(c,v); set_nz(c,v,amode16(c)); } break;
        case 0x55: { uint16_t v=(uint16_t)(get_a(c) ^ load_m(c,ea_direct_x(c))); set_a(c,v); set_nz(c,v,amode16(c)); } break;
        case 0x59: { uint16_t v=(uint16_t)(get_a(c) ^ load_m(c,ea_abs_y(c))); set_a(c,v); set_nz(c,v,amode16(c)); } break;
        case 0x5D: { uint16_t v=(uint16_t)(get_a(c) ^ load_m(c,ea_abs_x(c))); set_a(c,v); set_nz(c,v,amode16(c)); } break;
        case 0x61: op_adc(c, load_m(c, ea_x_ind(c)));    break;
        case 0x71: op_adc(c, load_m(c, ea_ind_y(c)));    break;
        case 0x75: op_adc(c, load_m(c, ea_direct_x(c))); break;
        case 0x67: op_adc(c, load_m(c, ea_indl(c)));     break;
        case 0x77: op_adc(c, load_m(c, ea_indl_y(c)));   break;
        case 0x6F: op_adc(c, load_m(c, ea_long(c)));     break;
        case 0x7F: op_adc(c, load_m(c, ea_long_x(c)));   break;
        case 0xE1: op_sbc(c, load_m(c, ea_x_ind(c)));    break;
        case 0xF1: op_sbc(c, load_m(c, ea_ind_y(c)));    break;
        case 0xF5: op_sbc(c, load_m(c, ea_direct_x(c))); break;
        case 0xE7: op_sbc(c, load_m(c, ea_indl(c)));     break;
        case 0xF7: op_sbc(c, load_m(c, ea_indl_y(c)));   break;
        case 0xEF: op_sbc(c, load_m(c, ea_long(c)));     break;
        case 0xFF: op_sbc(c, load_m(c, ea_long_x(c)));   break;
        case 0xC1: op_cmp_val(c, get_a(c), load_m(c,ea_x_ind(c)),    amode16(c)); break;
        case 0xD1: op_cmp_val(c, get_a(c), load_m(c,ea_ind_y(c)),    amode16(c)); break;
        case 0xD5: op_cmp_val(c, get_a(c), load_m(c,ea_direct_x(c)), amode16(c)); break;
        case 0xC7: op_cmp_val(c, get_a(c), load_m(c,ea_indl(c)),     amode16(c)); break;
        case 0xD7: op_cmp_val(c, get_a(c), load_m(c,ea_indl_y(c)),   amode16(c)); break;
        case 0xCF: op_cmp_val(c, get_a(c), load_m(c,ea_long(c)),     amode16(c)); break;
        case 0xDF: op_cmp_val(c, get_a(c), load_m(c,ea_long_x(c)),   amode16(c)); break;
        case 0x07: { uint16_t v=(uint16_t)(get_a(c) | load_m(c,ea_indl(c)));   set_a(c,v); set_nz(c,v,amode16(c)); } break;
        case 0x17: { uint16_t v=(uint16_t)(get_a(c) | load_m(c,ea_indl_y(c))); set_a(c,v); set_nz(c,v,amode16(c)); } break;
        case 0x27: { uint16_t v=(uint16_t)(get_a(c) & load_m(c,ea_indl(c)));   set_a(c,v); set_nz(c,v,amode16(c)); } break;
        case 0x37: { uint16_t v=(uint16_t)(get_a(c) & load_m(c,ea_indl_y(c))); set_a(c,v); set_nz(c,v,amode16(c)); } break;
        case 0x47: { uint16_t v=(uint16_t)(get_a(c) ^ load_m(c,ea_indl(c)));   set_a(c,v); set_nz(c,v,amode16(c)); } break;
        case 0x57: { uint16_t v=(uint16_t)(get_a(c) ^ load_m(c,ea_indl_y(c))); set_a(c,v); set_nz(c,v,amode16(c)); } break;
        case 0x0F: { uint16_t v=(uint16_t)(get_a(c) | load_m(c,ea_long(c)));   set_a(c,v); set_nz(c,v,amode16(c)); } break;
        case 0x1F: { uint16_t v=(uint16_t)(get_a(c) | load_m(c,ea_long_x(c))); set_a(c,v); set_nz(c,v,amode16(c)); } break;
        case 0x2F: { uint16_t v=(uint16_t)(get_a(c) & load_m(c,ea_long(c)));   set_a(c,v); set_nz(c,v,amode16(c)); } break;
        case 0x3F: { uint16_t v=(uint16_t)(get_a(c) & load_m(c,ea_long_x(c))); set_a(c,v); set_nz(c,v,amode16(c)); } break;
        case 0x4F: { uint16_t v=(uint16_t)(get_a(c) ^ load_m(c,ea_long(c)));   set_a(c,v); set_nz(c,v,amode16(c)); } break;
        case 0x5F: { uint16_t v=(uint16_t)(get_a(c) ^ load_m(c,ea_long_x(c))); set_a(c,v); set_nz(c,v,amode16(c)); } break;

        /* --- read-modify-write on memory --------------------------------------- */
        case 0x06: rmw_shift(c, ea_direct(c),   0); break;   /* ASL */
        case 0x0E: rmw_shift(c, ea_abs(c),      0); break;
        case 0x16: rmw_shift(c, ea_direct_x(c), 0); break;
        case 0x1E: rmw_shift(c, ea_abs_x(c),    0); break;
        case 0x46: rmw_shift(c, ea_direct(c),   1); break;   /* LSR */
        case 0x4E: rmw_shift(c, ea_abs(c),      1); break;
        case 0x56: rmw_shift(c, ea_direct_x(c), 1); break;
        case 0x5E: rmw_shift(c, ea_abs_x(c),    1); break;
        case 0x26: rmw_shift(c, ea_direct(c),   2); break;   /* ROL */
        case 0x2E: rmw_shift(c, ea_abs(c),      2); break;
        case 0x36: rmw_shift(c, ea_direct_x(c), 2); break;
        case 0x3E: rmw_shift(c, ea_abs_x(c),    2); break;
        case 0x66: rmw_shift(c, ea_direct(c),   3); break;   /* ROR */
        case 0x6E: rmw_shift(c, ea_abs(c),      3); break;
        case 0x76: rmw_shift(c, ea_direct_x(c), 3); break;
        case 0x7E: rmw_shift(c, ea_abs_x(c),    3); break;
        case 0xF6: { uint32_t a=ea_direct_x(c); uint16_t v=(uint16_t)(load_m(c,a)+1); store_m(c,a,v); set_nz(c,v,amode16(c)); } break;
        case 0xFE: { uint32_t a=ea_abs_x(c);    uint16_t v=(uint16_t)(load_m(c,a)+1); store_m(c,a,v); set_nz(c,v,amode16(c)); } break;
        case 0xD6: { uint32_t a=ea_direct_x(c); uint16_t v=(uint16_t)(load_m(c,a)-1); store_m(c,a,v); set_nz(c,v,amode16(c)); } break;
        case 0xDE: { uint32_t a=ea_abs_x(c);    uint16_t v=(uint16_t)(load_m(c,a)-1); store_m(c,a,v); set_nz(c,v,amode16(c)); } break;

        /* --- misc ----------------------------------------------------------- */
        case 0xEA: break;                                             /* NOP */
        case 0xDB: c->stopped = true; break;                          /* STP */
        case 0xCB: c->stopped = true; break;                          /* WAI */

        /* --- the last of the addressing modes -------------------------------
         * Direct-indirect (dp), stack-relative-indirect-indexed (sr,S),Y and
         * direct-indirect-long [dp], plus the long jumps and the block moves.
         * These only started being reached once the board's own interrupt
         * lines were driven -- the handlers at 0xC2EA/0xC2FC/0xC302 use them
         * and the idle loop at 0xC12B does not. */
        case 0xB2: { uint16_t v = load_m(c, ea_ind(c));     set_a(c,v); set_nz(c,v,amode16(c)); } break; /* LDA (dp)   */
        case 0xA7: { uint16_t v = load_m(c, ea_indl(c));    set_a(c,v); set_nz(c,v,amode16(c)); } break; /* LDA [dp]   */
        case 0x92: store_m(c, ea_ind(c),  get_a(c)); break;                                              /* STA (dp)   */
        case 0x87: store_m(c, ea_indl(c), get_a(c)); break;                                              /* STA [dp]   */
        case 0x52: { uint16_t v=(uint16_t)(get_a(c) ^ load_m(c,ea_ind(c)));     set_a(c,v); set_nz(c,v,amode16(c)); } break;
        case 0x53: { uint16_t v=(uint16_t)(get_a(c) ^ load_m(c,ea_stack_y(c))); set_a(c,v); set_nz(c,v,amode16(c)); } break;
        case 0x33: { uint16_t v=(uint16_t)(get_a(c) & load_m(c,ea_stack_y(c))); set_a(c,v); set_nz(c,v,amode16(c)); } break;
        case 0x72: op_adc(c, load_m(c, ea_ind(c)));     break;
        case 0x73: op_adc(c, load_m(c, ea_stack_y(c))); break;
        case 0xF2: op_sbc(c, load_m(c, ea_ind(c)));     break;
        case 0xF3: op_sbc(c, load_m(c, ea_stack_y(c))); break;
        case 0xD3: op_cmp_val(c, get_a(c), load_m(c, ea_stack_y(c)), amode16(c)); break;

        case 0x82: { int16_t d = (int16_t)fetch16(c); c->pc = (uint16_t)(c->pc + d); } break;  /* BRL  */
        case 0x62: { int16_t d = (int16_t)fetch16(c); push16(c, (uint16_t)(c->pc + d)); } break; /* PER */
        case 0xD4: { uint32_t a = ea_direct(c); push16(c, rd16(c, a)); } break;                /* PEI  */
        case 0x7C: { uint16_t t = (uint16_t)(fetch16(c) + c->x);                               /* JMP (abs,X) */
                     c->pc = rd16(c, ((uint32_t)c->pg << 16) | t); } break;
        case 0xFC: { uint16_t t = (uint16_t)(fetch16(c) + c->x);                               /* JSR (abs,X) */
                     uint16_t d = rd16(c, ((uint32_t)c->pg << 16) | t);
                     push16(c, c->pc); c->pc = d; } break;
        case 0xDC: { uint32_t a = fetch16(c);                                                  /* JML [abs] */
                     c->pc = rd16(c, a); c->pg = rd8(c, a + 2); } break;

        /* MVN/MVP: block move, one byte per execution, backing the PC up over
         * its own three bytes until the counter in A:B runs out. A is the
         * count MINUS ONE and ends at 0xFFFF, which is why the loop test is on
         * the 16-bit value and not on zero. */
        case 0x54: case 0x44: {
            int up = (op == 0x54);                      /* MVN ascends, MVP descends */
            uint32_t dstb = (uint32_t)fetch8(c) << 16;
            uint32_t srcb = (uint32_t)fetch8(c) << 16;
            c->dt = (uint8_t)(dstb >> 16);
            wr8(c, dstb | c->y, rd8(c, srcb | c->x));
            if (imode16(c)) { c->x = (uint16_t)(c->x + (up ? 1 : -1));
                              c->y = (uint16_t)(c->y + (up ? 1 : -1)); }
            else { c->x = (uint16_t)((c->x & 0xFF00) | ((c->x + (up ? 1 : -1)) & 0xFF));
                   c->y = (uint16_t)((c->y & 0xFF00) | ((c->y + (up ? 1 : -1)) & 0xFF)); }
            c->a = (uint16_t)(c->a - 1);
            if (c->a != 0xFFFF) c->pc = (uint16_t)(c->pc - 3);
        } break;

        case 0x02: (void)fetch8(c); break;   /* unused two-byte opcode; MAME treats it as a NOP */

    default:
        return false;
    }
    return true;
}
#endif
