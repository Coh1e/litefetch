#include "ed2k_transfer.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#include <cstring>
#include <filesystem>
#include <map>
#include <utility>
#include <fstream>
#include <vector>

namespace litefetch {
namespace {
constexpr unsigned char helloRequest = 0x01;
constexpr unsigned char helloAnswer = 0x4C;
constexpr unsigned char fileRequest = 0x58;
constexpr unsigned char fileRequestAnswer = 0x59;
constexpr unsigned char fileDescription = 0x61;
constexpr unsigned char requestParts = 0x47;
constexpr unsigned char sendingPart = 0x46;
constexpr unsigned char queueRanking = 0x93;
constexpr unsigned char queueFull = 0x94;
constexpr unsigned char outOfParts = 0x95;

std::vector<unsigned char> hexToBytes(const std::string& hex)
{
    std::vector<unsigned char> bytes;
    for (std::size_t index = 0; index + 1 < hex.size(); index += 2)
        bytes.push_back(static_cast<unsigned char>(std::stoul(hex.substr(index, 2), nullptr, 16)));
    return bytes;
}

void appendUint32(std::vector<unsigned char>& out, std::uint32_t value)
{
    for (int shift = 0; shift < 32; shift += 8)
        out.push_back(static_cast<unsigned char>((value >> shift) & 0xff));
}

void appendUint64(std::vector<unsigned char>& out, std::uint64_t value)
{
    for (int shift = 0; shift < 64; shift += 8)
        out.push_back(static_cast<unsigned char>((value >> shift) & 0xff));
}

std::uint32_t readUint32(const unsigned char* bytes)
{
    return static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8)
        | (static_cast<std::uint32_t>(bytes[2]) << 16)
        | (static_cast<std::uint32_t>(bytes[3]) << 24);
}

std::uint64_t readUint64(const unsigned char* bytes)
{
    std::uint64_t value = 0;
    for (int index = 7; index >= 0; --index) value = (value << 8) | bytes[index];
    return value;
}

class PeerSocket {
public:
    PeerSocket() = default;
    ~PeerSocket() { if (socket_ != INVALID_SOCKET) closesocket(socket_); }
    PeerSocket(const PeerSocket&) = delete;
    PeerSocket& operator=(const PeerSocket&) = delete;

    bool open(int timeoutMillis)
    {
        socket_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (socket_ == INVALID_SOCKET) return false;
        const DWORD timeout = static_cast<DWORD>(timeoutMillis);
        setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout),
            sizeof(timeout));
        setsockopt(socket_, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout),
            sizeof(timeout));
        return true;
    }
    bool connect(const std::string& host, std::uint16_t port)
    {
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        if (::inet_pton(AF_INET, host.c_str(), &address.sin_addr) != 1) return false;
        return ::connect(socket_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0;
    }
    bool sendAll(const std::vector<unsigned char>& data)
    {
        int sent = 0;
        const auto size = static_cast<int>(data.size());
        while (sent < size) {
            const int step = ::send(socket_, reinterpret_cast<const char*>(data.data()) + sent,
                size - sent, 0);
            if (step <= 0) return false;
            sent += step;
        }
        return true;
    }
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

bool md4Raw(const unsigned char* data, std::size_t size, unsigned char digest[16])
{
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_MD4_ALGORITHM, nullptr, 0) < 0) return false;
    BCRYPT_HASH_HANDLE hash = nullptr;
    bool ok = false;
    if (BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) >= 0) {
        if (BCryptHashData(hash, const_cast<PUCHAR>(data), static_cast<ULONG>(size), 0) >= 0)
            ok = BCryptFinishHash(hash, digest, 16, 0) >= 0;
        BCryptDestroyHash(hash);
    }
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return ok;
}

std::string hexOf(const unsigned char digest[16])
{
    static constexpr char hex[] = "0123456789abcdef";
    std::string text;
    for (int index = 0; index < 16; ++index) {
        text.push_back(hex[digest[index] >> 4]);
        text.push_back(hex[digest[index] & 0x0f]);
    }
    return text;
}

std::string md4HexBuffer(const unsigned char* data, std::size_t size)
{
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_MD4_ALGORITHM, nullptr, 0) < 0) return {};
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::string result;
    if (BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) >= 0) {
        if (BCryptHashData(hash, const_cast<PUCHAR>(data), static_cast<ULONG>(size), 0) >= 0) {
            unsigned char digest[16]{};
            if (BCryptFinishHash(hash, digest, 16, 0) >= 0) {
                static constexpr char hex[] = "0123456789abcdef";
                for (const unsigned char byte : digest) {
                    result.push_back(hex[byte >> 4]);
                    result.push_back(hex[byte & 0x0f]);
                }
            }
        }
        BCryptDestroyHash(hash);
    }
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return result;
}
} // namespace

std::string md4Hex(const void* data, std::size_t size)
{
    return md4HexBuffer(static_cast<const unsigned char*>(data), size);
}

std::string ed2kFileHash(const std::string& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    // The root hash is the MD4 of the concatenated binary part digests.
    std::vector<unsigned char> combined;
    std::vector<char> buffer(static_cast<std::size_t>(ed2kPartSize));
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto read = static_cast<std::size_t>(input.gcount());
        if (read == 0) break;
        unsigned char digest[16]{};
        if (!md4Raw(reinterpret_cast<const unsigned char*>(buffer.data()), read, digest)) return {};
        combined.insert(combined.end(), digest, digest + 16);
    }
    unsigned char root[16]{};
    if (!md4Raw(combined.data(), combined.size(), root)) return {};
    return hexOf(root);
}

namespace {
std::string sidecarPath(const std::string& partPath)
{
    return partPath + ".ed2kmeta";
}

std::map<int, std::string> loadPartRecords(const std::string& partPath)
{
    std::map<int, std::string> records;
    std::ifstream input(sidecarPath(partPath));
    std::string line;
    while (std::getline(input, line)) {
        const auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        const int index = std::atoi(line.substr(0, colon).c_str());
        const auto hash = line.substr(colon + 1);
        if (index >= 0 && hash.size() == 32) records[index] = hash;
    }
    return records;
}

void savePartRecord(const std::string& partPath, int index, const std::string& hash)
{
    auto records = loadPartRecords(partPath);
    records[index] = hash;
    std::ofstream output(sidecarPath(partPath), std::ios::trunc);
    for (const auto& [key, value] : records) output << key << ':' << value << '\n';
}

int partCountFor(std::int64_t size)
{
    if (size <= 0) return 0;
    return static_cast<int>((size + ed2kPartSize - 1) / ed2kPartSize);
}

std::pair<std::int64_t, std::int64_t> partRange(int index, std::int64_t size)
{
    const std::int64_t start = static_cast<std::int64_t>(index) * ed2kPartSize;
    const std::int64_t end = (std::min)(start + ed2kPartSize, size) - 1;
    return {start, end};
}

std::string md4HexOfRange(const std::string& path, std::int64_t start, std::int64_t end)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    input.seekg(start);
    std::vector<char> buffer(static_cast<std::size_t>(end - start + 1));
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    if (static_cast<std::size_t>(input.gcount()) != buffer.size()) return {};
    return md4Hex(buffer.data(), buffer.size());
}

// Reads the bytes of one part and compares them with the recorded digest.
bool verifyPart(const std::string& partPath, int index, std::int64_t size,
    const std::string& expected)
{
    const auto [start, end] = partRange(index, size);
    std::ifstream input(partPath, std::ios::binary);
    if (!input) return false;
    input.seekg(start);
    std::vector<char> buffer(static_cast<std::size_t>(end - start + 1));
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    if (static_cast<std::size_t>(input.gcount()) != buffer.size()) return false;
    return md4Hex(buffer.data(), buffer.size()) == expected;
}
} // namespace

std::pair<std::int64_t, std::int64_t> ed2kPartRange(int index, std::int64_t fileSize)
{
    return partRange(index, fileSize);
}

Ed2kResumeState inspectEd2kParts(const std::string& partPath, std::int64_t fileSize,
    const std::string& fileHash)
{
    (void)fileHash;
    Ed2kResumeState state;
    const auto recorded = loadPartRecords(partPath);
    const int parts = partCountFor(fileSize);
    for (int index = 0; index < parts; ++index) {
        const auto found = recorded.find(index);
        if (found != recorded.end() && verifyPart(partPath, index, fileSize, found->second))
            ++state.verifiedParts;
        else {
            ++state.missingParts;
            const auto [start, end] = partRange(index, fileSize);
            state.missingBytes += end - start + 1;
        }
    }
    return state;
}
Ed2kPeerResult downloadFromEd2kPeer(const Ed2kPeerRequest& request,
    const std::atomic_bool& cancelled)
{
    Ed2kPeerResult result;
    const auto hash = hexToBytes(request.fileHash);
    if (hash.size() != 16) {
        result.error = "the file hash is not a valid ed2k hash";
        return result;
    }

    PeerSocket peer;
    if (!peer.open(request.timeoutMillis)) {
        result.error = "cannot create a socket";
        return result;
    }
    if (!peer.connect(request.host, request.port)) {
        result.error = "cannot reach the peer";
        return result;
    }

    // Hello: hash, client id, port, no tags.
    std::vector<unsigned char> hello{helloRequest};
    hello.insert(hello.end(), hash.begin(), hash.end());
    appendUint32(hello, 0);
    hello.push_back(0);
    hello.push_back(0);
    appendUint32(hello, 0);
    if (!peer.sendAll(hello)) {
        result.error = "the peer closed the connection during hello";
        return result;
    }

    unsigned char answer = 0;
    if (!peer.receiveExact(&answer, 1)) {
        result.error = "the peer did not answer the hello";
        return result;
    }
    if (answer != helloAnswer) {
        result.error = "unexpected answer to the hello";
        return result;
    }
    // hash(16) + client id(4) + port(2) + tag count(4) + server ip(4) + port(2)
    unsigned char helloBody[32]{};
    if (!peer.receiveExact(helloBody, 32)) {
        result.error = "the hello answer was cut short";
        return result;
    }
    const auto tagCount = readUint32(helloBody + 22);
    if (tagCount > 256) {
        result.error = "the peer sent an implausible tag count";
        return result;
    }
    std::vector<unsigned char> tags(static_cast<std::size_t>(tagCount) * 9);
    if (!tags.empty() && !peer.receiveExact(tags.data(), static_cast<int>(tags.size()))) {
        result.error = "the hello tags were cut short";
        return result;
    }

    // Ask for the file itself.
    std::vector<unsigned char> fileRequestBytes{fileRequest};
    fileRequestBytes.insert(fileRequestBytes.end(), hash.begin(), hash.end());
    if (!peer.sendAll(fileRequestBytes)) {
        result.error = "the peer closed the connection during the file request";
        return result;
    }
    unsigned char fileAnswer = 0;
    if (!peer.receiveExact(&fileAnswer, 1)) {
        result.error = "the peer did not answer the file request";
        return result;
    }
    if (fileAnswer == fileRequestAnswer) {
        result.error = "the peer does not have this file or its queue is full";
        return result;
    }
    if (fileAnswer != fileDescription) {
        result.error = "unexpected answer to the file request";
        return result;
    }
    // hash(16) + client id(4) + port(2) + tag count(4) + tags
    unsigned char description[26]{};
    if (!peer.receiveExact(description, 26)) {
        result.error = "the file description was cut short";
        return result;
    }
    const auto descriptionTags = readUint32(description + 22);
    if (descriptionTags > 256) {
        result.error = "the peer sent an implausible tag count";
        return result;
    }
    std::vector<unsigned char> descriptionTagBytes(static_cast<std::size_t>(descriptionTags) * 9);
    if (!descriptionTagBytes.empty()
        && !peer.receiveExact(descriptionTagBytes.data(), static_cast<int>(descriptionTagBytes.size()))) {
        result.error = "the file description tags were cut short";
        return result;
    }

    // Which parts are already here and still match their recorded digest? Only
    // the missing span is requested, so a restart continues instead of starting
    // over.
    const int totalParts = partCountFor(request.fileSize);
    const auto recordedParts = loadPartRecords(request.partPath);
    int firstMissing = -1;
    int lastMissing = -1;
    for (int index = 0; index < totalParts; ++index) {
        const auto found = recordedParts.find(index);
        const bool have = found != recordedParts.end()
            && verifyPart(request.partPath, index, request.fileSize, found->second);
        if (!have) {
            if (firstMissing < 0) firstMissing = index;
            lastMissing = index;
        }
    }
    if (firstMissing < 0) {
        // Everything is on disk: just confirm the root hash.
        if (!request.fileHash.empty() && ed2kFileHash(request.partPath) != request.fileHash) {
            result.error = "the data on disk does not match the ed2k hash";
            return result;
        }
        result.ok = true;
        result.received = request.fileSize;
        return result;
    }
    const auto missingStart = partRange(firstMissing, request.fileSize).first;
    const auto missingEnd = partRange(lastMissing, request.fileSize).second;
    const auto missingBytes = missingEnd - missingStart + 1;
    const auto alreadyHere = request.fileSize - missingBytes;

    // Ask for the missing range; the peer answers with part blocks.
    std::vector<unsigned char> parts{requestParts};
    parts.insert(parts.end(), hash.begin(), hash.end());
    const unsigned char rangeCount[2] = {1, 0};
    parts.insert(parts.end(), rangeCount, rangeCount + 2);
    appendUint64(parts, static_cast<std::uint64_t>(missingStart));
    appendUint64(parts, static_cast<std::uint64_t>(missingEnd));
    if (!peer.sendAll(parts)) {
        result.error = "the peer closed the connection during the part request";
        return result;
    }

    // Resume must not truncate: the verified parts are already in this file.
    if (!std::filesystem::exists(request.partPath)) {
        std::ofstream create(request.partPath, std::ios::binary | std::ios::trunc);
        if (!create) {
            result.error = "cannot create the part file";
            return result;
        }
        create.seekp(static_cast<std::streamoff>(request.fileSize) - 1);
        create.put('\0');
    }
    std::fstream output(request.partPath, std::ios::binary | std::ios::in | std::ios::out);
    if (!output) {
        result.error = "cannot open the part file for writing";
        return result;
    }

    std::int64_t written = 0;
    while (written < missingBytes) {
        if (cancelled.load()) {
            result.error = "cancelled";
            return result;
        }
        unsigned char kind = 0;
        if (!peer.receiveExact(&kind, 1)) {
            result.error = "the peer closed the connection during the transfer";
            return result;
        }
        if (kind == queueRanking || kind == queueFull || kind == outOfParts) {
            result.error = kind == outOfParts ? "the peer has no more parts for us"
                                              : "the peer put this file in its queue";
            return result;
        }
        if (kind != sendingPart) {
            result.error = "unexpected packet during the transfer";
            return result;
        }
        // hash(16) + start(8) + end(8), then 2-byte length prefixed blocks.
        unsigned char header[32]{};
        if (!peer.receiveExact(header, 32)) {
            result.error = "a part header was cut short";
            return result;
        }
        const auto start = static_cast<std::int64_t>(readUint64(header + 16));
        const auto end = static_cast<std::int64_t>(readUint64(header + 24));
        // The end offset is inclusive.
        if (end < start || end >= request.fileSize) {
            result.error = "the peer sent an invalid part range";
            return result;
        }
        output.seekp(static_cast<std::streamoff>(start));
        auto offset = start;
        while (offset <= end) {
            unsigned char lengthBytes[2]{};
            if (!peer.receiveExact(lengthBytes, 2)) {
                result.error = "a part block length was cut short";
                return result;
            }
            const auto length = static_cast<int>(lengthBytes[0] | (lengthBytes[1] << 8));
            if (length <= 0 || offset + length > end + 1) {
                result.error = "the peer sent an invalid part block";
                return result;
            }
            std::vector<char> block(static_cast<std::size_t>(length));
            if (!peer.receiveExact(block.data(), length)) {
                result.error = "a part block was cut short";
                return result;
            }
            output.write(block.data(), length);
            offset += length;
        }
        written += end - start + 1;
        result.received = alreadyHere + written;
        // Record every part the packet completed, so a later run can trust it.
        // The buffered writes must reach the disk before the digest is read.
        output.flush();
        for (int index = firstMissing; index <= lastMissing; ++index) {
            const auto [partStart, partEnd] = partRange(index, request.fileSize);
            if (partStart < start || partEnd > end) continue;
            const auto digest = md4HexOfRange(request.partPath, partStart, partEnd);
            if (!digest.empty()) savePartRecord(request.partPath, index, digest);
        }
        if (cancelled.load()) {
            result.error = "cancelled";
            return result;
        }
    }
    output.flush();
    if (!output.good()) {
        result.error = "cannot write the part file";
        return result;
    }
    if (!request.fileHash.empty()) {
        const auto actual = ed2kFileHash(request.partPath);
        if (actual != request.fileHash) {
            result.error = "the finished file does not match the ed2k hash";
            return result;
        }
    }
    result.ok = true;
    return result;
}

} // namespace litefetch
