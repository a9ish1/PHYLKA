#include "keys.h"

#include <secp256k1.h>
#include <secp256k1_extrakeys.h>
#include <secp256k1_schnorrsig.h>

#include <algorithm>
#include <cstdlib>
#include <sstream>
#include <stdexcept>

extern "C" {
#include "api.h"
extern thread_local const uint8_t* plk_fixed_random;
extern thread_local size_t plk_fixed_random_len;
}

namespace plk {

// ---------------------------------------------------------------- BIP39

namespace bip39 {
static const char* const WORDS[2048] = {
#include "bip39_words.inc"
};

const char* word(int index) { return WORDS[index]; }

static int index_of(const std::string& w) {
    for (int i = 0; i < 2048; i++)
        if (w == WORDS[i]) return i;
    return -1;
}

static std::vector<std::string> split(const std::string& s) {
    std::istringstream in(s);
    std::vector<std::string> out;
    for (std::string w; in >> w;) out.push_back(w);
    return out;
}

std::string from_entropy(const Bytes& entropy) {
    if (entropy.size() % 4 || entropy.size() < 16 || entropy.size() > 32) throw std::invalid_argument("entropy size");
    Hash256 h = sha256(entropy);
    size_t ent_bits = entropy.size() * 8, cs_bits = ent_bits / 32;
    auto bit = [&](size_t i) -> int {
        return i < ent_bits ? (entropy[i / 8] >> (7 - i % 8)) & 1 : (h[(i - ent_bits) / 8] >> (7 - (i - ent_bits) % 8)) & 1;
    };
    std::string out;
    for (size_t w = 0; w < (ent_bits + cs_bits) / 11; w++) {
        int idx = 0;
        for (int b = 0; b < 11; b++) idx = idx << 1 | bit(w * 11 + b);
        if (w) out += ' ';
        out += WORDS[idx];
    }
    return out;
}

std::string generate() {
    Bytes e(32);
    random_bytes(e.data(), e.size());
    std::string m = from_entropy(e);
    wipe(e.data(), e.size());
    return m;
}

bool valid(const std::string& mnemonic) {
    auto words = split(mnemonic);
    if (words.size() % 3 || words.size() < 12 || words.size() > 24) return false;
    std::vector<int> bits;
    for (auto& w : words) {
        int i = index_of(w);
        if (i < 0) return false;
        for (int b = 10; b >= 0; b--) bits.push_back(i >> b & 1);
    }
    size_t cs_bits = bits.size() / 33, ent_bits = bits.size() - cs_bits;
    Bytes ent(ent_bits / 8);
    for (size_t i = 0; i < ent_bits; i++) ent[i / 8] |= bits[i] << (7 - i % 8);
    Hash256 h = sha256(ent);
    for (size_t i = 0; i < cs_bits; i++)
        if (bits[ent_bits + i] != ((h[i / 8] >> (7 - i % 8)) & 1)) return false;
    return true;
}

Bytes to_seed(const std::string& mnemonic, const std::string& passphrase) {
    auto words = split(mnemonic);
    std::string norm;
    for (size_t i = 0; i < words.size(); i++) norm += (i ? " " : "") + words[i];
    std::string salt = "mnemonic" + passphrase;
    Bytes seed = pbkdf2_hmac_sha512(Bytes(norm.begin(), norm.end()), Bytes(salt.begin(), salt.end()), 2048, 64);
    wipe(norm.data(), norm.size());
    return seed;
}
}  // namespace bip39

// ---------------------------------------------------------------- hybrid keys

static secp256k1_context* ctx() {
    static secp256k1_context* c = [] {
        secp256k1_context* x = secp256k1_context_create(SECP256K1_CONTEXT_NONE);
        uint8_t r[32];
        random_bytes(r, 32);
        if (!secp256k1_context_randomize(x, r)) std::abort();   // blinding against side channels
        return x;
    }();
    return c;
}

static const uint8_t MLDSA_CTX[] = {'P', 'H', 'Y', 'L', 'K', 'A'};   // FIPS 204 context string

Hash256 PublicKey::program() const {
    Bytes b{0x01};   // scheme 1 = secp256k1 Schnorr + ML-DSA-44
    b.insert(b.end(), schnorr.begin(), schnorr.end());
    b.insert(b.end(), mldsa.begin(), mldsa.end());
    return sha3_256(b);
}

HybridKey::~HybridKey() {
    wipe(secp_secret_.data(), secp_secret_.size());
    if (!mldsa_secret_.empty()) wipe(mldsa_secret_.data(), mldsa_secret_.size());
}

HybridKey::HybridKey(HybridKey&& o) noexcept
    : secp_secret_(o.secp_secret_), mldsa_secret_(std::move(o.mldsa_secret_)), pub_(std::move(o.pub_)) {
    wipe(o.secp_secret_.data(), o.secp_secret_.size());
}

HybridKey HybridKey::derive(const Bytes& seed, uint32_t index) {
    // node = HMAC-SHA512("PHYLKA hybrid seed", seed || index); left half -> secp256k1, right half -> ML-DSA
    std::string tag = "PHYLKA hybrid seed";
    Bytes msg = seed;
    msg.push_back(index >> 24); msg.push_back(index >> 16); msg.push_back(index >> 8); msg.push_back(index);
    HybridKey k;
    for (uint32_t attempt = 0;; attempt++) {
        Bytes m = msg;
        m.push_back(uint8_t(attempt));
        auto node = hmac_sha512(Bytes(tag.begin(), tag.end()), m);
        wipe(m.data(), m.size());
        std::copy(node.begin(), node.begin() + 32, k.secp_secret_.begin());
        if (secp256k1_ec_seckey_verify(ctx(), k.secp_secret_.data())) {
            Hash256 xi = sha3_256(Bytes(node.begin() + 32, node.end()));
            wipe(node.data(), node.size());
            k.pub_.mldsa.resize(MLDSA_PK);
            k.mldsa_secret_.resize(MLDSA_SK);
            plk_fixed_random = xi.data();
            plk_fixed_random_len = xi.size();
            int rc = PQCLEAN_MLDSA44_CLEAN_crypto_sign_keypair(k.pub_.mldsa.data(), k.mldsa_secret_.data());
            wipe(xi.data(), xi.size());
            if (rc != 0 || plk_fixed_random) std::abort();
            break;
        }
        wipe(node.data(), node.size());
    }
    secp256k1_keypair kp;
    secp256k1_xonly_pubkey xpk;
    if (!secp256k1_keypair_create(ctx(), &kp, k.secp_secret_.data()) || !secp256k1_keypair_xonly_pub(ctx(), &xpk, nullptr, &kp))
        std::abort();
    secp256k1_xonly_pubkey_serialize(ctx(), k.pub_.schnorr.data(), &xpk);
    wipe(&kp, sizeof kp);
    return k;
}

HybridSignature HybridKey::sign(const Hash256& msg) const {
    HybridSignature s;
    secp256k1_keypair kp;
    uint8_t aux[32];
    random_bytes(aux, 32);
    if (!secp256k1_keypair_create(ctx(), &kp, secp_secret_.data()) ||
        !secp256k1_schnorrsig_sign32(ctx(), s.schnorr.data(), msg.data(), &kp, aux))
        std::abort();
    wipe(&kp, sizeof kp);
    s.mldsa.resize(MLDSA_SIG);
    size_t len = 0;
    if (PQCLEAN_MLDSA44_CLEAN_crypto_sign_signature_ctx(s.mldsa.data(), &len, msg.data(), msg.size(), MLDSA_CTX,
                                                        sizeof MLDSA_CTX, mldsa_secret_.data()) != 0 || len != MLDSA_SIG)
        std::abort();
    return s;
}

bool verify(const PublicKey& pk, const Hash256& msg, const HybridSignature& sig) {
    if (pk.mldsa.size() != MLDSA_PK || sig.mldsa.size() != MLDSA_SIG) return false;
    secp256k1_xonly_pubkey xpk;
    if (!secp256k1_xonly_pubkey_parse(ctx(), &xpk, pk.schnorr.data())) return false;
    if (!secp256k1_schnorrsig_verify(ctx(), sig.schnorr.data(), msg.data(), msg.size(), &xpk)) return false;
    return PQCLEAN_MLDSA44_CLEAN_crypto_sign_verify_ctx(sig.mldsa.data(), sig.mldsa.size(), msg.data(), msg.size(),
                                                        MLDSA_CTX, sizeof MLDSA_CTX, pk.mldsa.data()) == 0;
}

}  // namespace plk
