#!/usr/bin/env python3
"""nmn_fake.py -- fake NMN2 clients of any game, for testing a server (docs/NETPLAY.md).

    python3 tools/net/nmn_fake.py --server 127.0.0.1:27750 --game tw --ver 3 --frame 100 --players 2 --seconds 60

Starts N clients in one process (one UDP socket each): the first CREATEs a room, the others JOIN it, everyone goes Ready, the
first sends START, and after GO every client sends one FRAME per 1/60 s whose payload is (slot, frame counter) repeated. Each
client checks everything it receives: the game id / version / room in the header, the session id, that a payload decodes to
another member's slot and a frame counter that only grows. Prints one line per client and exits 0 only when every client
received frames from every other client and saw no crosstalk.
"""
import argparse, socket, struct, sys, time

T_HELLO, T_WELCOME, T_ROSTER, T_READY, T_START, T_GO, T_LEAVE, T_FRAME, T_PING, T_PONG, T_ACK = range(1, 12)
T_CHAT, T_ROOMS, T_ROOMLIST = 0x0E, 0x0F, 0x10


class Client:
    def __init__(s, a, game, ver, fsize, name, idx):
        s.a, s.game, s.ver, s.fsize, s.name, s.idx = a, game, ver, fsize, name, idx
        s.sk = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.sk.setblocking(False)
        s.room = 0; s.slot = -1; s.sid = 0; s.state = 'connecting'
        s.seq = 1; s.hello_seq = 0; s.t_hello = 0; s.roster = []
        s.fseq = 0; s.rx = {}; s.last = {}; s.bad = []; s.tx = 0; s.t_ping = 0

    def msg(s, kind, seq, room, payload):
        return b'NMN2' + struct.pack('<BBHH', kind, 0, seq, len(payload)) + s.game + struct.pack('<HH', s.ver, room) + payload

    def send(s, kind, payload=b'', seq=0, room=None):
        s.sk.sendto(s.msg(kind, seq, s.room if room is None else room, payload), s.a)

    def hello(s, mode, room):
        n = s.name.encode()
        p = bytes([len(n)]) + n + bytes([mode, 8]) + struct.pack('<H', s.fsize) + b'\x00'
        p += b'\x00' * (128 - len(p))
        if not s.hello_seq:
            s.hello_seq = s.seq; s.seq += 1
        s.send(T_HELLO, p, s.hello_seq, room)
        s.t_hello = time.time()

    def roster_parse(s, p, off):
        n = p[off]; off += 1; r = []
        for _ in range(n):
            slot, rdy, nl = p[off], p[off + 1], p[off + 2]; r.append((slot, rdy, p[off + 3:off + 3 + nl].decode('utf-8', 'replace'))); off += 3 + nl
        s.roster = r

    def recv(s):
        while True:
            try:
                d, frm = s.sk.recvfrom(4096)
            except BlockingIOError:
                return
            if d[:4] != b'NMN2' or len(d) < 16:
                s.bad.append('not NMN2'); continue
            kind, _, seq, ln = struct.unpack('<BBHH', d[4:10])
            game, ver, room = d[10:12], struct.unpack('<H', d[12:14])[0], struct.unpack('<H', d[14:16])[0]
            p = d[16:]
            if ln != len(p) or game != s.game or ver != s.ver:
                s.bad.append('crosstalk: game %r ver %d kind %d' % (game, ver, kind)); continue
            if s.room and room != s.room and kind not in (T_ROOMLIST,):
                s.bad.append('crosstalk: room %d (mine %d) kind %d' % (room, s.room, kind)); continue
            if kind == T_WELCOME and seq == s.hello_seq and s.state == 'connecting':
                if p[0] == 0xFF:
                    s.bad.append('refused, reason %d' % p[6]); s.state = 'refused'; continue
                s.slot, s.room, s.state = p[0], room, 'lobby'
                s.roster_parse(p, 10)
            elif kind == T_ROSTER:
                s.roster_parse(p, 5)
            elif kind == T_GO:
                s.send(T_ACK, d[6:8])
                if s.state != 'session':
                    s.sid = struct.unpack('<I', p[:4])[0]; s.state = 'session'; s.roster_parse(p, 4)
            elif kind == T_FRAME:
                if len(p) != 9 + s.fsize:
                    s.bad.append('frame size %d' % len(p)); continue
                sid, fs, slot = struct.unpack('<IIB', p[:9])
                if sid != s.sid or slot == s.slot:
                    s.bad.append('frame session/slot'); continue
                pslot, pfs = struct.unpack('<HI', p[9:15])
                if pslot != slot or pfs != fs or p[9:] != (struct.pack('<HI', slot, fs) * (s.fsize // 6 + 1))[:s.fsize]:
                    s.bad.append('payload not from slot %d' % slot); continue
                if fs <= s.last.get(slot, -1):
                    continue
                s.last[slot] = fs; s.rx[slot] = s.rx.get(slot, 0) + 1

    def frame(s):
        pay = (struct.pack('<HI', s.slot, s.fseq) * (s.fsize // 6 + 1))[:s.fsize]
        s.send(T_FRAME, struct.pack('<IIB', s.sid, s.fseq, s.slot) + pay)
        s.fseq += 1; s.tx += 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--server', default='127.0.0.1:27750')
    ap.add_argument('--game', default='tw'); ap.add_argument('--ver', type=int, default=1)
    ap.add_argument('--frame', type=int, default=64); ap.add_argument('--players', type=int, default=2)
    ap.add_argument('--seconds', type=float, default=30); ap.add_argument('--name', default='Fake')
    a = ap.parse_args()
    host, port = a.server.rsplit(':', 1)
    addr = (socket.gethostbyname(host), int(port))
    cl = [Client(addr, a.game.encode(), a.ver, a.frame, '%s%d' % (a.name, i + 1), i) for i in range(a.players)]
    cl[0].hello(2, 0)
    t0 = time.time(); started = False; tick = 1 / 60; nxt = t0
    while time.time() - t0 < a.seconds:
        for c in cl:
            c.recv()
        now = time.time()
        for c in cl:
            if c.state == 'connecting' and now - c.t_hello > 0.5:
                if c.idx == 0: c.hello(2, 0)
                elif cl[0].room: c.hello(1, cl[0].room)
            if c.state in ('lobby', 'session') and now - c.t_ping > 1:
                c.send(T_PING, struct.pack('<I', int(now * 1000) & 0xFFFFFFFF)); c.t_ping = now
        lobby = [c for c in cl if c.state == 'lobby']
        if len(lobby) == len(cl) and len(cl[0].roster) == len(cl):
            for c in cl:
                if not any(r[0] == c.slot and r[1] for r in c.roster):
                    c.send(T_READY, b'\x01', c.seq); c.seq += 1
            if all(r[1] for r in cl[0].roster) and not started:
                cl[0].send(T_START, b'', cl[0].seq); cl[0].seq += 1; started = True
        if now >= nxt:
            nxt += tick
            for c in cl:
                if c.state == 'session':
                    c.frame()
        time.sleep(0.002)
    ok = True
    for c in cl:
        peers = [o.slot for o in cl if o is not c]
        got = all(c.rx.get(p, 0) > 0 for p in peers)
        print('[fake %s %s] room %d slot %d state %s session %08X  tx %d  rx %s  bad %d%s' % (
            a.game, c.name, c.room, c.slot, c.state, c.sid, c.tx, dict(sorted(c.rx.items())), len(c.bad),
            (' first: ' + c.bad[0]) if c.bad else ''))
        ok = ok and got and not c.bad
        c.send(T_LEAVE); c.send(T_LEAVE)
    print('[fake %s] %s' % (a.game, 'PASS' if ok else 'FAIL'))
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
