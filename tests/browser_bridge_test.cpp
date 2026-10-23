#include "browser_bridge.h"
#include "native_protocol.h"
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTextStream>
#include <QUuid>
#include <chrono>
#include <thread>
using namespace downloader;
namespace {
bool waitUntil(const std::function<bool()>& condition)
{
    QElapsedTimer clock; clock.start();
    while (!condition() && clock.elapsed()<10000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents,5);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return condition();
}
}
int main(int argc,char** argv)
{
    QCoreApplication app(argc,argv);
    if (app.arguments().value(1)=="--claim-endpoint") {
        const auto root=app.arguments().value(3);
        TaskManager manager(QDir(root).filePath("claim.sqlite"));
        try { BrowserBridge bridge(manager,{true,app.arguments().value(2),root,
            {"chrome-extension://"+QString(32,'a')+"/"}}); }
        catch (...) { return 0; }
        return 2;
    }
    int failures=0;
    const auto check=[&](bool ok,const char* name) { if (!ok) { ++failures; QTextStream(stderr)<<"FAIL "<<name<<'\n'; } };
    QTemporaryDir directory(app.applicationDirPath()+"/browser-bridge-check-XXXXXX");
    if (!directory.isValid()) return 10;
    const auto host=directory.filePath("native-downloader-host.exe");
    if (!QFile::copy(app.arguments().value(1),host)) return 11;
    const auto endpoint="NativeDownloader-bridge-test-"+QUuid::createUuid().toString(QUuid::Id128);
    const auto origin="chrome-extension://"+QString(32,'a')+"/";
    const auto other="chrome-extension://"+QString(32,'b')+"/";
    QFile config(directory.filePath("native-host.json"));
    if (!config.open(QIODevice::WriteOnly)) return 12;
    // The second origin is permitted by the transport but denied by the application.
    config.write(QJsonDocument(QJsonObject{{"ipcName",endpoint},{"allowedOrigins",QJsonArray{origin,other}}}).toJson()); config.close();
    QTcpServer http;
    if (!http.listen(QHostAddress::LocalHost,0)) return 13;
    int requests=0;
    QByteArray payload(256*1024,Qt::Uninitialized);
    for (qsizetype index=0;index<payload.size();++index) payload[index]=static_cast<char>(index%127);
    QObject::connect(&http,&QTcpServer::newConnection,&app,[&] {
        while (auto* socket=http.nextPendingConnection()) {
            auto bytes=std::make_shared<QByteArray>(); auto sent=std::make_shared<bool>(false);
            QObject::connect(socket,&QTcpSocket::disconnected,socket,&QObject::deleteLater);
            QObject::connect(socket,&QTcpSocket::readyRead,socket,[&,socket,bytes,sent] {
                bytes->append(socket->readAll());
                if (*sent || !bytes->contains("\r\n\r\n")) return;
                *sent=true; ++requests;
                socket->write("HTTP/1.1 200 OK\r\nContent-Length: "+QByteArray::number(payload.size())
                    +"\r\nETag: \"bridge-fixture\"\r\nConnection: close\r\n\r\n"+payload);
                socket->disconnectFromHost();
            });
        }
    });
    const auto database=directory.filePath("tasks.sqlite");
    TaskManager manager(database);
    QFile applicationConfig(directory.filePath("browser-bridge.json"));
    if (!applicationConfig.open(QIODevice::WriteOnly)) return 15;
    applicationConfig.write(QJsonDocument(QJsonObject{{"enabled",true},{"ipcName",endpoint},
        {"downloadDirectory",directory.path()},{"allowedOrigins",QJsonArray{origin}}}).toJson()); applicationConfig.close();
    BrowserBridge service(manager,loadBrowserBridgeConfig(applicationConfig.fileName()));
    bool duplicateRejected=false;
    try { BrowserBridge duplicate(manager,{true,endpoint,directory.path(),{origin}}); }
    catch (...) { duplicateRejected=true; }
    check(duplicateRejected,"duplicate service cannot take over an active endpoint");
    QProcess competitor;
    competitor.start(app.applicationFilePath(),{"--claim-endpoint",endpoint.toUpper(),directory.path()});
    check(competitor.waitForStarted(2000) && competitor.waitForFinished(3000) && competitor.exitCode()==0,
        "separate process with case variant cannot claim active Windows endpoint");
    QProcess process; process.start(host,{origin});
    if (!process.waitForStarted(2000)) return 14;
    int sequence=0;
    const auto send=[&](QProcess& target,const QString& method,const QString& requestId,const QJsonObject& value=QJsonObject{}) {
        const auto id="message-"+QString::number(++sequence);
        QJsonObject message{{"version",1},{"id",id},{"method",method},{"requestId",requestId}};
        if (method=="prepare") message.insert("payload",value);
        target.write(native::frame(QJsonDocument(message).toJson(QJsonDocument::Compact)));
        QByteArray bytes;
        check(waitUntil([&] {
            bytes.append(target.readAllStandardOutput());
            if (bytes.size()<4) return false;
            auto copy=bytes;
            try { return native::takeFrame(copy).has_value(); } catch (...) { return true; }
        }),"host returns framed application response");
        try {
            const auto body=native::takeFrame(bytes);
            if (!body) return QJsonObject{};
            const auto result=native::parseObject(*body);
            check(result.value("id").toString()==id,"response stays correlated through actual host");
            return result;
        } catch (...) { check(false,"parse actual host reply"); return QJsonObject{}; }
    };
    const auto url=QString("http://127.0.0.1:%1/file.bin").arg(http.serverPort());
    const QJsonObject request{{"url",url},{"method","GET"},{"uniqueMatch",true},{"replaySafe",true}};
    const auto prepared=send(process,"prepare","transfer",request);
    check(prepared.value("ok").toBool() && prepared.value("result").toObject().value("state")=="prepared","application persists real prepare");
    check(manager.tasks().isEmpty() && requests==0,"prepare performs no HTTP transfer");
    const auto taskId=prepared.value("result").toObject().value("taskId").toString();
    service.updatePreferences(false,directory.filePath("removed-folder"));
    QJsonObject newRequest=request; newRequest.insert("url",QString("http://127.0.0.1:%1/disabled.bin").arg(http.serverPort()));
    check(!send(process,"prepare","disabled",newRequest).value("ok").toBool(),"disabled service rejects new preparations");
    const auto repeated=send(process,"prepare","transfer",request);
    check(repeated.value("result").toObject().value("taskId").toString()==taskId,"IPC duplicate prepare stable");
    const auto committed=send(process,"commit","transfer");
    check(committed.value("result").toObject().value("state")=="committed","IPC commit enters real scheduler");
    check(waitUntil([&] { return manager.tasks().size()==1 && manager.tasks()[0].state==TaskState::Completed; }),"real HTTP task completes");
    QFile output(directory.filePath("file.bin"));
    check(output.open(QIODevice::ReadOnly) && output.readAll()==payload,"final file is exact controlled HTTP payload"); output.close();
    service.updatePreferences(true,directory.path());
    send(process,"commit","transfer");
    check(requests==1 && manager.tasks().size()==1,"duplicate IPC commit does not redownload");
    send(process,"abort","transfer");
    check(send(process,"query","transfer").value("result").toObject().value("state")=="committed","abort cannot cancel completed handoff");
    const QJsonObject abortRequest{{"url",QString("http://127.0.0.1:%1/abort.bin").arg(http.serverPort())},
        {"method","GET"},{"uniqueMatch",true},{"replaySafe",true}};
    send(process,"prepare","abort",abortRequest); send(process,"abort","abort");
    check(send(process,"commit","abort").value("result").toObject().value("state")=="aborted","aborted IPC reservation cannot start");
    check(requests==1,"aborted preparation sends no HTTP request");
    QProcess denied; denied.start(host,{other});
    check(denied.waitForStarted(2000),"second caller transport starts");
    check(!send(denied,"query","transfer").value("ok").toBool(),"application whitelist rejects transport-approved other origin");
    denied.closeWriteChannel(); denied.waitForFinished(3000);
    process.closeWriteChannel(); process.waitForFinished(3000);
    check(process.exitCode()==0 && process.readAllStandardError().isEmpty(),"actual host clean shutdown without payload diagnostics");
    QTextStream(stdout)<<"Browser host to queue actual HTTP integration: "<<(failures?"FAILED":"PASS")<<'\n';
    return failures?1:0;
}
