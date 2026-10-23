#include "ed2k_wire.h"
#include <iostream>
#include <stdexcept>
using namespace downloader::ed2k;
static void check(bool value,const char* text){if(!value)throw std::runtime_error(text);}
int main(){try{
    const auto bytes=QByteArray::fromHex("e30400000001616263");
    check(encode({Protocol::Edonkey,1,"abc"})==bytes,"Known wire bytes differ");
    for(qsizetype size=0;size<bytes.size();++size)
        check(std::holds_alternative<IncompletePacket>(decode(QByteArrayView(bytes).first(size))),"Fragment treated as complete");
    const auto decoded=std::get<DecodedPacket>(decode(bytes+bytes));
    check(decoded.consumed==bytes.size()&&decoded.packet.payload=="abc"&&decoded.packet.opcode==1,"Coalesced frame boundary lost");
    check(std::holds_alternative<InvalidPacket>(decode(QByteArray::fromHex("e300000000"))),"Zero wire length accepted");
    check(std::holds_alternative<InvalidPacket>(decode(QByteArray::fromHex("e3ffffffff"))),"Oversized header accepted");
    check(std::holds_alternative<InvalidPacket>(decode(QByteArray::fromHex("e4"))),"UDP marker accepted as TCP");
    check(std::holds_alternative<InvalidPacket>(decode(bytes,2)),"Configured payload limit ignored");
    for(auto protocol:{Protocol::Edonkey,Protocol::Emule,Protocol::Packed}){
        const auto packet=std::get<DecodedPacket>(decode(encode({protocol,0xff,{}}))).packet;
        check(packet.protocol==protocol&&packet.opcode==0xff&&packet.payload.isEmpty(),"Empty packet roundtrip failed");
    }
    bool rejected=false;try{encode({Protocol::Edonkey,1,"abc"},2);}catch(const std::invalid_argument&){rejected=true;}
    check(rejected,"Oversized encode accepted");
    std::cout<<"ed2k Qt-only TCP framing checks passed\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
