// The shared byte budget: a limited transfer must take about as long as the
// bytes divided by the limit, and an unlimited one must not wait at all.
#include "rate_limit.h"
#include "test_support.h"

#include <chrono>
#include <functional>

using namespace litefetch;
using namespace litefetch::test;

namespace {
double spentSeconds(const std::function<void()>& work)
{
    const auto started = std::chrono::steady_clock::now();
    work();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
}

void unlimitedDoesNotWait()
{
    RateBudget budget;
    budget.setLimit(0);
    const auto seconds = spentSeconds([&] {
        for (int step = 0; step < 100; ++step) budget.spend(64 * 1024);
    });
    check(seconds < 0.2, "an unlimited budget does not sleep");
    checkEqual(budget.limit(), 0LL, "the limit is reported");
}

void limitedSpendsTime()
{
    RateBudget budget;
    // 200 KiB/s with 400 KiB to spend: the bucket starts full, so the first
    // 200 KiB are free and the rest must take about a second.
    budget.setLimit(200 * 1024);
    const auto seconds = spentSeconds([&] {
        budget.spend(200 * 1024);
        budget.spend(200 * 1024);
    });
    check(seconds > 0.7, "a limited budget slows the transfer down");
    check(seconds < 3.0, "the wait is bounded by the limit, not longer");
}

void twoCallersShareTheBudget()
{
    RateBudget budget;
    budget.setLimit(200 * 1024);
    // Two transfers spending 200 KiB each must also take about a second in
    // total, not half a second each (which per-engine limits would allow).
    const auto seconds = spentSeconds([&] {
        budget.spend(200 * 1024);
        budget.spend(100 * 1024);
        budget.spend(100 * 1024);
    });
    check(seconds > 0.6, "the budget is shared between callers");
}
} // namespace

int main()
{
    return run("rate-limit", [] {
        unlimitedDoesNotWait();
        limitedSpendsTime();
        twoCallersShareTheBudget();
        return 0;
    });
}
