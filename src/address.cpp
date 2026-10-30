#include "address.h"

#include <algorithm>
#include <cstring>

namespace plk {
namespace bech32m {

static const char* CHARSET = "qpzry9x8gf2tvdw0s3jn54khce6mua7l";
static const uint32_t BECH32M_CONST = 0x2bc830a3;

static uint32_t polymod(const Bytes& v) {
    uint32_t c = 1;
    for (uint8_t x : v) {
        uint8_t c0 = c >> 25;
        c = ((c & 0x1ffffff) << 5) ^ x;
        if (c0 & 1) c ^= 0x3b6a57b2;
        if (c0 & 2) c ^= 0x26508e6d;
        if (c0 & 4) c ^= 0x1ea119fa;
        if (c0 & 8) c ^= 0x3d4233dd;
        if (c0 & 16) c ^= 0x2a1462b3;
    }
    return c;
}

static Bytes hrp_expand(const std::string& hrp) {
    Bytes r;
    for (char c : hrp) r.push_back(uint8_t(c) >> 5);
    r.push_back(0);
    for (char c : hrp) r.push_back(uint8_t(c) & 31);
    return r;
}

std::string encode(const std::string& hrp, const Bytes& data) {
    Bytes v = hrp_expand(hrp);
    v.insert(v.end(), data.begin(), data.end());
    v.insert(v.end(), 6, 0);
    uint32_t mod = polymod(v) ^ BECH32M_CONST;
    std::string out = hrp + "1";
    for (uint8_t d : data) out += CHARSET[d];
    for (int i = 0; i < 6; i++) out += CHARSET[(mod >> (5 * (5 - i))) & 31];
    return out;
}

std::optional<std::pair<std::string, Bytes>> decode(const std::string& str) {
    bool lower = false, upper = false;
    for (char c : str) {
        if (c < 33 || c > 126) return std::nullopt;
        if (c >= 'a' && c <= 'z') lower = true;
        if (c >= 'A' && c <= 'Z') upper = true;
    }
    if (lower && upper) return std::nullopt;   // mixed case is never valid
    std::string s = str;
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    size_t pos = s.rfind('1');
    if (pos == std::string::npos || pos == 0 || pos + 7 > s.size() || s.size() > 90) return std::nullopt;
    Bytes data;
    for (size_t i = pos + 1; i < s.size(); i++) {
        const char* p = std::strchr(CHARSET, s[i]);
        if (!p || !*p) return std::nullopt;
        data.push_back(uint8_t(p - CHARSET));
    }
    std::string hrp = s.substr(0, pos);
    Bytes v = hrp_expand(hrp);
    v.insert(v.end(), data.begin(), data.end());
    if (polymod(v) != BECH32M_CONST) return std::nullopt;
    data.resize(data.size() - 6);
    return std::make_pair(hrp, data);
}

bool convert_bits(Bytes& out, const Bytes& in, int from, int to, bool pad) {
    int acc = 0, bits = 0;
    const int maxv = (1 << to) - 1;
    for (uint8_t v : in) {
        if (v >> from) return false;
        acc = (acc << from) | v;
        bits += from;
        while (bits >= to) {
            bits -= to;
            out.push_back((acc >> bits) & maxv);
        }
    }
    if (pad) {
        if (bits) out.push_back((acc << (to - bits)) & maxv);
    } else if (bits >= from || ((acc << (to - bits)) & maxv)) {
        return false;
    }
    return true;
}

}  // namespace bech32m

std::string Address::str(const std::string& hrp) const {
    Bytes data{uint8_t(version)};
    bech32m::convert_bits(data, Bytes(program.begin(), program.end()), 8, 5, true);
    return bech32m::encode(hrp, data);
}

std::optional<Address> Address::parse(const std::string& s, const std::string& hrp) {
    auto d = bech32m::decode(s);
    if (!d || d->first != hrp || d->second.empty()) return std::nullopt;
    Address a;
    a.version = d->second[0];
    if (!known_address_version(a.version)) return std::nullopt;
    Bytes prog;
    if (!bech32m::convert_bits(prog, Bytes(d->second.begin() + 1, d->second.end()), 5, 8, false) || prog.size() != 32)
        return std::nullopt;
    std::copy(prog.begin(), prog.end(), a.program.begin());
    return a;
}

}  // namespace plk
