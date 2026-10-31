// PHYLKA GUI wallet: a small local web server (127.0.0.1 only) that serves a single-page app and a JSON API.
// It is a separate process from the node. Like the CLI wallet it talks to the node over RPC and signs locally.
//
// Security model (see docs/RUNNING.md):
//  - bound to 127.0.0.1; every /api request needs the random session token (Authorization: Bearer ...)
//  - Host and Origin are checked against http://127.0.0.1:PORT and http://localhost:PORT (DNS rebinding / CSRF)
//  - state-changing endpoints are POST with Content-Type: application/json; there are no CORS headers
//  - the wallet password is needed for every signing operation and is never stored
#pragma once
#include <atomic>
#include <filesystem>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "chain.h"
#include "wallet.h"

namespace plk {

struct GuiConfig {
    const ChainParams* params = nullptr;
    std::filesystem::path wallet;
    uint16_t rpc = 0;
    uint16_t port = 0;           // 0 = pick a free port (tests)
    int request_timeout_s = 10;  // whole request (headers + body)
};

/// Comparison whose running time does not depend on where the inputs differ (for equal lengths)
bool ct_equal(const std::string& a, const std::string& b);

class GuiServer {
public:
    explicit GuiServer(GuiConfig cfg);
    ~GuiServer();
    GuiServer(const GuiServer&) = delete;
    GuiServer& operator=(const GuiServer&) = delete;

    uint16_t port() const { return port_; }
    const std::string& token() const { return token_; }
    /// Address the listening socket is actually bound to (always "127.0.0.1")
    std::string bound_address() const;
    /// http://127.0.0.1:PORT/#t=TOKEN
    std::string url() const;
    /// Single-use URL (valid 60 s) that redirects to url(); it is what is handed to the browser launcher so the token never shows up in `ps`
    std::string launch_url();

private:
    struct Worker {
        std::thread th;
        std::atomic<bool> done{false};
        int fd = -1;
    };
    struct Impl;
    void loop();
    void serve(int fd);
    void reap(bool all);

    GuiConfig cfg_;
    uint16_t port_ = 0;
    std::string token_;
    int fd_ = -1;
    std::atomic<bool> stop_{false};
    std::list<std::unique_ptr<Worker>> workers_;
    std::thread th_;
    std::unique_ptr<Impl> impl_;
};

/// `phylka gui`: runs until Ctrl+C
int gui_command(const GuiConfig& cfg, bool open_browser);

/// Checks used by `phylka selftest`; `check(ok, description)` records a result
void gui_selftest(const std::function<void(bool, const std::string&)>& check);

}  // namespace plk
