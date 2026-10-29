// The browser handoff protocol against a real task service: prepare, query,
// commit and abort, including the lease rules.
#include "browser_bridge.h"
#include "browser_pipe_server.h"
#include "pipe_channel.h"
#include "native_frame.h"
#include "test_support.h"

#include <chrono>
#include <thread>

using namespace litefetch;
using namespace litefetch::browser;
using namespace litefetch::test;

namespace {
std::string request(std::int64_t id, const std::string& method, const std::string& requestId,
    const std::string& source = {}, const std::string& destination = {})
{
    auto root = JsonValue::makeObject();
    root.set("version", JsonValue::makeNumber(1));
    root.set("id", JsonValue::makeNumber(id));
    root.set("method", JsonValue::makeString(method));
    root.set("requestId", JsonValue::makeString(requestId));
    if (!source.empty() || !destination.empty()) {
        auto payload = JsonValue::makeObject();
        payload.set("source", JsonValue::makeString(source));
        payload.set("destination", JsonValue::makeString(destination));
        root.set("payload", std::move(payload));
    }
    return root.dump();
}

JsonValue answer(const std::string& json)
{
    auto parsed = JsonValue::parse(json);
    check(parsed.has_value(), ("the answer is valid JSON: " + json).c_str());
    return *parsed;
}

void prepareCreatesATaskWithoutStartingIt(const std::filesystem::path& root)
{
    TaskRepository repository(root / "bridge.db");
    TaskService service(repository);
    BrowserBridge bridge(service);

    const auto prepared = answer(bridge.handle(request(1, "prepare", "req-1",
        "https://example.test/file.bin", (root / "file.bin").string())));
    check(prepared.boolOr("ok", false), "prepare is accepted");
    const auto* result = prepared.find("result");
    check(result != nullptr, "prepare returns a result");
    checkEqual(result->stringOr("state", ""), std::string("prepared"), "the task is prepared");
    const auto taskId = result->stringOr("taskId", "");
    check(!taskId.empty(), "prepare names the task");

    const auto stored = service.snapshot(taskId);
    check(stored.has_value(), "the prepared task exists");
    check(stored->state == TaskState::Queued, "a prepared task has not started");

    // The same requestId maps to the same task, never to a second one.
    const auto again = answer(bridge.handle(request(2, "prepare", "req-1",
        "https://example.test/file.bin", (root / "file.bin").string())));
    check(again.boolOr("ok", false), "a repeated prepare is accepted");
    checkEqual(again.find("result")->stringOr("taskId", ""), taskId,
        "the same requestId keeps its task");
    checkEqual(static_cast<long long>(service.snapshots().size()), 1LL,
        "a repeated prepare does not create a second task");
}

void queryReportsTheLeaseState(const std::filesystem::path& root)
{
    TaskRepository repository(root / "bridge-query.db");
    TaskService service(repository);
    BrowserBridge bridge(service);

    const auto unknown = answer(bridge.handle(request(1, "query", "nope")));
    checkEqual(unknown.find("result")->stringOr("state", ""), std::string("unknown"),
        "an unknown request is reported as unknown");

    const auto prepared = answer(bridge.handle(request(2, "prepare", "req-2",
        "https://example.test/q.bin", (root / "q.bin").string())));
    const auto taskId = prepared.find("result")->stringOr("taskId", "");
    const auto known = answer(bridge.handle(request(3, "query", "req-2")));
    checkEqual(known.find("result")->stringOr("state", ""), std::string("prepared"), "the lease is prepared");
    checkEqual(known.find("result")->stringOr("taskId", ""), taskId, "query names the same task");
}

void commitAndAbortFollowTheRules(const std::filesystem::path& root)
{
    TaskRepository repository(root / "bridge-commit.db");
    TaskService service(repository);
    BrowserBridge bridge(service);

    const auto prepared = answer(bridge.handle(request(1, "prepare", "req-3",
        "https://example.test/c.bin", (root / "c.bin").string())));
    const auto taskId = prepared.find("result")->stringOr("taskId", "");

    const auto committed = answer(bridge.handle(request(2, "commit", "req-3")));
    check(committed.boolOr("ok", false), "commit is accepted");
    checkEqual(committed.find("result")->stringOr("state", ""), std::string("committed"),
        "the lease is committed");
    check(service.snapshot(taskId).has_value(), "a committed task stays");

    // A committed request cannot be aborted away.
    const auto refused = answer(bridge.handle(request(3, "abort", "req-3")));
    check(!refused.boolOr("ok", true), "aborting a committed request is refused");
    checkEqual(refused.stringOr("error", ""), std::string("the request is already committed"),
        "the refusal explains itself");

    // An uncommitted one is aborted and its task goes away.
    const auto second = answer(bridge.handle(request(4, "prepare", "req-4",
        "https://example.test/d.bin", (root / "d.bin").string())));
    const auto secondId = second.find("result")->stringOr("taskId", "");
    const auto aborted = answer(bridge.handle(request(5, "abort", "req-4")));
    check(aborted.boolOr("ok", false), "abort is accepted");
    check(!service.snapshot(secondId).has_value(), "an aborted prepared task is removed");
}

void anExpiredLeaseIsRefused(const std::filesystem::path& root)
{
    TaskRepository repository(root / "bridge-expiry.db");
    TaskService service(repository);
    BrowserBridge bridge(service);
    bridge.setLeaseDuration(std::chrono::seconds(0));

    bridge.handle(request(1, "prepare", "req-5", "https://example.test/e.bin",
        (root / "e.bin").string()));
    const auto query = answer(bridge.handle(request(2, "query", "req-5")));
    checkEqual(query.find("result")->stringOr("state", ""), std::string("expired"),
        "the lease expires");
    const auto refused = answer(bridge.handle(request(3, "commit", "req-5")));
    check(!refused.boolOr("ok", true), "committing an expired lease is refused");
}

void theServerAnswersOverARealPipe(const std::filesystem::path& root)
{
    TaskRepository repository(root / "bridge-pipe.db");
    TaskService service(repository);
    BrowserBridge bridge(service);
    BrowserPipeServer server("litefetch-bridge-test");
    std::string error;
    check(server.start(bridge, error), ("the server did not start: " + error).c_str());

    const auto destination = (root / "piped.bin").string();
    std::string channelError;
    const auto raw = pipeRoundTrip("litefetch-bridge-test",
        request(11, "prepare", "req-pipe", "https://example.test/piped.bin", destination),
        5000, channelError);
    check(channelError.empty(), ("the channel failed: " + channelError).c_str());
    const auto prepared = answer(raw);
    check(prepared.boolOr("ok", false), "the server answers over the pipe");
    checkEqual(static_cast<long long>(prepared.numberOr("id", 0)), 11LL, "the id crosses the pipe");
    const auto taskId = prepared.find("result")->stringOr("taskId", "");
    check(!taskId.empty(), "the prepared task is named");
    check(service.snapshot(taskId).has_value(), "the task exists in the service");

    // A second connection works too: the server keeps listening.
    const auto again = answer(pipeRoundTrip("litefetch-bridge-test",
        request(12, "query", "req-pipe"), 5000, channelError));
    check(again.boolOr("ok", false), "the server accepts a second connection");
    checkEqual(again.find("result")->stringOr("state", ""), std::string("prepared"),
        "the lease survives between connections");

    server.stop();
}void malformedRequestsAreAnsweredNotThrown(const std::filesystem::path& root)
{
    TaskRepository repository(root / "bridge-bad.db");
    TaskService service(repository);
    BrowserBridge bridge(service);

    const auto broken = answer(bridge.handle("{"));
    check(!broken.boolOr("ok", true), "broken JSON is refused");
    const auto noMethod = answer(bridge.handle(request(9, "dance", "req-6")));
    check(!noMethod.boolOr("ok", true), "an unknown method is refused");
    checkEqual(static_cast<long long>(noMethod.numberOr("id", 0)), 9LL, "the id is kept");
    const auto noRequestId = answer(bridge.handle(R"({"version":1,"id":5,"method":"query"})"));
    check(!noRequestId.boolOr("ok", true), "a missing requestId is refused");
}
} // namespace

int main()
{
    return run("browser-bridge", [] {
        TempDir root;
        prepareCreatesATaskWithoutStartingIt(root.path());
        queryReportsTheLeaseState(root.path());
        commitAndAbortFollowTheRules(root.path());
        anExpiredLeaseIsRefused(root.path());
        malformedRequestsAreAnsweredNotThrown(root.path());
        theServerAnswersOverARealPipe(root.path());
        return 0;
    });
}
