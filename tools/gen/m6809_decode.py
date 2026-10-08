"""m6809_decode.py -- MC6809 instruction decoder for tools/gen/m6809_translate.py (shared, game-independent).

decode(b) -> (mnemonic, mode, length, flow) for the instruction whose bytes start at b[0].

LENGTHS MIRROR THE SEMANTICS the translation executes (cybsled/src/snd/m6809_sem.h, m6809_exec): exactly the bytes
sem_fetch() consumes, including the oddities of that source -- extra 0x10/0x11 prefixes are skipped one byte each,
an illegal opcode fetches nothing more, an immediate-mode store (0x87, 0x8F, 0xC7, 0xCD, 0xCF) fetches nothing more,
page-2/3 opcodes outside the CMP/LD/ST columns fetch only their addressing bytes. A disagreement here would make
the translation read past its constant array, so the translator also checks this decoder against the instruction
lengths in MAME's own disassembly traces (--check-trace).

flow:  'next'              falls through
       ('br', t)           conditional: t and next
       ('jmp', t)          unconditional to t only
       ('call', t)         t and next (BSR / LBSR / JSR extended)
       'call?'             a call whose target is not static (JSR direct / indexed): next only
       'stop'              RTS / RTI / JMP direct|indexed / PULS|PULU PC / TFR|EXG to PC / illegal
"""

RMW = ['NEG', 'NEG*', 'XNC*', 'COM', 'LSR', 'LSR*', 'ROR', 'ASR', 'ASL', 'ROL', 'DEC', 'DEC*', 'INC', 'TST', 'JMP', 'CLR']
COL = ['SUB', 'CMP', 'SBC', 'SUBD', 'AND', 'BIT', 'LD', 'ST', 'EOR', 'ADC', 'OR', 'ADD', 'CMPX', 'JSR', 'LDX', 'STX']
COLB = ['SUB', 'CMP', 'SBC', 'ADDD', 'AND', 'BIT', 'LD', 'ST', 'EOR', 'ADC', 'OR', 'ADD', 'LDD', 'STD', 'LDU', 'STU']
BR = ['BRA', 'BRN', 'BHI', 'BLS', 'BCC', 'BCS', 'BNE', 'BEQ', 'BVC', 'BVS', 'BPL', 'BMI', 'BGE', 'BLT', 'BGT', 'BLE']
MISC = {0x12: 'NOP', 0x13: 'SYNC', 0x16: 'LBRA', 0x17: 'LBSR', 0x19: 'DAA', 0x1A: 'ORCC', 0x1C: 'ANDCC', 0x1D: 'SEX',
        0x1E: 'EXG', 0x1F: 'TFR', 0x30: 'LEAX', 0x31: 'LEAY', 0x32: 'LEAS', 0x33: 'LEAU', 0x34: 'PSHS', 0x35: 'PULS',
        0x36: 'PSHU', 0x37: 'PULU', 0x39: 'RTS', 0x3A: 'ABX', 0x3B: 'RTI', 0x3C: 'CWAI', 0x3D: 'MUL', 0x3F: 'SWI'}
MODES = ['imm', 'dir', 'idx', 'ext']


def idx_extra(pb):
    """bytes after the indexed postbyte (sem_indexed)"""
    if not pb & 0x80:
        return 0
    k = pb & 0x0F
    if k in (0x8, 0xC):
        return 1
    if k in (0x9, 0xD, 0xF):
        return 2
    return 0


def s8(v):
    return v - 256 if v >= 128 else v


def s16(v):
    return v - 65536 if v >= 32768 else v


def decode(b, pc=0):
    """b: at least 6 bytes from pc. Returns (mnemonic, mode, length, flow). Targets are 16-bit addresses."""
    op = b[0]
    n = 1
    if op in (0x10, 0x11):
        page = op
        op = b[n]; n += 1
        while op in (0x10, 0x11):              # extra prefixes: one byte each, ignored
            op = b[n]; n += 1
        if page == 0x10 and 0x20 <= op < 0x30:
            t = (pc + n + 2 + s16(b[n] << 8 | b[n + 1])) & 0xFFFF
            mn = 'L' + BR[op & 15]
            n += 2
            if op == 0x20: return mn, 'rel', n, ('jmp', t)
            if op == 0x21: return mn, 'rel', n, 'next'
            return mn, 'rel', n, ('br', t)
        if op == 0x3F:
            return ('SWI2' if page == 0x10 else 'SWI3'), 'inh', n, 'next'
        mode = (op >> 4) & 3
        if mode == 1: n += 1
        elif mode == 2: n += 1 + idx_extra(b[n])
        elif mode == 3: n += 2
        m = op & 0xCF
        if page == 0x10:
            names = {0x83: 'CMPD', 0x8C: 'CMPY', 0x8E: 'LDY', 0xCE: 'LDS', 0x8F: 'STY', 0xCF: 'STS'}
            if mode == 0 and m in (0x83, 0x8C, 0x8E, 0xCE): n += 2
        else:
            names = {0x83: 'CMPU', 0x8C: 'CMPS'}
            if mode == 0 and m in (0x83, 0x8C): n += 2
        if m not in names:
            return 'ILL', MODES[mode], n, 'stop'
        return names[m], MODES[mode], n, 'next'

    if op < 0x10 or 0x40 <= op < 0x80:
        hi = op >> 4
        mn = RMW[op & 15]
        if hi in (4, 5):
            return (mn + ('A' if hi == 4 else 'B')), 'inh', 1, 'next'
        if hi == 0: n, mode = 2, 'dir'
        elif hi == 6: n, mode = 2 + idx_extra(b[1]), 'idx'
        else: n, mode = 3, 'ext'
        if (op & 15) == 0xE:
            if mode == 'ext': return 'JMP', mode, n, ('jmp', b[1] << 8 | b[2])
            return 'JMP', mode, n, 'stop'
        return mn, mode, n, 'next'

    if op >= 0x80:
        mode = (op >> 4) & 3
        isb = op & 0x40
        lo = op & 15
        mn = (COLB if isb else COL)[lo]
        if lo in (0x0, 0x1, 0x2, 0x4, 0x5, 0x6, 0x7, 0x8, 0x9, 0xA, 0xB):
            mn += 'B' if isb else 'A'
        if mode == 0:
            if lo in (0x3, 0xC, 0xE): return mn, 'imm', 3, 'next'
            if lo == 0xD:
                if isb: return 'ILL', 'imm', 1, 'stop'
                return 'BSR', 'rel', 2, ('call', (pc + 2 + s8(b[1])) & 0xFFFF)
            if lo in (0x7, 0xF): return 'ILL', 'imm', 1, 'stop'
            return mn, 'imm', 2, 'next'
        if mode == 1: n = 2
        elif mode == 2: n = 2 + idx_extra(b[1])
        else: n = 3
        if lo == 0xD and not isb:
            if mode == 3: return 'JSR', 'ext', n, ('call', b[1] << 8 | b[2])
            return 'JSR', MODES[mode], n, 'call?'
        return mn, MODES[mode], n, 'next'

    if 0x20 <= op < 0x30:
        t = (pc + 2 + s8(b[1])) & 0xFFFF
        if op == 0x20: return 'BRA', 'rel', 2, ('jmp', t)
        if op == 0x21: return 'BRN', 'rel', 2, 'next'
        return BR[op & 15], 'rel', 2, ('br', t)
    if op == 0x16: return 'LBRA', 'rel', 3, ('jmp', (pc + 3 + s16(b[1] << 8 | b[2])) & 0xFFFF)
    if op == 0x17: return 'LBSR', 'rel', 3, ('call', (pc + 3 + s16(b[1] << 8 | b[2])) & 0xFFFF)
    if op in (0x1A, 0x1C, 0x3C): return MISC[op], 'imm', 2, 'next'
    if op in (0x1E, 0x1F):
        return MISC[op], 'imm', 2, ('stop' if (b[1] & 15) == 5 or (op == 0x1E and (b[1] >> 4) == 5) else 'next')
    if 0x30 <= op <= 0x33: return MISC[op], 'idx', 2 + idx_extra(b[1]), 'next'
    if op in (0x34, 0x36): return MISC[op], 'imm', 2, 'next'
    if op in (0x35, 0x37): return MISC[op], 'imm', 2, ('stop' if b[1] & 0x80 else 'next')
    if op in (0x39, 0x3B): return MISC[op], 'inh', 1, 'stop'
    if op in MISC: return MISC[op], 'inh', 1, 'next'
    return 'ILL', 'inh', 1, 'stop'


def disasm(b, pc):
    mn, mode, n, flow = decode(b, pc)
    raw = ' '.join(f'{x:02X}' for x in b[:n])
    t = ''
    if isinstance(flow, tuple): t = f' ${flow[1]:04X}'
    elif mode == 'ext': t = f' ${b[n - 2] << 8 | b[n - 1]:04X}'
    elif mode == 'dir': t = f' <${b[n - 1]:02X}'
    elif mode == 'imm' and n >= 2:
        k = 1
        while b[k - 1] in (0x10, 0x11) and k < n: k += 1
        if n > k: t = ' #$' + ''.join(f'{x:02X}' for x in b[k:n])
    elif mode == 'idx': t = ' ,idx'
    return f'{raw:<14} {mn}{t}'
