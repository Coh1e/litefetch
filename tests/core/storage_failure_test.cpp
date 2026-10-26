#include "task_service.h"
#include "task_store.h"
#include "test_support.h"

#include <filesystem>
#include <functional>

using namespace litefetch;
using namespace litefetch::test;

namespace {
int body()
{
    TempDir directory;
    const auto database = directory.file("readonly.db");
    const auto destination = directory.file("readonly.bin");
    std::string id;
    {
        TaskRepository repository(database);
        int calls = 0;
        TaskService service(repository, [&](const TransferRequest&, const std::atomic_bool&,
                                               const std::function<void(const TransferProgressInfo&)>&) {
            ++calls;
            TransferResult result;
            result.status = TransferStatus::Completed;
            return result;
        });
        const auto added = service.execute(AddTaskCommand{
            "http://127.0.0.1:1/file.bin", destination.string()});
        check(added.ok, "initial add failed");
        id = added.task.id;
    }

    {
        auto repository = TaskRepository::openReadOnly(database);
        int calls = 0;
        TaskService service(repository, [&](const TransferRequest&, const std::atomic_bool&,
                                               const std::function<void(const TransferProgressInfo&)>&) {
            ++calls;
            TransferResult result;
            result.status = TransferStatus::Completed;
            return result;
        });

        const auto cancelled = service.execute(CancelTaskCommand{id});
        check(!cancelled.ok, "read-only cancel was reported as success");
        check(cancelled.error.rfind("storage:", 0) == 0, "cancel storage failure reason is missing");

        const auto started = service.execute(StartTaskCommand{id});
        check(!started.ok, "read-only commit was reported as success");
        check(started.error.rfind("storage:", 0) == 0, "storage failure reason is missing");
        checkEqual(calls, 0, "engine was started without a committed running transition");

        const auto added = service.execute(AddTaskCommand{
            "http://127.0.0.1:1/other.bin", (directory.file("other.bin")).string()});
        check(!added.ok, "read-only insert was reported as success");
        checkEqual(static_cast<long long>(repository.all().size()), 1, "read-only database gained a row");
    }

    {
        TaskRepository repository(database);
        const auto task = repository.find(id);
        check(task.has_value(), "task disappeared");
        check(task->state == TaskState::Queued, "failed running commit changed persisted state");
        checkEqual(static_cast<long long>(repository.all().size()), 1, "failed insert gained a row");
    }
    check(!std::filesystem::exists(destination), "storage failure still created a destination file");
    return 0;
}
} // namespace

int main()
{
    return run("storage-failure", &body);
}
