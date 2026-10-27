#pragma once

#include <windows.h>

#include <filesystem>
#include <optional>
#include <string>

namespace litefetch::ui {

struct NewDownloadRequest {
    std::string source;              // UTF-8 address exactly as typed
    std::filesystem::path directory; // native (wide) save folder
    std::string suggestedFileName;   // UTF-8 file name derived from the address
    bool autoStart = true;
};

// Native modal dialog. Returns nothing when the user cancels. Only the
// capabilities of this ticket are offered: one HTTP(S) address, an explicit
// destination folder and "start immediately"; batch add is not implied.
std::optional<NewDownloadRequest> showNewDownloadDialog(HWND owner,
    const std::filesystem::path& initialDirectory);

} // namespace litefetch::ui
