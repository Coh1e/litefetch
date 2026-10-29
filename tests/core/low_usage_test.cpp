// Low-usage policy: the decision is a pure function, so idle behaviour is
// tested without waiting for real idle time.
#include "low_usage.h"
#include "test_support.h"

using namespace litefetch;
using namespace litefetch::test;
using namespace std::chrono_literals;

namespace {
LowUsagePolicy policy(bool enabled, int concurrency, int idleSeconds)
{
    LowUsagePolicy value;
    value.enabled = enabled;
    value.userConcurrency = concurrency;
    value.idleAfter = std::chrono::seconds(idleSeconds);
    return value;
}

void disabledKeepsTheUserSetting()
{
    const auto off = policy(false, 3, 60);
    checkEqual(static_cast<long long>(off.effectiveConcurrency(0, 10min)), 3LL,
        "with low usage off the user limit stays");
    checkEqual(static_cast<long long>(off.effectiveConcurrency(0, 0s)), 3LL,
        "the idle time does not matter when off");
    check(!off.shouldPauseSeeding(10min, 0), "seeding keeps running when off");
}

void idleBacksOffToOne()
{
    const auto on = policy(true, 4, 60);
    checkEqual(static_cast<long long>(on.effectiveConcurrency(2, 5s)), 4LL,
        "while active the user limit applies");
    checkEqual(static_cast<long long>(on.effectiveConcurrency(0, 5min)), 1LL,
        "after the idle window only one transfer may run");
    // A person who asked for one transfer keeps one, never more.
    const auto single = policy(true, 1, 60);
    checkEqual(static_cast<long long>(single.effectiveConcurrency(0, 5min)), 1LL,
        "an explicit one stays one");
    const auto unlimited = policy(true, 0, 60);
    checkEqual(static_cast<long long>(unlimited.effectiveConcurrency(0, 5min)), 1LL,
        "unlimited becomes one when idle");
}

void seedingPausesOnlyWhenIdleAndEmpty()
{
    const auto on = policy(true, 2, 60);
    check(!on.shouldPauseSeeding(30s, 0), "not idle yet, keep seeding");
    check(on.shouldPauseSeeding(5min, 0), "idle and nothing queued: pause seeding");
    check(!on.shouldPauseSeeding(5min, 2), "a queued task keeps the link busy");
}
} // namespace

int main()
{
    return run("low-usage", [] {
        disabledKeepsTheUserSetting();
        idleBacksOffToOne();
        seedingPausesOnlyWhenIdleAndEmpty();
        return 0;
    });
}
