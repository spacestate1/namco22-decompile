"""PUSH-AND-RTS DISPATCH TABLES of the M377xx sound programs, found statically (shared by tools/gen/snd_translate.py and
raverace/tools/gen/snd_translate.py).

The C74 BIOS (Rave Racer, Ace Driver) runs a small byte-code interpreter whose handlers are reached by
    ADC/ADCB #table ; ... load the word at the result ; PHA/PHB ; RTS
(0xCE43 / 0xCE4E with the table at 0xCE5B, 0xCEEA / 0xCEF2 with the table at 0xCEF4). The static walk cannot follow an RTS, so a
handler no scenario happened to run was left untranslated and TRAPPED -- the sound stopped for good (GitHub #62, Rave Racer lap 3:
"no translation for this address at 00CECC (M/X=0)", the table's AND handler). Each table lies directly before its handlers, so it
ends where the first handler begins; every entry is seeded at the M/X the dispatching RTS runs with (an RTS keeps the flags).
This only ADDS (address, M/X) cases: every translated instruction is the ROM's own bytes, decoded exactly.
"""

FLOW_END = {'RTS', 'RTL', 'RTI', 'STP', 'BRK', 'COP', 'UNK', 'PLP', 'PUL', 'WDM', 'JMP', 'JML', 'BRA', 'BRL',
            'BPL', 'BMI', 'BVC', 'BVS', 'BCC', 'BCS', 'BNE', 'BEQ', 'BBC', 'BBS', 'JSR', 'JSL'}


def _mx_after(mn, b, mx):
    if mn == 'SEM': return mx | 2
    if mn == 'CLM': return mx & 1
    if mn in ('SEP', 'CLP'):
        k = (b[1] >> 4) & 3
        return (mx | k) if mn == 'SEP' else (mx & ~k & 3)
    return mx


def dispatch_seeds(ins, source, decode, log=None):
    """(address, M/X) pairs that push-and-RTS tables reached from the instructions in `ins` ({pc: {mx: len}}) dispatch to"""
    out = set()
    for pc in list(ins):
        for mx in list(ins[pc]):
            b, ram = source(pc, 6)
            if b is None: continue
            mn, ea, n, pre = decode(b, mx)
            if mn not in ('ADC', 'ADCB') or ea != 'IMM' or n - (1 if b[0] in (0x42, 0x89) else 0) != 3: continue   # a 16-bit immediate
            bank = pc & 0xFF0000
            table = bank | int.from_bytes(b[n - 2:n], 'little')
            tb, tram = source(table, 2)
            if tb is None or tram: continue                     # a RAM table (e.g. 0x5100): filled at run time, needs coverage
            # within the next few instructions, straight-line: a push, then the RTS
            p, m, pushed, rts_mx = bank | ((pc & 0xFFFF) + n), mx, False, None
            for _ in range(10):
                bb, _ = source(p, 6)
                if bb is None: break
                mn2, ea2, n2, _p = decode(bb, m)
                if mn2 in ('PHA', 'PHB'): pushed = True
                if mn2 == 'RTS':
                    if pushed: rts_mx = m
                    break
                if mn2 in FLOW_END: break
                m = _mx_after(mn2, bb, m)
                p = bank | ((p & 0xFFFF) + n2)
            if rts_mx is None: continue
            entries, lim, a = [], None, table
            while len(entries) < 64 and (lim is None or a < lim):
                wb, wram = source(a, 2)
                if wb is None or wram: break
                t = bank | int.from_bytes(wb, 'little')
                tb2, tram2 = source(t, 6)
                if tb2 is None or tram2: break
                entries.append(t)
                if t > table and (lim is None or t < lim): lim = t
                a += 2
            if lim is None: continue                            # no handler after the table: not this idiom
            for t in entries:
                out.add((t, rts_mx))
            if log: log(f'dispatch table {table:06X} (from {pc:06X} mx={mx}): {len(entries)} entries at M/X {rts_mx}')
    return out
