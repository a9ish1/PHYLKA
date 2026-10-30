# PHYLKA architecture in diagrams

How the network works, piece by piece. All parameters are the main-network
values from `src/chain.cpp`; the dev network uses the same rules with faster
timings. The byte-level specification is in [PROTOCOL.md](PROTOCOL.md).

- [1. Components](#1-components)
- [2. Keys and addresses](#2-keys-and-addresses)
- [3. Life of a transaction](#3-life-of-a-transaction)
- [4. Block layout](#4-block-layout)
- [5. Proof of work and difficulty](#5-proof-of-work-and-difficulty)
- [6. Validator finality](#6-validator-finality)
- [7. Validator lifecycle](#7-validator-lifecycle)
- [8. Fork choice](#8-fork-choice)
- [9. Network split and recovery](#9-network-split-and-recovery)
- [10. Peer-to-peer sync and discovery](#10-peer-to-peer-sync-and-discovery)
- [11. Upgrade activation](#11-upgrade-activation)
- [12. Deployment](#12-deployment)

## 1. Components

One binary, `phylka`, contains the node, the miner, the validator, the wallet
and a local block explorer.

```mermaid
flowchart LR
    subgraph phylka["phylka binary"]
        CLI["CLI<br/>main.cpp"]
        Wallet["Wallet<br/>wallet.cpp<br/>BIP39 seed, encrypted file"]
        Node["Node core<br/>node.cpp<br/>UTXO set, mempool, fork choice, finality"]
        Chain["Consensus rules<br/>chain.cpp, ext.cpp<br/>blocks, txs, PoW, extension"]
        Net["P2P + RPC<br/>net.cpp<br/>sync, relay, discovery, bans"]
        Miner["CPU miner<br/>SHA-256d midstate"]
        Validator["Validator<br/>checkpoint votes"]
        Explorer["Explorer<br/>explorer.cpp<br/>HTML on 127.0.0.1"]
        Crypto["Crypto<br/>secp256k1 Schnorr<br/>ML-DSA-44, SHA-2, SHA-3"]
    end
    Disk[("datadir<br/>blocks/ undo/ votes/<br/>evidence/ peers.dat")]
    Peers(("other nodes<br/>port 7333"))
    User(("you<br/>127.0.0.1:7334"))

    CLI --> Node
    CLI --> Wallet
    Wallet -- "signed tx via RPC" --> Net
    Miner --> Node
    Validator --> Node
    Node --> Chain
    Chain --> Crypto
    Wallet --> Crypto
    Node <--> Disk
    Net <--> Node
    Net <--> Peers
    Explorer --> Node
    User --> Explorer
```

## 2. Keys and addresses

Everything comes from one BIP39 seed phrase. Every account has two key pairs,
and the address commits to both public keys, so neither can be swapped.

```mermaid
flowchart TD
    Seed["24-word BIP39 phrase<br/>+ optional passphrase"] --> PBKDF["PBKDF2-HMAC-SHA512<br/>BIP39 seed, 64 bytes"]
    PBKDF --> HMAC["HMAC-SHA512 with key 'PHYLKA hybrid seed'<br/>over seed + account index"]
    HMAC --> SK1["left half<br/>secp256k1 secret key"]
    HMAC --> SK2["right half<br/>ML-DSA-44 key generation seed"]
    SK1 --> PK1["Schnorr x-only public key<br/>32 bytes"]
    SK2 --> PK2["ML-DSA-44 public key<br/>1312 bytes"]
    PK1 --> H["SHA3-256 of<br/>0x01 + xonly + mldsa_pk"]
    PK2 --> H
    H --> Prog["32-byte witness program"]
    Prog --> Addr["bech32m address<br/>plk1p...  63 characters"]
```

Spending needs **both** signatures over the same sighash: a Schnorr signature
(64 bytes, BIP340) and an ML-DSA-44 signature (2420 bytes, FIPS 204). Breaking
one scheme is not enough to steal coins.

## 3. Life of a transaction

```mermaid
sequenceDiagram
    autonumber
    actor U as You
    participant W as Wallet
    participant N as Your node
    participant P as Peers
    participant M as Miner
    U->>W: phylka wallet send plk1... 1.5
    W->>N: ask for your UTXOs (RPC, loopback)
    W->>W: build tx, sign every input with Schnorr AND ML-DSA-44
    W->>N: submit signed tx
    N->>N: check inputs exist, amounts, both signatures, fee >= 1 fyl/byte
    N->>P: relay
    P->>M: tx reaches a miner's mempool
    M->>M: pick txs by fee rate, build block, find nonce
    M->>P: new block
    P->>N: block
    N->>N: validate block, update UTXO set, write undo data
    N-->>U: wallet balance / history shows the payment
```

Mined coins (coinbase) can be spent after 100 blocks. Staked coins are locked
for 1008 blocks.

## 4. Block layout

```mermaid
flowchart TB
    subgraph Block
        direction TB
        H["80-byte header<br/>version | prev hash | merkle root | time | bits | nonce<br/>identical layout to Bitcoin, hashed with SHA-256d"]
        T["transactions<br/>coinbase first, then payments, stakes, unstakes"]
        E["extension, from the upgrade height on<br/>key registrations | slashing evidence | finality certificate"]
    end
    H --- T --- E
    CB["coinbase data after the upgrade<br/>height | UTXO-set root | checkpoint commitment | extension hash"]
    T -.contains.-> CB
    CB -.commits to.-> E
```

The header never changes, so any SHA-256 miner could work on PLK through a
stratum bridge. The extension sits outside the header and the merkle tree; the
coinbase commits to its hash, and the merkle root commits to the coinbase.

## 5. Proof of work and difficulty

```mermaid
flowchart LR
    A["block template"] --> B["try nonce"]
    B --> C{"SHA-256d of header<br/>below target?"}
    C -- no --> B
    C -- yes --> D["broadcast block"]
    D --> E["LWMA over the last 45 blocks<br/>recent solve times weigh more"]
    E --> F["next target<br/>never easier than 0x1e00ffff"]
    F --> A
```

- Target spacing: 600 s. Difficulty retargets every block, so a small network
  does not stall when a miner leaves.
- Reward: 50 PLK, halving every 210 000 blocks, hard cap 21 000 000 PLK.
- Chain work is summed exactly as a 256-bit integer.

## 6. Validator finality

Proof of work orders blocks. Validators, who lock stake, make history final.

```mermaid
sequenceDiagram
    participant Miners
    participant Chain
    participant V as Validators
    Miners->>Chain: block 600  (checkpoint, 600 is a multiple of 6)
    Miners->>Chain: blocks 601, 602
    Note over Chain: checkpoint 600 is now 2 blocks deep
    V->>Chain: each validator signs "block 600 = hash" (Schnorr + ML-DSA)
    Note over V: votes spread over P2P
    alt more than 2/3 of the snapshot stake signed
        Chain->>Chain: block 600 is FINAL
        Note over Chain: no reorg below 600, whatever the hash power
        Miners->>Chain: a later block carries the finality certificate
    else 2/3 not reached
        Note over Chain: chain stays proof-of-work only until enough votes
    end
```

| Parameter (main) | Value |
|---|---|
| Checkpoint interval | every 6 blocks, about 1 hour |
| Vote depth | 2 blocks |
| Finality threshold | more than 2/3 of snapshot stake |
| Minimum stake | 10 PLK per validator |
| Finality starts at | 100 PLK total stake |
| Stake lock | 1008 blocks, about 1 week |
| Stake counts after | 6 blocks |
| Voter fee share | 10 % of block fees, split by stake (after the upgrade) |

The **finality certificate** (aggregated votes inside a block) lets a node that
syncs from scratch prove finality from chain data alone.

## 7. Validator lifecycle

```mermaid
stateDiagram-v2
    [*] --> Holder: own PLK
    Holder --> Locked: wallet stake N, at least 10 PLK
    Locked --> Registered: key registration on chain (after the upgrade)
    Registered --> Active: 6 blocks later, enters the stake snapshot
    Active --> Active: signs each checkpoint, earns a share of fees
    Active --> Slashed: signs two different blocks at one height
    Slashed --> [*]: stake frozen forever, excluded from snapshots
    Active --> Unlocked: 1008 blocks after locking
    Unlocked --> Holder: wallet unstake
```

Equivocation (two signatures for different blocks at the same checkpoint
height) is provable by anyone. The evidence goes on chain and the validator's
stake can never be spent again. A validator that stays offline is not
punished; it just earns nothing.

## 8. Fork choice

How a node decides which chain is the real one, in order of priority.

```mermaid
flowchart TD
    S["candidate chain"] --> C1{"matches every<br/>hard-coded checkpoint?"}
    C1 -- no --> R1["rejected"]
    C1 -- yes --> C2{"contains every block<br/>this node knows is final?"}
    C2 -- no --> R2["rejected"]
    C2 -- yes --> C3{"is there a block signed by<br/>more than 2/3 of stake<br/>on another branch?"}
    C3 -- yes --> F["follow that branch<br/>finality beats work"]
    C3 -- no --> W["most total proof of work wins"]
```

Hard-coded checkpoints pin the network to genesis
`00000000df01a464…a70a0`. Nodes also compare the genesis hash in the handshake
and drop peers from any other network.

## 9. Network split and recovery

Tested in `tests/test-splitbrain.sh`.

```mermaid
flowchart LR
    subgraph Before["one network"]
        A0["checkpoint 130 final"]
    end
    subgraph Split["internet split"]
        direction TB
        M["majority side<br/>80 % of stake"] --> M2["keeps finalizing<br/>130 to 145"]
        m["minority side<br/>20 % of stake, MORE hash power"] --> m2["cannot finalize<br/>stuck at 130"]
    end
    subgraph After["rejoined"]
        J["every node follows the majority<br/>checkpoint 145 is in every chain<br/>nobody is slashed"]
    end
    Before --> Split --> After
```

If neither side has more than 2/3 of the stake, neither finalizes during the
split; after rejoining the nodes converge on the chain with more work and
finality resumes.

## 10. Peer-to-peer sync and discovery

```mermaid
sequenceDiagram
    participant New as New node
    participant Seed as Seed node
    participant Other as Other nodes
    New->>Seed: version (height, genesis hash, listen port)
    Seed->>New: version
    alt different genesis
        Seed--xNew: disconnect "foreign network"
    end
    New->>Seed: getaddr
    Seed->>New: addr (up to 100 known public nodes)
    New->>Other: connect, keeps about 8 outbound peers
    Seed->>New: inv (its tip)
    New->>Seed: getblock for each missing block
    Seed->>New: block
    Note over New: validates every block, writes undo data
    Note over New: mining waits until synced past the last checkpoint
    Other->>New: new blocks, txs, votes relayed live
```

Protections: per-peer rate limits, size caps checked before allocation,
32 inbound peers (4 per IP), misbehaviour score with 24 h bans, handshake
timeout. The RPC/explorer port listens on 127.0.0.1 only.

## 11. Upgrade activation

New consensus rules switch on at a fixed height, so the early chain stays
valid exactly as it was mined.

```mermaid
timeline
    title Main network rule set by height
    0 : genesis (reward burned)
      : PoW + finality votes
    12 : first hard-coded checkpoint
    1000 : upgrade activates
         : block extension, UTXO and checkpoint commitments
         : on-chain key registry and finality certificates
         : slashing evidence, 10 % voter fee share
```

## 12. Deployment

```mermaid
flowchart LR
    subgraph VPS["VPS, always on"]
        S["phylka node --public<br/>systemd, user phylka<br/>port 7333 open"]
    end
    subgraph Home["your computer"]
        L["phylka node --seed vps:7333"]
        W["wallet + seed phrase<br/>stays here"]
        B["browser<br/>127.0.0.1:7334"]
    end
    subgraph Others["other participants"]
        O1["node"]
        O2["node"]
    end
    L <--> S
    O1 <--> S
    O2 <--> O1
    W --> L
    B --> L
    You(("admin")) -- "ssh -L 7334:127.0.0.1:7334" --> S
```

Step-by-step setup for every box in this picture: [RUNNING.md](RUNNING.md).
