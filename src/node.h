// PHYLKA node state: block tree, active chain, UTXO set, mempool, block templates and the miner.
// Simple by design: blocks are files in <datadir>/blocks, the UTXO set is rebuilt by replaying the
// active chain at start. A reorganisation disconnects blocks with per-block undo data (<datadir>/undo,
// regenerated from the blocks if missing) and connects the new branch.
#pragma once
#include <atomic>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>

#include "arith.h"
#include "chain.h"

namespace plk {

constexpr size_t MAX_BLOCK_BYTES = 4'000'000;
// Relay policy only (not consensus): minimum fee per serialized byte (with witness) and the mempool size cap
constexpr uint64_t MIN_RELAY_FEE_PER_BYTE = 1;
constexpr size_t MAX_MEMPOOL_BYTES = 50'000'000;

/// Error texts that other code matches on (peer-scoring policy in net.cpp, selftest). Messages
/// returned by Node are built from these constants, so rewording them cannot break the matching.
namespace errmsg {
constexpr const char* INPUT_MISSING = "input not found or already spent";   // double-spend race or reorg, not malice
constexpr const char* FEE_LOW = "fee too low";
constexpr const char* MEMPOOL_FULL = "mempool full";
constexpr const char* FUTURE_BLOCK = "block time is in the future";
constexpr const char* BLOCK_KNOWN = "block already known";
constexpr const char* IN_MEMPOOL = "already in mempool";
constexpr const char* VOTE_EQUIVOCATION = "two different blocks";
constexpr const char* VOTE_REGISTRY = "key registry";
constexpr const char* VOTE_UNREGISTERED = "not registered";
constexpr const char* VOTE_COMPACT = "compact";
constexpr const char* VOTE_SIG = "signature";
constexpr const char* VOTE_STAKE = "stake";
constexpr const char* VOTE_CKPT = "checkpoint";
}
inline bool err_has(const std::string& err, const char* what) { return err.find(what) != std::string::npos; }

bool operator<(const OutPoint& a, const OutPoint& b);
/// target * weighted_solvetimes / k, capped at the easiest allowed target (the core of LWMA)
uint32_t lwma_scale(U256 target, uint64_t weighted, uint64_t k, const U256& limit, uint32_t limit_bits);

struct BlockIndex {
    Hash256 hash{};
    BlockHeader header;
    uint32_t height = 0;
    U256 chainwork;   // exact total work up to and including this block
    BlockIndex* prev = nullptr;
    bool invalid = false;
};

struct Coin {
    TxOut out;
    uint32_t height = 0;
    bool coinbase = false;
};

struct UtxoView {
    std::map<OutPoint, Coin> coins;
};

/// Coins spent by a block, in input order of its non-coinbase transactions (enough to disconnect it)
using BlockUndo = std::vector<Coin>;

/// Cheap copy-free layer over a UtxoView: spends and new outputs stay in the overlay until apply_to().
class UtxoOverlay {
public:
    explicit UtxoOverlay(const UtxoView& base) : base_(base) {}
    const Coin* find(const OutPoint& op) const;
    /// Removes the coin; false if it does not exist. The spent coin is copied to *out when given.
    bool spend(const OutPoint& op, Coin* out = nullptr);
    void add(const OutPoint& op, Coin c) { added_[op] = std::move(c); }
    void apply_to(UtxoView& base) const;

private:
    const UtxoView& base_;
    std::map<OutPoint, Coin> added_;
    std::set<OutPoint> spent_;   // coins of base_ that are gone
};

/// A validator's signature on a checkpoint block. Same hybrid keys as an address (Schnorr + ML-DSA-44).
/// Before the upgrade a vote carries the 1312-byte ML-DSA key; from upgrade_height it names a registered validator id instead.
struct Vote {
    uint32_t height = 0;
    Hash256 block{};
    PublicKey key;          // full form only
    HybridSignature sig;
    bool compact = false;   // compact form: `vid` (the address program) refers to the on-chain key registry
    Hash256 vid{};

    Hash256 message() const;   // tagged hash of (height, block)
    Hash256 program() const { return compact ? vid : key.program(); }
    Bytes serialize() const;
    static std::optional<Vote> parse(const Bytes& b, size_t& pos);          // full form (3864 bytes)
    static std::optional<Vote> parse_compact(const Bytes& b, size_t& pos);  // compact form (2552 bytes)
};
constexpr size_t VOTE_FULL_BYTES = 4 + 32 + SCHNORR_PK + MLDSA_PK + SCHNORR_SIG + MLDSA_SIG;
constexpr size_t VOTE_COMPACT_BYTES = 4 + 32 + 32 + SCHNORR_SIG + MLDSA_SIG;
constexpr size_t REG_BYTES = SCHNORR_PK + MLDSA_PK;

/// Stake per validator (address program) at one checkpoint
using StakeSet = std::map<Hash256, uint64_t>;

/// a * b / c without overflow (128-bit intermediate); c != 0 and the result must fit 64 bits
uint64_t mul_div(uint64_t a, uint64_t b, uint64_t c);
/// SHA3-256 over every unspent output in key order (see PROTOCOL.md). O(n) per call: simple and auditable, not incremental.
Hash256 utxo_commitment(const UtxoView& u);
/// Coinbase outputs owed to the voters of a certificate: pct% of the fees, pro rata to their snapshot stake (zero shares omitted)
std::vector<TxOut> voter_shares(const ChainParams& p, uint64_t fees, const Cert& c, const StakeSet& snap);

struct RegEntry { PublicKey key; uint32_t height = 0; };   // height = block that registered the key
struct CertInfo { uint32_t cp_h = 0; Hash256 cp_hash{}; };

class Node {
public:
    Node(const ChainParams& p, std::filesystem::path datadir);

    const ChainParams& params() const { return p_; }
    std::mutex& mutex() { return m_; }   // guards everything below

    uint32_t height() const { return uint32_t(active_.size() - 1); }
    const BlockIndex* tip() const { return active_.back(); }
    const BlockIndex* at(uint32_t h) const { return h < active_.size() ? active_[h] : nullptr; }
    /// true if a block with this hash at this height violates a hard-coded checkpoint
    bool violates_checkpoint(uint32_t height, const Hash256& h) const;
    const std::filesystem::path& datadir() const { return dir_; }
    /// Mining guard: true (with a reason) while a miner would only build a useless private branch:
    /// below the highest hard-coded checkpoint, or on main with no peers while the tip is older than 24 h (initial sync).
    bool mining_blocked(size_t peers, time_t now, std::string& why) const;
    const Hash256& genesis_hash() const { return genesis_hash_; }
    bool known(const Hash256& h) const { return index_.count(h) > 0; }
    std::optional<Block> read_block(const Hash256& h) const;

    /// Validates and stores a block; switches to it if it extends the most-work chain.
    /// Returns false with a reason if it is invalid; `missing_parent` is set when we need its parent first.
    bool accept_block(const Block& b, std::string& err, bool* missing_parent = nullptr, bool* new_tip = nullptr);
    bool accept_tx(const Transaction& t, std::string& err);

    uint32_t next_bits(const BlockIndex* prev) const;
    uint32_t median_time_past(const BlockIndex* b) const;
    Block block_template(const Address& miner, uint64_t extranonce);

    // ---- validator finality
    enum class VoteResult { Accepted, Known, Rejected };
    /// Checks and records a vote; may finalize a checkpoint. Locks the node.
    VoteResult accept_vote(const Vote& v, std::string& err);
    uint32_t finalized_height() const { return final_h_; }
    const Hash256& finalized_hash() const { return final_hash_; }
    /// Stake snapshot of a checkpoint on the active chain (nullptr if it is not a checkpoint)
    const StakeSet* stake_at(uint32_t checkpoint) const;
    uint64_t total_stake(const StakeSet& s) const;
    /// Stake that voted for the active block at a checkpoint
    uint64_t voted_stake(uint32_t checkpoint) const;
    std::vector<Vote> votes_from(uint32_t height) const;
    bool voted(uint32_t height, const Hash256& program) const;
    /// True if this validator has signed ANY block at this height (on any branch). A second signature would be provable
    /// equivocation, so a validator must never sign at a height where this is true.
    bool voted_any(uint32_t height, const Hash256& program) const;
    /// Height of a block we do not have although validators holding more than 2/3 of our stake signed it (0 = none):
    /// the network should be asked for that branch ("finality beats work").
    uint32_t finality_hint() const;
    size_t equivocators() const { return equivocators_.size(); }

    // ---- phase 3 (active from params().upgrade_height)
    /// Key registry: queued locally until a miner includes it in a block (needs a stake of at least min_stake)
    VoteResult accept_registration(const PublicKey& k, std::string& err);
    VoteResult accept_evidence(const Evidence& e, std::string& err);
    std::vector<Evidence> take_new_evidence();   // evidence found since the last call (to relay)
    bool registered(const Hash256& vid) const { return registry_.count(vid) > 0; }
    size_t registry_size() const { return registry_.size(); }
    bool slashed(const Hash256& vid) const { return slashed_.count(vid) > 0; }
    size_t slashed_count() const { return slashed_.size(); }
    /// Highest checkpoint justified by a certificate in the active chain (finality from chain data alone)
    uint32_t chain_final() const { return certs_.empty() ? 0 : certs_.rbegin()->second.cp_h; }
    Hash256 ckpt_commit() const;
    Hash256 utxo_commit() const { return utxo_commitment(utxo_); }
    size_t vote_pool_size() const;

    /// Relay policy knob (tests use a tiny cap); the default is MAX_MEMPOOL_BYTES
    void set_mempool_limit(size_t bytes) { mem_limit_ = bytes; }
    const UtxoView& utxo() const { return utxo_; }
    const std::map<Hash256, Transaction>& mempool() const { return mempool_; }
    std::atomic<uint64_t> tip_version{0};   // bumps whenever the tip changes (miners restart)

private:
    using CoinLookup = std::function<bool(const OutPoint&, Coin&)>;
    struct MemInfo { uint64_t fee = 0; size_t size = 0; uint64_t seq = 0; };

    struct ExtCtx { const Cert* cert = nullptr; const StakeSet* snap = nullptr; };
    bool connect_block(const Block& b, uint32_t height, UtxoOverlay& view, bool check_sigs, BlockUndo& undo, const ExtCtx& ctx, std::string& err) const;
    // phase 3 block rules
    bool check_form(const Block& b, uint32_t h, std::string& err) const;
    bool check_ext(const Block& b, uint32_t h, bool sigs, std::string& err) const;
    bool check_evidence(const Evidence& e, bool sigs, std::string& err) const;
    void apply_ext(const Block& b, uint32_t h);
    void revert_ext(const Block& b, uint32_t h);
    BlockExt build_ext() const;
    uint64_t stake_of(const Hash256& prog) const;
    VoteResult process_vote(const Vote& v, std::string& err, bool persist);
    bool buffer_ok(const Vote& v, const Hash256& prog, std::string& err) const;
    void record_equivocation(const Vote& first, const Vote& second, const Hash256& prog);
    const std::map<Hash256, Vote>* active_votes(uint32_t cp) const;
    void load_evidence();
    /// Everything about a transaction's inputs except signatures; fills the spent outputs and the fee
    bool check_tx_context(const Transaction& t, const CoinLookup& find, uint32_t spend_height, std::vector<TxOut>& spent, uint64_t& fee, std::string& err) const;
    /// Spent outputs of every non-coinbase transaction of a block extending the tip (for unlocked signature checks)
    bool collect_spent(const Block& b, uint32_t height, std::vector<std::vector<TxOut>>& spent) const;
    bool activate_best(std::string& err);
    // chain switching with undo data
    bool set_active(const std::vector<BlockIndex*>& path, std::string& err, std::vector<Transaction>& returned);
    bool connect_tip(BlockIndex* bi, bool check_sigs, bool check_ext_sigs, std::string& err);
    bool disconnect_tip(std::vector<Transaction>* returned);
    bool disconnect_to(size_t keep, std::vector<Transaction>* returned);
    BlockIndex* connect_from(const std::vector<BlockIndex*>& path, size_t from, bool check_sigs, std::string& err);
    void reset_state();
    bool read_undo(const Hash256& h, const Block& b, BlockUndo& u) const;
    void write_undo(const Hash256& h, const BlockUndo& u) const;
    void store(const Block& b, const Hash256& h);
    // mempool
    bool mem_coin(const OutPoint& op, Coin& c) const;
    bool mem_precheck(const Transaction& t, size_t size, std::vector<TxOut>& spent, uint64_t& fee, std::string& err) const;
    bool mem_insert(const Hash256& id, const Transaction& t, size_t size, uint64_t fee, std::string& err);
    void mem_remove_tree(const Hash256& id);
    void mem_trim();
    bool mem_full_for(size_t size, uint64_t fee) const;
    void reload_mempool(std::vector<Transaction> returned);
    StakeSet snapshot(uint32_t checkpoint) const;
    bool allowed_by_finality(const BlockIndex* b) const;
    /// A block off the active chain that buffered votes of > 2/3 of our stake name (the winning branch of a split)
    const BlockIndex* finality_candidate(uint32_t* unknown_height = nullptr) const;
    /// Hard-coded checkpoints: the chain through b must match them (and may not fork below the highest one)
    bool allowed_by_checkpoints(const BlockIndex* b) const;
    void update_finality();
    void save_finality() const;

    const ChainParams& p_;
    Hash256 genesis_hash_{};
    std::filesystem::path dir_;
    mutable std::mutex m_;
    std::map<Hash256, std::unique_ptr<BlockIndex>> index_;
    std::vector<BlockIndex*> active_;
    UtxoView utxo_;
    std::map<Hash256, Transaction> mempool_;
    std::map<Hash256, MemInfo> minfo_;                        // fee, size and arrival order of each mempool entry
    std::map<OutPoint, Hash256> mem_spent_;                   // outpoint -> mempool transaction spending it
    size_t mem_bytes_ = 0;
    size_t mem_limit_ = MAX_MEMPOOL_BYTES;
    uint64_t mem_seq_ = 0;
    Hash256 presigned_{};                                     // block whose signatures were already checked (unlocked)
    bool has_presigned_ = false;
    std::map<uint32_t, StakeSet> snapshots_;                 // checkpoints of the active chain
    // Vote pool: height -> block -> validator -> vote. Blocks not on the active chain are buffered (bounded) and
    // counted automatically if their branch becomes active.
    std::map<uint32_t, std::map<Hash256, std::map<Hash256, Vote>>> pool_;
    std::set<Hash256> vote_rejected_;                        // candidates whose branch did not confirm the votes (no flip-flop)
    std::set<Hash256> equivocators_;                         // signed two different blocks at one height (persisted in evidence/)
    std::map<Hash256, Evidence> evidence_pool_;              // evidence waiting for an on-chain slash
    std::vector<Evidence> new_evidence_;
    std::map<Hash256, PublicKey> reg_pool_;                  // registrations waiting for a block
    std::map<Hash256, RegEntry> registry_;                   // on-chain key registry of the active chain
    std::map<Hash256, uint32_t> slashed_;                    // validator -> height of the block with its evidence
    std::map<uint32_t, CertInfo> certs_;                     // carrier block height -> certified checkpoint
    uint32_t final_h_ = 0;
    Hash256 final_hash_{};
};

/// Multi-threaded CPU miner: builds a template, searches nonces on N threads, restarts on a new tip.
class Miner {
public:
    /// `may_mine` (optional) is polled before each template; while it returns false the miner waits
    Miner(Node& n, Address to, int threads, std::function<void(const Block&)> on_found, std::function<bool()> may_mine = {});
    ~Miner();
    double hashrate() const { return hashrate_; }

private:
    void run();
    Node& node_;
    Address to_;
    int threads_;
    std::function<void(const Block&)> on_found_;
    std::function<bool()> may_mine_;
    std::atomic<bool> stop_{false};
    std::atomic<double> hashrate_{0};
    std::thread thread_;
};

}  // namespace plk
