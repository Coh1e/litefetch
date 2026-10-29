#pragma once

#include "task.h"

#include <filesystem>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <vector>

struct sqlite3;

namespace litefetch {

class StorageError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Persistence boundary for the Qt-free task core. Construction creates or
// migrates the native schema; every commit is synchronous and is either fully
// durable or reported as StorageError. The repository is safe to use from the
// command thread and a progress callback thread; SQLite access is serialized.
class TaskRepository {
public:
    explicit TaskRepository(const std::filesystem::path& databasePath);
    static TaskRepository openReadOnly(const std::filesystem::path& databasePath);
    ~TaskRepository();
    TaskRepository(const TaskRepository&) = delete;
    TaskRepository& operator=(const TaskRepository&) = delete;
    TaskRepository(TaskRepository&& other) noexcept;
    TaskRepository& operator=(TaskRepository&& other) = delete;

    void insert(const TaskSnapshot& task);
    void update(const TaskSnapshot& task);
    // Deletes the record only. Callers are responsible for stopping writers
    // first; owned files are never removed here.
    void remove(std::string_view id);
    // Key/value area of the same database; used for user settings.
    std::string metaValue(std::string_view key) const;
    void setMetaValue(std::string_view key, std::string value);
    std::optional<TaskSnapshot> find(std::string_view id);
    std::vector<TaskSnapshot> all();

private:
    TaskRepository(sqlite3* database, bool readOnly);
    void initialize();
    void verifySchema();
    sqlite3* database_ = nullptr;
    bool readOnly_ = false;
    std::mutex mutex_;
};

} // namespace litefetch
