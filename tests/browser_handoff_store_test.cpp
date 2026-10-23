#include "task_manager.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QTextStream>
#include <sqlite3.h>
#include <chrono>
#include <future>
#include <thread>
using namespace downloader;
namespace {
const QString origin = "chrome-extension://" + QString(32,'a') + "/";
DownloadRequest source(const QString& name = "file") { return std::get<DownloadRequest>(parseRequest("https://example.test/"+name)); }
bool sql(const QString& path, const char* command)
{
    sqlite3* db = nullptr;
    if (sqlite3_open(path.toUtf8().constData(),&db) != SQLITE_OK) { sqlite3_close(db); return false; }
    const auto code = sqlite3_exec(db,command,nullptr,nullptr,nullptr); sqlite3_close(db); return code == SQLITE_OK;
}
bool waitUntil(const std::function<bool()>& condition)
{
    QElapsedTimer clock; clock.start();
    while (!condition() && clock.elapsed()<5000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents,5); std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return condition();
}
}
int main(int argc, char** argv)
{
    QCoreApplication app(argc,argv);
    if (app.arguments().value(1)=="--commit-child") {
        TaskStore store(app.arguments().value(2)); store.commitHandoff(origin,"crash",1001);
        QTextStream(stdout)<<"COMMITTED\n"<<Qt::flush;
        std::this_thread::sleep_for(std::chrono::seconds(30)); return 0;
    }
    QTemporaryDir directory(app.applicationDirPath()+"/handoff-store-XXXXXX");
    if (!directory.isValid()) return 10;
    int failures=0;
    const auto check=[&](bool ok,const char* name) { if (!ok) { ++failures; QTextStream(stderr)<<"FAIL "<<name<<'\n'; } };
    const auto rejects=[&](auto operation,const char* name) { bool threw=false; try { operation(); } catch (...) { threw=true; } check(threw,name); };
    const auto database=directory.filePath("事务.sqlite"), destination=directory.filePath("文件.bin");
    QString id;
    {
        TaskStore store(database);
        const auto prepared=store.prepareHandoff(origin,"first",source(),destination,1000);
        id=prepared.taskId;
        check(store.load().isEmpty(),"prepare cannot enter runnable queue");
        const auto duplicate=store.prepareHandoff(origin,"first",source(),destination,2000);
        check(duplicate.taskId==id && duplicate.expires==prepared.expires,"prepare retry preserves identity and lease");
        rejects([&] { store.prepareHandoff(origin,"first",source("different"),destination,2000); },"request ID cannot change source");
        rejects([&] { store.prepareHandoff(origin,"other",source(),directory.filePath("different.bin"),2000); },"prepared source is reserved");
        rejects([&] { store.prepareHandoff(origin,"other",source("other"),destination,2000); },"prepared destination is reserved");
        check(store.reserved(source(),directory.filePath("unused"),2000),"active reservation visible");
        rejects([&] { const auto request=source(); store.save({"manual",request,directory.filePath("manual.bin"),taskIdentity(request)}); },
            "low level task save respects prepared reservation");
    }
    {
        TaskStore store(database);
        check(store.queryHandoff(origin,"first",2000)->taskId==id && store.load().isEmpty(),"prepared state survives reopen without task creation");
        check(store.commitHandoff(origin,"first",2000).state==HandoffState::Committed,"commit accepted");
        check(store.commitHandoff(origin,"first",3000).taskId==id && store.load().size()==1,"duplicate commit has one queue row");
        check(store.abortHandoff(origin,"first",3000)->state==HandoffState::Committed,"abort cannot undo committed task");
        check(store.load()[0].id==id && store.load()[0].state==TaskState::Queued,"queue identity matches handoff");
        store.erase(id);
        store.commitHandoff(origin,"first",4000);
        check(store.load().isEmpty(),"removed committed task is not resurrected by replay");
    }
    {
        TaskStore store(database);
        auto expired=store.prepareHandoff(origin,"expires",source("expires"),directory.filePath("expires.bin"),1000,1000);
        check(store.commitHandoff(origin,"expires",2000).state==HandoffState::Expired,"expiry boundary cannot commit");
        check(store.load().isEmpty(),"expired task never queued");
        check(store.prepareHandoff(origin,"expires",source("expires"),expired.destination,3000).state==HandoffState::Expired,"expired ID cannot renew itself");
        auto aborted=store.prepareHandoff(origin,"abort",source("abort"),directory.filePath("abort.bin"),3000);
        store.abortHandoff(origin,"abort",3001);
        check(store.commitHandoff(origin,"abort",3002).state==HandoffState::Aborted,"aborted task cannot commit");
        check(!store.reserved(source("abort"),aborted.destination,3002),"abort releases reservation");
    }
    {
        TaskStore store(database);
        store.prepareHandoff(origin,"rollback",source("rollback"),directory.filePath("rollback.bin"),1000);
        check(sql(database,"CREATE TRIGGER fail_handoff_commit BEFORE UPDATE OF state ON browser_handoffs "
            "WHEN NEW.state=1 BEGIN SELECT RAISE(ABORT,'injected commit failure'); END"),"install transaction failure fixture");
        rejects([&] { store.commitHandoff(origin,"rollback",1001); },"commit marker failure propagates");
        check(store.load().isEmpty() && store.queryHandoff(origin,"rollback",1002)->state==HandoffState::Prepared,
            "failed marker rolls queue insertion back atomically");
        check(sql(database,"DROP TRIGGER fail_handoff_commit"),"remove injected trigger");
        check(store.commitHandoff(origin,"rollback",1003).state==HandoffState::Committed && store.load().size()==1,
            "retry after rollback creates exactly one task");
    }
    {
        TaskStore store(database);
        const auto blocked=directory.filePath("occupied.bin");
        store.prepareHandoff(origin,"occupied",source("occupied"),blocked,1000);
        QFile occupied(blocked); check(occupied.open(QIODevice::WriteOnly),"create destination race fixture"); occupied.write("keep"); occupied.close();
        const auto before=store.load().size();
        rejects([&] { store.commitHandoff(origin,"occupied",1001); },"destination appearing after prepare blocks commit");
        check(store.load().size()==before && store.queryHandoff(origin,"occupied",1002)->state==HandoffState::Prepared,
            "destination collision leaves reservation uncommitted");
        rejects([&] { store.prepareHandoff("unapproved-origin","bad",source("bad"),directory.filePath("bad.bin"),1000); },"invalid origin rejected");
        rejects([&] { store.prepareHandoff(origin,"bad id",source("bad"),directory.filePath("bad.bin"),1000); },"invalid request ID rejected");
        const auto ftp=std::get<DownloadRequest>(parseRequest("ftp://example.test/file"));
        rejects([&] { store.prepareHandoff(origin,"ftp",ftp,directory.filePath("ftp.bin"),1000); },"browser prepare rejects unsupported protocol");
    }
    {
        const auto crashDb=directory.filePath("crash.sqlite");
        { TaskStore store(crashDb); store.prepareHandoff(origin,"crash",source("crash"),directory.filePath("crash.bin"),1000); }
        QProcess process; process.start(app.applicationFilePath(),{"--commit-child",crashDb});
        check(process.waitForStarted(2000) && process.waitForReadyRead(5000),"commit child reaches committed boundary");
        check(process.readAllStandardOutput().contains("COMMITTED"),"child confirms durable commit before termination");
        process.kill(); process.waitForFinished(5000);
        TaskStore restored(crashDb);
        check(restored.queryHandoff(origin,"crash",1002)->state==HandoffState::Committed && restored.load().size()==1,
            "process death after commit before scheduling preserves one queued task");
        std::atomic_int recoveredStarts=0;
        TaskManager recovered(crashDb,1,[&](const auto&,const auto&,const auto&,const auto&) {
            ++recoveredStarts; return TransferResult{TransferStatus::Completed};
        });
        check(waitUntil([&] { return recovered.tasks().size()==1 && recovered.tasks()[0].state==TaskState::Completed; }),
            "normal manager restart adopts durable committed queue row");
        check(recoveredStarts==1,"recovered committed task starts once");
    }
    {
        const auto concurrentDb=directory.filePath("concurrent.sqlite"), target=directory.filePath("concurrent.bin");
        { TaskStore store(concurrentDb); }
        const auto prepare=[&] { TaskStore store(concurrentDb); return store.prepareHandoff(origin,"same",source("concurrent"),target,1000).taskId; };
        auto first=std::async(std::launch::async,prepare), second=std::async(std::launch::async,prepare);
        check(first.get()==second.get(),"separate SQLite connections agree on duplicate prepare identity");
    }
    {
        const auto managerDb=directory.filePath("manager.sqlite");
        std::atomic_int starts=0;
        TaskManager manager(managerDb,1,[&](const auto&,const auto&,const auto&,const auto&) { ++starts; return TransferResult{TransferStatus::Completed}; });
        const auto now=QDateTime::currentMSecsSinceEpoch();
        const auto prepared=manager.prepareBrowserHandoff(origin,"manager",source("manager"),directory.filePath("manager.bin"),now);
        check(prepared.expires>now+60000 && prepared.expires<=now+300000,"browser lease covers recovery interval within protocol bound");
        QCoreApplication::processEvents();
        check(starts==0 && manager.tasks().isEmpty(),"manager cannot schedule preparation");
        check(!manager.add(source("manager"),directory.filePath("manual.bin")).added,"manual add respects browser source reservation");
        check(manager.queryBrowserHandoff(origin,"manager",now+61000)->state==HandoffState::Prepared,"reservation remains prepared after nominal recovery alarm");
        manager.commitBrowserHandoff(origin,"manager",now+61001);
        check(waitUntil([&] { return manager.tasks().size()==1 && manager.tasks()[0].state==TaskState::Completed; }),"committed task executes in unified scheduler");
        manager.commitBrowserHandoff(origin,"manager",now+61002); QCoreApplication::processEvents();
        check(starts==1 && manager.tasks()[0].id==prepared.taskId,"duplicate manager commit never starts a second transfer");
    }
    {
        const auto migrationDb=directory.filePath("version3.sqlite");
        { TaskStore store(migrationDb); const auto request=source("migration");
          store.save({"old",request,directory.filePath("old.bin"),taskIdentity(request),TaskState::Paused}); }
        check(sql(migrationDb,"ALTER TABLE tasks DROP COLUMN error_type; DROP TABLE browser_handoffs; PRAGMA user_version=3"),"create prior schema fixture");
        TaskStore migrated(migrationDb);
        check(migrated.load().size()==1 && migrated.load()[0].state==TaskState::Paused,"schema 3 migration retains existing task");
        check(migrated.prepareHandoff(origin,"migrated",source("new"),directory.filePath("new.bin"),1000).state==HandoffState::Prepared,
            "migrated database supports browser preparation");
    }
    QTextStream(stdout)<<"Browser handoff storage checks: "<<(failures?"FAILED":"PASS")<<'\n';
    return failures?1:0;
}
