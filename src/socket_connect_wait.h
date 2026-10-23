#pragma once
#include <QAbstractSocket>
#include <QDeadlineTimer>
#include <functional>
namespace downloader::network {
void connectToHost(QAbstractSocket& socket,const QHostAddress& address,quint16 port,
    QDeadlineTimer& deadline,const std::function<bool()>& cancelled={});
// Runs only the owning worker thread's Qt event loop. The connection has
// already been started; stop/error/deadline abort it before returning failure.
void waitForConnected(QAbstractSocket& socket,QDeadlineTimer& deadline,
    const std::function<bool()>& cancelled={});
}
