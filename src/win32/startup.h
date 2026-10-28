#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace litefetch::ui {

// Where the window keeps its task database and where new downloads land by
// default. The product fills these from the user's known folders before parsing
// the command line, so an acceptance run can redirect both with --db and
// --download-dir and touch nothing outside the project.
struct AppOptions {
    std::filesystem::path databasePath;
    std::filesystem::path downloadDirectory;
};

struct StartupOptions {
    AppOptions window;
    std::string error;
};

// Applies the command line to the defaults. Any unknown option, or an option
// whose value is missing, leaves the defaults in place and fills `error` with
// the usage text the startup message box shows.
StartupOptions parseStartupOptions(const AppOptions& defaults,
    const std::vector<std::wstring>& arguments);

} // namespace litefetch::ui
