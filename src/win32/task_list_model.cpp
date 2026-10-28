#include "task_list_model.h"

#include "task_view.h"
#include "text_utf8.h"
#include "ui_text.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <set>
#include <system_error>
#include <utility>

namespace litefetch::ui {
namespace {
std::string stateText(const TaskRow& row)
{
    switch (row.state) {
    case TaskState::Queued: return "等待中";
    case TaskState::Running:
        if (row.interrupted) return "已中断";
        return row.received > 0 ? "下载中" : "正在连接";
    case TaskState::Completed: return "已完成";
    case TaskState::Failed: return "下载失败";
    case TaskState::Cancelled: return "已停止";
    }
    return std::string(unknownValue);
}
} // namespace

void TaskListModel::update(std::vector<TaskSnapshot> tasks, const std::vector<std::string>& activeIds,
    std::int64_t nowMillis)
{
    const std::set<std::string, std::less<>> active(activeIds.begin(), activeIds.end());
    std::map<std::string, Sample, std::less<>> samples;
    std::vector<TaskRow> rows;
    rows.reserve(tasks.size());
    for (const auto& snapshot : tasks) {
        TaskRow row;
        row.id = snapshot.id;
        row.source = snapshot.source;
        row.destination = snapshot.destination;
        row.name = fileNameFromPath(snapshot.destination);
        if (row.name.empty()) row.name = snapshot.source;
        row.protocol = protocolLabel(snapshot.source);
        row.address = redactAddress(snapshot.source);
        row.savePath = snapshot.destination;
        row.error = snapshot.error;
        row.state = snapshot.state;
        row.received = snapshot.received;
        row.total = snapshot.total;
        row.createdAt = snapshot.createdAt;
        row.completedAt = snapshot.state == TaskState::Completed ? snapshot.updatedAt : 0;
        row.updatedAt = snapshot.updatedAt;
        row.active = active.contains(snapshot.id);
        row.interrupted = snapshot.state == TaskState::Running && !row.active;
        row.size = formatSize(snapshot.total);
        row.progress = formatProgress(snapshot.received, snapshot.total);

        if (row.active && snapshot.state == TaskState::Running) {
            Sample sample;
            const auto previous = samples_.find(snapshot.id);
            if (previous != samples_.end()) {
                sample = previous->second;
                const auto elapsed = nowMillis - previous->second.at;
                if (elapsed >= 200) {
                    const double instant = static_cast<double>(snapshot.received - previous->second.received)
                        * 1000.0 / static_cast<double>(elapsed);
                    sample.speed = previous->second.speed > 0.0
                        ? previous->second.speed * 0.5 + std::max(0.0, instant) * 0.5
                        : std::max(0.0, instant);
                    sample.received = snapshot.received;
                    sample.at = nowMillis;
                }
            } else {
                sample.received = snapshot.received;
                sample.at = nowMillis;
            }
            samples.emplace(snapshot.id, sample);
            row.speedBytesPerSecond = sample.speed;
            row.speedKnown = true;
            row.speed = formatSpeed(sample.speed);
            if (snapshot.total >= 0 && sample.speed > 0.0) {
                const auto remainingBytes = snapshot.total - snapshot.received;
                row.remainingSeconds = remainingBytes > 0
                    ? static_cast<double>(remainingBytes) / sample.speed : 0.0;
                row.remainingKnown = true;
                row.remaining = formatRemaining(row.remainingSeconds);
            } else {
                row.remaining = std::string(unknownValue);
            }
        } else {
            row.speed = std::string(unknownValue);
            row.remaining = std::string(unknownValue);
        }
        row.status = stateText(row);
        rows.push_back(std::move(row));
    }
    rows_ = std::move(rows);
    samples_ = std::move(samples);
}

std::size_t TaskListModel::indexOf(std::string_view id) const
{
    for (std::size_t index = 0; index < rows_.size(); ++index) {
        if (rows_[index].id == id) return index;
    }
    return rows_.size();
}

ActionState TaskListModel::actionsFor(std::string_view selectedId) const
{
    if (selectedId.empty()) return ActionState{};
    return actionsForSelection({std::string(selectedId)});
}

ActionState TaskListModel::actionsForSelection(const std::vector<std::string>& selectedIds) const
{
    ActionState actions;
    actions.selectedCount = selectedIds.size();
    const bool single = selectedIds.size() == 1;
    for (const auto& id : selectedIds) {
        const auto index = indexOf(id);
        if (index >= rows_.size()) continue;
        const auto& row = rows_[index];
        if (row.state == TaskState::Queued || row.state == TaskState::Failed) ++actions.startCount;
        if (row.state == TaskState::Queued || row.state == TaskState::Running) ++actions.stopCount;
        ++actions.deleteCount;
        if (!row.source.empty()) ++actions.copyLinkCount;
        if (!row.destination.empty()) ++actions.copyPathCount;

        if (!single || row.destination.empty()) continue;
        std::filesystem::path destination;
        try {
            destination = pathFromUtf8(row.destination);
        } catch (const std::exception&) {
            continue;
        }
        std::error_code error;
        const bool fileExists = std::filesystem::is_regular_file(destination, error);
        if (row.state == TaskState::Completed && fileExists) ++actions.openFileCount;
        const auto parent = destination.parent_path();
        if (fileExists || (!parent.empty() && std::filesystem::is_directory(parent, error)))
            ++actions.openFolderCount;
    }
    actions.start = actions.startCount > 0;
    actions.stop = actions.stopCount > 0;
    actions.deleteTask = actions.deleteCount > 0;
    actions.openFile = actions.openFileCount > 0;
    actions.openFolder = actions.openFolderCount > 0;
    actions.copyLink = actions.copyLinkCount > 0;
    actions.copyPath = actions.copyPathCount > 0;
    return actions;
}

double TaskListModel::totalDownloadSpeed() const
{
    double total = 0.0;
    for (const auto& row : rows_) total += row.speedBytesPerSecond;
    return total;
}

StatusTexts TaskListModel::status() const
{
    return status(rows_.size(), false);
}

StatusTexts TaskListModel::status(std::size_t visibleCount, bool filtered) const
{
    StatusTexts texts;
    std::size_t active = 0;
    for (const auto& row : rows_) {
        if (row.active) ++active;
    }
    texts.tasks = filtered
        ? "可见: " + std::to_string(visibleCount) + " / " + std::to_string(rows_.size())
        : "任务: " + std::to_string(rows_.size());
    texts.active = "活动: " + std::to_string(active);
    texts.download = "\xE2\x86\x93 " + formatSpeed(totalDownloadSpeed());
    // Upload/seeding is not part of this ticket; the field must not invent a
    // number for a capability the engine does not provide yet.
    texts.upload = "\xE2\x86\x91 " + std::string(unknownValue);
    return texts;
}

} // namespace litefetch::ui
