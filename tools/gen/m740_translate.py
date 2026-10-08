#!/usr/bin/env python3
"""m740_translate.py -- translate a Mitsubishi 740-family program (M3745x, M5074x, M3800x ...: a 6502 with the 740
additions) to C AHEAD OF TIME. Shared, game-independent (HARD RULE 4); first user: Cyber Sled's C68 I/O MCU (an M37450
running c68.bin), cybsled/src/io/.

  m740_translate.py --rom extracted/c68.bin [--base 0x8000] [--cov tools/gen/cs_c68.cov] [--table ADDR:COUNT ...]
                    [--vectors 0xFFE0] --out gen/cs_c68.c --prefix cs_c68 [--list FILE]

Input: the program image (mapped at --base .. 0xFFFF), the test oracle's COVERAGE ("PPPP LEN" per executed instruction,
addresses + lengths only, no ROM data: the oracle's S21_C68_COV) and a STATIC WALK from the reset / interrupt vectors
(--vectors .. 0xFFFF), JSR $FFxx (the special page), and the code-pointer tables of `JMP (zp)` dispatches: decoded
automatically for the idiom `AND #m ; ASL A ; TAX ; LDA t,X ; STA z ; LDA t+1,X ; STA z+1 ; JMP (z)` (m + 1 entries),
or named by hand with --table. The instruction BYTES come from the ROM file, never from a running program.

Output: one `case` per instruction address; its bytes (and the ones after it: the 6502-pattern dummy read past a
one-byte instruction reads the next byte) as a `static const uint8_t` array, passed to that opcode's handler -- m740_exec()
of m740_sem.h, the same source the DEV oracle executes, folded by the compiler to that one opcode (one handler per opcode:
folding every address separately needs GBs of compiler RAM for nothing measurable). An address with no translation
TRAPS LOUDLY: the CPU stops and says where -- never falls back to an interpreter.
  <prefix>_reset(m740_t *)   power-on / reset (the opcode fetch is the next case's constant)
  <prefix>_step(m740_t *)    one instruction or interrupt entry, exactly as the oracle's m740_step(); returns cycles
"""
import argparse, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from m740_decode import TABLE, decode, branch_target, fmt, COND

ap = argparse.ArgumentParser()
ap.add_argument('--rom', required=True)
ap.add_argument('--base', default='0x8000')
ap.add_argument('--cov')
ap.add_argument('--table', action='append', default=[], help='ADDR:COUNT -- a table of COUNT little-endian code pointers')
ap.add_argument('--vectors', default='0xFFE0', help='lowest interrupt vector (the table runs to 0xFFFF)')
ap.add_argument('--out', required=True)
ap.add_argument('--prefix', required=True)
ap.add_argument('--list', help='also write a listing of every translated instruction')
ap.add_argument('--drop', action='append', default=[], help='DEV negative control: leave this address untranslated')
a = ap.parse_args()

rom = open(a.rom, 'rb').read()
base = int(a.base, 0)
if base + len(rom) != 0x10000: raise SystemExit(f'{a.rom}: {len(rom):#x} bytes at {base:#06x} does not end at 0xFFFF')
def inrom(pc): return base <= pc <= 0xFFFF
def rb(pc, n=4): return bytes(rom[pc - base + i] if pc + i <= 0xFFFF else 0 for i in range(n))
def w16(pc): return rom[pc - base] | rom[pc - base + 1] << 8

ins = {}                                 # pc -> length
src = {}                                 # pc -> 'cov' | 'static'

# ---- coverage ----
ncov = 0
if a.cov and os.path.exists(a.cov):
    for l in open(a.cov):
        p = l.split()
        if len(p) < 2 or p[0].startswith('#'): continue
        pc, ln = int(p[0], 16), int(p[1])
        if not inrom(pc): raise SystemExit(f'coverage: executed code at {pc:04X} is outside the program image (RAM code?)')
        mn, mode, n = decode(rb(pc))
        if n != ln: raise SystemExit(f'coverage: the decoder disagrees with the oracle at {pc:04X} ({mn} {n} vs {ln})')
        if mn == 'KIL': raise SystemExit(f'coverage: the oracle executed an undefined opcode at {pc:04X}')
        ins[pc] = ln; src[pc] = 'cov'; ncov += 1

# ---- static walk ----
STOP = {'RTS', 'RTI', 'BRK', 'STP', 'KIL'}
tables = {}                              # address of the JMP (z) -> [targets]
for t in a.table:
    ta, cnt = t.split(':')
    ta, cnt = int(ta, 0), int(cnt, 0)
    tables[('hand', ta)] = [w16(ta + 2 * i) for i in range(cnt)]

def auto_table(pc):
    """JMP (z) at pc: the m+1 targets of `AND #m ; ASL A ; TAX ; LDA t,X ; STA z ; LDA t+1,X ; STA z+1 ; JMP (z)`"""
    z = rom[pc - base + 1]
    seq = [(0x29, 2), (0x0A, 1), (0xAA, 1), (0xBD, 3), (0x85, 2), (0xBD, 3), (0x85, 2)]
    start = pc - sum(n for _, n in seq)
    if start < base: return None
    q = start; f = []
    for op, n in seq:
        if rom[q - base] != op: return None
        f.append(rb(q)); q += n
    t = f[3][1] | f[3][2] << 8
    if f[4][1] != z or f[6][1] != (z + 1) & 0xFF or (f[5][1] | f[5][2] << 8) != t + 1: return None
    cnt = f[0][1] + 1
    if cnt & (cnt - 1): return None        # AND with a non-mask: not a bound
    return [w16(t + 2 * i) for i in range(cnt)]

unresolved = []
def successors(pc):
    b = rb(pc); mn, mode, n = decode(b); nxt = (pc + n) & 0xFFFF
    if mn in STOP: return []
    if mn == 'BRA': return [branch_target(pc, b)]
    if mn in COND: return [branch_target(pc, b), nxt]
    if mn == 'JMP':
        if mode == 'abs': return [b[1] | b[2] << 8]
        tg = auto_table(pc) if mode == 'zind' else None
        if tg is not None: tables[('auto', pc)] = tg; return tg
        unresolved.append(pc); return []
    if mn == 'JSR':
        if mode == 'abs': return [b[1] | b[2] << 8, nxt]
        if mode == 'spec': return [0xFF00 | b[1], nxt]
        unresolved.append(pc); return [nxt]
    if mn in ('STP', 'WIT'): return [nxt]
    return [nxt]

vlo = int(a.vectors, 0)
seeds = [w16(v) for v in range(vlo, 0x10000, 2)]
for tg in tables.values(): seeds += tg
work = [s for s in seeds if inrom(s)] + list(ins)
seen = set()
while work:
    pc = work.pop()
    if pc in seen: continue
    seen.add(pc)
    if not inrom(pc): continue
    mn, mode, n = decode(rb(pc))
    if mn == 'KIL': continue               # data, not code: no case (the oracle would stop there too)
    if pc not in ins: ins[pc] = n; src[pc] = 'static'
    for s in successors(pc):
        if s not in seen: work.append(s)
    for tg in tables.values():
        for s in tg:
            if s not in seen: work.append(s)
for d in a.drop: ins.pop(int(d, 16), None); src.pop(int(d, 16), None)
ncov = sum(1 for p in src.values() if p == 'cov')
nstat = sum(1 for p in src.values() if p == 'static')

# overlapping instructions are allowed (each address is its own case), but say so
over = [pc for pc in ins for k in range(1, ins[pc]) if pc + k in ins]

out = []
w = out.append
P = a.prefix
w(f'/* GENERATED by tools/gen/m740_translate.py from {os.path.basename(a.rom)} (the instruction bytes) and the oracle\'s')
w(' * coverage (addresses only) -- do not edit. ROM-derived: never committed.')
w(f' * {len(ins)} instructions: {ncov} executed by the oracle, {nstat} reached statically. */')
w('#include <stdio.h>')
w('#include "m740_sem.h"')
w('')
w(f'void {P}_reset(m740_t *c);')
w(f'int {P}_step(m740_t *c);')
w('')
w('static void trap(m740_t *c, const char *why)')
w('{')
w(f'    if (!c->trapped) fprintf(stderr, "[S21] the C68 I/O MCU stopped at %04X: TRANSLATED M740 ({P}): %s -- the controls freeze\\n", c->npc, why);')
w('    c->trapped = 1; c->trap_pc = c->npc;')
w('}')
w('')
w(f'void {P}_reset(m740_t *c) {{ c->trapped = 0; m740_sem_reset(c, 1); }}')
w('')
# one handler per OPCODE (m740_exec folded to that opcode inside it); each address's case passes its constant bytes.
# One function holding every address with m740_exec inlined needed ~6 GB of RAM at -O2, one per address ~2.5 GB;
# this ~0.4 GB (the limited-RAM rule). The opcode, the dispatch and the bus-access sequence are fixed at build time;
# the operand bytes are constants of the case.
lst = []
for op in sorted(set(rb(pc)[0] for pc in ins)):
    w(f'static __attribute__((noinline)) void op{op:02X}(m740_t *c, const uint8_t *o) '
      f'{{ const uint8_t b[4] = {{ 0x{op:02X}, o[1], o[2], o[3] }}; m740_exec(c, b); }}   /* {TABLE[op][0]} {TABLE[op][1]} */')
for pc in sorted(ins):
    lst.append(f'{pc:04X} {ins[pc]} {src[pc]:6s} {fmt(pc, rb(pc))}')
w('')
w(f'int {P}_step(m740_t *c)')
w('{')
w('    const uint64_t c0 = c->cycles;')
w('    if (c->trapped) { c->cycles++; return 1; }')
w('    int r = m740_enter(c, 1);')
w('    if (r >= 0) return r;')
w('    if (c->irq_taken) { static const uint8_t irq[4] = { 0x00 }; m740_exec(c, irq); return (int)(c->cycles - c0); }   /* interrupt entry */')
w('    switch (c->npc) {')
for pc in sorted(ins):
    bb = rb(pc)
    arr = ','.join(f'0x{x:02X}' for x in bb)
    w(f'    case 0x{pc:04X}: {{ static const uint8_t o[4] = {{{arr}}}; op{bb[0]:02X}(c, o); break; }}   /* {fmt(pc, bb).replace("*/", "* /")} */')
w('    default: trap(c, "no translation for this address"); break;')
w('    }')
w('    return (int)(c->cycles - c0);')
w('}')
os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
open(a.out, 'w').write('\n'.join(out) + '\n')
if a.list: open(a.list, 'w').write('\n'.join(lst) + '\n')
for k, tg in tables.items():
    print(f'  dispatch table ({k[0]}) {k[1]:04X}: {len(tg)} targets ' + ' '.join(f'{x:04X}' for x in tg))
for pc in unresolved: print(f'  WARNING: indirect jump at {pc:04X} ({fmt(pc, rb(pc))}) not decoded: its targets come from coverage only')
if over: print(f'  note: {len(over)} instructions overlap another (code reached at two alignments)')
print(f'{P}: translated {len(ins)} instructions: {ncov} executed by the oracle + {nstat} reached statically -> {a.out}')
