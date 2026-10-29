#include "browser_pipe_server.h"

#include "native_frame.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdio>

namespace litefetch::browser {
namespace {
std::wstring wideOf(const std::string& text)
{
    return std::wstring(text.begin(), text.end());
}

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

BrowserPipeServer::BrowserPipeServer(std::string pipeName) : pipeName_(std::move(pipeName)) {}

BrowserPipeServer::~BrowserPipeServer()
{
    stop();
}

bool BrowserPipeServer::start(BrowserBridge& bridge, std::string& error)
{
    if (thread_.joinable()) return true;
    const auto fullName = L"\\\\.\\pipe\\" + wideOf(pipeName_);
    // Create it once here so a failure to bind is reported to the caller instead
    // of only failing inside the thread.
    HANDLE pipe = CreateNamedPipeW(fullName.c_str(), PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 64 * 1024, 64 * 1024, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
        error = "cannot create the local channel";
        return false;
    }
    listener_.store(reinterpret_cast<std::uintptr_t>(pipe));
    stopping_.store(false);
    thread_ = std::thread([this, &bridge] { serve(bridge); });
    return true;
}

void BrowserPipeServer::serve(BrowserBridge& bridge)
{
    const auto fullName = L"\\\\.\\pipe\\" + wideOf(pipeName_);
    for (;;) {
        if (stopping_.load()) break;
        auto pipe = reinterpret_cast<HANDLE>(listener_.load());
        if (pipe == INVALID_HANDLE_VALUE || pipe == nullptr) break;
        const bool connected = ConnectNamedPipe(pipe, nullptr) != 0
            || GetLastError() == ERROR_PIPE_CONNECTED;
        if (!connected) {
            if (stopping_.load()) break;
            continue;
        }
        char header[4]{};
        if (readExact(pipe, header, 4)) {
            const auto size = static_cast<std::size_t>(static_cast<unsigned char>(header[0]))
                | (static_cast<std::size_t>(static_cast<unsigned char>(header[1])) << 8)
                | (static_cast<std::size_t>(static_cast<unsigned char>(header[2])) << 16)
                | (static_cast<std::size_t>(static_cast<unsigned char>(header[3])) << 24);
            if (size > 0 && size <= maxNativeMessageBytes) {
                std::string request(size, '\0');
                if (readExact(pipe, request.data(), static_cast<DWORD>(size))) {
                    const auto answer = bridge.handle(request);
                    writeAll(pipe, frameMessage(answer));
                }
            }
        }
        // Close rather than DisconnectNamedPipe: disconnecting flushes data the
        // client may not have read yet.
        CloseHandle(pipe);
        if (stopping_.load()) break;
        pipe = CreateNamedPipeW(fullName.c_str(), PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 64 * 1024, 64 * 1024, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE) break;
        listener_.store(reinterpret_cast<std::uintptr_t>(pipe));
    }
    listener_.store(0);
}

void BrowserPipeServer::stop()
{
    if (!thread_.joinable()) return;
    stopping_.store(true);
    const auto pipe = reinterpret_cast<HANDLE>(listener_.load());
    if (pipe != nullptr && pipe != INVALID_HANDLE_VALUE) {
        // Wake the blocked ConnectNamedPipe so the thread can exit.
        const auto fullName = L"\\\\.\\pipe\\" + wideOf(pipeName_);
        HANDLE wake = CreateFileW(fullName.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
            OPEN_EXISTING, 0, nullptr);
        if (wake != INVALID_HANDLE_VALUE) CloseHandle(wake);
    }
    thread_.join();
}

} // namespace litefetch::browser
