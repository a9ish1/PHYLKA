#include "gui.h"

#include "gui_assets.h"
#include "net.h"
#include "platform.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace plk {

namespace fs = std::filesystem;

namespace {

constexpr size_t MAX_HEAD = 16 * 1024, MAX_BODY = 16 * 1024, MAX_WORKERS = 8;

double secs() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

// ---------------------------------------------------------------- JSON (just enough for small request bodies and our replies)

struct JVal {
    enum Type { Null, Bool, Num, Str, Arr, Obj } t = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<JVal> a;
    std::map<std::string, JVal> o;

    const JVal* get(const std::string& k) const {
        auto it = o.find(k);
        return it == o.end() ? nullptr : &it->second;
    }
    std::string str(const std::string& k) const {
        auto v = get(k);
        return v && v->t == Str ? v->s : "";
    }
    bool flag(const std::string& k) const {
        auto v = get(k);
        return v && v->t == Bool && v->b;
    }
    bool has_num(const std::string& k) const {
        auto v = get(k);
        return v && v->t == Num;
    }
    uint64_t num(const std::string& k) const {
        auto v = get(k);
        return v && v->t == Num && v->n >= 0 && v->n < 9e15 ? uint64_t(v->n) : 0;
    }
};

void wipe_json(JVal& v) {
    if (!v.s.empty()) wipe(v.s.data(), v.s.size());
    for (auto& x : v.a) wipe_json(x);
    for (auto& [k, x] : v.o) wipe_json(x);
}

class JParser {
public:
    explicit JParser(const std::string& s) : s_(s) {}
    bool parse(JVal& out) {
        try {
            value(out, 0);
            ws();
            return i_ == s_.size();
        } catch (const std::exception&) {
            return false;
        }
    }

private:
    const std::string& s_;
    size_t i_ = 0;
    [[noreturn]] void fail() { throw std::runtime_error("json"); }
    void ws() { while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\n' || s_[i_] == '\r')) i_++; }
    char peek() { return i_ < s_.size() ? s_[i_] : '\0'; }
    bool lit(const char* w) {
        size_t n = std::strlen(w);
        if (s_.compare(i_, n, w) != 0) return false;
        i_ += n;
        return true;
    }
    void value(JVal& v, int depth) {
        if (depth > 6) fail();
        ws();
        char c = peek();
        if (c == '{') return object(v, depth);
        if (c == '[') return array(v, depth);
        if (c == '"') { v.t = JVal::Str; return string(v.s); }
        if (lit("true")) { v.t = JVal::Bool; v.b = true; return; }
        if (lit("false")) { v.t = JVal::Bool; return; }
        if (lit("null")) return;
        number(v);
    }
    void number(JVal& v) {
        size_t st = i_;
        while (i_ < s_.size() && (std::isdigit(uint8_t(s_[i_])) || s_[i_] == '-' || s_[i_] == '+' || s_[i_] == '.' || s_[i_] == 'e' || s_[i_] == 'E')) i_++;
        if (i_ == st || i_ - st > 24) fail();
        v.t = JVal::Num;
        v.n = std::stod(s_.substr(st, i_ - st));
    }
    void object(JVal& v, int depth) {
        v.t = JVal::Obj;
        i_++;
        ws();
        if (peek() == '}') { i_++; return; }
        for (;;) {
            ws();
            if (peek() != '"') fail();
            std::string k;
            string(k);
            ws();
            if (peek() != ':') fail();
            i_++;
            if (v.o.size() >= 32 || v.o.count(k)) fail();
            value(v.o[k], depth + 1);
            ws();
            if (peek() == ',') { i_++; continue; }
            if (peek() == '}') { i_++; return; }
            fail();
        }
    }
    void array(JVal& v, int depth) {
        v.t = JVal::Arr;
        i_++;
        ws();
        if (peek() == ']') { i_++; return; }
        for (;;) {
            if (v.a.size() >= 64) fail();
            v.a.emplace_back();
            value(v.a.back(), depth + 1);
            ws();
            if (peek() == ',') { i_++; continue; }
            if (peek() == ']') { i_++; return; }
            fail();
        }
    }
    unsigned hex4() {
        if (i_ + 4 > s_.size()) fail();
        unsigned v = 0;
        for (int k = 0; k < 4; k++) {
            char c = s_[i_++];
            v = v * 16 + (c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : (fail(), 0));
        }
        return v;
    }
    static void utf8(std::string& o, unsigned cp) {
        if (cp < 0x80) o += char(cp);
        else if (cp < 0x800) { o += char(0xC0 | cp >> 6); o += char(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { o += char(0xE0 | cp >> 12); o += char(0x80 | (cp >> 6 & 0x3F)); o += char(0x80 | (cp & 0x3F)); }
        else { o += char(0xF0 | cp >> 18); o += char(0x80 | (cp >> 12 & 0x3F)); o += char(0x80 | (cp >> 6 & 0x3F)); o += char(0x80 | (cp & 0x3F)); }
    }
    void string(std::string& out) {
        i_++;   // opening quote
        for (;;) {
            if (i_ >= s_.size()) fail();
            char c = s_[i_++];
            if (c == '"') return;
            if (uint8_t(c) < 0x20) fail();
            if (c != '\\') { out += c; continue; }
            if (i_ >= s_.size()) fail();
            char e = s_[i_++];
            switch (e) {
                case '"': case '\\': case '/': out += e; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    unsigned cp = hex4();
                    if (cp >= 0xD800 && cp < 0xDC00) {   // surrogate pair
                        if (s_.compare(i_, 2, "\\u") != 0) fail();
                        i_ += 2;
                        unsigned lo = hex4();
                        if (lo < 0xDC00 || lo > 0xDFFF) fail();
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    utf8(out, cp);
                    break;
                }
                default: fail();
            }
        }
    }
};

std::string jq(const std::string& s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += char(c); }
        else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += char(c);
    }
    return o + "\"";
}
std::string ju(uint64_t v) { return std::to_string(v); }
std::string jb(bool v) { return v ? "true" : "false"; }

// ---------------------------------------------------------------- HTTP

struct Req {
    std::string method, path, query, body;
    std::map<std::string, std::string> h;   // lower-case names
    std::string header(const std::string& k) const {
        auto it = h.find(k);
        return it == h.end() ? "" : it->second;
    }
};

struct Resp {
    int status = 200;
    std::string type = "application/json; charset=utf-8";
    std::string body;
    std::string location;
};

struct ApiError {
    int status;
    std::string msg, code;
};

const char* reason(int s) {
    switch (s) {
        case 200: return "OK";
        case 302: return "Found";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 409: return "Conflict";
        case 413: return "Payload Too Large";
        case 415: return "Unsupported Media Type";
        case 431: return "Request Header Fields Too Large";
        case 503: return "Service Unavailable";
        default: return "Error";
    }
}

Resp err_resp(int status, const std::string& msg, const std::string& code = "") {
    Resp r;
    r.status = status;
    r.body = "{\"error\":" + jq(msg) + (code.empty() ? "" : ",\"code\":" + jq(code)) + "}";
    return r;
}

std::string lower(std::string s) {
    for (auto& c : s) c = char(std::tolower(uint8_t(c)));
    return s;
}

/// Parses the request line and headers. Returns 0 or the HTTP status to answer with.
/// Duplicate headers, folded lines, control characters and Transfer-Encoding are refused (request smuggling).
int parse_head(const std::string& head, Req& r) {
    size_t eol = head.find("\r\n");
    std::string line = head.substr(0, eol);
    size_t s1 = line.find(' '), s2 = line.rfind(' ');
    if (s1 == std::string::npos || s2 == s1) return 400;
    r.method = line.substr(0, s1);
    std::string target = line.substr(s1 + 1, s2 - s1 - 1), ver = line.substr(s2 + 1);
    if (ver != "HTTP/1.1" && ver != "HTTP/1.0") return 400;
    if (target.empty() || target[0] != '/' || target.size() > 2048) return 400;
    for (unsigned char c : target) if (c <= 0x20 || c == 0x7f) return 400;
    size_t q = target.find('?');
    r.path = target.substr(0, q);
    r.query = q == std::string::npos ? "" : target.substr(q + 1);
    size_t pos = eol;
    while (pos != std::string::npos && pos + 2 < head.size()) {
        pos += 2;
        size_t e = head.find("\r\n", pos);
        std::string l = head.substr(pos, e == std::string::npos ? std::string::npos : e - pos);
        pos = e;
        if (l.empty()) break;
        size_t c = l.find(':');
        if (c == std::string::npos || c == 0 || l[0] == ' ' || l[0] == '\t') return 400;
        std::string name = lower(l.substr(0, c)), val = l.substr(c + 1);
        for (unsigned char ch : name) if (ch <= 0x20 || ch == 0x7f) return 400;
        for (unsigned char ch : val) if ((ch < 0x20 && ch != '\t') || ch == 0x7f) return 400;
        size_t a = val.find_first_not_of(" \t"), b = val.find_last_not_of(" \t");
        val = a == std::string::npos ? "" : val.substr(a, b - a + 1);
        if (r.h.count(name)) return 400;
        r.h[name] = val;
    }
    if (r.h.count("transfer-encoding")) return 400;
    return 0;
}

std::string hex_token(size_t bytes) {
    Bytes b(bytes);
    random_bytes(b.data(), b.size());
    return to_hex(b);
}

bool write_all(int fd, const std::string& s) {
    size_t off = 0;
    while (off < s.size()) {
        ssize_t w = ::send(fd, s.data() + off, s.size() - off, platform::SEND_FLAGS);
        if (w <= 0) return false;
        off += size_t(w);
    }
    return true;
}

void set_timeouts(int fd, int s) {
    timeval t{s, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &t, sizeof t);
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &t, sizeof t);
}

std::string opt_query(const std::string& q, const std::string& key) {
    size_t p = 0;
    while (p <= q.size()) {
        size_t e = q.find('&', p);
        std::string kv = q.substr(p, e == std::string::npos ? std::string::npos : e - p);
        if (kv.rfind(key + "=", 0) == 0) return kv.substr(key.size() + 1);
        if (e == std::string::npos) break;
        p = e + 1;
    }
    return "";
}

const char* SEC_HEADERS =
    "Content-Security-Policy: default-src 'self'; script-src 'self'; style-src 'self'; img-src 'self' data:; connect-src 'self'; "
    "object-src 'none'; base-uri 'none'; form-action 'none'; frame-ancestors 'none'\r\n"
    "X-Frame-Options: DENY\r\n"
    "Referrer-Policy: no-referrer\r\n"
    "Cache-Control: no-store\r\n"
    "X-Content-Type-Options: nosniff\r\n"
    "Cross-Origin-Opener-Policy: same-origin\r\n"
    "Cross-Origin-Resource-Policy: same-origin\r\n";

std::string render(const Resp& r) {
    std::string o = "HTTP/1.1 " + std::to_string(r.status) + " " + reason(r.status) + "\r\n" + SEC_HEADERS + "Content-Type: " + r.type +
                    "\r\nContent-Length: " + std::to_string(r.body.size()) + "\r\nConnection: close\r\n";
    if (!r.location.empty()) o += "Location: " + r.location + "\r\n";
    return o + "\r\n" + r.body;
}

}  // namespace

bool ct_equal(const std::string& a, const std::string& b) {
    uint8_t diff = a.size() == b.size() ? 0 : 1;
    size_t n = std::max(a.size(), b.size());
    for (size_t i = 0; i < n; i++) diff |= uint8_t((i < a.size() ? a[i] : 0) ^ (i < b.size() ? b[i] : 0));
    return diff == 0;
}

// ---------------------------------------------------------------- application

struct GuiServer::Impl {
    GuiConfig cfg;
    uint16_t port;
    const std::string& token;
    const ChainParams& p;

    std::mutex op_mu;   // serializes everything that opens the vault or changes the wallet files
    std::mutex pend_mu;
    struct Pending {
        WalletSecret secret;
        std::vector<int> challenge;
        double created = 0;
    } pending;
    std::mutex boot_mu;
    std::string boot_code;
    double boot_expiry = 0;

    Impl(const GuiConfig& c, uint16_t prt, const std::string& tok) : cfg(c), port(prt), token(tok), p(*c.params) {}

    void clear_pending() {   // caller holds pend_mu
        wipe(pending.secret.mnemonic.data(), pending.secret.mnemonic.size());
        pending.secret.mnemonic.clear();
        pending.challenge.clear();
    }

    std::string launch_code() {
        std::lock_guard<std::mutex> lk(boot_mu);
        boot_code = hex_token(16);
        boot_expiry = secs() + 60;
        return boot_code;
    }

    // ---- gatekeeping

    bool host_ok(const std::string& h) const {
        std::string ps = ":" + std::to_string(port);
        return h == "127.0.0.1" + ps || h == "localhost" + ps;
    }
    bool origin_ok(const std::string& o) const {
        std::string ps = ":" + std::to_string(port);
        return o == "http://127.0.0.1" + ps || o == "http://localhost" + ps;
    }
    bool authorized(const Req& r) const {
        std::string a = r.header("authorization");
        if (a.size() < 8 || a.compare(0, 7, "Bearer ") != 0) return false;
        return ct_equal(a.substr(7), token);
    }

    Resp handle(const Req& r) {
        try {
            if (!r.h.count("host") || !host_ok(r.header("host"))) return err_resp(403, "bad Host header");
            if (r.h.count("origin") && !origin_ok(r.header("origin"))) return err_resp(403, "bad Origin");
            if (r.header("sec-fetch-site") == "cross-site") return err_resp(403, "cross-site request");
            if (r.path.rfind("/api/", 0) == 0) return api(r);
            return page(r);
        } catch (const ApiError& e) {
            return err_resp(e.status, e.msg, e.code);
        } catch (const std::exception& e) {
            return err_resp(500, e.what());
        }
    }

    Resp page(const Req& r) {
        if (r.path == "/open") return open_redirect(r);
        if (r.method != "GET") return err_resp(405, "method not allowed");
        for (size_t i = 0; i < GUI_ASSET_COUNT; i++) {
            const GuiAsset& a = GUI_ASSETS[i];
            if (r.path != a.route && !(r.path == "/index.html" && std::strcmp(a.route, "/") == 0)) continue;
            Resp o;
            o.type = std::string(a.type) + "; charset=utf-8";
            o.body.assign(reinterpret_cast<const char*>(a.data), a.size);
            return o;
        }
        return err_resp(404, "not found");
    }

    /// One-time launch link: the browser opener gets this instead of the real token
    Resp open_redirect(const Req& r) {
        if (r.method != "GET") return err_resp(405, "method not allowed");
        std::lock_guard<std::mutex> lk(boot_mu);
        std::string c = opt_query(r.query, "c");
        bool ok = !boot_code.empty() && secs() < boot_expiry && ct_equal(c, boot_code);
        if (!ok) return err_resp(403, "launch link expired - use the URL printed by phylka gui");
        boot_code.clear();   // single use
        Resp o;
        o.status = 302;
        o.type = "text/plain; charset=utf-8";
        o.location = "/#t=" + token;
        return o;
    }

    Resp api(const Req& r) {
        if (!authorized(r)) return err_resp(401, "missing or wrong session token", "unauthorized");
        const std::string& m = r.method;
        const std::string& u = r.path;
        static const char* const GETS[] = {"/api/state", "/api/overview", "/api/history", "/api/stakes", "/api/check"};
        static const char* const POSTS[] = {"/api/wallet/generate", "/api/wallet/create", "/api/wallet/restore", "/api/unlock", "/api/address/new",
                                            "/api/tx/preview", "/api/tx/send", "/api/seed"};
        bool is_get = std::any_of(std::begin(GETS), std::end(GETS), [&](const char* x) { return u == x; });
        bool is_post = std::any_of(std::begin(POSTS), std::end(POSTS), [&](const char* x) { return u == x; });
        if (!is_get && !is_post) return err_resp(404, "unknown endpoint");
        if ((is_get && m != "GET") || (is_post && m != "POST")) return err_resp(405, "method not allowed");
        if (is_get) return get_route(r);
        return post_route(r);
    }

    Resp get_route(const Req& r) {
        if (r.path == "/api/state") return ok(state());
        if (r.path == "/api/overview") return ok(overview());
        if (r.path == "/api/history") return ok(history());
        if (r.path == "/api/stakes") return ok(stakes());
        return ok(check_address(opt_query(r.query, "addr")));
    }

    Resp post_route(const Req& r) {
        std::string ct = lower(r.header("content-type"));
        if (ct != "application/json" && ct.rfind("application/json;", 0) != 0) return err_resp(415, "Content-Type must be application/json");
        JVal b;
        Resp out;
        if (!JParser(r.body).parse(b) || b.t != JVal::Obj) { wipe_json(b); return err_resp(400, "invalid JSON body"); }
        try {
            out = post_dispatch(r.path, b);
        } catch (...) {
            wipe_json(b);
            throw;
        }
        wipe_json(b);
        return out;
    }

    Resp post_dispatch(const std::string& u, const JVal& b) {
        if (u == "/api/wallet/generate") return ok(generate());
        if (u == "/api/wallet/create") return ok(create(b));
        if (u == "/api/wallet/restore") return ok(restore(b));
        if (u == "/api/unlock") return ok(unlock(b));
        if (u == "/api/address/new") return ok(new_address(b));
        if (u == "/api/tx/preview") return ok(tx_preview(b));
        if (u == "/api/tx/send") return ok(tx_send(b));
        return ok(show_seed(b));
    }

    static Resp ok(const std::string& json) {
        Resp r;
        r.body = json;
        return r;
    }

    // ---- wallet access

    WalletFile wallet_file() const {
        WalletFile w;
        w.path = cfg.wallet;
        return w;
    }
    WalletFile existing_wallet() const {
        WalletFile w = wallet_file();
        if (!w.exists()) throw ApiError{409, "No wallet yet - create or restore one first.", "no_wallet"};
        w.load();
        return w;
    }
    std::vector<std::string> addresses_of(const WalletFile& w) const {
        auto a = wallet_addresses(w);
        if (a.size() < w.addresses) throw ApiError{409, "The address list next to the wallet is damaged. Run: phylka wallet newaddress", "bad_addr_cache"};
        return a;
    }

    /// Decrypts under the operation lock; a wrong password costs one second (on top of the key stretching)
    WalletSecret open_vault(const WalletFile& w, const std::string& password) {
        try {
            return vault::open(w.sealed, password);
        } catch (const std::exception&) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            throw ApiError{403, "Wrong password.", "wrong_password"};
        }
    }

    static void require_password(const JVal& b) {
        if (b.str("password").empty()) throw ApiError{400, "Enter your wallet password.", "no_password"};
    }

    // ---- node access

    template <class F>
    auto with_node(F f) -> decltype(f()) {
        try {
            return f();
        } catch (const std::runtime_error& e) {
            std::string m = e.what();
            if (m.rfind("node is not running", 0) == 0) throw ApiError{503, m, "node_down"};
            throw ApiError{502, m, "node_error"};
        }
    }

    std::map<std::string, std::string> node_info() {
        auto text = with_node([&] { return http_get(cfg.rpc, "/info"); });
        std::map<std::string, std::string> m;
        std::istringstream in(text);
        for (std::string line; std::getline(in, line);) {
            auto sp = line.find(' ');
            if (sp != std::string::npos) m[line.substr(0, sp)] = line.substr(sp + 1);
        }
        if (!m.count("height")) throw ApiError{502, "The node gave an unexpected reply on /info.", "node_error"};
        if (m["network"] != p.name)
            throw ApiError{409, "The node on this RPC port runs the '" + m["network"] + "' network but this wallet is for '" + p.name + "'.", "wrong_network"};
        return m;
    }

    std::string explorer_url() const { return "http://127.0.0.1:" + std::to_string(cfg.rpc) + "/"; }

    // ---- GET endpoints

    std::string state() {
        WalletFile w = wallet_file();
        std::ostringstream o;
        o << "{\"network\":" << jq(p.name) << ",\"hrp\":" << jq(p.hrp) << ",\"wallet_path\":" << jq(cfg.wallet.string()) << ",\"wallet_exists\":" << jb(w.exists())
          << ",\"rpc_port\":" << cfg.rpc << ",\"explorer_url\":" << jq(explorer_url()) << ",\"min_password\":" << MIN_PASSWORD_LEN
          << ",\"min_stake\":" << p.min_stake << ",\"stake_lock\":" << p.stake_lock << ",\"stake_activation\":" << p.stake_activation
          << ",\"coinbase_maturity\":" << p.coinbase_maturity << ",\"fee_rate\":" << WALLET_FEE_RATE << ",\"version\":\"0.1 beta\"}";
        return o.str();
    }

    std::string node_json(const std::map<std::string, std::string>& m) const {
        auto n = [&](const char* k) { auto it = m.find(k); return it == m.end() ? std::string("0") : it->second; };
        uint64_t tip_time = std::strtoull(n("time").c_str(), nullptr, 10);
        uint64_t now = uint64_t(std::time(nullptr));
        uint64_t age = now > tip_time ? now - tip_time : 0;
        uint64_t limit = std::max<uint64_t>(20ull * p.target_spacing, 900);
        std::ostringstream o;
        o << "{\"running\":true,\"network\":" << jq(p.name) << ",\"height\":" << n("height") << ",\"peers\":" << n("peers") << ",\"finalized\":" << n("finalized")
          << ",\"mempool\":" << n("mempool") << ",\"validators\":" << n("validators") << ",\"tip\":" << jq(m.count("tip") ? m.at("tip") : "")
          << ",\"tip_age\":" << age << ",\"synced\":" << jb(age < limit) << "}";
        return o.str();
    }

    std::string overview() {
        WalletFile w = wallet_file();
        std::ostringstream o;
        std::map<std::string, std::string> info;
        std::string node_part, err_code, err_msg;
        try {
            info = node_info();
            node_part = node_json(info);
        } catch (const ApiError& e) {
            err_code = e.code;
            err_msg = e.msg;
            node_part = "{\"running\":false,\"code\":" + jq(e.code) + ",\"message\":" + jq(e.msg) + "}";
        }
        o << "{\"node\":" << node_part << ",\"wallet\":";
        if (!w.exists()) return o.str() + "{\"exists\":false}}";
        w.load();
        auto addrs = addresses_of(w);
        o << "{\"exists\":true,\"address\":" << jq(addrs.back()) << ",\"addresses\":[";
        WalletBalance bal;
        bool have = err_code.empty();
        if (have) bal = wallet_balance(p, with_node([&] { return wallet_fetch_utxos(cfg.rpc, addrs); }));
        for (size_t i = 0; i < addrs.size(); i++)
            o << (i ? "," : "") << "{\"index\":" << i << ",\"address\":" << jq(addrs[i]) << ",\"balance\":" << (have ? ju(bal.per_address[uint32_t(i)]) : "null") << "}";
        o << "],\"balance\":";
        if (have)
            o << "{\"spendable\":" << bal.spendable << ",\"immature\":" << bal.immature << ",\"staked\":" << bal.staked << ",\"unlockable\":" << bal.unlockable << "}";
        else
            o << "null";
        return o.str() + "}}";
    }

    std::string history() {
        WalletFile w = existing_wallet();
        auto addrs = addresses_of(w);
        auto info = node_info();
        uint64_t height = std::strtoull(info["height"].c_str(), nullptr, 10);
        struct Row { std::string height, txid, kind; long long time = 0, delta = 0; };
        std::vector<Row> rows;
        std::map<std::string, size_t> by_tx;   // one row per transaction even if several of our addresses took part
        for (auto& a : addrs)
            for (auto& r : with_node([&] { return wallet_history(cfg.rpc, a); })) {
                auto it = by_tx.find(r.txid);
                if (it != by_tx.end()) { rows[it->second].delta += r.delta; continue; }
                by_tx[r.txid] = rows.size();
                rows.push_back(Row{r.height, r.txid, r.kind, r.time, r.delta});
            }
        std::stable_sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
            auto h = [](const Row& x) { return x.height == "pending" ? ~0ull : std::strtoull(x.height.c_str(), nullptr, 10); };
            return h(a) > h(b);
        });
        if (rows.size() > 300) rows.resize(300);
        std::ostringstream o;
        o << "{\"height\":" << height << ",\"explorer_url\":" << jq(explorer_url()) << ",\"rows\":[";
        for (size_t i = 0; i < rows.size(); i++) {
            const Row& r = rows[i];
            bool pend = r.height == "pending";
            uint64_t h = pend ? 0 : std::strtoull(r.height.c_str(), nullptr, 10);
            o << (i ? "," : "") << "{\"txid\":" << jq(r.txid) << ",\"pending\":" << jb(pend) << ",\"height\":" << h << ",\"time\":" << r.time
              << ",\"delta\":" << r.delta << ",\"mined\":" << jb(r.kind == "mined") << ",\"confirmations\":" << (pend || h > height ? 0 : height - h + 1) << "}";
        }
        return o.str() + "]}";
    }

    std::string stakes() {
        WalletFile w = existing_wallet();
        auto addrs = addresses_of(w);
        auto info = node_info();
        uint64_t tip = std::strtoull(info["height"].c_str(), nullptr, 10);
        auto all = with_node([&] { return wallet_fetch_utxos(cfg.rpc, addrs); });
        WalletBalance bal = wallet_balance(p, all);
        std::ostringstream o;
        o << "{\"min_stake\":" << p.min_stake << ",\"stake_lock\":" << p.stake_lock << ",\"stake_activation\":" << p.stake_activation
          << ",\"spendable\":" << bal.spendable << ",\"staked\":" << bal.staked << ",\"unlockable\":" << bal.unlockable << ",\"height\":" << tip << ",\"stakes\":[";
        bool first = true;
        for (auto& x : all) {
            if (x.version != ADDRESS_VERSION_STAKE) continue;
            uint64_t created = tip + 1 - x.conf;   // block that contains the stake output
            o << (first ? "" : ",") << "{\"txid\":" << jq(hash_hex(x.op.txid)) << ",\"vout\":" << x.op.vout << ",\"value\":" << x.value << ",\"confirmations\":" << x.conf
              << ",\"unlock_height\":" << created + p.stake_lock - 1 << ",\"unlockable\":" << jb(x.conf >= p.stake_lock)
              << ",\"blocks_left\":" << (x.conf >= p.stake_lock ? 0 : p.stake_lock - x.conf) << "}";
            first = false;
        }
        return o.str() + "]}";
    }

    std::string check_address(const std::string& a) const {
        auto addr = Address::parse(a, p.hrp);
        std::string why;
        if (!addr) why = "Not a valid " + p.name + " address (expected " + p.hrp + "1...). Check for typos and the network.";
        else if (addr->version == ADDRESS_VERSION_STAKE) why = "This is a stake address. Use the Staking page to lock coins.";
        return "{\"valid\":" + jb(why.empty()) + ",\"reason\":" + jq(why) + "}";
    }

    // ---- create / restore / unlock

    std::string generate() {
        std::lock_guard<std::mutex> lk(pend_mu);
        if (wallet_file().exists()) throw ApiError{409, "A wallet already exists at " + cfg.wallet.string(), "wallet_exists"};
        clear_pending();
        pending.secret.mnemonic = bip39::generate();
        pending.created = secs();
        while (pending.challenge.size() < 3) {   // three distinct random positions to prove the phrase was written down
            uint8_t r;
            random_bytes(&r, 1);
            int i = r % 24;
            if (std::find(pending.challenge.begin(), pending.challenge.end(), i) == pending.challenge.end()) pending.challenge.push_back(i);
        }
        std::sort(pending.challenge.begin(), pending.challenge.end());
        std::ostringstream o, words;
        std::istringstream in(pending.secret.mnemonic);
        std::string wd;
        bool first = true;
        while (in >> wd) { words << (first ? "" : ",") << jq(wd); first = false; }
        o << "{\"words\":[" << words.str() << "],\"challenge\":[";
        for (size_t i = 0; i < 3; i++) o << (i ? "," : "") << pending.challenge[i] + 1;
        return o.str() + "]}";
    }

    std::string wallet_made(WalletFile& w) const {
        return "{\"address\":" + jq(wallet_addresses(w)[0]) + "}";
    }

    std::string create(const JVal& b) {
        std::lock_guard<std::mutex> ol(op_mu);
        std::lock_guard<std::mutex> pl(pend_mu);
        WalletFile w = wallet_file();
        if (w.exists()) throw ApiError{409, "A wallet already exists at " + cfg.wallet.string(), "wallet_exists"};
        if (pending.secret.mnemonic.empty() || secs() - pending.created > 1800)
            throw ApiError{409, "The recovery phrase expired. Start again.", "expired"};
        const JVal* ans = b.get("answers");
        if (!ans || ans->t != JVal::Arr || ans->a.size() != 3) throw ApiError{400, "Enter the three requested words.", "bad_answers"};
        std::istringstream in(pending.secret.mnemonic);
        std::vector<std::string> words;
        for (std::string x; in >> x;) words.push_back(x);
        for (size_t i = 0; i < 3; i++)
            if (ans->a[i].t != JVal::Str || lower(ans->a[i].s) != words[size_t(pending.challenge[i])])
                throw ApiError{400, "Those words do not match. Check your paper copy - nothing was saved.", "bad_answers"};
        std::string pw = b.str("password");
        if (pw.size() < MIN_PASSWORD_LEN) throw ApiError{400, "The password must have at least 10 characters.", "weak_password"};
        wallet_create(w, p, pending.secret, pw);
        clear_pending();
        return wallet_made(w);
    }

    static std::string normalize_phrase(const std::string& in) {
        std::istringstream s(in);
        std::string out, wd;
        while (s >> wd) out += (out.empty() ? "" : " ") + lower(wd);
        return out;
    }

    std::string restore(const JVal& b) {
        std::lock_guard<std::mutex> ol(op_mu);
        WalletFile w = wallet_file();
        if (w.exists()) throw ApiError{409, "A wallet already exists at " + cfg.wallet.string(), "wallet_exists"};
        WalletSecret s;
        s.mnemonic = normalize_phrase(b.str("mnemonic"));
        s.passphrase = b.str("passphrase");
        if (!bip39::valid(s.mnemonic)) throw ApiError{400, "Invalid phrase: check the words and their order.", "bad_phrase"};
        std::string pw = b.str("password");
        if (pw.size() < MIN_PASSWORD_LEN) throw ApiError{400, "The password must have at least 10 characters.", "weak_password"};
        wallet_create(w, p, s, pw);
        return wallet_made(w);
    }

    std::string unlock(const JVal& b) {
        std::lock_guard<std::mutex> ol(op_mu);
        require_password(b);
        WalletFile w = existing_wallet();
        std::string pw = b.str("password");
        open_vault(w, pw);   // throws on a wrong password
        return "{\"ok\":true}";
    }

    std::string new_address(const JVal& b) {
        std::lock_guard<std::mutex> ol(op_mu);
        require_password(b);
        WalletFile w = existing_wallet();
        WalletSecret s = open_vault(w, b.str("password"));
        return "{\"address\":" + jq(wallet_new_address(w, p, s)) + "}";
    }

    std::string show_seed(const JVal& b) {
        std::lock_guard<std::mutex> ol(op_mu);
        require_password(b);
        WalletFile w = existing_wallet();
        WalletSecret s = open_vault(w, b.str("password"));
        std::string out = "{\"mnemonic\":" + jq(s.mnemonic) + ",\"passphrase_set\":" + jb(!s.passphrase.empty()) + "}";
        return out;
    }

    // ---- payments

    struct Planned {
        std::string kind;
        Address to;
        PaymentPlan plan;
    };

    Planned plan_tx(const JVal& b, const std::vector<std::string>& addrs, const std::vector<WalletUtxo>& all) {
        Planned pl;
        pl.kind = b.str("kind");
        bool max = b.flag("max");
        if (pl.kind == "send") {
            auto to = Address::parse(b.str("to"), p.hrp);
            if (!to) throw ApiError{400, "Invalid address or wrong network (expected " + p.hrp + "1...).", "bad_address"};
            if (to->version != ADDRESS_VERSION_HYBRID) throw ApiError{400, "This is a stake address. Use the Staking page to lock coins.", "bad_address"};
            pl.to = *to;
            uint64_t amount = max ? 0 : amount_of(b);
            pl.plan = wallet_plan_payment(wallet_spendable(all), amount, max, WALLET_FEE_RATE, wallet_sigv2_active(cfg.rpc));
        } else if (pl.kind == "stake") {
            uint64_t amount = amount_of(b);
            if (amount < p.min_stake) throw ApiError{400, "The minimum stake is " + format_plk(int64_t(p.min_stake)) + ".", "bad_amount"};
            pl.to = Address{ADDRESS_VERSION_STAKE, Address::parse(addrs[0], p.hrp)->program};
            pl.plan = wallet_plan_payment(wallet_spendable(all), amount, false, WALLET_FEE_RATE, wallet_sigv2_active(cfg.rpc));
        } else if (pl.kind == "unstake") {
            auto v = wallet_unlockable_stakes(p, all);
            if (v.empty()) throw ApiError{400, "No stakes whose lock period has expired.", "nothing_to_unstake"};
            pl.to = *Address::parse(addrs[0], p.hrp);
            pl.plan = wallet_plan_payment(v, 0, true, WALLET_FEE_RATE, wallet_sigv2_active(cfg.rpc));
        } else {
            throw ApiError{400, "unknown transaction kind", "bad_kind"};
        }
        return pl;
    }

    static uint64_t amount_of(const JVal& b) {
        try {
            return parse_amount(b.str("amount"));
        } catch (const std::exception& e) {
            throw ApiError{400, e.what(), "bad_amount"};
        }
    }

    Planned plan_from_node(const JVal& b, const WalletFile& w) {
        auto addrs = addresses_of(w);
        node_info();   // also verifies the network
        auto all = with_node([&] { return wallet_fetch_utxos(cfg.rpc, addrs); });
        try {
            return plan_tx(b, addrs, all);
        } catch (const std::runtime_error& e) {   // insufficient funds etc.
            throw ApiError{400, e.what(), "insufficient"};
        }
    }

    std::string tx_preview(const JVal& b) {
        WalletFile w = existing_wallet();
        Planned pl = plan_from_node(b, w);
        std::ostringstream o;
        o << "{\"kind\":" << jq(pl.kind) << ",\"to\":" << jq(pl.to.str(p.hrp)) << ",\"amount\":" << pl.plan.amount << ",\"fee\":" << pl.plan.fee
          << ",\"total\":" << pl.plan.amount + pl.plan.fee << ",\"inputs\":" << pl.plan.use.size() << ",\"input_total\":" << pl.plan.in_sum << "}";
        return o.str();
    }

    std::string tx_send(const JVal& b) {
        std::lock_guard<std::mutex> ol(op_mu);   // one payment at a time: no two requests can pick the same coins
        require_password(b);
        WalletFile w = existing_wallet();
        Planned pl = plan_from_node(b, w);
        if (!b.has_num("expect_amount") || !b.has_num("expect_fee") || b.num("expect_amount") != pl.plan.amount || b.num("expect_fee") != pl.plan.fee)
            throw ApiError{409, "The amount or fee changed since the preview. Review the transaction again.", "changed"};
        WalletSecret s = open_vault(w, b.str("password"));
        Transaction t = wallet_sign_payment(p, s, addresses_of(w), pl.plan, pl.to);
        std::string txid = with_node([&] { return wallet_submit(cfg.rpc, t); });
        return "{\"txid\":" + jq(txid) + ",\"explorer_url\":" + jq(explorer_url()) + "}";
    }
};

// ---------------------------------------------------------------- server

GuiServer::GuiServer(GuiConfig cfg) : cfg_(std::move(cfg)) {
    if (!cfg_.params) throw std::runtime_error("GuiServer: no network parameters");
    token_ = hex_token(32);
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) throw std::runtime_error("cannot create socket");
    int one = 1;
    ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    platform::set_nosigpipe(fd_);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(cfg_.port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   // never reachable from other machines
    if (::bind(fd_, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || ::listen(fd_, 16) != 0) {
        platform::close_socket(fd_);
        throw std::runtime_error("GUI: port " + std::to_string(cfg_.port) + " in use (choose another with --port)");
    }
    socklen_t len = sizeof a;
    ::getsockname(fd_, reinterpret_cast<sockaddr*>(&a), &len);
    port_ = ntohs(a.sin_port);
    impl_ = std::make_unique<Impl>(cfg_, port_, token_);
    th_ = std::thread([this] { loop(); });
}

GuiServer::~GuiServer() {
    stop_ = true;
    if (th_.joinable()) th_.join();
    for (auto& w : workers_) ::shutdown(w->fd, SHUT_RDWR);
    reap(true);
    platform::close_socket(fd_);
}

std::string GuiServer::bound_address() const {
    sockaddr_in a{};
    socklen_t len = sizeof a;
    if (::getsockname(fd_, reinterpret_cast<sockaddr*>(&a), &len) != 0) return "";
    char b[INET_ADDRSTRLEN] = {0};
    ::inet_ntop(AF_INET, &a.sin_addr, b, sizeof b);
    return b;
}

std::string GuiServer::url() const { return "http://127.0.0.1:" + std::to_string(port_) + "/#t=" + token_; }

std::string GuiServer::launch_url() { return "http://127.0.0.1:" + std::to_string(port_) + "/open?c=" + impl_->launch_code(); }

void GuiServer::reap(bool all) {
    for (auto it = workers_.begin(); it != workers_.end();) {
        if (!all && !(*it)->done) { ++it; continue; }
        if ((*it)->th.joinable()) (*it)->th.join();
        platform::close_socket((*it)->fd);
        it = workers_.erase(it);
    }
}

void GuiServer::loop() {
    while (!stop_) {
        reap(false);
        pollfd pf{fd_, POLLIN, 0};
        if (::poll(&pf, 1, 200) <= 0) continue;
        int c = ::accept(fd_, nullptr, nullptr);
        if (c < 0) continue;
        if (workers_.size() >= MAX_WORKERS) { platform::close_socket(c); continue; }   // too busy: refuse
        platform::set_nosigpipe(c);
        set_timeouts(c, std::min(5, cfg_.request_timeout_s));
        auto w = std::make_unique<Worker>();
        w->fd = c;
        Worker* raw = w.get();
        w->th = std::thread([this, raw] {
            serve(raw->fd);
            raw->done = true;
        });
        workers_.push_back(std::move(w));
    }
}

namespace {

enum class Read { Ok, Reply, Drop };

/// Reads one request (headers + body) within `timeout_s`. Reply = answer `err` and close; Drop = just close.
Read read_request(int c, int timeout_s, const std::atomic<bool>& stop, Req& req, Resp& err) {
    std::string in;
    char buf[4096];
    double deadline = secs() + timeout_s;   // the whole request, not just one recv
    size_t head_end = std::string::npos;
    while (head_end == std::string::npos && in.size() <= MAX_HEAD && secs() < deadline && !stop) {
        ssize_t r = ::recv(c, buf, sizeof buf, 0);
        if (r <= 0) return Read::Drop;   // closed, timed out or errored
        in.append(buf, size_t(r));
        head_end = in.find("\r\n\r\n");
    }
    if (head_end == std::string::npos || head_end > MAX_HEAD) {
        if (head_end == std::string::npos && in.size() <= MAX_HEAD) return Read::Drop;
        err = err_resp(431, "request headers too large");
        return Read::Reply;
    }
    if (int bad = parse_head(in.substr(0, head_end + 2), req)) { err = err_resp(bad, "malformed request"); return Read::Reply; }
    size_t want = 0;
    auto cl = req.h.find("content-length");
    if (cl != req.h.end()) {
        if (cl->second.empty() || cl->second.size() > 9 || cl->second.find_first_not_of("0123456789") != std::string::npos) { err = err_resp(400, "bad Content-Length"); return Read::Reply; }
        want = std::stoul(cl->second);
        if (want > MAX_BODY) { err = err_resp(413, "request body too large"); return Read::Reply; }
    }
    req.body = in.substr(head_end + 4);
    wipe(in.data(), in.size());
    while (req.body.size() < want && secs() < deadline && !stop) {
        ssize_t r = ::recv(c, buf, std::min(sizeof buf, want - req.body.size()), 0);
        if (r <= 0) return Read::Drop;
        req.body.append(buf, size_t(r));
    }
    wipe(buf, sizeof buf);
    if (req.body.size() != want) { err = err_resp(400, "request body incomplete"); return Read::Reply; }
    return Read::Ok;
}

/// Half-close, then swallow whatever the client is still sending (briefly), so the answer is not lost to a TCP reset
void finish(int c) {
    ::shutdown(c, SHUT_WR);
    timeval t{0, 200000};
    ::setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &t, sizeof t);
    char buf[4096];
    for (size_t total = 0; total < 256 * 1024;) {
        ssize_t r = ::recv(c, buf, sizeof buf, 0);
        if (r <= 0) break;
        total += size_t(r);
    }
}

}  // namespace

void GuiServer::serve(int c) {
    Req req;
    Resp resp;
    Read st = read_request(c, cfg_.request_timeout_s, stop_, req, resp);
    if (st == Read::Drop) { wipe(req.body.data(), req.body.size()); return; }
    if (st == Read::Ok) resp = impl_->handle(req);
    wipe(req.body.data(), req.body.size());
    std::string out = render(resp);
    wipe(resp.body.data(), resp.body.size());   // may carry the recovery phrase
    write_all(c, out);
    wipe(out.data(), out.size());
    finish(c);
}

// ---------------------------------------------------------------- command

namespace {
std::atomic<bool> g_gui_stop{false};
void on_signal(int) { g_gui_stop = true; }

void launch_browser(const std::string& url) {
#if defined(__APPLE__)
    const char* tool = "open";
#else
    const char* tool = "xdg-open";
#endif
    pid_t pid = ::fork();
    if (pid != 0) {
        if (pid > 0) std::thread([pid] { int st; ::waitpid(pid, &st, 0); }).detach();
        return;
    }
    if (!freopen("/dev/null", "w", stdout) || !freopen("/dev/null", "w", stderr)) ::_exit(127);
    ::execlp(tool, tool, url.c_str(), static_cast<char*>(nullptr));
    ::_exit(127);
}
}  // namespace

int gui_command(const GuiConfig& cfg, bool open_browser) {
    try {
        GuiServer srv(cfg);
        std::cout << "PHYLKA GUI wallet (" << cfg.params->name << ")\n"
                  << "  Open:   " << srv.url() << "\n"
                  << "  Wallet: " << cfg.wallet.string() << (fs::exists(cfg.wallet) ? "" : "  (not created yet)") << "\n"
                  << "  Node:   RPC 127.0.0.1:" << cfg.rpc << "  (start it with: phylka node" << (cfg.params->name == "dev" ? " --dev" : "") << " ...)\n"
                  << "Listening on 127.0.0.1 only. The link contains a secret session token - do not share it. Press Ctrl+C to stop." << std::endl;
        if (open_browser) launch_browser(srv.launch_url());
        std::signal(SIGINT, on_signal);
        std::signal(SIGTERM, on_signal);
        while (!g_gui_stop) std::this_thread::sleep_for(std::chrono::milliseconds(200));
        std::cout << "Stopping." << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
}

// ---------------------------------------------------------------- selftest

namespace {

/// Sends raw bytes to 127.0.0.1:port and returns everything the server answers until it closes the connection
std::string raw_http(uint16_t port, const std::string& request, int wait_s = 5) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) { ::close(fd); return ""; }
    set_timeouts(fd, wait_s);
    write_all(fd, request);
    std::string resp;
    char buf[4096];
    for (ssize_t r; (r = ::recv(fd, buf, sizeof buf, 0)) > 0;) resp.append(buf, size_t(r));
    ::close(fd);
    return resp;
}

struct Http {
    uint16_t port;
    std::string token;
    std::string host_default() const { return "127.0.0.1:" + std::to_string(port); }
    /// extra: full header lines ("Name: value\r\n"); host/auth added unless suppressed through the flags
    std::string call(const std::string& method, const std::string& path, const std::string& body = "", const std::string& extra = "",
                     const std::string& host = "", bool auth = true, const std::string& ctype = "application/json") const {
        std::string r = method + " " + path + " HTTP/1.1\r\nHost: " + (host.empty() ? host_default() : host) + "\r\n";
        if (auth) r += "Authorization: Bearer " + token + "\r\n";
        if (!body.empty() || method == "POST") r += "Content-Type: " + ctype + "\r\nContent-Length: " + std::to_string(body.size()) + "\r\n";
        return raw_http(port, r + extra + "\r\n" + body);
    }
    static int status(const std::string& resp) { return resp.size() > 12 ? std::atoi(resp.c_str() + 9) : 0; }
    static std::string body(const std::string& resp) {
        auto p = resp.find("\r\n\r\n");
        return p == std::string::npos ? "" : resp.substr(p + 4);
    }
    static bool has_header(const std::string& resp, const std::string& name) {
        std::string head = lower(resp.substr(0, resp.find("\r\n\r\n")));
        return head.find("\r\n" + lower(name) + ":") != std::string::npos;
    }
};

std::string field_of(const std::string& json, const std::string& key) {
    JVal v;
    JParser(json).parse(v);
    return v.str(key);
}

}  // namespace

void gui_selftest(const std::function<void(bool, const std::string&)>& check) {
    const ChainParams& dev = ChainParams::dev();
    fs::path dir = fs::temp_directory_path() / ("phylka-selftest-gui-" + std::to_string(::getpid()));
    fs::remove_all(dir);
    fs::create_directories(dir);

    std::cout << "GUI wallet: request gate\n";
    check(ct_equal("abc", "abc") && !ct_equal("abc", "abd") && !ct_equal("abc", "abcd") && !ct_equal("", "a") && ct_equal("", ""), "constant-time compare gives the right answers");
    {
        JVal v;
        check(JParser(R"({"a":"xé😀","n":12,"b":true,"l":["p","q"]})").parse(v) && v.str("a") == "x\xC3\xA9\xF0\x9F\x98\x80" && v.num("n") == 12 && v.flag("b") &&
                  v.get("l")->a.size() == 2,
              "JSON parser: escapes, surrogate pairs, arrays");
        JVal bad;
        check(!JParser("{\"a\":1,\"a\":2}").parse(bad) && !JParser("{\"a\":").parse(bad) && !JParser("[1,2]x").parse(bad) && !JParser("{\"a\":\"\x01\"}").parse(bad),
              "JSON parser refuses duplicate keys, truncated input, trailing garbage and control characters");
    }

    GuiConfig cfg;
    cfg.params = &dev;
    cfg.wallet = dir / "gui.wallet";
    {   // an RPC port nobody listens on: the gate must work without a node, and node errors must be reported cleanly
        int t = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ::bind(t, reinterpret_cast<sockaddr*>(&a), sizeof a);
        socklen_t l = sizeof a;
        ::getsockname(t, reinterpret_cast<sockaddr*>(&a), &l);
        cfg.rpc = ntohs(a.sin_port);
        ::close(t);
    }
    cfg.request_timeout_s = 2;
    GuiServer srv(cfg);
    GuiServer srv2(cfg);
    Http h{srv.port(), srv.token()};

    check(srv.token().size() == 64 && srv.token().find_first_not_of("0123456789abcdef") == std::string::npos && srv.token() != srv2.token(),
          "session token: 256 random bits as hex, different per start");
    check(srv.bound_address() == "127.0.0.1", "listening socket is bound to 127.0.0.1 only");
    {
        int probe = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in t{};
        t.sin_family = AF_INET;
        t.sin_port = htons(srv.port());
        t.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        check(::connect(probe, reinterpret_cast<sockaddr*>(&t), sizeof t) == 0, "server accepts connections on the loopback address");
        ::close(probe);
    }

    std::cout << "GUI wallet: headers, tokens, Host/Origin, methods\n";
    std::string page = h.call("GET", "/", "", "", "", false);
    check(Http::status(page) == 200 && Http::body(page).find("<html") != std::string::npos, "GET / serves the embedded page without a token");
    check(Http::has_header(page, "Content-Security-Policy") && page.find("default-src 'self'") != std::string::npos && page.find("frame-ancestors 'none'") != std::string::npos,
          "Content-Security-Policy: default-src 'self', no framing");
    check(page.find("X-Frame-Options: DENY") != std::string::npos && page.find("Referrer-Policy: no-referrer") != std::string::npos &&
              page.find("Cache-Control: no-store") != std::string::npos && page.find("X-Content-Type-Options: nosniff") != std::string::npos,
          "X-Frame-Options DENY, Referrer-Policy no-referrer, Cache-Control no-store, nosniff");
    check(!Http::has_header(page, "Access-Control-Allow-Origin") && !Http::has_header(page, "Access-Control-Allow-Methods"), "no CORS headers");
    {
        std::string html = Http::body(page), js = Http::body(h.call("GET", "/app.js", "", "", "", false)), css = Http::body(h.call("GET", "/app.css", "", "", "", false));
        bool inline_script = false;
        for (size_t p = html.find("<script"); p != std::string::npos; p = html.find("<script", p + 1))
            if (html.compare(p, html.find('>', p) - p + 1, "<script>") == 0 || html.substr(p, html.find('>', p) - p).find("src=") == std::string::npos) inline_script = true;
        check(!js.empty() && !css.empty() && !inline_script && html.find(" style=") == std::string::npos && html.find(" onclick=") == std::string::npos,
              "page uses separate /app.js and /app.css, no inline script, style or event handler (CSP can forbid them)");
        const std::string disclaimer = "https://github.com/a9ish1/PHYLKA/blob/main/DISCLAIMER.md";   // the only URL: a link the user may click, never fetched
        for (size_t p = js.find(disclaimer); p != std::string::npos; p = js.find(disclaimer)) js.erase(p, disclaimer.size());
        check(html.find("http://") == std::string::npos && html.find("https://") == std::string::npos && js.find("http://") == std::string::npos &&
                  js.find("https://") == std::string::npos && css.find("url(") == std::string::npos && js.find("eval(") == std::string::npos &&
                  js.find("new Function") == std::string::npos && js.find("innerHTML") == std::string::npos && js.find("document.write") == std::string::npos,
              "no external URLs, no eval/Function/innerHTML/document.write in the app");
        check(html.size() + js.size() + css.size() < 150 * 1024, "embedded assets stay under 150 KB");
    }
    check(Http::status(h.call("GET", "/api/state", "", "", "", false)) == 401, "API without token -> 401");
    {
        Http wrong{srv.port(), std::string(64, 'a')};
        check(Http::status(wrong.call("GET", "/api/state")) == 401, "API with a wrong token -> 401");
        Http shorter{srv.port(), srv.token().substr(0, 63)};
        check(Http::status(shorter.call("GET", "/api/state")) == 401, "API with a truncated token -> 401");
        check(Http::status(h.call("GET", "/api/state", "", "Authorization: Basic " + srv.token() + "\r\n", "", false)) == 401, "API with another auth scheme -> 401");
        check(Http::status(h.call("POST", "/api/seed", "{}", "", "", false)) == 401, "POST without token -> 401");
    }
    std::string st = h.call("GET", "/api/state");
    check(Http::status(st) == 200 && field_of(Http::body(st), "network") == "dev", "API with the right token -> 200");
    check(Http::status(h.call("GET", "/api/state", "", "", "evil.example.com")) == 403, "Host: evil.example.com -> 403 (DNS rebinding)");
    check(Http::status(h.call("GET", "/", "", "", "evil.example.com", false)) == 403, "Host check applies to the page too");
    check(Http::status(h.call("GET", "/api/state", "", "", "127.0.0.1")) == 403 && Http::status(h.call("GET", "/api/state", "", "", "127.0.0.1:1")) == 403 &&
              Http::status(h.call("GET", "/api/state", "", "", "localhost:" + std::to_string(srv.port()) + ".evil.com")) == 403,
          "Host without or with another port, or with a suffix -> 403");
    check(Http::status(h.call("GET", "/api/state", "", "", "localhost:" + std::to_string(srv.port()))) == 200, "Host: localhost:PORT is accepted");
    std::string port_s = std::to_string(srv.port());
    check(Http::status(h.call("GET", "/api/state", "", "Origin: http://evil.example.com\r\n")) == 403 && Http::status(h.call("GET", "/api/state", "", "Origin: null\r\n")) == 403 &&
              Http::status(h.call("GET", "/api/state", "", "Origin: https://127.0.0.1:" + port_s + "\r\n")) == 403 &&
              Http::status(h.call("GET", "/api/state", "", "Origin: http://127.0.0.1:" + port_s + ".evil.com\r\n")) == 403,
          "foreign, null, https and look-alike Origin -> 403 (CSRF)");
    check(Http::status(h.call("GET", "/api/state", "", "Origin: http://127.0.0.1:" + port_s + "\r\n")) == 200 &&
              Http::status(h.call("GET", "/api/state", "", "Origin: http://localhost:" + port_s + "\r\n")) == 200,
          "own Origin is accepted");
    check(Http::status(h.call("GET", "/api/state", "", "Sec-Fetch-Site: cross-site\r\n")) == 403, "Sec-Fetch-Site: cross-site -> 403");
    check(Http::status(h.call("POST", "/api/tx/send", "{}", "Origin: http://evil.example.com\r\n")) == 403, "state-changing POST from a foreign Origin -> 403");
    check(Http::status(h.call("GET", "/api/seed")) == 405 && Http::status(h.call("GET", "/api/tx/send")) == 405 && Http::status(h.call("GET", "/api/wallet/create")) == 405,
          "GET on a POST endpoint -> 405");
    check(Http::status(h.call("POST", "/api/overview", "{}")) == 405 && Http::status(h.call("PUT", "/api/seed", "{}")) == 405 && Http::status(h.call("DELETE", "/api/state")) == 405,
          "POST/PUT/DELETE on a read endpoint -> 405");
    {
        std::string pre = h.call("OPTIONS", "/api/seed", "", "Origin: http://evil.example.com\r\nAccess-Control-Request-Method: POST\r\n");
        check(Http::status(pre) == 403 && !Http::has_header(pre, "Access-Control-Allow-Origin"), "CORS preflight is refused and carries no CORS headers");
        std::string pre2 = h.call("OPTIONS", "/api/seed", "", "");
        check(Http::status(pre2) == 405 && !Http::has_header(pre2, "Access-Control-Allow-Origin"), "OPTIONS from the own origin is not answered with CORS headers either");
    }
    check(Http::status(h.call("POST", "/api/seed", "{\"password\":\"x\"}", "", "", true, "text/plain")) == 415 &&
              Http::status(h.call("POST", "/api/seed", "password=x", "", "", true, "application/x-www-form-urlencoded")) == 415,
          "POST needs Content-Type: application/json (form and text/plain posts -> 415)");
    check(Http::status(h.call("POST", "/api/seed", "not json")) == 400 && Http::status(h.call("POST", "/api/seed", "[1]")) == 400, "POST with invalid JSON -> 400");
    check(Http::status(raw_http(srv.port(), "POST /api/seed HTTP/1.1\r\nHost: " + h.host_default() + "\r\nAuthorization: Bearer " + srv.token() +
                                                "\r\nContent-Type: application/json\r\nContent-Length: 100000\r\n\r\n")) == 413,
          "oversized body is refused before it is read (413)");
    check(Http::status(raw_http(srv.port(), "GET /api/state HTTP/1.1\r\nHost: " + h.host_default() + "\r\nHost: localhost:" + port_s + "\r\n\r\n")) == 400 &&
              Http::status(raw_http(srv.port(), "POST /api/seed HTTP/1.1\r\nHost: " + h.host_default() + "\r\nTransfer-Encoding: chunked\r\n\r\n")) == 400,
          "duplicate headers and Transfer-Encoding are refused (request smuggling)");
    check(Http::status(raw_http(srv.port(), "GET /" + std::string(5000, 'a') + " HTTP/1.1\r\nHost: " + h.host_default() + "\r\n\r\n")) == 400 &&
              Http::status(raw_http(srv.port(), std::string("GET / HTTP/1.1\r\nHost: ") + h.host_default() + "\r\n" + std::string(20000, 'x') + "\r\n\r\n")) == 431,
          "oversized request line or headers are refused");
    {
        double t0 = secs();
        std::string r = raw_http(srv.port(), "GET / HTTP/1.1\r\nHost: " + h.host_default() + "\r\n", 6);   // never finishes the headers
        check(r.empty() && secs() - t0 < 5, "a stalled request is dropped after the timeout");
    }
    {
        std::string a = h.call("GET", "/nope", "", "", "", false), b = h.call("GET", "/api/nope");
        check(Http::status(a) == 404 && Http::status(b) == 404 && Http::status(h.call("GET", "/api/nope", "", "", "", false)) == 401, "unknown paths -> 404 (and unknown API paths without token -> 401)");
    }

    std::cout << "GUI wallet: launch link\n";
    {
        std::string lu = srv.launch_url();
        std::string path = lu.substr(lu.find('/', 8));
        check(lu.find(srv.token()) == std::string::npos, "browser launch link does not contain the session token");
        std::string r = h.call("GET", path, "", "", "", false);
        check(Http::status(r) == 302 && r.find("Location: /#t=" + srv.token()) != std::string::npos, "launch link redirects to the fragment URL with the token");
        check(Http::status(h.call("GET", path, "", "", "", false)) == 403, "launch link works only once");
        check(Http::status(h.call("GET", "/open?c=00", "", "", "", false)) == 403, "wrong launch code -> 403");
    }

    std::cout << "GUI wallet: wallet creation, password, seed\n";
    std::string gen = Http::body(h.call("POST", "/api/wallet/generate", "{}"));
    JVal gv;
    JParser(gen).parse(gv);
    std::string phrase;
    for (auto& w : gv.get("words")->a) phrase += (phrase.empty() ? "" : " ") + w.s;
    std::vector<int> ch;
    for (auto& c : gv.get("challenge")->a) ch.push_back(int(c.n));
    check(gv.get("words")->a.size() == 24 && bip39::valid(phrase) && ch.size() == 3 && ch[0] < ch[1] && ch[1] < ch[2] && ch[0] >= 1 && ch[2] <= 24, "generate: 24 valid words and three distinct positions to confirm");
    auto word_at = [&](int i) { return gv.get("words")->a[size_t(i - 1)].s; };
    std::string right = "[" + jq(word_at(ch[0])) + "," + jq(word_at(ch[1])) + "," + jq(word_at(ch[2])) + "]";
    std::string wrong = "[" + jq(word_at(ch[0])) + "," + jq(word_at(ch[1])) + ",\"zoo\"]";
    check(Http::status(h.call("POST", "/api/wallet/create", "{\"password\":\"long enough pw\",\"answers\":" + wrong + "}")) == 400 && !fs::exists(cfg.wallet),
          "create with a wrong confirmation word saves nothing");
    check(Http::status(h.call("POST", "/api/wallet/create", "{\"password\":\"short\",\"answers\":" + right + "}")) == 400 && !fs::exists(cfg.wallet),
          "create with a password under 10 characters saves nothing");
    std::ostringstream captured;
    auto* old_buf = std::cout.rdbuf(captured.rdbuf());   // nothing sensitive may reach the log
    std::string made = h.call("POST", "/api/wallet/create", "{\"password\":\"correct horse battery\",\"answers\":" + right + "}");
    std::string again = h.call("POST", "/api/wallet/create", "{\"password\":\"correct horse battery\",\"answers\":" + right + "}");
    std::string bad_seed = h.call("POST", "/api/seed", "{\"password\":\"wrong password!!\"}");
    std::string good_seed = h.call("POST", "/api/seed", "{\"password\":\"correct horse battery\"}");
    std::string bad_new = h.call("POST", "/api/address/new", "{\"password\":\"wrong password!!\"}");
    std::string bad_unlock = h.call("POST", "/api/unlock", "{\"password\":\"wrong password!!\"}");
    std::string ok_unlock = h.call("POST", "/api/unlock", "{\"password\":\"correct horse battery\"}");
    std::cout.rdbuf(old_buf);
    check(Http::status(made) == 200 && fs::exists(cfg.wallet), "create with the right words and a good password writes the wallet");
    check(Http::status(again) == 409, "a second create is refused (wallet exists)");
    check(Http::status(bad_seed) == 403 && Http::body(bad_seed).find("Wrong password") != std::string::npos && Http::body(bad_seed).find(phrase.substr(0, 8)) == std::string::npos,
          "show seed with a wrong password -> 403, nothing leaked");
    check(Http::status(good_seed) == 200 && field_of(Http::body(good_seed), "mnemonic") == phrase, "show seed with the right password returns the phrase");
    check(Http::status(bad_new) == 403 && Http::status(bad_unlock) == 403 && Http::status(ok_unlock) == 200, "new address and unlock verify the password");
    {
        std::string log = captured.str();
        check(log.find("correct horse") == std::string::npos && log.find("wrong password") == std::string::npos && log.find(phrase.substr(0, 12)) == std::string::npos,
              "nothing secret (password, phrase) is ever printed");
    }
    {
        WalletFile w;
        w.path = cfg.wallet;
        w.load();
        WalletSecret s = vault::open(w.sealed, "correct horse battery");
        struct stat sb {};
        ::stat(cfg.wallet.c_str(), &sb);
        check(s.mnemonic == phrase && (sb.st_mode & 0777) == 0600, "wallet file opens with the password and has mode 0600");
        check(std::search(w.sealed.begin(), w.sealed.end(), phrase.begin(), phrase.begin() + 8) == w.sealed.end(), "recovery phrase is not stored in plain text");
    }
    {
        std::string ov = h.call("GET", "/api/overview");
        JVal o;
        JParser(Http::body(ov)).parse(o);
        const JVal* node = o.get("node");
        check(Http::status(ov) == 200 && node && node->get("running") && !node->get("running")->b && node->str("code") == "node_down", "node not running is reported as a clear status, not an error page");
        check(Http::status(h.call("GET", "/api/history")) == 503 && Http::status(h.call("POST", "/api/tx/preview", "{\"kind\":\"send\",\"to\":\"x\",\"amount\":\"1\"}")) == 503,
              "history and payments say 503 while the node is down");
        check(Http::body(h.call("GET", "/api/check?addr=" + std::string("plk1notanaddress"))).find("\"valid\":false") != std::string::npos, "address check rejects garbage");
        std::string mine = field_of(Http::body(ov), "address");
        JVal ow;
        JParser(Http::body(ov)).parse(ow);
        mine = ow.get("wallet")->str("address");
        check(mine.rfind("plkd1", 0) == 0 && Http::body(h.call("GET", "/api/check?addr=" + mine)).find("\"valid\":true") != std::string::npos, "own dev address validates");
        Address stake{ADDRESS_VERSION_STAKE, Address::parse(mine, dev.hrp)->program};
        check(Http::body(h.call("GET", "/api/check?addr=" + stake.str(dev.hrp))).find("\"valid\":false") != std::string::npos, "stake addresses are refused as payment targets");
        Address main_addr{ADDRESS_VERSION_HYBRID, Address::parse(mine, dev.hrp)->program};
        check(Http::body(h.call("GET", "/api/check?addr=" + main_addr.str(ChainParams::main().hrp))).find("\"valid\":false") != std::string::npos, "addresses of the other network are refused");
    }
    {
        std::string rst = h.call("POST", "/api/wallet/restore", "{\"mnemonic\":\"" + phrase + "\",\"password\":\"another long one\"}");
        check(Http::status(rst) == 409, "restore refuses to overwrite an existing wallet");
        GuiConfig c2 = cfg;
        c2.wallet = dir / "restored.wallet";
        GuiServer s2(c2);
        Http h2{s2.port(), s2.token()};
        check(Http::status(h2.call("POST", "/api/wallet/restore", "{\"mnemonic\":\"abandon abandon\",\"password\":\"another long one\"}")) == 400, "restore rejects an invalid phrase");
        std::string up = phrase;
        for (auto& c : up) c = char(std::toupper(uint8_t(c)));
        std::string r2 = h2.call("POST", "/api/wallet/restore", "{\"mnemonic\":\"  " + up + " \",\"password\":\"another long one\"}");
        std::string first_wallet_addr;
        {
            WalletFile w1;
            w1.path = cfg.wallet;
            first_wallet_addr = wallet_addresses(w1)[0];
        }
        check(Http::status(r2) == 200 && field_of(Http::body(r2), "address") == first_wallet_addr, "restore (case and spacing tolerant) derives the same first address as the original wallet");
    }

    std::cout << "GUI wallet: fee formula, amounts, signing without a node\n";
    {
        check(parse_amount("1.5") == 150000000 && parse_amount("0.00000001") == 1 && parse_amount("21000000") == MAX_MONEY, "amounts parse exactly");
        bool all_bad = true;
        for (const char* x : {"", "0", "-1", "1.123456789", "abc", "1e5", ".5", "1.", "21000001", "99999999999"}) {
            try { parse_amount(x); if (std::string(x) != "1.") all_bad = false; } catch (const std::exception&) {}
        }
        check(all_bad, "invalid amounts are rejected");
        WalletSecret s;
        s.mnemonic = bip39::generate();
        Bytes seed = wallet_seed(s);
        Address mine{ADDRESS_VERSION_HYBRID, HybridKey::derive(seed, 0).pub().program()};
        std::vector<std::string> addrs = {mine.str(dev.hrp)};
        std::vector<WalletUtxo> coins;
        for (uint8_t i = 1; i <= 3; i++) {
            WalletUtxo u{};
            u.op.txid.fill(i);
            u.value = uint64_t(i) * COIN;
            u.conf = 10;
            u.mature = true;
            coins.push_back(u);
        }
        PaymentPlan one = wallet_plan_payment(coins, COIN, false, 1), two = wallet_plan_payment(coins, 3 * COIN + 5, false, 1), all = wallet_plan_payment(coins, 0, true, 1);
        check(one.use.size() == 1 && one.fee == 100 + 3866 && two.use.size() == 2 && two.fee == 100 + 2 * 3866 && all.use.size() == 3 && all.amount == 6 * COIN - all.fee,
              "fee = (100 + 3866 * inputs) * rate; largest coins first; max sends everything minus the fee");
        bool broke = false;
        try { wallet_plan_payment(coins, 7 * COIN, false, 1); } catch (const std::runtime_error&) { broke = true; }
        check(broke, "insufficient funds are refused");
        Transaction t = wallet_sign_payment(dev, s, addrs, one, mine);
        check(t.serialize().size() <= one.fee && t.vin.size() == 1 && t.vout.size() == 2, "signed transaction fits the previewed fee (relay minimum is one fyl per byte)");
        wipe(seed.data(), seed.size());
    }

    fs::remove_all(dir);
}

}  // namespace plk
