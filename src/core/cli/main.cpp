#include "task_service.h"
#include "task_store.h"

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

std::map<std::string, std::string> options(int argc, char** argv, int start)
{
    std::map<std::string, std::string> result;
    for (int i = start; i < argc; ++i) {
        const std::string key = argv[i];
        if (key.rfind("--", 0) != 0) throw std::runtime_error("unexpected argument: " + key);
        if (i + 1 >= argc) throw std::runtime_error("missing value for " + key);
        result[key.substr(2)] = argv[++i];
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

int main(int argc, char** argv)
{
    if (argc < 3) {
        usage();
        return 1;
    }
    try {
        const std::string command = argv[1];
        const auto values = options(argc, argv, 2);
        const auto database = required(values, "db");
        const std::filesystem::path databasePath(database);
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
        usage();
        return 1;
    } catch (const std::exception& error) {
        std::cerr << "litefetch-core-cli: " << error.what() << '\n';
        return 2;
    }
}
