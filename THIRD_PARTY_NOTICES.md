# Third-party notices

PHYLKA's own code (`src/`, `CMakeLists.txt`, `docs/`) is MIT-licensed, see
[LICENSE](LICENSE). The directory `deps/` holds vendored third-party code that
keeps its original license. Nothing here is modified except where noted.

| Component | Path | Upstream | License |
|-----------|------|----------|---------|
| libsecp256k1 (v0.6.0 per its CHANGELOG), Schnorr/BIP340 and extrakeys modules used | `deps/secp256k1/` | https://github.com/bitcoin-core/secp256k1 | MIT (`deps/secp256k1/COPYING`, copyright (c) 2013 Pieter Wuille) |
| ML-DSA-44 (FIPS 204), clean reference implementation | `deps/mldsa44/` | PQClean, https://github.com/PQClean/PQClean (`crypto_sign/ml-dsa-44/clean`) | Public domain / CC0 1.0 (`deps/mldsa44/LICENSE`). The upstream reference code derives from CRYSTALS-Dilithium, which is also available under Apache 2.0. |
| SHA-2 (`sha2.c`), SHA-3/SHAKE (`fips202.c`) and helper headers | `deps/pqcommon/` | PQClean `common/` | Public domain. The file headers state they are based on public-domain code from SUPERCOP (D. J. Bernstein; Ronny Van Keer) and TweetFips202. |
| BIP39 English word list (2048 words) | `deps/bip39-english.txt`, `src/bip39_words.inc` | https://github.com/bitcoin/bips/blob/master/bip-0039/english.txt | Part of BIP-39, which is licensed BSD-2-Clause. SHA-256 of the list file: `2f5eed53a4727b4bf8880d8f3f199efc90e58503646d9ff8eff3a2ed3b24dbda`. |

The GUI wallet (`src/gui/`, `src/gui.cpp`) vendors nothing: its QR code encoder (byte mode, error correction level M,
versions 1-6) is original PHYLKA code written after ISO/IEC 18004 and is covered by the project license.

## libsecp256k1 (MIT)

```
Copyright (c) 2013 Pieter Wuille

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
```

## PQClean ML-DSA-44 and common code (public domain)

The text of `deps/mldsa44/LICENSE` reads:

```
Public Domain (https://creativecommons.org/share-your-work/public-domain/cc0/)

For Keccak and AES we are using public-domain
code from sources and by authors listed in
comments on top of the respective files.
```

## Notes for maintainers

- Only the secp256k1 license file and the PQClean notice are shipped in the
  tree. If upstream license texts for the BIP39 word list or CRYSTALS-Dilithium
  are required by your distribution policy, add them here before release.
- When updating anything under `deps/`, update the version and source in the
  table above.
