#pragma once
#include "ed2k_blocks.h"
#include <QTemporaryFile>
#include <map>
#include <memory>
#include <functional>
namespace downloader::ed2k {
// Worker-owned, disposable staging file. Resume persistence belongs to the task layer.
class FileReceiver {
public:
    FileReceiver(QString destination,QByteArray hash,quint64 size,std::function<bool()> cancelled={});
    quint64 accept(const DataBlock& block); // Newly covered bytes only.
    // Caller supplies a part digest authenticated by the file hash/hashset.
    // Incomplete parts throw; a digest mismatch discards coverage for that part.
    bool verifyPart(quint64 index,const QByteArray& digest);
    void discardPart(quint64 index);
    void copyPart(quint64 index,QIODevice& output);
    void publish(); // Full identity check, then rename without overwriting.
    quint64 received()const{return received_;}
private:
    QString destination_;
    QByteArray hash_;
    quint64 size_,received_=0;
    std::unique_ptr<QTemporaryFile> file_;
    std::map<quint64,quint64> ranges_;
    bool failed_=false,published_=false;
    std::function<bool()> cancelled_;
    void checkStop()const;
};
}
