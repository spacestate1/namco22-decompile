#!/usr/bin/env python3
"""m6809_translate.py --game cs --roms DIR --cov FILE [--cov FILE...] --out FILE [--prefix NAME]
                     [--drop ADDR] [--check-trace TRACE[.gz] ...] [--list FILE]

Translate an MC6809 program to C, AHEAD OF TIME -- the shared tool for every board with a 6809 (HARD RULE 1 and 4 in
CLAUDE.md; first user: Cyber Sled's System 21 sound CPU, cybsled/). Same design as tools/gen/snd_translate.py
(M37710) and tools/gen/c25_translate.py (TMS320C25):

  - Instruction BOUNDARIES come from the oracle's COVERAGE (addresses the DEV interpreter executed; no ROM bytes in
    the file: "PCPC" for the fixed ROM window, "PCPC B" for the banked window with its bank number), extended by a
    STATIC WALK along control flow from those addresses and from the CPU's vectors (reset, NMI, SWI, IRQ, FIRQ,
    SWI2, SWI3 -- the ones that point into ROM).
  - Instruction BYTES come from the ROM file, never from a running program.
  - Each instruction becomes a `case` holding its bytes as a `static const uint8_t` array and a call of
    m6809_exec(c, o) -- the semantics in <game>/src/snd/m6809_sem.h, the SAME source the oracle runs -- which gcc
    folds to that one instruction. So the translation can be gated as EQUAL to the oracle.
  - Code in the BANKED window is keyed on (bank, pc): c->bank is set by the board when the program writes the bank
    register. The fixed window is keyed on pc alone.
  - A PC with no translation TRAPS LOUDLY (c->trapped / c->trap_pc set, the CPU stops, stderr says where): never a
    fall back to an interpreter, never skipped.
  - Polling loops: the heads of tight backward-branch loops are listed (<prefix>_spin[]) for the board's EXACT
    fast-forward (it probes one pass and skips only provably identical passes; cybsled/src/snd/s21_snd.c).

The functions emitted: int <prefix>_step(m6809_t *c) -- one step exactly like the oracle's m6809_step (interrupt
sampling and the DEV hooks through m6809_enter, then the instruction); int <prefix>_exec(m6809_t *c) -- the
instruction at c->pc only. Instructions are grouped one C function per 256-byte page so gcc's memory stays small.

--check-trace: check this tool's instruction lengths against MAME's own disassembly traces (consecutive executed
addresses of non-branching instructions), so a length error cannot make a translation read past its bytes.
--drop ADDR: DEV negative control -- leave that address untranslated (the gate must then report the trap).
"""
import argparse, gzip, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from m6809_decode import decode, disasm

ap = argparse.ArgumentParser()
ap.add_argument('--game', required=True, choices=['cs'])
ap.add_argument('--roms', required=True)
ap.add_argument('--cov', action='append', default=[])
ap.add_argument('--out')
ap.add_argument('--prefix', default=None)
ap.add_argument('--drop', action='append', default=[])
ap.add_argument('--check-trace', action='append', default=[])
ap.add_argument('--list', help='DEV: write a disassembly listing of every translated instruction')
a = ap.parse_args()

# ---- the memory map of the program -------------------------------------------------------------------------------
if a.game == 'cs':
    # Cyber Sled (Namco System 21) sound CPU, MAME namcos21_c67 sound map: cy1-snd0.8j (128 KB); ROM offset 0 fixed at
    # 0xC000-0xFFFF; 0x0000-0x3FFF a window onto 8 banks of 16 KB, bank = (write to 0xC001) >> 4, taken modulo 8.
    rom = open(os.path.join(a.roms, 'cy1-snd0.8j'), 'rb').read()
    FIXED_LO, FIXED_HI, FIXED_OFF = 0xC000, 0x10000, 0
    BANK_LO, BANK_HI, BANK_SIZE, NBANK = 0x0000, 0x4000, 0x4000, 8
    prefix = a.prefix or 'cs_6809'
    # the two bytecode interpreters' opcode tables (music 0xD44E, sequences 0xDE5E: `LDX #table ; ABX ; LDD 2,X ; STD
    # <handler ; ... JSR [handler]`): 4-byte entries, the handler address at +2, the table ending where the first
    # handler begins (0x23 opcodes each). A static walk cannot see through the RAM pointer: these are its seeds.
    TABLES = [(0xD4F5, 4, 2), (0xDEFE, 4, 2)]


def src(pc, bank, n=6):
    """the ROM bytes at pc (bank for the banked window), or None when pc is not ROM-backed"""
    if FIXED_LO <= pc < FIXED_HI:
        o = FIXED_OFF + pc - FIXED_LO
        if pc + n > FIXED_HI: n = FIXED_HI - pc
    elif BANK_LO <= pc < BANK_HI and bank is not None:
        o = (bank % NBANK) * BANK_SIZE + pc - BANK_LO
        if pc + n > BANK_HI: n = BANK_HI - pc
    else:
        return None
    b = rom[o:o + n]
    return b + bytes(6 - len(b))


def key(pc, bank):
    return (pc, bank if BANK_LO <= pc < BANK_HI else None)


# ---- length check against MAME's disassembly -------------------------------------------------------------------
if a.check_trace:
    ok = bad = 0
    BRANCHY = {'JMP', 'JSR', 'BSR', 'LBSR', 'LBRA', 'RTS', 'RTI', 'SWI', 'SWI2', 'SWI3', 'CWAI', 'SYNC', 'PULS', 'PULU', 'TFR', 'EXG'}
    for t in a.check_trace:
        f = gzip.open(t, 'rt', errors='replace') if t.endswith('.gz') else open(t, errors='replace')
        prev = None
        seen = set()
        for l in f:
            if 'interrupted' in l: prev = None; continue
            if len(l) < 6 or l[4] != ':': continue
            pc = int(l[:4], 16)
            if prev is not None and prev not in seen:
                seen.add(prev)
                b = src(prev, None)
                if b is not None:
                    mn, mode, n, flow = decode(b, prev)
                    mm = l_prev.split()[1] if len(l_prev.split()) > 1 else ''
                    if flow == 'next' and mm not in BRANCHY and not mm.startswith('B') and not mm.startswith('LB'):
                        if pc == (prev + n) & 0xFFFF: ok += 1
                        else:
                            bad += 1
                            print(f'length mismatch at {prev:04X}: ours {n} ({mn}), MAME next {pc:04X}: {l_prev.strip()}')
            prev, l_prev = pc, l
        f.close()
    print(f'check-trace: {ok} instruction lengths agree with MAME\'s disassembly, {bad} disagree')
    if bad: sys.exit(1)
    if not a.out: sys.exit(0)

# ---- coverage ----------------------------------------------------------------------------------------------------
ins = {}                                   # (pc, bank|None) -> length
covered = set()
for path in a.cov:
    for l in open(path):
        p = l.split()
        if not p or p[0].startswith('#'): continue
        pc = int(p[0], 16)
        bank = int(p[1]) if len(p) > 1 else None
        k = key(pc, bank)
        b = src(*k)
        if b is None:
            raise SystemExit(f'{path}: executed code at {pc:04X} is not in ROM (RAM / device code cannot be translated)')
        covered.add(k)
        ins[k] = decode(b, pc)[2]

# ---- static walk -------------------------------------------------------------------------------------------------
STOP_MN = {'ILL', 'SWI', 'SWI2', 'SWI3'}
work = list(ins)
vec = {0xFFF2: 'SWI3', 0xFFF4: 'SWI2', 0xFFF6: 'FIRQ', 0xFFF8: 'IRQ', 0xFFFA: 'SWI', 0xFFFC: 'NMI', 0xFFFE: 'RESET'}
for v in vec:
    b = src(v, None, 2)
    t = b[0] << 8 | b[1]
    if FIXED_LO <= t < FIXED_HI and (t, None) not in ins:
        work.append((t, None)); ins[(t, None)] = decode(src(t, None), t)[2]
ntab = 0
for base, stride, off in TABLES:
    targets, i = [], 0
    while True:
        e = base + stride * i
        if targets and e >= min(targets): break
        b = src(e + off, None, 2)
        t = b[0] << 8 | b[1]
        if not (FIXED_LO <= t < FIXED_HI): break
        targets.append(t); i += 1
    for t in set(targets):
        if (t, None) not in ins:
            work.append((t, None)); ins[(t, None)] = decode(src(t, None), t)[2]
    ntab += i
seen = set(work)
while work:
    pc, bank = work.pop()
    b = src(pc, bank)
    mn, mode, n, flow = decode(b, pc)
    succ = []
    nxt = (pc + n) & 0xFFFF
    if flow in ('next', 'call?'): succ = [nxt]
    elif flow == 'stop': succ = []
    elif flow[0] == 'jmp': succ = [flow[1]]
    else: succ = [flow[1], nxt]                 # br / call
    if mn in STOP_MN: succ = []
    for t in succ:
        if BANK_LO <= t < BANK_HI and bank is None: continue    # into the banked window from fixed code: the bank is not static
        k = key(t, bank)
        if k in seen: continue
        bt = src(*k)
        if bt is None: continue
        seen.add(k); work.append(k)
        ins[k] = decode(bt, t)[2]
for d in a.drop:
    for k in [k for k in ins if k[0] == int(d, 16)]:
        del ins[k]
        print(f'DEV negative control: {k[0]:04X} left untranslated')
static = len(ins) - len(covered & set(ins))

# ---- polling-loop heads: a short backward branch whose loop body is straight-line code ----------------------------
spin = []
for (pc, bank) in sorted(ins, key=lambda k: (k[1] if k[1] is not None else -1, k[0])):
    if bank is not None: continue
    b = src(pc, bank)
    mn, mode, n, flow = decode(b, pc)
    if not (isinstance(flow, tuple) and flow[0] in ('br', 'jmp') and b[0] in range(0x20, 0x30)): continue
    t = flow[1]
    if not (t <= pc and pc - t <= 12): continue
    q, good = t, True
    while q < pc:
        if (q, None) not in ins: good = False; break
        mq, _, nq, fq = decode(src(q, None), q)
        if fq != 'next' or mq in ('SYNC', 'CWAI', 'PSHS', 'PULS', 'PSHU', 'PULU'): good = False; break
        q += nq
    if good and q == pc: spin.append(t)
spin = sorted(set(spin))

if not a.out: sys.exit(0)

# ---- emit --------------------------------------------------------------------------------------------------------
groups = {}                                # (bank|None, page) -> [(pc, bytes)]
for (pc, bank), n in ins.items():
    groups.setdefault((bank, pc >> 8), []).append((pc, src(pc, bank, n)[:n]))
out = []
w = out.append
w(f'/* GENERATED by tools/gen/m6809_translate.py --game {a.game} from the ROM and the oracle\'s coverage -- do not edit.')
w(' * ROM-derived: gen/ is not committed. One case per instruction: its bytes as a constant, the semantics of m6809_sem.h')
w(' * (the oracle\'s own source) folded to that instruction. Untranslated code TRAPS. */')
w('#include <stdio.h>')
w('#include "m6809_sem.h"')
w('')
w(f'static int {prefix}_trap(m6809_t *c, const char *why)')
w('{')
w(f'    fprintf(stderr, "[6809] TRANSLATED 6809: %s at %04X (bank %d) -- the CPU stopped\\n", why, c->pc, c->bank);')
w('    c->trapped = 1; c->trap_pc = c->pc;')
w('    return 0;')
w('}')
# one handler per OPCODE (a prefix 0x10 / 0x11 and its second byte count as one opcode): m6809_exec folded to that opcode
# inside it, the operand bytes passed in as the case's constants. One function per page with m6809_exec inlined at every
# address needed ~3.1 GB of RAM at -O2 for this 64 KB file; this keeps the compile small (the limited-RAM rule). The
# opcode, its dispatch and its bus accesses are still fixed at build time; the operands are constants of the case.
def opkey(b): return tuple(b[:2]) if b[0] in (0x10, 0x11) else (b[0],)
keys = sorted(set(opkey(b) for g in groups.values() for _, b in g))
for k in keys:
    nm = 'op' + ''.join(f'{x:02X}' for x in k)
    fixed = ', '.join([f'0x{x:02X}' for x in k] + [f'o[{i}]' for i in range(len(k), 5)])
    w(f'static __attribute__((noinline)) int {nm}(m6809_t *c, const uint8_t *o) {{ const uint8_t b[5] = {{ {fixed} }}; return m6809_exec(c, b); }}')
w('')
fn = {}
for (bank, page) in sorted(groups, key=lambda g: (g[0] if g[0] is not None else -1, g[1])):
    name = f'p_{page:02X}' if bank is None else f'b{bank}_{page:02X}'
    fn[(bank, page)] = name
    w(f'static int {name}(m6809_t *c)')
    w('{')
    w('    switch (c->pc) {')
    for pc, b in sorted(groups[(bank, page)]):
        bb = list(b) + [0] * (5 - len(b))
        w(f'    case 0x{pc:04X}: {{ static const uint8_t o[5] = {{ {",".join(f"0x{x:02X}" for x in bb)} }}; return op{"".join(f"{x:02X}" for x in opkey(b))}(c, o); }}')
    w(f'    default: return {prefix}_trap(c, "no translation for this address");')
    w('    }')
    w('}')
w('')
w(f'/* the instruction at c->pc (no interrupt sampling) */')
w(f'int {prefix}_exec(m6809_t *c)')
w('{')
w(f'    if (c->pc >= 0x{BANK_LO:04X} && c->pc < 0x{BANK_HI:04X}) {{')
w(f'        switch (((c->bank % {NBANK}) << 8) | (c->pc >> 8)) {{')
for (bank, page), name in sorted(((k, v) for k, v in fn.items() if k[0] is not None)):
    w(f'        case 0x{bank << 8 | page:03X}: return {name}(c);')
w(f'        default: return {prefix}_trap(c, "no translation for this address in this bank");')
w('        }')
w('    }')
w('    switch (c->pc >> 8) {')
for (bank, page), name in sorted(((k, v) for k, v in fn.items() if k[0] is None)):
    w(f'    case 0x{page:02X}: return {name}(c);')
w(f'    default: return {prefix}_trap(c, "no translation for this address");')
w('    }')
w('}')
w('')
w(f'/* one step, exactly as the oracle\'s m6809_step: interrupt sampling and the DEV hooks (m6809_enter), then the')
w(f'   instruction. A trapped CPU does nothing (0 cycles): the board stops running it. */')
w(f'int {prefix}_step(m6809_t *c)')
w('{')
w('    if (c->trapped) return 0;')
w('    int cyc = m6809_enter(c);')
w('    if (cyc >= 0) return cyc;')
w(f'    return {prefix}_exec(c);')
w('}')
w('')
w(f'/* heads of tight polling loops, for the board\'s exact fast-forward (fixed window only) */')
w(f'const uint16_t {prefix}_spin[] = {{ ' + (', '.join(f'0x{x:04X}' for x in spin) if spin else '0') + ' };')
w(f'const int {prefix}_nspin = {len(spin)};')
w(f'const int {prefix}_ninsn = {len(ins)};')
os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
open(a.out, 'w').write('\n'.join(out) + '\n')
if a.list:
    with open(a.list, 'w') as f:
        for (pc, bank) in sorted(ins, key=lambda k: (k[1] if k[1] is not None else -1, k[0])):
            f.write(f'{"--" if bank is None else bank:>2} {pc:04X}{"*" if (pc, bank) in covered else " "} {disasm(src(pc, bank), pc)}\n')
nb = sum(1 for k in ins if k[1] is not None)
print(f'{prefix}: translated {len(ins)} instructions ({len(covered & set(ins))} executed by the oracle + {static} reached '
      f'statically, {ntab} dispatch-table entries followed; {nb} in the banked window), {len(spin)} polling-loop heads -> {a.out}')
