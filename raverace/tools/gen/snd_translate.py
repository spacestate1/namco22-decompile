#!/usr/bin/env python3
"""snd_translate.py COVERAGE [ROMDIR] -> gen/snd_driver.c

Translate the Rave Racer C74 sound program to C, AHEAD OF TIME. Input: the
oracle's coverage ("PCPCPC mx len" per executed instruction, rr_sndoracle
RR_SNDCOV) for the instruction boundaries and their M/X-dependent lengths; the
instruction BYTES come from the ROM files (never from a running program):
    0x00C000-0x00FFFF  c74.bin
    0x200000-0x27FFFF  rv1data.6r
    0x00BA74-...       rv1data.6r[0..]   copied to shared RAM by the BIOS   (guarded)
    0x004200-...       68K ROM 0x419E..  uploaded to shared RAM by the 68K  (guarded)
Every instruction becomes: operand bytes as a const array, and a call to
m377_exec() with the opcode as a constant -- folded by gcc to that instruction's
semantics (engine/snd/m377_sem.h, the same source the oracle executes). The loop
mirrors the oracle's m37710_run() exactly (64-cycle chunks, service, then
execute) so the translation can be gated as equal to the oracle, cycle for
cycle. A PC with no translation, or RAM code whose bytes are not the ROM's,
TRAPS LOUDLY (the sound stops and says where) -- never silently skipped.
"""
import sys, os, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from snd_rom import source
sys.path.append(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..', 'tools', 'gen'))   # the shared snd_dispatch.py (appended: our own snd_rom stays first)
from m7700_decode import decode
cov_path = sys.argv[1] if len(sys.argv) > 1 else 'tools/gen/snd.cov'

ins = collections.defaultdict(dict)                 # pc -> {mx: len}
for l in open(cov_path):
    p = l.split()
    if len(p) != 3: continue
    pc, mx, ln = int(p[0], 16), int(p[1]), int(p[2])
    b, _ = source(pc, 6)
    if b is None: raise SystemExit(f'no ROM source for executed code at {pc:06X}')
    if decode(b, mx)[2] != ln: raise SystemExit(f'decoder disagrees with the oracle at {pc:06X} mx={mx}')
    ins[pc][mx] = ln
# The BIOS is the same ROM on every C74 game: what one game executed is real code for all of them (tools/gen/c74_bios.cov, GitHub #95)
C74_BIOS_SHA1 = '8f3248b1890abb2e649927240ae46f73bb171e3b'     # the c74.bin the pool was measured on
if not os.environ.get('SND_NO_POOL'):                       # SND_NO_POOL=1: this game's own coverage only, for A/B
    import hashlib
    _h = hashlib.sha1(source(0xC000, 0x4000)[0]).hexdigest()
    if _h != C74_BIOS_SHA1:
        raise SystemExit(f'c74.bin sha1 {_h} is not the BIOS c74_bios.cov was measured on ({C74_BIOS_SHA1}); '
                         'rebuild the pool for it, or set SND_NO_POOL=1')
    for l in open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'c74_bios.cov')):
        if l.lstrip().startswith('#'): continue
        p = l.split()
        if len(p) != 3: continue
        pc, mx, ln = int(p[0], 16), int(p[1]), int(p[2])
        if decode(source(pc, 6)[0], mx)[2] != ln: raise SystemExit(f'decoder disagrees with c74_bios.cov at {pc:06X} mx={mx}')
        ins[pc][mx] = ln
# Interrupt entry points, at every M/X (an interrupt keeps the interrupted code's M/X), as the shared translator does: the BIOS's
# vectors at 0xFFD0 point at its `JMP (RAM vector)` trampolines (0xC68B..0xC6C7), which coverage only ever saw in the M/X the
# scenarios happened to interrupt (Rave Racer's sound sweep trapped at 0x00C6A9 M/X=0).
if not os.environ.get('SND_NO_VECMX'):                      # SND_NO_VECMX=1: as before, for A/B
    _vec = source(0xFFD0, 0x30)[0]
    for i in range(0, 0x30, 2):
        t = _vec[i] | _vec[i + 1] << 8
        if 0xC000 <= t < 0x10000:
            for mx in range(4):
                if mx not in ins[t]: ins[t][mx] = decode(source(t, 6)[0], mx)[2]
covered = sum(len(v) for v in ins.values())

# ---- static walk: extend coverage along control flow with known M/X ----
COND = {'BPL', 'BMI', 'BVC', 'BVS', 'BCC', 'BCS', 'BNE', 'BEQ', 'BBC', 'BBS'}
STOP = {'RTS', 'RTL', 'RTI', 'STP', 'BRK', 'COP', 'UNK', 'PLP', 'PUL', 'WDM'}
def successors(pc, mx):
    b, _ = source(pc, 6)
    mn, ea, n, pre = decode(b, mx)
    bank, pc16 = pc & 0xFF0000, pc & 0xFFFF
    nxt = bank | ((pc16 + n) & 0xFFFF)
    s8 = lambda v: v - 256 if v >= 128 else v
    if mn in STOP: return []
    if mn in ('SEM', 'CLM', 'SEP', 'CLP'):
        m2 = mx
        if mn == 'SEM': m2 = mx | 2
        elif mn == 'CLM': m2 = mx & 1
        else:
            k = (b[1] >> 4) & 3
            m2 = (mx | k) if mn == 'SEP' else (mx & ~k & 3)
        return [(nxt, m2)]
    if mn == 'BRA': return [(bank | ((pc16 + n + s8(b[n - 1])) & 0xFFFF), mx)]
    if mn == 'BRL': return [(bank | ((pc16 + n + int.from_bytes(b[n - 2:n], 'little', signed=True)) & 0xFFFF), mx)]
    if mn in COND: return [(bank | ((pc16 + n + s8(b[n - 1])) & 0xFFFF), mx), (nxt, mx)]
    if mn in ('JMP', 'JML'):
        if ea == 'A': return [(bank | int.from_bytes(b[1:3], 'little'), mx)]
        if ea == 'AL': return [(int.from_bytes(b[1:4], 'little'), mx)]
        return []                                        # indirect: needs coverage
    if mn in ('JSR', 'JSL'):
        out = [(nxt, mx)]
        tgt = None
        if ea == 'A': tgt = bank | int.from_bytes(b[1:3], 'little')
        if ea == 'AL': tgt = int.from_bytes(b[1:4], 'little')
        if tgt is not None:
            out.append((tgt, mx))
            if not os.environ.get('SND_NO_EXITS'):          # the return point also in every M/X the callee can RETURN with (tools/gen/snd_exits.py, GitHub #62)
                out += [(nxt, m) for m in sorted(_exits(tgt, mx, 'RTS' if mn == 'JSR' else 'RTL')) if m != mx]
        return out
    return [(nxt, mx)]

from snd_exits import Exits, plp_seeds, flagset_seeds
_exits = Exits(source, decode, successors)
work = [(pc, mx) for pc in ins for mx in ins[pc]]
seen = set(work)
def _walk():
    while work and len(seen) < 60000:
        pc, mx = work.pop()
        for t in successors(pc, mx):
            if t in seen: continue
            b, _ = source(t[0], 6)
            if b is None: continue                      # not ROM-backed (e.g. data RAM): no static code there
            seen.add(t); work.append(t)
            ins[t[0]][t[1]] = decode(b, t[1])[2]
# push-and-RTS dispatch tables (tools/gen/snd_dispatch.py, GitHub #62): their handlers, then walk on from them, until nothing new
# and the code pointers kept in RAM (tools/gen/snd_indirect.py, GitHub #62): the targets of the indirect jumps that read them
from snd_dispatch import dispatch_seeds
from snd_indirect import indirect_seeds
_walk()
while not os.environ.get('SND_NO_DISPATCH'):                # SND_NO_DISPATCH=1: without the tables, for A/B
    _cand = set(dispatch_seeds(ins, source, decode))
    if not os.environ.get('SND_NO_INDIRECT'):               # SND_NO_INDIRECT=1: without the RAM pointers, for A/B
        _cand |= indirect_seeds(ins, source, decode, _exits)
    if not os.environ.get('SND_NO_PLP'):                    # SND_NO_PLP=1: without the flags a PLP restores, for A/B (GitHub #95)
        _cand |= plp_seeds(ins, source, decode)
    _cand |= flagset_seeds(ins, source, decode)
    _new = [t for t in _cand if t not in seen and source(t[0], 6)[0] is not None]
    if not _new: break
    for t in _new:
        seen.add(t); work.append(t); ins[t[0]][t[1]] = decode(source(t[0], 6)[0], t[1])[2]
    _walk()
static = sum(len(v) for v in ins.values()) - covered


# POLLING LOOPS (tools/gen/snd_spin.inc, the shared template): `LDA dp ; BEQ/BNE self` -- the driver's wait-for-interrupt loop
spin = []
for _pc in sorted(ins):
    for _mx in sorted(ins[_pc]):
        _b, _ = source(_pc, 6)
        if _b is None: continue
        _mn, _ea, _n, _pre = decode(_b, _mx)
        if _mn != 'LDA' or _ea != 'D' or _n != 2: continue
        _nb, _ = source(_pc + 2, 6)
        if _nb is None or _mx not in ins.get(_pc + 2, {}): continue
        _mn2, _ea2, _n2, _ = decode(_nb, _mx)
        if _mn2 in ('BEQ', 'BNE') and _n2 == 2 and _nb[1] == 0xFC: spin.append((_pc, _mx, _b[1]))

out = []
w = out.append
w('/* GENERATED by tools/gen/snd_translate.py from the ROM files and the oracle\'s')
w(' * coverage -- do not edit. ROM-derived: gen/ is not committed. */')
w('/* m377_exec is NOT forced inline: forcing it into the one huge step() made gcc need\n * 8.3 GB of RAM (-O1) to compile this file -- a "hang" on an 8 GB machine -- for no\n * measurable speed (same CPU time, byte-identical audio). noinline: 318 MB, 8 s. */')
w('#define M377_EXEC_ATTR __attribute__((noinline))')
w('#include <stdio.h>\n#include "m377_sem.h"\n#include "snd_board.h"\n')
w('static void trap(m37710_t *c, uint32_t pc, const char *why)')
w('{')
w('    fprintf(stderr, "[SND] TRANSLATED DRIVER: %s at %06X (M/X=%u) -- sound stopped\\n", why, pc, (c->ps >> 4) & 3u);')
w('    c->unimpl_hit = true; c->unimpl_pc = pc; c->unimpl_op = 0xFFFF;')
w('}')
w('static int ram_ok(m37710_t *c, uint32_t a, const uint8_t *b, int n)   /* RAM code must be what we translated */')
w('{')
w('    for (int i = 0; i < n; i++) if (c->read8(c->user, a + (uint32_t)i) != b[i]) return 0;')
w('    return 1;')
w('}')
w('')
nram = 0
w('static void run_chunk(m37710_t *c, uint64_t until)')
w('{')
w('    while (c->cycles < until && !c->unimpl_hit) {')
w('        m37710_service(c);')
w('        if (c->stopped) { c->cycles = until; break; }')
w('        const uint32_t pc = ((uint32_t)c->pg << 16) | c->pc;')
w('        const unsigned mx = (c->ps >> 4) & 3u;')
w('        switch (pc) {')
for pc in sorted(ins):
    w(f'        case 0x{pc:06X}:')
    for mx in sorted(ins[pc]):
        n = ins[pc][mx]
        b, ram = source(pc, n)
        b = b[:n]
        op = b[0]; pre = False
        if op == 0x42: pre = True; op = b[1]
        arr = ','.join(f'0x{x:02X}' for x in b)
        guard = f'if (!ram_ok(c, pc, o, {n})) {{ trap(c, pc, "RAM code differs from ROM"); break; }} ' if ram else ''
        nram += ram
        body = (f'{{ static const uint8_t o[] = {{{arr}}}; {guard}c->ops = o; (void)fetch8(c); '
                + ('c->use_b = true; (void)fetch8(c); ' if pre else 'c->use_b = false; ')
                + f'c->cycles += 1; m377_exec(c, 0x{op:02X}); c->ops = NULL; break; }}')
        w(f'            if (mx == {mx}) {body}')
    w(f'            trap(c, pc, "no translation for this M/X"); break;')
w('        default: trap(c, pc, "no translation for this address"); break;')
w('        }')
w('    }')
w('}')
w('')
_inc = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..', 'tools', 'gen', 'snd_spin.inc')
w(open(_inc).read().replace('@SITES@', ', '.join('{0x%06X, %d, 0x%02X}' % t for t in spin) if spin else '{0, 9, 0}'))
w('')
w('void snd_executor_init(void) { fprintf(stderr, "[SND] translated sound program (%d instructions)\\n", ' + str(sum(len(v) for v in ins.values())) + '); }')
w('')
w('/* Same chunking as the oracle (snd_execute -> m37710_run in 64-cycle pieces). */')
w('void snd_execute(m37710_t *c, uint64_t end)')
w('{')
w('    snd_board_port4();')
w('    while (c->cycles < end && !c->unimpl_hit) {')
w('        snd_spin_fast(c, end);                       /* a polling loop is jumped, exactly (tools/gen/snd_spin.inc) */')
w('        if (c->cycles >= end) break;')
w('        uint64_t rem = end - c->cycles;')
w('        run_chunk(c, c->cycles + (rem < 64 ? rem : 64));')
w('    }')
w('}')
os.makedirs('gen', exist_ok=True)
open('gen/snd_driver.c', 'w').write('\n'.join(out) + '\n')
print(f'translated {sum(len(v) for v in ins.values())} instructions at {len(ins)} addresses: {covered} executed by the oracle '
      f'+ {static} reached statically; {nram} from RAM (guarded) -> gen/snd_driver.c')
