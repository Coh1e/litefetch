#pragma once
#include "kad_bootstrap.h"
namespace downloader::kad {
struct FileSource {
    QByteArray userHash;
    quint32 ipv4=0;
    quint16 tcpPort=0,udpPort=0;
    quint8 type=0,cryptOptions=0;
    bool needsCallback()const{return type==3||type==5||type==6;}
    bool directlyUsable()const{return (type==1||type==4)&&ipv4&&tcpPort&&!(cryptOptions&4);}
};
struct SourcePage {QVector<FileSource> sources;quint16 entries=0;};
using SourcePageResult=std::variant<SourcePage,InvalidDatagram>;
QByteArray sourceRequest(const QByteArray& fileHash,quint64 size,quint16 start=0);
SourcePageResult parseSourcePage(QByteArrayView bytes,const QByteArray& fileHash,
    quint64 size,const QByteArray& responderId);
}
