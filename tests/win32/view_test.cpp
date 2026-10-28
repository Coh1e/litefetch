// UI boundary tests for search/filter matching, URL redaction, optional-column
// text and numeric/time sorting. These functions feed the native ListView, so
// they are verified without a window.
#include "columns.h"
#include "task_list_model.h"
#include "task_view.h"
#include "test_support.h"
#include "ui_text.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using namespace litefetch;
using namespace litefetch::test;
using namespace litefetch::ui;

namespace {
TaskRow makeRow(std::string id, std::string name = "file.bin")
{
    TaskRow row;
    row.id = std::move(id);
    row.name = std::move(name);
    row.address = "http://example.com/" + row.id;
    row.destination = "D:/Downloads/" + row.id;
    row.savePath = row.destination;
    row.protocol = "HTTP";
    row.size = std::string(unknownValue);
    row.progress = std::string(unknownValue);
    row.speed = std::string(unknownValue);
    row.remaining = std::string(unknownValue);
    row.status = "等待中";
    return row;
}

std::vector<std::string> ids(const std::vector<TaskRow>& rows)
{
    std::vector<std::string> result;
    for (const auto& row : rows) result.push_back(row.id);
    return result;
}

void redactionAndProtocol()
{
    check(redactAddress("http://user:pass@example.com/a?token=secret&x=1")
            == "http://***:***@example.com/a?token=***&x=1",
        "userinfo and token query value are redacted");
    check(redactAddress("https://user@example.com") == "https://***@example.com",
        "username without password is redacted");
    check(redactAddress("ftp://user:pass@example.com/pub/file")
            == "ftp://***:***@example.com/pub/file",
        "ftp userinfo is redacted too");
    check(redactAddress("http://example.com/path?password=abc#frag")
            == "http://example.com/path?password=***#frag",
        "password query value is redacted and fragment preserved");
    check(redactAddress("http://example.com/?q=1&auth=abc")
            == "http://example.com/?q=1&auth=***",
        "auth query value is redacted");
    check(redactAddress("magnet:?xt=urn:btih:abc") == "magnet:?xt=urn:btih:abc",
        "non-sensitive query is unchanged");
    check(redactAddress("http://example.com/plain.bin") == "http://example.com/plain.bin",
        "plain URL is unchanged");

    check(protocolLabel("HTTP://example.com/a") == "HTTP", "HTTP protocol");
    check(protocolLabel("https://example.com/a") == "HTTPS", "HTTPS protocol");
    check(protocolLabel("ftp://example.com/a") == "FTP", "FTP protocol");
    check(protocolLabel("magnet:?xt=urn:btih:abc") == "BitTorrent", "magnet protocol");
    check(protocolLabel("ed2k://|file|a|1|H|/") == "ED2K", "ed2k protocol");
    check(protocolLabel("htp://example.com/a") == std::string(unknownValue),
        "unknown protocol uses the em dash");
}

void searchAndFilter()
{
    auto completed = makeRow("a", "Report-2026.PDF");
    completed.state = TaskState::Completed;
    completed.address = "http://example.com/Files/report.pdf";
    completed.destination = "D:/Downloads/报告/Report-2026.PDF";
    auto running = makeRow("b", "工具包.zip");
    running.state = TaskState::Running;
    running.active = true;
    running.address = redactAddress("http://user:pass@example.com/tool.zip?token=secret");
    auto interrupted = makeRow("c", "interrupted.bin");
    interrupted.state = TaskState::Running;
    interrupted.interrupted = true;
    auto failed = makeRow("d", "failed.bin");
    failed.state = TaskState::Failed;

    check(matchesSearch(completed, "report"), "case-insensitive name search");
    check(matchesSearch(completed, "REPORT"), "uppercase query");
    check(matchesSearch(completed, "报告"), "Chinese path search");
    check(matchesSearch(running, "example.com"), "address search");
    const auto redactedRunning = redactAddress(running.address);
    check(redactedRunning.find("secret") == std::string::npos,
        ("debug redaction: " + redactedRunning).c_str());
    check(!matchesSearch(running, "zzzz"), "debug unrelated query does not match");
    check(!matchesSearch(running, "secret"),
        ("redacted credential is not searchable: name=" + running.name + " addr="
            + redactedRunning + " dest=" + running.destination).c_str());
    check(!matchesSearch(running, "user:pass"),
        ("redacted userinfo is not searchable: " + redactedRunning).c_str());
    check(!matchesSearch(failed, "missing"), "non-matching query");

    check(matchesFilter(running, TaskFilter::Active), "live running task is active");
    check(!matchesFilter(interrupted, TaskFilter::Active),
        "persisted running row without a worker is not active");
    check(matchesFilter(completed, TaskFilter::Completed), "completed filter");
    check(matchesFilter(failed, TaskFilter::Failed), "failed filter");
    check(!matchesFilter(completed, TaskFilter::Failed), "completed is not failed");
    check(rowMatches(running, TaskFilter::Active, "TOOL"), "query ANDs with filter");
    check(!rowMatches(running, TaskFilter::Failed, "TOOL"), "filter excludes the row");
}

void optionalColumnText()
{
    auto row = makeRow("a", "包.zip");
    row.total = -1;
    row.received = 0;
    row.state = TaskState::Completed;
    row.createdAt = 1'700'000'000'000;
    row.completedAt = 1'700'000'100'000;
    check(columnText(row, ColumnId::Protocol) == "HTTP", "protocol text");
    check(columnText(row, ColumnId::UploadSpeed) == std::string(unknownValue),
        "upload speed is unknown, not zero");
    check(columnText(row, ColumnId::Connections) == std::string(unknownValue),
        "connection count is unknown");
    check(columnText(row, ColumnId::Sources) == std::string(unknownValue),
        "source count is unknown");
    check(columnText(row, ColumnId::Uploaded) == std::string(unknownValue),
        "uploaded bytes are unknown");
    check(columnText(row, ColumnId::Size) == std::string(unknownValue), "unknown size keeps em dash");
    check(columnText(row, ColumnId::Downloaded) == "0 B", "downloaded zero is a known zero");
    check(columnText(row, ColumnId::AddedTime) != std::string(unknownValue),
        "added time comes from the persisted row");
    check(columnText(row, ColumnId::CompletedTime) != std::string(unknownValue),
        "completed time comes from the persisted row");
    row.completedAt = 0;
    check(columnText(row, ColumnId::CompletedTime) == std::string(unknownValue),
        "uncompleted task has no completion time");
    row.address.clear();
    row.destination.clear();
    check(columnText(row, ColumnId::Address) == std::string(unknownValue), "no address is unknown");
    check(columnText(row, ColumnId::SavePath) == std::string(unknownValue), "no path is unknown");
}

void numericAndTimeSorting()
{
    std::vector<TaskRow> rows;
    auto smallFile = makeRow("small", "zzz-small.bin");
    smallFile.total = 900 * 1024;
    smallFile.received = smallFile.total;
    smallFile.size = "900 KB";
    auto largeFile = makeRow("large", "aaa-large.bin");
    largeFile.total = 1024 * 1024;
    largeFile.received = largeFile.total;
    largeFile.size = "1.00 MB";
    auto unknownSize = makeRow("unknown-size", "mmm-unknown.bin");
    unknownSize.total = -1;
    rows = {largeFile, unknownSize, smallFile};

    sortRows(rows, ColumnId::Size, SortDirection::Ascending);
    check((ids(rows) == std::vector<std::string>{"small", "large", "unknown-size"}),
        "size sorts by bytes (900 KB before 1 MB), unknown last");
    sortRows(rows, ColumnId::Size, SortDirection::Descending);
    check((ids(rows) == std::vector<std::string>{"large", "small", "unknown-size"}),
        "descending keeps unknown last");

    // Equal sizes tie-break by stable TaskId even in descending order.
    auto first = makeRow("a-tie");
    first.total = 100;
    auto second = makeRow("b-tie");
    second.total = 100;
    rows = {second, first};
    sortRows(rows, ColumnId::Size, SortDirection::Descending);
    check((ids(rows) == std::vector<std::string>{"a-tie", "b-tie"}),
        "ties use TaskId ascending");

    auto fast = makeRow("fast");
    fast.state = TaskState::Running;
    fast.active = true;
    fast.speedKnown = true;
    fast.speedBytesPerSecond = 2 * 1024 * 1024;
    auto slow = makeRow("slow");
    slow.state = TaskState::Running;
    slow.active = true;
    slow.speedKnown = true;
    slow.speedBytesPerSecond = 1024 * 1024;
    auto idle = makeRow("idle");
    idle.state = TaskState::Queued;
    rows = {fast, idle, slow};
    sortRows(rows, ColumnId::DownloadSpeed, SortDirection::Ascending);
    check((ids(rows) == std::vector<std::string>{"slow", "fast", "idle"}),
        "speed sorts numerically with non-transferring last");

    auto firstTime = makeRow("first");
    firstTime.createdAt = 1000;
    auto secondTime = makeRow("second");
    secondTime.createdAt = 2000;
    auto noTime = makeRow("none");
    rows = {secondTime, noTime, firstTime};
    sortRows(rows, ColumnId::AddedTime, SortDirection::Ascending);
    check((ids(rows) == std::vector<std::string>{"first", "second", "none"}),
        "time sorts by persisted timestamp, unknown last");

    auto doneEarlier = makeRow("done-earlier");
    doneEarlier.completedAt = 50;
    auto doneLater = makeRow("done-later");
    doneLater.completedAt = 100;
    rows = {doneLater, noTime, doneEarlier};
    sortRows(rows, ColumnId::CompletedTime, SortDirection::Ascending);
    check((ids(rows) == std::vector<std::string>{"done-earlier", "done-later", "none"}),
        "completion time is numeric and unknown stays last");
}

void filteredStatusText()
{
    TaskListModel model;
    TaskSnapshot running;
    running.id = "r";
    running.source = "http://example.com/r";
    running.destination = "D:/Downloads/r.bin";
    running.state = TaskState::Running;
    running.total = 100;
    running.received = 50;
    TaskSnapshot completed;
    completed.id = "c";
    completed.source = "http://example.com/c";
    completed.destination = "D:/Downloads/c.bin";
    completed.state = TaskState::Completed;
    completed.total = 100;
    completed.received = 100;
    model.update({running, completed}, {"r"}, 1000);
    const auto status = model.status(1, true);
    check(status.tasks == "可见: 1 / 2", "filtered first status part shows visible / total");
    check(status.active == "活动: 1", "active part stays whole-queue");
    const auto unfiltered = model.status();
    check(unfiltered.tasks == "任务: 2", "unfiltered first part shows total");
}
} // namespace

int main()
{
    return run("ui-view", [] {
        redactionAndProtocol();
        searchAndFilter();
        optionalColumnText();
        numericAndTimeSorting();
        filteredStatusText();
        return 0;
    });
}
