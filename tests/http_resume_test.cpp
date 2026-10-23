#include "http_resume.h"
#include "task_manager.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSaveFile>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTextStream>
#include <future>
#include <mutex>
#include <thread>

using namespace downloader;
namespace {
QByteArray read(const QString& path)
{
    QFile file(path); if (!file.open(QIODevice::ReadOnly)) return {}; return file.readAll();
}
qint64 checkpointBytes(const QString& destination, const QString& id)
{
    return QJsonDocument::fromJson(read(partialPath(destination, id) + ".json")).object().value("bytes").toString().toLongLong();
}
bool waitUntil(const std::function<bool()>& predicate, int timeout = 10000)
{
    QElapsedTimer clock; clock.start();
    while (!predicate() && clock.elapsed() < timeout) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return predicate();
}
struct Observation { QByteArray path, range, condition; };
class Server {
public:
    QByteArray data{1024 * 1024, Qt::Uninitialized};
    QByteArray replacement, large;
    std::atomic_int version{1};
    quint16 port = 0;
    std::atomic_bool stop{false};
    std::mutex mutex;
    QVector<Observation> observations;
    std::thread thread;
    Server() {
        for (qsizetype i = 0; i < data.size(); ++i) data[i] = static_cast<char>(i * 17 % 251);
        replacement = data; for (auto& byte : replacement) byte ^= 0x5a;
        large = data.repeated(8);
        std::promise<quint16> ready; auto future = ready.get_future();
        thread = std::thread([this, &ready] {
            QTcpServer listener;
            if (!listener.listen(QHostAddress::LocalHost, 0)) { ready.set_value(0); return; }
            ready.set_value(listener.serverPort());
            while (!stop.load()) {
                if (!listener.hasPendingConnections() && !listener.waitForNewConnection(100)) continue;
                std::unique_ptr<QTcpSocket> socket(listener.nextPendingConnection());
                QByteArray request;
                while (!request.contains("\r\n\r\n") && request.size() < 32768) {
                    request += socket->readAll();
                    if (request.contains("\r\n\r\n") || !socket->waitForReadyRead(2000)) break;
                }
                Observation observed{request.split(' ').value(1), {}, {}};
                for (const auto& raw : request.split('\n')) {
                    const auto line = raw.trimmed();
                    if (line.toLower().startsWith("range:")) observed.range = line.mid(6).trimmed();
                    if (line.toLower().startsWith("if-range:")) observed.condition = line.mid(9).trimmed();
                }
                { std::lock_guard guard(mutex); observations.append(observed); }
                if (observed.path == "/no-space" && version.load() == 3) {
                    socket->write("HTTP/1.1 200 OK\r\nContent-Length: 9223372036854775807\r\nConnection: close\r\n\r\n");
                    if (socket->bytesToWrite()) socket->waitForBytesWritten(2000);
                    socket->disconnectFromHost(); continue;
                }
                if (observed.path == "/redirect-resource") {
                    socket->write("HTTP/1.1 302 Found\r\nLocation: " + QByteArray(version.load() == 1 ? "/resource-one" : "/resource-two")
                        + "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                    if (socket->bytesToWrite()) socket->waitForBytesWritten(2000);
                    socket->disconnectFromHost(); continue;
                }
                const auto& body = observed.path == "/crash" || observed.path == "/trailer-large" ? large : version.load() == 2 || observed.path == "/resource-two" ? replacement : data;
                auto etag = version.load() == 2 ? QByteArray("\"v2\"") : QByteArray("\"v1\"");
                if (observed.path == "/resource-one" || observed.path == "/resource-two") etag = "\"shared-tag\"";
                if (observed.path == "/weak") etag.prepend("W/");
                const bool chunked = observed.path == "/trailer" || observed.path == "/trailer-large";
                qint64 start = 0;
                bool partial = observed.range.startsWith("bytes=") && observed.condition == etag
                    && observed.path != "/ignore";
                if (partial) start = observed.range.mid(6).chopped(1).toLongLong();
                if (observed.path == "/bad-etag" && partial) etag = "\"unexpected\"";
                const qint64 total = observed.path == "/empty" ? 0 : body.size();
                QByteArray headers;
                if (partial && start == total) {
                    headers = "HTTP/1.1 416 Range Not Satisfiable\r\nContent-Range: bytes */" + QByteArray::number(total) + "\r\nContent-Length: 0\r\n";
                } else if (partial) {
                    headers = "HTTP/1.1 206 Partial Content\r\nContent-Range: bytes "
                        + QByteArray::number(start + (observed.path == "/bad-range" ? 1 : 0)) + '-'
                        + QByteArray::number(total - 1) + '/' + QByteArray::number(total) + "\r\nContent-Length: " + QByteArray::number(total - start) + "\r\n";
                } else {
                    headers = "HTTP/1.1 200 OK\r\n";
                    headers += chunked ? QByteArray("Transfer-Encoding: chunked\r\n") : "Content-Length: " + QByteArray::number(total) + "\r\n";
                }
                if (observed.path != "/no-validator") headers += "ETag: " + etag + "\r\n";
                headers += "Connection: close\r\n\r\n";
                socket->write(headers);
                for (qint64 offset = start; offset < total && !stop.load(); offset += 65536) {
                    const auto count = qMin<qint64>(65536, total - offset);
                    if (chunked && !partial) socket->write(QByteArray::number(count, 16) + "\r\n");
                    if (socket->write(body.constData() + offset, count) < 0) break;
                    if (chunked && !partial) socket->write("\r\n");
                    bool flushed=true;
                    while(socket->bytesToWrite())if(!socket->waitForBytesWritten(2000)){flushed=false;break;}
                    if(!flushed)break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                if (chunked && !partial) socket->write("0\r\nETag: \"trailer-must-not-reset-file\"\r\n\r\n");
                while(socket->bytesToWrite())if(!socket->waitForBytesWritten(2000))break;
                socket->disconnectFromHost();
            }
        });
        port = future.get();
    }
    ~Server() { stop = true; thread.join(); }
    DownloadRequest request(const QString& path) const { return std::get<DownloadRequest>(parseRequest(QString("http://127.0.0.1:%1%2").arg(port).arg(path))); }
    Observation last() { std::lock_guard guard(mutex); return observations.last(); }
    qsizetype count() { std::lock_guard guard(mutex); return observations.size(); }
};
}
int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    if (app.arguments().value(1) == "--crash-child") {
        const auto request = std::get<DownloadRequest>(parseRequest(app.arguments().value(2)));
        const auto destination = app.arguments().value(3), id = app.arguments().value(4);
        std::atomic_bool stopped{false}; bool announced = false;
        const auto result = transferResumable(request, destination, id, stopped, [&](auto) {
            if (!announced && checkpointBytes(destination, id) > 0) {
                announced = true; QTextStream(stdout) << "CHECKPOINT\n" << Qt::flush;
            }
        });
        return result.status == TransferStatus::Completed ? 0 : 10;
    }
    Server server;
    if (!server.port) return 2;
    QTemporaryDir directory(QCoreApplication::applicationDirPath() + "/resume-tests-XXXXXX");
    if (!directory.isValid()) return 3;
    int failures = 0;
    auto check = [&](bool ok, const char* message) { if (!ok) { ++failures; QTextStream(stderr) << "FAIL: " << message << '\n'; } };
    int sequence = 0;
    auto id = [&] { return QString::number(++sequence, 16).rightJustified(32, '0'); };
    std::atomic_bool stopped{false};
    auto pause = [&](const QString& path, const QString& destination, const QString& taskId) {
        stopped = false;
        const auto result = transferResumable(server.request(path), destination, taskId, stopped,
            [&](auto progress) { if (progress.received >= 32768) stopped = true; });
        check(result.status == TransferStatus::Cancelled && checkpointBytes(destination, taskId) >= 32768
            && !QFileInfo::exists(destination), "pause persists verified prefix without publishing");
        stopped = false;
        return checkpointBytes(destination, taskId);
    };
    {
        const auto destination=directory.filePath("large-unknown.bin"),taskId=id();
        const auto result=transferResumable(server.request("/trailer-large"),destination,taskId,stopped);
        if(result.status!=TransferStatus::Completed||read(destination)!=server.large)
            QTextStream(stderr)<<"large chunked status="<<int(result.status)<<" error="<<int(result.error)
                <<" received="<<result.received<<" output="<<QFileInfo(destination).size()<<" message="<<result.message<<'\n';
        check(result.status==TransferStatus::Completed&&read(destination)==server.large,
            "large unknown-length chunked response remains exact across runtime space checks");
    }
    {
        const auto destination = directory.filePath("no-space.bin"), taskId = id();
        server.version = 3;
        const auto result = transferResumable(server.request("/no-space"), destination, taskId, stopped);
        server.version = 1;
        check(result.status == TransferStatus::Failed && result.error == TransferError::Disk
            && result.received == 0 && !QFileInfo::exists(destination), "reject impossible allocation before body");
    }
    {
        const auto destination = directory.filePath("no-space-resume.bin"), taskId = id();
        const auto bytes = pause("/no-space", destination, taskId);
        const auto prefix = read(partialPath(destination, taskId));
        const auto manifest = read(partialPath(destination, taskId) + ".json");
        server.version = 3;
        const auto result = transferResumable(server.request("/no-space"), destination, taskId, stopped);
        server.version = 1;
        check(result.status == TransferStatus::Failed && result.error == TransferError::Disk
            && result.received == bytes && read(partialPath(destination, taskId)) == prefix
            && read(partialPath(destination, taskId) + ".json") == manifest,
            "insufficient restart space preserves prefix and recovery manifest");
        check(transferResumable(server.request("/no-space"), destination, taskId, stopped).status == TransferStatus::Completed
            && read(destination) == server.data, "resume after space rejection remains exact");
    }
    for (const auto& path : {QString("/range"), QString("/ignore"), QString("/weak"), QString("/no-validator"), QString("/trailer")}) {
        const auto destination = directory.filePath(QString::number(sequence) + ".bin"), taskId = id();
        const auto offset = pause(path, destination, taskId);
        const auto result = transferResumable(server.request(path), destination, taskId, stopped);
        check(result.status == TransferStatus::Completed && read(destination) == server.data, "resumed or safely restarted binary is exact");
        const auto observed = server.last();
        if (path == "/weak" || path == "/no-validator") check(observed.range.isEmpty(), "unverifiable resource restarts without Range");
        else check(observed.range == "bytes=" + QByteArray::number(offset) + '-' && observed.condition == "\"v1\"", "Range uses checkpoint offset and strong If-Range");
        const auto count = server.count();
        const auto recovered = transferResumable(server.request(path), destination, taskId, stopped);
        check(recovered.status == TransferStatus::Completed && server.count() == count, "recover rename-before-database gap by local content hash");
        acknowledgePublished(destination, taskId);
        check(!QFileInfo::exists(partialPath(destination, taskId) + ".json"), "acknowledged completion removes recovery manifest");
    }
    {
        const auto destination = directory.filePath("changed.bin"), taskId = id();
        pause("/changed", destination, taskId); server.version = 2;
        const auto result = transferResumable(server.request("/changed"), destination, taskId, stopped);
        check(result.status == TransferStatus::Completed && read(destination) == server.replacement, "changed resource replaces prefix before writing full response");
        server.version = 1;
    }
    {
        const auto destination = directory.filePath("redirect-changed.bin"), taskId = id();
        pause("/redirect-resource", destination, taskId); server.version = 2;
        const auto result = transferResumable(server.request("/redirect-resource"), destination, taskId, stopped);
        check(result.status == TransferStatus::Failed && result.error == TransferError::ResourceChanged && !QFileInfo::exists(destination),
            "same ETag at changed effective URL cannot join different resources");
        server.version = 1;
    }
    for (const auto& path : {QString("/bad-range"), QString("/bad-etag")}) {
        const auto destination = directory.filePath(QString::number(sequence) + ".bin"), taskId = id();
        const auto bytes = pause(path, destination, taskId);
        const auto prefix = read(partialPath(destination, taskId));
        const auto result = transferResumable(server.request(path), destination, taskId, stopped);
        check(result.status == TransferStatus::Failed && result.error == TransferError::ResourceChanged && !QFileInfo::exists(destination)
            && read(partialPath(destination, taskId)) == prefix && checkpointBytes(destination, taskId) == bytes,
            "reject inconsistent 206 before modifying prefix");
        check(discardPartial(destination, taskId), "explicit scratch retry discards owned staging only");
        check(transferResumable(server.request(path), destination, taskId, stopped).status == TransferStatus::Completed
            && read(destination) == server.data, "fresh retry after invalid range succeeds");
    }
    {
        const auto destination = directory.filePath("tampered.bin"), taskId = id();
        pause("/range", destination, taskId);
        QFile part(partialPath(destination, taskId));
        check(part.open(QIODevice::ReadWrite) && part.write("x", 1) == 1, "modify local prefix fixture"); part.close();
        check(transferResumable(server.request("/range"), destination, taskId, stopped).status == TransferStatus::Completed
            && read(destination) == server.data && server.last().range.isEmpty(), "modified prefix triggers full restart");
    }
    {
        const auto destination = directory.filePath("uncommitted-tail.bin"), taskId = id();
        const auto bytes = pause("/range", destination, taskId);
        QFile part(partialPath(destination, taskId));
        check(part.open(QIODevice::Append) && part.write("uncommitted", 11) == 11, "append crash tail fixture"); part.close();
        check(transferResumable(server.request("/range"), destination, taskId, stopped).status == TransferStatus::Completed
            && read(destination) == server.data && server.last().range == "bytes=" + QByteArray::number(bytes) + '-',
            "discard uncheckpointed tail before resuming");
    }
    {
        const auto destination = directory.filePath("empty.bin"), taskId = id();
        check(transferResumable(server.request("/empty"), destination, taskId, stopped).status == TransferStatus::Completed
            && QFileInfo::exists(destination) && QFileInfo(destination).size() == 0, "zero-byte resource publishes correctly");
    }
    for (const auto& phase : {QString("partial"), QString("downloaded")}) {
        const auto destination = directory.filePath("complete-" + phase + ".bin"), taskId = id();
        check(transferResumable(server.request("/range"), destination, taskId, stopped).status == TransferStatus::Completed, "prepare complete staging fixture");
        const auto part = partialPath(destination, taskId);
        check(QFile::rename(destination, part), "simulate stop before publication");
        auto object = QJsonDocument::fromJson(read(part + ".json")).object(); object["phase"] = phase;
        QSaveFile metadata(part + ".json"); const auto bytes = QJsonDocument(object).toJson();
        check(metadata.open(QIODevice::WriteOnly) && metadata.write(bytes) == bytes.size() && metadata.commit(), "save pre-publication phase fixture");
        const auto count = server.count();
        const auto result = transferResumable(server.request("/range"), destination, taskId, stopped);
        check(result.status == TransferStatus::Completed && read(destination) == server.data, "complete verified prefix publishes after restart");
        if (phase == "partial") check(result.responseCode == 416, "validated 416 identifies already complete prefix");
        else check(server.count() == count, "downloaded checkpoint needs no new network request");
    }
    {
        const auto destination = directory.filePath("published-modified.bin"), taskId = id();
        check(transferResumable(server.request("/range"), destination, taskId, stopped).status == TransferStatus::Completed, "publish target fixture");
        QFile target(destination); check(target.open(QIODevice::ReadWrite) && target.write("x", 1) == 1, "modify published target fixture"); target.close();
        const auto modified = read(destination);
        check(transferResumable(server.request("/range"), destination, taskId, stopped).error == TransferError::Destination
            && read(destination) == modified, "changed published file is neither overwritten nor marked complete by old manifest");
    }
    {
        const auto database = directory.filePath("managed.sqlite"), destination = directory.filePath("managed.bin");
        QString taskId;
        {
            TaskManager manager(database, 1);
            taskId = manager.add(server.request("/managed"), destination).id;
            QObject::connect(&manager, &TaskManager::changed, &app, [&](const auto&) {
                const auto& task = manager.tasks()[0];
                if (task.state == TaskState::Running && task.received >= 32768) manager.pause(taskId);
            });
            check(waitUntil([&] { return manager.tasks()[0].state == TaskState::Paused; }), "manager pause reaches persisted Paused");
        }
        {
            TaskManager manager(database, 1);
            check(manager.tasks()[0].state == TaskState::Paused && manager.resume(taskId), "reopen paused task and continue");
            check(waitUntil([&] { return manager.tasks()[0].state == TaskState::Completed; }) && read(destination) == server.data,
                "managed resume across manager restart produces exact file");
        }
    }
    {
        const auto destination = directory.filePath("cancel-paused.bin");
        TaskManager manager(directory.filePath("cancel-paused.sqlite"), 1);
        const auto taskId = manager.add(server.request("/cancel-paused"), destination).id;
        QObject::connect(&manager, &TaskManager::changed, &app, [&](const auto&) {
            const auto& task = manager.tasks()[0];
            if (task.state == TaskState::Running && task.received >= 32768) manager.pause(taskId);
        });
        check(waitUntil([&] { return manager.tasks()[0].state == TaskState::Paused; }), "pause before explicit cancellation");
        check(manager.cancel(taskId) && manager.tasks()[0].state == TaskState::Cancelled
            && !QFileInfo::exists(partialPath(destination, taskId)) && !QFileInfo::exists(partialPath(destination, taskId) + ".json")
            && !QFileInfo::exists(destination), "cancel paused task discards owned data but never publishes target");
    }
    {
        const auto destination = directory.filePath("killed.bin"), taskId = id();
        QProcess child; child.setProgram(QCoreApplication::applicationFilePath());
        child.setArguments({"--crash-child", server.request("/crash").source, destination, taskId}); child.start();
        check(child.waitForStarted(3000) && child.waitForReadyRead(7000) && child.readAllStandardOutput().contains("CHECKPOINT"), "child commits nonempty real-data checkpoint");
        child.kill(); check(child.waitForFinished(3000), "terminate active real HTTP transfer");
        const auto bytes = checkpointBytes(destination, taskId);
        const auto result = transferResumable(server.request("/crash"), destination, taskId, stopped);
        check(bytes > 0 && result.status == TransferStatus::Completed && read(destination) == server.large
            && server.last().range == "bytes=" + QByteArray::number(bytes) + '-', "real process crash resumes verified bytes at committed offset");
    }
    QTextStream(stdout) << "HTTP resume checks: " << failures << " failures\n";
    return failures ? 1 : 0;
}
