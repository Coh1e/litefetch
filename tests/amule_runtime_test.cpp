#include "amule_runtime.h"
#include "amule_api.h"
#include <QCoreApplication>
#include <QDir>
#include <QTemporaryDir>
#include <QThread>
#include <iostream>
using namespace downloader;
static void check(bool value,const char* text){if(!value)throw std::runtime_error(text);}
int main(int argc,char** argv){
    QCoreApplication app(argc,argv);
    try{
        check(argc==3,"Expected runtime binaries and project-local temporary parent");
        QTemporaryDir state(QDir(app.arguments().at(2)).filePath("amule-runtime-XXXXXX"));
        check(state.isValid(),"Cannot create runtime fixture");
        state.setAutoRemove(false);
        std::cout<<"Runtime fixture: "<<state.path().toStdString()<<'\n';
        const AmuleRuntimeOptions options{app.arguments().at(1),state.path()};
        AmuleConnection previous;
        const QString hash="3456789abcdef0123456789abcdef012";
        {
            AmuleRuntime runtime(options);check(runtime.running(),"Runtime not ready");
            previous=runtime.connection();
            AmuleApi api(previous.port,previous.password);
            bool duplicateRejected=false;
            try{AmuleRuntime duplicate(options);}catch(const std::exception&){duplicateRejected=true;}
            check(duplicateRejected,"Duplicate state owner accepted");
            check(api.connected(),"Duplicate attempt harmed original runtime");
            const auto request=std::get<DownloadRequest>(parseRequest("ed2k://|file|runtime-fixture.bin|32768|"+hash+"|/"));
            api.add(request);
            Ed2kSnapshot snapshot;
            for(int i=0;i<30;++i){try{snapshot=api.task(hash);break;}catch(const AmuleApiError& error){if(error.status!=404)throw;}QThread::msleep(100);}
            check(snapshot.hash==hash,"Created task missing");
            api.resume(hash);api.pause(hash);check(api.task(hash).status=="paused","Pause failed");
        }
        bool closed=false;try{AmuleApi old(previous.port,previous.password);}catch(const AmuleApiError&){closed=true;}
        check(closed,"API still alive after runtime destruction");
        {
            AmuleRuntime runtime(options);const auto current=runtime.connection();
            check(current.password!=previous.password,"Credentials did not rotate");
            AmuleApi api(current.port,current.password);const auto restored=api.task(hash);
            check(restored.status=="stopped"||restored.status=="paused","Inactive task not restored");
            check(restored.size==32768&&restored.completed==0,"Restored snapshot mismatch");
        }
        std::cout<<"aMule runtime: random credentials/ports, exclusive ownership, real controls, shutdown and restart persistence passed\n";
        state.setAutoRemove(true);
        return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
