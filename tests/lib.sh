#!/bin/bash
# Shared helpers for the dev-network test scripts. DEV NETWORK ONLY: every node is started with --dev, an explicit
# --datadir under $PLK_TEST_DIR and explicit ports from $PLK_TEST_PORT upward. Never touches ~/phylka or the main net.
# Env: PLK_BIN (binary, default ../build/phylka), PLK_TEST_DIR (default /tmp/phylka-tests), PLK_TEST_PORT (default 24000).
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
B="${PLK_BIN:-$HERE/../build/phylka}"
BASE="${PLK_TEST_DIR:-/tmp/phylka-tests}"
PORT0="${PLK_TEST_PORT:-24000}"
[ -x "$B" ] || { echo "binary not found: $B (build first, or set PLK_BIN)"; exit 2; }
case "$BASE" in /*) ;; *) echo "PLK_TEST_DIR must be absolute"; exit 2;; esac
case "$BASE" in "$HOME/phylka"|"$HOME/phylka/"*) echo "refusing to use ~/phylka"; exit 2;; esac
if [ "$PORT0" -eq 7333 ] || [ "$PORT0" -eq 7334 ] || [ "$PORT0" -lt 1024 ]; then echo "bad PLK_TEST_PORT"; exit 2; fi
export PHYLKA_TEST_PASSWORD='test wallet pass' PHYLKA_TEST_YES=1
PIDS=()
cleanup() { for p in "${PIDS[@]}"; do kill -9 "$p" 2>/dev/null; done; }
trap cleanup EXIT
fail() { echo "FAIL: $*"; exit 1; }
field() { curl -s --max-time 3 "http://127.0.0.1:$1/info" | awk -v k="$2" '$1==k{print $2}'; }
stopnode() { kill -INT "$1" 2>/dev/null; for _ in $(seq 1 100); do kill -0 "$1" 2>/dev/null || { wait "$1" 2>/dev/null; return 0; }; sleep 0.2; done; fail "node $1 did not stop"; }
