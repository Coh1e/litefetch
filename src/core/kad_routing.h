#pragma once

#include "kad_id.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace litefetch {

struct KadContact {
    KadId id;
    std::string address;
    std::uint16_t port = 0;
    // Wall clock so the table survives a restart with meaningful ages.
    std::int64_t lastSeenUnix = 0;
};

// Kademlia routing table: one bucket per shared-prefix length, at most
// `bucketSize` contacts each, oldest first so eviction is cheap. A peer that is
// heard from again moves to the back of its bucket.
class KadRoutingTable {
public:
    static constexpr std::size_t bucketSize = 10;

    explicit KadRoutingTable(KadId self = {}) : self_(self) {}

    void setSelf(const KadId& self) { self_ = self; }
    const KadId& self() const { return self_; }

    // Returns true when the contact was stored or refreshed.
    bool add(const KadContact& contact, std::int64_t nowUnix);
    // Removes a contact that stopped answering.
    bool remove(const KadId& id);
    std::size_t size() const;
    // The `count` contacts nearest to `target`, nearest first.
    std::vector<KadContact> closest(const KadId& target, std::size_t count) const;
    const std::vector<KadContact>& bucket(std::size_t index) const { return buckets_[index]; }

    // "id address port lastSeen" per line.
    std::string serialize() const;
    bool load(const std::string& text);

private:
    std::size_t bucketIndexFor(const KadId& id) const;

    KadId self_;
    std::vector<std::vector<KadContact>> buckets_ = std::vector<std::vector<KadContact>>(129);
};

} // namespace litefetch
