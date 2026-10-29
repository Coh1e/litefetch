// Native messaging framing and the small JSON reader the browser handoff uses.
#include "native_frame.h"
#include "test_support.h"

using namespace litefetch;
using namespace litefetch::browser;
using namespace litefetch::test;

namespace {
void framingRoundTrips()
{
    const std::string json = R"({"id":7,"method":"prepare"})";
    std::string buffer = frameMessage(json);
    std::string message;
    check(popMessage(buffer, message), "a complete frame pops");
    checkEqual(message, json, "the payload survives framing");
    check(buffer.empty(), "the buffer is consumed");

    // Two messages in one read, in order.
    buffer = frameMessage("{\"id\":1}") + frameMessage("{\"id\":2}");
    std::string first;
    std::string second;
    check(popMessage(buffer, first), "the first frame pops");
    check(popMessage(buffer, second), "the second frame pops");
    checkEqual(first, std::string(R"({"id":1})"), "the first payload is intact");
    checkEqual(second, std::string(R"({"id":2})"), "the second payload is intact");

    // A partial frame waits instead of being consumed.
    buffer = frameMessage("{\"id\":3}");
    buffer.pop_back();
    std::string none;
    check(!popMessage(buffer, none), "a partial frame does not pop");
    check(!buffer.empty(), "the partial frame is kept for the next read");

    // A length claiming more than the limit is a broken stream.
    std::string broken(4, '\0');
    broken[0] = '\xff';
    broken[1] = '\xff';
    broken[2] = '\xff';
    broken[3] = '\x7f';
    check(!popMessage(broken, none), "an absurd length is refused");
    check(broken.empty(), "the broken stream is dropped");
}

void jsonReadsTheProtocol()
{
    const auto parsed = JsonValue::parse(R"({"version":1,"id":42,"method":"prepare",
        "requestId":"abc","payload":{"source":"https://example.test/a.bin","size":12}})");
    check(parsed.has_value(), "a request object parses");
    checkEqual(static_cast<long long>(parsed->numberOr("version", 0)), 1LL, "the version is read");
    checkEqual(static_cast<long long>(parsed->numberOr("id", 0)), 42LL, "the message id is read");
    checkEqual(parsed->stringOr("method", ""), std::string("prepare"), "the method is read");
    checkEqual(parsed->stringOr("requestId", ""), std::string("abc"), "the request id is read");

    const auto* payload = parsed->find("payload");
    check(payload != nullptr, "the nested payload is found");
    checkEqual(payload->stringOr("source", ""), std::string("https://example.test/a.bin"),
        "the nested source is read");
    checkEqual(static_cast<long long>(payload->numberOr("size", 0)), 12LL, "the nested size is read");

    // Missing keys fall back instead of failing.
    checkEqual(parsed->stringOr("missing", "fallback"), std::string("fallback"),
        "a missing key uses the fallback");
    checkEqual(static_cast<long long>(parsed->numberOr("missing", 7)), 7LL,
        "a missing number uses the fallback");

    // Malformed input is refused rather than half read.
    check(!JsonValue::parse("{").has_value(), "an unfinished object is refused");
    check(!JsonValue::parse(R"({"a":1,})").has_value(), "a trailing comma is refused");
    check(!JsonValue::parse(R"({"a":1} tail)").has_value(), "trailing bytes are refused");
    check(!JsonValue::parse(R"({"a":1.5})").has_value(), "a fractional number is refused");
    check(!JsonValue::parse(R"({"a":"\u0041"})").has_value(), "an unsupported escape is refused");
}

void jsonWritesResponses()
{
    auto response = JsonValue::makeObject();
    response.set("version", JsonValue::makeNumber(1));
    response.set("id", JsonValue::makeNumber(9));
    response.set("ok", JsonValue::makeBool(true));
    auto result = JsonValue::makeObject();
    result.set("taskId", JsonValue::makeString("t-1"));
    response.set("result", std::move(result));
    checkEqual(response.dump(), std::string(R"({"version":1,"id":9,"ok":true,"result":{"taskId":"t-1"}})"),
        "a response is written in the protocol's shape");

    // Strings are escaped so a path with quotes cannot break the frame.
    auto quoted = JsonValue::makeObject();
    quoted.set("path", JsonValue::makeString("C:\\a\"b"));
    checkEqual(quoted.dump(), std::string(R"({"path":"C:\\a\"b"})"), "quotes and slashes are escaped");
    const auto roundTrip = JsonValue::parse(quoted.dump());
    check(roundTrip.has_value(), "the escaped output parses back");
    checkEqual(roundTrip->stringOr("path", ""), std::string("C:\\a\"b"),
        "the escaped value round trips");
}
} // namespace

int main()
{
    return run("native-frame", [] {
        framingRoundTrips();
        jsonReadsTheProtocol();
        jsonWritesResponses();
        return 0;
    });
}
