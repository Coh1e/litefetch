#include "ui_text.h"

#include <cmath>
#include <cstdio>
#include <limits>

namespace litefetch::ui {
namespace {
std::string formatBytes(double value)
{
    static constexpr const char* units[] = {"B", "KB", "MB", "GB", "TB", "PB"};
    std::size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < std::size(units)) {
        value /= 1024.0;
        ++unit;
    }
    char buffer[64];
    if (unit == 0) {
        std::snprintf(buffer, sizeof(buffer), "%lld %s", static_cast<long long>(value), units[unit]);
    } else if (value < 10.0) {
        std::snprintf(buffer, sizeof(buffer), "%.2f %s", value, units[unit]);
    } else {
        std::snprintf(buffer, sizeof(buffer), "%.0f %s", value, units[unit]);
    }
    return buffer;
}
} // namespace

std::string formatSize(std::int64_t bytes)
{
    if (bytes < 0) return std::string(unknownValue);
    return formatBytes(static_cast<double>(bytes));
}

std::string formatProgress(std::int64_t received, std::int64_t total)
{
    if (total < 0 || received < 0) return std::string(unknownValue);
    if (total == 0) return "100.0%";
    if (received > total) received = total;
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.1f%%", static_cast<double>(received) * 100.0 / static_cast<double>(total));
    return buffer;
}

std::string formatSpeed(double bytesPerSecond)
{
    if (!(bytesPerSecond > 0.0) || !std::isfinite(bytesPerSecond)) return std::string(unknownValue);
    static constexpr const char* units[] = {"B/s", "KB/s", "MB/s", "GB/s", "TB/s"};
    std::size_t unit = 0;
    while (bytesPerSecond >= 1024.0 && unit + 1 < std::size(units)) {
        bytesPerSecond /= 1024.0;
        ++unit;
    }
    char buffer[64];
    if (bytesPerSecond >= 100.0) {
        std::snprintf(buffer, sizeof(buffer), "%.0f %s", bytesPerSecond, units[unit]);
    } else {
        std::snprintf(buffer, sizeof(buffer), "%.1f %s", bytesPerSecond, units[unit]);
    }
    return buffer;
}

std::string formatRemaining(double seconds)
{
    if (!(seconds >= 0.0) || !std::isfinite(seconds)) return std::string(unknownValue);
    if (seconds > 359999.0) seconds = 359999.0;
    const long long total = static_cast<long long>(std::ceil(seconds));
    char buffer[32];
    if (total >= 3600) {
        std::snprintf(buffer, sizeof(buffer), "%lld:%02lld:%02lld", total / 3600, (total % 3600) / 60, total % 60);
    } else {
        std::snprintf(buffer, sizeof(buffer), "%02lld:%02lld", total / 60, total % 60);
    }
    return buffer;
}

std::string fileNameFromPath(std::string_view utf8Path)
{
    const auto separator = utf8Path.find_last_of("/\\");
    if (separator == std::string_view::npos) return std::string(utf8Path);
    return std::string(utf8Path.substr(separator + 1));
}

} // namespace litefetch::ui
