#include "ed2k_storage.h"
#include "ed2k_hash.h"
#include <QTemporaryDir>
#include <QDir>
#include <QFile>
#include <iostream>
#include <stdexcept>
using namespace downloader::ed2k;
static void check(bool v,const char* m){if(!v)throw std::runtime_error(m);}
template<class F> static void rejects(F f){bool rejected=false;try{f();}catch(const std::exception&){rejected=true;}check(rejected,"Expected rejection");}
int main(){try{
    QTemporaryDir dir(QDir::currentPath()+"/ed2k-storage-XXXXXX");check(dir.isValid(),"Temporary directory failed");
    const QByteArray bytes="abcdefghij";
    FileHasher hasher;hasher.add(bytes);const auto hash=hasher.result();
    const auto target=dir.filePath("result.bin");
    {
        FileReceiver receiver(target,hash,10);
        rejects([&]{receiver.publish();});
        check(receiver.accept({hash,{6,10},"ghij"})==4,"Tail coverage");
        check(receiver.accept({hash,{6,10},"ghij"})==0,"Duplicate counted");
        rejects([&]{receiver.accept({hash,{5,8},"fXX"});});
        check(receiver.received()==4,"Conflict changed coverage");
        check(receiver.accept({hash,{0,3},"abc"})==3,"Head coverage");
        check(receiver.accept({hash,{2,8},"cdefgh"})==3,"Bridge overlap count");
        check(receiver.received()==10&&!QFile::exists(target),"Early publication");
        receiver.publish();QFile result(target);check(result.open(QIODevice::ReadOnly)&&result.readAll()==bytes,"Published bytes");
        rejects([&]{receiver.accept({hash,{0,1},"a"});});
    }
    rejects([&]{FileReceiver existing(target,hash,10);});
    {
        const auto unicode=dir.filePath(QString::fromUtf8("下载完成-测试.bin"));
        FileHasher emptyHash;FileReceiver empty(unicode,emptyHash.result(),0);
        empty.publish();check(QFile::exists(unicode)&&QFileInfo(unicode).size()==0,"Empty Unicode publication failed");
        rejects([&]{empty.publish();});
    }
    {
        const auto corrupt=dir.filePath("corrupt.bin");FileReceiver receiver(corrupt,hash,10);
        receiver.accept({hash,{0,10},"0123456789"});rejects([&]{receiver.publish();});
        check(!QFile::exists(corrupt),"Corrupt file published");
    }
    {
        const auto race=dir.filePath("race.bin");FileReceiver receiver(race,hash,10);
        receiver.accept({hash,{0,10},bytes});QFile existing(race);
        check(existing.open(QIODevice::WriteOnly)&&existing.write("keep")==4,"Race fixture");existing.close();
        rejects([&]{receiver.publish();});check(existing.open(QIODevice::ReadOnly)&&existing.readAll()=="keep","Existing file overwritten");
    }
    {
        FileReceiver receiver(dir.filePath("invalid.bin"),hash,10);
        rejects([&]{receiver.accept({QByteArray(16,'x'),{0,1},"a"});});
        rejects([&]{receiver.accept({hash,{0,11},"abcdefghijk"});});
        rejects([&]{receiver.accept({hash,{0,3},"ab"});});
        check(receiver.received()==0,"Invalid block counted");
    }
    check(QDir(dir.path()).entryList({"*.part"},QDir::Files|QDir::Hidden).isEmpty(),"Staging files leaked");
    std::cout<<"ed2k storage tests passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
