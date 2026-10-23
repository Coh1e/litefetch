#include "download_request.h"
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTextStream>
#include <limits>

using namespace downloader;

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    int failures = 0;
    auto check = [&](bool condition, const char* description) {
        if (!condition) { QTextStream(stderr) << "FAIL: " << description << '\n'; ++failures; }
    };
    auto request = [](const QString& text) { return std::get<DownloadRequest>(parseRequest(text)); };
    auto rejected = [](const QString& text) { return std::holds_alternative<InvalidRequest>(parseRequest(text)); };

    const auto http = request(" HTTPS://EXAMPLE.org/a%20b.zip?token=b%2Fa&x=1#section ");
    check(http.protocol == Protocol::Http, "HTTPS classification");
    check(http.source == "https://example.org/a%20b.zip?token=b%2Fa&x=1", "preserve signed query encoding and order; remove fragment");
    check(http.suggestedName == "a b.zip", "decode suggested name once");
    check(request("ftp://user:password@example.org/file").protocol == Protocol::Ftp, "authenticated FTP");
    for (const auto* bad : {"", "https://", "https://host:0/a", "https://host:99999/a",
             "https://host/%XX", "https://host/a\r\nCookie: value", "javascript:alert(1)", "blob:https://host/id"})
        check(rejected(QString::fromLatin1(bad)), bad);

    const auto hex = request("magnet:?xt=urn:btih:0000000000000000000000000000000000000000&dn=a%20b");
    const auto base32 = request("magnet:?xt=urn:btih:AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA");
    check(hex.identities == base32.identities, "base32 and hex deduplicate to one identity");
    check(hex.suggestedName == "a b", "magnet display name");
    const auto hybrid = request("magnet:?xt=urn:btih:0000000000000000000000000000000000000000&xt=urn:btmh:1220" + QString(64, 'a'));
    check(hybrid.identities.size() == 2, "hybrid torrent retains both identities");
    for (const auto* bad : {"magnet:?dn=nohash", "magnet:?xt=urn:btih:BAD", "magnet://host/?xt=urn:btih:AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"})
        check(rejected(QString::fromLatin1(bad)), bad);

    const QString ed2kHash = "0123456789ABCDEF0123456789ABCDEF";
    const auto ed2k = request("ed2k://|file|%E4%B8%AD%20a.zip|18446744073709551615|" + ed2kHash + "|/");
    check(ed2k.expectedSize == std::numeric_limits<quint64>::max(), "64-bit unsigned file size");
    check(ed2k.suggestedName == QString::fromUtf8("中 a.zip"), "Unicode ed2k filename");
    check(ed2k.identities[0].contains(ed2kHash.toLower()), "canonical ed2k hash");
    for (const auto& bad : {QString("ed2k://|file|name|18446744073709551616|" + ed2kHash + "|/"),
             QString("ed2k://|file|%00|-1|" + ed2kHash + "|/"),
             QString("ed2k://|file|bad%XX|1|" + ed2kHash + "|/"),
             QString("ed2k://|server|name|1|" + ed2kHash + "|/")})
        check(rejected(bad), "invalid ed2k input");

    QTemporaryDir directory(QCoreApplication::applicationDirPath() + "/request-tests-XXXXXX");
    check(directory.isValid(), "temporary fixture directory");
    const auto metainfo = directory.filePath("sample.TORRENT");
    QFile file(metainfo);
    check(file.open(QIODevice::WriteOnly), "create metainfo path fixture");
    file.close();
    check(request(QUrl::fromLocalFile(metainfo).toString()).source == QFileInfo(metainfo).canonicalFilePath(), "local file URL");
    check(rejected(directory.filePath("missing.torrent")), "missing metainfo");
    QTextStream(stdout) << "Download request checks: " << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}
