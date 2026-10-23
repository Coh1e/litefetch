#pragma once
#include <QCryptographicHash>
#include <QByteArrayView>
namespace downloader::ed2k {
inline constexpr quint64 partSize=9728000;
// Streaming ed2k identity hash, including the legacy empty-part hash at exact
// part boundaries. Constant memory; result() freezes further input.
class FileHasher {
public:
    FileHasher();
    void add(QByteArrayView bytes);
    QByteArray result();
    quint64 size()const{return size_;}
private:
    QCryptographicHash part_{QCryptographicHash::Md4},root_{QCryptographicHash::Md4};
    quint64 size_=0,partBytes_=0,fullParts_=0;
    QByteArray result_;
    bool finished_=false;
};
}
