#include "ed2k_queue.h"
#include "ed2k_hash.h"
#include <QtEndian>
#include <iostream>
#include <stdexcept>
using namespace downloader::ed2k;
static void check(bool v,const char* m){if(!v)throw std::runtime_error(m);}
template<class F>void rejects(F f){bool rejected=false;try{f();}catch(const std::exception&){rejected=true;}check(rejected,"Expected rejection");}
int main(){try{
    const QByteArray hash(16,'h');const Packet name{Protocol::Edonkey,0x59,hash+QByteArray::fromHex("010041")};
    DownloadQueue small(hash,100);check(small.begin().opcode==0x58,"Filename request opcode");
    check(small.consume(name)->opcode==0x54&&small.hasPart(0)&&!small.hasPart(1),"Single-part path");
    small.consume({Protocol::Edonkey,0x5c,QByteArray::fromHex("78563412")});check(small.rank()==0x12345678,"32-bit rank");
    small.consume({Protocol::Emule,0x60,QByteArray::fromHex("341200000000000000000000")});check(small.rank()==0x1234,"Extended rank");
    small.consume({Protocol::Edonkey,0x55,{}});check(small.state()==QueueState::Ready&&small.rank()==0,"Slot grant");
    rejects([&]{small.begin();});
    DownloadQueue multi(hash,partSize*2);multi.begin();check(multi.consume(name)->opcode==0x4f,"Multipart status request");
    check(multi.consume({Protocol::Edonkey,0x50,hash+QByteArray::fromHex("030002")})->opcode==0x54,"Multipart queue start");
    check(!multi.hasPart(0)&&multi.hasPart(1)&&!multi.hasPart(2),"Bitmap mapping or empty part exposed");
    DownloadQueue complete(hash,partSize+1);complete.begin();complete.consume(name);
    complete.consume({Protocol::Edonkey,0x50,hash+QByteArray::fromHex("0000")});check(complete.hasPart(0)&&complete.hasPart(1),"Complete status sentinel");
    DownloadQueue empty(hash,partSize+1);empty.begin();empty.consume(name);
    empty.consume({Protocol::Edonkey,0x50,hash+QByteArray::fromHex("020000")});check(empty.state()==QueueState::Unavailable,"Empty bitmap queued");
    for(int scenario=0;scenario<4;++scenario){DownloadQueue bad(hash,partSize+1);bad.begin();
        if(scenario==0)rejects([&]{bad.consume({Protocol::Edonkey,0x55,{}});});
        if(scenario==1)rejects([&]{bad.consume({Protocol::Edonkey,0x59,QByteArray(16,'x')+QByteArray::fromHex("010041")});});
        if(scenario==2){bad.consume(name);rejects([&]{bad.consume({Protocol::Edonkey,0x50,hash+QByteArray::fromHex("030001")});});}
        if(scenario==3)rejects([&]{bad.consume({Protocol::Edonkey,0x59,hash+QByteArray::fromHex("020041")});});
        check(bad.state()==QueueState::Failed,"Malformed negotiation not terminal");
    }
    DownloadQueue missing(hash,100);missing.begin();missing.consume({Protocol::Edonkey,0x48,hash});check(missing.state()==QueueState::Unavailable,"No-file response ignored");
    std::cout<<"ed2k queue tests passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
