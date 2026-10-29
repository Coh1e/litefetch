#pragma once

#include "native_frame.h"
#include "task_service.h"

#include <chrono>
#include <filesystem>
#include <map>
#include <string>

namespace litefetch::browser {

// The handoff protocol the browser extension sees. Requests arrive as JSON over
// the local channel; this class turns them into task commands, so the extension
// can prepare a task, ask about it, commit it or abort it without touching the
// database itself.
class BrowserBridge {
public:
    // The download directory fills in a destination the extension cannot know.
    BrowserBridge(TaskService& service, std::filesystem::path downloadDirectory);

    // Answers one request. Never throws: a malformed request becomes an error
    // answer with the id kept when it was readable.
    std::string handle(const std::string& requestJson);

    // Prepared requests expire; a commit after that is refused rather than
    // silently creating a task much later.
    void setLeaseDuration(std::chrono::seconds duration) { lease_ = duration; }

private:
    struct Lease {
        std::string taskId;
        bool committed = false;
        std::chrono::steady_clock::time_point expiresAt;
    };

    std::string prepare(const JsonValue& id, const std::string& requestId,
        const std::string& source, const std::string& destination);
    std::string query(const JsonValue& id, const std::string& requestId);
    std::string commit(const JsonValue& id, const std::string& requestId);
    std::string abort(const JsonValue& id, const std::string& requestId);

    TaskService& service_;
    std::filesystem::path downloadDirectory_;
    std::map<std::string, Lease, std::less<>> leases_;
    std::chrono::seconds lease_{300};
};

} // namespace litefetch::browser
