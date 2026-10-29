#include "options_dialog.h"

#include "resource.h"
#include "schedule.h"
#include "text_utf8.h"

#include <cstdlib>
#include <string>
#include <vector>

namespace litefetch::ui {
namespace {
std::wstring wideOf(std::string_view utf8)
{
    if (utf8.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
        nullptr, 0);
    if (length <= 0) return {};
    std::wstring text(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), text.data(), length);
    return text;
}

struct OptionsContext {
    Settings current;
    std::optional<OptionsResult> result;
    bool forgetCredentials = false;
};

std::string windowTextUtf8(HWND dialog, int id)
{
    wchar_t buffer[4096]{};
    GetDlgItemTextW(dialog, id, buffer, 4096);
    return utf8FromUtf16(buffer);
}

// Every non-empty line must parse; the first bad one is reported with its
// number so the user can fix it instead of losing the whole list.
template <typename Validator>
bool validateLines(const std::string& text, Validator valid, HWND dialog, const wchar_t* title)
{
    std::size_t start = 0;
    int lineNumber = 0;
    while (start <= text.size()) {
        auto end = text.find('\n', start);
        auto line = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        ++lineNumber;
        if (!line.empty() && !valid(line)) {
            const auto message = std::wstring(title) + L" 第 " + std::to_wstring(lineNumber)
                + L" 行格式不正确。";
            MessageBoxW(dialog, message.c_str(), L"选项", MB_OK | MB_ICONWARNING);
            return false;
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return true;
}

INT_PTR CALLBACK optionsDialogProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam)
{
    auto* context = reinterpret_cast<OptionsContext*>(GetWindowLongPtrW(dialog, GWLP_USERDATA));
    switch (message) {
    case WM_INITDIALOG: {
        context = reinterpret_cast<OptionsContext*>(lParam);
        SetWindowLongPtrW(dialog, GWLP_USERDATA, lParam);
        if (!context) return FALSE;
        // The dialog works in KB/s because that is what users type.
        SetDlgItemInt(dialog, IDC_OPT_SPEED,
            static_cast<UINT>(context->current.speedLimitBytesPerSecond / 1024), FALSE);
        SetDlgItemTextW(dialog, IDC_OPT_PROXY, wideOf(context->current.httpProxy).c_str());
        SetDlgItemInt(dialog, IDC_OPT_TIMEOUT,
            static_cast<UINT>(context->current.connectTimeoutSeconds), FALSE);
        std::string servers;
        for (const auto& server : context->current.ed2kServers) {
            if (!servers.empty()) servers += "\r\n";
            servers += server;
        }
        SetDlgItemTextW(dialog, IDC_OPT_ED2K, wideOf(servers).c_str());
        CheckDlgButton(dialog, IDC_OPT_RENAME,
            context->current.renameOnConflict ? BST_CHECKED : BST_UNCHECKED);
        SetDlgItemInt(dialog, IDC_OPT_CONCURRENCY,
            static_cast<UINT>(context->current.maxConcurrentDownloads), FALSE);
        CheckDlgButton(dialog, IDC_OPT_CATEGORY,
            context->current.saveByCategory ? BST_CHECKED : BST_UNCHECKED);
        SetDlgItemTextW(dialog, IDC_OPT_BANDS,
            wideOf(formatRateBands(context->current.rateBands)).c_str());
        SetDlgItemTextW(dialog, IDC_OPT_SCHEDULE,
            wideOf(formatScheduledActions(context->current.scheduledActions)).c_str());
        return TRUE;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK) {
            if (context) {
                Settings settings;
                BOOL translated = FALSE;
                const auto speedKb = GetDlgItemInt(dialog, IDC_OPT_SPEED, &translated, FALSE);
                settings.speedLimitBytesPerSecond = translated
                    ? static_cast<std::int64_t>(speedKb) * 1024 : 0;
                settings.httpProxy = windowTextUtf8(dialog, IDC_OPT_PROXY);
                const auto timeout = GetDlgItemInt(dialog, IDC_OPT_TIMEOUT, &translated, FALSE);
                settings.connectTimeoutSeconds = translated ? static_cast<int>(timeout) : 30;
                if (settings.connectTimeoutSeconds < 5) settings.connectTimeoutSeconds = 5;
                const auto concurrent = GetDlgItemInt(dialog, IDC_OPT_CONCURRENCY, &translated, FALSE);
                settings.maxConcurrentDownloads = translated ? static_cast<int>(concurrent) : 0;
                settings.saveByCategory =
                    IsDlgButtonChecked(dialog, IDC_OPT_CATEGORY) == BST_CHECKED;
                settings.renameOnConflict =
                    IsDlgButtonChecked(dialog, IDC_OPT_RENAME) == BST_CHECKED;
                // Time bands and the schedule are typed text; a bad line is
                // reported instead of being silently dropped.
                const auto bands = windowTextUtf8(dialog, IDC_OPT_BANDS);
                if (!validateLines(bands, [](const std::string& line) {
                        litefetch::RateBand band;
                        return litefetch::parseRateBand(line, band);
                    }, dialog, L"时段限速")) {
                    return TRUE;
                }
                settings.rateBands = parseRateBands(bands);
                const auto schedule = windowTextUtf8(dialog, IDC_OPT_SCHEDULE);
                if (!validateLines(schedule, [](const std::string& line) {
                        litefetch::ScheduledAction action;
                        return litefetch::parseScheduledAction(line, action);
                    }, dialog, L"计划任务")) {
                    return TRUE;
                }
                settings.scheduledActions = parseScheduledActions(schedule);
                settings.ed2kServers.clear();
                const auto servers = windowTextUtf8(dialog, IDC_OPT_ED2K);
                std::size_t start = 0;
                while (start < servers.size()) {
                    auto end = servers.find('\n', start);
                    auto line = servers.substr(start,
                        end == std::string::npos ? std::string::npos : end - start);
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    if (!line.empty()) settings.ed2kServers.push_back(line);
                    if (end == std::string::npos) break;
                    start = end + 1;
                }
                OptionsResult result;
                result.settings = settings;
                result.forgetCredentials = context->forgetCredentials;
                context->result = result;
            }
            EndDialog(dialog, IDOK);
            return TRUE;
        }
        if (LOWORD(wParam) == IDC_OPT_FORGET) {
            if (MessageBoxW(dialog, L"清除这台账户下记住的所有站点密码？", L"选项",
                    MB_OKCANCEL | MB_ICONQUESTION) == IDOK) {
                if (context) context->forgetCredentials = true;
                SetDlgItemTextW(dialog, IDC_OPT_HINT, L"已请求清除记住的密码，确定后生效。");
            }
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
} // namespace

std::optional<OptionsResult> showOptionsDialog(HWND owner, const Settings& current)
{
    OptionsContext context;
    context.current = current;
    const auto answer = DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_OPTIONS),
        owner, optionsDialogProc, reinterpret_cast<LPARAM>(&context));
    if (answer != IDOK) return std::nullopt;
    return context.result;
}

} // namespace litefetch::ui
