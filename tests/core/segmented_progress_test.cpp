// A segmented transfer must tell the UI its total and live bytes while the
// ranges are still arriving. Real CDN downloads run four ranges in parallel at
// the same speed, so reporting only after a whole range completes hides the
// entire transfer behind "正在连接" and a blank size.
#include "http_engine.h"
#include "http_fixture.h"
#include "test_support.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <future>
#include <mutex>
#include <thread>
#include <vector>

namespace {

using litefetch::test::HttpFixture;
using litefetch::test::payload;

int segmentedReportsLiveProgress()
{
    const auto body = payload(8 * 1024 * 1024);
    HttpFixture::Options options{body};
    options.chunk = 16 * 1024;
    // Slow enough that one range (a quarter of the body) needs several
    // seconds, so "no report" cannot be confused with "already finished".
    options.chunkDelay = std::chrono::milliseconds(30);
    options.supportsRange = true;
    HttpFixture server(options);
    litefetch::test::TempDir root;
    const auto destination = root.file("segmented-progress.bin");

    std::atomic_bool cancelled{false};
    std::mutex mutex;
    std::vector<litefetch::TransferProgressInfo> events;
    auto progress = [&](const litefetch::TransferProgressInfo& info) {
        std::lock_guard lock(mutex);
        events.push_back(info);
    };

    std::promise<litefetch::TransferResult> finished;
    auto result = finished.get_future();
    std::jthread worker([&] {
        finished.set_value(litefetch::transferHttp(
            litefetch::TransferRequest{server.url(), destination.string()}, cancelled, progress));
    });

    // Poll for the earliest usable report instead of sleeping a fixed time:
    // a green run finishes the check as soon as the first progress arrives.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    bool liveProgress = false;
    while (std::chrono::steady_clock::now() < deadline) {
        {
            std::lock_guard lock(mutex);
            for (const auto& info : events) {
                if (info.total == static_cast<std::int64_t>(body.size()) && info.received > 0) {
                    liveProgress = true;
                    break;
                }
            }
        }
        if (liveProgress) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    cancelled.store(true);
    worker.join();
    const auto transferResult = result.get();
    litefetch::test::check(transferResult.status == litefetch::TransferStatus::Cancelled,
        "cancelled segmented transfer did not report cancellation");
    litefetch::test::check(liveProgress,
        "segmented transfer reported no total and no bytes before a whole segment finished");
    return 0;
}

} // namespace

int main()
{
    return litefetch::test::run("core-segmented-progress", segmentedReportsLiveProgress);
}
