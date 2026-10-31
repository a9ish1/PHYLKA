// phylka - command-line tool for the PHYLKA (PLK) network core.
//   phylka selftest                 check every primitive against published test vectors (no mining)
//   phylka newseed                  new 24-word seed phrase and its first address
//   phylka address [index] [--dev]  address of a seed phrase (read without echo)
//   phylka gui [--dev]              local GUI wallet (web page on 127.0.0.1)

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>

#include <unistd.h>

extern "C" {
#include "sha2.h"
}
#include "arith.h"
#include "gui.h"
#include "net.h"
#include "platform.h"
#include "wallet.h"

using namespace plk;

static int failures = 0;
static void check(bool ok, const std::string& what) {
    std::cout << (ok ? "  ok   " : "  FAIL ") << what << "\n";
    if (!ok) failures++;
}

static Bytes str_bytes(const std::string& s) { return Bytes(s.begin(), s.end()); }

static Hash256 from_display_hex(const std::string& hex) {
    Bytes b;
    from_hex(hex, b);
    Hash256 h{};
    std::reverse_copy(b.begin(), b.end(), h.begin());
    return h;
}

static double ms_since(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}


// ---- node-level selftest helpers (dev network, trivial difficulty, throw-away datadirs)

static Block mine_on(Node& n, const Address& to, uint64_t extranonce) {
    Block b = n.block_template(to, extranonce);
    b.header.time = n.tip()->header.time + 5;   // keep the dev difficulty at its limit
    mine(b.header, 1ULL << 32);
    return b;
}

static bool add_block(Node& n, const Block& b, std::string* why = nullptr) {
    std::string e;
    bool ok = n.accept_block(b, e);
    if (why) *why = e;
    return ok;
}

static bool same_utxo(const UtxoView& a, const UtxoView& b) {
    if (a.coins.size() != b.coins.size()) return false;
    for (auto& [op, c] : a.coins) {
        auto it = b.coins.find(op);
        if (it == b.coins.end() || it->second.out.value != c.out.value || it->second.out.to.program != c.out.to.program ||
            it->second.height != c.height || it->second.coinbase != c.coinbase)
            return false;
    }
    return true;
}

static Transaction spend_coin(const HybridKey& key, const OutPoint& op, const TxOut& prev, const Address& to, uint64_t amount, uint64_t fee, const Address& change) {
    Transaction t;
    t.vin.push_back(TxIn{op, {}});
    t.vout.push_back(TxOut{amount, to});
    t.vout.push_back(TxOut{prev.value - amount - fee, change});
    t.sign(0, key, {prev});
    return t;
}

static void selftest_u256() {
    std::cout << "exact chain work (U256)\n";
    check(U256::divide(U256::from_u64(100), U256::from_u64(7)).to_ld() == 14, "256-bit division 100 / 7 = 14");
    U256 big = U256::from_compact(0x1d00ffff);
    check(U256::divide(big, big) == U256::from_u64(1) && U256::divide(U256::from_u64(5), big).is_zero(), "x / x = 1, small / big = 0");
    check(U256::from_compact(0x1d00ffff).work() == U256::from_u64(0x100010001ULL), "work(difficulty 1) = 0x100010001, as in Bitcoin");
    check(U256::from_compact(0x207fffff).work() == U256::from_u64(2), "work(dev limit) = 2");
    U256 w1 = U256::from_compact(0x1d00ffff).work(), w2 = U256::from_compact(0x1c00ffff).work();
    check(w2 > w1 && !(w1 > w2) && w1 != w2 && w1 < w2, "harder target -> more work (exact comparison)");
    U256 s = w1 + w2;
    check(s > w2 && s.to_ld() > 0, "chain work adds exactly");
    U256 d = s;
    d.sub(w2);
    check(d == w1, "sub undoes add");
    // two sums that long double (64-bit mantissa) cannot tell apart, U256 can
    U256 huge = U256::from_compact(0x03000001).work();   // ~2^233 hashes
    U256 huge1 = huge + U256::from_u64(1);
    check(huge1 > huge && huge1.to_ld() == huge.to_ld(), "difference of 1 hash at 2^233 work is still visible");
}

static void selftest_node(const HybridKey& k0, const HybridKey& k1) {
    namespace fs = std::filesystem;
    const ChainParams& dev = ChainParams::dev();
    Address a0{ADDRESS_VERSION_HYBRID, k0.pub().program()}, a1{ADDRESS_VERSION_HYBRID, k1.pub().program()};
    fs::path root = fs::temp_directory_path() / ("phylka-selftest-" + std::to_string(::getpid()));
    fs::remove_all(root);

    std::cout << "UTXO overlay\n";
    {
        UtxoView base;
        OutPoint o1{Hash256{}, 1}, o2{Hash256{}, 2};
        base.coins[o1] = Coin{TxOut{5, a0}, 1, false};
        UtxoOverlay ov(base);
        Coin got;
        check(ov.spend(o1, &got) && got.out.value == 5 && !ov.find(o1) && !ov.spend(o1), "overlay spend hides the coin and cannot be repeated");
        ov.add(o2, Coin{TxOut{7, a1}, 2, false});
        check(ov.find(o2) && base.coins.size() == 1 && base.coins.count(o1), "overlay changes do not touch the base");
        ov.apply_to(base);
        check(base.coins.size() == 1 && base.coins.count(o2) && !base.coins.count(o1), "apply_to commits spends and new coins");
    }

    std::cout << "node: failing block never mutates the UTXO set, mempool fee policy\n";
    {
        Node x(dev, root / "x");
        std::vector<Block> chain;
        for (int i = 0; i < 4; i++) {   // heights 1..4, coinbases 1 and 2 are mature
            chain.push_back(mine_on(x, a0, i));
            add_block(x, chain.back());
        }
        check(x.height() == 4, "mined 4 dev blocks");
        const Transaction& cb1 = chain[0].txs[0];
        OutPoint op1{cb1.txid(), 0};
        TxOut prev = cb1.vout[0];

        Transaction nofee = spend_coin(k0, op1, prev, a1, 10 * COIN, 0, a0);
        std::string err;
        check(!x.accept_tx(nofee, err) && err_has(err, errmsg::FEE_LOW), "zero-fee transaction refused (" + err + ")");
        size_t size = nofee.serialize().size();
        Transaction low = spend_coin(k0, op1, prev, a1, 10 * COIN, size - 1, a0);
        check(!x.accept_tx(low, err) && err_has(err, errmsg::FEE_LOW), "fee just below 1 fyl/byte refused");
        Transaction ok = spend_coin(k0, op1, prev, a1, 10 * COIN, size, a0);
        check(x.accept_tx(ok, err) && x.mempool().count(ok.txid()), "fee of exactly 1 fyl/byte accepted");
        check(x.accept_tx(spend_coin(k0, op1, prev, a1, 11 * COIN, size + 50, a0), err) == false, "double spend of a pooled input refused (" + err + ")");

        // A block with a valid spend but a greedy coinbase fails during connect: the real UTXO set must stay intact
        UtxoView before = x.utxo();
        Block bad = x.block_template(a0, 77);
        bad.txs[0] = make_coinbase(dev, 5, a0, "greedy", 1'000'000 * COIN);
        bad.header.merkle = bad.merkle_root();
        bad.header.time = x.tip()->header.time + 5;
        mine(bad.header, 1ULL << 32);
        std::string why;
        check(!add_block(x, bad, &why) && x.height() == 4 && same_utxo(before, x.utxo()), "block with greedy coinbase rejected, UTXO set untouched (" + why + ")");
        // And one whose second transaction has a broken signature (found by the unlocked signature check)
        Transaction forged = spend_coin(k0, OutPoint{chain[1].txs[0].txid(), 0}, chain[1].txs[0].vout[0], a1, 10 * COIN, size, a0);
        forged.witness[0].sig.mldsa[10] ^= 1;
        Block bad2 = x.block_template(a0, 78);
        bad2.txs.push_back(forged);
        bad2.header.merkle = bad2.merkle_root();
        bad2.header.time = x.tip()->header.time + 5;
        mine(bad2.header, 1ULL << 32);
        check(!add_block(x, bad2, &why) && x.height() == 4 && same_utxo(before, x.utxo()), "block with a forged signature rejected, UTXO set untouched");

        std::cout << "node: mempool size cap evicts the lowest fee rate\n";
        Node m(dev, root / "m");
        std::vector<Block> mc;
        for (int i = 0; i < 6; i++) { mc.push_back(mine_on(m, a0, 100 + i)); add_block(m, mc.back()); }
        auto cbspend = [&](int i, uint64_t fee) { return spend_coin(k0, OutPoint{mc[i].txs[0].txid(), 0}, mc[i].txs[0].vout[0], a1, COIN, fee, a0); };
        size_t sz = cbspend(0, 0).serialize().size();
        m.set_mempool_limit(sz * 2 + sz / 2);   // room for two
        Transaction t_mid = cbspend(0, sz * 5), t_high = cbspend(1, sz * 9), t_low = cbspend(2, sz * 2), t_top = cbspend(3, sz * 20);
        check(m.accept_tx(t_mid, err) && m.accept_tx(t_high, err), "two transactions fit under the cap");
        check(!m.accept_tx(t_low, err) && err_has(err, errmsg::MEMPOOL_FULL) && m.mempool().size() == 2, "a lower fee rate than the pool is refused (" + err + ")");
        check(m.accept_tx(t_top, err) && m.mempool().size() == 2 && !m.mempool().count(t_mid.txid()) && m.mempool().count(t_high.txid()) && m.mempool().count(t_top.txid()),
              "a better payer evicts the lowest fee rate");
    }

    std::cout << "node: reorganisation with undo data, restart\n";
    {
        Node xa(dev, root / "xa"), xb(dev, root / "xb");
        for (int i = 0; i < 2; i++) {   // shared blocks 1, 2
            Block b = mine_on(xa, a0, i);
            add_block(xa, b);
            add_block(xb, b);
        }
        Block b1 = *xa.read_block(xa.at(1)->hash);
        OutPoint op{b1.txs[0].txid(), 0};
        Transaction pay = spend_coin(k0, op, b1.txs[0].vout[0], a1, 20 * COIN, 5000, a0);
        std::string err;
        check(xa.accept_tx(pay, err), "A: transaction in mempool");
        Block a3 = mine_on(xa, a0, 3);
        check(add_block(xa, a3) && a3.txs.size() == 2 && xa.mempool().empty(), "A: block 3 confirms it");
        check(xa.utxo().coins.count(OutPoint{pay.txid(), 0}), "A: payment output exists");
        // B builds a longer chain on blocks 1, 2 without that transaction
        std::vector<Block> bb;
        for (int i = 0; i < 2; i++) { bb.push_back(mine_on(xb, a1, 50 + i)); add_block(xb, bb.back()); }
        check(add_block(xa, bb[0]) && xa.tip()->hash == a3.header.hash(), "equal work: no switch (exact chainwork comparison)");
        check(xa.tip()->chainwork < xb.tip()->chainwork, "B has strictly more chain work");
        check(add_block(xa, bb[1]) && xa.tip()->hash == xb.tip()->hash && xa.height() == 4, "A reorganises to the heavier chain");
        check(same_utxo(xa.utxo(), xb.utxo()), "UTXO set after undo-based reorg equals a clean B node");
        check(!xa.utxo().coins.count(OutPoint{pay.txid(), 0}) && xa.mempool().count(pay.txid()), "disconnected transaction returned to the mempool");
        check(fs::exists(root / "xa" / "undo" / (hash_hex(a3.header.hash()) + ".undo")), "undo file written for the disconnected block");
        Block a4 = mine_on(xa, a0, 90);   // confirms the returned transaction on top of B
        add_block(xa, a4);
        check(a4.txs.size() == 2 && xa.utxo().coins.count(OutPoint{pay.txid(), 0}), "returned transaction mined again");
        Hash256 tip = xa.tip()->hash;
        uint32_t height = xa.height();
        UtxoView snap = xa.utxo();
        { Node reload(dev, root / "xa"); check(reload.tip()->hash == tip && reload.height() == height && same_utxo(reload.utxo(), snap), "restart: same tip and same UTXO set"); }
        fs::remove_all(root / "xa" / "undo");   // lose all undo data: the node must still reload
        { Node reload(dev, root / "xa");
          check(reload.tip()->hash == tip && same_utxo(reload.utxo(), snap), "restart without undo files: same tip and UTXO set");
          check(fs::exists(root / "xa" / "undo" / (hash_hex(tip) + ".undo")), "undo files rebuilt from the blocks"); }
    }
    fs::remove_all(root);
}

// ---- phase 3: height-activated consensus rules

static Block remine(Node& n, Block b) {   // after the coinbase or extension was changed by a test
    b.header.merkle = b.merkle_root();
    b.header.time = n.tip()->header.time + 5;
    b.header.nonce = 0;
    mine(b.header, 1ULL << 32);
    return b;
}

static void set_commit(Block& b, size_t off, const Hash256& h) {   // off: 4 utxo root, 36 checkpoint commitment, 68 ext hash
    auto& d = b.txs[0].vin[0].coinbase_data;
    std::copy(h.begin(), h.end(), d.begin() + off);
}

static bool rejects(Node& n, const Block& b, const std::string& needle, const std::string& what) {
    std::string why;
    bool ok = !add_block(n, b, &why) && why.find(needle) != std::string::npos;
    check(ok, what + " (" + why + ")");
    return ok;
}

static void selftest_phase3(const HybridKey& k0, const HybridKey& k1) {
    namespace fs = std::filesystem;
    ChainParams t = ChainParams::dev();   // small numbers so the test crosses every threshold quickly
    t.checkpoint_interval = 3; t.finality_depth = 1; t.stake_lock = 3; t.stake_activation = 2;
    t.min_stake = 10 * COIN; t.min_total_stake = 10 * COIN; t.upgrade_height = 14; t.voter_fee_pct = 10;
    Address a0{ADDRESS_VERSION_HYBRID, k0.pub().program()}, a1{ADDRESS_VERSION_HYBRID, k1.pub().program()};
    Address stake_addr{ADDRESS_VERSION_STAKE, k0.pub().program()};
    Hash256 vid = k0.pub().program();
    fs::path root = fs::temp_directory_path() / ("phylka-selftest3-" + std::to_string(::getpid()));
    fs::remove_all(root);
    std::string err, why;

    std::cout << "phase 3: fee share arithmetic\n";
    {
        check(mul_div(21'000'000ULL * COIN / 10, 21'000'000ULL * COIN, 21'000'000ULL * COIN) == 21'000'000ULL * COIN / 10, "mul_div: 128-bit intermediate, no overflow");
        check(mul_div(7, 3, 2) == 10 && mul_div(0, 5, 3) == 0, "mul_div rounds down");
        Cert c;
        Hash256 va{}, vb{}, vc{};
        va[0] = 1; vb[0] = 2; vc[0] = 3;
        c.votes = {CertVote{va, {}}, CertVote{vb, {}}, CertVote{vc, {}}};
        StakeSet s{{va, 10 * COIN}, {vb, 30 * COIN}, {vc, 10 * COIN}};
        auto sh = voter_shares(t, 1'000'003, c, s);   // pool = 100 000
        uint64_t sum = 0;
        for (auto& o : sh) sum += o.value;
        check(sh.size() == 3 && sh[0].value == 20000 && sh[1].value == 60000 && sh[2].value == 20000 && sum <= 100'000, "10% of fees split pro rata to stake, rounded down");
        check(voter_shares(t, 5, c, s).empty(), "dust: shares that round to zero are omitted");
        uint64_t max_fees = MAX_MONEY;
        auto big = voter_shares(t, max_fees, c, s);
        sum = 0;
        for (auto& o : big) sum += o.value;
        check(sum <= max_fees / 10, "shares never exceed the pool even for maximal fees (no minting)");
    }

    std::cout << "phase 3: vote buffering for another branch\n";
    Node A(t, root / "a"), B(t, root / "b");
    std::vector<Block> pre;
    for (int i = 1; i <= 4; i++) { pre.push_back(mine_on(A, a0, i)); add_block(A, pre.back()); add_block(B, pre.back()); }
    const Transaction& cb1 = pre[0].txs[0];
    OutPoint stake_op;
    Transaction stake_tx = spend_coin(k0, OutPoint{cb1.txid(), 0}, cb1.vout[0], stake_addr, 40 * COIN, 20000, a0);
    check(A.accept_tx(stake_tx, err), "stake transaction accepted");
    stake_op = OutPoint{stake_tx.txid(), 0};
    Block b5 = mine_on(A, a0, 5);
    add_block(A, b5);
    add_block(B, b5);
    for (int i = 6; i <= 10; i++) add_block(A, mine_on(A, a0, 100 + i));
    std::vector<Block> fork;
    for (int i = 6; i <= 11; i++) { fork.push_back(mine_on(B, a1, 200 + i)); add_block(B, fork.back()); }
    {
        Vote v{9, fork[3].header.hash(), k0.pub(), {}};
        v.sig = k0.sign(v.message());
        check(A.accept_vote(v, err) == Node::VoteResult::Accepted && A.vote_pool_size() == 1 && A.finalized_height() == 0,
              "vote for a checkpoint on a branch we do not have is buffered, not dropped");
        Vote stranger{9, fork[3].header.hash(), k1.pub(), {}};
        stranger.sig = k1.sign(stranger.message());
        check(A.accept_vote(stranger, err) == Node::VoteResult::Rejected && A.vote_pool_size() == 1, "buffering is limited to known validators (" + err + ")");
        check(A.voted_any(9, k0.pub().program()) && !A.voted(9, k0.pub().program()),
              "a validator that signed on another branch counts as having voted (a second signature at that height would be slashable)");
        check(A.finality_hint() == 9, "votes with > 2/3 of the stake name a block we lack: height 9 is requested from peers");
        for (int i = 0; i < 3; i++) add_block(A, fork[i]);
        check(A.height() == 10 && A.finalized_height() == 0, "the voted block is not known yet: equal work, no switch");
        add_block(A, fork[3]);
        check(A.height() == 9 && A.tip()->hash == fork[3].header.hash() && A.finalized_height() == 9 && A.finalized_hash() == fork[3].header.hash(),
              "finality beats work: the voted block arrives, its branch is activated although our chain is longer, checkpoint 9 is final");
        add_block(A, fork[4]);
        check(A.height() == 10 && A.tip()->hash == fork[4].header.hash() && A.finalized_height() == 9, "the finalized branch is followed");
        add_block(A, fork[5]);
        check(A.height() == 11 && A.tip()->hash == fork[5].header.hash() && A.finalized_height() == 9 && A.finalized_hash() == fork[3].header.hash(),
              "branch extended: checkpoint 9 stays final");
        check(!A.accept_block(fork[3], err) || err == errmsg::BLOCK_KNOWN, "known block again is harmless");
    }

    std::cout << "phase 3: activation boundary (height 14)\n";
    for (int i = 12; i <= 12; i++) add_block(A, mine_on(A, a0, 300 + i));
    {
        Block t13 = A.block_template(a0, 313);
        check(!t13.has_ext && t13.txs[0].vin[0].coinbase_data.size() <= 100, "block 13 (below activation) is built without extension or commitments");
        Block with_ext = t13;
        with_ext.has_ext = true;
        rejects(A, remine(A, with_ext), "before the activation", "pre-activation block carrying an extension is invalid");
        check(add_block(A, mine_on(A, a0, 313)), "pre-activation block without new fields stays valid (as before the upgrade)");
        check(A.height() == 13, "height 13");
        PublicKey stranger_key = k1.pub();
        check(A.accept_registration(stranger_key, err) == Node::VoteResult::Rejected, "registration needs a stake (" + err + ")");
        check(A.accept_registration(k0.pub(), err) == Node::VoteResult::Accepted, "validator key queued for registration");
        Block t14 = A.block_template(a0, 314);
        check(t14.has_ext && t14.ext.regs.size() == 1 && t14.txs[0].vin[0].coinbase_data.size() == 4 + COMMIT_BYTES + 16, "block 14 template carries extension + commitments");
        Block old_style = t14;
        old_style.has_ext = false;
        old_style.txs[0] = make_coinbase(t, 14, a0, "old style");
        rejects(A, remine(A, old_style), "after activation", "post-activation block without the new fields is invalid");
        Block bad_utxo = t14;
        set_commit(bad_utxo, 4, Hash256{});
        rejects(A, remine(A, bad_utxo), "UTXO commitment", "UTXO commitment mismatch rejected");
        Block bad_ck = t14;
        set_commit(bad_ck, 36, Hash256{});
        rejects(A, remine(A, bad_ck), "checkpoint commitment", "checkpoint commitment mismatch rejected");
        Block bad_ext = t14;
        set_commit(bad_ext, 68, Hash256{});
        rejects(A, remine(A, bad_ext), "extension hash", "extension hash mismatch rejected");
        Block dup_reg = t14;
        dup_reg.ext.regs.push_back(dup_reg.ext.regs[0]);
        set_commit(dup_reg, 68, dup_reg.ext.hash());
        rejects(A, remine(A, dup_reg), "duplicate key registration", "duplicate registration rejected");
        Hash256 root_before = A.utxo_commit();
        check(add_block(A, remine(A, t14), &why), "valid post-activation block accepted (" + why + ")");
        check(A.height() == 14 && A.registered(vid) && !A.registered(k1.pub().program()) && A.registry_size() == 1, "key registry lookup: validator registered on-chain");
        check(root_before != A.utxo_commit(), "UTXO commitment changes with the set");
        Bytes raw = A.read_block(A.tip()->hash)->serialize();
        auto parsed = block_from_bytes(raw);
        check(parsed && parsed->has_ext && parsed->serialize() == raw, "block with extension round-trips through serialization");
    }

    std::cout << "phase 3: compact votes, certificate, voter fee share\n";
    add_block(A, mine_on(A, a0, 315));
    add_block(A, mine_on(A, a0, 316));   // tip 16: checkpoint 15 is one block deep
    {
        Vote full{15, A.at(15)->hash, k0.pub(), {}};
        full.sig = k0.sign(full.message());
        check(A.accept_vote(full, err) == Node::VoteResult::Rejected && err_has(err, "after activation"), "full-form vote after activation refused (" + err + ")");
        Vote unreg{15, A.at(15)->hash, k1.pub(), {}};
        unreg.compact = true;
        unreg.vid = k1.pub().program();
        unreg.sig = k1.sign(unreg.message());
        check(A.accept_vote(unreg, err) == Node::VoteResult::Rejected && err_has(err, errmsg::VOTE_UNREGISTERED), "compact vote of an unregistered key refused (" + err + ")");
        Vote cv = full;
        cv.compact = true;
        cv.vid = vid;
        cv.key = PublicKey{};
        check(A.accept_vote(cv, err) == Node::VoteResult::Accepted && A.finalized_height() == 15, "compact vote resolved through the registry; checkpoint 15 final");
        check(cv.serialize().size() == VOTE_COMPACT_BYTES && full.serialize().size() == VOTE_FULL_BYTES, "compact vote is 1312 bytes smaller");
        // a transaction with a fee, so there is something to share
        const Transaction& cb2 = pre[1].txs[0];
        Transaction pay = spend_coin(k0, OutPoint{cb2.txid(), 0}, cb2.vout[0], a1, 10 * COIN, 100'000, a0);
        check(A.accept_tx(pay, err), "fee-paying transaction in the mempool");
        Block t17 = A.block_template(a0, 317);
        check(t17.ext.cert && t17.ext.cert->height == 15 && t17.txs[0].vout.size() == 2 && t17.txs[0].vout[1].value == 10'000 &&
                  t17.txs[0].vout[1].to.program == vid && t17.txs[0].vout[0].value == 50 * COIN + 90'000,
              "template: certificate for 15, 10% of the 100000 fyl fee goes to the voter");
        Block bad_share = t17;
        bad_share.txs[0].vout[1].value += 1;
        bad_share.txs[0].vout[0].value -= 1;
        rejects(A, remine(A, bad_share), "fee share computed", "wrong voter share rejected");
        Block no_share = t17;
        no_share.txs[0].vout[0].value += no_share.txs[0].vout[1].value;
        no_share.txs[0].vout.pop_back();
        rejects(A, remine(A, no_share), "no fee share", "missing voter share rejected");
        Block greedy = t17;
        greedy.txs[0].vout[0].value += 1;
        rejects(A, remine(A, greedy), "more than reward and fees", "supply cap: coinbase above reward + fees rejected");
        Block bad_sig = t17;
        bad_sig.ext.cert->votes[0].sig.mldsa[3] ^= 1;
        set_commit(bad_sig, 68, bad_sig.ext.hash());
        rejects(A, remine(A, bad_sig), "invalid vote signature", "certificate with a forged signature rejected");
        Block bad_vid = t17;
        bad_vid.ext.cert->votes[0].vid[0] ^= 1;
        set_commit(bad_vid, 68, bad_vid.ext.hash());
        rejects(A, remine(A, bad_vid), "not from a registered", "certificate vote from an unknown validator rejected");
        Block empty = t17;
        empty.ext.cert->votes.clear();
        set_commit(empty, 68, empty.ext.hash());
        rejects(A, remine(A, empty), "certificate", "certificate without votes rejected");
        check(add_block(A, remine(A, t17), &why), "valid certificate block accepted (" + why + ")");
        uint64_t out = 0;
        auto tipb = A.read_block(A.tip()->hash);
        for (auto& o : tipb->txs[0].vout) out += o.value;
        check(out == 50 * COIN + 100'000, "coinbase total = reward + fees: fees redistributed, nothing minted (" + std::to_string(out) + ")");
        check(A.chain_final() == 15, "chain-certified finality = 15");
    }
    {
        fs::create_directories(root / "fresh");
        fs::copy(root / "a" / "blocks", root / "fresh" / "blocks", fs::copy_options::recursive);   // blocks only: no votes, no finality.dat
        Node F(t, root / "fresh");
        check(F.height() == A.height() && F.chain_final() == 15 && F.finalized_height() == 15 && F.registered(vid) && F.utxo_commit() == A.utxo_commit(),
              "fresh node replaying only the blocks derives finality 15, the registry and the same UTXO commitment");
    }

    std::cout << "phase 3: equivocation evidence and slashing\n";
    for (int i = 18; i <= 19; i++) add_block(A, mine_on(A, a0, 320 + i));
    {
        Vote v{18, A.at(18)->hash, PublicKey{}, {}};
        v.compact = true;
        v.vid = vid;
        v.sig = k0.sign(v.message());
        check(A.accept_vote(v, err) == Node::VoteResult::Accepted, "validator votes for checkpoint 18");
        Vote bad = v;
        bad.block[0] ^= 0x55;
        bad.sig = k0.sign(bad.message());
        check(A.accept_vote(bad, err) == Node::VoteResult::Rejected && err_has(err, errmsg::VOTE_EQUIVOCATION), "conflicting vote at the same height detected (" + err + ")");
        auto ev = A.take_new_evidence();
        check(ev.size() == 1 && ev[0].vid == vid && A.take_new_evidence().empty(), "evidence produced once");
        Evidence forged = ev[0];
        forged.sig_b.mldsa[0] ^= 1;
        Node other(t, root / "other");   // an unrelated node has no registry: evidence cannot be checked there
        check(other.accept_evidence(ev[0], err) == Node::VoteResult::Rejected, "evidence is only accepted against a registered key");
        Block t20 = A.block_template(a0, 340);
        check(t20.ext.evidence.size() == 1, "template includes the evidence");
        Block t20_bad = t20;
        t20_bad.ext.evidence[0].sig_b.mldsa[0] ^= 1;
        set_commit(t20_bad, 68, t20_bad.ext.hash());
        rejects(A, remine(A, t20_bad), "evidence", "forged evidence rejected");
        Block same_block = t20;
        same_block.ext.evidence[0].block_b = same_block.ext.evidence[0].block_a;
        set_commit(same_block, 68, same_block.ext.hash());
        rejects(A, remine(A, same_block), "evidence", "evidence with identical blocks rejected");
        check(add_block(A, remine(A, t20), &why), "block with evidence accepted (" + why + ")");
        check(A.slashed(vid) && A.slashed_count() == 1, "equivocator is slashed on-chain");
        for (int i = 21; i <= 22; i++) add_block(A, mine_on(A, a0, 340 + i));
        const StakeSet* s21 = A.stake_at(21);
        check(s21 && s21->empty(), "slashed validator is excluded from later stake snapshots");
        Transaction exit = spend_coin(k0, stake_op, stake_tx.vout[0], a1, 10 * COIN, 20000, a0);
        check(!A.accept_tx(exit, err) && err_has(err, "frozen"), "slashed stake can never be withdrawn (" + err + ")");
        { Node R(t, root / "a"); check(R.slashed(vid) && R.slashed_count() == 1 && R.equivocators() == 1, "slashing and equivocator list survive a restart"); }
        fs::remove_all(root / "a" / "evidence");
        Node R2(t, root / "a");
        check(R2.slashed(vid) && R2.tip()->hash == A.tip()->hash, "on-chain slashing is rebuilt from the blocks alone");
    }
    fs::remove_all(root);
}

static void selftest_checkpoints(const HybridKey& k0, const HybridKey& k1) {
    namespace fs = std::filesystem;
    std::cout << "hard-coded checkpoints\n";
    const ChainParams& m = ChainParams::main();
    check(m.checkpoints.size() >= 2 && m.checkpoints[0].first == 0 && m.checkpoints[0].second == hash_hex(m.genesis().header.hash()),
          "main checkpoint 0 is the genesis hash");
    check(m.checkpoints.back().first == 12 && m.checkpoints.back().second == "000000006bcea5c8d0ad42901bcc4f74bf30214767dc2a66c81ee7fdab7e1709",
          "main checkpoint 12 is the current tip");
    Address a0{ADDRESS_VERSION_HYBRID, k0.pub().program()}, a1{ADDRESS_VERSION_HYBRID, k1.pub().program()};
    fs::path root = fs::temp_directory_path() / ("phylka-selftest-cp-" + std::to_string(::getpid()));
    fs::remove_all(root);
    ChainParams plain = ChainParams::dev();
    Node src(plain, root / "src"), alt(plain, root / "alt");
    std::vector<Block> good, other;
    for (int i = 0; i < 3; i++) { good.push_back(mine_on(src, a0, i)); add_block(src, good.back()); }
    for (int i = 0; i < 5; i++) { other.push_back(mine_on(alt, a1, 50 + i)); add_block(alt, other.back()); }
    ChainParams locked = plain;   // checkpoint at height 2 = the block of `src`
    locked.checkpoints = {{2, hash_hex(good[1].header.hash())}};
    Node n(locked, root / "n");
    std::string why;
    check(!add_block(n, other[0]) || n.height() == 1, "height 1 is not constrained");
    check(!add_block(n, other[1], &why) && why.find("checkpoint") != std::string::npos, "a different block at the checkpoint height is rejected (" + why + ")");
    check(add_block(n, good[0]) && add_block(n, good[1]) && n.height() == 2 && n.tip()->hash == good[1].header.hash(), "the checkpointed block is accepted");
    check(add_block(n, other[1]) == false, "the competing block stays rejected");
    for (int i = 2; i < 5; i++) add_block(n, other[i]);
    check(n.tip()->hash != other[4].header.hash() && n.height() <= 3, "a heavier chain that forks below the checkpoint is never activated");
    std::cout << "mining guard\n";
    {
        std::string w;
        Node mn(ChainParams::main(), root / "mainnode");
        check(mn.mining_blocked(5, std::time(nullptr), w), "main node below the last checkpoint does not mine (" + w + ")");
        ChainParams nocp = ChainParams::main();
        nocp.checkpoints.clear();
        Node sn(nocp, root / "syncnode");
        time_t tt = time_t(sn.tip()->header.time);
        check(sn.mining_blocked(0, tt + 25 * 3600, w), "no peers and a tip older than 24 h: initial sync, mining deferred (" + w + ")");
        check(!sn.mining_blocked(1, tt + 25 * 3600, w) && !sn.mining_blocked(0, tt + 3600, w), "with a peer, or with a fresh tip, mining is allowed");
        check(!n.mining_blocked(0, std::time(nullptr), w), "dev network without checkpoints is not blocked");
    }
    fs::remove_all(root);
}

static int selftest() {
    std::cout << "hashes\n";
    check(to_hex(sha256(str_bytes("abc"))) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "SHA-256 (FIPS 180 'abc')");
    check(to_hex(sha3_256(str_bytes("abc"))) == "3a985da74fe225b2045c172d6bd390bd855f086e3e9d525b46bfe24511431532", "SHA3-256 (FIPS 202 'abc')");
    check(to_hex(hmac_sha512(Bytes(20, 0x0b), str_bytes("Hi There"))) ==
              "87aa7cdea5ef619d4ff0b4241a1d6cb02379f4e2ce4ec2787ad0b30545e17cdedaa833b7d6b8a702038b274eaea3f4e4be9d914eeb61f1702e696c203a126854",
          "HMAC-SHA512 (RFC 4231 case 1)");

    std::cout << "seed phrase (BIP39 vectors)\n";
    std::string m12 = bip39::from_entropy(Bytes(16, 0));
    check(m12 == "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about", "12 words from zero entropy");
    check(to_hex(bip39::to_seed(m12, "TREZOR")) ==
              "c55257c360c07c72029aebc1b53c05ed0362ada38ead3e3e9efa3708e53495531f09a6987599d18264c1e1c92f2cf141630c7a3c4ab7c81b2f001698e7463b04",
          "seed of 12 words + passphrase TREZOR");
    std::string m24 = bip39::from_entropy(Bytes(32, 0));
    check(m24.size() > 4 && m24.substr(m24.size() - 4) == " art" && bip39::valid(m24), "24 words end with 'art', checksum valid");
    check(to_hex(bip39::to_seed(m24, "TREZOR")) ==
              "bda85446c68413707090a52022edd26a1c9462295029f2e60cd7c4f2bbd3097170af7a4d73245cafa9c3cca8d561a7c3de6f5d4a10be8ed2a5e608d68f92fcc8",
          "seed of 24 words + passphrase TREZOR");
    check(!bip39::valid(m12.substr(0, m12.size() - 5) + "abandon"), "wrong checksum word rejected");
    check(bip39::valid(bip39::generate()), "new random phrase is valid");

    std::cout << "addresses (bech32m, BIP350 vectors)\n";
    check(bech32m::decode("A1LQFN3A").has_value(), "valid bech32m string accepted");
    auto btc = bech32m::decode("bc1pw508d6qejxtdg4y5r3zarvary0c5xw7kw508d6qejxtdg4y5r3zarvary0c5xw7kt5nd6y");
    Bytes prog;
    check(btc && bech32m::convert_bits(prog, Bytes(btc->second.begin() + 1, btc->second.end()), 5, 8, false) &&
              to_hex(prog) == "751e76e8199196d454941c45d1b3a323f1433bd6751e76e8199196d454941c45d1b3a323f1433bd6",
          "BIP350 segwit v1 vector decodes to the right program");
    check(!bech32m::decode("bc1qw508d6qejxtdg4y5r3zarvary0c5xw7kv8f3t4").has_value(), "bech32 (v0 checksum) rejected as bech32m");

    std::cout << "hybrid keys (Schnorr + ML-DSA-44)\n";
    Bytes seed = bip39::to_seed(m24);
    auto t0 = std::chrono::steady_clock::now();
    HybridKey k0 = HybridKey::derive(seed, 0);
    double derive_ms = ms_since(t0);
    HybridKey k0b = HybridKey::derive(seed, 0);
    HybridKey k1 = HybridKey::derive(seed, 1);
    check(k0.pub().program() == k0b.pub().program(), "same seed and index -> same keys (deterministic)");
    check(k0.pub().program() != k1.pub().program(), "different index -> different keys");
    Address a0{ADDRESS_VERSION_HYBRID, k0.pub().program()};
    std::string s0 = a0.str();
    check(s0.rfind("plk1p", 0) == 0 && s0.size() == 63, "address " + s0);
    check(Address::parse(s0) && Address::parse(s0)->program == a0.program, "address round-trips");
    std::string typo = s0;
    typo[20] = typo[20] == 'q' ? 'p' : 'q';
    check(!Address::parse(typo), "one-character typo rejected by checksum");
    check(!Address::parse(a0.str("plkd")), "dev-network address rejected on main");
    Hash256 msg = sha256(str_bytes("PHYLKA"));
    t0 = std::chrono::steady_clock::now();
    HybridSignature sig = k0.sign(msg);
    double sign_ms = ms_since(t0);
    t0 = std::chrono::steady_clock::now();
    bool good = verify(k0.pub(), msg, sig);
    double verify_ms = ms_since(t0);
    check(good, "hybrid signature verifies");
    HybridSignature bad1 = sig;
    bad1.schnorr[5] ^= 1;
    HybridSignature bad2 = sig;
    bad2.mldsa[100] ^= 1;
    check(!verify(k0.pub(), msg, bad1), "broken Schnorr half rejected (ML-DSA alone is not enough)");
    check(!verify(k0.pub(), msg, bad2), "broken ML-DSA half rejected (Schnorr alone is not enough)");
    check(!verify(k1.pub(), msg, sig), "signature does not verify under another key");
    std::printf("         key derivation %.1f ms, sign %.1f ms, verify %.2f ms\n", derive_ms, sign_ms, verify_ms);

    std::cout << "transactions\n";
    const ChainParams& dev = ChainParams::dev();
    Address a1{ADDRESS_VERSION_HYBRID, k1.pub().program()};
    Transaction fund = make_coinbase(dev, 1, a0, "test funding");
    Transaction tx;
    tx.vin.push_back(TxIn{OutPoint{fund.txid(), 0}, {}});
    tx.vout.push_back(TxOut{30 * COIN, a1});
    tx.vout.push_back(TxOut{20 * COIN - 1000, a0});   // change, 1000 fyl fee
    std::vector<TxOut> spent{fund.vout[0]};
    tx.sign(0, k0, spent);
    std::string why;
    check(tx.verify_input(0, spent, &why), "spend signed by the owner verifies");
    Bytes raw = tx.serialize();
    size_t pos = 0;
    auto back = Transaction::parse(raw, pos);
    check(back && pos == raw.size() && back->wtxid() == tx.wtxid() && back->verify_input(0, spent),
          "serialize -> parse round-trip, " + std::to_string(raw.size()) + " bytes");
    Transaction stolen = tx;
    stolen.vout[0].to = a0;   // redirect the payment
    check(!stolen.verify_input(0, spent, &why), "changed recipient breaks the signature (" + why + ")");
    std::vector<TxOut> lie{TxOut{1000 * COIN, a0}};
    check(!tx.verify_input(0, lie, &why), "lying about the spent amount is caught (" + why + ")");
    Transaction thief = tx;
    thief.sign(0, k1, spent);
    check(!thief.verify_input(0, spent, &why), "someone else's key cannot spend (" + why + ")");

    std::cout << "blocks (no mining: checked against Bitcoin's real genesis header)\n";
    BlockHeader btc_genesis;
    btc_genesis.version = 1;
    btc_genesis.merkle = from_display_hex("4a5e1e4baab89f3a32518a88c31bc87f618f76673e2cc77ab2127b7afdeda33b");
    btc_genesis.time = 1231006505;
    btc_genesis.bits = 0x1d00ffff;
    btc_genesis.nonce = 2083236893;
    check(btc_genesis.serialize().size() == 80, "header is 80 bytes (Bitcoin-compatible)");
    {
        HeaderHasher hh(btc_genesis.serialize());
        check(hash_hex(hh.hash(2083236893)) == "000000000019d6689c085ae165831e934ff763ae46a2a6c172b3f1b60a8ce26f",
              "fast midstate miner hash matches on Bitcoin's genesis");
        bool same = true;
        Bytes hdr = btc_genesis.serialize();
        for (uint32_t n = 0; n < 2000 && same; n++) {
            hdr[76] = uint8_t(n); hdr[77] = uint8_t(n >> 8); hdr[78] = 0; hdr[79] = 0;
            Bytes ref = hdr;
            Hash256 a = hh.hash(n);
            uint8_t first[32];
            ::sha256(first, ref.data(), ref.size());     // PQClean reference implementation
            uint8_t second[32];
            ::sha256(second, first, 32);
            same = std::equal(a.begin(), a.end(), second);
        }
        check(same, "midstate + CommonCrypto == PQClean reference SHA-256 on 2000 nonces");
    }
    check(hash_hex(btc_genesis.hash()) == "000000000019d6689c085ae165831e934ff763ae46a2a6c172b3f1b60a8ce26f",
          "Bitcoin genesis header hashes to 000000000019d6…ce26f");
    check(check_pow(btc_genesis.hash(), btc_genesis.bits), "its proof of work passes our difficulty check");
    btc_genesis.nonce++;
    check(!check_pow(btc_genesis.hash(), btc_genesis.bits), "any other nonce fails it");
    Block b = make_genesis(dev, a0, "selftest", 1793000000);
    b.txs.push_back(tx);
    Hash256 r1 = b.merkle_root();
    b.txs[1].witness[0].sig.schnorr[0] ^= 1;
    check(b.merkle_root() != r1, "merkle root commits to signatures");
    check(block_reward(ChainParams::main(), 0) == 50 * COIN && block_reward(ChainParams::main(), 210'000) == 25 * COIN,
          "reward 50 PLK, halves after 210 000 blocks (21 M PLK in total)");

    {
        Block mg = ChainParams::main().genesis();
        check(hash_hex(mg.header.hash()) == "00000000df01a464bd9f7ad30b88da74059e25ea5b60eccaac0f95f2ba1a70a0" &&
                  check_pow(mg.header.hash(), mg.header.bits),
              "PHYLKA main genesis 00000000df01a464…a70a0 is valid");
        check(hash_hex(mg.header.merkle) == "c13eca05af56c23f64944ce260820cd4d2e2d8fac2e3a96de9acc72fcb325b1d", "genesis merkle root");
    }

    std::cout << "difficulty (LWMA, 45 blocks, 10 minutes)\n";
    {
        U256 lim = U256::from_compact(0x1e00ffff), g = U256::from_compact(0x1d00ffff);
        uint64_t n = 45, k = n * (n + 1) / 2 * 600;
        auto with = [&](uint64_t solve) { return lwma_scale(g, k / 600 * solve, k, lim, 0x1e00ffff); };
        check(with(600) == 0x1d00ffff, "blocks on time -> difficulty unchanged");
        check(U256::from_compact(with(300)).work().to_ld() > g.work().to_ld() * 1.9, "blocks twice as fast -> twice the difficulty");
        check(U256::from_compact(with(1200)).work().to_ld() < g.work().to_ld() * 0.51, "blocks twice as slow -> half the difficulty");
        check(with(600 * 1000) == 0x1e00ffff, "never easier than the limit (difficulty 1/256)");
        U256 dev = U256::from_compact(0x207fffff);
        uint64_t kd = n * (n + 1) / 2 * 5;
        check(U256::from_compact(lwma_scale(dev, kd / 5 * 1, kd, dev, 0x207fffff)).work().to_ld() > dev.work().to_ld() * 4,
              "dev network: huge target no longer overflows, blocks 5x too fast -> 5x harder");
    }

    selftest_u256();
    selftest_node(k0, k1);
    selftest_phase3(k0, k1);
    selftest_checkpoints(k0, k1);

    std::cout << "wallet file (encrypted seed)\n";
    WalletSecret ws;
    ws.mnemonic = m24;
    Bytes sealed = vault::seal(ws, "correct horse battery");
    check(vault::open(sealed, "correct horse battery").mnemonic == m24, "right password opens the wallet");
    bool rejected = false;
    try { vault::open(sealed, "wrong password!!"); } catch (const std::exception&) { rejected = true; }
    check(rejected, "wrong password rejected");
    sealed[50] ^= 1;
    rejected = false;
    try { vault::open(sealed, "correct horse battery"); } catch (const std::exception&) { rejected = true; }
    check(rejected, "tampered file rejected");
    check(std::search(sealed.begin(), sealed.end(), m24.begin(), m24.begin() + 7) == sealed.end(), "seed words are not stored in plain text");

    gui_selftest(check);

    std::cout << (failures ? std::to_string(failures) + " FAILED\n" : "ALL OK\n");
    return failures ? 1 : 0;
}

static std::atomic<bool> g_stop{false};

#ifdef PLK_ENABLE_MKGENESIS
/// Mines Bitcoin-difficulty-1 genesis for main on all cores; prints the values to put into ChainParams::main()
static int mkgenesis(int threads) {
    const ChainParams& p = ChainParams::main();
    std::string tag = "PHYLKA genesis: " + p.genesis_message;
    Address burn{ADDRESS_VERSION_HYBRID, sha3_256(Bytes(tag.begin(), tag.end()))};
    Block g = make_genesis(p, burn, p.genesis_message, uint32_t(std::time(nullptr)));
    std::cout << "Mining the main-network genesis on " << threads << " threads, difficulty 1 (0x1d00ffff)…\n"
              << "Message: " << p.genesis_message << std::endl;
    std::atomic<bool> found{false};
    std::atomic<uint64_t> total{0};
    uint32_t nonce_found = 0;
    std::mutex fm;
    auto t0 = std::chrono::steady_clock::now();
    // One timestamp gives 2^32 nonces (~63% chance at difficulty 1); if none fits, move the time and go on
    for (int round = 0; !found; round++) {
        if (round) {
            g.header.time++;
            std::cout << "\n  no block found for this timestamp - time +1 s, continuing" << std::endl;
        }
        std::vector<std::thread> ws;
        for (int w = 0; w < threads; w++) {
            ws.emplace_back([&, w] {
                HeaderHasher hasher(g.header.serialize());
                uint64_t span = (1ULL << 32) / threads;
                uint64_t end = w == threads - 1 ? (1ULL << 32) : (w + 1) * span;
                for (uint64_t n = w * span; n < end && !found; n++) {
                    if (check_pow(hasher.hash(uint32_t(n)), g.header.bits)) {
                        std::lock_guard<std::mutex> lk(fm);
                        if (!found) { nonce_found = uint32_t(n); found = true; }
                    }
                    if ((n & 0xfffff) == 0xfffff) {
                        total += 0x100000;
                        if (w == 0) {
                            double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
                            std::printf("\r  %.0f M hashes, %.2f M/s   ", total / 1e6, total / sec / 1e6);
                            std::fflush(stdout);
                        }
                    }
                }
            });
        }
        for (auto& t : ws) t.join();
    }
    g.header.nonce = nonce_found;
    std::cout << "\n\nGenesis found!\n  hash:  " << hash_hex(g.header.hash()) << "\n  time:  " << g.header.time
              << "\n  nonce: " << g.header.nonce << "\n  merkle: " << hash_hex(g.header.merkle) << "\n";
    return 0;
}
#endif

static int usage(int code) {
    (code ? std::cerr : std::cout) << "PHYLKA (PLK)\n"
                                   "  phylka node [--dev] [--mine ADDRESS] [--threads N] [--validator] [--wallet FILE] [--mine-anyway (--dev only)]\n"
                 "              [--connect host:port]... [--addnode host:port]... [--seed host:port]... [--noseeds]\n"
                 "              [--port P] [--rpc R] [--datadir DIR] [--conf FILE] [--public] [--upgrade-height N (--dev only)]\n"
                 "     --public: listen on all interfaces (default: loopback only); RPC is always 127.0.0.1 only\n"
                 "     config: DIR/phylka.conf or --conf FILE, key=value lines (keys are flag names without \"--\", # starts a comment)\n"
                 "  phylka wallet create|restore|address|newaddress|balance|history|send ADDRESS AMOUNT|showseed [--dev] [--rpc R]\n"
                 "  phylka gui [--dev] [--wallet FILE] [--rpc R] [--port P] [--no-browser]\n"
                 "     GUI wallet: a local web page on 127.0.0.1:P (default 7335, dev 17335); it signs locally and talks to the node over RPC\n"
                 "  phylka selftest | newseed [--dev] | help\n";
    return code;
}

/// One `key=value` per line, `#` comments. Keys are the long CLI flags without "--"; flags without a value take 1/true/yes.
/// Command-line values win; connect/seed/addnode lines are added to the command-line ones.
static void load_config(const std::filesystem::path& file, std::map<std::string, std::string>& opt, std::map<std::string, std::vector<std::string>>& lists) {
    std::ifstream in(file);
    for (std::string line; std::getline(in, line);) {
        auto hash = line.find('#');
        if (hash != std::string::npos) line.erase(hash);
        auto eq = line.find('=');
        auto trim = [](std::string x) {
            size_t a = x.find_first_not_of(" \t\r"), b = x.find_last_not_of(" \t\r");
            return a == std::string::npos ? std::string() : x.substr(a, b - a + 1);
        };
        std::string k = trim(line.substr(0, eq)), v = eq == std::string::npos ? "1" : trim(line.substr(eq + 1));
        if (k.empty() || k == "conf") continue;
        if (k == "connect" || k == "seed" || k == "addnode") lists[k].push_back(v);
        else if (!opt.count(k)) opt[k] = (v == "true" || v == "yes" || v.empty()) ? "1" : v;
    }
}

int main(int argc, char** argv) {
    platform::net_init();
    std::string cmd = argc > 1 ? argv[1] : "";
    std::map<std::string, std::string> opt;
    std::map<std::string, std::vector<std::string>> lists;   // repeatable flags: connect, seed, addnode
    std::vector<std::string> pos;
    static const std::set<std::string> valued = {"datadir", "port", "rpc", "connect", "seed", "addnode", "mine", "threads", "wallet", "conf", "upgrade-height"};
    for (int i = 2; i < argc; i++) {
        std::string a = argv[i];
        if (a.rfind("--", 0) == 0) {
            std::string k = a.substr(2);
            std::string v = valued.count(k) && i + 1 < argc ? argv[++i] : "1";
            if (k == "connect" || k == "seed" || k == "addnode") lists[k].push_back(v);
            else opt[k] = v;
        } else {
            pos.push_back(a);
        }
    }
    static const std::set<std::string> commands = {"selftest", "wallet", "node", "newseed", "gui", "help"};
    static const std::set<std::string> flags = {"dev", "datadir", "port", "rpc", "connect", "seed", "addnode", "noseeds", "mine", "threads", "wallet",
                                                "conf", "upgrade-height", "validator", "public", "test-equivocate", "mine-anyway", "no-browser", "help"};
    bool help = cmd == "--help" || cmd == "-h" || cmd == "help" || opt.count("help");
    for (int i = 2; i < argc; i++) if (std::string(argv[i]) == "-h") help = true;
    if (help) return usage(0);
    if (!commands.count(cmd)) { std::cerr << "unknown command: " << cmd << "\n"; return usage(2); }
    for (auto& [k, v] : opt) if (!flags.count(k)) { std::cerr << "unknown flag: --" << k << "\n"; return usage(2); }
    for (auto& [k, v] : lists) if (!flags.count(k)) { std::cerr << "unknown flag: --" << k << "\n"; return usage(2); }
    if (cmd == "node" && !pos.empty()) { std::cerr << "unexpected argument: " << pos[0] << "\n"; return usage(2); }
    std::string home = std::getenv("HOME") ? std::getenv("HOME") : ".";
    if (cmd == "node") {   // config file: --conf FILE, else <datadir>/phylka.conf
        std::filesystem::path d = opt.count("datadir") ? opt["datadir"] : home + "/phylka/data/" + (opt.count("dev") ? "dev" : "main");
        load_config(opt.count("conf") ? std::filesystem::path(opt["conf"]) : d / "phylka.conf", opt, lists);
    }
    static ChainParams custom = ChainParams::dev();
    if (opt.count("upgrade-height")) {
        if (!opt.count("dev")) { std::cerr << "--upgrade-height: --dev only (on the main network the activation height is part of consensus)\n"; return 1; }
        custom.upgrade_height = uint32_t(std::stoul(opt["upgrade-height"]));
    }
    const ChainParams& p = opt.count("dev") ? (opt.count("upgrade-height") ? custom : ChainParams::dev()) : ChainParams::main();
    uint16_t rpc_port = opt.count("rpc") ? uint16_t(std::stoi(opt["rpc"])) : p.rpc_port;

    if (cmd == "selftest") return selftest();
#ifdef PLK_ENABLE_MKGENESIS
    if (cmd == "mkgenesis") return mkgenesis(opt.count("threads") ? std::stoi(opt["threads"]) : int(std::thread::hardware_concurrency()));
#endif
    if (cmd == "wallet") {
        std::filesystem::path f = opt.count("wallet") ? opt["wallet"] : home + "/phylka/wallets/" + p.name + ".wallet";
        return wallet_command(p, f, rpc_port, pos);
    }
    if (cmd == "gui") {
        if (!pos.empty()) { std::cerr << "unexpected argument: " << pos[0] << "\n"; return usage(2); }
        GuiConfig g;
        g.params = &p;
        g.wallet = opt.count("wallet") ? opt["wallet"] : home + "/phylka/wallets/" + p.name + ".wallet";
        g.rpc = rpc_port;
        g.port = opt.count("port") ? uint16_t(std::stoi(opt["port"])) : uint16_t(p.rpc_port + 1);   // 7335 main, 17335 dev
        return gui_command(g, !opt.count("no-browser"));
    }
    if (cmd == "node") {
        std::filesystem::path dir = opt.count("datadir") ? opt["datadir"] : home + "/phylka/data/" + p.name;
        uint16_t port = opt.count("port") ? uint16_t(std::stoi(opt["port"])) : p.p2p_port;
        try {
            Node node(p, dir);
            std::cout << "PHYLKA (" << p.name << ")  height " << node.height() << "  data: " << dir.string() << std::endl;
            // Peers to dial (host:port, DNS names are resolved on every attempt): --connect, --addnode, --seed and the built-in seeds
            std::vector<std::string> targets;
            auto add_target = [&](const std::string& t) { if (std::find(targets.begin(), targets.end(), t) == targets.end()) targets.push_back(t); };
            for (auto* k : {"connect", "addnode", "seed"}) for (auto& t : lists[k]) add_target(t);
            if (!opt.count("noseeds")) for (auto& t : p.seeds) add_target(t);
            Net net(node, port, targets, opt.count("public") > 0);
            std::unique_ptr<Miner> miner;
            Rpc rpc(node, net, rpc_port, [&] { return miner ? miner->hashrate() : 0.0; });
            if (opt.count("mine")) {
                auto to = Address::parse(opt["mine"], p.hrp);
                if (!to) { std::cerr << "--mine: invalid address or wrong network (" << p.hrp << "1…)\n"; return 1; }
                int th = opt.count("threads") ? std::stoi(opt["threads"]) : 1;
                if (opt.count("mine-anyway") && !opt.count("dev")) { std::cerr << "--mine-anyway: --dev only\n"; return 1; }
                // Guard: never mine a private branch while below the last checkpoint or while clearly still syncing
                auto may_mine = [&]() {
                    if (opt.count("mine-anyway")) return true;
                    std::string why;
                    static std::atomic<time_t> last_log{0};
                    if (!node.mining_blocked(net.peers(), std::time(nullptr), why)) return true;
                    if (std::time(nullptr) - last_log > 30) { last_log = std::time(nullptr); std::cout << "waiting for sync, mining postponed (" << why << ")" << std::endl; }
                    return false;
                };
                miner = std::make_unique<Miner>(node, *to, th, [&](const Block& b) { net.submit_block(b); }, may_mine);
                std::cout << "Mining on " << th << " threads -> " << to->str(p.hrp) << std::endl;
            }
            // Validator: signs each checkpoint once it is finality_depth blocks deep, if our stake counts there
            std::unique_ptr<HybridKey> vkey;
            std::thread vthread;
            if (opt.count("validator")) {
                std::filesystem::path wf = opt.count("wallet") ? opt["wallet"] : home + "/phylka/wallets/" + p.name + ".wallet";
                vkey = std::make_unique<HybridKey>(validator_key(p, wf));
                Address stake{ADDRESS_VERSION_STAKE, vkey->pub().program()};
                std::cout << "Validator: stake at " << stake.str(p.hrp) << std::endl;
                vthread = std::thread([&] {
                    Hash256 prog = vkey->pub().program();
                    auto last_reg = std::chrono::steady_clock::now() - std::chrono::hours(1);
                    while (!g_stop) {
                        std::this_thread::sleep_for(std::chrono::seconds(1));
                        std::optional<Vote> v;
                        bool want_reg = false;
                        {
                            std::lock_guard<std::mutex> lk(node.mutex());
                            uint32_t h = node.height();
                            want_reg = p.upgraded(h + 1) && !node.registered(prog);
                            if (h >= p.finality_depth + p.checkpoint_interval) {
                                uint32_t cp = (h - p.finality_depth) / p.checkpoint_interval * p.checkpoint_interval;
                                const StakeSet* s = node.stake_at(cp);
                                if (s && s->count(prog) && !node.voted_any(cp, prog) && (!p.upgraded(cp) || node.registered(prog))) {
                                    v = Vote{cp, node.at(cp)->hash, vkey->pub(), {}};
                                    v->compact = p.upgraded(cp);   // from the upgrade on, the vote names the registered key by id
                                    if (v->compact) v->vid = prog;
                                }
                            }
                        }
                        if (want_reg && std::chrono::steady_clock::now() - last_reg > std::chrono::seconds(20)) {
                            std::string err;
                            last_reg = std::chrono::steady_clock::now();
                            if (node.accept_registration(vkey->pub(), err) == Node::VoteResult::Rejected) std::cout << "Key registration: " << err << std::endl;
                            else { net.broadcast_reg(vkey->pub()); std::cout << "Validator key sent for on-chain registration" << std::endl; }
                        }
                        if (!v) continue;
                        v->sig = vkey->sign(v->message());
                        std::string err;
                        uint32_t before = node.finalized_height();
                        if (node.accept_vote(*v, err) == Node::VoteResult::Accepted) {
                            net.broadcast_vote(*v);
                            std::cout << "Checkpoint vote #" << v->height << " signed and broadcast" << std::endl;
                            if (node.finalized_height() > before)
                                std::cout << "Finalized block #" << node.finalized_height() << " - it can no longer be reverted" << std::endl;
                            if (opt.count("test-equivocate") && opt.count("dev") && v->compact) {   // dev-only fault injection for the slashing test
                                Vote bad = *v;
                                bad.block = sha256(Bytes(v->block.begin(), v->block.end()));
                                bad.sig = vkey->sign(bad.message());
                                node.accept_vote(bad, err);
                                net.broadcast_vote(bad);
                                for (auto& e : node.take_new_evidence()) net.broadcast_evidence(e);
                                std::cout << "TEST: signed a conflicting vote at #" << v->height << std::endl;
                            }
                        }
                    }
                });
            }
            std::signal(SIGINT, [](int) { g_stop = true; });
            std::signal(SIGTERM, [](int) { g_stop = true; });
            while (!g_stop) std::this_thread::sleep_for(std::chrono::milliseconds(200));
            std::cout << "\nShutting down…" << std::endl;
            if (vthread.joinable()) vthread.join();
            miner.reset();
            return 0;
        } catch (const std::exception& e) {
            std::cerr << "Error: " << e.what() << "\n";
            return 1;
        }
    }
    if (cmd == "newseed") {
        std::string m = bip39::generate();
        Bytes seed = bip39::to_seed(m);
        HybridKey k = HybridKey::derive(seed, 0);
        wipe(seed.data(), seed.size());
        std::cout << "Seed phrase (write it down on paper, show it to no one):\n\n  " << m << "\n\n"
                  << "Address #0: " << Address{ADDRESS_VERSION_HYBRID, k.pub().program()}.str(p.hrp) << "\n";
        wipe(m.data(), m.size());
        return 0;
    }
    return usage(2);
}
