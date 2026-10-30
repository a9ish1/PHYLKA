#!/bin/bash
# Dev-network finality test: 4 mining validators cross the activation height (--upgrade-height 250); certificates appear on chain;
# a fresh node derives finality from blocks alone; an injected equivocator (N4) is slashed. Dev network only, ports PLK_TEST_PORT+300..399.
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
D=$BASE/fin

UP=250
P=(0 $((PORT0+300)) $((PORT0+310)) $((PORT0+320)) $((PORT0+330)) $((PORT0+340))); R=(0 $((PORT0+301)) $((PORT0+311)) $((PORT0+321)) $((PORT0+331)) $((PORT0+341)))
rm -rf $D; mkdir -p $D
PIDS=()
cleanup() { for p in "${PIDS[@]}"; do kill -9 $p 2>/dev/null; done; }
trap cleanup EXIT
fail() { echo "FAIL: $*"; exit 1; }
field() { curl -s --max-time 3 http://127.0.0.1:$1/info | awk -v k=$2 '$1==k{print $2}'; }
for i in 1 2 3 4; do
  $B wallet create --dev --wallet $D/w$i.wallet >/dev/null 2>&1 || fail "wallet $i"
  A[$i]=$($B wallet address --dev --wallet $D/w$i.wallet 2>/dev/null | tail -1)
done
start() {  # i extra-args...
  local i=$1; shift
  $B node --dev --upgrade-height $UP --mine ${A[$i]} --threads 1 --validator --wallet $D/w$i.wallet --port ${P[$i]} --rpc ${R[$i]} --datadir $D/n$i "$@" > $D/n$i.log 2>&1 &
  PIDS+=($!); NPID[$i]=$!
}
start 1
sleep 1
start 2 --connect 127.0.0.1:${P[1]}
start 3 --connect 127.0.0.1:${P[1]}
start 4 --connect 127.0.0.1:${P[1]} --test-equivocate

echo "== stake 40 PLK from each wallet once its first coinbase is mature"
for i in 1 2 3 4; do
  ok=0
  for t in $(seq 1 60); do
    if $B wallet stake 40 --dev --wallet $D/w$i.wallet --rpc ${R[$i]} > $D/stake$i.out 2>&1 && ! grep -qi "error" $D/stake$i.out; then
      sa=$(grep "^  to:" $D/stake$i.out | awk '{print $2}')
      # with several racing miners the stake tx can be orphaned together with the coinbase it spends: confirm it, else stake again
      for w in $(seq 1 30); do
        if curl -s --max-time 3 "http://127.0.0.1:${R[1]}/utxos?addr=$sa" | awk '$7==2{f=1} END{exit !f}'; then ok=1; break; fi
        sleep 2
      done
      [ $ok = 1 ] && break
    fi
    sleep 3
  done
  [ $ok = 1 ] || { cat $D/stake$i.out; fail "stake $i"; }
done
echo "staked; height $(field ${R[1]} height)"

echo "== wait for height >= $((UP+40)) on all nodes"
for t in $(seq 1 900); do
  m=999999
  for i in 1 2 3 4; do h=$(field ${R[$i]} height); [ -n "$h" ] && [ "$h" -lt "$m" ] && m=$h; done
  [ "$m" -ge $((UP+40)) ] && break
  sleep 2
done
for i in 1 2 3 4; do echo "N$i height=$(field ${R[$i]} height) finalized=$(field ${R[$i]} finalized) chain_final=$(field ${R[$i]} chain_final) registered=$(field ${R[$i]} registered) slashed=$(field ${R[$i]} slashed)"; done
[ "$m" -ge $((UP+40)) ] || fail "chain did not reach height $((UP+40))"
# wait for slashing + a certificate after it
for t in $(seq 1 120); do
  s=$(field ${R[1]} slashed); cf=$(field ${R[1]} chain_final)
  [ "${s:-0}" -ge 1 ] && [ "${cf:-0}" -gt $UP ] && break
  sleep 2
done
for i in 1 2 3 4; do echo "N$i height=$(field ${R[$i]} height) finalized=$(field ${R[$i]} finalized) chain_final=$(field ${R[$i]} chain_final) registered=$(field ${R[$i]} registered) slashed=$(field ${R[$i]} slashed)"; done
[ "$(field ${R[1]} registered)" -ge 3 ] || fail "validators were not registered on chain"
[ "$(field ${R[1]} finalized)" -gt $UP ] || fail "no checkpoint finalized after the activation height"
pre=$(grep -ho "Finalized block #[0-9]*" $D/n1.log $D/n2.log $D/n3.log $D/n4.log | tr -dc "0-9\n" | awk -v u=$UP '$1<u' | wc -l)
[ "$pre" -ge 1 ] || fail "nothing was finalized below the activation height"
echo "PASS finalization continues across the activation height; registry populated"
[ "$(field ${R[1]} chain_final)" -gt $UP ] || fail "no certificate on chain"
certs=0; ev=0
H=$(field ${R[1]} height)
for h in $(seq $UP $H); do
  l=$(curl -s --max-time 3 "http://127.0.0.1:${R[1]}/block?height=$h" | grep '^ext')
  echo "$l" | grep -qv "cert_height 0" && echo "$l" | grep -q cert_height && certs=$((certs+1))
  echo "$l" | grep -q "evidence [1-9]" && ev=$((ev+1))
done
echo "blocks with a certificate: $certs, blocks with slashing evidence: $ev"
[ $certs -ge 1 ] || fail "no certificate block found"
echo "PASS certificates on chain"
[ "$(field ${R[1]} slashed)" -ge 1 ] && [ $ev -ge 1 ] || fail "equivocator not slashed"
echo "PASS injected equivocation slashed on chain (slashed=$(field ${R[1]} slashed), all nodes: $(for i in 1 2 3 4; do printf '%s ' $(field ${R[$i]} slashed); done))"

echo "== stop everything, restart N1 alone without mining"
for i in 1 2 3 4; do kill -INT ${NPID[$i]}; done
for i in 1 2 3 4; do wait ${NPID[$i]} || fail "node $i unclean exit"; done
PIDS=()
$B node --dev --upgrade-height $UP --port ${P[1]} --rpc ${R[1]} --datadir $D/n1 > $D/n1r.log 2>&1 & PIDS+=($!); N1=$!
sleep 3
H1=$(field ${R[1]} height); T1=$(field ${R[1]} tip); F1=$(field ${R[1]} finalized); C1=$(field ${R[1]} chain_final); U1=$(field ${R[1]} utxo_commit)
echo "N1: height=$H1 finalized=$F1 chain_final=$C1"
echo "== (a) offline fresh node from the blocks directory only (no votes, no finality.dat)"
mkdir -p $D/fresh; cp -R $D/n1/blocks $D/fresh/blocks
$B node --dev --upgrade-height $UP --port ${P[5]} --rpc ${R[5]} --datadir $D/fresh > $D/fresh.log 2>&1 & PIDS+=($!); FP=$!
sleep 4
echo "fresh(offline): height=$(field ${R[5]} height) finalized=$(field ${R[5]} finalized) chain_final=$(field ${R[5]} chain_final)"
[ "$(field ${R[5]} tip)" = "$T1" ] && [ "$(field ${R[5]} chain_final)" = "$C1" ] && [ "$(field ${R[5]} finalized)" = "$C1" ] && [ "$(field ${R[5]} utxo_commit)" = "$U1" ] || fail "fresh node differs (finalized from chain data alone must equal chain_final $C1)"
echo "PASS fresh node reports finalized height $C1 from chain data alone, same UTXO commitment"
kill -INT $FP; wait $FP
echo "== (b) fresh node syncing over P2P"
rm -rf $D/fresh2
$B node --dev --upgrade-height $UP --port ${P[5]} --rpc ${R[5]} --datadir $D/fresh2 --connect 127.0.0.1:${P[1]} > $D/fresh2.log 2>&1 & PIDS+=($!); FP=$!
for t in $(seq 1 120); do [ "$(field ${R[5]} tip)" = "$T1" ] && [ "$(field ${R[5]} finalized)" -ge "$C1" ] && break; sleep 1; done
echo "fresh(p2p): height=$(field ${R[5]} height) finalized=$(field ${R[5]} finalized) chain_final=$(field ${R[5]} chain_final)"
[ "$(field ${R[5]} tip)" = "$T1" ] && [ "$(field ${R[5]} chain_final)" = "$C1" ] && [ "$(field ${R[5]} finalized)" -ge "$C1" ] || fail "p2p fresh node mismatch"
echo "PASS fresh node synced over P2P: same tip, chain_final $C1, finalized $(field ${R[5]} finalized) (N1: $F1)"
kill -INT $FP; wait $FP; kill -INT $N1; wait $N1; PIDS=()
echo "ALL PASS"
