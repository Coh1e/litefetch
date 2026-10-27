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

void sameDestinationRace(const std::filesystem::path& root)
{
    const auto bodyA = payload(1024 * 1024);
    auto bodyB = bodyA;
    for (auto& byte : bodyB) byte = static_cast<unsigned char>(byte ^ 0x5a);
    HttpFixture serverA(HttpFixture::Options{bodyA, 200, 8 * 1024, std::chrono::milliseconds(20)});
    HttpFixture serverB(HttpFixture::Options{bodyB, 200, 8 * 1024, std::chrono::milliseconds(20)});
    const auto destination = root / "race.bin";
    const auto database = root / "race.db";
    TaskRepository repository(database);
    TaskService service(repository);
    const auto addedA = service.execute(AddTaskCommand{serverA.url(), destination.string()});
    const auto addedB = service.execute(AddTaskCommand{serverB.url(), destination.string()});
    require(addedA.ok, "first add failed: " + addedA.error);
    require(addedB.ok, "second add failed: " + addedB.error);
    require(addedA.task.id != addedB.task.id, "race sources unexpectedly share identity");

    std::promise<CommandResult> finishedA;
    std::promise<CommandResult> finishedB;
    auto resultA = finishedA.get_future();
    auto resultB = finishedB.get_future();
    std::jthread workerA([&] { finishedA.set_value(service.execute(StartTaskCommand{addedA.task.id})); });
    std::jthread workerB([&] { finishedB.set_value(service.execute(StartTaskCommand{addedB.task.id})); });
    require(waitFor([&] {
        const auto a = service.snapshot(addedA.task.id);
        const auto b = service.snapshot(addedB.task.id);
        return a && b && a->state == TaskState::Running && b->state == TaskState::Running
            && a->received > 0 && b->received > 0;
    }, std::chrono::seconds(10)), "both same-destination transfers did not overlap");

    const auto startedA = resultA.get();
    const auto startedB = resultB.get();
    workerA.join();
    workerB.join();
    require(startedA.ok, "first start command was not committed: " + startedA.error);
    require(startedB.ok, "second start command was not committed: " + startedB.error);

    const auto published = readFile(destination);
    const bool aWon = published == bodyA;
    const bool bWon = published == bodyB;
    require(aWon != bWon, "published file does not match exactly one transfer");
    const auto& winner = aWon ? startedA : startedB;
    const auto& loser = aWon ? startedB : startedA;
    require(winner.task.state == TaskState::Completed, "winner did not persist completed state");
    require(loser.task.state == TaskState::Failed, "loser did not persist failed state");
    require(loser.task.error.find("destination") != std::string::npos
        || loser.task.error.find("exists") != std::string::npos,
        "loser failure does not identify the destination conflict: " + loser.task.error);
    checkEqual(static_cast<long long>(published.size()),
        static_cast<long long>(aWon ? bodyA.size() : bodyB.size()), "published length matches winner");

    {
        TaskRepository reopened(database);
        TaskService serviceAfterReopen(reopened);
        const auto persistedA = serviceAfterReopen.snapshot(addedA.task.id);
        const auto persistedB = serviceAfterReopen.snapshot(addedB.task.id);
        require(persistedA && persistedB, "race tasks did not survive reopen");
        require(persistedA->state == (aWon ? TaskState::Completed : TaskState::Failed),
            "first task state after reopen is inconsistent");
        require(persistedB->state == (bWon ? TaskState::Completed : TaskState::Failed),
            "second task state after reopen is inconsistent");
    }
    checkNoStaging(root, "race.bin");
}

void cancelThenStart(const std::filesystem::path& root)
{
    HttpFixture server(HttpFixture::Options{payload(1024)});
    const auto destination = root / "cancel-then-start.bin";
    const auto database = root / "cancel-then-start.db";
    TaskRepository repository(database);
    TaskService service(repository);
    const auto added = service.execute(AddTaskCommand{server.url(), destination.string()});
    require(added.ok, "add failed: " + added.error);
    const auto cancelled = service.execute(CancelTaskCommand{added.task.id});
    require(cancelled.ok, "queued cancel failed: " + cancelled.error);
    require(cancelled.task.state == TaskState::Cancelled, "queued cancel did not persist cancelled state");

    const auto started = service.execute(StartTaskCommand{added.task.id});
    require(!started.ok, "start accepted a task after cancel committed");
    const auto persisted = service.snapshot(added.task.id);
    require(persisted.has_value() && persisted->state == TaskState::Cancelled,
        "cancel result was overwritten by a later start");
    check(!std::filesystem::exists(destination), "start after cancel created a file");
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
    sameDestinationRace(root.path());
    cancelThenStart(root.path());
    cancellation(root.path());
    return 0;
}
} // namespace

int main()
{
    return run("service", &body);
}
