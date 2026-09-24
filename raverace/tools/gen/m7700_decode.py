#!/usr/bin/env python3
"""Static 7700-family instruction decoding for the sound translator: opcode
tables parsed from MAME's m7700ds.cpp (the disassembler source, fetched from
mamedev/mame; the three 256-entry tables: base page, 0x42 page, 0x89 page) and
MAME's operand sizes per addressing mode. mx = (PS >> 4) & 3: bit 1 = M, bit 0 = X
(1 = 8-bit). Validate with: python3 tools/gen/m7700_decode.py cov/snd.cov ROMDIR"""
import re, os, sys
HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, 'm7700ds_tables.cpp.txt')        # MAME's m7700ds.cpp, verbatim

def _tables():
    t = open(SRC).read()
    out = []
    for name in ('s_opcodes[256]', 's_opcodes_prefix42[256]', 's_opcodes_prefix89[256]'):
        i = t.index('m7700_disassembler::' + name)
        body = t[i:t.index('};', i)]
        rows = re.findall(r'\{\s*op::(\w+)\s*,\s*(\w)\s*,\s*(\w+)\s*\}', body)
        assert len(rows) == 256, (name, len(rows))
        out.append(rows)
    return out
BASE, P42, P89 = _tables()

FIXED = dict(IMP=0, ACC=0, ACCB=0, RELB=1, RELW=2, PER=2, A=2, PEA=2, AI=2, AL=3, ALX=3,
             AX=2, AXI=2, AY=2, D=1, DI=1, PEI=1, DIY=1, DLI=1, DLIY=1, DX=1, DXI=1, DY=1,
             S=1, SIY=1, SIG=1, MVN=2, MVP=2)
WIDE = dict(IMM=(2, 1), BBCD=(4, 3), BBCA=(5, 4), LDM4=(3, 2), LDM5=(4, 3), LDM4X=(3, 2), LDM5X=(4, 3))

def decode(b, mx):
    """(mnemonic, ea, length, prefix) for the bytes b at an instruction start"""
    m_flag, x_flag = (mx >> 1) & 1, mx & 1
    n, pre = 1, None
    if b[0] == 0x42: pre, tab, op = 0x42, P42, b[1]; n = 2
    elif b[0] == 0x89: pre, tab, op = 0x89, P89, b[1]; n = 2
    else: tab, op = BASE, b[0]
    mn, fl, ea = tab[op]
    if ea in FIXED: n += FIXED[ea]
    else:
        wide = (fl == 'M' and not m_flag) or (fl == 'X' and not x_flag)
        n += WIDE[ea][0 if wide else 1]
    return mn, ea, n, pre

if __name__ == '__main__':
    sys.path.insert(0, HERE)
    from snd_rom import source
    bad = ok = 0
    for l in open(sys.argv[1]):
        pc, mx, ln = l.split(); pc, mx, ln = int(pc, 16), int(mx), int(ln)
        b, _ = source(pc, 6)
        mn, ea, n, pre = decode(b, mx)
        if n == ln: ok += 1
        else:
            bad += 1
            if bad <= 20: print(f'{pc:06X} mx={mx} measured {ln} decoded {n}: {mn} {ea} bytes {b.hex()}')
    print(f'lengths: {ok} agree, {bad} differ')
