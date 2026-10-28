#pragma once

#include "http_engine.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace litefetch {

// What the server said about the resource before any bytes are written.
struct SegmentProbe {
    bool ok = false;
    bool ranges = false;
    std::int64_t total = -1;
    std::string etag;
    std::string lastModified;
    std::string host;
    std::string message;
};

// HEAD probe used to decide between one stream and several ranges. A failed or
// unusual probe is not an error: the caller falls back to a single stream.
SegmentProbe probeForSegments(const std::string& url, const std::string& username,
    const std::string& password, const std::atomic_bool& cancelled);

// One connection per range, each writing its own segment file, merged in order
// when every range arrives. Partial segments are merged as a prefix so a later
// single-stream start resumes from the bytes that really arrived.
TransferResult transferSegmented(const std::string& url, const std::string& username,
    const std::string& password, const std::filesystem::path& destination,
    const std::filesystem::path& partial, const std::filesystem::path& metaPath,
    const SegmentProbe& probe, int segments, const std::atomic_bool& cancelled,
    const std::function<void(const TransferProgressInfo&)>& progress);

} // namespace litefetch
