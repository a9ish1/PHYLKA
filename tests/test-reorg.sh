#!/bin/bash
# Reorg test on the dev network: two unconnected miners, then connect; the loser reorgs via undo data, restarts consistently.
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
D=$BASE/reorg

PA=$((PORT0+200)); RA=$((PORT0+201)); PB=$((PORT0+210)); RB=$((PORT0+211))
rm -rf $D; mkdir -p $D
PIDS=()
cleanup() { for p in "${PIDS[@]}"; do kill -9 $p 2>/dev/null; done; }
trap cleanup EXIT
fail() { echo "FAIL: $*"; exit 1; }
info() { curl -s --max-time 3 http://127.0.0.1:$1/info; }
field() { info $1 | awk -v k=$2 '$1==k{print $2}'; }
stopnode() { kill -INT $1; for i in $(seq 1 50); do kill -0 $1 2>/dev/null || return 0; sleep 0.2; done; fail "node did not stop"; }
bal() { $B wallet balance --dev --wallet $1 --rpc $2 2>&1; }

$B wallet create --dev --wallet $D/wa.wallet >/dev/null 2>&1 || fail "wallet a"
$B wallet create --dev --wallet $D/wb.wallet >/dev/null 2>&1 || fail "wallet b"
AA=$($B wallet address --dev --wallet $D/wa.wallet 2>/dev/null | tail -1)
AB=$($B wallet address --dev --wallet $D/wb.wallet 2>/dev/null | tail -1)

echo "== phase 1: A and B mine separately (not connected)"
$B node --dev --mine $AB --threads 1 --port $PB --rpc $RB --datadir $D/b > $D/b1.log 2>&1 & PIDS+=($!); BPID=$!
sleep 25   # B gets a head start so that it will have more work than A
$B node --dev --mine $AA --threads 1 --port $PA --rpc $RA --datadir $D/a > $D/a1.log 2>&1 & PIDS+=($!); APID=$!
for i in $(seq 1 90); do [ "$(field $RA height)" -ge 12 ] 2>/dev/null && break; sleep 1; done
echo "A height $(field $RA height) B height $(field $RB height); peers A=$(field $RA peers) B=$(field $RB peers)"
[ "$(field $RA peers)" = "0" ] && [ "$(field $RB peers)" = "0" ] || fail "nodes must not be connected"
$B wallet send --dev --wallet $D/wa.wallet --rpc $RA $AB 1 2>&1 | tail -2
for i in $(seq 1 60); do [ "$(field $RA mempool)" = "0" ] && break; sleep 1; done
[ "$(field $RA mempool)" = "0" ] || fail "tx on A was not mined"
echo "A: tx confirmed. A balance of wallet B (pays 1 PLK to B):"; bal $D/wb.wallet $RA | head -1
sleep 5
stopnode $APID; stopnode $BPID; PIDS=()
echo "== phase 2: restart without mining; B connects to A"
$B node --dev --port $PA --rpc $RA --datadir $D/a > $D/a2.log 2>&1 & PIDS+=($!); APID=$!
$B node --dev --port $PB --rpc $RB --datadir $D/b > $D/b2.log 2>&1 & PIDS+=($!); BPID=$!
sleep 2
WA=$(field $RA chainwork); WB=$(field $RB chainwork); HA=$(field $RA height); HB=$(field $RB height); TA=$(field $RA tip); TB=$(field $RB tip)
echo "A: height $HA work $WA"; echo "B: height $HB work $WB"
[ "$TA" != "$TB" ] || fail "tips already equal"
if [[ "$WB" > "$WA" ]]; then WIN=B; WTIP=$TB; else WIN=A; WTIP=$TA; fi
echo "expected winner: $WIN ($WTIP)"
stopnode $BPID; PIDS=($APID)
$B node --dev --port $PB --rpc $RB --datadir $D/b --connect 127.0.0.1:$PA > $D/b3.log 2>&1 & PIDS+=($!); BPID=$!
for i in $(seq 1 60); do [ "$(field $RA tip)" = "$WTIP" ] && [ "$(field $RB tip)" = "$WTIP" ] && break; sleep 1; done
[ "$(field $RA tip)" = "$WTIP" ] && [ "$(field $RB tip)" = "$WTIP" ] || fail "tips did not converge to the heavier chain"
echo "PASS converged on $WIN's chain: height $(field $RA height), mempool A=$(field $RA mempool) B=$(field $RB mempool)"
grep -iE "reorg" $D/a3.log $D/a2.log $D/b3.log 2>/dev/null | tail -3
sleep 2
for w in wa wb; do
  x=$(bal $D/$w.wallet $RA); y=$(bal $D/$w.wallet $RB)
  [ "$x" = "$y" ] || fail "balance of $w differs between nodes: [$x] vs [$y]"
  echo "balance $w identical on both nodes:"; echo "$x" | head -2
done
for a in $AA $AB; do
  [ "$(curl -s http://127.0.0.1:$RA/utxos?addr=$a | sort)" = "$(curl -s http://127.0.0.1:$RB/utxos?addr=$a | sort)" ] || fail "utxo list differs for $a"
done
echo "PASS balances + UTXO lists consistent"
SNAPA=$(for a in $AA $AB; do curl -s http://127.0.0.1:$RA/utxos?addr=$a | sort; done | md5)
BALA=$(bal $D/wa.wallet $RA; bal $D/wb.wallet $RA)
echo "== phase 3: restart the node that had to reorganise (and the other one)"
LOSER=A; [ "$WIN" = "A" ] && LOSER=B
if [ "$LOSER" = "A" ]; then LP=$PA; LR=$RA; LD=$D/a; LPID=$APID; else LP=$PB; LR=$RB; LD=$D/b; LPID=$BPID; fi
echo "loser=$LOSER; undo files: $(ls $LD/undo | wc -l)"
stopnode $LPID; PIDS=($APID $BPID)   # the other node stays under cleanup
$B node --dev --port $LP --rpc $LR --datadir $LD > $D/restart.log 2>&1 & PIDS+=($!)
sleep 3
[ "$(field $LR tip)" = "$WTIP" ] || fail "tip after restart differs: $(field $LR tip)"
SNAP2=$(for a in $AA $AB; do curl -s http://127.0.0.1:$LR/utxos?addr=$a | sort; done | md5)
BAL2=$(bal $D/wa.wallet $LR; bal $D/wb.wallet $LR)
[ "$SNAPA" = "$SNAP2" ] && [ "$BALA" = "$BAL2" ] || fail "UTXO/balances differ after restart"
echo "PASS restart: same tip $WTIP, same UTXO and balances"
echo "ALL PASS"
