#include "task_service.h"
#include "task_store.h"
#include "text_utf8.h"

#include <filesystem>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
using namespace litefetch;

std::string escape(std::string_view value)
{
    std::string result;
    result.reserve(value.size() + 8);
    for (const char c : value) {
        switch (c) {
        case '"': result += "\\\""; break;
        case '\\': result += "\\\\"; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                static constexpr char hex[] = "0123456789abcdef";
                result += "\\u00";
                result.push_back(hex[(static_cast<unsigned char>(c) >> 4) & 0x0f]);
                result.push_back(hex[static_cast<unsigned char>(c) & 0x0f]);
            } else {
                result.push_back(c);
            }
        }
    }
    return result;
}

std::string taskJson(const TaskSnapshot& task)
{
    return "{\"id\":\"" + escape(task.id) + "\",\"source\":\"" + escape(task.source)
        + "\",\"destination\":\"" + escape(task.destination) + "\",\"state\":\""
        + std::string(taskStateName(task.state)) + "\",\"received\":" + std::to_string(task.received)
        + ",\"total\":" + std::to_string(task.total) + ",\"error\":\"" + escape(task.error)
        + "\",\"createdAt\":" + std::to_string(task.createdAt)
        + ",\"updatedAt\":" + std::to_string(task.updatedAt) + "}";
}

void printResult(const CommandResult& result)
{
    std::cout << "{\"ok\":" << (result.ok ? "true" : "false") << ",\"task\":"
              << (result.task.id.empty() ? "null" : taskJson(result.task))
              << ",\"error\":\"" << escape(result.error) << "\"}\n";
}

void usage()
{
    std::cerr << "usage:\n"
              << "  litefetch-core-cli add --db <path> --source <url> --destination <path>\n"
              << "  litefetch-core-cli start --db <path> --id <task-id>\n"
              << "  litefetch-core-cli cancel --db <path> --id <task-id>\n"
              << "  litefetch-core-cli list --db <path>\n";
}

std::map<std::string, std::string> options(const std::vector<std::string>& arguments, int start)
{
    std::map<std::string, std::string> result;
    for (int i = start; i < static_cast<int>(arguments.size()); ++i) {
        const std::string key = arguments[static_cast<std::size_t>(i)];
        if (key.rfind("--", 0) != 0) throw std::runtime_error("unexpected argument: " + key);
        if (i + 1 >= static_cast<int>(arguments.size())) throw std::runtime_error("missing value for " + key);
        result[key.substr(2)] = arguments[static_cast<std::size_t>(i) + 1];
        ++i;
    }
    return result;
}

std::string required(const std::map<std::string, std::string>& values, const std::string& key)
{
    const auto found = values.find(key);
    if (found == values.end() || found->second.empty()) throw std::runtime_error("missing --" + key);
    return found->second;
}
} // namespace

int run(const std::vector<std::string>& arguments)
{
    if (arguments.size() < 3) {
        usage();
        return 1;
    }
    try {
        const std::string command = arguments[1];
        const auto values = options(arguments, 2);
        const auto database = required(values, "db");
        const std::filesystem::path databasePath = pathFromUtf8(database);
        if (command == "list") {
            // A read-only connection never takes a write lock, so listing while
            // the Win32 window is transferring cannot block the task writer.
            TaskRepository repository = TaskRepository::openReadOnly(databasePath);
            TaskService service(repository);
            std::cout << "{\"ok\":true,\"tasks\":[";
            bool first = true;
            for (const auto& task : service.snapshots()) {
                if (!first) std::cout << ',';
                first = false;
                std::cout << taskJson(task);
            }
            std::cout << "]}\n";
            return 0;
        }
        TaskRepository repository(databasePath);
        TaskService service(repository);
        if (command == "add") {
            const auto result = service.execute(AddTaskCommand{
                required(values, "source"), required(values, "destination")});
            printResult(result);
            return result.ok ? 0 : 2;
        }
        if (command == "start") {
            const auto result = service.execute(StartTaskCommand{required(values, "id")});
            printResult(result);
            // A committed but failed/cancelled transfer is still a failed CLI run.
            return result.ok && result.task.state == TaskState::Completed ? 0 : 2;
        }
        if (command == "cancel") {
            const auto result = service.execute(CancelTaskCommand{required(values, "id")});
            printResult(result);
            return result.ok && result.task.state == TaskState::Cancelled ? 0 : 2;
        }
        if (command == "list") {
            std::cout << "{\"ok\":true,\"tasks\":[";
            bool first = true;
            for (const auto& task : service.snapshots()) {
                if (!first) std::cout << ',';
                first = false;
                std::cout << taskJson(task);
            }
            std::cout << "]}\n";
            return 0;
        }
        usage();        return 1;
    } catch (const std::exception& error) {
        std::cerr << "litefetch-core-cli: " << error.what() << '\n';
        return 2;
    }
}

#ifdef _WIN32
int wmain(int argc, wchar_t** argv)
{
    std::vector<std::string> arguments;
    arguments.reserve(static_cast<std::size_t>(argc));
    try {
        for (int i = 0; i < argc; ++i) arguments.push_back(utf8FromUtf16(argv[i]));
    } catch (const std::exception& error) {
        std::cerr << "litefetch-core-cli: " << error.what() << '\n';
        return 2;
    }
    return run(arguments);
}
#else
int main(int argc, char** argv)
{
    std::vector<std::string> arguments(argv, argv + argc);
    return run(arguments);
}
#endif
