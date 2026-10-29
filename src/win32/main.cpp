#include "main_window.h"

#include "startup.h"
#include "text_utf8.h"

#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifdef _DEBUG
#include <crtdbg.h>
#endif

namespace {
std::filesystem::path failureLogPath;

// std::terminate means a C++ exception left a noexcept boundary (a destructor,
// a window procedure, a thread body). The default abort reports exit code 3
// with no reason, so record it beside the database first.
void logAndAbort()
{
    {
        std::ofstream log(failureLogPath, std::ios::app);
        log << "terminate: ";
        if (const auto error = std::current_exception()) {
            try {
                std::rethrow_exception(error);
            } catch (const std::exception& caught) {
                log << caught.what();
            } catch (...) {
                log << "unknown exception";
            }
        } else {
            log << "no active exception";
        }
        log << '\n';
    }
    std::abort();
}

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

// A folder the process can actually write to, proven by creating and removing a
// file: attributes and ACLs alone lie often enough on a locked-down desktop.
bool writable(const std::filesystem::path& directory, std::error_code& error)
{
    std::filesystem::create_directories(directory, error);
    if (error) return false;
    const auto probe = directory / L".litefetch-write-probe";
    {
        std::ofstream file(probe, std::ios::binary);
        if (!file) return false;
        file << "probe";
        if (!file.good()) return false;
    }
    std::filesystem::remove(probe, error);
    error.clear();
    return true;
}

std::filesystem::path executableDirectory()
{
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const auto length = GetModuleFileNameW(nullptr, buffer.data(),
            static_cast<DWORD>(buffer.size()));
        if (length == 0) return std::filesystem::current_path();
        if (length < buffer.size()) {
            buffer.resize(length);
            return std::filesystem::path(buffer).parent_path();
        }
        buffer.resize(buffer.size() * 2);
    }
}

// The product fills the defaults from the user's known folders; the shared
// parser (and its unit test) owns everything after the executable name.
litefetch::ui::StartupOptions parseArguments(const std::vector<std::wstring>& arguments)
{
    litefetch::ui::AppOptions defaults;
    defaults.databasePath = knownFolder(FOLDERID_LocalAppData) / L"LiteFetch" / L"tasks.db";
    defaults.downloadDirectory = knownFolder(FOLDERID_Downloads);
    if (defaults.downloadDirectory.empty())
        defaults.downloadDirectory = std::filesystem::current_path();
    auto options = litefetch::ui::parseStartupOptions(defaults, arguments);
    // A restricted desktop cannot create the profile folder, and an error box on
    // first launch helps nobody. Fall back to a portable folder beside the
    // executable and keep going; the same rule the launch script uses.
    std::error_code error;
    if (options.error.empty()
        && !writable(options.window.databasePath.parent_path(), error)) {
        const auto portable = executableDirectory() / L"data";
        if (writable(portable, error)) {
            options.window.databasePath = portable / L"tasks.db";
            options.window.downloadDirectory = executableDirectory() / L"downloads";
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
    failureLogPath = options.window.databasePath.parent_path() / L"ui-error.log";
    std::set_terminate(logAndAbort);
#ifdef _DEBUG
    // Route CRT assertions and heap diagnostics into the same file: an abort
    // without a C++ exception has no other visible reason in a GUI process.
    if (FILE* report = _wfopen(failureLogPath.c_str(), L"a")) {
        for (const int mode : {_CRT_WARN, _CRT_ERROR, _CRT_ASSERT}) {
            _CrtSetReportMode(mode, _CRTDBG_MODE_FILE);
            _CrtSetReportFile(mode, report);
        }
    }
    _CrtSetDbgFlag(_CrtSetDbgFlag(_CRTDBG_REPORT_FLAG) | _CRTDBG_ALLOC_MEM_DF | _CRTDBG_DELAY_FREE_MEM_DF);
#endif
    if (!options.error.empty()) {
        MessageBoxW(nullptr, litefetch::utf16FromUtf8(options.error).c_str(), L"lite fetch",
            MB_OK | MB_ICONERROR);
        OleUninitialize();
        return 2;
    }

    try {
        litefetch::ui::MainWindow window(options.window);
        if (window.create(instance, showCommand)) {
            MSG message{};
            while (GetMessageW(&message, nullptr, 0, 0) > 0) {
                if (window.activeDialog() && IsDialogMessageW(window.activeDialog(), &message))
                    continue;
                if (window.accelerators()
                    && TranslateAcceleratorW(window.handle(), window.accelerators(), &message))
                    continue;
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            result = static_cast<int>(message.wParam);
            {
                std::ofstream trace(failureLogPath, std::ios::app);
                trace << "message loop exited: wParam=" << message.wParam << " result=" << result << "\n";
            }
        }
    } catch (const std::exception& error) {
        std::ofstream debug(failureLogPath, std::ios::app);
        debug << "wWinMain exception: " << error.what() << "\n";
        result = 3;
    } catch (...) {
        std::ofstream debug(failureLogPath, std::ios::app);
        debug << "wWinMain unknown exception\n";
        result = 3;
    }
    OleUninitialize();
    return result;
}
