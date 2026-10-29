#include "browser_bridge.h"

#include "native_frame.h"

namespace litefetch::browser {
namespace {
std::string okAnswer(const JsonValue& id, JsonValue result)
{
    auto answer = JsonValue::makeObject();
    answer.set("version", JsonValue::makeNumber(1));
    answer.set("id", id);
    answer.set("ok", JsonValue::makeBool(true));
    answer.set("result", std::move(result));
    return answer.dump();
}

std::string failAnswer(const JsonValue& id, const std::string& reason)
{
    auto answer = JsonValue::makeObject();
    answer.set("version", JsonValue::makeNumber(1));
    answer.set("id", id);
    answer.set("ok", JsonValue::makeBool(false));
    answer.set("error", JsonValue::makeString(reason));
    return answer.dump();
}
} // namespace

namespace {
std::string fileNameOf(const std::string& url)
{
    const auto query = url.find_first_of("?#");
    auto path = url.substr(0, query);
    const auto slash = path.find_last_of('/');
    auto name = slash == std::string::npos ? std::string() : path.substr(slash + 1);
    if (name.empty()) name = "download.bin";
    return name;
}
} // namespace

BrowserBridge::BrowserBridge(TaskService& service, std::filesystem::path downloadDirectory)
    : service_(service), downloadDirectory_(std::move(downloadDirectory)) {}

std::string BrowserBridge::handle(const std::string& requestJson)
{
    const auto parsed = JsonValue::parse(requestJson);
    if (!parsed) return failAnswer(JsonValue::makeString(""), "malformed request");
    // The browser matches answers by id, and it uses strings; the id is echoed
    // with the type it arrived with rather than converted.
    JsonValue id = JsonValue::makeString("");
    if (const auto* given = parsed->find("id")) id = *given;
    const auto method = parsed->stringOr("method", "");
    // A ping answers whether the application is there at all; it carries no
    // request, so it must not require one.
    if (method == "ping") {
        auto result = JsonValue::makeObject();
        result.set("state", JsonValue::makeString("ok"));
        result.set("version", JsonValue::makeNumber(1));
        return okAnswer(id, std::move(result));
    }
    const auto requestId = parsed->stringOr("requestId", "");
    if (requestId.empty()) return failAnswer(id, "the request has no requestId");

    if (method == "prepare") {
        const auto* payload = parsed->find("payload");
        if (!payload) return failAnswer(id, "prepare needs a payload");
        // The extension names the address "url"; "source" is kept for the
        // tests and for any other caller of this protocol.
        auto source = payload->stringOr("url", "");
        if (source.empty()) source = payload->stringOr("source", "");
        const auto destination = payload->stringOr("destination", "");
        if (source.empty()) return failAnswer(id, "prepare needs a source");
        return prepare(id, requestId, source, destination);
    }
    if (method == "query") return query(id, requestId);
    if (method == "commit") return commit(id, requestId);
    if (method == "abort") return abort(id, requestId);
    return failAnswer(id, "unknown method: " + method);
}

std::string BrowserBridge::prepare(const JsonValue& id, const std::string& requestId,
    const std::string& source, const std::string& destination)
{
    const auto known = leases_.find(requestId);
    if (known != leases_.end() && known->second.expiresAt > std::chrono::steady_clock::now()) {
        // The same requestId must map to the same task, never to a second one.
        auto result = JsonValue::makeObject();
        result.set("state", JsonValue::makeString("prepared"));
        result.set("taskId", JsonValue::makeString(known->second.taskId));
        return okAnswer(id, std::move(result));
    }

    AddTaskCommand add;
    add.source = source;
    // The extension names no destination: the app decides where its downloads
    // live, and the file name comes from the address.
    add.destination = destination.empty()
        ? (downloadDirectory_ / fileNameOf(source)).string()
        : destination;
    const auto added = service_.execute(add);
    if (!added.ok) return failAnswer(id, added.error);

    Lease lease;
    lease.taskId = added.task.id;
    lease.expiresAt = std::chrono::steady_clock::now() + lease_;
    leases_[requestId] = lease;

    // The extension compares this with Date.now(), so it is an absolute epoch
    // millisecond value, not a duration.
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        lease.expiresAt - std::chrono::steady_clock::now());
    const auto epochNow = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch());
    auto result = JsonValue::makeObject();
    result.set("state", JsonValue::makeString("prepared"));
    result.set("taskId", JsonValue::makeString(added.task.id));
    result.set("expiresAt", JsonValue::makeNumber(epochNow.count() + remaining.count()));
    result.set("expiresInMs", JsonValue::makeNumber(remaining.count()));
    return okAnswer(id, std::move(result));
}

std::string BrowserBridge::query(const JsonValue& id, const std::string& requestId)
{
    const auto known = leases_.find(requestId);
    if (known == leases_.end()) {
        auto result = JsonValue::makeObject();
        result.set("state", JsonValue::makeString("unknown"));
        return okAnswer(id, std::move(result));
    }
    const auto expired = known->second.expiresAt <= std::chrono::steady_clock::now();
    auto result = JsonValue::makeObject();
    result.set("state", JsonValue::makeString(known->second.committed ? "committed"
        : (expired ? "expired" : "prepared")));
    result.set("taskId", JsonValue::makeString(known->second.taskId));
    if (const auto task = service_.snapshot(known->second.taskId))
        result.set("taskState", JsonValue::makeNumber(static_cast<int>(task->state)));
    return okAnswer(id, std::move(result));
}

std::string BrowserBridge::commit(const JsonValue& id, const std::string& requestId)
{
    const auto known = leases_.find(requestId);
    if (known == leases_.end()) return failAnswer(id, "unknown request");
    if (known->second.expiresAt <= std::chrono::steady_clock::now())
        return failAnswer(id, "the prepared request expired");
    known->second.committed = true;
    // The task is the user's now; the application starts it, so confirming the
    // takeover in the browser is all the confirmation that is needed.
    if (onCommitted_) onCommitted_(known->second.taskId);
    auto result = JsonValue::makeObject();
    result.set("state", JsonValue::makeString("committed"));
    result.set("taskId", JsonValue::makeString(known->second.taskId));
    return okAnswer(id, std::move(result));
}

std::string BrowserBridge::abort(const JsonValue& id, const std::string& requestId)
{
    const auto known = leases_.find(requestId);
    if (known == leases_.end()) return failAnswer(id, "unknown request");
    if (known->second.committed) return failAnswer(id, "the request is already committed");
    const auto taskId = known->second.taskId;
    leases_.erase(known);
    const auto removed = service_.execute(DeleteTaskCommand{taskId});
    if (!removed.ok) return failAnswer(id, removed.error);
    auto result = JsonValue::makeObject();
    result.set("state", JsonValue::makeString("aborted"));
    result.set("taskId", JsonValue::makeString(taskId));
    return okAnswer(id, std::move(result));
}

} // namespace litefetch::browser
