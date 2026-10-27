// Worker-boundary tests: the real HTTP fixture, the real SQLite task store and
// the real worker threads. These verify that starting a download does not block
// the caller, that cancellation and window-close shutdown join every worker
// thread, and that a cancelled transfer never publishes a file.
#include "http_fixture.h"
#include "task_store.h"
#include "task_worker.h"
#include "test_support.h"

#include <chrono>
#include <filesystem>
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

void invalidAddReportsError(const std::filesystem::path& root)
{
    TaskWorker worker(root / "worker-error.db");
    std::string error;
    check(worker.start(error), ("worker start failed: " + error).c_str());
    const auto before = worker.snapshot().lastErrorSequence;
    worker.postAdd("ftp://example.org/file.bin", (root / "nope.bin").string(), false);
    check(waitFor([&] { return worker.snapshot().lastErrorSequence > before; }, std::chrono::seconds(5)),
        "add failure did not reach the worker error channel");
    const auto message = worker.snapshot().lastError;
    check(message.find("添加失败") != std::string::npos, ("add failure text: " + message).c_str());
    worker.shutdown();
}

int body()
{
    TempDir root;
    completedDownload(root.path());
    cancelledDownload(root.path());
    shutdownDuringDownload(root.path());
    invalidAddReportsError(root.path());
    return 0;
}
} // namespace

int main()
{
    return run("ui-worker", &body);
}
