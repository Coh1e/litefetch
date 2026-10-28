#include "batch_dialog.h"

#include "resource.h"
#include "text_utf8.h"

#include <commctrl.h>
#include <shlobj.h>

#include <system_error>

namespace litefetch::ui {
namespace {
struct DialogContext {
    std::filesystem::path initialDirectory;
    std::string initialText;
    BatchInput input;
    bool onlyValid = false;
    std::optional<BatchAddRequest> result;
};

std::wstring windowText(HWND control)
{
    const int length = GetWindowTextLengthW(control);
    if (length <= 0) return {};
    std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
    const int copied = GetWindowTextW(control, text.data(), static_cast<int>(text.size()));
    if (copied <= 0) return {};
    text.resize(static_cast<std::size_t>(copied));
    return text;
}

std::string controlUtf8(HWND dialog, int id)
{
    try {
        return utf8FromUtf16(windowText(GetDlgItem(dialog, id)));
    } catch (const std::exception&) {
        return {};
    }
}

void centerOnOwner(HWND dialog, HWND owner)
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

void browseForFolder(HWND dialog)
{
    BROWSEINFOW browse{};
    browse.hwndOwner = dialog;
    browse.lpszTitle = L"选择保存文件夹";
    browse.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    PIDLIST_ABSOLUTE item = SHBrowseForFolderW(&browse);
    if (!item) return;
    wchar_t path[MAX_PATH]{};
    if (SHGetPathFromIDListW(item, path)) SetDlgItemTextW(dialog, IDC_BATCH_DIR, path);
    CoTaskMemFree(item);
}

std::wstring verdictText(const BatchLine& line)
{
    try {
        return utf16FromUtf8(line.verdict);
    } catch (const std::exception&) {
        return L"";
    }
}

void fillResults(HWND dialog, const BatchInput& input)
{
    HWND list = GetDlgItem(dialog, IDC_BATCH_LIST);
    SendMessageW(list, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(list);
    for (std::size_t index = 0; index < input.lines.size(); ++index) {
        const auto& line = input.lines[index];
        const auto text = [](const std::string& value) {
            try {
                return utf16FromUtf8(value);
            } catch (const std::exception&) {
                return std::wstring{};
            }
        };
        const auto address = text(line.text);
        const auto protocol = text(line.protocol);
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = static_cast<int>(index);
        item.pszText = const_cast<LPWSTR>(address.c_str());
        ListView_InsertItem(list, &item);
        ListView_SetItemText(list, static_cast<int>(index), 1, const_cast<LPWSTR>(protocol.c_str()));
        const auto verdict = verdictText(line);
        ListView_SetItemText(list, static_cast<int>(index), 2, const_cast<LPWSTR>(verdict.c_str()));
    }
    SendMessageW(list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(list, nullptr, TRUE);
}

void refresh(HWND dialog, DialogContext& context)
{
    context.input = analyzeBatchInput(controlUtf8(dialog, IDC_BATCH_URLS));
    fillResults(dialog, context.input);
    try {
        SetDlgItemTextW(dialog, IDC_BATCH_SUMMARY,
            utf16FromUtf8(batchSummary(context.input)).c_str());
    } catch (const std::exception&) {
        SetDlgItemTextW(dialog, IDC_BATCH_SUMMARY, L"");
    }

    const std::size_t total = context.input.lines.size();
    const std::size_t valid = context.input.validCount;
    const std::size_t invalid = context.input.invalidCount;
    const bool partial = invalid > 0 && valid > 0;
    HWND checkbox = GetDlgItem(dialog, IDC_BATCH_ONLY_VALID);
    EnableWindow(checkbox, partial);
    if (!partial) {
        CheckDlgButton(dialog, IDC_BATCH_ONLY_VALID, BST_UNCHECKED);
        context.onlyValid = false;
    } else {
        context.onlyValid = IsDlgButtonChecked(dialog, IDC_BATCH_ONLY_VALID) == BST_CHECKED;
    }
    const auto label = L"仅添加 " + std::to_wstring(valid) + L" 条有效链接（其余 "
        + std::to_wstring(invalid) + L" 条不添加）";
    SetDlgItemTextW(dialog, IDC_BATCH_ONLY_VALID, label.c_str());

    const bool canAdd = total > 0 && (invalid == 0 || (partial && context.onlyValid));
    EnableWindow(GetDlgItem(dialog, IDOK), canAdd);
}

bool accept(HWND dialog, DialogContext& context)
{
    refresh(dialog, context);
    if (context.input.lines.empty()) {
        MessageBoxW(dialog, L"请输入至少一个下载地址（每行一个）。", L"批量添加",
            MB_OK | MB_ICONWARNING);
        SetFocus(GetDlgItem(dialog, IDC_BATCH_URLS));
        return false;
    }
    if (context.input.invalidCount > 0 && !context.onlyValid) {
        MessageBoxW(dialog,
            L"存在无效或暂不支持的地址。请修正它们，或勾选“仅添加有效链接”。",
            L"批量添加", MB_OK | MB_ICONWARNING);
        return false;
    }
    const std::wstring directoryText = windowText(GetDlgItem(dialog, IDC_BATCH_DIR));
    if (directoryText.empty()) {
        MessageBoxW(dialog, L"请选择统一保存文件夹。", L"批量添加", MB_OK | MB_ICONWARNING);
        SetFocus(GetDlgItem(dialog, IDC_BATCH_DIR));
        return false;
    }
    std::filesystem::path directory(directoryText);
    if (!directory.is_absolute()) {
        MessageBoxW(dialog, L"保存目录必须是绝对路径。", L"批量添加", MB_OK | MB_ICONWARNING);
        SetFocus(GetDlgItem(dialog, IDC_BATCH_DIR));
        return false;
    }
    std::error_code error;
    if (!std::filesystem::is_directory(directory, error)) {
        MessageBoxW(dialog, L"保存文件夹不存在或无法访问，请检查路径与权限。", L"批量添加",
            MB_OK | MB_ICONERROR);
        SetFocus(GetDlgItem(dialog, IDC_BATCH_DIR));
        return false;
    }

    context.result = BatchAddRequest{context.input.lines, directory,
        IsDlgButtonChecked(dialog, IDC_BATCH_AUTO_START) == BST_CHECKED, context.onlyValid};
    return true;
}

INT_PTR CALLBACK dialogProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam)
{
    auto* context = reinterpret_cast<DialogContext*>(GetWindowLongPtrW(dialog, GWLP_USERDATA));
    switch (message) {
    case WM_INITDIALOG: {
        context = reinterpret_cast<DialogContext*>(lParam);
        SetWindowLongPtrW(dialog, GWLP_USERDATA, lParam);
        if (!context) return FALSE;
        HWND list = GetDlgItem(dialog, IDC_BATCH_LIST);
        ListView_SetExtendedListViewStyle(list,
            LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
        LVCOLUMNW column{};
        column.mask = LVCF_TEXT | LVCF_WIDTH;
        column.pszText = const_cast<LPWSTR>(L"地址");
        column.cx = 240;
        ListView_InsertColumn(list, 0, &column);
        column.pszText = const_cast<LPWSTR>(L"协议");
        column.cx = 80;
        ListView_InsertColumn(list, 1, &column);
        column.pszText = const_cast<LPWSTR>(L"验证结果");
        column.cx = 260;
        ListView_InsertColumn(list, 2, &column);
        if (!context->initialText.empty()) {
            try {
                SetDlgItemTextW(dialog, IDC_BATCH_URLS,
                    utf16FromUtf8(context->initialText).c_str());
            } catch (const std::exception&) {
            }
        }
        if (!context->initialDirectory.empty())
            SetDlgItemTextW(dialog, IDC_BATCH_DIR, context->initialDirectory.c_str());
        CheckDlgButton(dialog, IDC_BATCH_AUTO_START, BST_CHECKED);
        centerOnOwner(dialog, GetParent(dialog));
        refresh(dialog, *context);
        SetFocus(GetDlgItem(dialog, IDC_BATCH_URLS));
        return FALSE;
    }
    case WM_COMMAND:
        if (!context) return FALSE;
        switch (LOWORD(wParam)) {
        case IDC_BATCH_URLS:
            if (HIWORD(wParam) == EN_CHANGE) {
                refresh(dialog, *context);
                return TRUE;
            }
            return FALSE;
        case IDC_BATCH_ONLY_VALID:
            if (HIWORD(wParam) == BN_CLICKED) {
                refresh(dialog, *context);
                return TRUE;
            }
            return FALSE;
        case IDC_BATCH_BROWSE:
            if (HIWORD(wParam) == BN_CLICKED) {
                browseForFolder(dialog);
                return TRUE;
            }
            return FALSE;
        case IDOK:
            if (accept(dialog, *context)) EndDialog(dialog, IDOK);
            return TRUE;
        case IDCANCEL:
            EndDialog(dialog, IDCANCEL);
            return TRUE;
        default:
            return FALSE;
        }
    case WM_CLOSE:
        EndDialog(dialog, IDCANCEL);
        return TRUE;
    default:
        return FALSE;
    }
}
} // namespace

std::optional<BatchAddRequest> showBatchAddDialog(HWND owner,
    const std::filesystem::path& initialDirectory, std::string_view initialText)
{
    DialogContext context;
    context.initialDirectory = initialDirectory;
    context.initialText = std::string(initialText);
    const INT_PTR result = DialogBoxParamW(GetModuleHandleW(nullptr),
        MAKEINTRESOURCEW(IDD_BATCH_ADD), owner, dialogProc,
        reinterpret_cast<LPARAM>(&context));
    if (result == IDOK && context.result) return context.result;
    return std::nullopt;
}

} // namespace litefetch::ui
