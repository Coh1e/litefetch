#include "list_layout.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace litefetch::ui {
namespace {
// Design widths at 1000 px / 96 DPI, including the flexible 状态 column.
constexpr std::array<int, 6> designWidths{342, 104, 82, 126, 108, 238};
constexpr int designTotal = 1000;
} // namespace

std::array<int, 6> listColumnWidths(int clientWidth, int dpi)
{
    std::array<int, 6> widths{};
    if (clientWidth <= 0 || dpi <= 0) return widths;

    const auto scaled = [dpi](int design) { return MulDiv(design, dpi, 96); };
    int fixedSum = 0;
    for (std::size_t index = 0; index < 5; ++index) {
        widths[index] = scaled(designWidths[index]);
        fixedSum += widths[index];
    }

    if (clientWidth >= fixedSum) {
        // Normal case: the design's five column pitches stay fixed and 状态
        // takes every remaining pixel, exactly like flex-[1_0_0] in Figma.
        widths[5] = clientWidth - fixedSum;
        return widths;
    }

    // Below the design width at this DPI (e.g. the minimum window size), scale
    // all six columns proportionally so nothing overflows and no fake
    // horizontal scrollbar appears.
    int allocated = 0;
    for (std::size_t index = 0; index < 5; ++index) {
        widths[index] = MulDiv(designWidths[index], clientWidth * dpi, 96 * designTotal);
        allocated += widths[index];
    }
    widths[5] = clientWidth - allocated;
    return widths;
}

} // namespace litefetch::ui
