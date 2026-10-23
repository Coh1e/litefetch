#include "native_protocol.h"
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalServer>
#include <QLocalSocket>
#include <QProcess>
#include <QTemporaryDir>
#include <QTextStream>
#include <QUuid>
#include <memory>
using namespace downloader::native;
namespace {
struct Run { int code; QByteArray output; QByteArray diagnostic; };
}
int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    int failures = 0;
    const auto expect = [&](bool success, const char* name) {
        if (!success) { ++failures; QTextStream(stderr) << "FAIL " << name << '\n'; }
    };
    const QString origin = "chrome-extension://" + QString(32, 'a') + "/";
    QTemporaryDir directory(QDir(app.applicationDirPath()).filePath("native-host-check-XXXXXX"));
    if (!directory.isValid()) return 10;
    const auto executable = directory.filePath("native-downloader-host.exe");
    if (!QFile::copy(app.arguments().value(1), executable)) return 11;
    const auto endpoint = "NativeDownloader-test-" + QUuid::createUuid().toString(QUuid::Id128);
    QFile config(directory.filePath("native-host.json"));
    if (!config.open(QIODevice::WriteOnly)) return 12;
    config.write(QJsonDocument(QJsonObject{{"ipcName", endpoint}, {"allowedOrigins", QJsonArray{origin}}}).toJson());
    config.close();
    QLocalServer server;
    server.setSocketOptions(QLocalServer::UserAccessOption);
    if (!server.listen(endpoint)) return 13;
    int forwarded = 0;
    enum class Response { Normal, WrongId, Oversized } responseMode = Response::Normal;
    QObject::connect(&server, &QLocalServer::newConnection, &app, [&] {
        while (auto* socket = server.nextPendingConnection()) {
            const auto buffer = std::make_shared<QByteArray>();
            QObject::connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
            QObject::connect(socket, &QLocalSocket::readyRead, socket, [&, socket, buffer] {
                buffer->append(socket->readAll());
                try {
                    const auto body = takeFrame(*buffer);
                    if (!body) return;
                    const auto request = parseObject(*body);
                    ++forwarded;
                    expect(request.value("origin").toString() == origin, "origin forwarded from process argument");
                    if (responseMode == Response::Oversized) {
                        socket->write(QByteArray::fromHex("01000100")); socket->flush(); return;
                    }
                    const auto id = responseMode == Response::WrongId ? QString("wrong") : request.value("id").toString();
                    const auto reply = QJsonObject{{"version", 1}, {"id", id}, {"ok", true},
                        {"result", QJsonObject{{"state", "prepared"}, {"taskId", "task"}, {"expiresAt", 10000}}}};
                    const auto wire = frame(QJsonDocument(reply).toJson(QJsonDocument::Compact));
                    // Exercise a split header/body response from the application.
                    socket->write(wire.left(2)); socket->flush();
                    socket->write(wire.mid(2)); socket->flush();
                } catch (...) { expect(false, "application receives valid framed request"); }
            });
        }
    });
    const auto run = [&](const QByteArray& input, const QString& caller, bool fragmented = false) -> Run {
        QProcess process;
        process.setProgram(executable); process.setArguments({caller});
        process.setWorkingDirectory(directory.path());
        process.start();
        if (!process.waitForStarted(2000)) return {-100, {}, {}};
        if (fragmented) {
            for (qsizetype index = 0; index < input.size(); index += 3) {
                process.write(input.mid(index, 3)); process.waitForBytesWritten(1000);
            }
        } else process.write(input);
        process.closeWriteChannel();
        QElapsedTimer clock; clock.start();
        while (process.state() != QProcess::NotRunning && clock.elapsed() < 8000) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
            process.waitForFinished(1);
        }
        if (process.state() != QProcess::NotRunning) { process.kill(); process.waitForFinished(); return {-101, {}, {}}; }
        return {process.exitCode(), process.readAllStandardOutput(), process.readAllStandardError()};
    };
    const QJsonObject query{{"version", 1}, {"id", "message1"}, {"method", "query"}, {"requestId", "request1"}};
    const auto encode = [](const QJsonObject& object) { return frame(QJsonDocument(object).toJson(QJsonDocument::Compact)); };
    const auto replyObject = [&](const Run& result) {
        auto bytes = result.output;
        const auto body = takeFrame(bytes);
        expect(body.has_value() && bytes.isEmpty(), "exactly one framed response");
        return body ? parseObject(*body) : QJsonObject{};
    };
    {
        const auto result = run(encode(query), origin, true);
        expect(result.code == 0, "valid fragmented stdin clean exit");
        expect(replyObject(result).value("ok").toBool(), "actual local IPC result forwarded");
        expect(result.diagnostic.isEmpty(), "no diagnostic leakage on valid protocol");
    }
    {
        const auto result=run(encode(query),origin.chopped(1));
        expect(result.code==0 && replyObject(result).value("ok").toBool(),"caller origin without trailing slash normalizes safely");
    }
    {
        auto next = query; next.insert("id", "message2");
        auto result = run(encode(query) + encode(next), origin);
        expect(result.code == 0, "multiple frames accepted");
        const auto first = takeFrame(result.output), second = takeFrame(result.output);
        expect(first && second && result.output.isEmpty(), "two independent framed replies");
    }
    {
        auto request = query; request.insert("method", "prepare");
        request.insert("origin", "chrome-extension://" + QString(32, 'b') + "/");
        request.insert("payload", QJsonObject{{"url", QString::fromUtf8("https://example.test/文件?token=opaque")},
            {"method", "GET"}, {"uniqueMatch", true}, {"replaySafe", true}});
        const auto result = run(encode(request), origin, true);
        expect(replyObject(result).value("ok").toBool(), "UTF8 prepare and authoritative process origin");
    }
    {
        const int before = forwarded;
        const auto result = run(encode(query), "chrome-extension://" + QString(32, 'b') + "/");
        expect(result.code == 3 && result.output.isEmpty() && forwarded == before, "unapproved caller denied before IPC");
    }
    {
        auto request = query; request.insert("method", "execute");
        const int before = forwarded;
        const auto result = run(encode(request), origin);
        expect(result.code == 0 && !replyObject(result).value("ok").toBool() && forwarded == before,
            "unknown operation rejected without IPC");
    }
    {
        auto request = query; request.insert("method", "prepare");
        request.insert("payload", QJsonObject{{"url", "file:///C:/outside"}, {"method", "GET"},
            {"uniqueMatch", true}, {"replaySafe", true}});
        const int before = forwarded;
        const auto result = run(encode(request), origin);
        expect(!replyObject(result).value("ok").toBool() && forwarded == before, "non HTTP payload rejected");
    }
    for (const auto& malformed : {QByteArray::fromHex("00000000"), QByteArray::fromHex("01000100"),
        QByteArray::fromHex("0500"), QByteArray::fromHex("050000007b"), frame("[]"), frame("{broken"), frame(QByteArray("{\"x\":\"") + QByteArray::fromHex("ff") + "\"}")}) {
        const auto result = run(malformed, origin);
        expect(result.code == 2 && result.output.isEmpty(), "malformed frame/JSON rejected");
    }
    {
        responseMode = Response::WrongId;
        const auto result = run(encode(query), origin);
        expect(!replyObject(result).value("ok").toBool(), "mismatched application reply rejected");
        responseMode = Response::Oversized;
        const auto large = run(encode(query), origin);
        expect(!replyObject(large).value("ok").toBool(), "oversized application reply rejected");
        responseMode = Response::Normal;
    }
    server.close();
    {
        const auto result = run(encode(query), origin);
        expect(result.code == 0 && !replyObject(result).value("ok").toBool(), "application offline gives correlated rejection");
    }
    expect(run({}, origin).code == 0, "empty stream clean EOF");
    QTextStream(stdout) << "Native host actual-process checks: " << (failures ? "FAILED" : "PASS") << '\n';
    return failures ? 1 : 0;
}
