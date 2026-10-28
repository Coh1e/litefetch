// Prints the ed2k root hash of a file, which is what an ed2k link carries.
// Used to prepare fixtures for the real-client comparison and to check a
// finished download from a script.
#include "ed2k_transfer.h"

#include <cstdio>
#include <string>

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::fprintf(stderr, "usage: litefetch-ed2khash <file>\n");
        return 2;
    }
    const auto hash = litefetch::ed2kFileHash(argv[1]);
    if (hash.empty()) {
        std::fprintf(stderr, "cannot hash %s\n", argv[1]);
        return 1;
    }
    std::printf("%s\n", hash.c_str());
    return 0;
}
