#pragma once

#include "settings.h"

#include <windows.h>

#include <optional>

namespace litefetch::ui {

// Modal options dialog: speed limit, HTTP proxy, connect timeout and the ed2k
// server list. Returns std::nullopt when the user cancels.
std::optional<Settings> showOptionsDialog(HWND owner, const Settings& current);

} // namespace litefetch::ui
