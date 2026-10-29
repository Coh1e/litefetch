#include "main_window.h"

#include "batch_dialog.h"
#include "bt_engine.h"
#include "ed2k_task.h"
#include "options_dialog.h"
#include "settings.h"
#include "column_dialog.h"
#include "explorer_target.h"
#include "new_download_dialog.h"
#include "resource.h"
#include "task_view.h"
#include "text_utf8.h"
#include "ui_text.h"

#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <cwctype>
#include <fstream>
#include <system_error>

namespace litefetch::ui {
namespace {
constexpr wchar_t windowClass[] = L"LiteFetchMainWindow";
constexpr int refreshTimerMillis = 500;
constexpr int shutdownTimerMillis = 100;
constexpr wchar_t title[] = L"lite fetch";
constexpr unsigned int appRefreshColumnsMessage = WM_APP + 2;
constexpr unsigned int appUpdateActionsMessage = WM_APP + 3;
constexpr unsigned int appTorrentMetadataMessage = WM_APP + 4;
constexpr unsigned int appTrayMessage = WM_APP + 5;
constexpr unsigned int trayIconId = 1;

std::string hostOfUrl(const std::string& url)
{
    // "scheme://host:port/path" -> "host". Anything else yields an empty host,
    // and an empty host means the credential is not remembered.
    const auto scheme = url.find("://");
    if (scheme == std::string::npos) return {};
    const auto start = scheme + 3;
    auto end = url.find_first_of("/?#", start);
    if (end == std::string::npos) end = url.size();
    auto authority = url.substr(start, end - start);
    const auto at = authority.rfind('@');
    if (at != std::string::npos) authority = authority.substr(at + 1);
    const auto colon = authority.rfind(':');
    if (colon != std::string::npos) authority = authority.substr(0, colon);
    return authority;
}

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
    {ID_TASK_OPEN_FOLDER, L"打开文件", IDI_FOLDER, false, 0},
    {0, L"", 0, true, 0},
    {ID_TOOLS_OPTIONS, L"选项", IDI_SETTINGS, false, 0},
}};

int scale(int value, int dpi)
{
    return MulDiv(value, dpi, 96);
}

std::wstring statusBarText(const std::string& utf8)
{
    return toWide(utf8);
}

std::wstring shellErrorText(INT_PTR code)
{
    switch (code) {
    case 0: return L"系统内存或资源不足（错误 0）";
    case ERROR_FILE_NOT_FOUND: return L"文件不存在";
    case ERROR_PATH_NOT_FOUND: return L"路径不存在";
    case ERROR_ACCESS_DENIED: return L"访问被拒绝（权限不足）";
    case ERROR_BAD_FORMAT: return L"程序格式无效";
    case SE_ERR_ASSOCINCOMPLETE: return L"文件关联不完整";
    case SE_ERR_NOASSOC: return L"没有可打开此文件的程序";
    case SE_ERR_OOM: return L"系统内存不足";
    case SE_ERR_SHARE: return L"文件正被其他程序占用";
    default: return L"错误代码 " + std::to_wstring(code);
    }
}

bool isEditFocus()
{
    HWND focus = GetFocus();
    if (!focus) return false;
    wchar_t name[64]{};
    if (GetClassNameW(focus, name, static_cast<int>(std::size(name))) == 0) return false;
    return _wcsicmp(name, L"Edit") == 0;
}

std::wstring readClipboardText(HWND owner)
{
    if (!OpenClipboard(owner)) return {};
    std::wstring result;
    if (HANDLE data = GetClipboardData(CF_UNICODETEXT)) {
        if (const auto* text = static_cast<const wchar_t*>(GlobalLock(data))) {
            result = text;
            GlobalUnlock(data);
        }
    }
    CloseClipboard();
    return result;
}

void centerDialog(HWND dialog, HWND owner)
{
    if (!owner) return;
    RECT ownerRect{};
    RECT dialogRect{};
    if (!GetWindowRect(owner, &ownerRect) || !GetWindowRect(dialog, &dialogRect)) return;
    const int width = dialogRect.right - dialogRect.left;
    const int height = dialogRect.bottom - dialogRect.top;
    SetWindowPos(dialog, nullptr,
        ownerRect.left + ((ownerRect.right - ownerRect.left) - width) / 2,
        ownerRect.top + ((ownerRect.bottom - ownerRect.top) - height) / 2,
        0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

struct BatchResultRow {
    int line = 0;
    std::string address;
    std::string result;
};

struct BatchResultContext {
    std::vector<BatchResultRow> rows;
};

INT_PTR CALLBACK batchResultDialogProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_INITDIALOG: {
        auto* context = reinterpret_cast<BatchResultContext*>(lParam);
        SetWindowLongPtrW(dialog, GWLP_USERDATA, lParam);
        if (!context) return FALSE;
        HWND list = GetDlgItem(dialog, IDC_BATCH_RESULT_LIST);
        ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        LVCOLUMNW column{};
        column.mask = LVCF_TEXT | LVCF_WIDTH;
        column.pszText = const_cast<LPWSTR>(L"行");
        column.cx = 40;
        ListView_InsertColumn(list, 0, &column);
        column.pszText = const_cast<LPWSTR>(L"地址");
        column.cx = 230;
        ListView_InsertColumn(list, 1, &column);
        column.pszText = const_cast<LPWSTR>(L"结果");
        column.cx = 150;
        ListView_InsertColumn(list, 2, &column);
        for (std::size_t index = 0; index < context->rows.size(); ++index) {
            const auto& row = context->rows[index];
            const auto line = std::to_wstring(row.line);
            const auto address = toWide(row.address);
            const auto result = toWide(row.result);
            LVITEMW item{};
            item.mask = LVIF_TEXT;
            item.iItem = static_cast<int>(index);
            item.pszText = const_cast<LPWSTR>(line.c_str());
            ListView_InsertItem(list, &item);
            ListView_SetItemText(list, static_cast<int>(index), 1, const_cast<LPWSTR>(address.c_str()));
            ListView_SetItemText(list, static_cast<int>(index), 2, const_cast<LPWSTR>(result.c_str()));
        }
        centerDialog(dialog, GetParent(dialog));
        SetFocus(GetDlgItem(dialog, IDOK));
        return FALSE;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL) {
            EndDialog(dialog, IDOK);
            return TRUE;
        }
        return FALSE;
    case WM_CLOSE:
        EndDialog(dialog, IDOK);
        return TRUE;
    default:
        return FALSE;
    }
}

void showBatchResultDialog(HWND owner, const std::vector<BatchResultRow>& rows)
{
    BatchResultContext context;
    context.rows = rows;
    DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_BATCH_RESULT), owner,
        batchResultDialogProc, reinterpret_cast<LPARAM>(&context));
}

bool readTextFile(const std::filesystem::path& path, std::string& text)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) return false;
    std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xff
        && static_cast<unsigned char>(bytes[1]) == 0xfe) {
        try {
            const auto* wide = reinterpret_cast<const wchar_t*>(bytes.data() + 2);
            const auto count = (bytes.size() - 2) / sizeof(wchar_t);
            text = utf8FromUtf16(std::wstring_view(wide, count));
            return true;
        } catch (const std::exception&) {
            return false;
        }
    }
    if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xef
        && static_cast<unsigned char>(bytes[1]) == 0xbb
        && static_cast<unsigned char>(bytes[2]) == 0xbf)
        bytes.erase(0, 3);
    text = std::move(bytes);
    return true;
}

struct CredentialPrompt {
    std::string host;
    std::string username;
    std::string password;
    bool remember = false;
};

// The credentials dialog is modal: it runs its own loop and returns the typed
// values before the caller retries the transfer.
INT_PTR CALLBACK credentialDialogProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam)
{
    auto* context = reinterpret_cast<CredentialPrompt*>(GetWindowLongPtrW(dialog, GWLP_USERDATA));
    switch (message) {
    case WM_INITDIALOG: {
        context = reinterpret_cast<CredentialPrompt*>(lParam);
        SetWindowLongPtrW(dialog, GWLP_USERDATA, lParam);
        SetDlgItemTextW(dialog, IDC_CRED_HINT,
            L"服务器要求登录（HTTP 401）。凭据只用于本次运行，不会保存。");
        centerDialog(dialog, GetParent(dialog));
        SetFocus(GetDlgItem(dialog, IDC_CRED_USER));
        return FALSE;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK) {
            wchar_t buffer[256]{};
            GetDlgItemTextW(dialog, IDC_CRED_USER, buffer, 256);
            if (context) context->username = utf8FromUtf16(buffer);
            GetDlgItemTextW(dialog, IDC_CRED_PASS, buffer, 256);
            if (context) context->password = utf8FromUtf16(buffer);
            EndDialog(dialog, IDOK);
            return TRUE;
        }
        if (LOWORD(wParam) == IDCANCEL) {
            EndDialog(dialog, IDCANCEL);
            return TRUE;
        }
        return FALSE;
    case WM_CLOSE:
        EndDialog(dialog, IDCANCEL);
        return TRUE;
    default:
        return FALSE;
    }
}
struct TorrentFetchResult {
    BtMetadataResult metadata;
    std::string source;
};

struct TorrentSelection {
    std::wstring name;
    std::vector<BtFile> files;
    std::vector<int> selected;
};

// The dialog shows the metadata the engine already fetched; nothing is
// downloaded until the user confirms the checked rows.
INT_PTR CALLBACK torrentFilesDialogProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam)
{
    auto* selection = reinterpret_cast<TorrentSelection*>(GetWindowLongPtrW(dialog, GWLP_USERDATA));
    switch (message) {
    case WM_INITDIALOG: {
        selection = reinterpret_cast<TorrentSelection*>(lParam);
        SetWindowLongPtrW(dialog, GWLP_USERDATA, lParam);
        if (!selection) return FALSE;
        SetDlgItemTextW(dialog, IDC_TORRENT_NAME, selection->name.c_str());
        const HWND list = GetDlgItem(dialog, IDC_TORRENT_LIST);
        ListView_SetExtendedListViewStyle(list,
            LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_CHECKBOXES);
        LVCOLUMNW column{};
        column.mask = LVCF_TEXT | LVCF_WIDTH;
        column.pszText = const_cast<LPWSTR>(L"文件");
        column.cx = 280;
        ListView_InsertColumn(list, 0, &column);
        column.pszText = const_cast<LPWSTR>(L"大小");
        column.cx = 100;
        ListView_InsertColumn(list, 1, &column);
        for (std::size_t index = 0; index < selection->files.size(); ++index) {
            const auto& file = selection->files[index];
            const auto path = toWide(file.path);
            const auto size = toWide(formatSize(file.size));
            LVITEMW item{};
            item.mask = LVIF_TEXT;
            item.iItem = static_cast<int>(index);
            item.pszText = const_cast<LPWSTR>(path.c_str());
            ListView_InsertItem(list, &item);
            ListView_SetItemText(list, static_cast<int>(index), 1, const_cast<LPWSTR>(size.c_str()));
            ListView_SetCheckState(list, static_cast<int>(index), TRUE);
        }
        centerDialog(dialog, GetParent(dialog));
        return TRUE;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK) {
            const HWND list = GetDlgItem(dialog, IDC_TORRENT_LIST);
            if (selection) {
                selection->selected.clear();
                for (std::size_t index = 0; index < selection->files.size(); ++index) {
                    if (ListView_GetCheckState(list, static_cast<int>(index)))
                        selection->selected.push_back(selection->files[index].index);
                }
            }
            EndDialog(dialog, IDOK);
            return TRUE;
        }
        if (LOWORD(wParam) == IDCANCEL) {
            EndDialog(dialog, IDCANCEL);
            return TRUE;
        }
        return FALSE;
    case WM_CLOSE:
        EndDialog(dialog, IDCANCEL);
        return TRUE;
    default:
        return FALSE;
    }
}

struct TorrentDetailsState {
    std::wstring name;
    std::wstring identity;
    std::wstring state;
    std::wstring size;
    std::wstring savePath;
    std::wstring privateText;
    std::wstring done;
    std::wstring progress;
    std::wstring rate;
    std::wstring uploadRate;
    std::wstring remaining;
    std::wstring peers;
    std::wstring protocol;
    std::wstring seeds;
    std::wstring uploaded;
    int seedMode = 0;
    int seedRatioMilli = 1000;
    int seedMinutes = 30;
    std::vector<TorrentFileInfo> files;
    std::vector<int> priorities;
    std::vector<std::string> trackers;
    bool privateTorrent = false;
    bool dirty = false;
    std::function<void(const TorrentDetailsState&)> apply;
};

std::wstring priorityText(int priority)
{
    if (priority <= 0) return L"跳过";
    if (priority >= 2) return L"高";
    return L"正常";
}

void refreshTorrentFileList(HWND list, const TorrentDetailsState& state)
{
    for (std::size_t index = 0; index < state.files.size(); ++index) {
        const auto text = priorityText(index < state.priorities.size() ? state.priorities[index] : 1);
        ListView_SetItemText(list, static_cast<int>(index), 2, const_cast<LPWSTR>(text.c_str()));
    }
}

constexpr int torrentTabCount = 5;

// Which controls belong to which tab; every page shares the same rectangle and
// only the active page is visible.
struct TabPage {
    int tab;
    std::initializer_list<int> controls;
};

const TabPage torrentTabPages[] = {
    {0, {IDC_TD_NAME, IDC_TD_HASH, IDC_TD_STATE, IDC_TD_SIZE, IDC_TD_PATH, IDC_TD_PRIVATE}},
    {1, {IDC_TD_DONE, IDC_TD_PROGRESS, IDC_TD_RATE, IDC_TD_UP, IDC_TD_LEFT, IDC_TD_UPLOADED,
        IDC_TD_SEED_STOP, IDC_TD_SEED_RATIO, IDC_TD_RATIO_EDIT, IDC_TD_SEED_TIME,
        IDC_TD_MINUTES_EDIT}},
    {2, {IDC_TD_PEERS, IDC_TD_PROTO, IDC_TD_SEEDS, IDC_TD_NOTE}},
    {3, {IDC_TD_FILES, IDC_TD_SKIP, IDC_TD_NORMAL, IDC_TD_HIGH}},
    {4, {IDC_TD_TRACKERS, IDC_TD_TRACKER, IDC_TD_ADD, IDC_TD_REMOVE}},
};

// Static labels on each page are numbered per page in the resource file; a
// page also owns the labels that sit in its rectangle, so they are toggled by
// position rather than by id.
void showTorrentPage(HWND dialog, int page)
{
    for (int index = 0; index < torrentTabCount; ++index) {
        const bool visible = index == page;
        for (const int id : torrentTabPages[index].controls)
            ShowWindow(GetDlgItem(dialog, id), visible ? SW_SHOW : SW_HIDE);
    }
}

// The properties page stages every edit: priority buttons and tracker edits
// only touch the working copy until 应用 posts the command.
INT_PTR CALLBACK torrentDetailsDialogProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam)
{
    auto* state = reinterpret_cast<TorrentDetailsState*>(GetWindowLongPtrW(dialog, GWLP_USERDATA));
    switch (message) {
    case WM_INITDIALOG: {
        state = reinterpret_cast<TorrentDetailsState*>(lParam);
        SetWindowLongPtrW(dialog, GWLP_USERDATA, lParam);
        if (!state) return FALSE;
        SetDlgItemTextW(dialog, IDC_TD_NAME, state->name.c_str());
        SetDlgItemTextW(dialog, IDC_TD_HASH, state->identity.c_str());
        SetDlgItemTextW(dialog, IDC_TD_STATE, state->state.c_str());
        SetDlgItemTextW(dialog, IDC_TD_SIZE, state->size.c_str());
        SetDlgItemTextW(dialog, IDC_TD_PATH, state->savePath.c_str());
        SetDlgItemTextW(dialog, IDC_TD_PRIVATE, state->privateText.c_str());
        SetDlgItemTextW(dialog, IDC_TD_DONE, state->done.c_str());
        SetDlgItemTextW(dialog, IDC_TD_PROGRESS, state->progress.c_str());
        SetDlgItemTextW(dialog, IDC_TD_RATE, state->rate.c_str());
        SetDlgItemTextW(dialog, IDC_TD_UP, state->uploadRate.c_str());
        SetDlgItemTextW(dialog, IDC_TD_LEFT, state->remaining.c_str());
        SetDlgItemTextW(dialog, IDC_TD_PEERS, state->peers.c_str());
        SetDlgItemTextW(dialog, IDC_TD_PROTO, state->protocol.c_str());
        SetDlgItemTextW(dialog, IDC_TD_SEEDS, state->seeds.c_str());
        SetDlgItemTextW(dialog, IDC_TD_UPLOADED, state->uploaded.c_str());
        CheckRadioButton(dialog, IDC_TD_SEED_STOP, IDC_TD_SEED_TIME,
            state->seedMode == 1 ? IDC_TD_SEED_RATIO
                                 : (state->seedMode == 2 ? IDC_TD_SEED_TIME : IDC_TD_SEED_STOP));
        SetDlgItemInt(dialog, IDC_TD_RATIO_EDIT, static_cast<UINT>(state->seedRatioMilli), FALSE);
        SetDlgItemInt(dialog, IDC_TD_MINUTES_EDIT, static_cast<UINT>(state->seedMinutes), FALSE);

        const HWND tabs = GetDlgItem(dialog, IDC_TD_TABS);
        const wchar_t* captions[torrentTabCount] = {L"常规", L"传输", L"连接", L"文件", L"追踪器"};
        for (int index = 0; index < torrentTabCount; ++index) {
            TCITEMW item{};
            item.mask = TCIF_TEXT;
            item.pszText = const_cast<LPWSTR>(captions[index]);
            TabCtrl_InsertItem(tabs, index, &item);
        }

        const HWND files = GetDlgItem(dialog, IDC_TD_FILES);
        ListView_SetExtendedListViewStyle(files, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        LVCOLUMNW column{};
        column.mask = LVCF_TEXT | LVCF_WIDTH;
        column.pszText = const_cast<LPWSTR>(L"文件");
        column.cx = 330;
        ListView_InsertColumn(files, 0, &column);
        column.pszText = const_cast<LPWSTR>(L"大小");
        column.cx = 80;
        ListView_InsertColumn(files, 1, &column);
        column.pszText = const_cast<LPWSTR>(L"优先级");
        column.cx = 60;
        ListView_InsertColumn(files, 2, &column);
        for (std::size_t index = 0; index < state->files.size(); ++index) {
            const auto& file = state->files[index];
            const auto path = toWide(file.path);
            const auto size = toWide(formatSize(file.size));
            LVITEMW item{};
            item.mask = LVIF_TEXT;
            item.iItem = static_cast<int>(index);
            item.pszText = const_cast<LPWSTR>(path.c_str());
            ListView_InsertItem(files, &item);
            ListView_SetItemText(files, static_cast<int>(index), 1, const_cast<LPWSTR>(size.c_str()));
        }
        refreshTorrentFileList(files, *state);

        const HWND trackers = GetDlgItem(dialog, IDC_TD_TRACKERS);
        ListView_SetExtendedListViewStyle(trackers, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        column.pszText = const_cast<LPWSTR>(L"追踪器");
        column.cx = 470;
        ListView_InsertColumn(trackers, 0, &column);
        for (std::size_t index = 0; index < state->trackers.size(); ++index) {
            const auto text = toWide(state->trackers[index]);
            LVITEMW item{};
            item.mask = LVIF_TEXT;
            item.iItem = static_cast<int>(index);
            item.pszText = const_cast<LPWSTR>(text.c_str());
            ListView_InsertItem(trackers, &item);
        }

        // A private torrent may only use the trackers baked into it.
        if (state->privateTorrent) {
            EnableWindow(GetDlgItem(dialog, IDC_TD_TRACKER), FALSE);
            EnableWindow(GetDlgItem(dialog, IDC_TD_ADD), FALSE);
            EnableWindow(GetDlgItem(dialog, IDC_TD_REMOVE), FALSE);
            SetDlgItemTextW(dialog, IDC_TD_TRACKER, L"私有种子不可修改追踪器");
        }
        EnableWindow(GetDlgItem(dialog, IDC_TD_APPLY), FALSE);
        showTorrentPage(dialog, 0);
        centerDialog(dialog, GetParent(dialog));
        return TRUE;
    }
    case WM_NOTIFY: {
        const auto* header = reinterpret_cast<NMHDR*>(lParam);
        if (header && header->idFrom == IDC_TD_TABS && header->code == TCN_SELCHANGE) {
            showTorrentPage(dialog, TabCtrl_GetCurSel(GetDlgItem(dialog, IDC_TD_TABS)));
            return TRUE;
        }
        return FALSE;
    }
    case WM_COMMAND: {
        const auto id = LOWORD(wParam);
        if (!state) return FALSE;
        const HWND files = GetDlgItem(dialog, IDC_TD_FILES);
        if (id == IDC_TD_SKIP || id == IDC_TD_NORMAL || id == IDC_TD_HIGH) {
            const int priority = id == IDC_TD_SKIP ? 0 : (id == IDC_TD_HIGH ? 2 : 1);
            const int selected = ListView_GetNextItem(files, -1, LVNI_SELECTED);
            const int first = selected == -1 ? 0 : selected;
            const int last = selected == -1 ? static_cast<int>(state->priorities.size()) - 1
                                            : selected;
            for (int index = first; index <= last && index < static_cast<int>(state->priorities.size());
                 ++index) {
                if (index < 0) continue;
                state->priorities[static_cast<std::size_t>(index)] = priority;
            }
            refreshTorrentFileList(files, *state);
            state->dirty = true;
            EnableWindow(GetDlgItem(dialog, IDC_TD_APPLY), TRUE);
            return TRUE;
        }
        if (id == IDC_TD_ADD) {
            wchar_t buffer[1024]{};
            GetDlgItemTextW(dialog, IDC_TD_TRACKER, buffer, 1024);
            const auto url = utf8FromUtf16(buffer);
            if (!url.empty()) {
                state->trackers.push_back(url);
                const HWND trackers = GetDlgItem(dialog, IDC_TD_TRACKERS);
                const auto text = toWide(url);
                LVITEMW item{};
                item.mask = LVIF_TEXT;
                item.iItem = static_cast<int>(state->trackers.size()) - 1;
                item.pszText = const_cast<LPWSTR>(text.c_str());
                ListView_InsertItem(trackers, &item);
                SetDlgItemTextW(dialog, IDC_TD_TRACKER, L"");
                state->dirty = true;
                EnableWindow(GetDlgItem(dialog, IDC_TD_APPLY), TRUE);
            }
            return TRUE;
        }
        if (id == IDC_TD_REMOVE) {
            const HWND trackers = GetDlgItem(dialog, IDC_TD_TRACKERS);
            const int selected = ListView_GetNextItem(trackers, -1, LVNI_SELECTED);
            if (selected >= 0 && selected < static_cast<int>(state->trackers.size())) {
                state->trackers.erase(state->trackers.begin() + selected);
                ListView_DeleteItem(trackers, selected);
                state->dirty = true;
                EnableWindow(GetDlgItem(dialog, IDC_TD_APPLY), TRUE);
            }
            return TRUE;
        }
        if (id == IDC_TD_APPLY) {
            BOOL translated = FALSE;
            state->seedMode = IsDlgButtonChecked(dialog, IDC_TD_SEED_RATIO) ? 1
                : (IsDlgButtonChecked(dialog, IDC_TD_SEED_TIME) ? 2 : 0);
            const UINT ratio = GetDlgItemInt(dialog, IDC_TD_RATIO_EDIT, &translated, FALSE);
            if (translated) state->seedRatioMilli = static_cast<int>(ratio);
            const UINT minutes = GetDlgItemInt(dialog, IDC_TD_MINUTES_EDIT, &translated, FALSE);
            if (translated) state->seedMinutes = static_cast<int>(minutes);
            if (state->apply) state->apply(*state);
            state->dirty = false;
            EnableWindow(GetDlgItem(dialog, IDC_TD_APPLY), FALSE);
            return TRUE;
        }
        if (id == IDOK || id == IDCANCEL) {
            EndDialog(dialog, id);
            return TRUE;
        }
        return FALSE;
    }
    case WM_CLOSE:
        EndDialog(dialog, IDOK);
        return TRUE;
    default:
        return FALSE;
    }
}

struct PropertyFields {
    std::wstring name;
    std::wstring source;
    std::wstring destination;
    std::wstring size;
    std::wstring state;
    std::wstring protocol;
    std::wstring connection;
};

INT_PTR CALLBACK propertiesDialogProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam)
{
    auto* fields = reinterpret_cast<PropertyFields*>(GetWindowLongPtrW(dialog, GWLP_USERDATA));
    switch (message) {
    case WM_INITDIALOG: {
        fields = reinterpret_cast<PropertyFields*>(lParam);
        SetWindowLongPtrW(dialog, GWLP_USERDATA, lParam);
        if (!fields) return FALSE;
        SetDlgItemTextW(dialog, IDC_PROP_NAME, fields->name.c_str());
        SetDlgItemTextW(dialog, IDC_PROP_SOURCE, fields->source.c_str());
        SetDlgItemTextW(dialog, IDC_PROP_DEST, fields->destination.c_str());
        SetDlgItemTextW(dialog, IDC_PROP_SIZE, fields->size.c_str());
        SetDlgItemTextW(dialog, IDC_PROP_STATE, fields->state.c_str());
        SetDlgItemTextW(dialog, IDC_PROP_PROTOCOL, fields->protocol.c_str());
        SetDlgItemTextW(dialog, IDC_PROP_CONNECTION, fields->connection.c_str());
        centerDialog(dialog, GetParent(dialog));
        return TRUE;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL) {
            EndDialog(dialog, IDOK);
            return TRUE;
        }
        return FALSE;
    case WM_CLOSE:
        EndDialog(dialog, IDOK);
        return TRUE;
    default:
        return FALSE;
    }
}
} // namespace
INT_PTR CALLBACK MainWindow::deleteDialogProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam)
{
    auto* self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(dialog, GWLP_USERDATA));
    switch (message) {
    case WM_INITDIALOG: {
        self = reinterpret_cast<MainWindow*>(lParam);
        SetWindowLongPtrW(dialog, GWLP_USERDATA, lParam);
        if (!self) return FALSE;
        const auto text = L"确定从列表中删除所选的 "
            + std::to_wstring(self->pendingDeleteSelection_.size()) + L" 个任务？";
        SetDlgItemTextW(dialog, IDC_DELETE_MESSAGE, text.c_str());
        centerDialog(dialog, GetParent(dialog));
        SetFocus(GetDlgItem(dialog, IDCANCEL));
        return FALSE;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK) {
            if (self) self->finishDeleteDialog(true);
            return TRUE;
        }
        if (LOWORD(wParam) == IDCANCEL) {
            if (self) self->finishDeleteDialog(false);
            return TRUE;
        }
        return FALSE;
    case WM_CLOSE:
        if (self) self->finishDeleteDialog(false);
        return TRUE;
    default:
        return FALSE;
    }
}

MainWindow::MainWindow(AppOptions options)
    : options_(std::move(options)), columnSettings_(defaultColumnSettings())
{
}

MainWindow::~MainWindow()
{
    if (deleteDialog_) {
        HWND dialog = deleteDialog_;
        deleteDialog_ = nullptr;
        DestroyWindow(dialog);
    }
    if (worker_) worker_->shutdown();
    if (rowImages_) ImageList_Destroy(rowImages_);
    if (toolbarImages_) ImageList_Destroy(toolbarImages_);
    // The menu is assigned to the window, which destroys it with the window.
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
    if (!RegisterClassExW(&windowClassInfo)) {
        // A second window in the same process (acceptance restart, reopened
        // window) reuses the class the first one registered.
        if (GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    }

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
    try {
        return self->handleMessage(message, wParam, lParam);
    } catch (const std::exception& error) {
        self->logUiFailure("window message " + std::to_string(message) + " failed: " + error.what());
        return 0;
    } catch (...) {
        self->logUiFailure("window message " + std::to_string(message) + " failed: unknown exception");
        return 0;
    }
}

void MainWindow::logUiFailure(std::string_view message) const
{
    const auto directory = options_.databasePath.parent_path();
    if (directory.empty()) return;
    std::error_code ignored;
    std::filesystem::create_directories(directory, ignored);
    std::ofstream log(directory / L"ui-error.log", std::ios::binary | std::ios::app);
    if (!log) return;
    log << message << '\n';
}

LRESULT CALLBACK MainWindow::searchSubclassStatic(HWND window, UINT message, WPARAM wParam,
    LPARAM lParam, UINT_PTR, DWORD_PTR reference)
{
    auto* self = reinterpret_cast<MainWindow*>(reference);
    if (message == WM_KEYDOWN) {
        if (wParam == VK_ESCAPE) {
            if (self) self->hideSearch();
            return 0;
        }
        if (wParam == VK_RETURN) {
            if (self && self->list_) SetFocus(self->list_);
            return 0;
        }
    }
    return DefSubclassProc(window, message, wParam, lParam);
}

LRESULT MainWindow::handleMessage(UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_CREATE:
        if (!createControls()) return -1;
        return 0;
    case WM_SIZE:
        layoutControls();
        layoutStatusParts();
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
        lastColumnClientWidth_ = -1;
        lastColumnDpi_ = 0;
        applyColumnWidths();
        layoutStatusParts();
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
    case appRefreshColumnsMessage:
        rebuildColumns();
        syncList();
        updateStatus();
        updateActions();
        return 0;
    case appUpdateActionsMessage:
        updateActions();
        return 0;
    case appTorrentMetadataMessage: {
        auto* result = reinterpret_cast<TorrentFetchResult*>(lParam);
        if (!result) return 0;
        const auto metadata = std::move(result->metadata);
        const auto source = std::move(result->source);
        delete result;
        handleTorrentMetadata(std::move(metadata), source);
        return 0;
    }
    case WM_DROPFILES:
        handleDrop(reinterpret_cast<HDROP>(wParam));
        return 0;
    case WM_NOTIFY: {
        auto* header = reinterpret_cast<NMHDR*>(lParam);
        if (header->hwndFrom == tooltip_ && header->code == TTN_GETDISPINFOW) {
            updateTooltip(*reinterpret_cast<NMTTDISPINFOW*>(lParam));
            return 0;
        }
        if (list_ && header->hwndFrom == ListView_GetHeader(list_)) {
            if (header->code == HDN_ENDDRAG) {
                auto* info = reinterpret_cast<NMHEADERW*>(lParam);
                if (info->iItem >= 0 && info->pitem && info->pitem->iOrder >= 0
                    && info->iItem < static_cast<int>(visibleColumns_.size())
                    && info->pitem->iOrder < static_cast<int>(visibleColumns_.size())
                    && info->iItem != info->pitem->iOrder) {
                    const auto from = static_cast<std::size_t>(info->iItem);
                    const auto to = static_cast<std::size_t>(info->pitem->iOrder);
                    std::vector<std::size_t> slots;
                    std::vector<ColumnState> states;
                    for (std::size_t index = 0; index < columnSettings_.columns.size(); ++index) {
                        if (!columnSettings_.columns[index].visible) continue;
                        slots.push_back(index);
                        states.push_back(columnSettings_.columns[index]);
                    }
                    if (states.size() == visibleColumns_.size()) {
                        auto moved = states[from];
                        states.erase(states.begin() + static_cast<std::ptrdiff_t>(from));
                        states.insert(states.begin() + static_cast<std::ptrdiff_t>(to), moved);
                        for (std::size_t index = 0; index < slots.size(); ++index)
                            columnSettings_.columns[slots[index]] = states[index];
                        saveColumnSettings();
                        PostMessageW(hwnd_, appRefreshColumnsMessage, 0, 0);
                    }
                }
                return TRUE; // the order is applied by our rebuild
            }
            if (header->code == HDN_ENDTRACK) {
                auto* info = reinterpret_cast<NMHEADERW*>(lParam);
                const int tracked = info->iItem;
                HWND listHeader = ListView_GetHeader(list_);
                const int count = Header_GetItemCount(listHeader);
                for (int index = 0; index < count && index < static_cast<int>(visibleColumns_.size());
                     ++index) {
                    HDITEMW item{};
                    item.mask = HDI_WIDTH;
                    if (!Header_GetItem(listHeader, index, &item) || item.cxy <= 0) continue;
                    const int width = item.cxy;
                    const auto id = visibleColumns_[static_cast<std::size_t>(index)];
                    for (auto& state : columnSettings_.columns) {
                        if (state.id != id) continue;
                        if (index == count - 1 && index != tracked) break;
                        state.width = std::clamp(MulDiv(width, 96, dpi_), 40, 2000);
                        break;
                    }
                }
                saveColumnSettings();
                lastColumnClientWidth_ = -1;
                applyColumnWidths();
                return 0;
            }
        }
        if (list_ && header->hwndFrom == list_) {
            if (header->code == LVN_ITEMCHANGED) {
                // Do not re-enter the ListView while it is delivering this
                // notification (a cross-thread LVM_SETITEMSTATE would block).
                if (!suppressSelection_) PostMessageW(hwnd_, appUpdateActionsMessage, 0, 0);
                return 0;
            }
            if (header->code == LVN_KEYDOWN) {
                const auto* key = reinterpret_cast<NMLVKEYDOWN*>(lParam);
                if (key->wVKey == VK_DELETE) {
                    if (GetKeyState(VK_SHIFT) < 0) {
                        setNotice("#18 将提供“删除任务及文件”的独立确认；本次未删除任何内容。");
                    } else {
                        deleteSelected();
                    }
                    return 0;
                }
                if (key->wVKey == VK_RETURN) {
                    startSelected();
                    return 0;
                }
                if (key->wVKey == VK_ESCAPE && searchVisible_) {
                    hideSearch();
                    return 0;
                }
                return 0;
            }
            if (header->code == LVN_COLUMNCLICK) {
                const auto* info = reinterpret_cast<NMLISTVIEW*>(lParam);
                if (info->iSubItem >= 0
                    && info->iSubItem < static_cast<int>(visibleColumns_.size())) {
                    const auto column = visibleColumns_[static_cast<std::size_t>(info->iSubItem)];
                    if (columnSettings_.sortColumn == column) {
                        columnSettings_.sortAscending = !columnSettings_.sortAscending;
                    } else {
                        columnSettings_.sortColumn = column;
                        columnSettings_.sortAscending = true;
                    }
                    saveColumnSettings();
                    applySortArrow();
                    syncList();
                    updateStatus();
                    updateActions();
                }
                return 0;
            }
            if (header->code == LVN_GETINFOTIPW) {
                auto* tip = reinterpret_cast<NMLVGETINFOTIPW*>(lParam);
                if (tip->iItem >= 0
                    && static_cast<std::size_t>(tip->iItem) < viewRows_.size()) {
                    const auto& row = viewRows_[static_cast<std::size_t>(tip->iItem)];
                    std::wstring text = toWide(row.name) + L"\r\n" + toWide(row.address)
                        + L"\r\n" + toWide(row.destination);
                    if (!row.error.empty()) text += L"\r\n" + toWide(row.error);
                    lstrcpynW(tip->pszText, text.c_str(), tip->cchTextMax);
                }
                return 0;
            }
            if (header->code == NM_DBLCLK) {
                showProperties();
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
        case ID_FILE_BATCH: openBatchAdd(); return 0;
        case ID_FILE_OPEN_TORRENT: openTorrentBoundary(); return 0;
        case ID_FILE_EXIT: PostMessageW(hwnd_, WM_CLOSE, 0, 0); return 0;
        case ID_EDIT_COPY_LINK:
            if (isEditFocus()) {
                if (HWND focus = GetFocus()) SendMessageW(focus, WM_COPY, 0, 0);
            } else {
                copySelectedLinks();
            }
            return 0;
        case ID_EDIT_COPY_PATH: copySelectedPaths(); return 0;
        case ID_EDIT_SELECT_ALL: selectAllVisible(); return 0;
        case ID_EDIT_FIND: showSearch(); return 0;
        case ID_EDIT_PASTE:
            if (isEditFocus()) {
                if (HWND focus = GetFocus()) SendMessageW(focus, WM_PASTE, 0, 0);
            } else {
                pasteFromClipboard();
            }
            return 0;
        case ID_VIEW_REFRESH:
            if (worker_) worker_->postRefresh();
            return 0;
        case ID_VIEW_FILTER_ALL: applyFilter(TaskFilter::All); return 0;
        case ID_VIEW_FILTER_ACTIVE: applyFilter(TaskFilter::Active); return 0;
        case ID_VIEW_FILTER_COMPLETED: applyFilter(TaskFilter::Completed); return 0;
        case ID_VIEW_FILTER_FAILED: applyFilter(TaskFilter::Failed); return 0;
        case ID_VIEW_TOOLBAR: toggleToolbar(); return 0;
        case ID_VIEW_STATUSBAR: toggleStatusBar(); return 0;
        case ID_VIEW_COLUMNS: showColumnChooser(); return 0;
        case ID_TASK_START: startSelected(); return 0;
        case ID_TASK_STOP: stopSelected(); return 0;
        case ID_TASK_PAUSE: pauseSelected(); return 0;
        case ID_TASK_DELETE: deleteSelected(); return 0;
        case ID_TASK_OPEN_FILE: openFileSelected(); return 0;
        case ID_TRAY_SHOW:
            ShowWindow(hwnd_, SW_RESTORE);
            SetForegroundWindow(hwnd_);
            return 0;
        case ID_TASK_OPEN_FOLDER: openFolderSelected(); return 0;
        case ID_TASK_PRIORITY_HIGH:
            setSelectedPriority(10);
            return 0;
        case ID_TASK_PRIORITY_NORMAL:
            setSelectedPriority(0);
            return 0;
        case ID_TASK_PRIORITY_LOW:
            setSelectedPriority(-10);
            return 0;
        case ID_TASK_VERIFY: verifySelected(); return 0;
        case ID_TASK_REDOWNLOAD:
            setNotice("重新下载需要目标冲突与文件保护确认（后续版本）；本次未创建任务。");
            return 0;
        case ID_TASK_DELETE_FILES: {
            // Deleting files is its own confirmation: the record alone is what
            // the other menu item removes.
            const auto row = rowForId(focusedId());
            if (!row) return 0;
            const auto answer = MessageBoxW(hwnd_,
                toWide("将删除任务记录以及它下载的文件：\n" + row->savePath
                    + "\n\n已下载或未完成的片段都会删除，且不可恢复。是否继续？").c_str(),
                L"删除任务及文件", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
            if (answer != IDYES) return 0;
            worker_->postDeleteFiles(row->id);
            setNotice("正在删除任务及文件：" + row->name);
            return 0;
        }
        case ID_TASK_PROPERTIES:            showProperties();
            return 0;
        case ID_TOOLS_OPTIONS:
            setNotice("选项界面将在后续版本提供。");
            return 0;
        case ID_HELP_CAPABILITIES: showCapabilities(); return 0;
        case ID_HELP_ABOUT: showAbout(); return 0;
        case IDC_SEARCH:
            if (HIWORD(wParam) == EN_CHANGE) {
                updateSearchQuery();
                return 0;
            }
            break;
        case IDC_SEARCH_CLOSE:
            if (HIWORD(wParam) == BN_CLICKED) {
                hideSearch();
                return 0;
            }
            break;
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
    {
        ColumnSettings loaded;
        std::string error;
        if (loadColumnSettings(uiStatePath(), loaded, error))
            columnSettings_ = std::move(loaded);
        else
            columnSettings_ = defaultColumnSettings();
    }
    createFont();
    createToolbar();
    createList();
    createStatusBar();
    createTooltip();
    createSearchOverlay();
    applyFont();
    updateLayoutMetrics();
    layoutControls();
    layoutStatusParts();
    DragAcceptFiles(hwnd_, TRUE);

    // ed2k servers come from ed2k-servers.txt next to the database (one
    // host:port per line) until the settings dialog owns this list.
    std::vector<Ed2kServerEndpoint> ed2kServers;
    {
        std::ifstream servers(options_.databasePath.parent_path() / "ed2k-servers.txt");
        std::string line;
        while (std::getline(servers, line)) {
            const auto colon = line.rfind(':');
            if (colon == std::string::npos) continue;
            const auto port = std::atoi(line.c_str() + colon + 1);
            if (port <= 0 || port > 65535) continue;
            ed2kServers.push_back({line.substr(0, colon), static_cast<std::uint16_t>(port)});
        }
    }
    worker_ = std::make_unique<TaskWorker>(options_.databasePath, hwnd_, btDownload,
        [ed2kServers](const Ed2kTaskRequest& request, const std::atomic_bool& cancelled,
            const std::function<void(std::int64_t, std::int64_t)>& progress) {
            auto withServers = request;
            withServers.servers = ed2kServers;
            return downloadEd2kTask(withServers, cancelled, progress);
        });
    std::string error;
    if (!worker_->start(error)) {
        MessageBoxW(hwnd_, toWide("无法打开任务数据库：" + error).c_str(), L"lite fetch",
            MB_OK | MB_ICONERROR);
        return false;
    }
    refreshFromWorker();
    SetTimer(hwnd_, IDT_REFRESH, refreshTimerMillis, nullptr);
    return true;
}

void MainWindow::notifyTaskFinished(const TaskSnapshot& task)
{
    if (!trayVisible_) return; // the shell is not reachable in this session
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = hwnd_;
    data.uID = trayIconId;
    data.uFlags = NIF_INFO;
    const auto name = task.destination.empty() ? task.source : task.destination;
    const auto fileName = name.substr(name.find_last_of("\\\\/") + 1);
    const auto title = toWide(fileName);
    wcsncpy_s(data.szInfoTitle, title.c_str(), _TRUNCATE);
    const auto body = task.state == TaskState::Completed ? L"下载完成" : L"下载失败";
    wcsncpy_s(data.szInfo, body, _TRUNCATE);
    data.dwInfoFlags = task.state == TaskState::Completed ? NIIF_INFO : NIIF_ERROR;
    Shell_NotifyIconW(NIM_MODIFY, &data);
}

void MainWindow::addTrayIcon()
{
    if (trayVisible_ || !hwnd_) return;
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = hwnd_;
    data.uID = trayIconId;
    data.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    data.uCallbackMessage = appTrayMessage;
    data.hIcon = largeIcon_ ? largeIcon_ : LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APP));
    // The tooltip keeps the task count so the window can stay minimised.
    std::wstring tip = L"LiteFetch";
    const auto count = model_.rows().size();
    if (count > 0) tip += L" - " + std::to_wstring(count) + L" 个任务";
    wcsncpy_s(data.szTip, tip.c_str(), _TRUNCATE);
    trayVisible_ = Shell_NotifyIconW(NIM_ADD, &data) == TRUE;
}

void MainWindow::removeTrayIcon()
{
    if (!trayVisible_) return;
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = hwnd_;
    data.uID = trayIconId;
    Shell_NotifyIconW(NIM_DELETE, &data);
    trayVisible_ = false;
}

void MainWindow::showTrayMenu()
{
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING, ID_TRAY_SHOW, L"显示主窗口(&O)");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_FILE_EXIT, L"退出(&X)");
    POINT cursor{};
    GetCursorPos(&cursor);
    SetForegroundWindow(hwnd_);
    const auto command = static_cast<UINT>(TrackPopupMenu(menu,
        TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY, cursor.x, cursor.y, 0, hwnd_, nullptr));
    DestroyMenu(menu);
    if (command != 0) PostMessageW(hwnd_, WM_COMMAND, MAKEWPARAM(command, 0), 0);
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
    toolbarImages_ = images;
    SendMessageW(toolbar_, TB_ADDBUTTONSW, static_cast<WPARAM>(buttons.size()),
        reinterpret_cast<LPARAM>(buttons.data()));
    SendMessageW(toolbar_, TB_AUTOSIZE, 0, 0);
    updateActions();
}

void MainWindow::createList()
{
    list_ = CreateWindowExW(0, WC_LISTVIEWW, L"任务列表",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | LVS_REPORT | LVS_SHOWSELALWAYS,
        0, 0, 0, 0, hwnd_, nullptr, instance_, nullptr);
    if (!list_) return;
    ListView_SetExtendedListViewStyle(list_,
        LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP | LVS_EX_HEADERDRAGDROP);
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
        rowImages_ = rows;
    }

    rebuildColumns();
}

void MainWindow::rebuildColumns()
{
    if (!list_) return;
    HWND header = ListView_GetHeader(list_);
    const int count = header ? Header_GetItemCount(header) : 0;
    for (int index = count - 1; index >= 0; --index) ListView_DeleteColumn(list_, index);
    visibleColumns_ = visibleColumns(columnSettings_);
    const auto widths = columnWidths(columnSettings_, scale(998, dpi_), dpi_);
    for (std::size_t index = 0; index < visibleColumns_.size(); ++index) {
        const auto titleText = toWide(columnTitle(visibleColumns_[index]));
        LVCOLUMNW column{};
        column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM | LVCF_FMT;
        column.fmt = LVCFMT_LEFT;
        column.cx = index < widths.size() ? widths[index]
                                          : scale(defaultColumnWidth(visibleColumns_[index]), dpi_);
        column.iSubItem = static_cast<int>(index);
        column.pszText = const_cast<LPWSTR>(titleText.c_str());
        ListView_InsertColumn(list_, static_cast<int>(index), &column);
    }
    columnsDirty_ = true;
    displayedIds_.clear();
    lastColumnClientWidth_ = -1;
    applySortArrow();
}

void MainWindow::createStatusBar()
{
    status_ = CreateWindowExW(0, STATUSCLASSNAMEW, L"",
        WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP | SBT_TOOLTIPS, 0, 0, 0, 0, hwnd_, nullptr,
        instance_, nullptr);
    if (!status_) return;
    layoutStatusParts();
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

void MainWindow::createSearchOverlay()
{
    searchEdit_ = CreateWindowExW(0, L"EDIT", L"",
        WS_CHILD | ES_AUTOHSCROLL | WS_TABSTOP, 0, 0, 0, 0, hwnd_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SEARCH)), instance_, nullptr);
    searchClose_ = CreateWindowExW(0, L"BUTTON", L"×",
        WS_CHILD | BS_PUSHBUTTON | WS_TABSTOP, 0, 0, 0, 0, hwnd_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SEARCH_CLOSE)), instance_, nullptr);
    if (!searchEdit_ || !searchClose_) return;
    SendMessageW(searchEdit_, EM_SETCUEBANNER, TRUE,
        reinterpret_cast<LPARAM>(L"查找（名称/地址/路径）"));
    SetWindowSubclass(searchEdit_, searchSubclassStatic, 1, reinterpret_cast<DWORD_PTR>(this));
    ShowWindow(searchEdit_, SW_HIDE);
    ShowWindow(searchClose_, SW_HIDE);
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
    if (toolbar_) SendMessageW(toolbar_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    if (status_) SendMessageW(status_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    if (searchEdit_) SendMessageW(searchEdit_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    if (searchClose_) SendMessageW(searchClose_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
}

void MainWindow::updateLayoutMetrics()
{
    if (toolbar_ && showToolbar_) {
        SendMessageW(toolbar_, TB_AUTOSIZE, 0, 0);
        RECT rect{};
        GetWindowRect(toolbar_, &rect);
        toolbarHeight_ = rect.bottom - rect.top;
    } else {
        toolbarHeight_ = 0;
    }
}

void MainWindow::layoutControls()
{
    if (!hwnd_) return;
    RECT client{};
    GetClientRect(hwnd_, &client);
    updateLayoutMetrics();
    int statusHeight = 0;
    if (status_ && showStatusBar_) {
        SendMessageW(status_, WM_SIZE, 0, 0);
        RECT rect{};
        GetWindowRect(status_, &rect);
        statusHeight = rect.bottom - rect.top;
    }
    if (toolbar_ && showToolbar_)
        MoveWindow(toolbar_, 0, 0, client.right, toolbarHeight_, TRUE);
    else if (toolbar_)
        MoveWindow(toolbar_, 0, 0, 0, 0, TRUE);
    if (list_) {
        const int top = showToolbar_ ? toolbarHeight_ : 0;
        const int height = std::max(0, static_cast<int>(client.bottom) - top - statusHeight);
        MoveWindow(list_, 0, top, client.right, height, TRUE);
        applyColumnWidths();
    }
    if (searchVisible_ && searchEdit_ && searchClose_ && list_) {
        RECT listRect{};
        GetWindowRect(list_, &listRect);
        MapWindowPoints(nullptr, hwnd_, reinterpret_cast<POINT*>(&listRect), 2);
        int headerHeight = scale(25, dpi_);
        if (HWND header = ListView_GetHeader(list_)) {
            RECT headerRect{};
            if (GetWindowRect(header, &headerRect))
                headerHeight = headerRect.bottom - headerRect.top;
        }
        const int editHeight = scale(24, dpi_);
        const int closeWidth = editHeight;
        const int listWidth = listRect.right - listRect.left;
        const int width = std::min(scale(300, dpi_),
            std::max(scale(120, dpi_), listWidth - closeWidth - scale(12, dpi_)));
        const int left = listRect.right - width - closeWidth - scale(4, dpi_);
        const int top = listRect.top + headerHeight + scale(4, dpi_);
        MoveWindow(searchEdit_, left, top, width, editHeight, TRUE);
        MoveWindow(searchClose_, left + width, top, closeWidth, editHeight, TRUE);
        SetWindowPos(searchEdit_, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        SetWindowPos(searchClose_, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
}

void MainWindow::layoutStatusParts()
{
    if (!status_ || !showStatusBar_) return;
    RECT client{};
    GetClientRect(hwnd_, &client);
    const int width = client.right;
    if (width <= 0) return;
    int first = std::max(scale(140, dpi_), MulDiv(width, 28, 100));
    int second = first + std::max(scale(90, dpi_), MulDiv(width, 19, 100));
    int third = second + std::max(scale(120, dpi_), MulDiv(width, 26, 100));
    if (third > width - scale(60, dpi_)) third = std::max(first, width - scale(60, dpi_));
    const int parts[4] = {first, second, third, -1};
    SendMessageW(status_, SB_SETPARTS, 4, reinterpret_cast<LPARAM>(parts));
}

void MainWindow::applyColumnWidths()
{
    if (!list_) return;
    RECT client{};
    GetClientRect(list_, &client);
    const int width = client.right - client.left;
    if (width <= 0) return;
    if (width == lastColumnClientWidth_ && dpi_ == lastColumnDpi_) return;
    if (ListView_GetHeader(list_) == nullptr) return;
    const auto widths = columnWidths(columnSettings_, width, dpi_);
    for (std::size_t index = 0; index < widths.size(); ++index)
        ListView_SetColumnWidth(list_, static_cast<int>(index), widths[index]);
    lastColumnClientWidth_ = width;
    lastColumnDpi_ = dpi_;
}

void MainWindow::applySortArrow()
{
    if (!list_) return;
    HWND header = ListView_GetHeader(list_);
    if (!header) return;
    for (int index = 0; index < Header_GetItemCount(header); ++index) {
        HDITEMW item{};
        item.mask = HDI_FORMAT;
        if (!Header_GetItem(header, index, &item)) continue;
        item.fmt &= ~(HDF_SORTUP | HDF_SORTDOWN);
        if (index < static_cast<int>(visibleColumns_.size())
            && columnSettings_.sortColumn == visibleColumns_[static_cast<std::size_t>(index)])
            item.fmt |= columnSettings_.sortAscending ? HDF_SORTUP : HDF_SORTDOWN;
        Header_SetItem(header, index, &item);
    }
}

void MainWindow::setNotice(std::string text)
{
    noticeText_ = toWide(text);
    noticeAt_ = std::chrono::steady_clock::now();
    updateStatus();
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
    handleAddResults(snapshot);
    handleOutcomes(snapshot);
    syncList();
    updateStatus();
    updateActions();

    // A 401 means the site wants credentials. A remembered credential for that
    // host is tried first; otherwise the user is asked once per task.
    for (const auto& task : snapshot.tasks) {
        if (task.state != TaskState::Failed) continue;
        if (task.error.find("401") == std::string::npos) continue;
        if (!promptedCredentials_.insert(task.id).second) continue;
        if (const auto stored = worker_->storedCredential(hostOfUrl(task.source))) {
            worker_->postStart(task.id, false, stored->username, stored->password);
            setNotice("已使用记住的凭据重试");
            continue;
        }
        promptForCredentials(task.id);
        break;
    }
}

void MainWindow::setSelectedPriority(int priority)
{
    if (!worker_ || isClosing()) return;
    const auto row = rowForId(focusedId());
    if (!row) return;
    worker_->postPriority(row->id, priority);
    setNotice(priority > 0 ? "已提高优先级" : (priority < 0 ? "已降低优先级" : "已恢复普通优先级"));
}

void MainWindow::promptForCredentials(const std::string& taskId)
{
    CredentialPrompt prompt;
    // Only the host of the source is remembered, never the full URL.
    for (const auto& row : model_.rows()) {
        if (row.id != taskId) continue;
        prompt.host = hostOfUrl(row.source);
        break;
    }
    if (DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_CREDENTIALS), hwnd_,
            credentialDialogProc, reinterpret_cast<LPARAM>(&prompt)) != IDOK)
        return;
    if (prompt.username.empty()) return;
    if (prompt.remember && !prompt.host.empty())
        worker_->rememberCredential(prompt.host, prompt.username, prompt.password);
    worker_->postStart(taskId, false, prompt.username, prompt.password);
    setNotice(prompt.remember ? "已使用输入的凭据重试，并已记住"
                              : "已使用输入的凭据重试；凭据仅用于本次运行");
}

void MainWindow::syncList()
{
    if (!list_) return;
    std::vector<TaskRow> rows;
    rows.reserve(model_.rows().size());
    for (const auto& row : model_.rows()) {
        if (rowMatches(row, filter_, query_)) rows.push_back(row);
    }
    if (columnSettings_.sortColumn != ColumnId::Count) {
        sortRows(rows, columnSettings_.sortColumn,
            columnSettings_.sortAscending ? SortDirection::Ascending : SortDirection::Descending);
    }
    viewRows_ = std::move(rows);

    std::vector<std::string> ids;
    ids.reserve(viewRows_.size());
    for (const auto& row : viewRows_) ids.push_back(row.id);

    bool rebuild = columnsDirty_ || ids.size() != displayedIds_.size();
    if (!rebuild) {
        for (std::size_t index = 0; index < ids.size(); ++index) {
            if (ids[index] != displayedIds_[index]) {
                rebuild = true;
                break;
            }
        }
    }

    if (rebuild) {
        const auto oldSelected = selectedIds();
        const std::set<std::string, std::less<>> selectedSet(oldSelected.begin(), oldSelected.end());
        const auto oldFocus = focusedId();
        int oldFocusIndex = -1;
        for (std::size_t index = 0; index < displayedIds_.size(); ++index) {
            if (displayedIds_[index] == oldFocus) {
                oldFocusIndex = static_cast<int>(index);
                break;
            }
        }
        const int top = ListView_GetTopIndex(list_);
        std::string anchor;
        if (top >= 0 && static_cast<std::size_t>(top) < displayedIds_.size())
            anchor = displayedIds_[static_cast<std::size_t>(top)];
        const int perPage = std::max(1, ListView_GetCountPerPage(list_));
        const bool focusWasVisible = oldFocusIndex >= top && oldFocusIndex < top + perPage;

        suppressSelection_ = true;
        SendMessageW(list_, WM_SETREDRAW, FALSE, 0);
        ListView_DeleteAllItems(list_);
        for (std::size_t rowIndex = 0; rowIndex < viewRows_.size(); ++rowIndex) {
            const auto& row = viewRows_[rowIndex];
            std::wstring first = toWide(columnText(row, visibleColumns_.empty()
                    ? ColumnId::Name : visibleColumns_.front()));
            LVITEMW item{};
            item.mask = LVIF_TEXT;
            item.iItem = static_cast<int>(rowIndex);
            item.iSubItem = 0;
            item.pszText = const_cast<LPWSTR>(first.c_str());
            ListView_InsertItem(list_, &item);
            for (std::size_t column = 1; column < visibleColumns_.size(); ++column) {
                const auto text = toWide(columnText(row, visibleColumns_[column]));
                ListView_SetItemText(list_, static_cast<int>(rowIndex), static_cast<int>(column),
                    const_cast<LPWSTR>(text.c_str()));
            }
        }

        bool selectedAny = false;
        for (std::size_t index = 0; index < viewRows_.size(); ++index) {
            if (!selectedSet.contains(viewRows_[index].id)) continue;
            ListView_SetItemState(list_, static_cast<int>(index), LVIS_SELECTED, LVIS_SELECTED);
            selectedAny = true;
        }
        int focusIndex = viewIndexOf(oldFocus);
        if (focusIndex >= 0) {
            ListView_SetItemState(list_, focusIndex,
                LVIS_FOCUSED | LVIS_SELECTED, LVIS_FOCUSED | LVIS_SELECTED);
            selectedAny = true;
        } else if (selectedAny) {
            for (int index = 0; index < static_cast<int>(viewRows_.size()); ++index) {
                if (ListView_GetItemState(list_, index, LVIS_SELECTED)) {
                    ListView_SetItemState(list_, index, LVIS_FOCUSED, LVIS_FOCUSED);
                    break;
                }
            }
        }
        if (!pendingSelectTaskId_.empty()) {
            const int pending = viewIndexOf(pendingSelectTaskId_);
            if (pending >= 0) {
                ListView_SetItemState(list_, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
                ListView_SetItemState(list_, pending,
                    LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
                pendingSelectTaskId_.clear();
                selectedAny = true;
                focusIndex = pending;
                ListView_EnsureVisible(list_, pending, FALSE);
            }
        }
        if (!selectedAny && !initialSelectionDone_ && !viewRows_.empty()) {
            ListView_SetItemState(list_, 0, LVIS_SELECTED | LVIS_FOCUSED,
                LVIS_SELECTED | LVIS_FOCUSED);
            initialSelectionDone_ = true;
        }

        if (!viewRows_.empty()) initialSelectionDone_ = true;
        const int anchorIndex = viewIndexOf(anchor);
        if (anchorIndex >= 0) ListView_EnsureVisible(list_, anchorIndex, FALSE);
        if (focusWasVisible && focusIndex >= 0) ListView_EnsureVisible(list_, focusIndex, FALSE);
        if (!pendingSelectTaskId_.empty()) {
            const int pending = viewIndexOf(pendingSelectTaskId_);
            if (pending >= 0) ListView_EnsureVisible(list_, pending, FALSE);
        }

        displayedIds_ = std::move(ids);
        columnsDirty_ = false;
        SendMessageW(list_, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(list_, nullptr, TRUE);
        suppressSelection_ = false;
    } else {
        for (std::size_t rowIndex = 0; rowIndex < viewRows_.size(); ++rowIndex) {
            const auto& row = viewRows_[rowIndex];
            for (std::size_t column = 0; column < visibleColumns_.size(); ++column) {
                const auto wanted = toWide(columnText(row, visibleColumns_[column]));
                wchar_t existing[512]{};
                ListView_GetItemText(list_, static_cast<int>(rowIndex), static_cast<int>(column),
                    existing, static_cast<int>(std::size(existing)));
                if (wanted == existing) continue;
                if (column == 0) {
                    LVITEMW item{};
                    item.mask = LVIF_TEXT;
                    item.iItem = static_cast<int>(rowIndex);
                    item.iSubItem = 0;
                    item.pszText = const_cast<LPWSTR>(wanted.c_str());
                    ListView_SetItem(list_, &item);
                } else {
                    ListView_SetItemText(list_, static_cast<int>(rowIndex),
                        static_cast<int>(column), const_cast<LPWSTR>(wanted.c_str()));
                }
            }
        }
    }
    applyColumnWidths();
}

void MainWindow::updateStatus()
{
    if (!status_) return;
    const auto texts = model_.status(viewRows_.size(), filter_ != TaskFilter::All || !query_.empty());
    std::array<std::wstring, 4> parts{statusBarText(texts.tasks), statusBarText(texts.active),
        statusBarText(texts.download), statusBarText(texts.upload)};
    const auto now = std::chrono::steady_clock::now();
    const bool errorVisible = !lastErrorText_.empty() && now - lastErrorAt_ < std::chrono::seconds(10);
    const bool noticeVisible = !noticeText_.empty() && now - noticeAt_ < std::chrono::seconds(10);
    if (errorVisible) parts[3] = statusBarText(lastErrorText_);
    else if (noticeVisible) parts[3] = noticeText_;
    if (isClosing()) parts[3] = L"正在停止任务…";
    for (int index = 0; index < 4; ++index) {
        SendMessageW(status_, SB_SETTEXTW, static_cast<WPARAM>(index),
            reinterpret_cast<LPARAM>(parts[static_cast<std::size_t>(index)].c_str()));
    }
    statusTips_[0] = L"可见任务数 / 全部任务数（启用筛选或查找时）；分段 2-4 始终为全部任务合计。";
    statusTips_[1] = L"全部任务中的活动传输数。";
    statusTips_[2] = L"全部任务合计下载速度。";
    statusTips_[3] = L"全部任务合计上传速度；本版本引擎尚不上报上传。";
    for (int index = 0; index < 4; ++index) {
        SendMessageW(status_, SB_SETTIPTEXTW, static_cast<WPARAM>(index),
            reinterpret_cast<LPARAM>(statusTips_[static_cast<std::size_t>(index)].c_str()));
    }
}

void MainWindow::updateActions()
{
    const auto actions = model_.actionsForSelection(isClosing() ? std::vector<std::string>{}
                                                               : selectedIds());
    const bool interactive = !isClosing();
    auto enableMenu = [this](UINT id, bool enabled) {
        if (menu_) EnableMenuItem(menu_, id, MF_BYCOMMAND | (enabled ? MF_ENABLED : MF_GRAYED));
    };
    auto enableButton = [this](UINT id, bool enabled) {
        if (toolbar_) SendMessageW(toolbar_, TB_ENABLEBUTTON, id, MAKELONG(enabled ? TRUE : FALSE, 0));
    };
    enableMenu(ID_FILE_NEW, interactive);
    enableMenu(ID_FILE_BATCH, interactive);
    enableMenu(ID_FILE_OPEN_TORRENT, interactive);
    enableMenu(ID_EDIT_SELECT_ALL, interactive);
    enableMenu(ID_EDIT_FIND, interactive);
    enableMenu(ID_EDIT_PASTE, interactive);
    enableMenu(ID_EDIT_COPY_LINK, interactive && actions.copyLink);
    enableMenu(ID_EDIT_COPY_PATH, interactive && actions.copyPath);
    enableMenu(ID_VIEW_REFRESH, interactive);
    enableMenu(ID_VIEW_FILTER_ALL, interactive);
    enableMenu(ID_VIEW_FILTER_ACTIVE, interactive);
    enableMenu(ID_VIEW_FILTER_COMPLETED, interactive);
    enableMenu(ID_VIEW_FILTER_FAILED, interactive);
    enableMenu(ID_VIEW_TOOLBAR, interactive);
    enableMenu(ID_VIEW_STATUSBAR, interactive);
    enableMenu(ID_VIEW_COLUMNS, interactive);
    enableMenu(ID_TASK_START, interactive && actions.start);
    enableMenu(ID_TASK_PAUSE, interactive && actions.pause);
    enableMenu(ID_TASK_STOP, interactive && actions.stop);
    enableMenu(ID_TASK_REDOWNLOAD, false);
    enableMenu(ID_TASK_DELETE, interactive && actions.deleteTask);
    enableMenu(ID_TASK_DELETE_FILES, interactive && actions.selectedCount == 1);
    enableMenu(ID_TASK_OPEN_FILE, interactive && actions.openFile);
    enableMenu(ID_TASK_OPEN_FOLDER, interactive && actions.openFolder);
    enableMenu(ID_TASK_PROPERTIES, interactive && actions.selectedCount == 1);
    {
        const auto row = rowForId(focusedId());
        enableMenu(ID_TASK_VERIFY,
            interactive && actions.selectedCount == 1 && row && !row->torrentFiles.empty());
    }
    enableMenu(ID_TOOLS_OPTIONS, interactive);
    enableButton(ID_FILE_NEW, interactive);
    enableButton(ID_TASK_START, interactive && actions.start);
    enableButton(ID_TASK_PAUSE, interactive && actions.pause);
    enableButton(ID_TASK_STOP, interactive && actions.stop);
    enableButton(ID_TASK_DELETE, interactive && actions.deleteTask);
    enableButton(ID_TASK_OPEN_FOLDER, interactive && actions.openFolder);
    enableButton(ID_TOOLS_OPTIONS, false);
    if (toolbar_ && startImageEnabled_ >= 0) {
        SendMessageW(toolbar_, TB_CHANGEBITMAP, ID_TASK_START,
            MAKELPARAM(actions.start ? startImageEnabled_ : startImageDisabled_, 0));
    }
    if (menu_) {
        UINT filterId = ID_VIEW_FILTER_ALL;
        switch (filter_) {
        case TaskFilter::All: filterId = ID_VIEW_FILTER_ALL; break;
        case TaskFilter::Active: filterId = ID_VIEW_FILTER_ACTIVE; break;
        case TaskFilter::Completed: filterId = ID_VIEW_FILTER_COMPLETED; break;
        case TaskFilter::Failed: filterId = ID_VIEW_FILTER_FAILED; break;
        }
        CheckMenuRadioItem(menu_, ID_VIEW_FILTER_ALL, ID_VIEW_FILTER_FAILED, filterId, MF_BYCOMMAND);
        CheckMenuItem(menu_, ID_VIEW_TOOLBAR, MF_BYCOMMAND | (showToolbar_ ? MF_CHECKED : MF_UNCHECKED));
        CheckMenuItem(menu_, ID_VIEW_STATUSBAR,
            MF_BYCOMMAND | (showStatusBar_ ? MF_CHECKED : MF_UNCHECKED));
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
    const auto actions = model_.actionsForSelection(selectedIds());
    switch (command) {
    case ID_FILE_NEW: tooltipText_ = L"新建下载 (Ctrl+N)"; break;
    case ID_TASK_START:
        tooltipText_ = actions.start
            ? L"开始/继续（" + std::to_wstring(actions.startCount) + L" 个可用任务）"
            : L"开始/继续 — 当前选择没有可开始的任务";
        break;
    case ID_TASK_PAUSE:
        tooltipText_ = actions.pause
            ? L"暂停（" + std::to_wstring(actions.pauseCount)
                + L" 个任务）— 保留已下载数据，继续时从断点续传"
            : L"暂停 — 当前选择没有正在下载的任务";
        break;
    case ID_TASK_STOP:
        tooltipText_ = actions.stop
            ? L"停止（" + std::to_wstring(actions.stopCount)
                + L" 个可用任务）— 取消传输并保留已下载数据；不是可续传暂停"
            : L"停止 — 当前选择没有可停止的任务";
        break;
    case ID_TASK_DELETE:
        tooltipText_ = actions.deleteTask
            ? L"删除任务记录（" + std::to_wstring(actions.deleteCount) + L" 个）— 文件保留"
            : L"删除 — 请选择任务记录";
        break;
    case ID_TASK_OPEN_FOLDER:
        tooltipText_ = actions.openFolder
            ? L"打开所在文件夹"
            : L"打开文件夹 — 需选择一个保存位置存在的任务";
        break;
    case ID_TOOLS_OPTIONS:
        tooltipText_ = L"选项 — 尚未实现：设置界面将在后续版本提供";
        break;
    default: tooltipText_.clear(); break;
    }
    info.lpszText = tooltipText_.empty() ? const_cast<LPWSTR>(L"") : tooltipText_.data();
}

std::vector<std::string> MainWindow::selectedIds() const
{
    std::vector<std::string> result;
    if (!list_) return result;
    int index = -1;
    while ((index = ListView_GetNextItem(list_, index, LVNI_SELECTED)) != -1) {
        if (static_cast<std::size_t>(index) < viewRows_.size())
            result.push_back(viewRows_[static_cast<std::size_t>(index)].id);
    }
    return result;
}

std::string MainWindow::focusedId() const
{
    if (!list_) return {};
    const int index = ListView_GetNextItem(list_, -1, LVNI_FOCUSED);
    if (index < 0 || static_cast<std::size_t>(index) >= viewRows_.size()) return {};
    return viewRows_[static_cast<std::size_t>(index)].id;
}

int MainWindow::viewIndexOf(const std::string& taskId) const
{
    if (taskId.empty()) return -1;
    for (std::size_t index = 0; index < viewRows_.size(); ++index) {
        if (viewRows_[index].id == taskId) return static_cast<int>(index);
    }
    return -1;
}

const TaskRow* MainWindow::rowForId(const std::string& taskId) const
{
    const int index = viewIndexOf(taskId);
    if (index < 0) return nullptr;
    return &viewRows_[static_cast<std::size_t>(index)];
}

std::filesystem::path MainWindow::uiStatePath() const
{
    return uiStatePathFor(options_.databasePath);
}

void MainWindow::saveColumnSettings()
{
    std::string error;
    if (!litefetch::ui::saveColumnSettings(uiStatePath(), columnSettings_, error))
        setNotice("无法保存列设置：" + error);
}

void MainWindow::showSearch()
{
    if (!searchEdit_ || !searchClose_) return;
    searchVisible_ = true;
    layoutControls();
    ShowWindow(searchEdit_, SW_SHOW);
    ShowWindow(searchClose_, SW_SHOW);
    SetFocus(searchEdit_);
    SendMessageW(searchEdit_, EM_SETSEL, 0, -1);
}

void MainWindow::hideSearch()
{
    if (!searchEdit_) return;
    const auto previousFocus = focusedId();
    searchVisible_ = false;
    pendingSelectTaskId_ = previousFocus;
    query_.clear();
    SendMessageW(searchEdit_, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(L""));
    ShowWindow(searchEdit_, SW_HIDE);
    ShowWindow(searchClose_, SW_HIDE);
    syncList();
    if (list_) SetFocus(list_);
    const int index = viewIndexOf(previousFocus);
    if (index >= 0) {
        ListView_SetItemState(list_, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_SetItemState(list_, index,
            LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(list_, index, FALSE);
    }
    updateActions();
    updateStatus();
}

void MainWindow::updateSearchQuery()
{
    if (!searchEdit_) return;
    const int length = GetWindowTextLengthW(searchEdit_);
    std::wstring text(static_cast<std::size_t>(std::max(0, length)) + 1, L'\0');
    if (length > 0) GetWindowTextW(searchEdit_, text.data(), static_cast<int>(text.size()));
    text.resize(static_cast<std::size_t>(std::max(0, length)));
    try {
        query_ = utf8FromUtf16(text);
    } catch (const std::exception&) {
        query_.clear();
    }
    syncList();
    updateStatus();
    updateActions();
}

void MainWindow::applyFilter(TaskFilter filter)
{
    filter_ = filter;
    syncList();
    updateStatus();
    updateActions();
}

void MainWindow::showColumnChooser()
{
    ColumnSettings edited = columnSettings_;
    if (!showColumnDialog(hwnd_, edited)) return;
    columnSettings_ = sanitizeColumnSettings(std::move(edited));
    saveColumnSettings();
    rebuildColumns();
    syncList();
    updateStatus();
    updateActions();
}

void MainWindow::toggleToolbar()
{
    showToolbar_ = !showToolbar_;
    if (toolbar_) ShowWindow(toolbar_, showToolbar_ ? SW_SHOW : SW_HIDE);
    layoutControls();
    updateActions();
}

void MainWindow::toggleStatusBar()
{
    showStatusBar_ = !showStatusBar_;
    if (status_) ShowWindow(status_, showStatusBar_ ? SW_SHOW : SW_HIDE);
    layoutControls();
    if (showStatusBar_) layoutStatusParts();
    updateActions();
}

void MainWindow::selectTaskById(const std::string& taskId, bool clearFilters)
{
    if (taskId.empty()) return;
    if (clearFilters) {
        filter_ = TaskFilter::All;
        if (!query_.empty() || (searchEdit_ && GetWindowTextLengthW(searchEdit_) > 0)) {
            query_.clear();
            if (searchEdit_) {
                SendMessageW(searchEdit_, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(L""));
                ShowWindow(searchEdit_, SW_HIDE);
                ShowWindow(searchClose_, SW_HIDE);
                searchVisible_ = false;
            }
        }
    }
    pendingSelectTaskId_ = taskId;
    syncList();
    const int index = viewIndexOf(taskId);
    if (index >= 0) {
        suppressSelection_ = true;
        ListView_SetItemState(list_, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_SetItemState(list_, index,
            LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(list_, index, FALSE);
        suppressSelection_ = false;
    }
    updateActions();
    updateStatus();
}

void MainWindow::openNewDownload()
{
    if (!worker_ || isClosing()) return;
    const auto request = showNewDownloadDialog(hwnd_, options_.downloadDirectory);
    if (!request) return;
    if (request->source.rfind("magnet:", 0) == 0) {
        beginTorrentAdd(request->source);
        return;
    }
    if (request->switchToBatch) {
        openBatchAdd(request->batchText);
        return;
    }
    options_.downloadDirectory = request->directory;

    std::filesystem::path destination;
    try {
        destination = request->directory / pathFromUtf8(request->suggestedFileName);
    } catch (const std::exception&) {
        MessageBoxW(hwnd_, L"无法从下载地址推导文件名。", L"新建下载", MB_OK | MB_ICONERROR);
        return;
    }

    const TaskRow* existing = nullptr;
    for (const auto& row : model_.rows()) {
        if (row.source == request->source) {
            existing = &row;
            break;
        }
    }

    bool allowDuplicate = false;
    if (existing) {
        const int choice = MessageBoxW(hwnd_,
            (L"已存在相同下载地址的任务：\r\n" + toWide(existing->name)
                + L"\r\n\r\n“是”：定位现有任务\r\n“否”：另建一份（自动使用新文件名）\r\n"
                  L"“取消”：不添加").c_str(),
            L"已存在相同任务", MB_YESNOCANCEL | MB_ICONQUESTION | MB_DEFBUTTON1);
        if (choice == IDCANCEL) return;
        if (choice == IDYES) {
            selectTaskById(existing->id, true);
            setNotice("已定位现有任务：" + existing->name);
            return;
        }
        allowDuplicate = true;
        const auto stem = destination.stem().wstring();
        const auto extension = destination.extension().wstring();
        for (int counter = 2; counter < 10000; ++counter) {
            const auto candidate = destination.parent_path()
                / (stem + L" (" + std::to_wstring(counter) + L")" + extension);
            std::error_code error;
            if (std::filesystem::exists(candidate, error)) continue;
            bool usedByTask = false;
            for (const auto& row : model_.rows()) {
                try {
                    if (_wcsicmp(pathFromUtf8(row.destination).wstring().c_str(),
                            candidate.wstring().c_str()) == 0) {
                        usedByTask = true;
                        break;
                    }
                } catch (const std::exception&) {
                }
            }
            if (usedByTask) continue;
            destination = candidate;
            break;
        }
    }

    BatchLine line;
    line.line = 1;
    line.text = request->source;
    line.protocol = protocolLabel(line.text);
    line.valid = true;
    line.verdict = "有效";
    try {
        line.suggestedFileName = pathToUtf8(destination.filename());
    } catch (const std::exception&) {
        MessageBoxW(hwnd_, L"无法从下载地址推导文件名。", L"新建下载", MB_OK | MB_ICONERROR);
        return;
    }
    BatchAddRequest batch;
    batch.lines.push_back(std::move(line));
    batch.directory = request->directory;
    batch.autoStart = request->autoStart;
    batch.onlyValid = false;
    batch.allowDuplicate = allowDuplicate;
    submitBatch(batch, false);
}

void MainWindow::openBatchAdd(std::string initialText)
{
    if (!worker_ || isClosing()) return;
    const auto request = showBatchAddDialog(hwnd_, options_.downloadDirectory, initialText);
    if (!request) return;
    options_.downloadDirectory = request->directory;
    submitBatch(*request, true);
}

void MainWindow::submitBatch(const BatchAddRequest& request, bool showDialog)
{
    if (!worker_) return;
    PendingBatch batch;
    batch.id = nextBatchId_++;
    batch.showDialog = showDialog;
    for (const auto& line : request.lines) {
        PendingLine pending;
        pending.line = line;
        pending.submitted = line.valid;
        if (pending.submitted) {
            try {
                const auto destination = request.directory / pathFromUtf8(line.suggestedFileName);
                pending.destination = pathToUtf8(destination);
                ++batch.submittedCount;
            } catch (const std::exception& error) {
                pending.submitted = false;
                pending.error = "无法推导保存路径: " + std::string(error.what());
            }
        }
        batch.lines.push_back(std::move(pending));
    }
    if (batch.submittedCount == 0) return;
    const auto batchId = batch.id;
    pendingBatches_[batchId] = std::move(batch);
    const auto& stored = pendingBatches_[batchId];
    for (const auto& line : stored.lines) {
        if (!line.submitted) continue;
        worker_->postAdd(line.line.text, line.destination, request.autoStart, batchId,
            line.line.line, request.allowDuplicate);
    }
}

void MainWindow::handleAddResults(const WorkerSnapshot& snapshot)
{
    for (const auto& result : snapshot.addResults) {
        if (result.sequence <= lastAddSequence_) continue;
        const auto found = pendingBatches_.find(result.batchId);
        if (found == pendingBatches_.end()) continue;
        auto& batch = found->second;
        PendingLine* pendingLine = nullptr;
        for (auto& line : batch.lines) {
            if (line.line.line == result.line) {
                pendingLine = &line;
                break;
            }
        }
        if (!pendingLine) continue;
        pendingLine->done = true;
        pendingLine->ok = result.ok;
        pendingLine->existing = result.existing;
        pendingLine->taskId = result.taskId;
        pendingLine->error = result.error;
        ++batch.completedCount;

        if (!batch.showDialog) {
            if (result.ok) {
                pendingSelectTaskId_ = result.taskId;
                if (result.existing)
                    setNotice("已定位现有任务：" + pendingLine->line.text);
                else
                    setNotice("已添加任务：" + pendingLine->line.text);
                if (!result.taskId.empty()) selectTaskById(result.taskId, true);
            } else {
                lastErrorText_ = "添加失败: " + result.error;
                lastErrorAt_ = std::chrono::steady_clock::now();
                MessageBoxW(hwnd_, toWide("添加失败: " + result.error).c_str(), L"添加任务",
                    MB_OK | MB_ICONERROR);
            }
            pendingBatches_.erase(found);
            continue;
        }
        if (batch.completedCount >= batch.submittedCount) {
            finalizeBatch(batch);
            pendingBatches_.erase(found);
            continue;
        }
    }
    lastAddSequence_ = snapshot.addResultSequence;
}

void MainWindow::finalizeBatch(PendingBatch& batch)
{
    std::vector<BatchResultRow> rows;
    rows.reserve(batch.lines.size());
    for (const auto& line : batch.lines) {
        BatchResultRow row;
        row.line = line.line.line;
        row.address = redactAddress(line.line.text);
        if (!line.submitted) {
            row.result = "未提交：" + (line.error.empty() ? line.line.verdict : line.error);
        } else if (line.ok) {
            row.result = line.existing ? "已定位现有任务" : "已添加";
        } else {
            row.result = "提交失败：" + (line.error.empty() ? "unknown error" : line.error);
        }
        rows.push_back(std::move(row));
    }
    showBatchResultDialog(hwnd_, rows);
}

void MainWindow::handleOutcomes(const WorkerSnapshot& snapshot)
{
    for (const auto& outcome : snapshot.outcomes) {
        if (outcome.sequence <= lastOutcomeSequence_) continue;
        if (outcome.action != "delete") continue;
        if (pendingDeleteIds_.erase(outcome.taskId) == 0) continue;
        if (!outcome.ok)
            pendingDeleteFailures_.push_back(outcome.taskId + ": " + outcome.error);
        if (pendingDeleteIds_.empty()) {
            if (pendingDeleteFailures_.empty()) {
                setNotice("已删除 " + std::to_string(pendingDeleteTotal_)
                    + " 个任务记录（文件保留）。");
            } else {
                std::wstring text = L"以下任务记录未能删除：\r\n";
                for (const auto& failure : pendingDeleteFailures_)
                    text += toWide(failure) + L"\r\n";
                MessageBoxW(hwnd_, text.c_str(), L"删除任务记录失败", MB_OK | MB_ICONERROR);
                setNotice("部分任务记录删除失败；文件未删除。");
            }
            pendingDeleteFailures_.clear();
            pendingDeleteTotal_ = 0;
        }
    }
    lastOutcomeSequence_ = snapshot.outcomeSequence;
}

void MainWindow::verifySelected()
{
    if (!worker_ || isClosing()) return;
    const auto row = rowForId(focusedId());
    if (!row || row->torrentFiles.empty()) {
        setNotice("重新校验只适用于种子任务");
        return;
    }
    // A full recheck reads every piece from disk and fetches again what does not
    // match, so damaged files are repaired instead of reported as complete.
    worker_->postStart(row->id, false, {}, {}, true);
    setNotice("已开始重新校验：" + row->name);
}

void MainWindow::startSelected()
{
    if (!worker_ || isClosing()) return;
    const auto ids = selectedIds();
    std::size_t count = 0;
    for (const auto& id : ids) {
        const auto actions = model_.actionsFor(id);
        if (!actions.start) continue;
        // Stored bytes that no longer match the resource need an explicit yes:
        // starting over discards them.
        const auto row = rowForId(id);
        if (row && row->restartRequired) {
            const auto answer = MessageBoxW(hwnd_,
                L"已下载的片段来自旧版本的资源，无法继续续传。\n\n"
                L"重新下载会删除这些片段并从头开始。是否继续？",
                L"重新下载", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
            if (answer != IDYES) continue;
            worker_->postStart(id, true);
            ++count;
            continue;
        }
        worker_->postStart(id);
        ++count;
    }
    if (count > 0)
        setNotice("已请求开始 " + std::to_string(count) + " 个任务");
}

void MainWindow::pauseSelected()
{
    if (!worker_ || isClosing()) return;
    const auto ids = selectedIds();
    std::size_t count = 0;
    for (const auto& id : ids) {
        const auto actions = model_.actionsFor(id);
        if (!actions.pause) continue;
        worker_->postPause(id);
        ++count;
    }
    if (count > 0)
        setNotice("已请求暂停 " + std::to_string(count) + " 个任务；继续时从已下载的位置续传");
}

void MainWindow::stopSelected()
{
    if (!worker_ || isClosing()) return;
    const auto ids = selectedIds();
    std::size_t count = 0;
    for (const auto& id : ids) {
        const auto actions = model_.actionsFor(id);
        if (!actions.stop) continue;
        worker_->postCancel(id);
        ++count;
    }
    if (count > 0)
        setNotice("已请求停止 " + std::to_string(count)
            + " 个任务；停止后仍可从已下载位置续传");
}

void MainWindow::showProperties()
{
    const auto row = rowForId(focusedId());
    if (!row) return;
    if (!row->torrentFiles.empty()) {
        TorrentDetailsState state;
        state.name = toWide(row->name);
        state.identity = toWide(row->id.substr(0, 16)) + L"…";
        state.state = toWide(row->status);
        if (!row->sha256.empty())
            state.state += row->verified ? L"（已校验）" : L"（校验值未通过或尚未校验）";
        state.size = toWide(row->size);
        state.savePath = toWide(row->savePath);
        state.privateText = row->torrentPrivate ? L"是（不可修改追踪器）" : L"否";
        state.done = toWide(row->progress);
        state.progress = toWide(row->progress);
        state.rate = toWide(row->speed);
        state.uploadRate = L"——";
        state.remaining = toWide(row->remaining);
        state.peers = row->connections.empty() ? L"——" : toWide(row->connections);
        state.protocol = toWide(row->protocol);
        state.seeds = L"——";
        state.uploaded = toWide(formatSize(row->uploadedBytes));
        state.seedMode = row->seedMode;
        state.seedRatioMilli = row->seedRatioMilli;
        state.seedMinutes = row->seedMinutes;
        state.files = row->torrentFiles;
        state.priorities = row->filePriorities;
        state.priorities.resize(state.files.size(), 1);
        state.trackers = row->trackers;
        state.privateTorrent = row->torrentPrivate;
        const auto taskId = row->id;
        state.apply = [this, taskId](const TorrentDetailsState& edited) {
            std::vector<int> files;
            std::vector<int> priorities;
            for (std::size_t index = 0; index < edited.files.size(); ++index) {
                const int priority = index < edited.priorities.size() ? edited.priorities[index] : 1;
                if (priority <= 0) continue;
                files.push_back(static_cast<int>(index));
                priorities.push_back(priority);
            }
            worker_->postUpdateTorrent(taskId, files, priorities, edited.trackers,
                edited.seedMode, edited.seedRatioMilli, edited.seedMinutes);
            setNotice("已应用种子设置：" + std::to_string(files.size()) + " 个文件");
        };
        DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_TORRENT_DETAILS), hwnd_,
            torrentDetailsDialogProc, reinterpret_cast<LPARAM>(&state));
        return;
    }
    PropertyFields fields;
    fields.name = toWide(row->name);
    fields.source = toWide(row->address);
    fields.destination = toWide(row->savePath);
    fields.size = toWide(row->size);
    fields.state = toWide(row->status);
    if (!row->sha256.empty()) {
        fields.state += row->verified ? L"（已校验）" : L"（校验值未通过或尚未校验）";
    }
    fields.protocol = toWide(row->protocol);

    // Connection facts come from the task's real protocol: FTP uses a control
    // connection plus a passive data connection the server opens; HTTP(S) uses
    // one connection per request, several when segmentation is active.
    std::string host;
    const auto scheme = row->address.find("://");
    if (scheme != std::string::npos) {
        const auto start = scheme + 3;
        const auto end = row->address.find_first_of("/?#", start);
        host = row->address.substr(start, end == std::string::npos ? std::string::npos : end - start);
    }
    const bool ftp = row->protocol == "FTP" || row->protocol == "FTPS";
    const std::string connection = ftp
        ? "控制连接：" + host + "\n数据连接：被动模式（服务器分配端口）\n认证：本次运行输入，未保存"
        : "连接：" + host + "\n每个请求一条连接；分段下载时按站点上限并行";
    fields.connection = toWide(connection);

    DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_PROPERTIES), hwnd_,
        propertiesDialogProc, reinterpret_cast<LPARAM>(&fields));
}

void MainWindow::deleteSelected()
{
    if (!worker_ || isClosing()) return;
    if (deleteDialog_) {
        SetForegroundWindow(deleteDialog_);
        return;
    }
    const auto ids = selectedIds();
    if (ids.empty()) return;
    const auto actions = model_.actionsForSelection(ids);
    if (actions.deleteCount == 0) return;
    pendingDeleteSelection_ = ids;
    deleteDialog_ = CreateDialogParamW(GetModuleHandleW(nullptr),
        MAKEINTRESOURCEW(IDD_DELETE_TASK), hwnd_, deleteDialogProc,
        reinterpret_cast<LPARAM>(this));
    if (!deleteDialog_) {
        pendingDeleteSelection_.clear();
        return;
    }
    EnableWindow(hwnd_, FALSE);
    ShowWindow(deleteDialog_, SW_SHOW);
}

void MainWindow::finishDeleteDialog(bool confirmed)
{
    HWND dialog = deleteDialog_;
    deleteDialog_ = nullptr;
    if (dialog) DestroyWindow(dialog);
    if (!isClosing()) {
        EnableWindow(hwnd_, TRUE);
        if (list_) SetFocus(list_);
    }
    if (!confirmed) {
        pendingDeleteSelection_.clear();
        return;
    }
    const auto ids = std::move(pendingDeleteSelection_);
    pendingDeleteSelection_.clear();
    pendingDeleteIds_.clear();
    pendingDeleteFailures_.clear();
    pendingDeleteTotal_ = ids.size();
    for (const auto& id : ids) pendingDeleteIds_.insert(id);
    for (const auto& id : ids) worker_->postDelete(id);
    setNotice("正在停止并删除 " + std::to_string(ids.size())
        + " 个任务记录（文件保留）。");
}

void MainWindow::openFileSelected()
{
    const auto ids = selectedIds();
    if (ids.size() != 1) {
        MessageBoxW(hwnd_, L"请选择一个已完成的任务以打开文件。", L"打开文件",
            MB_OK | MB_ICONWARNING);
        return;
    }
    const auto* row = rowForId(ids.front());
    if (!row || row->state != TaskState::Completed) {
        MessageBoxW(hwnd_, L"只有已完成的任务才能打开文件；未完成或失败的任务不会执行。",
            L"打开文件", MB_OK | MB_ICONWARNING);
        return;
    }
    std::filesystem::path path;
    try {
        path = pathFromUtf8(row->destination);
    } catch (const std::exception&) {
        MessageBoxW(hwnd_, L"任务保存路径无效。", L"打开文件", MB_OK | MB_ICONERROR);
        return;
    }
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error)) {
        MessageBoxW(hwnd_, (L"文件不存在或无法访问：\r\n" + path.wstring()).c_str(),
            L"打开文件", MB_OK | MB_ICONWARNING);
        return;
    }
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_FLAG_NO_UI;
    info.hwnd = hwnd_;
    info.lpVerb = L"open";
    info.lpFile = path.c_str();
    info.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&info)) {
        const DWORD code = GetLastError();
        std::wstring reason = shellErrorText(static_cast<INT_PTR>(code));
        MessageBoxW(hwnd_, (L"无法打开文件：\r\n" + path.wstring() + L"\r\n\r\n" + reason).c_str(),
            L"打开文件", MB_OK | MB_ICONERROR);
    }
}

void MainWindow::openFolderSelected()
{
    const auto ids = selectedIds();
    if (ids.size() != 1) {
        MessageBoxW(hwnd_, L"请选择一个任务以打开所在文件夹。", L"打开文件",
            MB_OK | MB_ICONWARNING);
        return;
    }
    const auto* row = rowForId(ids.front());
    if (!row || row->destination.empty()) {
        MessageBoxW(hwnd_, L"所选任务没有保存位置。", L"打开文件", MB_OK | MB_ICONWARNING);
        return;
    }
    std::filesystem::path destination;
    try {
        destination = pathFromUtf8(row->destination);
    } catch (const std::exception&) {
        MessageBoxW(hwnd_, L"任务保存路径无效。", L"打开文件", MB_OK | MB_ICONERROR);
        return;
    }
    const auto commandLine = explorerCommandLine(destination);
    if (!commandLine) {
        MessageBoxW(hwnd_, (L"文件夹不存在或无法访问：\r\n" + destination.parent_path().wstring()).c_str(),
            L"打开文件", MB_OK | MB_ICONWARNING);
        return;
    }
    const auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(hwnd_, L"open", L"explorer.exe",
        commandLine->c_str(), nullptr, SW_SHOWNORMAL));
    if (result <= 32) {
        MessageBoxW(hwnd_, (L"无法启动资源管理器。\r\n" + shellErrorText(result)).c_str(),
            L"打开文件", MB_OK | MB_ICONERROR);
    }
}

void MainWindow::copyText(const std::wstring& text, std::string notice)
{
    bool opened = false;
    for (int attempt = 0; attempt < 10 && !opened; ++attempt) {
        opened = OpenClipboard(hwnd_) != FALSE;
        if (!opened) Sleep(20);
    }
    if (!opened) {
        setNotice("无法访问剪贴板，请重试。");
        return;
    }
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
    setNotice(std::move(notice));
}

void MainWindow::copySelectedLinks()
{
    const auto ids = selectedIds();
    const auto actions = model_.actionsForSelection(ids);
    if (actions.copyLinkCount == 0) return;
    std::wstring text;
    for (const auto& id : ids) {
        const auto* row = rowForId(id);
        if (!row || row->source.empty()) continue;
        const auto redacted = redactAddress(row->source);
        if (!text.empty()) text += L"\r\n";
        text += toWide(redacted);
    }
    copyText(text, "已复制 " + std::to_string(actions.copyLinkCount)
        + " 个下载地址（凭据已隐藏）。");
}

void MainWindow::copySelectedPaths()
{
    const auto ids = selectedIds();
    const auto actions = model_.actionsForSelection(ids);
    if (actions.copyPathCount == 0) return;
    std::wstring text;
    for (const auto& id : ids) {
        const auto* row = rowForId(id);
        if (!row || row->destination.empty()) continue;
        if (!text.empty()) text += L"\r\n";
        text += toWide(row->destination);
    }
    copyText(text, "已复制 " + std::to_string(actions.copyPathCount) + " 个文件路径。");
}

void MainWindow::selectAllVisible()
{
    if (isEditFocus()) {
        if (HWND focus = GetFocus()) SendMessageW(focus, EM_SETSEL, 0, -1);
        return;
    }
    if (!list_) return;
    suppressSelection_ = true;
    ListView_SetItemState(list_, -1, LVIS_SELECTED, LVIS_SELECTED);
    if (!viewRows_.empty())
        ListView_SetItemState(list_, 0, LVIS_FOCUSED, LVIS_FOCUSED);
    suppressSelection_ = false;
    updateActions();
}

void MainWindow::pasteFromClipboard()
{
    const auto text = readClipboardText(hwnd_);
    if (text.empty()) return;
    try {
        const auto utf8 = utf8FromUtf16(text);
        if (analyzeBatchInput(utf8).lines.empty()) return;
        openBatchAdd(utf8);
    } catch (const std::exception&) {
    }
}

void MainWindow::handleDrop(HDROP drop)
{
    if (!drop) return;
    const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    std::vector<std::filesystem::path> paths;
    for (UINT index = 0; index < count; ++index) {
        const UINT length = DragQueryFileW(drop, index, nullptr, 0);
        std::wstring buffer(static_cast<std::size_t>(length) + 1, L'\0');
        if (DragQueryFileW(drop, index, buffer.data(), static_cast<UINT>(buffer.size())) > 0) {
            buffer.resize(length);
            paths.emplace_back(buffer);
        }
    }
    DragFinish(drop);

    std::vector<std::filesystem::path> torrentPaths;
    bool otherFile = false;
    std::string text;
    for (const auto& path : paths) {
        auto extension = path.extension().wstring();
        std::transform(extension.begin(), extension.end(), extension.begin(),
            [](wchar_t character) { return static_cast<wchar_t>(std::towlower(character)); });
        if (extension == L".torrent") {
            torrentPaths.push_back(path);
        } else if (extension == L".txt") {
            std::string fileText;
            if (readTextFile(path, fileText)) {
                if (!text.empty()) text += "\n";
                text += fileText;
            } else {
                otherFile = true;
            }
        } else {
            otherFile = true;
        }
    }
    if (!text.empty()) openBatchAdd(text);
    for (const auto& path : torrentPaths) beginTorrentAdd(utf8FromUtf16(path.wstring()));
    if (otherFile) {
        MessageBoxW(hwnd_,
            L"拖放的本地文件不会创建下载任务。\r\n请拖入 .txt 地址列表，或使用“批量添加”。",
            L"拖放", MB_OK | MB_ICONINFORMATION);
    }
}

void MainWindow::openTorrentBoundary()
{
    wchar_t path[MAX_PATH]{};
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = hwnd_;
    dialog.lpstrFilter = L"种子文件 (*.torrent)\\0*.torrent\\0所有文件\\0*.*\\0";
    dialog.lpstrFile = path;
    dialog.nMaxFile = MAX_PATH;
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameW(&dialog)) return;
    beginTorrentAdd(utf8FromUtf16(path));
}

void MainWindow::beginTorrentAdd(const std::string& source)
{
    if (!worker_ || isClosing()) return;
    if (torrentFetchActive_) {
        setNotice("已有种子正在读取元数据，请稍候");
        return;
    }
    // Metadata is fetched on its own thread: a magnet with no peers can take
    // seconds, and the window must stay responsive while it waits.
    torrentFetchActive_ = true;
    torrentFetchCancel_ = std::make_shared<std::atomic_bool>(false);
    const HWND window = hwnd_;
    const auto directory = options_.downloadDirectory.string();
    std::thread([window, source, directory, cancel = torrentFetchCancel_] {
        auto* result = new TorrentFetchResult{
            btFetchMetadata(source, directory, 20000, *cancel), source};
        if (!PostMessageW(window, appTorrentMetadataMessage, 0, reinterpret_cast<LPARAM>(result)))
            delete result;
    }).detach();
}

void MainWindow::handleTorrentMetadata(BtMetadataResult metadata, const std::string& source)
{
    torrentFetchActive_ = false;
    if (!metadata.ok) {
        MessageBoxW(hwnd_, toWide("无法读取种子元数据：" + metadata.error).c_str(), L"打开种子",
            MB_OK | MB_ICONWARNING);
        return;
    }
    TorrentSelection selection;
    selection.name = toWide(metadata.metadata.name.empty() ? source : metadata.metadata.name);
    selection.files = metadata.metadata.files;
    const auto answer = DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_TORRENT_FILES),
        hwnd_, torrentFilesDialogProc, reinterpret_cast<LPARAM>(&selection));
    if (answer != IDOK) return;
    if (selection.selected.empty()) {
        setNotice("未勾选任何文件，未创建 BT 任务");
        return;
    }
    std::vector<TorrentFileInfo> files;
    for (const auto& file : metadata.metadata.files)
        files.push_back(TorrentFileInfo{file.path, file.size});
    worker_->postAdd(source, options_.downloadDirectory.string(), true, 0, 0, false,
        selection.selected, metadata.metadata.infoHash, std::move(files),
        metadata.metadata.isPrivate);
    setNotice("已创建 BT 任务：" + std::to_string(selection.selected.size()) + " 个文件");
}

void MainWindow::showCapabilities()
{
    MessageBoxW(hwnd_,
        L"本版本已实现：\n"
        L"• 新建 HTTP/HTTPS 下载与 FTP 下载（真实传输、真实文件）\n"
        L"• 暂停/续传（保留已下载片段）与资源变化后的确认重下\n"
        L"• BT/磁力：先读元数据，勾选文件后再下载\n"
        L"• 批量添加（每行一个地址，逐行验证/报告；当前仅提交 HTTP/HTTPS）\n"
        L"• 多选、临时查找（Ctrl+F）、状态筛选、数值/时间排序\n"
        L"• 列顺序/宽度/显示与排序持久化，至少保留“名称”列\n"
        L"• 开始等待中/失败任务、停止等待中/下载中任务\n"
        L"• 删除任务记录（先取消并等待，文件保留）、打开文件/所在文件夹、复制链接/路径\n"
        L"• 相同来源默认定位现有任务，可明确另建一份；显示/复制会隐藏 URL 凭据\n"
        L"\n"
        L"尚未实现，因此按钮保持禁用：\n"
        L"• ed2k（#12）：引擎接入前不会假报支持\n"
        L"• 删除任务及文件、重新下载、选项 — 后续版本\n",
        L"本版本能力说明", MB_OK | MB_ICONINFORMATION);
}

void MainWindow::showAbout()
{
    MessageBoxW(hwnd_,
        L"LiteFetch 0.1\n\n"
        L"原生 Win32 下载管理器（开发版本）\n"
        L"HTTP 下载（HTTPS 由传输库支持，本票未验收）、真实任务列表、批量/查找/多选与列设置；无 Qt 依赖。",
        L"关于 LiteFetch", MB_OK | MB_ICONINFORMATION);
}

void MainWindow::beginClose()
{
    if (closing_) return;
    if (torrentFetchCancel_) torrentFetchCancel_->store(true);
    if (deleteDialog_) finishDeleteDialog(false);
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

void MainWindow::setShutdownStatus()
{
    if (status_) SendMessageW(status_, SB_SETTEXTW, 3, reinterpret_cast<LPARAM>(L"正在停止任务…"));
}

} // namespace litefetch::ui