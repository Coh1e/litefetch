#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <mutex>
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
        // Range support is what makes a resume possible; turning it off models
        // a server that ignores the request and sends the whole resource.
        bool supportsRange = true;
        std::string etag = "\"fixture-v1\"";
        // When set, a request without exactly this Authorization header gets a
        // 401 with a Basic challenge.
        std::string requiredAuthorization;
        // When set, every request is answered with a redirect to this URL.
        std::string redirectTo;
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
    unsigned short port() const { return port_; }

    // The last request seen, for credential-boundary assertions.
    std::string lastAuthorization()
    {
        std::lock_guard lock(stateMutex_);
        return lastAuthorization_;
    }
    std::string lastPath()
    {
        std::lock_guard lock(stateMutex_);
        return lastPath_;
    }
    int requestCount()
    {
        std::lock_guard lock(stateMutex_);
        return requests_;
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

        const auto header = [&](const std::string& name) {
            const auto key = "\r\n" + name + ":";
            const auto found = request.find(key);
            if (found == std::string::npos) return std::string();
            const auto start = found + key.size();
            const auto end = request.find("\r\n", start);
            auto value = request.substr(start, end == std::string::npos ? std::string::npos : end - start);
            while (!value.empty() && value.front() == ' ') value.erase(value.begin());
            return value;
        };

        {
            std::lock_guard lock(stateMutex_);
            lastAuthorization_ = header("Authorization");
            lastPath_ = path;
            ++requests_;
        }

        if (!options_.redirectTo.empty()) {
            const std::string response = "HTTP/1.1 302 Found\r\nLocation: " + options_.redirectTo
                + "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            sendAll(client, response.data(), response.size());
            return;
        }

        if (!options_.requiredAuthorization.empty()
            && header("Authorization") != options_.requiredAuthorization) {
            const std::string response = "HTTP/1.1 401 Unauthorized\r\n"
                "WWW-Authenticate: Basic realm=\"fixture\"\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            sendAll(client, response.data(), response.size());
            return;
        }

        if (path == "/missing" || options_.status != 200) {
            const int status = path == "/missing" ? 404 : options_.status;
            const std::string reason = status == 404 ? "Not Found" : "Error";
            const std::string response = "HTTP/1.1 " + std::to_string(status) + " " + reason
                + "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            sendAll(client, response.data(), response.size());
            return;
        }

        std::size_t start = 0;
        const std::string range = header("Range");
        const std::string ifRange = header("If-Range");
        bool partial = false;
        if (options_.supportsRange && !range.empty()) {
            const auto dash = range.find('-');
            const auto prefix = range.substr(0, 5);
            if (prefix == "bytes" && dash != std::string::npos) {
                std::size_t requested = 0;
                const auto digits = range.substr(range.find('=') + 1, dash - range.find('=') - 1);
                if (!digits.empty()
                    && std::from_chars(digits.data(), digits.data() + digits.size(), requested).ec
                        == std::errc{}) {
                    const bool validatorMatches = ifRange.empty() || ifRange == options_.etag;
                    if (validatorMatches && requested < options_.body.size()) {
                        start = requested;
                        partial = true;
                    } else if (validatorMatches && requested >= options_.body.size()) {
                        const std::string response = "HTTP/1.1 416 Range Not Satisfiable\r\nContent-Range: bytes */"
                            + std::to_string(options_.body.size()) + "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
                        sendAll(client, response.data(), response.size());
                        return;
                    }
                }
            }
        }

        const std::size_t remaining = options_.body.size() - start;
        std::string head = partial ? "HTTP/1.1 206 Partial Content\r\n" : "HTTP/1.1 200 OK\r\n";
        head += "Content-Type: application/octet-stream\r\n";
        head += "ETag: " + options_.etag + "\r\n";
        head += "Content-Length: " + std::to_string(remaining) + "\r\n";
        if (partial) {
            head += "Content-Range: bytes " + std::to_string(start) + "-"
                + std::to_string(options_.body.size() - 1) + "/"
                + std::to_string(options_.body.size()) + "\r\n";
        }
        head += "Connection: close\r\n\r\n";
        if (!sendAll(client, head.data(), head.size())) return;

        const std::size_t chunk = options_.chunk == 0 ? remaining : options_.chunk;
        for (std::size_t offset = start; offset < options_.body.size(); offset += chunk) {
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
    std::mutex stateMutex_;
    std::string lastAuthorization_;
    std::string lastPath_;
    int requests_ = 0;
    std::atomic<SOCKET> listener_{INVALID_SOCKET};
    std::atomic_bool stopping_{false};
    unsigned short port_ = 0;
    std::jthread thread_;
};

} // namespace litefetch::test
