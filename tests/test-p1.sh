#!/bin/bash
# P2P/RPC hardening test: 2 dev nodes, sync, per-IP cap, garbage, handshake timeout, clean SIGINT exit.
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
D=$BASE/p1

PA=$((PORT0+0)); RA=$((PORT0+1)); PB=$((PORT0+10)); RB=$((PORT0+11))
mkdir -p $D; rm -rf $D/a $D/b $D/w.wallet $D/*.log
PIDS=()
cleanup() { for p in "${PIDS[@]}"; do kill -9 $p 2>/dev/null; done; }
trap cleanup EXIT
fail() { echo "FAIL: $*"; exit 1; }
info() { curl -s --max-time 3 http://127.0.0.1:$1/info; }
field() { info $1 | awk -v k=$2 '$1==k{print $2}'; }

$B wallet create --dev --wallet $D/w.wallet >/dev/null 2>&1 || fail "wallet create"
ADDR=$($B wallet address --dev --wallet $D/w.wallet 2>/dev/null | tail -1)
$B node --dev --mine $ADDR --threads 1 --port $PA --rpc $RA --datadir $D/a > $D/a.log 2>&1 & PIDS+=($!); APID=$!
sleep 1
$B node --dev --port $PB --rpc $RB --datadir $D/b --connect 127.0.0.1:$PA > $D/b.log 2>&1 & PIDS+=($!); BPID=$!

echo "== sync"
for i in $(seq 1 60); do
  ha=$(field $RA height); hb=$(field $RB height)
  [ -n "$ha" ] && [ -n "$hb" ] && [ "$ha" -ge 3 ] && [ "$hb" -ge $((ha-3)) ] && break
  sleep 1
done
echo "A height=$ha B height=$hb peersA=$(field $RA peers) peersB=$(field $RB peers)"
[ "$ha" -ge 3 ] && [ "$hb" -ge $((ha-3)) ] || fail "nodes did not sync"
echo "PASS sync"

echo "== (a) per-IP cap (B already holds 1 inbound slot on A)"
python3 -I - <<PY || fail "per-IP test"
import socket, time
socks = [socket.create_connection(("127.0.0.1", $PA)) for _ in range(10)]
time.sleep(1)
alive = 0
for s in socks:
    s.settimeout(0.3)
    try:
        d = s.recv(1)
        if d == b"": continue       # EOF: refused
        alive += 1                  # got our peer's version bytes: accepted
    except socket.timeout:
        alive += 1                  # still open and silent: accepted
print("accepted by node A from this IP:", alive, "of 10 (expected 3: cap 4 minus B)")
assert alive == 3, alive
# silent sockets must be dropped by the 10 s handshake timeout
t0 = time.time(); closed = 0
for s in socks:
    s.settimeout(max(0.1, 14 - (time.time() - t0)))
    try:
        while True:
            d = s.recv(4096)
            if d == b"": closed += 1; break
    except socket.timeout: pass
print("handshake timeout closed %d sockets after %.1fs" % (closed, time.time() - t0))
assert closed >= 3
PY
echo "PASS per-IP cap + handshake timeout"

echo "== (b) garbage"
python3 -I - <<PY || fail "garbage test"
import socket, os, struct, time
def eof(s, t=3):
    s.settimeout(t)
    try:
        while True:
            if s.recv(4096) == b"": return True
    except socket.timeout: return False
    except ConnectionError: return True
magic = bytes([0xf1,0x9c,0x4b,0xde])
cases = {
 "random bytes": os.urandom(200),
 "good magic, bad checksum": magic + b"version\0\0\0\0\0" + struct.pack("<I", 4) + b"\0\0\0\0" + b"abcd",
 "oversized length": magic + b"block\0\0\0\0\0\0\0" + struct.pack("<I", 0x7fffffff) + b"\0\0\0\0",
}
for name, data in cases.items():
    s = socket.create_connection(("127.0.0.1", $PA)); s.sendall(data)
    r = eof(s); print("%-28s dropped=%s" % (name, r)); assert r, name
# non-version first message
s = socket.create_connection(("127.0.0.1", $PA)); s.sendall(magic + b"ping\0\0\0\0\0\0\0\0" + struct.pack("<I", 0) + b"\x5d\xf6\xe0\xe2"); 
r = eof(s); print("%-28s dropped=%s" % ("ping before version", r)); assert r
PY
h=$(field $RA height); [ -n "$h" ] || fail "RPC /info not responding after garbage"
echo "RPC /info after garbage: height=$h peers=$(field $RA peers)"
grep -E "misbehaving|disconnecting|loopback" $D/a.log | tail -6
[ "$(field $RB height)" -ge $((h-1)) ] || fail "B lost sync"
echo "PASS garbage handling (loopback is never banned, so peer is dropped, not banned)"

echo "== (c) clean SIGINT exit"
for name in B A; do
  eval pid=\$${name}PID
  kill -INT $pid; t0=$(date +%s)
  while kill -0 $pid 2>/dev/null && [ $(( $(date +%s) - t0 )) -lt 5 ]; do sleep 0.1; done
  kill -0 $pid 2>/dev/null && fail "$name still running 5 s after SIGINT"
  wait $pid; rc=$?
  echo "node $name exit code $rc after $(( $(date +%s) - t0 )) s"
  [ $rc -eq 0 ] || fail "node $name exit code $rc"
done
PIDS=()
echo "ALL PASS"
