#pragma once

// Minimal passive-mode FTP server for the engine boundary tests. It speaks
// just enough of the protocol for libcurl: USER/PASS/PWD/TYPE/SIZE/EPSV/REST/
// RETR/QUIT, one control connection at a time, binary transfers only.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace litefetch::test {

class FtpFixture {
public:
    struct Options {
        std::vector<unsigned char> body;
        std::string username = "user";
        std::string password = "pass";
        bool supportsRest = true;
        bool fileExists = true;
        std::chrono::milliseconds chunkDelay{0};
        std::size_t chunk = 16 * 1024;
    };

    explicit FtpFixture(Options options) : options_(std::move(options))
    {
        static const bool winsockReady = [] {
            WSADATA data{};
            if (WSAStartup(MAKEWORD(2, 2), &data) != 0) throw std::runtime_error("WSAStartup failed");
            return true;
        }();
        (void)winsockReady;
        listener_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener_ == INVALID_SOCKET) throw std::runtime_error("ftp fixture socket failed");
        BOOL reuse = TRUE;
        setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        if (bind(listener_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0)
            throw std::runtime_error("ftp fixture bind failed");
        if (listen(listener_, 4) != 0) throw std::runtime_error("ftp fixture listen failed");
        int length = sizeof(address);
        if (getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &length) != 0)
            throw std::runtime_error("ftp fixture getsockname failed");
        port_ = ntohs(address.sin_port);
        thread_ = std::jthread([this] { serve(); });
    }

    ~FtpFixture() { stop(); }
    FtpFixture(const FtpFixture&) = delete;
    FtpFixture& operator=(const FtpFixture&) = delete;

    std::string url(const std::string& path = "/file.bin") const
    {
        return "ftp://127.0.0.1:" + std::to_string(port_) + path;
    }
    unsigned short port() const { return port_; }
    bool resumeUsed()
    {
        std::lock_guard lock(stateMutex_);
        return resumeUsed_;
    }
    int loginFailures()
    {
        std::lock_guard lock(stateMutex_);
        return loginFailures_;
    }
    std::string commandLog()
    {
        std::lock_guard lock(stateMutex_);
        std::string joined;
        for (const auto& command : commands_) {
            if (!joined.empty()) joined += " | ";
            joined += command;
        }
        return joined;
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
            handleControl(client);
            closesocket(client);
        }
    }

    bool sendText(SOCKET socket, const std::string& text)
    {
        std::size_t offset = 0;
        while (offset < text.size()) {
            const int sent = send(socket, text.data() + offset,
                static_cast<int>(text.size() - offset), 0);
            if (sent <= 0) return false;
            offset += static_cast<std::size_t>(sent);
        }
        return true;
    }

    std::string readLine(SOCKET socket, std::string& pending)
    {
        while (true) {
            const auto end = pending.find("\r\n");
            if (end != std::string::npos) {
                std::string line = pending.substr(0, end);
                pending.erase(0, end + 2);
                return line;
            }
            char buffer[1024];
            const int received = recv(socket, buffer, sizeof(buffer), 0);
            if (received <= 0) return {};
            pending.append(buffer, static_cast<std::size_t>(received));
        }
    }

    static SOCKET makeListener(unsigned short& port)
    {
        const SOCKET socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (socket == INVALID_SOCKET) return INVALID_SOCKET;
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        if (::bind(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0
            || ::listen(socket, 1) != 0) {
            closesocket(socket);
            return INVALID_SOCKET;
        }
        int length = sizeof(address);
        getsockname(socket, reinterpret_cast<sockaddr*>(&address), &length);
        port = ntohs(address.sin_port);
        return socket;
    }

    void handleControl(SOCKET control)
    {
        sendText(control, "220 LiteFetch test FTP\r\n");
        std::string pending;
        SOCKET dataListener = INVALID_SOCKET;
        std::size_t resumeFrom = 0;
        bool loggedIn = false;
        while (!stopping_.load()) {
            const std::string line = readLine(control, pending);
            if (line.empty()) break;
            const auto space = line.find(' ');
            const std::string command = space == std::string::npos ? line : line.substr(0, space);
            const std::string argument = space == std::string::npos ? std::string() : line.substr(space + 1);
            {
                std::lock_guard lock(stateMutex_);
                commands_.push_back(command + (argument.empty() ? "" : " " + argument));
            }
            if (command == "USER") {
                sendText(control, "331 Password required\r\n");
            } else if (command == "PASS") {
                if (argument == options_.password && !options_.username.empty()) {
                    loggedIn = true;
                    sendText(control, "230 Logged in\r\n");
                } else {
                    {
                        std::lock_guard lock(stateMutex_);
                        ++loginFailures_;
                    }
                    sendText(control, "530 Login incorrect\r\n");
                }
            } else if (command == "PWD") {
                sendText(control, "257 \"/\"\r\n");
            } else if (command == "TYPE") {
                sendText(control, "200 Type set\r\n");
            } else if (command == "SIZE") {
                if (!loggedIn || !options_.fileExists) sendText(control, "550 No such file\r\n");
                else sendText(control, "213 " + std::to_string(options_.body.size()) + "\r\n");
            } else if (command == "EPSV") {
                unsigned short dataPort = 0;
                if (dataListener != INVALID_SOCKET) closesocket(dataListener);
                dataListener = makeListener(dataPort);
                if (dataListener == INVALID_SOCKET) sendText(control, "425 Cannot open data connection\r\n");
                else sendText(control, "229 Entering Extended Passive Mode (|||" + std::to_string(dataPort) + "|)\r\n");
            } else if (command == "PASV") {
                unsigned short dataPort = 0;
                if (dataListener != INVALID_SOCKET) closesocket(dataListener);
                dataListener = makeListener(dataPort);
                if (dataListener == INVALID_SOCKET) sendText(control, "425 Cannot open data connection\r\n");
                else {
                    const auto high = static_cast<unsigned>(dataPort >> 8);
                    const auto low = static_cast<unsigned>(dataPort & 0xff);
                    sendText(control, "227 Entering Passive Mode (127,0,0,1," + std::to_string(high)
                        + "," + std::to_string(low) + ")\r\n");
                }
            } else if (command == "REST") {
                if (!options_.supportsRest) {
                    sendText(control, "504 REST not implemented\r\n");
                } else {
                    std::size_t offset = 0;
                    if (std::from_chars(argument.data(), argument.data() + argument.size(), offset).ec
                        == std::errc{}) {
                        resumeFrom = offset;
                        if (offset > 0) {
                            std::lock_guard lock(stateMutex_);
                            resumeUsed_ = true;
                        }
                        sendText(control, "350 Restarting at " + std::to_string(offset) + "\r\n");
                    } else {
                        sendText(control, "501 Bad offset\r\n");
                    }
                }
            } else if (command == "RETR") {
                if (!loggedIn || !options_.fileExists || dataListener == INVALID_SOCKET) {
                    sendText(control, "550 File unavailable\r\n");
                    continue;
                }
                sendText(control, "150 Opening binary mode data connection\r\n");
                const SOCKET data = accept(dataListener, nullptr, nullptr);
                closesocket(dataListener);
                dataListener = INVALID_SOCKET;
                if (data != INVALID_SOCKET) {
                    const std::size_t start = (std::min)(resumeFrom, options_.body.size());
                    for (std::size_t offset = start; offset < options_.body.size();) {
                        if (stopping_.load()) break;
                        if (options_.chunkDelay.count() > 0)
                            std::this_thread::sleep_for(options_.chunkDelay);
                        const auto size = (std::min)(options_.chunk, options_.body.size() - offset);
                        if (send(data, reinterpret_cast<const char*>(options_.body.data() + offset),
                                static_cast<int>(size), 0) <= 0) break;
                        offset += size;
                    }
                    closesocket(data);
                }
                resumeFrom = 0;
                sendText(control, "226 Transfer complete\r\n");
            } else if (command == "QUIT") {
                sendText(control, "221 Bye\r\n");
                break;
            } else {
                sendText(control, "502 Command not implemented\r\n");
            }
        }
        if (dataListener != INVALID_SOCKET) closesocket(dataListener);
    }

    Options options_;
    std::mutex stateMutex_;
    bool resumeUsed_ = false;
    int loginFailures_ = 0;
    std::vector<std::string> commands_;
    std::atomic<SOCKET> listener_{INVALID_SOCKET};
    std::atomic_bool stopping_{false};
    unsigned short port_ = 0;
    std::jthread thread_;
};

} // namespace litefetch::test
