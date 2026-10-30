#include "kad_wire.h"

#include <cstring>

namespace litefetch {
namespace {
constexpr std::uint8_t marker = 0xE4;
constexpr std::size_t contactSize = 22; // 16 id + 4 address + 2 port

bool opcodeFromByte(std::uint8_t value, KadOpcode& opcode)
{
    switch (value) {
    case 0x01: opcode = KadOpcode::BootstrapRequest; return true;
    case 0x09: opcode = KadOpcode::BootstrapResponse; return true;
    case 0x11: opcode = KadOpcode::HelloRequest; return true;
    case 0x19: opcode = KadOpcode::HelloResponse; return true;
    case 0x21: opcode = KadOpcode::FindRequest; return true;
    case 0x29: opcode = KadOpcode::FindResponse; return true;
    case 0x31: opcode = KadOpcode::PublishRequest; return true;
    case 0x39: opcode = KadOpcode::PublishResponse; return true;
    case 0x33: opcode = KadOpcode::SourcesRequest; return true;
    case 0x3B: opcode = KadOpcode::SourcesResponse; return true;
    default: return false;
    }
}

void appendContact(std::vector<std::uint8_t>& out, const KadContact& contact)
{
    out.insert(out.end(), contact.id.bytes.begin(), contact.id.bytes.end());
    // Addresses are numeric IPv4 in this subset; a hostname never reaches a
    // datagram.
    // A small parser rather than sscanf: the CRT flags sscanf as unsafe, and this
    // project treats warnings as errors so a deprecation cannot be ignored.
    const auto* text = contact.address.c_str();
    auto octet = [&text](unsigned& value) {
        unsigned parsed = 0;
        int digits = 0;
        while (*text >= '0' && *text <= '9' && digits < 3) {
            parsed = parsed * 10 + static_cast<unsigned>(*text - '0');
            ++text;
            ++digits;
        }
        if (digits == 0 || parsed > 255) return false;
        value = parsed;
        return true;
    };
    unsigned a = 0, b = 0, c = 0, d = 0;
    const bool parsed = octet(a) && *text++ == '.' && octet(b) && *text++ == '.'
        && octet(c) && *text++ == '.' && octet(d) && *text == '\0';
    if (!parsed) a = b = c = d = 0;
    out.push_back(static_cast<std::uint8_t>(a & 0xff));
    out.push_back(static_cast<std::uint8_t>(b & 0xff));
    out.push_back(static_cast<std::uint8_t>(c & 0xff));
    out.push_back(static_cast<std::uint8_t>(d & 0xff));
    out.push_back(static_cast<std::uint8_t>((contact.port >> 8) & 0xff));
    out.push_back(static_cast<std::uint8_t>(contact.port & 0xff));
}

bool readContact(const std::uint8_t* data, KadContact& contact)
{
    std::memcpy(contact.id.bytes.data(), data, 16);
    char text[32]{};
    std::snprintf(text, sizeof(text), "%u.%u.%u.%u", data[16], data[17], data[18], data[19]);
    contact.address = text;
    contact.port = static_cast<std::uint16_t>((data[20] << 8) | data[21]);
    return true;
}
} // namespace

std::vector<std::uint8_t> encodeKadPacket(const KadPacket& packet)
{
    std::vector<std::uint8_t> out;
    out.push_back(marker);
    out.push_back(static_cast<std::uint8_t>(packet.opcode));
    const auto count = static_cast<std::uint8_t>((std::min)(packet.contacts.size(),
        static_cast<std::size_t>(255)));
    out.push_back(count);
    for (std::size_t index = 0; index < count; ++index)
        appendContact(out, packet.contacts[index]);
    if (packet.opcode == KadOpcode::FindRequest) {
        out.insert(out.end(), packet.target.bytes.begin(), packet.target.bytes.end());
        out.push_back(packet.wanted);
    }
    if (packet.hasFileHash
        && (packet.opcode == KadOpcode::PublishRequest
            || packet.opcode == KadOpcode::PublishResponse
            || packet.opcode == KadOpcode::SourcesRequest
            || packet.opcode == KadOpcode::SourcesResponse)) {
        out.insert(out.end(), packet.fileHash.bytes.begin(), packet.fileHash.bytes.end());
    }
    return out;
}

bool decodeKadPacket(const std::uint8_t* data, std::size_t size, KadPacket& packet)
{
    if (!data || size < 3 || data[0] != marker) return false;
    if (!opcodeFromByte(data[1], packet.opcode)) return false;
    const auto count = data[2];
    std::size_t offset = 3;
    if (size < offset + count * contactSize) return false;
    packet.contacts.clear();
    packet.contacts.reserve(count);
    for (std::uint8_t index = 0; index < count; ++index) {
        KadContact contact;
        readContact(data + offset, contact);
        if (contact.port == 0) return false;
        packet.contacts.push_back(std::move(contact));
        offset += contactSize;
    }
    if (packet.opcode == KadOpcode::FindRequest) {
        if (size < offset + 17) return false;
        std::memcpy(packet.target.bytes.data(), data + offset, 16);
        packet.wanted = data[offset + 16];
        offset += 17;
    }
    if (packet.opcode == KadOpcode::PublishRequest || packet.opcode == KadOpcode::PublishResponse
        || packet.opcode == KadOpcode::SourcesRequest
        || packet.opcode == KadOpcode::SourcesResponse) {
        if (size < offset + 16) return false;
        std::memcpy(packet.fileHash.bytes.data(), data + offset, 16);
        packet.hasFileHash = true;
    }
    return true;
}

std::string describeKadOpcode(KadOpcode opcode)
{
    switch (opcode) {
    case KadOpcode::BootstrapRequest: return "bootstrap request";
    case KadOpcode::BootstrapResponse: return "bootstrap response";
    case KadOpcode::HelloRequest: return "hello request";
    case KadOpcode::HelloResponse: return "hello response";
    case KadOpcode::FindRequest: return "find request";
    case KadOpcode::FindResponse: return "find response";
    case KadOpcode::PublishRequest: return "publish request";
    case KadOpcode::PublishResponse: return "publish response";
    case KadOpcode::SourcesRequest: return "sources request";
    case KadOpcode::SourcesResponse: return "sources response";
    }
    return "unknown";
}

} // namespace litefetch
