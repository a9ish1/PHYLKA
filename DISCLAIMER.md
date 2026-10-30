# PHYLKA is beta software

**Status: public beta.** Use it only for testing. Everything below applies to
every version, binary, network and document in this repository until a release
explicitly says otherwise.

## Not yet done

| Area | Status |
|---|---|
| **Testing** | Unit self-tests and local multi-node tests only. No long-running public testnet, no fuzzing campaign, no load testing at scale. |
| **Qualification** | Built and run on macOS. Linux, Windows and Docker builds are untested or only built in CI. No supported-platform matrix. |
| **Validation** | The consensus rules (PoW + finality, slashing, fee share, commitments) have no independent re-implementation and no formal specification review. Edge cases in reorganisation, finality and peer handling may still be wrong. |
| **Audit** | **No external security audit** of the code, the cryptographic construction or the economics. The hybrid Schnorr + ML-DSA-44 scheme and the finality layer are new code. |

## What this means for you

- **Do not store value you cannot afford to lose.** Coins, balances and the
  chain itself may be reset, forked or rendered worthless by a bug or a
  required upgrade.
- **Not an investment.** PLK has no issuer, no promise of value, no
  exchange listing and no support. Nothing in this repository is financial,
  legal or tax advice.
- **No warranty.** The software is provided "as is" under the MIT license
  (see [LICENSE](LICENSE)). The authors are not liable for any loss.
- **Protect your seed.** The 24-word phrase is the only key to your coins.
  Never type it into a website, a chat or a machine you don't control. Nobody
  from the project will ever ask for it.
- **Run nodes at your own risk.** A public node accepts connections from
  strangers. Keep the RPC port (7334) closed to the internet, keep the system
  updated, and don't run a node on a machine that holds other sensitive data.
- **Report problems privately.** Security issues go through
  [SECURITY.md](SECURITY.md), not public issues.

The beta label will be removed only after a long public testnet, independent
review of the consensus rules and an external security audit, each of which
will be linked here.
