#include "browser_bridge.h"

#include "native_frame.h"

namespace litefetch::browser {
namespace {
std::string okAnswer(std::int64_t id, JsonValue result)
{
    auto answer = JsonValue::makeObject();
    answer.set("version", JsonValue::makeNumber(1));
    answer.set("id", JsonValue::makeNumber(id));
    answer.set("ok", JsonValue::makeBool(true));
    answer.set("result", std::move(result));
    return answer.dump();
}

std::string failAnswer(std::int64_t id, const std::string& reason)
{
    auto answer = JsonValue::makeObject();
    answer.set("version", JsonValue::makeNumber(1));
    answer.set("id", JsonValue::makeNumber(id));
    answer.set("ok", JsonValue::makeBool(false));
    answer.set("error", JsonValue::makeString(reason));
    return answer.dump();
}
} // namespace

BrowserBridge::BrowserBridge(TaskService& service) : service_(service) {}

std::string BrowserBridge::handle(const std::string& requestJson)
{
    const auto parsed = JsonValue::parse(requestJson);
    if (!parsed) return failAnswer(0, "malformed request");
    const auto id = parsed->numberOr("id", 0);
    const auto method = parsed->stringOr("method", "");
    const auto requestId = parsed->stringOr("requestId", "");
    if (requestId.empty()) return failAnswer(id, "the request has no requestId");

    if (method == "prepare") {
        const auto* payload = parsed->find("payload");
        if (!payload) return failAnswer(id, "prepare needs a payload");
        const auto source = payload->stringOr("source", "");
        const auto destination = payload->stringOr("destination", "");
        if (source.empty()) return failAnswer(id, "prepare needs a source");
        return prepare(id, requestId, source, destination);
    }
    if (method == "query") return query(id, requestId);
    if (method == "commit") return commit(id, requestId);
    if (method == "abort") return abort(id, requestId);
    return failAnswer(id, "unknown method: " + method);
}

std::string BrowserBridge::prepare(std::int64_t id, const std::string& requestId,
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
    add.destination = destination;
    const auto added = service_.execute(add);
    if (!added.ok) return failAnswer(id, added.error);

    Lease lease;
    lease.taskId = added.task.id;
    lease.expiresAt = std::chrono::steady_clock::now() + lease_;
    leases_[requestId] = lease;

    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        lease.expiresAt - std::chrono::steady_clock::now());
    auto result = JsonValue::makeObject();
    result.set("state", JsonValue::makeString("prepared"));
    result.set("taskId", JsonValue::makeString(added.task.id));
    result.set("expiresInMs", JsonValue::makeNumber(remaining.count()));
    return okAnswer(id, std::move(result));
}

std::string BrowserBridge::query(std::int64_t id, const std::string& requestId)
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

std::string BrowserBridge::commit(std::int64_t id, const std::string& requestId)
{
    const auto known = leases_.find(requestId);
    if (known == leases_.end()) return failAnswer(id, "unknown request");
    if (known->second.expiresAt <= std::chrono::steady_clock::now())
        return failAnswer(id, "the prepared request expired");
    known->second.committed = true;
    auto result = JsonValue::makeObject();
    result.set("state", JsonValue::makeString("committed"));
    result.set("taskId", JsonValue::makeString(known->second.taskId));
    return okAnswer(id, std::move(result));
}

std::string BrowserBridge::abort(std::int64_t id, const std::string& requestId)
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
