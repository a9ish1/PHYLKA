#!/bin/bash
# GUI wallet end to end on the dev network: one mining node, `phylka gui`, curl against the JSON API.
# Create/restore, balance after mining, send between two wallets, history, stake/unstake, and the negative security checks.
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
D=$BASE/gui; NP=$((PORT0+0)); RP=$((PORT0+1)); GP=$((PORT0+2))
mkdir -p $D; rm -rf $D/node $D/*.wallet* $D/*.log
fail() { echo "FAIL: $*"; exit 1; }
jget() { python3 -c "import sys,json; d=json.load(sys.stdin)
for k in '$1'.split('.'): d=d[int(k)] if k.isdigit() else d[k]
print(d)"; }

# 1. wallets: A is created through the GUI API (generate -> confirm three words -> create), B with the CLI
$B gui --dev --wallet $D/a.wallet --rpc $RP --port $GP --no-browser > $D/gui.log 2>&1 & PIDS+=($!)
sleep 1
T=$(grep -o '#t=[0-9a-f]*' $D/gui.log | cut -c4-); [ ${#T} -eq 64 ] || fail "no session token in the gui output"
U=http://127.0.0.1:$GP
api() { curl -s -H "Authorization: Bearer $T" "$@"; }
post() { api -X POST -H 'Content-Type: application/json' $U$1 -d "$2"; }
G=$(post /api/wallet/generate '{}')
BODY=$(python3 -c "import json,sys; g=json.loads(sys.argv[1]); print(json.dumps({'password':'test wallet pass','answers':[g['words'][i-1] for i in g['challenge']]}))" "$G")
ADDR=$(post /api/wallet/create "$BODY" | jget address); case "$ADDR" in plkd1*) ;; *) fail "create";; esac
echo "PASS create wallet through the GUI ($ADDR)"
$B wallet create --dev --wallet $D/b.wallet >/dev/null 2>&1 || fail "cli create"
BADDR=$($B wallet address --dev --wallet $D/b.wallet | tail -1)
[ "$(api $U/api/overview | jget node.running)" = "False" ] || fail "node should be reported as not running"
echo "PASS node-not-running is reported"

# 2. node mines to A, balance appears
$B node --dev --mine $ADDR --threads 1 --port $NP --rpc $RP --datadir $D/node --noseeds > $D/node.log 2>&1 & PIDS+=($!)
for i in $(seq 1 60); do S=$(api $U/api/overview | jget wallet.balance.spendable 2>/dev/null); [ -n "$S" ] && [ "$S" != "None" ] && [ "$S" -gt 0 ] && break; sleep 1; done
[ "$S" -gt 0 ] || fail "no balance after mining"
echo "PASS balance after mining: $S fyl"

# 3. send A -> B: preview, wrong password, stale fee, real send
P=$(post /api/tx/preview "{\"kind\":\"send\",\"to\":\"$BADDR\",\"amount\":\"1.5\"}"); FEE=$(echo "$P" | jget fee)
[ "$FEE" = "3966" ] || fail "fee preview: $P"
post /api/tx/send "{\"kind\":\"send\",\"to\":\"$BADDR\",\"amount\":\"1.5\",\"password\":\"nope nope nope\",\"expect_amount\":150000000,\"expect_fee\":$FEE}" | grep -q wrong_password || fail "wrong password accepted"
post /api/tx/send "{\"kind\":\"send\",\"to\":\"$BADDR\",\"amount\":\"1.5\",\"password\":\"test wallet pass\",\"expect_amount\":150000000,\"expect_fee\":1}" | grep -q changed || fail "stale preview accepted"
TX=$(post /api/tx/send "{\"kind\":\"send\",\"to\":\"$BADDR\",\"amount\":\"1.5\",\"password\":\"test wallet pass\",\"expect_amount\":150000000,\"expect_fee\":$FEE}" | jget txid)
[ ${#TX} -eq 64 ] || fail "send"
for i in $(seq 1 60); do $B wallet balance --dev --wallet $D/b.wallet --rpc $RP | grep -q "Available:      1.5 PLK" && break; sleep 1; done
$B wallet balance --dev --wallet $D/b.wallet --rpc $RP | grep -q "Available:      1.5 PLK" || fail "B did not receive the payment"
api $U/api/history | grep -q "$TX" || fail "history misses the payment"
echo "PASS send $TX, received by B (checked with the CLI), shows in history"

# 4. stake and unstake
post /api/tx/preview '{"kind":"stake","amount":"5"}' | grep -q bad_amount || fail "stake below minimum accepted"
post /api/tx/send "{\"kind\":\"stake\",\"amount\":\"10\",\"password\":\"test wallet pass\",\"expect_amount\":1000000000,\"expect_fee\":$FEE}" | grep -q txid || fail "stake"
for i in $(seq 1 120); do [ "$(api $U/api/stakes | jget unlockable)" -gt 0 ] && break; sleep 1; done
[ "$(api $U/api/stakes | jget unlockable)" = "1000000000" ] || fail "stake did not unlock"
UN=$(post /api/tx/preview '{"kind":"unstake"}'); AM=$(echo "$UN" | jget amount)
post /api/tx/send "{\"kind\":\"unstake\",\"password\":\"test wallet pass\",\"expect_amount\":$AM,\"expect_fee\":$FEE}" | grep -q txid || fail "unstake"
echo "PASS stake and unstake"

# 5. negative security checks
code() { curl -s -o /dev/null -w "%{http_code}" "$@"; }
[ "$(code $U/api/state)" = 401 ] || fail "no token"
[ "$(code -H 'Authorization: Bearer 00' $U/api/state)" = 401 ] || fail "wrong token"
[ "$(code -H "Authorization: Bearer $T" -H 'Host: evil.example' $U/api/state)" = 403 ] || fail "bad Host"
[ "$(code -H "Authorization: Bearer $T" -H 'Origin: http://evil.example' $U/api/state)" = 403 ] || fail "foreign Origin"
[ "$(code -H "Authorization: Bearer $T" $U/api/seed)" = 405 ] || fail "GET on POST endpoint"
[ "$(code -H "Authorization: Bearer $T" -H 'Content-Type: text/plain' -d '{}' $U/api/seed)" = 415 ] || fail "text/plain POST"
curl -si $U/ | grep -qi "^Content-Security-Policy: default-src 'self'" || fail "CSP header"
curl -si $U/ | grep -qi "^Access-Control" && fail "CORS header present"
echo "PASS negative security checks"
echo ALL PASS
