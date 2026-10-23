#include "native_protocol.h"
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QRegularExpression>
#include <QtEndian>
#include <cstdio>
#ifdef Q_OS_WIN
#include <fcntl.h>
#include <io.h>
#endif

namespace {
using namespace downloader::native;
bool readExact(QFile& input, QByteArray& bytes, qsizetype length)
{
    while (bytes.size() < length) {
        const auto chunk = input.read(length - bytes.size());
        if (chunk.isEmpty()) return false;
        bytes.append(chunk);
    }
    return true;
}
QJsonObject forward(QJsonObject message, const QString& origin, const QString& endpoint)
{
    const auto id = message.value("id").toString();
    message.insert("origin", origin); // Never trust an origin supplied in the JSON body.
    const auto bytes = frame(QJsonDocument(message).toJson(QJsonDocument::Compact));
    QElapsedTimer clock; clock.start();
    const auto remaining = [&] { return qMax(0, 3000 - static_cast<int>(clock.elapsed())); };
    QLocalSocket socket;
    socket.connectToServer(endpoint);
    if (!socket.waitForConnected(qMin(1000, remaining()))) return rejected(id);
    if (socket.write(bytes) != bytes.size()) return rejected(id);
    while (socket.bytesToWrite()) {
        if (!remaining() || !socket.waitForBytesWritten(remaining())) return rejected(id);
    }
    QByteArray response;
    while (remaining()) {
        response.append(socket.read(maxMessageBytes + 5 - response.size()));
        if (response.size() > maxMessageBytes + 4) return rejected(id);
        if (auto json = takeFrame(response)) {
            if (!response.isEmpty()) return rejected(id);
            const auto reply = parseObject(*json);
            return validReply(reply, id) ? reply : rejected(id);
        }
        if (!socket.waitForReadyRead(remaining())) return rejected(id);
    }
    return rejected(id);
}
}
int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
#ifdef Q_OS_WIN
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    // A persistent native port must receive each message before stdin closes.
    // Avoid QFile's read-ahead requesting a larger buffered stdio block.
    std::setvbuf(stdin,nullptr,_IONBF,0);
    std::setvbuf(stdout,nullptr,_IONBF,0);
    try {
        const auto arguments = app.arguments();
        if (arguments.size() < 2 || arguments.size() > 3) return 3;
        auto origin = arguments.at(1);
        const QRegularExpression originPattern(QStringLiteral("^chrome-extension://[a-p]{32}/?$"));
        if (!originPattern.match(origin).hasMatch()) return 3;
        if (!origin.endsWith('/')) origin.append('/');
        if (arguments.size() == 3 && !QRegularExpression(QStringLiteral("^--parent-window=[0-9]+$")).match(arguments.at(2)).hasMatch()) return 3;
        QFile configuration(QDir(app.applicationDirPath()).filePath("native-host.json"));
        if (!configuration.open(QIODevice::ReadOnly) || configuration.size() > 16384) return 3;
        const auto config = parseObject(configuration.readAll());
        bool permitted = false;
        const auto allowed = config.value("allowedOrigins").toArray();
        if (allowed.size() > 10) return 3;
        for (const auto& entry : allowed) if (entry.toString() == origin) permitted = true;
        const auto endpoint = config.value("ipcName").toString();
        if (!permitted || !QRegularExpression(QStringLiteral("^[A-Za-z0-9_.-]{1,100}$")).match(endpoint).hasMatch()) return 3;
        QFile input, output;
        if (!input.open(stdin, QIODevice::ReadOnly | QIODevice::Unbuffered, QFileDevice::DontCloseHandle)
            || !output.open(stdout, QIODevice::WriteOnly | QIODevice::Unbuffered, QFileDevice::DontCloseHandle)) return 2;
        for (;;) {
            QByteArray header;
            if (!readExact(input, header, 4)) return header.isEmpty() ? 0 : 2;
            const auto length = qFromLittleEndian<quint32>(header.constData());
            if (!length || length > maxMessageBytes) return 2;
            QByteArray body;
            if (!readExact(input, body, length)) return 2;
            const auto request = parseObject(body);
            const auto id = request.value("id").toString();
            QJsonObject reply = rejected(id);
            if (validRequest(request)) {
                try { reply = forward(request, origin, endpoint); }
                catch (...) { reply = rejected(id); }
            }
            const auto wire = frame(QJsonDocument(reply).toJson(QJsonDocument::Compact));
            if (output.write(wire) != wire.size() || !output.flush()) return 2;
        }
    } catch (...) {
        // No raw URLs, credentials, payloads, or protocol text in diagnostics.
        std::fputs("Native bridge input or configuration failed.\n", stderr);
        return 2;
    }
}
