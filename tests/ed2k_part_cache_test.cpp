#include "ed2k_part_cache.h"
#include "ed2k_hash.h"
#include "ed2k_download.h"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <iostream>
#include <cstdlib>
#include <stdexcept>
using namespace downloader::ed2k;
static void check(bool v,const char* m){if(!v)throw std::runtime_error(m);}
template<class F>void rejects(F f){bool rejected=false;try{f();}catch(const std::exception&){rejected=true;}check(rejected,"Expected rejection");}
static QByteArray md4(QByteArrayView v){return QCryptographicHash::hash(v,QCryptographicHash::Md4);}
static void fill(FileReceiver& receiver,const QByteArray& hash,quint64 begin,QByteArrayView bytes){
    for(quint64 at=0;at<quint64(bytes.size());){const auto n=qMin<quint64>(blockSize,quint64(bytes.size())-at);
        receiver.accept({hash,{begin+at,begin+at+n},bytes.sliced(qsizetype(at),qsizetype(n)).toByteArray()});at+=n;}
}
int main(int argc,char** argv){QCoreApplication app(argc,argv);try{
    const QByteArray part(qsizetype(partSize),'x'),tail(12345,'t');
    const QVector<QByteArray> hashes{md4(part),md4(tail)};const auto hash=md4(hashes[0]+hashes[1]);const auto size=partSize+quint64(tail.size());
    if(argc==3&&QString::fromLocal8Bit(argv[1])=="--seed"){
        const auto root=QString::fromLocal8Bit(argv[2]);FileReceiver receiver(root+"/unused.bin",hash,size);
        fill(receiver,hash,0,part);PartCache cache(root+"/cache",hash,size);cache.storeHashes(hashes);cache.storePart(receiver,0);
        std::_Exit(0); // Intentionally skip every destructor, including cache unlock.
    }
    QTemporaryDir dir(QDir::currentPath()+"/ed2k-cache-XXXXXX");check(dir.isValid(),"Test directory failed");
    QProcess seed;seed.start(QCoreApplication::applicationFilePath(),{"--seed",dir.path()});
    check(seed.waitForFinished(10000)&&seed.exitStatus()==QProcess::NormalExit&&seed.exitCode()==0,"Separate-process checkpoint seed failed");
    const auto cacheDir=dir.filePath("cache");const auto prefix=cacheDir+"/"+QString::fromLatin1(hash.toHex())+"-"+QString::number(size);
    {
        FileReceiver receiver(dir.filePath("restored.bin"),hash,size);QVector<bool> verified(2,false);
        PartCache cache(cacheDir,hash,size);rejects([&]{PartCache conflict(cacheDir,hash,size);});
        check(cache.restore(receiver,verified)==hashes&&verified[0]&&!verified[1]&&receiver.received()==partSize,"Checkpoint did not survive process exit");
    }
    {
        QFile corrupt(prefix+"-0.part");check(corrupt.open(QIODevice::ReadWrite)&&corrupt.write("!")==1,"Corruption fixture failed");corrupt.close();
        FileReceiver receiver(dir.filePath("repair.bin"),hash,size);QVector<bool> verified(2,false);PartCache cache(cacheDir,hash,size);
        check(cache.restore(receiver,verified)==hashes&&!verified[0]&&!verified[1]&&receiver.received()==0,"Corrupt cache trusted");
        fill(receiver,hash,0,part);cache.storePart(receiver,0);fill(receiver,hash,partSize,tail);cache.storePart(receiver,1);
    }
    {
        ServerDownload request;request.server=QHostAddress::LocalHost;request.serverPort=1;request.userHash=QByteArray(16,'u');request.fileHash=hash;
        request.nickname="CacheTest";request.size=size;request.destination=dir.filePath("offline.bin");request.checkpointDirectory=cacheDir;
        const auto result=downloadFromServer(request);check(result.attemptedSources==0&&result.bytes==size,"Complete cache attempted network");
        QFile file(request.destination);check(file.open(QIODevice::ReadOnly)&&file.readAll()==part+tail,"Offline checkpoint output differs");
        check(!QFile::exists(prefix+".hashset")&&!QFile::exists(prefix+"-0.part")&&!QFile::exists(prefix+"-1.part"),"Published cache not cleaned");
    }
    {
        QFile metadata(prefix+".hashset");check(metadata.open(QIODevice::WriteOnly)&&metadata.write("broken")==6,"Metadata fixture failed");metadata.close();
        FileReceiver receiver(dir.filePath("bad-metadata.bin"),hash,size);QVector<bool> verified(2,true);PartCache cache(cacheDir,hash,size);
        check(cache.restore(receiver,verified).isEmpty()&&!verified[0]&&!verified[1]&&receiver.received()==0,"Invalid metadata trusted");
    }
    std::cout<<"ed2k complete-part checkpoint tests passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
