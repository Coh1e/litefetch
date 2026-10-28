#include "task_view.h"

#include "text_utf8.h"
#include "ui_text.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>

namespace litefetch::ui {
namespace {
bool startsWithNoCase(std::string_view text, std::string_view prefix)
{
    if (text.size() < prefix.size()) return false;
    for (std::size_t index = 0; index < prefix.size(); ++index) {
        const auto left = static_cast<unsigned char>(text[index]);
        const auto right = static_cast<unsigned char>(prefix[index]);
        if (std::tolower(left) != std::tolower(right)) return false;
    }
    return true;
}

std::string lowerAscii(std::string_view text)
{
    std::string result;
    result.reserve(text.size());
    for (const char character : text) {
        const auto value = static_cast<unsigned char>(character);
        result.push_back(static_cast<char>(std::tolower(value)));
    }
    return result;
}

bool sensitiveQueryName(std::string_view name)
{
    const auto lower = lowerAscii(name);
    static constexpr std::string_view names[] = {"password", "passwd", "pwd", "token", "access_token",
        "api_key", "apikey", "key", "secret", "sig", "signature", "auth", "authorization",
        "session", "sessionid", "credential", "credentials"};
    for (const auto candidate : names) {
        if (lower == candidate) return true;
    }
    return false;
}

std::wstring fold(std::string_view utf8)
{
    try {
        std::wstring text = utf16FromUtf8(utf8);
        if (!text.empty()) CharUpperBuffW(text.data(), static_cast<DWORD>(text.size()));
        return text;
    } catch (const std::exception&) {
        return {};
    }
}

bool containsNoCase(std::string_view haystack, const std::wstring& foldedNeedle)
{
    if (foldedNeedle.empty()) return true;
    std::wstring foldedHaystack = fold(haystack);
    return foldedHaystack.find(foldedNeedle) != std::wstring::npos;
}

int compareText(std::string_view left, std::string_view right)
{
    const auto foldedLeft = fold(left);
    const auto foldedRight = fold(right);
    if (foldedLeft < foldedRight) return -1;
    if (foldedLeft > foldedRight) return 1;
    if (left < right) return -1;
    if (left > right) return 1;
    return 0;
}

int statusRank(const TaskRow& row)
{
    switch (row.state) {
    case TaskState::Running: return 0;
    case TaskState::Queued: return 1;
    case TaskState::Failed: return 2;
    case TaskState::Cancelled: return 3;
    case TaskState::Completed: return 4;
    }
    return 5;
}

bool knownValue(const TaskRow& row, ColumnId column)
{
    switch (column) {
    case ColumnId::Name: return !row.name.empty();
    case ColumnId::Size: return row.total >= 0;
    case ColumnId::Progress: return row.total >= 0;
    case ColumnId::DownloadSpeed: return row.speedKnown;
    case ColumnId::RemainingTime: return row.remainingKnown;
    case ColumnId::Status: return true;
    case ColumnId::Protocol: return !row.protocol.empty() && row.protocol != std::string(unknownValue);
    case ColumnId::UploadSpeed: return false; // engine does not report upload yet
    case ColumnId::Connections: return false;
    case ColumnId::Sources: return false;
    case ColumnId::Downloaded: return row.received >= 0;
    case ColumnId::Uploaded: return false;
    case ColumnId::AddedTime: return row.createdAt > 0;
    case ColumnId::CompletedTime: return row.completedAt > 0;
    case ColumnId::SavePath: return !row.destination.empty();
    case ColumnId::Address: return !row.address.empty();
    case ColumnId::Count: return false;
    }
    return false;
}

int compareKnown(const TaskRow& left, const TaskRow& right, ColumnId column)
{
    switch (column) {
    case ColumnId::Name: return compareText(left.name, right.name);
    case ColumnId::Size:
        if (left.total < right.total) return -1;
        if (left.total > right.total) return 1;
        return 0;
    case ColumnId::Progress: {
        const double leftRatio = left.total > 0 ? static_cast<double>(left.received) / left.total
                                                : (left.total == 0 ? 1.0 : 0.0);
        const double rightRatio = right.total > 0 ? static_cast<double>(right.received) / right.total
                                                  : (right.total == 0 ? 1.0 : 0.0);
        if (leftRatio < rightRatio) return -1;
        if (leftRatio > rightRatio) return 1;
        return 0;
    }
    case ColumnId::DownloadSpeed:
        if (left.speedBytesPerSecond < right.speedBytesPerSecond) return -1;
        if (left.speedBytesPerSecond > right.speedBytesPerSecond) return 1;
        return 0;
    case ColumnId::RemainingTime:
        if (left.remainingSeconds < right.remainingSeconds) return -1;
        if (left.remainingSeconds > right.remainingSeconds) return 1;
        return 0;
    case ColumnId::Status:
        if (statusRank(left) < statusRank(right)) return -1;
        if (statusRank(left) > statusRank(right)) return 1;
        return 0;
    case ColumnId::Protocol: return compareText(left.protocol, right.protocol);
    case ColumnId::Downloaded:
        if (left.received < right.received) return -1;
        if (left.received > right.received) return 1;
        return 0;
    case ColumnId::AddedTime:
        if (left.createdAt < right.createdAt) return -1;
        if (left.createdAt > right.createdAt) return 1;
        return 0;
    case ColumnId::CompletedTime:
        if (left.completedAt < right.completedAt) return -1;
        if (left.completedAt > right.completedAt) return 1;
        return 0;
    case ColumnId::SavePath: return compareText(left.destination, right.destination);
    case ColumnId::Address: return compareText(left.address, right.address);
    case ColumnId::UploadSpeed:
    case ColumnId::Connections:
    case ColumnId::Sources:
    case ColumnId::Uploaded:
    case ColumnId::Count:
        return 0;
    }
    return 0;
}
} // namespace

std::string redactAddress(std::string_view address)
{
    std::string result(address);
    const auto scheme = result.find("://");
    if (scheme != std::string::npos) {
        const auto authorityStart = scheme + 3;
        const auto authorityEnd = result.find_first_of("/?#", authorityStart);
        const auto end = authorityEnd == std::string::npos ? result.size() : authorityEnd;
        auto at = std::string::npos;
        for (auto index = authorityStart; index < end; ++index) {
            if (result[index] == '@') at = index;
        }
        if (at != std::string::npos && at >= authorityStart) {
            const auto userinfo = std::string_view(result).substr(authorityStart, at - authorityStart);
            const auto replacement = userinfo.find(':') == std::string_view::npos ? "***" : "***:***";
            result.replace(authorityStart, at - authorityStart, replacement);
        }
    }

    const auto query = result.find('?');
    if (query != std::string::npos) {
        const auto fragment = result.find('#', query);
        const auto queryEnd = fragment == std::string::npos ? result.size() : fragment;
        const std::string_view original(result.data() + query + 1, queryEnd - query - 1);
        std::string rebuilt;
        std::size_t start = 0;
        while (start <= original.size()) {
            const auto separator = original.find('&', start);
            const auto part = separator == std::string_view::npos ? original.substr(start)
                                                                  : original.substr(start, separator - start);
            const auto equals = part.find('=');
            if (equals != std::string_view::npos && sensitiveQueryName(part.substr(0, equals))) {
                if (!rebuilt.empty()) rebuilt.push_back('&');
                rebuilt.append(part.substr(0, equals));
                rebuilt.append("=***");
            } else {
                if (!rebuilt.empty()) rebuilt.push_back('&');
                rebuilt.append(part);
            }
            if (separator == std::string_view::npos) break;
            start = separator + 1;
        }
        result.replace(query + 1, queryEnd - query - 1, rebuilt);
    }
    return result;
}

std::string protocolLabel(std::string_view source)
{
    if (startsWithNoCase(source, "https://") || startsWithNoCase(source, "ftps://"))
        return startsWithNoCase(source, "https://") ? "HTTPS" : "FTP";
    if (startsWithNoCase(source, "http://")) return "HTTP";
    if (startsWithNoCase(source, "ftp://")) return "FTP";
    if (startsWithNoCase(source, "magnet:")) return "BitTorrent";
    if (startsWithNoCase(source, "ed2k://")) return "ED2K";
    return std::string(unknownValue);
}

bool matchesSearch(const TaskRow& row, std::string_view query)
{
    if (query.empty()) return true;
    const auto folded = fold(query);
    if (folded.empty()) return false;
    return containsNoCase(row.name, folded) || containsNoCase(row.address, folded)
        || containsNoCase(row.destination, folded);
}

bool matchesFilter(const TaskRow& row, TaskFilter filter)
{
    switch (filter) {
    case TaskFilter::All: return true;
    case TaskFilter::Active:
        return row.state == TaskState::Running && row.active && !row.interrupted;
    case TaskFilter::Completed: return row.state == TaskState::Completed;
    case TaskFilter::Failed: return row.state == TaskState::Failed;
    }
    return true;
}

bool rowMatches(const TaskRow& row, TaskFilter filter, std::string_view query)
{
    return matchesFilter(row, filter) && matchesSearch(row, query);
}

std::string columnText(const TaskRow& row, ColumnId column)
{
    switch (column) {
    case ColumnId::Name: return row.name.empty() ? std::string(unknownValue) : row.name;
    case ColumnId::Size: return row.size;
    case ColumnId::Progress: return row.progress;
    case ColumnId::DownloadSpeed: return row.speed;
    case ColumnId::RemainingTime: return row.remaining;
    case ColumnId::Status: return row.status;
    case ColumnId::Protocol: return row.protocol.empty() ? std::string(unknownValue) : row.protocol;
    case ColumnId::UploadSpeed: return row.uploadSpeed.empty() ? std::string(unknownValue) : row.uploadSpeed;
    case ColumnId::Connections: return row.connections.empty() ? std::string(unknownValue) : row.connections;
    case ColumnId::Sources: return row.sources.empty() ? std::string(unknownValue) : row.sources;
    case ColumnId::Downloaded: return formatSize(row.received);
    case ColumnId::Uploaded: return row.uploaded.empty() ? std::string(unknownValue) : row.uploaded;
    case ColumnId::AddedTime: return formatDateTime(row.createdAt);
    case ColumnId::CompletedTime: return formatDateTime(row.completedAt);
    case ColumnId::SavePath:
        return row.destination.empty() ? std::string(unknownValue) : row.destination;
    case ColumnId::Address: return row.address.empty() ? std::string(unknownValue) : row.address;
    case ColumnId::Count: return std::string(unknownValue);
    }
    return std::string(unknownValue);
}

void sortRows(std::vector<TaskRow>& rows, ColumnId column, SortDirection direction)
{
    std::sort(rows.begin(), rows.end(), [column, direction](const TaskRow& left, const TaskRow& right) {
        const bool leftKnown = knownValue(left, column);
        const bool rightKnown = knownValue(right, column);
        if (leftKnown != rightKnown) return leftKnown;
        if (!leftKnown) return left.id < right.id;
        const int comparison = compareKnown(left, right, column);
        if (comparison == 0) return left.id < right.id;
        return direction == SortDirection::Ascending ? comparison < 0 : comparison > 0;
    });
}

} // namespace litefetch::ui
