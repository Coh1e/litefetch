#include "kad_datagram.h"
#include <zlib.h>
#include <stdexcept>
namespace downloader::kad {
QByteArray unpackDatagram(QByteArrayView bytes,qsizetype limit){
    if(limit<2||limit>60000||bytes.size()<2||bytes.size()>limit)
        throw std::runtime_error("Kad datagram exceeds framing limits");
    if(quint8(bytes[0])==0xe4)return bytes.toByteArray();
    if(quint8(bytes[0])!=0xe5||bytes.size()==2)
        throw std::runtime_error("Invalid Kad datagram protocol");
    QByteArray output(limit,Qt::Uninitialized);output[0]=char(0xe4);output[1]=bytes[1];
    z_stream stream{};
    stream.next_in=reinterpret_cast<Bytef*>(const_cast<char*>(bytes.data()+2));
    stream.avail_in=static_cast<uInt>(bytes.size()-2);
    stream.next_out=reinterpret_cast<Bytef*>(output.data()+2);
    stream.avail_out=static_cast<uInt>(limit-2);
    if(inflateInit(&stream)!=Z_OK)throw std::runtime_error("Cannot initialize Kad decompression");
    const auto status=inflate(&stream,Z_FINISH);
    const auto length=stream.total_out;const auto remaining=stream.avail_in;
    inflateEnd(&stream);
    if(status!=Z_STREAM_END||remaining!=0)throw std::runtime_error("Invalid or oversized compressed Kad datagram");
    output.resize(static_cast<qsizetype>(length)+2);return output;
}
}
