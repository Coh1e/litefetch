#include "transfer.h"
#include "task_manager.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QHostAddress>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTextStream>
#include <QElapsedTimer>
#include <chrono>
#include <future>
#include <thread>

using namespace downloader;

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QByteArray payload(1024 * 1024, Qt::Uninitialized);
    for (qsizetype i = 0; i < payload.size(); ++i) payload[i] = static_cast<char>(i * 31 % 251);
    std::atomic_bool stop{false};
    std::promise<quint16> ready;
    auto port = ready.get_future();
    std::thread server([&] {
        QTcpServer listener;
        if (!listener.listen(QHostAddress::LocalHost, 0)) { ready.set_value(0); return; }
        ready.set_value(listener.serverPort());
        while (!stop.load()) {
            if (!listener.hasPendingConnections() && !listener.waitForNewConnection(100)) continue;
            std::unique_ptr<QTcpSocket> socket(listener.nextPendingConnection());
            QByteArray request;
            while (!request.contains("\r\n\r\n") && request.size() < 16384) {
                request += socket->readAll();
                if (request.contains("\r\n\r\n") || !socket->waitForReadyRead(2000)) break;
            }
            const auto path = request.split(' ').value(1);
            if (path == "/redirect") {
                socket->write("HTTP/1.1 302 Found\r\nLocation: /file\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
            } else if (path == "/missing") {
                socket->write("HTTP/1.1 404 Not Found\r\nContent-Length: 5\r\nConnection: close\r\n\r\nerror");
            } else if (path == "/forbidden-redirect") {
                socket->write("HTTP/1.1 302 Found\r\nLocation: file:///C:/Windows/win.ini\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
            } else {
                socket->write(path=="/unknown"?QByteArray("HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n"):
                    "HTTP/1.1 200 OK\r\nContent-Length: " + QByteArray::number(payload.size()) + "\r\nConnection: close\r\n\r\n");
                const auto length = path == "/truncated" ? 10000 : payload.size();
                for (qsizetype offset = 0; offset < length && !stop.load(); offset += 8192) {
                    if (socket->write(payload.constData() + offset, qMin<qsizetype>(8192, length - offset)) < 0) break;
                    if (socket->bytesToWrite() && !socket->waitForBytesWritten(2000)) break;
                    if (path == "/slow") std::this_thread::sleep_for(std::chrono::milliseconds(3));
                }
            }
            if (socket->bytesToWrite()) socket->waitForBytesWritten(2000);
            socket->disconnectFromHost();
        }
    });
    struct Join { std::atomic_bool& stop; std::thread& thread; ~Join() { stop = true; thread.join(); } } join{stop, server};
    const auto listeningPort = port.get();
    if (!listeningPort) return 2;
    std::promise<quint16> ftpReady;
    auto ftpPort = ftpReady.get_future();
    std::thread ftpServer([&] {
        QTcpServer listener;
        if (!listener.listen(QHostAddress::LocalHost, 0)) { ftpReady.set_value(0); return; }
        ftpReady.set_value(listener.serverPort());
        while (!stop.load()) {
            if (!listener.hasPendingConnections() && !listener.waitForNewConnection(100)) continue;
            std::unique_ptr<QTcpSocket> control(listener.nextPendingConnection());
            QTcpServer dataServer;
            auto reply = [&](const QByteArray& text) { control->write(text + "\r\n"); return control->waitForBytesWritten(2000); };
            reply("220 Local test FTP");
            while (!stop.load()) {
                if (!control->canReadLine() && !control->waitForReadyRead(2000)) break;
                if (!control->canReadLine()) continue;
                const auto line = control->readLine().trimmed();
                const auto command = line.split(' ').first();
                if (command == "USER") reply("331 Password required");
                else if (command == "PASS") reply("230 Logged in");
                else if (command == "PWD") reply("257 \"/\"");
                else if (command == "TYPE") reply("200 Binary mode");
                else if (command == "SIZE") reply(line.contains("huge.bin")?QByteArray("213 9223372036854775807"):
                    "213 " + QByteArray::number(payload.size()));
                else if (command == "EPSV") {
                    dataServer.close();
                    if (!dataServer.listen(QHostAddress::LocalHost, 0)) break;
                    reply("229 Entering Extended Passive Mode (|||" + QByteArray::number(dataServer.serverPort()) + "|)");
                } else if (command == "RETR") {
                    if (!dataServer.hasPendingConnections() && !dataServer.waitForNewConnection(2000)) break;
                    std::unique_ptr<QTcpSocket> data(dataServer.nextPendingConnection());
                    reply("150 Opening data connection");
                    data->write(payload);
                    while (data->bytesToWrite() && data->waitForBytesWritten(2000)) {}
                    data->disconnectFromHost();
                    if (data->state() != QAbstractSocket::UnconnectedState) data->waitForDisconnected(2000);
                    reply("226 Transfer complete");
                } else if (command == "QUIT") { reply("221 Goodbye"); break; }
                else reply("500 Unsupported test command");
            }
        }
    });
    Join ftpJoin{stop, ftpServer};
    const auto ftpListeningPort = ftpPort.get();
    if (!ftpListeningPort) return 4;
    int failures = 0;
    auto check = [&](bool ok, const char* message) { if (!ok) { ++failures; QTextStream(stderr) << "FAIL: " << message << '\n'; } };
    QTemporaryDir directory(QCoreApplication::applicationDirPath() + "/transfer-tests-XXXXXX");
    if (!directory.isValid()) return 3;
    auto download = [&](const QString& path, const QString& name, std::atomic_bool& cancelled,
                        const std::function<void(TransferProgress)>& progress = {}) {
        auto parsed = parseRequest(QString("http://127.0.0.1:%1%2").arg(listeningPort).arg(path));
        auto result = transfer(std::get<DownloadRequest>(parsed), directory.filePath(name), cancelled, progress);
        QTextStream(stdout) << path << " status=" << static_cast<int>(result.status)
            << " error=" << static_cast<int>(result.error) << " response=" << result.responseCode
            << " bytes=" << result.received << " message=" << result.message << '\n';
        return result;
    };
    auto contents = [&](const QString& name) { QFile file(directory.filePath(name)); if (!file.open(QIODevice::ReadOnly)) return QByteArray{}; return file.readAll(); };
    std::atomic_bool cancelled{false};
    qint64 seen = 0;
    auto result = download("/file", "完整文件.bin", cancelled, [&](auto p) { seen = qMax(seen, p.received); });
    check(result.status == TransferStatus::Completed && result.received == payload.size(), "real HTTP transfer size");
    check(contents("完整文件.bin") == payload && seen > 0, "binary content and progress, Unicode destination");
    result = download("/unknown", "unknown.bin", cancelled);
    check(result.status == TransferStatus::Completed && contents("unknown.bin") == payload,
        "unknown-length EOF-delimited HTTP survives runtime space checks");
    result = download("/redirect", "redirect.bin", cancelled);
    check(result.status == TransferStatus::Completed && contents("redirect.bin") == payload, "HTTP redirect");
    result = download("/missing", "missing.bin", cancelled);
    check(result.status == TransferStatus::Failed && result.error == TransferError::Http && result.responseCode == 404, "HTTP error classification");
    check(!QFile::exists(directory.filePath("missing.bin")), "HTTP error body not published");
    result = download("/truncated", "truncated.bin", cancelled);
    check(result.status == TransferStatus::Failed && !QFile::exists(directory.filePath("truncated.bin")), "truncated response not published");
    result = download("/slow", "cancelled.bin", cancelled, [&](auto p) { if (p.received >= 32768) cancelled = true; });
    check(result.status == TransferStatus::Cancelled && !QFile::exists(directory.filePath("cancelled.bin")), "cancel during real transfer");
    cancelled = false;
    result = download("/file", "完整文件.bin", cancelled);
    check(result.error == TransferError::Destination && contents("完整文件.bin") == payload, "existing file unchanged");
    result = download("/forbidden-redirect", "forbidden.bin", cancelled);
    check(result.status == TransferStatus::Failed && !QFile::exists(directory.filePath("forbidden.bin")), "redirect cannot read local files");
    const auto ftpRequest = std::get<DownloadRequest>(parseRequest(QString("ftp://user:password@127.0.0.1:%1/file.bin").arg(ftpListeningPort)));
    result = transfer(ftpRequest, directory.filePath("ftp.bin"), cancelled);
    check(result.status == TransferStatus::Completed && contents("ftp.bin") == payload, "authenticated passive FTP binary transfer");
    const auto hugeFtp = std::get<DownloadRequest>(parseRequest(QString("ftp://user:password@127.0.0.1:%1/huge.bin").arg(ftpListeningPort)));
    result = transfer(hugeFtp, directory.filePath("huge-ftp.bin"), cancelled);
    check(result.status == TransferStatus::Failed && result.error == TransferError::Disk && result.received == 0
        && !QFile::exists(directory.filePath("huge-ftp.bin")), "FTP SIZE shortage rejects before writing payload");
    {
        const auto database = directory.filePath("pipeline.sqlite");
        {
            TaskManager manager(database, 2);
            const auto base = QString("http://127.0.0.1:%1").arg(listeningPort);
            check(manager.add(std::get<DownloadRequest>(parseRequest(base + "/file")), directory.filePath("managed.bin")).added, "persist and accept real download");
            check(manager.add(std::get<DownloadRequest>(parseRequest(base + "/redirect")), directory.filePath("managed-redirect.bin")).added, "accept second queued real download");
            QElapsedTimer timeout; timeout.start();
            auto finished = [&] {
                for (const auto& task : manager.tasks())
                    if (task.state == TaskState::Running || task.state == TaskState::Queued) return false;
                return true;
            };
            while (!finished() && timeout.elapsed() < 5000) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            check(finished() && manager.tasks().size() == 2
                && manager.tasks()[0].state == TaskState::Completed && manager.tasks()[1].state == TaskState::Completed,
                "real transfers pass through durable scheduler");
        }
        TaskStore reopened(database);
        const auto saved = reopened.load();
        check(saved.size() == 2 && saved[0].state == TaskState::Completed && saved[1].state == TaskState::Completed
            && contents("managed.bin") == payload && contents("managed-redirect.bin") == payload,
            "real output and terminal state survive reopening");
    }
    check(QDir(directory.path()).entryList({"*.part"}, QDir::Files).isEmpty(), "staging files cleaned on all exits");
    QTextStream(stdout) << "Transfer integration checks: " << failures << " failures\n";
    return failures ? 1 : 0;
}
