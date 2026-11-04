# PIP-0001: One hybrid signature per key in a transaction

| | |
|---|---|
| **Status** | Implemented and tested on the dev network. Disabled on every network until an activation height is chosen |
| **Type** | Consensus (hard fork, height-activated) |
| **Affects** | Transaction validation, sighash, mempool policy, wallet, GUI, docs |
| **Does not affect** | Block header, PoW, finality votes, key derivation, addresses, wallet file, P2P messages, block and transaction encoding |

## 1. Summary

Today every input carries its own public keys and its own Schnorr and ML-DSA-44
signatures, even when all inputs belong to the same key. This proposal adds
transaction version 2, in which each distinct key in a transaction appears once
and signs once, with the signature covering all inputs and outputs.

No new cryptography is involved. Both signature schemes stay mandatory, and
their parameters and encodings do not change.

## 2. Motivation

An input is 3866 bytes. Of those, 3829 bytes (99.0 %) are the witness:

```
input:    32 prev txid + 4 prev vout + 1 empty coinbase_data length = 37 bytes
witness:  1 scheme + 32 Schnorr key + 1312 ML-DSA key
          + 64 Schnorr signature + 2420 ML-DSA signature          = 3829 bytes
```

Better serialization cannot help much, because only 37 bytes per input are not
keys and signatures. Removing repeated witnesses can.

Size of a transaction with 2 outputs, `n` inputs and `k` distinct keys:

```
legacy:  93 + 3866 * n
v2:      93 + 37 * n + 3829 * k
```

| Inputs | Distinct keys | Legacy, bytes | v2, bytes | Saved |
|---:|---:|---:|---:|---:|
| 1 | 1 | 3 959 | 3 959 | 0 % |
| 2 | 1 | 7 825 | 3 996 | 48.9 % |
| 10 | 1 | 38 753 | 4 292 | 88.9 % |
| 50 | 1 | 193 393 | 5 772 | 97.0 % |
| 100 | 1 | 386 693 | 7 622 | 98.0 % |
| 10 | 2 | 38 753 | 8 121 | 79.0 % |
| 10 | 10 | 38 753 | 38 753 | 0 % |

The gain depends entirely on how many inputs share a key. There is no gain for a
transaction with one input or with all keys different. These are calculated
sizes; real distributions must be measured (section 11).

The main beneficiaries are miners and anyone consolidating many small outputs
of one address. The current 200 000-byte relay limit allows at most 51 legacy
inputs, so such consolidation cannot even be relayed as one transaction.

## 3. Specification

The key words MUST and MUST NOT are normative. `H` is the activation height
(section 7). Rules depend on the height of the block containing the transaction;
for the mempool that is tip height + 1.

### 3.1 Transaction versions

Below `H`, nothing changes. Consensus does not check the version field today, so
a transaction with `version = 2` below `H` remains valid under the legacy rules
and MUST keep validating that way during replay.

From `H` on, for every non-coinbase transaction:

- `version = 1`: legacy rules, unchanged;
- `version = 2`: the rules of this document;
- any other version: invalid.

The coinbase is not affected.

### 3.2 Encoding

The encoding does not change. A version 2 witness list uses the existing format:

```
varint n_witness
  n_witness x { u8 0x01 | 32 schnorr_pk | 1312 mldsa_pk | 64 schnorr_sig | 2420 mldsa_sig }
```

Only the meaning of the list and the value of `n_witness` differ. `txid` and
`wtxid` are computed as before.

### 3.3 Key groups

The list of groups is derived from the outputs being spent. It is not encoded:

```
groups = []
for each input i, in order:
    p = spent[i].to.program
    if p is not in groups: append p
```

Grouping uses the program only. Version 1 and version 2 (stake) outputs of one
key share a program, and so share a group. The address version is still covered
by the signature through the spent list.

### 3.4 Signature message

```
spent_hash = tagged_hash("PHYLKA/spent/v2",
                 varint len(spent) || for each spent: u64 value | u8 version | 32 program)

sighash_v2(p) = tagged_hash("PHYLKA/sighash/v2",
                 genesis_hash(32) || txid(32) || spent_hash(32) || p(32))
```

- `txid` commits to the version, all inputs, all outputs and the locktime.
- `spent_hash` commits to the amount and address of every output spent.
- `genesis_hash` binds the signature to one network. Legacy signatures do not
  include it.
- `p` separates the messages of different keys in one transaction.

`spent_hash` is computed once per transaction, so hashing stays linear in the
number of inputs plus groups.

### 3.5 Validity

A version 2 transaction is valid only if all of these hold:

1. All rules of the legacy context check still apply (inputs exist, no duplicates,
   maturity, stake lock, slashed stake, amounts, known address versions), except
   the rule that the witness count equals the input count.
2. `n_witness == len(groups)`. In particular, `n_witness` MUST NOT be 0.
3. For every `j`, `witness[j].key` hashes to `groups[j]`, with the existing
   derivation `SHA3-256(0x01 || schnorr_pk || mldsa_pk)`.
4. For every `j`, **both** the BIP340 signature and the ML-DSA-44 signature
   (context `PHYLKA`) in `witness[j]` verify over `sighash_v2(groups[j])`.

The witness order is forced by the input order, and each key appears exactly once,
so a given set of signatures has exactly one valid encoding.

### 3.6 Fee and limits

- Consensus limits do not change: 4 MB blocks, at most 10 000 inputs and outputs.
- The relay limit stays at 200 000 bytes and the minimum fee at 1 fyl per byte,
  computed on actual size.
- Wallet fee estimate becomes `(100 + 37 * n + 3829 * k) * rate`.

## 4. Wallet behaviour

- Below `H`, wallets keep creating version 1 transactions. They MUST read the tip
  height from the node to decide, and allow for reorganisations around `H`.
- From `H` on, they create version 2 only, including for single-input spends, so
  that the version does not distinguish transactions.
- Signing needs each key once, instead of once per input.
- Coin selection MAY prefer inputs of already selected keys, but MUST NOT reuse an
  address only to get a lower fee. See section 6.

## 5. Security

**Authorization.** A legacy witness signs a message containing the `txid`, the input
index and the full spent list. A v2 witness signs the `txid`, the spent list, its
own program and the network. It therefore authorizes the same transaction with the
same amounts, and adds network separation. Removing the input index does not let a
signature move to another transaction or another key.

**Forgery.** Spending still requires forging both schemes for the target key under
their standard unforgeability assumptions. This proposal does not claim a stronger
or formally proven composition. That remains a subject for the external audit.

**Domain separation.** New tags keep v2 messages distinct from legacy signatures and
from finality votes, which have their own message format.

**Malleability.** `txid` excludes the witness, as it does today. Section 3.5 removes
encoding freedom. A signer can still produce a different valid signature, which
changes `wtxid` but not `txid`, also as today.

**All-or-nothing signing.** One signature covers all of a key's inputs. Legacy signatures
already commit to the whole transaction, so no partial-signing capability is lost.

## 6. Costs and risks

- **Hard fork.** Nodes that do not upgrade reject v2 transactions and any block
  containing them, so they split from the network at the first such block.
- **Cheap inputs and block load.** Same-key inputs cost 37 bytes, so a 4 MB block can
  hold about 108 000 inputs instead of about 1 034. Coin lookups, UTXO changes and
  undo data grow accordingly. This must be benchmarked before activation (section 11);
  if needed, add a consensus limit on inputs per block.
- **Relay.** One transaction can carry up to about 5 299 same-key inputs instead of 51.
- **Worst case is unchanged.** With all keys different, signature count and size match
  legacy.
- **Fee incentive for address reuse.** Inputs of one address become much cheaper
  than inputs of different addresses. This rewards reuse, which links payments on chain
  and exposes the public key after the first spend. This risk has no consensus fix;
  wallet defaults and documentation must address it.
- **Bandwidth, not verification of history.** Old blocks keep their size.

## 7. Activation

A new parameter is added, independent of `upgrade_height`, because released nodes
already enforce that value:

```
ChainParams::sigv2_height    main: to be decided at release, dev: per test network
```

- **Main.** `H` MUST leave at least 2016 blocks (about 2 weeks) between the release and
  activation, and MUST be above the height of the last hard-coded checkpoint.
- **Dev.** Disabled by default, like main. The public dev network is already past low
  heights: choose `H` above its tip, or reset that network. `--sigv2-height N` is allowed
  only with `--dev`, and every node of that network must use the same value.
- Until a height is chosen, `sigv2_height` is the maximum value and the rules cannot apply.
  Nodes report the value in `/info`, and wallets read it from there.

Blocks below `H` validate exactly as before. Mempool entries that become invalid after a
reorganisation below `H` are dropped by the existing mempool reload.

## 8. Wording of the signature dogma

[WHY.md](../WHY.md) already says "every spend carries two signatures", which stays true
in the sense of this document, but other places say "on every input". The proposed rule is:

> Both Schnorr and ML-DSA-44 signatures are mandatory for every key that spends in a
> transaction. There is no classical-only spend.

The security property is unchanged, but the literal wording changes. The maintainer
accepted this wording on 2026-11-04. It takes effect only when this proposal activates;
until then the current wording stays in the documentation.

## 9. Implementation map

Line numbers refer to the code at the time of writing.

| File | Change |
|---|---|
| `src/chain.h`, `src/chain.cpp:130-153` | Add `sighash_v2`, group derivation, signing and verification |
| `src/chain.cpp:108` | Parser already accepts `n_witness <= n_in`; no format change |
| `src/node.cpp:704-706` | Height-dependent witness count and version rule in `check_tx_context` |
| `src/node.cpp:737-745`, `:760`, `:1144`, `:1178` | Pass height and genesis to signature checks |
| `src/wallet.cpp:291` | New fee estimate |
| `src/wallet.cpp:318` | Sign per key, choose the version from the tip height |
| `src/gui.cpp` | Fee preview through shared wallet functions |
| `src/chain.cpp` (`ChainParams`) | `sigv2_height` for main and dev |
| `docs/PROTOCOL.md`, `README.md`, `docs/ARCHITECTURE.md`, `docs/WHY.md` | Updated rules and sizes |

## 10. Test plan

**Selftest, consensus rules:**

- Below `H`: version 2 with a legacy witness list is valid; a grouped list is invalid.
- From `H`: one key with many inputs; two keys in interleaved input order; one key
  spending both hybrid and matured stake outputs.
- Rejected: too few or too many witnesses, zero witnesses, swapped witness order,
  a duplicate key, a key not matching the group.
- Rejected: only Schnorr valid; only ML-DSA valid; a signature from another transaction;
  a changed output, amount or spent address version; a legacy signature in a v2 transaction;
  a v2 signature from another network; versions 0 and 3.
- Encode-decode round trip; fixed test vectors for `spent_hash` and `sighash_v2`.

**Multi-node, dev network:**

- Activation boundary: blocks at `H-1` and `H`, in both directions.
- A reorganisation above `H` returns grouped transactions to the mempool and leaves UTXO
  state identical on all nodes.
- Crossing `H` upward removes legacy-signed version 2 transactions from the mempool.
- **Not covered by a test:** a reorganisation from above `H` to a tip below it. It needs a
  heavier chain with fewer blocks, which is hard to produce at dev difficulty. The mempool
  code re-verifies version 2 transactions when the rules flip in either direction.
- An old node splits at the first v2 block, and an upgraded node keeps following
  the upgraded chain.
- The existing suite in `tests/` still passes, including split-brain and finality.
- Stake lock and slashing still apply to grouped stake inputs.

## 11. Measurements

Run with `tests/bench-pip1.cpp` (`-DPLK_BUILD_BENCH=ON`) on one Intel Mac, dev parameters,
one run per case. Treat the timings as a rough scale. They do not include network
transfer or a slower disk.

| 4 MB block | Inputs | Transactions | Connect | Reorganisation | Undo data |
|---|---:|---:|---:|---:|---:|
| Legacy | 1 034 | 1 | 513 ms | 20 ms | 47.6 KB |
| Same-key version 2 | 106 944 | 11 | 1 160 ms | 272 ms | 4.9 MB |

- A full block of cheap inputs takes about twice as long to connect, well under
  the block interval. On this evidence no input limit per block is proposed.
- Undo data is about 46 bytes per input. A chain of such blocks would add about 4.9 MB
  of undo data per block. That needs existing outputs and fees to sustain, and there is
  no undo pruning yet.
- Enabling phase 3 commitments did not change the picture (1 108 ms connect in one run).
- A real wallet transaction on the dev network with 13 inputs of one key and 2 outputs took
  4 403 bytes, matching the formula. Legacy would need 50 351.

**Still open before activation:** the actual saving depends on real input and key
distribution, and the chains have almost no spending yet to measure.

## 12. Out of scope

- Merkle batch authorization of several transactions.
- Signature aggregation across different keys. ML-DSA has no standard aggregation.
- On-chain registry of spending keys.
- Fee discount or weight for witness data.
- Stealth addresses, confidential amounts and multisig escrow.

Each of these needs its own proposal.
