#!/usr/bin/env python3
"""c25_translate.py --game pc|rr --roms DIR --cov FILE [--cov FILE...] --out FILE --func NAME
                   [--block ADDR ...]

Translate a System 22 / Super System 22 MASTER DSP program -- the C71 BIOS
(c71.bin, program 0x0000) and the game's own master program (the block its 68K
uploads to program 0x4000) -- to C, AHEAD OF TIME. The shared tool for every
System 22 game (HARD RULE 1 and 4 in CLAUDE.md).

A board runs more than one master program: the game's, and the self-test
programs its 68K uploads in test mode, all to program 0x4000. Each is a
count-prefixed block in the 68K ROM (--block: the address of its count word).
A translated instruction depends only on its own words, so each address gets
one VARIANT per distinct word pair any program has there; at run time the
variant whose words program memory holds runs, and none -> TRAP.

Instruction BOUNDARIES come from the oracle's coverage (addresses the
interpreter executed: tools/c25oracle, C71_COV, reduced to addresses) extended
by a static walk along control flow from them and from the BIOS entry points;
instruction WORDS come from the ROM files, never from a running program. Each
instruction becomes a `case` that
  - checks program memory still holds the words it was translated from (the
    68K uploads the program at run time; a different upload, or code that
    rewrote itself, TRAPS instead of running the wrong translation),
  - runs c25_exec() (engine/c25/c25_sem.h, the SAME source the oracle runs)
    with the opcode as a CONSTANT, the second word from a const array,
  - repeats it for RPT exactly as the oracle's step does,
so the translation can be gated as EQUAL to the oracle. A PC with no
translation TRAPS LOUDLY (the master stops and says where) -- never skipped.
"""
import argparse, os, sys

ap = argparse.ArgumentParser()
ap.add_argument('--game', required=True, choices=['pc', 'rr'])
ap.add_argument('--roms', required=True)
ap.add_argument('--cov', action='append', default=[])
ap.add_argument('--out')
ap.add_argument('--func')
ap.add_argument('--attribute', nargs='+', help='DEV: C71_COV triple files -> the committed "ADDR TAG" coverage on stdout')
ap.add_argument('--main-rom', help='rr: the assembled 68K program (raverace_main.bin)')
ap.add_argument('--block', action='append', default=[], help='count-word address of a program block (hex); default: the game program')
a = ap.parse_args()

# ---- the program image, from the ROM files -------------------------------------
def words_be(b):
    return [(b[2 * i] << 8) | b[2 * i + 1] for i in range(len(b) // 2)]

bp = os.path.join(a.roms, 'c71.bin')
if os.path.exists(bp):
    bios_img = {i: w for i, w in enumerate(words_be(open(bp, 'rb').read())[:0x4000])}
else:   # optional in some ROM sets: the BIOS is then left untranslated (the master cannot boot)
    print(f'c25_translate: WARNING: no {bp}; the master DSP BIOS is not translated', file=sys.stderr)
    bios_img = {}

if a.game == 'pc':
    # Prop Cycle: pr2ver-a.1..4 byte-interleaved 4,3,2,1 (src/rom_loader.c);
    # the game program's count word at 0x43748 (src/master_dsp.c)
    chips = [open(os.path.join(a.roms, f'pr2ver-a.{k}'), 'rb').read() for k in (4, 3, 2, 1)]
    rom = bytearray(4 * len(chips[0]))
    for k in range(4): rom[k::4] = chips[k]
    main_block = 0x43748
else:
    # Rave Racer: the assembled 68K program; the game program's count at 0x31A68
    # (FUN_000318BE), the test-mode programs' at FUN_00006DA2's call sites
    if a.main_rom and os.path.exists(a.main_rom):
        rom = open(a.main_rom, 'rb').read()
    else:   # from the chips, as the game loads them (src/rr_mem.c rr_load_program)
        lanes = [open(os.path.join(a.roms, n), 'rb').read() for n in
                 ('rv2_prguub.6d', 'rv2_prgumb.8d', 'rv2_prglmb.2d', 'rv2_prgllb.4d')]
        rom = bytearray(4 * len(lanes[0]))
        for k in range(4): rom[k::4] = lanes[k]
    main_block = 0x31A68
blocks = [int(b, 16) for b in a.block] or [main_block]

images = {'bios': bios_img}                  # tag -> {program address: word}
for ca in blocks:
    cnt = ((rom[ca] << 8) | rom[ca + 1]) + 1
    if not 0 < cnt <= 0xC000: sys.exit(f'bad master program size {cnt} at {ca:X}')
    images[f'{ca:X}'] = {0x4000 + i: w for i, w in enumerate(words_be(rom[ca + 2: ca + 2 + 2 * cnt]))}
cnt = len(images[f'{main_block:X}']) if f'{main_block:X}' in images else 0

# ---- decode: length and control flow (engine/c25/c25_sem.h c25_exec/misc) ---------
def length(op):
    hi, lo = op >> 8, op & 0xFF
    if hi in (0x5C, 0x5D, 0x5E, 0x5F, 0xFC, 0xFD): return 2          # MACD MAC BC BNC BLKP BLKD
    if (hi & 0xF0) == 0xD0: return 2                                  # long immediates
    if hi in (0xFF, 0xFE, 0xF1, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF8, 0xF9, 0xFA, 0xFB) and (lo & 0x80):
        return 2                                                      # B CALL Bcond BANZ
    return 1

def valid(op):
    """False where the oracle faults (it would stop there too)."""
    hi, lo = op >> 8, op & 0xFF
    if (hi & 0xF0) == 0xD0: return lo <= 6
    if hi in (0xFF, 0xFE, 0xF1, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF8, 0xF9, 0xFA, 0xFB): return bool(lo & 0x80)
    if hi == 0xF7 or hi == 0xF0: return False
    if 0x5C <= hi <= 0x5F or hi in (0xFC, 0xFD): return True
    return True        # the rest: exec1/misc decide; a fault there faults in both

def successors(pc, op, op2):
    hi, lo = op >> 8, op & 0xFF
    nxt = (pc + length(op)) & 0xFFFF
    if hi == 0xFF and (lo & 0x80): return [op2]                       # B
    if hi == 0xFE and (lo & 0x80): return [op2, nxt]                  # CALL
    if (hi in (0xF1, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF8, 0xF9, 0xFA, 0xFB) and (lo & 0x80)) or hi in (0x5E, 0x5F):
        return [op2, nxt]                                             # conditional, BANZ, BC/BNC
    if hi == 0xCE:
        if lo in (0x25, 0x26): return []                              # BACC, RET: dynamic
        if lo == 0x1E: return [0x1E, nxt]                             # TRAP
    return [nxt]

# ---- DEV: attribute the oracle's (pc, word0, word1) executions to programs --------
if a.attribute:
    lines = set()
    for path in a.attribute:
        for l in open(path):
            p = l.split()
            if len(p) < 3: continue
            pc, w0, w1 = (int(x, 16) for x in p[:3])
            tags = [t for t, img in images.items()
                    if img.get(pc) == w0 and (length(w0) == 1 or img.get(pc + 1) == w1)]
            if not tags: sys.exit(f'{path}: {pc:04X} {w0:04X} is in no program given')
            for t in tags: lines.add((pc, t))   # identical words in several programs: all of them
    for pc, t in sorted(lines): print(f'{pc:04X} {t}')
    sys.exit(0)

# ---- which addresses are instructions, per program --------------------------------
# coverage lines: "ADDR TAG" (TAG = bios or a block's count address) -- addresses only
cov = {t: set() for t in images}
for path in a.cov:
    for l in open(path):
        p = l.split()
        if not p or p[0].startswith('#'): continue
        tag = p[1] if len(p) > 1 else ('bios' if int(p[0], 16) < 0x4000 else f'{main_block:X}')
        if tag not in images: sys.exit(f'{path}: coverage for a program not given (--block {tag})')
        if int(p[0], 16) not in images[tag]: sys.exit(f'{path}: {p[0]} is outside program {tag}')
        cov[tag].add(int(p[0], 16))

def walk(img, seeds):
    ins = set(); work = list(seeds)
    while work:
        pc = work.pop()
        if pc in ins or pc not in img: continue
        op = img[pc]
        if not valid(op): continue
        if length(op) == 2 and (pc + 1) not in img: continue
        ins.add(pc)
        for t in successors(pc, op, img.get(pc + 1, 0)):
            if t not in ins and t in img: work.append(t)
    return ins

variants = {}                                # pc -> {(op, op2 or None)}
n_cov = n_all = 0
for tag, img in images.items():
    seeds = set(cov[tag])
    if tag == 'bios': seeds |= {0x0000, 0x0002, 0x0004, 0x0006, 0x0018, 0x001A, 0x001C, 0x001E}
    ins = walk(img, seeds)
    n_cov += len(cov[tag] & ins); n_all += len(ins)
    for pc in ins:
        op = img[pc]
        variants.setdefault(pc, set()).add((op, img[pc + 1] if length(op) == 2 else None))
ins = set(variants)
static = n_all - n_cov

# ---- emit --------------------------------------------------------------------------
out = []
w = out.append
w(f'/* GENERATED by tools/gen/c25_translate.py --game {a.game} from the ROM files and the')
w(' * oracle\'s coverage -- do not edit. ROM-derived: gen/ is not committed.')
w(f' * {len(ins)} addresses, {sum(len(v) for v in variants.values())} instruction variants ({n_cov} executed by')
w(f' * the oracle, {static} from the static walk); programs: ' + ', '.join(images) + ' */')
w('#include <stdio.h>')
w('#include "c25.h"')
w('#include "c25_sem.h"')
w('')
w('static bool trap(c71_t *d, int pc, const char *why)')
w('{')
w('    snprintf(d->error, sizeof d->error, "TRANSLATED C25: %s at %04X", why, pc);')
w('    return false;')
w('}')
w('')
w('/* One instruction and its RPT repeats: the step loop of the oracle, with the')
w(' * opcode a constant. */')
w('#define RUN(PC, OP, O) do { \\')
w('        d->ops = (O); d->pc = (uint16_t)((PC) + 1); \\')
w('        const int n_ = d->rpt + 1; d->rpt = 0; \\')
w('        for (int it_ = 0; it_ < n_; it_++) { \\')
w('            if (c25_hook_iter) c25_hook_iter(); \\')
w('            if (!c25_exec(d, (PC), (OP), it_)) { d->ops = NULL; return false; } \\')
w('        } \\')
w('        d->ops = NULL; return true; \\')
w('    } while (0)')
w('')
w(f'bool {a.func}(c71_t *d, int pc)')
w('{')
w('    switch (pc) {')
for pc in sorted(ins):
    w(f'    case 0x{pc:04X}:')
    for op, op2 in sorted(variants[pc]):
        if op2 is not None:
            w(f'        if (d->prog[0x{pc:04X}] == 0x{op:04X} && d->prog[0x{(pc + 1) & 0xFFFF:04X}] == 0x{op2:04X}) {{')
            w(f'            static const uint16_t o[2] = {{ 0x{op:04X}, 0x{op2:04X} }}; RUN(0x{pc:04X}, 0x{op:04X}, o); }}')
        else:
            w(f'        if (d->prog[0x{pc:04X}] == 0x{op:04X}) RUN(0x{pc:04X}, 0x{op:04X}, NULL);')
    w('        return trap(d, pc, "program memory holds no translated program here");')
w('    default: return trap(d, pc, "no translation for this address");')
w('    }')
w('}')
os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
open(a.out, 'w').write('\n'.join(out) + '\n')
print(f'c25_translate: {a.game}: {len(ins)} addresses, {sum(len(v) for v in variants.values())} variants ({n_cov} covered, {static} static) -> {a.out}',
      file=sys.stderr)
