#!/usr/bin/env bash
# Start N PHYLKA dev-network nodes on one machine and connect them in a chain.
#
#   contrib/local-testnet.sh start [N]     (default N=3)
#   contrib/local-testnet.sh status
#   contrib/local-testnet.sh stop
#
# Environment:
#   PHYLKA_BIN       path to the binary        (default: ./build/phylka)
#   PHYLKA_TESTNET   base directory            (default: ./testnet)
#   PHYLKA_BASE_PORT first port                (default: 27000)
#
# Node i (0-based) uses P2P port BASE+2i and RPC/explorer port BASE+2i+1,
# datadir <base>/node<i>. Node 0 mines to a fresh DEV wallet at <base>/miner.wallet.
# The wallet password comes from PHYLKA_TEST_PASSWORD, which the binary honours
# on the dev network only. These are throw-away dev keys: never reuse them.
# Always runs with --dev; it never starts a main-network node.
set -euo pipefail

BIN="${PHYLKA_BIN:-./build/phylka}"
BASE="${PHYLKA_TESTNET:-./testnet}"
BASE_PORT="${PHYLKA_BASE_PORT:-27000}"

die() { echo "error: $*" >&2; exit 1; }

# Resolve to absolute paths so later `cd` or other cwd does not matter.
mkdir -p "$BASE"
BASE="$(cd "$BASE" && pwd)"
[ -x "$BIN" ] || die "binary not found or not executable: $BIN (build first, or set PHYLKA_BIN)"
BIN="$(cd "$(dirname "$BIN")" && pwd)/$(basename "$BIN")"

p2p_port() { echo $((BASE_PORT + 2 * $1)); }
rpc_port() { echo $((BASE_PORT + 2 * $1 + 1)); }

alive() { # alive PIDFILE
  [ -f "$1" ] && kill -0 "$(cat "$1")" 2>/dev/null
}

start() {
  local n="${1:-3}" i
  case "$n" in ''|*[!0-9]*) die "N must be a number" ;; esac
  [ "$n" -ge 1 ] || die "N must be >= 1"
  [ $((BASE_PORT + 2 * n)) -le 65535 ] || die "ports out of range"

  export PHYLKA_TEST_PASSWORD="${PHYLKA_TEST_PASSWORD:-dev-only-password}"
  export PHYLKA_TEST_YES=1
  local wallet="$BASE/miner.wallet"
  if [ ! -f "$wallet" ]; then
    # The seed phrase is printed by `wallet create`; discard it, this is a throw-away dev wallet.
    "$BIN" wallet create --dev --wallet "$wallet" >/dev/null
  fi
  local addr
  addr="$("$BIN" wallet address --dev --wallet "$wallet")"

  for ((i = 0; i < n; i++)); do
    local dir="$BASE/node$i" pidf="$BASE/node$i.pid"
    if alive "$pidf"; then echo "node$i already running (pid $(cat "$pidf"))"; continue; fi
    mkdir -p "$dir"
    local args=(node --dev --datadir "$dir" --port "$(p2p_port "$i")" --rpc "$(rpc_port "$i")")
    # chain topology: node i connects to node i-1
    if [ "$i" -gt 0 ]; then args+=(--connect "127.0.0.1:$(p2p_port $((i - 1)))"); fi
    if [ "$i" -eq 0 ]; then args+=(--mine "$addr" --threads 1); fi
    nohup "$BIN" "${args[@]}" >"$BASE/node$i.log" 2>&1 &
    echo $! >"$pidf"
    echo "node$i: pid $(cat "$pidf")  p2p 127.0.0.1:$(p2p_port "$i")  explorer http://127.0.0.1:$(rpc_port "$i")/  log $BASE/node$i.log"
  done
  echo "mining to $addr (wallet $wallet)"
}

status() {
  local f i
  for f in "$BASE"/node*.pid; do
    [ -e "$f" ] || { echo "no nodes recorded in $BASE"; return 0; }
    i="${f##*/node}"; i="${i%.pid}"
    if alive "$f"; then
      echo "node$i: running (pid $(cat "$f"))  $(curl -fsS "http://127.0.0.1:$(rpc_port "$i")/api/info" 2>/dev/null || echo '(RPC not answering yet)')"
    else
      echo "node$i: stopped"
    fi
  done
}

stop() {
  local f pid
  for f in "$BASE"/node*.pid; do
    [ -e "$f" ] || { echo "nothing to stop in $BASE"; return 0; }
    if alive "$f"; then
      pid="$(cat "$f")"
      kill -INT "$pid" 2>/dev/null || true
      for _ in $(seq 1 50); do kill -0 "$pid" 2>/dev/null || break; sleep 0.2; done
      if kill -0 "$pid" 2>/dev/null; then kill -TERM "$pid" 2>/dev/null || true; fi
      echo "stopped pid $pid"
    fi
    rm -f "$f"
  done
}

case "${1:-}" in
  start)  start "${2:-3}" ;;
  status) status ;;
  stop)   stop ;;
  *) echo "usage: $0 start [N] | status | stop" >&2; exit 2 ;;
esac
