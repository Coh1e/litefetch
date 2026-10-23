#include "socket_connect_wait.h"
#include <QCoreApplication>
#include <QTcpSocket>
#include <QTimer>
#include <QElapsedTimer>
#include <iostream>
class ControlledSocket:public QTcpSocket {
public:
    void pending(){setSocketState(QAbstractSocket::ConnectingState);}
    void complete(){setSocketState(QAbstractSocket::ConnectedState);emit connected();}
    void refuse(){setSocketError(QAbstractSocket::ConnectionRefusedError);setSocketState(QAbstractSocket::UnconnectedState);emit errorOccurred(QAbstractSocket::ConnectionRefusedError);}
};
static void check(bool v,const char* m){if(!v)throw std::runtime_error(m);}
int main(int argc,char** argv){QCoreApplication app(argc,argv);try{
    for(int scenario=0;scenario<6;++scenario){
        ControlledSocket socket;socket.pending();bool cancelled=scenario==5;
        QDeadlineTimer deadline(scenario==2?120:30000);QElapsedTimer timer;timer.start();
        QTimer trigger;trigger.setSingleShot(true);
        QObject::connect(&trigger,&QTimer::timeout,&socket,[&]{
            if(scenario==0)socket.complete();
            if(scenario==1)cancelled=true;
            if(scenario==3)socket.refuse();
            if(scenario==4){cancelled=true;socket.complete();}
        });
        if(scenario!=2&&scenario!=5)trigger.start(scenario==0?200:50);
        bool failed=false;try{downloader::network::waitForConnected(socket,deadline,[&]{return cancelled;});}catch(const std::runtime_error&){failed=true;}
        const auto elapsed=timer.elapsed();
        if(scenario==0)check(!failed&&socket.state()==QAbstractSocket::ConnectedState&&elapsed>=150&&elapsed<1500,"Delayed valid connection failed");
        else check(failed&&socket.state()==QAbstractSocket::UnconnectedState&&elapsed<1500,"Pending connection did not abort on stop/error/deadline");
        if(scenario==2)check(elapsed>=80,"Connection deadline failed prematurely");
        socket.abort();
    }
    std::cout<<"TCP pending connection cancellation, deadline and completion race tests passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
