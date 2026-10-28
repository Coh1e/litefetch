#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace litefetch {

// An ed2k file link: ed2k://|file|<name>|<size>|<hash>|/
struct Ed2kFileLink {
    std::string name;
    std::int64_t size = 0;
    // The ed2k hash is the MD4-based root hash of the part tree, hex encoded.
    std::string hash;
};

struct Ed2kLinkResult {
    bool ok = false;
    Ed2kFileLink link;
    std::string error;
};

// Rejects server links (ed2k://|server|...), malformed hashes and sizes that
// cannot be a real file, so a bad link never reaches the network layer.
Ed2kLinkResult parseEd2kLink(std::string_view link);

// One peer the server knows about. The server sends a 32-bit id that carries
// three address bytes and the high byte of the peer port; the low port byte is
// not transmitted, which is a limitation of the 32-bit server source record.
struct Ed2kSource {
    std::uint32_t id = 0;
    std::string address;
    std::uint16_t port = 0;
};

Ed2kSource decodeEd2kSource(std::uint32_t id);

} // namespace litefetch
