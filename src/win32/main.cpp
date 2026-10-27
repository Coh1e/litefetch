#include "main_window.h"

#include "text_utf8.h"

#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>

#include <filesystem>
#include <string>
#include <vector>

namespace {
std::filesystem::path knownFolder(REFKNOWNFOLDERID id)
{
    PWSTR raw = nullptr;
    if (FAILED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &raw))) return {};
    std::filesystem::path result(raw);
    CoTaskMemFree(raw);
    return result;
}

void enablePerMonitorDpi()
{
    using SetDpiAwarenessContext = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
    const auto setContext = reinterpret_cast<SetDpiAwarenessContext>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext"));
    if (setContext) setContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
}

struct StartupOptions {
    litefetch::ui::AppOptions window;
    std::string error;
};

StartupOptions parseArguments(const std::vector<std::wstring>& arguments)
{
    StartupOptions options;
    options.window.databasePath = knownFolder(FOLDERID_LocalAppData) / L"LiteFetch" / L"tasks.db";
    options.window.downloadDirectory = knownFolder(FOLDERID_Downloads);
    if (options.window.downloadDirectory.empty())
        options.window.downloadDirectory = std::filesystem::current_path();
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
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand)
{
    enablePerMonitorDpi();
    INITCOMMONCONTROLSEX commonControls{};
    commonControls.dwSize = sizeof(commonControls);
    commonControls.dwICC = ICC_WIN95_CLASSES | ICC_BAR_CLASSES | ICC_LISTVIEW_CLASSES
        | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&commonControls);
    if (FAILED(OleInitialize(nullptr))) return 1;

    int result = 1;
    int argumentCount = 0;
    LPWSTR* rawArguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
    std::vector<std::wstring> arguments;
    if (rawArguments) {
        for (int index = 0; index < argumentCount; ++index) arguments.emplace_back(rawArguments[index]);
        LocalFree(rawArguments);
    }
    const auto options = parseArguments(arguments);
    if (!options.error.empty()) {
        MessageBoxW(nullptr, litefetch::utf16FromUtf8(options.error).c_str(), L"lite fetch",
            MB_OK | MB_ICONERROR);
        OleUninitialize();
        return 2;
    }

    {
        litefetch::ui::MainWindow window(options.window);
        if (window.create(instance, showCommand)) {
            MSG message{};
            while (GetMessageW(&message, nullptr, 0, 0) > 0) {
                if (window.accelerators()
                    && TranslateAcceleratorW(window.handle(), window.accelerators(), &message))
                    continue;
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            result = static_cast<int>(message.wParam);
        }
    }
    OleUninitialize();
    return result;
}
