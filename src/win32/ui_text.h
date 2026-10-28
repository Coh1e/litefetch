#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace litefetch::ui {

// The design shows an em dash for values the engine does not know. Unknown and
// known zero must stay distinguishable: only negative/absent values use it.
inline constexpr std::string_view unknownValue = "\xE2\x80\x94";

std::string formatSize(std::int64_t bytes);
std::string formatProgress(std::int64_t received, std::int64_t total);
std::string formatSpeed(double bytesPerSecond);
std::string formatRemaining(double seconds);
std::string fileNameFromPath(std::string_view utf8Path);
// Local time in the fixed YYYY-MM-DD HH:MM form; non-positive timestamps are
// unknown. Times shown in the list come from the persisted task row only.
std::string formatDateTime(std::int64_t epochMillis);

} // namespace litefetch::ui
