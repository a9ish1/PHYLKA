#include "net.h"

#include "explorer.h"
#include "platform.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstring>
#include <ctime>
#include <iostream>
#include <random>
#include <fstream>
#include <sstream>

namespace plk {

using Clock = std::chrono::steady_clock;
static double secs() { return std::chrono::duration<double>(Clock::now().time_since_epoch()).count(); }

static void log(const std::string& s) {
    static std::mutex m;
    std::lock_guard<std::mutex> lk(m);
    auto t = std::time(nullptr);
    char buf[16];
    std::strftime(buf, sizeof buf, "%H:%M:%S", std::localtime(&t));
    std::cout << buf << "  " << s << std::endl;
}

static bool write_full(int fd, const uint8_t* p, size_t n) {
    while (n) {
        ssize_t r = ::send(fd, p, n, platform::SEND_FLAGS);
        if (r <= 0) return false;
        p += r;
        n -= size_t(r);
    }
    return true;
}

static void set_timeouts(int fd, int rcv_s, int snd_s) {
    timeval r{rcv_s, 0}, s{snd_s, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &r, sizeof r);
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &s, sizeof s);
}

static Bytes u32le(uint32_t v) { return Bytes{uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)}; }
static uint32_t get_u32(const Bytes& b, size_t o = 0) {
    return b.size() < o + 4 ? 0 : uint32_t(b[o]) | b[o + 1] << 8 | b[o + 2] << 16 | uint32_t(b[o + 3]) << 24;
}
static uint64_t get_u64(const Bytes& b, size_t o = 0) { return uint64_t(get_u32(b, o)) | uint64_t(get_u32(b, o + 4)) << 32; }
static void put_u64(Bytes& b, uint64_t v) { for (int i = 0; i < 8; i++) b.push_back(uint8_t(v >> (8 * i))); }

// Payload cap per command, checked before anything is allocated. Unknown commands get a small cap and are ignored.
static constexpr size_t UNKNOWN_CAP = 1024, PREVERSION_CAP = 1024;
static size_t payload_cap(const std::string& c, bool& known) {
    static const std::map<std::string, size_t> caps = {
        {"version", 46}, {"getaddr", 0}, {"addr", 1001}, {"getblock", 4}, {"getvotes", 4}, {"inv", 36}, {"ping", 8}, {"pong", 8},
        {"vote", 4096},  {"reg", REG_BYTES}, {"evid", 5068}, {"tx", 200'000}, {"block", MAX_BLOCK_BYTES}};
    auto it = caps.find(c);
    known = it != caps.end();
    return known ? it->second : UNKNOWN_CAP;
}

static constexpr int MAX_INBOUND = 32, MAX_INBOUND_PER_IP = 4;
static constexpr size_t MAX_WANT = 2000;
static constexpr uint32_t MAX_AHEAD = 2000, MAX_VOTES_REPLY = 500;
static constexpr double HANDSHAKE_S = 10, PING_S = 120, SILENCE_S = 300, MSG_S = 60, GETVOTES_PERIOD_S = 60, GETVOTES_MIN_S = 11, REQUEST_TIMEOUT_S = 10;
static constexpr time_t BAN_S = 24 * 3600;

bool Net::Bucket::take(double now) {
    if (last == 0) last = now;
    tokens = std::min(burst, tokens + (now - last) * rate);
    last = now;
    if (tokens < 1) return false;
    tokens -= 1;
    return true;
}

Net::Peer::~Peer() {
    if (th.joinable()) th.join();
    if (fd >= 0) platform::close_socket(fd);
}

// ---------------------------------------------------------------- P2P

Net::Net(Node& node, uint16_t port, std::vector<std::string> connect_to, bool listen_all)
    : node_(node), port_(port), listen_all_(listen_all), connect_to_(std::move(connect_to)) {
    std::random_device rd;
    while (!nonce_) nonce_ = uint64_t(rd()) << 32 | rd();
    book_load();
    threads_.emplace_back([this] { listen_loop(); });
    threads_.emplace_back([this] { connect_loop(); });
}

Net::~Net() {
    stop_ = true;
    book_save();
    for (auto& t : threads_) if (t.joinable()) t.join();   // loops poll with short timeouts, so they notice stop_
    auto ps = snapshot();
    for (auto& p : ps) ::shutdown(p->fd, SHUT_RDWR);       // wakes blocking recv; fds are closed only in ~Peer
    for (auto& p : ps) if (p->th.joinable()) p->th.join();
    std::lock_guard<std::mutex> lk(pm_);
    peers_.clear();
}

std::vector<std::shared_ptr<Net::Peer>> Net::snapshot() {
    std::lock_guard<std::mutex> lk(pm_);
    return peers_;
}

size_t Net::peers() {
    std::lock_guard<std::mutex> lk(pm_);
    size_t n = 0;
    for (auto& p : peers_) n += p->alive;
    return n;
}

void Net::reap() {
    std::vector<std::shared_ptr<Peer>> dead;
    {
        std::lock_guard<std::mutex> lk(pm_);
        auto it = std::stable_partition(peers_.begin(), peers_.end(), [](auto& x) { return !x->done; });
        dead.assign(it, peers_.end());
        peers_.erase(it, peers_.end());
    }
    for (auto& p : dead) if (p->th.joinable()) p->th.join();   // done is set as the thread's last act
}

void Net::ban(const std::string& ip) {
    if (ip.rfind("127.", 0) == 0) { log("P2P: " + ip + " - loopback, ban not applied"); return; }
    std::lock_guard<std::mutex> lk(bm_);
    bans_[ip] = std::time(nullptr) + BAN_S;
    log("P2P: " + ip + " banned for 24 hours");
}

bool Net::is_banned(const std::string& ip) {
    std::lock_guard<std::mutex> lk(bm_);
    auto it = bans_.find(ip);
    if (it == bans_.end()) return false;
    if (it->second > std::time(nullptr)) return true;
    bans_.erase(it);
    return false;
}

void Net::drop(Peer& p) {
    p.alive = false;
    ::shutdown(p.fd, SHUT_RDWR);
}

void Net::misbehave(Peer& p, int points, const std::string& why) {
    int s = (p.score += points);
    if (points > 1 || s >= 100) log("P2P: " + p.name + " - misbehaving (" + why + "), +" + std::to_string(points) + ", total " + std::to_string(s));
    if (s < 100) return;
    ban(p.ip);
    drop(p);
}

void Net::listen_loop() {
    int lfd = ::socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    ::setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port_);
    a.sin_addr.s_addr = listen_all_ ? INADDR_ANY : htonl(INADDR_LOOPBACK);
    if (::bind(lfd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || ::listen(lfd, 16) != 0) {
        log("P2P: port " + std::to_string(port_) + " in use - inbound connections disabled");
        platform::close_socket(lfd);
        return;
    }
    log("P2P: listening on " + std::string(listen_all_ ? "0.0.0.0" : "127.0.0.1") + ":" + std::to_string(port_));
    double last_refuse_log = 0;
    while (!stop_) {
        reap();
        pollfd pf{lfd, POLLIN, 0};
        if (::poll(&pf, 1, 200) <= 0) continue;
        sockaddr_in from{};
        socklen_t len = sizeof from;
        int fd = ::accept(lfd, reinterpret_cast<sockaddr*>(&from), &len);
        if (fd < 0) continue;
        char ipbuf[INET_ADDRSTRLEN] = {};
        ::inet_ntop(AF_INET, &from.sin_addr, ipbuf, sizeof ipbuf);
        std::string ip = ipbuf;
        if (is_banned(ip) || !start_peer(fd, ip + ":" + std::to_string(ntohs(from.sin_port)), ip, false)) {
            platform::close_socket(fd);   // banned or over the connection limits: refuse at once
            if (secs() - last_refuse_log > 5) { last_refuse_log = secs(); log("P2P: inbound from " + ip + " rejected (banned or connection limit)"); }
        }
    }
    platform::close_socket(lfd);
}

// Non-blocking connect that gives up after `ms` and notices shutdown
static int connect_timeout(const sockaddr* sa, socklen_t len, int ms, const std::atomic<bool>& stop) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int fl = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    bool ok = ::connect(fd, sa, len) == 0;
    bool pending = !ok && errno == EINPROGRESS;
    for (int waited = 0; pending && !stop && waited < ms; waited += 100) {
        pollfd pf{fd, POLLOUT, 0};
        if (::poll(&pf, 1, 100) > 0) {
            int err = 1;
            socklen_t el = sizeof err;
            ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el);
            ok = err == 0;
            break;
        }
    }
    if (!ok) { platform::close_socket(fd); return -1; }
    ::fcntl(fd, F_SETFL, fl);
    return fd;
}

void Net::connect_loop() {
    while (!stop_) {
        for (auto& target : connect_to_) {
            if (stop_) break;
            bool skip = false;
            {
                std::lock_guard<std::mutex> lk(pm_);
                skip = self_targets_.count(target) > 0;
                for (auto& p : peers_) skip |= p->alive && p->outbound && p->name == target;
            }
            auto colon = target.rfind(':');
            if (skip || colon == std::string::npos) continue;
            std::string host = target.substr(0, colon), port = target.substr(colon + 1);
            addrinfo hints{}, *res = nullptr;
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_STREAM;
            if (::getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0) continue;
            char ipbuf[INET_ADDRSTRLEN] = {};
            ::inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr, ipbuf, sizeof ipbuf);
            int fd = is_banned(ipbuf) ? -1 : connect_timeout(res->ai_addr, res->ai_addrlen, 3000, stop_);
            if (fd >= 0 && !start_peer(fd, target, ipbuf, true)) platform::close_socket(fd);
            ::freeaddrinfo(res);
        }
        dial_from_book();
        if (secs() - book_saved_ > 30) { book_saved_ = secs(); book_save(); }
        for (int i = 0; i < 50 && !stop_; i++) {
            reap();
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}

bool Net::start_peer(int fd, const std::string& name, const std::string& ip, bool outbound) {
    int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    platform::set_nosigpipe(fd);
    set_timeouts(fd, 1, 10);   // recv wakes every second for the keep-alive checks; a stuck send gives up after 10 s
    auto p = std::make_shared<Peer>();
    p->fd = fd;
    p->name = name;
    p->ip = ip;
    p->outbound = outbound;
    p->t_start = p->last_recv = p->last_ping = secs();
    {
        std::lock_guard<std::mutex> lk(pm_);
        if (!outbound) {
            int total = 0, same = 0;
            for (auto& x : peers_)
                if (x->alive && !x->outbound) total++, same += x->ip == ip;
            if (total >= MAX_INBOUND || same >= MAX_INBOUND_PER_IP) { p->fd = -1; return false; }   // caller closes fd
        }
        peers_.push_back(p);
        Peer* raw = p.get();   // the Net keeps the Peer alive until its thread is joined
        p->th = std::thread([this, raw] { peer_loop(raw); });
    }
    log("P2P: connected " + name + (outbound ? " (outbound)" : " (inbound)"));
    return true;
}

void Net::send(Peer& p, const std::string& cmd, const Bytes& payload) {
    Bytes m(node_.params().magic.begin(), node_.params().magic.end());
    char c[12] = {};
    std::strncpy(c, cmd.c_str(), 12);
    m.insert(m.end(), c, c + 12);
    Bytes len = u32le(uint32_t(payload.size()));
    m.insert(m.end(), len.begin(), len.end());
    Hash256 ck = sha256d(payload);
    m.insert(m.end(), ck.begin(), ck.begin() + 4);
    m.insert(m.end(), payload.begin(), payload.end());
    std::lock_guard<std::mutex> lk(p.wm);
    if (!write_full(p.fd, m.data(), m.size())) p.alive = false;
}

// Reads exactly n bytes; every receive timeout runs the keep-alive / deadline checks
bool Net::recv_n(Peer& p, uint8_t* b, size_t n) {
    while (n) {
        if (!p.alive || stop_) return false;
        ssize_t r = ::recv(p.fd, b, n, 0);
        if (r > 0) {
            p.last_recv = secs();
            if (!p.msg_start) p.msg_start = p.last_recv;
            b += r;
            n -= size_t(r);
        } else if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            if (!tick(p)) return false;
        } else {
            return false;
        }
    }
    return true;
}

// Deadlines, keep-alive ping, vote sync and stalled-sync recovery. Returns false to disconnect.
bool Net::tick(Peer& p) {
    double t = secs();
    if (!p.alive || stop_) return false;
    if (!p.got_version && t - p.t_start > HANDSHAKE_S) { log("P2P: " + p.name + " - no version within 10 s, disconnecting"); return false; }
    if (t - p.last_recv > SILENCE_S) { log("P2P: " + p.name + " - silent for over 300 s, disconnecting"); return false; }
    if (p.msg_start && t - p.msg_start > MSG_S) { log("P2P: " + p.name + " - message arriving too slowly, disconnecting"); return false; }
    if (!p.got_version) return true;
    if (t - p.last_ping >= PING_S) {
        p.last_ping = t;
        Bytes n;
        put_u64(n, nonce_ ^ uint64_t(t * 1000));
        send(p, "ping", n);
    }
    uint32_t ours, fin;
    uint64_t tv;
    {
        std::lock_guard<std::mutex> lk(node_.mutex());
        ours = node_.height();
        fin = node_.finalized_height();
        tv = node_.tip_version;
    }
    bool changed = tv != p.gv_tip || fin != p.gv_fin;
    if ((changed && t - p.last_gv >= GETVOTES_MIN_S) || t - p.last_gv >= GETVOTES_PERIOD_S) {
        // Start one checkpoint back: a node on a losing branch needs the votes that finalized the winning one
        uint32_t ci = node_.params().checkpoint_interval;
        send(p, "getvotes", u32le(fin >= ci ? fin - ci : 0));
        p.last_gv = t, p.gv_tip = tv, p.gv_fin = fin;
    }
    // Validators with > 2/3 of the stake signed a block we do not have (a branch with less work than ours): fetch that branch
    uint32_t hint;
    {
        std::lock_guard<std::mutex> lk(node_.mutex());
        hint = node_.finality_hint();
    }
    if (hint && t - p.last_hint >= 5) {
        p.last_hint = t;
        {
            std::lock_guard<std::mutex> lk(p.wm);
            p.want.push_back(hint);
        }
        request_next(p);
    }
    // A peer at or below our height may still be on a chain with MORE WORK (e.g. higher difficulty): fetch its tip once
    // (the answer is harmless if we already have it) and again every minute; a longer chain is followed as before
    uint32_t th = p.their_height;
    if (th && th <= ours && (th != p.probe_h || t - p.last_probe >= 60)) {
        p.probe_h = th, p.last_probe = t;
        {
            std::lock_guard<std::mutex> lk(p.wm);
            p.want.push_back(th);
        }
        request_next(p);
    }
    if (p.their_height > ours) request_next(p);   // retries a lost/dropped getblock after REQUEST_TIMEOUT_S
    return p.alive;
}

void Net::peer_loop(Peer* p) {
    uint32_t h;
    {
        std::lock_guard<std::mutex> lk(node_.mutex());
        h = node_.height();
    }
    Bytes ver = u32le(h);
    put_u64(ver, nonce_);
    ver.insert(ver.end(), node_.genesis_hash().begin(), node_.genesis_hash().end());   // genesis hash: peers of another chain are refused
    // Advertise our listen port only if others can reach us (--public); the dev network also allows loopback peers
    uint16_t adv = (listen_all_ || node_.params().name == "dev") ? port_ : 0;
    ver.push_back(uint8_t(adv)); ver.push_back(uint8_t(adv >> 8));
    send(*p, "version", ver);
    while (p->alive && !stop_) {
        uint8_t hdr[24];
        if (!recv_n(*p, hdr, sizeof hdr)) break;
        if (!std::equal(node_.params().magic.begin(), node_.params().magic.end(), hdr)) { misbehave(*p, 100, "foreign network"); break; }
        const uint8_t* cend = std::find(hdr + 4, hdr + 16, uint8_t(0));   // command is NUL padded (std::strnlen is not standard C++)
        std::string cmd(reinterpret_cast<const char*>(hdr + 4), size_t(cend - (hdr + 4)));
        uint32_t len = get_u32(Bytes(hdr + 16, hdr + 20));
        if (!p->got_version && cmd != "version") { log("P2P: " + p->name + " - first message is not version, disconnecting"); break; }
        bool known;
        size_t cap = payload_cap(cmd, known);
        if (!p->got_version) cap = std::min(cap, PREVERSION_CAP);
        if (len > cap) { misbehave(*p, 100, "oversized message " + cmd); break; }
        Bytes payload(len);
        if (!recv_n(*p, payload.data(), len)) break;
        p->msg_start = 0;
        Hash256 ck = sha256d(payload);
        if (std::memcmp(ck.data(), hdr + 20, 4) != 0) { misbehave(*p, 100, "bad checksum"); break; }
        if (!known) misbehave(*p, 1, "unknown command");
        else handle(*p, cmd, payload);
        if (!tick(*p)) break;
    }
    p->alive = false;
    ::shutdown(p->fd, SHUT_RDWR);
    log("P2P: disconnected " + p->name);
    p->done = true;   // last act: the reaper may join this thread from now on
}

void Net::send_getblock(Peer& p, uint32_t height) {
    double wait = 0.06 - (secs() - p.last_gb);   // stay under the receiver's 20 getblock/s limit
    if (wait > 0) std::this_thread::sleep_for(std::chrono::duration<double>(wait));
    p.last_gb = p.req_t = secs();
    p.req_h = height;
    send(p, "getblock", u32le(height));
}

void Net::request_next(Peer& p) {
    if (p.req_h && secs() - p.req_t < REQUEST_TIMEOUT_S) return;   // one request in flight at a time, or the pipelines multiply
    uint32_t ours;
    {
        std::lock_guard<std::mutex> lk(node_.mutex());
        ours = node_.height();
    }
    uint32_t want = 0;
    {
        std::lock_guard<std::mutex> lk(p.wm);
        if (!p.want.empty()) { want = p.want.back(); p.want.pop_back(); }
    }
    if (want) send_getblock(p, want);
    else if (p.their_height > ours) send_getblock(p, ours + 1);
}

void Net::on_version(Peer& p, const Bytes& payload) {
    if (p.got_version) return misbehave(p, 10, "duplicate version");
    if (payload.size() != 4 && payload.size() != 12 && payload.size() != 44 && payload.size() != 46) return misbehave(p, 100, "bad version");
    if (payload.size() >= 44 && !std::equal(payload.begin() + 12, payload.begin() + 44, node_.genesis_hash().begin())) {
        log("P2P: " + p.name + " - foreign network (different genesis), disconnecting");
        return drop(p);   // older peers send no genesis hash; they are accepted, the magic bytes already match
    }
    uint64_t nonce = payload.size() == 12 ? get_u64(payload, 4) : 0;   // old peers send the height only
    if (nonce) {
        std::lock_guard<std::mutex> lk(pm_);
        if (nonce == nonce_) {
            log("P2P: " + p.name + " - that is ourselves, disconnecting");
            if (p.outbound) self_targets_.insert(p.name);
            return drop(p);
        }
        for (auto& q : peers_)
            if (q.get() != &p && q->alive && q->nonce == nonce) {
                log("P2P: " + p.name + " - duplicate connection to the same node, disconnecting");
                return drop(p);
            }
        p.nonce = nonce;
    }
    p.got_version = true;
    p.their_height = get_u32(payload);
    if (payload.size() == 46) {
        p.listen_port = uint16_t(payload[44] | payload[45] << 8);
        if (p.listen_port && addr_ok(p.ip)) {   // a fresh self-announcement: remember it and tell two other peers
            book_add(p.ip, p.listen_port, std::time(nullptr));
            Bytes one{1};
            in_addr ia{};
            ::inet_pton(AF_INET, p.ip.c_str(), &ia);
            const uint8_t* ib = reinterpret_cast<const uint8_t*>(&ia);
            one.insert(one.end(), ib, ib + 4);
            one.push_back(uint8_t(p.listen_port)); one.push_back(uint8_t(p.listen_port >> 8));
            Bytes t = u32le(uint32_t(std::time(nullptr)));
            one.insert(one.end(), t.begin(), t.end());
            int sent = 0;
            for (auto& q : snapshot())
                if (q.get() != &p && q->alive && q->got_version && sent < 2) { send(*q, "addr", one); sent++; }
        }
    }
    send(p, "getaddr", {});
    request_next(p);
}

// ---------------------------------------------------------------- peer discovery (address book)

static constexpr size_t MAX_BOOK = 1000, MAX_ADDR_PER_MSG = 100, MAX_OUTBOUND = 8;

/// Remote peers may only announce publicly routable IPv4 addresses (the dev network also allows loopback/private ones)
bool Net::addr_ok(const std::string& ip) const {
    in_addr a{};
    if (::inet_pton(AF_INET, ip.c_str(), &a) != 1) return false;
    uint32_t x = ntohl(a.s_addr);
    if (node_.params().name == "dev") return x != 0;
    uint8_t b0 = x >> 24, b1 = (x >> 16) & 255;
    if (b0 == 0 || b0 == 10 || b0 == 127 || b0 >= 224) return false;
    if (b0 == 169 && b1 == 254) return false;
    if (b0 == 172 && b1 >= 16 && b1 <= 31) return false;
    if (b0 == 192 && b1 == 168) return false;
    if (b0 == 100 && b1 >= 64 && b1 <= 127) return false;   // carrier-grade NAT
    return true;
}

void Net::book_add(const std::string& ip, uint16_t port, time_t seen) {
    if (!port || (port == port_ && (ip == "127.0.0.1" || ip == "0.0.0.0"))) return;   // never remember ourselves
    std::lock_guard<std::mutex> lk(am_);
    std::string key = ip + ":" + std::to_string(port);
    time_t now = std::time(nullptr);
    seen = std::min(seen, now);
    auto it = book_.find(key);
    if (it == book_.end()) {
        if (book_.size() >= MAX_BOOK) {   // evict the least recently seen
            auto old = book_.begin();
            for (auto j = book_.begin(); j != book_.end(); ++j) if (j->second < old->second) old = j;
            if (old->second >= seen) return;
            book_.erase(old);
        }
        book_[key] = seen;
    } else if (seen > it->second) {
        it->second = seen;
    }
    book_dirty_ = true;
}

size_t Net::known_addresses() {
    std::lock_guard<std::mutex> lk(am_);
    return book_.size();
}

std::vector<std::string> Net::book_sample(size_t n) {
    std::lock_guard<std::mutex> lk(am_);
    std::vector<std::string> v;
    time_t now = std::time(nullptr);
    for (auto& [k, t] : book_) if (now - t < 30 * 24 * 3600) v.push_back(k);
    static thread_local std::mt19937 rng{std::random_device{}()};
    std::shuffle(v.begin(), v.end(), rng);
    if (v.size() > n) v.resize(n);
    return v;
}

void Net::book_load() {
    std::ifstream f(node_.datadir() / "peers.dat");
    std::string key;
    long long t;
    while (f >> key >> t) {
        auto c = key.rfind(':');
        if (c == std::string::npos || !addr_ok(key.substr(0, c))) continue;
        book_add(key.substr(0, c), uint16_t(std::stoi(key.substr(c + 1))), time_t(t));
    }
}

void Net::book_save() {
    std::lock_guard<std::mutex> lk(am_);
    if (!book_dirty_) return;
    std::filesystem::path f = node_.datadir() / "peers.dat", tmp = node_.datadir() / "peers.dat.tmp";
    {
        std::ofstream o(tmp);
        for (auto& [k, t] : book_) o << k << " " << t << "\n";
    }
    std::filesystem::rename(tmp, f);
    book_dirty_ = false;
}

void Net::on_getaddr(Peer& p) {
    auto sample = book_sample(MAX_ADDR_PER_MSG);
    Bytes out;
    uint8_t n = 0;
    std::lock_guard<std::mutex> lk(am_);
    for (auto& k : sample) {
        auto c = k.rfind(':');
        in_addr ia{};
        if (::inet_pton(AF_INET, k.substr(0, c).c_str(), &ia) != 1) continue;
        const uint8_t* ib = reinterpret_cast<const uint8_t*>(&ia);
        out.insert(out.end(), ib, ib + 4);
        uint16_t port = uint16_t(std::stoi(k.substr(c + 1)));
        out.push_back(uint8_t(port)); out.push_back(uint8_t(port >> 8));
        Bytes t = u32le(uint32_t(book_[k]));
        out.insert(out.end(), t.begin(), t.end());
        n++;
    }
    if (!n) return;
    out.insert(out.begin(), n);
    send(p, "addr", out);
}

void Net::on_addr(Peer& p, const Bytes& payload) {
    size_t n = payload.empty() ? 0 : payload[0];
    if (n > MAX_ADDR_PER_MSG || payload.size() != 1 + 10 * n) return misbehave(p, 20, "bad addr message");
    for (size_t i = 0; i < n; i++) {
        const uint8_t* e = &payload[1 + 10 * i];
        char ip[INET_ADDRSTRLEN];
        in_addr ia{};
        std::memcpy(&ia, e, 4);
        ::inet_ntop(AF_INET, &ia, ip, sizeof ip);
        uint16_t port = uint16_t(e[4] | e[5] << 8);
        if (addr_ok(ip) && port) book_add(ip, port, time_t(get_u32(payload, 1 + 10 * i + 6)));
    }
}

/// Keep up to MAX_OUTBOUND outbound connections: top up from the address book (seeds/--connect are dialled separately)
void Net::dial_from_book() {
    int out = 0;
    std::set<std::string> have;
    {
        std::lock_guard<std::mutex> lk(pm_);
        for (auto& q : peers_) {
            if (!q->alive) continue;
            out += q->outbound;
            have.insert(q->ip + ":" + std::to_string(q->listen_port.load()));
            if (q->outbound) have.insert(q->name);
        }
    }
    for (auto& key : book_sample(MAX_OUTBOUND * 2)) {
        if (out >= int(MAX_OUTBOUND) || stop_) break;
        if (have.count(key) || (key.rfind("127.0.0.1:", 0) == 0 && key == "127.0.0.1:" + std::to_string(port_))) continue;
        auto c = key.rfind(':');
        std::string ip = key.substr(0, c);
        if (is_banned(ip)) continue;
        addrinfo hints{}, *res = nullptr;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        if (::getaddrinfo(ip.c_str(), key.substr(c + 1).c_str(), &hints, &res) != 0) continue;
        int fd = connect_timeout(res->ai_addr, res->ai_addrlen, 3000, stop_);
        ::freeaddrinfo(res);
        if (fd >= 0 && !start_peer(fd, key, ip, true)) platform::close_socket(fd);
        else if (fd >= 0) out++;
    }
}

void Net::on_block(Peer& p, const Bytes& payload) {
    p.req_h = 0;   // the outstanding getblock (if any) is answered
    auto b = block_from_bytes(payload);
    if (!b || b->txs.empty()) return misbehave(p, 50, "block not parsed");
    uint32_t bh = coinbase_height(b->txs[0]);
    uint32_t ours;
    {
        std::lock_guard<std::mutex> lk(node_.mutex());
        ours = node_.height();
    }
    if (uint64_t(bh) > uint64_t(ours) + MAX_AHEAD) return;   // unsolicited and too far ahead
    std::string err;
    bool missing = false, new_tip = false;
    bool ok = node_.accept_block(*b, err, &missing, &new_tip);
    if (missing && bh > 0) {   // fork: walk back to a common ancestor, then come forward again
        {
            std::lock_guard<std::mutex> lk(p.wm);
            p.want.push_back(bh);
            if (p.want.size() > MAX_WANT) p.want.pop_front();   // drop the oldest
        }
        return send_getblock(p, bh - 1);
    }
    if (!ok) {
        log("P2P: block from " + p.name + " rejected: " + err);
        if (!missing && !err_has(err, errmsg::FUTURE_BLOCK)) misbehave(p, 100, "invalid block");   // a skewed clock is not malice
        return;
    }
    if (bh > p.their_height) p.their_height = bh;
    if (new_tip) {
        log("New block #" + std::to_string(bh) + "  " + hash_hex(b->header.hash()).substr(0, 16) + "…  from " + p.name);
        broadcast_block(*b);
    }
    request_next(p);
}

void Net::on_vote(Peer& p, const Bytes& payload) {
    size_t pos = 0;
    auto v = payload.size() == VOTE_COMPACT_BYTES ? Vote::parse_compact(payload, pos) : Vote::parse(payload, pos);
    if (!v || pos != payload.size()) return misbehave(p, 50, "vote not parsed");
    std::string err;
    uint32_t before;
    {
        std::lock_guard<std::mutex> lk(node_.mutex());
        before = node_.finalized_height();
    }
    auto r = node_.accept_vote(*v, err);
    if (r == Node::VoteResult::Accepted) {
        broadcast_vote(*v);
        std::lock_guard<std::mutex> lk(node_.mutex());
        if (node_.finalized_height() > before)
            log("Finalized block #" + std::to_string(node_.finalized_height()) + " - it can no longer be reverted");
    } else if (r == Node::VoteResult::Rejected) {
        if (err_has(err, errmsg::VOTE_EQUIVOCATION)) {
            log("WARNING: " + err);
            for (auto& e : node_.take_new_evidence()) broadcast_evidence(e);
        }
        else if (err_has(err, errmsg::VOTE_REGISTRY) || err_has(err, errmsg::VOTE_UNREGISTERED) || err_has(err, errmsg::VOTE_COMPACT)) return;   // may be a registration we have not seen yet
        else if (err_has(err, errmsg::VOTE_SIG) || err_has(err, errmsg::VOTE_STAKE) || err_has(err, errmsg::VOTE_CKPT))
            misbehave(p, 20, "bad vote: " + err);   // "block not on our chain" is not the peer's fault
    }
}

void Net::on_reg(Peer& p, const Bytes& payload) {
    PublicKey k;
    std::copy(payload.begin(), payload.begin() + SCHNORR_PK, k.schnorr.begin());
    k.mldsa.assign(payload.begin() + SCHNORR_PK, payload.end());
    std::string err;
    if (node_.accept_registration(k, err) == Node::VoteResult::Accepted) {
        log("Validator key registration accepted (from " + p.name + ")");
        broadcast_reg(k);
    }
}

void Net::on_evid(Peer& p, const Bytes& payload) {
    size_t pos = 0;
    auto e = Evidence::parse(payload, pos);
    if (!e || pos != payload.size()) return misbehave(p, 50, "evidence not parsed");
    std::string err;
    if (node_.accept_evidence(*e, err) == Node::VoteResult::Accepted) {
        log("WARNING: double-vote evidence received (from " + p.name + ")");
        broadcast_evidence(*e);
    }
}

void Net::on_tx(Peer& p, const Bytes& payload) {
    size_t pos = 0;
    auto t = Transaction::parse(payload, pos);
    if (!t || pos != payload.size()) return misbehave(p, 50, "transaction not parsed");
    std::string err;
    bool had;
    {
        std::lock_guard<std::mutex> lk(node_.mutex());
        had = node_.mempool().count(t->txid());
    }
    if (!node_.accept_tx(*t, err)) {
        if (err_has(err, errmsg::INPUT_MISSING)) return;   // a double-spend race or a reorg, not malice
        if (err_has(err, errmsg::FEE_LOW) || err_has(err, errmsg::MEMPOOL_FULL)) return;   // relay policy, not malice
        return misbehave(p, 10, "invalid transaction: " + err);
    }
    if (!had) {
        log("Transaction " + hash_hex(t->txid()).substr(0, 16) + "… in mempool (from " + p.name + ")");
        broadcast_tx(*t);
    }
}

void Net::on_getvotes(Peer& p, const Bytes& payload) {
    std::vector<Vote> vs;
    {
        std::lock_guard<std::mutex> lk(node_.mutex());
        vs = node_.votes_from(get_u32(payload));
    }
    std::stable_sort(vs.begin(), vs.end(), [](const Vote& a, const Vote& b) { return a.height < b.height; });
    if (vs.size() > MAX_VOTES_REPLY) vs.resize(MAX_VOTES_REPLY);
    size_t i = 0;
    for (auto& v : vs) {
        if (!p.alive || stop_) return;
        if (i++ > 250) std::this_thread::sleep_for(std::chrono::milliseconds(22));   // keep under the receiver's vote rate limit
        send(p, "vote", v.serialize());
    }
}

void Net::handle(Peer& p, const std::string& cmd, const Bytes& payload) {
    double now = secs();
    if ((cmd == "tx" && !p.tx.take(now)) || ((cmd == "vote" || cmd == "reg" || cmd == "evid") && !p.vote.take(now)) || (cmd == "getblock" && !p.getblock.take(now)) ||
        (cmd == "getvotes" && !p.getvotes.take(now)) || (cmd == "addr" && !p.addr.take(now)) || (cmd == "getaddr" && !p.getaddr.take(now)))
        return misbehave(p, 1, "rate limit " + cmd);   // excess messages are dropped
    bool fixed4 = cmd == "getblock" || cmd == "getvotes";
    if ((fixed4 && payload.size() != 4) || (cmd == "inv" && payload.size() != 36) || (cmd == "getaddr" && !payload.empty()) || ((cmd == "ping" || cmd == "pong") && payload.size() != 8))
        return misbehave(p, 50, "bad size " + cmd);
    if (cmd == "version") {
        on_version(p, payload);
    } else if (cmd == "ping") {
        send(p, "pong", payload);
    } else if (cmd == "getblock") {
        std::optional<Block> b;
        {
            std::lock_guard<std::mutex> lk(node_.mutex());
            if (auto bi = node_.at(get_u32(payload))) b = node_.read_block(bi->hash);
        }
        if (b) send(p, "block", b->serialize());
    } else if (cmd == "block") {
        on_block(p, payload);
    } else if (cmd == "inv") {
        uint32_t h = get_u32(payload);
        Hash256 hash{};
        std::copy(payload.begin() + 4, payload.begin() + 36, hash.begin());
        if (h > p.their_height) p.their_height = h;
        bool known;
        {
            std::lock_guard<std::mutex> lk(node_.mutex());
            known = node_.known(hash);
        }
        if (!known) {   // an unknown tip at or below our height may carry more work: ask for it too
            uint32_t ours;
            {
                std::lock_guard<std::mutex> lk(node_.mutex());
                ours = node_.height();
            }
            if (h <= ours) {
                std::lock_guard<std::mutex> lk(p.wm);
                p.want.push_back(h);
            }
            request_next(p);
        }
    } else if (cmd == "getaddr") {
        on_getaddr(p);
    } else if (cmd == "addr") {
        on_addr(p, payload);
    } else if (cmd == "getvotes") {
        on_getvotes(p, payload);
    } else if (cmd == "vote") {
        on_vote(p, payload);
    } else if (cmd == "reg" && payload.size() == REG_BYTES) {
        on_reg(p, payload);
    } else if (cmd == "evid") {
        on_evid(p, payload);
    } else if (cmd == "tx") {
        on_tx(p, payload);
    }   // pong: nothing to do, receiving it already refreshed last_recv
}

void Net::broadcast_block(const Block& b) {
    Bytes inv = u32le(coinbase_height(b.txs[0]));
    Hash256 h = b.header.hash();
    inv.insert(inv.end(), h.begin(), h.end());
    for (auto& p : snapshot())
        if (p->alive && p->got_version) send(*p, "inv", inv);
}

void Net::broadcast_tx(const Transaction& t) {
    Bytes raw = t.serialize();
    for (auto& p : snapshot())
        if (p->alive && p->got_version) send(*p, "tx", raw);
}

void Net::broadcast_vote(const Vote& v) {
    Bytes raw = v.serialize();
    for (auto& p : snapshot())
        if (p->alive && p->got_version) send(*p, "vote", raw);
}

void Net::broadcast_reg(const PublicKey& k) {
    Bytes raw(k.schnorr.begin(), k.schnorr.end());
    raw.insert(raw.end(), k.mldsa.begin(), k.mldsa.end());
    for (auto& p : snapshot())
        if (p->alive && p->got_version) send(*p, "reg", raw);
}

void Net::broadcast_evidence(const Evidence& e) {
    Bytes raw = e.serialize();
    for (auto& p : snapshot())
        if (p->alive && p->got_version) send(*p, "evid", raw);
}

void Net::submit_block(const Block& b) {
    std::string err;
    bool new_tip = false;
    if (!node_.accept_block(b, err, nullptr, &new_tip)) {
        log("Own block rejected: " + err);
        return;
    }
    if (new_tip) {
        log("Found block #" + std::to_string(coinbase_height(b.txs[0])) + "  " + hash_hex(b.header.hash()).substr(0, 16) +
            "…  reward " + std::to_string(double(b.txs[0].vout[0].value) / COIN) + " PLK");
        broadcast_block(b);
    }
}

// ---------------------------------------------------------------- RPC

static std::string query(const std::string& path, const std::string& key) {
    auto q = path.find('?');
    if (q == std::string::npos) return "";
    std::istringstream in(path.substr(q + 1));
    for (std::string kv; std::getline(in, kv, '&');) {
        auto eq = kv.find('=');
        if (eq != std::string::npos && kv.substr(0, eq) == key) return kv.substr(eq + 1);
    }
    return "";
}

static std::string fmt_plk(uint64_t fyl) {
    char b[32];
    std::snprintf(b, sizeof b, "%llu.%08llu", (unsigned long long)(fyl / COIN), (unsigned long long)(fyl % COIN));
    return b;
}

Rpc::Rpc(Node& node, Net& net, uint16_t port, std::function<double()> hashrate)
    : node_(node), net_(net), hashrate_(std::move(hashrate)) {
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   // never exposed outside this Mac
    if (::bind(fd_, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || ::listen(fd_, 16) != 0) {
        platform::close_socket(fd_);
        throw std::runtime_error("RPC: port " + std::to_string(port) + " in use");
    }
    log("RPC: http://127.0.0.1:" + std::to_string(port));
    th_ = std::thread([this] { loop(); });
}

Rpc::~Rpc() {
    stop_ = true;
    if (th_.joinable()) th_.join();   // the loop polls, so it notices stop_ within 200 ms
    for (auto& w : workers_) ::shutdown(w->fd, SHUT_RDWR);
    reap(true);
    platform::close_socket(fd_);
}

// Joins finished workers (or all of them) and closes their sockets
void Rpc::reap(bool all) {
    for (auto it = workers_.begin(); it != workers_.end();) {
        if (!all && !(*it)->done) { ++it; continue; }
        if ((*it)->th.joinable()) (*it)->th.join();
        platform::close_socket((*it)->fd);
        it = workers_.erase(it);
    }
}

void Rpc::loop() {
    constexpr size_t MAX_WORKERS = 8;
    while (!stop_) {
        reap(false);
        pollfd pf{fd_, POLLIN, 0};
        if (::poll(&pf, 1, 200) <= 0) continue;
        int c = ::accept(fd_, nullptr, nullptr);
        if (c < 0) continue;
        if (workers_.size() >= MAX_WORKERS) { platform::close_socket(c); continue; }   // too busy: refuse
        set_timeouts(c, 5, 5);
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

void Rpc::serve(int c) {
    std::string req;
    char buf[4096];
    double deadline = secs() + 5;   // the whole request, not just one recv
    while (req.find("\r\n\r\n") == std::string::npos && req.size() < 65536 && secs() < deadline && !stop_) {
        ssize_t r = ::recv(c, buf, sizeof buf, 0);
        if (r <= 0) break;
        req.append(buf, size_t(r));
    }
    if (req.find("\r\n\r\n") == std::string::npos) return;   // incomplete, too large or timed out: just drop it
    std::string path;
    if (req.rfind("GET ", 0) == 0) path = req.substr(4, req.find(' ', 4) - 4);
    std::string body, ctype = "text/plain; charset=utf-8";
    try {
        if (!explorer_route(node_, net_.peers(), hashrate_ ? hashrate_() : 0, path, body, ctype)) body = handle(path);
    } catch (const std::exception& e) {
        body = std::string("error ") + e.what() + "\n";
    }
    std::string resp = "HTTP/1.0 200 OK\r\nContent-Type: " + ctype + "\r\nCache-Control: no-store\r\nContent-Length: " +
                       std::to_string(body.size()) + "\r\n\r\n" + body;
    write_full(c, reinterpret_cast<const uint8_t*>(resp.data()), resp.size());
}

std::string Rpc::handle(const std::string& path) {
    std::ostringstream o;
    std::string route = path.substr(0, path.find('?'));
    const ChainParams& p = node_.params();
    if (route == "/info") {
        std::lock_guard<std::mutex> lk(node_.mutex());
        const BlockIndex* t = node_.tip();
        double diff = double(U256::from_compact(t->header.bits).work().to_ld() / U256::from_compact(0x1d00ffff).work().to_ld());   // 1 = Bitcoin's genesis
        o << "network " << p.name << "\nheight " << node_.height() << "\ntip " << hash_hex(t->hash) << "\nchainwork " << t->chainwork.to_hex() << "\ntime " << t->header.time
          << "\ndifficulty " << diff << "\nnext_bits " << std::hex << node_.next_bits(t) << std::dec << "\nmempool " << node_.mempool().size()
          << "\nfinalized " << node_.finalized_height() << "\nvalidators "
          << (node_.at(node_.height() / p.checkpoint_interval * p.checkpoint_interval) && node_.stake_at(node_.height() / p.checkpoint_interval * p.checkpoint_interval)
                  ? node_.stake_at(node_.height() / p.checkpoint_interval * p.checkpoint_interval)->size() : 0)
          << "\npeers " << net_.peers() << "\nhashrate " << (hashrate_ ? hashrate_() : 0) << "\nmaturity " << p.coinbase_maturity << "\nupgrade_height " << p.upgrade_height << "\nsigv2_height " << p.sigv2_height << "\nchain_final " << node_.chain_final()
          << "\nregistered " << node_.registry_size() << "\nslashed " << node_.slashed_count() << "\nutxo_commit " << to_hex(node_.utxo_commit()) << "\n";
        return o.str();
    }
    if (route == "/utxos") {
        auto a = Address::parse(query(path, "addr"), p.hrp);
        if (!a) return "error invalid address or wrong network\n";
        std::lock_guard<std::mutex> lk(node_.mutex());
        std::set<OutPoint> pending;
        for (auto& [id, t] : node_.mempool()) for (auto& in : t.vin) pending.insert(in.prev);
        for (auto& [op, c] : node_.utxo().coins) {
            if (c.out.to.program != a->program || pending.count(op)) continue;
            uint32_t conf = node_.height() - c.height + 1;
            bool mature = !c.coinbase || node_.height() + 1 >= c.height + p.coinbase_maturity;
            o << hash_hex(op.txid) << " " << op.vout << " " << c.out.value << " " << conf << " " << (c.coinbase ? 1 : 0) << " " << (mature ? 1 : 0)
              << " " << c.out.to.version << "\n";
        }
        return o.str();
    }
    if (route == "/history") {
        auto a = Address::parse(query(path, "addr"), p.hrp);
        if (!a) return "error invalid address or wrong network\n";
        std::lock_guard<std::mutex> lk(node_.mutex());
        std::map<OutPoint, uint64_t> mine;
        for (uint32_t h = 1; h <= node_.height(); h++) {
            auto b = node_.read_block(node_.at(h)->hash);
            if (!b) continue;
            for (auto& t : b->txs) {
                int64_t delta = 0;
                for (auto& in : t.vin) {
                    auto it = mine.find(in.prev);
                    if (it != mine.end()) { delta -= int64_t(it->second); mine.erase(it); }
                }
                Hash256 id = t.txid();
                for (uint32_t i = 0; i < t.vout.size(); i++)
                    if (t.vout[i].to.program == a->program) { delta += int64_t(t.vout[i].value); mine[OutPoint{id, i}] = t.vout[i].value; }
                if (delta) o << h << " " << b->header.time << " " << hash_hex(id) << " " << delta << " " << (t.is_coinbase() ? "mined" : "tx") << "\n";
            }
        }
        for (auto& [id, t] : node_.mempool()) {
            int64_t delta = 0;
            for (auto& in : t.vin) if (mine.count(in.prev)) delta -= int64_t(mine[in.prev]);
            for (auto& out : t.vout) if (out.to.program == a->program) delta += int64_t(out.value);
            if (delta) o << "pending 0 " << hash_hex(id) << " " << delta << " tx\n";
        }
        return o.str();
    }
    if (route == "/send") {
        Bytes raw;
        if (!from_hex(query(path, "tx"), raw)) return "error invalid hex\n";
        size_t pos = 0;
        auto t = Transaction::parse(raw, pos);
        if (!t || pos != raw.size()) return "error transaction not parsed\n";
        std::string err;
        if (!node_.accept_tx(*t, err)) return "error " + err + "\n";
        net_.broadcast_tx(*t);
        log("Transaction " + hash_hex(t->txid()).substr(0, 16) + "… accepted from wallet and relayed");
        return "ok " + hash_hex(t->txid()) + "\n";
    }
    if (route == "/block") {
        std::lock_guard<std::mutex> lk(node_.mutex());
        auto bi = node_.at(uint32_t(std::stoul(query(path, "height").empty() ? "0" : query(path, "height"))));
        if (!bi) return "error no such block\n";
        auto b = node_.read_block(bi->hash);
        o << "height " << bi->height << "\nhash " << hash_hex(bi->hash) << "\nprev " << hash_hex(bi->header.prev) << "\ntime "
          << bi->header.time << "\nbits " << std::hex << bi->header.bits << std::dec << "\nnonce " << bi->header.nonce << "\ntxs "
          << (b ? b->txs.size() : 0) << "\n";
        if (b && !b->txs.empty()) {
            auto& d = b->txs[0].vin[0].coinbase_data;
            std::string text(d.begin() + std::min<size_t>(d.size(), coinbase_prefix_len(p, bi->height)), d.end());
            o << "coinbase_text " << text << "\nreward " << fmt_plk(b->txs[0].vout[0].value) << "\n";
            if (b->has_ext)
                o << "ext regs " << b->ext.regs.size() << " evidence " << b->ext.evidence.size() << " cert_height " << (b->ext.cert ? b->ext.cert->height : 0)
                  << " cert_votes " << (b->ext.cert ? b->ext.cert->votes.size() : 0) << "\n";
        }
        return o.str();
    }
    return "error unknown request; available: /info /utxos?addr= /history?addr= /send?tx= /block?height=\n";
}

std::string http_get(uint16_t port, const std::string& path) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
        platform::close_socket(fd);
        throw std::runtime_error("node is not running (no reply on 127.0.0.1:" + std::to_string(port) + ")");
    }
    std::string req = "GET " + path + " HTTP/1.0\r\nHost: localhost\r\n\r\n";
    write_full(fd, reinterpret_cast<const uint8_t*>(req.data()), req.size());
    std::string resp;
    char buf[8192];
    for (ssize_t r; (r = ::recv(fd, buf, sizeof buf, 0)) > 0;) resp.append(buf, size_t(r));
    platform::close_socket(fd);
    auto body = resp.find("\r\n\r\n");
    return body == std::string::npos ? "" : resp.substr(body + 4);
}

}  // namespace plk
