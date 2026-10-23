#pragma once
#include "ed2k_storage.h"
#include <QLockFile>
namespace downloader::ed2k {
// Per-task cache directory supplied by the task layer. Atomic complete-part
// files, never trusted without rechecking identity and bytes on restoration.
class PartCache {
public:
    PartCache(QString directory,QByteArray hash,quint64 size,std::function<bool()> cancelled={});
    QVector<QByteArray> restore(FileReceiver& receiver,QVector<bool>& verified);
    void storeHashes(const QVector<QByteArray>& hashes);
    void storePart(FileReceiver& receiver,quint64 index);
    void preparePublication(const QString& destination);
    bool recoverPublication(const QString& destination);
    void clear(bool keepPublicationReceipt=false);
private:
    QString prefix_;
    QByteArray hash_;
    quint64 size_;
    std::unique_ptr<QLockFile> lock_;
    QVector<QByteArray> hashes_;
    std::function<bool()> cancelled_;
    void checkStop()const;
    QString partPath(quint64 index)const;
};
}
