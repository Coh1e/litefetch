#include "task_manager.h"
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QProcess>
#include <QSemaphore>
#include <QTemporaryDir>
#include <QTextStream>
#include <QTimer>
#include <sqlite3.h>
#include <chrono>
#include <mutex>

using namespace downloader;
namespace {
bool waitUntil(const std::function<bool()>& condition, int timeout = 5000)
{
    QElapsedTimer clock; clock.start();
    while (!condition() && clock.elapsed() < timeout) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return condition();
}
DownloadRequest request(const QString& suffix)
{
    return std::get<DownloadRequest>(parseRequest("https://example.org/" + suffix));
}
bool executeSql(const QString& path, const char* sql)
{
    sqlite3* db = nullptr;
    if (sqlite3_open(path.toUtf8().constData(), &db) != SQLITE_OK) { sqlite3_close(db); return false; }
    const auto code = sqlite3_exec(db, sql, nullptr, nullptr, nullptr);
    sqlite3_close(db); return code == SQLITE_OK;
}
}
int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    if (app.arguments().value(1) == "--seed-ui") {
        TaskStore store(app.arguments().value(2));
        const auto source = request("restored-example.zip");
        store.save({"ui-restore-fixture", source, app.arguments().value(2) + ".zip", taskIdentity(source),
            TaskState::Interrupted, 0, 1, 123456, 1048576, QStringLiteral("恢复测试记录；不会自动发起网络请求。")});
        return 0;
    }
    if (app.arguments().value(1) == "--crash-child") {
        TaskManager manager(app.arguments().value(2), 1, [](const auto&, const auto&, const auto& cancelled, const auto&) {
            while (!cancelled.load()) std::this_thread::sleep_for(std::chrono::milliseconds(10));
            return TransferResult{TransferStatus::Cancelled};
        });
        const auto result = manager.add(request("crash"), app.arguments().value(2) + ".bin");
        if (!result.added) return 10;
        QTimer::singleShot(100, &app, [&] {
            if (manager.tasks()[0].state == TaskState::Running) QTextStream(stdout) << "RUNNING\n" << Qt::flush;
        });
        return app.exec();
    }
    int failures = 0;
    auto check = [&](bool ok, const char* message) { if (!ok) { ++failures; QTextStream(stderr) << "FAIL: " << message << '\n'; } };
    QTemporaryDir directory(QCoreApplication::applicationDirPath() + "/manager-tests-XXXXXX");
    if (!directory.isValid()) return 2;
    {
        const auto errorDatabase=directory.filePath("error-types.sqlite");
        {
            TaskManager manager(errorDatabase,1,[](const auto&,const auto&,const auto&,const auto& progress){
                progress(TransferProgress{123,1000,TransferPhase::Seeding,456,789});
                return TransferResult{TransferStatus::Failed,TransferError::Disk,QStringLiteral("磁盘测试原因"),0,123};
            });
            manager.setSeedingPolicy({30,150});
            check(manager.seedingPolicy().seconds==30&&manager.seedingPolicy().ratioPercent==150,"manager accepts future-run seeding policy");
            bool invalidPolicy=false;try{manager.setSeedingPolicy({-1,0});}catch(const std::invalid_argument&){invalidPolicy=true;}
            check(invalidPolicy&&manager.seedingPolicy().seconds==30,"invalid policy leaves current configuration unchanged");
            check(manager.add(request("disk-failure"),directory.filePath("disk-error.bin")).added,"accept classified error fixture");
            check(waitUntil([&]{return manager.tasks()[0].state==TaskState::Failed;}),"worker failure delivered");
            check(manager.tasks()[0].errorType==TransferError::Disk,"worker classification reaches task");
            check(manager.tasks()[0].uploaded==456&&manager.tasks()[0].uploadBytesPerSecond==0,
                "live upload counter reaches task and stopped rate clears");
        }
        {
            TaskStore store(errorDatabase);const auto restored=store.load();
            check(restored.size()==1&&restored[0].errorType==TransferError::Disk&&restored[0].received==123
                &&restored[0].error==QStringLiteral("磁盘测试原因"),"error category and reason survive database reopen");
        }
        check(executeSql(errorDatabase,"ALTER TABLE tasks DROP COLUMN error_type; PRAGMA user_version=4;"),"create legacy version 4 error fixture");
        {
            TaskStore store(errorDatabase);const auto migrated=store.load();
            check(migrated.size()==1&&migrated[0].errorType==TransferError::None
                &&migrated[0].error==QStringLiteral("磁盘测试原因"),"version 4 migration retains reason without guessing category");
        }
    }
    const auto database = directory.filePath("任务.sqlite");
    QSemaphore permits;
    std::atomic_int active{0}, peak{0};
    std::mutex mutex;
    QStringList order;
    auto runner = [&](const DownloadRequest& req, const QString&, const std::atomic_bool& cancelled, const auto& progress) {
        const auto count = active.fetch_add(1) + 1;
        int previous = peak.load();
        while (count > previous && !peak.compare_exchange_weak(previous, count)) {}
        { std::lock_guard guard(mutex); order.append(req.source); }
        progress(TransferProgress{123, 1000});
        while (!cancelled.load() && !permits.tryAcquire(1, 10)) {}
        --active;
        return TransferResult{cancelled.load() ? TransferStatus::Cancelled : TransferStatus::Completed,
            TransferError::None, {}, 200, 1000};
    };
    QString lastId;
    QStringList insertionOrder;
    {
        TaskManager manager(database, 2, runner);
        const auto a = manager.add(request("a"), directory.filePath("a.bin"));
        const auto b = manager.add(request("b"), directory.filePath("b.bin"), 10);
        const auto c = manager.add(request("c"), directory.filePath("c.bin"), 5);
        const auto d = manager.add(request("d?quoted='值'"), directory.filePath("d'中.bin"));
        lastId = d.id;
        insertionOrder = {a.id, b.id, c.id, d.id};
        check(a.added && b.added && c.added && d.added, "durable task acceptance");
        { TaskStore independent(database);
          const auto accepted = independent.load();
          check(accepted.size() == 4 && accepted[0].state == TaskState::Queued,
              "accepted records visible to independent connection before dispatch"); }
        const auto duplicate = manager.add(request("b"), directory.filePath("duplicate.bin"));
        check(!duplicate.added && duplicate.id == b.id, "source duplicate selects existing task");
        check(!manager.add(request("other"), directory.filePath("b.bin")).added, "destination collision rejected");
        check(manager.cancel(a.id), "cancel queued task");
        check(waitUntil([&] { std::lock_guard guard(mutex); return active.load() == 2 && order.size() == 2; }), "workers start");
        { std::lock_guard guard(mutex);
          check(order.size() == 2 && order.contains(request("b").source) && order.contains(request("c").source), "highest priority tasks dispatched first"); }
        check(!manager.remove(b.id) && !manager.retry(b.id), "active task cannot be removed or restarted");
        check(manager.cancel(b.id), "cancel running task");
        check(waitUntil([&] {
            for (const auto& task : manager.tasks()) if (task.id == b.id) return task.state == TaskState::Cancelled;
            return false;
        }), "worker cancellation reaches terminal state");
        permits.release(8);
        check(waitUntil([&] {
            for (const auto& task : manager.tasks()) if (task.state == TaskState::Running || task.state == TaskState::Queued) return false;
            return true;
        }), "remaining queue drains");
        check(peak.load() <= 2, "concurrency ceiling across cancellation and scheduling");
        { std::lock_guard guard(mutex); check(order.size() == 3 && !order.contains(request("a").source), "cancelled queued task never dispatched"); }
        check(manager.retry(a.id), "retry cancelled task");
        check(waitUntil([&] { return manager.tasks()[0].state == TaskState::Completed; }), "retry eventually completes");
        manager.setSeedingPolicy({30,150});
        check(!manager.canReseed(manager.tasks()[0].id)&&!manager.resume(manager.tasks()[0].id)
            &&manager.tasks()[0].state==TaskState::Completed,"seeding policy cannot restart completed HTTP task");
    }
    {
        TaskStore reopened(database);
        const auto tasks = reopened.load();
        check(tasks.size() == 4 && tasks[0].state == TaskState::Completed, "records survive close and reopen");
        QStringList reopenedOrder;
        for (const auto& task : tasks) reopenedOrder.append(task.id);
        check(reopenedOrder == insertionOrder, "FIFO ordering survives reopen for rapid additions");
        bool unicode = false;
        for (const auto& task : tasks) if (task.id == lastId) unicode = task.destination.endsWith("d'中.bin") && task.request.source.contains("quoted=");
        check(unicode, "bound SQL preserves quotes and Unicode");
    }
    {
        TaskManager manager(database, 1, runner);
        check(manager.remove(lastId) && manager.tasks().size() == 3, "remove persisted record");
        check(!manager.retry("nonexistent"), "unknown task rejected");
    }
    const auto failedDatabase = directory.filePath("failure.sqlite");
    {
        std::atomic_int dispatched{0};
        TaskManager manager(failedDatabase, 1, [&](const auto&, const auto&, const auto&, const auto&) {
            ++dispatched; return TransferResult{TransferStatus::Completed};
        });
        bool errorSeen = false;
        QObject::connect(&manager, &TaskManager::storageError, &app, [&](const auto&) { errorSeen = true; });
        check(executeSql(failedDatabase, "CREATE TRIGGER fail_start BEFORE UPDATE OF state ON tasks WHEN NEW.state=1 BEGIN SELECT RAISE(ABORT,'injected storage failure'); END"), "install storage fault fixture");
        check(manager.add(request("storage-failure"), directory.filePath("failure.bin")).added, "queued acceptance before injected start failure");
        check(waitUntil([&] { return errorSeen; }), "storage failure is reported");
        check(dispatched.load() == 0 && manager.tasks()[0].state == TaskState::Queued, "no transfer starts without committed running state");
        check(!manager.add(request("after-error"), directory.filePath("after.bin")).added, "fail closed after storage error");
    }
    const auto crashDatabase = directory.filePath("crash.sqlite");
    {
        const auto legacy = directory.filePath("legacy.sqlite");
        check(executeSql(legacy, "CREATE TABLE tasks(id TEXT PRIMARY KEY NOT NULL, source TEXT NOT NULL, destination TEXT NOT NULL,"
            "destination_key TEXT NOT NULL UNIQUE, identity TEXT NOT NULL UNIQUE, state INTEGER NOT NULL CHECK(state BETWEEN 0 AND 6),"
            "priority INTEGER NOT NULL, created INTEGER NOT NULL, received INTEGER NOT NULL, total INTEGER NOT NULL, error TEXT NOT NULL);"
            "INSERT INTO tasks VALUES('00000000000000000000000000000001','https://example.org/legacy','D:/legacy.bin','d:/legacy.bin','legacy',4,0,1,0,0,'');"
            "PRAGMA user_version=1;"), "create version 1 database fixture");
        TaskStore upgraded(legacy); auto records = upgraded.load();
        check(records.size() == 1 && records[0].state == TaskState::Cancelled, "schema migration retains old task");
        if (!records.isEmpty()) { records[0].state = TaskState::Paused; upgraded.save(records[0]); }
        check(upgraded.load()[0].state == TaskState::Paused, "migrated schema accepts persistent pause state");
    }
    QProcess child;
    child.setProgram(QCoreApplication::applicationFilePath());
    child.setArguments({"--crash-child", crashDatabase});
    child.start();
    check(child.waitForStarted(3000) && child.waitForReadyRead(5000) && child.readAllStandardOutput().contains("RUNNING"), "child committed running task before crash");
    child.kill(); check(child.waitForFinished(3000), "force child process termination");
    {
        std::atomic_int dispatched{0};
        TaskManager manager(crashDatabase, 1, [&](const auto&, const auto&, const auto&, const auto&) {
            ++dispatched; return TransferResult{TransferStatus::Completed};
        });
        check(manager.tasks().size() == 1 && manager.tasks()[0].state == TaskState::Interrupted, "abrupt process exit recovers as interrupted");
        QCoreApplication::processEvents();
        check(dispatched.load() == 0, "interrupted task waits for explicit retry");
    }
    QTextStream(stdout) << "Task persistence/scheduler checks: " << failures << " failures\n";
    return failures ? 1 : 0;
}
