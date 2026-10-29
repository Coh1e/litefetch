// In-process harness for the real Win32 main window.
//
// The window is created on the test thread and commands are sent with
// SendMessage, so every check runs after its handler finished: no polling a
// second process, no racing its timers, and a crash in the close path fails
// this test instead of an external script.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include "main_window.h"

#include "columns.h"
#include "http_fixture.h"
#include "resource.h"
#include "task_service.h"
#include "task_store.h"
#include "test_support.h"
#include "text_utf8.h"

#include <commctrl.h>
#include <ole2.h>
#include <gdiplus.h>
#include <uiautomation.h>

#include <functional>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

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

std::wstring itemText(HWND list, int row, int column)
{
    wchar_t text[512]{};
    LVITEMW item{};
    item.iSubItem = column;
    item.pszText = text;
    item.cchTextMax = 512;
    SendMessageW(list, LVM_GETITEMTEXTW, static_cast<WPARAM>(row), reinterpret_cast<LPARAM>(&item));
    return text;
}

int rowOf(HWND list, const std::wstring& name)
{
    const int count = static_cast<int>(SendMessageW(list, LVM_GETITEMCOUNT, 0, 0));
    for (int row = 0; row < count; ++row)
        if (itemText(list, row, 0) == name) return row;
    return -1;
}

bool commandEnabled(HWND toolbar, int command)
{
    return SendMessageW(toolbar, TB_ISBUTTONENABLED, static_cast<WPARAM>(command), 0) != 0;
}

bool clipboardUsable()
{
    if (!OpenClipboard(nullptr)) return false;
    CloseClipboard();
    return true;
}

std::wstring clipboardText()
{
    std::wstring text;
    if (!OpenClipboard(nullptr)) return text;
    if (HANDLE data = GetClipboardData(CF_UNICODETEXT)) {
        if (const auto* characters = static_cast<const wchar_t*>(GlobalLock(data))) {
            text = characters;
            GlobalUnlock(data);
        }
    }
    CloseClipboard();
    return text;
}

// The live ListView must show exactly what the layout model computes for its own
// client width and DPI; this is the seam between the Figma pitches and the real
// control. Only valid while the column settings are still the defaults.
void checkColumnGeometry(HWND handle, HWND list, const char* label)
{
    const int client = listClientWidth(list);
    const int dpi = static_cast<int>(GetDpiForWindow(handle));
    const auto expected = columnWidths(defaultColumnSettings(), client, dpi);
    std::string detail = std::string(label) + ": dpi=" + std::to_string(dpi)
        + " client=" + std::to_string(client) + " widths=";
    for (std::size_t index = 0; index < expected.size(); ++index) {
        const int actual = static_cast<int>(SendMessageW(list, LVM_GETCOLUMNWIDTH,
            static_cast<WPARAM>(index), 0));
        detail += std::to_string(actual) + "/" + std::to_string(expected[index]) + " ";
        if (actual != expected[index]) throw std::runtime_error(detail + "(" + label + ")");
    }
    check(static_cast<int>(expected.size()) == headerColumnCount(list),
        "layout model and list agree on the column count");
}

int pngEncoder(CLSID& encoder)
{
    UINT count = 0;
    UINT size = 0;
    if (Gdiplus::GetImageEncodersSize(&count, &size) != Gdiplus::Ok || size == 0) return -1;
    std::vector<BYTE> buffer(size);
    auto* encoders = reinterpret_cast<Gdiplus::ImageCodecInfo*>(buffer.data());
    if (Gdiplus::GetImageEncoders(count, size, encoders) != Gdiplus::Ok) return -1;
    for (UINT index = 0; index < count; ++index) {
        if (wcscmp(encoders[index].MimeType, L"image/png") == 0) {
            encoder = encoders[index].Clsid;
            return static_cast<int>(index);
        }
    }
    return -1;
}

// Evidence screenshots are produced here instead of by an external driver: the
// window is already on this thread, so PrintWindow needs no desktop session and
// steals no foreground. Skipped unless LITEFETCH_EVIDENCE_DIR is set.
void captureWindow(HWND handle, const std::string& name)
{
    const auto directory = environmentValue("LITEFETCH_EVIDENCE_DIR");
    if (directory.empty()) return;
    RECT rect{};
    if (!GetWindowRect(handle, &rect)) return;
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    if (width <= 0 || height <= 0) return;

    const HDC screen = GetDC(nullptr);
    const HDC memory = CreateCompatibleDC(screen);
    const HBITMAP bitmap = CreateCompatibleBitmap(screen, width, height);
    const HGDIOBJ previous = SelectObject(memory, bitmap);
    const BOOL printed = PrintWindow(handle, memory, 2 /* PW_RENDERFULLCONTENT */);
    SelectObject(memory, previous);
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);

    std::error_code error;
    std::filesystem::create_directories(directory, error);
    CLSID encoder{};
    if (printed && pngEncoder(encoder) >= 0) {
        Gdiplus::Bitmap image(bitmap, nullptr);
        const auto path = std::filesystem::path(directory) / (name + ".png");
        std::wstring output = path.wstring();
        image.Save(output.c_str(), &encoder, nullptr);
    }
    DeleteObject(bitmap);
    check(printed != FALSE, "PrintWindow captured the window for evidence");
}


// Semantic probe: exactly the surface a UI Automation client (FlaUI, pywinauto,
// WinAppDriver) sees. Asserting through it keeps the checks off raw messages and
// exercises the accessibility contract ticket #26 must ship anyway.
// Every command a keyboard user needs must be in the accelerator table; a menu
// entry without a key is a dead end for anyone who cannot use the mouse.
void checkAcceleratorCoverage()
{
    const auto table = LoadAcceleratorsW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDR_ACCELERATORS));
    check(table != nullptr, "the window has an accelerator table");
    const int count = CopyAcceleratorTableW(table, nullptr, 0);
    check(count > 0, "the accelerator table is not empty");
    std::vector<ACCEL> entries(static_cast<std::size_t>(count));
    CopyAcceleratorTableW(table, entries.data(), count);

    const auto has = [&](WORD command) {
        for (const auto& entry : entries)
            if (entry.cmd == command) return true;
        return false;
    };
    const auto hasKey = [&](BYTE key, WORD command) {
        for (const auto& entry : entries)
            if (entry.key == key && entry.cmd == command) return true;
        return false;
    };

    check(hasKey('N', ID_FILE_NEW), "Ctrl+N opens a new download");
    check(hasKey('O', ID_FILE_OPEN_TORRENT), "Ctrl+O opens a torrent file");
    check(hasKey('F', ID_EDIT_FIND), "Ctrl+F starts a search");
    check(hasKey('A', ID_EDIT_SELECT_ALL), "Ctrl+A selects every row");
    check(hasKey('C', ID_EDIT_COPY_LINK), "Ctrl+C copies the link");
    check(hasKey(VK_F5, ID_VIEW_REFRESH), "F5 refreshes the list");
    check(hasKey(VK_DELETE, ID_TASK_DELETE), "Delete removes the selected task");
    check(hasKey(VK_F2, ID_TASK_PROPERTIES), "F2 opens the properties");
    for (const WORD command : {ID_FILE_NEW, ID_EDIT_FIND, ID_EDIT_SELECT_ALL,
             ID_EDIT_COPY_LINK, ID_VIEW_REFRESH, ID_TASK_DELETE, ID_TASK_PROPERTIES}) {
        check(has(command), "a menu command is reachable from the keyboard");
    }
}

// High contrast is a system setting this session cannot change, so the check is
// that the window survives being told about it and still shows its rows.
void checkHighContrastNotification(HWND handle, HWND list)
{
    SendMessageW(handle, WM_SETTINGCHANGE, SPI_SETHIGHCONTRAST, 0);
    pumpUntil([] { return false; }, 200);
    check(SendMessageW(list, LVM_GETITEMCOUNT, 0, 0) > 0,
        "the list survives a high contrast notification");
    HIGHCONTRASTW contrast{};
    contrast.cbSize = sizeof(contrast);
    if (SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0)) {
        std::fprintf(stderr, "ui-window: high contrast currently %s\n",
            (contrast.dwFlags & HCF_HIGHCONTRASTON) ? "on" : "off");
    }
}

void checkAutomationSurface(HWND handle, int expectedRows)
{
    IUIAutomation* automation = nullptr;
    HRESULT result = CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&automation));
    check(SUCCEEDED(result) && automation != nullptr, "UI Automation client starts");

    IUIAutomationElement* window = nullptr;
    result = automation->ElementFromHandle(handle, &window);
    check(SUCCEEDED(result) && window != nullptr, "automation reaches the main window");

    VARIANT kind;
    VariantInit(&kind);
    kind.vt = VT_I4;
    kind.lVal = UIA_ListControlTypeId;
    IUIAutomationCondition* isList = nullptr;
    automation->CreatePropertyCondition(UIA_ControlTypePropertyId, kind, &isList);
    IUIAutomationElement* list = nullptr;
    window->FindFirst(TreeScope_Descendants, isList, &list);
    check(list != nullptr, "automation exposes the task list");

    VARIANT itemKind;
    VariantInit(&itemKind);
    itemKind.vt = VT_I4;
    itemKind.lVal = UIA_ListItemControlTypeId;
    IUIAutomationCondition* isItem = nullptr;
    automation->CreatePropertyCondition(UIA_ControlTypePropertyId, itemKind, &isItem);
    IUIAutomationElementArray* rows = nullptr;
    list->FindAll(TreeScope_Descendants, isItem, &rows);
    int rowCount = 0;
    if (rows) rows->get_Length(&rowCount);
    if (rowCount != expectedRows)
        throw std::runtime_error("automation row count is " + std::to_string(rowCount));

    IUIAutomationElement* firstRow = nullptr;
    if (rows) rows->GetElement(0, &firstRow);
    BSTR name = nullptr;
    if (firstRow) firstRow->get_CurrentName(&name);
    check(name != nullptr && SysStringLen(name) > 0, "list items expose a readable name");
    SysFreeString(name);

    // A screen reader needs a selection pattern, not just a raw HWND.
    IUIAutomationSelectionPattern* selection = nullptr;
    list->GetCurrentPatternAs(UIA_SelectionPatternId, IID_PPV_ARGS(&selection));
    check(selection != nullptr, "the list exposes a selection pattern");
    if (selection) selection->Release();

    if (firstRow) firstRow->Release();
    if (rows) rows->Release();
    if (isItem) isItem->Release();
    if (list) list->Release();
    if (isList) isList->Release();
    if (window) window->Release();
    automation->Release();
}

void selectRow(HWND list, int index){
    const int count = static_cast<int>(SendMessageW(list, LVM_GETITEMCOUNT, 0, 0));
    for (int row = 0; row < count; ++row) {
        LVITEMW clear{};
        clear.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
        clear.state = 0;
        SendMessageW(list, LVM_SETITEMSTATE, static_cast<WPARAM>(row), reinterpret_cast<LPARAM>(&clear));
    }
    LVITEMW item{};
    item.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
    item.state = LVIS_SELECTED | LVIS_FOCUSED;
    SendMessageW(list, LVM_SETITEMSTATE, static_cast<WPARAM>(index), reinterpret_cast<LPARAM>(&item));
}

std::vector<unsigned char> readBytes(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return std::vector<unsigned char>(std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>());
}

void runChecks()
{
    TempDir directory;
    AppOptions options;
    options.databasePath = directory.file("tasks.db");
    options.downloadDirectory = directory.path() / "downloads";
    std::filesystem::create_directories(options.downloadDirectory);

    const auto body = payload(96 * 1024);
    HttpFixture fixture(HttpFixture::Options{body});
    const auto destination = options.downloadDirectory / "payload.bin";
    // The slow peer leaves time to pause mid-transfer.
    // Large and slow enough that a pause always lands mid-transfer even when
    // the machine is busy running the rest of the suite.
    const auto slowBody = payload(2 * 1024 * 1024);
    HttpFixture slowFixture(HttpFixture::Options{slowBody, 200, 8 * 1024,
        std::chrono::milliseconds(60)});
    const auto slowDestination = options.downloadDirectory / "slow.bin";

    // Seed rows through the same core service the window reads, before the
    // window opens its own worker on the database.
    {
        TaskRepository repository(options.databasePath);
        TaskService service(repository);
        for (const char* url : {"http://example.invalid/one.bin",
                 "http://example.invalid/two.bin"}) {
            const auto added = service.execute(AddTaskCommand{
                url, (options.downloadDirectory / std::filesystem::path(url).filename()).string()});
            check(added.ok, "seed task is added");
        }
        const auto added = service.execute(AddTaskCommand{fixture.url("/payload.bin"),
            destination.string()});
        check(added.ok, "download task is queued");
        const auto slow = service.execute(AddTaskCommand{slowFixture.url("/slow.bin"),
            slowDestination.string()});
        check(slow.ok, "slow download task is queued");
    }

    const HINSTANCE instance = GetModuleHandleW(nullptr);
    int widthAfterDrag = 0;
    {
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
        check(pumpUntil([&] { return SendMessageW(list, LVM_GETITEMCOUNT, 0, 0) == 4; }, 5000),
            "seeded tasks reach the list");
        checkAcceleratorCoverage();
        checkHighContrastNotification(handle, list);
        checkAutomationSurface(handle, 4);
        checkColumnGeometry(handle, list, "default window");
        captureWindow(handle, "ui-window-default");

        // A queued task: start is available, pause is not, and the link is
        // copied from the same row the user selected.
        const int queuedRow = rowOf(list, L"one.bin");
        check(queuedRow >= 0, "the seeded queued task is visible by name");
        selectRow(list, queuedRow);
        if (clipboardUsable()) {
            SendMessageW(handle, WM_COMMAND, MAKEWPARAM(ID_EDIT_COPY_LINK, 0), 0);
            check(clipboardText() == L"http://example.invalid/one.bin",
                "copy link puts the selected task source on the clipboard");
        } else {
            // A locked desktop can refuse the clipboard to every process; that is
            // an environment limit, not a window defect.
            std::fprintf(stderr, "ui-window: clipboard unavailable, copy link check skipped\\n");
        }
        check(commandEnabled(toolbar, ID_TASK_START), "start is enabled for a queued task");
        check(!commandEnabled(toolbar, ID_TASK_PAUSE), "pause is disabled for a queued task");
        captureWindow(handle, "ui-window-queued");

        // Commands are synchronous: when the send returns, the handler has run.
        const HWND searchEdit = GetDlgItem(handle, IDC_SEARCH);
        check(searchEdit != nullptr, "search field is addressable by id");
        SendMessageW(handle, WM_COMMAND, MAKEWPARAM(ID_EDIT_FIND, 0), 0);
        check(IsWindowVisible(searchEdit), "find shows the search field");
        SendMessageW(searchEdit, WM_KEYDOWN, VK_ESCAPE, 0);
        check(!IsWindowVisible(searchEdit), "escape hides the search field");

        SendMessageW(handle, WM_COMMAND, MAKEWPARAM(ID_VIEW_FILTER_FAILED, 0), 0);
        check(SendMessageW(list, LVM_GETITEMCOUNT, 0, 0) == 0, "failed filter hides queued tasks");
        check(statusText(status, 0).find(L"可见: 0 / 4") != std::wstring::npos,
            "status separates visible from total while filtering");
        SendMessageW(handle, WM_COMMAND, MAKEWPARAM(ID_VIEW_FILTER_ALL, 0), 0);
        check(SendMessageW(list, LVM_GETITEMCOUNT, 0, 0) == 4, "all filter restores the queue");
        check(statusText(status, 0).find(L"任务: 4") != std::wstring::npos,
            "status reports the queue total without a filter");

        // Priority is a task property with no dialog in the way: the menu
        // command is enough to prove the path from menu to stored task.
        {
            auto* window = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(handle, GWLP_USERDATA));
            selectRow(list, rowOf(list, L"payload.bin"));
            SendMessageW(handle, WM_COMMAND, MAKEWPARAM(ID_TASK_PRIORITY_HIGH, 0), 0);
            check(pumpUntil([&] { return window->priorityOfTaskNamed("payload.bin") == 10; }, 5000),
                "raising the priority did not reach the task");
            SendMessageW(handle, WM_COMMAND, MAKEWPARAM(ID_TASK_PRIORITY_NORMAL, 0), 0);
            check(pumpUntil([&] { return window->priorityOfTaskNamed("payload.bin") == 0; }, 5000),
                "resetting the priority did not reach the task");
        }

        // A single selection enables the properties command (FTP connection
        // facts live in that dialog).
        selectRow(list, 0);
        SendMessageW(handle, WM_COMMAND, MAKEWPARAM(ID_VIEW_REFRESH, 0), 0);
        check(pumpUntil([&] {
            const auto state = GetMenuState(GetMenu(handle), ID_TASK_PROPERTIES, MF_BYCOMMAND);
            return state != static_cast<UINT>(-1) && (state & MF_GRAYED) == 0;
        }, 3000), "properties is enabled for a single selection");
        // Start the real transfer from the list and wait for the published file.
        // Select by name: rows added in the same millisecond have no stable order.
        selectRow(list, rowOf(list, L"payload.bin"));
        SendMessageW(handle, WM_COMMAND, MAKEWPARAM(ID_TASK_START, 0), 0);
        const bool published = pumpUntil([&] {
            std::error_code error;
            return std::filesystem::file_size(destination, error) == body.size();
        }, 30000);
        if (!published) {
            // Read each status part once: taking begin() from one temporary and
            // end() from another is undefined behaviour, and it crashed here as
            // 0xc0000409 instead of reporting why the download stalled.
            const int stalledRow = rowOf(list, L"payload.bin");
            std::string detail = "row=" + std::to_string(stalledRow)
                + " text=" + utf8FromUtf16(stalledRow >= 0 ? itemText(list, stalledRow, 5) : std::wstring())
                + " status=" + utf8FromUtf16(statusText(status, 0))
                + " | error=" + utf8FromUtf16(statusText(status, 3))
                + " rows=" + std::to_string(SendMessageW(list, LVM_GETITEMCOUNT, 0, 0));
            throw std::runtime_error("download did not publish: " + detail);
        }
        check(readBytes(destination) == body, "downloaded bytes match the controlled peer");

        const int completedRow = rowOf(list, L"payload.bin");
        check(completedRow >= 0, "the downloaded task is visible by name");
        check(itemText(list, completedRow, 2) == L"100.0%", "completed row reports 100.0% progress");
        // The row catches up on the next refresh tick; wait for it instead of
        // assuming the tick already happened.
        check(pumpUntil([&] { return itemText(list, completedRow, 5) == L"已完成"; }, 5000),
            "completed row reports the completed status");
        selectRow(list, completedRow);
        check(!commandEnabled(toolbar, ID_TASK_START), "start is disabled for a completed task");
        check(!commandEnabled(toolbar, ID_TASK_STOP), "stop is disabled for a completed task");
        captureWindow(handle, "ui-window-completed");

        // Deleting the record must keep the downloaded file.
        SendMessageW(handle, WM_COMMAND, MAKEWPARAM(ID_TASK_DELETE, 0), 0);
        HWND confirm = FindWindowW(nullptr, L"删除任务");
        check(confirm != nullptr, "delete asks for confirmation");
        PostMessageW(confirm, WM_COMMAND, MAKEWPARAM(IDOK, 0),
            reinterpret_cast<LPARAM>(GetDlgItem(confirm, IDOK)));
        const bool deleted = pumpUntil([&] {
            return SendMessageW(list, LVM_GETITEMCOUNT, 0, 0) == 3
                && FindWindowW(nullptr, L"删除任务") == nullptr;
        }, 10000);
        if (!deleted) {
            std::wstring detail = L"count=" + std::to_wstring(SendMessageW(list, LVM_GETITEMCOUNT, 0, 0));
            if (FindWindowW(nullptr, L"删除任务记录失败") != nullptr) detail += L" error-dialog";
            if (FindWindowW(nullptr, L"删除任务") != nullptr) detail += L" confirm-open";
            throw std::runtime_error(std::string(detail.begin(), detail.end()));
        }
        check(std::filesystem::exists(destination), "deleting the record keeps the file");

        // Pause keeps the partial bytes, resume continues them and publishes
        // the complete file. The slow peer leaves time to pause mid-transfer.
        selectRow(list, rowOf(list, L"slow.bin"));
        SendMessageW(handle, WM_COMMAND, MAKEWPARAM(ID_TASK_START, 0), 0);
        const auto slowPartial = partialPathFor(slowDestination);
        check(pumpUntil([&] {
            std::error_code error;
            return std::filesystem::file_size(slowPartial, error) > 0;
        }, 15000), "slow download starts writing partial data");
        // Pause only once the row reports a running transfer: a pause sent while
        // the task is still queued is a no-op, and the transfer would finish.
        const int slowRow = rowOf(list, L"slow.bin");
        check(slowRow >= 0, "the slow task is listed");
        check(pumpUntil([&] {
            const auto status = itemText(list, slowRow, 5);
            return status == L"下载中" || status == L"正在连接";
        }, 15000), "the slow task reports a running transfer before the pause");
        SendMessageW(handle, WM_COMMAND, MAKEWPARAM(ID_TASK_PAUSE, 0), 0);
        // The invariant is the state, not the file size: the partial is written
        // by another thread and sampling it raced with the write and the message
        // loop. "Keeps the partial data" is covered by the core resume tests.
        {
            auto* window = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(handle, GWLP_USERDATA));
            check(pumpUntil([&] { return window->statusOfTaskNamed("slow.bin") == "已暂停"; }, 20000),
                "pause did not reach the paused state");
            pumpUntil([] { return false; }, 1000);
            check(window->statusOfTaskNamed("slow.bin") == "已暂停",
                "the paused task did not stay paused");
        }
        check(!std::filesystem::exists(slowDestination), "paused transfer is not published");
        SendMessageW(handle, WM_COMMAND, MAKEWPARAM(ID_TASK_START, 0), 0);
        check(pumpUntil([&] { return std::filesystem::exists(slowDestination); }, 30000),
            "resumed transfer publishes the file");
        check(readBytes(slowDestination) == slowBody, "resumed file is byte-identical to the peer");

        // The download directory is gone from here on: later layout checks must
        // not depend on it. The modal "open folder" message box is deliberately
        // not driven here; a modal loop would block this harness, and the text
        // itself is covered by the ticket-4 acceptance evidence.
        std::filesystem::remove_all(options.downloadDirectory);

        // Layout at both extremes: the columns always cover the client width.
        check(listColumnWidthSum(list) == listClientWidth(list), "columns fill the default width");
        checkColumnGeometry(handle, list, "restored default window");
        SetWindowPos(handle, nullptr, 0, 0, 700, 480, SWP_NOMOVE | SWP_NOZORDER);
        check(pumpUntil([&] { return listColumnWidthSum(list) == listClientWidth(list); }, 3000),
            "columns fill a narrow window");
        checkColumnGeometry(handle, list, "narrow window");
        SetWindowPos(handle, nullptr, 0, 0, 1200, 800, SWP_NOMOVE | SWP_NOZORDER);
        check(pumpUntil([&] { return listColumnWidthSum(list) == listClientWidth(list); }, 3000),
            "columns fill a wide window");
        checkColumnGeometry(handle, list, "wide window");

        // A header drag is the only way the window saves a column width.
        const HWND header = childOfClass(list, L"SysHeader32");
        check(header != nullptr, "list header exists");
        const int beforeDrag = static_cast<int>(SendMessageW(list, LVM_GETCOLUMNWIDTH, 0, 0));
        RECT headerRect{};
        GetClientRect(header, &headerRect);
        const int divider = beforeDrag;
        const int middle = (headerRect.bottom - headerRect.top) / 2;
        SendMessageW(header, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(divider, middle));
        SendMessageW(header, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(divider + 40, middle));
        SendMessageW(header, WM_LBUTTONUP, 0, MAKELPARAM(divider + 40, middle));
        check(std::filesystem::exists(uiStatePathFor(options.databasePath)),
            "column settings are saved beside the database");
        widthAfterDrag = static_cast<int>(SendMessageW(list, LVM_GETCOLUMNWIDTH, 0, 0));
        check(widthAfterDrag > beforeDrag, "dragging the divider widens the name column");

        // The drag is a user pitch; a resize re-applies it to the layout.
        SetWindowPos(handle, nullptr, 0, 0, 900, 620, SWP_NOMOVE | SWP_NOZORDER);
        check(pumpUntil([&] { return listColumnWidthSum(list) == listClientWidth(list); }, 3000),
            "columns fill the width after a drag and resize");

        SendMessageW(handle, WM_COMMAND, MAKEWPARAM(ID_VIEW_TOOLBAR, 0), 0);
        check(!IsWindowVisible(toolbar), "toolbar can be hidden");
        SendMessageW(handle, WM_COMMAND, MAKEWPARAM(ID_VIEW_TOOLBAR, 0), 0);
        check(IsWindowVisible(toolbar), "toolbar can be shown again");

        {
            // The notification icon keeps LiteFetch reachable while the window is
            // minimised. The shell may be absent in an automation session, so the
            // icon install is only reported, while the restore path behind the
            // same message is checked for real.
            auto* window = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(handle, GWLP_USERDATA));
            check(window != nullptr, "the window object is reachable from the HWND");
            if (!window->trayIconVisible())
                std::fprintf(stderr,
                    "ui-window: notification area unavailable in this session, tray install check skipped\n");
            ShowWindow(handle, SW_MINIMIZE);
            pumpUntil([] { return false; }, 100);
            PostMessageW(handle, WM_APP + 5, 0, WM_LBUTTONDBLCLK);
            check(pumpUntil([&] { return IsWindowVisible(handle) != FALSE; }, 3000),
                "the tray double click restores the window");
            ShowWindow(handle, SW_RESTORE);
            pumpUntil([] { return false; }, 100);
        }

        // Close: the loop must end with WM_QUIT(0). An abort or a stuck loop
        // fails here instead of in a separate acceptance script.
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

    // Reopening the window restores the width saved by the drag.
    {
        MainWindow reopened(options);
        check(reopened.create(instance, SW_SHOWNORMAL), "window reopens on the same database");
        const HWND handle = reopened.handle();
        const HWND list = childOfClass(handle, L"SysListView32");
        check(pumpUntil([&] { return SendMessageW(list, LVM_GETITEMCOUNT, 0, 0) == 3; }, 5000),
            "records survive the restart");
        const int restored = static_cast<int>(SendMessageW(list, LVM_GETCOLUMNWIDTH, 0, 0));
        check(restored > 0 && restored != widthAfterDrag, "restored width is recomputed for the layout");
        check(listColumnWidthSum(list) == listClientWidth(list),
            "restored columns fill the list client width");
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
        check(quit, "second window closes cleanly");
    }
}

} // namespace

namespace {
// A test that aborts prints nothing, so route the CRT and terminate reports to
// a file that survives the crash (the per-run temp directory does not).
std::filesystem::path crashLogPath()
{
    const auto root = environmentValue("LITEFETCH_TEST_TEMP");
    if (root.empty()) return {};
    return std::filesystem::path(root) / "ui-window-crash.log";
}

void logCrash(const char* text)
{
    const auto path = crashLogPath();
    if (path.empty()) return;
    std::ofstream log(path, std::ios::app);
    log << text << "\n";
}

void onTerminate()
{
    logCrash("terminate: unhandled C++ exception in the test process");
    std::abort();
}

void onInvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, uintptr_t)
{
    logCrash("invalid parameter passed to the CRT");
    std::abort();
}
} // namespace

int main()
{
    std::set_terminate(onTerminate);
    _set_invalid_parameter_handler(onInvalidParameter);
#ifdef _DEBUG
    if (const auto path = crashLogPath(); !path.empty()) {
        if (FILE* report = _wfopen(path.c_str(), L"a")) {
            for (const int mode : {_CRT_WARN, _CRT_ERROR, _CRT_ASSERT}) {
                _CrtSetReportMode(mode, _CRTDBG_MODE_FILE);
                _CrtSetReportFile(mode, report);
            }
        }
    }
    _CrtSetDbgFlag(_CrtSetDbgFlag(_CRTDBG_REPORT_FLAG) | _CRTDBG_ALLOC_MEM_DF | _CRTDBG_DELAY_FREE_MEM_DF);
#endif
    INITCOMMONCONTROLSEX common{};
    common.dwSize = sizeof(common);
    common.dwICC = ICC_WIN95_CLASSES | ICC_BAR_CLASSES | ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&common);
    if (FAILED(OleInitialize(nullptr))) return 1;
    Gdiplus::GdiplusStartupInput gdiplusInput;
    ULONG_PTR gdiplusToken = 0;
    if (Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusInput, nullptr) != Gdiplus::Ok) return 1;

    const int result = run("ui-window", [] {
        runChecks();
        return 0;
    });
    Gdiplus::GdiplusShutdown(gdiplusToken);
    OleUninitialize();
    return result;
}