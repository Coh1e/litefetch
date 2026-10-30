#pragma once

#include <functional>
#include <string>

namespace litefetch::browser {

// The local channel between the native messaging host and the running app. A
// named pipe keeps it on this machine and inside the user's session, and a fixed
// name means the extension manifest does not need a port.
std::string defaultPipeName();

// Sends one framed request and returns the framed answer. Returns an empty string
// on failure and fills `error` with the reason.
std::string pipeRoundTrip(const std::string& pipeName, const std::string& request,
    int timeoutMillis, std::string& error);

// Same, but calls `startApplication` when the first attempt finds nobody
// listening, then retries while the application comes up. A browser extension
// cannot start a program by itself, so the host — which the browser did start —
// wakes the downloader instead of telling the user to go and open it.
std::string pipeRoundTripStarting(const std::string& pipeName, const std::string& request,
    const std::function<void()>& startApplication,
    int attempts = 20, int delayMillis = 200, std::string* error = nullptr);

} // namespace litefetch::browser
