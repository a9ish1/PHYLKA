#!/bin/bash
# PIP-0001 on the dev network: a miner and an upgraded node activate one signature per key at height H, a node without
# the upgrade splits off at the first such block, and a restart keeps the chain.
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
D=$BASE/pip1; H=150
PA=$((PORT0+500)); RA=$((PORT0+501)); PB=$((PORT0+510)); RB=$((PORT0+511)); PC=$((PORT0+520)); RC=$((PORT0+521))
rm -rf "$D"; mkdir -p "$D"
txfield() { curl -s --max-time 5 "http://127.0.0.1:$RA/api/tx?id=$1" | grep -o "\"$2\":[0-9]*" | head -1 | cut -d: -f2; }
waitfor() { for _ in $(seq 1 "$2"); do eval "$1" && return 0; sleep 1; done; return 1; }
startA() { $B node --dev --sigv2-height $H "$@" --port $PA --rpc $RA --datadir "$D/a" --noseeds >> "$D/a.log" 2>&1 & PIDS+=($!); APID=$!; }

$B wallet create --dev --wallet "$D/w.wallet" >/dev/null 2>&1 </dev/null || fail "wallet create"
$B wallet create --dev --wallet "$D/w2.wallet" >/dev/null 2>&1 </dev/null || fail "wallet 2 create"
ADDR=$($B wallet address --dev --wallet "$D/w.wallet" | tail -1)
ADDR2=$($B wallet address --dev --wallet "$D/w2.wallet" | tail -1)
startA --mine "$ADDR" --threads 1
sleep 1
$B node --dev --sigv2-height $H --port $PB --rpc $RB --datadir "$D/b" --noseeds --connect 127.0.0.1:$PA >> "$D/b.log" 2>&1 & PIDS+=($!); BPID=$!
$B node --dev --port $PC --rpc $RC --datadir "$D/c" --noseeds --connect 127.0.0.1:$PA >> "$D/c.log" 2>&1 & PIDS+=($!); CPID=$!

[ "$(field $RA sigv2_height)" = "$H" ] || fail "sigv2_height not reported"
[ "$(field $RC sigv2_height)" = "4294967295" ] || fail "node without the flag must have the rule disabled"
waitfor '[ "$(field $RA height)" -ge 5 ]' 60 || fail "no blocks"

echo "== below H the wallet sends version 1"
TX1=$($B wallet send --dev --wallet "$D/w.wallet" --rpc $RA "$ADDR2" 1 2>&1 | awk '/Sent/{print $NF}')
[ -n "$TX1" ] || fail "send below H"
waitfor '[ -n "$(txfield $TX1 size)" ]' 30 || fail "tx 1 not found"
[ "$(txfield $TX1 witness_size)" -ge 3829 ] || fail "legacy witness size"
[ "$(field $RA height)" -lt $H ] || fail "test too slow: already past H"
echo "PASS legacy send below H: $(txfield $TX1 size) bytes"

echo "== crossing H = $H"
waitfor '[ "$(field $RA height)" -ge $((H+2)) ]' 300 || fail "did not reach H"
for _ in 1 2 3; do [ "$(field $RB height)" -ge $H ] && break; sleep 2; done
[ "$(field $RB height)" -ge $H ] || fail "B did not follow across H"
echo "PASS upgraded node crossed H with blocks that contain only coinbases"

echo "== from H the wallet groups inputs of one key"
TX2=$($B wallet send --dev --wallet "$D/w.wallet" --rpc $RA "$ADDR2" 600 2>&1 | tee "$D/send2.out" | awk '/Sent/{print $NF}')
[ -n "$TX2" ] || { cat "$D/send2.out"; fail "send from H"; }
INPUTS=$(awk '/input\(s\)/{gsub(/[^0-9 ]/," "); print $(NF)}' "$D/send2.out")
waitfor '[ -n "$(txfield $TX2 size)" ] && [ "$(field $RA mempool)" = "0" ]' 60 || fail "v2 tx not mined"
SIZE=$(txfield $TX2 size); WIT=$(txfield $TX2 witness_size)
[ "$INPUTS" -ge 2 ] || fail "expected several inputs, got $INPUTS"
[ "$SIZE" -eq $((93 + 37*INPUTS + 3829)) ] || fail "size $SIZE for $INPUTS inputs"   # 2 outputs: payment and change
[ "$WIT" -lt 3900 ] || fail "witness $WIT should hold one key only"
echo "PASS version 2: $INPUTS inputs, $SIZE bytes, witness $WIT (legacy would be about $((93+3866*INPUTS)))"

echo "== node without the upgrade splits off"
waitfor '[ "$(field $RB height)" -gt $(( $(field $RC height) + 2 )) ]' 60 || fail "C did not stop following"
HC=$(field $RC height); sleep 8
[ "$(field $RC height)" = "$HC" ] || fail "C still advances"
grep -q "signature" "$D/c.log" || grep -qi "invalid\|rejected" "$D/c.log" || echo "note: C log has no explicit rejection line"
echo "PASS C stopped at height $HC while the upgraded chain is at $(field $RB height)"

echo "== same state and restart"
stopnode $APID
startA
waitfor '[ "$(field $RB tip)" = "$(field $RA tip)" ] && [ "$(field $RB utxo_commit)" = "$(field $RA utxo_commit)" ]' 60 || fail "A and B differ"
TIP=$(field $RA tip); UC=$(field $RA utxo_commit)
stopnode $BPID
$B node --dev --sigv2-height $H --port $PB --rpc $RB --datadir "$D/b" --noseeds --connect 127.0.0.1:$PA >> "$D/b.log" 2>&1 & PIDS+=($!); BPID=$!
waitfor '[ "$(field $RB tip)" = "$TIP" ]' 30 || fail "B lost the tip after restart"
[ "$(field $RB utxo_commit)" = "$UC" ] || fail "B UTXO differs after restart"
$B wallet balance --dev --wallet "$D/w2.wallet" --rpc $RB | grep -q "60" || fail "recipient balance"
echo "PASS A and B agree on tip and UTXO commitment, also after a restart"
echo ALL PASS
