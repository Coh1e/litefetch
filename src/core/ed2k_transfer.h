#pragma once

#include <atomic>
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
};

struct Ed2kPeerResult {
    bool ok = false;
    std::int64_t received = 0;
    std::string error;
};

// Downloads one file from one peer over the eDonkey peer protocol on the
// calling thread, writing into `partPath` at the offsets the peer reports.
// The caller verifies the finished file; this function only moves bytes.
Ed2kPeerResult downloadFromEd2kPeer(const Ed2kPeerRequest& request,
    const std::atomic_bool& cancelled);

} // namespace litefetch
