#include "download_batch.h"
#include "task_manager.h"
#include <QCoreApplication>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QElapsedTimer>
#include <QThread>
#include <QFile>
#include <QDir>
#include <future>
#include <iostream>
using namespace downloader;
static void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
int main(int argc,char** argv){QCoreApplication app(argc,argv);try{
    QTemporaryDir directory;check(directory.isValid(),"Temporary directory unavailable");
    std::atomic_int requests=0;std::promise<quint16> ready;auto port=ready.get_future();
    const QByteArray payload(128*1024,'x');
    std::jthread server([&](std::stop_token stop){QTcpServer listener;
        if(!listener.listen(QHostAddress::LocalHost,0)){ready.set_value(0);return;}
        ready.set_value(listener.serverPort());
        while(!stop.stop_requested()){
            if(!listener.hasPendingConnections()&&!listener.waitForNewConnection(50))continue;
            std::unique_ptr<QTcpSocket> socket(listener.nextPendingConnection());QByteArray input;QElapsedTimer deadline;deadline.start();
            while(!input.contains("\r\n\r\n")&&input.size()<16384&&deadline.elapsed()<2000){
                input+=socket->readAll();if(!input.contains("\r\n\r\n"))socket->waitForReadyRead(50);
            }
            ++requests;socket->write("HTTP/1.1 200 OK\r\nContent-Length: "+QByteArray::number(payload.size())+"\r\nConnection: close\r\n\r\n"+payload);
            while(socket->bytesToWrite()&&socket->waitForBytesWritten(1000)){}
            socket->disconnectFromHost();
        }
    });
    const auto listeningPort=port.get();check(listeningPort!=0,"Server listen failed");
    const QString base=QStringLiteral("http://127.0.0.1:%1/").arg(listeningPort);
    TaskManager manager(QDir(directory.path()).filePath("tasks.sqlite"),2);
    manager.setHttpDownloadLimit(128*1024);
    const auto invalid=prepareBatch({base+"one.bin",base+"one.bin","not a supported link"});
    check(std::holds_alternative<InvalidRequest>(invalid)&&std::get<InvalidRequest>(invalid).message.startsWith(QStringLiteral("第3项")),"Invalid batch row not identified");
    check(manager.tasks().isEmpty()&&requests==0,"Validation started a transfer");
    QStringList tooMany;for(int i=0;i<101;++i)tooMany.append(base+QString::number(i));
    check(std::holds_alternative<InvalidRequest>(prepareBatch(tooMany)),"Batch count limit ignored");
    check(std::holds_alternative<InvalidRequest>(prepareBatch({QString(65537,'x')})),"Batch character limit ignored");
    const auto prepared=prepareBatch({"  "+base+"one.bin  ",base+"one.bin#ignored",base+"two.bin",""});
    check(std::holds_alternative<QStringList>(prepared),"Valid batch rejected");
    const auto inputs=std::get<QStringList>(prepared);check(inputs.size()==2,"Normalized duplicate not removed");
    for(const auto& input:inputs){const auto request=std::get<DownloadRequest>(parseRequest(input));
        const auto added=manager.add(request,QDir(directory.path()).filePath(request.suggestedName));check(added.added,"Batch task not added");}
    QElapsedTimer timeout;timeout.start();bool completed=false;
    while(timeout.elapsed()<10000){QCoreApplication::processEvents(QEventLoop::AllEvents,10);completed=true;
        for(const auto& task:manager.tasks())completed&=task.state==TaskState::Completed;
        if(completed)break;QThread::msleep(1);
    }
    check(completed&&manager.tasks().size()==2&&requests==2,"Batch completion or request count differs");
    check(timeout.elapsed()>=1700&&timeout.elapsed()<7000,"Shared payload limit did not govern two HTTP tasks");
    for(const auto& task:manager.tasks()){QFile output(task.destination);check(output.open(QIODevice::ReadOnly)&&output.readAll()==payload,"Batch downloaded bytes differ");}
    const auto request=std::get<DownloadRequest>(parseRequest(base+"one.bin"));
    check(!manager.add(request,QDir(directory.path()).filePath("another.bin")).added,"Existing task duplicate accepted");
    std::cout<<"Batch validation, deduplication and two real HTTP downloads passed\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
