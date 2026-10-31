#include "credential_dialog.h"

#include "dialog_layout.h"
#include "resource.h"
#include "text_utf8.h"

#include <exception>
#include <utility>

namespace litefetch::ui {
namespace {

std::string controlText(HWND dialog, int id)
{
    const HWND control = GetDlgItem(dialog, id);
    const int length = GetWindowTextLengthW(control);
    if (length <= 0) return {};
    std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
    const int copied = GetWindowTextW(control, text.data(), static_cast<int>(text.size()));
    if (copied <= 0) return {};
    text.resize(static_cast<std::size_t>(copied));
    try {
        return utf8FromUtf16(text);
    } catch (const std::exception&) {
        return {};
    }
}

void closeDialog(HWND dialog, const CredentialPrompt* prompt, INT_PTR result)
{
    // A modeless dialog is destroyed, not ended: EndDialog only unwinds the
    // modal loop that DialogBoxParamW installed.
    if (prompt && prompt->modeless)
        DestroyWindow(dialog);
    else
        EndDialog(dialog, result);
}

} // namespace

INT_PTR CALLBACK credentialDialogProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam)
{
    auto* context = reinterpret_cast<CredentialPrompt*>(GetWindowLongPtrW(dialog, GWLP_USERDATA));
    switch (message) {
    case WM_INITDIALOG: {
        context = reinterpret_cast<CredentialPrompt*>(lParam);
        SetWindowLongPtrW(dialog, GWLP_USERDATA, lParam);
        // The text has to describe what actually happens; the old copy claimed
        // nothing was ever saved while the checkbox offered to save it.
        SetDlgItemTextW(dialog, IDC_CRED_HINT,
            L"服务器要求登录（HTTP 401）。勾选“记住密码”后由 Windows 保护存储保存；"
            L"不勾选只用于本次运行。");
        centerDialogOnOwner(dialog, GetParent(dialog));
        SetFocus(GetDlgItem(dialog, IDC_CRED_USER));
        return FALSE;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK) {
            if (context) {
                context->username = controlText(dialog, IDC_CRED_USER);
                context->password = controlText(dialog, IDC_CRED_PASS);
                context->remember =
                    IsDlgButtonChecked(dialog, IDC_CRED_REMEMBER) == BST_CHECKED;
            }
            closeDialog(dialog, context, IDOK);
            return TRUE;
        }
        if (LOWORD(wParam) == IDCANCEL) {
            closeDialog(dialog, context, IDCANCEL);
            return TRUE;
        }
        return FALSE;
    case WM_CLOSE:
        closeDialog(dialog, context, IDCANCEL);
        return TRUE;
    default:
        return FALSE;
    }
}

std::optional<CredentialPrompt> showCredentialDialog(HWND owner, std::string host)
{
    CredentialPrompt prompt;
    prompt.host = std::move(host);
    if (DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_CREDENTIALS), owner,
            credentialDialogProc, reinterpret_cast<LPARAM>(&prompt)) != IDOK)
        return std::nullopt;
    return prompt;
}

bool storeAcceptedCredential(const CredentialPrompt& prompt,
    const std::function<bool(const std::string&, const std::string&, const std::string&)>& save)
{
    if (!prompt.remember || prompt.host.empty() || prompt.username.empty()) return false;
    return save(prompt.host, prompt.username, prompt.password);
}

} // namespace litefetch::ui
