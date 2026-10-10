#!/usr/bin/env python3
"""[H8_ROMSIZE=n] h8_translate.py ROM ADVANCED NAME OUT.c [COVERAGE-PREFIX ...] -- translate an H8 program to C, one `case` per instruction address.
Reached: the reset/exception vectors, every executed PC and every landing in the MAME coverage (tools/mame/h8cov), and everything static
recursive descent finds from those (fall-through, branch and call targets). An address that is none of these TRAPS loudly at run time
(h8_untranslated) -- never a guess. The semantics are include/h8_sem.h (our own code); the decoder is tools/h8/h8_decode.py.
The function made:  void NAME_run(h8_cpu *c)  -- runs until c->budget <= 0 (or a sleep), the host services interrupts in between."""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from h8_decode import decode, operand

rom = open(sys.argv[1], "rb").read(); ADV = int(sys.argv[2]); NAME = sys.argv[3]; OUT = sys.argv[4]; COV = sys.argv[5:]
if os.environ.get("H8_ROMSIZE"): rom = rom[:int(os.environ["H8_ROMSIZE"], 0)]   # only what the CPU maps as ROM (the I/O board: 0x4000 of the 256 KB file)
AM = 0xFFFFFF if ADV else 0xFFFF
def r16(a): return rom[a] << 8 | rom[a + 1]
def r32(a): return r16(a) << 16 | r16(a + 2)

seeds = set()
nvec = 64 if ADV else 48
for v in range(nvec):
    t = (r32(v * 4) & AM) if ADV else r16(v * 2)
    if 0 < t < len(rom) and not t & 1: seeds.add(t)
for p in COV:
    for k in ("pcs", "calls", "jumps"):
        try:
            for l in open("%s.%s" % (p, k)):
                if l.strip(): seeds.add(int(l.split()[0], 16) & AM)
        except FileNotFoundError: pass
seeds = {s for s in seeds if s < len(rom) and not s & 1}

TERMINAL = {"rts", "rte", "jmp", "bt", "trapa", "sleep"}      # no fall-through (bt = bra; sleep resumes through an interrupt)
code = {}
def descend(work):
    while work:
        pc = work.pop()
        while pc not in code and pc < len(rom):
            i = decode(rom, pc, ADV)
            if i is None: break
            code[pc] = i
            for am in (i.am1, i.am2):
                if am in ("rel8", "rel16", "abs16e", "abs24e"):
                    t = operand(rom, i, am)[1]
                    if t < len(rom) and t not in code: work.append(t)
            if i.mnem in TERMINAL: break
            pc += i.size
descend(sorted(seeds))
# HANDLER TABLES: the sound program dispatches through RAM tables it fills from ROM tables of 16-bit routine addresses (sub-CPU
# 0x6F0A.. command handlers, 0x6F3C.. voice handlers -> jmp @er5), which static descent cannot see and coverage only partly took
# (stage 2 halted at 0x4616 and 0x5738). A run of >= 6 words that are all even, decodable addresses inside the code found so far is
# taken as such a table and every entry translated; repeated to a fixed point. A wrong guess only adds a case nothing jumps to.
def tables():
    lo, hi = min(code), max(code)
    found = set(); a = 0
    while a + 2 <= len(rom):
        run = []; b = a
        while b + 2 <= len(rom):
            t = r16(b)
            if t & 1 or not (lo <= t <= hi) or decode(rom, t, ADV) is None: break
            run.append(t); b += 2
        if len(run) >= 6: found.update(run); a = b
        else: a += 2
    # ADVANCED MODE (the H8/3002 sub-CPU) keeps its tables as 32-bit LONGS (0000xxxx): the 16-bit scan above sees a 0 every other word and
    # never took them -- the sequencer opcode table at 0x2576 (dispatch 0x2524), the system-command table 0x3BC4 (0x3B7C) and the state table
    # 0x3A5A were only translated where MAME's coverage happened to run them, and any other entry halted the CPU (sound + JVS dead). A run of
    # >= 3 longs (on any word boundary: 0x2576 is not a multiple of 4) that are all even, decodable addresses inside the code found so far is such a table.
    if ADV:
        a = 0
        while a + 4 <= len(rom):
            run = []; b = a
            while b + 4 <= len(rom):
                t = r32(b)
                if t >> 24 or t & 1 or not (lo <= t <= hi) or decode(rom, t, ADV) is None: break
                run.append(t); b += 4
            if len(run) >= 3: found.update(run); a = b
            else: a += 2
    return found
# ROUTINE POINTERS AS IMMEDIATES: the program also builds a continuation as a constant and stores it for a later jmp @Rn
# (mov.w #h'5744, r4 ; mov.w r4, @h'fffe00 -- stage 2 halted at 0x5744). Every 16/32-bit immediate of translated code that is an even,
# decodable address inside the code range is taken as one (the 68K lifter's imm_seeds class). Tables and immediates repeat to a fixed point.
def immediates():
    lo, hi = min(code), max(code)
    found = set()
    for i in list(code.values()):
        for am in (i.am1, i.am2):
            if am in ("imm16", "imm32"):
                t = operand(rom, i, am)[1] & AM
                if not t & 1 and lo <= t <= hi and decode(rom, t, ADV) is not None: found.add(t)
    return found
ntab = nimm = 0
while True:
    tnew = sorted(t for t in tables() if t not in code)
    inew = sorted(t for t in immediates() if t not in code and t not in tnew)
    if not tnew and not inew: break
    ntab += len(tnew); nimm += len(inew); descend(tnew + inew)
print("%s: %d table entries, %d routine-pointer immediates added" % (NAME, ntab, nimm), file=sys.stderr)
print("%s: %d instructions (%d seeds)" % (NAME, len(code), len(seeds)), file=sys.stderr)

SZ = {"b": 8, "w": 16, "l": 32}
def bits_of(m): return SZ.get(m.rsplit(".", 1)[-1], 8) if "." in m else 8
MASK = {8: "0xFFu", 16: "0xFFFFu", 32: "0xFFFFFFFFu"}

class Unsup(Exception): pass

def ea_pre(i, k, bits):
    """(pre-statements, address expression, post-statements) for a memory operand"""
    t = k[0]; step = bits // 8
    if t == "ind": return [], "h8_ea(c, %d)" % k[1], []
    if t == "pre": return ["h8_ea_add(c, %d, -%d);" % (k[1], step)], "h8_ea(c, %d)" % k[1], []
    if t == "post": return [], "h8_ea(c, %d)" % k[1], ["h8_ea_add(c, %d, %d);" % (k[1], step)]
    if t == "d16": return [], "(h8_ea(c, %d) + (uint32_t)%d)" % (k[1], k[2]), []
    if t == "d32": return [], "(h8_ea(c, %d) + 0x%XU)" % (k[1], k[2]), []
    if t == "abs": return [], "0x%XU" % k[1], []
    raise Unsup(t)

def rd(k, bits, out):
    """an expression for an operand's value; memory reads are hoisted into `out` (with their pre/post register updates)"""
    t = k[0]
    if t == "imm": return "0x%XU" % k[1]
    if t == "r8": return "h8_r8(c, %d)" % k[1]
    if t == "r16": return "h8_r16(c, %d)" % k[1]
    if t == "r32": return "c->er[%d]" % k[1]
    pre, a, post = ea_pre(None, k, bits)
    out += pre
    out.append("const uint32_t v%d = %s;" % (len(out), {8: "h8_rb", 16: "h8_rw", 32: "h8_rl"}[bits] + "(c, %s)" % a))
    name = "v%d" % (len(out) - 1)
    out += post
    return name

def wr(k, bits, val, out):
    t = k[0]
    if t == "r8": out.append("h8_w8(c, %d, (uint8_t)(%s));" % (k[1], val)); return
    if t == "r16": out.append("h8_w16(c, %d, (uint16_t)(%s));" % (k[1], val)); return
    if t == "r32": out.append("c->er[%d] = (uint32_t)(%s);" % (k[1], val)); return
    pre, a, post = ea_pre(None, k, bits)
    out += pre
    out.append("%s(c, %s, %s);" % ({8: "h8_wb", 16: "h8_ww", 32: "h8_wl"}[bits], a, ("(uint8_t)(%s)", "(uint16_t)(%s)", "(uint32_t)(%s)")[{8: 0, 16: 1, 32: 2}[bits]] % val))
    out += post

def is_mem(k): return k[0] in ("ind", "pre", "post", "d16", "d32", "abs")

CC = {"bt": 0, "bf": 1, "bhi": 2, "bls": 3, "bcc": 4, "bcs": 5, "bne": 6, "beq": 7, "bvc": 8, "bvs": 9, "bpl": 10, "bmi": 11, "bge": 12, "blt": 13, "bgt": 14, "ble": 15}
SHIFT = {"shal": "a", "shar": "r", "shll": "l", "shlr": "s", "rotl": "L", "rotr": "R", "rotxl": "X", "rotxr": "x"}

def body(i):
    """C statements for one instruction (c->pc already holds the next instruction's address)"""
    m = i.mnem; base = m.split(".")[0]; bits = bits_of(m)
    k1 = operand(rom, i, i.am1) if i.am1 != "-" else None
    k2 = operand(rom, i, i.am2) if i.am2 != "-" else None
    o = []
    if base == "nop": return o
    if base == "mov":
        v = rd(k1, bits, o); o.append("const uint32_t r_ = %s;" % v); o.append("h8_logic(c, r_ & %s, %d);" % (MASK[bits], bits)); wr(k2, bits, "r_", o); return o
    if base in ("add", "sub", "cmp", "addx", "subx"):
        s = rd(k1, bits, o); d = rd(k2, bits, o)
        if base == "add":  o.append("const uint32_t r_ = h8_add(c, %s, %s, %d, 0, 0);" % (d, s, bits))
        if base == "addx": o.append("const uint32_t r_ = h8_add(c, %s, %s, %d, c->ccr & 1, 1);" % (d, s, bits))
        if base in ("sub", "cmp"): o.append("const uint32_t r_ = h8_sub(c, %s, %s, %d, 0, 0);" % (d, s, bits))
        if base == "subx": o.append("const uint32_t r_ = h8_sub(c, %s, %s, %d, c->ccr & 1, 1);" % (d, s, bits))
        if base != "cmp": wr(k2, bits, "r_", o)
        else: o.append("(void)r_;")
        return o
    if base in ("adds", "subs"):
        n = k1[1] if k1[0] == "imm" else 1
        sign = "" if base == "adds" else "-"
        if k2[0] == "r32": o.append("c->er[%d] += (uint32_t)(%s%d);" % (k2[1], sign, n))
        else: o.append("h8_w16(c, %d, (uint16_t)(h8_r16(c, %d) + %s%d));" % (k2[1], k2[1], sign, n))
        return o
    if base in ("inc", "dec"):
        if k2 is None: k1, k2 = ("imm", 1), k1
        n = k1[1]; d = rd(k2, bits, o)
        o.append("const uint32_t r_ = h8_incdec(c, %s, %s%d, %d);" % (d, "" if base == "inc" else "-", n, bits)); wr(k2, bits, "r_", o); return o
    if base in ("and", "or", "xor"):
        s = rd(k1, bits, o); d = rd(k2, bits, o); opc = {"and": "&", "or": "|", "xor": "^"}[base]
        o.append("const uint32_t r_ = (%s %s %s) & %s;" % (d, opc, s, MASK[bits])); o.append("h8_logic(c, r_, %d);" % bits); wr(k2, bits, "r_", o); return o
    if base == "not":
        d = rd(k1, bits, o); o.append("const uint32_t r_ = ~%s & %s;" % (d, MASK[bits])); o.append("h8_logic(c, r_, %d);" % bits); wr(k1, bits, "r_", o); return o
    if base == "neg":
        d = rd(k1, bits, o); o.append("const uint32_t r_ = h8_sub(c, 0, %s, %d, 0, 0);" % (d, bits)); wr(k1, bits, "r_", o); return o
    if base in ("extu", "exts"):
        d = rd(k1, bits, o); half = bits // 2
        if base == "extu": o.append("const uint32_t r_ = %s & ((1u << %d) - 1);" % (d, half))
        else: o.append("const uint32_t r_ = (uint32_t)(int32_t)(int%d_t)%s & %s;" % (half, d, MASK[bits]))
        o.append("h8_logic(c, r_, %d);" % bits); wr(k1, bits, "r_", o); return o
    if base in SHIFT:
        n = 1; tgt = k1
        if k2 is not None: n = k1[1]; tgt = k2
        d = rd(tgt, bits, o); o.append("uint32_t r_ = %s;" % d)
        for _ in range(n): o.append("r_ = h8_shift(c, '%s', r_, %d);" % (SHIFT[base], bits))
        wr(tgt, bits, "r_", o); return o
    if base in ("mulxu", "mulxs"):
        # mulxu.b rs(8), rd(16): rd = rd.low8 * rs; mulxu.w rs(16), erd(32): erd = erd.low16 * rs
        sb = 8 if m.endswith(".b") else 16
        s = rd(k1, sb, o); d = rd(k2, sb * 2, o)
        if base == "mulxu": o.append("const uint32_t r_ = (uint32_t)((%s & %s) * (%s & %s));" % (d, MASK[sb], s, MASK[sb]))
        else:
            o.append("const uint32_t r_ = (uint32_t)((int32_t)(int%d_t)%s * (int32_t)(int%d_t)%s) & %s;" % (sb, d, sb, s, MASK[sb * 2]))
            o.append("h8_logic(c, r_, %d);" % (sb * 2))
        wr(k2, sb * 2, "r_", o); return o
    if base in ("divxu", "divxs"):
        sb = 8 if m.endswith(".b") else 16
        s = rd(k1, sb, o); d = rd(k2, sb * 2, o)
        o.append("const uint32_t s_ = %s & %s, d_ = %s;" % (s, MASK[sb], d))
        if base == "divxu":
            o.append("c->ccr = (uint8_t)((c->ccr & ~(H8_N | H8_Z)) | ((s_ >> %d) & 1 ? H8_N : 0) | (s_ == 0 ? H8_Z : 0));" % (sb - 1))
            o.append("uint32_t r_ = d_; if (s_) { const uint32_t q = d_ / s_, rm = d_ %% s_; r_ = (rm & %s) << %d | (q & %s); }" % (MASK[sb], sb, MASK[sb]))
        else:
            o.append("uint32_t r_ = d_; if (s_) { const int32_t sv = (int%d_t)s_, dv = (int%d_t)d_, q = dv / sv, rm = dv %% sv; r_ = ((uint32_t)rm & %s) << %d | ((uint32_t)q & %s);"
                     " c->ccr = (uint8_t)((c->ccr & ~(H8_N | H8_Z)) | (q < 0 ? H8_N : 0)); } else c->ccr |= H8_Z;" % (sb, sb * 2, MASK[sb], sb, MASK[sb]))
        wr(k2, sb * 2, "r_", o); return o
    if base in ("daa", "das"):
        d = rd(k1, 8, o); o.append("const uint32_t r_ = h8_daa(c, (uint8_t)%s, %d);" % (d, base == "das")); wr(k1, 8, "r_", o); return o
    if base in ("bset", "bclr", "bnot", "btst", "bld", "bild", "bst", "bist", "band", "biand", "bor", "bior", "bxor", "bixor"):
        bit = ("%d" % k1[1]) if k1[0] == "imm" else ("(h8_r8(c, %d) & 7)" % k1[1])
        d = rd(k2, 8, o); o.append("uint32_t r_ = %s; const int b_ = (int)(r_ >> %s & 1);" % (d, bit))
        if base == "btst": o.append("c->ccr = (uint8_t)((c->ccr & ~H8_Z) | (b_ ? 0 : H8_Z));"); return o
        if base in ("bld", "bild"): o.append("c->ccr = (uint8_t)((c->ccr & ~H8_C) | (b_ %s 1 ? 0 : 0) | ((%sb_) ? H8_C : 0));" % ("^", "" if base == "bld" else "!")); return o
        if base in ("band", "biand", "bor", "bior", "bxor", "bixor"):
            v = "b_" if not base.startswith("bi") else "!b_"
            opc = {"and": "&", "or": "|", "xor": "^"}[base[2:] if base.startswith("bi") else base[1:]]
            o.append("const int c_ = (c->ccr & H8_C) %s (%s); c->ccr = (uint8_t)((c->ccr & ~H8_C) | (c_ ? H8_C : 0));" % (opc, v)); return o
        if base == "bset": o.append("r_ |= 1u << %s;" % bit)
        if base == "bclr": o.append("r_ &= ~(1u << %s);" % bit)
        if base == "bnot": o.append("r_ ^= 1u << %s;" % bit)
        if base in ("bst", "bist"): o.append("r_ = (r_ & ~(1u << %s)) | (uint32_t)((%s(c->ccr & H8_C)) ? 1 : 0) << %s;" % (bit, "" if base == "bst" else "!", bit))
        o.append("(void)b_;"); wr(k2, 8, "r_", o); return o
    if base in CC:
        o.append("if (h8_cond(c, %d)) c->pc = 0x%XU;" % (CC[base], k1[1])); return o
    if base == "bsr":
        o.append("h8_push_pc(c, c->pc); c->pc = 0x%XU;" % k1[1]); return o
    if base in ("jmp", "jsr"):
        if k1[0] == "target": t = "0x%XU" % k1[1]
        elif k1[0] in ("r16", "r32", "ind"): t = "h8_ea(c, %d)" % k1[1]
        elif k1[0] == "ind8": t = ("(h8_rl(c, 0x%XU) & 0xFFFFFF)" if ADV else "h8_rw(c, 0x%XU)") % k1[1]
        else: raise Unsup(k1[0])
        o.append("{ const uint32_t t_ = %s;%s c->pc = t_; }" % (t, " h8_push_pc(c, c->pc);" if base == "jsr" else "")); return o
    if base == "rts": o.append("c->pc = h8_pop_pc(c);"); return o
    if base == "rte": o.append("h8_rte(c);"); return o
    if base == "trapa": o.append("h8_exception(c, %d, 0);" % (8 + k1[1])); return o
    if base == "sleep": o.append("c->sleeping = 1; c->budget = 0;"); return o
    if base in ("ldc", "stc"):
        if base == "ldc":
            src = k1; bb = 16 if m.endswith(".w") else 8
            v = rd(src, bb, o) if src[0] != "imm" else "0x%XU" % src[1]
            o.append("c->ccr = (uint8_t)(%s%s);" % (v, " >> 8" if bb == 16 else ""))
        else:
            bb = 16 if m.endswith(".w") else 8
            wr(k2, bb, "(uint32_t)c->ccr%s" % (" << 8 | c->ccr" if bb == 16 else ""), o)
        return o
    if base in ("andc", "orc", "xorc"):
        o.append("c->ccr = (uint8_t)(c->ccr %s 0x%XU);" % ({"andc": "&", "orc": "|", "xorc": "^"}[base], k1[1])); return o
    if base == "eepmov":
        if m.endswith(".b"): o.append("for (unsigned n = h8_r8(c, 12); n; n--) { h8_wb(c, h8_ea(c, 6), h8_rb(c, h8_ea(c, 5))); h8_ea_add(c, 5, 1); h8_ea_add(c, 6, 1); h8_w8(c, 12, (uint8_t)(n - 1)); c->budget -= 4; c->cycles += 4; }")
        else: o.append("for (unsigned n = h8_r16(c, 4); n; n--) { h8_wb(c, h8_ea(c, 6), h8_rb(c, h8_ea(c, 5))); h8_ea_add(c, 5, 1); h8_ea_add(c, 6, 1); h8_w16(c, 4, (uint16_t)(n - 1)); c->budget -= 4; c->cycles += 4; }")
        return o
    raise Unsup(m)

# EXECUTION TIME: MAME's states per instruction (tools/h8/h8_timing.txt, tools/h8/extract_timing.py) -- the reference the peripherals' timing
# (timers, serial frames) is gated against
TIMING = {}
for l in open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "h8_timing.txt")):
    if l.startswith("#") or not l.strip(): continue
    t = l.split()
    TIMING[(int(t[0], 16), int(t[1], 16), int(t[2]), t[3], t[4], t[5], t[6])] = int(t[7])
def states(i):
    o = i.w[1]
    if o.mnem.startswith("eepmov"): return 4      # + 4 per byte moved, charged in the loop
    return TIMING[(o.val, o.mask, o.skip, o.mnem, o.am1, o.am2, o.lvl)]

out = ["/* %s -- GENERATED by tools/h8/h8_translate.py from %s (%s mode): do not edit. */" % (os.path.basename(OUT), os.path.basename(sys.argv[1]), "advanced" if ADV else "normal"),
       '#include "h8_sem.h"', "void h8_untranslated(h8_cpu *c);", "void %s_run(h8_cpu *c)" % NAME, "{", "    while (c->budget > 0 && !c->sleeping) {",
       "        if (c->trace) c->trace(c->ctx, c->pc);", "        switch (c->pc) {"]
unsup = {}
for pc in sorted(code):
    i = code[pc]
    try: b = body(i)
    except Unsup as e:
        unsup[i.mnem] = unsup.get(i.mnem, 0) + 1
        b = ["c->pc = 0x%XU; h8_untranslated(c); return;   /* not translated: %s */" % (pc, e)]
    out.append("        case 0x%XU: { c->pc = 0x%XU; c->budget -= %d; c->cycles += %d;" % (pc, (pc + i.size) & AM, states(i), states(i)))
    out += ["            " + x for x in b]
    out.append("            break; }")
out += ["        default: h8_untranslated(c); return;", "        }", "    }", "}"]
open(OUT, "w").write("\n".join(out) + "\n")
print("%s: wrote %s%s" % (NAME, OUT, ("; NOT translated: " + str(unsup)) if unsup else ""), file=sys.stderr)
