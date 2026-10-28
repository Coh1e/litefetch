#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace litefetch::ui {

// One non-empty input line, kept in its original order. `line` is the 1-based
// line number in the original text so every rejection or commit failure can be
// traced back to the user's input.
struct BatchLine {
    int line = 0;
    std::string text;
    std::string protocol;
    std::string suggestedFileName;
    bool valid = false;
    std::string verdict;
};

struct BatchInput {
    std::vector<BatchLine> lines;
    std::size_t blankLines = 0;
    std::size_t validCount = 0;
    std::size_t invalidCount = 0;
};

BatchInput analyzeBatchInput(std::string_view text);
std::string batchSummary(const BatchInput& input);

} // namespace litefetch::ui
