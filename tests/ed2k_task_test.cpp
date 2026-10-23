#include "task_manager.h"
#include "ed2k_part_cache.h"
#include "ed2k_hash.h"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QDir>
#include <QFile>
#include <QTcpServer>
#include <QElapsedTimer>
#include <QProcess>
#include <iostream>
#include <cstdlib>
using namespace downloader;
static void check(bool v,const char* m){if(!v)throw std::runtime_error(m);}
static bool wait(const std::function<bool()>& predicate){QElapsedTimer timer;timer.start();while(!predicate()&&timer.elapsed()<5000){QCoreApplication::processEvents();std::this_thread::sleep_for(std::chrono::milliseconds(1));}return predicate();}
int main(int argc,char** argv){QCoreApplication app(argc,argv);try{
    const auto hash=QByteArray::fromHex("a448017aaf21d8525fc10ae87aa6729d");
    const auto request=std::get<DownloadRequest>(parseRequest("ed2k://|file|abc.bin|3|"+QString::fromLatin1(hash.toHex())+"|/"));
    if(argc==3&&QString::fromLocal8Bit(argv[1])=="--seed-publication"){
        const auto folder=QString::fromLocal8Bit(argv[2]),target=folder+"/result.bin",taskId=QStringLiteral("publicationcrash");
        TaskStore store(folder+"/tasks.sqlite");store.save({taskId,request,target,taskIdentity(request),TaskState::Running,0,1});
        ed2k::FileReceiver receiver(target,hash,3);receiver.accept({hash,{0,3},"abc"});
        ed2k::PartCache cache(ed2kTaskCachePath(folder+"/ed2k-cache",taskId),hash,3);
        cache.storeHashes({hash});cache.storePart(receiver,0);cache.preparePublication(target);receiver.publish();cache.clear(true);
        std::_Exit(0); // File published, SQLite still Running, no destructor/unlock.
    }
    QTemporaryDir dir(QDir::currentPath()+"/ed2k-task-XXXXXX");check(dir.isValid(),"Test directory failed");
    const auto db=dir.filePath("tasks.sqlite"),destination=dir.filePath("abc.bin"),root=dir.filePath("ed2k-cache");QString id;
    Ed2kRuntimeOptions options;options.server=QHostAddress::LocalHost;options.serverPort=1;options.userHash=QByteArray(16,'u');options.operationTimeoutMs=200;
    {
        const auto blocker=dir.filePath("not-a-directory");QFile file(blocker);
        check(file.open(QIODevice::WriteOnly)&&file.write("keep")==4,"Create blocked destination fixture");file.close();
        std::atomic_bool cancelled{false};
        const auto disk=runEd2kTask(request,blocker+"/child.bin","disk-failure",root,options,cancelled,{});
        check(disk.status==TransferStatus::Failed&&disk.error==TransferError::Disk&&disk.received==0,
            "Staging creation failure is storage, not network");
        const auto conflict=runEd2kTask(request,blocker,"destination-conflict",root,options,cancelled,{});
        check(conflict.status==TransferStatus::Failed&&conflict.error==TransferError::Destination,
            "Unowned existing destination is classified without overwriting");
        check(file.open(QIODevice::ReadOnly)&&file.readAll()=="keep","Error classification modified existing file");
    }
    const auto seed=[&](const QString& taskId){ed2k::FileReceiver receiver(dir.filePath("seed.bin"),hash,3);receiver.accept({hash,{0,3},"abc"});
        ed2k::PartCache cache(ed2kTaskCachePath(root,taskId),hash,3);cache.storeHashes({hash});cache.storePart(receiver,0);};
    {
        TaskManager manager(db,1,{},nullptr,{},options);const auto added=manager.add(request,destination);check(added.added,"ed2k task rejected");id=added.id;
        check(manager.pause(id)&&manager.tasks()[0].state==TaskState::Paused,"Queued pause failed");seed(id);
    }
    {
        TaskManager manager(db,1,{},nullptr,{},options);check(manager.tasks().size()==1&&manager.tasks()[0].state==TaskState::Paused,"Paused task not restored");
        check(manager.resume(id)&&wait([&]{return manager.tasks()[0].state==TaskState::Completed;}),"Cached task did not complete through queue");
        check(manager.tasks()[0].received==3&&manager.tasks()[0].total==3,"Task progress not delivered");
        QFile file(destination);check(file.open(QIODevice::ReadOnly)&&file.readAll()=="abc","Task result differs");file.close();
        check(manager.remove(id)&&QFile::exists(destination),"Removing record removed final file");
    }
    {
        TaskManager manager(db,1,{},nullptr,{},options);const auto added=manager.add(request,dir.filePath("retry.bin"));check(added.added,"Retry task rejected");seed(added.id);
        check(manager.cancel(added.id),"Queued cancel failed");
        const auto metadata=QDir(ed2kTaskCachePath(root,added.id)).filePath(QString::fromLatin1(hash.toHex())+"-3.hashset");
        check(QFile::exists(metadata),"Cancel discarded verified cache");
        check(manager.retry(added.id)&&!QFile::exists(metadata)&&manager.pause(added.id),"From-start retry kept cache");
        check(manager.remove(added.id),"Paused task removal failed");
    }
    {
        QTcpServer server;check(server.listen(QHostAddress::LocalHost,0),"Silent server fixture failed");options.serverPort=server.serverPort();
        options.operationTimeoutMs=30000;
        TaskManager manager(db,1,{},nullptr,{},options);const auto added=manager.add(request,dir.filePath("paused.bin"));check(added.added,"Running pause task rejected");
        check(wait([&]{return server.hasPendingConnections();}),"Task did not reach real TCP server");
        check(!manager.configureEd2k(options),"Settings changed while ed2k worker was running");
        QElapsedTimer pauseTimer;pauseTimer.start();
        check(manager.pause(added.id)&&wait([&]{return manager.tasks()[0].state==TaskState::Paused;})&&pauseTimer.elapsed()<1500,"Running ed2k pause waited for long timeout");
        check(manager.configureEd2k(options),"Settings remained locked after pause");
        check(!QFile::exists(dir.filePath("paused.bin")),"Paused task published incomplete file");
        check(manager.remove(added.id),"Running-pause record cleanup failed");
        options.operationTimeoutMs=200;
    }
    {
        TaskManager manager(db);check(!manager.add(request,dir.filePath("unconfigured.bin")).added,"Unconfigured ed2k accepted");
    }
    for(int scenario=0;scenario<3;++scenario){
        const auto taskId="publication-"+QString::number(scenario),target=dir.filePath(taskId+".bin");
        const auto folder=ed2kTaskCachePath(root,taskId);
        const auto receipt=QDir(folder).filePath(QString::fromLatin1(hash.toHex())+"-3.publishing");
        {
            ed2k::FileReceiver receiver(target,hash,3);receiver.accept({hash,{0,3},"abc"});
            ed2k::PartCache cache(folder,hash,3);cache.storeHashes({hash});cache.storePart(receiver,0);
            if(scenario!=2)cache.preparePublication(target);
            receiver.publish();cache.clear(scenario!=2);
        }
        if(scenario==1){QFile changed(target);check(changed.open(QIODevice::WriteOnly)&&changed.write("xyz")==3,"Changed final file fixture failed");}
        {TaskStore store(db);store.save({taskId,request,target,taskIdentity(request),TaskState::Running,0,1});}
        {
            TaskManager manager(db,1,{},nullptr,{},options);
            check(manager.tasks().size()==1&&manager.tasks()[0].state==TaskState::Interrupted,"Publication gap not restored as interrupted");
            check(manager.resume(taskId)&&wait([&]{const auto s=manager.tasks()[0].state;return s==TaskState::Completed||s==TaskState::Failed;}),"Publication recovery did not terminate");
            check(manager.tasks()[0].state==(scenario==0?TaskState::Completed:TaskState::Failed),"Unverified existing file accepted or valid receipt rejected");
            QFile result(target);check(result.open(QIODevice::ReadOnly)&&result.readAll()==(scenario==1?"xyz":"abc"),"Publication recovery modified existing file");result.close();
            if(scenario==0)check(!QFile::exists(receipt)&&manager.tasks()[0].received==3,"Committed publication receipt retained");
            check(manager.remove(taskId),"Publication record could not be removed");
            check(QFile::exists(target),"Publication record removal deleted final file");
        }
    }
    {
        const auto folder=dir.filePath("crash");check(QDir().mkpath(folder),"Crash directory failed");
        QProcess child;child.start(QCoreApplication::applicationFilePath(),{"--seed-publication",folder});
        check(child.waitForFinished(10000)&&child.exitStatus()==QProcess::NormalExit&&child.exitCode()==0,"Publication gap child failed");
        const auto receipt=QDir(ed2kTaskCachePath(folder+"/ed2k-cache","publicationcrash")).filePath(QString::fromLatin1(hash.toHex())+"-3.publishing");
        check(QFile::exists(receipt)&&QFile::exists(folder+"/result.bin"),"Child did not leave publication gap");
        {
            TaskManager manager(folder+"/tasks.sqlite",1,{},nullptr,{},options);
            check(manager.tasks().size()==1&&manager.tasks()[0].state==TaskState::Interrupted,"Crash DB not recovered");
            check(manager.resume("publicationcrash")&&wait([&]{return manager.tasks()[0].state==TaskState::Completed;}),"Cross-process publication not accepted");
            check(!QFile::exists(receipt),"Receipt removed before/after wrong DB state");
        }
        TaskStore persisted(folder+"/tasks.sqlite");check(persisted.load()[0].state==TaskState::Completed,"Completion not committed to SQLite");
    }
    std::cout<<"ed2k task queue integration tests passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
