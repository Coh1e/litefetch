#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace litefetch {

// A time band that replaces the global speed limit between two minutes of the
// day. Bands are what make "slow during the day, full speed at night" possible
// without the user touching the setting twice a day.
struct RateBand {
    int startMinute = 0;  // minutes since midnight, inclusive
    int endMinute = 0;    // exclusive; a start greater than end wraps midnight
    std::int64_t limitBytesPerSecond = 0; // 0 means unlimited inside the band
};

// The limit that applies at `minute`, given the user's base limit. The first
// matching band wins, so a narrower band must be listed before a wider one.
std::int64_t limitAtMinute(const std::vector<RateBand>& bands, int minute,
    std::int64_t baseLimit);

// "480-1080:524288" <-> RateBand. Malformed text is rejected instead of being
// read as midnight.
bool parseRateBand(const std::string& text, RateBand& band);
std::string formatRateBand(const RateBand& band);

// Whole lines from the settings value.
std::vector<RateBand> parseRateBands(const std::string& text);
std::string formatRateBands(const std::vector<RateBand>& bands);

} // namespace litefetch
