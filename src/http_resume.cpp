#include "http_resume.h"
#include "bandwidth_limiter.h"
#include "curl_runtime.h"
#include "disk_space.h"
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QRegularExpression>
#include <QSaveFile>
#include <limits>
#include <memory>

namespace downloader {
namespace {
bool validId(const QString& id)
{
    return QRegularExpression(QStringLiteral("\\A[0-9a-f]{32}\\z")).match(id).hasMatch();
}
struct Manifest {
    QString source, destination, phase = "partial";
    QByteArray etag, sha256;
    qint64 bytes = 0, total = -1;
    QString resolved;
};
bool saveManifest(const QString& path, const Manifest& value)
{
    const auto bytes = QJsonDocument(QJsonObject{{"version", 1}, {"source", value.source},
        {"destination", value.destination}, {"phase", value.phase}, {"resolved", value.resolved},
        {"etag", QString::fromLatin1(value.etag)}, {"sha256", QString::fromLatin1(value.sha256)},
        {"bytes", QString::number(value.bytes)}, {"total", QString::number(value.total)}}).toJson(QJsonDocument::Compact);
    QSaveFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
}
std::optional<Manifest> loadManifest(const QString& path, const QString& source, const QString& destination)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 128 * 1024) return std::nullopt;
    const auto object = QJsonDocument::fromJson(file.readAll()).object();
    if (object.value("version").toInt() != 1 || object.value("source").toString() != source
        || object.value("destination").toString() != destination) return std::nullopt;
    bool bytesValid = false, totalValid = false;
    const auto bytes = object.value("bytes").toString().toLongLong(&bytesValid);
    const auto total = object.value("total").toString().toLongLong(&totalValid);
    const auto phase = object.value("phase").toString();
    const auto hash = object.value("sha256").toString().toLatin1();
    if (!bytesValid || !totalValid || bytes < 0 || total < -1 || (total >= 0 && bytes > total)
        || (phase != "partial" && phase != "downloaded" && phase != "ready") || hash.size() != 64) return std::nullopt;
    return Manifest{source, destination, phase, object.value("etag").toString().toLatin1(), hash, bytes, total,
        object.value("resolved").toString()};
}
bool strongEtag(const QByteArray& value)
{
    if (value.size() < 2 || !value.startsWith('"') || !value.endsWith('"')) return false;
    for (qsizetype i = 1; i < value.size() - 1; ++i) {
        const auto c = static_cast<unsigned char>(value[i]);
        if (c < 0x21 || c == 0x22 || c == 0x7f) return false;
    }
    return true;
}
bool hashPrefix(QFile& file, qint64 count, QCryptographicHash& hash, const std::atomic_bool& stopped)
{
    if (!file.seek(0)) return false;
    while (count > 0 && !stopped.load()) {
        const auto data = file.read(qMin<qint64>(count, 1024 * 1024));
        if (data.isEmpty()) return false;
        hash.addData(data); count -= data.size();
    }
    return count == 0;
}
TransferResult failure(TransferError error, const QString& message, qint64 received = 0, long response = 0)
{
    return {TransferStatus::Failed, error, message, response, received};
}
struct Context {
    QFile& output;
    QString manifestPath;
    Manifest manifest;
    QCryptographicHash& hash;
    const std::atomic_bool& stopped;
    const std::function<void(TransferProgress)>& progress;
    QElapsedTimer checkpointClock;
    QMap<QByteArray, QByteArray> headers;
    qint64 offset = 0, expectedEnd = -1, checkpointBytes = 0;
    qsizetype headerBytes = 0;
    long status = 0;
    bool ready = false, completeRange = false, diskError = false, headerError = false, callbackError = false;
    QString message;
    CURL* handle = nullptr;
    BandwidthLimiter* limiter=nullptr;
    QElapsedTimer spaceClock;
    qint64 spaceCheckedAt=0;
    bool checkpoint(const QString& phase = "partial") {
        if (!output.flush()) { diskError = true; message = output.errorString(); return false; }
        auto next = manifest;
        next.phase = phase; next.bytes = output.pos(); next.sha256 = hash.result().toHex();
        if (!saveManifest(manifestPath, next)) {
            diskError = true; message = QStringLiteral("无法保存下载恢复信息。"); return false;
        }
        manifest = next; checkpointBytes = next.bytes; checkpointClock.restart(); return true;
    }
    bool reject(const QString& reason) { headerError = true; message = reason; return false; }
    bool checkSpace(qint64 required) {
        const auto shortage = insufficientDiskSpace(output.fileName(), required);
        if (shortage.isEmpty()) return true;
        diskError = true; message = shortage; return false;
    }
    bool acceptHeaders() {
        char* effective = nullptr;
        curl_easy_getinfo(handle, CURLINFO_EFFECTIVE_URL, &effective);
        const auto resolved = effective ? QString::fromUtf8(effective) : QString{};
        if (status == 416 && offset > 0) {
            const auto match = QRegularExpression(QStringLiteral("\\Abytes \\*/([0-9]+)\\z"))
                .match(QString::fromLatin1(headers.value("content-range")));
            bool ok = false; const auto total = match.captured(1).toLongLong(&ok);
            completeRange = match.hasMatch() && ok && total == offset && total == manifest.total
                && strongEtag(manifest.etag) && headers.value("etag") == manifest.etag && resolved == manifest.resolved;
            return true;
        }
        if (status < 200 || status >= 300) return true;
        const auto encoding = headers.value("content-encoding").toLower();
        if (!encoding.isEmpty() && encoding != "identity")
            return reject(QStringLiteral("服务器返回了无法安全续传的内容编码。"));
        qint64 length = -1;
        if (headers.contains("content-length")) {
            bool ok = false; length = headers.value("content-length").toLongLong(&ok);
            if (!ok || length < 0) return reject(QStringLiteral("服务器文件长度无效。"));
        }
        if (status == 206) {
            const auto match = QRegularExpression(QStringLiteral("\\Abytes ([0-9]+)-([0-9]+)/([0-9]+)\\z"))
                .match(QString::fromLatin1(headers.value("content-range")));
            bool startOk = false, endOk = false, totalOk = false;
            const auto start = match.captured(1).toLongLong(&startOk);
            const auto end = match.captured(2).toLongLong(&endOk);
            const auto total = match.captured(3).toLongLong(&totalOk);
            if (offset <= 0 || !match.hasMatch() || !startOk || !endOk || !totalOk || start != offset
                || end < start || end >= total || end != total - 1 || (length >= 0 && length != end - start + 1)
                || (manifest.total >= 0 && total != manifest.total) || headers.value("etag") != manifest.etag
                || resolved != manifest.resolved)
                return reject(QStringLiteral("服务器续传范围或资源标识不一致，请从头重试。"));
            expectedEnd = total;
            if (!checkSpace(total - offset)) return false;
        } else if (status == 200 || status == 204) {
            // Preserve the existing checkpoint on rejection. Do not credit its
            // logical length as free space: sparse/compressed allocation differs.
            if (!checkSpace(length)) return false;
            // If-Range changed or Range ignored: replace the prefix before any body.
            if (!output.resize(0) || !output.seek(0)) { diskError = true; message = output.errorString(); return false; }
            hash.reset(); offset = 0; expectedEnd = length;
            manifest.etag = headers.value("etag");
            manifest.resolved = resolved;
        } else return reject(QStringLiteral("服务器未返回完整文件或有效的续传响应。"));
        manifest.total = expectedEnd;
        ready = checkpoint();
        return ready;
    }
};
size_t receiveHeader(char* data, size_t size, size_t count, void* user) noexcept
{
    auto& state = *static_cast<Context*>(user);
    try {
        if (state.stopped.load() || (count && size > 65536 / count)) return 0;
        const auto length = static_cast<qsizetype>(size * count);
        const auto line = QByteArray(data, length).trimmed();
        if (line.startsWith("HTTP/")) {
            state.headers.clear(); state.headerBytes = 0; state.ready = false; state.completeRange = false;
            bool ok = false; state.status = line.split(' ').value(1).toLong(&ok);
            if (!ok) return 0;
        } else if (line.isEmpty()) {
            // A trailer must not reset/truncate the already written response.
            if (!state.ready && !state.acceptHeaders()) return 0;
        } else {
            if (state.ready) return static_cast<size_t>(length);
            state.headerBytes += length;
            if (state.headerBytes > 65536) return 0;
            const auto separator = line.indexOf(':');
            if (separator > 0) {
                const auto name = line.first(separator).toLower();
                const auto value = line.mid(separator + 1).trimmed();
                if ((name == "etag" || name == "content-range" || name == "content-length")
                    && state.headers.contains(name) && state.headers.value(name) != value)
                {
                    state.reject(QStringLiteral("服务器返回了相互冲突的下载信息。")); return 0;
                }
                state.headers[name] = value;
            }
        }
        return static_cast<size_t>(length);
    } catch (...) { state.callbackError = true; return 0; }
}
size_t receiveBody(char* data, size_t size, size_t count, void* user) noexcept
{
    auto& state = *static_cast<Context*>(user);
    try {
        if (state.stopped.load() || (count && size > static_cast<size_t>((std::numeric_limits<qint64>::max)()) / count)) return CURL_WRITEFUNC_ERROR;
        const auto length = static_cast<qint64>(size * count);
        if (state.status >= 300 && state.status < 400) return size * count;
        if (!state.ready || (state.expectedEnd >= 0 && length > state.expectedEnd - state.output.pos())) return CURL_WRITEFUNC_ERROR;
        if(state.limiter&&!state.limiter->consume(length,state.stopped))return CURL_WRITEFUNC_ERROR;
        if(!state.spaceClock.isValid()||state.output.pos()-state.spaceCheckedAt>=4*1024*1024||state.spaceClock.elapsed()>=1000){
            state.spaceCheckedAt=state.output.pos();state.spaceClock.start();
            if(!state.checkSpace(state.expectedEnd<0?qMax<qint64>(length,64*1024):state.expectedEnd-state.output.pos()))
                return CURL_WRITEFUNC_ERROR;
        }
        qint64 written = 0;
        while (written < length) {
            const auto bytes = state.output.write(data + written, length - written);
            if (bytes <= 0) { state.diskError = true; state.message = state.output.errorString(); return CURL_WRITEFUNC_ERROR; }
            state.hash.addData(QByteArrayView(data + written, bytes)); written += bytes;
        }
        if (state.output.pos() - state.checkpointBytes >= 4 * 1024 * 1024 || state.checkpointClock.elapsed() >= 1000)
            if (!state.checkpoint()) return CURL_WRITEFUNC_ERROR;
        return size * count;
    } catch (...) { state.callbackError = true; return CURL_WRITEFUNC_ERROR; }
}
int progressCallback(void* user, curl_off_t total, curl_off_t now, curl_off_t, curl_off_t) noexcept
{
    auto& state = *static_cast<Context*>(user);
    if (state.stopped.load()) return 1;
    try {
        if (state.progress && state.ready) state.progress({state.output.pos(), state.expectedEnd >= 0 ? state.expectedEnd : total > 0 ? state.offset + total : 0});
    } catch (...) { state.callbackError = true; return 1; }
    Q_UNUSED(now);
    return state.stopped.load() ? 1 : 0;
}
TransferResult publish(QFile& output, const QString& metadataPath, Manifest manifest, const QString& destination,
    QCryptographicHash& hash, const std::atomic_bool& stopped)
{
    if (stopped.load()) return {TransferStatus::Cancelled, TransferError::None, {}, 0, output.size()};
    if (!output.flush()) return failure(TransferError::Disk, output.errorString(), output.size());
    manifest.phase = "ready"; manifest.bytes = output.size(); manifest.total = output.size(); manifest.sha256 = hash.result().toHex();
    if (!saveManifest(metadataPath, manifest)) return failure(TransferError::Disk, QStringLiteral("无法保存文件发布记录。"), output.size());
    const auto part = output.fileName(); const auto bytes = output.size();
    output.close();
    if (!output.rename(destination)) return failure(TransferError::Destination, output.errorString(), bytes);
    output.setFileName(part);
    // Keep 'ready' until the database has committed Completed.
    return {TransferStatus::Completed, TransferError::None, {}, 200, bytes};
}
}
QString partialPath(const QString& destination, const QString& taskId)
{
    return validId(taskId) ? destination + "." + taskId + ".part" : QString{};
}
bool discardPartial(const QString& destination, const QString& taskId)
{
    const auto path = partialPath(destination, taskId);
    if (path.isEmpty()) return false;
    bool ok = true;
    for (const auto& file : {path, path + ".json"}) if (QFileInfo::exists(file) && !QFile::remove(file)) ok = false;
    return ok;
}
void acknowledgePublished(const QString& destination, const QString& taskId)
{
    const auto part = partialPath(destination, taskId);
    if (!part.isEmpty()) QFile::remove(part + ".json");
}
TransferResult transferResumable(const DownloadRequest& request, const QString& destination,
    const QString& taskId, const std::atomic_bool& stopped, const std::function<void(TransferProgress)>& progress,BandwidthLimiter* limiter)
{
    if (request.protocol != Protocol::Http) return transfer(request, destination, stopped, progress,limiter);
    const auto parsed = parseRequest(request.source);
    const auto* checked = std::get_if<DownloadRequest>(&parsed);
    if (!checked || checked->protocol != Protocol::Http || !validId(taskId) || !QFileInfo(destination).isAbsolute())
        return failure(TransferError::InvalidRequest, QStringLiteral("下载任务或恢复标识无效。"));
    if (detail::initializeCurl() != CURLE_OK) return failure(TransferError::Internal, QStringLiteral("无法初始化网络组件。"));
    const auto part = partialPath(destination, taskId);
    const auto metadata = part + ".json";
    auto saved = loadManifest(metadata, checked->source, destination);
    if (stopped.load()) return {TransferStatus::Cancelled};
    if (QFileInfo::exists(destination)) {
        // Recover a crash between rename and the database completion commit.
        if (saved && saved->phase == "ready") {
            QFile target(destination); QCryptographicHash hash(QCryptographicHash::Sha256);
            if (target.open(QIODevice::ReadOnly) && target.size() == saved->bytes
                && hashPrefix(target, saved->bytes, hash, stopped) && hash.result().toHex() == saved->sha256)
                return {TransferStatus::Completed, TransferError::None, {}, 200, saved->bytes};
        }
        if (stopped.load()) return {TransferStatus::Cancelled};
        return failure(TransferError::Destination, QStringLiteral("目标文件已存在，未覆盖。"));
    }
    const bool exists = QFileInfo::exists(part);
    QFile output(part);
    if (!output.open(exists ? QIODevice::ReadWrite : QIODevice::ReadWrite | QIODevice::NewOnly))
        return failure(TransferError::Destination, output.errorString());
    QCryptographicHash hash(QCryptographicHash::Sha256);
    bool verified = saved && output.size() >= saved->bytes && hashPrefix(output, saved->bytes, hash, stopped)
        && hash.result().toHex() == saved->sha256;
    if (stopped.load()) return {TransferStatus::Cancelled, TransferError::None, {}, 0, saved ? saved->bytes : 0};
    if (verified && (saved->phase == "downloaded" || saved->phase == "ready") && saved->bytes == saved->total) {
        if (!output.resize(saved->bytes) || !output.seek(saved->bytes)) return failure(TransferError::Disk, output.errorString());
        return publish(output, metadata, *saved, destination, hash, stopped);
    }
    qint64 offset = verified && strongEtag(saved->etag) && !saved->resolved.isEmpty() ? saved->bytes : 0;
    Manifest manifest = verified ? *saved : Manifest{checked->source, destination};
    if (!output.resize(offset) || !output.seek(offset)) return failure(TransferError::Disk, output.errorString());
    if (!offset) { hash.reset(); manifest = Manifest{checked->source, destination}; }
    Context context{output, metadata, manifest, hash, stopped, progress};
    context.limiter=limiter;
    context.offset = offset; context.checkpointBytes = offset; context.checkpointClock.start();
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> handle(curl_easy_init(), curl_easy_cleanup);
    if (!handle) return failure(TransferError::Internal, QStringLiteral("无法创建下载连接。"));
    context.handle = handle.get();
    CURLcode options = CURLE_OK;
    auto set = [&](CURLoption option, auto value) { if (options == CURLE_OK) options = curl_easy_setopt(handle.get(), option, value); };
    const auto source = checked->source.toUtf8();
    set(CURLOPT_URL, source.constData()); set(CURLOPT_PROTOCOLS_STR, "http,https");
    set(CURLOPT_REDIR_PROTOCOLS_STR, QUrl(checked->source).scheme() == "https" ? "https" : "http,https");
    set(CURLOPT_FOLLOWLOCATION, 1L); set(CURLOPT_MAXREDIRS, 10L); set(CURLOPT_FAILONERROR, 1L);
    set(CURLOPT_PROXY, ""); set(CURLOPT_NOSIGNAL, 1L); set(CURLOPT_CONNECTTIMEOUT, 30L);
    set(CURLOPT_LOW_SPEED_LIMIT, 1L); set(CURLOPT_LOW_SPEED_TIME, 60L);
    set(CURLOPT_USERAGENT, "NativeDownloader/0.1"); set(CURLOPT_NOPROGRESS, 0L);
    set(CURLOPT_WRITEFUNCTION, &receiveBody); set(CURLOPT_WRITEDATA, &context);
    set(CURLOPT_HEADERFUNCTION, &receiveHeader); set(CURLOPT_HEADERDATA, &context);
    set(CURLOPT_XFERINFOFUNCTION, &progressCallback); set(CURLOPT_XFERINFODATA, &context);
    curl_slist* rawHeaders = curl_slist_append(nullptr, "Accept-Encoding: identity");
    if (!rawHeaders) return failure(TransferError::Internal, QStringLiteral("无法创建请求信息。"));
    std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headers(rawHeaders, curl_slist_free_all);
    const auto range = QByteArray::number(offset) + '-';
    if (offset > 0) {
        const auto condition = "If-Range: " + manifest.etag;
        const auto appended = curl_slist_append(headers.get(), condition.constData());
        if (!appended) return failure(TransferError::Internal, QStringLiteral("无法创建续传请求。"));
        headers.release(); headers.reset(appended);
        set(CURLOPT_RANGE, range.constData());
    }
    set(CURLOPT_HTTPHEADER, headers.get());
    if (options != CURLE_OK) return failure(TransferError::Internal, QString::fromLatin1(curl_easy_strerror(options)));
    const auto code = curl_easy_perform(handle.get());
    long response = 0; curl_easy_getinfo(handle.get(), CURLINFO_RESPONSE_CODE, &response);
    if (context.ready && !context.diskError) context.checkpoint();
    const auto bytes = output.pos();
    if (context.diskError) return failure(TransferError::Disk, context.message, bytes, response);
    if (stopped.load()) return {TransferStatus::Cancelled, TransferError::None, {}, response, bytes};
    if (context.headerError) return failure(TransferError::ResourceChanged, context.message, bytes, response);
    if (context.callbackError) return failure(TransferError::Internal, QStringLiteral("处理下载数据失败。"), bytes, response);
    if (code != CURLE_OK && !context.completeRange) {
        const auto error = code == CURLE_HTTP_RETURNED_ERROR ? TransferError::Http
            : code == CURLE_PEER_FAILED_VERIFICATION || code == CURLE_SSL_CONNECT_ERROR ? TransferError::Tls : TransferError::Network;
        return failure(error, QString::fromLatin1(curl_easy_strerror(code)), bytes, response);
    }
    if ((!context.ready && !context.completeRange) || (context.expectedEnd >= 0 && bytes != context.expectedEnd))
        return failure(TransferError::Http, QStringLiteral("服务器未返回完整且可验证的文件。"), bytes, response);
    if (!context.checkpoint("downloaded")) return failure(TransferError::Disk, context.message, bytes, response);
    auto result = publish(output, metadata, context.manifest, destination, hash, stopped);
    result.responseCode = response;
    return result;
}
}
