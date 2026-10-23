#include "ed2k_hash.h"
#include "ed2k_blocks.h"
#include <QtEndian>
#include <iostream>
#include <stdexcept>
using namespace downloader::ed2k;
static void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
static QByteArray md4(QByteArrayView bytes){return QCryptographicHash::hash(bytes,QCryptographicHash::Md4);}
static QByteArray digest(QByteArrayView bytes){FileHasher hash;for(qsizetype offset=0;offset<bytes.size();offset+=65537)hash.add(bytes.sliced(offset,qMin<qsizetype>(65537,bytes.size()-offset)));check(hash.size()==quint64(bytes.size()),"Hash size tracking failed");return hash.result();}
int main(){try{
    check(digest({}).toHex()=="31d6cfe0d16ae931b73c59d7e0c089c0","RFC empty MD4 mismatch");
    check(digest("a").toHex()=="bde52cb31de33e46245e05fbdbd6fb24","RFC a MD4 mismatch");
    check(digest("abc").toHex()=="a448017aaf21d8525fc10ae87aa6729d","RFC abc MD4 mismatch");
    const QByteArray part(static_cast<qsizetype>(partSize),'x');const auto partHash=md4(part),emptyHash=md4({});
    check(digest(QByteArrayView(part).first(part.size()-1))==md4(QByteArrayView(part).first(part.size()-1)),"Subpart digest mismatch");
    check(digest(part)==md4(partHash+emptyHash),"Exact part omitted legacy empty hash");
    check(digest(part+"y")==md4(partHash+md4("y")),"Part plus tail digest mismatch");
    check(digest(part+part)==md4(partHash+partHash+emptyHash),"Exact double part digest mismatch");
    FileHasher frozen;frozen.add("abc");check(frozen.result()==frozen.result(),"Repeated result changed");
    bool rejected=false;try{frozen.add("x");}catch(const std::logic_error&){rejected=true;}check(rejected,"Finalized hasher accepted data");
    const auto hash=QByteArray::fromHex("000102030405060708090a0b0c0d0e0f");
    const auto request=requestBlocks(hash,{{1,3},{7,9}},16,false);
    check(request.protocol==Protocol::Edonkey&&request.opcode==0x47,"Wrong 32-bit request opcode");
    check(request.payload==hash+QByteArray::fromHex("010000000700000000000000030000000900000000000000"),"Request offsets interleaved or padding wrong");
    Packet block{Protocol::Edonkey,0x46,hash+QByteArray::fromHex("0100000003000000")+"ab"};
    const auto decoded=std::get<DataBlock>(parseDataBlock(block,hash,{{0,4}},16));
    check(decoded.range.begin==1&&decoded.range.end==3&&decoded.bytes=="ab","Data block fields mismatch");
    check(std::holds_alternative<InvalidPacket>(parseDataBlock(block,hash,{{3,5}},16)),"Unrequested data accepted");
    check(std::holds_alternative<InvalidPacket>(parseDataBlock(block,QByteArray(16,'z'),{{0,4}},16)),"Wrong file identity accepted");
    block.payload.chop(1);check(std::holds_alternative<InvalidPacket>(parseDataBlock(block,hash,{{0,4}},16)),"Truncated data accepted");
    const quint64 begin=(quint64(1)<<32)+100;
    rejected=false;try{requestBlocks(hash,{{begin,begin+3}},begin+3,false);}catch(const std::invalid_argument&){rejected=true;}
    check(rejected,"Unnegotiated large offsets accepted");
    const auto large=requestBlocks(hash,{{begin,begin+3}},begin+3,true);
    check(large.opcode==0xa3&&large.protocol==Protocol::Emule&&large.payload.size()==64,"Wrong large request format");
    check(qFromLittleEndian<quint64>(large.payload.constData()+16)==begin&&qFromLittleEndian<quint64>(large.payload.constData()+40)==begin+3,"Large offset truncated");
    QByteArray largeBody=hash;largeBody.resize(32);
    qToLittleEndian<quint64>(begin,largeBody.data()+16);qToLittleEndian<quint64>(begin+3,largeBody.data()+24);largeBody+="xyz";
    const auto largeData=std::get<DataBlock>(parseDataBlock({Protocol::Emule,0xa2,largeBody},hash,{{begin,begin+3}},begin+3));
    check(largeData.range.begin==begin&&largeData.bytes=="xyz","Large received offset truncated");
    qToLittleEndian<quint64>(begin-1,largeBody.data()+24);
    check(std::holds_alternative<InvalidPacket>(parseDataBlock({Protocol::Emule,0xa2,largeBody},hash,{{begin,begin+3}},begin+3)),"Reversed large range accepted");
    for(const QVector<BlockRange>& ranges:{QVector<BlockRange>{{0,3},{2,4}},QVector<BlockRange>{{partSize-1,partSize+1}},QVector<BlockRange>{{0,blockSize+1}},QVector<BlockRange>{{0,0}}}){
        rejected=false;try{requestBlocks(hash,ranges,partSize*2,true);}catch(const std::invalid_argument&){rejected=true;}check(rejected,"Invalid request range accepted");
    }
    std::cout<<"ed2k data: RFC MD4, part boundaries, streaming, request bytes, 64-bit offsets and received-range validation passed\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
