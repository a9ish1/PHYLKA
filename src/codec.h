// Little-endian / CompactSize helpers shared by the transaction, block and extension codecs.
#pragma once
#include <cstring>

#include "crypto.h"

namespace plk {

inline void put32(Bytes& b, uint32_t v) { for (int i = 0; i < 4; i++) b.push_back(v >> (8 * i)); }
inline void put64(Bytes& b, uint64_t v) { for (int i = 0; i < 8; i++) b.push_back(v >> (8 * i)); }
inline void put_size(Bytes& b, uint64_t n) {
    if (n < 0xfd) b.push_back(uint8_t(n));
    else if (n <= 0xffff) { b.push_back(0xfd); b.push_back(n); b.push_back(n >> 8); }
    else { b.push_back(0xfe); put32(b, uint32_t(n)); }
}
inline void put_bytes(Bytes& b, const uint8_t* p, size_t n) { b.insert(b.end(), p, p + n); }
inline void put_var(Bytes& b, const Bytes& v) { put_size(b, v.size()); put_bytes(b, v.data(), v.size()); }

struct Reader {
    const Bytes& b;
    size_t& pos;
    bool ok = true;
    bool need(size_t n) { if (pos + n > b.size()) ok = false; return ok; }
    uint32_t u32() { if (!need(4)) return 0; uint32_t v = 0; for (int i = 0; i < 4; i++) v |= uint32_t(b[pos++]) << (8 * i); return v; }
    uint64_t u64() { if (!need(8)) return 0; uint64_t v = 0; for (int i = 0; i < 8; i++) v |= uint64_t(b[pos++]) << (8 * i); return v; }
    uint64_t size() {
        if (!need(1)) return 0;
        uint8_t t = b[pos++];
        if (t < 0xfd) return t;
        if (t == 0xfd) { if (!need(2)) return 0; uint64_t v = b[pos] | b[pos + 1] << 8; pos += 2; return v; }
        if (t == 0xfe) return u32();
        ok = false;
        return 0;
    }
    void raw(uint8_t* out, size_t n) { if (need(n)) { std::memcpy(out, &b[pos], n); pos += n; } }
    Bytes var(size_t max) { uint64_t n = size(); if (n > max) { ok = false; return {}; } Bytes v(n); raw(v.data(), n); return v; }
};

}  // namespace plk
