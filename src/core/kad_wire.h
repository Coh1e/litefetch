#pragma once

#include "kad_routing.h"

#include <cstdint>
#include <string>
#include <vector>

namespace litefetch {

// Kad packets are UDP datagrams that start with 0xE4 followed by an opcode.
// This subset covers bootstrapping, a liveness hello and a find-node walk; the
// payload shapes are documented in docs/specs/kad-subset.md.
enum class KadOpcode : std::uint8_t {
    BootstrapRequest = 0x01,
    BootstrapResponse = 0x09,
    HelloRequest = 0x11,
    HelloResponse = 0x19,
    FindRequest = 0x21,
    FindResponse = 0x29,
};

struct KadPacket {
    KadOpcode opcode = KadOpcode::HelloRequest;
    // Contact lists carried by every packet.
    std::vector<KadContact> contacts;
    // Find requests carry the target and how many contacts are wanted.
    KadId target;
    std::uint8_t wanted = 0;
};

// 0xE4 + opcode + 1-byte contact count + 22 bytes per contact, then the
// opcode specific part.
std::vector<std::uint8_t> encodeKadPacket(const KadPacket& packet);

// Returns false for a datagram that is not a Kad packet or is truncated.
bool decodeKadPacket(const std::uint8_t* data, std::size_t size, KadPacket& packet);

std::string describeKadOpcode(KadOpcode opcode);

} // namespace litefetch
