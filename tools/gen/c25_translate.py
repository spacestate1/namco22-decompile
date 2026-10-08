#!/usr/bin/env python3
"""c25_translate.py --game pc|rr|tw|dd|tc|cs --roms DIR --cov FILE [--cov FILE...] --out FILE --func NAME
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

--game cs: Cyber Sled (Namco System 21, cybsled/), a different chip and board: the C67 (a TMS320C25 with a
4K-word internal ROM, c67.bin, the BIOS at program 0) and the programs the master 68000 uploads to program
0x8000, held in the shared DATA ROM (cy1-data-u.3a even bytes / cy1-data-l.1a odd) as `count, count words`
(count = the number of words, no +1). Block 0 is the master DSP's program, block 1E16 the slave DSP's; the two
chips run different programs at the same addresses, so cybsled generates one function per chip (--block 0
-> cs_c67_master, --block 1E16 -> cs_c67_slave), each from its own chip's coverage.
"""
import argparse, os, re, sys

ap = argparse.ArgumentParser()
ap.add_argument('--game', required=True, choices=['pc', 'rr', 'tw', 'dd', 'tc', 'cs'])
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

# the C71 BIOS is built into the games (engine/c25/c71_bios.inc, made by tools/gen/c71_embed.py from MAME's c71.bin): the same words the run-time loads
_inc = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))), 'engine', 'c25', 'c71_bios.inc')
bios_img = {i: int(w, 16) for i, w in enumerate(re.findall(r'0x([0-9a-fA-F]{4})', open(_inc).read())[:0x4000])}
assert len(bios_img) == 0x1000, f'{_inc}: expected 4096 BIOS words, found {len(bios_img)}'

prog_base = 0x4000                           # where the uploaded programs land in program space
if a.game == 'cs':
    # Cyber Sled (System 21): the C67's own internal ROM is the BIOS (c67.bin, 4096 BE words at program 0), the uploaded programs
    # live in the data ROM, ROM_LOAD16_BYTE (u = even byte), and land at program 0x8000 (cybsled/src/dsp/s21_dsp.c).
    b = open(os.path.join(a.roms, 'c67.bin'), 'rb').read()
    bios_img = {i: w for i, w in enumerate(words_be(b[:0x2000]))}
    u = open(os.path.join(a.roms, 'cy1-data-u.3a'), 'rb').read(); l = open(os.path.join(a.roms, 'cy1-data-l.1a'), 'rb').read()
    rom = bytearray(2 * len(u)); rom[0::2] = u; rom[1::2] = l
    main_block = 0x0
    prog_base = 0x8000
elif a.game == 'dd':
    # Dirt Dash (Super System 22): two 2 MB chips, MAME ROM_LOAD32_WORD_SWAP -- dt2vera.2 the high word of every long, dt2vera.1 the
    # low, each word byte-swapped (dirtdash/src/dd_mem.c dd_load_program). The master program's count word is at 0x57F00 (0x187B: 6,268
    # words, program 0x4000..0x587B): found by matching MAME's master program RAM (tools/mame/mseq_run.sh captures) in the 68K ROM.
    hi = open(os.path.join(a.roms, 'dt2vera.2'), 'rb').read(); lo = open(os.path.join(a.roms, 'dt2vera.1'), 'rb').read()
    rom = bytearray(2 * len(hi) + 2 * len(lo))
    for i in range(0, len(hi), 2):
        rom[2 * i] = hi[i + 1]; rom[2 * i + 1] = hi[i]; rom[2 * i + 2] = lo[i + 1]; rom[2 * i + 3] = lo[i]
    main_block = 0x57F00
elif a.game == 'tc':
    # Time Crisis (Super System 22): the assembled 68K program (timecris/tools/setup_roms.py: four 1 MB chips ROM_LOAD32_BYTE). The upload routine
    # (0xBBA3C..) copies the master program's block at 0xABA8C to polygon RAM 0xC00C00 before it writes 0xFF to syscon 0x1C; a second block at
    # 0xBB616 follows the handshake (the slave's, as in Rave Racer / Ace Driver). --main-rom timecris_main.bin
    rom = open(a.main_rom, 'rb').read() if a.main_rom else open(os.path.join(os.path.dirname(os.path.abspath(a.roms)), 'timecris_main.bin'), 'rb').read()
    main_block = 0xABA8C
elif a.game in ('pc', 'tw'):
    # Super System 22: <game>ver-a.1..4 byte-interleaved 4,3,2,1 (src/rom_loader.c; ROM_LOAD32_BYTE).
    #   Prop Cycle: pr2ver-a.*, the game program's count word at 0x43748 (src/master_dsp.c)
    #   Tokyo Wars: tw2ver-a.*, the master program's count word at 0x127816 (FUN_0012ED28 uploads it
    #   as block 1; blocks 0x12EECA and 0x12A90E are the SLAVE's, relayed through the master's port 7)
    pfx = 'pr2' if a.game == 'pc' else 'tw2'
    chips = [open(os.path.join(a.roms, f'{pfx}ver-a.{k}'), 'rb').read() for k in (4, 3, 2, 1)]
    rom = bytearray(4 * len(chips[0]))
    for k in range(4): rom[k::4] = chips[k]
    main_block = 0x43748 if a.game == 'pc' else 0x127816
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
    cnt = ((rom[ca] << 8) | rom[ca + 1]) + (0 if a.game == 'cs' else 1)
    if not 0 < cnt <= 0xC000: sys.exit(f'bad master program size {cnt} at {ca:X}')
    images[f'{ca:X}'] = {prog_base + i: w for i, w in enumerate(words_be(rom[ca + 2: ca + 2 + 2 * cnt]))}
cnt = len(images[f'{main_block:X}']) if f'{main_block:X}' in images else 0

# ---- decode: length and control flow (engine/c25/c25_sem.h c25_exec/misc) ---------
def length(op):
    hi, lo = op >> 8, op & 0xFF
    if hi in (0x5C, 0x5D, 0x5E, 0x5F, 0xFC, 0xFD): return 2          # MACD MAC BC BNC BLKP BLKD
    if (hi & 0xF0) == 0xD0: return 2                                  # long immediates
    if hi in (0xFF, 0xFE, 0xF0, 0xF1, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8, 0xF9, 0xFA, 0xFB) and (lo & 0x80):
        return 2                                                      # B CALL Bcond (incl. BV 0xF0 / BNV 0xF7) BANZ
    return 1

def valid(op):
    """False where the oracle faults (it would stop there too)."""
    hi, lo = op >> 8, op & 0xFF
    if (hi & 0xF0) == 0xD0: return lo <= 6
    # BV (0xF0) / BNV (0xF7): the engine has the OV flag since 2026-10-06 (cybsled's C67 programs use them); before, the
    # oracle faulted on them and this returned False. No other game's program reaches one (their translations are unchanged).
    if hi in (0xFF, 0xFE, 0xF0, 0xF1, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8, 0xF9, 0xFA, 0xFB): return bool(lo & 0x80)
    if 0x5C <= hi <= 0x5F or hi in (0xFC, 0xFD): return True
    return True        # the rest: exec1/misc decide; a fault there faults in both

def successors(pc, op, op2):
    hi, lo = op >> 8, op & 0xFF
    nxt = (pc + length(op)) & 0xFFFF
    if hi == 0xFF and (lo & 0x80): return [op2]                       # B
    if hi == 0xFE and (lo & 0x80): return [op2, nxt]                  # CALL
    if (hi in (0xF0, 0xF1, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8, 0xF9, 0xFA, 0xFB) and (lo & 0x80)) or hi in (0x5E, 0x5F):
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

def dispatch_seeds(img, ins):
    """Where the computed branches go, from the program's own tables (no ROM is interpreted: the words
    are read the way the disassembler reads them). Two idioms cover every BACC/CALA in the master
    programs seen so far:
      BACC  `ADLK/LALK base ; ... ; BACC` followed IN LINE by a table of `B target` (0xFF80 target),
            one per index -- every `B` right after the BACC is a case, executed or not;
      CALA  `LALK T ; ADD index ; SACL ; LAR ARn ; LAC * ; CALA`: a table of code pointers at T in
            program memory (data space aliases 0x4000..), read while the words are addresses in this image;
            or, when T itself holds `B` (0xFF80), `LALK T ; ADD index ; CALA` into an in-line `B` table as for BACC.
    A coverage-only translation traps on every case the oracle's scenarios never took; this finds them.
    Returns (seeds, [(kind, site, entries)])."""
    seeds, sites = set(), []
    for pc in sorted(ins):
        op = img[pc]
        if op == 0xCE25:
            a, n = pc + 1, 0
            while img.get(a) == 0xFF80 and (a + 1) in img: seeds.add(a); a += 2; n += 1
            sites.append(('BACC', pc, n))
        elif op == 0xCE24:
            base = None
            for a in range(pc - 1, pc - 17, -1):
                if a in ins and img.get(a) == 0xD001 and (a + 1) in img: base = img[a + 1]; break
            n = 0
            if base is not None and img.get(base) == 0xFF80:
                # `LALK T ; ADD index ; CALA` straight into an IN-LINE table of `B target` (cybsled's C67 master program, 0x86A2 /
                # 0x86FA -> 0x86A5): every `B` from T on is a case, as for BACC
                a2 = base
                while img.get(a2) == 0xFF80 and (a2 + 1) in img: seeds.add(a2); a2 += 2; n += 1
            elif base is not None:
                while n < 64 and img.get(base + n) is not None and (base + n) in img and img[base + n] in img: seeds.add(img[base + n]); n += 1
            sites.append(('CALA', pc, n))
    return seeds, sites

variants = {}                                # pc -> {(op, op2 or None)}
n_cov = n_all = 0
dispatch = []
for tag, img in images.items():
    seeds = set(cov[tag])
    if tag == 'bios': seeds |= {0x0000, 0x0002, 0x0004, 0x0006, 0x0018, 0x001A, 0x001C, 0x001E}
    elif a.game == 'cs':
        # the C67 BIOS enters an uploaded program at its first word, and forwards the chip's vectors (INT0/1/2 at 2/4/6, TINT/RINT/
        # XINT/TRAP at 0x18..0x1E) to `B` entries at program+2..+0xE: a per-image walk cannot follow the BIOS's jumps there
        seeds |= {prog_base + 2 * k for k in range(8)}
    ins = walk(img, seeds)
    while True:                                  # tables can lead to code with more tables
        extra, sites = dispatch_seeds(img, ins)
        new = walk(img, seeds | ins | extra)
        if new == ins: break
        ins = new
    dispatch += [(tag,) + x for x in sites]
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
w(' * opcode a constant. The iter hook exists only in C25_DEV_HOOKS builds. */')
w('#ifdef C25_DEV_HOOKS')
w('#define RUN_HOOK_ITER if (c25_hook_iter) c25_hook_iter();')
w('#else')
w('#define RUN_HOOK_ITER')
w('#endif')
w('#define RUN(PC, OP, O) do { \\')
w('        d->ops = (O); d->pc = (uint16_t)((PC) + 1); \\')
w('        const int n_ = d->rpt + 1; d->rpt = 0; \\')
w('        for (int it_ = 0; it_ < n_; it_++) { \\')
w('            RUN_HOOK_ITER \\')
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
# BIOS sites (the same c71.bin in every game) are data-driven and covered by every game's coverage: one summary line.
# A site in a GAME program with no readable table is a real gap in a coverage-only translation: named.
bios_open = [f'{kind} {site:04X}' for tag, kind, site, n in dispatch if n == 0 and tag == 'bios']
if bios_open: print(f'c25_translate: note: {len(bios_open)} BIOS BACC/CALA sites have no table this tool can read (coverage only): ' + ', '.join(bios_open), file=sys.stderr)
for tag, kind, site, n in dispatch:
    if n == 0 and tag != 'bios': print(f'c25_translate: WARNING: {kind} at {tag}:{site:04X} has no table this tool can read -- its targets come from coverage only', file=sys.stderr)
print(f'c25_translate: {a.game}: {len(ins)} addresses, {sum(len(v) for v in variants.values())} variants ({n_cov} covered, {static} static) -> {a.out}',
      file=sys.stderr)
