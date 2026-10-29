#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>

namespace litefetch {

// One byte budget shared by every engine. A transfer asks for the bytes it just
// received and blocks until the bucket allows them, so the *sum* of all
// transfers stays under the limit instead of each engine limiting itself.
class RateBudget {
public:
    // Bytes per second; 0 or less means unlimited.
    void setLimit(std::int64_t bytesPerSecond);
    std::int64_t limit() const;

    // Blocks until `bytes` may be spent. Returns immediately when unlimited or
    // when the wait would be absurdly long, so a cancelled transfer cannot hang
    // here.
    void spend(std::int64_t bytes);

    // Adds tokens for the time that passed since the last call.
    void refill();

private:
    // Caller holds the mutex.
    void refillLocked();
    using Clock = std::chrono::steady_clock;
    mutable std::mutex mutex_;
    std::int64_t limit_ = 0;
    double tokens_ = 0;
    Clock::time_point last_ = Clock::now();
};

} // namespace litefetch
