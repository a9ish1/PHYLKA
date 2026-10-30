// PHYLKA keys. The seed phrase (BIP39) is the one and only root of an account.
// From it each account index gets a hybrid key pair: secp256k1 Schnorr (BIP340) + ML-DSA-44 (FIPS 204).
// A spend is valid only if BOTH signatures verify, so it stays safe if either scheme is ever broken.
#pragma once
#include <string>
#include <vector>

#include "crypto.h"

namespace plk {

constexpr size_t MLDSA_PK = 1312, MLDSA_SK = 2560, MLDSA_SIG = 2420;
constexpr size_t SCHNORR_PK = 32, SCHNORR_SIG = 64;

namespace bip39 {
/// 24 words from 32 bytes of entropy (16 bytes -> 12 words)
std::string from_entropy(const Bytes& entropy);
/// New random 24-word phrase
std::string generate();
/// Checks words and checksum
bool valid(const std::string& mnemonic);
/// PBKDF2-HMAC-SHA512, 2048 rounds, salt "mnemonic" + passphrase (standard BIP39)
Bytes to_seed(const std::string& mnemonic, const std::string& passphrase = "");
const char* word(int index);
}  // namespace bip39

struct PublicKey {
    std::array<uint8_t, SCHNORR_PK> schnorr{};   // x-only secp256k1 key
    Bytes mldsa;                                 // 1312 bytes
    /// Address program: SHA3-256 over the scheme id and both keys
    Hash256 program() const;
};

struct HybridSignature {
    std::array<uint8_t, SCHNORR_SIG> schnorr{};
    Bytes mldsa;                                 // 2420 bytes
};

class HybridKey {
public:
    /// Deterministic key for account `index` of a BIP39 seed
    static HybridKey derive(const Bytes& seed, uint32_t index);
    ~HybridKey();
    HybridKey(HybridKey&&) noexcept;
    HybridKey(const HybridKey&) = delete;
    HybridKey& operator=(const HybridKey&) = delete;

    const PublicKey& pub() const { return pub_; }
    HybridSignature sign(const Hash256& msg) const;

private:
    HybridKey() = default;
    std::array<uint8_t, 32> secp_secret_{};
    Bytes mldsa_secret_;
    PublicKey pub_;
};

bool verify(const PublicKey& pk, const Hash256& msg, const HybridSignature& sig);

}  // namespace plk
