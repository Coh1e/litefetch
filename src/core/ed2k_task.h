#pragma once

#include "ed2k_client.h"
#include "ed2k_link.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace litefetch {

struct Ed2kTaskRequest {
    std::string link;         // ed2k://|file|name|size|hash|/
    std::string destination;  // final path; the bytes land in <destination>.part first
    // Servers to ask for sources. At least one is needed unless a peer is given.
    std::vector<Ed2kServerEndpoint> servers;
    // Peers to try in order without asking a server: manual entry, and the only
    // way a test can point at scripted peers. The first one that serves wins.
    std::vector<Ed2kServerEndpoint> peers;
    int timeoutMillis = 15000;
};

struct Ed2kTaskResult {
    bool ok = false;
    std::int64_t received = 0;
    std::string error;
    // True when at least one server answered but knew no sources, which is a
    // different situation from every server failing.
    bool noSources = false;
};

// One file, one peer, one file on disk: parse the link, find a peer (servers or
// the explicit one), transfer with part resume, verify the ed2k hash, then move
// the finished part file into place.
Ed2kTaskResult downloadEd2kTask(const Ed2kTaskRequest& request,
    const std::atomic_bool& cancelled,
    const std::function<void(std::int64_t received, std::int64_t total)>& progress = {});

} // namespace litefetch
