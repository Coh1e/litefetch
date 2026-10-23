#include "ed2k_storage.h"
#include "ed2k_hash.h"
#include "ed2k_part_cache.h"
#include <QTemporaryDir>
#include <QDir>
#include <QFile>
#include <iostream>
#include <stdexcept>
using namespace downloader::ed2k;
static void check(bool v,const char* m){if(!v)throw std::runtime_error(m);}
template<class F>void rejects(F f){bool stopped=false;try{f();}catch(const std::runtime_error&){stopped=true;}check(stopped,"Expected interrupted file operation");}
class InterruptingOutput:public QIODevice {
public:
    explicit InterruptingOutput(bool& stop):stop_(stop){open(QIODevice::WriteOnly);}
    qint64 written=0;
protected:
    qint64 writeData(const char*,qint64 size)override{written+=size;stop_=true;return size;}
    qint64 readData(char*,qint64)override{return -1;}
private:bool& stop_;
};
static QByteArray readFile(const QString& path){QFile file(path);check(file.open(QIODevice::ReadOnly),"Fixture read failed");return file.readAll();}
int main(){try{
    QTemporaryDir dir(QDir::currentPath()+"/ed2k-disk-cancel-XXXXXX");check(dir.isValid(),"Test directory failed");
    const QByteArray data(1024*1024,'x');const auto hash=QCryptographicHash::hash(data,QCryptographicHash::Md4);const auto size=quint64(data.size());
    bool stop=false,interruptScan=false;int checks=0;
    const auto target=dir.filePath("final.bin");FileReceiver receiver(target,hash,size,[&]{return stop||(interruptScan&&++checks>=4);});
    for(quint64 offset=0;offset<size;){const auto end=qMin(size,offset+blockSize);receiver.accept({hash,{offset,end},data.mid(qsizetype(offset),qsizetype(end-offset))});offset=end;}
    interruptScan=true;checks=0;rejects([&]{receiver.verifyPart(0,hash);});interruptScan=false;
    check(receiver.received()==size&&receiver.verifyPart(0,hash),"Cancelled hash damaged coverage");
    InterruptingOutput output(stop);rejects([&]{receiver.copyPart(0,output);});stop=false;
    check(output.written>0&&output.written<data.size()&&receiver.verifyPart(0,hash),"Copy ignored stop or damaged source");
    const auto folder=dir.filePath("cache");bool stopCommit=false;int commitChecks=0;
    PartCache cache(folder,hash,size,[&]{return stopCommit&&++commitChecks>=2;});cache.storeHashes({hash});cache.storePart(receiver,0);
    const auto part=QDir(folder).filePath(QString::fromLatin1(hash.toHex())+"-"+QString::number(size)+"-0.part");
    stopCommit=true;commitChecks=0;rejects([&]{cache.storePart(receiver,0);});stopCommit=false;
    check(readFile(part)==data,"Interrupted cache commit lost previous complete part");
    {
        FileReceiver* restoring=nullptr;bool active=false;
        FileReceiver partial(dir.filePath("partial.bin"),hash,size,[&]{return active&&restoring&&restoring->received()>0;});restoring=&partial;
        QVector<bool> verified(1,false);active=true;rejects([&]{cache.restore(partial,verified);});active=false;
        check(partial.received()>0&&partial.received()<size&&!verified[0]&&readFile(part)==data,"Cancelled restore trusted partial data or destroyed cache");
    }
    {
        FileReceiver restored(dir.filePath("restored.bin"),hash,size);QVector<bool> verified(1,false);
        check(cache.restore(restored,verified).size()==1&&verified[0]&&restored.received()==size,"Cache unusable after cancelled restore");
    }
    interruptScan=true;checks=0;rejects([&]{receiver.publish();});interruptScan=false;
    check(!QFile::exists(target)&&receiver.received()==size,"Cancelled final verification published output");
    cache.preparePublication(target);receiver.publish();check(readFile(target)==data,"Publish failed after cancelled verification");
    stopCommit=true;commitChecks=0;rejects([&]{cache.recoverPublication(target);});stopCommit=false;
    check(cache.recoverPublication(target)&&readFile(target)==data,"Cancelled published-file verification damaged recovery");
    cache.clear();std::cout<<"ed2k disk cancellation preserves verified data and atomic cache\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
