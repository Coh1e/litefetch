#pragma once

#include <array>

namespace litefetch::ui {

// Column geometry from the Figma main board (1000 px client, 96 DPI).
//
// The design's six text slots are 331/93/71/115/97 px wide with an 11 px gap
// between slots. A ListView column includes both, so the first five column
// pitches are 342/104/82/126/108 px. The last column ("状态") is the flexible
// remainder of the real ListView client area, not a fixed width.
inline constexpr std::array<int, 6> listColumnPitches{342, 104, 82, 126, 108, 0};

// Widths in device pixels for the current ListView client width and DPI. The
// first five columns scale with the DPI and the last fills what remains; below
// the design width at that DPI all columns shrink proportionally so the layout
// never leaves an empty tail or a fake horizontal scrollbar.
std::array<int, 6> listColumnWidths(int clientWidth, int dpi);

} // namespace litefetch::ui
