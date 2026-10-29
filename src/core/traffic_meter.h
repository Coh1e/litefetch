#pragma once

#include <array>
#include <cstdint>
#include <mutex>
#include <string>

namespace litefetch {

// Traffic is not all the same: bytes fetched for the user's file, bytes we send
// while sharing, bytes spent fetching torrent metadata and bytes read back to
// verify a finished file. They are counted apart so the totals can be explained
// instead of merged into one number.
enum class TrafficCategory { Download = 0, Upload = 1, Metadata = 2, Verify = 3 };

class TrafficMeter {
public:
    void add(TrafficCategory category, std::int64_t bytes);
    std::int64_t bytes(TrafficCategory category) const;
    std::int64_t total() const;
    void reset();

    static const char* name(TrafficCategory category);

private:
    mutable std::mutex mutex_;
    std::array<std::int64_t, 4> bytes_{};
};

} // namespace litefetch
