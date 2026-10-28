// UI boundary tests that do not need a window: column formatting, action
// enablement, status parts and source/destination parsing are the contract the
// Win32 controls display, so they are verified directly.
#include "task_list_model.h"
#include "ui_text.h"
#include "url_filename.h"
#include "list_layout.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace litefetch;
using namespace litefetch::ui;

namespace {
void check(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}

std::string utf8(const char8_t* text)
{
    return std::string(reinterpret_cast<const char*>(text));
}

TaskSnapshot task(std::string id, TaskState state, std::int64_t received, std::int64_t total,
    std::string destination = "D:/Downloads/文件.bin")
{
    TaskSnapshot snapshot;
    snapshot.id = std::move(id);
    snapshot.source = "http://127.0.0.1:1/" + snapshot.id;
    snapshot.destination = std::move(destination);
    snapshot.state = state;
    snapshot.received = received;
    snapshot.total = total;
    return snapshot;
}

void listLayout()
{
    // 998 is the ListView client width of the 1000 px design window at 96 DPI
    // (1000 client minus the native 2 px ListView border).
    auto widths = listColumnWidths(998, 96);
    check(widths == std::array<int, 6>{342, 104, 82, 126, 108, 236},
        "design-width client keeps the Figma pitches and fills the status column");

    widths = listColumnWidths(1000, 96);
    check(widths == std::array<int, 6>{342, 104, 82, 126, 108, 238},
        "extra width above the design size goes only to the status column");
    widths = listColumnWidths(1300, 96);
    check(widths == std::array<int, 6>{342, 104, 82, 126, 108, 538},
        "resizing wider keeps the five pitches and grows only the status column");

    // Narrow-window boundary: 998/997 and 763/762/761 must stay proportional,
    // keep 状态 readable and avoid a width cliff or 0/1 px columns.
    widths = listColumnWidths(997, 96);
    check(widths[5] == 235, "one pixel below the design width still fills the client");
    widths = listColumnWidths(900, 96);
    check(widths == std::array<int, 6>{308, 94, 74, 114, 97, 213},
        "900 px client shrinks every column proportionally and keeps a readable status column");
    const auto at763 = listColumnWidths(763, 96);
    const auto at762 = listColumnWidths(762, 96);
    const auto at761 = listColumnWidths(761, 96);
    check(at763 == std::array<int, 6>{261, 80, 63, 96, 83, 180},
        "763 px client uses the proportional layout with a readable status column");
    check(at762 == std::array<int, 6>{261, 79, 63, 96, 82, 181},
        "762 px client does not collapse the status column");
    check(at761 == std::array<int, 6>{261, 79, 63, 96, 82, 180},
        "761 px client stays in the same proportional layout");
    check(std::abs(at763[5] - at762[5]) <= 2 && std::abs(at762[5] - at761[5]) <= 2,
        "no width cliff across the old 762 px threshold");
    for (const auto* layout : {&at763, &at762, &at761}) {
        for (const int width : *layout) check(width >= 40, "narrow layout has no 0/1 px column");
    }
    widths = listColumnWidths(640, 96);
    check(widths == std::array<int, 6>{219, 67, 53, 81, 69, 151},
        "minimum window width keeps all six columns usable");

    // DPI boundary: at 120 DPI the design client width is 1249 px. At or above
    // it the scaled pitches stay fixed; below it every column shrinks.
    const auto at1249 = listColumnWidths(1249, 120);
    check(at1249 == std::array<int, 6>{428, 130, 103, 158, 135, 295},
        "120 DPI design width keeps the scaled pitches");
    const auto at1250 = listColumnWidths(1250, 120);
    check(at1250[5] == 296 && at1250[0] == 428, "120 DPI extra width goes to the status column");
    widths = listColumnWidths(1000, 120);
    check(widths == std::array<int, 6>{343, 104, 82, 127, 108, 236},
        "below the 120 DPI design width all six columns shrink; the status column stays readable");

    for (const int clientWidth : {640, 761, 762, 763, 900, 998, 1000, 1300}) {
        for (const int dpi : {96, 120, 144, 168}) {
            const auto layout = listColumnWidths(clientWidth, dpi);
            int total = 0;
            int smallest = layout[0];
            for (const int width : layout) {
                total += width;
                smallest = std::min(smallest, width);
            }
            check(total == clientWidth, "column widths always fill the ListView client width");
            check(smallest >= 40, "no column collapses at any tested width or DPI");
            check(layout[5] >= 120, "status column stays readable at any tested width or DPI");
        }
    }
}

void formatting()
{
    check(formatSize(-1) == unknownValue, "unknown size must render as the unknown marker");
    check(formatSize(0) == "0 B", "zero bytes");
    check(formatSize(512) == "512 B", "bytes");
    check(formatSize(12 * 1024) == "12 KB", "kilobytes use the reference integer form");
    check(formatSize(24LL * 1024 * 1024) == "24 MB", "megabytes");
    check(formatSize(800LL * 1024 * 1024) == "800 MB", "800 MB");
    check(formatSize(2LL * 1024 * 1024 * 1024) == "2.00 GB", "small gigabyte values keep two decimals");

    check(formatProgress(0, 1000) == "0.0%", "zero progress");
    check(formatProgress(600, 1000) == "60.0%", "one decimal progress");
    check(formatProgress(1000, 1000) == "100.0%", "complete progress");
    check(formatProgress(0, 0) == "100.0%", "known empty transfer is complete");
    check(formatProgress(10, -1) == unknownValue, "unknown total has no percentage");

    check(formatSpeed(0) == unknownValue, "zero speed is unknown");
    check(formatSpeed(8.0 * 1024 * 1024) == "8.0 MB/s", "speed formatting");
    check(formatSpeed(12.4 * 1024 * 1024) == "12.4 MB/s", "speed keeps a decimal");
    check(formatSpeed(1024.0 * 1024 * 1024) == "1.0 GB/s", "large speed unit");

    check(formatRemaining(-1) == unknownValue, "unknown remaining time");
    check(formatRemaining(40) == "00:40", "seconds");
    check(formatRemaining(270) == "04:30", "minutes");
    check(formatRemaining(3725) == "1:02:05", "hours");

    check(fileNameFromPath("D:\\下载\\工具包.zip") == utf8(u8"工具包.zip"), "Chinese file name from a path");
    check(fileNameFromPath("/a/b/c.txt") == "c.txt", "posix style path");
    check(fileNameFromPath("D:/Downloads/") == "", "trailing separator has no file name");
}

void sourceParsing()
{
    const auto https = analyzeSourceInput(
        utf8(u8"https://example.org/releases/%E5%B7%A5%E5%85%B7%E5%8C%85.zip"));
    check(https.kind == SourceKind::Https, "https detection");
    check(https.suggestedFileName == utf8(u8"工具包.zip"), "percent-decoded Chinese file name");

    const auto http = analyzeSourceInput("http://127.0.0.1:8080/file.bin?token=1#frag");
    check(http.kind == SourceKind::Http, "http detection");
    check(http.suggestedFileName == "file.bin", "query and fragment are removed");

    const auto multiple = analyzeSourceInput("http://a/1.bin\nhttp://b/2.bin\n");
    check(multiple.kind == SourceKind::Multiple, "multiple addresses are recognized");
    check(analyzeSourceInput("").kind == SourceKind::Empty, "empty input");
    check(analyzeSourceInput("   \r\n ").kind == SourceKind::Empty, "whitespace only input");
    check(analyzeSourceInput("ftp://example.org/a").kind == SourceKind::Unsupported, "ftp is not supported yet");
    check(analyzeSourceInput("magnet:?xt=urn:btih:abc").kind == SourceKind::Unsupported, "magnet is not supported yet");

    check(suggestedFileName("https://example.org/") == "download.bin", "root URL falls back to a name");
    check(suggestedFileName("https://example.org/a%2Fb.bin") == "a_b.bin", "path separators are sanitized");
    check(suggestedFileName("https://example.org/bad<name>.bin") == "bad_name_.bin", "reserved characters");
}

void modelProgressAndActions()
{
    const auto root = std::filesystem::temp_directory_path()
        / ("litefetch-model-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    const auto queuedDestination = (root / "queued.bin").string();
    const auto runningDestination = (root / "running.bin").string();
    const auto completedDestination = (root / "completed.bin").string();
    {
        std::ofstream completedFile(completedDestination, std::ios::binary);
        completedFile << "done";
    }

    TaskListModel model;
    const auto queued = task("queued", TaskState::Queued, 0, 1000, queuedDestination);
    const auto running = task("running", TaskState::Running, 0, 4 * 1024 * 1024,
        runningDestination);
    const auto completed = task("completed", TaskState::Completed, 1000, 1000,
        completedDestination);
    model.update({queued, running, completed}, {"running"}, 1000);

    check(model.rows().size() == 3, "row count");
    check(model.rows()[0].status == utf8(u8"等待中"), "queued status text");
    check(model.rows()[1].status == utf8(u8"正在连接"), "running without bytes is connecting");
    check(model.rows()[1].name == "running.bin", "row name uses the file name");
    check(model.rows()[1].size == "4.00 MB", "row size");
    check(model.rows()[1].progress == "0.0%", "row progress");
    check(model.rows()[2].status == utf8(u8"已完成"), "completed status text");

    check(model.actionsFor("queued").start, "queued task can start");
    check(model.actionsFor("queued").stop, "queued task can stop");
    check(!model.actionsFor("running").start, "running task cannot start again");
    check(model.actionsFor("running").stop, "running task can stop");
    check(!model.actionsFor("completed").start, "completed task cannot start");
    check(!model.actionsFor("completed").stop, "completed task cannot stop");
    check(!model.actionsFor("running").pause, "pause stays disabled until resume exists");
    check(model.actionsFor("running").deleteTask, "delete record is available in the list ticket");
    check(!model.actionsFor("running").options, "options stays disabled until implemented");
    check(model.actionsFor("running").openFolder,
        "open folder is eligible when the parent exists");
    check(!model.actionsFor("running").openFile,
        "an incomplete task cannot open a file");
    check(model.actionsFor("completed").openFile,
        "a completed task with a real file can open it");
    check(model.actionsFor("completed").openFolder,
        "a completed task can open its folder");
    check(model.actionsFor("running").copyLink, "copy link needs a selection");
    check(!model.actionsFor("").openFolder, "no selection has no folder action");

    const auto multiple = model.actionsForSelection({"queued", "running", "completed"});
    check(multiple.selectedCount == 3, "selection count");
    check(multiple.startCount == 1 && multiple.stopCount == 2,
        "mixed selection counts only eligible tasks");
    check(multiple.deleteCount == 3, "all selected records are deletable");
    check(multiple.openFileCount == 0 && multiple.openFolderCount == 0,
        "open actions require exactly one selected task");
    check(multiple.copyLinkCount == 3 && multiple.copyPathCount == 3,
        "mixed copy reports eligible counts");

    TaskListModel failedModel;
    failedModel.update({task("failed", TaskState::Failed, 10, 100)}, {}, 1000);
    check(failedModel.actionsFor("failed").start, "failed task can be retried by starting it");
    check(!failedModel.actionsFor("failed").stop, "failed task cannot be stopped");
    check(failedModel.rows()[0].status == utf8(u8"下载失败"), "failed status text");

    // One second later the same task has transferred 1 MiB: the row must show a
    // measured speed and the remaining time implied by it.
    model.update({queued, task("running", TaskState::Running, 1024 * 1024, 4 * 1024 * 1024,
                     runningDestination),
                     completed},
        {"running"}, 2000);
    const auto& row = model.rows()[1];
    check(std::fabs(row.speedBytesPerSecond - 1024.0 * 1024.0) < 2048.0,
        "speed is derived from the real progress delta: " + std::to_string(row.speedBytesPerSecond));
    check(row.progress == "25.0%", "progress after one MiB");
    check(row.remaining == "00:03", "remaining time from the measured speed: " + row.remaining);
    check(row.status == utf8(u8"下载中"), "running with bytes is downloading");

    const auto status = model.status();
    check(status.tasks == utf8(u8"任务: 3"), "status task count");
    check(status.active == utf8(u8"活动: 1"), "status active count");
    check(status.download == utf8(u8"↓ 1.0 MB/s"), "status download speed: " + status.download);
    check(status.upload == utf8(u8"↑ —"), "upload is unknown, not fabricated");

    // A persisted running row without a worker thread is an interrupted task.
    model.update({task("stale", TaskState::Running, 10, 100)}, {}, 3000);
    check(model.rows()[0].status == utf8(u8"已中断"), "stale running row is shown as interrupted");
    check(model.rows()[0].speed == unknownValue, "interrupted row has no speed");
    check(model.status().download == utf8(u8"↓ —"), "no active transfer shows unknown aggregate speed");

    const auto filtered = model.status(1, true);
    check(filtered.tasks == utf8(u8"可见: 1 / 1"), "filtered status shows visible / total");
    check(filtered.active == utf8(u8"活动: 0"), "filtered status keeps whole-queue active count");

    std::error_code cleanupError;
    std::filesystem::remove_all(root, cleanupError);
}
} // namespace

int main()
{
    try {
        formatting();
        sourceParsing();
        listLayout();
        modelProgressAndActions();
        std::cout << "ui-model: PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "ui-model: FAIL: " << error.what() << '\n';
        return 1;
    }
}
