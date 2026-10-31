#include "chain.h"

#include <algorithm>
#include <cstring>

#include "codec.h"

namespace plk {

static const char* GENESIS_TEXT = "Why are we using only classic things in bad times? We need a revolution in methods.";

const ChainParams& ChainParams::main() {
    // Bitcoin's schedule: a block every 10 minutes, 50 PLK halving every 210 000 blocks (~4 years), 21 M PLK in total.
    // Genesis is at difficulty 1 like Bitcoin's; afterwards LWMA retargets every block and may go down to
    // 1/256 so a small network (a laptop, a NerdMiner) keeps a 10-minute pace.
    static ChainParams p{"main", "plk", 0x1d00ffff, 0x1e00ffff, 50 * COIN, 210'000, 600, 45, 100,
                         {0xf1, 0x9c, 0x4b, 0x01}, 7333, 7334, GENESIS_TEXT,
                         1793135950, 4000626186,   // mined 2026-10-28: 00000000df01a464bd9f7ad30b88da74059e25ea5b60eccaac0f95f2ba1a70a0
                         // finality: checkpoint every 6 blocks (~1 h), voted 2 blocks deep; stake locked 1008 blocks (~1 week),
                         // counts after 6 blocks; at least 10 PLK per validator and 100 PLK in total before finality starts
                         6, 2, 1008, 6, 10 * COIN, 100 * COIN,
                         // phase 3 rules activate at height 1000 (the main chain is far below), 10% of fees go to voters.
                         // Built-in seed nodes ("host:port"); nodes dial them at start unless --noseeds is given.
                         1000, 10, {"149.56.215.9:7333"},
                         // Checkpoints lock the network to this chain. Before each release add the newest block that is
                         // buried deep (e.g. 100+ blocks): { height, "hash in display order" }, keeping the list ascending.
                         {{0, "00000000df01a464bd9f7ad30b88da74059e25ea5b60eccaac0f95f2ba1a70a0"},
                          {12, "000000006bcea5c8d0ad42901bcc4f74bf30214767dc2a66c81ee7fdab7e1709"}}};
    return p;
}

const ChainParams& ChainParams::dev() {
    // Same rules for local testing: trivial difficulty, 5-second blocks, short maturity
    static ChainParams p{"dev", "plkd", 0x207fffff, 0x207fffff, 50 * COIN, 150, 5, 20, 2,
                         {0xf1, 0x9c, 0x4b, 0xde}, 17333, 17334, GENESIS_TEXT, 1793000000, 0,
                         5, 1, 20, 2, 10 * COIN, 10 * COIN,
                         20, 10, {}, {}};   // dev: upgrade at 20 (override with --upgrade-height), no built-in seeds
    return p;
}

uint32_t coinbase_height(const Transaction& cb) {
    if (cb.vin.empty() || cb.vin[0].coinbase_data.size() < 4) return 0xffffffff;
    auto& d = cb.vin[0].coinbase_data;
    return d[0] | d[1] << 8 | d[2] << 16 | uint32_t(d[3]) << 24;
}

// ---------------------------------------------------------------- serialization

bool OutPoint::null() const { return vout == 0xffffffff && std::all_of(txid.begin(), txid.end(), [](uint8_t x) { return x == 0; }); }

static void put_out(Bytes& b, const TxOut& o) {
    put64(b, o.value);
    b.push_back(uint8_t(o.to.version));
    put_bytes(b, o.to.program.data(), 32);
}

Bytes Transaction::serialize(bool with_witness) const {
    Bytes b;
    put32(b, version);
    put_size(b, vin.size());
    for (auto& in : vin) {
        put_bytes(b, in.prev.txid.data(), 32);
        put32(b, in.prev.vout);
        put_var(b, in.coinbase_data);
    }
    put_size(b, vout.size());
    for (auto& o : vout) put_out(b, o);
    put32(b, locktime);
    if (with_witness) {
        put_size(b, witness.size());
        for (auto& w : witness) {
            b.push_back(0x01);   // scheme: hybrid Schnorr + ML-DSA-44
            put_bytes(b, w.key.schnorr.data(), SCHNORR_PK);
            put_bytes(b, w.key.mldsa.data(), w.key.mldsa.size());
            put_bytes(b, w.sig.schnorr.data(), SCHNORR_SIG);
            put_bytes(b, w.sig.mldsa.data(), w.sig.mldsa.size());
        }
    }
    return b;
}

std::optional<Transaction> Transaction::parse(const Bytes& b, size_t& pos) {
    Reader r{b, pos};
    Transaction t;
    t.version = r.u32();
    uint64_t nin = r.size();
    if (nin > 10000) return std::nullopt;
    for (uint64_t i = 0; i < nin && r.ok; i++) {
        TxIn in;
        r.raw(in.prev.txid.data(), 32);
        in.prev.vout = r.u32();
        in.coinbase_data = r.var(200);   // consensus limits it to 100 bytes below the upgrade height
        t.vin.push_back(in);
    }
    uint64_t nout = r.size();
    if (nout > 10000) return std::nullopt;
    for (uint64_t i = 0; i < nout && r.ok; i++) {
        TxOut o;
        o.value = r.u64();
        uint8_t v = 0;
        r.raw(&v, 1);
        o.to.version = v;
        r.raw(o.to.program.data(), 32);
        t.vout.push_back(o);
    }
    t.locktime = r.u32();
    uint64_t nw = r.size();
    if (nw > nin) return std::nullopt;
    for (uint64_t i = 0; i < nw && r.ok; i++) {
        uint8_t scheme = 0;
        r.raw(&scheme, 1);
        if (scheme != 0x01) return std::nullopt;
        Witness w;
        r.raw(w.key.schnorr.data(), SCHNORR_PK);
        w.key.mldsa.resize(MLDSA_PK);
        r.raw(w.key.mldsa.data(), MLDSA_PK);
        r.raw(w.sig.schnorr.data(), SCHNORR_SIG);
        w.sig.mldsa.resize(MLDSA_SIG);
        r.raw(w.sig.mldsa.data(), MLDSA_SIG);
        t.witness.push_back(std::move(w));
    }
    if (!r.ok) return std::nullopt;
    return t;
}

Hash256 Transaction::txid() const { return sha256d(serialize(false)); }
Hash256 Transaction::wtxid() const { return sha256d(serialize(true)); }
bool Transaction::is_coinbase() const { return vin.size() == 1 && vin[0].prev.null(); }

Hash256 Transaction::sighash(size_t index, const std::vector<TxOut>& spent) const {
    Bytes b;
    Hash256 id = txid();
    put_bytes(b, id.data(), 32);
    put32(b, uint32_t(index));
    put_size(b, spent.size());
    for (auto& o : spent) put_out(b, o);
    return tagged_hash("PHYLKA/sighash", b);
}

void Transaction::sign(size_t index, const HybridKey& key, const std::vector<TxOut>& spent) {
    if (witness.size() < vin.size()) witness.resize(vin.size());
    witness[index] = Witness{key.pub(), key.sign(sighash(index, spent))};
}

bool Transaction::verify_input(size_t index, const std::vector<TxOut>& spent, std::string* why) const {
    auto fail = [&](const char* m) { if (why) *why = m; return false; };
    if (index >= vin.size() || index >= witness.size() || spent.size() != vin.size()) return fail("missing signature for input");
    const TxOut& out = spent[index];
    const Witness& w = witness[index];
    if (!known_address_version(out.to.version)) return fail("unknown address version");
    if (w.key.program() != out.to.program) return fail("keys do not match the address");
    if (!verify(w.key, sighash(index, spent), w.sig)) return fail("invalid signature");
    return true;
}

// ---------------------------------------------------------------- blocks

Bytes BlockHeader::serialize() const {
    Bytes b;
    put32(b, uint32_t(version));
    put_bytes(b, prev.data(), 32);
    put_bytes(b, merkle.data(), 32);
    put32(b, time);
    put32(b, bits);
    put32(b, nonce);
    return b;
}

Hash256 BlockHeader::hash() const { return sha256d(serialize()); }

Hash256 Block::merkle_root() const {
    std::vector<Hash256> level;
    for (auto& t : txs) level.push_back(t.wtxid());
    if (level.empty()) return Hash256{};
    while (level.size() > 1) {
        if (level.size() % 2) level.push_back(level.back());
        std::vector<Hash256> next;
        for (size_t i = 0; i < level.size(); i += 2) {
            Bytes b(level[i].begin(), level[i].end());
            b.insert(b.end(), level[i + 1].begin(), level[i + 1].end());
            next.push_back(sha256d(b));
        }
        level.swap(next);
    }
    return level[0];
}

Bytes Block::serialize() const {
    Bytes b = header.serialize();
    put_size(b, txs.size());
    for (auto& t : txs) {
        Bytes tb = t.serialize(true);
        b.insert(b.end(), tb.begin(), tb.end());
    }
    if (has_ext) put_var(b, ext.serialize());
    return b;
}

std::string hash_hex(const Hash256& h) {
    Hash256 r;
    std::reverse_copy(h.begin(), h.end(), r.begin());
    return to_hex(r);
}

/// 256-bit target from the compact "bits" form (same encoding as Bitcoin)
static Hash256 target_of(uint32_t bits) {
    Hash256 t{};   // little-endian
    uint32_t exp = bits >> 24, mant = bits & 0x007fffff;
    if (bits & 0x00800000) return t;   // negative targets are invalid
    for (int i = 0; i < 3; i++) {
        int pos = int(exp) - 3 + i;
        if (pos >= 0 && pos < 32) t[pos] = (mant >> (8 * i)) & 0xff;
    }
    return t;
}

bool check_pow(const Hash256& hash, uint32_t bits) {
    Hash256 t = target_of(bits);
    for (int i = 31; i >= 0; i--) {   // compare as little-endian 256-bit numbers
        if (hash[i] != t[i]) return hash[i] < t[i];
    }
    return true;
}

bool mine(BlockHeader& h, uint64_t max_tries, uint64_t* tries) {
    Bytes b = h.serialize();
    for (uint64_t n = 0; n < max_tries; n++) {
        b[76] = h.nonce; b[77] = h.nonce >> 8; b[78] = h.nonce >> 16; b[79] = h.nonce >> 24;
        Hash256 x = sha256d(b);
        if (check_pow(x, h.bits)) {
            if (tries) *tries = n + 1;
            return true;
        }
        if (++h.nonce == 0) {   // nonce space exhausted: move the timestamp
            h.time++;
            b[68] = h.time; b[69] = h.time >> 8; b[70] = h.time >> 16; b[71] = h.time >> 24;
        }
    }
    if (tries) *tries = max_tries;
    return false;
}

uint64_t block_reward(const ChainParams& p, uint32_t height) {
    uint32_t halvings = height / p.halving_interval;
    return halvings >= 64 ? 0 : p.initial_reward >> halvings;
}

size_t coinbase_prefix_len(const ChainParams& p, uint32_t height) {
    if (height == 0) return 4;
    return 4 + (p.upgraded(height) ? COMMIT_BYTES : 0) + 8;
}

Transaction make_coinbase(const ChainParams& p, uint32_t height, const Address& to, const std::string& text, uint64_t fees, const Bytes& commit) {
    Transaction t;
    TxIn in;
    in.prev.vout = 0xffffffff;
    put32(in.coinbase_data, height);   // makes every coinbase unique
    in.coinbase_data.insert(in.coinbase_data.end(), commit.begin(), commit.end());
    in.coinbase_data.insert(in.coinbase_data.end(), text.begin(), text.end());
    t.vin.push_back(in);
    t.vout.push_back(TxOut{block_reward(p, height) + fees, to});
    return t;
}

Block ChainParams::genesis() const {
    std::string tag = "PHYLKA genesis: " + genesis_message;
    Address burn{ADDRESS_VERSION_HYBRID, sha3_256(Bytes(tag.begin(), tag.end()))};
    Block g = make_genesis(*this, burn, genesis_message, genesis_time);
    g.header.nonce = genesis_nonce;
    if (genesis_nonce == 0 && name == "dev") mine(g.header, 1'000'000);   // dev genesis is found in a few tries
    return g;
}

Block make_genesis(const ChainParams& p, const Address& to, const std::string& message, uint32_t time) {
    Block g;
    g.txs.push_back(make_coinbase(p, 0, to, message));
    g.header.version = 1;
    g.header.time = time;
    g.header.bits = p.genesis_bits;
    g.header.merkle = g.merkle_root();
    return g;
}

}  // namespace plk
