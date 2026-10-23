#include "bandwidth_settings.h"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QDir>
#include <iostream>
using namespace downloader;
static void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
static QByteArray read(const QString& path){QFile file(path);check(file.open(QIODevice::ReadOnly),"Read failed");return file.readAll();}
int main(int argc,char** argv){QCoreApplication app(argc,argv);try{
    QTemporaryDir directory;check(directory.isValid(),"Temp directory unavailable");const auto path=directory.filePath("bandwidth.json");
    const auto defaults=loadBandwidthPreferences(path);check(defaults.directDownload==0&&defaults.btDownload==0&&defaults.btUpload==131072,"Missing config defaults changed");
    saveBandwidthPreferences(path,{1024,2048,4096});const auto saved=read(path);const auto loaded=loadBandwidthPreferences(path);
    check(loaded.directDownload==1024&&loaded.btDownload==2048&&loaded.btUpload==4096,"Settings roundtrip failed");
    const auto capped=effectiveBandwidthPreferences({0,1024*1024,128*1024,true});
    check(capped.directDownload==128*1024&&capped.btDownload==128*1024&&capped.btUpload==32*1024,"Low usage caps not applied");
    const BandwidthPreferences custom{8192,16384,4096,true};const auto lower=effectiveBandwidthPreferences(custom);
    check(lower.directDownload==8192&&lower.btDownload==16384&&lower.btUpload==4096,"Low usage raised a smaller custom limit");
    auto restored=custom;restored.lowUsage=false;const auto original=effectiveBandwidthPreferences(restored);
    check(original.directDownload==custom.directDownload&&original.btDownload==custom.btDownload&&original.btUpload==custom.btUpload,"Disabling mode lost custom values");
    saveBandwidthPreferences(path,{262144,524288,65536,true});const auto mode=loadBandwidthPreferences(path);
    check(mode.lowUsage&&mode.directDownload==262144&&mode.btDownload==524288&&mode.btUpload==65536,"Mode persistence rewrote base limits");
    saveBandwidthPreferences(path,loaded);
    bool rejected=false;try{saveBandwidthPreferences(path,{-1,0,0});}catch(const std::exception&){rejected=true;}
    check(rejected&&read(path)==saved,"Invalid settings replaced previous file");
    for(const auto& bytes:QList<QByteArray>{"{", "[]", "{\"version\":2}",
        "{\"version\":1,\"directDownload\":\"0\",\"btDownload\":0,\"btUpload\":0}",
        "{\"version\":1,\"directDownload\":1.5,\"btDownload\":0,\"btUpload\":0}",
        "{\"version\":1,\"directDownload\":0,\"btDownload\":0,\"btUpload\":0,\"lowUsage\":1}",
        "{\"version\":1,\"directDownload\":1073741825,\"btDownload\":0,\"btUpload\":0}",
        "{\"version\":1,\"directDownload\":0,\"btUpload\":0}",QByteArray(4097,'x')}){
        QFile file(path);check(file.open(QIODevice::WriteOnly|QIODevice::Truncate)&&file.write(bytes)==bytes.size(),"Fixture write failed");file.close();
        rejected=false;try{loadBandwidthPreferences(path);}catch(const std::exception&){rejected=true;}
        check(rejected&&read(path)==bytes,"Malformed file accepted or modified");
    }
    {QFile file(path);check(file.open(QIODevice::WriteOnly|QIODevice::Truncate),"Legacy fixture failed");file.write("{\"version\":1,\"directDownload\":0,\"btDownload\":0,\"btUpload\":0}");file.close();
     check(!loadBandwidthPreferences(path).lowUsage,"Legacy settings enabled mode unexpectedly");}
    saveBandwidthPreferences(path,{1073741824,0,1073741824});check(loadBandwidthPreferences(path).btUpload==1073741824,"Boundary rate rejected");
    const auto before=read(path);rejected=false;try{saveBandwidthPreferences(directory.path(),{});}catch(const std::exception&){rejected=true;}
    check(rejected&&read(path)==before,"Save failure damaged existing settings");
    std::cout<<"Bandwidth settings validation, preservation and atomic saves passed\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
