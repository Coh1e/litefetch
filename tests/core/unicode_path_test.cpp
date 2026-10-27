// Unicode (Chinese) destination and database paths must survive the UTF-8
// string command boundary and reach the real Windows file system unchanged.
#include "http_fixture.h"
#include "task_service.h"
#include "task_store.h"
#include "test_support.h"

#include <filesystem>
#include <stdexcept>
#include <string>

using namespace litefetch;
using namespace litefetch::test;

namespace {
std::string utf8(const char8_t* text)
{
    return std::string(reinterpret_cast<const char*>(text));
}

void unicodeRoundTrip(const std::filesystem::path& root)
{
    const auto body = payload(64 * 1024);
    HttpFixture server(HttpFixture::Options{body});

    const std::filesystem::path folder = root / std::filesystem::path(L"下载目录");
    std::filesystem::create_directories(folder);
    const std::filesystem::path destination = folder / std::filesystem::path(L"工具包.zip");
    const std::filesystem::path databaseFolder = root / std::filesystem::path(L"任务数据库");
    const std::filesystem::path database = databaseFolder / "tasks.db";

    // Command text is UTF-8 exactly as produced by the Win32 window.
    const std::string folderUtf8 = root.string() + "/" + utf8(u8"下载目录");
    const std::string destinationUtf8 = folderUtf8 + "/" + utf8(u8"工具包.zip");

    TaskRepository repository(database);
    TaskService service(repository);
    const auto added = service.execute(AddTaskCommand{server.url(), destinationUtf8});
    check(added.ok, ("add failed: " + added.error).c_str());
    check(added.task.destination == destinationUtf8, "stored destination is not the UTF-8 input");

    const auto started = service.execute(StartTaskCommand{added.task.id});
    check(started.ok, ("start failed: " + started.error).c_str());
    check(started.task.state == TaskState::Completed,
        ("transfer did not complete: " + std::string(taskStateName(started.task.state)) + " "
            + started.task.error)
            .c_str());
    check(std::filesystem::exists(destination), "file was not created at the Chinese destination path");
    check(readFile(destination) == body, "downloaded bytes differ at the Chinese destination path");
    check(std::filesystem::exists(database), "database was not created at the Chinese database path");

    // The snapshot must still carry the original UTF-8 text after reopening.
    TaskRepository reopened(database);
    TaskService reopenedService(reopened);
    const auto persisted = reopenedService.snapshot(added.task.id);
    check(persisted.has_value(), "task did not survive reopening the Chinese database path");
    check(persisted->destination == destinationUtf8, "persisted destination is not UTF-8");
    for (const auto& entry : std::filesystem::directory_iterator(folder)) {
        const auto name = entry.path().filename().wstring();
        check(name == L"工具包.zip", "unexpected file name next to the Chinese destination");
    }
}

int body()
{
    TempDir root;
    unicodeRoundTrip(root.path());
    return 0;
}
} // namespace

int main()
{
    return run("unicode-path", &body);
}
