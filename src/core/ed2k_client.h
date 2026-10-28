#pragma once

#include "ed2k_link.h"

#include <cstdint>
#include <string>
#include <vector>

namespace litefetch {

struct Ed2kServerEndpoint {
    std::string host;
    std::uint16_t port = 0;
};

struct Ed2kSourceQuery {
    bool ok = false;
    // The server accepted the login and answered the query. Zero sources is a
    // normal answer, not a failure.
    std::vector<Ed2kSource> sources;
    std::uint32_t assignedId = 0;
    std::string error;
};

// Talks the eDonkey server protocol on the calling thread: connect, login,
// ask for the sources of one file, disconnect. It never downloads file data.
Ed2kSourceQuery queryEd2kSources(const Ed2kServerEndpoint& server, const Ed2kFileLink& file,
    int timeoutMillis);

} // namespace litefetch
