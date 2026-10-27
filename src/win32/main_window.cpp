#include "main_window.h"

#include "new_download_dialog.h"
#include "resource.h"
#include "task_worker.h"
#include "text_utf8.h"

#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <system_error>

namespace litefetch::ui {
namespace {
constexpr wchar_t windowClass[] = L"LiteFetchMainWindow";
constexpr int refreshTimerMillis = 500;
constexpr int shutdownTimerMillis = 100;
constexpr wchar_t title[] = L"lite fetch";

std::wstring toWide(std::string_view utf8, const wchar_t* fallback = L"")
{
    try {
        return utf16FromUtf8(utf8);
    } catch (const std::exception&) {
        return fallback;
    }
}

struct ToolbarButton {
    UINT id = 0;
    const wchar_t* text = L"";
    int icon = 0;
    bool separator = false;
    int activeIcon = 0;
};

const std::array<ToolbarButton, 10> toolbarButtons{{
    {ID_FILE_NEW, L"新建下载", IDI_NEW, false, 0},
    {0, L"", 0, true, 0},
    {ID_TASK_START, L"开始/继续", IDI_START, false, IDI_START_ACTIVE},
    {ID_TASK_PAUSE, L"暂停", IDI_PAUSE, false, 0},
    {ID_TASK_STOP, L"停止", IDI_STOP, false, 0},
    {ID_TASK_DELETE, L"删除", IDI_DELETE, false, 0},
    {0, L"", 0, true, 0},
    {ID_TASK_OPEN_FOLDER, L"打开文件夹", IDI_FOLDER, false, 0},
    {0, L"", 0, true, 0},
    {ID_TOOLS_OPTIONS, L"选项", IDI_SETTINGS, false, 0},
}};

struct ColumnSpec {
    const wchar_t* title;
    int width;
};

const std::array<ColumnSpec, 6> columns{{
    {L"名称", 331},
    {L"大小", 93},
    {L"进度", 71},
    {L"下载速度", 115},
    {L"剩余时间", 97},
    {L"状态", 110},
}};

int scale(int value, int dpi)
{
    return MulDiv(value, dpi, 96);
}

std::wstring statusBarText(const std::string& utf8)
{
    return toWide(utf8);
}
} // namespace

MainWindow::MainWindow(AppOptions options) : options_(std::move(options)) {}

MainWindow::~MainWindow()
{
    if (worker_) worker_->shutdown();
    if (font_) DeleteObject(font_);
    if (largeIcon_) DestroyIcon(largeIcon_);
    if (smallIcon_) DestroyIcon(smallIcon_);
    if (accelerators_) DestroyAcceleratorTable(accelerators_);
}

bool MainWindow::create(HINSTANCE instance, int showCommand)
{
    instance_ = instance;
    WNDCLASSEXW windowClassInfo{};
    windowClassInfo.cbSize = sizeof(windowClassInfo);
    windowClassInfo.style = CS_HREDRAW | CS_VREDRAW;
    windowClassInfo.lpfnWndProc = windowProcStatic;
    windowClassInfo.hInstance = instance_;
    windowClassInfo.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClassInfo.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    windowClassInfo.lpszClassName = windowClass;
    largeIcon_ = static_cast<HICON>(LoadImageW(instance_, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
        scale(32, 96), scale(32, 96), LR_DEFAULTCOLOR));
    smallIcon_ = static_cast<HICON>(LoadImageW(instance_, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
        scale(16, 96), scale(16, 96), LR_DEFAULTCOLOR));
    windowClassInfo.hIcon = largeIcon_;
    windowClassInfo.hIconSm = smallIcon_;
    if (!RegisterClassExW(&windowClassInfo)) return false;

    menu_ = LoadMenuW(instance_, MAKEINTRESOURCEW(IDR_MAIN_MENU));
    accelerators_ = LoadAcceleratorsW(instance_, MAKEINTRESOURCEW(IDR_ACCELERATORS));

    RECT bounds{0, 0, scale(1000, 96), scale(650, 96)};
    AdjustWindowRectEx(&bounds, WS_OVERLAPPEDWINDOW, TRUE, 0);
    hwnd_ = CreateWindowExW(0, windowClass, title,
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
        bounds.right - bounds.left, bounds.bottom - bounds.top, nullptr, menu_, instance_, this);
    if (!hwnd_) return false;
    ShowWindow(hwnd_, showCommand);
    UpdateWindow(hwnd_);
    return true;
}

LRESULT CALLBACK MainWindow::windowProcStatic(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    MainWindow* self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        auto* createStruct = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<MainWindow*>(createStruct->lpCreateParams);
        self->hwnd_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        return DefWindowProcW(window, message, wParam, lParam);
    }
    if (!self) return DefWindowProcW(window, message, wParam, lParam);
    return self->handleMessage(message, wParam, lParam);
}

LRESULT MainWindow::handleMessage(UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_CREATE:
        if (!createControls()) return -1;
        return 0;
    case WM_SIZE:
        layoutControls();
        return 0;
    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
        info->ptMinTrackSize.x = scale(640, dpi_);
        info->ptMinTrackSize.y = scale(400, dpi_);
        return 0;
    }
    case WM_DPICHANGED: {
        dpi_ = HIWORD(wParam);
        const auto* suggested = reinterpret_cast<RECT*>(lParam);
        SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top,
            suggested->right - suggested->left, suggested->bottom - suggested->top,
            SWP_NOZORDER | SWP_NOACTIVATE);
        createFont();
        applyFont();
        updateLayoutMetrics();
        layoutControls();
        return 0;
    }
    case WM_TIMER:
        if (wParam == IDT_REFRESH) {
            refreshFromWorker();
            return 0;
        }
        if (wParam == IDT_SHUTDOWN) {
            if (!worker_ || worker_->shutdownComplete()) {
                finishClose();
            } else {
                setShutdownStatus();
            }
            return 0;
        }
        break;
    case workerWakeMessage:
        refreshFromWorker();
        return 0;
    case WM_NOTIFY: {
        auto* header = reinterpret_cast<NMHDR*>(lParam);
        if (header->hwndFrom == tooltip_ && header->code == TTN_GETDISPINFOW) {
            updateTooltip(*reinterpret_cast<NMTTDISPINFOW*>(lParam));
            return 0;
        }
        if (header->hwndFrom == list_ && header->code == LVN_ITEMCHANGED) {
            auto* change = reinterpret_cast<NMLISTVIEW*>(lParam);
            const bool becameSelected = (change->uNewState & LVIS_SELECTED)
                && !(change->uOldState & LVIS_SELECTED);
            if (becameSelected && change->iItem >= 0
                && static_cast<std::size_t>(change->iItem) < model_.rows().size()) {
                selectedId_ = model_.rows()[static_cast<std::size_t>(change->iItem)].id;
                updateActions();
            }
            return 0;
        }
        if (header->hwndFrom == list_ && header->code == LVN_KEYDOWN) {
            const auto* key = reinterpret_cast<NMLVKEYDOWN*>(lParam);
            if (key->wVKey == VK_RETURN) {
                startSelected();
                return 0;
            }
        }
        break;
    }
    case WM_INITMENUPOPUP:
        updateActions();
        return 0;
    case WM_COMMAND: {
        const int id = LOWORD(wParam);
        if (isClosing() && id != ID_FILE_EXIT) return 0;
        switch (id) {
        case ID_FILE_NEW: openNewDownload(); return 0;
        case ID_FILE_EXIT: PostMessageW(hwnd_, WM_CLOSE, 0, 0); return 0;
        case ID_EDIT_COPY_LINK: copyLinkSelected(); return 0;
        case ID_VIEW_REFRESH:
            if (worker_) worker_->postRefresh();
            return 0;
        case ID_TASK_START: startSelected(); return 0;
        case ID_TASK_STOP: stopSelected(); return 0;
        case ID_TASK_OPEN_FOLDER: openFolderSelected(); return 0;
        case ID_TASK_PAUSE:
        case ID_TASK_DELETE:
        case ID_TOOLS_OPTIONS:
            return 0;
        case ID_HELP_CAPABILITIES: showCapabilities(); return 0;
        case ID_HELP_ABOUT: showAbout(); return 0;
        default:
            break;
        }
        break;
    }
    case WM_CLOSE:
        beginClose();
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd_, IDT_REFRESH);
        KillTimer(hwnd_, IDT_SHUTDOWN);
        if (worker_) worker_->shutdown();
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd_, message, wParam, lParam);
}

bool MainWindow::createControls()
{
    dpi_ = GetDpiForWindow(hwnd_);
    if (dpi_ <= 0) dpi_ = 96;
    createFont();
    createToolbar();
    createList();
    createStatusBar();
    createTooltip();
    applyFont();
    updateLayoutMetrics();
    layoutControls();

    worker_ = std::make_unique<TaskWorker>(options_.databasePath, hwnd_);
    std::string error;
    if (!worker_->start(error)) {
        MessageBoxW(hwnd_, toWide("无法打开任务数据库: " + error).c_str(), L"lite fetch",
            MB_OK | MB_ICONERROR);
        return false;
    }
    refreshFromWorker();
    SetTimer(hwnd_, IDT_REFRESH, refreshTimerMillis, nullptr);
    return true;
}

void MainWindow::createFont()
{
    if (font_) {
        DeleteObject(font_);
        font_ = nullptr;
    }
    NONCLIENTMETRICSW metrics{};
    metrics.cbSize = sizeof(metrics);
    if (!SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0)) return;
    font_ = CreateFontIndirectW(&metrics.lfMessageFont);
}

void MainWindow::createToolbar()
{
    toolbar_ = CreateWindowExW(0, TOOLBARCLASSNAMEW, nullptr,
        WS_CHILD | WS_VISIBLE | TBSTYLE_FLAT | TBSTYLE_LIST | CCS_NODIVIDER | CCS_NOMOVEY | CCS_TOP,
        0, 0, 0, 0, hwnd_, nullptr, instance_, nullptr);
    if (!toolbar_) return;
    SendMessageW(toolbar_, TB_BUTTONSTRUCTSIZE, static_cast<WPARAM>(sizeof(TBBUTTON)), 0);
    SendMessageW(toolbar_, TB_SETEXTENDEDSTYLE, 0, TBSTYLE_EX_MIXEDBUTTONS);
    SendMessageW(toolbar_, TB_SETMAXTEXTROWS, 1, 0);
    SendMessageW(toolbar_, TB_SETPADDING, 0, MAKELPARAM(scale(6, dpi_), scale(2, dpi_)));

    const int iconSize = scale(16, dpi_);
    HIMAGELIST images = ImageList_Create(iconSize, iconSize, ILC_COLOR32 | ILC_MASK, 8, 0);
    std::vector<TBBUTTON> buttons;
    buttons.reserve(toolbarButtons.size());
    for (const auto& spec : toolbarButtons) {
        TBBUTTON button{};
        if (spec.separator) {
            button.fsStyle = BTNS_SEP;
            button.iBitmap = std::max(1, iconSize / 2);
            button.fsState = TBSTATE_ENABLED;
        } else {
            HICON icon = static_cast<HICON>(LoadImageW(instance_, MAKEINTRESOURCEW(spec.icon),
                IMAGE_ICON, iconSize, iconSize, LR_DEFAULTCOLOR));
            const int imageIndex = icon ? ImageList_AddIcon(images, icon) : -1;
            if (icon) DestroyIcon(icon);
            if (spec.id == ID_TASK_START) startImageDisabled_ = imageIndex;
            if (spec.activeIcon) {
                HICON activeIcon = static_cast<HICON>(LoadImageW(instance_,
                    MAKEINTRESOURCEW(spec.activeIcon), IMAGE_ICON, iconSize, iconSize, LR_DEFAULTCOLOR));
                const int activeIndex = activeIcon ? ImageList_AddIcon(images, activeIcon) : -1;
                if (activeIcon) DestroyIcon(activeIcon);
                if (spec.id == ID_TASK_START) startImageEnabled_ = activeIndex;
            }
            const LRESULT stringIndex = SendMessageW(toolbar_, TB_ADDSTRINGW, 0,
                reinterpret_cast<LPARAM>(spec.text));
            button.iBitmap = imageIndex;
            button.idCommand = spec.id;
            button.fsState = TBSTATE_ENABLED;
            button.fsStyle = BTNS_BUTTON | BTNS_AUTOSIZE | BTNS_SHOWTEXT;
            button.iString = stringIndex;
        }
        buttons.push_back(button);
    }
    SendMessageW(toolbar_, TB_SETIMAGELIST, 0, reinterpret_cast<LPARAM>(images));
    SendMessageW(toolbar_, TB_ADDBUTTONSW, static_cast<WPARAM>(buttons.size()),
        reinterpret_cast<LPARAM>(buttons.data()));
    SendMessageW(toolbar_, TB_AUTOSIZE, 0, 0);
    updateActions();
}

void MainWindow::createList()
{
    list_ = CreateWindowExW(0, WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | LVS_REPORT | LVS_SINGLESEL
            | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER,
        0, 0, 0, 0, hwnd_, nullptr, instance_, nullptr);
    if (!list_) return;
    ListView_SetExtendedListViewStyle(list_,
        LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
    ListView_SetBkColor(list_, GetSysColor(COLOR_WINDOW));
    ListView_SetTextBkColor(list_, GetSysColor(COLOR_WINDOW));
    ListView_SetTextColor(list_, GetSysColor(COLOR_WINDOWTEXT));

    const int iconSize = scale(16, dpi_);
    HIMAGELIST rows = ImageList_Create(iconSize, iconSize, ILC_COLOR24 | ILC_MASK, 1, 1);
    if (rows) {
        // The small image list exists only to give rows the reference height;
        // its single image must be fully transparent. A color-keyed mask keeps
        // it invisible on both the normal and the selected row background.
        HDC screen = GetDC(nullptr);
        HDC memory = CreateCompatibleDC(screen);
        HBITMAP color = CreateCompatibleBitmap(screen, iconSize, iconSize);
        HBITMAP mask = CreateBitmap(iconSize, iconSize, 1, 1, nullptr);
        if (color && mask) {
            HGDIOBJ previous = SelectObject(memory, color);
            RECT area{0, 0, iconSize, iconSize};
            FillRect(memory, &area, GetSysColorBrush(COLOR_WINDOW));
            SelectObject(memory, mask);
            FillRect(memory, &area, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
            SelectObject(memory, previous);
            ImageList_Add(rows, color, mask);
        }
        if (color) DeleteObject(color);
        if (mask) DeleteObject(mask);
        if (memory) DeleteDC(memory);
        ReleaseDC(nullptr, screen);
        ListView_SetImageList(list_, rows, LVSIL_SMALL);
    }

    for (std::size_t index = 0; index < columns.size(); ++index) {
        LVCOLUMNW column{};
        column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM | LVCF_FMT;
        column.fmt = LVCFMT_LEFT;
        column.cx = scale(columns[index].width, dpi_);
        column.iSubItem = static_cast<int>(index);
        column.pszText = const_cast<LPWSTR>(columns[index].title);
        ListView_InsertColumn(list_, static_cast<int>(index), &column);
    }
}

void MainWindow::createStatusBar()
{
    status_ = CreateWindowExW(0, STATUSCLASSNAMEW, L"",
        WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP, 0, 0, 0, 0, hwnd_, nullptr, instance_, nullptr);
    if (!status_) return;
    const int first = scale(280, dpi_);
    const int second = first + scale(190, dpi_);
    const int third = second + scale(260, dpi_);
    const int parts[4] = {first, second, third, -1};
    SendMessageW(status_, SB_SETPARTS, 4, reinterpret_cast<LPARAM>(parts));
    updateStatus();
}

void MainWindow::createTooltip()
{
    tooltip_ = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
        WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, CW_USEDEFAULT, CW_USEDEFAULT,
        CW_USEDEFAULT, CW_USEDEFAULT, hwnd_, nullptr, instance_, nullptr);
    if (!tooltip_ || !toolbar_) return;
    TOOLINFOW tool{};
    tool.cbSize = sizeof(tool);
    tool.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    tool.hwnd = hwnd_;
    tool.uId = reinterpret_cast<UINT_PTR>(toolbar_);
    tool.lpszText = const_cast<LPWSTR>(L"");
    SendMessageW(tooltip_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
    SendMessageW(tooltip_, TTM_SETMAXTIPWIDTH, 0, scale(360, dpi_));
}

void MainWindow::applyFont()
{
    if (!font_) return;
    SendMessageW(hwnd_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    if (list_) {
        SendMessageW(list_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        HWND header = ListView_GetHeader(list_);
        if (header) SendMessageW(header, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    }
}

void MainWindow::updateLayoutMetrics()
{
    if (toolbar_) {
        SendMessageW(toolbar_, TB_AUTOSIZE, 0, 0);
        RECT rect{};
        GetWindowRect(toolbar_, &rect);
        toolbarHeight_ = rect.bottom - rect.top;
    }
}

void MainWindow::layoutControls()
{
    if (!hwnd_) return;
    RECT client{};
    GetClientRect(hwnd_, &client);
    updateLayoutMetrics();
    int statusHeight = 0;
    if (status_) {
        SendMessageW(status_, WM_SIZE, 0, 0);
        RECT rect{};
        GetWindowRect(status_, &rect);
        statusHeight = rect.bottom - rect.top;
    }
    if (toolbar_) MoveWindow(toolbar_, 0, 0, client.right, toolbarHeight_, TRUE);
    if (list_) {
        const int top = toolbarHeight_;
        const int height = std::max(0, static_cast<int>(client.bottom) - top - statusHeight);
        MoveWindow(list_, 0, top, client.right, height, TRUE);
    }
}

void MainWindow::refreshFromWorker()
{
    if (!worker_) return;
    const auto snapshot = worker_->snapshot();
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    model_.update(snapshot.tasks, snapshot.active, now);
    if (snapshot.lastErrorSequence != lastErrorSequence_) {
        lastErrorSequence_ = snapshot.lastErrorSequence;
        lastErrorText_ = snapshot.lastError;
        lastErrorAt_ = std::chrono::steady_clock::now();
    }
    syncList();
    updateStatus();
    updateActions();
}

void MainWindow::syncList()
{
    if (!list_) return;
    const auto& rows = model_.rows();
    bool rebuild = displayedIds_.size() != rows.size();
    if (!rebuild) {
        for (std::size_t index = 0; index < rows.size(); ++index) {
            if (displayedIds_[index] != rows[index].id) {
                rebuild = true;
                break;
            }
        }
    }
    if (rebuild) {
        SendMessageW(list_, WM_SETREDRAW, FALSE, 0);
        ListView_DeleteAllItems(list_);
        for (std::size_t index = 0; index < rows.size(); ++index) {
            const auto& row = rows[index];
            const auto name = toWide(row.name);
            LVITEMW item{};
            item.mask = LVIF_TEXT;
            item.iItem = static_cast<int>(index);
            item.iSubItem = 0;
            item.pszText = const_cast<LPWSTR>(name.c_str());
            ListView_InsertItem(list_, &item);
            const std::array<std::wstring, 5> texts{toWide(row.size), toWide(row.progress),
                toWide(row.speed), toWide(row.remaining), toWide(row.status)};
            for (std::size_t column = 0; column < texts.size(); ++column) {
                ListView_SetItemText(list_, static_cast<int>(index), static_cast<int>(column) + 1,
                    const_cast<LPWSTR>(texts[column].c_str()));
            }
        }
        displayedIds_.clear();
        displayedIds_.reserve(rows.size());
        for (const auto& row : rows) displayedIds_.push_back(row.id);
        SendMessageW(list_, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(list_, nullptr, TRUE);

        const auto selected = model_.indexOf(selectedId_);
        if (selected >= rows.size()) selectedId_.clear();
        ensureSelection();
    } else {
        for (std::size_t index = 0; index < rows.size(); ++index) {
            const auto& row = rows[index];
            const std::array<std::string, 6> texts{row.name, row.size, row.progress, row.speed,
                row.remaining, row.status};
            for (std::size_t column = 0; column < texts.size(); ++column) {
                wchar_t existing[512]{};
                ListView_GetItemText(list_, static_cast<int>(index), static_cast<int>(column),
                    existing, static_cast<int>(std::size(existing)));
                const auto wanted = toWide(texts[column]);
                if (wanted != existing) {
                    if (column == 0) {
                        LVITEMW item{};
                        item.mask = LVIF_TEXT;
                        item.iItem = static_cast<int>(index);
                        item.iSubItem = 0;
                        item.pszText = const_cast<LPWSTR>(wanted.c_str());
                        ListView_SetItem(list_, &item);
                    } else {
                        ListView_SetItemText(list_, static_cast<int>(index),
                            static_cast<int>(column), const_cast<LPWSTR>(wanted.c_str()));
                    }
                }
            }
        }
    }
}

void MainWindow::ensureSelection()
{
    const auto& rows = model_.rows();
    if (rows.empty()) {
        selectedId_.clear();
        return;
    }
    auto index = model_.indexOf(selectedId_);
    if (index >= rows.size()) index = 0;
    selectRow(index);
}

void MainWindow::selectRow(std::size_t index)
{
    const auto& rows = model_.rows();
    if (!list_ || index >= rows.size()) return;
    selectedId_ = rows[index].id;
    ListView_SetItemState(list_, static_cast<int>(index),
        LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    ListView_EnsureVisible(list_, static_cast<int>(index), FALSE);
    updateActions();
}

int MainWindow::selectedIndex() const
{
    if (selectedId_.empty()) return -1;
    const auto index = model_.indexOf(selectedId_);
    return index < model_.rows().size() ? static_cast<int>(index) : -1;
}

const TaskRow* MainWindow::selectedRow() const
{
    const auto index = selectedIndex();
    if (index < 0) return nullptr;
    return &model_.rows()[static_cast<std::size_t>(index)];
}

void MainWindow::updateStatus()
{
    if (!status_) return;
    const auto texts = model_.status();
    std::array<std::wstring, 4> parts{statusBarText(texts.tasks), statusBarText(texts.active),
        statusBarText(texts.download), statusBarText(texts.upload)};
    const bool errorVisible = !lastErrorText_.empty()
        && std::chrono::steady_clock::now() - lastErrorAt_ < std::chrono::seconds(10);
    if (errorVisible) parts[3] = statusBarText(lastErrorText_);
    if (isClosing()) parts[3] = L"正在停止任务…";
    for (int index = 0; index < 4; ++index) {
        SendMessageW(status_, SB_SETTEXTW, static_cast<WPARAM>(index),
            reinterpret_cast<LPARAM>(parts[static_cast<std::size_t>(index)].c_str()));
    }
}

void MainWindow::setShutdownStatus()
{
    if (status_) SendMessageW(status_, SB_SETTEXTW, 3, reinterpret_cast<LPARAM>(L"正在停止任务…"));
}

void MainWindow::updateActions()
{
    const auto actions = model_.actionsFor(selectedId_);
    auto enableMenu = [this](UINT id, bool enabled) {
        if (menu_) EnableMenuItem(menu_, id, MF_BYCOMMAND | (enabled ? MF_ENABLED : MF_GRAYED));
    };
    auto enableButton = [this](UINT id, bool enabled) {
        if (toolbar_) SendMessageW(toolbar_, TB_ENABLEBUTTON, id, MAKELONG(enabled ? TRUE : FALSE, 0));
    };
    enableMenu(ID_FILE_NEW, !isClosing());
    enableMenu(ID_EDIT_COPY_LINK, actions.copyLink);
    enableMenu(ID_VIEW_REFRESH, !isClosing());
    enableMenu(ID_TASK_START, actions.start);
    enableMenu(ID_TASK_PAUSE, actions.pause);
    enableMenu(ID_TASK_STOP, actions.stop);
    enableMenu(ID_TASK_DELETE, actions.deleteTask);
    enableMenu(ID_TASK_OPEN_FOLDER, actions.openFolder);
    enableMenu(ID_TOOLS_OPTIONS, actions.options);
    enableButton(ID_FILE_NEW, !isClosing());
    enableButton(ID_TASK_START, actions.start);
    enableButton(ID_TASK_PAUSE, actions.pause);
    enableButton(ID_TASK_STOP, actions.stop);
    enableButton(ID_TASK_DELETE, actions.deleteTask);
    enableButton(ID_TASK_OPEN_FOLDER, actions.openFolder);
    enableButton(ID_TOOLS_OPTIONS, actions.options);
    if (toolbar_ && startImageEnabled_ >= 0) {
        SendMessageW(toolbar_, TB_CHANGEBITMAP, ID_TASK_START,
            MAKELPARAM(actions.start ? startImageEnabled_ : startImageDisabled_, 0));
    }
}

void MainWindow::updateTooltip(NMTTDISPINFOW& info)
{
    POINT cursor{};
    GetCursorPos(&cursor);
    ScreenToClient(toolbar_, &cursor);
    const int hit = static_cast<int>(SendMessageW(toolbar_, TB_HITTEST, 0,
        reinterpret_cast<LPARAM>(&cursor)));
    UINT command = 0;
    if (hit >= 0) {
        TBBUTTON button{};
        if (SendMessageW(toolbar_, TB_GETBUTTON, static_cast<WPARAM>(hit),
                reinterpret_cast<LPARAM>(&button)))
            command = button.idCommand;
    }
    switch (command) {
    case ID_FILE_NEW: tooltipText_ = L"新建下载 (Ctrl+N)"; break;
    case ID_TASK_START:
        tooltipText_ = L"开始/继续 — 仅等待中或失败的任务可开始；断点续传尚未实现";
        break;
    case ID_TASK_PAUSE:
        tooltipText_ = L"暂停 — 尚未实现：需要断点续传支持（后续版本）";
        break;
    case ID_TASK_STOP: tooltipText_ = L"停止 — 停止选中的等待中或下载中任务"; break;
    case ID_TASK_DELETE:
        tooltipText_ = L"删除 — 尚未实现：将在列表与批量操作中提供";
        break;
    case ID_TASK_OPEN_FOLDER: tooltipText_ = L"打开文件夹 — 打开选中任务的保存位置"; break;
    case ID_TOOLS_OPTIONS:
        tooltipText_ = L"选项 — 尚未实现：设置界面将在后续版本提供";
        break;
    default: tooltipText_ = L""; break;
    }
    info.lpszText = tooltipText_.empty() ? const_cast<LPWSTR>(L"") : tooltipText_.data();
}

void MainWindow::openNewDownload()
{
    if (!worker_ || isClosing()) return;
    const auto request = showNewDownloadDialog(hwnd_, options_.downloadDirectory);
    if (!request) return;
    std::filesystem::path destination;
    try {
        destination = request->directory / pathFromUtf8(request->suggestedFileName);
    } catch (const std::exception&) {
        MessageBoxW(hwnd_, L"无法从下载地址推导文件名。", L"新建下载", MB_OK | MB_ICONERROR);
        return;
    }
    options_.downloadDirectory = request->directory;
    worker_->postAdd(request->source, pathToUtf8(destination), request->autoStart);
}

void MainWindow::startSelected()
{
    const auto* row = selectedRow();
    if (!row || !worker_) return;
    const auto actions = model_.actionsFor(row->id);
    if (!actions.start) return;
    worker_->postStart(row->id);
}

void MainWindow::stopSelected()
{
    const auto* row = selectedRow();
    if (!row || !worker_) return;
    const auto actions = model_.actionsFor(row->id);
    if (!actions.stop) return;
    worker_->postCancel(row->id);
}

void MainWindow::openFolderSelected()
{
    const auto* row = selectedRow();
    if (!row) return;
    std::filesystem::path destination;
    try {
        destination = pathFromUtf8(row->destination);
    } catch (const std::exception&) {
        MessageBoxW(hwnd_, L"任务保存路径无效。", L"打开文件夹", MB_OK | MB_ICONERROR);
        return;
    }
    std::error_code error;
    std::wstring commandLine;
    if (std::filesystem::is_regular_file(destination, error)) {
        commandLine = L"/select,\"" + destination.wstring() + L"\"";
    } else {
        const auto parent = destination.parent_path();
        if (!std::filesystem::is_directory(parent, error)) {
            MessageBoxW(hwnd_, L"文件夹不存在或无法访问。", L"打开文件夹", MB_OK | MB_ICONWARNING);
            return;
        }
        commandLine = L"\"" + parent.wstring() + L"\"";
    }
    const auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(hwnd_, L"open", L"explorer.exe",
        commandLine.c_str(), nullptr, SW_SHOWNORMAL));
    if (result <= 32)
        MessageBoxW(hwnd_, L"无法启动资源管理器。", L"打开文件夹", MB_OK | MB_ICONERROR);
}

void MainWindow::copyLinkSelected()
{
    const auto* row = selectedRow();
    if (!row || row->source.empty()) return;
    const auto text = toWide(row->source);
    if (!OpenClipboard(hwnd_)) return;
    EmptyClipboard();
    const std::size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    if (HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
        if (void* target = GlobalLock(memory)) {
            memcpy(target, text.c_str(), bytes);
            GlobalUnlock(memory);
            SetClipboardData(CF_UNICODETEXT, memory);
        } else {
            GlobalFree(memory);
        }
    }
    CloseClipboard();
}

void MainWindow::showCapabilities()
{
    MessageBoxW(hwnd_,
        L"本版本已实现：\n"
        L"• 新建 HTTP(S) 下载（真实传输、真实文件）\n"
        L"• 开始等待中或失败的任务\n"
        L"• 停止等待中或下载中的任务\n"
        L"• 打开任务保存文件夹、复制下载地址、刷新列表\n"
        L"\n"
        L"尚未实现，因此按钮保持禁用：\n"
        L"• 暂停 — 需要断点续传支持（后续版本）\n"
        L"• 删除 — 将随列表与批量操作提供\n"
        L"• 选项 — 设置界面将在后续版本提供\n"
        L"• 批量添加、续传、任务属性 — 后续版本\n",
        L"本版本能力说明", MB_OK | MB_ICONINFORMATION);
}

void MainWindow::showAbout()
{
    MessageBoxW(hwnd_,
        L"LiteFetch 0.1\n\n"
        L"原生 Win32 下载管理器（开发版本）\n"
        L"HTTP(S) 下载、真实任务列表与状态；无 Qt 依赖。",
        L"关于 LiteFetch", MB_OK | MB_ICONINFORMATION);
}

void MainWindow::beginClose()
{
    if (closing_) return;
    closing_ = true;
    KillTimer(hwnd_, IDT_REFRESH);
    updateActions();
    if (!worker_) {
        finishClose();
        return;
    }
    worker_->beginShutdown();
    setShutdownStatus();
    SetTimer(hwnd_, IDT_SHUTDOWN, shutdownTimerMillis, nullptr);
    if (worker_->shutdownComplete()) finishClose();
}

void MainWindow::finishClose()
{
    KillTimer(hwnd_, IDT_SHUTDOWN);
    if (worker_ && !closingWorkerReleased_) {
        worker_->join();
        closingWorkerReleased_ = true;
    }
    DestroyWindow(hwnd_);
}

} // namespace litefetch::ui
