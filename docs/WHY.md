# Why PHYLKA: dogmas, differences, trade-offs

## Dogmas

These rules are fixed. They are the identity of the network, and no upgrade or
fork that keeps the name PHYLKA may change them.

1. **The seed is the wallet.** A standard BIP39 24-word phrase restores every
   key and address. Key derivation (HMAC-SHA512 from the BIP39 seed) never
   changes, so a phrase written on paper today still opens its coins in any
   future version.
2. **Long addresses.** An address is a 63-character bech32m string `plk1…`. It
   commits to the *whole* hybrid key: `SHA3-256(0x01 || schnorr_xonly ||
   mldsa44_pk)`. The address never gets shortened to save space, because the
   commitment is what makes it safe.
3. **Post-quantum from genesis.** Every spend carries two signatures, Schnorr
   (BIP340, secp256k1) and ML-DSA-44 (FIPS 204), and both must verify. There is
   no "classical-only" output type to migrate away from later.
4. **Hard money.** 21 000 000 PLK cap, 50 PLK reward, halving every 210 000
   blocks, 10-minute blocks. No premine: the genesis reward goes to a burn
   address nobody can spend. No dev tax, no inflation switch.
5. **One chain.** The main network is the chain that starts at genesis
   `00000000df01a464bd9f7ad30b88da74059e25ea5b60eccaac0f95f2ba1a70a0`.
   Nodes refuse peers with another genesis, and hard-coded checkpoints pin the
   early history.

## How it differs

| | Bitcoin | Ethereum | PHYLKA |
|---|---|---|---|
| Ledger model | UTXO | accounts | UTXO |
| Consensus | PoW (longest chain by work) | PoS | PoW **plus** stake-weighted finality checkpoints |
| Finality | probabilistic (wait N blocks) | economic finality (~13 min) | probabilistic between checkpoints, absolute after a > 2/3-stake checkpoint |
| Signatures | ECDSA / Schnorr | ECDSA | Schnorr **and** ML-DSA-44 on every input |
| Quantum resistance | none yet (migration debated) | none yet | from block 0 |
| Address | 26–62 chars, several types | 42 chars hex | one type, 63 chars bech32m |
| Wallet backup | BIP39 (convention) | BIP39 (convention) | BIP39 (consensus-level dogma) |
| Mining hardware | SHA-256 ASICs | — | the same SHA-256d over an 80-byte header, so ASICs work in principle via a stratum bridge |
| Smart contracts | limited script | full EVM | none, payments only |

## Advantages

- **Quantum safety is not a future fork.** If Schnorr/ECDSA breaks, ML-DSA
  still protects every coin. If ML-DSA turns out weak, Schnorr still does.
  An attacker must break both.
- **Reorg protection that PoW alone does not give.** Once validators finalize a
  checkpoint, no amount of hash power can rewrite history below it. This makes
  a 51 % attack on a small young chain much less useful.
- **Familiar mining.** Same hash function and header layout as Bitcoin, LWMA
  difficulty that reacts every block, so small networks don't stall when miners
  leave.
- **Simple to audit.** About 4–5 thousand lines of C++, no virtual machine, no
  account state, deterministic UTXO rules.
- **One seed, one format.** No legacy address types, no "which derivation path?"
  confusion.

## Honest trade-offs

- **Big transactions.** ML-DSA-44 signatures (2420 bytes) and public keys
  (1312 bytes) make an input about 3.9 KB, roughly 25× a Bitcoin input. With
  4 MB blocks that is on the order of 1 000 inputs per block, about
  1–2 transactions per second. Fees per byte must account for this.
- **Experimental.** No external security audit. The finality layer is new code.
- **No smart contracts.** By design: payments only.
- **Beta.** See [DISCLAIMER.md](../DISCLAIMER.md): not yet tested at scale, qualified, validated or audited.
- **Early mining.** The founder mined the first blocks of the main network
  before public release (visible on-chain, starting at block 1). Nothing was
  pre-allocated in genesis; those coins came from ordinary block rewards.
- **Small network.** Until many independent nodes and validators exist, the
  network depends on a few seed nodes.
