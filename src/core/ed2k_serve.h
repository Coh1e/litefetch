#pragma once

#include <atomic>
#include <cstdint>
#include <string>

namespace litefetch {

// A minimal eDonkey peer that serves exactly one file. It exists so two
// LiteFetch processes can form their own ed2k network — a seeder and a
// downloader — without any third-party client or server in the loop.
class Ed2kPeerServer {
public:
    Ed2kPeerServer(std::string filePath, std::string fileHash, std::uint16_t port);
    ~Ed2kPeerServer();
    Ed2kPeerServer(const Ed2kPeerServer&) = delete;
    Ed2kPeerServer& operator=(const Ed2kPeerServer&) = delete;

    // Binds the port and starts the accept loop.
    bool start(std::string& error);
    std::uint16_t port() const { return port_; }
    // Bytes actually handed to peers, for upload accounting.
    std::int64_t uploadedBytes() const { return uploaded_.load(); }
    void stop();

private:
    void serve();
    void handleClient(std::uintptr_t client);

    std::string filePath_;
    std::string fileHash_;
    std::uint16_t port_ = 0;
    std::uintptr_t listener_ = 0;
    std::atomic_bool stopping_{false};
    std::atomic<std::int64_t> uploaded_{0};
    struct Thread;
    Thread* thread_ = nullptr;
};

} // namespace litefetch
