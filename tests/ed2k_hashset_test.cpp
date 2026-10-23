#include "ed2k_hashset.h"
#include "ed2k_hash.h"
#include "ed2k_storage.h"
#include <QTemporaryDir>
#include <QDir>
#include <QFile>
#include <QtEndian>
#include <iostream>
#include <stdexcept>
using namespace downloader::ed2k;
static void check(bool v,const char* m){if(!v)throw std::runtime_error(m);}
template<class F>void rejects(F f){bool rejected=false;try{f();}catch(const std::exception&){rejected=true;}check(rejected,"Expected rejection");}
static QByteArray md4(QByteArrayView bytes){return QCryptographicHash::hash(bytes,QCryptographicHash::Md4);}
static Packet answer(const QByteArray& hash,const QVector<QByteArray>& hashes){QByteArray payload=hash;char count[2];qToLittleEndian<quint16>(quint16(hashes.size()),count);payload.append(count,2);for(const auto& part:hashes)payload+=part;return {Protocol::Edonkey,0x52,payload};}
int main(){try{
    const QByteArray part(qsizetype(partSize),'x'),tail(17,'t');
    const auto partHash=md4(part),tailHash=md4(tail),emptyHash=md4({});
    const auto hash=md4(partHash+partHash+tailHash);auto packet=answer(hash,{partHash,partHash,tailHash});
    check(std::get<QVector<QByteArray>>(parseHashset(packet,hash,2*partSize+17)).size()==3,"Valid hashset rejected");
    for(qsizetype n=0;n<packet.payload.size();++n){auto truncated=packet;truncated.payload.truncate(n);check(std::holds_alternative<InvalidPacket>(parseHashset(truncated,hash,2*partSize+17)),"Truncated hashset accepted");}
    auto changed=packet;changed.payload[changed.payload.size()-1]^=1;check(std::holds_alternative<InvalidPacket>(parseHashset(changed,hash,2*partSize+17)),"Untrusted part hashes accepted");
    check(std::holds_alternative<InvalidPacket>(parseHashset(packet,hash,partSize+17)),"Wrong count accepted");
    const auto exact=md4(partHash+emptyHash);check(std::get<QVector<QByteArray>>(parseHashset(answer(exact,{partHash,emptyHash}),exact,partSize)).size()==2,"Exact-boundary hashset rejected");
    const auto invalidEmpty=md4(partHash+partHash);check(std::holds_alternative<InvalidPacket>(parseHashset(answer(invalidEmpty,{partHash,partHash}),invalidEmpty,partSize)),"Invalid empty part accepted");
    check(std::get<QVector<QByteArray>>(parseHashset(answer(tailHash,{}),tailHash,17)).first()==tailHash,"Small identity not reused");
    QTemporaryDir dir(QDir::currentPath()+"/ed2k-hashset-XXXXXX");check(dir.isValid(),"Test directory failed");
    const auto target=dir.filePath("verified.bin");FileReceiver receiver(target,hash,2*partSize+17);
    const auto fill=[&](quint64 begin,QByteArrayView bytes){quint64 at=0;while(at<quint64(bytes.size())){
        const auto count=qMin<quint64>(blockSize,quint64(bytes.size())-at);
        receiver.accept({hash,{begin+at,begin+at+count},bytes.sliced(qsizetype(at),qsizetype(count)).toByteArray()});at+=count;}};
    rejects([&]{receiver.verifyPart(0,partHash);});fill(0,part);fill(partSize,QByteArray(qsizetype(partSize),'z'));fill(2*partSize,tail);
    check(receiver.verifyPart(0,partHash)&&receiver.verifyPart(2,tailHash),"Good neighboring parts rejected");
    check(!receiver.verifyPart(1,partHash)&&receiver.received()==partSize+17,"Bad part coverage retained");
    check(receiver.verifyPart(0,partHash)&&receiver.verifyPart(2,tailHash),"Discard damaged neighboring ranges");
    rejects([&]{receiver.verifyPart(1,partHash);});fill(partSize,part);
    check(receiver.verifyPart(1,partHash)&&receiver.received()==2*partSize+17,"Replacement part not accepted");
    receiver.publish();QFile result(target);check(result.open(QIODevice::ReadOnly)&&result.readAll()==part+part+tail,"Repaired file bytes differ");
    std::cout<<"ed2k authenticated hashset and bad-part repair tests passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
