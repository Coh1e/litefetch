#include "kad_routing.h"

#include <algorithm>
#include <sstream>

namespace litefetch {
namespace {
// Contacts older than this are replaced by a newcomer when their bucket is full.
constexpr std::int64_t staleSeconds = 15 * 60;
} // namespace

std::size_t KadRoutingTable::bucketIndexFor(const KadId& id) const
{
    // The bucket holds peers that share a prefix with us up to the first
    // differing bit; 128 is the "identical id" case.
    std::size_t index = 0;
    for (; index < 16; ++index) {
        const auto difference = static_cast<std::uint8_t>(id.bytes[index] ^ self_.bytes[index]);
        if (difference == 0) continue;
        for (int bit = 7; bit >= 0; --bit) {
            if ((difference >> bit) & 1) return static_cast<std::size_t>(index * 8 + (7 - bit));
        }
    }
    return 128;
}

bool KadRoutingTable::add(const KadContact& contact, std::int64_t nowUnix)
{
    if (contact.id == self_ || contact.port == 0 || contact.address.empty()) return false;
    auto& bucket = buckets_[bucketIndexFor(contact.id)];
    const auto found = std::find_if(bucket.begin(), bucket.end(),
        [&](const KadContact& existing) { return existing.id == contact.id; });
    if (found != bucket.end()) {
        found->address = contact.address;
        found->port = contact.port;
        found->lastSeenUnix = nowUnix;
        // Keep the most recently heard peer at the back.
        std::rotate(found, found + 1, bucket.end());
        return true;
    }
    if (bucket.size() < bucketSize) {
        bucket.push_back(contact);
        return true;
    }
    // Full bucket: only a stale head is worth replacing.
    if (nowUnix - bucket.front().lastSeenUnix < staleSeconds) return false;
    bucket.erase(bucket.begin());
    bucket.push_back(contact);
    return true;
}

bool KadRoutingTable::remove(const KadId& id)
{
    auto& bucket = buckets_[bucketIndexFor(id)];
    const auto found = std::find_if(bucket.begin(), bucket.end(),
        [&](const KadContact& existing) { return existing.id == id; });
    if (found == bucket.end()) return false;
    bucket.erase(found);
    return true;
}

std::size_t KadRoutingTable::size() const
{
    std::size_t total = 0;
    for (const auto& bucket : buckets_) total += bucket.size();
    return total;
}

std::vector<KadContact> KadRoutingTable::closest(const KadId& target, std::size_t count) const
{
    std::vector<KadContact> all;
    for (const auto& bucket : buckets_)
        all.insert(all.end(), bucket.begin(), bucket.end());
    std::sort(all.begin(), all.end(), [&](const KadContact& left, const KadContact& right) {
        return closerTo(target, left.id, right.id);
    });
    if (all.size() > count) all.resize(count);
    return all;
}

std::string KadRoutingTable::serialize() const
{
    std::ostringstream out;
    for (const auto& bucket : buckets_) {
        for (const auto& contact : bucket) {
            out << contact.id.toHex() << ' ' << contact.address << ' ' << contact.port << ' '
                << contact.lastSeenUnix << '\n';
        }
    }
    return out.str();
}

bool KadRoutingTable::load(const std::string& text)
{
    std::istringstream input(text);
    std::string idText;
    std::string address;
    int port = 0;
    std::int64_t lastSeen = 0;
    while (input >> idText >> address >> port >> lastSeen) {
        KadContact contact;
        if (!KadId::fromHex(idText, contact.id)) return false;
        contact.address = address;
        contact.port = static_cast<std::uint16_t>(port);
        contact.lastSeenUnix = lastSeen;
        if (port <= 0 || port > 65535) continue;
        // A loaded contact is old by definition; clamp so a full bucket can
        // still make room for live peers.
        add(contact, lastSeen);
    }
    return true;
}

} // namespace litefetch
