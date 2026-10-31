#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace litefetch::ui {

// Centers a dialog on its owner. Header-only because three dialogs need the
// same twelve lines and none of them owns the concept; a real dialog chrome
// helper is #41.
inline void centerDialogOnOwner(HWND dialog, HWND owner)
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

} // namespace litefetch::ui
