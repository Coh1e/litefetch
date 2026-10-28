#include "ed2k_client.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include <chrono>
#include <cstring>
#include <vector>

namespace litefetch {
namespace {
constexpr unsigned char protocolMarker = 0xE3;
constexpr unsigned char loginRequest = 0x01;
constexpr unsigned char idChange = 0x40;
constexpr unsigned char getSources = 0x19;
constexpr unsigned char foundSources = 0x42;
constexpr unsigned char serverMessage = 0x38;
constexpr unsigned char serverReject = 0x05;

class Socket {
public:
    Socket() = default;
    ~Socket() { if (socket_ != INVALID_SOCKET) closesocket(socket_); }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    bool open()
    {
        socket_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        return socket_ != INVALID_SOCKET;
    }
    bool connect(const std::string& host, std::uint16_t port, int timeoutMillis)
    {
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        if (::inet_pton(AF_INET, host.c_str(), &address.sin_addr) != 1) return false;
        const DWORD timeout = static_cast<DWORD>(timeoutMillis);
        setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout),
            sizeof(timeout));
        setsockopt(socket_, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout),
            sizeof(timeout));
        return ::connect(socket_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0;
    }
    bool sendAll(const void* data, int size)
    {
        const auto* bytes = static_cast<const char*>(data);
        int sent = 0;
        while (sent < size) {
            const int step = ::send(socket_, bytes + sent, size - sent, 0);
            if (step <= 0) return false;
            sent += step;
        }
        return true;
    }
    // Returns false on timeout or disconnect; the number of bytes read is then
    // meaningless, which the caller treats as a protocol failure.
    bool receiveExact(void* data, int size)
    {
        auto* bytes = static_cast<char*>(data);
        int received = 0;
        while (received < size) {
            const int step = ::recv(socket_, bytes + received, size - received, 0);
            if (step <= 0) return false;
            received += step;
        }
        return true;
    }

private:
    SOCKET socket_ = INVALID_SOCKET;
};

std::vector<unsigned char> hexToBytes(const std::string& hex)
{
    std::vector<unsigned char> bytes;
    for (std::size_t index = 0; index + 1 < hex.size(); index += 2) {
        const auto pair = hex.substr(index, 2);
        bytes.push_back(static_cast<unsigned char>(std::stoul(pair, nullptr, 16)));
    }
    return bytes;
}
} // namespace

Ed2kSourceQuery queryEd2kSources(const Ed2kServerEndpoint& server, const Ed2kFileLink& file,
    int timeoutMillis)
{
    Ed2kSourceQuery query;
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(timeoutMillis);

    Socket socket;
    if (!socket.open()) {
        query.error = "cannot create a socket";
        return query;
    }
    if (!socket.connect(server.host, server.port, timeoutMillis)) {
        query.error = "cannot reach the ed2k server";
        return query;
    }

    // Greeting + login: marker, request, client hash (16), client id (4),
    // listening port (2), no tags.
    std::vector<unsigned char> login;
    login.push_back(protocolMarker);
    login.push_back(loginRequest);
    for (int index = 0; index < 16; ++index) login.push_back(static_cast<unsigned char>(index * 7 + 1));
    for (int index = 0; index < 4; ++index) login.push_back(0);
    login.push_back(0);
    login.push_back(0);
    if (!socket.sendAll(login.data(), static_cast<int>(login.size()))) {
        query.error = "the server closed the connection during login";
        return query;
    }

    unsigned char header[2]{};
    if (!socket.receiveExact(header, 2)) {
        query.error = "no answer from the ed2k server";
        return query;
    }
    if (header[1] == serverReject) {
        query.error = "the ed2k server rejected the login";
        return query;
    }
    if (header[1] != idChange) {
        query.error = "unexpected answer from the ed2k server";
        return query;
    }
    unsigned char idBytes[8]{};
    if (!socket.receiveExact(idBytes, 8)) {
        query.error = "the login answer was cut short";
        return query;
    }
    std::memcpy(&query.assignedId, idBytes, 4);

    // Ask for the sources of this file.
    std::vector<unsigned char> request;
    request.push_back(protocolMarker);
    request.push_back(getSources);
    auto hashBytes = hexToBytes(file.hash);
    if (hashBytes.size() != 16) {
        query.error = "the file hash is not a valid ed2k hash";
        return query;
    }
    request.insert(request.end(), hashBytes.begin(), hashBytes.end());
    for (int shift = 24; shift >= 0; shift -= 8)
        request.push_back(static_cast<unsigned char>((file.size >> shift) & 0xff));
    if (!socket.sendAll(request.data(), static_cast<int>(request.size()))) {
        query.error = "the server closed the connection during the source query";
        return query;
    }

    while (std::chrono::steady_clock::now() < deadline) {
        unsigned char reply[2]{};
        if (!socket.receiveExact(reply, 2)) {
            query.error = "the server did not answer the source query";
            return query;
        }
        if (reply[1] == serverMessage) {
            // A human readable server message: length prefix, then text.
            unsigned char length[2]{};
            if (!socket.receiveExact(length, 2)) {
                query.error = "a server message was cut short";
                return query;
            }
            const auto size = static_cast<int>(length[0] | (length[1] << 8));
            std::vector<char> text(static_cast<std::size_t>(size));
            if (size > 0 && !socket.receiveExact(text.data(), size)) {
                query.error = "a server message was cut short";
                return query;
            }
            continue;
        }
        if (reply[1] != foundSources) {
            query.error = "unexpected answer to the source query";
            return query;
        }
        unsigned char count = 0;
        if (!socket.receiveExact(&count, 1)) {
            query.error = "the source answer was cut short";
            return query;
        }
        std::vector<unsigned char> ids(static_cast<std::size_t>(count) * 4);
        if (!ids.empty() && !socket.receiveExact(ids.data(), static_cast<int>(ids.size()))) {
            query.error = "the source answer was cut short";
            return query;
        }
        for (int index = 0; index < count; ++index) {
            std::uint32_t id = 0;
            std::memcpy(&id, ids.data() + index * 4, 4);
            query.sources.push_back(decodeEd2kSource(id));
        }
        query.ok = true;
        return query;
    }
    query.error = "the source query timed out";
    return query;
}

} // namespace litefetch
