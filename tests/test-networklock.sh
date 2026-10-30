#!/bin/bash
# Network lock on the dev network, using SCRATCH builds (the real genesis/checkpoints are never changed):
#  (a) a node whose dev genesis message differs must be disconnected: "foreign network (different genesis)", no blocks exchanged
#  (b) a node with a hard-coded checkpoint that the peer's chain contradicts rejects that block and does not follow the chain
#  (c) a node below its last hard-coded checkpoint does not mine ("waiting for sync, mining postponed")
# The scratch copies live in $PLK_TEST_DIR/scratch (sources copied from this tree, patched with sed, built there).
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
D=$BASE/networklock
S=$BASE/scratch
SRC="$HERE/.."
PA=$((PORT0+500)); RA=$((PORT0+501)); PB=$((PORT0+510)); RB=$((PORT0+511))
mkdir -p "$S"; rm -rf "$D"; mkdir -p "$D"
build_variant() {  # name sed-expression
  local v=$S/$1
  if [ ! -x "$v/build/phylka" ]; then
    rm -rf "$v"; mkdir -p "$v"
    cp -R "$SRC/src" "$SRC/deps" "$SRC/CMakeLists.txt" "$v/"
    [ -e "$SRC/contrib" ] && cp -R "$SRC/contrib" "$v/"
    cp "$v/src/chain.cpp" "$v/chain.orig"
    sed -i.bak "$2" "$v/src/chain.cpp"; rm -f "$v/src/chain.cpp.bak"
    cmp -s "$v/src/chain.cpp" "$v/chain.orig" && fail "variant $1: patch did not apply"
    (cmake -S "$v" -B "$v/build" -DCMAKE_BUILD_TYPE=Release >/dev/null && cmake --build "$v/build" -j4 >/dev/null 2>&1) || fail "variant $1 did not build"
  fi
  echo "$v/build/phylka"
}
ALT=${PLK_ALT_BIN:-$(build_variant altgen '/17333, 17334, GENESIS_TEXT, 1793000000/s/GENESIS_TEXT/"ALT scratch genesis"/')}
CKP=${PLK_CKPT_BIN:-$(build_variant ckpt '/20, 10, {}, {}};/s/{}, {}}/{}, {{5, "00000000bad0bad0bad0bad0bad0bad0bad0bad0bad0bad0bad0bad0bad0bad0"}}}/')}
"$B" wallet create --dev --wallet "$D/w.wallet" >/dev/null 2>&1 || fail "wallet"
ADDR=$("$B" wallet address --dev --wallet "$D/w.wallet" 2>/dev/null | tail -1)

echo "== (a) different genesis"
"$B" node --dev --mine "$ADDR" --threads 1 --port $PA --rpc $RA --datadir "$D/a" > "$D/a.log" 2>&1 & PIDS+=($!); APID=$!
for _ in $(seq 1 60); do [ "$(field $RA height)" -ge 8 ] 2>/dev/null && break; sleep 1; done
"$ALT" node --dev --port $PB --rpc $RB --datadir "$D/b" --connect 127.0.0.1:$PA > "$D/b.log" 2>&1 & PIDS+=($!); BPID=$!
sleep 6
[ "$(field $RB height)" = "0" ] || fail "alt-genesis node synced blocks from a foreign chain"
grep -q "foreign network (different genesis)" "$D/a.log" "$D/b.log" || fail "no 'foreign network' message"
[ "$(field $RA peers)" = "0" ] && [ "$(field $RB peers)" = "0" ] || fail "foreign node stayed connected"
echo "PASS different genesis: disconnected ($(grep -h 'foreign network' "$D/a.log" "$D/b.log" | head -1)), peers 0/0, no blocks exchanged"
stopnode $BPID

echo "== (b) checkpoint rejection: node requires hash 00000000bad0… at height 5"
"$CKP" node --dev --port $PB --rpc $RB --datadir "$D/c" --connect 127.0.0.1:$PA > "$D/c.log" 2>&1 & PIDS+=($!); CPID=$!
sleep 8
H=$(field $RB height)
[ "$H" -lt 5 ] 2>/dev/null || fail "checkpoint node accepted a block at/after height 5 (height $H)"
grep -q "checkpoint" "$D/c.log" || { tail -5 "$D/c.log"; fail "no checkpoint-rejection log line"; }
echo "PASS block at height 5 contradicting the checkpoint is rejected (checkpoint node stuck at height $H, A at $(field $RA height))"
stopnode $CPID

stopnode $APID; PIDS=()
echo "== (c) mining guard: checkpoint node with --mine and no peers stays below the checkpoint"
"$CKP" node --dev --mine "$ADDR" --threads 1 --port $PB --rpc $RB --datadir "$D/d" > "$D/d.log" 2>&1 & PIDS+=($!); DPID=$!
sleep 8
[ "$(field $RB height)" = "0" ] && ! grep -q "Found block" "$D/d.log" || fail "guarded node mined (height $(field $RB height))"
grep -q "waiting for sync, mining postponed" "$D/d.log" || fail "guard message missing"
echo "PASS node below the last hard-coded checkpoint does not mine"
stopnode $DPID; PIDS=()
echo "ALL PASS"
