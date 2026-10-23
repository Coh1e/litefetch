#include "amule_api.h"
#include <QCoreApplication>
#include <QFile>
#include <QThread>
#include <iostream>
using namespace downloader;
static void check(bool passed, const char* message) { if (!passed) throw std::runtime_error(message); }
int main(int argc,char** argv)
{
    QCoreApplication app(argc,argv);
    try {
        check(argc==3,"Expected API port and fixture password file");
        bool valid=false;const auto port=app.arguments().at(1).toUShort(&valid);check(valid&&port,"Invalid fixture port");
        QFile password(app.arguments().at(2));check(password.open(QIODevice::ReadOnly),"Fixture password unavailable");
        const auto passwordBytes=password.readAll().trimmed();
        bool unauthorized=false;try{AmuleApi wrong(port,passwordBytes+"-incorrect");}
        catch(const AmuleApiError& error){unauthorized=error.status==401;}
        check(unauthorized,"Incorrect password was not rejected");
        AmuleApi api(port,passwordBytes);check(api.connected(),"Core is not connected");
        const QString hash="fedcba98765432100123456789abcdef";
        bool missing=false;
        try {api.task(hash);} catch(const AmuleApiError& error){missing=error.status==404;}
        check(missing,"Test task must not already exist");
        const auto parsed=parseRequest("ed2k://|file|native-api-fixture.bin|32768|"+hash+"|/");
        api.add(std::get<DownloadRequest>(parsed));
        Ed2kSnapshot snapshot;
        for(int i=0;i<30;++i){
            try{snapshot=api.task(hash);break;}catch(const AmuleApiError& error){if(error.status!=404)throw;}
            QThread::msleep(100);
        }
        check(snapshot.hash==hash&&snapshot.size==32768&&snapshot.completed==0,"Added task snapshot mismatch");
        api.resume(hash);api.pause(hash);
        check(api.task(hash).status=="paused","Pause did not reach daemon");
        api.priority(hash,"high");check(api.task(hash).priority=="high","Priority did not reach daemon");
        api.resume(hash);const auto resumed=api.task(hash);
        check(resumed.status!="paused"&&resumed.status!="stopped","Resume did not reach daemon");
        api.pause(hash);
        bool rejected=false;try{api.task("../preferences");}catch(const AmuleApiError&){rejected=true;}
        check(rejected,"Invalid task identity accepted");
        auto invalid=QJsonObject{{"hash",hash},{"name","bad"},{"status","paused"},
            {"size_bytes",10},{"completed_bytes",11},{"speed_bytes_per_second",0}};
        rejected=false;try{AmuleApi::parseSnapshot(invalid);}catch(const AmuleApiError&){rejected=true;}
        check(rejected,"Invalid progress accepted");
        std::cout<<"aMule native client: auth, health, add, snapshot, pause/resume, priority and invalid-data checks passed\n";
        return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
