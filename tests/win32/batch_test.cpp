// Batch-input boundary: one result per input line, original order, explicit
// unsupported protocols and no silent drops.
#include "batch_input.h"
#include "test_support.h"

#include <string>

using namespace litefetch::test;
using namespace litefetch::ui;

namespace {
void mixedInput()
{
    const std::string text =
        "https://example.org/%E8%B5%84%E6%96%99.zip\n"
        "http://example.org/plain.bin\n"
        "magnet:?xt=urn:btih:0123456789012345678901234567890123456789\n"
        "ed2k://|file|%E6%97%A7%E7%89%88.iso|650000000|0123456789ABCDEF0123456789ABCDEF|/\n"
        "ftp://ftp.example.org/pub/archive.zip\n"
        "\n"
        "htp://example.org/error.zip\n";
    const auto input = analyzeBatchInput(text);
    check(input.lines.size() == 6, "one result per non-empty line");
    check(input.validCount == 2, "only HTTP and HTTPS are valid for the current engine");
    check(input.invalidCount == 4, "unsupported and unknown addresses are counted");
    check(input.blankLines == 1, "interior blank line is reported, not silently dropped");
    check(input.lines[0].line == 1 && input.lines[1].line == 2 && input.lines[2].line == 3
            && input.lines[3].line == 4 && input.lines[4].line == 5 && input.lines[5].line == 7,
        "original line numbers survive blank lines");
    check(input.lines[0].protocol == "HTTPS", "HTTPS protocol shown");
    check(input.lines[0].suggestedFileName == "\xE8\xB5\x84\xE6\x96\x99.zip",
        "percent-decoded UTF-8 file name");
    check(input.lines[1].valid && input.lines[1].verdict == "有效", "HTTP valid");
    check(!input.lines[2].valid && input.lines[2].verdict.find("BitTorrent") != std::string::npos,
        "magnet is explicitly unsupported, not silently accepted");
    check(!input.lines[3].valid && input.lines[3].verdict.find("ED2K") != std::string::npos,
        "ed2k is explicitly unsupported");
    check(!input.lines[4].valid && input.lines[4].verdict.find("FTP") != std::string::npos,
        "ftp is explicitly unsupported");
    check(!input.lines[5].valid && input.lines[5].verdict.find("未知地址类型") != std::string::npos,
        "typo scheme is invalid with a reason");

    const auto summary = batchSummary(input);
    check(summary.find("有效: 2 / 6") != std::string::npos, "summary keeps the valid/total counts");
    check(summary.find("另有 1 条") != std::string::npos,
        "summary reports the remaining rejected lines instead of hiding them");
}

void edgeCases()
{
    const auto trailing = analyzeBatchInput("http://a/1.bin\nhttp://b/2.bin\n");
    check(trailing.lines.size() == 2, "trailing newline is not an extra line");
    check(trailing.blankLines == 0, "trailing newline is not a blank input line");
    check(trailing.validCount == 2, "both lines valid");

    const auto multiplePerLine = analyzeBatchInput("http://a/1.bin http://b/2.bin\n");
    check(multiplePerLine.lines.size() == 1 && !multiplePerLine.lines[0].valid,
        "a line with two addresses is rejected instead of split silently");
    check(multiplePerLine.lines[0].verdict.find("每行只能包含一个地址") != std::string::npos,
        "multi-address line explains the rule");

    const auto empty = analyzeBatchInput("");
    check(empty.lines.empty() && empty.validCount == 0, "empty text has no lines");
    check(analyzeBatchInput("   \r\n \n").lines.empty(), "blank-only text has no address lines");
}
} // namespace

int main()
{
    return run("ui-batch", [] {
        mixedInput();
        edgeCases();
        return 0;
    });
}
