#include "task_store.h"

#include "text_utf8.h"

#include <sqlite3.h>

#include <system_error>
#include <utility>

namespace litefetch {
namespace {
[[noreturn]] void fail(sqlite3* database)
{
    throw StorageError(database ? sqlite3_errmsg(database) : "database is closed");
}

class Statement {
public:
    Statement(sqlite3* database, const char* sql) : database_(database)
    {
        if (sqlite3_prepare_v2(database_, sql, -1, &statement_, nullptr) != SQLITE_OK) fail(database_);
    }
    ~Statement() { sqlite3_finalize(statement_); }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    void text(int index, std::string_view value)
    {
        if (value.size() > static_cast<std::size_t>(0x7fffffff)) throw StorageError("text value is too large");
        if (sqlite3_bind_text(statement_, index, value.data(), static_cast<int>(value.size()),
                SQLITE_TRANSIENT) != SQLITE_OK)
            fail(database_);
    }
    void integer(int index, std::int64_t value)
    {
        if (sqlite3_bind_int64(statement_, index, value) != SQLITE_OK) fail(database_);
    }
    bool row()
    {
        const int code = sqlite3_step(statement_);
        if (code == SQLITE_ROW) return true;
        if (code == SQLITE_DONE) return false;
        fail(database_);
    }
    void done()
    {
        if (sqlite3_step(statement_) != SQLITE_DONE) fail(database_);
    }
    std::string text(int column) const
    {
        const auto* value = sqlite3_column_text(statement_, column);
        return value ? std::string(reinterpret_cast<const char*>(value),
                           static_cast<std::size_t>(sqlite3_column_bytes(statement_, column)))
                     : std::string();
    }
    std::int64_t integer(int column) const { return sqlite3_column_int64(statement_, column); }

private:
    sqlite3* database_ = nullptr;
    sqlite3_stmt* statement_ = nullptr;
};

void execute(sqlite3* database, const char* sql)
{
    if (sqlite3_exec(database, sql, nullptr, nullptr, nullptr) != SQLITE_OK) fail(database);
}

// BitTorrent file selection is stored as an order-preserving "index:priority"
// list; a plain "index" entry (the original format) means normal priority.
void parseFileEntries(std::string_view text, std::vector<int>& files,
    std::vector<int>& priorities)
{
    files.clear();
    priorities.clear();
    std::size_t start = 0;
    while (start < text.size()) {
        const auto end = text.find_first_of(",;", start);
        const auto item = text.substr(start,
            end == std::string_view::npos ? std::string_view::npos : end - start);
        if (!item.empty()) {
            const auto colon = item.find(':');
            int index = 0;
            int priority = 1;
            const auto indexText = colon == std::string_view::npos ? item : item.substr(0, colon);
            const auto priorityText = colon == std::string_view::npos
                ? std::string_view("1") : item.substr(colon + 1);
            const auto indexResult = std::from_chars(indexText.data(),
                indexText.data() + indexText.size(), index);
            const auto priorityResult = std::from_chars(priorityText.data(),
                priorityText.data() + priorityText.size(), priority);
            if (indexResult.ec == std::errc{} && index >= 0) {
                files.push_back(index);
                priorities.push_back(priorityResult.ec == std::errc{} ? priority : 1);
            }
        }
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
}

std::string serializeFileList(const std::vector<int>& files, const std::vector<int>& priorities)
{
    std::string text;
    for (std::size_t position = 0; position < files.size(); ++position) {
        if (!text.empty()) text += ';';
        text += std::to_string(files[position]);
        text += ':';
        text += std::to_string(position < priorities.size() ? priorities[position] : 1);
    }
    return text;
}

std::vector<std::string> parseTrackerList(std::string_view text)
{
    std::vector<std::string> trackers;
    std::size_t start = 0;
    while (start < text.size()) {
        const auto end = text.find('\n', start);
        const auto item = text.substr(start,
            end == std::string_view::npos ? std::string_view::npos : end - start);
        if (!item.empty()) trackers.emplace_back(item);
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return trackers;
}

std::string serializeTrackerList(const std::vector<std::string>& trackers)
{
    std::string text;
    for (const auto& tracker : trackers) {
        if (!text.empty()) text += '\n';
        text += tracker;
    }
    return text;
}

TaskSnapshot readTask(Statement& statement)
{
    TaskSnapshot task;
    task.id = statement.text(0);
    task.source = statement.text(1);
    task.destination = statement.text(2);
    const auto state = parseTaskState(statement.text(3));
    if (!state) throw StorageError("Stored task state is not recognized");
    task.state = *state;
    task.received = statement.integer(4);
    task.total = statement.integer(5);
    task.error = statement.text(6);
    task.restartRequired = statement.integer(9) != 0;
    parseFileEntries(statement.text(10), task.files, task.filePriorities);
    task.trackers = parseTrackerList(statement.text(11));
    task.createdAt = statement.integer(7);
    task.updatedAt = statement.integer(8);
    return task;
}

// Schema upgrades keep every row. Version 1 predates the persisted 'paused'
// state and is rebuilt with the same shape plus the restart flag; version 2
// only needs the new column.
void migrateSchema(sqlite3* database)
{
    std::string version;
    {
        Statement statement(database, "SELECT value FROM native_meta WHERE key='schema_version'");
        if (!statement.row()) return;
        version = statement.text(0);
    }
    if (version == "1") {
        execute(database,
            "DROP TABLE IF EXISTS native_tasks_v2;"
            "CREATE TABLE native_tasks_v2("
            "id TEXT PRIMARY KEY NOT NULL,"
            "source TEXT NOT NULL,"
            "destination TEXT NOT NULL,"
            "state TEXT NOT NULL,"
            "received INTEGER NOT NULL CHECK(received >= 0),"
            "total INTEGER NOT NULL CHECK(total >= -1),"
            "error TEXT NOT NULL,"
            "created_at INTEGER NOT NULL,"
            "updated_at INTEGER NOT NULL,"
            "restart_required INTEGER NOT NULL DEFAULT 0,"
            "bt_files TEXT NOT NULL DEFAULT '',"
            "bt_trackers TEXT NOT NULL DEFAULT '' );"
            "INSERT INTO native_tasks_v2(id,source,destination,state,received,total,error,"
            "created_at,updated_at) SELECT id,source,destination,state,received,total,error,"
            "created_at,updated_at FROM native_tasks;"
            "DROP TABLE native_tasks;"
            "ALTER TABLE native_tasks_v2 RENAME TO native_tasks;");
    } else if (version == "2") {
        execute(database,
            "ALTER TABLE native_tasks ADD COLUMN restart_required INTEGER NOT NULL DEFAULT 0;"
            "ALTER TABLE native_tasks ADD COLUMN bt_files TEXT NOT NULL DEFAULT '';"
            "ALTER TABLE native_tasks ADD COLUMN bt_trackers TEXT NOT NULL DEFAULT '';");
    } else if (version == "3") {
        execute(database,
            "ALTER TABLE native_tasks ADD COLUMN bt_files TEXT NOT NULL DEFAULT '';"
            "ALTER TABLE native_tasks ADD COLUMN bt_trackers TEXT NOT NULL DEFAULT '';");
    } else if (version == "4") {
        execute(database, "ALTER TABLE native_tasks ADD COLUMN bt_trackers TEXT NOT NULL DEFAULT '';");
    } else {
        return;
    }
    execute(database, "UPDATE native_meta SET value='5' WHERE key='schema_version';");
}

void verifyVersion(sqlite3* database)
{
    Statement statement(database, "SELECT value FROM native_meta WHERE key='schema_version'");
    if (!statement.row()) throw StorageError("Database is not a LiteFetch native task database");
    const auto version = statement.text(0);
    // 1: original states. 2: paused. 3: restart flag. 4: torrent files.
    // 5: user trackers.
    if (version != "1" && version != "2" && version != "3" && version != "4" && version != "5")
        throw StorageError("Database was created by a newer LiteFetch native version");
}

sqlite3* openDatabase(const std::filesystem::path& path, bool readOnly)
{
    if (!readOnly && path.has_parent_path()) {
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        if (error) throw StorageError("Cannot create database directory: " + error.message());
    }
    int flags = readOnly ? SQLITE_OPEN_READONLY : (SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
    flags |= SQLITE_OPEN_FULLMUTEX;
    sqlite3* database = nullptr;
    const std::string utf8 = pathToUtf8(path);
    const int code = sqlite3_open_v2(utf8.c_str(), &database, flags, nullptr);
    if (code != SQLITE_OK) {
        const std::string message = database ? sqlite3_errmsg(database) : "Cannot allocate database";
        sqlite3_close(database);
        throw StorageError(message);
    }
    sqlite3_busy_timeout(database, 5000);
    return database;
}
} // namespace

TaskRepository::TaskRepository(const std::filesystem::path& databasePath)
    : database_(openDatabase(databasePath, false))
{
    try {
        initialize();
    } catch (...) {
        sqlite3_close(database_);
        database_ = nullptr;
        throw;
    }
}

TaskRepository::TaskRepository(sqlite3* database, bool readOnly) : database_(database), readOnly_(readOnly) {}
TaskRepository TaskRepository::openReadOnly(const std::filesystem::path& databasePath)
{
    auto* database = openDatabase(databasePath, true);
    try {
        execute(database, "PRAGMA query_only=ON;");
        verifyVersion(database);
    } catch (...) {
        sqlite3_close(database);
        throw;
    }
    return TaskRepository(database, true);
}

TaskRepository::~TaskRepository()
{
    if (database_) sqlite3_close(database_);
}

TaskRepository::TaskRepository(TaskRepository&& other) noexcept
    : database_(other.database_), readOnly_(other.readOnly_)
{
    other.database_ = nullptr;
}

void TaskRepository::initialize()
{
    execute(database_, "PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL;");
    execute(database_, "BEGIN IMMEDIATE;");
    try {
        execute(database_,
            "CREATE TABLE IF NOT EXISTS native_meta("
            "key TEXT PRIMARY KEY NOT NULL,value TEXT NOT NULL);"
            "CREATE TABLE IF NOT EXISTS native_tasks("
            "id TEXT PRIMARY KEY NOT NULL,"
            "source TEXT NOT NULL,"
            "destination TEXT NOT NULL,"
            "state TEXT NOT NULL,"
            "received INTEGER NOT NULL CHECK(received >= 0),"
            "total INTEGER NOT NULL CHECK(total >= -1),"
            "error TEXT NOT NULL,"
            "created_at INTEGER NOT NULL,"
            "updated_at INTEGER NOT NULL,"
            "restart_required INTEGER NOT NULL DEFAULT 0,"
            "bt_files TEXT NOT NULL DEFAULT '',"
            "bt_trackers TEXT NOT NULL DEFAULT '');");
        execute(database_, "INSERT OR IGNORE INTO native_meta(key,value) VALUES('schema_version','1');");
        migrateSchema(database_);
        execute(database_, "COMMIT;");
    } catch (...) {
        sqlite3_exec(database_, "ROLLBACK", nullptr, nullptr, nullptr);
        throw;
    }
    verifyVersion(database_);
}

void TaskRepository::insert(const TaskSnapshot& task)
{
    std::lock_guard lock(mutex_);
    if (readOnly_) throw StorageError("Database is read-only");
    Statement statement(database_,
        "INSERT INTO native_tasks(id,source,destination,state,received,total,error,created_at,updated_at,restart_required,bt_files,bt_trackers) "
        "VALUES(?,?,?,?,?,?,?,?,?,?,?,?)");
    statement.text(1, task.id);
    statement.text(2, task.source);
    statement.text(3, task.destination);
    statement.text(4, taskStateName(task.state));
    statement.integer(5, task.received);
    statement.integer(6, task.total);
    statement.text(7, task.error);
    statement.integer(8, task.createdAt);
    statement.integer(9, task.updatedAt);
    statement.integer(10, task.restartRequired ? 1 : 0);
    statement.text(11, serializeFileList(task.files, task.filePriorities));
    statement.text(12, serializeTrackerList(task.trackers));
    statement.done();
}

void TaskRepository::update(const TaskSnapshot& task)
{
    std::lock_guard lock(mutex_);
    if (readOnly_) throw StorageError("Database is read-only");
    Statement statement(database_,
        "UPDATE native_tasks SET source=?,destination=?,state=?,received=?,total=?,error=?,updated_at=?,"
        "restart_required=?,bt_files=?,bt_trackers=? WHERE id=?");
    statement.text(1, task.source);
    statement.text(2, task.destination);
    statement.text(3, taskStateName(task.state));
    statement.integer(4, task.received);
    statement.integer(5, task.total);
    statement.text(6, task.error);
    statement.integer(7, task.updatedAt);
    statement.integer(8, task.restartRequired ? 1 : 0);
    statement.text(9, serializeFileList(task.files, task.filePriorities));
    statement.text(10, serializeTrackerList(task.trackers));
    statement.text(11, task.id);
    statement.done();
    if (sqlite3_changes(database_) != 1) throw StorageError("Task row was not updated");
}

void TaskRepository::remove(std::string_view id)
{
    std::lock_guard lock(mutex_);
    if (readOnly_) throw StorageError("Database is read-only");
    Statement statement(database_, "DELETE FROM native_tasks WHERE id=?");
    statement.text(1, id);
    statement.done();
    if (sqlite3_changes(database_) != 1) throw StorageError("Task row was not deleted");
}

std::optional<TaskSnapshot> TaskRepository::find(std::string_view id)
{
    std::lock_guard lock(mutex_);
    Statement statement(database_, "SELECT id,source,destination,state,received,total,error,created_at,updated_at,restart_required,bt_files,bt_trackers "
        "FROM native_tasks WHERE id=?");
    statement.text(1, id);
    if (!statement.row()) return std::nullopt;
    return readTask(statement);
}

std::vector<TaskSnapshot> TaskRepository::all()
{
    std::lock_guard lock(mutex_);
    Statement statement(database_, "SELECT id,source,destination,state,received,total,error,created_at,updated_at,restart_required,bt_files,bt_trackers "
        "FROM native_tasks ORDER BY created_at,id");
    std::vector<TaskSnapshot> tasks;
    while (statement.row()) tasks.push_back(readTask(statement));
    return tasks;
}

} // namespace litefetch
