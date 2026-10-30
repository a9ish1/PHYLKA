// PHYLKA wallet. The seed phrase is the root; the wallet file only keeps it encrypted with your password:
//   PBKDF2-HMAC-SHA512 (210 000 rounds) -> encryption key + MAC key, SHAKE256 keystream, HMAC-SHA512 tag.
// A wrong password or any change to the file is detected before anything is decrypted.
#pragma once
#include <filesystem>
#include <string>

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

/// Asks for the wallet password and returns the key of address #0, used to sign validator votes
HybridKey validator_key(const ChainParams& p, const std::filesystem::path& file);

int wallet_command(const ChainParams& p, const std::filesystem::path& file, uint16_t rpc_port, const std::vector<std::string>& args);

}  // namespace plk
