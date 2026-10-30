// PHYLKA addresses: bech32m (BIP350) with the human-readable part "plk".
//   plk1 + version (one char) + 32-byte program + 6-char checksum, e.g. plk1p...  (63 characters)
// Version 1 ("p") = hybrid secp256k1 Schnorr + ML-DSA-44. The checksum catches any typo of up to 4 characters.
#pragma once
#include <optional>
#include <string>

#include "crypto.h"

namespace plk {

constexpr const char* ADDRESS_HRP = "plk";
constexpr int ADDRESS_VERSION_HYBRID = 1;
/// Version 2 ("z"): stake address. Same hybrid keys, but coins sent here are locked as validator stake.
constexpr int ADDRESS_VERSION_STAKE = 2;
inline bool known_address_version(int v) { return v == ADDRESS_VERSION_HYBRID || v == ADDRESS_VERSION_STAKE; }

namespace bech32m {
std::string encode(const std::string& hrp, const Bytes& data5);
/// Returns (hrp, 5-bit data) or nullopt if the string or its checksum is invalid
std::optional<std::pair<std::string, Bytes>> decode(const std::string& s);
bool convert_bits(Bytes& out, const Bytes& in, int from, int to, bool pad);
}  // namespace bech32m

struct Address {
    int version = ADDRESS_VERSION_HYBRID;
    Hash256 program{};
    std::string str(const std::string& hrp = ADDRESS_HRP) const;
    static std::optional<Address> parse(const std::string& s, const std::string& hrp = ADDRESS_HRP);
};

}  // namespace plk
