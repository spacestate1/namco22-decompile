#!/usr/bin/env python3
"""snd_translate.py [COVERAGE] -> gen/snd_driver.c

Translate Prop Cycle's sound program (pr1data.8k, run by the Super System 22's
M37710) to C, AHEAD OF TIME. Input: the test oracle's coverage ("PCPCPC mx len"
per executed instruction; propcycl_sndoracle PROPCYCL_SNDCOV) for instruction
boundaries and their M/X-dependent lengths, extended by a static walk along
control flow with known M/X; the instruction BYTES come from the ROM file
(tools/gen/snd_rom.py), never from a running program. Each instruction becomes
its operand bytes as a const array plus m377_exec() with a constant opcode --
folded by gcc to that instruction's semantics (src/snd/m377_sem.h, the same
source the oracle executes). The loop mirrors m37710_run() exactly, so the
translation can be gated as EQUAL to the oracle. A PC with no translation TRAPS
LOUDLY (the sound stops and says where) -- never silently skipped.
"""
import sys, os, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from snd_rom import source
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
        if ea == 'A': out.append((bank | int.from_bytes(b[1:3], 'little'), mx))
        if ea == 'AL': out.append((int.from_bytes(b[1:4], 'little'), mx))
        return out
    return [(nxt, mx)]

# Interrupt entry points, at every M/X (an interrupt keeps the interrupted code's
# M/X), so a handler's first instruction is translated even if coverage missed it.
vec, _ = source(0xFFD0, 0x30)
for i in range(0, 0x30, 2):
    t = vec[i] | vec[i + 1] << 8
    if 0xC000 <= t < 0x10000:
        for mx in range(4):
            if mx not in ins[t]: ins[t][mx] = decode(source(t, 6)[0], mx)[2]
work = [(pc, mx) for pc in ins for mx in ins[pc]]
seen = set(work)
while work and len(seen) < 60000:
    pc, mx = work.pop()
    for t in successors(pc, mx):
        if t in seen: continue
        b, _ = source(t[0], 6)
        if b is None: continue                          # not ROM-backed (e.g. data RAM): no static code there
        seen.add(t); work.append(t)
        ins[t[0]][t[1]] = decode(b, t[1])[2]
static = sum(len(v) for v in ins.values()) - covered

# READABLE ROUTINES (src/snd/snd_engine.c): an `SND_ENTRY(0xADDR, name, MAXCYC)`
# marker calls the readable C function at ADDR, which runs the whole routine and
# returns as the original does -- but only when no interrupt, pin or frame end
# can fall within MAXCYC cycles (snd_atomic_ok). Otherwise the original
# instructions run, so an interrupt lands between the same two instructions it
# would have: the translation stays exactly equal to the oracle. The function
# returns 1 when it ran the routine; 0 (having touched nothing) sends the
# original instructions to run instead -- how a path no scenario exercises, and
# which therefore cannot be gated, stays translated rather than guessed at.
import re as _re
overrides = {}
_eng = 'src/snd/snd_engine.c'
if os.path.exists(_eng):
    for m in _re.finditer(r'SND_ENTRY\(\s*0x([0-9A-Fa-f]+)\s*,\s*(\w+)\s*,\s*(\d+)\s*\)', open(_eng).read()):
        overrides[int(m.group(1), 16)] = (m.group(2), int(m.group(3)))
for a in overrides:
    if a not in ins: raise SystemExit(f'SND_ENTRY at {a:06X}: no instruction there')

out = []
w = out.append
w('/* GENERATED by tools/gen/snd_translate.py from the ROM files and the oracle\'s')
w(' * coverage -- do not edit. ROM-derived: gen/ is not committed. */')
w('/* m377_exec is NOT forced inline: forcing it into the one huge step() made gcc need\n * 8.3 GB of RAM (-O1) to compile this file -- a "hang" on an 8 GB machine -- for no\n * measurable speed (same CPU time, byte-identical audio). noinline: 318 MB, 8 s. */')
w('#define M377_EXEC_ATTR __attribute__((noinline))')
w('#include <stdio.h>\n#include "m37710.h"\n#include "m377_sem.h"\n#include "snd_engine.h"\n')
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
w('/* PROPCYCL_SNDXPROF=<file>: at exit, how many times each address of the')
w(' * TRANSLATED program ran (readable routines excluded) -- what is left to rewrite. */')
w('#include <stdlib.h>')
w('static uint32_t *g_xprof; static const char *g_xprof_path;')
w('static void xprof_dump(void) { FILE *f = fopen(g_xprof_path, "w"); if (!f) return; for (int i = 0; i < 0x10000; i++) if (g_xprof[i]) fprintf(f, "%04X %u\\n", i, g_xprof[i]); fclose(f); }')
w('static void xprof_init(void) { static int done; if (done) return; done = 1; g_xprof_path = getenv("PROPCYCL_SNDXPROF"); if (g_xprof_path) { g_xprof = calloc(0x10000, sizeof *g_xprof); atexit(xprof_dump); } }')
w('')
w('/* one instruction of the translated program (or a whole readable routine) */')
w('static void step(m37710_t *c)')
w('{')
w('    {')
w('        const uint32_t pc = ((uint32_t)c->pg << 16) | c->pc;')
w('        if (g_xprof) g_xprof[pc & 0xFFFF]++;')
w('        const unsigned mx = (c->ps >> 4) & 3u;')
w('        switch (pc) {')
for pc in sorted(ins):
    w(f'        case 0x{pc:06X}:')
    if pc in overrides:
        fn, mc = overrides[pc]
        if mc == 0:   # untimed: plain C, held by the event gate (tools/gen/snd_event_gate.sh)
            w(f'            if ({fn}(c)) break;   /* readable, untimed: src/snd/snd_engine.c */')
        else:
            w(f'            if (snd_atomic_ok(c, {mc})) {{ const uint64_t t0 = c->cycles; if ({fn}(c)) {{ snd_cost_check(c, 0x{pc:06X}, t0, {mc}); break; }} }}   /* readable: src/snd/snd_engine.c */')
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
w('static void run_chunk(m37710_t *c, uint64_t until)')
w('{')
w('    while (c->cycles < until && !c->unimpl_hit) {')
w('        m37710_service(c);')
w('        if (c->stopped) { c->cycles = until; break; }')
w('        step(c);')
w('    }')
w('}')
w('')
w('/* Call a routine of the translated program from readable C and run it until it')
w(' * returns. A sentinel return address (bank 0, 0x0000: the peripheral block,')
w(' * never code) is pushed the way JSR pushes one, in the very stack slot the')
w(' * original\'s return address occupies, so the routine\'s own RTS ends the call.')
w(' * Interrupts and the board pins are serviced between its instructions exactly')
w(' * as in run_chunk (making the call one event lost Timer B0 underflows).')
w(' *')
w(' * Returns 0 when the routine returned. Returns nonzero when the call must be')
w(' * ABANDONED: the sound stopped, or the routine consumed the sentinel instead')
w(' * of returning to it -- the driver exits through several levels on purpose')
w(' * (end of a track at 0xE251: PLY drops its caller\'s return, and the RTS')
w(' * goes one level further up). The translated code then simply carries on from')
w(' * that exact state and returns up the real stack, as the original does, so a')
w(' * readable caller does `if (snd_call(...)) return 1;` and does nothing else. */')
w('void mcu_sound_raise_due_pins(void);')
w('int snd_call(m37710_t *c, uint16_t addr)')
w('{')
w('    push16(c, 0x0000);')
w('    const uint16_t s_call = c->s;               /* the sentinel sits at s_call+1..+2 */')
w('    c->pg = 0; c->pc = addr;')
w('    for (unsigned long n = 0; ; n++) {')
w('        if (c->unimpl_hit) return 1;')
w('        const uint16_t d = (uint16_t)(c->s - s_call);')
w('        /* Every sentinel is 0x0000, so PC = 0 alone does not say WHOSE: a callee')
w('         * that drops a frame and returns in one step (a readable 0xE18D: PLY ;')
w('         * RTS) lands on an OUTER call\'s sentinel. Only our own, popped by that')
w('         * return, leaves the stack exactly 2 above where we pushed it. */')
w('        if (c->pg == 0 && c->pc == 0x0000) return d == 2 ? (c->unimpl_hit ? 1 : 0) : 2;')
w('        if (d >= 1 && d < 0x8000) return 2;         /* unwound past the call */')
w('        mcu_sound_raise_due_pins();')
w('        m37710_service(c);')
w('        if (c->stopped || n > 50000000ul) { trap(c, ((uint32_t)c->pg << 16) | c->pc, c->stopped ? "stopped inside a call from readable C" : "call from readable C did not return"); return 1; }')
w('        step(c);')
w('    }')
w('}')
w('')
w('void snd_executor_init(void) { xprof_init(); printf("[SND] translated sound program (%d instructions)\\n", ' + str(sum(len(v) for v in ins.values())) + '); }')
w('')
w('/* src/mcu_sound.c\'s contract, identical to m37710_run(): run `cycles` cycles. */')
w('int snd_run(m37710_t *c, int cycles) { run_chunk(c, c->cycles + (uint64_t)cycles); return 0; }')
os.makedirs('gen', exist_ok=True)
open('gen/snd_driver.c', 'w').write('\n'.join(out) + '\n')
print(f'{len(overrides)} readable routines; translated {sum(len(v) for v in ins.values())} instructions at {len(ins)} addresses: {covered} executed by the oracle '
      f'+ {static} reached statically; {nram} from RAM (guarded) -> gen/snd_driver.c')
