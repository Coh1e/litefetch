#pragma once
#include "download_request.h"
#include <QJsonObject>
#include <stdexcept>

namespace downloader {
struct Ed2kSnapshot {
    QString hash, name, status, priority;
    qint64 size = 0, completed = 0, speed = 0;
};
class AmuleApiError : public std::runtime_error {
public:
    explicit AmuleApiError(const char* message, long status = 0)
        : std::runtime_error(message), status(status) {}
    long status;
};
// Blocking, per-worker client for the application's loopback-only aMule API.
// No implicit retry of mutations; callers reconcile task identity after failure.
class AmuleApi {
public:
    AmuleApi(quint16 port, const QByteArray& password);
    bool connected();
    void add(const DownloadRequest& request);
    Ed2kSnapshot task(const QString& hash);
    void pause(const QString& hash);
    void resume(const QString& hash);
    void priority(const QString& hash, const QString& value);
    static Ed2kSnapshot parseSnapshot(const QJsonObject& object);
private:
    quint16 port_;
    QByteArray token_;
    QJsonObject call(const QByteArray& method, const QByteArray& path,
        const QJsonObject* body = nullptr, bool authenticated = true);
    void action(const QString& hash, const QString& action);
    static QByteArray taskPath(const QString& hash);
};
}
