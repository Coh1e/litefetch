// The browser handoff protocol against a real task service: prepare, query,
// commit and abort, including the lease rules.
#include "browser_bridge.h"
#include "browser_pipe_server.h"
#include "native_host_loop.h"
#include "pipe_channel.h"
#include "native_frame.h"
#include "test_support.h"

#include <chrono>
#include <sstream>
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
    BrowserBridge bridge(service, root);

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
    BrowserBridge bridge(service, root);

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
    BrowserBridge bridge(service, root);

    const auto prepared = answer(bridge.handle(request(1, "prepare", "req-3",
        "https://example.test/c.bin", (root / "c.bin").string())));
    const auto taskId = prepared.find("result")->stringOr("taskId", "");

    // The application learns about the commit so it can start the task: the user
    // confirmed the takeover in the browser, and a second press in the window
    // would be a surprise.
    std::string startedTask;
    bridge.setCommittedHandler([&](const std::string& id) { startedTask = id; });
    const auto committed = answer(bridge.handle(request(2, "commit", "req-3")));
    check(committed.boolOr("ok", false), "commit is accepted");
    checkEqual(committed.find("result")->stringOr("state", ""), std::string("committed"),
        "the lease is committed");
    check(service.snapshot(taskId).has_value(), "a committed task stays");
    checkEqual(startedTask, taskId, "the application is told which task to start");

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
    BrowserBridge bridge(service, root);
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
    BrowserBridge bridge(service, root);
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
}void theHostLoopReachesTheRunningServer(const std::filesystem::path& root)
{
    // The whole chain the browser uses, minus the browser: framed stdin ->
    // channel -> server -> bridge -> framed stdout.
    TaskRepository repository(root / "bridge-host.db");
    TaskService service(repository);
    BrowserBridge bridge(service, root);
    const auto pipeName = defaultPipeName() + "-host";
    BrowserPipeServer server(pipeName);
    std::string error;
    check(server.start(bridge, error), ("the server did not start: " + error).c_str());

    std::istringstream input(frameMessage(request(21, "prepare", "req-host",
        "https://example.test/host.bin", (root / "host.bin").string())));
    std::ostringstream output;
    const auto code = runHostLoop(input, output, [&](const std::string& message) {
        std::string channelError;
        const auto answer = pipeRoundTrip(pipeName, message, 5000, channelError);
        if (answer.empty()) throw std::runtime_error(channelError);
        return answer;
    });
    checkEqual(static_cast<long long>(code), 0LL, "the host loop exits cleanly");
    std::string buffer = output.str();
    std::string answerJson;
    check(popMessage(buffer, answerJson), "the host wrote one framed answer");
    const auto parsed = JsonValue::parse(answerJson);
    check(parsed.has_value(), "the answer is JSON");
    check(parsed->boolOr("ok", false), "the answer says the prepare worked");
    const auto taskId = parsed->find("result")->stringOr("taskId", "");
    check(service.snapshot(taskId).has_value(), "the task reached the service");
    server.stop();
}void theExtensionShapeIsUnderstood(const std::filesystem::path& root)
{
    // Exactly what browser/adapters.mjs sends: string ids, payload.url, and the
    // answer must echo the id with the same type or the extension drops it.
    TaskRepository repository(root / "bridge-ext.db");
    TaskService service(repository);
    BrowserBridge bridge(service, root);

    const auto requestJson = [&] {
        auto root = JsonValue::makeObject();
        root.set("version", JsonValue::makeNumber(1));
        root.set("id", JsonValue::makeString("m-9f2"));
        root.set("method", JsonValue::makeString("prepare"));
        root.set("requestId", JsonValue::makeString("req-ext"));
        auto payload = JsonValue::makeObject();
        payload.set("url", JsonValue::makeString("https://example.test/ext.bin"));
        payload.set("method", JsonValue::makeString("GET"));
        payload.set("uniqueMatch", JsonValue::makeBool(true));
        payload.set("replaySafe", JsonValue::makeBool(true));
        root.set("payload", std::move(payload));
        return root.dump();
    }();

    const auto answered = answer(bridge.handle(requestJson));
    check(answered.boolOr("ok", false), "the extension's prepare is accepted");
    const auto* id = answered.find("id");
    check(id != nullptr, "the answer has an id");
    check(id->kind() == JsonValue::Kind::String, "the string id is echoed as a string");
    checkEqual(id->asString(), std::string("m-9f2"), "the id value is unchanged");
    const auto taskId = answered.find("result")->stringOr("taskId", "");
    check(!taskId.empty(), "the task was created");
    // The extension checks this against Date.now(), so it must be an absolute
    // epoch millisecond value rather than a duration.
    const auto expiresAt = answered.find("result")->numberOr("expiresAt", 0);
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    check(expiresAt > now, "the lease expiry is in the future");
    check(expiresAt < now + 600000, "the lease expiry is bounded");
    const auto stored = service.snapshot(taskId);
    check(stored.has_value() && stored->source == "https://example.test/ext.bin",
        "payload.url became the task source");
    check(stored->state == TaskState::Queued, "the task is not started by prepare");
}
void malformedRequestsAreAnsweredNotThrown(const std::filesystem::path& root)
{
    TaskRepository repository(root / "bridge-bad.db");
    TaskService service(repository);
    BrowserBridge bridge(service, root);

    const auto broken = answer(bridge.handle("{"));
    check(!broken.boolOr("ok", true), "broken JSON is refused");
    const auto noMethod = answer(bridge.handle(request(9, "dance", "req-6")));
    check(!noMethod.boolOr("ok", true), "an unknown method is refused");
    // The popup asks whether the application is there before blaming the site
    // allowlist; that question carries no request.
    const auto ping = answer(bridge.handle(R"({"version":1,"id":"p1","method":"ping"})"));
    check(ping.boolOr("ok", false), "ping is accepted without a request");
    check(ping.find("result")->stringOr("state", "") == "ok", "ping says the bridge is up");
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
        theHostLoopReachesTheRunningServer(root.path());
        theExtensionShapeIsUnderstood(root.path());
        return 0;
    });
}
