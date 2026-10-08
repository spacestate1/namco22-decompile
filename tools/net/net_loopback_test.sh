#!/bin/bash
# Online-play loopback test (docs/NETPLAY.md): two Rave Racer clients race through a server while, AT THE SAME TIME, a Tokyo
# Wars room (3 fake clients) and a Cyber Sled room (2 fake clients, tools/net/nmn_fake.py) exchange frames on that server.
# usage: net_loopback_test.sh OUTDIR [server-binary] [rr-A] [rr-B]; env PORT=27992 F=3600 FAKE=1 XARGS (extra server args)
#        REMOTE=host:port uses a running server instead of starting one.
R=$(cd "$(dirname "$0")/../.." && pwd)
OUT=$(realpath -m $1); SRV=${2:-$R/server/target/release/nmn-server}; RA=${3:-$R/raverace/build/rr}; RB=${4:-$RA}
PORT=${PORT:-27992}; F=${F:-3600}; FAKE=${FAKE:-1}
mkdir -p $OUT; rm -f $OUT/*.log
ADDR=${ADDR:-127.0.0.1:$PORT}
if [ -z "$REMOTE" ]; then $SRV --port $PORT --bind 127.0.0.1 ${XARGS---status-sec 10} > $OUT/srv.log 2>&1 & SP=$!; else ADDR=$REMOTE; SP=; fi
sleep 0.5
cd $R/raverace
E="RR_PACE=1 RR_NET_AUTOSTART=2 RR_LINK_DEBUG=1 RR_NET_DEBUG=1"
env $E RR_NET_SERVER=$ADDR RR_NET_NAME=Alice RR_NET_SAY="hi from alice" $RA extracted --frames $F --coin 300 --gas 600 > $OUT/a.log 2>&1 &
A=$!
env $E RR_NET_SERVER=$ADDR RR_NET_NAME=Bob RR_NET_SAY="hi from bob" $RB extracted --frames $F --coin 300 --gas 600 > $OUT/b.log 2>&1 &
B=$!
if [ "$FAKE" = 1 ]; then
  sleep 3
  python3 $R/tools/net/nmn_fake.py --server $ADDR --game tw --ver 3 --frame 100 --players 3 --seconds $((F / 60 - 10)) > $OUT/tw.log 2>&1 &
  T=$!
  python3 $R/tools/net/nmn_fake.py --server $ADDR --game cs --ver 1 --frame 64 --players 2 --seconds $((F / 60 - 10)) --name Sled > $OUT/cs.log 2>&1 &
  C=$!
  wait $T $C
fi
wait $A $B
[ -n "$SP" ] && kill $SP
python3 - $OUT <<'P'
import sys,re
o=sys.argv[1]
for p in ('a','b'):
    L=open(f'{o}/{p}.log',errors='replace').read().splitlines()
    rx=[int(m.group(1)) for l in L for m in [re.search(r'\[LINK\] tx \d+ +kick \d+ +rx (\d+)',l)] if m]
    go=[l for l in L if 'GO:' in l]
    chat=[l.split('chat: ')[1] for l in L if '[NET] chat:' in l]
    fr=[l for l in L if 'frames tx' in l]
    print(p, 'GO' if go else 'no GO', go[0].split('] ')[1] if go else '', '| link rx in last 10 s:', rx[-1]-rx[-11] if len(rx)>11 else rx, '| chat:', chat, '|', fr[-1].split('] ')[1] if fr else '')
P
cat $OUT/tw.log $OUT/cs.log 2>/dev/null | grep -E "PASS|FAIL|fake"
grep -E "room opened|race start|join slot|session over|status\]" $OUT/srv.log 2>/dev/null | head -30
