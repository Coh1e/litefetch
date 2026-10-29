// Traffic categories stay apart: a download total must not hide the bytes we
// sent or the bytes read to verify a file.
#include "traffic_meter.h"
#include "test_support.h"

#include <thread>
#include <vector>

using namespace litefetch;
using namespace litefetch::test;

namespace {
void categoriesStayApart()
{
    TrafficMeter meter;
    meter.add(TrafficCategory::Download, 1000);
    meter.add(TrafficCategory::Upload, 250);
    meter.add(TrafficCategory::Metadata, 40);
    meter.add(TrafficCategory::Verify, 10);
    checkEqual(meter.bytes(TrafficCategory::Download), 1000LL, "download bytes");
    checkEqual(meter.bytes(TrafficCategory::Upload), 250LL, "upload bytes");
    checkEqual(meter.bytes(TrafficCategory::Metadata), 40LL, "metadata bytes");
    checkEqual(meter.bytes(TrafficCategory::Verify), 10LL, "verify bytes");
    checkEqual(meter.total(), 1300LL, "the total is the sum of the categories");

    meter.add(TrafficCategory::Download, -5);
    checkEqual(meter.bytes(TrafficCategory::Download), 1000LL, "negative additions are ignored");

    meter.reset();
    checkEqual(meter.total(), 0LL, "reset clears every category");
    checkEqual(std::string(TrafficMeter::name(TrafficCategory::Verify)), std::string("verify"),
        "a category has a name");
}

void countingSurvivesConcurrency()
{
    TrafficMeter meter;
    std::vector<std::thread> threads;
    for (int thread = 0; thread < 4; ++thread) {
        threads.emplace_back([&meter] {
            for (int step = 0; step < 500; ++step) meter.add(TrafficCategory::Download, 2);
        });
    }
    for (auto& thread : threads) thread.join();
    checkEqual(meter.bytes(TrafficCategory::Download), 4000LL, "every thread's bytes are counted");
}
} // namespace

int main()
{
    return run("traffic", [] {
        categoriesStayApart();
        countingSurvivesConcurrency();
        return 0;
    });
}
