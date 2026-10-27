#include "task_service.h"
#include "task_store.h"
#include "test_support.h"

#include <sqlite3.h>

#include <filesystem>
#include <functional>

using namespace litefetch;
using namespace litefetch::test;

namespace {
void corruptReadContract(const std::filesystem::path& database)
{
    sqlite3* raw = nullptr;
    check(sqlite3_open(reinterpret_cast<const char*>(database.u8string().c_str()), &raw) == SQLITE_OK,
        "cannot create corrupt fixture");
    const char* sql =
        "CREATE TABLE native_meta(key TEXT PRIMARY KEY NOT NULL,value TEXT NOT NULL);"
        "INSERT INTO native_meta(key,value) VALUES('schema_version','1');"
        "CREATE TABLE native_tasks(id TEXT PRIMARY KEY NOT NULL,source TEXT NOT NULL,"
        "destination TEXT NOT NULL,state TEXT NOT NULL,received INTEGER NOT NULL,total INTEGER NOT NULL,"
        "error TEXT NOT NULL,created_at INTEGER NOT NULL,updated_at INTEGER NOT NULL);"
        "INSERT INTO native_tasks(id,source,destination,state,received,total,error,created_at,updated_at)"
        " VALUES('broken','http://127.0.0.1:1/broken','D:\\broken.bin','not-a-state',0,-1,'',1,1);";
    check(sqlite3_exec(raw, sql, nullptr, nullptr, nullptr) == SQLITE_OK, "cannot seed corrupt fixture");
    sqlite3_close(raw);

    TaskRepository repository(database);
    int calls = 0;
    TaskService service(repository, [&](const TransferRequest&, const std::atomic_bool&,
                                           const std::function<void(const TransferProgressInfo&)>&) {
        ++calls;
        TransferResult result;
        result.status = TransferStatus::Completed;
        return result;
    });
    const auto started = service.execute(StartTaskCommand{"broken"});
    check(!started.ok, "corrupt stored state was reported as command success");
    check(started.error.rfind("storage:", 0) == 0, "read failure was not reported as a storage failure");
    checkEqual(calls, 0, "engine was started for a task that could not be read");

    bool queryThrew = false;
    try {
        service.snapshot("broken");
    } catch (const StorageError&) {
        queryThrew = true;
    }
    check(queryThrew, "query contract did not report a corrupt stored state");
}

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
    corruptReadContract(directory.file("corrupt.db"));
    return 0;
}
} // namespace

int main()
{
    return run("storage-failure", &body);
}
