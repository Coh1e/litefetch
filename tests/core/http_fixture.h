#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace litefetch::test {

// Minimal controlled HTTP peer for the command -> transfer -> file boundary.
// It serves one deterministic body on every path except /missing (404).
class HttpFixture {
public:
    struct Options {
        std::vector<unsigned char> body;
        int status = 200;
        std::size_t chunk = 16 * 1024;
        std::chrono::milliseconds chunkDelay{0};
    };

    explicit HttpFixture(Options options) : options_(std::move(options))
    {
        static const bool winsockReady = [] {
            WSADATA data{};
            if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
                throw std::runtime_error("WSAStartup failed");
            return true;
        }();
        (void)winsockReady;

        listener_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener_ == INVALID_SOCKET) throw std::runtime_error("fixture socket failed");
        BOOL reuse = TRUE;
        setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        if (bind(listener_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0)
            throw std::runtime_error("fixture bind failed");
        if (listen(listener_, 8) != 0) throw std::runtime_error("fixture listen failed");
        int length = sizeof(address);
        if (getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &length) != 0)
            throw std::runtime_error("fixture getsockname failed");
        port_ = ntohs(address.sin_port);
        thread_ = std::jthread([this] { serve(); });
    }

    ~HttpFixture() { stop(); }
    HttpFixture(const HttpFixture&) = delete;
    HttpFixture& operator=(const HttpFixture&) = delete;

    std::string url(const std::string& path = "/payload.bin") const
    {
        return "http://127.0.0.1:" + std::to_string(port_) + path;
    }

    void stop()
    {
        stopping_.store(true);
        const SOCKET listener = listener_.exchange(INVALID_SOCKET);
        if (listener != INVALID_SOCKET) closesocket(listener);
        if (thread_.joinable()) thread_.join();
    }

private:
    void serve()
    {
        while (!stopping_.load()) {
            const SOCKET listener = listener_.load();
            if (listener == INVALID_SOCKET) break;
            const SOCKET client = accept(listener, nullptr, nullptr);
            if (client == INVALID_SOCKET) {
                if (stopping_.load()) break;
                continue;
            }
            handle(client);
            closesocket(client);
        }
    }

    void handle(SOCKET client)
    {
        std::string request;
        char buffer[2048];
        while (request.find("\r\n\r\n") == std::string::npos && request.size() < 16384) {
            const int received = recv(client, buffer, sizeof(buffer), 0);
            if (received <= 0) return;
            request.append(buffer, static_cast<std::size_t>(received));
        }
        const auto lineEnd = request.find("\r\n");
        const auto firstSpace = request.find(' ');
        if (lineEnd == std::string::npos || firstSpace == std::string::npos) return;
        const auto secondSpace = request.find(' ', firstSpace + 1);
        if (secondSpace == std::string::npos) return;
        const std::string path = request.substr(firstSpace + 1, secondSpace - firstSpace - 1);

        if (path == "/missing" || options_.status != 200) {
            const int status = path == "/missing" ? 404 : options_.status;
            const std::string reason = status == 404 ? "Not Found" : "Error";
            const std::string response = "HTTP/1.1 " + std::to_string(status) + " " + reason
                + "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            sendAll(client, response.data(), response.size());
            return;
        }

        const std::string header = "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: "
            + std::to_string(options_.body.size()) + "\r\nConnection: close\r\n\r\n";
        if (!sendAll(client, header.data(), header.size())) return;

        const std::size_t chunk = options_.chunk == 0 ? options_.body.size() : options_.chunk;
        for (std::size_t offset = 0; offset < options_.body.size(); offset += chunk) {
            if (stopping_.load()) return;
            if (options_.chunkDelay.count() > 0)
                std::this_thread::sleep_for(options_.chunkDelay);
            const auto size = (std::min)(chunk, options_.body.size() - offset);
            if (!sendAll(client, reinterpret_cast<const char*>(options_.body.data() + offset), size)) return;
        }
    }

    bool sendAll(SOCKET socket, const char* data, std::size_t size)
    {
        while (size > 0) {
            const int sent = send(socket, data, static_cast<int>((std::min)(size, static_cast<std::size_t>(0x7fffffff))), 0);
            if (sent <= 0) return false;
            data += sent;
            size -= static_cast<std::size_t>(sent);
        }
        return true;
    }

    Options options_;
    std::atomic<SOCKET> listener_{INVALID_SOCKET};
    std::atomic_bool stopping_{false};
    unsigned short port_ = 0;
    std::jthread thread_;
};

} // namespace litefetch::test
