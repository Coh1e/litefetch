#include "batch_input.h"

#include "task_view.h"
#include "url_filename.h"

#include <algorithm>

namespace litefetch::ui {
namespace {
std::string_view trim(std::string_view text)
{
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r'))
        text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r'))
        text.remove_suffix(1);
    return text;
}
} // namespace

BatchInput analyzeBatchInput(std::string_view text)
{
    BatchInput input;
    std::size_t start = 0;
    int lineNumber = 1;
    while (start <= text.size()) {
        const auto end = text.find('\n', start);
        const auto raw = end == std::string_view::npos ? text.substr(start) : text.substr(start, end - start);
        const auto line = trim(raw);
        if (line.empty()) {
            // A trailing newline is not an extra input line; interior blank
            // lines are reported so nothing is silently dropped.
            if (end != std::string_view::npos && start < text.size()) ++input.blankLines;
        } else {
            BatchLine entry;
            entry.line = lineNumber;
            entry.text = std::string(line);
            const auto parsed = analyzeSourceInput(line);
            entry.protocol = protocolLabel(line);
            entry.suggestedFileName = parsed.suggestedFileName;
            switch (parsed.kind) {
            case SourceKind::Http:
                entry.valid = true;
                entry.verdict = "有效";
                break;
            case SourceKind::Https:
                entry.valid = true;
                entry.verdict = "有效 · HTTPS";
                break;
            case SourceKind::Ftp:
                entry.valid = true;
                entry.verdict = "有效 · FTP";
                break;
            case SourceKind::Magnet:
                entry.valid = true;
                entry.verdict = "有效 · 磁力链接";
                break;
            case SourceKind::Ed2k:
                entry.valid = true;
                entry.verdict = "有效 · ed2k";
                break;
            case SourceKind::Multiple:
                entry.verdict = "无效 · 每行只能包含一个地址";
                break;
            case SourceKind::Unsupported: {
                entry.verdict = "无效 · 未知地址类型";
                break;
            }
            case SourceKind::Empty:
                entry.verdict = "无效 · 空地址";
                break;
            }
            if (entry.valid) {
                ++input.validCount;
            } else {
                ++input.invalidCount;
            }
            input.lines.push_back(std::move(entry));
        }
        if (end == std::string_view::npos) break;
        start = end + 1;
        ++lineNumber;
    }
    return input;
}

std::string batchSummary(const BatchInput& input)
{
    const auto total = input.lines.size();
    std::string summary = "有效: " + std::to_string(input.validCount) + " / " + std::to_string(total) + "。";
    if (input.blankLines > 0)
        summary += " 已忽略 " + std::to_string(input.blankLines) + " 个空行。";
    if (input.invalidCount > 0) {
        summary += " 未添加: ";
        std::size_t shown = 0;
        for (const auto& line : input.lines) {
            if (line.valid) continue;
            if (shown > 0) summary += "；";
            summary += "第" + std::to_string(line.line) + "行 " + line.text + "（" + line.verdict + "）";
            if (++shown == 3) {
                if (input.invalidCount > shown)
                    summary += "；另有 " + std::to_string(input.invalidCount - shown) + " 条";
                break;
            }
        }
    }
    return summary;
}

} // namespace litefetch::ui
