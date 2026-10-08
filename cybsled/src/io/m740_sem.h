/* m740_sem.h -- the Mitsubishi 740 family (M37450: the C68 I/O MCU) instruction SEMANTICS, one copy for both uses
 * (cf. src/snd/m6809_sem.h, engine/c25/c25_sem.h):
 *   - the DEV oracle (m37450.c, m740_step): m740_exec(c, NULL) executes the prefetched opcode c->ir and fetches every
 *     operand byte from the bus;
 *   - the TRANSLATED program (gen/<game>_c68.c, tools/gen/m740_translate.py): m740_exec(c, o) with `o` a CONSTANT array
 *     of the instruction's bytes (and the next ones), copied from the ROM at build time -- the compiler folds the
 *     dispatch to that one instruction. Program bytes then cost their bus cycle but are never fetched; every other bus
 *     access (data, stack, the 740's dummy reads) is made exactly as the oracle makes it. Nothing reads ROM CODE at
 *     run time (data tables in the ROM are read through the bus, as the program reads them).
 * That both run this source is what lets the translation be gated as EQUAL to the oracle, which is gated against MAME
 * (cybsled/PLAN.md, Module D: 14,216,885 / 14,216,885 C68 instructions). Moved unchanged from the validated interpreter:
 * every instruction issues the bus accesses (incl. dummy reads) MAME's m740 core does, one cycle each. */
#ifndef M740_SEM_H
#define M740_SEM_H
#include <stddef.h>
#include "m37450.h"

#define M740_SEM static inline __attribute__((always_inline))


#define RD(a)    (c->cycles++, c->rd(c->ctx, (uint16_t)(a)))
#define WR(a, v) do { c->cycles++; c->wr(c->ctx, (uint16_t)(a), (uint8_t)(v)); } while (0)
#define SETL(base, v) (uint16_t)(((base) & 0xff00) | ((v) & 0xff))

/* a byte of the instruction being executed, at c->pc: the translation's constant when it supplies them (o = the bytes at
   c->npc, and the bytes after them -- the 6502-pattern dummy read of the byte past a one-byte instruction included),
   the bus otherwise. Either way one bus cycle. */
M740_SEM uint8_t sem_rpc(m740_t *c, const uint8_t *o)
{
	if (o) { c->cycles++; return o[(uint16_t)(c->pc - c->npc) & 3]; }
	return RD(c->pc);
}

M740_SEM void m740s_set_nz(m740_t *c, uint8_t v)
{
	c->p &= ~(M740_Z | M740_N);
	if (v & 0x80) c->p |= M740_N;
	if (!v) c->p |= M740_Z;
}

/* the opcode fetch that ends every instruction; samples the interrupt line with the CURRENT P
   (CLI / SEI / PLP change P only after it, as on the NMOS 6502) */
M740_SEM void m740s_prefetch(m740_t *c, int xl)
{
	c->npc = c->pc;
	if (xl) c->cycles++;          /* translation: the opcode is a constant of the next case -- the bus cycle, no fetch */
	else c->ir = RD(c->pc);
	if (c->irq_line && !(c->p & M740_I) && !c->replay) {
		c->irq_taken = 1;
		c->ir = 0x00;
	} else
		c->pc++;
}

M740_SEM void m740s_push(m740_t *c, uint8_t v) { WR(0x100 | c->s, v); c->s--; }

/* ---- arithmetic (6502 semantics for A; 740 T-mode semantics work on a memory byte) ---- */
M740_SEM uint8_t m740s_adc8(m740_t *c, uint8_t a, uint8_t val)
{
	uint8_t cin = c->p & M740_C ? 1 : 0;
	if (c->p & M740_D) {
		c->p &= ~(M740_N | M740_V | M740_Z | M740_C);
		uint8_t al = (a & 15) + (val & 15) + cin;
		if (al > 9) al += 6;
		uint8_t ah = (a >> 4) + (val >> 4) + (al > 15);
		if (!(uint8_t)(a + val + cin)) c->p |= M740_Z;
		else if (ah & 8) c->p |= M740_N;
		if (~(a ^ val) & (a ^ (ah << 4)) & 0x80) c->p |= M740_V;
		if (ah > 9) ah += 6;
		if (ah > 15) c->p |= M740_C;
		return (uint8_t)((ah << 4) | (al & 15));
	} else {
		uint16_t sum = a + val + cin;
		c->p &= ~(M740_N | M740_V | M740_Z | M740_C);
		if (!(uint8_t)sum) c->p |= M740_Z;
		else if (sum & 0x80) c->p |= M740_N;
		if (~(a ^ val) & (a ^ sum) & 0x80) c->p |= M740_V;
		if (sum & 0xff00) c->p |= M740_C;
		return (uint8_t)sum;
	}
}

/* NMOS 6502 decimal SBC (used for A) */
M740_SEM uint8_t m740s_sbc8(m740_t *c, uint8_t a, uint8_t val)
{
	uint8_t bin = c->p & M740_C ? 0 : 1;
	uint16_t diff = a - val - bin;
	if (c->p & M740_D) {
		c->p &= ~(M740_N | M740_V | M740_Z | M740_C);
		uint8_t al = (a & 15) - (val & 15) - bin;
		uint8_t ah = (a >> 4) - (val >> 4) - ((int8_t)al < 0);
		if (!(uint8_t)diff) c->p |= M740_Z;
		else if (diff & 0x80) c->p |= M740_N;
		if ((a ^ val) & (a ^ diff) & 0x80) c->p |= M740_V;
		if (!(diff & 0xff00)) c->p |= M740_C;
		if ((int8_t)al < 0) al -= 6;
		if ((int8_t)ah < 0) ah -= 6;
		return (uint8_t)((ah << 4) | (al & 15));
	} else {
		c->p &= ~(M740_N | M740_V | M740_Z | M740_C);
		if (!(uint8_t)diff) c->p |= M740_Z;
		else if (diff & 0x80) c->p |= M740_N;
		if ((a ^ val) & (a ^ diff) & 0x80) c->p |= M740_V;
		if (!(diff & 0xff00)) c->p |= M740_C;
		return (uint8_t)diff;
	}
}

/* 740 T-mode decimal SBC: the low-nibble borrow is adjusted BEFORE the high nibble is computed */
M740_SEM uint8_t m740s_sbct8(m740_t *c, uint8_t a, uint8_t val)
{
	if (!(c->p & M740_D)) return m740s_sbc8(c, a, val);
	uint8_t bin = c->p & M740_C ? 0 : 1;
	c->p &= ~(M740_N | M740_V | M740_Z | M740_C);
	uint16_t diff = a - val - bin;
	uint8_t al = (a & 15) - (val & 15) - bin;
	if ((int8_t)al < 0) al -= 6;
	uint8_t ah = (a >> 4) - (val >> 4) - ((int8_t)al < 0);
	if (!(uint8_t)diff) c->p |= M740_Z;
	else if (diff & 0x80) c->p |= M740_N;
	if ((a ^ val) & (a ^ diff) & 0x80) c->p |= M740_V;
	if (!(diff & 0xff00)) c->p |= M740_C;
	if ((int8_t)ah < 0) ah -= 6;
	return (uint8_t)((ah << 4) | (al & 15));
}

M740_SEM void m740s_cmp8(m740_t *c, uint8_t v1, uint8_t v2)
{
	c->p &= ~(M740_N | M740_Z | M740_C);
	uint16_t r = v1 - v2;
	if (!r) c->p |= M740_Z;
	else if (r & 0x80) c->p |= M740_N;
	if (!(r & 0xff00)) c->p |= M740_C;
}

M740_SEM void m740s_bit8(m740_t *c, uint8_t val)
{
	c->p &= ~(M740_N | M740_Z | M740_V);
	if (!(c->a & val)) c->p |= M740_Z;
	if (val & 0x80) c->p |= M740_N;
	if (val & 0x40) c->p |= M740_V;
}

M740_SEM uint8_t m740s_asl8(m740_t *c, uint8_t v)
{
	c->p &= ~(M740_N | M740_Z | M740_C);
	uint8_t r = (uint8_t)(v << 1);
	if (!r) c->p |= M740_Z; else if (r & 0x80) c->p |= M740_N;
	if (v & 0x80) c->p |= M740_C;
	return r;
}
M740_SEM uint8_t m740s_lsr8(m740_t *c, uint8_t v)
{
	c->p &= ~(M740_N | M740_Z | M740_C);
	if (v & 1) c->p |= M740_C;
	v >>= 1;
	if (!v) c->p |= M740_Z;
	return v;
}
M740_SEM uint8_t m740s_ror8(m740_t *c, uint8_t v)
{
	int cin = c->p & M740_C;
	c->p &= ~(M740_N | M740_Z | M740_C);
	if (v & 1) c->p |= M740_C;
	v >>= 1;
	if (cin) v |= 0x80;
	if (!v) c->p |= M740_Z; else if (v & 0x80) c->p |= M740_N;
	return v;
}
M740_SEM uint8_t m740s_rol8(m740_t *c, uint8_t v)
{
	int cin = c->p & M740_C;
	c->p &= ~(M740_N | M740_Z | M740_C);
	if (v & 0x80) c->p |= M740_C;
	v = (uint8_t)(v << 1);
	if (cin) v |= 1;
	if (!v) c->p |= M740_Z; else if (v & 0x80) c->p |= M740_N;
	return v;
}
M740_SEM uint8_t m740s_inc8(m740_t *c, uint8_t v) { v++; m740s_set_nz(c, v); return v; }
M740_SEM uint8_t m740s_dec8(m740_t *c, uint8_t v) { v--; m740s_set_nz(c, v); return v; }

/* ---- effective addresses: each performs exactly the bus accesses of its addressing mode ---- */
M740_SEM uint16_t m740s_ea_zpg(m740_t *c, const uint8_t *o) { uint16_t t = sem_rpc(c, o); c->pc++; return t; }
M740_SEM uint16_t m740s_ea_aba(m740_t *c, const uint8_t *o) { uint16_t t = sem_rpc(c, o); c->pc++; t |= sem_rpc(c, o) << 8; c->pc++; return t; }
/* 740 zp,X / zp,Y: a dummy read of (PC & 0xff) */
M740_SEM uint16_t m740s_ea_zpi(m740_t *c, const uint8_t *o, uint8_t idx) { uint8_t t = sem_rpc(c, o); RD(c->pc & 0xff); c->pc++; return (uint8_t)(t + idx); }
/* 740 abs,X / abs,Y: always one dummy read at the un-carried address */
M740_SEM uint16_t m740s_ea_abi(m740_t *c, const uint8_t *o, uint8_t idx)
{
	uint16_t t = m740s_ea_aba(c, o);
	RD(SETL(t, t + idx));
	return (uint16_t)(t + idx);
}
M740_SEM uint16_t m740s_ea_idx(m740_t *c, const uint8_t *o)
{
	uint8_t t2 = sem_rpc(c, o); RD(c->pc & 0xff); c->pc++;
	t2 += c->x;
	uint16_t t = RD(t2);
	t |= RD((uint8_t)(t2 + 1)) << 8;
	return t;
}
M740_SEM uint16_t m740s_ea_idy(m740_t *c, const uint8_t *o)
{
	uint8_t t2 = sem_rpc(c, o); c->pc++;
	uint16_t t = RD(t2);
	t |= RD((uint8_t)(t2 + 1)) << 8;
	RD(SETL(t, t + c->y));
	return (uint16_t)(t + c->y);
}

/* read-m740s_operand fetchers for the A / T instructions: mode index from the opcode's low bits */
enum { M_IMM, M_ZPG, M_ZPX, M_ABA, M_ABX, M_ABY, M_IDX, M_IDY };
M740_SEM uint8_t m740s_operand(m740_t *c, const uint8_t *o, int mode)
{
	switch (mode) {
	case M_IMM: { uint8_t v = sem_rpc(c, o); c->pc++; return v; }
	case M_ZPG: return RD(m740s_ea_zpg(c, o));
	case M_ZPX: return RD(m740s_ea_zpi(c, o, c->x));
	case M_ABA: return RD(m740s_ea_aba(c, o));
	case M_ABX: return RD(m740s_ea_abi(c, o, c->x));
	case M_ABY: return RD(m740s_ea_abi(c, o, c->y));
	case M_IDX: return RD(m740s_ea_idx(c, o));
	default:    return RD(m740s_ea_idy(c, o));
	}
}
/* the column of the 01/05/09/0D/11/15/19/1D group */
M740_SEM int m740s_group1_mode(uint8_t op)
{
	switch (op & 0x1f) {
	case 0x01: return M_IDX; case 0x05: return M_ZPG; case 0x09: return M_IMM; case 0x0d: return M_ABA;
	case 0x11: return M_IDY; case 0x15: return M_ZPX; case 0x19: return M_ABY; default: return M_ABX;
	}
}

M740_SEM void m740s_store(m740_t *c, uint16_t ea, uint8_t v) { RD(ea); WR(ea, v); }

M740_SEM void m740s_rmw(m740_t *c, uint16_t ea, uint8_t (*f)(m740_t *, uint8_t))
{
	uint8_t v = RD(ea);
	RD(ea);
	WR(ea, f(c, v));
}

M740_SEM void m740s_branch(m740_t *c, const uint8_t *o, int cond)
{
	if (cond) {
		int8_t off = (int8_t)sem_rpc(c, o);
		RD(SETL(c->pc, c->pc + 1));
		c->pc++;
		RD(SETL(c->pc, c->pc + off));
		c->pc = (uint16_t)(c->pc + off);
	} else {
		sem_rpc(c, o);
		c->pc++;
	}
}

/* power-on / reset (xl: 1 = the translation, whose opcode fetch is a constant) */
static inline void m740_sem_reset(m740_t *c, int xl)
{
	c->a = 0; c->x = 0x80; c->y = 0; c->p = 0x36;   /* MAME's power-on values (P keeps T=E bit 0x20 set) */
	c->s = 0xff;                                    /* M3745x: stack in page 1, SP = 0x01FF */
	c->tbase = 0; c->irq_taken = 0; c->halted = 0;
	c->irq_line = 0; c->irq_vector = 0xfffc; c->force_irq_pc = -1;
	/* reset_m */
	c->p |= M740_I;
	RD(0x100 | c->s); c->s--;
	RD(0x100 | c->s); c->s--;
	RD(0x100 | c->s); c->s--;
	c->pc = RD(0xfffe);
	c->pc |= RD(0xffff) << 8;
	m740s_prefetch(c, xl);
}

/* What every step does before its instruction (oracle and translation alike): the DEV trace hook, a replayed reset or
   interrupt entry, WIT / STP. Returns the cycles of this step if it ended here, -1 to go on and execute the instruction
   at c->npc (c->irq_taken: the interrupt entry). */
static inline int m740_enter(m740_t *c, int xl)
{
	uint64_t c0 = c->cycles;
	if (c->trace && !c->halted) c->trace(c->ctx, c);
	if (c->force_irq_pc == -3) { c->force_irq_pc = -1; return (int)(c->cycles - c0); }   /* the hook reset the CPU */
	if (c->force_irq_pc >= 0) {   /* replayed interrupt entry: the prefetched opcode is not executed */
		c->pc = c->npc;
		m740s_push(c, c->pc >> 8);
		m740s_push(c, (uint8_t)c->pc);
		m740s_push(c, c->p & ~M740_B);
		c->p |= M740_I;
		c->pc = (uint16_t)c->force_irq_pc; c->force_irq_pc = -1;
		c->halted = 0;
		c->cycles += 3;           /* read_pc + 2 vector reads; the 3 pushes are counted by WR, the opcode read by the prefetch */
		m740s_prefetch(c, xl);
		return (int)(c->cycles - c0);
	}
	if (c->halted) {           /* WIT / STP: burn cycles until an interrupt request appears */
		if (!c->irq_line) { c->cycles++; return 1; }
		c->halted = 0;
		m740s_prefetch(c, xl);
		return (int)(c->cycles - c0);
	}
	return -1;
}

/* one instruction (or the interrupt entry): o = NULL executes c->ir from the bus (oracle); o = the constant bytes at
   c->npc (translation; the caller passes {0x00, ...} for an interrupt entry, c->irq_taken). Returns its cycles. */
M740_SEM int m740_exec(m740_t *c, const uint8_t *o)
{
	uint64_t c0 = c->cycles;
	uint8_t op = o ? o[0] : c->ir, t;
	uint16_t ea;

	if (c->tbase) {
		/* T-mode: the A-group instructions operate on the byte at [X] instead of A */
		int kind = -1;
		switch (op) {
		case 0x01: case 0x05: case 0x09: case 0x0d: case 0x11: case 0x15: case 0x19: case 0x1d: kind = 0; break; /* ort */
		case 0x21: case 0x25: case 0x29: case 0x2d: case 0x31: case 0x35: case 0x39: case 0x3d: kind = 1; break; /* andt */
		case 0x41: case 0x45: case 0x49: case 0x4d: case 0x51: case 0x55: case 0x59: case 0x5d: kind = 2; break; /* eort */
		case 0x61: case 0x65: case 0x69: case 0x6d: case 0x71: case 0x75: case 0x79: case 0x7d: kind = 3; break; /* adct */
		case 0xa1: case 0xa5: case 0xa9: case 0xad: case 0xb1: case 0xb5: case 0xb9: case 0xbd: kind = 4; break; /* ldt */
		case 0xc5: case 0xc9: case 0xcd: case 0xd5: case 0xd9: case 0xdd: kind = 5; break;                     /* cmpt (C1/D1 stay CMP) */
		case 0xe1: case 0xe5: case 0xed: case 0xf1: case 0xf5: case 0xfd: kind = 6; break;                     /* sbct (E9/F9 stay SBC) */
		}
		if (kind >= 0) {
			uint8_t src = m740s_operand(c, o, m740s_group1_mode(op)), m;
			switch (kind) {
			case 4: RD(c->x); m740s_set_nz(c, src); WR(c->x, src); break;
			case 5: m = RD(c->x); m740s_cmp8(c, m, src); break;
			default:
				m = RD(c->x); RD(c->x);
				switch (kind) {
				case 0: m |= src; m740s_set_nz(c, m); break;
				case 1: m &= src; m740s_set_nz(c, m); break;
				case 2: m ^= src; m740s_set_nz(c, m); break;
				case 3: m = m740s_adc8(c, m, src); break;
				default: m = m740s_sbct8(c, m, src); break;
				}
				WR(c->x, m);
			}
			m740s_prefetch(c, o != NULL);
			return (int)(c->cycles - c0);
		}
	}

	/* bit instructions: low 5 bits select the form, bits 7-5 the bit number */
	switch (op & 0x1f) {
	case 0x03: case 0x13: {   /* BBS / BBC A,rel */
		int b = (op >> 5) & 7;
		sem_rpc(c, o); sem_rpc(c, o);
		m740s_branch(c, o, ((c->a >> b) & 1) == ((op & 0x10) ? 0 : 1));
		m740s_prefetch(c, o != NULL);
		return (int)(c->cycles - c0);
	}
	case 0x07: case 0x17: {   /* BBS / BBC zp,rel */
		int b = (op >> 5) & 7;
		ea = m740s_ea_zpg(c, o);
		t = RD(ea);
		sem_rpc(c, o);
		m740s_branch(c, o, ((t >> b) & 1) == ((op & 0x10) ? 0 : 1));
		m740s_prefetch(c, o != NULL);
		return (int)(c->cycles - c0);
	}
	case 0x0b: case 0x1b: {   /* SEB / CLB A */
		int b = (op >> 5) & 7;
		sem_rpc(c, o);
		if (op & 0x10) c->a &= ~(1 << b); else c->a |= (1 << b);
		m740s_prefetch(c, o != NULL);
		return (int)(c->cycles - c0);
	}
	case 0x0f: case 0x1f: {   /* SEB / CLB zp */
		int b = (op >> 5) & 7;
		ea = m740s_ea_zpg(c, o);
		t = RD(ea);
		RD(ea);
		if (op & 0x10) t &= ~(1 << b); else t |= (1 << b);
		WR(ea, t);
		m740s_prefetch(c, o != NULL);
		return (int)(c->cycles - c0);
	}
	}

	switch (op) {
	/* ---- interrupts / flow ---- */
	case 0x00: {   /* BRK, and the interrupt entry */
		int irq = c->irq_taken;
		sem_rpc(c, o);
		if (!irq) c->pc++;
		m740s_push(c, c->pc >> 8);
		m740s_push(c, (uint8_t)c->pc);
		m740s_push(c, irq ? (c->p & ~M740_B) : c->p);
		c->pc = RD(c->irq_vector);
		c->pc |= RD(c->irq_vector + 1) << 8;
		c->irq_taken = 0;
		c->p |= M740_I;
		break;
	}
	case 0x20: {   /* JSR abs */
		uint16_t lo = sem_rpc(c, o); c->pc++;
		RD(0x100 | c->s);
		m740s_push(c, c->pc >> 8); m740s_push(c, (uint8_t)c->pc);
		lo |= sem_rpc(c, o) << 8; c->pc++;
		c->pc = lo;
		break;
	}
	case 0x22: {   /* JSR \$FFxx (special page) */
		t = sem_rpc(c, o);
		RD(0x100 | c->s);
		m740s_push(c, c->pc >> 8); m740s_push(c, (uint8_t)c->pc);
		c->pc = 0xff00 | t;
		break;
	}
	case 0x02: {   /* JSR (zp) -- pushes the address of the m740s_operand byte */
		uint8_t z = sem_rpc(c, o);
		RD(0x100 | c->s);
		m740s_push(c, c->pc >> 8); m740s_push(c, (uint8_t)c->pc);
		uint16_t d = RD(z);
		d |= RD((uint8_t)(z + 1)) << 8;
		c->pc = d;
		break;
	}
	case 0xb2: {   /* JMP (zp) */
		uint8_t z = sem_rpc(c, o);
		uint16_t d = RD(z);
		d |= RD((uint8_t)(z + 1)) << 8;
		c->pc = d;
		break;
	}
	case 0x4c: { uint16_t d = sem_rpc(c, o); c->pc++; d |= sem_rpc(c, o) << 8; c->pc = d; break; }   /* JMP abs */
	case 0x6c: {   /* JMP (abs), NMOS page bug */
		uint16_t p = m740s_ea_aba(c, o);
		uint16_t d = RD(p);
		d |= RD(SETL(p, p + 1)) << 8;
		c->pc = d;
		break;
	}
	case 0x40:     /* RTI */
		sem_rpc(c, o);
		RD(0x100 | c->s); c->s++;
		c->p = RD(0x100 | c->s) | M740_B;
		c->tbase = (c->p & M740_T) ? 0x100 : 0;
		c->s++;
		c->pc = RD(0x100 | c->s);
		c->s++;
		c->pc |= RD(0x100 | c->s) << 8;
		break;
	case 0x60:     /* RTS */
		sem_rpc(c, o);
		RD(0x100 | c->s); c->s++;
		c->pc = RD(0x100 | c->s);
		c->s++;
		c->pc |= RD(0x100 | c->s) << 8;
		RD(c->pc);
		c->pc++;
		break;
	case 0x80: m740s_branch(c, o, 1); break;                       /* BRA */
	case 0x10: m740s_branch(c, o, !(c->p & M740_N)); break;
	case 0x30: m740s_branch(c, o, (c->p & M740_N)); break;
	case 0x50: m740s_branch(c, o, !(c->p & M740_V)); break;
	case 0x70: m740s_branch(c, o, (c->p & M740_V)); break;
	case 0x90: m740s_branch(c, o, !(c->p & M740_C)); break;
	case 0xb0: m740s_branch(c, o, (c->p & M740_C)); break;
	case 0xd0: m740s_branch(c, o, !(c->p & M740_Z)); break;
	case 0xf0: m740s_branch(c, o, (c->p & M740_Z)); break;

	/* ---- stack / flags (P changes after the m740s_prefetch for PLP / CLI / SEI) ---- */
	case 0x08: sem_rpc(c, o); m740s_push(c, c->p); break;                 /* PHP */
	case 0x48: sem_rpc(c, o); m740s_push(c, c->a); break;                 /* PHA */
	case 0x68: sem_rpc(c, o); RD(0x100 | c->s); c->s++; c->a = RD(0x100 | c->s); m740s_set_nz(c, c->a); break;  /* PLA */
	case 0x28: {   /* PLP */
		sem_rpc(c, o); RD(0x100 | c->s); c->s++;
		uint8_t np = RD(0x100 | c->s) | M740_B;
		c->tbase = (np & M740_T) ? 0x100 : 0;
		m740s_prefetch(c, o != NULL);
		c->p = np;
		return (int)(c->cycles - c0);
	}
	case 0x58: sem_rpc(c, o); m740s_prefetch(c, o != NULL); c->p &= ~M740_I; return (int)(c->cycles - c0);   /* CLI */
	case 0x78: sem_rpc(c, o); m740s_prefetch(c, o != NULL); c->p |= M740_I; return (int)(c->cycles - c0);    /* SEI */
	case 0x18: sem_rpc(c, o); c->p &= ~M740_C; break;
	case 0x38: sem_rpc(c, o); c->p |= M740_C; break;
	case 0xb8: sem_rpc(c, o); c->p &= ~M740_V; break;
	case 0xd8: sem_rpc(c, o); c->p &= ~M740_D; break;
	case 0xf8: sem_rpc(c, o); c->p |= M740_D; break;
	case 0x32: sem_rpc(c, o); c->p |= M740_T; c->tbase = 0x100; break;   /* SET */
	case 0x12: sem_rpc(c, o); c->p &= ~M740_T; c->tbase = 0; break;     /* CLT */
	case 0x42: case 0xc2:                                       /* STP / WIT */
		sem_rpc(c, o);
		if (!c->irq_line) { c->halted = 1; return (int)(c->cycles - c0); }
		break;

	/* ---- register ops ---- */
	case 0xaa: sem_rpc(c, o); c->x = c->a; m740s_set_nz(c, c->x); break;
	case 0xa8: sem_rpc(c, o); c->y = c->a; m740s_set_nz(c, c->y); break;
	case 0x8a: sem_rpc(c, o); c->a = c->x; m740s_set_nz(c, c->a); break;
	case 0x98: sem_rpc(c, o); c->a = c->y; m740s_set_nz(c, c->a); break;
	case 0xba: sem_rpc(c, o); c->x = c->s; m740s_set_nz(c, c->x); break;
	case 0x9a: sem_rpc(c, o); c->s = c->x; break;
	case 0xe8: sem_rpc(c, o); c->x++; m740s_set_nz(c, c->x); break;
	case 0xc8: sem_rpc(c, o); c->y++; m740s_set_nz(c, c->y); break;
	case 0xca: sem_rpc(c, o); c->x--; m740s_set_nz(c, c->x); break;
	case 0x88: sem_rpc(c, o); c->y--; m740s_set_nz(c, c->y); break;
	case 0x1a: sem_rpc(c, o); c->a--; m740s_set_nz(c, c->a); break;   /* DEC A */
	case 0x3a: sem_rpc(c, o); c->a++; m740s_set_nz(c, c->a); break;   /* INC A */
	case 0x0a: sem_rpc(c, o); c->a = m740s_asl8(c, c->a); break;
	case 0x2a: sem_rpc(c, o); c->a = m740s_rol8(c, c->a); break;
	case 0x4a: sem_rpc(c, o); c->a = m740s_lsr8(c, c->a); break;
	case 0x6a: sem_rpc(c, o); c->a = m740s_ror8(c, c->a); break;
	case 0xea: case 0x5a: case 0x7a: case 0xda: case 0xfa: sem_rpc(c, o); break;   /* NOP */

	/* ---- the A group (01..1D columns) ---- */
	case 0x01: case 0x05: case 0x09: case 0x0d: case 0x11: case 0x15: case 0x19: case 0x1d:
		c->a |= m740s_operand(c, o, m740s_group1_mode(op)); m740s_set_nz(c, c->a); break;
	case 0x21: case 0x25: case 0x29: case 0x2d: case 0x31: case 0x35: case 0x39: case 0x3d:
		c->a &= m740s_operand(c, o, m740s_group1_mode(op)); m740s_set_nz(c, c->a); break;
	case 0x41: case 0x45: case 0x49: case 0x4d: case 0x51: case 0x55: case 0x59: case 0x5d:
		c->a ^= m740s_operand(c, o, m740s_group1_mode(op)); m740s_set_nz(c, c->a); break;
	case 0x61: case 0x65: case 0x69: case 0x6d: case 0x71: case 0x75: case 0x79: case 0x7d:
		c->a = m740s_adc8(c, c->a, m740s_operand(c, o, m740s_group1_mode(op))); break;
	case 0xa1: case 0xa5: case 0xa9: case 0xad: case 0xb1: case 0xb5: case 0xb9: case 0xbd:
		c->a = m740s_operand(c, o, m740s_group1_mode(op)); m740s_set_nz(c, c->a); break;
	case 0xc1: case 0xc5: case 0xc9: case 0xcd: case 0xd1: case 0xd5: case 0xd9: case 0xdd:
		m740s_cmp8(c, c->a, m740s_operand(c, o, m740s_group1_mode(op))); break;
	case 0xf5: {   /* SBC zp,X uses the plain 6502 access pattern on the 740 */
		uint8_t z = sem_rpc(c, o); c->pc++;
		RD(z);
		c->a = m740s_sbc8(c, c->a, RD((uint8_t)(z + c->x)));
		break;
	}
	case 0xe1: case 0xe5: case 0xe9: case 0xed: case 0xf1: case 0xf9: case 0xfd:
		c->a = m740s_sbc8(c, c->a, m740s_operand(c, o, m740s_group1_mode(op))); break;

	/* stores */
	case 0x81: m740s_store(c, m740s_ea_idx(c, o), c->a); break;
	case 0x85: m740s_store(c, m740s_ea_zpg(c, o), c->a); break;
	case 0x8d: m740s_store(c, m740s_ea_aba(c, o), c->a); break;
	case 0x91: m740s_store(c, m740s_ea_idy(c, o), c->a); break;
	case 0x95: m740s_store(c, m740s_ea_zpi(c, o, c->x), c->a); break;
	case 0x99: m740s_store(c, m740s_ea_abi(c, o, c->y), c->a); break;
	case 0x9d: m740s_store(c, m740s_ea_abi(c, o, c->x), c->a); break;
	case 0x86: m740s_store(c, m740s_ea_zpg(c, o), c->x); break;
	case 0x8e: m740s_store(c, m740s_ea_aba(c, o), c->x); break;
	case 0x96: m740s_store(c, m740s_ea_zpi(c, o, c->y), c->x); break;
	case 0x84: m740s_store(c, m740s_ea_zpg(c, o), c->y); break;
	case 0x8c: m740s_store(c, m740s_ea_aba(c, o), c->y); break;
	case 0x94: m740s_store(c, m740s_ea_zpi(c, o, c->x), c->y); break;
	case 0x3c: { uint8_t v = sem_rpc(c, o); c->pc++; uint8_t z = sem_rpc(c, o); c->pc++; WR(z, v); break; }   /* LDM #imm,zp */

	/* X / Y loads and compares */
	case 0xa2: c->x = sem_rpc(c, o); c->pc++; m740s_set_nz(c, c->x); break;
	case 0xa6: c->x = RD(m740s_ea_zpg(c, o)); m740s_set_nz(c, c->x); break;
	case 0xae: c->x = RD(m740s_ea_aba(c, o)); m740s_set_nz(c, c->x); break;
	case 0xb6: c->x = RD(m740s_ea_zpi(c, o, c->y)); m740s_set_nz(c, c->x); break;
	case 0xbe: c->x = RD(m740s_ea_abi(c, o, c->y)); m740s_set_nz(c, c->x); break;
	case 0xa0: c->y = sem_rpc(c, o); c->pc++; m740s_set_nz(c, c->y); break;
	case 0xa4: c->y = RD(m740s_ea_zpg(c, o)); m740s_set_nz(c, c->y); break;
	case 0xac: c->y = RD(m740s_ea_aba(c, o)); m740s_set_nz(c, c->y); break;
	case 0xb4: c->y = RD(m740s_ea_zpi(c, o, c->x)); m740s_set_nz(c, c->y); break;
	case 0xbc: c->y = RD(m740s_ea_abi(c, o, c->x)); m740s_set_nz(c, c->y); break;
	case 0xe0: t = sem_rpc(c, o); c->pc++; m740s_cmp8(c, c->x, t); break;
	case 0xe4: m740s_cmp8(c, c->x, RD(m740s_ea_zpg(c, o))); break;
	case 0xec: m740s_cmp8(c, c->x, RD(m740s_ea_aba(c, o))); break;
	case 0xc0: t = sem_rpc(c, o); c->pc++; m740s_cmp8(c, c->y, t); break;
	case 0xc4: m740s_cmp8(c, c->y, RD(m740s_ea_zpg(c, o))); break;
	case 0xcc: m740s_cmp8(c, c->y, RD(m740s_ea_aba(c, o))); break;
	case 0x24: m740s_bit8(c, RD(m740s_ea_zpg(c, o))); break;
	case 0x2c: m740s_bit8(c, RD(m740s_ea_aba(c, o))); break;

	/* read-modify-write */
	case 0x06: m740s_rmw(c, m740s_ea_zpg(c, o), m740s_asl8); break;
	case 0x0e: m740s_rmw(c, m740s_ea_aba(c, o), m740s_asl8); break;
	case 0x16: m740s_rmw(c, m740s_ea_zpi(c, o, c->x), m740s_asl8); break;
	case 0x1e: m740s_rmw(c, m740s_ea_abi(c, o, c->x), m740s_asl8); break;
	case 0x26: m740s_rmw(c, m740s_ea_zpg(c, o), m740s_rol8); break;
	case 0x2e: m740s_rmw(c, m740s_ea_aba(c, o), m740s_rol8); break;
	case 0x36: m740s_rmw(c, m740s_ea_zpi(c, o, c->x), m740s_rol8); break;
	case 0x3e: m740s_rmw(c, m740s_ea_abi(c, o, c->x), m740s_rol8); break;
	case 0x46: m740s_rmw(c, m740s_ea_zpg(c, o), m740s_lsr8); break;
	case 0x4e: m740s_rmw(c, m740s_ea_aba(c, o), m740s_lsr8); break;
	case 0x56: m740s_rmw(c, m740s_ea_zpi(c, o, c->x), m740s_lsr8); break;
	case 0x5e: m740s_rmw(c, m740s_ea_abi(c, o, c->x), m740s_lsr8); break;
	case 0x66: m740s_rmw(c, m740s_ea_zpg(c, o), m740s_ror8); break;
	case 0x6e: m740s_rmw(c, m740s_ea_aba(c, o), m740s_ror8); break;
	case 0x76: m740s_rmw(c, m740s_ea_zpi(c, o, c->x), m740s_ror8); break;
	case 0x7e: m740s_rmw(c, m740s_ea_abi(c, o, c->x), m740s_ror8); break;
	case 0xc6: m740s_rmw(c, m740s_ea_zpg(c, o), m740s_dec8); break;
	case 0xce: m740s_rmw(c, m740s_ea_aba(c, o), m740s_dec8); break;
	case 0xd6: m740s_rmw(c, m740s_ea_zpi(c, o, c->x), m740s_dec8); break;
	case 0xde: m740s_rmw(c, m740s_ea_abi(c, o, c->x), m740s_dec8); break;
	case 0xe6: m740s_rmw(c, m740s_ea_zpg(c, o), m740s_inc8); break;
	case 0xee: m740s_rmw(c, m740s_ea_aba(c, o), m740s_inc8); break;
	case 0xf6: m740s_rmw(c, m740s_ea_zpi(c, o, c->x), m740s_inc8); break;
	case 0xfe: m740s_rmw(c, m740s_ea_abi(c, o, c->x), m740s_inc8); break;
	case 0x44: { ea = m740s_ea_zpg(c, o); t = RD(ea); RD(ea); t ^= 0xff; m740s_set_nz(c, t); WR(ea, t); break; }   /* COM zp */
	case 0x82: { ea = m740s_ea_zpg(c, o); t = RD(ea); RD(ea); RD(ea); RD(ea); RD(ea);                       /* RRF zp */
		WR(ea, (uint8_t)((t << 4) | (t >> 4))); break; }
	case 0x64: m740s_set_nz(c, RD(m740s_ea_zpg(c, o))); break;                                                   /* TST zp */

	/* NOPs with operands (the 6502 access patterns) */
	case 0x04: RD(m740s_ea_zpg(c, o)); break;
	case 0x0c: RD(m740s_ea_aba(c, o)); break;
	case 0x89: case 0xe2: sem_rpc(c, o); c->pc++; break;
	case 0x14: case 0x34: case 0x54: case 0x74: case 0xd4: case 0xf4: {
		uint8_t z = sem_rpc(c, o); c->pc++; RD(z); RD((uint8_t)(z + c->x)); break;
	}
	case 0x1c: case 0x5c: case 0x7c: case 0xdc: case 0xfc: {
		uint16_t a = m740s_ea_aba(c, o);
		if ((a & 0xff00) != ((a + c->x) & 0xff00)) RD(SETL(a, a + c->x));
		RD((uint16_t)(a + c->x));
		break;
	}

	default:   /* KIL / undocumented: stop like the 6502's system killers */
		c->pc--;
		c->halted = 2;
		return (int)(c->cycles - c0);
	}
	m740s_prefetch(c, o != NULL);
	return (int)(c->cycles - c0);
}

#endif
