// PIP-0001 block benchmark, dev parameters, throw-away datadir. Not part of the node.
//   bench-pip1 DIR [commitments]
// Compares connecting and reorganising a full block of legacy inputs with a full block of same-key version 2 inputs.
// With "commitments" the phase 3 rules (UTXO commitment in every block) are active as well.
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>

#include "chain.h"
#include "keys.h"
#include "node.h"

using namespace plk;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

static double ms(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

static Block seal(Node& n, Block b, uint32_t step = 5) {
    b.header.merkle = b.merkle_root();
    b.header.time = n.tip()->header.time + step;
    mine(b.header, 1ULL << 32);
    return b;
}

static void must(Node& n, const Block& b, const char* what) {
    std::string e;
    if (!n.accept_block(b, e) || n.tip()->hash != b.header.hash()) {
        std::cerr << what << " failed: " << e << "\n";
        std::exit(1);
    }
}

static uint64_t dir_bytes(const fs::path& d) {
    uint64_t t = 0;
    for (auto& f : fs::directory_iterator(d)) t += f.file_size();
    return t;
}

struct Result { size_t inputs, txs, bytes; double build, connect, reorg, undo; };

static Result run(const ChainParams& p, const fs::path& root, const HybridKey& key, bool grouped, int want_outputs, int fan_txs_per_block) {
    fs::remove_all(root);
    Node n(p, root / "a"), fork(p, root / "b");
    Address me{ADDRESS_VERSION_HYBRID, key.pub().program()};
    const Hash256 genesis = n.genesis_hash();
    uint64_t en = 1;
    auto empty = [&](Node& on) {
        Block b = on.block_template(me, en++);
        return seal(on, b);
    };
    auto both = [&](const Block& b) { must(n, b, "block"); must(fork, b, "block (fork)"); };

    // Coinbases to fan out
    const int per_tx = 10000;
    const int fans = (want_outputs + per_tx - 1) / per_tx;
    std::vector<Block> mined;
    for (int i = 0; i < fans + 4; i++) { mined.push_back(empty(n)); both(mined.back()); }
    // Fan out: 10 000 small outputs per transaction, signed with the rules in force (one input, so the same size)
    std::vector<OutPoint> coins;
    std::vector<TxOut> values;
    for (int f = 0; f < fans; f += fan_txs_per_block) {
        Block b = n.block_template(me, en++);
        for (int i = f; i < std::min(f + fan_txs_per_block, fans); i++) {
            const TxOut prev = mined[size_t(i)].txs[0].vout[0];
            Transaction t;
            t.version = grouped ? 2 : 1;
            t.vin.push_back(TxIn{OutPoint{mined[size_t(i)].txs[0].txid(), 0}, {}});
            uint64_t each = prev.value / (per_tx + 1);
            for (int o = 0; o < per_tx; o++) t.vout.push_back(TxOut{each, me});
            if (grouped) t.sign_v2(genesis, key, {prev}); else t.sign(0, key, {prev});
            Hash256 id = t.txid();
            for (uint32_t o = 0; o < t.vout.size(); o++) { coins.push_back(OutPoint{id, o}); values.push_back(t.vout[o]); }
            b.txs.push_back(std::move(t));
        }
        both(seal(n, b));
    }

    // The block under test: as many inputs as fit into 4 MB
    auto t0 = Clock::now();
    Block big = n.block_template(me, en++);
    size_t used = 300, next = 0;
    while (next < coins.size()) {
        size_t room = MAX_BLOCK_BYTES - used, n_in;
        if (grouped) {
            if (room < 93 + 37 + 3829) break;
            n_in = std::min<size_t>(per_tx, (room - 93 - 3829) / 37);
        } else {
            if (room < 93 + 3866) break;
            n_in = std::min<size_t>(per_tx, (room - 93) / 3866);
        }
        n_in = std::min(n_in, coins.size() - next);
        Transaction t;
        t.version = grouped ? 2 : 1;
        std::vector<TxOut> spent;
        uint64_t sum = 0;
        for (size_t i = 0; i < n_in; i++) {
            t.vin.push_back(TxIn{coins[next + i], {}});
            spent.push_back(values[next + i]);
            sum += values[next + i].value;
        }
        next += n_in;
        t.vout.push_back(TxOut{sum, me});
        if (grouped) t.sign_v2(genesis, key, spent);
        else for (size_t i = 0; i < n_in; i++) t.sign(i, key, spent);
        used += t.serialize().size();
        big.txs.push_back(std::move(t));
    }
    Block sealed = seal(n, big);
    Result r{};
    r.build = ms(t0);
    r.bytes = sealed.serialize().size();
    r.txs = sealed.txs.size() - 1;
    for (size_t i = 1; i < sealed.txs.size(); i++) r.inputs += sealed.txs[i].vin.size();

    const fs::path undo = root / "a" / "undo";
    uint64_t before = dir_bytes(undo);
    t0 = Clock::now();
    must(n, sealed, "big block");
    r.connect = ms(t0);
    r.undo = double(dir_bytes(undo) - before);

    // A heavier branch without the block: disconnect it with undo data, return its transactions to the mempool
    Block f1 = empty(fork);
    must(fork, f1, "fork 1");
    Block f2 = empty(fork);
    must(fork, f2, "fork 2");
    std::string e;
    n.accept_block(f1, e);
    t0 = Clock::now();
    must(n, f2, "reorg");
    r.reorg = ms(t0);
    fs::remove_all(root);
    return r;
}

int main(int argc, char** argv) {
    if (argc < 2) { std::cerr << "usage: bench-pip1 DIR [commitments]\n"; return 2; }
    const bool commitments = argc > 2 && !std::strcmp(argv[2], "commitments");
    ChainParams legacy = ChainParams::dev();
    legacy.upgrade_height = commitments ? 1 : 100000000;
    legacy.halving_interval = 100000000;
    legacy.coinbase_maturity = 1;
    legacy.lwma_window = 45;
    ChainParams grouped = legacy;
    grouped.sigv2_height = 0;

    Bytes seed(64, 7);
    HybridKey key = HybridKey::derive(seed, 0);
    fs::path dir = argv[1];
    std::printf("%-10s %8s %5s %10s %10s %11s %10s %12s\n", "block", "inputs", "txs", "bytes", "build ms", "connect ms", "reorg ms", "undo bytes");
    Result a = run(legacy, dir / "legacy", key, false, 2000, 1);
    std::printf("%-10s %8zu %5zu %10zu %10.0f %11.0f %10.0f %12.0f\n", "legacy", a.inputs, a.txs, a.bytes, a.build, a.connect, a.reorg, a.undo);
    Result b = run(grouped, dir / "grouped", key, true, 110000, 9);
    std::printf("%-10s %8zu %5zu %10zu %10.0f %11.0f %10.0f %12.0f\n", "per-key", b.inputs, b.txs, b.bytes, b.build, b.connect, b.reorg, b.undo);
    return 0;
}
