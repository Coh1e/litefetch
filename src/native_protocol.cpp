#include "native_protocol.h"
#include <QJsonDocument>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QStringDecoder>
#include <QUrl>
#include <QtEndian>
#include <bit>
#include <stdexcept>

namespace downloader::native {
static_assert(std::endian::native == std::endian::little, "Windows Native Messaging byte order");
QByteArray frame(const QByteArray& json)
{
    if (json.isEmpty() || json.size() > maxMessageBytes) throw std::runtime_error("Invalid native frame length");
    QByteArray result(4, Qt::Uninitialized);
    qToLittleEndian<quint32>(static_cast<quint32>(json.size()), result.data());
    result.append(json);
    return result;
}
std::optional<QByteArray> takeFrame(QByteArray& input)
{
    if (input.size() < 4) return std::nullopt;
    const auto length = qFromLittleEndian<quint32>(input.constData());
    if (length == 0 || length > maxMessageBytes) throw std::runtime_error("Invalid native frame length");
    if (input.size() < 4 + static_cast<qsizetype>(length)) return std::nullopt;
    auto result = input.mid(4, length);
    input.remove(0, 4 + length);
    return result;
}
QJsonObject parseObject(const QByteArray& input)
{
    QStringDecoder decoder(QStringDecoder::Utf8);
    const QString decoded = decoder.decode(input);
    Q_UNUSED(decoded);
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(input, &error);
    if (decoder.hasError() || error.error != QJsonParseError::NoError || !document.isObject()) {
        throw std::runtime_error("Invalid native JSON");
    }
    return document.object();
}
namespace {
bool identifier(const QJsonValue& value)
{
    static const QRegularExpression pattern(QStringLiteral("^[A-Za-z0-9_-]{1,128}$"));
    return value.isString() && pattern.match(value.toString()).hasMatch();
}
}
bool validRequest(const QJsonObject& request)
{
    if (request.value("version").toInt(-1) != 1 || !identifier(request.value("id"))
        || !identifier(request.value("requestId"))) return false;
    const auto method = request.value("method").toString();
    if (method != "prepare") return (method == "query" || method == "commit" || method == "abort")
        && !request.contains("payload");
    if (!request.value("payload").isObject()) return false;
    const auto payload = request.value("payload").toObject();
    const auto source = payload.value("url").toString();
    const QUrl url(source, QUrl::StrictMode);
    return !source.isEmpty() && source.size() <= 8192 && url.isValid() && !url.host().isEmpty()
        && (url.scheme() == "http" || url.scheme() == "https") && url.userInfo().isEmpty()
        && payload.value("method").toString() == "GET" && payload.value("uniqueMatch").isBool()
        && payload.value("uniqueMatch").toBool() && payload.value("replaySafe").isBool()
        && payload.value("replaySafe").toBool();
}
bool validReply(const QJsonObject& reply, const QString& messageId)
{
    return reply.value("version").toInt(-1) == 1 && reply.value("id").toString() == messageId
        && reply.value("ok").isBool() && (!reply.value("ok").toBool() || reply.value("result").isObject());
}
QJsonObject rejected(const QString& messageId)
{
    return {{"version", 1}, {"id", messageId}, {"ok", false}};
}
}
