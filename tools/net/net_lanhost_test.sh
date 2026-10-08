#!/bin/bash
# Built-in LAN host test: host (rr binary $2, RR_NET_HOST=1) + a guest that FINDS it by LAN discovery (binary $3); $1 = out dir
R=$(cd "$(dirname "$0")/../.." && pwd); OUT=$(realpath -m $1); H=$2; G=$3; F=${F:-2400}
mkdir -p $OUT; rm -f $OUT/*.log; cd $R/raverace
E="RR_PACE=1 RR_NET_AUTOSTART=2 RR_LINK_DEBUG=1 RR_NET_DEBUG=1"
env $E RR_NET_HOST=1 RR_NET_NAME=Host RR_NET_SAY="hello from host" $H extracted --frames $F --coin 300 --gas 600 > $OUT/a.log 2>&1 &
sleep 3
env $E RR_NET_DISCOVER=1 RR_NET_NAME=Guest RR_NET_SAY="hello from guest" $G extracted --frames $F --coin 300 --gas 600 > $OUT/b.log 2>&1 &
wait
python3 - $OUT <<'P'
import sys,re
o=sys.argv[1]
for p in ('a','b'):
    L=open(f'{o}/{p}.log',errors='replace').read().splitlines()
    rx=[int(m.group(1)) for l in L for m in [re.search(r'\[LINK\] tx \d+ +kick \d+ +rx (\d+)',l)] if m]
    go=[l for l in L if 'GO:' in l]; chat=[l.split('chat: ')[1] for l in L if '[NET] chat:' in l]
    fnd=[l for l in L if '[NET] found' in l or 'joined' in l or 'NETD] hosting' in l]
    print(p, 'GO' if go else 'no GO', '| link rx in last 10 s:', rx[-1]-rx[-11] if len(rx)>11 else rx, '| chat:', chat, '|', fnd[:2])
P
