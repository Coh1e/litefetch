#include "task_list_model.h"

#include "ui_text.h"

#include <algorithm>
#include <cmath>
#include <set>
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
        row.state = snapshot.state;
        row.received = snapshot.received;
        row.total = snapshot.total;
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
            row.speed = formatSpeed(sample.speed);
            if (snapshot.total >= 0 && sample.speed > 0.0) {
                const auto remainingBytes = snapshot.total - snapshot.received;
                row.remaining = formatRemaining(remainingBytes > 0
                        ? static_cast<double>(remainingBytes) / sample.speed
                        : 0.0);
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
    ActionState actions;
    const auto index = indexOf(selectedId);
    if (index >= rows_.size()) return actions;
    const auto& row = rows_[index];
    actions.start = row.state == TaskState::Queued || row.state == TaskState::Failed;
    actions.stop = row.state == TaskState::Queued || row.state == TaskState::Running;
    actions.openFolder = !row.destination.empty();
    actions.copyLink = !row.source.empty();
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
    StatusTexts texts;
    std::size_t active = 0;
    for (const auto& row : rows_) {
        if (row.active) ++active;
    }
    texts.tasks = "任务: " + std::to_string(rows_.size());
    texts.active = "活动: " + std::to_string(active);
    texts.download = "\xE2\x86\x93 " + formatSpeed(totalDownloadSpeed());
    // Upload/seeding is not part of this ticket; the field must not invent a
    // number for a capability the engine does not provide yet.
    texts.upload = "\xE2\x86\x91 " + std::string(unknownValue);
    return texts;
}

} // namespace litefetch::ui
