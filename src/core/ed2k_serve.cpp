#include "ed2k_serve.h"

#include "ed2k_transfer.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <thread>
#include <vector>

namespace litefetch {
namespace {
constexpr unsigned char protocolMarker = 0xE3;
constexpr unsigned char helloAnswer = 0x4C;
constexpr unsigned char fileRequestAnswer = 0x59;
constexpr unsigned char fileDescription = 0x61;
constexpr unsigned char requestParts = 0x47;
constexpr unsigned char sendingPart = 0x46;

void ensureWinsock()
{
    static const bool ready = [] {
        WSADATA data{};
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    (void)ready;
}

bool receiveExact(SOCKET socket, void* data, int size)
{
    auto* bytes = static_cast<char*>(data);
    int received = 0;
    while (received < size) {
        const int step = ::recv(socket, bytes + received, size - received, 0);
        if (step <= 0) return false;
        received += step;
    }
    return true;
}

bool sendAll(SOCKET socket, const std::vector<unsigned char>& data)
{
    int sent = 0;
    const auto size = static_cast<int>(data.size());
    while (sent < size) {
        const int step = ::send(socket, reinterpret_cast<const char*>(data.data()) + sent,
            size - sent, 0);
        if (step <= 0) return false;
        sent += step;
    }
    return true;
}

std::vector<unsigned char> hexToBytes(const std::string& hex)
{
    std::vector<unsigned char> bytes;
    for (std::size_t index = 0; index + 1 < hex.size(); index += 2)
        bytes.push_back(static_cast<unsigned char>(std::stoul(hex.substr(index, 2), nullptr, 16)));
    return bytes;
}

void appendUint64(std::vector<unsigned char>& out, std::uint64_t value)
{
    for (int shift = 0; shift < 64; shift += 8)
        out.push_back(static_cast<unsigned char>((value >> shift) & 0xff));
}
} // namespace

struct Ed2kPeerServer::Thread {
    std::thread worker;
};

Ed2kPeerServer::Ed2kPeerServer(std::string filePath, std::string fileHash, std::uint16_t port)
    : filePath_(std::move(filePath)), fileHash_(std::move(fileHash)), port_(port)
{
}

Ed2kPeerServer::~Ed2kPeerServer()
{
    stop();
    delete thread_;
}

bool Ed2kPeerServer::start(std::string& error)
{
    ensureWinsock();
    const auto listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) {
        error = "cannot create the listen socket";
        return false;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port_);
    if (::bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        closesocket(listener);
        error = "cannot bind the listen port";
        return false;
    }
    ::listen(listener, 8);
    int length = sizeof(address);
    ::getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length);
    port_ = ntohs(address.sin_port);
    listener_ = static_cast<std::uintptr_t>(listener);
    thread_ = new Thread{std::thread([this] { serve(); })};
    return true;
}

void Ed2kPeerServer::stop()
{
    stopping_.store(true);
    if (listener_ != 0) {
        closesocket(static_cast<SOCKET>(listener_));
        listener_ = 0;
    }
    if (thread_ && thread_->worker.joinable()) thread_->worker.join();
}

void Ed2kPeerServer::serve()
{
    while (!stopping_.load()) {
        const SOCKET client = ::accept(static_cast<SOCKET>(listener_), nullptr, nullptr);
        if (client == INVALID_SOCKET) return;
        handleClient(static_cast<std::uintptr_t>(client));
        closesocket(client);
    }
}

void Ed2kPeerServer::handleClient(std::uintptr_t clientValue)
{
    const auto client = static_cast<SOCKET>(clientValue);
    // The first packet is marker + HELLO: hash, id, port, tag count.
    unsigned char hello[2 + 16 + 4 + 2 + 4]{};
    if (!receiveExact(client, hello, sizeof(hello))) return;
    if (hello[0] != protocolMarker || hello[1] != 0x01) return;

    std::vector<unsigned char> answer{protocolMarker, helloAnswer};
    answer.resize(2 + 32, 0);
    if (!sendAll(client, answer)) return;

    unsigned char request[1 + 16]{};
    if (!receiveExact(client, request, sizeof(request))) return;
    if (request[0] != 0x58) return;
    const auto wanted = hexToBytes(fileHash_);
    if (wanted.size() != 16 || std::memcmp(wanted.data(), request + 1, 16) != 0) {
        std::vector<unsigned char> refused{fileRequestAnswer};
        refused.insert(refused.end(), request + 1, request + 17);
        sendAll(client, refused);
        return;
    }

    std::vector<unsigned char> description{fileDescription};
    description.resize(1 + 26, 0);
    std::memcpy(description.data() + 1, wanted.data(), 16);
    if (!sendAll(client, description)) return;

    unsigned char partsRequest[1 + 16 + 2 + 16]{};
    if (!receiveExact(client, partsRequest, sizeof(partsRequest))) return;
    if (partsRequest[0] != requestParts) return;
    std::uint64_t rangeStart = 0;
    std::uint64_t rangeEnd = 0;
    for (int index = 7; index >= 0; --index) rangeStart = (rangeStart << 8) | partsRequest[19 + index];
    for (int index = 7; index >= 0; --index) rangeEnd = (rangeEnd << 8) | partsRequest[27 + index];

    std::ifstream file(filePath_, std::ios::binary);
    if (!file) return;
    file.seekg(0, std::ios::end);
    const auto fileSize = static_cast<std::uint64_t>(file.tellg());
    if (rangeEnd >= fileSize) rangeEnd = fileSize - 1;

    std::uint64_t partStart = rangeStart;
    while (partStart <= rangeEnd && !stopping_.load()) {
        const std::uint64_t partEnd = (std::min)(
            partStart + static_cast<std::uint64_t>(ed2kPartSize) - 1, rangeEnd);
        std::vector<unsigned char> packet{sendingPart};
        packet.resize(1 + 16, 0);
        appendUint64(packet, partStart);
        appendUint64(packet, partEnd);
        std::uint64_t offset = partStart;
        std::vector<char> block(10240);
        while (offset <= partEnd) {
            const auto chunk = (std::min)(static_cast<std::uint64_t>(block.size()),
                partEnd - offset + 1);
            file.seekg(static_cast<std::streamoff>(offset));
            file.read(block.data(), static_cast<std::streamsize>(chunk));
            if (static_cast<std::uint64_t>(file.gcount()) != chunk) return;
            packet.push_back(static_cast<unsigned char>(chunk & 0xff));
            packet.push_back(static_cast<unsigned char>((chunk >> 8) & 0xff));
            packet.insert(packet.end(),
                reinterpret_cast<const unsigned char*>(block.data()),
                reinterpret_cast<const unsigned char*>(block.data()) + chunk);
            offset += chunk;
        }
        if (!sendAll(client, packet)) return;
        partStart = partEnd + 1;
    }
}

} // namespace litefetch
