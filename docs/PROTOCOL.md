# PHYLKA protocol

This document describes the protocol as implemented in `src/` at the time of
writing. Where code and this text disagree, the code wins; please file an issue.
All integers are little-endian unless stated. `H` = SHA-256, `H2` = double
SHA-256 (`sha256d`), `SHA3` = SHA3-256.

## 1. Parameters

| | main | dev |
|-|------|-----|
| HRP | `plk` | `plkd` |
| Magic | `f19c4b01` | `f19c4bde` |
| P2P / RPC port | 7333 / 7334 | 17333 / 17334 |
| Genesis bits | `0x1d00ffff` | `0x207fffff` |
| Easiest allowed bits | `0x1e00ffff` | `0x207fffff` |
| Target spacing | 600 s | 5 s |
| LWMA window | 45 | 20 |
| Halving interval | 210 000 | 150 |
| Initial reward | 50 PLK | 50 PLK |
| Coinbase maturity | 100 | 2 |
| Checkpoint interval | 6 | 5 |
| Finality depth | 2 | 1 |
| Stake lock / activation | 1008 / 6 | 20 / 2 |
| Phase 3 activation height | 1000 | 20 (`--upgrade-height`) |
| Voter fee share | 10% | 10% |
| Hard-coded checkpoints | 0, 12 | none |
| Min stake / min total stake | 10 / 100 PLK | 10 / 10 PLK |

`COIN = 100 000 000 fyl`, `MAX_MONEY = 21 000 000 * COIN`. Reward at height `h`
is `initial >> (h / halving_interval)` (zero after 64 halvings). Main genesis:
time 1793135950, nonce 4000626186, hash
`00000000df01a464bd9f7ad30b88da74059e25ea5b60eccaac0f95f2ba1a70a0`, coinbase
text "Why are we using only classic things in bad times? We need a revolution in
methods." The genesis coinbase pays an unspendable address
`SHA3("PHYLKA genesis: " || text)` and never enters the UTXO set.

## 2. Serialization

`varint` (CompactSize): `< 0xfd` one byte; `0xfd` + u16; `0xfe` + u32. The
`0xff` (u64) form is not accepted.

### Header (exactly 80 bytes, identical layout to Bitcoin)

```
i32 version | 32 prev hash | 32 merkle root | u32 time | u32 bits | u32 nonce
```

Block hash = `H2(header)`. Hashes are displayed byte-reversed.

### Transaction

```
u32 version
varint n_in
  n_in x { 32 prev txid | u32 prev vout | varint len | coinbase_data[len] (<= 100) }
varint n_out
  n_out x { u64 value | u8 address_version | 32 program }
u32 locktime
-- witness part (omitted when computing txid) --
varint n_witness            (== n_in; 0 for coinbase)
  n_witness x { u8 0x01 | 32 schnorr_xonly_pk | 1312 mldsa_pk | 64 schnorr_sig | 2420 mldsa_sig }
```

- `txid = H2(serialization without the witness part)` (the witness count itself
  is also omitted).
- `wtxid = H2(full serialization)`.
- A coinbase has one input with `prev txid = 0` and `prev vout = 0xffffffff`.
  Its `coinbase_data` starts with the block height as u32 and then free text
  (miners append an 8-byte extranonce and `/PHYLKA/`).
- A hybrid input witness is about 3.8 KB, so a typical 1-in/2-out transaction
  is about 3.96 KB.
- Parse limits: at most 10 000 inputs and 10 000 outputs.

### Block

```
header (80) | varint n_tx | transactions (full serialization, with witness)
```

Merkle root: binary tree over `wtxid`s (so it commits to signatures), pair hash
`H2(left || right)`, an odd level duplicates its last node.

## 3. Keys and addresses

Seed: BIP39 mnemonic (24 words from 32 bytes of entropy; 12-24 words accepted),
`seed = PBKDF2-HMAC-SHA512(mnemonic, "mnemonic" || passphrase, 2048, 64)`.

Hybrid key for account `index` (u32):

```
node = HMAC-SHA512(key = "PHYLKA hybrid seed", seed || index_be32 || attempt_byte)   // attempt = 0,1,...
secp256k1 secret = node[0..32]            // retry with attempt+1 if not a valid secret
ML-DSA-44 keygen seed (xi) = SHA3(node[32..64])
```

ML-DSA-44 is the FIPS 204 parameter set (public key 1312 B, signature 2420 B,
secret key 2560 B). Signing uses the context string `PHYLKA`.

Address program and string:

```
program = SHA3( 0x01 || schnorr_xonly_pk (32) || mldsa_pk (1312) )     // 32 bytes
address = bech32m( hrp, [version] || convert_bits(program, 8 -> 5, pad) )
```

Address versions: `1` (character `p`) = spendable hybrid address, `2` (`z`) =
validator stake address (same keys, coins locked as stake). Main addresses look
like `plk1p...` (63 characters). Checksum constant is the BIP350 bech32m
constant `0x2bc830a3`; mixed case is rejected.

## 4. Signatures and validity of a spend

For input `i` of transaction `T` spending outputs `spent[]`:

```
sighash = tagged_hash("PHYLKA/sighash",
              txid(32) || u32 i || varint len(spent) || for each spent: u64 value | u8 version | 32 program)
tagged_hash(tag, data) = H( H(tag) || H(tag) || data )
```

The witness is valid iff (a) `SHA3(0x01 || pk_schnorr || pk_mldsa)` equals the
program of the spent output, (b) the BIP340 Schnorr signature verifies over
`sighash`, **and** (c) the ML-DSA-44 signature (context `PHYLKA`) verifies over
`sighash`. Both signatures are required.

Transaction rules: non-empty inputs and outputs; witness count equals input
count; every output value in `1..MAX_MONEY` and known address version; no
duplicate inputs; inputs must exist in the UTXO set; sum(in) >= sum(out), the
difference is the fee; coinbase outputs need `maturity` blocks; stake outputs
(version 2) cannot be spent before `stake_lock` blocks. Standalone (relayed)
transactions are limited to 200 000 bytes and may not be coinbase.

## 5. Proof of work and difficulty

PoW: interpret `H2(header)` as a 256-bit little-endian number; valid iff it is
`<=` the target decoded from `bits` (Bitcoin compact encoding; negative
targets invalid).

Block validity (header): parent known and not invalid, PoW valid, `bits`
equals the computed next bits, `time > median(time of last 11 blocks)`,
`time <= now + 7200`, first transaction is the only coinbase, its height field
equals the block height, merkle root matches, serialized size `<= 4 000 000`.
The coinbase may claim at most `reward(height) + fees`.

Difficulty (LWMA, per block): for heights `< 2` use genesis bits. Otherwise
`n = min(window, tip_height)` over the last `n` blocks `i = 1..n` (oldest to
newest):

```
st_i      = clamp(time_i - time_(i-1), 1, 6*T)
weighted  = sum(i * st_i)
avg_target = sum( target_i / n )
k         = n(n+1)/2 * T
next      = min( avg_target * weighted / k , easiest_allowed_target )
```

`T` is the target spacing. Fork choice is the valid chain with the most
accumulated work (`sum 2^256 / (target+1)`), restricted by finality (Section 6).

## 6. Validator finality

- Every block whose height is a multiple of `checkpoint_interval` is a
  checkpoint. For each checkpoint on the active chain a stake snapshot is
  taken from the UTXO set before the checkpoint block is applied: all unspent
  outputs with address version 2 whose creation height `+ stake_activation <=`
  checkpoint height, summed per program; validators below `min_stake` are
  dropped.
- A validator with stake votes once the checkpoint is `finality_depth` blocks
  deep. Vote message:

  ```
  vote_msg = tagged_hash("PHYLKA/vote", u32 height || 32 block hash)
  ```

  signed with the same hybrid scheme (Schnorr + ML-DSA-44). The validator is
  identified by `SHA3(0x01 || pks)` of its keys, i.e. the stake address program.
- Vote serialization (3864 bytes):
  `u32 height | 32 block | 32 schnorr_pk | 1312 mldsa_pk | 64 schnorr_sig | 2420 mldsa_sig`.
- A vote is accepted if the height is a nonzero multiple of the interval, the
  signature verifies, the block is on our active chain at that height and the
  voter has stake in that snapshot. The first valid vote per (height,
  validator) is kept. A second valid signature for a different block at the same
  height is equivocation: that validator's votes are ignored from then on.
- A checkpoint is **final** when the snapshot's total stake is at least
  `min_total_stake` and the stake that voted for the block exceeds 2/3 of the
  total (`voted*3 > total*2`). The highest such checkpoint is the finalized
  block (persisted in `finality.dat`).
- Chains that do not contain the finalized block (and are not an ancestor of
  it) are never activated, so finalized history cannot be reorganised.
- Below `min_total_stake` the chain is pure proof of work.

## 6a. Phase 3 rules (active from `upgrade_height`)

Blocks with height `< upgrade_height` are validated exactly as before and must not
carry an extension (coinbase data is limited to 100 bytes there). From
`upgrade_height` on:

**Block extension.** After the transactions the block carries `varint len | ext`
(outside the 80-byte header and the merkle tree). The block is invalid without it.

```
ext  := varint n_reg | n_reg x { 32 schnorr_xonly | 1312 mldsa_pk }      (<= 8)
        varint n_ev  | n_ev  x evidence                                   (<= 4)
        u8 has_cert  | [ u32 cp_height | 32 cp_hash | varint n | n x { 32 vid | 64 schnorr | 2420 mldsa } ]
evidence := u32 height | 32 vid | 32 block_a | sig_a | 32 block_b | sig_b    (5068 bytes)
```

**Commitments.** The coinbase data is `u32 height | 32 utxo_root | 32 ckpt_commit | 32 ext_hash | free text`.
- `utxo_root` = SHA3-256 of `"PHYLKA/utxo"` followed by, for every unspent output in
  ascending `(txid, vout)` order: `txid | u32 vout | u64 value | u8 version | 32 program | u32 height | u8 coinbase`.
  It commits to the UTXO set *before* this block (a block cannot commit to a set that contains its own coinbase txid).
  Recomputed in full for each block (O(n)): simple and auditable, not incremental.
- `ckpt_commit` = tagged hash `"PHYLKA/ckpt"` of `(u32 height, 32 hash)` of the latest checkpoint certified on the chain
  so far (`(0, genesis)` if none).
- `ext_hash` = tagged hash `"PHYLKA/ext"` of the extension bytes. The coinbase is in the merkle tree, so the block hash fixes the extension.

**Key registry.** A registration `{xonly, mldsa_pk}` is valid if the key is not yet registered and its program owns at least
`min_stake` of stake outputs in the UTXO set before the block. The validator id is the program (`SHA3(0x01 || pks)`), which is
fork-independent. Votes for checkpoints at height `>= upgrade_height` are *compact* (`u32 height | 32 block | 32 vid | 64 | 2420`,
2552 bytes) and are verified with the registry key; full-form votes (3864 bytes) are refused there and compact ones below.
Registrations, evidence and certificates are all checked against the state *before* the block.

**Finality certificate (optional per block).** A block may carry a certificate for a checkpoint `>= upgrade_height`, newer than the
latest certified one, at least `finality_depth` deep, on the block's own chain. Votes are sorted by id, each from a registered
validator in that checkpoint's stake snapshot with a valid hybrid signature; their stake must exceed 2/3 of the snapshot total
(and total `>= min_total_stake`). Miners include one as soon as they know a newer finalized checkpoint, but it is not mandatory
(a block must not become invalid because a miner has not seen votes yet). A node that replays only the blocks therefore learns
finality from chain data alone (`chain_final` in `/info`).

**Voter fee share.** In a block with a certificate, `pool = fees * 10 / 100` (fees of the block's transactions, rounded down) is split
among the certificate's voters pro rata to their snapshot stake (`pool * stake / voted_stake`, rounded down, 128-bit intermediate,
zero shares omitted). The *last* coinbase outputs must be exactly these shares, paid to the hybrid address of each voter in
certificate order; the coinbase total still may not exceed `reward + fees`, so the 21M cap is untouched (the miner keeps rounding dust).

**Slashing.** Evidence = two valid signatures of one registered validator on different blocks at the same checkpoint height.
When included, the validator is recorded as slashed: it is excluded from all later stake snapshots and every version-2 output of its
program can never be spent (frozen, equivalent to burned; the coins stay in the UTXO set so the supply is unchanged). Nodes also
persist locally detected equivocators (`evidence/`) and ignore their votes. The evidence submitter gets no reward (limitation).

**Vote buffering.** Votes for checkpoint blocks that are not on the active chain are kept in a bounded pool (at most 4 off-chain
blocks per height, 20 000 votes, heights above the final one and at most 10 checkpoints ahead, and only from validators of our own
latest snapshot) and are counted automatically if their branch becomes active.

**Network lock.** `ChainParams::checkpoints` (main: height 0 and 12): a block at such a height with another hash is invalid, and a
chain that diverges below the highest checkpoint is never activated. The P2P `version` payload is now
`u32 height | u64 nonce | 32 genesis hash | u16 listen port` (46 bytes; the 44-, 12- and 4-byte forms are still parsed). The listen port is non-zero only for nodes started with `--public` (and on the dev network, which also allows loopback peers); a peer announcing another genesis
hash is disconnected ("foreign network (different genesis)").

## 7. P2P protocol (TCP)

Peer discovery: a peer that announces a listen port is remembered (a fresh announcement is also relayed to two other peers). A node
keeps about 8 outbound connections, topping up from the address book every ~5 s after dialling `--connect`/`--addnode`/`--seed`
and the built-in seeds. Mining guard: a node does not mine while its tip is below the highest hard-coded checkpoint, or (main only)
while it has no peers and the tip is older than 24 h; it logs "waiting for sync, mining postponed". `--mine-anyway` (dev only) disables
the guard.

Message framing:

```
magic (4) | command (12, NUL padded) | u32 payload length | checksum (4) | payload
checksum = first 4 bytes of H2(payload)
```

| Command | Payload | Meaning |
|---------|---------|---------|
| `version` | `u32 height` + `u64 nonce` + `32 genesis hash` (the 12- and 4-byte forms are accepted) | Must be the first message, within 10 s. The nonce detects self and duplicate connections. |
| `getblock` | `u32 height` | Request the block at that height of the sender's active chain. |
| `block` | raw block | Validated; a missing parent is requested by height, walking back to a fork point. |
| `inv` | `u32 height` + `32 hash` | Announces a new tip. |
| `tx` | raw transaction | Mempool admission and relay. |
| `vote` | vote, full (3864 B) or compact (2552 B) (see Sections 6, 6a) | Checked against the stake snapshot, relayed. |
| `getaddr` | empty | Sent once after the handshake. Reply: `addr` with up to 100 random entries seen in the last 30 days. |
| `addr` | `u8 n` (<= 100) + n x `{4 IPv4 | u16 port | u32 last seen}` | Fills the address book (`peers.dat`, at most 1000 entries, least recently seen evicted). Loopback, private, link-local, CGNAT and multicast addresses are ignored on main. Rate limit 1 per 5 s (burst 3). |
| `reg` | 1344-byte key registration (xonly + ML-DSA pk) | Queued for the next block (upgrade only; needs stake). |
| `evid` | 5068-byte evidence | Verified against the registry, queued for an on-chain slash. |
| `getvotes` | `u32 height` | Reply with known votes from that height (at most 500, oldest first). |
| `ping` / `pong` | `u64 nonce` | Keep-alive every 120 s; 300 s of silence drops the peer. |

Payload size caps (checked before allocation): version 46, getaddr 0, addr 1001, reg 1344, evid 5068, getblock 4,
getvotes 4, inv 36, ping/pong 8, vote 4096, tx 200 000, block 4 000 000;
unknown commands 1024 and are scored as misbehaviour. A wrong magic, an
oversized payload or a bad checksum scores 100 (immediate ban). Peers with a
score of 100 or more are disconnected and their IP is banned for 24 hours
(loopback is never banned). Rate limits (token buckets) apply to `tx`, `vote`,
`getblock` and `getvotes`. At most 32 inbound connections, 4 per IP. The node
listens on loopback unless started with `--public`.

## 8. RPC and explorer (HTTP on 127.0.0.1)

Served on the RPC port (7334 main, 17334 dev), loopback only, no authentication,
`GET` only, HTTP/1.0. Wallet endpoints return plain text:

| Endpoint | Response |
|----------|----------|
| `/info` | key/value lines: network, height, tip, time, difficulty, next_bits, mempool, finalized, validators, peers, hashrate, maturity, upgrade_height, chain_final, registered, slashed, utxo_commit |
| `/utxos?addr=ADDR` | one line per unspent output of the address: `txid vout value conf is_coinbase is_mature address_version` (outputs already spent in the mempool are hidden) |
| `/history?addr=ADDR` | lines `height time txid delta kind` (`kind` = `mined` or `tx`; unconfirmed entries start with `pending`) |
| `/send?tx=HEX` | submits a serialized transaction; `ok TXID` or `error REASON` |
| `/block?height=N` | header fields, tx count, coinbase text and reward |

Explorer: `/` (or `/explorer`) serves a single-page web UI, with JSON under
`/api/info`, `/api/blocks?count=&before=`, `/api/block?h=HEIGHT|HASH`,
`/api/tx?id=` and `/api/address?a=`.


## 9. Wallet file (`*.wallet`)

```
"PLKW1" | u32 BE iterations (210000) | 16 salt | 16 nonce | ciphertext | 32 tag
(enc_key, mac_key) = PBKDF2-HMAC-SHA512(password, salt, iterations, 64)
ciphertext = (mnemonic || "\n" || passphrase) XOR SHAKE256(enc_key || nonce)
tag        = first 32 bytes of HMAC-SHA512(mac_key, everything before the tag)
```

The tag is checked before decryption. Companion files: `*.wallet.idx` (number
of derived addresses) and `*.wallet.addr` (cached public addresses, no
secrets).
