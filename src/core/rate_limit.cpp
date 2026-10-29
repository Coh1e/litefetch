#include "rate_limit.h"

#include <algorithm>
#include <thread>

namespace litefetch {

void RateBudget::setLimit(std::int64_t bytesPerSecond)
{
    std::lock_guard lock(mutex_);
    limit_ = std::max<std::int64_t>(0, bytesPerSecond);
    // Start from a full second of tokens so a small transfer is not delayed by
    // an empty bucket at startup.
    tokens_ = static_cast<double>(limit_);
    last_ = Clock::now();
}

std::int64_t RateBudget::limit() const
{
    std::lock_guard lock(mutex_);
    return limit_;
}

void RateBudget::refillLocked()
{
    const auto now = Clock::now();
    const auto elapsed = std::chrono::duration<double>(now - last_).count();
    last_ = now;
    if (limit_ <= 0) return;
    tokens_ = std::min(static_cast<double>(limit_), tokens_ + elapsed * static_cast<double>(limit_));
}

void RateBudget::refill()
{
    std::lock_guard lock(mutex_);
    refillLocked();
}

void RateBudget::spend(std::int64_t bytes)
{
    if (bytes <= 0) return;
    {
        std::lock_guard lock(mutex_);
        refillLocked();
        if (limit_ <= 0) return;
        // Charge once; the loop below only waits for the bucket to catch up.
        tokens_ -= static_cast<double>(bytes);
    }
    for (;;) {
        std::int64_t waitMicros = 0;
        {
            std::lock_guard lock(mutex_);
            refillLocked();
            if (limit_ <= 0) return;
            if (tokens_ >= 0) return;
            // How long until the deficit is paid off.
            const auto deficit = -tokens_;
            waitMicros = static_cast<std::int64_t>(deficit / static_cast<double>(limit_) * 1'000'000.0);
        }
        // A large request is paid off in steps so a cancelled transfer never
        // waits for the whole amount at once.
        if (waitMicros > 50'000) waitMicros = 50'000;
        if (waitMicros <= 0) waitMicros = 1'000;
        std::this_thread::sleep_for(std::chrono::microseconds(waitMicros));
    }
}

} // namespace litefetch
