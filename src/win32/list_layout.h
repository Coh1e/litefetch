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

// Widths in device pixels for the current ListView client width and DPI.
// At or above the design ListView client width (998 px at 96 DPI, scaled by
// DPI) the first five columns keep the Figma pitches and 状态 fills the rest.
// Below that width all six columns shrink proportionally (including 状态), so
// no column collapses to 0/1 px and there is no layout cliff at the threshold.
std::array<int, 6> listColumnWidths(int clientWidth, int dpi);

} // namespace litefetch::ui
