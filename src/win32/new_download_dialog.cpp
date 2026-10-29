#include "new_download_dialog.h"

#include "resource.h"
#include "text_utf8.h"
#include "url_filename.h"

#include <commctrl.h>
#include <shlobj.h>

#include <system_error>
#include <vector>

namespace litefetch::ui {
namespace {
struct DialogContext {
    std::filesystem::path initialDirectory;
    std::string initialUrl;
    std::optional<NewDownloadRequest> result;
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

void updateHint(HWND dialog)
{
    const auto input = analyzeSourceInput(controlUtf8(dialog, IDC_URL));
    try {
        SetDlgItemTextW(dialog, IDC_URL_HINT, utf16FromUtf8(input.hint).c_str());
    } catch (const std::exception&) {
        SetDlgItemTextW(dialog, IDC_URL_HINT, L"已识别: —。");
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
    const int x = ownerRect.left + ((ownerRect.right - ownerRect.left) - width) / 2;
    const int y = ownerRect.top + ((ownerRect.bottom - ownerRect.top) - height) / 2;
    SetWindowPos(dialog, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
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
    if (SHGetPathFromIDListW(item, path)) SetDlgItemTextW(dialog, IDC_SAVE_DIR, path);
    CoTaskMemFree(item);
}

// Returns true when the dialog may close. Validation errors keep the dialog
// open and explain the real reason instead of accepting a bad destination.
bool accept(HWND dialog, DialogContext& context)
{
    const auto input = analyzeSourceInput(controlUtf8(dialog, IDC_URL));
    switch (input.kind) {
    case SourceKind::Empty:
        MessageBoxW(dialog, L"请输入下载地址。", L"新建下载", MB_OK | MB_ICONWARNING);
        SetFocus(GetDlgItem(dialog, IDC_URL));
        return false;
    case SourceKind::Multiple: {
        const int choice = MessageBoxW(dialog,
            L"检测到多个下载地址。是否切换到“批量添加”？\n\n"
            L"选择“否”会保留当前文本，不会创建任何任务。",
            L"新建下载", MB_YESNO | MB_DEFBUTTON2 | MB_ICONQUESTION);
        if (choice == IDYES) {
            context.result = NewDownloadRequest{};
            context.result->switchToBatch = true;
            context.result->batchText = controlUtf8(dialog, IDC_URL);
            context.result->directory = std::filesystem::path(
                windowText(GetDlgItem(dialog, IDC_SAVE_DIR)));
            return true;
        }
        SetFocus(GetDlgItem(dialog, IDC_URL));
        return false;
    }
    case SourceKind::Unsupported:
        MessageBoxW(dialog,
            L"不支持的地址类型。支持 HTTP(S)、FTP、磁力链接与 ed2k 文件链接。",
            L"新建下载", MB_OK | MB_ICONWARNING);
        SetFocus(GetDlgItem(dialog, IDC_URL));
        return false;
    case SourceKind::Http:
    case SourceKind::Https:
    case SourceKind::Ftp:
    case SourceKind::Magnet:
    case SourceKind::Ed2k:
        break;
    }

    // The checksum is optional; when present it must look like a SHA-256 so a
    // typo is caught before the download starts.
    std::string checksum;
    {
        const auto checksumText = windowText(GetDlgItem(dialog, IDC_CHECKSUM));
        checksum = utf8FromUtf16(checksumText);
        if (!checksum.empty()) {
            bool hex = checksum.size() == 64;
            for (const char c : checksum) {
                const bool digit = c >= '0' && c <= '9';
                const bool lower = c >= 'a' && c <= 'f';
                const bool upper = c >= 'A' && c <= 'F';
                if (!digit && !lower && !upper) hex = false;
            }
            if (!hex) {
                MessageBoxW(dialog, L"SHA-256 必须是 64 个十六进制字符。", L"新建下载",
                    MB_OK | MB_ICONWARNING);
                SetFocus(GetDlgItem(dialog, IDC_CHECKSUM));
                return false;
            }
        }
    }

    const std::wstring directoryText = windowText(GetDlgItem(dialog, IDC_SAVE_DIR));
    if (directoryText.empty()) {
        MessageBoxW(dialog, L"请选择保存文件夹。", L"新建下载", MB_OK | MB_ICONWARNING);
        SetFocus(GetDlgItem(dialog, IDC_SAVE_DIR));
        return false;
    }
    std::filesystem::path directory(directoryText);
    if (!directory.is_absolute()) {
        MessageBoxW(dialog, L"保存目录必须是绝对路径。", L"新建下载", MB_OK | MB_ICONWARNING);
        SetFocus(GetDlgItem(dialog, IDC_SAVE_DIR));
        return false;
    }
    std::error_code error;
    if (!std::filesystem::is_directory(directory, error)) {
        std::filesystem::create_directories(directory, error);
        if (error || !std::filesystem::is_directory(directory, error)) {
            MessageBoxW(dialog, L"无法创建或访问保存文件夹，请检查路径与权限。", L"新建下载",
                MB_OK | MB_ICONERROR);
            SetFocus(GetDlgItem(dialog, IDC_SAVE_DIR));
            return false;
        }
    }

    std::filesystem::path destination;
    try {
        destination = directory / pathFromUtf8(input.suggestedFileName);
    } catch (const std::exception&) {
        MessageBoxW(dialog, L"无法从地址推导文件名。", L"新建下载", MB_OK | MB_ICONERROR);
        return false;
    }
    if (std::filesystem::exists(destination, error)) {
        MessageBoxW(dialog,
            L"目标文件已存在，不会覆盖。请更换保存文件夹或删除同名文件后重试。",
            L"新建下载", MB_OK | MB_ICONWARNING);
        return false;
    }

    context.result = NewDownloadRequest{input.source, directory, input.suggestedFileName,
        IsDlgButtonChecked(dialog, IDC_AUTO_START) == BST_CHECKED};
    context.result->sha256 = checksum;
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
        if (!context->initialDirectory.empty())
            SetDlgItemTextW(dialog, IDC_SAVE_DIR, context->initialDirectory.c_str());
        CheckDlgButton(dialog, IDC_AUTO_START, BST_CHECKED);
        if (!context->initialUrl.empty()) {
            try {
                SetDlgItemTextW(dialog, IDC_URL, utf16FromUtf8(context->initialUrl).c_str());
            } catch (const std::exception&) {
            }
        }
        updateHint(dialog);
        centerOnOwner(dialog, GetParent(dialog));
        SetFocus(GetDlgItem(dialog, IDC_URL));
        SendDlgItemMessageW(dialog, IDC_URL, EM_SETSEL, 0, -1);
        return FALSE;
    }
    case WM_COMMAND: {
        const int id = LOWORD(wParam);
        const int code = HIWORD(wParam);
        if (id == IDC_URL && code == EN_CHANGE) {
            updateHint(dialog);
            return TRUE;
        }
        if (id == IDC_BROWSE && code == BN_CLICKED) {
            browseForFolder(dialog);
            return TRUE;
        }
        if (id == IDOK) {
            if (!context) return TRUE;
            if (accept(dialog, *context)) EndDialog(dialog, IDOK);
            return TRUE;
        }
        if (id == IDCANCEL) {
            EndDialog(dialog, IDCANCEL);
            return TRUE;
        }
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

std::optional<NewDownloadRequest> showNewDownloadDialog(HWND owner,
    const std::filesystem::path& initialDirectory, std::string_view initialUrl)
{
    DialogContext context;
    context.initialDirectory = initialDirectory;
    context.initialUrl = std::string(initialUrl);
    const INT_PTR result = DialogBoxParamW(GetModuleHandleW(nullptr),
        MAKEINTRESOURCEW(IDD_NEW_DOWNLOAD), owner, dialogProc,
        reinterpret_cast<LPARAM>(&context));
    if (result == IDOK && context.result) return context.result;
    return std::nullopt;
}

} // namespace litefetch::ui
