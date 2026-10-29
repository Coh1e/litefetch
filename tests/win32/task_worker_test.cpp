// Worker-boundary tests: the real HTTP fixture, the real SQLite task store and
// the real worker threads. These verify that starting a download does not block
// the caller, that cancellation and window-close shutdown join every worker
// thread, and that a cancelled transfer never publishes a file.
#include "http_fixture.h"
#include "task_service.h"
#include "task_store.h"
#include "task_worker.h"
#include "test_support.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <thread>

using namespace litefetch;
using namespace litefetch::test;

namespace {
template <typename Predicate>
bool waitFor(Predicate predicate, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return predicate();
}

std::optional<TaskSnapshot> onlyTask(TaskWorker& worker)
{
    const auto snapshot = worker.snapshot();
    if (snapshot.tasks.size() != 1) return std::nullopt;
    return snapshot.tasks.front();
}

bool isActive(TaskWorker& worker, const std::string& id)
{
    for (const auto& activeId : worker.snapshot().active) {
        if (activeId == id) return true;
    }
    return false;
}

void checkNoStaging(const std::filesystem::path& root)
{
    for (const auto& entry : std::filesystem::directory_iterator(root)) {
        const auto name = entry.path().filename().string();
        if (name.find(".part-") != std::string::npos)
            throw std::runtime_error("transfer left a staging file: " + name);
    }
}

void completedDownload(const std::filesystem::path& root)
{
    const auto body = payload(1024 * 1024);
    HttpFixture server(HttpFixture::Options{body, 200, 64 * 1024, std::chrono::milliseconds(5)});
    const auto database = root / "worker-complete.db";
    const auto destination = root / "worker-complete.bin";
    TaskWorker worker(database);
    std::string error;
    check(worker.start(error), ("worker start failed: " + error).c_str());

    worker.postAdd(server.url(), destination.string(), true);
    std::string id;
    check(waitFor([&] {
        const auto task = onlyTask(worker);
        if (!task) return false;
        id = task->id;
        return isActive(worker, id) && task->received > 0;
    }, std::chrono::seconds(15)), "start did not become active with live progress");

    check(waitFor([&] {
        const auto task = onlyTask(worker);
        return task && task->state == TaskState::Completed && !isActive(worker, id);
    }, std::chrono::seconds(20)), "download did not complete through the worker");

    const auto task = onlyTask(worker);
    check(task.has_value(), "task disappeared");
    check(task->received == static_cast<std::int64_t>(body.size()), "final byte count");
    check(readFile(destination) == body, "downloaded bytes differ from the controlled server");
    checkNoStaging(root);

    worker.shutdown();
    check(worker.shutdownComplete(), "worker did not finish shutdown");
    check(worker.snapshot().active.empty(), "worker still reports active tasks after shutdown");

    TaskRepository reopened(database);
    const auto persisted = reopened.find(id);
    check(persisted.has_value() && persisted->state == TaskState::Completed, "completed state is not durable");
}

void extraStartsWaitForASlot(const std::filesystem::path& root)
{
    // One slot, two tasks: the second must stay queued until the first finishes.
    // Slow enough that the first cannot finish before the assertions run.
    const auto body = payload(512 * 1024);
    HttpFixture server(HttpFixture::Options{body, 200, 8 * 1024, std::chrono::milliseconds(30)});
    const auto database = root / "worker-slots.db";
    TaskWorker worker(database);
    std::string error;
    check(worker.start(error), ("worker start failed: " + error).c_str());
    worker.setMaxConcurrent(1);

    worker.postAdd(server.url("/first.bin"), (root / "first.bin").string(), true);
    std::string firstId;
    check(waitFor([&] {
        const auto tasks = worker.snapshot().tasks;
        if (tasks.empty()) return false;
        firstId = tasks.front().id;
        return tasks.front().received > 0;
    }, std::chrono::seconds(15)), "the first task did not start");

    worker.postAdd(server.url("/second.bin"), (root / "second.bin").string(), true);
    // The second add lands while the first still occupies the slot.
    check(waitFor([&] { return worker.snapshot().tasks.size() == 2; },
        std::chrono::seconds(10)), "the second task was not added");
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    for (const auto& task : worker.snapshot().tasks) {
        if (task.id == firstId) continue;
        check(task.state == TaskState::Queued || task.state == TaskState::Paused,
            "the second task must wait for a free slot");
        check(worker.snapshot().active.size() == 1,
            "only one transfer may be active under a single slot");
    }

    check(waitFor([&] {
        const auto tasks = worker.snapshot().tasks;
        if (tasks.size() != 2) return false;
        for (const auto& task : tasks)
            if (task.state != TaskState::Completed) return false;
        return true;
    }, std::chrono::seconds(40)), "both tasks did not finish through the queue");

    check(readFile(root / "first.bin") == body, "the first file is complete");
    check(readFile(root / "second.bin") == body, "the queued file is complete");
    worker.shutdown();
}
void priorityJumpsTheQueue(const std::filesystem::path& root)
{
    const auto body = payload(512 * 1024);
    HttpFixture server(HttpFixture::Options{body, 200, 8 * 1024, std::chrono::milliseconds(30)});
    TaskWorker worker(root / "worker-priority.db");
    std::string error;
    check(worker.start(error), ("worker start failed: " + error).c_str());
    worker.setMaxConcurrent(1);

    worker.postAdd(server.url("/a.bin"), (root / "a.bin").string(), true);
    std::string firstId;
    check(waitFor([&] {
        const auto tasks = worker.snapshot().tasks;
        if (tasks.empty()) return false;
        firstId = tasks.front().id;
        return tasks.front().received > 0;
    }, std::chrono::seconds(15)), "the first task did not start");

    // Second task waits behind the first.
    worker.postAdd(server.url("/b.bin"), (root / "b.bin").string(), true);
    check(waitFor([&] { return worker.snapshot().tasks.size() == 2; },
        std::chrono::seconds(10)), "the waiting task was not added");

    // Third task is created paused, raised above the second, then started.
    worker.postAdd(server.url("/c.bin"), (root / "c.bin").string(), false);
    std::string thirdId;
    check(waitFor([&] {
        for (const auto& task : worker.snapshot().tasks)
            if (task.id != firstId && task.destination.find("c.bin") != std::string::npos)
                thirdId = task.id;
        return !thirdId.empty();
    }, std::chrono::seconds(10)), "the third task was not added");
    worker.postPriority(thirdId, 10);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    worker.postStart(thirdId);

    // Once the first finishes, the high-priority third task runs before the
    // second one, which is still waiting.
    check(waitFor([&] {
        for (const auto& task : worker.snapshot().tasks)
            if (task.id == thirdId) return task.state == TaskState::Completed;
        return false;
    }, std::chrono::seconds(40)), "the high-priority task did not finish");
    bool secondStillWaiting = false;
    for (const auto& task : worker.snapshot().tasks) {
        if (task.id == firstId || task.id == thirdId) continue;
        secondStillWaiting = task.state != TaskState::Completed;
    }
    check(secondStillWaiting, "the low-priority task should still be waiting");

    check(waitFor([&] {
        for (const auto& task : worker.snapshot().tasks)
            if (task.state != TaskState::Completed) return false;
        return true;
    }, std::chrono::seconds(40)), "the queued task never ran");
    worker.shutdown();
}
void lowUsageBacksOffWhenIdle(const std::filesystem::path& root)
{
    TaskWorker worker(root / "worker-low-usage.db");
    std::string error;
    check(worker.start(error), ("worker start failed: " + error).c_str());
    worker.setMaxConcurrent(4);
    worker.setLowUsageIdleSeconds(1);

    check(waitFor([&] { return worker.snapshot().runtimeConcurrency == 4; },
        std::chrono::seconds(5)), "the user limit is in force at the start");

    // No command, no transfer: after the idle window only one slot may remain.
    check(waitFor([&] { return worker.snapshot().runtimeConcurrency == 1; },
        std::chrono::seconds(10)), "idle did not lower the slot budget");

    // Any activity brings the user's limit back.
    worker.postRefresh();
    check(waitFor([&] { return worker.snapshot().runtimeConcurrency == 4; },
        std::chrono::seconds(10)), "activity did not restore the user limit");
    worker.shutdown();
}
void cancelledDownload(const std::filesystem::path& root)
{
    const auto body = payload(4 * 1024 * 1024);
    HttpFixture server(HttpFixture::Options{body, 200, 16 * 1024, std::chrono::milliseconds(20)});
    const auto database = root / "worker-cancel.db";
    const auto destination = root / "worker-cancel.bin";
    TaskWorker worker(database);
    std::string error;
    check(worker.start(error), ("worker start failed: " + error).c_str());

    worker.postAdd(server.url(), destination.string(), true);
    std::string id;
    check(waitFor([&] {
        const auto task = onlyTask(worker);
        if (!task) return false;
        id = task->id;
        return isActive(worker, id) && task->received > 0;
    }, std::chrono::seconds(15)), "download did not reach running state");

    worker.postCancel(id);
    check(waitFor([&] {
        const auto task = onlyTask(worker);
        return task && task->state == TaskState::Cancelled && !isActive(worker, id);
    }, std::chrono::seconds(15)), "cancel did not stop the worker download");

    check(!std::filesystem::exists(destination), "cancelled transfer published a file");
    checkNoStaging(root);
    worker.shutdown();

    TaskRepository reopened(database);
    const auto persisted = reopened.find(id);
    check(persisted.has_value() && persisted->state == TaskState::Cancelled, "cancelled state is not durable");
}

void shutdownDuringDownload(const std::filesystem::path& root)
{
    const auto body = payload(8 * 1024 * 1024);
    HttpFixture server(HttpFixture::Options{body, 200, 16 * 1024, std::chrono::milliseconds(25)});
    const auto database = root / "worker-close.db";
    const auto destination = root / "worker-close.bin";
    TaskWorker worker(database);
    std::string error;
    check(worker.start(error), ("worker start failed: " + error).c_str());

    worker.postAdd(server.url(), destination.string(), true);
    std::string id;
    check(waitFor([&] {
        const auto task = onlyTask(worker);
        if (!task) return false;
        id = task->id;
        return isActive(worker, id) && task->received > 0;
    }, std::chrono::seconds(15)), "download did not reach running state");

    // This is the window-close path: request shutdown while bytes are moving.
    worker.beginShutdown();
    check(waitFor([&] { return worker.shutdownComplete(); }, std::chrono::seconds(20)),
        "worker shutdown did not finish while a transfer was running");
    worker.join();
    check(worker.snapshot().active.empty(), "active tasks remain after shutdown");

    check(!std::filesystem::exists(destination), "shutdown published a partial file");
    checkNoStaging(root);

    TaskRepository reopened(database);
    const auto persisted = reopened.find(id);
    check(persisted.has_value() && persisted->state == TaskState::Cancelled,
        "shutdown did not persist the cancelled outcome");
}

void existingTasksVisibleOnStart(const std::filesystem::path& root)
{
    const auto database = root / "worker-existing.db";
    const auto destination = root / "existing.bin";
    std::string id;
    {
        TaskRepository repository(database);
        TaskService service(repository);
        const auto added = service.execute(AddTaskCommand{
            "http://127.0.0.1:1/existing.bin", destination.string()});
        check(added.ok, ("seed add failed: " + added.error).c_str());
        id = added.task.id;
    }
    TaskWorker worker(database);
    std::string error;
    check(worker.start(error), ("worker start failed: " + error).c_str());
    check(waitFor([&] { return worker.snapshot().tasks.size() == 1; }, std::chrono::seconds(5)),
        "existing rows are loaded before the first command");
    check(worker.snapshot().tasks.front().id == id, "startup snapshot keeps the seeded task id");
    worker.shutdown();
}

void batchResultsAndRecordDelete(const std::filesystem::path& root)
{
    TaskWorker worker(root / "worker-batch.db");
    std::string error;
    check(worker.start(error), ("worker start failed: " + error).c_str());

    const auto first = root / "batch-first.bin";
    const auto second = root / "batch-second.bin";
    worker.postAdd("http://127.0.0.1:1/first.bin", first.string(), false, 42, 1);
    worker.postAdd("http://127.0.0.1:1/second.bin", second.string(), false, 42, 2);
    worker.postAdd("gopher://127.0.0.1/unsupported.bin", (root / "unsupported.bin").string(),
        false, 43, 1);
    check(waitFor([&] { return worker.snapshot().addResults.size() >= 3; },
              std::chrono::seconds(5)), "batch results did not reach the UI snapshot");
    const auto snapshot = worker.snapshot();
    check(snapshot.tasks.size() == 2, "two supported lines created two records");
    check(snapshot.addResults[0].batchId == 42 && snapshot.addResults[0].line == 1
            && snapshot.addResults[0].ok, "first line result keeps batch/line identity");
    check(snapshot.addResults[1].batchId == 42 && snapshot.addResults[1].line == 2
            && snapshot.addResults[1].ok, "second line result keeps original order");
    check(snapshot.addResults[2].batchId == 43 && !snapshot.addResults[2].ok,
        "unsupported line is reported as a failed add, not silently dropped");

    std::string firstId;
    std::string secondId;
    for (const auto& task : snapshot.tasks) {
        if (task.destination == first.string()) firstId = task.id;
        if (task.destination == second.string()) secondId = task.id;
    }
    check(!firstId.empty() && !secondId.empty(), "both record ids are available");

    { std::ofstream file(first, std::ios::binary); file << "kept"; }
    worker.postDelete(firstId);
    check(waitFor([&] { return !worker.snapshot().outcomes.empty(); }, std::chrono::seconds(5)),
        "delete outcome did not reach the UI snapshot");
    const auto afterDelete = worker.snapshot();
    check(afterDelete.outcomes.back().action == "delete" && afterDelete.outcomes.back().ok,
        "record delete succeeded");
    check(afterDelete.tasks.size() == 1, "only the deleted record is gone");
    check(std::filesystem::exists(first), "record deletion kept the file");
    check(afterDelete.tasks.front().id == secondId, "the other record remains");

    worker.postDelete(secondId);
    check(waitFor([&] { return worker.snapshot().tasks.empty(); }, std::chrono::seconds(5)),
        "queued record delete did not complete");
    check(!std::filesystem::exists(second), "record delete never creates the file");
    worker.shutdown();
}

void invalidAddReportsError(const std::filesystem::path& root)
{
    TaskWorker worker(root / "worker-error.db");
    std::string error;
    check(worker.start(error), ("worker start failed: " + error).c_str());
    const auto before = worker.snapshot().lastErrorSequence;
    worker.postAdd("gopher://127.0.0.1/file.bin", (root / "nope.bin").string(), false);
    check(waitFor([&] { return worker.snapshot().lastErrorSequence > before; }, std::chrono::seconds(5)),
        "add failure did not reach the worker error channel");
    const auto message = worker.snapshot().lastError;
    check(message.find("添加失败") != std::string::npos, ("add failure text: " + message).c_str());
    worker.shutdown();
}

int body()
{
    TempDir root;
    existingTasksVisibleOnStart(root.path());
    completedDownload(root.path());
    cancelledDownload(root.path());
    shutdownDuringDownload(root.path());
    batchResultsAndRecordDelete(root.path());
    invalidAddReportsError(root.path());
    extraStartsWaitForASlot(root.path());
    priorityJumpsTheQueue(root.path());
    lowUsageBacksOffWhenIdle(root.path());
    return 0;
}
} // namespace

int main()
{
    return run("ui-worker", &body);
}
