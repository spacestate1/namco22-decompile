#!/usr/bin/env python3
"""h8_decode.py -- the H8/300 and H8/300H instruction decoder for the H8 translations (Time Crisis 2's sub-CPU, an H8/3002 = H8/300H in
advanced mode, 24-bit addresses; its I/O board's H8/3334 = H8/300, 16-bit). Table-driven from tools/h8/h8_ops.txt (the encodings), the
first match winning; checked against MAME's own disassembly of every executed instruction (tools/mame/h8cov writes <cov>.dasm):
  python3 tools/h8/h8_decode.py ROM ADVANCED(0|1) cov/h8sub_play1.dasm        -> how many of MAME's lines we reproduce
decode(mem, pc, advanced) -> Ins(pc, size, mnem, am1, am2, f) with the fields an instruction needs (f: 'op' the opcode words as one int per
slot, 'r1', 'r2' ..., 'imm', 'disp', 'abs', 'target')."""
import os, sys
HERE = os.path.dirname(os.path.abspath(__file__))

class Op:
    def __init__(s, val, mask, skip, mnem, am1, am2, lvl):
        s.val, s.mask, s.skip, s.mnem, s.am1, s.am2, s.lvl = int(val, 16), int(mask, 16), int(skip), mnem, am1, am2, lvl
        s.nbytes = len(val) // 2
        ex = 0
        if (am1 in ("abs16", "abs16e", "abs24e") or am2 == "abs16") and s.skip == 0: ex += 1
        if (am1 == "abs32" or am2 == "abs32") and s.skip == 0: ex += 2
        if am1 in ("imm16", "rel16", "r16d16h", "r32d16h") or am2 in ("r16d16h", "r32d16h"): ex += 1
        if am1 in ("imm32", "r32d32hh") or am2 == "r32d32hh": ex += 2
        s.size = s.nbytes + 2 * s.skip + 2 * ex
OPS = [Op(*l.split()) for l in open(os.path.join(HERE, "h8_ops.txt")) if l.strip() and not l.startswith("#")]

def table(advanced):
    out = []
    for o in OPS:
        if (o.lvl == "o" and advanced) or (o.lvl == "h" and not advanced): continue
        val, mask = o.val, o.mask
        if not advanced:                         # H8/300: 16-bit registers are R0-R7 only (MAME h8make.py forces the E bit clear)
            if "r16l" in (o.am1, o.am2): mask |= 0x08
            if "r16h" in (o.am1, o.am2): mask |= 0x80
        out.append((o, val, mask))
    return out
TAB = {0: table(0), 1: table(1)}

class Ins:
    __slots__ = ("pc", "size", "mnem", "am1", "am2", "slot", "w", "advanced")
def r16(m, a): return (m[a] << 8 | m[a + 1]) if a + 1 < len(m) else 0
def r32(m, a): return r16(m, a) << 16 | r16(m, a + 2)

def decode(m, pc, advanced):
    w0 = r16(m, pc)
    slots = [w0, r32(m, pc), w0 << 16 | r16(m, pc + 4), w0 << 16 | r16(m, pc + 6), r32(m, pc + 2)]
    for o, val, mask in TAB[advanced]:
        if o.nbytes == 2:
            if w0 & mask != val: continue
            slot = 0
        elif o.nbytes == 4:
            slot = o.skip + 1
            if slots[slot] & mask != val: continue
        else:                                    # 6 bytes: the first word, then words 1-2
            if w0 & (mask >> 32) != val >> 32 or slots[4] & (mask & 0xFFFFFFFF) != val & 0xFFFFFFFF: continue
            slot = 4
        i = Ins(); i.pc, i.size, i.mnem, i.am1, i.am2, i.advanced = pc, o.size, o.mnem, o.am1, o.am2, advanced
        i.slot = slots[slot]; i.w = (slots, o)
        return i
    return None

R8 = ["r0h", "r1h", "r2h", "r3h", "r4h", "r5h", "r6h", "r7h", "r0l", "r1l", "r2l", "r3l", "r4l", "r5l", "r6l", "r7l"]
R16 = ["r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7", "e0", "e1", "e2", "e3", "e4", "e5", "e6", "e7"]
R32 = ["er0", "er1", "er2", "er3", "er4", "er5", "er6", "sp"]
def s8(v): return v - 0x100 if v & 0x80 else v
def s16(v): return v - 0x10000 if v & 0x8000 else v

def operand(m, i, am):
    """(kind, value) of an operand: ('r8', n) ('r16', n) ('r32', n) ('ind', n) ('pre', n) ('post', n) ('d16', n, d) ('d32', n, d)
    ('abs', addr) ('imm', v) ('target', addr) ('ind8', addr) ('ccr',) ('exr',) ('block', first, count) ('psp',) ('spp',)"""
    op, pc, adv = i.slot, i.pc, i.advanced
    epc = pc + i.size
    if am == "r8l": return ("r8", op & 15)
    if am == "r8h": return ("r8", op >> 4 & 15)
    if am == "r8u": return ("r8", op >> 8 & 15)
    if am == "r16l": return ("r16", op & 15)
    if am == "r16h": return ("r16", op >> 4 & 15)
    if am == "r32l": return ("r32", op & 7)
    if am == "r32h": return ("r32", op >> 4 & 7)
    if am in ("r16ih", "r32ih"): return ("ind", op >> 4 & 7)
    if am in ("r16ihh", "r32ihh"): return ("ind", op >> 20 & 7)
    if am in ("pr16h", "pr32h"): return ("pre", op >> 4 & 7)
    if am in ("r16ph", "r32ph"): return ("post", op >> 4 & 7)
    if am == "r32pl": return ("post", op & 7)
    if am in ("r16d16h", "r32d16h"): return ("d16", op >> 4 & 7, s16(r16(m, epc - 2)))
    if am == "r32d32hh": return ("d32", op >> 20 & 7, r32(m, epc - 4))
    if am == "psp": return ("pre", 7)
    if am == "spp": return ("post", 7)
    if am == "r32n2l": return ("block", op & 6, 2)
    if am == "r32n3l": return ("block", op & 4, 3)
    if am == "r32n4l": return ("block", op & 4, 4)
    if am == "abs8": return ("abs", (0xFFFF00 if adv else 0xFF00) | m[pc + 1])
    if am == "abs16":
        a = r16(m, epc - 4) if i.w[1].nbytes == 4 and i.w[1].skip == 1 else r16(m, epc - 2)
        return ("abs", (s16(a) & 0xFFFFFF) if adv else a)
    if am == "abs32":
        a = r32(m, epc - 6) if i.w[1].nbytes == 4 and i.w[1].skip == 2 else r32(m, epc - 4)
        return ("abs", a & 0xFFFFFF)
    if am == "abs8i": return ("ind8", m[pc + 1])
    if am == "abs16e": return ("target", r16(m, pc + 2))
    if am == "abs24e": return ("target", r32(m, pc) & 0xFFFFFF)
    if am == "rel8": return ("target", (pc + 2 + s8(m[pc + 1])) & (0xFFFFFF if adv else 0xFFFF))
    if am == "rel16": return ("target", (pc + 4 + s16(r16(m, pc + 2))) & (0xFFFFFF if adv else 0xFFFF))
    if am == "one": return ("imm", 1)
    if am == "two": return ("imm", 2)
    if am == "four": return ("imm", 4)
    if am == "imm2": return ("imm", op >> 4 & 3)
    if am == "imm3": return ("imm", op >> 4 & 7)
    if am == "imm8": return ("imm", m[epc - 1])
    if am == "imm16": return ("imm", r16(m, pc + 2))
    if am == "imm32": return ("imm", r32(m, pc + 2))
    if am == "ccr": return ("ccr",)
    if am == "exr": return ("exr",)
    raise ValueError("operand kind " + am)

def text(m, i):
    """MAME's disassembly format (for the check against its trace)"""
    def one(am):
        k = operand(m, i, am); adv = i.advanced
        t = k[0]
        if t == "r8": return R8[k[1]]
        if t == "r16": return R16[k[1]]
        if t == "r32": return R32[k[1]]
        if t == "ind": return "@" + (R32 if am.startswith("r32") else R16)[k[1]]
        if t == "pre": return "@-sp" if am == "psp" else "@-" + (R32 if am.startswith("pr32") else R16)[k[1]]
        if t == "post": return "@sp+" if am == "spp" else "@" + (R32 if am.startswith("r32") else R16)[k[1]] + "+"
        if t == "d16":
            d = r16(m, i.pc + i.size - 2)
            return "@(h'%x, %s)" % (d, (R32 if am.startswith("r32") else R16)[k[1]])
        if t == "d32": return "@(h'%x, %s)" % (k[2], R32[k[1]])
        if t == "block": return "%s-%s" % (R32[k[1]], R32[k[1] + k[2] - 1])
        if t == "abs":
            if am == "abs8": return "@h'%06x" % k[1] if adv else "@h'%04x" % k[1]
            if am == "abs32": return "@h'%06x" % k[1]
            return "@h'%06x" % k[1] if adv else "@h'%04x" % k[1]
        if t == "ind8": return "@h'%02x" % k[1]
        if t == "target":
            if am == "abs16e": return "h'%04x" % k[1]
            if am == "abs24e": return "h'%06x" % k[1]
            return "h'%06x" % k[1] if adv else "h'%04x" % k[1]
        if t == "imm":
            if am in ("one", "two", "four"): return "#%d" % k[1]
            if am in ("imm2", "imm3"): return "#%x" % k[1]
            if am == "imm8": return "#h'%02x" % k[1]
            if am == "imm16": return "#h'%04x" % k[1]
            return "#h'%08x" % k[1]
        return t
    if i.am1 == "-": return i.mnem
    s = "%-8s" % i.mnem + one(i.am1)
    if i.am2 != "-": s += ", " + one(i.am2)
    return s

if __name__ == "__main__":
    rom = open(sys.argv[1], "rb").read(); adv = int(sys.argv[2])
    ok = bad = 0; shown = 0
    for l in open(sys.argv[3]):
        if ":" not in l: continue
        pc = int(l[:6], 16); want = l[8:].rstrip()
        if pc >= len(rom): continue
        i = decode(rom, pc, adv)
        got = text(rom, i) if i else "<none>"
        if " ".join(got.split()) == " ".join(want.split()): ok += 1
        else:
            bad += 1
            if shown < 25: print("%06X  MAME: %-40s ours: %s" % (pc, want, got)); shown += 1
    print("decoder vs MAME: %d of %d executed instructions identical (%d differ)" % (ok, ok + bad, bad))
