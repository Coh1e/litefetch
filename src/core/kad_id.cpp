#include "kad_id.h"

#include <algorithm>
#include <cstdio>
#include <random>

namespace litefetch {

KadId KadId::random()
{
    static thread_local std::mt19937_64 engine{std::random_device{}()};
    KadId id;
    for (std::size_t offset = 0; offset < id.bytes.size(); offset += 8) {
        const auto value = engine();
        for (int index = 0; index < 8; ++index)
            id.bytes[offset + index] = static_cast<std::uint8_t>((value >> (index * 8)) & 0xff);
    }
    return id;
}

bool KadId::fromHex(std::string_view hex, KadId& out)
{
    if (hex.size() != 32) return false;
    for (std::size_t index = 0; index < 16; ++index) {
        const auto pair = std::string(hex.substr(index * 2, 2));
        try {
            out.bytes[index] = static_cast<std::uint8_t>(std::stoul(pair, nullptr, 16));
        } catch (...) {
            return false;
        }
    }
    return true;
}

std::string KadId::toHex() const
{
    static constexpr char digits[] = "0123456789abcdef";
    std::string text;
    text.reserve(32);
    for (const auto byte : bytes) {
        text.push_back(digits[byte >> 4]);
        text.push_back(digits[byte & 0x0f]);
    }
    return text;
}

bool KadId::isZero() const
{
    for (const auto byte : bytes)
        if (byte != 0) return false;
    return true;
}

int KadId::leadingZeroBits() const
{
    int count = 0;
    for (const auto byte : bytes) {
        if (byte == 0) {
            count += 8;
            continue;
        }
        for (int bit = 7; bit >= 0; --bit) {
            if ((byte >> bit) & 1) return count;
            ++count;
        }
    }
    return count;
}

bool operator==(const KadId& left, const KadId& right)
{
    return left.bytes == right.bytes;
}

bool operator!=(const KadId& left, const KadId& right)
{
    return !(left == right);
}

bool closerTo(const KadId& target, const KadId& left, const KadId& right)
{
    for (std::size_t index = 0; index < 16; ++index) {
        const auto leftDistance = static_cast<std::uint8_t>(target.bytes[index] ^ left.bytes[index]);
        const auto rightDistance = static_cast<std::uint8_t>(target.bytes[index] ^ right.bytes[index]);
        if (leftDistance != rightDistance) return leftDistance < rightDistance;
    }
    return false;
}

} // namespace litefetch
