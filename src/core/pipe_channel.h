#pragma once

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

} // namespace litefetch::browser
