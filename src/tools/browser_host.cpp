// The native messaging host: speaks the browser's framing on stdin/stdout and
// forwards each request to the running application over the local channel.
//
// It is a thin shell over runHostLoop(), which is where the behaviour lives and
// which the tests drive without starting a second process.
#include "native_host_loop.h"
#include "pipe_channel.h"

#include <cstdio>
#include <fcntl.h>
#include <io.h>
#include <iostream>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace {
// Starts the downloader that shipped beside this host. No waiting: the caller
// retries the channel while the application comes up.
void startApplicationBesideThisHost()
{
    if (GetEnvironmentVariableA("LITEFETCH_NO_AUTOSTART", nullptr, 0) > 0) return;
    wchar_t path[MAX_PATH]{};
    const auto length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return;
    std::wstring executable(path, length);
    const auto slash = executable.find_last_of(L'\\');
    if (slash == std::wstring::npos) return;
    const auto application = executable.substr(0, slash + 1) + L"litefetch-win32.exe";
    if (GetFileAttributesW(application.c_str()) == INVALID_FILE_ATTRIBUTES) return;

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    std::wstring command = L"\"" + application + L"\"";
    if (CreateProcessW(application.c_str(), command.data(), nullptr, nullptr, FALSE,
            DETACHED_PROCESS, nullptr, nullptr, &startup, &process)) {
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    }
}
} // namespace

int wmain(int argc, wchar_t** argv)
{
    // The frame is binary (a 4 byte little endian length), so the standard
    // handles must not translate line endings or stop at a control byte. Chrome
    // sends bytes, and without this the length arrives corrupted.
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);

    // The browser passes the calling extension's origin as the first argument.
    if (argc > 1) {
        std::fwprintf(stderr, L"litefetch browser host: origin %s\n", argv[1]);
    }
    const auto pipeName = litefetch::browser::defaultPipeName();
    const auto channel = [pipeName](const std::string& request) {
        std::string error;
        // The browser starts this host, and this host starts the downloader: the
        // extension has no way to launch a program, and telling the user to open
        // the application first is how "nothing happened" feels.
        const auto answer = litefetch::browser::pipeRoundTripStarting(pipeName, request,
            [] { startApplicationBesideThisHost(); });
        if (answer.empty()) throw std::runtime_error(error.empty() ? "no answer" : error);
        return answer;
    };
    return litefetch::browser::runHostLoop(std::cin, std::cout, channel);
}
