#include "traffic_meter.h"

namespace litefetch {

void TrafficMeter::add(TrafficCategory category, std::int64_t bytes)
{
    if (bytes <= 0) return;
    std::lock_guard lock(mutex_);
    bytes_[static_cast<std::size_t>(category)] += bytes;
}

std::int64_t TrafficMeter::bytes(TrafficCategory category) const
{
    std::lock_guard lock(mutex_);
    return bytes_[static_cast<std::size_t>(category)];
}

std::int64_t TrafficMeter::total() const
{
    std::lock_guard lock(mutex_);
    std::int64_t sum = 0;
    for (const auto value : bytes_) sum += value;
    return sum;
}

void TrafficMeter::reset()
{
    std::lock_guard lock(mutex_);
    bytes_.fill(0);
}

const char* TrafficMeter::name(TrafficCategory category)
{
    switch (category) {
    case TrafficCategory::Download: return "download";
    case TrafficCategory::Upload: return "upload";
    case TrafficCategory::Metadata: return "metadata";
    case TrafficCategory::Verify: return "verify";
    }
    return "unknown";
}

} // namespace litefetch
