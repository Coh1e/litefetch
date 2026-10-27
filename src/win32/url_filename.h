#pragma once

#include <string>
#include <string_view>

namespace litefetch::ui {

enum class SourceKind { Empty, Http, Https, Unsupported, Multiple };

struct SourceInput {
    SourceKind kind = SourceKind::Empty;
    std::string source;            // UTF-8, first non-empty address
    std::string suggestedFileName; // UTF-8 file name derived from the address
    std::string hint;              // UTF-8 status line for the dialog
};

// Parses the new-download address field exactly as typed. HTTP is the verified
// path in this ticket; the parser and engine also accept HTTPS, but HTTPS
// transfer is not verified here (#6). Multiple addresses point at the future
// batch dialog and are not silently trimmed to the first one.
SourceInput analyzeSourceInput(std::string_view text);

// UTF-8 file name from a single HTTP(S) URL (HTTPS URLs are parsed; HTTPS
// transfer is not verified by this ticket): query/fragment removed, %XX decoded
// as UTF-8 bytes, Windows-reserved characters replaced with '_'.
std::string suggestedFileName(std::string_view url);

} // namespace litefetch::ui
