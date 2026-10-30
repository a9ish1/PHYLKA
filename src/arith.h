// Minimal 256-bit unsigned integer for difficulty targets (compact "bits" <-> 256-bit target, LWMA retarget)
#pragma once
#include <array>
#include <cmath>
#include <cstdint>

#include "crypto.h"

#ifdef _MSC_VER
#include <intrin.h>
#endif

namespace plk {

/// Portable count of leading zero bits of a non-zero 32-bit value
inline int clz32(uint32_t x) {
#ifdef _MSC_VER
    unsigned long i;
    _BitScanReverse(&i, x);
    return 31 - int(i);
#else
    return __builtin_clz(x);
#endif
}

/// Portable count of leading zero bits of a non-zero 64-bit value
inline int clz64(uint64_t x) {
    uint32_t hi = uint32_t(x >> 32);
    return hi ? clz32(hi) : 32 + clz32(uint32_t(x));
}

class U256 {
public:
    std::array<uint32_t, 8> w{};   // little-endian 32-bit limbs

    static U256 from_compact(uint32_t bits) {
        U256 r;
        uint32_t exp = bits >> 24, mant = bits & 0x007fffff;
        if (bits & 0x00800000) return r;
        if (exp <= 3) {
            mant >>= 8 * (3 - exp);
            r.w[0] = mant;
        } else {
            r.w[0] = mant;
            r.shl(8 * (exp - 3));
        }
        return r;
    }

    uint32_t to_compact() const {
        int bytes = (bits() + 7) / 8;
        uint32_t mant;
        if (bytes <= 3) {
            mant = w[0] << (8 * (3 - bytes));
        } else {
            U256 t = *this;
            t.shr(8 * (bytes - 3));
            mant = t.w[0];
        }
        if (mant & 0x00800000) {   // keep the sign bit clear
            mant >>= 8;
            bytes++;
        }
        return uint32_t(bytes) << 24 | (mant & 0x007fffff);
    }

    static U256 from_hash(const Hash256& h) {
        U256 r;
        for (int i = 0; i < 8; i++) r.w[i] = h[4 * i] | h[4 * i + 1] << 8 | h[4 * i + 2] << 16 | uint32_t(h[4 * i + 3]) << 24;
        return r;
    }

    int bits() const {
        for (int i = 7; i >= 0; i--)
            if (w[i]) return 32 * i + 32 - clz32(w[i]);
        return 0;
    }

    void shl(int n) {
        while (n >= 32) { for (int i = 7; i > 0; i--) w[i] = w[i - 1]; w[0] = 0; n -= 32; }
        if (!n) return;
        for (int i = 7; i > 0; i--) w[i] = (w[i] << n) | (w[i - 1] >> (32 - n));
        w[0] <<= n;
    }

    void shr(int n) {
        while (n >= 32) { for (int i = 0; i < 7; i++) w[i] = w[i + 1]; w[7] = 0; n -= 32; }
        if (!n) return;
        for (int i = 0; i < 7; i++) w[i] = (w[i] >> n) | (w[i + 1] << (32 - n));
        w[7] >>= n;
    }

    U256& add(const U256& o) {
        uint64_t c = 0;
        for (int i = 0; i < 8; i++) { c += uint64_t(w[i]) + o.w[i]; w[i] = uint32_t(c); c >>= 32; }
        return *this;
    }

    /// Multiplies by a small number; returns false on overflow
    bool mul(uint32_t m) {
        uint64_t c = 0;
        for (int i = 0; i < 8; i++) { c += uint64_t(w[i]) * m; w[i] = uint32_t(c); c >>= 32; }
        return c == 0;
    }

    U256& div(uint32_t d) {
        uint64_t r = 0;
        for (int i = 7; i >= 0; i--) { uint64_t cur = (r << 32) | w[i]; w[i] = uint32_t(cur / d); r = cur % d; }
        return *this;
    }

    bool operator<=(const U256& o) const {
        for (int i = 7; i >= 0; i--) if (w[i] != o.w[i]) return w[i] < o.w[i];
        return true;
    }
    bool operator<(const U256& o) const {
        for (int i = 7; i >= 0; i--) if (w[i] != o.w[i]) return w[i] < o.w[i];
        return false;
    }
    bool operator>(const U256& o) const { return o < *this; }
    bool operator>=(const U256& o) const { return o <= *this; }
    bool operator==(const U256& o) const { return w == o.w; }
    bool operator!=(const U256& o) const { return w != o.w; }
    bool is_zero() const { return bits() == 0; }

    static U256 from_u64(uint64_t v) {
        U256 r;
        r.w[0] = uint32_t(v);
        r.w[1] = uint32_t(v >> 32);
        return r;
    }

    U256 operator~() const {
        U256 r;
        for (int i = 0; i < 8; i++) r.w[i] = ~w[i];
        return r;
    }

    U256 operator+(const U256& o) const { U256 r = *this; return r.add(o); }

    /// Subtraction modulo 2^256
    U256& sub(const U256& o) {
        uint64_t borrow = 0;
        for (int i = 0; i < 8; i++) {
            uint64_t d = uint64_t(w[i]) - o.w[i] - borrow;
            w[i] = uint32_t(d);
            borrow = (d >> 32) & 1;
        }
        return *this;
    }

    /// Floor division n / d (bit-by-bit long division; d must be non-zero)
    static U256 divide(const U256& n, const U256& d) {
        U256 q, r;
        for (int i = 255; i >= 0; i--) {
            bool carry = r.w[7] >> 31;
            r.shl(1);
            r.w[0] |= (n.w[i / 32] >> (i % 32)) & 1;
            if (carry || !(r < d)) {
                r.sub(d);
                q.w[i / 32] |= 1u << (i % 32);
            }
        }
        return q;
    }

    /// Expected number of hashes to meet this target, exactly as Bitcoin: ~target / (target + 1) + 1 (= 2^256 / (target + 1))
    U256 work() const {
        U256 one = from_u64(1), t1 = *this + one;
        if (t1.is_zero()) return one;   // target 2^256 - 1
        return divide(~*this, t1) + one;
    }

    /// 64 hex digits, most significant first
    std::string to_hex() const {
        static const char* d = "0123456789abcdef";
        std::string s;
        for (int i = 7; i >= 0; i--) for (int k = 28; k >= 0; k -= 4) s += d[(w[i] >> k) & 15];
        return s;
    }

    /// Approximation for display and ratios only (never for consensus decisions)
    long double to_ld() const {
        long double t = 0;
        for (int i = 7; i >= 0; i--) t = t * 4294967296.0L + w[i];
        return t;
    }
};

}  // namespace plk
