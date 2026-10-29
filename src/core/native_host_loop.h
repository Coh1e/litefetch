#pragma once

#include <functional>
#include <istream>
#include <ostream>
#include <string>

namespace litefetch::browser {

// The native messaging host itself: read framed JSON requests from the browser
// on stdin, hand each one to `channel`, and write the framed answer to stdout.
// The loop is a function so it can be driven without a second process (see
// docs/adr/0001-acceptance-without-a-second-process.md).
int runHostLoop(std::istream& input, std::ostream& output,
    const std::function<std::string(const std::string&)>& channel);

// Builds the response the browser sees when the app cannot be reached. It keeps
// the message id so the extension can match it, and says why.
std::string errorResponse(const std::string& requestJson, const std::string& reason);

} // namespace litefetch::browser
