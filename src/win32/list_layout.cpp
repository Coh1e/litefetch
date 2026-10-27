#include "list_layout.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace litefetch::ui {
namespace {
// Design widths at the design ListView client width of 998 px (1000 px window
// client minus the native 2 px ListView border) at 96 DPI.
constexpr std::array<int, 6> designWidths{342, 104, 82, 126, 108, 236};
constexpr int designFixedSum = 342 + 104 + 82 + 126 + 108;
constexpr int designStatusWidth = 236;
constexpr int designClientWidth = designFixedSum + designStatusWidth; // 998
} // namespace

std::array<int, 6> listColumnWidths(int clientWidth, int dpi)
{
    std::array<int, 6> widths{};
    if (clientWidth <= 0 || dpi <= 0) return widths;

    std::array<int, 6> scaled{};
    int fixedSum = 0;
    for (std::size_t index = 0; index < 5; ++index) {
        scaled[index] = MulDiv(designWidths[index], dpi, 96);
        fixedSum += scaled[index];
    }
    scaled[5] = MulDiv(designStatusWidth, dpi, 96);
    const int designClient = fixedSum + scaled[5];

    if (clientWidth >= designClient) {
        // At or above the design width: keep the five Figma column pitches and
        // let 状态 absorb every extra pixel (flex-[1_0_0] behavior).
        for (std::size_t index = 0; index < 5; ++index) widths[index] = scaled[index];
        widths[5] = clientWidth - fixedSum;
        return widths;
    }

    // Below the design width every column shrinks proportionally, including
    // 状态, so no column collapses to 0/1 px and there is no width cliff at the
    // threshold. At exactly designClient this equals the fixed layout above.
    int allocated = 0;
    for (std::size_t index = 0; index < 5; ++index) {
        widths[index] = MulDiv(scaled[index], clientWidth, designClient);
        allocated += widths[index];
    }
    widths[5] = clientWidth - allocated;
    return widths;
}

} // namespace litefetch::ui
