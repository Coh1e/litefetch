#pragma once

#include "kad_routing.h"
#include "kad_wire.h"

#include <atomic>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <string>
#include <vector>

namespace litefetch {

// A Kad node on a UDP socket. It answers bootstrap, hello and find requests and
// can ask other nodes for contacts. Iterative lookups walk the contacts it
// already knows; a full Kademlia lookup over a live network is a later step.
class KadNode {
public:
    explicit KadNode(KadId id = KadId::random());
    ~KadNode();
    KadNode(const KadNode&) = delete;
    KadNode& operator=(const KadNode&) = delete;

    // Binds a UDP socket; port 0 picks a free one.
    bool start(std::uint16_t port, std::string& error);
    void stop();
    std::uint16_t port() const { return port_; }
    const KadId& id() const { return table_.self(); }
    KadRoutingTable& table() { return table_; }

    // Sends one bootstrap request and merges the answer. False on timeout with
    // the reason in `error`.
    bool bootstrap(const std::string& host, std::uint16_t port, int timeoutMillis,
        std::string& error);
    // Hello to every known contact; returns how many answered.
    int refresh(int timeoutMillis);
    // Asks the closest known contacts for contacts near `target`.
    std::vector<KadContact> findNode(const KadId& target, std::uint8_t wanted,
        int timeoutMillis);

    // Announces that `contact` has the file with `fileHash`: stored locally and
    // sent to the closest known nodes. Returns how many nodes acknowledged.
    int publishSource(const KadId& fileHash, const KadContact& contact, int timeoutMillis);
    // Asks the known nodes for sources of `fileHash`; local entries included.
    std::vector<KadContact> findSources(const KadId& fileHash, int timeoutMillis);
    bool saveContacts(const std::string& path) const;
    bool loadContacts(const std::string& path);

private:
    // Waits for one datagram. Requests are answered on the spot, responses are
    // queued for whichever query is waiting for them.
    bool pollOnce(int timeoutMillis);
    void handle(const KadPacket& packet, const std::string& fromAddress, std::uint16_t fromPort);
    bool send(const KadPacket& packet, const std::string& address, std::uint16_t port);
    // Pops the oldest response with this opcode, waiting until the deadline.
    bool takeResponse(KadOpcode opcode, KadPacket& packet, int timeoutMillis);
    void receiveLoop();

    KadRoutingTable table_;
    std::uintptr_t socket_ = 0;
    std::uint16_t port_ = 0;
    std::atomic_bool stopping_{false};
    std::thread receiver_;
    std::mutex storeMutex_;
    std::map<std::string, std::vector<KadContact>> sources_;
    std::mutex responseMutex_;
    std::deque<KadPacket> responses_;
};

} // namespace litefetch
