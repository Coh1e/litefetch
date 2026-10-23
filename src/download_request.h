#pragma once

#include <QString>
#include <QStringList>
#include <QUrl>
#include <optional>
#include <variant>

namespace downloader {

enum class Protocol { Http, Ftp, BitTorrent, Ed2k };

struct DownloadRequest {
    Protocol protocol;
    QString source;
    QString suggestedName;
    QStringList identities;
    std::optional<quint64> expectedSize;
};

struct InvalidRequest { QString message; };
using RequestResult = std::variant<DownloadRequest, InvalidRequest>;

// Pure parsing: no network, filesystem writes, or destination path selection.
RequestResult parseRequest(const QString& input);

} // namespace downloader
