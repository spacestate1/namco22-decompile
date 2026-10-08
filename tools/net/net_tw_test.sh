#!/bin/bash
# Tokyo Wars online through nmn-server: two headless `tw --autoplay` cabinets linked over a server (loopback, or REMOTE=host:port).
# usage: net_tw_test.sh OUTDIR [frames]; env SLOW_B="1:15:1" slows cabinet B (ENG_SLOWFRAME), LEAD=<n> (TW_NET_LEAD, 1000 = no
# stall-and-wait), PORT=27997, PLAYERS=2..4.
R=$(cd "$(dirname "$0")/../.." && pwd); OUT=$(realpath -m $1); F=${2:-3000}; PORT=${PORT:-27997}; N=${PLAYERS:-2}
mkdir -p $OUT; rm -f $OUT/*.log
if [ -z "$REMOTE" ]; then $R/server/target/release/nmn-server --port $PORT --bind 127.0.0.1 --status-sec 10 > $OUT/srv.log 2>&1 & SP=$!; ADDR=127.0.0.1:$PORT; else ADDR=$REMOTE; SP=; fi
sleep 0.5; cd $R/tokyowar
for i in $(seq 0 $((N - 1))); do
  S=""; [ $i = 1 ] && [ -n "$SLOW_B" ] && S="ENG_SLOWFRAME=$SLOW_B"
  env $S ${LEAD:+TW_NET_LEAD=$LEAD} TW_NET_SERVER=$ADDR TW_NET_NAME=Cab$i TW_NET_AUTOSTART=$N TW_NET_DEBUG=1 TW_LINKDBG=1 \
      ./build/tw extracted --frames $F --autoplay > $OUT/c$i.log 2>&1 &
  P[$i]=$!
done
for i in $(seq 0 $((N - 1))); do wait ${P[$i]}; done
[ -n "$SP" ] && kill $SP
for i in $(seq 0 $((N - 1))); do
  echo "== cabinet $i: $(grep -c 'GO:' $OUT/c$i.log) GO, $(grep -E 'game:' $OUT/c$i.log | grep -c 'ok 1') game-lines with ok 1 of $(grep -c 'game:' $OUT/c$i.log)"
  grep -E "game:" $OUT/c$i.log | tail -1; grep -E "\[NET\] SESSION" $OUT/c$i.log | tail -1; grep -ciE "trap|stall timeout" $OUT/c$i.log
done
grep -E "room opened|race start|session over" $OUT/srv.log 2>/dev/null
