// Downloads one ed2k link to a path, with progress on stderr. Used to compare
// behaviour against a real client and to drive a download from a script.
#include "ed2k_task.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>

int main(int argc, char** argv)
{
    if (argc != 3) {
        std::fprintf(stderr, "usage: litefetch-ed2kget <ed2k-link> <destination>\n");
        return 2;
    }
    litefetch::Ed2kTaskRequest request;
    request.link = argv[1];
    request.destination = argv[2];
    request.timeoutMillis = 30000;
    // Optional server list file: one host:port per line.
    if (const char* list = std::getenv("LITEFETCH_ED2K_SERVERS")) {
        std::ifstream servers(list);
        std::string line;
        while (std::getline(servers, line)) {
            const auto colon = line.rfind(':');
            if (colon == std::string::npos) continue;
            const int port = std::atoi(line.c_str() + colon + 1);
            if (port <= 0 || port > 65535) continue;
            request.servers.push_back({line.substr(0, colon), static_cast<std::uint16_t>(port)});
        }
    }
    std::atomic_bool cancelled{false};
    const auto result = litefetch::downloadEd2kTask(request, cancelled,
        [](std::int64_t received, std::int64_t total) {
            std::fprintf(stderr, "\r%d/%d", static_cast<int>(received), static_cast<int>(total));
        });
    std::fprintf(stderr, "\n");
    if (!result.ok) {
        std::fprintf(stderr, "failed: %s\n", result.error.c_str());
        return 1;
    }
    std::printf("ok %lld bytes\n", static_cast<long long>(result.received));
    return 0;
}
