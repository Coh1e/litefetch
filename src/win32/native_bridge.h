#pragma once

#include <cstdint>
#include <iosfwd>
#include <string>
#include <string_view>

namespace litefetch {

// Chrome, Edge and Firefox all carry native messaging traffic as a 32-bit
// little-endian length followed by that many bytes of UTF-8 JSON. The browser
// caps a message at 1 MiB, so a longer frame is a protocol error, not a reason
// to allocate.
constexpr std::uint32_t maxNativeMessageBytes = 1024 * 1024;

// One handoff request from the browser extension.
struct BrowserHandoff {
    std::string url;
    std::string referrer;
    std::string cookies;
    std::string userAgent;
    // The extension's own download id, used to correlate a cancellation.
    std::string downloadId;
};

struct HandoffParse {
    bool ok = false;
    BrowserHandoff handoff;
    std::string error;
};

// Reads one framed message. Returns false and fills `error` when the stream
// ends, the frame is truncated, or it exceeds the cap.
bool readNativeMessage(std::istream& input, std::string& payload, std::string& error);

// Writes one framed message. Returns false when the payload is too large.
bool writeNativeMessage(std::ostream& output, std::string_view payload);

// Parses the JSON the extension sends and validates the URL scheme. Only the
// schemes the core can download are accepted; anything else is reported back so
// the extension can leave the browser download alone.
HandoffParse parseHandoff(std::string_view json);

// Builds the JSON answer for a handoff. `accepted` false means the extension
// must keep its own download.
std::string handoffResponse(bool accepted, std::string_view detail);

} // namespace litefetch
