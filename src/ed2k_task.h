#pragma once
#include "transfer.h"
#include <QHostAddress>
#include <QVector>
#include <QPair>
namespace downloader {
struct Ed2kRuntimeOptions {
    QHostAddress server;
    quint16 serverPort=4661,incomingPort=4662;
    QByteArray userHash;
    QString nickname=QStringLiteral("NativeDownloader");
    int operationTimeoutMs=5000;
    bool useKad=false;
    QVector<QPair<QHostAddress,quint16>> kadSeeds;
    int kadDiscoveryTimeoutMs=5000;
};
bool validEd2kOptions(const Ed2kRuntimeOptions& options);
QString ed2kTaskCachePath(const QString& root,const QString& taskId);
void discardEd2kCache(const QString& root,const QString& taskId,const DownloadRequest& request);
TransferResult runEd2kTask(const DownloadRequest& request,const QString& destination,
    const QString& taskId,const QString& cacheRoot,const Ed2kRuntimeOptions& options,
    const std::atomic_bool& cancelled,const std::function<void(TransferProgress)>& progress,BandwidthLimiter* limiter=nullptr);
}
