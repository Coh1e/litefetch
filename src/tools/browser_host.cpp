// The native messaging host: speaks the browser's framing on stdin/stdout and
// forwards each request to the running application over the local channel.
//
// It is a thin shell over runHostLoop(), which is where the behaviour lives and
// which the tests drive without starting a second process.
#include "native_host_loop.h"
#include "pipe_channel.h"

#include <cstdio>
#include <iostream>
#include <string>

int wmain(int argc, wchar_t** argv)
{
    // The browser passes the calling extension's origin as the first argument.
    // This host serves one origin, so it only reports what it was given.
    if (argc > 1) {
        std::fwprintf(stderr, L"litefetch browser host: origin %s\n", argv[1]);
    }
    const auto pipeName = litefetch::browser::defaultPipeName();
    const auto channel = [pipeName](const std::string& request) {
        std::string error;
        const auto answer = litefetch::browser::pipeRoundTrip(pipeName, request, 5000, error);
        if (answer.empty()) throw std::runtime_error(error.empty() ? "no answer" : error);
        return answer;
    };
    return litefetch::browser::runHostLoop(std::cin, std::cout, channel);
}
