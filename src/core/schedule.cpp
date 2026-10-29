#include "schedule.h"

#include <charconv>
#include <string_view>

namespace litefetch {
namespace {
bool parseNumber(std::string_view text, long long& value)
{
    if (text.empty()) return false;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

// Minutes are a day position; 1440 is allowed so "22:00-24:00" is expressible.
bool validMinute(long long minute)
{
    return minute >= 0 && minute <= 1440;
}
} // namespace

std::int64_t limitAtMinute(const std::vector<RateBand>& bands, int minute,
    std::int64_t baseLimit)
{
    const int now = ((minute % 1440) + 1440) % 1440;
    for (const auto& band : bands) {
        bool inside = false;
        if (band.startMinute <= band.endMinute) {
            inside = now >= band.startMinute && now < band.endMinute;
        } else {
            // The band wraps midnight, e.g. 23:00-06:00.
            inside = now >= band.startMinute || now < band.endMinute;
        }
        if (inside) return band.limitBytesPerSecond;
    }
    return baseLimit;
}

bool parseRateBand(const std::string& text, RateBand& band)
{
    const auto colon = text.rfind(':');
    if (colon == std::string::npos) return false;
    const auto dash = text.find('-');
    if (dash == std::string::npos || dash > colon) return false;

    long long start = 0;
    long long end = 0;
    long long limit = 0;
    if (!parseNumber(std::string_view(text).substr(0, dash), start)) return false;
    if (!parseNumber(std::string_view(text).substr(dash + 1, colon - dash - 1), end)) return false;
    if (!parseNumber(std::string_view(text).substr(colon + 1), limit)) return false;
    if (!validMinute(start) || !validMinute(end) || start == end) return false;
    if (limit < 0) return false;

    band.startMinute = static_cast<int>(start);
    band.endMinute = static_cast<int>(end);
    band.limitBytesPerSecond = limit;
    return true;
}

std::string formatRateBand(const RateBand& band)
{
    return std::to_string(band.startMinute) + "-" + std::to_string(band.endMinute) + ":"
        + std::to_string(band.limitBytesPerSecond);
}

std::vector<RateBand> parseRateBands(const std::string& text)
{
    std::vector<RateBand> bands;
    std::size_t start = 0;
    while (start < text.size()) {
        auto end = text.find('\n', start);
        auto line = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        RateBand band;
        if (!line.empty() && parseRateBand(line, band)) bands.push_back(band);
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return bands;
}

std::string formatRateBands(const std::vector<RateBand>& bands)
{
    std::string text;
    for (const auto& band : bands) {
        if (!text.empty()) text += '\n';
        text += formatRateBand(band);
    }
    return text;
}

std::optional<ScheduledAction> actionAtMinute(const std::vector<ScheduledAction>& actions,
    int minute)
{
    const int now = ((minute % 1440) + 1440) % 1440;
    for (const auto& action : actions) {
        if (action.minute == now) return action;
    }
    return std::nullopt;
}

bool shouldFire(int lastFiredMinute, int currentMinute, const ScheduledAction& action)
{
    if (action.minute != currentMinute) return false;
    return lastFiredMinute != currentMinute;
}

bool parseScheduledAction(const std::string& text, ScheduledAction& action)
{
    const auto colon = text.rfind(':');
    if (colon == std::string::npos) return false;
    long long minute = 0;
    if (!parseNumber(std::string_view(text).substr(0, colon), minute)) return false;
    if (minute < 0 || minute > 1439) return false;
    const auto flag = text.substr(colon + 1);
    if (flag != "s" && flag != "p") return false;
    action.minute = static_cast<int>(minute);
    action.start = flag == "s";
    return true;
}

std::string formatScheduledAction(const ScheduledAction& action)
{
    return std::to_string(action.minute) + (action.start ? ":s" : ":p");
}

std::vector<ScheduledAction> parseScheduledActions(const std::string& text)
{
    std::vector<ScheduledAction> actions;
    std::size_t start = 0;
    while (start < text.size()) {
        auto end = text.find('\n', start);
        auto line = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        ScheduledAction action;
        if (!line.empty() && parseScheduledAction(line, action)) actions.push_back(action);
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return actions;
}

std::string formatScheduledActions(const std::vector<ScheduledAction>& actions)
{
    std::string text;
    for (const auto& action : actions) {
        if (!text.empty()) text += '\n';
        text += formatScheduledAction(action);
    }
    return text;
}

} // namespace litefetch
