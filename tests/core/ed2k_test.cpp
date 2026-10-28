// ed2k link parsing and the server source query, exercised against a scripted
// server on loopback. No real ed2k traffic leaves the machine.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include "ed2k_client.h"
#include "ed2k_transfer.h"
#include "ed2k_link.h"
#include "test_support.h"

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

using namespace litefetch;
using namespace litefetch::test;

namespace {

enum class ServerBehaviour { sources, zeroSources, rejectLogin, silent, closeAfterLogin };

// Minimal eDonkey server: accept one login, answer it, then answer one source
// query according to the scripted behaviour.
class FakeEd2kServer {
public:
    explicit FakeEd2kServer(ServerBehaviour behaviour, int sourceCount = 0)
        : behaviour_(behaviour), sourceCount_(sourceCount)
    {
        WSADATA data{};
        WSAStartup(MAKEWORD(2, 2), &data);
        listener_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        ::bind(listener_, reinterpret_cast<const sockaddr*>(&address), sizeof(address));
        ::listen(listener_, 4);
        int length = sizeof(address);
        ::getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &length);
        port_ = ntohs(address.sin_port);
        thread_ = std::jthread([this] { serve(); });
    }
    ~FakeEd2kServer()
    {
        if (listener_ != INVALID_SOCKET) closesocket(listener_);
    }
    std::uint16_t port() const { return port_; }

private:
    static void finish(SOCKET socket)
    {
        // A hard close right after the last byte can turn into an RST and drop
        // the answer before the client reads it.
        ::shutdown(socket, SD_SEND);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        closesocket(socket);
    }
    static bool sendAll(SOCKET socket, const void* data, int size)
    {
        return ::send(socket, static_cast<const char*>(data), size, 0) == size;
    }
    static bool receiveExact(SOCKET socket, void* data, int size)
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

    void serve()
    {
        const SOCKET client = ::accept(listener_, nullptr, nullptr);
        if (client == INVALID_SOCKET) return;
        std::vector<unsigned char> request(22);
        if (!receiveExact(client, request.data(), static_cast<int>(request.size()))) {
            closesocket(client);
            return;
        }
        if (behaviour_ == ServerBehaviour::rejectLogin) {
            const unsigned char reject[2] = {0xE3, 0x05};
            sendAll(client, reject, 2);
            finish(client);
            return;
        }
        const unsigned char idChange[10] = {0xE3, 0x40, 0x01, 0x00, 0x00, 0x7f,
            0x00, 0x00, 0x00, 0x00};
        if (!sendAll(client, idChange, sizeof(idChange))) {
            closesocket(client);
            return;
        }
        if (behaviour_ == ServerBehaviour::closeAfterLogin) {
            closesocket(client);
            return;
        }
        if (behaviour_ == ServerBehaviour::silent) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1500));
            closesocket(client);
            return;
        }
        // Source query: marker, request, hash (16), size (4).
        std::vector<unsigned char> query(22);
        if (!receiveExact(client, query.data(), static_cast<int>(query.size()))) {
            closesocket(client);
            return;
        }
        std::vector<unsigned char> answer{0xE3, 0x42, static_cast<unsigned char>(sourceCount_)};
        for (int index = 0; index < sourceCount_; ++index) {
            // 192.168.0.<index+1> : 4000 + index, packed the eDonkey way.
            const std::uint32_t ip = 0x0000a8c0u | (static_cast<std::uint32_t>(index + 1) << 24);
            const std::uint32_t port = 4000u + static_cast<std::uint32_t>(index);
            const std::uint32_t packed = (ip & 0xff000000u) | ((0x00c0a800u >> 8) << 8)
                | port;
            const std::uint32_t value = (index + 1) << 24 | 0x0000a8c0u | port;
            const std::uint32_t chosen = index == 0 ? packed : value;
            answer.push_back(static_cast<unsigned char>((chosen >> 24) & 0xff));
            answer.push_back(static_cast<unsigned char>((chosen >> 16) & 0xff));
            answer.push_back(static_cast<unsigned char>((chosen >> 8) & 0xff));
            answer.push_back(static_cast<unsigned char>(chosen & 0xff));
        }
        sendAll(client, answer.data(), static_cast<int>(answer.size()));
        finish(client);
    }

    ServerBehaviour behaviour_;
    int sourceCount_;
    SOCKET listener_ = INVALID_SOCKET;
    std::uint16_t port_ = 0;
    std::jthread thread_;
};

// A scripted eDonkey peer that answers hello, file request and part requests.
class FakeEd2kPeer {
public:
    explicit FakeEd2kPeer(std::vector<unsigned char> payload, bool refuseFile = false)
        : payload_(std::move(payload)), refuseFile_(refuseFile)
    {
        WSADATA data{};
        WSAStartup(MAKEWORD(2, 2), &data);
        listener_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        ::bind(listener_, reinterpret_cast<const sockaddr*>(&address), sizeof(address));
        ::listen(listener_, 4);
        int length = sizeof(address);
        ::getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &length);
        port_ = ntohs(address.sin_port);
        thread_ = std::jthread([this] { serve(); });
    }
    ~FakeEd2kPeer() { if (listener_ != INVALID_SOCKET) closesocket(listener_); }
    std::uint16_t port() const { return port_; }

private:
    static bool sendAll(SOCKET socket, const void* data, int size)
    {
        return ::send(socket, static_cast<const char*>(data), size, 0) == size;
    }
    static bool receiveExact(SOCKET socket, void* data, int size)
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

    void serve()
    {
        const SOCKET client = ::accept(listener_, nullptr, nullptr);
        if (client == INVALID_SOCKET) return;
        std::vector<unsigned char> hello(27);
        if (!receiveExact(client, hello.data(), 27)) { closesocket(client); return; }
        std::vector<unsigned char> answer{0x4C};
        answer.resize(1 + 32, 0);
        if (!sendAll(client, answer.data(), static_cast<int>(answer.size()))) { closesocket(client); return; }

        std::vector<unsigned char> request(17);
        if (!receiveExact(client, request.data(), 17)) { closesocket(client); return; }
        const unsigned char refused[1] = {0x59};
        if (refuseFile_) { sendAll(client, refused, 1); closesocket(client); return; }
        std::vector<unsigned char> description{0x61};
        description.resize(1 + 26, 0);
        if (!sendAll(client, description.data(), static_cast<int>(description.size()))) { closesocket(client); return; }

        std::vector<unsigned char> partsRequest(35);
        if (!receiveExact(client, partsRequest.data(), 35)) { closesocket(client); return; }

        // One SENDINGPART covering the whole file, in 10240-byte blocks.
        const auto size = static_cast<std::uint64_t>(payload_.size());
        std::vector<unsigned char> packet;
        packet.push_back(0x46);
        packet.resize(1 + 16, 0); // file hash, all zero in this scripted peer
        const std::uint64_t start = 0;
        const std::uint64_t end = size - 1;
        for (int shift = 0; shift < 64; shift += 8)
            packet.push_back(static_cast<unsigned char>((start >> shift) & 0xff));
        for (int shift = 0; shift < 64; shift += 8)
            packet.push_back(static_cast<unsigned char>((end >> shift) & 0xff));
        std::size_t offset = 0;
        while (offset < payload_.size()) {
            const auto chunk = (std::min)(static_cast<std::size_t>(10240), payload_.size() - offset);
            packet.push_back(static_cast<unsigned char>(chunk & 0xff));
            packet.push_back(static_cast<unsigned char>((chunk >> 8) & 0xff));
            packet.insert(packet.end(), payload_.begin() + static_cast<std::ptrdiff_t>(offset),
                payload_.begin() + static_cast<std::ptrdiff_t>(offset + chunk));
            offset += chunk;
        }
        sendAll(client, packet.data(), static_cast<int>(packet.size()));
        ::shutdown(client, SD_SEND);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        closesocket(client);
    }

    std::vector<unsigned char> payload_;
    bool refuseFile_;
    SOCKET listener_ = INVALID_SOCKET;
    std::uint16_t port_ = 0;
    std::jthread thread_;
};

void md4Vectors()
{
    // RFC 1320 test vectors.
    checkEqual(md4Hex("", 0), std::string("31d6cfe0d16ae931b73c59d7e0c089c0"), "MD4 empty");
    checkEqual(md4Hex("abc", 3), std::string("a448017aaf21d8525fc10ae87aa6729d"), "MD4 abc");
    checkEqual(md4Hex("message digest", 14),
        std::string("d9130a8164549fe818874806e1c7014b"), "MD4 message digest");
}

void peerTransfer(const std::filesystem::path& root)
{
    const auto payload = litefetch::test::payload(30000);
    {
        FakeEd2kPeer peer(payload);
        const auto partPath = root / "ed2k-part.bin";
        Ed2kPeerRequest request;
        request.host = "127.0.0.1";
        request.port = peer.port();
        request.fileHash = "0123456789abcdef0123456789abcdef";
        request.fileSize = static_cast<std::int64_t>(payload.size());
        request.partPath = partPath.string();
        request.timeoutMillis = 5000;
        const std::atomic_bool cancelled{false};
        const auto result = downloadFromEd2kPeer(request, cancelled);
        check(result.ok, ("peer transfer failed: " + result.error).c_str());
        checkEqual(result.received, static_cast<long long>(payload.size()), "all bytes arrived");
        check(readFile(partPath) == payload, "downloaded bytes match the peer payload");
        check(!ed2kFileHash(partPath.string()).empty(), "the ed2k root hash can be computed");
    }
    {
        FakeEd2kPeer peer(payload, true);
        const auto partPath = root / "ed2k-refused.bin";
        Ed2kPeerRequest request;
        request.host = "127.0.0.1";
        request.port = peer.port();
        request.fileHash = "0123456789abcdef0123456789abcdef";
        request.fileSize = static_cast<std::int64_t>(payload.size());
        request.partPath = partPath.string();
        request.timeoutMillis = 5000;
        const std::atomic_bool cancelled{false};
        const auto result = downloadFromEd2kPeer(request, cancelled);
        check(!result.ok, "a peer without the file is a failure");
        check(result.error.find("does not have") != std::string::npos,
            ("the refusal is not explained: " + result.error).c_str());
    }
}
void linkParsing()
{
    const auto good = parseEd2kLink("ed2k://|file|movie.mkv|734003200|0123456789abcdef0123456789abcdef|/");
    check(good.ok, ("valid link rejected: " + good.error).c_str());
    checkEqual(good.link.name, std::string("movie.mkv"), "file name");
    checkEqual(good.link.size, static_cast<long long>(734003200), "file size");
    checkEqual(good.link.hash, std::string("0123456789abcdef0123456789abcdef"), "hash");

    check(!parseEd2kLink("ed2k://|server|192.168.0.1|4661|/").ok, "server link is not a file");
    check(!parseEd2kLink("ed2k://|file|a.bin|0|0123456789abcdef0123456789abcdef|/").ok,
        "zero size is rejected");
    check(!parseEd2kLink("ed2k://|file|a.bin|10|zzz|/").ok, "bad hash is rejected");
    check(!parseEd2kLink("http://example.com/x").ok, "non-ed2k link is rejected");

    // 192.168.1.x with port 0x1100 (4352) travels as three address bytes and
    // the port high byte.
    const auto source = decodeEd2kSource(0xc0a80111u);
    checkEqual(source.address, std::string("192.168.1.0"), "source address is decoded");
    checkEqual(static_cast<long long>(source.port), 0x1100LL, "source port is decoded");
}

void sourceQuery()
{
    const auto link = parseEd2kLink(
        "ed2k://|file|payload.bin|1048576|0123456789abcdef0123456789abcdef|/");
    check(link.ok, "fixture link parses");

    {
        FakeEd2kServer server(ServerBehaviour::sources, 2);
        const auto query = queryEd2kSources({"127.0.0.1", server.port()}, link.link, 5000);
        check(query.ok, ("source query failed: " + query.error).c_str());
        checkEqual(static_cast<long long>(query.sources.size()), 2, "two sources reported");
        check(query.sources[0].port != 0, "source port is filled");
    }
    {
        // Zero sources is an answer, not an error.
        FakeEd2kServer server(ServerBehaviour::zeroSources);
        const auto query = queryEd2kSources({"127.0.0.1", server.port()}, link.link, 5000);
        check(query.ok, ("zero-source query failed: " + query.error).c_str());
        check(query.sources.empty(), "zero sources reported");
    }
    {
        FakeEd2kServer server(ServerBehaviour::rejectLogin);
        const auto query = queryEd2kSources({"127.0.0.1", server.port()}, link.link, 5000);
        check(!query.ok, "rejected login is a failure");
        check(query.error.find("rejected") != std::string::npos,
            ("rejection is not explained: " + query.error).c_str());
    }
    {
        FakeEd2kServer server(ServerBehaviour::closeAfterLogin);
        const auto query = queryEd2kSources({"127.0.0.1", server.port()}, link.link, 5000);
        check(!query.ok, "a closed connection is a failure");
    }
    {
        FakeEd2kServer server(ServerBehaviour::silent);
        const auto started = std::chrono::steady_clock::now();
        const auto query = queryEd2kSources({"127.0.0.1", server.port()}, link.link, 500);
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started).count();
        check(!query.ok, "a silent server is a failure");
        check(elapsed < 5000, "the timeout is honoured");
    }
    {
        const auto query = queryEd2kSources({"127.0.0.1", 1}, link.link, 1000);
        check(!query.ok, "an unreachable server is a failure");
        check(query.error.find("reach") != std::string::npos,
            ("unreachable is not explained: " + query.error).c_str());
    }
}

} // namespace

int main()
{
    return run("ed2k", [] {
        TempDir root;
        linkParsing();
        md4Vectors();
        peerTransfer(root.path());
        sourceQuery();
        return 0;
    });
}
