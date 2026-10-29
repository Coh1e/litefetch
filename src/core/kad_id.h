#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace litefetch {

// Kad node identifiers are 128 bits. Distance is the XOR of two ids, compared
// as a big-endian number, which is the whole geometry Kademlia needs.
struct KadId {
    std::array<std::uint8_t, 16> bytes{};

    static KadId random();
    static bool fromHex(std::string_view hex, KadId& out);
    std::string toHex() const;
    bool isZero() const;
    // Leading zero bits, used to pick the routing bucket.
    int leadingZeroBits() const;
};

bool operator==(const KadId& left, const KadId& right);
bool operator!=(const KadId& left, const KadId& right);
// True when `left` is closer to `target` than `right` (XOR distance).
bool closerTo(const KadId& target, const KadId& left, const KadId& right);

} // namespace litefetch
