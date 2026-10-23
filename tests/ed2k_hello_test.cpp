#include "ed2k_hello.h"
#include <QtEndian>
#include <iostream>
#include <stdexcept>
using namespace downloader::ed2k;
static void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
int main(){try{
    const auto hash=QByteArray::fromHex("000102030405060708090a0b0c0d0e0f");
    const auto expected=QByteArray::fromHex("10000102030405060708090a0b0c0d0e0f0403020136120200000002010001010041030100113c000000000000000000");
    auto packet=makeHello(hash,0x01020304,4662,"A");
    check(packet.payload==expected,"Canonical hello differs from known wire vector");
    const auto hello=std::get<Hello>(parseHello(packet));
    check(hello.userHash==hash&&hello.clientId==0x01020304&&hello.tcpPort==4662,"Hello identity mismatch");
    check(hello.tags.size()==2&&std::get<QByteArray>(hello.tags[0].value)=="A"&&std::get<quint64>(hello.tags[1].value)==0x3c,"Hello tags mismatch");
    for(qsizetype length=0;length<expected.size();++length){
        auto truncated=packet;truncated.payload.truncate(length);
        check(std::holds_alternative<InvalidPacket>(parseHello(truncated)),"Truncated hello accepted");
    }
    packet.payload[0]=15;check(std::holds_alternative<InvalidPacket>(parseHello(packet)),"Invalid hash length accepted");
    packet=makeHello(hash,0x01020304,4662,"A",true);
    check(packet.opcode==0x4c&&packet.payload==expected.mid(1),"Hello answer has request-only prefix");
    packet.payload=QByteArray::fromHex("000102030405060708090a0b0c0d0e0f04030201361202000000820101004183113c000000000000000000");
    check(std::get<Hello>(parseHello(packet)).tags.size()==2,"Compact numeric tag names unsupported");
    packet=makeHello(hash,0,4662,"A");
    qToLittleEndian<quint32>(129,packet.payload.data()+23);
    check(std::holds_alternative<InvalidPacket>(parseHello(packet)),"Excessive tag count accepted");
    packet=makeHello(hash,0,4662,"A");
    const auto originalTags=packet.payload.mid(27,packet.payload.size()-27-6);
    packet.payload.insert(packet.payload.size()-6,originalTags);
    qToLittleEndian<quint32>(4,packet.payload.data()+23);
    check(std::holds_alternative<InvalidPacket>(parseHello(packet)),"Ambiguous duplicate identity tags accepted");
    packet=makeHello(hash,0,4662,"A");packet.payload[27]=char(0x7e);
    check(std::holds_alternative<InvalidPacket>(parseHello(packet)),"Unknown tag type accepted");
    packet=makeHello(hash,0,4662,"A");packet.payload.append(QByteArray::fromHex("4d4c444b"));
    check(std::get<Hello>(parseHello(packet)).trailer.size()==4,"Legacy trailer discarded");
    packet.payload.append('x');check(std::holds_alternative<InvalidPacket>(parseHello(packet)),"Unexpected trailer length accepted");
    bool rejected=false;try{makeHello(hash,0,4662,QString(1025,'x'));}catch(const std::invalid_argument&){rejected=true;}
    check(rejected,"Oversized local nickname accepted");
    std::cout<<"ed2k hello: known bytes, request/answer, compact tags, truncation, duplicate and limit checks passed\n";
    return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
