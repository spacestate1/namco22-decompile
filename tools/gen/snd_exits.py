"""snd_exits.py -- the M/X a subroutine RETURNS with, for the sound translators' static walk (GitHub #62 / #95).

The walk used to give a JSR's return point the CALLER's M/X. A callee that changes M or X before its RTS (Rave Racer's
0x00DE6F starts with CLM and returns with M clear) then leaves the code after the call translated only in the wrong state,
and the real driver traps there: "no translation for this M/X at 00D6AC (M/X=0) -- sound stopped".

exits(entry, mx) walks the callee from (entry, mx) -- following its own JSRs through their own summaries -- and returns the
set of M/X values at every RTS (JSR) / RTL (JSL) it can reach. A PLP restores what a PHP on the way saved (GitHub #95, Ridge
Racer 2: 0x00DB7C does CLM ... PHP ... PLP ... RTS, so its caller at 0x00DB5D resumes at 0x00DB60 with M clear: "no translation
for this M/X at 00DB60 (M/X=0)"). Paths that end where the state is unknown (a PLP with no PHP seen, an indirect jump) contribute
nothing; the caller still gets its own M/X as before, so this only ever ADDS return states.

    from snd_exits import Exits
    ex = Exits(source, decode, plain_successors)      # plain_successors(pc, mx): the walk's successors WITHOUT the JSR return
    ex(target, mx, 'RTS')                             # -> set of M/X at the callee's returns
"""

import os

NO_PLP = bool(os.environ.get('SND_NO_PLP'))     # SND_NO_PLP=1: a PLP ends the path, as before, for A/B
ENDS = {'RTS', 'RTL', 'RTI'}                     # instructions that end a routine (plp_seeds pairs nothing across one)

class Exits:
    def __init__(self, source, decode, plain, max_states=20000, max_depth=12):
        self.source, self.decode, self.plain = source, decode, plain
        self.memo, self.busy = {}, set()
        self.max_states, self.max_depth = max_states, max_depth

    def __call__(self, entry, mx, ret='RTS', depth=0):
        key = (entry, mx, ret)
        if key in self.memo: return self.memo[key]
        if key in self.busy or depth > self.max_depth: return set()      # recursion: what is known so far is enough
        self.busy.add(key)
        # `saved` is every M/X a PHP in this callee was reached with; a PLP continues in each of them. It is flow-insensitive
        # (an over-approximation, which only adds states), and it is a FIXPOINT: a PHP found after a PLP was already walked
        # re-feeds that PLP's successor with the new state, so the result does not depend on the order of the work list.
        out, seen, work, saved, plp_after = set(), {(entry, mx)}, [(entry, mx)], set(), set()
        while work and len(seen) < self.max_states:
            pc, m = work.pop()
            b, _ = self.source(pc, 6)
            if b is None: continue
            mn, ea, n, _pre = self.decode(b, m)
            if mn == ret: out.add(m); continue
            nxt = []
            if mn == 'PHP' and m not in saved and not NO_PLP:
                saved.add(m)
                nxt += [(a, m) for a in plp_after]          # PLPs already walked: they can restore this state too
            if mn == 'PLP' and not NO_PLP:
                after = (pc & 0xFF0000) | (((pc & 0xFFFF) + n) & 0xFFFF)
                plp_after.add(after)
                nxt = [(after, s) for s in saved]
            elif mn in ('JSR', 'JSL'):
                bank = pc & 0xFF0000
                tgt = None
                if ea == 'A': tgt = bank | int.from_bytes(b[1:3], 'little')
                if ea == 'AL': tgt = int.from_bytes(b[1:4], 'little')
                after = bank | (((pc & 0xFFFF) + n) & 0xFFFF)
                states = {m}
                if tgt is not None: states |= self(tgt, m, 'RTS' if mn == 'JSR' else 'RTL', depth + 1)
                nxt = [(after, s) for s in states]
            else:
                nxt += self.plain(pc, m)
            for t in nxt:
                if t not in seen: seen.add(t); work.append(t)
        self.busy.discard(key)
        self.memo[key] = out
        return out


def plp_seeds(ins, source, decode, window=0x100):
    """The static walk stops at a PLP. Seed the instruction after it in every M/X that a PHP of the SAME ROUTINE was translated
    with (`ins` = {pc: {mx: len}}); run to a fixpoint with the walk. "Same routine" is measured by address: the PHP lies at most
    `window` bytes before the PLP and no RTS/RTL/RTI that the translation knows of lies between them. A PLP with no such PHP stays
    unknown (the old behaviour). An over-approximation: an extra state is dead code, never a wrong one."""
    if NO_PLP: return set()
    php, plp, ends = [], [], set()
    for pc in list(ins):
        for mx in list(ins[pc]):
            b, _ = source(pc, 6)
            if b is None: continue
            mn, _ea, n, _pre = decode(b, mx)
            if mn == 'PHP': php.append((pc, mx))
            elif mn == 'PLP': plp.append(((pc & 0xFF0000) | (((pc & 0xFFFF) + n) & 0xFFFF), pc))
            elif mn in ENDS: ends.add(pc)
    ends = sorted(ends)
    import bisect
    def ended_between(lo, hi):                        # an RTS/RTL/RTI at an address in (lo, hi)?
        i = bisect.bisect_right(ends, lo)
        return i < len(ends) and ends[i] < hi
    return {(after, mx) for after, pc in plp for p, mx in php
            if 0 < pc - p <= window and not ended_between(p, pc)}


def flagset_seeds(ins, source, decode):
    """An instruction that sets BOTH M and X itself (CLP/SEP #imm with 0x30 in imm) has the same bytes and leaves the same state
    whatever M/X it is entered in, so translating it in every M/X is exact. It catches the entries no static analysis can see: an
    interrupt handler the 68K installs by writing a RAM vector (Rave Racer's uploaded handler at 0x00430C, `CLP #$3B`, reached
    through the BIOS trampoline at 0x00C6A9 in whatever M/X the driver was interrupted in). SND_NO_FLAGSET=1 turns it off."""
    if os.environ.get('SND_NO_FLAGSET'): return set()
    out = set()
    for pc in list(ins):
        for mx in list(ins[pc]):
            b, _ = source(pc, 6)
            if b is None: continue
            mn, ea, _n, _pre = decode(b, mx)
            if mn in ('CLP', 'SEP') and ea == 'IMM' and (b[1] & 0x30) == 0x30:
                out.update((pc, m) for m in range(4))
            break                                       # its decode does not depend on mx: one look per address
    return out
