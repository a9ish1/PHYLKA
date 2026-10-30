#!/usr/bin/env bash
# Build PHYLKA from the current checkout and install it as a systemd service on
# Ubuntu/Debian. Idempotent: safe to run again to upgrade.
#
#   sudo contrib/install-vps.sh            # install / upgrade, then (re)start if enabled
#
# It does NOT create wallets or seed phrases, and does not start the service
# the first time: review /etc/default/phylka and start it yourself.
set -euo pipefail

SERVICE_USER=phylka
DATADIR=/var/lib/phylka
PREFIX=/usr/local
P2P_PORT=7333

die() { echo "error: $*" >&2; exit 1; }

[ "$(id -u)" -eq 0 ] || die "run as root: sudo $0"
command -v apt-get >/dev/null || die "this script supports Ubuntu/Debian (apt) only"
command -v systemctl >/dev/null || die "systemd is required"

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
[ -f "$REPO/CMakeLists.txt" ] && [ -f "$REPO/contrib/systemd/phylka.service" ] || die "run from a PHYLKA checkout"

echo "==> installing build dependencies"
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq build-essential cmake git ca-certificates

echo "==> building ($REPO/build)"
cmake -S "$REPO" -B "$REPO/build" -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$REPO/build" --parallel "$(nproc)"
"$REPO/build/phylka" selftest >/dev/null || die "selftest failed, not installing"

echo "==> installing binary to $PREFIX/bin/phylka"
install -m 0755 "$REPO/build/phylka" "$PREFIX/bin/phylka"

echo "==> user and data directory"
if ! id "$SERVICE_USER" >/dev/null 2>&1; then
  useradd --system --home-dir "$DATADIR" --shell /usr/sbin/nologin --user-group "$SERVICE_USER"
fi
install -d -m 0700 -o "$SERVICE_USER" -g "$SERVICE_USER" "$DATADIR"

echo "==> systemd unit"
install -m 0644 "$REPO/contrib/systemd/phylka.service" /etc/systemd/system/phylka.service
if [ ! -e /etc/default/phylka ]; then
  cat >/etc/default/phylka <<'CONF'
# Extra arguments for `phylka node`, for example:
#   PHYLKA_ARGS="--connect seed.example.org:7333"
#   PHYLKA_ARGS="--mine plk1... --threads 2"
PHYLKA_ARGS=""
CONF
  chmod 0644 /etc/default/phylka
fi
systemctl daemon-reload

if command -v ufw >/dev/null && ufw status | grep -q "Status: active"; then
  echo "==> ufw is active: allowing ${P2P_PORT}/tcp (the RPC port 7334 is never opened)"
  ufw allow "${P2P_PORT}/tcp" >/dev/null
else
  echo "==> ufw not active: make sure ${P2P_PORT}/tcp is reachable if you want inbound peers"
fi

if systemctl is-enabled --quiet phylka 2>/dev/null; then
  echo "==> restarting phylka"
  systemctl restart phylka
fi

cat <<NEXT

Installed. Next steps:
  1. Edit /etc/default/phylka (set --connect host:port to an existing node,
     or leave empty to be a seed node others connect to).
  2. sudo systemctl enable --now phylka
  3. journalctl -u phylka -f
  4. Query the node locally: curl -s http://127.0.0.1:7334/api/info
Create wallets on your own computer, not on this server. See docs/RUNNING.md.
NEXT
