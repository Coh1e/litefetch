#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace litefetch::ui {

// One line that explains where the traffic went, so a download total is never
// confused with the bytes we sent or read back to verify a file.
std::string trafficSummary(const std::array<std::int64_t, 4>& totals);

} // namespace litefetch::ui
