#pragma once

#include "browser_bridge.h"

#include <atomic>
#include <string>
#include <thread>

namespace litefetch::browser {

// Listens on the local channel and hands each request to the bridge. One
// connection carries one request and one answer, which is what the native
// messaging host needs and keeps the server simple enough to reason about.
class BrowserPipeServer {
public:
    explicit BrowserPipeServer(std::string pipeName);
    ~BrowserPipeServer();
    BrowserPipeServer(const BrowserPipeServer&) = delete;
    BrowserPipeServer& operator=(const BrowserPipeServer&) = delete;

    // Starts the listening thread. The bridge must outlive the server.
    bool start(BrowserBridge& bridge, std::string& error);
    void stop();

private:
    void serve(BrowserBridge& bridge);

    std::string pipeName_;
    std::atomic_bool stopping_{false};
    std::thread thread_;
    std::atomic<std::uintptr_t> listener_{0};
};

} // namespace litefetch::browser
