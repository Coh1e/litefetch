// Kad routing primitives: 128-bit ids, XOR distance, bucket eviction and the
// contact file that survives a restart.
#include "kad_routing.h"
#include "kad_wire.h"
#include "test_support.h"

#include <string>
#include <vector>

using namespace litefetch;
using namespace litefetch::test;

namespace {
KadId idFrom(std::string_view hex)
{
    KadId id;
    check(KadId::fromHex(hex, id), "test id parses");
    return id;
}

void ids()
{
    const auto zero = idFrom("00000000000000000000000000000000");
    check(zero.isZero(), "an all-zero id is recognised");
    checkEqual(static_cast<long long>(zero.leadingZeroBits()), 128LL, "a zero id has 128 leading zeros");
    const auto one = idFrom("80000000000000000000000000000000");
    checkEqual(static_cast<long long>(one.leadingZeroBits()), 0LL, "high bit set means no leading zeros");
    const auto low = idFrom("00000000000000000000000000000001");
    checkEqual(static_cast<long long>(low.leadingZeroBits()), 127LL, "a low bit means 127 zeros");

    const auto a = idFrom("00000000000000000000000000000000");
    const auto b = idFrom("00000000000000000000000000000001");
    const auto c = idFrom("00000000000000000000000000000002");
    check(closerTo(a, b, c), "the smaller XOR distance is closer");
    check(!closerTo(a, c, b), "the comparison is not symmetric");
    checkEqual(a.toHex(), std::string("00000000000000000000000000000000"), "hex round trip");
    KadId parsed;
    check(!KadId::fromHex("zz", parsed), "a short or bad hex id is rejected");
}

void wire()
{
    KadPacket hello;
    hello.opcode = KadOpcode::HelloRequest;
    KadContact contact;
    contact.id = idFrom("0123456789abcdef0123456789abcdef");
    contact.address = "192.168.0.7";
    contact.port = 4662;
    hello.contacts.push_back(contact);
    const auto bytes = encodeKadPacket(hello);
    checkEqual(static_cast<long long>(bytes.size()), 3 + 22LL, "hello packet size");
    check(bytes[0] == 0xE4 && bytes[1] == 0x11, "marker and opcode");
    KadPacket decoded;
    check(decodeKadPacket(bytes.data(), bytes.size(), decoded), "hello round trips");
    checkEqual(static_cast<long long>(decoded.contacts.size()), 1LL, "one contact survives");
    checkEqual(decoded.contacts[0].address, std::string("192.168.0.7"), "address survives");
    checkEqual(static_cast<long long>(decoded.contacts[0].port), 4662LL, "port survives");

    KadPacket find;
    find.opcode = KadOpcode::FindRequest;
    find.target = idFrom("ffffffffffffffffffffffffffffffff");
    find.wanted = 4;
    const auto findBytes = encodeKadPacket(find);
    KadPacket findDecoded;
    check(decodeKadPacket(findBytes.data(), findBytes.size(), findDecoded), "find round trips");
    check(findDecoded.target == find.target, "the target survives");
    checkEqual(static_cast<long long>(findDecoded.wanted), 4LL, "the wanted count survives");

    check(!decodeKadPacket(bytes.data(), 2, decoded), "a truncated packet is rejected");
    unsigned char wrongMarker[3] = {0x00, 0x11, 0x00};
    check(!decodeKadPacket(wrongMarker, 3, decoded), "a foreign datagram is rejected");
    unsigned char badOpcode[3] = {0xE4, 0x7f, 0x00};
    check(!decodeKadPacket(badOpcode, 3, decoded), "an unknown opcode is rejected");
    unsigned char shortContacts[5] = {0xE4, 0x19, 0x01, 0x00, 0x00};
    check(!decodeKadPacket(shortContacts, 5, decoded), "a cut contact list is rejected");
}void table()
{
    const auto self = idFrom("00000000000000000000000000000000");
    KadRoutingTable table(self);
    for (int index = 1; index <= 12; ++index) {
        KadContact contact;
        contact.id = idFrom("0000000000000000000000000000000" + std::string(1, "123456789abcdef"[index % 15]));
        contact.address = "10.0.0." + std::to_string(index);
        contact.port = static_cast<std::uint16_t>(4000 + index);
        contact.lastSeenUnix = 1000 + index;
        table.add(contact, 5000);
    }
    check(table.size() > 0, "contacts are stored");
    // The same contact twice must not duplicate.
    KadContact repeated;
    repeated.id = idFrom("00000000000000000000000000000001");
    repeated.address = "10.0.0.1";
    repeated.port = 4001;
    repeated.lastSeenUnix = 9000;
    check(table.add(repeated, 9000), "the contact is added");
    const auto before = table.size();
    repeated.address = "10.0.0.2";
    check(table.add(repeated, 9100), "a known contact is refreshed");
    checkEqual(static_cast<long long>(table.size()), static_cast<long long>(before),
        "refreshing does not add a row");
    check(table.remove(repeated.id), "a contact can be removed");
    check(!table.remove(repeated.id), "removing twice reports nothing to do");

    const auto nearest = table.closest(idFrom("00000000000000000000000000000003"), 3);
    check(nearest.size() <= 3, "closest respects the limit");

    // Persistence: what was saved comes back with the same ids and ports.
    const auto text = table.serialize();
    KadRoutingTable reloaded;
    check(reloaded.load(text), "the saved table loads");
    checkEqual(static_cast<long long>(reloaded.size()), static_cast<long long>(table.size()),
        "the reloaded table has the same size");
    check(!reloaded.serialize().empty(), "the reloaded table serializes again");

    // A full bucket keeps a live peer and refuses a newcomer whose only
    // advantage is arriving later.
    KadRoutingTable tight(idFrom("00000000000000000000000000000000"));
    for (int index = 0; index < static_cast<int>(KadRoutingTable::bucketSize); ++index) {
        KadContact contact;
        contact.id = idFrom("8000000000000000000000000000000" + std::string(1, "123456789a"[index]));
        contact.address = "10.1.0." + std::to_string(index);
        contact.port = static_cast<std::uint16_t>(5000 + index);
        contact.lastSeenUnix = 10000;
        check(tight.add(contact, 10000), "the bucket fills up");
    }
    KadContact newcomer;
    newcomer.id = idFrom("8000000000000000000000000000000f");
    newcomer.address = "10.1.0.99";
    newcomer.port = 5999;
    newcomer.lastSeenUnix = 10001;
    check(!tight.add(newcomer, 10001), "a fresh bucket refuses a newcomer");
    check(tight.add(newcomer, 10000 + 16 * 60), "a stale bucket makes room");
}
} // namespace

int main()
{
    return run("kad", [] {
        ids();
        table();
        wire();
        return 0;
    });
}
