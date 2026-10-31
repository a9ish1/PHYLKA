#include "wallet.h"

#include "platform.h"


#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "net.h"

extern "C" {
#include "fips202.h"
}

namespace plk {

namespace fs = std::filesystem;

WalletSecret::~WalletSecret() {
    wipe(mnemonic.data(), mnemonic.size());
    wipe(passphrase.data(), passphrase.size());
}

// ---------------------------------------------------------------- vault

static const char MAGIC[] = "PLKW1";
static const uint32_t ITERS = 210'000;

static void derive(const std::string& pw, const uint8_t* salt, uint32_t iters, uint8_t enc[32], uint8_t mac[32]) {
    Bytes k = pbkdf2_hmac_sha512(Bytes(pw.begin(), pw.end()), Bytes(salt, salt + 16), iters, 64);
    std::copy(k.begin(), k.begin() + 32, enc);
    std::copy(k.begin() + 32, k.end(), mac);
    wipe(k.data(), k.size());
}

static void keystream_xor(Bytes& data, const uint8_t enc[32], const uint8_t nonce[16]) {
    uint8_t in[48];
    std::copy(enc, enc + 32, in);
    std::copy(nonce, nonce + 16, in + 32);
    Bytes ks(data.size());
    shake256(ks.data(), ks.size(), in, sizeof in);
    for (size_t i = 0; i < data.size(); i++) data[i] ^= ks[i];
    wipe(ks.data(), ks.size());
    wipe(in, sizeof in);
}

Bytes vault::seal(const WalletSecret& s, const std::string& password) {
    Bytes out(MAGIC, MAGIC + 5);
    out.push_back(ITERS >> 24); out.push_back(ITERS >> 16); out.push_back(ITERS >> 8); out.push_back(ITERS);
    uint8_t salt[16], nonce[16], enc[32], mac[32];
    random_bytes(salt, 16);
    random_bytes(nonce, 16);
    derive(password, salt, ITERS, enc, mac);
    out.insert(out.end(), salt, salt + 16);
    out.insert(out.end(), nonce, nonce + 16);
    std::string plain = s.mnemonic + "\n" + s.passphrase;
    Bytes ct(plain.begin(), plain.end());
    wipe(plain.data(), plain.size());
    keystream_xor(ct, enc, nonce);
    out.insert(out.end(), ct.begin(), ct.end());
    auto tag = hmac_sha512(Bytes(mac, mac + 32), out);
    out.insert(out.end(), tag.begin(), tag.begin() + 32);
    wipe(enc, 32);
    wipe(mac, 32);
    return out;
}

WalletSecret vault::open(const Bytes& f, const std::string& password) {
    const size_t head = 5 + 4 + 16 + 16;
    if (f.size() < head + 32 || !std::equal(MAGIC, MAGIC + 5, f.begin())) throw std::runtime_error("not a PHYLKA wallet file");
    uint32_t iters = uint32_t(f[5]) << 24 | f[6] << 16 | f[7] << 8 | f[8];
    uint8_t enc[32], mac[32];
    derive(password, &f[9], iters, enc, mac);
    Bytes body(f.begin(), f.end() - 32);
    auto tag = hmac_sha512(Bytes(mac, mac + 32), body);
    wipe(mac, 32);
    uint8_t diff = 0;
    for (int i = 0; i < 32; i++) diff |= tag[i] ^ f[f.size() - 32 + i];   // constant-time compare
    if (diff) {
        wipe(enc, 32);
        throw std::runtime_error("wrong password or corrupted file");
    }
    Bytes pt(f.begin() + head, f.end() - 32);
    keystream_xor(pt, enc, &f[25]);
    wipe(enc, 32);
    std::string s(pt.begin(), pt.end());
    wipe(pt.data(), pt.size());
    WalletSecret w;
    auto nl = s.find('\n');
    w.mnemonic = s.substr(0, nl);
    w.passphrase = nl == std::string::npos ? "" : s.substr(nl + 1);
    wipe(s.data(), s.size());
    return w;
}

// ---------------------------------------------------------------- terminal helpers

// Automated tests on the dev network may pass secrets through environment variables; never on main
static const ChainParams* g_params = nullptr;
static std::string test_env(const char* name) {
    if (!g_params || g_params->name != "dev") return "";
    const char* v = std::getenv(name);
    return v ? v : "";
}

static std::string secret(const char* prompt, const char* env = nullptr) {
    if (env) {
        std::string v = test_env(env);
        if (!v.empty()) return v;
    }
    if (!platform::has_tty()) throw std::runtime_error("no terminal for input");
    return platform::read_secret(prompt, false);
}

static std::string ask(const char* prompt) {
    if (!test_env("PHYLKA_TEST_YES").empty()) return "y";
    return platform::read_secret(prompt, true);
}

static std::string new_password() {
    for (;;) {
        std::string a = secret("Wallet file password (at least 10 characters): ", "PHYLKA_TEST_PASSWORD");
        if (a.size() < 10) { std::cout << "Too short.\n"; continue; }
        std::string b = test_env("PHYLKA_TEST_PASSWORD").empty() ? secret("Repeat password: ") : a;
        if (a == b) return a;
        std::cout << "Passwords do not match.\n";
    }
}

// ---------------------------------------------------------------- shared wallet logic

std::string format_plk(int64_t fyl) {
    char b[48];
    uint64_t a = uint64_t(fyl < 0 ? -fyl : fyl);
    std::snprintf(b, sizeof b, "%s%llu.%08llu", fyl < 0 ? "-" : "", (unsigned long long)(a / COIN), (unsigned long long)(a % COIN));
    std::string s = b;
    while (s.back() == '0') s.pop_back();
    if (s.back() == '.') s.pop_back();
    return s + " PLK";
}

uint64_t parse_amount(const std::string& s) {
    auto dot = s.find('.');
    std::string whole = s.substr(0, dot), frac = dot == std::string::npos ? "" : s.substr(dot + 1);
    if (whole.empty() || whole.size() > 10 || whole.find_first_not_of("0123456789") != std::string::npos ||
        frac.find_first_not_of("0123456789") != std::string::npos || frac.size() > 8)
        throw std::runtime_error("amount must be a number with at most 8 decimal places");
    frac.resize(8, '0');
    uint64_t v = std::stoull(whole) * COIN + std::stoull(frac);
    if (v == 0 || v > MAX_MONEY) throw std::runtime_error("invalid amount");
    return v;
}

fs::path WalletFile::idx() const { return fs::path(path.string() + ".idx"); }
bool WalletFile::exists() const { return fs::exists(path); }

void WalletFile::load() {
    std::ifstream f(path, std::ios::binary);
    sealed.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    std::ifstream i(idx());
    if (i) i >> addresses;
    if (addresses < 1) addresses = 1;
}

void WalletFile::write_private(const Bytes& data) const {
#ifndef _WIN32
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) throw std::runtime_error("cannot create wallet file");
    ::fchmod(fd, 0600);   // also tightens a pre-existing file
    size_t off = 0;
    while (off < data.size()) {
        ssize_t w = ::write(fd, data.data() + off, data.size() - off);
        if (w <= 0) { ::close(fd); throw std::runtime_error("cannot write wallet file"); }
        off += size_t(w);
    }
    ::close(fd);
#else
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(data.data()), data.size());
    fs::permissions(path, fs::perms::owner_read | fs::perms::owner_write);
#endif
}

void WalletFile::save() const {
    fs::create_directories(path.parent_path());
    write_private(sealed);
    std::ofstream(idx()) << addresses << "\n";
}

Bytes wallet_seed(const WalletSecret& s) { return bip39::to_seed(s.mnemonic, s.passphrase); }

std::vector<std::string> wallet_addresses(const WalletFile& w) {
    std::vector<std::string> out;
    std::ifstream f(w.path.string() + ".addr");
    for (std::string a; std::getline(f, a);) if (!a.empty()) out.push_back(a);
    return out;
}

void wallet_cache_addresses(const WalletFile& w, const ChainParams& p, const Bytes& seed) {
    std::ofstream f(w.path.string() + ".addr");
    for (uint32_t i = 0; i < w.addresses; i++)
        f << Address{ADDRESS_VERSION_HYBRID, HybridKey::derive(seed, i).pub().program()}.str(p.hrp) << "\n";
}

void wallet_create(WalletFile& w, const ChainParams& p, const WalletSecret& s, const std::string& password) {
    if (password.size() < MIN_PASSWORD_LEN) throw std::runtime_error("password must have at least 10 characters");
    w.sealed = vault::seal(s, password);
    w.addresses = 1;
    w.save();
    Bytes seed = wallet_seed(s);
    wallet_cache_addresses(w, p, seed);
    wipe(seed.data(), seed.size());
}

std::string wallet_new_address(WalletFile& w, const ChainParams& p, const WalletSecret& s) {
    if (wallet_addresses(w).size() >= w.addresses) w.addresses++;
    w.save();
    Bytes seed = wallet_seed(s);
    wallet_cache_addresses(w, p, seed);
    wipe(seed.data(), seed.size());
    return wallet_addresses(w).back();
}

std::vector<WalletUtxo> wallet_fetch_utxos(uint16_t rpc, const std::vector<std::string>& addrs) {
    std::vector<WalletUtxo> out;
    for (uint32_t i = 0; i < addrs.size(); i++) {
        std::istringstream in(http_get(rpc, "/utxos?addr=" + addrs[i]));
        for (std::string line; std::getline(in, line);) {
            if (line.rfind("error", 0) == 0) throw std::runtime_error(line.substr(6));
            std::istringstream l(line);
            std::string txid;
            WalletUtxo u{};
            int cb, mat;
            l >> txid >> u.op.vout >> u.value >> u.conf >> cb >> mat;
            if (!(l >> u.version)) u.version = ADDRESS_VERSION_HYBRID;
            Bytes b;
            from_hex(txid, b);
            std::reverse_copy(b.begin(), b.end(), u.op.txid.begin());
            u.coinbase = cb;
            u.mature = mat;
            u.index = i;
            out.push_back(u);
        }
    }
    return out;
}

std::vector<WalletUtxo> wallet_spendable(const std::vector<WalletUtxo>& all) {
    std::vector<WalletUtxo> v;
    for (auto& x : all) if (x.mature && x.version == ADDRESS_VERSION_HYBRID) v.push_back(x);
    return v;
}

std::vector<WalletUtxo> wallet_unlockable_stakes(const ChainParams& p, const std::vector<WalletUtxo>& all) {
    std::vector<WalletUtxo> v;
    for (auto& x : all) if (x.version == ADDRESS_VERSION_STAKE && x.conf >= p.stake_lock) v.push_back(x);
    return v;
}

WalletBalance wallet_balance(const ChainParams& p, const std::vector<WalletUtxo>& utxos) {
    WalletBalance b;
    for (auto& x : utxos) {
        if (x.version == ADDRESS_VERSION_STAKE) {
            b.staked += x.value;
            if (x.conf >= p.stake_lock) b.unlockable += x.value;
            continue;
        }
        (x.mature ? b.spendable : b.immature) += x.value;
        b.per_address[x.index] += x.value;
    }
    return b;
}

PaymentPlan wallet_plan_payment(std::vector<WalletUtxo> coins, uint64_t amount, bool send_all, uint64_t rate) {
    std::sort(coins.begin(), coins.end(), [](auto& a, auto& b) { return a.value > b.value; });
    PaymentPlan r;
    for (auto& c : coins) {   // ~3.8 KB per hybrid input
        r.use.push_back(c);
        r.in_sum += c.value;
        r.fee = (100 + 3866 * r.use.size()) * rate;   // real size: 93 + 3866 per hybrid input with change (relay minimum is 1 fyl per byte)
        if (!send_all && r.in_sum >= amount + r.fee) break;
    }
    if (send_all) {
        if (r.in_sum <= r.fee) throw std::runtime_error("nothing to withdraw: available " + format_plk(int64_t(r.in_sum)));
        amount = r.in_sum - r.fee;
    }
    if (r.in_sum < amount + r.fee)
        throw std::runtime_error("insufficient funds: available " + format_plk(int64_t(r.in_sum)) + ", need " + format_plk(int64_t(amount + r.fee)));
    r.amount = amount;
    return r;
}

Transaction wallet_sign_payment(const ChainParams& p, const WalletSecret& s, const std::vector<std::string>& addrs, const PaymentPlan& plan,
                                const Address& to) {
    Bytes seed = wallet_seed(s);
    Transaction t;
    std::vector<TxOut> spent;
    for (auto& c : plan.use) {
        t.vin.push_back(TxIn{c.op, {}});
        Address a = *Address::parse(addrs[c.index], p.hrp);
        a.version = c.version;   // the signature covers the exact output being spent
        spent.push_back(TxOut{c.value, a});
    }
    t.vout.push_back(TxOut{plan.amount, to});
    uint64_t change = plan.in_sum - plan.amount - plan.fee;
    if (change > 0) t.vout.push_back(TxOut{change, *Address::parse(addrs[0], p.hrp)});
    for (size_t i = 0; i < plan.use.size(); i++) t.sign(i, HybridKey::derive(seed, plan.use[i].index), spent);
    wipe(seed.data(), seed.size());
    return t;
}

std::string wallet_submit(uint16_t rpc, const Transaction& t) {
    std::string r = http_get(rpc, "/send?tx=" + to_hex(t.serialize()));
    if (r.rfind("ok ", 0) != 0) throw std::runtime_error("node rejected: " + r.substr(r.find(' ') + 1));
    std::string id = r.substr(3);
    while (!id.empty() && (id.back() == '\n' || id.back() == '\r')) id.pop_back();
    return id;
}

std::vector<HistoryRow> wallet_history(uint16_t rpc, const std::string& addr) {
    std::vector<HistoryRow> out;
    std::istringstream in(http_get(rpc, "/history?addr=" + addr));
    for (std::string line; std::getline(in, line);) {
        if (line.rfind("error", 0) == 0) throw std::runtime_error(line.substr(6));
        std::istringstream l(line);
        HistoryRow r;
        l >> r.height >> r.time >> r.txid >> r.delta >> r.kind;
        out.push_back(r);
    }
    return out;
}

HybridKey validator_key(const ChainParams& p, const fs::path& file) {
    g_params = &p;
    WalletFile w;
    w.path = file;
    if (!w.exists()) throw std::runtime_error("no wallet: " + file.string());
    w.load();
    std::string pw = secret("Wallet password (to sign validator votes): ", "PHYLKA_TEST_PASSWORD");
    WalletSecret s = vault::open(w.sealed, pw);
    wipe(pw.data(), pw.size());
    Bytes seed = wallet_seed(s);
    HybridKey k = HybridKey::derive(seed, 0);
    wipe(seed.data(), seed.size());
    return k;
}

// ---------------------------------------------------------------- commands

int wallet_command(const ChainParams& p, const fs::path& file, uint16_t rpc, const std::vector<std::string>& args) {
    g_params = &p;
    WalletFile w;
    w.path = file;
    std::string cmd = args.empty() ? "" : args[0];
    try {
        if (cmd == "create" || cmd == "restore") {
            if (w.exists()) throw std::runtime_error("wallet already exists: " + file.string());
            WalletSecret s;
            if (cmd == "create") {
                s.mnemonic = bip39::generate();
                std::cout << "\nYour seed phrase is the only key to all coins in this wallet.\n"
                             "Write the 24 words down on paper, in order. Do not photograph them or show them to anyone.\n\n";
                std::istringstream in(s.mnemonic);
                int n = 1;
                for (std::string word; in >> word; n++) std::printf("  %2d. %-10s%s", n, word.c_str(), n % 4 == 0 ? "\n" : "");
                std::cout << "\n";
                if (test_env("PHYLKA_TEST_YES").empty()) {
                    ask("Written down? Press Enter… ");
                    std::cout << "\033[2J\033[H";   // clear the screen before the check
                    std::istringstream in2(s.mnemonic);
                    std::vector<std::string> words;
                    for (std::string x; in2 >> x;) words.push_back(x);
                    for (int k = 0; k < 2; k++) {
                        uint8_t r;
                        random_bytes(&r, 1);
                        int i = r % 24;
                        std::string a = secret(("Check: word #" + std::to_string(i + 1) + ": ").c_str());
                        if (a != words[i]) throw std::runtime_error("word mismatch - wallet not created, start over");
                    }
                }
            } else {
                s.mnemonic = secret("Seed phrase (input hidden): ", "PHYLKA_TEST_MNEMONIC");
                if (!bip39::valid(s.mnemonic)) throw std::runtime_error("invalid phrase: check the words and their order");
                s.passphrase = test_env("PHYLKA_TEST_MNEMONIC").empty() ? secret("BIP39 passphrase (Enter for none): ") : "";
            }
            std::string pw = new_password();
            wallet_create(w, p, s, pw);
            wipe(pw.data(), pw.size());
            std::cout << "Wallet saved: " << file.string() << "\nReceiving address: " << wallet_addresses(w)[0] << "\n";
            return 0;
        }
        if (!w.exists()) throw std::runtime_error("no wallet - create one: phylka wallet create");
        w.load();
        auto addrs = wallet_addresses(w);
        if (addrs.size() < w.addresses) throw std::runtime_error("address list is corrupted - run: phylka wallet newaddress");

        if (cmd == "address") {
            std::cout << addrs.back() << "\n";
            return 0;
        }
        if (cmd == "newaddress") {
            std::string pw = secret("Wallet password: ", "PHYLKA_TEST_PASSWORD");
            WalletSecret s = vault::open(w.sealed, pw);
            wipe(pw.data(), pw.size());
            std::cout << wallet_new_address(w, p, s) << "\n";
            return 0;
        }
        if (cmd == "balance") {
            WalletBalance bal = wallet_balance(p, wallet_fetch_utxos(rpc, addrs));
            std::cout << "Available:      " << format_plk(int64_t(bal.spendable)) << "\n";
            if (bal.immature) std::cout << "Maturing:       " << format_plk(int64_t(bal.immature)) << "  (mined, waiting for " << p.coinbase_maturity << " blocks)\n";
            if (bal.staked)
                std::cout << "Staked:         " << format_plk(int64_t(bal.staked)) << "  (withdrawable " << format_plk(int64_t(bal.unlockable))
                          << ", stake locked for " << p.stake_lock << " blocks)\n";
            for (uint32_t i = 0; i < addrs.size(); i++)
                std::cout << "  #" << i << "  " << addrs[i] << "  " << format_plk(int64_t(bal.per_address[i])) << "\n";
            return 0;
        }
        if (cmd == "history") {
            for (auto& a : addrs)
                for (auto& r : wallet_history(rpc, a))
                    std::cout << (r.height == "pending" ? "pending" : "block " + r.height) << "  " << format_plk(r.delta) << "  "
                              << (r.kind == "mined" ? "mined" : "transfer") << "  " << r.txid.substr(0, 16) << "…\n";
            return 0;
        }
        // Plans, confirms, signs and broadcasts a payment from `coins` (largest first)
        auto pay = [&](const Address& to, uint64_t amount, bool send_all, std::vector<WalletUtxo> coins, uint64_t rate, const std::string& what) {
            PaymentPlan plan = wallet_plan_payment(std::move(coins), amount, send_all, rate);
            std::cout << what << " " << format_plk(int64_t(plan.amount)) << "\n  to:       " << to.str(p.hrp) << "\n  fee:      "
                      << format_plk(int64_t(plan.fee)) << "  (" << plan.use.size() << " input(s))\n";
            if (ask("Confirm? [y/N] ") != "y") { std::cout << "Cancelled.\n"; return 1; }
            std::string pw = secret("Wallet password: ", "PHYLKA_TEST_PASSWORD");
            WalletSecret s = vault::open(w.sealed, pw);
            wipe(pw.data(), pw.size());
            Transaction t = wallet_sign_payment(p, s, addrs, plan, to);
            std::cout << "Sent. Transaction " << wallet_submit(rpc, t) << "\n";
            return 0;
        };
        auto spendable = [&] { return wallet_spendable(wallet_fetch_utxos(rpc, addrs)); };
        if (cmd == "send") {
            if (args.size() < 3) throw std::runtime_error("usage: phylka wallet send ADDRESS AMOUNT [fee_rate_fyl_per_byte]");
            auto to = Address::parse(args[1], p.hrp);
            if (!to) throw std::runtime_error("invalid address or wrong network (expected " + p.hrp + "1…)");
            return pay(*to, parse_amount(args[2]), false, spendable(), args.size() > 3 ? std::stoull(args[3]) : 1, "Sending");
        }
        if (cmd == "stake") {
            if (args.size() < 2) throw std::runtime_error("usage: phylka wallet stake AMOUNT");
            uint64_t amount = parse_amount(args[1]);
            if (amount < p.min_stake) throw std::runtime_error("minimum stake is " + format_plk(int64_t(p.min_stake)));
            Address stake{ADDRESS_VERSION_STAKE, Address::parse(addrs[0], p.hrp)->program};
            std::cout << "The stake is locked for " << p.stake_lock << " blocks and starts counting after " << p.stake_activation
                      << ". Start the node with --validator to vote.\n";
            return pay(stake, amount, false, spendable(), 1, "Staking");
        }
        if (cmd == "unstake") {
            auto v = wallet_unlockable_stakes(p, wallet_fetch_utxos(rpc, addrs));
            if (v.empty()) throw std::runtime_error("no stakes whose lock period has expired");
            return pay(*Address::parse(addrs[0], p.hrp), 0, true, v, 1, "Unstaking");
        }
        if (cmd == "showseed") {
            std::string pw = secret("Wallet password: ", "PHYLKA_TEST_PASSWORD");
            WalletSecret s = vault::open(w.sealed, pw);
            wipe(pw.data(), pw.size());
            std::cout << s.mnemonic << "\n";
            return 0;
        }
        std::cerr << "wallet commands: create | restore | address | newaddress | balance | history | send ADDRESS AMOUNT | stake AMOUNT | unstake | showseed\n";
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
}

}  // namespace plk
