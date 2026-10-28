// In-process harness for the real Win32 main window.
//
// The window is created on the test thread and commands are sent with
// SendMessage, so every check runs after its handler finished: no polling a
// second process, no racing its timers, and a crash in the close path fails
// this test instead of an external script.
#include "main_window.h"

#include "resource.h"
#include "task_service.h"
#include "task_store.h"
#include "test_support.h"

#include <commctrl.h>
#include <ole2.h>

#include <functional>
#include <string>
#include <utility>

using namespace litefetch;
using namespace litefetch::test;
using namespace litefetch::ui;

namespace {

bool pumpUntil(const std::function<bool()>& done, int timeoutMillis)
{
    const ULONGLONG start = GetTickCount64();
    MSG message{};
    while (GetTickCount64() - start < static_cast<ULONGLONG>(timeoutMillis)) {
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) return done();
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (done()) return true;
        Sleep(1);
    }
    return done();
}

struct FindByClass {
    const wchar_t* className = L"";
    HWND found = nullptr;
};

BOOL CALLBACK findChildProc(HWND window, LPARAM param)
{
    auto* state = reinterpret_cast<FindByClass*>(param);
    wchar_t name[64]{};
    if (GetClassNameW(window, name, 64) > 0 && _wcsicmp(name, state->className) == 0) {
        state->found = window;
        return FALSE;
    }
    return TRUE;
}

HWND childOfClass(HWND parent, const wchar_t* className)
{
    FindByClass state{className, nullptr};
    EnumChildWindows(parent, findChildProc, reinterpret_cast<LPARAM>(&state));
    return state.found;
}

std::wstring windowTitle(HWND window)
{
    wchar_t text[128]{};
    GetWindowTextW(window, text, 128);
    return text;
}

std::wstring statusText(HWND status, int part)
{
    wchar_t text[256]{};
    const auto result = static_cast<int>(SendMessageW(status, SB_GETTEXTW, part,
        reinterpret_cast<LPARAM>(text)));
    const int length = result & 0xffff;
    return std::wstring(text, static_cast<std::size_t>(length > 0 ? length : 0));
}

int headerColumnCount(HWND list)
{
    HWND header = childOfClass(list, L"SysHeader32");
    if (!header) return 0;
    return static_cast<int>(SendMessageW(header, HDM_GETITEMCOUNT, 0, 0));
}

int listColumnWidthSum(HWND list)
{
    const int count = headerColumnCount(list);
    int total = 0;
    for (int index = 0; index < count; ++index)
        total += static_cast<int>(SendMessageW(list, LVM_GETCOLUMNWIDTH, index, 0));
    return total;
}

int listClientWidth(HWND list)
{
    RECT client{};
    GetClientRect(list, &client);
    return static_cast<int>(client.right - client.left);
}

// Modal dialogs run their own loop, so a callback timer is the only code that
// runs while they are up: it presses the button that ends the dialog.
void CALLBACK dismissDialogProc(HWND, UINT, UINT_PTR, DWORD)
{
    auto* state = reinterpret_cast<const std::wstring*>(
        reinterpret_cast<UINT_PTR>(GetPropW(GetDesktopWindow(), L"LiteFetchDismiss")));
    if (!state) return;
    EnumWindows([](HWND window, LPARAM param) -> BOOL {
        auto* title = reinterpret_cast<const std::wstring*>(param);
        wchar_t name[32]{};
        wchar_t text[128]{};
        GetClassNameW(window, name, 32);
        GetWindowTextW(window, text, 128);
        if (_wcsicmp(name, L"#32770") == 0 && *title == text) {
            PostMessageW(window, WM_COMMAND, MAKEWPARAM(IDOK, 0),
                reinterpret_cast<LPARAM>(GetDlgItem(window, IDOK)));
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(state));
}

void scheduleDismiss(const std::wstring& title)
{
    static std::wstring pending;
    pending = title;
    SetPropW(GetDesktopWindow(), L"LiteFetchDismiss",
        reinterpret_cast<HANDLE>(reinterpret_cast<UINT_PTR>(&pending)));
    SetTimer(nullptr, 0, 120, dismissDialogProc);
}

void stopDismiss()
{
    KillTimer(nullptr, 0);
    RemovePropW(GetDesktopWindow(), L"LiteFetchDismiss");
}

void runChecks()
{
    TempDir directory;
    AppOptions options;
    options.databasePath = directory.file("tasks.db");
    options.downloadDirectory = directory.path() / "downloads";
    std::filesystem::create_directories(options.downloadDirectory);

    // Seed rows through the same core service the window reads, before the
    // window opens its own worker on the database.
    {
        TaskRepository repository(options.databasePath);
        TaskService service(repository);
        for (const char* url : {"http://example.invalid/one.bin",
                 "http://example.invalid/two.bin", "http://example.invalid/three.bin"}) {
            const auto added = service.execute(AddTaskCommand{
                url, (options.downloadDirectory / std::filesystem::path(url).filename()).string()});
            check(added.ok, "seed task is added");
        }
    }

    const HINSTANCE instance = GetModuleHandleW(nullptr);
    MainWindow window(options);
    check(window.create(instance, SW_SHOWNORMAL), "main window is created");
    const HWND handle = window.handle();
    check(handle != nullptr, "window handle is available");

    check(pumpUntil([&] { return IsWindowVisible(handle) != FALSE; }, 5000), "window becomes visible");
    check(windowTitle(handle) == L"lite fetch", "title is the product name");

    const HWND list = childOfClass(handle, L"SysListView32");
    const HWND toolbar = childOfClass(handle, L"ToolbarWindow32");
    const HWND status = childOfClass(handle, L"msctls_statusbar32");
    check(list != nullptr && toolbar != nullptr && status != nullptr,
        "list, toolbar and status bar exist");
    check(headerColumnCount(list) == 6, "six default columns");
    check(SendMessageW(status, SB_GETPARTS, 0, 0) == 4, "status bar has four parts");
    check(pumpUntil([&] { return SendMessageW(list, LVM_GETITEMCOUNT, 0, 0) == 3; }, 5000),
        "seeded tasks reach the list");

    // Commands are synchronous: when the send returns, the handler has run.
    const HWND searchEdit = GetDlgItem(handle, IDC_SEARCH);
    check(searchEdit != nullptr, "search field is addressable by id");
    SendMessageW(handle, WM_COMMAND, MAKEWPARAM(ID_EDIT_FIND, 0), 0);
    check(IsWindowVisible(searchEdit), "find shows the search field");
    SendMessageW(searchEdit, WM_KEYDOWN, VK_ESCAPE, 0);
    check(!IsWindowVisible(searchEdit), "escape hides the search field");

    SendMessageW(handle, WM_COMMAND, MAKEWPARAM(ID_VIEW_FILTER_FAILED, 0), 0);
    check(SendMessageW(list, LVM_GETITEMCOUNT, 0, 0) == 0, "failed filter hides queued tasks");
    check(statusText(status, 0).find(L"可见: 0 / 3") != std::wstring::npos,
        "status separates visible from total while filtering");
    SendMessageW(handle, WM_COMMAND, MAKEWPARAM(ID_VIEW_FILTER_ALL, 0), 0);
    check(SendMessageW(list, LVM_GETITEMCOUNT, 0, 0) == 3, "all filter restores the queue");
    check(statusText(status, 0).find(L"任务: 3") != std::wstring::npos,
        "status reports the queue total without a filter");

    // Delete asks first and cancelling keeps the records.
    SendMessageW(handle, WM_COMMAND, MAKEWPARAM(ID_EDIT_SELECT_ALL, 0), 0);
    SendMessageW(handle, WM_COMMAND, MAKEWPARAM(ID_TASK_DELETE, 0), 0);
    HWND confirm = FindWindowW(nullptr, L"删除任务");
    check(confirm != nullptr, "delete asks for confirmation");
    PostMessageW(confirm, WM_COMMAND, MAKEWPARAM(IDCANCEL, 0),
        reinterpret_cast<LPARAM>(GetDlgItem(confirm, IDCANCEL)));
    check(pumpUntil([&] { return FindWindowW(nullptr, L"删除任务") == nullptr; }, 3000),
        "confirm dialog closes on cancel");
    check(SendMessageW(list, LVM_GETITEMCOUNT, 0, 0) == 3, "cancelling delete keeps every record");

    // Open folder for a task whose directory is gone shows the real reason and
    // returns once the user acknowledges it.
    std::filesystem::remove_all(options.downloadDirectory);
    scheduleDismiss(L"打开文件夹");
    SendMessageW(handle, WM_COMMAND, MAKEWPARAM(ID_TASK_OPEN_FOLDER, 0), 0);
    stopDismiss();
    check(true, "open folder failure is acknowledged without aborting");

    // Layout at both extremes: the columns always cover the client width.
    check(listColumnWidthSum(list) == listClientWidth(list), "columns fill the default width");
    SetWindowPos(handle, nullptr, 0, 0, 700, 480, SWP_NOMOVE | SWP_NOZORDER);
    check(pumpUntil([&] { return listColumnWidthSum(list) == listClientWidth(list); }, 3000),
        "columns fill a narrow window");
    SetWindowPos(handle, nullptr, 0, 0, 1200, 800, SWP_NOMOVE | SWP_NOZORDER);
    check(pumpUntil([&] { return listColumnWidthSum(list) == listClientWidth(list); }, 3000),
        "columns fill a wide window");

    SendMessageW(handle, WM_COMMAND, MAKEWPARAM(ID_VIEW_TOOLBAR, 0), 0);
    check(!IsWindowVisible(toolbar), "toolbar can be hidden");
    SendMessageW(handle, WM_COMMAND, MAKEWPARAM(ID_VIEW_TOOLBAR, 0), 0);
    check(IsWindowVisible(toolbar), "toolbar can be shown again");

    // Close: the loop must end with WM_QUIT(0). An abort or a stuck loop fails
    // here instead of in a separate acceptance script.
    PostMessageW(handle, WM_CLOSE, 0, 0);
    MSG message{};
    const ULONGLONG start = GetTickCount64();
    bool quit = false;
    while (GetTickCount64() - start < 30000) {
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) { quit = true; break; }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (quit) break;
        Sleep(1);
    }
    check(quit, "closing the window ends the message loop");
    check(message.wParam == 0, "clean close reports exit code 0");
}

} // namespace

int main()
{
    INITCOMMONCONTROLSEX common{};
    common.dwSize = sizeof(common);
    common.dwICC = ICC_WIN95_CLASSES | ICC_BAR_CLASSES | ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&common);
    if (FAILED(OleInitialize(nullptr))) return 1;

    const int result = run("ui-window", [] {
        runChecks();
        return 0;
    });
    OleUninitialize();
    return result;
}
