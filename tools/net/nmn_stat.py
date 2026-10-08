#!/usr/bin/env python3
"""nmn_stat.py -- what an nmn-server holds right now, per game: players, rooms, open rooms, and every room (docs/NETPLAY.md).

    tools/net/nmn_stat.py [host[:port]] [--games rr,ad,tw,cs,dd,tc,pc] [--json]

Uses only what the server answers to anyone (no slot needed): DISCOVER -> ANNOUNCE (server name, players / rooms / open rooms
of the asker's game) and ROOMS -> ROOMLIST (each room: id, link version, state, players, size, name). The ROOMS request is
padded to 400 bytes as the protocol asks of a stranger (no amplification). Default server: zonesync.net:27750.
"""
import socket, struct, sys, time, json

T_DISCOVER, T_ANNOUNCE, T_ROOMS, T_ROOMLIST = 0x0C, 0x0D, 0x0F, 0x10
NAMES = {'rr': 'Rave Racer', 'ad': 'Ace Driver', 'tw': 'Tokyo Wars', 'cs': 'Cyber Sled', 'dd': 'Dirt Dash', 'tc': 'Time Crisis',
         'pc': 'Prop Cycle'}

def msg(kind, game, payload=b'', ver=1):
    return b'NMN2' + struct.pack('<BBHH', kind, 0, 0, len(payload)) + game.encode() + struct.pack('<HH', ver, 0) + payload

def ask(sock, addr, game, timeout=1.5):
    sock.sendto(msg(T_DISCOVER, game), addr)
    sock.sendto(msg(T_ROOMS, game, bytes(400)), addr)
    ann, rooms, end = None, None, time.time() + timeout
    while time.time() < end and (ann is None or rooms is None):
        sock.settimeout(max(0.05, end - time.time()))
        try: d, _ = sock.recvfrom(4096)
        except socket.timeout: break
        if d[:4] != b'NMN2' or len(d) < 16 or d[10:12] != game.encode(): continue
        kind, (n,) = d[4], struct.unpack_from('<H', d, 8)
        p = d[16:16 + n]
        if kind == T_ANNOUNCE and p:
            ln = p[0]; name = p[1:1 + ln].decode(errors='replace')
            pl, rc, op = struct.unpack_from('<HHH', p, 1 + ln)
            ann = {'server': name, 'players': pl, 'rooms': rc, 'open_rooms': op}
        elif kind == T_ROOMLIST and p:
            rooms, o = [], 1
            for _ in range(p[0]):
                rid, ver = struct.unpack_from('<HH', p, o); st, pl, mx, mine, ln = p[o + 4:o + 9]
                rooms.append({'id': rid, 'ver': ver, 'state': 'playing' if st else 'lobby', 'players': pl, 'max': mx,
                              'name': p[o + 9:o + 9 + ln].decode(errors='replace')})
                o += 9 + ln
    return ann, rooms

def main():
    a = [x for x in sys.argv[1:] if not x.startswith('--')]
    games = list(NAMES)
    if '--games' in sys.argv: games = sys.argv[sys.argv.index('--games') + 1].split(',')
    if a and a[0] in games and '--games' in sys.argv: a = a[1:]
    host = a[0] if a else 'zonesync.net'
    h, _, p = host.partition(':')
    addr = (socket.gethostbyname(h), int(p or 27750))
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    out, srv = {}, None
    for g in games:
        ann, rooms = ask(s, addr, g)
        if ann: srv = ann['server']
        out[g] = {'announce': ann, 'rooms': rooms}
    if '--json' in sys.argv:
        print(json.dumps({'server': srv, 'address': f'{addr[0]}:{addr[1]}', 'games': out}, indent=1)); return
    if not any(v['announce'] for v in out.values()):
        print(f'{host}: no answer (server down, wrong port, or a firewall)'); sys.exit(1)
    print(f'{srv or "?"}  ({host} = {addr[0]}:{addr[1]})')
    tot = 0
    for g, v in out.items():
        an = v['announce']
        if not an: print(f'  {NAMES.get(g, g):12s} -- no answer'); continue
        tot += an['players']
        print(f'  {NAMES.get(g, g):12s} {an["players"]:3d} players  {an["rooms"]:3d} rooms ({an["open_rooms"]} open)')
        for r in v['rooms'] or []:
            print(f'      room {r["id"]:<4d} {r["name"][:28]:28s} {r["players"]}/{r["max"]}  {r["state"]:7s}  v{r["ver"]}')
    print(f'  total        {tot:3d} players')

if __name__ == '__main__':
    main()
