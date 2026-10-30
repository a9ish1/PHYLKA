// PHYLKA - hashing and byte helpers. SHA-2 and SHA-3 come from PQClean (vendored in deps/pqcommon).
#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace plk {

using Bytes = std::vector<uint8_t>;
using Hash256 = std::array<uint8_t, 32>;

Hash256 sha256(const uint8_t* data, size_t len);
inline Hash256 sha256(const Bytes& b) { return sha256(b.data(), b.size()); }
/// Double SHA-256: block and transaction ids, proof of work (same as Bitcoin)
Hash256 sha256d(const uint8_t* data, size_t len);
inline Hash256 sha256d(const Bytes& b) { return sha256d(b.data(), b.size()); }
Hash256 sha3_256(const Bytes& b);

/// Mining helper: hashes the constant first 64 bytes of an 80-byte header once ("midstate"),
/// then each nonce only costs the last 16 bytes plus the second SHA-256.
class HeaderHasher {
public:
    explicit HeaderHasher(const Bytes& header80);
    ~HeaderHasher();
    HeaderHasher(const HeaderHasher&) = delete;
    HeaderHasher& operator=(const HeaderHasher&) = delete;
    /// sha256d of the header with this nonce (bytes 76..79), same result as sha256d(header)
    Hash256 hash(uint32_t nonce);
private:
    alignas(16) uint8_t mid_[128];   // SHA-256 state after the first 64 bytes (CC_SHA256_CTX or PQClean sha256ctx)
    uint8_t tail_[16];
};
std::array<uint8_t, 64> sha512(const Bytes& b);
std::array<uint8_t, 64> hmac_sha512(const Bytes& key, const Bytes& data);
Bytes pbkdf2_hmac_sha512(const Bytes& password, const Bytes& salt, uint32_t iterations, size_t out_len);
/// BIP340-style domain separation: SHA256(SHA256(tag) || SHA256(tag) || data)
Hash256 tagged_hash(const std::string& tag, const Bytes& data);

/// Cryptographically secure random bytes from the OS
void random_bytes(uint8_t* out, size_t len);

std::string to_hex(const uint8_t* data, size_t len);
inline std::string to_hex(const Bytes& b) { return to_hex(b.data(), b.size()); }
template <size_t N> std::string to_hex(const std::array<uint8_t, N>& a) { return to_hex(a.data(), N); }
bool from_hex(const std::string& hex, Bytes& out);

/// Overwrites memory so secrets do not linger after use
void wipe(void* p, size_t len);

}  // namespace plk
