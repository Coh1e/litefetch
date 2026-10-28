#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "columns.h"

namespace litefetch::ui {

// Native checklist with move up/down and restore defaults. Returns true only
// after the user confirms a layout that still shows the required 名称 column.
bool showColumnDialog(HWND owner, ColumnSettings& settings);

} // namespace litefetch::ui
