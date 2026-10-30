// PHYLKA block extension (active from ChainParams::upgrade_height). Lives after the transaction list, outside the
// 80-byte header; the coinbase commits to its hash, so the block hash (via the merkle root) fixes it.
//   ext   := varint n_reg | n_reg x { 32 schnorr_xonly | 1312 mldsa_pk }          key registrations
//            varint n_ev  | n_ev x evidence                                       equivocation evidence
//            u8 has_cert  | [cert]                                                finality certificate
//   cert  := u32 checkpoint height | 32 checkpoint hash | varint n | n x { 32 validator id | 64 schnorr | 2420 mldsa }
//   evid. := u32 height | 32 validator id | 32 block A | sigs A (64 + 2420) | 32 block B | sigs B (64 + 2420)
#pragma once
#include <optional>
#include <vector>

#include "keys.h"

namespace plk {

constexpr size_t MAX_EXT_REGS = 8, MAX_EXT_EVIDENCE = 4, MAX_CERT_VOTES = 2000;
constexpr size_t COMMIT_BYTES = 96;   // coinbase data after the height: utxo root | checkpoint commitment | ext hash

/// What a validator signs: the checkpoint height and block hash
Hash256 vote_message(uint32_t height, const Hash256& block);
/// Commitment to a finalized checkpoint (height, hash)
Hash256 checkpoint_commitment(uint32_t height, const Hash256& block);

struct Evidence {
    uint32_t height = 0;
    Hash256 vid{};
    Hash256 block_a{}, block_b{};
    HybridSignature sig_a, sig_b;
    Bytes serialize() const;
    static std::optional<Evidence> parse(const Bytes& b, size_t& pos);
};

struct CertVote {
    Hash256 vid{};
    HybridSignature sig;
};

struct Cert {
    uint32_t height = 0;
    Hash256 block{};
    std::vector<CertVote> votes;   // strictly ascending by validator id
};

struct BlockExt {
    std::vector<PublicKey> regs;
    std::vector<Evidence> evidence;
    std::optional<Cert> cert;
    Bytes serialize() const;
    static std::optional<BlockExt> parse(const Bytes& b);
    Hash256 hash() const;   // tagged hash committed in the coinbase
};

}  // namespace plk
