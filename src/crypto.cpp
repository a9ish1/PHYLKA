#include "crypto.h"

#include "platform.h"

#if defined(__APPLE__) && !defined(PLK_PORTABLE_SHA)
#define PLK_USE_COMMONCRYPTO 1
#include <CommonCrypto/CommonDigest.h>
#endif

#include <cstdlib>
#include <cstring>
#include <stdexcept>

extern "C" {
#include "fips202.h"
#include "sha2.h"
}

namespace plk {

#ifdef PLK_USE_COMMONCRYPTO
// SHA-256 from macOS CommonCrypto (optimised for the CPU); same digest as the PQClean reference
Hash256 sha256(const uint8_t* data, size_t len) {
    Hash256 h;
    CC_SHA256(data, CC_LONG(len), h.data());
    return h;
}

static_assert(sizeof(CC_SHA256_CTX) <= 128, "midstate buffer too small");

HeaderHasher::HeaderHasher(const Bytes& header) {
    auto* c = reinterpret_cast<CC_SHA256_CTX*>(mid_);
    CC_SHA256_Init(c);
    CC_SHA256_Update(c, header.data(), 64);
    std::memcpy(tail_, header.data() + 64, 16);
}

HeaderHasher::~HeaderHasher() {}

Hash256 HeaderHasher::hash(uint32_t nonce) {
    CC_SHA256_CTX c;
    std::memcpy(&c, mid_, sizeof c);
    tail_[12] = uint8_t(nonce); tail_[13] = uint8_t(nonce >> 8); tail_[14] = uint8_t(nonce >> 16); tail_[15] = uint8_t(nonce >> 24);
    uint8_t first[32];
    CC_SHA256_Update(&c, tail_, 16);
    CC_SHA256_Final(first, &c);
    Hash256 h;
    CC_SHA256(first, 32, h.data());
    return h;
}
#else
// Portable SHA-256 from PQClean
Hash256 sha256(const uint8_t* data, size_t len) {
    Hash256 h;
    ::sha256(h.data(), data, len);
    return h;
}

static_assert(sizeof(sha256ctx) <= 128, "midstate buffer too small");
#define PLK_MID(p) (reinterpret_cast<sha256ctx*>(p))

HeaderHasher::HeaderHasher(const Bytes& header) {
    sha256_inc_init(PLK_MID(mid_));
    sha256_inc_blocks(PLK_MID(mid_), header.data(), 1);
    std::memcpy(tail_, header.data() + 64, 16);
}

HeaderHasher::~HeaderHasher() { sha256_inc_ctx_release(PLK_MID(mid_)); }

Hash256 HeaderHasher::hash(uint32_t nonce) {
    tail_[12] = uint8_t(nonce); tail_[13] = uint8_t(nonce >> 8); tail_[14] = uint8_t(nonce >> 16); tail_[15] = uint8_t(nonce >> 24);
    sha256ctx c;
    sha256_inc_ctx_clone(&c, PLK_MID(mid_));
    uint8_t first[32];
    sha256_inc_finalize(first, &c, tail_, 16);   // also releases c
    Hash256 h;
    ::sha256(h.data(), first, 32);
    return h;
}
#endif

Hash256 sha256d(const uint8_t* data, size_t len) {
    Hash256 h = sha256(data, len);
    return sha256(h.data(), h.size());
}

Hash256 sha3_256(const Bytes& b) {
    Hash256 h;
    ::sha3_256(h.data(), b.data(), b.size());
    return h;
}

std::array<uint8_t, 64> sha512(const Bytes& b) {
    std::array<uint8_t, 64> h;
    ::sha512(h.data(), b.data(), b.size());
    return h;
}

std::array<uint8_t, 64> hmac_sha512(const Bytes& key, const Bytes& data) {
    const size_t block = 128;
    Bytes k = key;
    if (k.size() > block) {
        auto kh = sha512(k);
        k.assign(kh.begin(), kh.end());
    }
    k.resize(block, 0);
    Bytes inner(block), outer(block);
    for (size_t i = 0; i < block; i++) {
        inner[i] = k[i] ^ 0x36;
        outer[i] = k[i] ^ 0x5c;
    }
    inner.insert(inner.end(), data.begin(), data.end());
    auto ih = sha512(inner);
    outer.insert(outer.end(), ih.begin(), ih.end());
    auto out = sha512(outer);
    wipe(k.data(), k.size());
    wipe(inner.data(), inner.size());
    wipe(outer.data(), outer.size());
    return out;
}

Bytes pbkdf2_hmac_sha512(const Bytes& password, const Bytes& salt, uint32_t iterations, size_t out_len) {
    Bytes out;
    for (uint32_t block = 1; out.size() < out_len; block++) {
        Bytes s = salt;
        s.push_back(block >> 24); s.push_back(block >> 16); s.push_back(block >> 8); s.push_back(block);
        auto u = hmac_sha512(password, s);
        auto t = u;
        for (uint32_t i = 1; i < iterations; i++) {
            u = hmac_sha512(password, Bytes(u.begin(), u.end()));
            for (size_t j = 0; j < t.size(); j++) t[j] ^= u[j];
        }
        out.insert(out.end(), t.begin(), t.end());
    }
    out.resize(out_len);
    return out;
}

Hash256 tagged_hash(const std::string& tag, const Bytes& data) {
    Hash256 th = sha256(reinterpret_cast<const uint8_t*>(tag.data()), tag.size());
    Bytes b(th.begin(), th.end());
    b.insert(b.end(), th.begin(), th.end());
    b.insert(b.end(), data.begin(), data.end());
    return sha256(b);
}

void random_bytes(uint8_t* out, size_t len) { platform::random_bytes(out, len); }

std::string to_hex(const uint8_t* data, size_t len) {
    static const char* d = "0123456789abcdef";
    std::string s;
    s.reserve(len * 2);
    for (size_t i = 0; i < len; i++) {
        s += d[data[i] >> 4];
        s += d[data[i] & 15];
    }
    return s;
}

bool from_hex(const std::string& hex, Bytes& out) {
    std::string h = hex.rfind("0x", 0) == 0 ? hex.substr(2) : hex;
    if (h.size() % 2) return false;
    out.clear();
    auto val = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < h.size(); i += 2) {
        int a = val(h[i]), b = val(h[i + 1]);
        if (a < 0 || b < 0) return false;
        out.push_back(uint8_t(a << 4 | b));
    }
    return true;
}

void wipe(void* p, size_t len) {
    volatile uint8_t* v = static_cast<volatile uint8_t*>(p);
    while (len--) *v++ = 0;
}

}  // namespace plk

// PQClean calls this for randomness. Key generation in keys.cpp temporarily supplies a seed instead,
// so the same seed phrase always yields the same ML-DSA key; everything else gets OS randomness.
extern "C" {
thread_local const uint8_t* plk_fixed_random = nullptr;
thread_local size_t plk_fixed_random_len = 0;

int PQCLEAN_randombytes(uint8_t* output, size_t n) {
    if (plk_fixed_random) {
        if (n != plk_fixed_random_len) std::abort();   // keygen asks for exactly one 32-byte seed
        std::memcpy(output, plk_fixed_random, n);
        plk_fixed_random = nullptr;                     // one use only
        plk_fixed_random_len = 0;
        return 0;
    }
    plk::platform::random_bytes(output, n);
    return 0;
}
}
