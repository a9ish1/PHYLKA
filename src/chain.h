// PHYLKA transactions and blocks.
//  - UTXO model like Bitcoin; amounts in "fyl" (1 PLK = 100 000 000 fyl).
//  - Every input carries a hybrid witness: both public keys + Schnorr and ML-DSA signatures.
//  - Block header is byte-compatible with Bitcoin's 80-byte header and proof of work is double SHA-256,
//    so ordinary SHA-256 miners (NerdMiner, Bitaxe, ASICs) can mine PLK through a stratum pool.
//  - The merkle root commits to witness hashes, so a block also fixes every signature it contains.
#pragma once
#include <optional>
#include <string>
#include <vector>

#include "address.h"
#include "ext.h"
#include "keys.h"

namespace plk {

constexpr uint64_t COIN = 100'000'000;   // 1 PLK = 100 000 000 fyl

struct Block;

struct ChainParams {
    std::string name;
    std::string hrp;
    uint32_t genesis_bits;
    uint32_t pow_limit_bits;      // easiest allowed target
    uint64_t initial_reward;
    uint32_t halving_interval;    // blocks
    uint32_t target_spacing;      // seconds
    uint32_t lwma_window;         // blocks used by the difficulty adjustment
    uint32_t coinbase_maturity;   // blocks before mined coins can be spent
    std::array<uint8_t, 4> magic; // first bytes of every P2P message
    uint16_t p2p_port, rpc_port;
    std::string genesis_message;
    uint32_t genesis_time;
    uint32_t genesis_nonce;
    // Validator finality (the second half of the hybrid consensus)
    uint32_t checkpoint_interval;   // every N-th block is a checkpoint validators vote on
    uint32_t finality_depth;        // validators vote once the checkpoint is this many blocks deep
    uint32_t stake_lock;            // stake can be withdrawn only this many blocks after it was locked
    uint32_t stake_activation;      // stake counts for checkpoints this many blocks after it was locked
    uint64_t min_stake;             // per validator
    uint64_t min_total_stake;       // below this the chain is pure proof of work (no finality yet)
    // Phase 3 consensus upgrade: blocks at height >= upgrade_height must carry the extension and commitments
    uint32_t upgrade_height;
    uint32_t voter_fee_pct;         // share of block fees paid to the validators of the block's certificate
    std::vector<std::string> seeds; // built-in seed nodes "host:port" (main: empty until real ones exist)
    // Hard-coded checkpoints (height, block hash in display order): a block at such a height with another hash is invalid
    // and no chain that diverges below the highest checkpoint is ever activated.
    std::vector<std::pair<uint32_t, std::string>> checkpoints;
    // PIP-0001 (one hybrid signature per key): disabled until an activation height is chosen for a release
    uint32_t sigv2_height = UINT32_MAX;
    bool upgraded(uint32_t height) const { return height >= upgrade_height; }
    bool sigv2(uint32_t height) const { return height >= sigv2_height; }

    static const ChainParams& main();
    static const ChainParams& dev();
    /// Genesis pays an unspendable output (no one holds its key) and, like Bitcoin's, never enters the UTXO set
    Block genesis() const;
};

constexpr uint64_t MAX_MONEY = 21'000'000 * 100'000'000ULL;

struct OutPoint {
    Hash256 txid{};
    uint32_t vout = 0;
    bool null() const;
};

struct TxIn {
    OutPoint prev;
    Bytes coinbase_data;   // only in the coinbase input: block height + free text
};

struct TxOut {
    uint64_t value = 0;
    Address to;
};

struct Witness {
    PublicKey key;
    HybridSignature sig;
};

struct Transaction {
    uint32_t version = 1;
    std::vector<TxIn> vin;
    std::vector<TxOut> vout;
    uint32_t locktime = 0;
    std::vector<Witness> witness;   // one per input (version 2 from sigv2_height: one per key); empty for the coinbase

    Bytes serialize(bool with_witness = true) const;
    static std::optional<Transaction> parse(const Bytes& b, size_t& pos);
    /// Identity of the transaction (without signatures), used to reference its outputs
    Hash256 txid() const;
    /// Hash including signatures, committed in the block's merkle root
    Hash256 wtxid() const;
    bool is_coinbase() const;
    /// What input `index` signs: the transaction plus the outputs it spends (amounts cannot be misrepresented)
    Hash256 sighash(size_t index, const std::vector<TxOut>& spent) const;
    void sign(size_t index, const HybridKey& key, const std::vector<TxOut>& spent);
    /// Checks keys match the spent address and both signatures are valid
    bool verify_input(size_t index, const std::vector<TxOut>& spent, std::string* why = nullptr) const;

    // ---- PIP-0001: version 2 transactions from sigv2_height
    /// Distinct key programs of the spent outputs, in order of first appearance. The witness list follows this order.
    static std::vector<Hash256> key_groups(const std::vector<TxOut>& spent);
    /// What the key with this program signs: the network, the transaction, every spent output and the program itself
    Hash256 sighash_v2(const Hash256& genesis, const Hash256& program, const std::vector<TxOut>& spent) const;
    /// Signs for this key's group. The transaction must already have version 2, all inputs and all outputs.
    void sign_v2(const Hash256& genesis, const HybridKey& key, const std::vector<TxOut>& spent);
    /// Checks one witness per key group: keys match the group and both signatures are valid
    bool verify_v2(const Hash256& genesis, const std::vector<TxOut>& spent, std::string* why = nullptr) const;
};

struct BlockHeader {
    int32_t version = 1;
    Hash256 prev{};
    Hash256 merkle{};
    uint32_t time = 0;
    uint32_t bits = 0;
    uint32_t nonce = 0;

    Bytes serialize() const;   // exactly 80 bytes
    Hash256 hash() const;
};

struct Block {
    BlockHeader header;
    std::vector<Transaction> txs;
    bool has_ext = false;   // extension after the transactions (required from upgrade_height, forbidden before)
    BlockExt ext;
    Hash256 merkle_root() const;
    Bytes serialize() const;
};

/// Hash in the usual display order (byte-reversed, like Bitcoin)
std::string hash_hex(const Hash256& h);
bool check_pow(const Hash256& hash, uint32_t bits);
/// Tries nonces (and bumps time when they run out) until the header meets its target
bool mine(BlockHeader& h, uint64_t max_tries, uint64_t* tries = nullptr);
uint64_t block_reward(const ChainParams& p, uint32_t height);
/// `commit` (COMMIT_BYTES, upgraded blocks only) goes between the height and the free text
Transaction make_coinbase(const ChainParams& p, uint32_t height, const Address& to, const std::string& text, uint64_t fees = 0, const Bytes& commit = {});
/// Bytes of the coinbase data before the free text of a mined block (height, commitments, 8-byte extranonce)
size_t coinbase_prefix_len(const ChainParams& p, uint32_t height);
Block make_genesis(const ChainParams& p, const Address& to, const std::string& message, uint32_t time);
/// Coinbase height encoded in the first 4 bytes of its data
uint32_t coinbase_height(const Transaction& coinbase);

}  // namespace plk
