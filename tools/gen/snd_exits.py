"""snd_exits.py -- the M/X a subroutine RETURNS with, for the sound translators' static walk (GitHub #62 / #95).

The walk used to give a JSR's return point the CALLER's M/X. A callee that changes M or X before its RTS (Rave Racer's
0x00DE6F starts with CLM and returns with M clear) then leaves the code after the call translated only in the wrong state,
and the real driver traps there: "no translation for this M/X at 00D6AC (M/X=0) -- sound stopped".

exits(entry, mx) walks the callee from (entry, mx) -- following its own JSRs through their own summaries -- and returns the
set of M/X values at every RTS (JSR) / RTL (JSL) it can reach. Paths that end where the state is unknown (PLP, an indirect
jump) contribute nothing; the caller still gets its own M/X as before, so this only ever ADDS return states.

    from snd_exits import Exits
    ex = Exits(source, decode, plain_successors)      # plain_successors(pc, mx): the walk's successors WITHOUT the JSR return
    ex(target, mx, 'RTS')                             # -> set of M/X at the callee's returns
"""

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
        out, seen, work = set(), {(entry, mx)}, [(entry, mx)]
        while work and len(seen) < self.max_states:
            pc, m = work.pop()
            b, _ = self.source(pc, 6)
            if b is None: continue
            mn, ea, n, _pre = self.decode(b, m)
            if mn == ret: out.add(m); continue
            nxt = []
            if mn in ('JSR', 'JSL'):
                bank = pc & 0xFF0000
                tgt = None
                if ea == 'A': tgt = bank | int.from_bytes(b[1:3], 'little')
                if ea == 'AL': tgt = int.from_bytes(b[1:4], 'little')
                after = bank | (((pc & 0xFFFF) + n) & 0xFFFF)
                states = {m}
                if tgt is not None: states |= self(tgt, m, 'RTS' if mn == 'JSR' else 'RTL', depth + 1)
                nxt = [(after, s) for s in states]
            else:
                nxt = self.plain(pc, m)
            for t in nxt:
                if t not in seen: seen.add(t); work.append(t)
        self.busy.discard(key)
        self.memo[key] = out
        return out
