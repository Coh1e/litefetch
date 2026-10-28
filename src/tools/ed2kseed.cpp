// Serves one file over the eDonkey peer protocol so a second LiteFetch instance
// (or a real client) can download it.
#include "ed2k_serve.h"
#include "ed2k_transfer.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: litefetch-ed2kseed <file> [port]\n");
        return 2;
    }
    const auto hash = litefetch::ed2kFileHash(argv[1]);
    if (hash.empty()) {
        std::fprintf(stderr, "cannot hash %s\n", argv[1]);
        return 1;
    }
    const auto port = argc > 2 ? static_cast<std::uint16_t>(std::atoi(argv[2])) : 0;
    litefetch::Ed2kPeerServer server(argv[1], hash, port);
    std::string error;
    if (!server.start(error)) {
        std::fprintf(stderr, "cannot start: %s\n", error.c_str());
        return 1;
    }
    std::printf("serving %s\nhash %s\nport %u\n", argv[1], hash.c_str(),
        static_cast<unsigned>(server.port()));
    std::fflush(stdout);
    for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
}
