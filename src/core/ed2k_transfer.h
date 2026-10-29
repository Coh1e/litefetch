#pragma once

#include "rate_limit.h"

#include <atomic>
#include <memory>
#include <utility>
#include <cstdint>
#include <string>

namespace litefetch {

// ed2k splits a file into 9,728,000-byte parts and hashes each part with MD4;
// the file hash is the MD4 of the concatenated part hashes. Both are needed to
// recognise a finished download and to verify what a peer sent.
constexpr std::int64_t ed2kPartSize = 9728000;

// MD4 digest of `bytes`, lowercase hex. Used for part hashes and the root.
std::string md4Hex(const void* data, std::size_t size);

// ed2k file hash of a finished file: MD4 over the part digests. Empty when the
// file cannot be read.
std::string ed2kFileHash(const std::string& path);

struct Ed2kPeerRequest {
    std::string host;
    std::uint16_t port = 0;
    std::string fileHash;   // 32 hex characters
    std::int64_t fileSize = 0;
    std::string partPath;   // where the bytes are written
    int timeoutMillis = 30000;
    // Optional shared budget: received blocks are charged to it.
    std::shared_ptr<RateBudget> budget;
};

struct Ed2kPeerResult {
    bool ok = false;
    std::int64_t received = 0;
    std::string error;
};

// Byte range of one part, inclusive at both ends. Exposed so the 64-bit math
// can be checked without transferring a 4 GiB file.
std::pair<std::int64_t, std::int64_t> ed2kPartRange(int index, std::int64_t fileSize);

// Downloads one file from one peer over the eDonkey peer protocol on the
// calling thread. Parts that a previous run already finished are kept only if
// their recorded MD4 still matches the bytes on disk, and only the missing
// range is requested. When `fileHash` is set, the finished file must reproduce
// it or the transfer fails.
Ed2kPeerResult downloadFromEd2kPeer(const Ed2kPeerRequest& request,
    const std::atomic_bool& cancelled);

// What a resumed transfer found on disk, for reporting and tests.
struct Ed2kResumeState {
    int verifiedParts = 0;
    int missingParts = 0;
    std::int64_t missingBytes = 0;
};

Ed2kResumeState inspectEd2kParts(const std::string& partPath, std::int64_t fileSize,
    const std::string& fileHash);

} // namespace litefetch
