// PHYLKA wallet. The seed phrase is the root; the wallet file only keeps it encrypted with your password:
//   PBKDF2-HMAC-SHA512 (210 000 rounds) -> encryption key + MAC key, SHAKE256 keystream, HMAC-SHA512 tag.
// A wrong password or any change to the file is detected before anything is decrypted.
#pragma once
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "chain.h"

namespace plk {

struct WalletSecret {
    std::string mnemonic;
    std::string passphrase;   // optional BIP39 passphrase
    ~WalletSecret();
};

namespace vault {
Bytes seal(const WalletSecret& s, const std::string& password);
/// Throws on wrong password or a damaged file
WalletSecret open(const Bytes& file, const std::string& password);
}  // namespace vault


// ---- Wallet logic shared by the command-line wallet (wallet_command) and the GUI (gui.cpp).
// Signing happens in the calling process; the node only ever receives finished transactions over RPC.

constexpr size_t MIN_PASSWORD_LEN = 10;
constexpr uint64_t WALLET_FEE_RATE = 1;   // fyl per byte-unit used by the fee formula (relay minimum)

/// "1.5 PLK" (trailing zeros trimmed)
std::string format_plk(int64_t fyl);
/// Decimal text with at most 8 places -> fyl; throws on anything else, zero or above MAX_MONEY
uint64_t parse_amount(const std::string& s);

/// Wallet file on disk plus the number of receive addresses handed out (kept next to it, no secrets)
struct WalletFile {
    std::filesystem::path path;
    Bytes sealed;
    uint32_t addresses = 1;

    std::filesystem::path idx() const;
    bool exists() const;
    void load();
    /// Created with mode 0600 from the start (never briefly readable by others)
    void write_private(const Bytes& data) const;
    void save() const;
};

Bytes wallet_seed(const WalletSecret& s);
/// Addresses are cached next to the wallet (public data), so they can be listed without the password
std::vector<std::string> wallet_addresses(const WalletFile& w);
void wallet_cache_addresses(const WalletFile& w, const ChainParams& p, const Bytes& seed);
/// Seals a new wallet file and writes it with its address cache. Throws if the password is shorter than MIN_PASSWORD_LEN.
void wallet_create(WalletFile& w, const ChainParams& p, const WalletSecret& s, const std::string& password);
/// Hands out the next receive address (or returns the existing one if the cache is ahead); `s` must already be decrypted
std::string wallet_new_address(WalletFile& w, const ChainParams& p, const WalletSecret& s);

struct WalletUtxo {
    OutPoint op;
    uint64_t value;
    uint32_t conf;
    bool coinbase, mature;
    uint32_t index;                         // which wallet address
    int version = ADDRESS_VERSION_HYBRID;   // 2 = locked validator stake
};
std::vector<WalletUtxo> wallet_fetch_utxos(uint16_t rpc, const std::vector<std::string>& addrs);
std::vector<WalletUtxo> wallet_spendable(const std::vector<WalletUtxo>& all);
std::vector<WalletUtxo> wallet_unlockable_stakes(const ChainParams& p, const std::vector<WalletUtxo>& all);

struct WalletBalance {
    uint64_t spendable = 0, immature = 0, staked = 0, unlockable = 0;
    std::map<uint32_t, uint64_t> per_address;   // spendable + immature per address index
};
WalletBalance wallet_balance(const ChainParams& p, const std::vector<WalletUtxo>& utxos);

struct PaymentPlan {
    std::vector<WalletUtxo> use;
    uint64_t amount = 0, fee = 0, in_sum = 0;
    bool sigv2 = false;   // sign as a version 2 transaction: one hybrid signature per key
};
/// Is the next block under the PIP-0001 rules, according to the node? Old nodes do not report it: no.
bool wallet_sigv2_active(uint16_t rpc);
/// Picks coins (largest first) and computes the fee: (100 + 3866 * inputs) * rate, or with sigv2 (100 + 37 * inputs + 3829 * keys) * rate.
/// Throws when funds are insufficient.
PaymentPlan wallet_plan_payment(std::vector<WalletUtxo> coins, uint64_t amount, bool send_all, uint64_t rate, bool sigv2 = false);
/// Builds and signs the transaction (change goes back to address #0)
Transaction wallet_sign_payment(const ChainParams& p, const WalletSecret& s, const std::vector<std::string>& addrs, const PaymentPlan& plan,
                                const Address& to);
/// Submits to the node; returns the txid or throws "node rejected: ..."
std::string wallet_submit(uint16_t rpc, const Transaction& t);

struct HistoryRow {
    std::string height;   // block height or "pending"
    long long time = 0;
    std::string txid;
    long long delta = 0;
    std::string kind;     // "mined" or "tx"
};
std::vector<HistoryRow> wallet_history(uint16_t rpc, const std::string& addr);

/// Asks for the wallet password and returns the key of address #0, used to sign validator votes
HybridKey validator_key(const ChainParams& p, const std::filesystem::path& file);

int wallet_command(const ChainParams& p, const std::filesystem::path& file, uint16_t rpc_port, const std::vector<std::string>& args);

}  // namespace plk
