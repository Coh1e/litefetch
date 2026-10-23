#pragma once
#include <QByteArray>
#include <QJsonObject>
#include <optional>
namespace downloader::native {
inline constexpr qsizetype maxMessageBytes = 64 * 1024;
QByteArray frame(const QByteArray& json);
// Incomplete input remains untouched; malformed lengths throw before allocation.
std::optional<QByteArray> takeFrame(QByteArray& input);
QJsonObject parseObject(const QByteArray& input);
bool validRequest(const QJsonObject& request);
bool validReply(const QJsonObject& reply, const QString& messageId);
QJsonObject rejected(const QString& messageId);
}
