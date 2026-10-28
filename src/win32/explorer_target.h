#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace litefetch::ui {

// The Explorer argument that `打开文件夹` should use for a task destination:
// the finished file gets selected, a missing file falls back to its folder.
// Empty when neither the file nor its folder exists, which the window reports
// instead of launching Explorer at nothing.
std::optional<std::wstring> explorerCommandLine(const std::filesystem::path& destination);

} // namespace litefetch::ui
