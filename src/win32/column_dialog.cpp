#include "column_dialog.h"

#include "resource.h"
#include "text_utf8.h"

#include <commctrl.h>

#include <algorithm>

namespace litefetch::ui {
namespace {
struct DialogContext {
    ColumnSettings* settings = nullptr;
};

std::wstring titleOf(ColumnId id)
{
    try {
        return utf16FromUtf8(columnTitle(id));
    } catch (const std::exception&) {
        return {};
    }
}

bool itemChecked(HWND list, int index)
{
    const auto state = ListView_GetItemState(list, index, LVIS_STATEIMAGEMASK);
    return (state >> 12) == 2;
}

void setItemChecked(HWND list, int index, bool checked)
{
    ListView_SetItemState(list, index, INDEXTOSTATEIMAGEMASK(checked ? 2 : 1), LVIS_STATEIMAGEMASK);
}

void fillList(HWND dialog, const ColumnSettings& settings)
{
    HWND list = GetDlgItem(dialog, IDC_COLUMN_LIST);
    SendMessageW(list, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(list);
    for (std::size_t index = 0; index < settings.columns.size(); ++index) {
        const auto& state = settings.columns[index];
        const auto title = titleOf(state.id);
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = static_cast<int>(index);
        item.pszText = const_cast<LPWSTR>(title.c_str());
        ListView_InsertItem(list, &item);
        const auto width = std::to_wstring(state.width) + L" px";
        ListView_SetItemText(list, static_cast<int>(index), 1, const_cast<LPWSTR>(width.c_str()));
        setItemChecked(list, static_cast<int>(index), state.visible);
    }
    if (!settings.columns.empty()) {
        ListView_SetItemState(list, 0, LVIS_SELECTED | LVIS_FOCUSED,
            LVIS_SELECTED | LVIS_FOCUSED);
    }
    SendMessageW(list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(list, nullptr, TRUE);
}

void collect(HWND dialog, ColumnSettings& settings)
{
    HWND list = GetDlgItem(dialog, IDC_COLUMN_LIST);
    for (std::size_t index = 0; index < settings.columns.size(); ++index)
        settings.columns[index].visible = itemChecked(list, static_cast<int>(index));
}

int selectedIndex(HWND list)
{
    return ListView_GetNextItem(list, -1, LVNI_SELECTED);
}

void moveSelected(HWND dialog, ColumnSettings& settings, int delta)
{
    HWND list = GetDlgItem(dialog, IDC_COLUMN_LIST);
    const int index = selectedIndex(list);
    if (index < 0) return;
    const int target = index + delta;
    if (target < 0 || target >= static_cast<int>(settings.columns.size())) return;
    collect(dialog, settings);
    std::swap(settings.columns[static_cast<std::size_t>(index)],
        settings.columns[static_cast<std::size_t>(target)]);
    fillList(dialog, settings);
    ListView_SetItemState(list, target, LVIS_SELECTED | LVIS_FOCUSED,
        LVIS_SELECTED | LVIS_FOCUSED);
    ListView_EnsureVisible(list, target, FALSE);
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

INT_PTR CALLBACK dialogProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam)
{
    auto* context = reinterpret_cast<DialogContext*>(GetWindowLongPtrW(dialog, GWLP_USERDATA));
    switch (message) {
    case WM_INITDIALOG: {
        context = reinterpret_cast<DialogContext*>(lParam);
        SetWindowLongPtrW(dialog, GWLP_USERDATA, lParam);
        if (!context || !context->settings) return FALSE;
        HWND list = GetDlgItem(dialog, IDC_COLUMN_LIST);
        ListView_SetExtendedListViewStyle(list,
            LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        LVCOLUMNW column{};
        column.mask = LVCF_TEXT | LVCF_WIDTH;
        column.pszText = const_cast<LPWSTR>(L"列");
        column.cx = 150;
        ListView_InsertColumn(list, 0, &column);
        column.pszText = const_cast<LPWSTR>(L"宽度");
        column.cx = 54;
        ListView_InsertColumn(list, 1, &column);
        fillList(dialog, *context->settings);
        centerOnOwner(dialog, GetParent(dialog));
        SetFocus(list);
        return FALSE;
    }
    case WM_COMMAND:
        if (!context || !context->settings) return FALSE;
        switch (LOWORD(wParam)) {
        case IDC_COLUMN_UP:
            moveSelected(dialog, *context->settings, -1);
            return TRUE;
        case IDC_COLUMN_DOWN:
            moveSelected(dialog, *context->settings, 1);
            return TRUE;
        case IDC_COLUMN_DEFAULT:
            *context->settings = defaultColumnSettings();
            fillList(dialog, *context->settings);
            return TRUE;
        case IDOK: {
            collect(dialog, *context->settings);
            const auto name = std::find_if(context->settings->columns.begin(),
                context->settings->columns.end(),
                [](const ColumnState& state) { return state.id == ColumnId::Name; });
            if (name == context->settings->columns.end() || !name->visible) {
                MessageBoxW(dialog, L"至少需要显示“名称”列。", L"选择列表列",
                    MB_OK | MB_ICONWARNING);
                return TRUE;
            }
            *context->settings = sanitizeColumnSettings(*context->settings);
            EndDialog(dialog, IDOK);
            return TRUE;
        }
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

bool showColumnDialog(HWND owner, ColumnSettings& settings)
{
    DialogContext context;
    context.settings = &settings;
    const INT_PTR result = DialogBoxParamW(GetModuleHandleW(nullptr),
        MAKEINTRESOURCEW(IDD_COLUMNS), owner, dialogProc,
        reinterpret_cast<LPARAM>(&context));
    return result == IDOK;
}

} // namespace litefetch::ui
