#pragma once

#include <cstdint>
#include <optional>
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

// Something to do at a minute of the day: start every queued task, or pause
// everything that is running.
struct ScheduledAction {
    int minute = 0;
    bool start = true; // false means pause
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

// The action due at `minute`, if any. A minute without an entry does nothing.
std::optional<ScheduledAction> actionAtMinute(const std::vector<ScheduledAction>& actions,
    int minute);

// True when the action for this minute has not fired yet. The minute itself is
// the reminder, so the same minute never fires twice.
bool shouldFire(int lastFiredMinute, int currentMinute, const ScheduledAction& action);

// "480:s" / "1080:p" <-> ScheduledAction.
bool parseScheduledAction(const std::string& text, ScheduledAction& action);
std::string formatScheduledAction(const ScheduledAction& action);
std::vector<ScheduledAction> parseScheduledActions(const std::string& text);
std::string formatScheduledActions(const std::vector<ScheduledAction>& actions);

} // namespace litefetch
