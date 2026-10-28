#include "startup.h"

namespace litefetch::ui {

StartupOptions parseStartupOptions(const AppOptions& defaults,
    const std::vector<std::wstring>& arguments)
{
    StartupOptions options;
    options.window = defaults;
    for (std::size_t index = 1; index < arguments.size(); ++index) {
        const auto& argument = arguments[index];
        if (argument == L"--db" && index + 1 < arguments.size()) {
            options.window.databasePath = arguments[++index];
        } else if (argument == L"--download-dir" && index + 1 < arguments.size()) {
            options.window.downloadDirectory = arguments[++index];
        } else {
            options.error = "unknown option; usage: litefetch-win32 [--db <path>] [--download-dir <path>]";
        }
    }
    return options;
}

} // namespace litefetch::ui
