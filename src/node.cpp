#include "node.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <fstream>
#include <queue>

#include "codec.h"

namespace plk {

namespace fs = std::filesystem;

bool operator<(const OutPoint& a, const OutPoint& b) {
    return a.txid != b.txid ? a.txid < b.txid : a.vout < b.vout;
}

static Bytes read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return Bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

static std::optional<Block> parse_block(const Bytes& b) {
    if (b.size() < 81) return std::nullopt;
    Block blk;
    auto le32 = [&](size_t o) { return uint32_t(b[o]) | b[o + 1] << 8 | b[o + 2] << 16 | uint32_t(b[o + 3]) << 24; };
    blk.header.version = int32_t(le32(0));
    std::copy(b.begin() + 4, b.begin() + 36, blk.header.prev.begin());
    std::copy(b.begin() + 36, b.begin() + 68, blk.header.merkle.begin());
    blk.header.time = le32(68);
    blk.header.bits = le32(72);
    blk.header.nonce = le32(76);
    size_t pos = 80;
    uint64_t n = b[pos++];
    if (n == 0xfd) { n = b[pos] | b[pos + 1] << 8; pos += 2; }
    else if (n >= 0xfe) return std::nullopt;
    for (uint64_t i = 0; i < n; i++) {
        auto t = Transaction::parse(b, pos);
        if (!t) return std::nullopt;
        blk.txs.push_back(std::move(*t));
    }
    if (pos < b.size()) {   // extension (only valid from the upgrade height; checked when the block is validated)
        Reader r{b, pos};
        Bytes e = r.var(MAX_BLOCK_BYTES);
        auto x = r.ok ? BlockExt::parse(e) : std::nullopt;
        if (!x) return std::nullopt;
        blk.has_ext = true;
        blk.ext = std::move(*x);
    }
    if (pos != b.size()) return std::nullopt;
    return blk;
}

std::optional<Block> block_from_bytes(const Bytes& b) { return parse_block(b); }

Node::Node(const ChainParams& p, fs::path datadir) : p_(p), dir_(std::move(datadir)) {
    fs::create_directories(dir_ / "blocks");
    fs::create_directories(dir_ / "undo");
    Block g = p_.genesis();
    Hash256 gh = g.header.hash();
    genesis_hash_ = gh;
    if (!check_pow(gh, g.header.bits)) throw std::runtime_error("genesis block of " + p_.name + " is not mined yet");
    auto gi = std::make_unique<BlockIndex>();
    gi->hash = gh;
    gi->header = g.header;
    gi->chainwork = U256::from_compact(g.header.bits).work();
    active_.push_back(gi.get());
    index_[gh] = std::move(gi);
    store(g, gh);

    // Load stored blocks parent-first, then pick the most-work valid chain
    std::map<Hash256, Block> pending;
    for (auto& e : fs::directory_iterator(dir_ / "blocks")) {
        if (e.path().extension() != ".blk") continue;
        if (auto b = parse_block(read_file(e.path()))) {
            Hash256 h = b->header.hash();
            if (h != gh) pending[h] = std::move(*b);
        }
    }
    for (bool progress = true; progress && !pending.empty();) {
        progress = false;
        for (auto it = pending.begin(); it != pending.end();) {
            auto parent = index_.find(it->second.header.prev);
            if (parent == index_.end()) { ++it; continue; }
            auto bi = std::make_unique<BlockIndex>();
            bi->hash = it->first;
            bi->header = it->second.header;
            bi->prev = parent->second.get();
            bi->height = bi->prev->height + 1;
            bi->chainwork = bi->prev->chainwork + U256::from_compact(bi->header.bits).work();
            index_[it->first] = std::move(bi);
            it = pending.erase(it);
            progress = true;
        }
    }
    fs::create_directories(dir_ / "votes");
    if (std::ifstream f{dir_ / "finality.dat", std::ios::binary}) {
        Bytes b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (b.size() == 36) {
            final_h_ = uint32_t(b[0]) | b[1] << 8 | b[2] << 16 | uint32_t(b[3]) << 24;
            std::copy(b.begin() + 4, b.end(), final_hash_.begin());
        }
    }
    std::string err;
    activate_best(err);
    load_evidence();
    // Replay stored votes (they are checked again exactly like votes from the network)
    for (auto& e : fs::directory_iterator(dir_ / "votes")) {
        bool compact = e.path().extension() == ".cvotes";
        if (!compact && e.path().extension() != ".votes") continue;
        Bytes b = read_file(e.path());
        size_t pos = 0;
        while (pos < b.size()) {
            auto v = compact ? Vote::parse_compact(b, pos) : Vote::parse(b, pos);
            if (!v) break;
            std::string err;
            process_vote(*v, err, false);
        }
    }
    update_finality();
    if (finality_candidate()) activate_best(err);   // stored votes may already prove a branch we hold
}

// ---------------------------------------------------------------- validator finality

Hash256 Vote::message() const { return vote_message(height, block); }

Bytes Vote::serialize() const {
    Bytes b;
    put32(b, height);
    put_bytes(b, block.data(), 32);
    if (compact) put_bytes(b, vid.data(), 32);
    else {
        put_bytes(b, key.schnorr.data(), SCHNORR_PK);
        put_bytes(b, key.mldsa.data(), key.mldsa.size());
    }
    put_bytes(b, sig.schnorr.data(), SCHNORR_SIG);
    put_bytes(b, sig.mldsa.data(), sig.mldsa.size());
    return b;
}

static std::optional<Vote> parse_vote(const Bytes& b, size_t& pos, bool compact) {
    if (pos + (compact ? VOTE_COMPACT_BYTES : VOTE_FULL_BYTES) > b.size()) return std::nullopt;
    Reader r{b, pos};
    Vote v;
    v.compact = compact;
    v.height = r.u32();
    r.raw(v.block.data(), 32);
    if (compact) r.raw(v.vid.data(), 32);
    else {
        r.raw(v.key.schnorr.data(), SCHNORR_PK);
        v.key.mldsa.resize(MLDSA_PK);
        r.raw(v.key.mldsa.data(), MLDSA_PK);
    }
    r.raw(v.sig.schnorr.data(), SCHNORR_SIG);
    v.sig.mldsa.resize(MLDSA_SIG);
    r.raw(v.sig.mldsa.data(), MLDSA_SIG);
    return v;
}

std::optional<Vote> Vote::parse(const Bytes& b, size_t& pos) { return parse_vote(b, pos, false); }
std::optional<Vote> Vote::parse_compact(const Bytes& b, size_t& pos) { return parse_vote(b, pos, true); }

StakeSet Node::snapshot(uint32_t checkpoint) const {
    StakeSet s;
    for (auto& [op, c] : utxo_.coins)
        if (c.out.to.version == ADDRESS_VERSION_STAKE && c.height + p_.stake_activation <= checkpoint && !slashed_.count(c.out.to.program))
            s[c.out.to.program] += c.out.value;   // slashed validators never enter a snapshot again
    for (auto it = s.begin(); it != s.end();) it = it->second < p_.min_stake ? s.erase(it) : std::next(it);
    return s;
}

uint64_t Node::stake_of(const Hash256& prog) const {
    uint64_t t = 0;
    for (auto& [op, c] : utxo_.coins)
        if (c.out.to.version == ADDRESS_VERSION_STAKE && c.out.to.program == prog) t += c.out.value;
    return t;
}

const StakeSet* Node::stake_at(uint32_t cp) const {
    auto it = snapshots_.find(cp);
    return it == snapshots_.end() ? nullptr : &it->second;
}

uint64_t Node::total_stake(const StakeSet& s) const {
    uint64_t t = 0;
    for (auto& [prog, v] : s) t += v;
    return t;
}

const std::map<Hash256, Vote>* Node::active_votes(uint32_t cp) const {
    auto h = pool_.find(cp);
    if (h == pool_.end() || !at(cp)) return nullptr;
    auto b = h->second.find(at(cp)->hash);
    return b == h->second.end() ? nullptr : &b->second;
}

uint64_t Node::voted_stake(uint32_t cp) const {
    const StakeSet* s = stake_at(cp);
    auto vs = active_votes(cp);
    if (!s || !vs) return 0;
    uint64_t t = 0;
    for (auto& [prog, v] : *vs)
        if (!equivocators_.count(prog) && s->count(prog)) t += s->at(prog);
    return t;
}

bool Node::voted(uint32_t h, const Hash256& prog) const {
    auto vs = active_votes(h);
    return vs && vs->count(prog);
}

bool Node::voted_any(uint32_t h, const Hash256& prog) const {
    auto it = pool_.find(h);
    if (it == pool_.end()) return false;
    for (auto& [blk, vm] : it->second)
        if (vm.count(prog)) return true;
    return false;
}

const BlockIndex* Node::finality_candidate(uint32_t* unknown_height) const {
    if (unknown_height) *unknown_height = 0;
    uint32_t last_cp = height() / p_.checkpoint_interval * p_.checkpoint_interval;
    for (auto it = pool_.rbegin(); it != pool_.rend(); ++it) {
        if (it->first <= final_h_) break;
        const StakeSet* ref = stake_at(std::min(it->first, last_cp));
        if (!ref) continue;
        uint64_t total = total_stake(*ref);
        if (total < p_.min_total_stake) continue;
        for (auto& [blk, vm] : it->second) {
            if (vote_rejected_.count(blk) || (at(it->first) && at(it->first)->hash == blk)) continue;
            uint64_t voted = 0;
            for (auto& [prog, v] : vm)
                if (!equivocators_.count(prog) && ref->count(prog)) voted += ref->at(prog);
            if (voted * 3 <= total * 2) continue;
            auto bi = index_.find(blk);
            if (bi == index_.end()) {
                if (unknown_height && !*unknown_height) *unknown_height = it->first;
                continue;
            }
            bool bad = false;
            for (const BlockIndex* a = bi->second.get(); a && !bad; a = a->prev) bad = a->invalid;
            if (!bad) return bi->second.get();
        }
    }
    return nullptr;
}

uint32_t Node::finality_hint() const {
    uint32_t h = 0;
    finality_candidate(&h);
    return h;
}

size_t Node::vote_pool_size() const {
    size_t n = 0;
    for (auto& [h, bm] : pool_) for (auto& [b, vm] : bm) n += vm.size();
    return n;
}

std::vector<Vote> Node::votes_from(uint32_t h) const {
    std::vector<Vote> out;
    for (auto it = pool_.lower_bound(h); it != pool_.end(); ++it)
        for (auto& [blk, vm] : it->second)
            for (auto& [prog, v] : vm) out.push_back(v);
    return out;
}

Hash256 Node::ckpt_commit() const {
    if (certs_.empty()) return checkpoint_commitment(0, active_[0]->hash);
    const CertInfo& c = certs_.rbegin()->second;
    return checkpoint_commitment(c.cp_h, c.cp_hash);
}

void Node::update_finality() {
    // A checkpoint certified on the active chain is final (this is how a fresh node learns finality from blocks alone)
    if (!certs_.empty()) {
        const CertInfo& c = certs_.rbegin()->second;
        if (c.cp_h > final_h_ && at(c.cp_h) && at(c.cp_h)->hash == c.cp_hash) {
            final_h_ = c.cp_h;
            final_hash_ = c.cp_hash;
            save_finality();
        }
    }
    // The highest checkpoint on the active chain backed by more than 2/3 of the stake becomes final
    for (auto it = snapshots_.rbegin(); it != snapshots_.rend() && it->first > final_h_; ++it) {
        uint64_t total = total_stake(it->second);
        if (total < p_.min_total_stake) continue;
        if (voted_stake(it->first) * 3 > total * 2) {
            final_h_ = it->first;
            final_hash_ = at(it->first)->hash;
            save_finality();
            break;
        }
    }
    // Votes far below the final checkpoint are no longer needed
    uint32_t keep = final_h_ > p_.checkpoint_interval ? final_h_ - p_.checkpoint_interval : 0;
    pool_.erase(pool_.begin(), pool_.lower_bound(keep));
}

void Node::save_finality() const {
    Bytes b{uint8_t(final_h_), uint8_t(final_h_ >> 8), uint8_t(final_h_ >> 16), uint8_t(final_h_ >> 24)};
    b.insert(b.end(), final_hash_.begin(), final_hash_.end());
    fs::path f = dir_ / "finality.dat", tmp = dir_ / "finality.dat.tmp";
    std::ofstream(tmp, std::ios::binary).write(reinterpret_cast<const char*>(b.data()), b.size());
    fs::rename(tmp, f);
}

bool Node::violates_checkpoint(uint32_t height, const Hash256& h) const {
    for (auto& [ch, hex] : p_.checkpoints)
        if (ch == height && hash_hex(h) != hex) return true;
    return false;
}

bool Node::allowed_by_checkpoints(const BlockIndex* b) const {
    if (p_.checkpoints.empty()) return true;
    for (auto& [ch, hex] : p_.checkpoints) {
        if (b->height < ch) continue;
        const BlockIndex* a = b;
        while (a->height > ch) a = a->prev;
        if (hash_hex(a->hash) != hex) return false;
    }
    // A chain shorter than the highest checkpoint must lead to it (when we have that block): no fork below it
    auto& top = p_.checkpoints.back();
    if (b->height < top.first)
        for (auto& [h, bi] : index_)
            if (bi->height == top.first && hash_hex(h) == top.second) {
                const BlockIndex* a = bi.get();
                while (a->height > b->height) a = a->prev;
                return a == b;
            }
    return true;
}

bool Node::allowed_by_finality(const BlockIndex* b) const {
    // A chain is acceptable only if it lies on the path through the final checkpoint:
    // either it contains that block, or it is shorter and the final block descends from it.
    if (final_h_ == 0) return true;
    auto f = index_.find(final_hash_);
    if (f == index_.end()) return true;   // we do not have the final block yet (still syncing)
    const BlockIndex* fin = f->second.get();
    if (b->height >= final_h_) {
        while (b->height > final_h_) b = b->prev;
        return b == fin;
    }
    while (fin->height > b->height) fin = fin->prev;
    return fin == b;
}

// Bounds for votes about blocks that are not on our active chain
constexpr size_t MAX_BUFFERED_BRANCHES = 4;      // distinct off-chain blocks per checkpoint height
constexpr size_t MAX_VOTE_POOL = 20000;          // all votes together
constexpr uint32_t MAX_VOTE_AHEAD_CHECKPOINTS = 10;

bool Node::buffer_ok(const Vote& v, const Hash256& prog, std::string& err) const {
    // Buffered votes are applied when their branch becomes active. Stake on that branch is unknown until then, so the voter
    // must at least be a validator of our own chain (latest known snapshot); this bounds the number of acceptable voters.
    if (v.height <= final_h_) return err = "vote for an already finalized height", false;
    if (v.height > height() + MAX_VOTE_AHEAD_CHECKPOINTS * p_.checkpoint_interval) return err = "vote too far ahead of our chain", false;
    uint32_t last_cp = height() / p_.checkpoint_interval * p_.checkpoint_interval;
    const StakeSet* ref = stake_at(std::min(v.height, last_cp));
    if (!ref || !ref->count(prog)) return err = "vote for a branch we do not have: voter is not among our known validators", false;
    auto h = pool_.find(v.height);
    if (h != pool_.end() && !h->second.count(v.block) && h->second.size() >= MAX_BUFFERED_BRANCHES)
        return err = "too many branches at this height - vote not stored", false;
    if (vote_pool_size() >= MAX_VOTE_POOL) return err = "vote buffer full", false;
    return true;
}

void Node::record_equivocation(const Vote& first, const Vote& second, const Hash256& prog) {
    if (!equivocators_.insert(prog).second) return;
    Evidence e;
    e.height = first.height;
    e.vid = prog;
    e.block_a = first.block;
    e.sig_a = first.sig;
    e.block_b = second.block;
    e.sig_b = second.sig;
    evidence_pool_[prog] = e;
    new_evidence_.push_back(e);
    fs::create_directories(dir_ / "evidence");
    Bytes raw = e.serialize();
    std::ofstream(dir_ / "evidence" / (to_hex(prog) + ".evid"), std::ios::binary).write(reinterpret_cast<const char*>(raw.data()), raw.size());
}

void Node::load_evidence() {
    fs::create_directories(dir_ / "evidence");
    for (auto& f : fs::directory_iterator(dir_ / "evidence")) {
        Bytes b = read_file(f.path());
        size_t pos = 0;
        auto e = Evidence::parse(b, pos);
        if (!e || pos != b.size()) continue;
        equivocators_.insert(e->vid);
        evidence_pool_[e->vid] = *e;
    }
}

std::vector<Evidence> Node::take_new_evidence() {
    std::lock_guard<std::mutex> lk(m_);
    std::vector<Evidence> out;
    out.swap(new_evidence_);
    return out;
}

Node::VoteResult Node::process_vote(const Vote& v, std::string& err, bool persist) {
    if (v.height == 0 || v.height % p_.checkpoint_interval) return err = std::string("vote is not for a ") + errmsg::VOTE_CKPT, VoteResult::Rejected;
    bool post = p_.upgraded(v.height);
    if (post != v.compact)
        return err = post ? std::string("after activation a vote must reference the ") + errmsg::VOTE_REGISTRY + " (" + errmsg::VOTE_COMPACT + " form)" : std::string(errmsg::VOTE_COMPACT) + " vote form is allowed only after activation", VoteResult::Rejected;
    Hash256 prog = v.program();
    const PublicKey* key = &v.key;
    if (v.compact) {
        auto r = registry_.find(prog);
        if (r == registry_.end()) return err = std::string("validator is ") + errmsg::VOTE_UNREGISTERED + " in the " + errmsg::VOTE_REGISTRY, VoteResult::Rejected;
        key = &r->second.key;
    }
    auto hp = pool_.find(v.height);
    if (hp != pool_.end()) {
        auto bp = hp->second.find(v.block);
        if (bp != hp->second.end() && bp->second.count(prog)) return VoteResult::Known;
    }
    if (equivocators_.count(prog)) return err = "validator already convicted of double voting", VoteResult::Rejected;
    if (!verify(*key, v.message(), v.sig)) return err = std::string("vote ") + errmsg::VOTE_SIG + " invalid", VoteResult::Rejected;
    if (hp != pool_.end())
        for (auto& [blk, vm] : hp->second) {
            auto it = vm.find(prog);
            if (it == vm.end()) continue;
            // Two valid signatures on different blocks at one height: provable equivocation (the pair is the evidence)
            record_equivocation(it->second, v, prog);
            return err = std::string("validator voted for ") + errmsg::VOTE_EQUIVOCATION + " - its votes are no longer counted", VoteResult::Rejected;
        }
    bool on_chain = at(v.height) && at(v.height)->hash == v.block;
    if (on_chain) {
        const StakeSet* s = stake_at(v.height);
        if (!s || !s->count(prog)) return err = std::string("voter has no active ") + errmsg::VOTE_STAKE + " at this height", VoteResult::Rejected;
    } else if (!buffer_ok(v, prog, err)) {
        return VoteResult::Rejected;
    }
    pool_[v.height][v.block][prog] = v;
    if (persist) {
        Bytes raw = v.serialize();
        std::ofstream(dir_ / "votes" / (std::to_string(v.height) + (v.compact ? ".cvotes" : ".votes")), std::ios::binary | std::ios::app)
            .write(reinterpret_cast<const char*>(raw.data()), raw.size());
    }
    update_finality();
    return VoteResult::Accepted;
}

Node::VoteResult Node::accept_vote(const Vote& v, std::string& err) {
    std::lock_guard<std::mutex> lk(m_);
    VoteResult r = process_vote(v, err, true);
    if (r == VoteResult::Accepted && finality_candidate()) {   // enough buffered votes for a branch we already hold
        std::string e;
        activate_best(e);
    }
    return r;
}

// ---------------------------------------------------------------- key registry, evidence, extension rules

Node::VoteResult Node::accept_registration(const PublicKey& k, std::string& err) {
    std::lock_guard<std::mutex> lk(m_);
    if (k.mldsa.size() != MLDSA_PK) return err = "invalid key", VoteResult::Rejected;
    Hash256 prog = k.program();
    if (registry_.count(prog) || reg_pool_.count(prog)) return VoteResult::Known;
    if (!p_.upgraded(height() + 1)) return err = "key registry works only after activation (height " + std::to_string(p_.upgrade_height) + ")", VoteResult::Rejected;
    if (stake_of(prog) < p_.min_stake) return err = "key has no stake of at least the minimum - registration not needed", VoteResult::Rejected;
    if (reg_pool_.size() >= 64) return err = "registration queue full", VoteResult::Rejected;
    reg_pool_[prog] = k;
    return VoteResult::Accepted;
}

bool Node::check_evidence(const Evidence& e, bool sigs, std::string& err) const {
    if (e.height == 0 || e.height % p_.checkpoint_interval) return err = "evidence is not about a checkpoint", false;
    if (e.block_a == e.block_b) return err = "evidence: blocks are identical", false;
    auto r = registry_.find(e.vid);
    if (r == registry_.end()) return err = "evidence: validator not registered", false;
    if (sigs && (!verify(r->second.key, vote_message(e.height, e.block_a), e.sig_a) || !verify(r->second.key, vote_message(e.height, e.block_b), e.sig_b)))
        return err = "evidence: invalid signature", false;
    return true;
}

Node::VoteResult Node::accept_evidence(const Evidence& e, std::string& err) {
    std::lock_guard<std::mutex> lk(m_);
    if (slashed_.count(e.vid) || evidence_pool_.count(e.vid)) return VoteResult::Known;
    if (!check_evidence(e, true, err)) return VoteResult::Rejected;
    equivocators_.insert(e.vid);
    evidence_pool_[e.vid] = e;
    fs::create_directories(dir_ / "evidence");
    Bytes raw = e.serialize();
    std::ofstream(dir_ / "evidence" / (to_hex(e.vid) + ".evid"), std::ios::binary).write(reinterpret_cast<const char*>(raw.data()), raw.size());
    return VoteResult::Accepted;
}

bool Node::check_form(const Block& b, uint32_t h, std::string& err) const {
    const Bytes& d = b.txs[0].vin[0].coinbase_data;
    if (!p_.upgraded(h)) {
        if (b.has_ext) return err = "extension field before the activation height is forbidden", false;
        if (d.size() > 100) return err = "coinbase longer than 100 bytes", false;
        return true;
    }
    if (!b.has_ext) return err = "after activation a block must carry the extension field", false;
    if (d.size() < 4 + COMMIT_BYTES) return err = "coinbase lacks commitments (UTXO, checkpoint, extension)", false;
    auto same = [&](size_t off, const Hash256& x) { return std::equal(x.begin(), x.end(), d.begin() + off); };
    if (!same(4, utxo_commitment(utxo_))) return err = "UTXO commitment in coinbase mismatch", false;
    if (!same(36, ckpt_commit())) return err = "checkpoint commitment in coinbase mismatch", false;
    if (!same(68, b.ext.hash())) return err = "extension hash in coinbase mismatch", false;
    return true;
}

bool Node::check_ext(const Block& b, uint32_t h, bool sigs, std::string& err) const {
    const BlockExt& x = b.ext;
    std::set<Hash256> seen;
    for (auto& k : x.regs) {
        Hash256 prog = k.program();
        if (k.mldsa.size() != MLDSA_PK || registry_.count(prog) || !seen.insert(prog).second) return err = "duplicate key registration", false;
        if (stake_of(prog) < p_.min_stake) return err = "key registration without stake", false;
    }
    seen.clear();
    for (auto& e : x.evidence) {
        if (!check_evidence(e, sigs, err)) return false;
        if (slashed_.count(e.vid) || !seen.insert(e.vid).second) return err = "evidence against an already punished validator", false;
    }
    if (!x.cert) return true;
    const Cert& c = *x.cert;
    if (!p_.upgraded(c.height) || c.height % p_.checkpoint_interval || c.height + p_.finality_depth > h - 1) return err = "certificate: invalid checkpoint", false;
    if (c.height <= chain_final()) return err = "certificate: checkpoint not newer than the certified one", false;
    if (!at(c.height) || at(c.height)->hash != c.block) return err = "certificate: block not in this chain", false;
    const StakeSet* snap = stake_at(c.height);
    if (!snap) return err = "certificate: no stake snapshot", false;
    uint64_t total = total_stake(*snap), voted = 0;
    if (total < p_.min_total_stake || c.votes.empty()) return err = "certificate: not enough stake for finality", false;
    Hash256 msg = vote_message(c.height, c.block), prev{};
    for (size_t i = 0; i < c.votes.size(); i++) {
        const CertVote& v = c.votes[i];
        if (i && !(prev < v.vid)) return err = "certificate: votes not ordered", false;
        prev = v.vid;
        auto st = snap->find(v.vid);
        auto r = registry_.find(v.vid);
        if (st == snap->end() || r == registry_.end()) return err = "certificate: vote not from a registered validator with stake", false;
        if (sigs && !verify(r->second.key, msg, v.sig)) return err = "certificate: invalid vote signature", false;
        voted += st->second;
    }
    if (voted * 3 <= total * 2) return err = "certificate: less than 2/3 of stake", false;
    return true;
}

void Node::apply_ext(const Block& b, uint32_t h) {
    for (auto& k : b.ext.regs) {
        registry_[k.program()] = RegEntry{k, h};
        reg_pool_.erase(k.program());
    }
    for (auto& e : b.ext.evidence) {
        slashed_[e.vid] = h;
        equivocators_.insert(e.vid);
    }
    if (b.ext.cert) certs_[h] = CertInfo{b.ext.cert->height, b.ext.cert->block};
}

void Node::revert_ext(const Block& b, uint32_t h) {
    for (auto& k : b.ext.regs) {
        auto it = registry_.find(k.program());
        if (it != registry_.end() && it->second.height == h) registry_.erase(it);
    }
    for (auto& e : b.ext.evidence) {
        auto it = slashed_.find(e.vid);
        if (it != slashed_.end() && it->second == h) slashed_.erase(it);
    }
    certs_.erase(h);
}

// ---------------------------------------------------------------- commitments and fee share arithmetic

uint64_t mul_div(uint64_t a, uint64_t b, uint64_t c) {
    U256 n = U256::from_u64(a), lo = n, hi = n;
    lo.mul(uint32_t(b));
    hi.mul(uint32_t(b >> 32));
    hi.shl(32);
    U256 q = U256::divide(lo + hi, U256::from_u64(c));
    return uint64_t(q.w[0]) | uint64_t(q.w[1]) << 32;
}

Hash256 utxo_commitment(const UtxoView& u) {
    Bytes b;
    static const char* tag = "PHYLKA/utxo";
    b.insert(b.end(), tag, tag + std::strlen(tag));
    for (auto& [op, c] : u.coins) {   // std::map: ascending by (txid, vout)
        put_bytes(b, op.txid.data(), 32);
        put32(b, op.vout);
        put64(b, c.out.value);
        b.push_back(c.out.to.version);
        put_bytes(b, c.out.to.program.data(), 32);
        put32(b, c.height);
        b.push_back(c.coinbase ? 1 : 0);
    }
    return sha3_256(b);
}

std::vector<TxOut> voter_shares(const ChainParams& p, uint64_t fees, const Cert& c, const StakeSet& snap) {
    std::vector<TxOut> out;
    uint64_t pool = mul_div(fees, p.voter_fee_pct, 100), total = 0;
    for (auto& v : c.votes) { auto it = snap.find(v.vid); if (it != snap.end()) total += it->second; }
    if (!total) return out;
    for (auto& v : c.votes) {
        auto it = snap.find(v.vid);
        uint64_t share = it == snap.end() ? 0 : mul_div(pool, it->second, total);   // rounded down: the sum never exceeds the pool
        if (share) out.push_back(TxOut{share, Address{ADDRESS_VERSION_HYBRID, v.vid}});
    }
    return out;
}

void Node::store(const Block& b, const Hash256& h) {
    fs::path f = dir_ / "blocks" / (hash_hex(h) + ".blk");
    if (fs::exists(f)) return;
    Bytes raw = b.serialize();
    fs::path tmp = f;
    tmp += ".tmp";
    std::ofstream(tmp, std::ios::binary).write(reinterpret_cast<const char*>(raw.data()), raw.size());
    fs::rename(tmp, f);
}

std::optional<Block> Node::read_block(const Hash256& h) const {
    if (h == active_[0]->hash) return p_.genesis();
    fs::path f = dir_ / "blocks" / (hash_hex(h) + ".blk");
    if (!fs::exists(f)) return std::nullopt;
    return parse_block(read_file(f));
}

uint32_t Node::median_time_past(const BlockIndex* b) const {
    std::vector<uint32_t> t;
    for (int i = 0; i < 11 && b; i++, b = b->prev) t.push_back(b->header.time);
    std::sort(t.begin(), t.end());
    return t[t.size() / 2];
}

uint32_t Node::next_bits(const BlockIndex* prev) const {
    // LWMA (zawy12): a weighted average of recent solve times; recent blocks count more, so the
    // difficulty follows hashrate within a few blocks instead of Bitcoin's two weeks.
    if (prev->height < 2) return p_.genesis_bits;
    const uint32_t n = std::min(p_.lwma_window, prev->height);
    const int64_t T = p_.target_spacing;
    std::vector<const BlockIndex*> w;
    for (const BlockIndex* b = prev; w.size() < n + 1 && b; b = b->prev) w.push_back(b);
    std::reverse(w.begin(), w.end());   // oldest first; w[0] is only a time reference
    U256 sum_target;
    uint64_t weighted = 0;
    for (uint32_t i = 1; i < w.size(); i++) {
        int64_t st = int64_t(w[i]->header.time) - int64_t(w[i - 1]->header.time);
        st = std::clamp<int64_t>(st, 1, 6 * T);
        weighted += i * uint64_t(st);
        U256 t = U256::from_compact(w[i]->header.bits);
        t.div(n);
        sum_target.add(t);
    }
    uint64_t k = uint64_t(n) * (n + 1) / 2 * T;
    U256 limit = U256::from_compact(p_.pow_limit_bits);
    U256 next = sum_target;
    return lwma_scale(next, weighted, k, limit, p_.pow_limit_bits);
}

uint32_t lwma_scale(U256 target, uint64_t weighted, uint64_t k, const U256& limit, uint32_t limit_bits) {
    while (weighted > 0xffffffffULL || k > 0xffffffffULL) { weighted >>= 1; k >>= 1; }
    k = std::max<uint64_t>(k, 1);
    // target * weighted / k without overflowing 256 bits: divide first when the target is huge
    int wbits = 64 - clz64(weighted | 1);
    if (target.bits() + wbits > 255) {
        target.div(uint32_t(k));
        if (!target.mul(uint32_t(weighted))) return limit_bits;
    } else {
        target.mul(uint32_t(weighted));
        target.div(uint32_t(k));
    }
    if (!(target <= limit)) return limit_bits;
    return target.to_compact();
}

// ---------------------------------------------------------------- UTXO overlay

const Coin* UtxoOverlay::find(const OutPoint& op) const {
    auto a = added_.find(op);
    if (a != added_.end()) return &a->second;
    if (spent_.count(op)) return nullptr;
    auto b = base_.coins.find(op);
    return b == base_.coins.end() ? nullptr : &b->second;
}

bool UtxoOverlay::spend(const OutPoint& op, Coin* out) {
    const Coin* c = find(op);
    if (!c) return false;
    if (out) *out = *c;
    added_.erase(op);
    if (base_.coins.count(op)) spent_.insert(op);   // hide the base entry as well
    return true;
}

void UtxoOverlay::apply_to(UtxoView& base) const {
    for (auto& op : spent_) base.coins.erase(op);
    for (auto& [op, c] : added_) base.coins[op] = c;
}

// ---------------------------------------------------------------- validation

bool Node::check_tx_context(const Transaction& t, const CoinLookup& find, uint32_t spend_height, std::vector<TxOut>& spent, uint64_t& fee, std::string& err) const {
    if (t.vin.empty() || t.vout.empty()) return err = "empty inputs or outputs", false;
    if (t.witness.size() != t.vin.size()) return err = "signature count differs from input count", false;
    uint64_t out_sum = 0;
    for (auto& o : t.vout) {
        if (!known_address_version(o.to.version)) return err = "unknown address version", false;
        if (o.value == 0 || o.value > MAX_MONEY) return err = "invalid output amount", false;
        out_sum += o.value;
        if (out_sum > MAX_MONEY) return err = "outputs exceed 21 million", false;
    }
    std::set<OutPoint> seen;
    spent.clear();
    uint64_t in_sum = 0;
    for (auto& in : t.vin) {
        if (in.prev.null() || !in.coinbase_data.empty()) return err = "extra data in input", false;
        if (!seen.insert(in.prev).second) return err = "input spent twice in one transaction", false;
        Coin c;
        if (!find(in.prev, c)) return err = errmsg::INPUT_MISSING, false;
        if (c.coinbase && spend_height < c.height + p_.coinbase_maturity)
            return err = "mined coins are not mature yet (" + std::to_string(p_.coinbase_maturity) + " blocks)", false;
        if (c.out.to.version == ADDRESS_VERSION_STAKE && slashed_.count(c.out.to.program))
            return err = "stake frozen forever: validator punished for double voting", false;
        if (c.out.to.version == ADDRESS_VERSION_STAKE && spend_height < c.height + p_.stake_lock)
            return err = "stake locked for " + std::to_string(c.height + p_.stake_lock - spend_height) + " more blocks", false;
        spent.push_back(c.out);
        in_sum += c.out.value;
    }
    if (in_sum < out_sum) return err = "outputs exceed inputs", false;
    fee = in_sum - out_sum;
    return true;
}

/// Hybrid signature checks (the expensive part: ML-DSA); touches no node state, so it runs without the lock
static bool verify_tx_sigs(const Transaction& t, const std::vector<TxOut>& spent, std::string& err) {
    for (size_t i = 0; i < t.vin.size(); i++)
        if (!t.verify_input(i, spent, &err)) return false;
    return true;
}

static bool verify_block_sigs(const Block& b, const std::vector<std::vector<TxOut>>& spent, std::string& err) {
    for (size_t i = 1; i < b.txs.size(); i++)
        if (!verify_tx_sigs(b.txs[i], spent[i - 1], err)) return err = "transaction " + hash_hex(b.txs[i].txid()).substr(0, 12) + ": " + err, false;
    return true;
}

bool Node::connect_block(const Block& b, uint32_t height, UtxoOverlay& view, bool check_sigs, BlockUndo& undo, const ExtCtx& ctx, std::string& err) const {
    uint64_t fees = 0;
    auto find = [&](const OutPoint& op, Coin& c) {
        const Coin* p = view.find(op);
        if (p) c = *p;
        return p != nullptr;
    };
    for (size_t i = 1; i < b.txs.size(); i++) {
        const Transaction& t = b.txs[i];
        uint64_t fee = 0;
        std::vector<TxOut> spent;
        if (!check_tx_context(t, find, height, spent, fee, err) || (check_sigs && !verify_tx_sigs(t, spent, err)))
            return err = "transaction " + hash_hex(t.txid()).substr(0, 12) + ": " + err, false;
        fees += fee;
        for (auto& in : t.vin) {
            Coin c;
            view.spend(in.prev, &c);
            undo.push_back(c);
        }
        Hash256 id = t.txid();
        for (uint32_t o = 0; o < t.vout.size(); o++) view.add(OutPoint{id, o}, Coin{t.vout[o], height, false});
    }
    const Transaction& cb = b.txs[0];
    uint64_t cb_sum = 0;
    for (auto& o : cb.vout) {
        if (o.value > MAX_MONEY) return err = "invalid amount in coinbase", false;
        cb_sum += o.value;
    }
    if (cb_sum > block_reward(p_, height) + fees) return err = "coinbase takes more than reward and fees", false;
    if (p_.upgraded(height)) {
        // The last outputs must be exactly the voters' fee share (fees are redistributed, never minted)
        std::vector<TxOut> want;
        if (ctx.cert && ctx.snap) want = voter_shares(p_, fees, *ctx.cert, *ctx.snap);
        if (cb.vout.size() < want.size() + 1) return err = "coinbase: no fee share for voting validators", false;
        for (size_t i = 0; i < want.size(); i++) {
            const TxOut& o = cb.vout[cb.vout.size() - want.size() + i];
            if (o.value != want[i].value || o.to.version != want[i].to.version || o.to.program != want[i].to.program)
                return err = "coinbase: validator fee share computed incorrectly", false;
        }
    }
    if (height > 0) {
        Hash256 id = cb.txid();
        for (uint32_t o = 0; o < cb.vout.size(); o++) view.add(OutPoint{id, o}, Coin{cb.vout[o], height, true});
    }
    return true;
}

bool Node::collect_spent(const Block& b, uint32_t height, std::vector<std::vector<TxOut>>& spent) const {
    UtxoOverlay view(utxo_);
    auto find = [&](const OutPoint& op, Coin& c) {
        const Coin* p = view.find(op);
        if (p) c = *p;
        return p != nullptr;
    };
    std::string err;
    for (size_t i = 1; i < b.txs.size(); i++) {
        const Transaction& t = b.txs[i];
        uint64_t fee = 0;
        spent.emplace_back();
        if (!check_tx_context(t, find, height, spent.back(), fee, err)) return false;
        for (auto& in : t.vin) view.spend(in.prev);
        Hash256 id = t.txid();
        for (uint32_t o = 0; o < t.vout.size(); o++) view.add(OutPoint{id, o}, Coin{t.vout[o], height, false});
    }
    return true;
}

// ---------------------------------------------------------------- undo data and chain switching

static void put_u32(Bytes& b, uint32_t v) { for (int i = 0; i < 4; i++) b.push_back(uint8_t(v >> (8 * i))); }

void Node::write_undo(const Hash256& h, const BlockUndo& u) const {
    fs::path f = dir_ / "undo" / (hash_hex(h) + ".undo");
    if (fs::exists(f)) return;   // content is fixed by the block's ancestry
    Bytes raw;
    put_u32(raw, uint32_t(u.size()));
    for (auto& c : u) {
        for (int i = 0; i < 8; i++) raw.push_back(uint8_t(c.out.value >> (8 * i)));
        raw.push_back(uint8_t(c.out.to.version));
        raw.insert(raw.end(), c.out.to.program.begin(), c.out.to.program.end());
        put_u32(raw, c.height);
        raw.push_back(c.coinbase ? 1 : 0);
    }
    fs::path tmp = f;
    tmp += ".tmp";
    std::ofstream(tmp, std::ios::binary).write(reinterpret_cast<const char*>(raw.data()), raw.size());
    fs::rename(tmp, f);
}

bool Node::read_undo(const Hash256& h, const Block& b, BlockUndo& u) const {
    constexpr size_t REC = 8 + 1 + 32 + 4 + 1;
    fs::path f = dir_ / "undo" / (hash_hex(h) + ".undo");
    if (!fs::exists(f)) return false;
    Bytes raw = read_file(f);
    size_t want = 0;
    for (size_t i = 1; i < b.txs.size(); i++) want += b.txs[i].vin.size();
    if (raw.size() != 4 + want * REC) return false;
    auto le32 = [&](size_t o) { return uint32_t(raw[o]) | raw[o + 1] << 8 | raw[o + 2] << 16 | uint32_t(raw[o + 3]) << 24; };
    if (le32(0) != want) return false;
    u.clear();
    for (size_t i = 0; i < want; i++) {
        const uint8_t* p = &raw[4 + i * REC];
        Coin c;
        for (int k = 0; k < 8; k++) c.out.value |= uint64_t(p[k]) << (8 * k);
        c.out.to.version = p[8];
        std::copy(p + 9, p + 41, c.out.to.program.begin());
        c.height = uint32_t(p[41]) | p[42] << 8 | p[43] << 16 | uint32_t(p[44]) << 24;
        c.coinbase = p[45] != 0;
        u.push_back(c);
    }
    return true;
}

void Node::reset_state() {
    active_.resize(1);
    utxo_.coins.clear();
    snapshots_.clear();
    registry_.clear();
    slashed_.clear();
    certs_.clear();
}

bool Node::connect_tip(BlockIndex* bi, bool check_sigs, bool check_ext_sigs, std::string& err) {
    auto blk = read_block(bi->hash);
    if (!blk) return err = "block not read from disk", false;
    uint32_t h = bi->height;
    bool post = p_.upgraded(h);
    if (!check_form(*blk, h, err)) return false;   // also compares the commitments with the state before this block
    if (h % p_.checkpoint_interval == 0) snapshots_[h] = snapshot(h);   // stake as of the block before
    if (post && !check_ext(*blk, h, check_ext_sigs, err)) {
        snapshots_.erase(h);
        return false;
    }
    if (post) apply_ext(*blk, h);
    UtxoOverlay view(utxo_);
    BlockUndo undo;
    ExtCtx ctx;
    if (post && blk->ext.cert) { ctx.cert = &*blk->ext.cert; ctx.snap = stake_at(blk->ext.cert->height); }
    if (!connect_block(*blk, h, view, check_sigs, undo, ctx, err)) {
        if (post) revert_ext(*blk, h);
        snapshots_.erase(h);
        return false;
    }
    view.apply_to(utxo_);
    write_undo(bi->hash, undo);
    active_.push_back(bi);
    return true;
}

bool Node::disconnect_tip(std::vector<Transaction>* returned) {
    BlockIndex* t = active_.back();
    auto blk = read_block(t->hash);
    BlockUndo u;
    if (!blk || !read_undo(t->hash, *blk, u)) return false;   // nothing has been changed yet
    size_t k = u.size();
    for (size_t i = blk->txs.size(); i-- > 0;) {
        const Transaction& tx = blk->txs[i];
        Hash256 id = tx.txid();
        for (uint32_t o = 0; o < tx.vout.size(); o++) utxo_.coins.erase(OutPoint{id, o});
        if (i == 0) break;
        for (size_t j = tx.vin.size(); j-- > 0;) utxo_.coins[tx.vin[j].prev] = u[--k];
    }
    snapshots_.erase(t->height);   // taken before the block was connected
    if (p_.upgraded(t->height) && blk->has_ext) revert_ext(*blk, t->height);
    active_.pop_back();
    if (returned) returned->insert(returned->begin(), blk->txs.begin() + 1, blk->txs.end());   // oldest block first overall
    return true;
}

bool Node::disconnect_to(size_t keep, std::vector<Transaction>* returned) {
    while (active_.size() > keep)
        if (!disconnect_tip(returned)) return false;
    return true;
}

BlockIndex* Node::connect_from(const std::vector<BlockIndex*>& path, size_t from, bool check_sigs, std::string& err) {
    for (size_t i = from; i < path.size(); i++) {
        bool presigned = has_presigned_ && path[i]->hash == presigned_;
        if (!connect_tip(path[i], check_sigs && !presigned, check_sigs, err)) return path[i];
    }
    return nullptr;
}

static size_t common_prefix(const std::vector<BlockIndex*>& a, const std::vector<BlockIndex*>& b) {
    size_t n = 0;
    while (n < a.size() && n < b.size() && a[n] == b[n]) n++;
    return n;
}

bool Node::set_active(const std::vector<BlockIndex*>& path, std::string& err, std::vector<Transaction>& returned) {
    std::vector<BlockIndex*> old = active_;
    std::vector<Transaction> ret;
    size_t fork = common_prefix(path, active_);
    if (!disconnect_to(fork, &ret)) {   // undo data missing or damaged: replay from genesis like a fresh start
        reset_state();
        fork = 1;
        ret.clear();
    }
    if (BlockIndex* bad = connect_from(path, fork, true, err)) {
        bad->invalid = true;
        // Back to the old chain (its blocks were valid: no signature checks needed)
        size_t back = common_prefix(old, active_);
        if (!disconnect_to(back, nullptr)) {
            reset_state();
            back = 1;
        }
        std::string e2;
        if (connect_from(old, back, false, e2)) {
            reset_state();
            connect_from(old, 1, true, e2);
        }
        return false;
    }
    returned = std::move(ret);
    return true;
}

bool Node::activate_best(std::string& err) {
    for (;;) {
        BlockIndex* best = active_.back();
        // Finality beats work: validators with > 2/3 of the stake signed a block that is not on our chain. Their branch (its
        // heaviest tip) wins even over a heavier branch of ours; if it does not confirm the votes it is not tried again.
        const BlockIndex* cand = finality_candidate();
        const BlockIndex* pref = nullptr;
        if (cand) best = nullptr;
        for (auto& [h, bi] : index_) {
            bool bad = false;
            for (BlockIndex* a = bi.get(); a && !bad; a = a->prev) bad = a->invalid;
            if (bad || !allowed_by_finality(bi.get()) || !allowed_by_checkpoints(bi.get())) continue;
            if (cand) {
                const BlockIndex* a = bi.get();
                while (a && a->height > cand->height) a = a->prev;
                if (a != cand) continue;
                if (!best || bi->chainwork > best->chainwork) best = bi.get();
            } else if (bi->chainwork > best->chainwork) best = bi.get();
        }
        if (cand) pref = cand;
        if (!best) best = active_.back();
        if (best == active_.back()) return true;

        std::vector<BlockIndex*> path;
        for (BlockIndex* a = best; a; a = a->prev) path.push_back(a);
        std::reverse(path.begin(), path.end());

        std::vector<Transaction> returned;
        if (!set_active(path, err, returned)) continue;   // that branch is invalid: try the next best one
        reload_mempool(std::move(returned));
        update_finality();
        if (pref && final_h_ < pref->height) vote_rejected_.insert(pref->hash);   // the votes did not hold on that branch
        tip_version++;
        return true;
    }
}

// ---------------------------------------------------------------- mempool (relay policy, not consensus)

bool Node::mem_coin(const OutPoint& op, Coin& c) const {
    if (mem_spent_.count(op)) return false;   // already spent by a waiting transaction
    auto u = utxo_.coins.find(op);
    if (u != utxo_.coins.end()) { c = u->second; return true; }
    auto m = mempool_.find(op.txid);   // output of a waiting parent
    if (m == mempool_.end() || op.vout >= m->second.vout.size()) return false;
    c = Coin{m->second.vout[op.vout], height() + 1, false};
    return true;
}

bool Node::mem_precheck(const Transaction& t, size_t size, std::vector<TxOut>& spent, uint64_t& fee, std::string& err) const {
    auto find = [&](const OutPoint& op, Coin& c) { return mem_coin(op, c); };
    if (!check_tx_context(t, find, height() + 1, spent, fee, err)) return false;
    if (fee < size * MIN_RELAY_FEE_PER_BYTE)
        return err = std::string(errmsg::FEE_LOW) + ": need at least " + std::to_string(size * MIN_RELAY_FEE_PER_BYTE) + " fyl (" +
                     std::to_string(MIN_RELAY_FEE_PER_BYTE) + " fyl per byte), got " + std::to_string(fee), false;
    return true;
}

static long double fee_rate(uint64_t fee, size_t size) { return (long double)fee / (long double)std::max<size_t>(size, 1); }

bool Node::mem_full_for(size_t size, uint64_t fee) const {
    if (mem_bytes_ + size <= mem_limit_) return false;
    long double lowest = -1;
    for (auto& [id, mi] : minfo_) {
        long double r = fee_rate(mi.fee, mi.size);
        if (lowest < 0 || r < lowest) lowest = r;
    }
    return lowest >= 0 && fee_rate(fee, size) <= lowest;   // would be the first to be evicted
}

void Node::mem_remove_tree(const Hash256& id) {
    std::vector<Hash256> todo{id};
    while (!todo.empty()) {
        Hash256 x = todo.back();
        todo.pop_back();
        auto it = mempool_.find(x);
        if (it == mempool_.end()) continue;
        for (uint32_t o = 0; o < it->second.vout.size(); o++) {
            auto child = mem_spent_.find(OutPoint{x, o});
            if (child != mem_spent_.end()) todo.push_back(child->second);
        }
        for (auto& in : it->second.vin) mem_spent_.erase(in.prev);
        mem_bytes_ -= minfo_[x].size;
        minfo_.erase(x);
        mempool_.erase(it);
    }
}

void Node::mem_trim() {
    while (mem_bytes_ > mem_limit_ && !minfo_.empty()) {
        auto worst = minfo_.begin();   // lowest fee rate; among equals the newest goes first
        for (auto it = minfo_.begin(); it != minfo_.end(); ++it) {
            long double a = fee_rate(it->second.fee, it->second.size), b = fee_rate(worst->second.fee, worst->second.size);
            if (a < b || (a == b && it->second.seq > worst->second.seq)) worst = it;
        }
        mem_remove_tree(Hash256(worst->first));
    }
}

bool Node::mem_insert(const Hash256& id, const Transaction& t, size_t size, uint64_t fee, std::string& err) {
    mempool_[id] = t;
    minfo_[id] = MemInfo{fee, size, mem_seq_++};
    for (auto& in : t.vin) mem_spent_[in.prev] = id;
    mem_bytes_ += size;
    mem_trim();
    if (!mempool_.count(id)) return err = std::string(errmsg::MEMPOOL_FULL) + ": fee is lower than the other transactions", false;
    return true;
}

void Node::reload_mempool(std::vector<Transaction> returned) {
    // Transactions of disconnected blocks first, then the old pool in arrival order (parents before children)
    std::vector<std::pair<uint64_t, Hash256>> order;
    for (auto& [id, mi] : minfo_) order.push_back({mi.seq, id});
    std::sort(order.begin(), order.end());
    for (auto& [seq, id] : order) returned.push_back(std::move(mempool_[id]));
    mempool_.clear();
    minfo_.clear();
    mem_spent_.clear();
    mem_bytes_ = 0;
    for (auto& t : returned) {
        Hash256 id = t.txid();
        if (mempool_.count(id)) continue;
        size_t size = t.serialize().size();
        std::vector<TxOut> spent;
        uint64_t fee;
        std::string e;
        if (mem_precheck(t, size, spent, fee, e)) mem_insert(id, t, size, fee, e);   // signatures were checked when it first arrived
    }
}

// ---------------------------------------------------------------- accepting blocks and transactions

bool Node::accept_block(const Block& b, std::string& err, bool* missing_parent, bool* new_tip) {
    std::unique_lock<std::mutex> lk(m_);
    if (missing_parent) *missing_parent = false;
    if (new_tip) *new_tip = false;
    Hash256 h = b.header.hash();
    if (index_.count(h)) return err = errmsg::BLOCK_KNOWN, true;
    auto parent = index_.find(b.header.prev);
    if (parent == index_.end()) {
        if (missing_parent) *missing_parent = true;
        return err = "unknown previous block", false;
    }
    BlockIndex* prev = parent->second.get();
    if (prev->invalid) return err = "extends an invalid branch", false;
    if (violates_checkpoint(prev->height + 1, h)) return err = "block contradicts a hard-coded checkpoint", false;
    if (!check_pow(h, b.header.bits)) return err = "insufficient work (hash above target)", false;
    if (b.header.bits != next_bits(prev)) return err = "wrong difficulty", false;
    if (b.header.time <= median_time_past(prev)) return err = "block time too early", false;
    if (b.header.time > uint32_t(std::time(nullptr)) + 7200) return err = errmsg::FUTURE_BLOCK, false;
    if (b.txs.empty() || !b.txs[0].is_coinbase()) return err = "first transaction must be coinbase", false;
    for (size_t i = 1; i < b.txs.size(); i++)
        if (b.txs[i].is_coinbase()) return err = "extra coinbase", false;
    if (coinbase_height(b.txs[0]) != prev->height + 1) return err = "wrong height in coinbase", false;
    if (b.header.merkle != b.merkle_root()) return err = "Merkle root mismatch", false;
    if (b.has_ext != p_.upgraded(prev->height + 1))
        return err = b.has_ext ? "extension field before the activation height is forbidden" : "after activation a block must carry the extension field", false;
    if (b.serialize().size() > MAX_BLOCK_BYTES) return err = "block larger than 4 MB", false;

    store(b, h);
    auto bi = std::make_unique<BlockIndex>();
    bi->hash = h;
    bi->header = b.header;
    bi->prev = prev;
    bi->height = prev->height + 1;
    bi->chainwork = prev->chainwork + U256::from_compact(b.header.bits).work();
    BlockIndex* raw = bi.get();
    index_[h] = std::move(bi);
    const BlockIndex* old_tip = active_.back();

    // A block on top of the tip: find the outputs it spends under the lock, verify the signatures without it,
    // and connect only if the tip is still the same (otherwise start over; after 3 tries verify under the lock).
    for (int attempt = 0; attempt < 3; attempt++) {
        const BlockIndex* base = active_.back();
        std::vector<std::vector<TxOut>> spent;
        if (raw->invalid || raw->prev != base || b.txs.size() < 2 || !collect_spent(b, raw->height, spent)) break;
        std::string serr;
        lk.unlock();
        bool sigs_ok = verify_block_sigs(b, spent, serr);
        lk.lock();
        if (raw->invalid) break;
        if (active_.back() != base) continue;
        if (!sigs_ok) {
            raw->invalid = true;
            return err = serr, false;
        }
        presigned_ = h;
        has_presigned_ = true;
        break;
    }
    activate_best(err);
    has_presigned_ = false;
    if (raw->invalid) return false;
    if (new_tip) *new_tip = active_.back() != old_tip;
    err.clear();
    return true;
}

bool Node::accept_tx(const Transaction& t, std::string& err) {
    Hash256 id = t.txid();
    size_t size = t.serialize().size();
    for (int attempt = 0; attempt < 3; attempt++) {
        std::vector<TxOut> spent;
        uint64_t fee = 0;
        {
            std::lock_guard<std::mutex> lk(m_);   // cheap checks and the UTXO lookup
            if (mempool_.count(id)) return err = errmsg::IN_MEMPOOL, true;
            if (t.is_coinbase()) return err = "a coinbase cannot be sent on its own", false;
            if (size > 200'000) return err = "transaction too large", false;
            if (!mem_precheck(t, size, spent, fee, err)) return false;
            if (mem_full_for(size, fee)) return err = std::string(errmsg::MEMPOOL_FULL) + ": fee is lower than the other transactions", false;
        }
        if (!verify_tx_sigs(t, spent, err)) return false;   // ML-DSA, outside the lock
        std::lock_guard<std::mutex> lk(m_);
        if (mempool_.count(id)) return err = errmsg::IN_MEMPOOL, true;
        std::vector<TxOut> again;
        uint64_t fee2 = 0;
        if (!mem_precheck(t, size, again, fee2, err)) return false;   // inputs may have been spent meanwhile
        bool same = again.size() == spent.size();
        for (size_t i = 0; same && i < spent.size(); i++)
            same = again[i].value == spent[i].value && again[i].to.version == spent[i].to.version && again[i].to.program == spent[i].to.program;
        if (!same) continue;   // an input now means something else: the signatures must be checked again
        return mem_insert(id, t, size, fee2, err);
    }
    return err = "chain state changed during validation, try again", false;
}

Block Node::block_template(const Address& miner, uint64_t extranonce) {
    std::lock_guard<std::mutex> lk(m_);
    Block b;
    uint32_t h = height() + 1;
    uint64_t fees = 0;
    size_t size = 200;
    // Highest fee rate first among transactions whose in-mempool parents are already placed
    auto parents = [&](const Transaction& t) {
        std::set<Hash256> ps;
        for (auto& in : t.vin)
            if (!utxo_.coins.count(in.prev) && mempool_.count(in.prev.txid)) ps.insert(in.prev.txid);
        return ps;
    };
    auto worse = [&](const Hash256& a, const Hash256& c) {
        const MemInfo &x = minfo_.at(a), &y = minfo_.at(c);
        long double rx = fee_rate(x.fee, x.size), ry = fee_rate(y.fee, y.size);
        return rx != ry ? rx < ry : x.seq > y.seq;
    };
    std::priority_queue<Hash256, std::vector<Hash256>, decltype(worse)> ready(worse);
    std::map<Hash256, size_t> waiting;
    for (auto& [id, t] : mempool_) {
        size_t n = parents(t).size();
        if (n) waiting[id] = n;
        else ready.push(id);
    }
    while (!ready.empty()) {
        Hash256 id = ready.top();
        ready.pop();
        const MemInfo& mi = minfo_.at(id);
        if (size + mi.size > MAX_BLOCK_BYTES - 1000) continue;   // does not fit; its children will not be placed either
        const Transaction& t = mempool_.at(id);
        fees += mi.fee;
        size += mi.size;
        b.txs.push_back(t);
        std::set<Hash256> kids;
        for (uint32_t o = 0; o < t.vout.size(); o++) {
            auto c = mem_spent_.find(OutPoint{id, o});
            if (c != mem_spent_.end()) kids.insert(c->second);
        }
        for (auto& k : kids)
            if (--waiting[k] == 0) ready.push(k);
    }
    std::string text;
    for (int i = 0; i < 8; i++) text += char(extranonce >> (8 * i));
    text += "/PHYLKA/";
    if (p_.upgraded(h)) {
        // Extension first (the coinbase commits to it), then the fee share for the voters of its certificate
        b.has_ext = true;
        b.ext = build_ext();
        Bytes commit;
        Hash256 u = utxo_commitment(utxo_), k = ckpt_commit(), x = b.ext.hash();
        commit.insert(commit.end(), u.begin(), u.end());
        commit.insert(commit.end(), k.begin(), k.end());
        commit.insert(commit.end(), x.begin(), x.end());
        Transaction cb = make_coinbase(p_, h, miner, text, fees, commit);
        if (b.ext.cert) {
            std::vector<TxOut> shares = voter_shares(p_, fees, *b.ext.cert, *stake_at(b.ext.cert->height));
            for (auto& o : shares) { cb.vout[0].value -= o.value; cb.vout.push_back(o); }
        }
        b.txs.insert(b.txs.begin(), cb);
    } else {
        b.txs.insert(b.txs.begin(), make_coinbase(p_, h, miner, text, fees));
    }
    b.header.version = 1;
    b.header.prev = tip()->hash;
    b.header.time = std::max<uint32_t>(uint32_t(std::time(nullptr)), median_time_past(tip()) + 1);
    b.header.bits = next_bits(tip());
    b.header.merkle = b.merkle_root();
    return b;
}

/// Extension for the next block: queued registrations and evidence, and a certificate for a newly finalized checkpoint
BlockExt Node::build_ext() const {
    BlockExt x;
    for (auto& [prog, k] : reg_pool_)
        if (x.regs.size() < MAX_EXT_REGS && !registry_.count(prog) && stake_of(prog) >= p_.min_stake) x.regs.push_back(k);
    std::string err;
    for (auto& [vid, e] : evidence_pool_)
        if (x.evidence.size() < MAX_EXT_EVIDENCE && !slashed_.count(vid) && check_evidence(e, false, err)) x.evidence.push_back(e);
    for (auto it = snapshots_.rbegin(); it != snapshots_.rend(); ++it) {
        uint32_t cp = it->first;
        if (cp <= chain_final() || !p_.upgraded(cp)) break;
        if (cp + p_.finality_depth > height() || !at(cp)) continue;
        uint64_t total = total_stake(it->second);
        auto vs = active_votes(cp);
        if (total < p_.min_total_stake || !vs) continue;
        Cert c;
        c.height = cp;
        c.block = at(cp)->hash;
        uint64_t voted = 0;
        for (auto& [prog, v] : *vs) {   // ascending by validator id
            if (!v.compact || !it->second.count(prog) || !registry_.count(prog)) continue;
            c.votes.push_back(CertVote{prog, v.sig});
            voted += it->second.at(prog);
        }
        if (voted * 3 > total * 2 && c.votes.size() <= MAX_CERT_VOTES) { x.cert = std::move(c); break; }
    }
    return x;
}

bool Node::mining_blocked(size_t peers, time_t now, std::string& why) const {
    std::lock_guard<std::mutex> lk(m_);
    uint32_t top = p_.checkpoints.empty() ? 0 : p_.checkpoints.back().first;
    if (height() < top) return why = "height " + std::to_string(height()) + " is below the hard-coded checkpoint " + std::to_string(top), true;
    if (p_.name == "main" && peers == 0 && now > time_t(tip()->header.time) + 24 * 3600)
        return why = "no peers and the last block is older than a day", true;
    return false;
}

// ---------------------------------------------------------------- miner

Miner::Miner(Node& n, Address to, int threads, std::function<void(const Block&)> on_found, std::function<bool()> may_mine)
    : node_(n), to_(to), threads_(std::max(1, threads)), on_found_(std::move(on_found)), may_mine_(std::move(may_mine)), thread_([this] { run(); }) {}

Miner::~Miner() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
}

void Miner::run() {
    uint64_t extranonce = 0;
    while (!stop_) {
        if (may_mine_ && !may_mine_()) {   // the guard logs the reason
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }
        uint64_t version = node_.tip_version;
        Block b = node_.block_template(to_, extranonce++);
        std::atomic<bool> found{false};
        std::atomic<uint64_t> hashes{0};
        uint32_t found_nonce = 0;
        std::mutex fm;
        auto t0 = std::chrono::steady_clock::now();
        std::vector<std::thread> workers;
        for (int w = 0; w < threads_; w++) {
            workers.emplace_back([&, w] {
                HeaderHasher hasher(b.header.serialize());
                uint64_t span = (1ULL << 32) / threads_;
                uint32_t start = uint32_t(w * span), end = uint32_t(w == threads_ - 1 ? 0xffffffffULL : (w + 1) * span - 1);
                for (uint64_t nonce = start; nonce <= end; nonce++) {
                    if (check_pow(hasher.hash(uint32_t(nonce)), b.header.bits)) {
                        std::lock_guard<std::mutex> lk(fm);
                        if (!found) { found_nonce = uint32_t(nonce); found = true; }
                    }
                    if ((nonce & 0xffff) == 0xffff) {
                        hashes += 0x10000;
                        if (found || stop_ || node_.tip_version != version) return;
                        double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
                        if (w == 0 && s > 1) hashrate_ = hashes / s;
                        if (s > 30) return;   // refresh the template (new transactions, timestamp)
                    }
                }
            });
        }
        for (auto& t : workers) t.join();
        if (found && !stop_) {
            b.header.nonce = found_nonce;
            on_found_(b);
        }
    }
}

}  // namespace plk
