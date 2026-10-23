#include "browser_bridge.h"
#include "native_protocol.h"
#include <QDateTime>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QRegularExpression>
#include <QTemporaryFile>
#include <QTimer>
#include <memory>
#include <stdexcept>
namespace downloader {
namespace {
void validate(BrowserBridgeConfig& config)
{
    const QRegularExpression endpoint(QStringLiteral("^[A-Za-z0-9_.-]{1,100}$"));
    const QRegularExpression origin(QStringLiteral("^chrome-extension://[a-p]{32}/$"));
    if (!endpoint.match(config.ipcName).hasMatch() || config.allowedOrigins.isEmpty()
        || config.allowedOrigins.size()>10 || !QFileInfo(config.downloadDirectory).isAbsolute()
        || (config.enabled && !QFileInfo(config.downloadDirectory).isDir())) throw std::runtime_error("Invalid browser bridge configuration");
    for (const auto& value : config.allowedOrigins) if (!origin.match(value).hasMatch())
        throw std::runtime_error("Invalid browser origin");
    const auto canonical=QFileInfo(config.downloadDirectory).canonicalFilePath();
    if (canonical.isEmpty() && config.enabled) throw std::runtime_error("Browser download directory cannot be resolved");
    config.downloadDirectory=canonical.isEmpty()?QDir::cleanPath(config.downloadDirectory):canonical;
}
QJsonObject result(const std::optional<BrowserHandoff>& value)
{
    if (!value) return {{"state","missing"}};
    QString state;
    switch (value->state) {
    case HandoffState::Prepared: state="prepared"; break;
    case HandoffState::Committed: state="committed"; break;
    case HandoffState::Aborted: state="aborted"; break;
    case HandoffState::Expired: state="expired"; break;
    }
    return {{"state",state},{"taskId",value->taskId},{"expiresAt",value->expires}};
}
QString filename(const DownloadRequest& request)
{
    auto name=request.suggestedName;
    name.replace(QRegularExpression(QStringLiteral("[<>:\"/\\\\|?*\\x{0000}-\\x{001f}]")),"_");
    while (name.endsWith(' ') || name.endsWith('.')) name.chop(1);
    if (name.isEmpty() || name=="." || name=="..") name="download";
    const auto stem=name.section('.',0,0).trimmed();
    if (QRegularExpression(QStringLiteral("^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])$"),QRegularExpression::CaseInsensitiveOption).match(stem).hasMatch()) name.prepend('_');
    if (name.size()>120) name=name.left(120);
    if (!name.isEmpty() && name.at(name.size()-1).isHighSurrogate()) name.chop(1);
    while (name.endsWith(' ') || name.endsWith('.')) name.chop(1);
    if (name.isEmpty()) name="download";
    return name;
}
}
BrowserBridgeConfig loadBrowserBridgeConfig(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size()>16384) throw std::runtime_error("Cannot read browser bridge configuration");
    const auto json=native::parseObject(file.readAll());
    BrowserBridgeConfig config; config.enabled=json.value("enabled").toBool();
    config.ipcName=json.value("ipcName").toString(); config.downloadDirectory=json.value("downloadDirectory").toString();
    if (!config.enabled && config.ipcName.isEmpty()) return config;
    for (const auto& item : json.value("allowedOrigins").toArray()) config.allowedOrigins.append(item.toString());
    validate(config); return config;
}
BrowserBridge::BrowserBridge(TaskManager& manager, BrowserBridgeConfig config, QObject* parent)
    : QObject(parent),manager_(manager),config_(std::move(config))
{
    validate(config_);
    // Windows named pipes permit multiple listening instances with one name.
    // Keep a kernel-backed ownership object alive before opening the pipe.
    owner_.setKey("NativeDownloaderBridge-"+QString::fromLatin1(QCryptographicHash::hash(
        config_.ipcName.toCaseFolded().toUtf8(),QCryptographicHash::Sha256).toHex()));
    if (!owner_.create(1)) throw std::runtime_error("Browser bridge endpoint is already owned");
    server_.setSocketOptions(QLocalServer::UserAccessOption);
    server_.setMaxPendingConnections(8);
    QObject::connect(&server_,&QLocalServer::newConnection,this,[this] {
        while (auto* socket=server_.nextPendingConnection()) {
            if (clients_.size()>=16) { socket->abort(); socket->deleteLater(); continue; }
            clients_.insert(socket);
            socket->setReadBufferSize(native::maxMessageBytes+5);
            const auto bytes=std::make_shared<QByteArray>(); const auto processed=std::make_shared<bool>(false);
            auto* deadline=new QTimer(socket); deadline->setSingleShot(true); deadline->start(3000);
            QObject::connect(deadline,&QTimer::timeout,socket,&QLocalSocket::abort);
            QObject::connect(socket,&QLocalSocket::disconnected,this,[this,socket] { clients_.remove(socket); socket->deleteLater(); });
            const auto receive=[this,socket,bytes,processed] {
                if (*processed) return;
                bytes->append(socket->read(native::maxMessageBytes+5-bytes->size()));
                if (bytes->size()>native::maxMessageBytes+4) { socket->abort(); return; }
                try {
                    const auto body=native::takeFrame(*bytes);
                    if (!body) return;
                    *processed=true;
                    if (!bytes->isEmpty() || socket->bytesAvailable()) { socket->abort(); return; }
                    const auto response=handle(native::parseObject(*body));
                    const auto wire=native::frame(QJsonDocument(response).toJson(QJsonDocument::Compact));
                    if (socket->write(wire)!=wire.size()) { socket->abort(); return; }
                    socket->disconnectFromServer();
                } catch (...) { socket->abort(); }
            };
            QObject::connect(socket,&QLocalSocket::readyRead,this,receive);
            receive();
        }
    });
    // Never delete another process's server name to make this listen succeed.
    if (!server_.listen(config_.ipcName)) throw std::runtime_error("Browser bridge endpoint is unavailable");
}
BrowserBridge::~BrowserBridge()
{
    server_.close();
    // Disconnect callbacks before member destruction; sockets are owned by the
    // server, while their callbacks otherwise reference clients_.
    const auto clients=clients_.values();
    for (auto* socket : clients) {
        QObject::disconnect(socket,nullptr,this,nullptr); socket->abort(); delete socket;
    }
    clients_.clear();
}
void BrowserBridge::updatePreferences(bool enabled,const QString& downloadDirectory)
{
    auto next=config_; next.enabled=enabled; next.downloadDirectory=downloadDirectory;
    validate(next); config_=std::move(next);
}
QJsonObject BrowserBridge::handle(const QJsonObject& message)
{
    const auto id=message.value("id").toString(),origin=message.value("origin").toString();
    if (!native::validRequest(message) || !config_.allowedOrigins.contains(origin)) return native::rejected(id);
    try {
        const auto requestId=message.value("requestId").toString(),method=message.value("method").toString();
        const auto now=QDateTime::currentMSecsSinceEpoch();
        std::optional<BrowserHandoff> record;
        if (method=="prepare") {
            const auto parsed=parseRequest(message.value("payload").toObject().value("url").toString());
            const auto* request=std::get_if<DownloadRequest>(&parsed);
            if (!request || request->protocol!=Protocol::Http) return native::rejected(id);
            record=manager_.queryBrowserHandoff(origin,requestId,now);
            if (record) {
                if (record->source!=request->source) return native::rejected(id);
            } else {
                if (!config_.enabled) return native::rejected(id);
                QTemporaryFile probe(QDir(config_.downloadDirectory).filePath(".download-write-XXXXXX"));
                if (!probe.open()) return native::rejected(id);
                probe.close(); // Auto-remove before replying; never reserve the final filename by overwriting it.
                record=manager_.prepareBrowserHandoff(origin,requestId,*request,
                    QDir(config_.downloadDirectory).filePath(filename(*request)),now);
            }
        } else if (method=="query") record=manager_.queryBrowserHandoff(origin,requestId,now);
        else if (method=="abort") record=manager_.abortBrowserHandoff(origin,requestId,now);
        else if (method=="commit") record=manager_.commitBrowserHandoff(origin,requestId,now);
        return {{"version",1},{"id",id},{"ok",true},{"result",result(record)}};
    } catch (...) { return native::rejected(id); }
}
}
