#include "ext.h"

#include "codec.h"

namespace plk {

Hash256 vote_message(uint32_t height, const Hash256& block) {
    Bytes b;
    put32(b, height);
    put_bytes(b, block.data(), 32);
    return tagged_hash("PHYLKA/vote", b);
}

Hash256 checkpoint_commitment(uint32_t height, const Hash256& block) {
    Bytes b;
    put32(b, height);
    put_bytes(b, block.data(), 32);
    return tagged_hash("PHYLKA/ckpt", b);
}

static void put_sig(Bytes& b, const HybridSignature& s) {
    put_bytes(b, s.schnorr.data(), SCHNORR_SIG);
    put_bytes(b, s.mldsa.data(), s.mldsa.size());
}

static void read_sig(Reader& r, HybridSignature& s) {
    r.raw(s.schnorr.data(), SCHNORR_SIG);
    s.mldsa.resize(MLDSA_SIG);
    r.raw(s.mldsa.data(), MLDSA_SIG);
}

Bytes Evidence::serialize() const {
    Bytes b;
    put32(b, height);
    put_bytes(b, vid.data(), 32);
    put_bytes(b, block_a.data(), 32);
    put_sig(b, sig_a);
    put_bytes(b, block_b.data(), 32);
    put_sig(b, sig_b);
    return b;
}

static bool read_evidence(Reader& r, Evidence& e) {
    e.height = r.u32();
    r.raw(e.vid.data(), 32);
    r.raw(e.block_a.data(), 32);
    read_sig(r, e.sig_a);
    r.raw(e.block_b.data(), 32);
    read_sig(r, e.sig_b);
    return r.ok;
}

std::optional<Evidence> Evidence::parse(const Bytes& b, size_t& pos) {
    Reader r{b, pos};
    Evidence e;
    if (!read_evidence(r, e)) return std::nullopt;
    return e;
}

Bytes BlockExt::serialize() const {
    Bytes b;
    put_size(b, regs.size());
    for (auto& k : regs) {
        put_bytes(b, k.schnorr.data(), SCHNORR_PK);
        put_bytes(b, k.mldsa.data(), k.mldsa.size());
    }
    put_size(b, evidence.size());
    for (auto& e : evidence) {
        Bytes x = e.serialize();
        b.insert(b.end(), x.begin(), x.end());
    }
    b.push_back(cert ? 1 : 0);
    if (cert) {
        put32(b, cert->height);
        put_bytes(b, cert->block.data(), 32);
        put_size(b, cert->votes.size());
        for (auto& v : cert->votes) {
            put_bytes(b, v.vid.data(), 32);
            put_sig(b, v.sig);
        }
    }
    return b;
}

std::optional<BlockExt> BlockExt::parse(const Bytes& b) {
    size_t pos = 0;
    Reader r{b, pos};
    BlockExt x;
    uint64_t nr = r.size();
    if (nr > MAX_EXT_REGS) return std::nullopt;
    for (uint64_t i = 0; i < nr && r.ok; i++) {
        PublicKey k;
        r.raw(k.schnorr.data(), SCHNORR_PK);
        k.mldsa.resize(MLDSA_PK);
        r.raw(k.mldsa.data(), MLDSA_PK);
        x.regs.push_back(std::move(k));
    }
    uint64_t ne = r.size();
    if (ne > MAX_EXT_EVIDENCE) return std::nullopt;
    for (uint64_t i = 0; i < ne && r.ok; i++) {
        Evidence e;
        if (!read_evidence(r, e)) return std::nullopt;
        x.evidence.push_back(std::move(e));
    }
    uint8_t has = 0;
    r.raw(&has, 1);
    if (has > 1) return std::nullopt;
    if (has) {
        Cert c;
        c.height = r.u32();
        r.raw(c.block.data(), 32);
        uint64_t n = r.size();
        if (n > MAX_CERT_VOTES) return std::nullopt;
        for (uint64_t i = 0; i < n && r.ok; i++) {
            CertVote v;
            r.raw(v.vid.data(), 32);
            read_sig(r, v.sig);
            c.votes.push_back(std::move(v));
        }
        x.cert = std::move(c);
    }
    if (!r.ok || pos != b.size()) return std::nullopt;
    return x;
}

Hash256 BlockExt::hash() const { return tagged_hash("PHYLKA/ext", serialize()); }

}  // namespace plk
