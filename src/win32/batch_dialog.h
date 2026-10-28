#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "batch_input.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace litefetch::ui {

struct BatchAddRequest {
    // Every non-empty source line in original order. Invalid/unsupported lines
    // are included so the caller can report them; only `valid` lines are
    // submitted when `onlyValid` is set or all lines are valid.
    std::vector<BatchLine> lines;
    std::filesystem::path directory;
    bool autoStart = true;
    bool onlyValid = false;
    // Set only for the explicit "keep another copy" choice after a duplicate
    // source was detected; the destination is already derived by the caller.
    bool allowDuplicate = false;
};

std::optional<BatchAddRequest> showBatchAddDialog(HWND owner,
    const std::filesystem::path& initialDirectory, std::string_view initialText);

} // namespace litefetch::ui
