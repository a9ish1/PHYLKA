#!/bin/bash
# Peer discovery on the dev network: D only knows A (--connect); B and C also only know A. D must find B or C via addr gossip.
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
D=$BASE/disc
PA=$((PORT0+400)); PB=$((PORT0+410)); PC=$((PORT0+420)); PD=$((PORT0+430)); RA=$((PORT0+401)); RB=$((PORT0+411)); RC=$((PORT0+421)); RD=$((PORT0+431))
rm -rf $D; mkdir -p $D
PIDS=()
cleanup() { for p in "${PIDS[@]}"; do kill -9 $p 2>/dev/null; done; }
trap cleanup EXIT
fail() { echo "FAIL: $*"; exit 1; }
field() { curl -s --max-time 3 http://127.0.0.1:$1/info | awk -v k=$2 '$1==k{print $2}'; }
$B node --dev --port $PA --rpc $RA --datadir $D/a > $D/a.log 2>&1 & PIDS+=($!)
sleep 1
$B node --dev --port $PB --rpc $RB --datadir $D/b --connect 127.0.0.1:$PA > $D/b.log 2>&1 & PIDS+=($!)
$B node --dev --port $PC --rpc $RC --datadir $D/c --connect 127.0.0.1:$PA > $D/c.log 2>&1 & PIDS+=($!)
sleep 2
$B node --dev --port $PD --rpc $RD --datadir $D/d --connect 127.0.0.1:$PA > $D/d.log 2>&1 & DPID=$!; PIDS+=($DPID)
ok=0
for i in $(seq 1 40); do
  if [ "$(field $RD peers)" -ge 3 ] 2>/dev/null; then ok=1; break; fi
  sleep 1
done
echo "D peers=$(field $RD peers) (A plus B/C, which D was never told about; they may connect in either direction)"
grep "connected" $D/d.log
[ $ok = 1 ] || fail "D did not discover B or C"
echo "PASS D discovered a peer it was never told about via addr gossip"
kill -INT $DPID; wait $DPID; sleep 1
[ -s $D/d/peers.dat ] || fail "peers.dat not written"
echo "peers.dat of D:"; cat $D/d/peers.dat
echo "== restart D with no --connect at all: it must reconnect from its address book"
$B node --dev --port $PD --rpc $RD --datadir $D/d > $D/d2.log 2>&1 & PIDS+=($!)
for i in $(seq 1 30); do [ "$(field $RD peers)" -ge 1 ] 2>/dev/null && break; sleep 1; done
[ "$(field $RD peers)" -ge 1 ] 2>/dev/null || fail "D did not reconnect from peers.dat"
echo "PASS reconnected from peers.dat (peers=$(field $RD peers))"
echo "ALL PASS"
