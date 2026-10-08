/* m6809_sem.h -- the MC6809E instruction SEMANTICS, one copy for both uses (cf. engine/c25/c25_sem.h):
 *   - the DEV oracle (m6809.c, m6809_step): m6809_exec(c, NULL) fetches every instruction byte from the bus;
 *   - the TRANSLATED program (gen/<game>_6809.c, tools/gen/m6809_translate.py): m6809_exec(c, o) with `o` a CONSTANT array
 *     holding that one instruction's bytes, copied from the ROM at build time -- the compiler folds the dispatch below to
 *     that single instruction. Nothing reads ROM code at run time.
 * That both run this source is what lets the translation be gated as EQUAL to the oracle, which is gated against MAME
 * (cybsled/PLAN.md, Module D: 12,360,693 / 12,360,693 instructions). Moved unchanged from the validated interpreter;
 * cycle counts are the Motorola datasheet's. */
#ifndef M6809_SEM_H
#define M6809_SEM_H
#include <stddef.h>
#include "m6809.h"

#define M6809_SEM static inline __attribute__((always_inline))

#define RD(a)    (c->rd(c->ctx, (uint16_t)(a)))
#define WR(a, v) (c->wr(c->ctx, (uint16_t)(a), (uint8_t)(v)))
#define D_GET()  ((uint16_t)(c->a << 8 | c->b))
#define D_SET(v) do { uint16_t _v = (uint16_t)(v); c->a = _v >> 8; c->b = (uint8_t)_v; } while (0)

/* an instruction byte: the translation's constant when it supplies them (*o), the bus (or the DEV fetch hook) otherwise;
   PC moves past it either way */
M6809_SEM uint8_t sem_fetch(m6809_t *c, const uint8_t **o)
{
	uint8_t v;
	if (*o) v = *(*o)++;
	else if (c->fetch) v = c->fetch(c->ctx, c->pc);
	else v = RD(c->pc);
	c->pc++;
	return v;
}
M6809_SEM uint16_t sem_fetch16(m6809_t *c, const uint8_t **o) { uint16_t h = sem_fetch(c, o); return (uint16_t)(h << 8 | sem_fetch(c, o)); }
M6809_SEM uint16_t sem_rd16(m6809_t *c, uint16_t a) { uint16_t h = RD(a); return (uint16_t)(h << 8 | RD((uint16_t)(a + 1))); }
M6809_SEM void sem_wr16(m6809_t *c, uint16_t a, uint16_t v) { WR(a, v >> 8); WR((uint16_t)(a + 1), v); }

M6809_SEM void sem_pushs8(m6809_t *c, uint8_t v) { WR(--c->s, v); }
M6809_SEM void sem_pushs16(m6809_t *c, uint16_t v) { WR(--c->s, v); WR(--c->s, v >> 8); }
M6809_SEM uint8_t sem_puls8(m6809_t *c) { return RD(c->s++); }
M6809_SEM uint16_t sem_puls16(m6809_t *c) { uint16_t h = RD(c->s); uint16_t v = (uint16_t)(h << 8 | RD((uint16_t)(c->s + 1))); c->s += 2; return v; }

/* ---- flags ---- */
M6809_SEM void sem_nz8(m6809_t *c, uint8_t v) { c->cc &= ~(CC_N | CC_Z); if (v & 0x80) c->cc |= CC_N; if (!v) c->cc |= CC_Z; }
M6809_SEM void sem_nz16(m6809_t *c, uint16_t v) { c->cc &= ~(CC_N | CC_Z); if (v & 0x8000) c->cc |= CC_N; if (!v) c->cc |= CC_Z; }

M6809_SEM uint8_t sem_add8(m6809_t *c, uint8_t a, uint8_t b, int carry)
{
	unsigned r = a + b + (carry ? 1 : 0);
	c->cc &= ~(CC_H | CC_N | CC_Z | CC_V | CC_C);
	if ((a ^ b ^ r) & 0x10) c->cc |= CC_H;
	if (r & 0x80) c->cc |= CC_N;
	if (!(r & 0xff)) c->cc |= CC_Z;
	if ((a ^ r) & (b ^ r) & 0x80) c->cc |= CC_V;
	if (r & 0x100) c->cc |= CC_C;
	return (uint8_t)r;
}
M6809_SEM uint8_t sem_sub8(m6809_t *c, uint8_t a, uint8_t b, int borrow)
{
	unsigned r = a - b - (borrow ? 1 : 0);
	c->cc &= ~(CC_N | CC_Z | CC_V | CC_C);
	if (r & 0x80) c->cc |= CC_N;
	if (!(r & 0xff)) c->cc |= CC_Z;
	if ((a ^ b) & (a ^ r) & 0x80) c->cc |= CC_V;
	if (r & 0x100) c->cc |= CC_C;
	return (uint8_t)r;
}
M6809_SEM uint16_t sem_add16(m6809_t *c, uint16_t a, uint16_t b)
{
	uint32_t r = (uint32_t)a + b;
	c->cc &= ~(CC_N | CC_Z | CC_V | CC_C);
	if (r & 0x8000) c->cc |= CC_N;
	if (!(r & 0xffff)) c->cc |= CC_Z;
	if ((a ^ r) & (b ^ r) & 0x8000) c->cc |= CC_V;
	if (r & 0x10000) c->cc |= CC_C;
	return (uint16_t)r;
}
M6809_SEM uint16_t sem_sub16(m6809_t *c, uint16_t a, uint16_t b)
{
	uint32_t r = (uint32_t)a - b;
	c->cc &= ~(CC_N | CC_Z | CC_V | CC_C);
	if (r & 0x8000) c->cc |= CC_N;
	if (!(r & 0xffff)) c->cc |= CC_Z;
	if ((a ^ b) & (a ^ r) & 0x8000) c->cc |= CC_V;
	if (r & 0x10000) c->cc |= CC_C;
	return (uint16_t)r;
}

/* the read-modify-write group 0x?0..0x?F (row 0, 4, 5, 6, 7); returns -1 for no write back (TST) */
M6809_SEM int sem_rmw(m6809_t *c, int op, uint8_t v)
{
	switch (op & 0x0f) {
	case 0x0: case 0x1: v = sem_sub8(c, 0, v, 0); return v;                                 /* NEG */
	case 0x3: v = (uint8_t)~v; sem_nz8(c, v); c->cc &= ~CC_V; c->cc |= CC_C; return v;        /* COM */
	case 0x4: case 0x5: c->cc &= ~(CC_N | CC_Z | CC_C); if (v & 1) c->cc |= CC_C; v >>= 1;  /* LSR */
		if (!v) c->cc |= CC_Z; return v;
	case 0x6: { int ci = c->cc & CC_C; c->cc &= ~CC_C; if (v & 1) c->cc |= CC_C;              /* ROR */
		v = (uint8_t)((v >> 1) | (ci ? 0x80 : 0)); sem_nz8(c, v); return v; }
	case 0x7: c->cc &= ~CC_C; if (v & 1) c->cc |= CC_C; v = (uint8_t)((int8_t)v >> 1); sem_nz8(c, v); return v; /* ASR */
	case 0x8: { c->cc &= ~(CC_C | CC_V); if (v & 0x80) c->cc |= CC_C;                         /* ASL */
		if ((v ^ (v << 1)) & 0x80) c->cc |= CC_V; v = (uint8_t)(v << 1); sem_nz8(c, v); return v; }
	case 0x9: { int ci = c->cc & CC_C; c->cc &= ~(CC_C | CC_V); if (v & 0x80) c->cc |= CC_C;   /* ROL */
		if ((v ^ (v << 1)) & 0x80) c->cc |= CC_V; v = (uint8_t)((v << 1) | (ci ? 1 : 0)); sem_nz8(c, v); return v; }
	case 0xa: case 0xb: c->cc &= ~CC_V; if (v == 0x80) c->cc |= CC_V; v--; sem_nz8(c, v); return v;   /* DEC */
	case 0xc: c->cc &= ~CC_V; if (v == 0x7f) c->cc |= CC_V; v++; sem_nz8(c, v); return v;              /* INC */
	case 0xd: sem_nz8(c, v); c->cc &= ~CC_V; return -1;                                                  /* TST */
	case 0xf: case 0xe: c->cc &= ~(CC_N | CC_V | CC_C); c->cc |= CC_Z; return 0;                       /* CLR */
	default: sem_nz8(c, (uint8_t)(-v)); return (uint8_t)(-v);                                           /* 0x2: XNC (undocumented) ~ NEG */
	}
}

M6809_SEM uint16_t *sem_reg16(m6809_t *c, int r)
{
	switch (r & 3) { case 0: return &c->x; case 1: return &c->y; case 2: return &c->u; default: return &c->s; }
}

/* indexed addressing: returns the effective address, adds the extra cycles to *cyc */
M6809_SEM uint16_t sem_indexed(m6809_t *c, const uint8_t **o, int *cyc)
{
	uint8_t pb = sem_fetch(c, o);
	uint16_t *r = sem_reg16(c, pb >> 5), ea;
	if (!(pb & 0x80)) {
		int off = pb & 0x1f; if (off & 0x10) off -= 0x20;
		*cyc += 1;
		return (uint16_t)(*r + off);
	}
	int ind = pb & 0x10;
	switch (pb & 0x0f) {
	case 0x0: ea = *r; *r += 1; *cyc += 2; break;
	case 0x1: ea = *r; *r += 2; *cyc += 3; break;
	case 0x2: *r -= 1; ea = *r; *cyc += 2; break;
	case 0x3: *r -= 2; ea = *r; *cyc += 3; break;
	case 0x4: ea = *r; break;
	case 0x5: ea = (uint16_t)(*r + (int8_t)c->b); *cyc += 1; break;
	case 0x6: ea = (uint16_t)(*r + (int8_t)c->a); *cyc += 1; break;
	case 0x8: { int8_t off = (int8_t)sem_fetch(c, o); ea = (uint16_t)(*r + off); *cyc += 1; break; }
	case 0x9: { uint16_t off = sem_fetch16(c, o); ea = (uint16_t)(*r + off); *cyc += 4; break; }
	case 0xb: ea = (uint16_t)(*r + D_GET()); *cyc += 4; break;
	case 0xc: { int8_t off = (int8_t)sem_fetch(c, o); ea = (uint16_t)(c->pc + off); *cyc += 1; break; }
	case 0xd: { uint16_t off = sem_fetch16(c, o); ea = (uint16_t)(c->pc + off); *cyc += 5; break; }
	case 0xf: ea = sem_fetch16(c, o); *cyc += 2; break;   /* [n16]: 2 + the 3 of the indirection = 5 */
	default: ea = *r; c->bad_op = 0x100 | pb; break;
	}
	if (ind) { ea = sem_rd16(c, ea); *cyc += 3; }
	return ea;
}

M6809_SEM uint16_t sem_tfr_get(m6809_t *c, int r)
{
	switch (r) {
	case 0: return D_GET(); case 1: return c->x; case 2: return c->y; case 3: return c->u; case 4: return c->s; case 5: return c->pc;
	case 8: return 0xff00 | c->a; case 9: return 0xff00 | c->b; case 10: return 0xff00 | c->cc; case 11: return 0xff00 | c->dp;
	default: return 0xffff;
	}
}
M6809_SEM void sem_tfr_set(m6809_t *c, int r, uint16_t v)
{
	switch (r) {
	case 0: D_SET(v); break; case 1: c->x = v; break; case 2: c->y = v; break; case 3: c->u = v; break; case 4: c->s = v; break;
	case 5: c->pc = v; break; case 8: c->a = (uint8_t)v; break; case 9: c->b = (uint8_t)v; break; case 10: c->cc = (uint8_t)v; break;
	case 11: c->dp = (uint8_t)v; break;
	}
}

M6809_SEM void sem_push_all(m6809_t *c)
{
	sem_pushs16(c, c->pc); sem_pushs16(c, c->u); sem_pushs16(c, c->y); sem_pushs16(c, c->x);
	sem_pushs8(c, c->dp); sem_pushs8(c, c->b); sem_pushs8(c, c->a); sem_pushs8(c, c->cc);
}

M6809_SEM int sem_cond(m6809_t *c, int op)
{
	uint8_t cc = c->cc;
	int n = !!(cc & CC_N), v = !!(cc & CC_V), z = !!(cc & CC_Z), cy = !!(cc & CC_C), r;
	switch ((op >> 1) & 7) {
	case 0: r = 1; break;                 /* BRA / BRN */
	case 1: r = !(cy || z); break;        /* BHI / BLS */
	case 2: r = !cy; break;               /* BCC / BCS */
	case 3: r = !z; break;                /* BNE / BEQ */
	case 4: r = !v; break;                /* BVC / BVS */
	case 5: r = !n; break;                /* BPL / BMI */
	case 6: r = n == v; break;            /* BGE / BLT */
	default: r = !z && n == v; break;     /* BGT / BLE */
	}
	return (op & 1) ? !r : r;
}

/* take a pending interrupt if any; returns its cycles or 0 */
static inline int m6809_interrupt(m6809_t *c)
{
	int firq = c->firq_line && !(c->cc & CC_F);
	int irq = c->irq_line && !(c->cc & CC_I);
	if (c->sync) {
		if (c->firq_line || c->irq_line || c->nmi_pending) c->sync = 0;   /* SYNC ends on any line, masked or not */
		else return 1;
	}
	if (c->nmi_pending) {
		c->nmi_pending = 0;
		if (!c->cwai) { c->cc |= CC_E; sem_push_all(c); }
		c->cwai = 0;
		c->cc |= CC_I | CC_F;
		c->pc = sem_rd16(c, 0xfffc);
		return 19;
	}
	if (firq) {
		int n = 10;
		if (!c->cwai) { c->cc &= ~CC_E; sem_pushs16(c, c->pc); sem_pushs8(c, c->cc); }
		else n = 7;
		c->cwai = 0;
		c->cc |= CC_I | CC_F;
		c->pc = sem_rd16(c, 0xfff6);
		return n;
	}
	if (irq) {
		int n = 19;
		if (!c->cwai) { c->cc |= CC_E; sem_push_all(c); }
		else n = 7;
		c->cwai = 0;
		c->cc |= CC_I;
		c->pc = sem_rd16(c, 0xfff8);
		return n;
	}
	if (c->cwai) return 1;
	return 0;
}

/* power-on / reset line release (oracle and translation alike) */
static inline void m6809_sem_reset(m6809_t *c)
{
	c->dp = 0; c->cc |= CC_I | CC_F;
	c->firq_line = c->irq_line = c->nmi_pending = 0;
	c->cwai = c->sync = 0;
	c->force_firq_pc = -1;
	c->pc = sem_rd16(c, 0xfffe);
}

/* What every step does before its instruction (oracle and translation alike): the interrupt lines, the DEV trace hook, a
   replayed reset or FIRQ entry. Returns the cycles of this step if it ended here (the caller returns them, already added
   to c->cycles), -1 to go on and execute the instruction at c->pc. */
static inline int m6809_enter(m6809_t *c)
{
	int cyc = c->replay ? 0 : m6809_interrupt(c);
	if (cyc) { c->cycles += cyc; return cyc; }
	if (c->trace) c->trace(c->ctx, c);
	if (c->force_firq_pc == -3) { c->force_firq_pc = -1; return 0; }   /* the hook reset the CPU */
	if (c->force_firq_pc >= 0) {        /* replayed interrupt entry (FIRQ: PC, CC with E clear) */
		c->cc &= ~CC_E; sem_pushs16(c, c->pc); sem_pushs8(c, c->cc);
		c->cc |= CC_I | CC_F;
		c->pc = (uint16_t)c->force_firq_pc; c->force_firq_pc = -1;
		c->cycles += 10; return 10;
	}
	return -1;
}

/* ONE instruction at c->pc. ops: its bytes (the translation's constant), or NULL to fetch them. Returns its cycles (also
   added to c->cycles). */
M6809_SEM int m6809_exec(m6809_t *c, const uint8_t *ops)
{
	const uint8_t *o_ = ops, **o = &o_;
	int cyc = 0;
	uint8_t op = sem_fetch(c, o);
	uint16_t ea = 0, t16;
	uint8_t t8;
	int page = 0;

	if (op == 0x10 || op == 0x11) {
		page = op;
		op = sem_fetch(c, o);
		/* consecutive prefixes: the 6809 ignores extra ones */
		while (op == 0x10 || op == 0x11) { cyc += 1; op = sem_fetch(c, o); }
	}

	if (page == 0x10) {
		if (op >= 0x20 && op < 0x30) {          /* long conditional branches */
			t16 = sem_fetch16(c, o);
			if (sem_cond(c, op)) { c->pc += t16; cyc += 6; } else cyc += 5;
			goto done;
		}
		if (op == 0x3f) {                        /* SWI2 */
			c->cc |= CC_E; sem_push_all(c);
			c->pc = sem_rd16(c, 0xfff4); cyc += 20; goto done;
		}
		int mode = (op >> 4) & 3, base;
		switch (mode) {
		case 0: base = 0; break;
		case 1: ea = (uint16_t)(c->dp << 8 | sem_fetch(c, o)); base = 2; break;
		case 2: base = 2; ea = sem_indexed(c, o, &cyc); break;
		default: ea = sem_fetch16(c, o); base = 3; break;
		}
		switch (op & 0xcf) {
		case 0x83: case 0x8c: {                  /* CMPD / CMPY */
			uint16_t v = mode == 0 ? sem_fetch16(c, o) : sem_rd16(c, ea);
			sem_sub16(c, (op & 0x0f) == 3 ? D_GET() : c->y, v);
			cyc += 5 + base; break;
		}
		case 0x8e: case 0xce: {                  /* LDY / LDS */
			uint16_t v = mode == 0 ? sem_fetch16(c, o) : sem_rd16(c, ea);
			if (op & 0x40) c->s = v; else c->y = v;
			sem_nz16(c, v); c->cc &= ~CC_V;
			cyc += 4 + base; break;
		}
		case 0x8f: case 0xcf: {                  /* STY / STS */
			uint16_t v = (op & 0x40) ? c->s : c->y;
			sem_wr16(c, ea, v); sem_nz16(c, v); c->cc &= ~CC_V;
			cyc += 4 + base; break;
		}
		default: c->bad_op = 0x1000 | op; cyc += 2; break;
		}
		goto done;
	}
	if (page == 0x11) {
		if (op == 0x3f) {                        /* SWI3 */
			c->cc |= CC_E; sem_push_all(c);
			c->pc = sem_rd16(c, 0xfff2); cyc += 20; goto done;
		}
		int mode = (op >> 4) & 3, base;
		switch (mode) {
		case 0: base = 0; break;
		case 1: ea = (uint16_t)(c->dp << 8 | sem_fetch(c, o)); base = 2; break;
		case 2: base = 2; ea = sem_indexed(c, o, &cyc); break;
		default: ea = sem_fetch16(c, o); base = 3; break;
		}
		if ((op & 0xcf) == 0x83 || (op & 0xcf) == 0x8c) {   /* CMPU / CMPS */
			uint16_t v = mode == 0 ? sem_fetch16(c, o) : sem_rd16(c, ea);
			sem_sub16(c, (op & 0x0f) == 3 ? c->u : c->s, v);
			cyc += 5 + base;
		} else { c->bad_op = 0x1100 | op; cyc += 2; }
		goto done;
	}

	/* ---- page 0 ---- */
	if (op < 0x10 || (op >= 0x40 && op < 0x80)) {
		/* read-modify-write: direct / A / B / indexed / extended */
		int hi = op >> 4;
		if (hi == 4 || hi == 5) {
			uint8_t *r = hi == 4 ? &c->a : &c->b;
			int w = sem_rmw(c, op, *r);
			if (w >= 0) *r = (uint8_t)w;
			cyc += 2; goto done;
		}
		if (hi == 0) { ea = (uint16_t)(c->dp << 8 | sem_fetch(c, o)); cyc += 6; }
		else if (hi == 6) { cyc += 6; ea = sem_indexed(c, o, &cyc); }
		else { ea = sem_fetch16(c, o); cyc += 7; }
		if ((op & 0x0f) == 0x0e) { c->pc = ea; cyc -= 3; goto done; }   /* JMP: 3 / 3+idx / 4 */
		int w = sem_rmw(c, op, RD(ea));                                 /* (CLR reads then writes on the 6809) */
		if (w >= 0) WR(ea, w);
		goto done;
	}

	if (op >= 0x80) {
		int mode = (op >> 4) & 3, isb = op & 0x40, base;
		switch (mode) {
		case 0: base = 0; break;
		case 1: ea = (uint16_t)(c->dp << 8 | sem_fetch(c, o)); base = 2; break;
		case 2: base = 2; ea = sem_indexed(c, o, &cyc); break;
		default: ea = sem_fetch16(c, o); base = 3; break;
		}
		uint8_t *r = isb ? &c->b : &c->a;
		int lo = op & 0x0f;
		/* 16-bit and special columns */
		if (lo == 0x3 || lo == 0xc || lo == 0xe) {
			uint16_t v = mode == 0 ? sem_fetch16(c, o) : sem_rd16(c, ea);
			if (lo == 0x3) {                                  /* SUBD / ADDD */
				D_SET(isb ? sem_add16(c, D_GET(), v) : sem_sub16(c, D_GET(), v));
				cyc += 4 + base;
			} else if (lo == 0xc) {
				if (!isb) { sem_sub16(c, c->x, v); cyc += 4 + base; }   /* CMPX */
				else { D_SET(v); sem_nz16(c, v); c->cc &= ~CC_V; cyc += 3 + base; }   /* LDD */
			} else {                                          /* LDX / LDU */
				if (isb) c->u = v; else c->x = v;
				sem_nz16(c, v); c->cc &= ~CC_V; cyc += 3 + base;
			}
			goto done;
		}
		if (lo == 0xd) {
			if (!isb) {                                       /* BSR / JSR */
				if (mode == 0) { int8_t off = (int8_t)sem_fetch(c, o); sem_pushs16(c, c->pc); c->pc += off; cyc += 7; }
				else { sem_pushs16(c, c->pc); c->pc = ea; cyc += 5 + base; }
			} else {                                          /* STD */
				if (mode == 0) { c->bad_op = op; cyc += 2; goto done; }
				sem_wr16(c, ea, D_GET()); sem_nz16(c, D_GET()); c->cc &= ~CC_V; cyc += 3 + base;
			}
			goto done;
		}
		if (lo == 0xf) {                                      /* STX / STU */
			if (mode == 0) { c->bad_op = op; cyc += 2; goto done; }
			uint16_t v = isb ? c->u : c->x;
			sem_wr16(c, ea, v); sem_nz16(c, v); c->cc &= ~CC_V; cyc += 3 + base;
			goto done;
		}
		if (lo == 0x7) {                                      /* STA / STB */
			if (mode == 0) { c->bad_op = op; cyc += 2; goto done; }
			WR(ea, *r); sem_nz8(c, *r); c->cc &= ~CC_V; cyc += 2 + base;
			goto done;
		}
		uint8_t v = mode == 0 ? sem_fetch(c, o) : RD(ea);
		cyc += 2 + base;
		switch (lo) {
		case 0x0: *r = sem_sub8(c, *r, v, 0); break;                          /* SUB */
		case 0x1: sem_sub8(c, *r, v, 0); break;                               /* CMP */
		case 0x2: *r = sem_sub8(c, *r, v, c->cc & CC_C); break;               /* SBC */
		case 0x4: *r &= v; sem_nz8(c, *r); c->cc &= ~CC_V; break;             /* AND */
		case 0x5: sem_nz8(c, *r & v); c->cc &= ~CC_V; break;                  /* BIT */
		case 0x6: *r = v; sem_nz8(c, v); c->cc &= ~CC_V; break;               /* LD */
		case 0x8: *r ^= v; sem_nz8(c, *r); c->cc &= ~CC_V; break;             /* EOR */
		case 0x9: *r = sem_add8(c, *r, v, c->cc & CC_C); break;               /* ADC */
		case 0xa: *r |= v; sem_nz8(c, *r); c->cc &= ~CC_V; break;             /* OR */
		case 0xb: *r = sem_add8(c, *r, v, 0); break;                          /* ADD */
		}
		goto done;
	}

	switch (op) {
	case 0x12: cyc += 2; break;                                   /* NOP */
	case 0x13: c->sync = 1; cyc += 4; break;                      /* SYNC */
	case 0x16: t16 = sem_fetch16(c, o); c->pc += t16; cyc += 5; break;   /* LBRA */
	case 0x17: t16 = sem_fetch16(c, o); sem_pushs16(c, c->pc); c->pc += t16; cyc += 9; break;   /* LBSR */
	case 0x19: {                                                  /* DAA */
		uint8_t a = c->a, cf = 0, msn = a & 0xf0, lsn = a & 0x0f;
		if (lsn > 9 || (c->cc & CC_H)) cf |= 0x06;
		if (msn > 0x80 && lsn > 9) cf |= 0x60;
		if (msn > 0x90 || (c->cc & CC_C)) cf |= 0x60;
		unsigned r = a + cf;
		c->cc &= ~(CC_N | CC_Z | CC_V);
		if (r & 0x100) c->cc |= CC_C;
		c->a = (uint8_t)r; sem_nz8(c, c->a);
		cyc += 2; break;
	}
	case 0x1a: c->cc |= sem_fetch(c, o); cyc += 3; break;         /* ORCC */
	case 0x1c: c->cc &= sem_fetch(c, o); cyc += 3; break;         /* ANDCC */
	case 0x1d: c->a = (c->b & 0x80) ? 0xff : 0; sem_nz16(c, D_GET()); cyc += 2; break;   /* SEX */
	case 0x1e: { t8 = sem_fetch(c, o); uint16_t a1 = sem_tfr_get(c, t8 >> 4), a2 = sem_tfr_get(c, t8 & 15);   /* EXG */
		sem_tfr_set(c, t8 >> 4, a2); sem_tfr_set(c, t8 & 15, a1); cyc += 8; break; }
	case 0x1f: t8 = sem_fetch(c, o); sem_tfr_set(c, t8 & 15, sem_tfr_get(c, t8 >> 4)); cyc += 6; break;        /* TFR */
	case 0x30: case 0x31: {                                        /* LEAX / LEAY */
		cyc += 4; ea = sem_indexed(c, o, &cyc);
		if (op & 1) c->y = ea; else c->x = ea;
		c->cc &= ~CC_Z; if (!ea) c->cc |= CC_Z; break;
	}
	case 0x32: cyc += 4; c->s = sem_indexed(c, o, &cyc); break;    /* LEAS */
	case 0x33: cyc += 4; c->u = sem_indexed(c, o, &cyc); break;    /* LEAU */
	case 0x34: case 0x36: {                                        /* PSHS / PSHU */
		uint8_t m = sem_fetch(c, o);
		uint16_t *sp = op == 0x34 ? &c->s : &c->u, other = op == 0x34 ? c->u : c->s;
		cyc += 5;
		if (m & 0x80) { WR(--*sp, c->pc); WR(--*sp, c->pc >> 8); cyc += 2; }
		if (m & 0x40) { WR(--*sp, other); WR(--*sp, other >> 8); cyc += 2; }
		if (m & 0x20) { WR(--*sp, c->y); WR(--*sp, c->y >> 8); cyc += 2; }
		if (m & 0x10) { WR(--*sp, c->x); WR(--*sp, c->x >> 8); cyc += 2; }
		if (m & 0x08) { WR(--*sp, c->dp); cyc++; }
		if (m & 0x04) { WR(--*sp, c->b); cyc++; }
		if (m & 0x02) { WR(--*sp, c->a); cyc++; }
		if (m & 0x01) { WR(--*sp, c->cc); cyc++; }
		break;
	}
	case 0x35: case 0x37: {                                        /* PULS / PULU */
		uint8_t m = sem_fetch(c, o);
		uint16_t *sp = op == 0x35 ? &c->s : &c->u;
		cyc += 5;
		if (m & 0x01) { c->cc = RD((*sp)++); cyc++; }
		if (m & 0x02) { c->a = RD((*sp)++); cyc++; }
		if (m & 0x04) { c->b = RD((*sp)++); cyc++; }
		if (m & 0x08) { c->dp = RD((*sp)++); cyc++; }
		if (m & 0x10) { c->x = sem_rd16(c, *sp); *sp += 2; cyc += 2; }
		if (m & 0x20) { c->y = sem_rd16(c, *sp); *sp += 2; cyc += 2; }
		if (m & 0x40) { uint16_t v = sem_rd16(c, *sp); *sp += 2; if (op == 0x35) c->u = v; else c->s = v; cyc += 2; }
		if (m & 0x80) { c->pc = sem_rd16(c, *sp); *sp += 2; cyc += 2; }
		break;
	}
	case 0x39: c->pc = sem_puls16(c); cyc += 5; break;            /* RTS */
	case 0x3a: c->x += c->b; cyc += 3; break;                     /* ABX */
	case 0x3b:                                                    /* RTI */
		c->cc = sem_puls8(c);
		if (c->cc & CC_E) {
			c->a = sem_puls8(c); c->b = sem_puls8(c); c->dp = sem_puls8(c);
			c->x = sem_puls16(c); c->y = sem_puls16(c); c->u = sem_puls16(c); cyc += 15;
		} else cyc += 6;
		c->pc = sem_puls16(c);
		break;
	case 0x3c:                                                    /* CWAI */
		c->cc &= sem_fetch(c, o);
		c->cc |= CC_E; sem_push_all(c);
		c->cwai = 1; cyc += 20; break;
	case 0x3d: { uint16_t r = (uint16_t)(c->a * c->b); D_SET(r);  /* MUL */
		c->cc &= ~(CC_Z | CC_C); if (!r) c->cc |= CC_Z; if (r & 0x80) c->cc |= CC_C; cyc += 11; break; }
	case 0x3f:                                                    /* SWI */
		c->cc |= CC_E; sem_push_all(c); c->cc |= CC_I | CC_F;
		c->pc = sem_rd16(c, 0xfffa); cyc += 19; break;
	default:
		if (op >= 0x20 && op < 0x30) {                            /* short branches */
			int8_t off = (int8_t)sem_fetch(c, o);
			if (sem_cond(c, op)) c->pc += off;
			cyc += 3;
			break;
		}
		c->bad_op = op; cyc += 2;
		break;
	}
done:
	c->cycles += cyc;
	return cyc;
}

#endif
