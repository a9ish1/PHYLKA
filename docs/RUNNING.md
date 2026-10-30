# Running PHYLKA

How to build the `phylka` node and run it on your own computer, as a small
private network on one machine, or on a VPS. Everything here uses commands that
exist in this repository.

> **Beta.** PHYLKA is not yet tested at scale, qualified, validated or audited.
> Do not put money you cannot lose on it. See [DISCLAIMER.md](../DISCLAIMER.md)
> and [SECURITY.md](../SECURITY.md).

Contents: [1 Build](#1-build) | [2 Local dev node](#2-local-single-node-dev-network)
| [3 Local multi-node](#3-local-multi-node-network-on-one-machine)
| [4 Your own network / joining main](#4-main-vs-dev-genesis-and-your-own-network)
| [5 VPS](#5-vps-deployment-ubuntudebian) | [6 Docker](#6-docker-untested)
| [7 Mining and staking](#7-mining-and-staking) | [8 Troubleshooting](#8-troubleshooting)

## Facts you need

| | main | dev (`--dev`) |
|---|---|---|
| P2P port (default) | 7333 | 17333 |
| RPC and explorer (default) | 7334 | 17334 |
| Address prefix | `plk1...` | `plkd1...` |
| Block time / coinbase maturity | 600 s / 100 blocks | 5 s / 2 blocks |
| Default data directory | `~/phylka/data/main` | `~/phylka/data/dev` |
| Default wallet file | `~/phylka/wallets/main.wallet` | `~/phylka/wallets/dev.wallet` |

- The P2P listener binds to `127.0.0.1` by default. `--public` makes it listen on
  `0.0.0.0`. The RPC/explorer port always binds to `127.0.0.1` and has **no
  authentication**: never expose it (no firewall rule, no Docker `-p`, no proxy).
- `--connect host:port` (repeatable) makes the node dial that peer and keep
  retrying. Hostnames are resolved with `getaddrinfo`, IPv4 only.
- Unknown subcommands and flags are an error (usage is printed, exit code 2),
  so a typo never starts a node. `phylka --help` lists every flag.
- A node does not mine until it has synced past the last hard-coded checkpoint
  (log: "waiting for sync, mining postponed"), so a fresh node never wastes
  work on a private branch.
- Wallet commands (`balance`, `send`, `stake`, ...) talk to a node on
  `127.0.0.1:<rpc port>`; pass `--rpc PORT` if the node does not use the
  default. The wallet signs locally; the node never sees your keys.

## 1. Build

### macOS

```sh
xcode-select --install            # compiler (skip if already installed)
brew install cmake                # needs Homebrew, https://brew.sh
git clone https://github.com/a9ish1/PHYLKA.git && cd PHYLKA
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
./build/phylka selftest
```

### Ubuntu / Debian

Needs CMake 3.20+ and GCC 9+ (Ubuntu 20.04 and Debian 11 ship older CMake;
Ubuntu 22.04+ and Debian 12 are fine). The maintainers have verified the macOS
build; Linux with GCC is expected to work.

```sh
sudo apt update
sudo apt install -y build-essential cmake git
git clone https://github.com/a9ish1/PHYLKA.git && cd PHYLKA
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel "$(nproc)"
./build/phylka selftest
```

If `cmake --version` is older than 3.20: `sudo snap install cmake --classic`
or `pip install --user cmake`.

`selftest` prints `ok` lines and should end without any `FAIL`. Run it
once after every build. It uses throw-away directories under the system temp
dir and does not touch your data.

## 2. Local single-node dev network

```sh
./build/phylka wallet create --dev          # shows 24 words ONCE; write them on paper
ADDR="$(./build/phylka wallet address --dev)"
./build/phylka node --dev --mine "$ADDR" --threads 2
```

Leave that running. In a second terminal:

```sh
./build/phylka wallet balance --dev         # coinbase matures after 2 blocks
curl -s http://127.0.0.1:17334/api/info     # JSON status
open http://127.0.0.1:17334/                # explorer (Linux: xdg-open)
```

Stop with Ctrl-C. Data lives in `~/phylka/data/dev`; delete that directory to
start from scratch.

## 3. Local multi-node network on one machine

Each node needs its **own** `--datadir`, `--port` and `--rpc`.

Automated (starts N nodes on ports 27000+, chains them, node 0 mines to a
fresh throw-away dev wallet):

```sh
contrib/local-testnet.sh start 3        # nodes in ./testnet
contrib/local-testnet.sh status
contrib/local-testnet.sh stop
```

Override with `PHYLKA_BIN`, `PHYLKA_TESTNET` (base directory) and
`PHYLKA_BASE_PORT`. Node *i* uses P2P `BASE+2i` and RPC/explorer `BASE+2i+1`
(node 0: 27000/27001, node 1: 27002/27003, ...); logs are
`testnet/node<i>.log`. The script sets `PHYLKA_TEST_PASSWORD`, a dev-only hook
the wallet honours **only on the dev network**; the keys are disposable.

By hand, three terminals:

```sh
D=/tmp/plk
./build/phylka node --dev --datadir $D/a --port 27000 --rpc 27001 --mine "$ADDR"
./build/phylka node --dev --datadir $D/b --port 27002 --rpc 27003 --connect 127.0.0.1:27000
./build/phylka node --dev --datadir $D/c --port 27004 --rpc 27005 --connect 127.0.0.1:27002
```

Check that blocks propagate: `curl -s http://127.0.0.1:27005/api/info` should
show a height close to node a's and `"peers"` of at least 1. Query a specific
node's balance with `./build/phylka wallet balance --dev --rpc 27003`.

Note: ban scoring does not apply to `127.x` addresses, so tests on loopback
are not affected by it.

## 4. Main vs dev, genesis, and your own network

**Genesis is compiled in.** `src/chain.cpp` defines `ChainParams::main()` and
`ChainParams::dev()`, including the network magic, ports, genesis message,
timestamp and nonce. A node has no way to import a different genesis at
runtime.

- **dev** has trivial difficulty (genesis is mined in a moment at start-up),
  5-second blocks, maturity 2. Every dev node in the world starts from the same
  genesis, so what separates a "private" dev network from anyone else's is only
  which peers you connect to.
- **main** has a fixed, already-mined genesis
  (`00000000df01a464bd9f7ad30b88da74059e25ea5b60eccaac0f95f2ba1a70a0`),
  10-minute blocks, maturity 100.

### Joining the existing PHYLKA main network

Be aware of the current state: there is **no peer discovery and no built-in
public seed node**. To join main you need the `host:port` of at least one node
that someone is already running (normally `host:7333`), obtained from the
project maintainers or the community. Then:

```sh
./build/phylka node --connect HOST:7333 --public
```

If nobody is running a reachable main node, there is nothing to join; you would
be the first, and the section below on running your own seed node applies.
Run a main node first without mining and watch the log to see whether it
syncs (`curl -s http://127.0.0.1:7334/api/info` shows `height` and `peers`).

### Starting a private network

Option A, no code changes: use **dev** on a VPS or a few machines
(`--dev --public`, see section 5) and give the address of your first node to
the others with `--connect`. Blocks are fast and cheap, which is fine for
testing and demos. Do not treat dev coins as anything but test coins.

Option B, a separate network with main-like parameters (code change, rebuild):

1. In `src/chain.cpp`, in `ChainParams::main()`, change at least the network
   magic bytes, the genesis message and the ports so your nodes can never mix
   with the real network. Set `genesis_nonce` to `0` temporarily.
2. Build with the genesis tool enabled (it is not in normal builds):
   `cmake -S . -B build -DPLK_ENABLE_MKGENESIS=ON && cmake --build build`,
   then run `./build/phylka mkgenesis --threads N`. It mines a genesis
   block at difficulty 1 (can take minutes to hours depending on your CPU) and
   prints the hash, time and nonce.
3. Put the printed time and nonce into `ChainParams::main()`, rebuild, and
   give every participant **that same source tree** (a fork of the repository).
4. Replace the `checkpoints` list in `ChainParams::main()` with your own
   genesis (height 0). The PHYLKA checkpoints would reject your chain.
5. `selftest` contains a check for the original main genesis hash and will fail
   on your fork; update or remove that check.

Nodes exchange the genesis hash in the handshake and drop peers from another
network ("foreign network (different genesis)"), so a fork can never mix with PHYLKA.
A fork is a different network, not PHYLKA.

Every node on your network must run the identical build. Run one node with
`--public` as the first seed; the others use `--connect` to it.

## 5. VPS deployment (Ubuntu/Debian)

Tested design: a small VPS (1 vCPU / 1 GB RAM is enough for a young chain), a
non-root `phylka` user, data in `/var/lib/phylka`, systemd, firewall allowing
only SSH and the P2P port. **The installer script was not run on a real VPS
by its author**; read it before you run it (`contrib/install-vps.sh`).

### 5.1 One-shot installer

```sh
ssh you@your-vps
sudo apt update && sudo apt install -y git
git clone https://github.com/a9ish1/PHYLKA.git && cd PHYLKA
sudo contrib/install-vps.sh
```

It installs build dependencies, builds, runs `selftest`, installs
`/usr/local/bin/phylka`, creates user `phylka` with data directory
`/var/lib/phylka` (mode 0700), installs the systemd unit and
`/etc/default/phylka`, and opens `7333/tcp` **only if ufw is already active**.
It creates no wallet and no seed, and does not start the service on the first
run. Re-running it upgrades the binary and restarts an enabled service.

### 5.2 Manual steps (what the installer does)

```sh
sudo apt install -y build-essential cmake git
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build --parallel "$(nproc)"
./build/phylka selftest
sudo install -m 0755 build/phylka /usr/local/bin/phylka

sudo useradd --system --home-dir /var/lib/phylka --shell /usr/sbin/nologin --user-group phylka
sudo install -d -m 0700 -o phylka -g phylka /var/lib/phylka

sudo cp contrib/systemd/phylka.service /etc/systemd/system/
sudo systemctl daemon-reload
```

### 5.3 Firewall

Allow SSH first, or you will lock yourself out:

```sh
sudo ufw allow OpenSSH
sudo ufw allow 7333/tcp          # P2P (dev network: 17333/tcp)
sudo ufw enable
sudo ufw status
```

Never open 7334. It is bound to loopback anyway, but a firewall rule is a
second line of defence. On cloud providers with their own security groups,
open 7333/tcp there too.

### 5.4 Configure and start

The unit runs `phylka node --datadir /var/lib/phylka --public $PHYLKA_ARGS`.
Put extra flags in `/etc/default/phylka`:

```sh
# join an existing node
PHYLKA_ARGS="--connect seed.example.org:7333"
# run the dev network instead (also open 17333/tcp)
# PHYLKA_ARGS="--dev"
```

Instead of `PHYLKA_ARGS` you can use a config file `/var/lib/phylka/phylka.conf`
(read from the datadir, or pass `--conf FILE`). One `key=value` per line, keys
are the long flags without `--`, `#` starts a comment, command-line flags win:

```ini
public=1
seed=seed.example.org:7333
seed=203.0.113.7:7333
```

`--seed`, `--connect` and `--addnode` accept DNS names and may be repeated.
After the first connection, nodes find more peers by themselves (addr gossip)
and remember them in `peers.dat`, keeping about 8 outbound connections.

```sh
sudo systemctl enable --now phylka
systemctl status phylka
```

Verify from the VPS itself:

```sh
curl -s http://127.0.0.1:7334/api/info          # height, peers, tip
```

### 5.5 How others connect to your node

Tell them your public address, `YOUR.IP.OR.HOST:7333`. They run:

```sh
phylka node --connect YOUR.IP.OR.HOST:7333              # --dev for dev
```

or add the same `--connect` to their `PHYLKA_ARGS`.
`--seed YOUR.IP.OR.HOST:7333` (or `seed=` in `phylka.conf`) works the same way.
Only nodes started with `--public` advertise themselves to the network.
Test reachability from another machine: `nc -vz YOUR.IP.OR.HOST 7333`.

### 5.6 Logs

```sh
journalctl -u phylka -f                 # follow
journalctl -u phylka --since "1 hour ago"
journalctl -u phylka -p err
sudo systemctl restart phylka
```

`New block #N` in the log is a new block.

### 5.7 Stopping and upgrades

The unit stops the node with `SIGINT` (a clean shutdown; `SIGTERM` is handled the
same way). To upgrade:

```sh
cd PHYLKA && git pull
sudo contrib/install-vps.sh      # rebuilds, reinstalls, restarts if enabled
journalctl -u phylka -n 50
```

If a release changes consensus rules or the genesis, the data directory may
have to be wiped: read the release notes first, and back up before upgrading.

### 5.8 Backups

- **Wallet**: keep wallets on your own machine, not on the VPS. The 24-word
  seed on paper is the real backup; the `.wallet` file (and its `.idx`
  companion) is the password-encrypted copy. Copy it to offline storage.
  Only put a wallet on the VPS if you knowingly mine or stake there (section 7),
  and then only a wallet that holds just the funds you need for it.
- **Chain data** (`/var/lib/phylka`) can be re-downloaded from peers, so it is
  low value. If you do want a copy: `sudo systemctl stop phylka`, then
  `sudo tar czf phylka-data.tgz -C /var/lib phylka`, then start again. Copying
  while running can capture an inconsistent state.

### 5.9 Using the RPC safely: SSH tunnel

The RPC has no authentication and listens on loopback, so reach it through SSH
and run the wallet on your own computer:

```sh
# on your laptop
ssh -N -L 7334:127.0.0.1:7334 you@your-vps
# in another laptop terminal; the wallet signs locally, the VPS only relays
phylka wallet balance
phylka wallet send plk1... 1.5
open http://127.0.0.1:7334/         # explorer of the VPS node
```

If a node also runs on your laptop on 7334, pick another local port:
`ssh -N -L 7344:127.0.0.1:7334 ...` and `phylka wallet balance --rpc 7344`.

## 6. Docker (untested)

The `Dockerfile` at the repository root is a multi-stage build (Debian
bookworm) producing a slim image that runs as a non-root user with data in the
`/data` volume. **It was written without Docker available and has never been
built or run.** Expect to fix small things.

```sh
docker build -t phylka .

# main network, P2P published, RPC deliberately NOT published
docker run -d --name phylka --restart unless-stopped \
  -p 7333:7333 -v phylka-data:/data phylka

# join a known node
docker run -d --name phylka -p 7333:7333 -v phylka-data:/data phylka \
  node --datadir /data --public --connect seed.example.org:7333

# dev network
docker run -d --name phylka-dev -p 17333:17333 -v phylka-dev:/data phylka \
  node --dev --datadir /data --public

docker logs -f phylka
docker exec phylka phylka wallet balance      # RPC is reachable inside the container only
docker stop phylka                            # sends SIGINT (STOPSIGNAL)
```

Do not add `-p 7334:7334`: the RPC listens on 127.0.0.1 inside the container,
so publishing would not work and, if made to work, would be unauthenticated.
Keep wallets outside the container.

## 7. Mining and staking

### Mining to your address

Create the wallet on your own computer and read the address:

```sh
phylka wallet create                 # main; add --dev for dev
phylka wallet address                # plk1...
```

Start the node with `--mine ADDRESS --threads N`; the address must belong to
the same network (`plk1...` for main, `plkd1...` for dev) or the node refuses
to start. On a VPS, put it into `/etc/default/phylka`:

```sh
PHYLKA_ARGS="--connect seed.example.org:7333 --mine plk1... --threads 2"
```

Only the **address** is on the server, not the wallet. Rewards appear in your
wallet after maturity (100 blocks main, 2 dev); check them through the
tunnel (5.9). The miner is a CPU SHA-256d miner; on main the difficulty
adjusts per block (LWMA) but never drops below `0x1e00ffff`, so a single CPU
will find blocks rarely unless the network is tiny.

### Staking as a validator

Validators lock coins and sign finality votes (main: at least 10 PLK each and
100 PLK in total before finality starts; stake locked for 1008 blocks and
active after 6 blocks).

```sh
phylka wallet stake 10 --rpc 7334      # locks coins from the wallet; add --dev for dev
phylka wallet balance                  # shows the staked amount
phylka node --validator --wallet ~/phylka/wallets/main.wallet --connect ...
phylka wallet unstake                  # after the lock period
```

`--validator` makes the node load the wallet's key and ask for its password
at start-up (a terminal is required), so it **cannot run as an unattended
systemd service**: run it in `tmux`/`screen` on a machine you control, and
understand that the signing key for your stake is then in that machine's
memory. For testing use the dev network, where `PHYLKA_TEST_PASSWORD` can
supply the password. The validator needs the node to be synced and connected
to peers.

## 8. Troubleshooting

| Symptom | Likely cause and fix |
|---|---|
| `cmake: command not found` / version too old | Install CMake 3.20+ (see section 1). |
| Build fails on `<filesystem>` | Compiler too old; use GCC 9+ or recent Clang. |
| `selftest` shows `FAIL` | Do not run the node. Rebuild from clean (`rm -rf build`), check the compiler version, report it. |
| Node printed `Error: ...` and exited | Read the message; common: data dir not writable (`sudo chown -R phylka: /var/lib/phylka`), or a corrupted datadir. |
| `bind` / address already in use | Another node already uses that port. Use distinct `--port`/`--rpc` per node. |
| `node is not running (no reply on 127.0.0.1:...)` from a wallet command | The node is not running, or it uses a different RPC port (`--rpc`), or your SSH tunnel is down. |
| `--mine: invalid address or wrong network` | Dev address (`plkd1`) used on main or vice versa. Pass or omit `--dev` consistently. |
| Peers stay at 0 | Wrong `host:port`; firewall/security group closed on the other side (`nc -vz host 7333`); the other node is not started with `--public` (it then listens on loopback only). |
| Connected but height stays 0 | The peer is on a different network or build (genesis mismatch), e.g. dev vs main, or a fork with another genesis. |
| Node on a VPS unreachable from outside | Missing `--public`, ufw/cloud firewall, or NAT without port forwarding at home. |
| Peer got banned | Bans last 24 hours, are keyed by IP and are held in memory, so restarting the node clears them. Loopback is never banned. |
| Wallet says coins are "maturing" | Wait for 100 blocks (main) or 2 blocks (dev) after the mining reward. |
| `Too short.` (password prompt) | Wallet passwords must be at least 10 characters. |
| `wallet already exists` | The wallet file exists; pick another `--wallet FILE`. |
| "waiting for sync, mining postponed" | The node has no peers or is still below the last checkpoint. Add `--seed`/`--connect` to a reachable node and wait for the sync. |
| "foreign network (different genesis)" | That peer runs a different network (a fork or old build). Use a PHYLKA seed. |
| Docker: build or run errors | The Dockerfile is untested; compare with section 5.2 and fix. |

If a problem remains, collect `phylka selftest` output, your OS and compiler
versions, and the last 100 log lines, and open an issue. Report security
problems privately, as described in [SECURITY.md](../SECURITY.md).
