// PHYLKA networking.
//  P2P (TCP): every message is  magic(4) | command(12) | length(4) | checksum(4) | payload.
//    version  u32 height          -> the other side learns how far we are
//    getblock u32 height          -> "block" from the sender's active chain
//    block    raw block           -> validated; a missing parent is requested by height, walking back to a fork point
//    inv      u32 height + hash   -> announcement of a new tip
//    tx       raw transaction     -> mempool, relayed further
//    vote     validator vote      -> checked against the stake snapshot, may finalize a checkpoint, relayed
//    reg      xonly key + ML-DSA key (1344 bytes) -> validator key registration, queued for the next block (after the upgrade)
//    evid     equivocation evidence (5068 bytes) -> queued for an on-chain slash
//    getvotes u32 height          -> known votes from that height (at most 500 per request, oldest first)
//    ping/pong u64 nonce          -> keep-alive every 120 s; 300 s of silence drops the peer
//  version is u32 height + u64 random nonce (the nonce detects self- and duplicate connections; the old 4-byte form is accepted).
//  A peer must send "version" first within 10 s. Payload sizes are capped per command before anything is allocated,
//  misbehaviour is scored per peer (>= 100 -> disconnect and a 24 h IP ban; loopback is never banned), and message rates are limited.
//  RPC (HTTP on 127.0.0.1 only, plain text): /info /utxos?addr= /history?addr= /send?tx= /block?height=
#pragma once
#include <atomic>
#include <deque>
#include <functional>
#include <list>
#include <map>
#include <set>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "node.h"

namespace plk {

std::optional<Block> block_from_bytes(const Bytes& b);

class Net {
public:
    Net(Node& node, uint16_t port, std::vector<std::string> connect_to, bool listen_all);
    /// Known public peers (address book size), for tests and /info
    size_t known_addresses();
    ~Net();
    void broadcast_block(const Block& b);
    void broadcast_tx(const Transaction& t);
    void broadcast_vote(const Vote& v);
    void broadcast_reg(const PublicKey& k);
    void broadcast_evidence(const Evidence& e);
    size_t peers();
    /// Accepts a block (from the miner or a peer) and announces it if it became the tip
    void submit_block(const Block& b);

private:
    /// Token bucket: `rate` tokens per second up to `burst`
    struct Bucket {
        double rate, burst, tokens;
        double last = 0;
        Bucket(double r, double b) : rate(r), burst(b), tokens(b) {}
        bool take(double now);
    };
    struct Peer {
        Peer() : tx(50, 200), vote(50, 300), getblock(20, 50), getvotes(0.1, 1), addr(0.2, 3), getaddr(0.05, 2) {}
        ~Peer();
        int fd = -1;                  // closed in the destructor, so a broadcaster holding a shared_ptr never sees a recycled fd
        std::string name, ip;
        std::mutex wm;                // guards writes and `want`
        std::atomic<uint32_t> their_height{0};
        std::deque<uint32_t> want;    // heights to request again after a missing parent arrives (capped)
        std::atomic<bool> alive{true}, done{false};
        std::atomic<uint64_t> nonce{0};
        std::atomic<int> score{0};
        std::atomic<bool> got_version{false};
        bool outbound = false;
        std::thread th;
        // touched by the peer's own thread only
        double t_start = 0, last_recv = 0, msg_start = 0, last_ping = 0, last_gv = -1e9, last_hint = -1e9, last_probe = -1e9, last_gb = 0, req_t = 0;
        uint32_t probe_h = 0;   // their tip height we last probed although it is not above ours
        uint32_t req_h = 0;   // height of the getblock in flight (0 = none)
        uint64_t gv_tip = ~0ULL;
        uint32_t gv_fin = 0;
        Bucket tx, vote, getblock, getvotes, addr, getaddr;
        std::atomic<uint16_t> listen_port{0};   // advertised in version (0 = does not accept connections)
    };
    void listen_loop();
    void connect_loop();
    bool start_peer(int fd, const std::string& name, const std::string& ip, bool outbound);
    void reap();
    void peer_loop(Peer* p);
    bool recv_n(Peer& p, uint8_t* b, size_t n);
    bool tick(Peer& p);
    void send(Peer& p, const std::string& cmd, const Bytes& payload);
    void handle(Peer& p, const std::string& cmd, const Bytes& payload);
    void on_version(Peer& p, const Bytes& payload);
    void on_block(Peer& p, const Bytes& payload);
    void on_vote(Peer& p, const Bytes& payload);
    void on_getaddr(Peer& p);
    void on_addr(Peer& p, const Bytes& payload);
    // address book (peer discovery)
    bool addr_ok(const std::string& ip) const;
    void book_add(const std::string& ip, uint16_t port, time_t seen);
    std::vector<std::string> book_sample(size_t n);
    void book_load();
    void book_save();
    void dial_from_book();
    void on_reg(Peer& p, const Bytes& payload);
    void on_evid(Peer& p, const Bytes& payload);
    void on_tx(Peer& p, const Bytes& payload);
    void on_getvotes(Peer& p, const Bytes& payload);
    void request_next(Peer& p);
    void send_getblock(Peer& p, uint32_t height);
    void misbehave(Peer& p, int points, const std::string& why);
    void drop(Peer& p);
    void ban(const std::string& ip);
    bool is_banned(const std::string& ip);
    std::vector<std::shared_ptr<Peer>> snapshot();

    Node& node_;
    uint16_t port_;
    bool listen_all_;
    std::vector<std::string> connect_to_;
    uint64_t nonce_ = 0;                          // identifies this process in "version"
    std::mutex pm_;
    std::vector<std::shared_ptr<Peer>> peers_;
    std::set<std::string> self_targets_;          // --connect targets that turned out to be ourselves
    std::mutex am_;
    std::map<std::string, time_t> book_;          // "ip:port" -> last seen (bounded, persisted in peers.dat)
    bool book_dirty_ = false;
    double book_saved_ = 0;
    std::mutex bm_;
    std::map<std::string, time_t> bans_;          // ip -> ban expiry
    std::atomic<bool> stop_{false};
    std::vector<std::thread> threads_;            // listen + connect loops
};

class Rpc {
public:
    Rpc(Node& node, Net& net, uint16_t port, std::function<double()> hashrate);
    ~Rpc();

private:
    struct Worker {
        std::thread th;
        std::atomic<bool> done{false};
        int fd = -1;
    };
    void loop();
    void serve(int fd);
    void reap(bool all);
    std::string handle(const std::string& path);
    Node& node_;
    Net& net_;
    std::function<double()> hashrate_;
    int fd_ = -1;
    std::atomic<bool> stop_{false};
    std::list<std::unique_ptr<Worker>> workers_;   // loop thread only (and the destructor after it is joined)
    std::thread th_;
};

/// Tiny HTTP GET client for the wallet: returns the body or throws
std::string http_get(uint16_t port, const std::string& path);

}  // namespace plk
