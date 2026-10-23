#include "task_manager.h"
#include "http_resume.h"
#include <QDateTime>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QFile>
#include <QDir>
#include <QTimer>
#include <QUuid>
#include <algorithm>

namespace downloader {
QString taskStateText(TaskState state)
{
    switch (state) {
    case TaskState::Queued: return QStringLiteral("排队中");
    case TaskState::Running: return QStringLiteral("下载中");
    case TaskState::Cancelling: return QStringLiteral("正在取消");
    case TaskState::Completed: return QStringLiteral("已完成");
    case TaskState::Cancelled: return QStringLiteral("已取消");
    case TaskState::Failed: return QStringLiteral("下载失败");
    case TaskState::Interrupted: return QStringLiteral("上次运行中断");
    case TaskState::Pausing: return QStringLiteral("正在暂停");
    case TaskState::Paused: return QStringLiteral("已暂停");
    }
    return {};
}
TaskManager::TaskManager(const QString& databasePath, int concurrency, TransferRunner runner, QObject* parent, TorrentRuntimeOptions torrent,Ed2kRuntimeOptions ed2k)
    : QObject(parent), store_(databasePath), tasks_(store_.load()), runner_(std::move(runner)),
      torrentOptions_(std::move(torrent)),ed2kOptions_(std::move(ed2k)),
      ed2kCacheRoot_(QFileInfo(databasePath).absoluteDir().filePath("ed2k-cache")),concurrency_(qBound(1, concurrency, 32))
{
    for (auto& task : tasks_) {
        if (task.state == TaskState::Running || task.state == TaskState::Cancelling || task.state == TaskState::Pausing) {
            task.state = task.state == TaskState::Cancelling ? TaskState::Cancelled
                : task.state == TaskState::Pausing ? TaskState::Paused : TaskState::Interrupted;
            task.error = task.request.protocol == Protocol::Http || task.request.protocol == Protocol::BitTorrent || task.request.protocol == Protocol::Ed2k
                ? QStringLiteral("可继续下载；会先校验恢复数据。")
                : QStringLiteral("上次运行中断；重试将从头下载。");
            if (task.state == TaskState::Cancelled && task.request.protocol != Protocol::BitTorrent&&task.request.protocol!=Protocol::Ed2k) discardPartial(task.destination, task.id);
            store_.save(task);
        }
    }
    QTimer::singleShot(0, this, [this] { schedule(); });
}
TaskManager::~TaskManager()
{
    closing_ = true;
    for (auto& [id, worker] : workers_) worker.cancelled->store(true);
    workers_.clear();
    torrentEngine_.reset();
}
Task* TaskManager::find(const QString& id)
{
    const auto it = std::find_if(tasks_.begin(), tasks_.end(), [&](const auto& task) { return task.id == id; });
    return it == tasks_.end() ? nullptr : &*it;
}
bool TaskManager::persist(const Task& task)
{
    if (storageFailed_) return false;
    try { store_.save(task); return true; }
    catch (const std::exception& error) {
        storageFailed_ = true;
        for (auto& [id, worker] : workers_) worker.cancelled->store(true);
        emit storageError(QString::fromUtf8(error.what()));
        return false;
    }
}
AddTaskResult TaskManager::add(const DownloadRequest& request, const QString& destination, int priority, QVector<int> filePriorities)
{
    if (closing_ || storageFailed_) return {{}, false, QStringLiteral("任务存储不可用。")};
    if (request.protocol == Protocol::Ed2k&&!validEd2kOptions(ed2kOptions_))
        return {{}, false, QStringLiteral("请先配置有效的 ed2k 找源方式。")};
    if (filePriorities.size() > 100000 || (!filePriorities.isEmpty() && request.protocol != Protocol::BitTorrent))
        return {{}, false, QStringLiteral("文件选择不适用于该任务。")};
    bool wanted = filePriorities.isEmpty();
    for (const auto value : filePriorities) {
        if (value < 0 || value > 7) return {{}, false, QStringLiteral("BT 文件优先级无效。")};
        if (value > 0) wanted = true;
    }
    if (!wanted) return {{}, false, QStringLiteral("请至少选择一个文件。")};
    if (!QFileInfo(destination).isAbsolute() || QFileInfo::exists(destination))
        return {{}, false, QStringLiteral("请选择尚不存在的目标文件。")};
    const auto parsed = parseRequest(request.source);
    const auto* valid = std::get_if<DownloadRequest>(&parsed);
    if (!valid || valid->protocol != request.protocol) return {{}, false, QStringLiteral("下载链接无效。")};
    auto checked = *valid;
    if(checked.protocol==Protocol::Ed2k&&(!checked.expectedSize||!*checked.expectedSize||*checked.expectedSize>0xffffffffULL))
        return {{},false,QStringLiteral("当前 ed2k 实现支持大于零且小于 4 GiB 的文件。")};
    QByteArray metainfo;
    if (checked.protocol == Protocol::BitTorrent && !checked.source.startsWith("magnet:", Qt::CaseInsensitive)) {
        QFile input(checked.source);
        if (!input.open(QIODevice::ReadOnly) || input.size() > 16 * 1024 * 1024)
            return {{}, false, QStringLiteral("种子无法读取或过大。")};
        metainfo = input.readAll();
        const auto inspected = inspectTorrentData(metainfo);
        const auto* metadata = std::get_if<TorrentMetadata>(&inspected);
        if (!metadata) return {{}, false, std::get<InvalidRequest>(inspected).message};
        if (!request.identities.isEmpty() && request.identities != metadata->identities)
            return {{}, false, QStringLiteral("种子文件已变更，请重新选择。")};
        if (!filePriorities.isEmpty()) {
            const auto error = validateTorrentSelection(*metadata, filePriorities);
            if (!error.isEmpty()) return {{}, false, error};
        }
        const auto normalized = parseRequest(metadata->magnet);
        const auto* canonical = std::get_if<DownloadRequest>(&normalized);
        if (!canonical) return {{}, false, QStringLiteral("种子的磁力身份无法保存。")};
        checked = *canonical;
    }
    const auto identity = taskIdentity(checked);
    try {
        if (store_.reserved(checked,destination,QDateTime::currentMSecsSinceEpoch()))
            return {{},false,QStringLiteral("浏览器接管正在等待确认，请稍后再试。")};
    } catch (...) { return {{},false,QStringLiteral("无法核对浏览器接管预留，没有开始下载。")}; }
    for (const auto& task : tasks_) {
        bool sameTorrent = false;
        if (checked.protocol == Protocol::BitTorrent && task.request.protocol == Protocol::BitTorrent)
            for (const auto& alias : checked.identities) if (task.request.identities.contains(alias)) sameTorrent = true;
        if (task.identity == identity || sameTorrent || destinationKey(task.destination) == destinationKey(destination))
            return {task.id, false, QStringLiteral("任务已存在，请查看已有任务或移除记录后重新添加。")};
    }
    auto created = QDateTime::currentMSecsSinceEpoch();
    // Persist FIFO even when additions share a millisecond or the clock moves back.
    for (const auto& existing : tasks_) created = qMax(created, existing.created + 1);
    Task task{QUuid::createUuid().toString(QUuid::Id128), checked, QDir::cleanPath(destination), identity,
        TaskState::Queued, qBound(-100, priority, 100), created};
    task.filePriorities = std::move(filePriorities); task.torrentMetainfo = std::move(metainfo);
    if (!persist(task)) return {{}, false, QStringLiteral("任务未能保存，没有开始下载。")};
    tasks_.append(task);
    emit changed(task.id);
    QTimer::singleShot(0, this, [this] { schedule(); });
    return {task.id, true, {}};
}
bool TaskManager::cancel(const QString& id)
{
    auto* task = find(id);
    if (!task || task->state == TaskState::Completed || task->state == TaskState::Cancelled || task->state == TaskState::Cancelling) return false;
    auto next = *task;
    next.state = workers_.contains(id) ? TaskState::Cancelling : TaskState::Cancelled;
    if (!persist(next)) return false;
    *task = next;
    if (workers_.contains(id)) workers_.at(id).cancelled->store(true);
    else if (next.request.protocol == Protocol::BitTorrent) {
        task->error = QStringLiteral("BT 任务已取消，已下载数据保留。"); persist(*task);
    } else if(next.request.protocol==Protocol::Ed2k){
        task->error=QStringLiteral("ed2k 任务已取消，完整校验分块保留。");persist(*task);
    } else if (!discardPartial(next.destination, next.id)) {
        task->error = QStringLiteral("任务已取消，但未能清理全部临时数据。"); persist(*task);
    }
    emit changed(id);
    return true;
}
bool TaskManager::pause(const QString& id)
{
    auto* task = find(id);
    if (!task || (task->request.protocol != Protocol::Http && task->request.protocol != Protocol::BitTorrent&&task->request.protocol!=Protocol::Ed2k)
        || (task->state != TaskState::Running && task->state != TaskState::Queued)) return false;
    auto next = *task;
    next.state = workers_.contains(id) ? TaskState::Pausing : TaskState::Paused;
    if (!persist(next)) return false;
    *task = next;
    if (workers_.contains(id)) workers_.at(id).cancelled->store(true);
    emit changed(id); return true;
}
bool TaskManager::canReseed(const QString& id) const
{
    if(workers_.contains(id)||(torrentOptions_.seeding.seconds==0&&torrentOptions_.seeding.ratioPercent==0))return false;
    for(const auto& task:tasks_)if(task.id==id)return task.state==TaskState::Completed
        &&task.request.protocol==Protocol::BitTorrent&&QFileInfo(task.destination).isDir();
    return false;
}
bool TaskManager::resume(const QString& id)
{
    auto* task = find(id);
    if (!task || workers_.contains(id) || task->state == TaskState::Queued
        ||(task->state==TaskState::Completed&&!canReseed(id))) return false;
    auto next = *task; next.state = TaskState::Queued; next.error.clear();next.errorType=TransferError::None;
    next.phase=TransferPhase::Receiving;
    next.uploaded=-1;next.uploadBytesPerSecond=0;
    if (!persist(next)) return false;
    *task = next; emit changed(id);
    QTimer::singleShot(0, this, [this] { schedule(); });
    return true;
}
bool TaskManager::retry(const QString& id)
{
    auto* task = find(id);
    if (!task || workers_.contains(id) || task->state == TaskState::Queued || task->state == TaskState::Completed) return false;
    if(task->request.protocol==Protocol::Ed2k){try{discardEd2kCache(ed2kCacheRoot_,id,task->request);}catch(...){return false;}}
    if (task->request.protocol != Protocol::BitTorrent&&task->request.protocol!=Protocol::Ed2k && !discardPartial(task->destination, task->id)) return false;
    auto next = *task;
    next.state = TaskState::Queued; next.received = 0; next.total = 0; next.error.clear();next.errorType=TransferError::None;
    next.phase=TransferPhase::Receiving;next.uploaded=-1;next.uploadBytesPerSecond=0;
    if (!persist(next)) return false;
    *task = next; emit changed(id);
    QTimer::singleShot(0, this, [this] { schedule(); });
    return true;
}
bool TaskManager::remove(const QString& id)
{
    if (workers_.contains(id) || storageFailed_ || !find(id)) return false;
    const auto* task = find(id);
    if(task->request.protocol==Protocol::Ed2k){try{discardEd2kCache(ed2kCacheRoot_,id,task->request);}catch(...){return false;}}
    if (task->request.protocol != Protocol::BitTorrent&&task->request.protocol!=Protocol::Ed2k && !discardPartial(task->destination, task->id)) return false;
    try { store_.erase(id); }
    catch (const std::exception& error) { emit storageError(QString::fromUtf8(error.what())); return false; }
    tasks_.removeIf([&](const auto& task) { return task.id == id; });
    emit changed(id); return true;
}
bool TaskManager::setPriority(const QString& id, int priority)
{
    auto* task = find(id);
    if (!task) return false;
    auto next = *task; next.priority = qBound(-100, priority, 100);
    if (!persist(next)) return false;
    *task = next; emit changed(id); return true;
}
bool TaskManager::configureEd2k(Ed2kRuntimeOptions options){
    if((options.useKad||!options.server.isNull())&&!validEd2kOptions(options))return false;
    for(const auto& [id,worker]:workers_){const auto* task=find(id);if(task&&task->request.protocol==Protocol::Ed2k)return false;}
    ed2kOptions_=std::move(options);return true;
}
BrowserHandoff TaskManager::prepareBrowserHandoff(const QString& origin, const QString& requestId,
    const DownloadRequest& request, const QString& destination, qint64 now)
{
    if (closing_ || storageFailed_) throw std::runtime_error("Task storage unavailable");
    // The extension recovery alarm runs once a minute. Use a longer lease for
    // worker recovery; delayed browser alarms still cannot bypass expiry.
    return store_.prepareHandoff(origin,requestId,request,destination,now,120000);
}
BrowserHandoff TaskManager::commitBrowserHandoff(const QString& origin, const QString& requestId, qint64 now)
{
    if (closing_ || storageFailed_) throw std::runtime_error("Task storage unavailable");
    auto record = store_.commitHandoff(origin,requestId,now);
    if (record.state == HandoffState::Committed && !find(record.taskId)) {
        // SQLite queue row and commit marker are already durable together. A
        // crash before this adoption leaves a normal queued task on restart.
        for (const auto& task : store_.load()) if (task.id == record.taskId) {
            tasks_.append(task); emit changed(task.id); break;
        }
    }
    if (record.state == HandoffState::Committed) QTimer::singleShot(0,this,[this] { schedule(); });
    return record;
}
std::optional<BrowserHandoff> TaskManager::queryBrowserHandoff(const QString& origin, const QString& requestId, qint64 now)
{
    return store_.queryHandoff(origin,requestId,now);
}
std::optional<BrowserHandoff> TaskManager::abortBrowserHandoff(const QString& origin, const QString& requestId, qint64 now)
{
    return store_.abortHandoff(origin,requestId,now);
}
void TaskManager::schedule()
{
    if (closing_ || storageFailed_) return;
    while (static_cast<int>(workers_.size()) < concurrency_) {
        Task* selected = nullptr;
        for (auto& task : tasks_) {
            if (task.state == TaskState::Queued && (!selected || task.priority > selected->priority)) selected = &task;
        }
        if (!selected) return;
        auto task = *selected; task.state = TaskState::Running;
        if (!persist(task)) return;
        *selected = task;
        auto cancellation = std::make_shared<std::atomic_bool>(false);
        const auto runner = runner_;
        const auto seedingPolicy=torrentOptions_.seeding;
        auto& worker = workers_[task.id]; worker.cancelled = cancellation;
        try {
            worker.thread = std::jthread([this, task, cancellation, runner, seedingPolicy] {
                QElapsedTimer clock; clock.start();
                qint64 last = -100;
                TransferResult result;
                try {
                    const auto progress = [&](auto progressValue) {
                        const auto now = clock.elapsed();
                        if (now - last < 100) return;
                        last = now;
                        QMetaObject::invokeMethod(this, [this, id = task.id, progressValue] {
                            auto* current = find(id);
                            if (!current || !workers_.contains(id)) return;
                            current->received = progressValue.received; current->total = progressValue.total; current->phase = progressValue.phase;
                            current->uploaded=progressValue.uploaded;current->uploadBytesPerSecond=progressValue.uploadBytesPerSecond;
                            emit changed(id);
                        }, Qt::QueuedConnection);
                    };
                    if (runner) result = runner(task.request, task.destination, *cancellation, progress);
                    else if (task.request.protocol == Protocol::BitTorrent) {
                        TorrentRunOptions options;
                        options.seeding=seedingPolicy;
                        options.filePriorities = task.filePriorities; options.metainfo = task.torrentMetainfo; options.peers = torrentOptions_.peers;
                        options.selectFiles = [this, id = task.id](const auto& metadata, const auto& stopped) {
                            return requestTorrentSelection(id, metadata, stopped);
                        };
                        result = torrentEngine()->run(task.request, task.destination, task.id, *cancellation, progress, std::move(options));
                    } else if(task.request.protocol==Protocol::Ed2k){
                        result=runEd2kTask(task.request,task.destination,task.id,ed2kCacheRoot_,ed2kOptions_,*cancellation,progress,&httpDownloadLimiter_);
                    } else result = transferResumable(task.request, task.destination, task.id, *cancellation, progress,&httpDownloadLimiter_);
                } catch (...) {
                    result = {TransferStatus::Failed, TransferError::Internal, QStringLiteral("下载核心异常终止。")};
                }
                QMetaObject::invokeMethod(this, [this, id = task.id, result] { finish(id, result); }, Qt::QueuedConnection);
            });
        } catch (...) {
            finish(task.id, {TransferStatus::Failed, TransferError::Internal, QStringLiteral("无法启动下载线程。")});
        }
        emit changed(task.id);
    }
}
void TaskManager::finish(const QString& id, const TransferResult& result)
{
    workers_.erase(id);
    selections_.erase(id);
    auto* task = find(id);
    if (!task) return;
    auto next = *task;
    next.state = result.status == TransferStatus::Completed ? TaskState::Completed
        : task->state == TaskState::Pausing ? TaskState::Paused
        : task->state == TaskState::Cancelling || result.status == TransferStatus::Cancelled ? TaskState::Cancelled : TaskState::Failed;
    next.received = result.received; next.error = result.message;
    if(result.uploaded>=0)next.uploaded=result.uploaded;
    next.uploadBytesPerSecond=0;
    next.errorType=next.state==TaskState::Failed?result.error:TransferError::None;
    if (next.state == TaskState::Cancelled) {
        if (next.request.protocol == Protocol::BitTorrent) next.error = QStringLiteral("BT 任务已取消，已下载数据保留。");
        else if(next.request.protocol==Protocol::Ed2k)next.error=QStringLiteral("ed2k 任务已取消，完整校验分块保留。");
        else if (!discardPartial(next.destination, next.id)) next.error = QStringLiteral("任务已取消，但未能清理全部临时数据。");
    }
    if (persist(next)) {
        *task = next;
        if(next.state==TaskState::Completed&&next.request.protocol==Protocol::Ed2k){
            try{discardEd2kCache(ed2kCacheRoot_,id,next.request);}catch(...){/* Committed task remains complete; receipt may be cleaned on removal. */}
        }
        if (next.state == TaskState::Completed && next.request.protocol == Protocol::Http) acknowledgePublished(next.destination, next.id);
    }
    emit changed(id);
    schedule();
}
std::shared_ptr<TorrentEngine> TaskManager::torrentEngine()
{
    std::lock_guard guard(torrentMutex_);
    if (!torrentEngine_) torrentEngine_ = std::make_shared<TorrentEngine>(torrentOptions_.engine);
    return torrentEngine_;
}
void TaskManager::setTorrentRateLimits(TorrentRateLimits limits){
    if(limits.download<0||limits.upload<0)throw std::invalid_argument("Invalid BT rate limits");
    std::lock_guard guard(torrentMutex_);
    if(torrentEngine_)torrentEngine_->setRateLimits(limits);
    torrentOptions_.engine.downloadBytesPerSecond=limits.download;
    torrentOptions_.engine.uploadBytesPerSecond=limits.upload;
}
TorrentRateLimits TaskManager::torrentRateLimits(){
    std::lock_guard guard(torrentMutex_);
    return {torrentOptions_.engine.downloadBytesPerSecond,torrentOptions_.engine.uploadBytesPerSecond};
}
QVector<int> TaskManager::requestTorrentSelection(const QString& id, const TorrentMetadata& metadata, const std::atomic_bool& stopped)
{
    auto selection = std::make_shared<Selection>(); selection->metadata = metadata;
    auto future = selection->answer.get_future();
    QMetaObject::invokeMethod(this, [this, id, selection] {
        const auto* task = find(id);
        if (closing_ || !task || task->state != TaskState::Running) { selection->answer.set_value({}); return; }
        selections_[id] = selection;
        auto* current = find(id); current->phase = TransferPhase::ChoosingFiles; current->total = selection->metadata.total;
        emit changed(id);
        emit torrentFilesReady(id, selection->metadata);
    }, Qt::QueuedConnection);
    while (!stopped.load()) {
        if (future.wait_for(std::chrono::milliseconds(100)) == std::future_status::ready) return future.get();
    }
    return {};
}
bool TaskManager::selectTorrentFiles(const QString& id, const QVector<int>& priorities)
{
    const auto found = selections_.find(id);
    auto* task = find(id);
    if (found == selections_.end() || !task || task->state != TaskState::Running
        || !validateTorrentSelection(found->second->metadata, priorities).isEmpty()) return false;
    auto next = *task; next.filePriorities = priorities; next.phase = TransferPhase::CheckingFiles;
    if (!persist(next)) return false;
    *task = next;
    found->second->answer.set_value(priorities); selections_.erase(found);
    emit changed(id); return true;
}
}
