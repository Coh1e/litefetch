// The native messaging wire format and the handoff contract, driven with a
// simulated browser peer. This is the only platform boundary that can be proven
// without registering a real extension host on the machine.
#include "native_bridge.h"
#include "test_support.h"

#include <nlohmann/json.hpp>

#include <sstream>
#include <string>

using namespace litefetch;
using namespace litefetch::test;

namespace {
void framingRoundTrip()
{
    std::stringstream channel;
    check(writeNativeMessage(channel, R"({"url":"https://example.com/a.bin"})"),
        "a small message is written");
    std::string payload;
    std::string error;
    check(readNativeMessage(channel, payload, error), ("read failed: " + error).c_str());
    checkEqual(payload, std::string(R"({"url":"https://example.com/a.bin"})"), "payload round trip");

    // The length prefix is little-endian and counts bytes, not characters.
    std::stringstream empty;
    check(writeNativeMessage(empty, "abc"), "three byte message is written");
    const auto bytes = empty.str();
    checkEqual(static_cast<long long>(bytes.size()), 7, "four byte header plus three bytes");
    check(bytes[0] == 3 && bytes[1] == 0 && bytes[2] == 0 && bytes[3] == 0, "length prefix is little endian");
}

void framingErrors()
{
    std::string payload;
    std::string error;

    std::stringstream truncated;
    const char truncatedBytes[6] = {5, 0, 0, 0, 'a', 'b'};
    truncated.write(truncatedBytes, 6);
    check(!readNativeMessage(truncated, payload, error), "a truncated frame is rejected");
    check(error.find("cut short") != std::string::npos, ("unexpected error: " + error).c_str());

    std::stringstream emptyFrame;
    const char emptyBytes[4] = {0, 0, 0, 0};
    emptyFrame.write(emptyBytes, 4);
    check(!readNativeMessage(emptyFrame, payload, error), "an empty frame is rejected");

    std::stringstream oversized;
    const char oversizedBytes[4] = {1, 0, 16, 0}; // 1 MiB + 1, little endian
    oversized.write(oversizedBytes, 4);
    check(!readNativeMessage(oversized, payload, error), "an oversized frame is rejected");
    check(error.find("larger") != std::string::npos, ("unexpected error: " + error).c_str());

    std::stringstream closed;
    check(!readNativeMessage(closed, payload, error), "a closed channel is reported");
}

void handoffContract()
{
    const auto ok = parseHandoff(R"({"url":"https://example.com/b.bin","cookies":"a=b","downloadId":"17"})");
    check(ok.ok, ("valid handoff rejected: " + ok.error).c_str());
    checkEqual(ok.handoff.url, std::string("https://example.com/b.bin"), "url is read");
    checkEqual(ok.handoff.cookies, std::string("a=b"), "cookies are read");
    checkEqual(ok.handoff.downloadId, std::string("17"), "download id is read");

    check(parseHandoff(R"({"url":"magnet:?xt=urn:btih:0123456789abcdef0123456789abcdef01234567"})").ok,
        "magnet is accepted");
    check(parseHandoff(R"json({"url":"ed2k://|file|a.bin|10|0123456789abcdef0123456789abcdef|/"})json").ok,
        "ed2k is accepted");

    check(!parseHandoff(R"json({"url":"javascript:alert(1)"})json").ok, "a script scheme is refused");
    check(!parseHandoff(R"json({"url":"file:///C:/x"})json").ok, "a local file is refused");
    check(!parseHandoff("{}").ok, "a handoff without url is refused");
    check(!parseHandoff("not json").ok, "a non-JSON handoff is refused");

    const auto accepted = nlohmann::json::parse(handoffResponse(true, "queued"));
    check(accepted["accepted"] == true, "acceptance is reported");
    checkEqual(accepted["detail"].get<std::string>(), std::string("queued"), "detail is reported");
    const auto refused = nlohmann::json::parse(handoffResponse(false, "unsupported scheme"));
    check(refused["accepted"] == false, "refusal is reported");
}
} // namespace

int main()
{
    return run("native-bridge", [] {
        framingRoundTrip();
        framingErrors();
        handoffContract();
        return 0;
    });
}
