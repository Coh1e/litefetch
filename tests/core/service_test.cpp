#include "http_fixture.h"
#include "task_identity.h"
#include "task_service.h"
#include "task_store.h"
#include "test_support.h"

#include <chrono>
#include <filesystem>
#include <future>
#include <stdexcept>
#include <thread>

using namespace litefetch;
using namespace litefetch::test;

namespace {
void require(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}

std::string stateName(const TaskSnapshot& task)
{
    return std::string(taskStateName(task.state));
}

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

void checkNoStaging(const std::filesystem::path& root, const std::string& destinationName)
{
    for (const auto& entry : std::filesystem::directory_iterator(root)) {
        const auto name = entry.path().filename().string();
        require(name.rfind(destinationName + ".part", 0) != 0,
            "transfer left a staging file: " + name);
    }
}

void completeRoundTrip(const std::filesystem::path& root)
{
    const auto body = payload();
    HttpFixture server(HttpFixture::Options{body});
    const auto destination = root / "complete.bin";
    const auto database = root / "complete.db";
    std::string id;
    {
        TaskRepository repository(database);
        TaskService service(repository);
        const auto added = service.execute(AddTaskCommand{server.url(), destination.string()});
        require(added.ok, "add failed: " + added.error);
        require(added.task.state == TaskState::Queued, "new task is not queued");
        id = added.task.id;
        checkEqual(id, taskIdentity(server.url()), "identity is derived from the source");

        const auto started = service.execute(StartTaskCommand{id});
        require(started.ok, "start failed: " + started.error);
        require(started.task.state == TaskState::Completed,
            "completed transfer did not produce completed state: " + stateName(started.task)
                + " " + started.task.error);
        checkEqual(started.task.received, static_cast<long long>(body.size()), "final received bytes");
        checkEqual(started.task.total, static_cast<long long>(body.size()), "server total bytes");
        check(readFile(destination) == body, "downloaded file bytes differ from the controlled server body");
        checkNoStaging(root, "complete.bin");
    }

    // The command result and the durable snapshot must agree after reopening.
    {
        TaskRepository repository(database);
        TaskService service(repository);
        const auto loaded = service.snapshot(id);
        check(loaded.has_value(), "task did not survive database reopen");
        require(loaded->state == TaskState::Completed, "persisted state is not completed");
        checkEqual(loaded->received, static_cast<long long>(body.size()), "persisted byte count");
        checkEqual(static_cast<long long>(service.snapshots().size()), 1, "unexpected row count");

        const auto duplicate = service.execute(AddTaskCommand{server.url(), destination.string()});
        require(duplicate.ok, "duplicate add failed: " + duplicate.error);
        checkEqual(duplicate.task.id, id, "same source keeps the same task identity");
        checkEqual(static_cast<long long>(service.snapshots().size()), 1, "duplicate add created another row");
    }
}

void httpFailure(const std::filesystem::path& root)
{
    HttpFixture server(HttpFixture::Options{payload(1024)});
    const auto destination = root / "missing.bin";
    const auto database = root / "missing.db";
    TaskRepository repository(database);
    TaskService service(repository);
    const auto added = service.execute(AddTaskCommand{server.url("/missing"), destination.string()});
    require(added.ok, "add failed: " + added.error);
    const auto started = service.execute(StartTaskCommand{added.task.id});
    require(started.ok, "start command was not committed: " + started.error);
    require(started.task.state == TaskState::Failed, "HTTP 404 did not produce failed state");
    require(started.task.error.find("404") != std::string::npos,
        "HTTP 404 reason is missing: " + started.task.error);
    check(!std::filesystem::exists(destination), "failed transfer published a destination file");
    checkNoStaging(root, "missing.bin");
    const auto persisted = service.snapshot(added.task.id);
    require(persisted.has_value() && persisted->state == TaskState::Failed, "failed state is not durable");
}

void cancellation(const std::filesystem::path& root)
{
    const auto body = payload(2 * 1024 * 1024);
    HttpFixture server(HttpFixture::Options{body, 200, 16 * 1024, std::chrono::milliseconds(20)});
    const auto destination = root / "cancel.bin";
    const auto database = root / "cancel.db";
    TaskRepository repository(database);
    TaskService service(repository);
    const auto added = service.execute(AddTaskCommand{server.url(), destination.string()});
    require(added.ok, "add failed: " + added.error);

    std::promise<CommandResult> finished;
    auto result = finished.get_future();
    std::jthread worker([&] { finished.set_value(service.execute(StartTaskCommand{added.task.id})); });
    require(waitFor([&] {
        const auto snapshot = service.snapshot(added.task.id);
        return snapshot && snapshot->state == TaskState::Running && snapshot->received > 0;
    }, std::chrono::seconds(10)), "download did not reach running state with progress");

    const auto requested = service.execute(CancelTaskCommand{added.task.id});
    require(requested.ok, "cancel request failed: " + requested.error);
    const auto started = result.get();
    worker.join();
    require(started.ok, "cancelled transfer state was not committed: " + started.error);
    require(started.task.state == TaskState::Cancelled, "cancel did not produce cancelled state");
    check(!std::filesystem::exists(destination), "cancelled transfer published a destination file");
    checkNoStaging(root, "cancel.bin");
}

int body()
{
    TempDir root;
    completeRoundTrip(root.path());
    httpFailure(root.path());
    cancellation(root.path());
    return 0;
}
} // namespace

int main()
{
    return run("service", &body);
}
