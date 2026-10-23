#pragma once
#include "ed2k_server.h"
#include <stop_token>
#include <functional>
namespace downloader::ed2k {
struct PeerEndpoint {QHostAddress address;quint16 port=0;QByteArray expectedUserHash;};
struct ServerDownload {
    QHostAddress server;
    quint16 serverPort=4661;
    QByteArray userHash,fileHash;
    quint16 incomingPort=4662;
    QString nickname,destination;
    QString checkpointDirectory; // Optional per-task directory; empty disables persistence.
    bool deferPublicationAcknowledgement=false; // Task DB must commit before receipt removal.
    quint64 size=0;
    int operationTimeoutMs=5000;
    int maxSources=32;
    // Alternative to server login, called only after offline recovery checks.
    std::function<QVector<PeerEndpoint>()> sourceProvider;
    // Called on the worker thread. Progress/cancel callbacks must not throw.
    std::function<bool()> cancelled;
    std::function<void(quint64)> progress;
    // Worker callback: reserve 1..maximum payload bytes before sending requests.
    // Zero means stopped. Offline verified data does not consume network budget.
    std::function<quint64(quint64)> downloadBudget;
};
struct DownloadResult {quint64 bytes;int attemptedSources;};
// Worker-thread operation. Sequential direct sources share only hash-verified
// complete parts; optional checkpoints restore independently verified parts.
DownloadResult downloadFromServer(const ServerDownload& request,std::stop_token stop={});
DownloadResult download(const ServerDownload& request,std::stop_token stop={});
}
