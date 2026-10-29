#include "kad_node.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include <chrono>
#include <cstdlib>
#include <fstream>

namespace litefetch {
namespace {
void ensureWinsock()
{
    static const bool ready = [] {
        WSADATA data{};
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    (void)ready;
}

std::int64_t nowUnix()
{
    return static_cast<std::int64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}
} // namespace

KadNode::KadNode(KadId id)
{
    table_.setSelf(id);
}

KadNode::~KadNode()
{
    stop();
}

bool KadNode::start(std::uint16_t port, std::string& error)
{
    ensureWinsock();
    const auto socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket == INVALID_SOCKET) {
        error = "cannot create a UDP socket";
        return false;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    if (::bind(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        closesocket(socket);
        error = "cannot bind the Kad UDP port";
        return false;
    }
    int length = sizeof(address);
    ::getsockname(socket, reinterpret_cast<sockaddr*>(&address), &length);
    port_ = ntohs(address.sin_port);
    socket_ = static_cast<std::uintptr_t>(socket);
    stopping_.store(false);
    receiver_ = std::thread([this] { receiveLoop(); });
    return true;
}

void KadNode::stop()
{
    stopping_.store(true);
    if (receiver_.joinable()) receiver_.join();
    if (socket_ != 0) {
        closesocket(static_cast<SOCKET>(socket_));
        socket_ = 0;
    }
}

bool KadNode::send(const KadPacket& packet, const std::string& address, std::uint16_t port)
{
    if (socket_ == 0) return false;
    sockaddr_in target{};
    target.sin_family = AF_INET;
    target.sin_port = htons(port);
    if (::inet_pton(AF_INET, address.c_str(), &target.sin_addr) != 1) return false;
    const auto bytes = encodeKadPacket(packet);
    const auto sent = ::sendto(static_cast<SOCKET>(socket_),
        reinterpret_cast<const char*>(bytes.data()), static_cast<int>(bytes.size()), 0,
        reinterpret_cast<const sockaddr*>(&target), sizeof(target));
    return sent == static_cast<int>(bytes.size());
}

void KadNode::receiveLoop()
{
    while (!stopping_.load()) pollOnce(100);
}

bool KadNode::takeResponse(KadOpcode opcode, KadPacket& packet, int timeoutMillis)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMillis);
    while (std::chrono::steady_clock::now() < deadline) {
        {
            std::lock_guard lock(responseMutex_);
            for (auto it = responses_.begin(); it != responses_.end(); ++it) {
                if (it->opcode != opcode) continue;
                packet = *it;
                responses_.erase(it);
                return true;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

bool KadNode::pollOnce(int timeoutMillis)
{
    if (socket_ == 0) return false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMillis);
    while (std::chrono::steady_clock::now() < deadline) {
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(static_cast<SOCKET>(socket_), &readable);
        timeval wait{0, 50 * 1000};
        const int ready = ::select(0, &readable, nullptr, nullptr, &wait);
        if (ready <= 0) continue;
        unsigned char buffer[2048]{};
        sockaddr_in from{};
        int fromLength = sizeof(from);
        const int received = ::recvfrom(static_cast<SOCKET>(socket_),
            reinterpret_cast<char*>(buffer), sizeof(buffer), 0,
            reinterpret_cast<sockaddr*>(&from), &fromLength);
        if (received <= 0) continue;
        KadPacket packet;
        if (!decodeKadPacket(buffer, static_cast<std::size_t>(received), packet)) continue;
        char text[32]{};
        ::inet_ntop(AF_INET, &from.sin_addr, text, sizeof(text));
        handle(packet, text, ntohs(from.sin_port));
        return true;
    }
    return false;
}

void KadNode::handle(const KadPacket& packet, const std::string& fromAddress,
    std::uint16_t fromPort)
{
    // Any packet tells us who is talking; the first contact in the list is the
    // sender.
    KadContact sender;
    if (!packet.contacts.empty()) {
        sender = packet.contacts.front();
        sender.address = fromAddress;
        sender.lastSeenUnix = nowUnix();
        table_.add(sender, sender.lastSeenUnix);
    }
    switch (packet.opcode) {
    case KadOpcode::BootstrapRequest: {
        // The caller already knows itself; answer with us first, then whatever
        // else we know.
        KadPacket answer;
        answer.opcode = KadOpcode::BootstrapResponse;
        KadContact self;
        self.id = table_.self();
        self.address = "127.0.0.1";
        self.port = port_;
        self.lastSeenUnix = nowUnix();
        answer.contacts.push_back(self);
        for (const auto& known : table_.closest(sender.id, 4)) {
            if (known.id == sender.id || known.id == self.id) continue;
            answer.contacts.push_back(known);
        }
        send(answer, fromAddress, fromPort);
        break;
    }
    case KadOpcode::HelloRequest: {
        KadPacket answer;
        answer.opcode = KadOpcode::HelloResponse;
        KadContact self;
        self.id = table_.self();
        self.address = "127.0.0.1";
        self.port = port_;
        self.lastSeenUnix = nowUnix();
        answer.contacts.push_back(self);
        send(answer, fromAddress, fromPort);
        break;
    }
    case KadOpcode::FindRequest: {
        KadPacket answer;
        answer.opcode = KadOpcode::FindResponse;
        KadContact self;
        self.id = table_.self();
        self.address = "127.0.0.1";
        self.port = port_;
        self.lastSeenUnix = nowUnix();
        answer.contacts.push_back(self);
        for (const auto& known : table_.closest(packet.target,
                 packet.wanted == 0 ? 4 : packet.wanted)) {
            if (known.id == self.id) continue;
            answer.contacts.push_back(known);
        }
        send(answer, fromAddress, fromPort);
        break;
    }
    case KadOpcode::BootstrapResponse:
    case KadOpcode::HelloResponse:
    case KadOpcode::FindResponse: {
        // The contacts were already merged above; hand the packet to whoever is
        // waiting for it.
        std::lock_guard lock(responseMutex_);
        responses_.push_back(packet);
        if (responses_.size() > 64) responses_.pop_front();
        break;
    }
    }
}

bool KadNode::bootstrap(const std::string& host, std::uint16_t port, int timeoutMillis,
    std::string& error)
{
    KadPacket request;
    request.opcode = KadOpcode::BootstrapRequest;
    KadContact self;
    self.id = table_.self();
    self.address = "127.0.0.1";
    self.port = port_;
    self.lastSeenUnix = nowUnix();
    request.contacts.push_back(self);
    if (!send(request, host, port)) {
        error = "cannot send the bootstrap request";
        return false;
    }
    KadPacket answer;
    if (!takeResponse(KadOpcode::BootstrapResponse, answer, timeoutMillis)) {
        error = "no node answered the bootstrap";
        return false;
    }
    return true;
}

int KadNode::refresh(int timeoutMillis)
{
    const auto contacts = table_.closest(table_.self(), 8);
    int answered = 0;
    for (const auto& contact : contacts) {
        KadPacket request;
        request.opcode = KadOpcode::HelloRequest;
        KadContact self;
        self.id = table_.self();
        self.address = "127.0.0.1";
        self.port = port_;
        self.lastSeenUnix = nowUnix();
        request.contacts.push_back(self);
        if (!send(request, contact.address, contact.port)) continue;
        KadPacket answer;
        const bool gotAnswer = takeResponse(KadOpcode::HelloResponse, answer, timeoutMillis);
        if (gotAnswer) {
            KadContact refreshed = contact;
            refreshed.lastSeenUnix = nowUnix();
            table_.add(refreshed, refreshed.lastSeenUnix);
            ++answered;
        } else {
            table_.remove(contact.id);
        }
    }
    return answered;
}

std::vector<KadContact> KadNode::findNode(const KadId& target, std::uint8_t wanted,
    int timeoutMillis)
{
    const auto known = table_.closest(target, 4);
    std::vector<KadContact> found;
    for (const auto& contact : known) {
        KadPacket request;
        request.opcode = KadOpcode::FindRequest;
        request.target = target;
        request.wanted = wanted;
        if (!send(request, contact.address, contact.port)) continue;
        KadPacket answer;
        if (!takeResponse(KadOpcode::FindResponse, answer, timeoutMillis)) continue;
        for (const auto& candidate : answer.contacts) found.push_back(candidate);
    }
    return found;
}

bool KadNode::saveContacts(const std::string& path) const
{
    std::ofstream output(path, std::ios::trunc);
    if (!output) return false;
    output << table_.serialize();
    return output.good();
}

bool KadNode::loadContacts(const std::string& path)
{
    std::ifstream input(path);
    if (!input) return false;
    std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    return table_.load(text);
}

} // namespace litefetch
