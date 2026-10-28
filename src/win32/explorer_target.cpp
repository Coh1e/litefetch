#include "explorer_target.h"

namespace litefetch::ui {

std::optional<std::wstring> explorerCommandLine(const std::filesystem::path& destination)
{
    if (destination.empty()) return std::nullopt;
    std::error_code error;
    if (std::filesystem::is_regular_file(destination, error))
        return L"/select,\"" + destination.wstring() + L"\"";
    const auto parent = destination.parent_path();
    if (parent.empty() || !std::filesystem::is_directory(parent, error)) return std::nullopt;
    return L"\"" + parent.wstring() + L"\"";
}

} // namespace litefetch::ui
