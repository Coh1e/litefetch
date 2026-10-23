#include "download_request.h"

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QUrlQuery>

namespace downloader {
namespace {
bool matches(const QString& value, const char* pattern)
{
    return QRegularExpression(QString::fromLatin1(pattern)).match(value).hasMatch();
}

RequestResult invalid(const char* text)
{
    return InvalidRequest{QString::fromUtf8(text)};
}
} // namespace

RequestResult parseRequest(const QString& input)
{
    const auto text = input.trimmed();
    if (text.isEmpty() || text.size() > 64 * 1024 || text.contains(QChar::Null)
        || text.contains('\r') || text.contains('\n'))
        return invalid("链接为空、过长或包含无效字符。");

    const QUrl url(text, QUrl::StrictMode);
    const auto scheme = url.scheme().toLower();
    if (scheme == "http" || scheme == "https" || scheme == "ftp") {
        if (!url.isValid() || url.host().isEmpty() || url.port() == 0)
            return invalid("下载链接的地址或端口无效。");
        const auto normalized = url.adjusted(QUrl::RemoveFragment);
        const auto source = normalized.toString(QUrl::FullyEncoded);
        return DownloadRequest{scheme == "ftp" ? Protocol::Ftp : Protocol::Http,
            source, QFileInfo(url.path()).fileName(), {source}, std::nullopt};
    }

    if (scheme == "magnet") {
        if (!url.isValid() || !url.host().isEmpty() || !url.path().isEmpty())
            return invalid("磁力链接格式无效。");
        const QUrlQuery query(url);
        QStringList identities;
        for (const auto& topic : query.allQueryItemValues("xt", QUrl::FullyDecoded)) {
            if (topic.startsWith("urn:btih:", Qt::CaseInsensitive)) {
                const auto hash = topic.mid(9);
                if (!matches(hash, "\\A(?:[0-9a-fA-F]{40}|[a-zA-Z2-7]{32})\\z"))
                    return invalid("磁力链接的 BT 哈希无效。");
                // Base32 and hexadecimal represent the same 20-byte identity.
                QByteArray bytes;
                if (hash.size() == 40) bytes = QByteArray::fromHex(hash.toLatin1());
                else {
                    quint32 accumulator = 0;
                    int bits = 0;
                    for (const auto ch : hash.toUpper()) {
                        const int value = QStringLiteral("ABCDEFGHIJKLMNOPQRSTUVWXYZ234567").indexOf(ch);
                        accumulator = (accumulator << 5) | static_cast<quint32>(value);
                        bits += 5;
                        if (bits >= 8) {
                            bits -= 8;
                            bytes.append(static_cast<char>((accumulator >> bits) & 255));
                        }
                    }
                }
                identities.append("btih:" + QString::fromLatin1(bytes.toHex()));
            } else if (topic.startsWith("urn:btmh:", Qt::CaseInsensitive)) {
                const auto hash = topic.mid(9);
                if (!matches(hash, "\\A1220[0-9a-fA-F]{64}\\z"))
                    return invalid("磁力链接的 BT v2 哈希无效。");
                identities.append("btmh:" + hash.toLower());
            }
        }
        identities.removeDuplicates();
        if (identities.isEmpty()) return invalid("磁力链接缺少支持的 BT 哈希。");
        return DownloadRequest{Protocol::BitTorrent, text,
            query.queryItemValue("dn", QUrl::FullyDecoded), identities, std::nullopt};
    }

    if (text.startsWith("ed2k://", Qt::CaseInsensitive)) {
        const auto fields = text.mid(7).split('|');
        if (fields.size() < 6 || !fields[0].isEmpty() || fields[1] != "file"
            || fields.last() != "/" || fields[2].isEmpty()
            || !matches(fields[3], "\\A[0-9]+\\z")
            || !matches(fields[4], "\\A[0-9a-fA-F]{32}\\z"))
            return invalid("ed2k 文件链接格式无效。");
        bool ok = false;
        const auto size = fields[3].toULongLong(&ok);
        if (!ok) return invalid("ed2k 文件大小超出范围。");
        if (QRegularExpression(QStringLiteral("%(?![0-9a-fA-F]{2})")).match(fields[2]).hasMatch())
            return invalid("ed2k 文件名编码无效。");
        const auto name = QUrl::fromPercentEncoding(fields[2].toUtf8());
        if (name.contains(QChar::Null)) return invalid("ed2k 文件名无效。");
        return DownloadRequest{Protocol::Ed2k, text, name,
            {"ed2k:" + fields[4].toLower() + ':' + QString::number(size)}, size};
    }

    // Local metainfo still needs bencode validation by the BT backend.
    const auto path = url.isLocalFile() ? url.toLocalFile() : text;
    const QFileInfo file(path);
    if ((url.isLocalFile() || QDir::isAbsolutePath(text)) && file.isFile()
        && file.suffix().compare("torrent", Qt::CaseInsensitive) == 0) {
        const auto canonical = file.canonicalFilePath();
        return DownloadRequest{Protocol::BitTorrent, canonical,
            file.fileName(), {}, std::nullopt};
    }
    return invalid("支持 HTTP、HTTPS、FTP、磁力链、ed2k 和本地种子文件。");
}
} // namespace downloader
