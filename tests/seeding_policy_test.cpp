#include "seeding_policy.h"
#include "seeding_settings.h"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <limits>
int main(int argc,char** argv)
{
    QCoreApplication app(argc,argv);
    using namespace downloader;
    if(!validSeedingPolicy({0,0})||validSeedingPolicy({-1,0})||validSeedingPolicy({0,100001}))return 1;
    if(!seedingLimitReached({},0,0,100))return 2;
    if(seedingLimitReached({2,0},1999,0,100)||!seedingLimitReached({2,0},2000,0,100))return 3;
    if(seedingLimitReached({0,150},100000,149,100)||!seedingLimitReached({0,150},0,150,100))return 4;
    if(!seedingLimitReached({20,150},20000,0,100)||!seedingLimitReached({20,150},0,150,100))return 5;
    if(!seedingLimitReached({0,100},0,0,0))return 6;
    const auto large=(std::numeric_limits<qint64>::max)();
    if(seedingLimitReached({0,200},0,large,large)||!seedingLimitReached({0,100},0,large,large))return 7;
    QTemporaryDir directory(QCoreApplication::applicationDirPath()+"/seeding-settings-XXXXXX");
    if(!directory.isValid())return 8;
    const auto path=directory.filePath("bt-seeding.json");
    if(loadSeedingPolicy(path).seconds!=0)return 9;
    saveSeedingPolicy(path,{3600,150});const auto restored=loadSeedingPolicy(path);
    if(restored.seconds!=3600||restored.ratioPercent!=150)return 10;
    try{saveSeedingPolicy(path,{-1,0});return 11;}catch(const std::invalid_argument&){}
    if(loadSeedingPolicy(path).seconds!=3600)return 12;
    QFile file(path);if(!file.open(QIODevice::WriteOnly|QIODevice::Truncate))return 13;
    const QByteArray malformed="{\"version\":1,\"seconds\":0.5,\"ratioPercent\":100}";
    if(file.write(malformed)!=malformed.size())return 14;file.close();
    try{loadSeedingPolicy(path);return 15;}catch(const std::runtime_error&){}
    if(!file.open(QIODevice::ReadOnly)||file.readAll()!=malformed)return 16;
}
