"""m740_decode.py -- the Mitsubishi 740 family (M3745x, M5074x, M3800x ...) instruction table, for the shared 740 translator
(tools/gen/m740_translate.py) and its disassembly listings. One row per opcode: (mnemonic, mode, length). It describes the
instruction set exactly as the DEV oracle (cybsled/src/io/m740_sem.h) executes it: the bit instructions (BBS/BBC/SEB/CLB) by
their low five bits, the 740 additions (LDM, COM, RRF, TST, JSR zp, JSR \\$FFxx, JMP (zp), SET/CLT, STP/WIT, INC/DEC A),
the 6502-pattern NOPs, and 'KIL' for an undefined opcode (the oracle stops on it).
Modes: imp acc imm zp zpx zpy abs abx aby idx idy rel ind zind spec bitA bitzp bbA bbzp ldm."""

def _build():
    t = [None] * 256
    def s(op, mn, mode, n): t[op] = (mn, mode, n)
    g1 = {0x01: ('idx', 2), 0x05: ('zp', 2), 0x09: ('imm', 2), 0x0d: ('abs', 3), 0x11: ('idy', 2), 0x15: ('zpx', 2),
          0x19: ('aby', 3), 0x1d: ('abx', 3)}
    for base, mn in ((0x00, 'ORA'), (0x20, 'AND'), (0x40, 'EOR'), (0x60, 'ADC'), (0xa0, 'LDA'), (0xc0, 'CMP'), (0xe0, 'SBC'),
                     (0x80, 'STA')):
        for lo, (mode, n) in g1.items():
            s(base | lo, mn, mode, n)
    s(0x89, 'NOP', 'imm', 2)
    imp = {0x08: 'PHP', 0x18: 'CLC', 0x28: 'PLP', 0x38: 'SEC', 0x48: 'PHA', 0x58: 'CLI', 0x68: 'PLA', 0x78: 'SEI', 0x88: 'DEY',
           0x98: 'TYA', 0xa8: 'TAY', 0xb8: 'CLV', 0xc8: 'INY', 0xd8: 'CLD', 0xe8: 'INX', 0xf8: 'SED', 0x8a: 'TXA', 0x9a: 'TXS',
           0xaa: 'TAX', 0xba: 'TSX', 0xca: 'DEX', 0xea: 'NOP', 0x5a: 'NOP', 0x7a: 'NOP', 0xda: 'NOP', 0xfa: 'NOP', 0x1a: 'DEC',
           0x3a: 'INC', 0x12: 'CLT', 0x32: 'SET', 0x42: 'STP', 0xc2: 'WIT', 0x40: 'RTI', 0x60: 'RTS', 0x00: 'BRK'}
    for op, mn in imp.items(): s(op, mn, 'imp', 1)
    for op, mn in ((0x0a, 'ASL'), (0x2a, 'ROL'), (0x4a, 'LSR'), (0x6a, 'ROR')): s(op, mn, 'acc', 1)
    for op, mn in ((0x10, 'BPL'), (0x30, 'BMI'), (0x50, 'BVC'), (0x70, 'BVS'), (0x90, 'BCC'), (0xb0, 'BCS'), (0xd0, 'BNE'),
                   (0xf0, 'BEQ'), (0x80, 'BRA')): s(op, mn, 'rel', 2)
    s(0x20, 'JSR', 'abs', 3); s(0x22, 'JSR', 'spec', 2); s(0x02, 'JSR', 'zind', 2); s(0xb2, 'JMP', 'zind', 2)
    s(0x4c, 'JMP', 'abs', 3); s(0x6c, 'JMP', 'ind', 3); s(0x3c, 'LDM', 'ldm', 3)
    for op, mn, mode, n in ((0xa2, 'LDX', 'imm', 2), (0xa6, 'LDX', 'zp', 2), (0xae, 'LDX', 'abs', 3), (0xb6, 'LDX', 'zpy', 2),
                            (0xbe, 'LDX', 'aby', 3), (0xa0, 'LDY', 'imm', 2), (0xa4, 'LDY', 'zp', 2), (0xac, 'LDY', 'abs', 3),
                            (0xb4, 'LDY', 'zpx', 2), (0xbc, 'LDY', 'abx', 3), (0xe0, 'CPX', 'imm', 2), (0xe4, 'CPX', 'zp', 2),
                            (0xec, 'CPX', 'abs', 3), (0xc0, 'CPY', 'imm', 2), (0xc4, 'CPY', 'zp', 2), (0xcc, 'CPY', 'abs', 3),
                            (0x24, 'BIT', 'zp', 2), (0x2c, 'BIT', 'abs', 3), (0x86, 'STX', 'zp', 2), (0x8e, 'STX', 'abs', 3),
                            (0x96, 'STX', 'zpy', 2), (0x84, 'STY', 'zp', 2), (0x8c, 'STY', 'abs', 3), (0x94, 'STY', 'zpx', 2),
                            (0x44, 'COM', 'zp', 2), (0x82, 'RRF', 'zp', 2), (0x64, 'TST', 'zp', 2), (0x04, 'NOP', 'zp', 2),
                            (0x0c, 'NOP', 'abs', 3), (0xe2, 'NOP', 'imm', 2)):
        s(op, mn, mode, n)
    for base, mn in ((0x06, 'ASL'), (0x26, 'ROL'), (0x46, 'LSR'), (0x66, 'ROR'), (0xc6, 'DEC'), (0xe6, 'INC')):
        s(base, mn, 'zp', 2); s(base | 0x08, mn, 'abs', 3); s(base | 0x10, mn, 'zpx', 2); s(base | 0x18, mn, 'abx', 3)
    for op in (0x14, 0x34, 0x54, 0x74, 0xd4, 0xf4): s(op, 'NOP', 'zpx', 2)
    for op in (0x1c, 0x5c, 0x7c, 0xdc, 0xfc): s(op, 'NOP', 'abx', 3)
    # the bit instructions take precedence by their low five bits (as the oracle decodes them)
    for op in range(256):
        lo, b = op & 0x1f, op >> 5
        if lo in (0x03, 0x13): t[op] = ('BBS' if lo == 0x03 else 'BBC', 'bbA%d' % b, 2)
        elif lo in (0x07, 0x17): t[op] = ('BBS' if lo == 0x07 else 'BBC', 'bbzp%d' % b, 3)
        elif lo in (0x0b, 0x1b): t[op] = ('SEB' if lo == 0x0b else 'CLB', 'bitA%d' % b, 1)
        elif lo in (0x0f, 0x1f): t[op] = ('SEB' if lo == 0x0f else 'CLB', 'bitzp%d' % b, 2)
    for op in range(256):
        if t[op] is None: t[op] = ('KIL', 'imp', 1)
    return t

TABLE = _build()
COND = {'BPL', 'BMI', 'BVC', 'BVS', 'BCC', 'BCS', 'BNE', 'BEQ', 'BBS', 'BBC'}

def decode(b):
    """b: bytes at the instruction -> (mnemonic, mode, length)"""
    return TABLE[b[0]]

def branch_target(pc, b):
    """the taken target of a relative branch (rel, bbA, bbzp) at pc"""
    mn, mode, n = TABLE[b[0]]
    off = b[n - 1]
    off = off - 256 if off >= 128 else off
    return (pc + n + off) & 0xFFFF

def fmt(pc, b):
    mn, mode, n = TABLE[b[0]]
    o = b[1:n]
    w = lambda: o[0] | o[1] << 8
    if mode in ('imp',): return mn
    if mode == 'acc': return mn + ' A'
    if mode == 'imm': return '%s #$%02X' % (mn, o[0])
    if mode == 'zp': return '%s $%02X' % (mn, o[0])
    if mode == 'zpx': return '%s $%02X,X' % (mn, o[0])
    if mode == 'zpy': return '%s $%02X,Y' % (mn, o[0])
    if mode == 'abs': return '%s $%04X' % (mn, w())
    if mode == 'abx': return '%s $%04X,X' % (mn, w())
    if mode == 'aby': return '%s $%04X,Y' % (mn, w())
    if mode == 'idx': return '%s ($%02X,X)' % (mn, o[0])
    if mode == 'idy': return '%s ($%02X),Y' % (mn, o[0])
    if mode == 'rel': return '%s $%04X' % (mn, branch_target(pc, b))
    if mode == 'ind': return '%s ($%04X)' % (mn, w())
    if mode == 'zind': return '%s ($%02X)' % (mn, o[0])
    if mode == 'spec': return '%s \\$FF%02X' % (mn, o[0])
    if mode == 'ldm': return 'LDM #$%02X,$%02X' % (o[0], o[1])
    if mode.startswith('bitA'): return '%s %s,A' % (mn, mode[4:])
    if mode.startswith('bitzp'): return '%s %s,$%02X' % (mn, mode[5:], o[0])
    if mode.startswith('bbA'): return '%s %s,A,$%04X' % (mn, mode[3:], branch_target(pc, b))
    if mode.startswith('bbzp'): return '%s %s,$%02X,$%04X' % (mn, mode[4:], o[0], branch_target(pc, b))
    return mn
