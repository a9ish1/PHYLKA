#!/bin/bash
# Split-brain test on the dev network (6 validators). Stakes: N1-N4 40 PLK each, N5/N6 20 PLK each (total 200).
#  Scenario 1: {N1..N4} (80% of stake) vs {N5,N6} (20%); the minority mines with MORE hash power. The majority keeps finalizing,
#              the minority cannot; after rejoin everybody must follow the majority chain (finality beats work).
#  Scenario 2: {N1,N2,N3} (60%) vs {N4,N5,N6} (40%): nobody has > 2/3, nobody finalizes; after rejoin they converge by
#              work and finality resumes.
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
D=$BASE/splitbrain
UP=60
N=6
STAKE=(0 40 40 40 40 20 20)
P=(); R=()
for i in $(seq 1 $N); do P[$i]=$((PORT0+100+i*10)); R[$i]=$((PORT0+101+i*10)); done
[ -n "$PLK_REUSE_SNAP" ] && [ -d "$D/snap" ] || { rm -rf "$D"; mkdir -p "$D"; }   # PLK_REUSE_SNAP=1: skip the (slow) setup of an earlier run
NPID=()

# start i mining_threads [connect idx...]
startn() {
  local i=$1 th=$2; shift 2
  local args=(--dev --upgrade-height $UP --validator --wallet "$D/w$i.wallet" --port ${P[$i]} --rpc ${R[$i]} --datadir "$D/n$i")
  [ "$th" -gt 0 ] && args+=(--mine "${A[$i]}" --threads "$th")
  for c in "$@"; do args+=(--connect 127.0.0.1:${P[$c]}); done
  "$B" node "${args[@]}" >> "$D/n$i.log" 2>&1 &
  NPID[$i]=$!; PIDS+=($!)
}
stopall() { for i in $(seq 1 $N); do [ -n "${NPID[$i]}" ] && kill -INT "${NPID[$i]}" 2>/dev/null; done
            for i in $(seq 1 $N); do [ -n "${NPID[$i]}" ] && { stopnode "${NPID[$i]}"; NPID[$i]=; }; done; PIDS=(); }
maxfin() { local m=0 x; for i in "$@"; do x=$(field ${R[$i]} finalized); [ "${x:-0}" -gt "$m" ] && m=$x; done; echo $m; }
minfin() { local m=999999999 x; for i in "$@"; do x=$(field ${R[$i]} finalized); [ "${x:-0}" -lt "$m" ] && m=${x:-0}; done; echo $m; }
minh() { local m=999999999 x; for i in "$@"; do x=$(field ${R[$i]} height); [ "${x:-0}" -lt "$m" ] && m=${x:-0}; done; echo $m; }
maxh() { local m=0 x; for i in "$@"; do x=$(field ${R[$i]} height); [ "${x:-0}" -gt "$m" ] && m=$x; done; echo $m; }
bhash() { curl -s --max-time 3 "http://127.0.0.1:$1/block?height=$2" | awk '$1=="hash"{print $2}'; }
# best (highest chainwork) of a node group: prints "work tip height"
bestwork() { local bw="" bt="" bh=""; for i in "$@"; do w=$(field ${R[$i]} chainwork); if [[ "$w" > "$bw" ]]; then bw=$w; bt=$(field ${R[$i]} tip); bh=$(field ${R[$i]} height); fi; done; echo "$bw $bt $bh"; }
status() { for i in $(seq 1 $N); do echo "  N$i height=$(field ${R[$i]} height) finalized=$(field ${R[$i]} finalized) chain_final=$(field ${R[$i]} chain_final) registered=$(field ${R[$i]} registered) peers=$(field ${R[$i]} peers)"; done; }
converged() {  # waits until all nodes have identical tip + utxo_commit; $1 = timeout seconds
  for _ in $(seq 1 "$1"); do
    t0=$(field ${R[1]} tip); u0=$(field ${R[1]} utxo_commit); ok=1
    for i in $(seq 2 $N); do [ "$(field ${R[$i]} tip)" = "$t0" ] && [ "$(field ${R[$i]} utxo_commit)" = "$u0" ] || { ok=0; break; }; done
    [ -n "$t0" ] && [ $ok = 1 ] && return 0
    sleep 1
  done
  return 1
}

# ---------- setup: 6 nodes, all mining and validating, stakes registered, finality running ----------
A=()
for i in $(seq 1 $N); do
  [ -f "$D/w$i.wallet" ] || "$B" wallet create --dev --wallet "$D/w$i.wallet" >/dev/null 2>&1 || fail "wallet $i"
  A[$i]=$("$B" wallet address --dev --wallet "$D/w$i.wallet" 2>/dev/null | tail -1)
done
if [ ! -d "$D/snap" ]; then
echo "== setup: N1 mines alone (a linear chain: with several racing miners a lone early staker could finalize an orphaned branch), then funds N2..N6"
startn 1 2
sleep 1
for i in 2 3 4 5 6; do startn $i 0 $((i-1)); sleep 1; done   # chain topology: the per-IP inbound cap (4) would refuse a 5-node star on loopback
for _ in $(seq 1 120); do [ "$(field ${R[1]} height)" -ge 12 ] 2>/dev/null && break; sleep 1; done
for i in 2 3 4 5 6; do
  ok=0
  for _ in $(seq 1 60); do
    if [ "$(field ${R[1]} mempool)" = "0" ] && "$B" wallet send --dev --wallet "$D/w1.wallet" --rpc ${R[1]} "${A[$i]}" $((STAKE[$i]+5)) > "$D/send$i.out" 2>&1 && ! grep -qi "error" "$D/send$i.out"; then ok=1; break; fi
    sleep 2
  done
  [ $ok = 1 ] || { cat "$D/send$i.out"; fail "funding N$i"; }
done
# Stake; re-issued until the stake output is on the chain (a stake tx can wait for the funding tx to confirm, or be dropped if its
# input was reorganised away).
SA=()
has_stake() { curl -s --max-time 3 "http://127.0.0.1:${R[1]}/utxos?addr=${SA[$1]}" | awk '$7==2{f=1} END{exit !f}'; }
stake() {
  "$B" wallet stake ${STAKE[$1]} --dev --wallet "$D/w$1.wallet" --rpc ${R[$1]} > "$D/stake$1.out" 2>&1 && ! grep -qi "error" "$D/stake$1.out" && SA[$1]=$(grep "^  to:" "$D/stake$1.out" | awk '{print $2}')
}
LAST=(); for i in $(seq 1 $N); do LAST[$i]=0; done
for _ in $(seq 1 600); do
  now=$(date +%s); alldone=1
  for i in $(seq 1 $N); do
    if [ -n "${SA[$i]}" ] && has_stake $i; then continue; fi
    alldone=0
    if [ $((now-LAST[$i])) -ge 25 ]; then LAST[$i]=$now; stake $i; fi
  done
  [ $alldone = 1 ] && [ "$(field ${R[1]} registered)" = "$N" ] && [ "$(minh 1 2 3 4 5 6)" -ge $((UP+25)) ] && [ "$(minfin 1 2 3 4 5 6)" -gt $((UP+5)) ] && break
  sleep 3
done
status
[ "$(field ${R[1]} registered)" = "$N" ] || fail "not all validators registered"
[ "$(minfin 1 2 3 4 5 6)" -gt $((UP+5)) ] || fail "finality is not running after setup"
echo "PASS setup: 6 validators registered, finalized height $(minfin 1 2 3 4 5 6)"
stopall
mkdir -p "$D/snap"; for i in $(seq 1 $N); do cp -R "$D/n$i" "$D/snap/n$i"; rm -f "$D/snap/n$i/peers.dat"; done

fi

restore() { stopall; for i in $(seq 1 $N); do rm -rf "$D/n$i"; cp -R "$D/snap/n$i" "$D/n$i"; done; }

# ---------- scenario 1 ----------
echo; echo "== scenario 1: majority {N1,N2,N3,N4} (80% stake, 1 thread on N1+N2) vs minority {N5,N6} (20%, 2 threads each)"
restore
startn 1 1;  startn 2 1 1;  startn 3 0 2;  startn 4 0 3
startn 5 2;  startn 6 2 5
sleep 3
PREFIN=$(maxfin 1 2 3 4 5 6)
echo "finalized at partition: $PREFIN"
for _ in $(seq 1 600); do
  mf=$(minfin 1 2 3 4); xf=$(maxfin 5 6)
  read -r wm _ hm <<<"$(bestwork 1 2 3 4)"; read -r wn _ hn <<<"$(bestwork 5 6)"
  [ "$mf" -ge $((PREFIN+15)) ] && [[ "$wn" > "$wm" ]] && [ "$(minh 5 6)" -ge $((PREFIN+20)) ] && break
  sleep 2
done
status
MAJFIN=$(minfin 1 2 3 4); MINFIN=$(maxfin 5 6)
echo "majority finalized=$MAJFIN (was $PREFIN), minority finalized=$MINFIN; work majority=$wm (h$hm) minority=$wn (h$hn)"
[ "$MAJFIN" -ge $((PREFIN+15)) ] || fail "scenario 1: majority did not keep finalizing"
echo "PASS majority keeps finalizing ($PREFIN -> $MAJFIN)"
[ "$MINFIN" -le "$PREFIN" ] || fail "scenario 1: minority finalized $MINFIN > $PREFIN"
echo "PASS minority cannot finalize (stays at $MINFIN)"
[[ "$wn" > "$wm" ]] || fail "scenario 1: premise failed, minority has no more work"
echo "PASS premise: the minority branch has more proof of work than the majority branch"
MAJHASH=$(bhash ${R[1]} $MAJFIN); MINHASH=$(bhash ${R[5]} $MAJFIN)
[ -n "$MAJHASH" ] && [ -n "$MINHASH" ] && [ "$MAJHASH" != "$MINHASH" ] || fail "scenario 1: branches did not diverge below the finalized height"
echo "finalized checkpoint #$MAJFIN: majority $MAJHASH, minority's block at that height $MINHASH"
echo "== rejoin (everybody restarted, not mining, chain topology N1-N2-…-N6 joins the groups at N4-N5)"
stopall
rm -f "$D"/n*/peers.dat
for i in $(seq 2 $N); do startn $i 0 $((i-1)); done; startn 1 0
converged 180 || { status; fail "scenario 1: nodes did not converge after rejoin (stuck)"; }
status
for i in $(seq 1 $N); do
  [ "$(bhash ${R[$i]} $MAJFIN)" = "$MAJHASH" ] || fail "scenario 1: N$i does not contain the finalized checkpoint $MAJFIN of the majority"
  [ "$(field ${R[$i]} finalized)" -ge "$MAJFIN" ] || fail "scenario 1: N$i finalized height below $MAJFIN"
done
echo "PASS all 6 nodes on one tip ($(field ${R[1]} tip | cut -c1-16)…), identical UTXO commitment, majority's finalized checkpoint #$MAJFIN contained in every chain, though the minority had more work"
echo "== finality resumes: all mine and validate again"
stopall
startn 1 1; for i in $(seq 2 $N); do startn $i 1 $((i-1)); done
F1=$(minfin 1 2 3 4 5 6)
for _ in $(seq 1 300); do [ "$(minfin 1 2 3 4 5 6)" -ge $((MAJFIN+15)) ] && break; sleep 2; done
status
[ "$(minfin 1 2 3 4 5 6)" -ge $((MAJFIN+15)) ] || fail "scenario 1: finality did not resume on all nodes"
echo "PASS scenario 1: finalization resumed on all nodes ($(minfin 1 2 3 4 5 6))"
for i in $(seq 1 $N); do
  [ "$(field ${R[$i]} slashed)" = "0" ] || fail "scenario 1: N$i reports slashed validators (a validator signed on both branches)"
done
! grep -q "voted for two different blocks" "$D"/n*.log || fail "scenario 1: an equivocation was detected in the logs (validators must not sign twice at one height)"
echo "PASS scenario 1: nobody was slashed (validators of the losing branch did not sign twice at one height)"

# ---------- scenario 2 ----------
echo; echo "== scenario 2: 3/3 split {N1,N2,N3} (60%) vs {N4,N5,N6} (40%), equal hash power"
restore
startn 1 1; startn 2 1 1; startn 3 1 2
startn 4 1; startn 5 1 4; startn 6 1 5
sleep 3
PREFIN=$(maxfin 1 2 3 4 5 6)
echo "finalized at partition: $PREFIN"
for _ in $(seq 1 600); do [ "$(minh 1 2 3 4 5 6)" -ge $((PREFIN+60)) ] && break; sleep 2; done
status
[ "$(maxfin 1 2 3 4 5 6)" -le "$PREFIN" ] || fail "scenario 2: somebody finalized without a 2/3 majority"
echo "PASS neither side finalizes (max finalized stays $PREFIN while both sides grew by $(( $(minh 1 2 3 4 5 6) - PREFIN )) blocks)"
read -r wa ta ha <<<"$(bestwork 1 2 3)"; read -r wb tb hb <<<"$(bestwork 4 5 6)"
[ "$ta" != "$tb" ] || fail "scenario 2: sides did not diverge"
if [[ "$wa" > "$wb" ]]; then WT=$ta; WH=$ha; WN=A; else WT=$tb; WH=$hb; WN=B; fi
echo "work A=$wa (h$ha) B=$wb (h$hb): expected winner $WN"
stopall
rm -f "$D"/n*/peers.dat
for i in $(seq 2 $N); do startn $i 0 $((i-1)); done; startn 1 0
converged 180 || { status; fail "scenario 2: nodes did not converge after rejoin"; }
[ "$(bhash ${R[1]} $WH)" = "$WT" ] || fail "scenario 2: converged on the lighter chain"
echo "PASS scenario 2: converged on the chain with more work ($WN, tip $(echo $WT | cut -c1-16)…), identical UTXO commitment"
stopall
startn 1 1; for i in $(seq 2 $N); do startn $i 1 $((i-1)); done
for _ in $(seq 1 300); do [ "$(minfin 1 2 3 4 5 6)" -ge $((PREFIN+15)) ] && break; sleep 2; done
status
[ "$(minfin 1 2 3 4 5 6)" -ge $((PREFIN+15)) ] || fail "scenario 2: finality did not resume after rejoin"
echo "PASS scenario 2: finality resumed after rejoin ($PREFIN -> $(minfin 1 2 3 4 5 6))"
for i in $(seq 1 $N); do [ "$(field ${R[$i]} slashed)" = "0" ] || fail "scenario 2: N$i reports slashed validators"; done
echo "PASS scenario 2: nobody was slashed"
stopall
echo "ALL PASS"
