#include "socket_connect_wait.h"
#include <QEventLoop>
#include <QTimer>
#include <stdexcept>
namespace downloader::network {
void connectToHost(QAbstractSocket& socket,const QHostAddress& address,quint16 port,
    QDeadlineTimer& deadline,const std::function<bool()>& cancelled){
    // Fresh std::jthreads have no Qt dispatcher yet. It must exist before
    // connectToHost creates the native socket's connection notifiers.
    QEventLoop dispatcher;
    try{
        if(cancelled&&cancelled())throw std::runtime_error("TCP connection cancelled");
        if(deadline.hasExpired())throw std::runtime_error("TCP connection deadline expired");
        socket.connectToHost(address,port);waitForConnected(socket,deadline,cancelled);
    }catch(...){socket.abort();throw;}
}
void waitForConnected(QAbstractSocket& socket,QDeadlineTimer& deadline,const std::function<bool()>& cancelled){
    try{
        QEventLoop loop;QTimer poll;poll.setSingleShot(true);
        QObject::connect(&poll,&QTimer::timeout,&loop,&QEventLoop::quit);
        QObject::connect(&socket,&QAbstractSocket::stateChanged,&loop,&QEventLoop::quit);
        QObject::connect(&socket,&QAbstractSocket::errorOccurred,&loop,&QEventLoop::quit);
        for(;;){
            if(cancelled&&cancelled())throw std::runtime_error("TCP connection cancelled");
            const auto remaining=deadline.remainingTime();
            if(remaining<=0)throw std::runtime_error("TCP connection deadline expired");
            if(socket.state()==QAbstractSocket::ConnectedState)return;
            if(socket.state()!=QAbstractSocket::ConnectingState&&socket.state()!=QAbstractSocket::HostLookupState)
                throw std::runtime_error("TCP connection failed");
            poll.start(static_cast<int>(qMin<qint64>(100,remaining)));
            loop.exec(QEventLoop::ExcludeUserInputEvents);
        }
    }catch(...){socket.abort();throw;}
}
}
