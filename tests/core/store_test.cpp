#include "task_store.h"
#include "test_support.h"

#include <sqlite3.h>

#include <filesystem>
#include <stdexcept>

using namespace litefetch;
using namespace litefetch::test;

namespace {
TaskSnapshot sample(const std::string& id, TaskState state = TaskState::Queued)
{
    TaskSnapshot snapshot;
    snapshot.id = id;
    snapshot.source = "http://127.0.0.1:1/file.bin";
    snapshot.destination = "D:\\data\\file.bin";
    snapshot.state = state;
    snapshot.received = 0;
    snapshot.total = 1024;
    snapshot.error.clear();
    snapshot.createdAt = 1000;
    snapshot.updatedAt = 1000;
    return snapshot;
}

void legacyCoexistence(const std::filesystem::path& database)
{
    sqlite3* raw = nullptr;
    check(sqlite3_open(reinterpret_cast<const char*>(database.u8string().c_str()), &raw) == SQLITE_OK,
        "cannot create legacy fixture");
    const char* legacy = "PRAGMA user_version=5; CREATE TABLE tasks(id TEXT PRIMARY KEY NOT NULL);"
        "INSERT INTO tasks(id) VALUES('legacy-task');";
    check(sqlite3_exec(raw, legacy, nullptr, nullptr, nullptr) == SQLITE_OK, "cannot seed legacy fixture");
    sqlite3_close(raw);

    {
        TaskRepository repository(database);
        repository.insert(sample("native-task"));
    }

    raw = nullptr;
    check(sqlite3_open(reinterpret_cast<const char*>(database.u8string().c_str()), &raw) == SQLITE_OK,
        "cannot inspect coexistence database");
    sqlite3_stmt* statement = nullptr;
    check(sqlite3_prepare_v2(raw, "PRAGMA user_version", -1, &statement, nullptr) == SQLITE_OK,
        "cannot prepare user_version");
    check(sqlite3_step(statement) == SQLITE_ROW, "cannot read user_version");
    checkEqual(sqlite3_column_int(statement, 0), 5, "legacy user_version changed");
    sqlite3_finalize(statement);
    check(sqlite3_prepare_v2(raw, "SELECT COUNT(*) FROM tasks WHERE id='legacy-task'", -1, &statement, nullptr) == SQLITE_OK,
        "cannot prepare legacy table query");
    check(sqlite3_step(statement) == SQLITE_ROW, "cannot read legacy table");
    checkEqual(sqlite3_column_int(statement, 0), 1, "legacy row changed");
    sqlite3_finalize(statement);
    check(sqlite3_prepare_v2(raw, "SELECT COUNT(*) FROM native_tasks WHERE id='native-task'", -1, &statement, nullptr) == SQLITE_OK,
        "cannot prepare native table query");
    check(sqlite3_step(statement) == SQLITE_ROW, "cannot read native table");
    checkEqual(sqlite3_column_int(statement, 0), 1, "native row missing");
    sqlite3_finalize(statement);
    sqlite3_close(raw);
}

// A database written by an early build reports schema 2. Opening it has to run
// every later ALTER TABLE, keep the rows, and end at the current schema. The
// v2-v5 branches once contained a statement with no ALTER TABLE, so this
// upgrade threw a syntax error and the application could not open the file.
void upgradesSchemaTwo(const std::filesystem::path& database)
{
    sqlite3* raw = nullptr;
    check(sqlite3_open(reinterpret_cast<const char*>(database.u8string().c_str()), &raw) == SQLITE_OK,
        "cannot create schema v2 fixture");
    const char* seed =
        "CREATE TABLE native_meta(key TEXT PRIMARY KEY NOT NULL,value TEXT NOT NULL);"
        "INSERT INTO native_meta VALUES('schema_version','2');"
        "CREATE TABLE native_tasks(id TEXT PRIMARY KEY NOT NULL,source TEXT NOT NULL,"
        "destination TEXT NOT NULL,state TEXT NOT NULL,received INTEGER NOT NULL,"
        "total INTEGER NOT NULL,error TEXT NOT NULL,created_at INTEGER NOT NULL,"
        "updated_at INTEGER NOT NULL);"
        "INSERT INTO native_tasks VALUES('old','http://127.0.0.1/file.bin','D:\\old.bin',"
        "'queued',0,1024,'',1000,1000);";
    check(sqlite3_exec(raw, seed, nullptr, nullptr, nullptr) == SQLITE_OK, "cannot seed schema v2");
    sqlite3_close(raw);

    {
        TaskRepository repository(database);
        const auto migrated = repository.find("old");
        check(migrated.has_value(), "a v2 row survives the upgrade");
        checkEqual(migrated->total, 1024, "a v2 row keeps its size");
        repository.insert(sample("after-upgrade"));
    }

    raw = nullptr;
    check(sqlite3_open(reinterpret_cast<const char*>(database.u8string().c_str()), &raw) == SQLITE_OK,
        "cannot inspect upgraded database");
    sqlite3_stmt* statement = nullptr;
    check(sqlite3_prepare_v2(raw,
        "SELECT value FROM native_meta WHERE key='schema_version'", -1, &statement, nullptr) == SQLITE_OK,
        "cannot read schema version");
    check(sqlite3_step(statement) == SQLITE_ROW, "schema version row missing");
    checkEqual(std::string(reinterpret_cast<const char*>(sqlite3_column_text(statement, 0))), "9",
        "schema version after upgrade");
    sqlite3_finalize(statement);
    check(sqlite3_prepare_v2(raw, "SELECT bt_uploaded FROM native_tasks LIMIT 1", -1, &statement, nullptr)
            == SQLITE_OK,
        "the v2 upgrade did not add bt_uploaded");
    sqlite3_finalize(statement);
    sqlite3_close(raw);
}

int body()
{
    TempDir directory;
    const auto database = directory.file("tasks.db");

    {
        TaskRepository repository(database);
        check(repository.all().empty(), "new database is empty");
        repository.insert(sample("task-a"));
        repository.insert(sample("task-b", TaskState::Failed));
    repository.insert(sample("task-c", TaskState::Seeding));
        checkEqual(static_cast<long long>(repository.all().size()), 3, "inserted row count");

        auto loaded = repository.find("task-a");
        check(loaded.has_value(), "inserted task can be found");
        checkEqual(loaded->id, "task-a", "task id round trip");
        check(loaded->state == TaskState::Queued, "state round trip");

        auto changed = *loaded;
        changed.state = TaskState::Completed;
        changed.received = 1024;
        changed.error.clear();
        changed.updatedAt = 2000;
        repository.update(changed);
        loaded = repository.find("task-a");
        check(loaded.has_value(), "updated task can be found");
        check(loaded->state == TaskState::Completed, "updated state is persisted");
        checkEqual(loaded->received, 1024, "updated byte count is persisted");
        checkEqual(loaded->updatedAt, 2000, "updated timestamp is persisted");
        check(!repository.find("missing").has_value(), "missing task is absent");
    }

    // Reopen: the durable format, not process memory, is the source of truth.
    {
        TaskRepository repository(database);
        const auto tasks = repository.all();
        checkEqual(static_cast<long long>(tasks.size()), 3, "rows survive reopen");
        check(tasks[0].state == TaskState::Completed, "completed state survives reopen");
        check(tasks[0].received == 1024, "progress survives reopen");

        auto missing = sample("not-inserted");
        bool rejected = false;
        try {
            repository.update(missing);
        } catch (const StorageError&) {
            rejected = true;
        }
        check(rejected, "updating an absent row is a storage failure");
    }

    // A read-only database must not accept a commit.
    {
        TaskRepository readOnly = TaskRepository::openReadOnly(database);
        bool rejected = false;
        try {
            readOnly.insert(sample("task-c"));
        } catch (const StorageError&) {
            rejected = true;
        }
        check(rejected, "read-only database rejects insert");
    }

    legacyCoexistence(directory.file("legacy.db"));
    upgradesSchemaTwo(directory.file("schema2.db"));
    return 0;
}
} // namespace

int main()
{
    return run("store", &body);
}
