"""snd_indirect.py -- code pointers kept in RAM by the M377xx sound programs, found statically (shared by tools/gen/snd_translate.py
and raverace/tools/gen/snd_translate.py; GitHub #62).

The C74 BIOS keeps per-voice and global hooks in RAM and calls them indirectly:
    LDM #$D8F2,$5A,X  (0x00D695)  ...  JMP ($005A,X)  (0x00D8EF)
    LDM #$BD6B,$01D6  (data ROM)  ...  PEA ret ; JMP ($01D6)
The static walk stops at an indirect jump, so a hook no scenario happened to run was left untranslated and TRAPPED -- the sound
stopped for good (Rave Racer, EXH*NOTE lap 3: "no translation for this address at 00D8F2 (M/X=0)").

A constant stored into a slot that a translated indirect jump reads is a target of that jump, at the M/X the jump runs with (a jump
keeps the flags). `PEA ret` directly before the jump is its return point: walked at the jump's M/X and at every M/X a known target
returns with. Stores are matched to jumps by slot and by indexing, so a data constant in an unrelated slot seeds nothing. A pointer
loaded from a table or computed at run time is not found here and still needs coverage. This only ADDS (address, M/X) cases: every
translated instruction is the ROM's own bytes, decoded exactly.
"""


def _word_store(pc, mx, source, decode):
    """(slot, indexed, value) if the instruction at pc stores a 16-bit constant in memory, else None"""
    b, _ = source(pc, 7)
    if b is None or mx & 2: return None                     # M set: a one-byte store cannot hold a pointer
    mn, ea, n, _pre = decode(b, mx)
    if mn == 'LDM':
        if ea == 'LDM4': return b[1], False, int.from_bytes(b[2:4], 'little')
        if ea == 'LDM4X': return b[1], True, int.from_bytes(b[2:4], 'little')
        if ea == 'LDM5': return int.from_bytes(b[1:3], 'little'), False, int.from_bytes(b[3:5], 'little')
        if ea == 'LDM5X': return int.from_bytes(b[1:3], 'little'), True, int.from_bytes(b[3:5], 'little')
        return None
    if mn == 'LDA' and ea == 'IMM' and b[0] not in (0x42, 0x89):        # LDA #imm ; STA slot
        v = int.from_bytes(b[1:3], 'little')
        nb, _ = source((pc & 0xFF0000) | ((pc + n) & 0xFFFF), 6)
        if nb is None: return None
        mn2, ea2, _n2, _p2 = decode(nb, mx)
        if mn2 != 'STA': return None
        if ea2 == 'D': return nb[1], False, v
        if ea2 == 'DX': return nb[1], True, v
        if ea2 == 'A': return int.from_bytes(nb[1:3], 'little'), False, v
        if ea2 == 'AX': return int.from_bytes(nb[1:3], 'little'), True, v
    return None


def indirect_seeds(ins, source, decode, exits=None, log=None):
    """(address, M/X) pairs the RAM code pointers stored and jumped through by the instructions in `ins` ({pc: {mx: len}}) reach.
    exits(target, mx, 'RTS') -> set of M/X at the target's returns (snd_exits.py), for the return points of `PEA ret ; JMP (slot)`."""
    stores, sites, pea = {}, [], {}
    for pc in list(ins):
        for mx in list(ins[pc]):
            b, _ = source(pc, 6)
            if b is None: continue
            mn, ea, n, _pre = decode(b, mx)
            s = _word_store(pc, mx, source, decode)
            if s: stores.setdefault(s[:2], set()).add(s[2])
            if mn == 'PEA': pea[((pc & 0xFF0000) | ((pc + n) & 0xFFFF), mx)] = (pc & 0xFF0000) | int.from_bytes(b[1:3], 'little')
            if mn in ('JMP', 'JSR') and ea in ('AI', 'AXI'):
                sites.append((pc, mx, int.from_bytes(b[1:3], 'little'), ea == 'AXI', mn))
    out = set()
    for pc, mx, slot, indexed, mn in sites:
        targets = sorted(stores.get((slot, indexed), ()))
        for v in targets:
            t = (pc & 0xFF0000) | v
            if source(t, 6)[0] is None: continue
            out.add((t, mx))
            if log: log(f'{mn} {"($%04X,X)" % slot if indexed else "($%04X)" % slot} at {pc:06X} mx={mx} -> {t:06X}')
        ret = pea.get((pc, mx))
        if ret is None or mn != 'JMP': continue
        out.add((ret, mx))
        if exits:
            for v in targets:
                t = (pc & 0xFF0000) | v
                if source(t, 6)[0] is None: continue
                out.update((ret, m) for m in exits(t, mx, 'RTS'))
    return out
