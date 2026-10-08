#!/bin/bash
# Cyber Sled online through nmn-server: two headless `cs21` cabinets (coin + start + scripted play) linked over a server (loopback,
# or REMOTE=host:port). The game's own verdict is the C139 ring size (C139 RAM word 0x10 -> shared 0x900208), as the two cabinets
# hear each other. The slave's SCI handler (0x37D8) stores hop - 1 of our own packet coming home: 1 = linked to one other cabinet,
# 0 = alone/unplugged. usage: net_cs_test.sh OUTDIR [frames]; env LEAD=<n> (CS_NET_LEAD, 1000 = no stall-and-wait), PORT=27996,
# SLOW_B="1:15:1" slows cabinet B (ENG_SLOWFRAME), BIN=<cs21> (default cybsled/build_a/cs21), SHOTS=<every> (PPMs in OUTDIR/s0, s1),
# COIN/START/PLAY frames (default 1000/1100/1500: after GO, which restarts both boards with their POSITION).
R=$(cd "$(dirname "$0")/../.." && pwd); OUT=$(realpath -m $1); F=${2:-3000}; PORT=${PORT:-27996}
BIN=${BIN:-$R/cybsled/build_a/cs21}
mkdir -p $OUT/s0 $OUT/s1; rm -f $OUT/*.log
if [ -z "$REMOTE" ]; then $R/server/target/release/nmn-server --port $PORT --bind 127.0.0.1 --status-sec 10 > $OUT/srv.log 2>&1 & SP=$!; ADDR=127.0.0.1:$PORT; else ADDR=$REMOTE; SP=; fi
sleep 0.5; cd $R/cybsled
for i in 0 1; do
  S=""; [ $i = 1 ] && [ -n "$SLOW_B" ] && S="ENG_SLOWFRAME=$SLOW_B"
  env $S ${LEAD:+CS_NET_LEAD=$LEAD} CS_NET_SERVER=$ADDR CS_NET_NAME=Cab$i CS_NET_AUTOSTART=2 CS_NET_DEBUG=1 CS_LINKLOG=1 \
      $BIN extracted --frames $F --coin ${COIN:-1000} --start ${START:-1100} --play ${PLAY:-1500} ${SHOTS:+--shots $OUT/s$i $SHOTS} > $OUT/c$i.log 2>&1 &
  P[$i]=$!
done
rc=0
for i in 0 1; do wait ${P[$i]} || rc=1; done
[ -n "$SP" ] && kill $SP
for i in 0 1; do
  last=$(grep -E '^\[LINK\] f[0-9]+ online' $OUT/c$i.log | tail -1)
  echo "== cabinet $i: $(grep -c 'online: cabinet' $OUT/c$i.log) session(s); first linked (ring 1) at $(grep -m1 -oE 'f[0-9]+ ring 1' $OUT/c$i.log)"
  echo "   $last"
  echo "   unlinked (ring 0) lines after the first link: $(awk '/ ring 1 /{s=1} s&&/ ring 0 /{n++} END{print n+0}' $OUT/c$i.log), traps/timeouts: $(grep -ciE 'trap|stall timeout|gone' $OUT/c$i.log)"
done
grep -E "room opened|race start|session over" $OUT/srv.log 2>/dev/null
exit $rc
