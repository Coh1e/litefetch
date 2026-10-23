#pragma once
#include <QFileInfo>
#include <QStorageInfo>
#include <QString>
#include <stdexcept>

namespace downloader {
class DiskSpaceError : public std::runtime_error {
public:
    explicit DiskSpaceError(const QString& message) : std::runtime_error(message.toStdString()), message_(message) {}
    const QString& message() const noexcept { return message_; }
private:
    QString message_;
};
// A preflight is advisory: concurrent writers can consume space after this query.
// An unavailable query is not evidence of either sufficient or insufficient space.
inline QString insufficientDiskSpace(const QString& path, qint64 required)
{
    if (required <= 0) return {};
    QFileInfo parent(path);
    while (!parent.exists()) {
        const auto next = parent.absolutePath();
        if (next == parent.absoluteFilePath()) return {};
        parent.setFile(next);
    }
    QStorageInfo storage(parent.absoluteFilePath());
    storage.refresh();
    if (!storage.isValid() || !storage.isReady() || storage.bytesAvailable() < 0) return {};
    if (storage.bytesAvailable() >= required) return {};
    return QStringLiteral("磁盘空间不足：需要 %1 字节，可用 %2 字节（%3）。请释放空间后继续。")
        .arg(required).arg(storage.bytesAvailable()).arg(storage.rootPath());
}
inline void requireDiskSpace(const QString& path, qint64 required)
{
    const auto message = insufficientDiskSpace(path, required);
    if (!message.isEmpty()) throw DiskSpaceError(message);
}
}
