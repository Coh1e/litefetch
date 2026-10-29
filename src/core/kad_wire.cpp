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
    default: return false;
    }
}

void appendContact(std::vector<std::uint8_t>& out, const KadContact& contact)
{
    out.insert(out.end(), contact.id.bytes.begin(), contact.id.bytes.end());
    // Addresses are numeric IPv4 in this subset; a hostname never reaches a
    // datagram.
    unsigned a = 0, b = 0, c = 0, d = 0;
    if (std::sscanf(contact.address.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) != 4) {
        a = b = c = d = 0;
    }
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
    }
    return "unknown";
}

} // namespace litefetch
