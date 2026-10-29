#pragma once

#include <windows.h>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace litefetch::ui {

struct NewDownloadRequest {
    std::string source;              // UTF-8 address exactly as typed
    std::filesystem::path directory; // native (wide) save folder
    std::string suggestedFileName;   // UTF-8 file name derived from the address
    bool autoStart = true;
    // A multi-line paste asks whether to switch to batch add. The original
    // text is carried over unchanged; declining never creates tasks.
    bool switchToBatch = false;
    std::string batchText;
    // Optional SHA-256 the finished file must match (hex, case insensitive).
    std::string sha256;
};

// Native modal dialog. Returns nothing when the user cancels. Only the
// capabilities of this ticket are offered: one HTTP address (HTTPS is accepted
// by the parser and engine but is not verified by this ticket), an explicit
// destination folder and "start immediately". Multiple non-empty lines offer
// the batch dialog instead of silently splitting the text.
std::optional<NewDownloadRequest> showNewDownloadDialog(HWND owner,
    const std::filesystem::path& initialDirectory, std::string_view initialUrl = {});

} // namespace litefetch::ui
