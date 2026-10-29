#include "pipe_channel.h"

#include "native_frame.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace litefetch::browser {
namespace {
bool readExact(HANDLE pipe, char* data, DWORD length)
{
    DWORD total = 0;
    while (total < length) {
        DWORD read = 0;
        if (!ReadFile(pipe, data + total, length - total, &read, nullptr) || read == 0) return false;
        total += read;
    }
    return true;
}

bool writeAll(HANDLE pipe, const std::string& bytes)
{
    DWORD total = 0;
    while (total < bytes.size()) {
        DWORD written = 0;
        if (!WriteFile(pipe, bytes.data() + total, static_cast<DWORD>(bytes.size() - total),
                &written, nullptr)
            || written == 0)
            return false;
        total += written;
    }
    return true;
}
} // namespace

std::string defaultPipeName()
{
    // Tests and side-by-side installs can point at another channel without a
    // rebuild; the shipped app uses the fixed name the manifest expects.
    char buffer[256]{};
    const auto length = GetEnvironmentVariableA("LITEFETCH_BRIDGE_PIPE", buffer, sizeof(buffer));
    if (length > 0 && length < sizeof(buffer)) return std::string(buffer, length);
    return "litefetch-browser";
}

std::string pipeRoundTrip(const std::string& pipeName, const std::string& request,
    int timeoutMillis, std::string& error)
{
    const auto fullName = "\\\\.\\pipe\\" + pipeName;
    // A short retry handles the moment when the app is starting its server.
    const auto deadline = GetTickCount64() + static_cast<DWORD>(timeoutMillis);
    HANDLE pipe = INVALID_HANDLE_VALUE;
    for (;;) {
        pipe = CreateFileW(std::wstring(fullName.begin(), fullName.end()).c_str(),
            GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) break;
        if (GetTickCount64() >= deadline) {
            error = "cannot reach the running application";
            return {};
        }
        Sleep(50);
    }

    std::string answer;
    if (!writeAll(pipe, frameMessage(request))) {
        error = "cannot send the request";
        CloseHandle(pipe);
        return {};
    }
    char header[4]{};
    if (!readExact(pipe, header, 4)) {
        error = "the application closed the channel";
        CloseHandle(pipe);
        return {};
    }
    const auto size = static_cast<std::size_t>(static_cast<unsigned char>(header[0]))
        | (static_cast<std::size_t>(static_cast<unsigned char>(header[1])) << 8)
        | (static_cast<std::size_t>(static_cast<unsigned char>(header[2])) << 16)
        | (static_cast<std::size_t>(static_cast<unsigned char>(header[3])) << 24);
    if (size == 0 || size > maxNativeMessageBytes) {
        error = "the application sent an invalid answer";
        CloseHandle(pipe);
        return {};
    }
    answer.resize(size);
    if (!readExact(pipe, answer.data(), static_cast<DWORD>(size))) {
        error = "the answer was cut short";
        CloseHandle(pipe);
        return {};
    }
    CloseHandle(pipe);
    return answer;
}

std::string pipeRoundTripStarting(const std::string& pipeName, const std::string& request,
    int timeoutMillis, const std::function<void()>& startApplication,
    int attempts, int delayMillis, std::string* error)
{
    std::string reason;
    // The first try uses a short budget: when nobody is listening there is no
    // point waiting out the full timeout before starting the application.
    auto answer = pipeRoundTrip(pipeName, request, 300, reason);
    if (!answer.empty() || !startApplication) {
        if (error) *error = reason;
        return answer;
    }
    startApplication();
    for (int attempt = 0; attempt < attempts; ++attempt) {
        Sleep(static_cast<DWORD>(delayMillis));
        answer = pipeRoundTrip(pipeName, request, 300, reason);
        if (!answer.empty()) return answer;
    }
    if (error) *error = reason.empty() ? "the application did not start" : reason;
    return {};
}

} // namespace litefetch::browser
