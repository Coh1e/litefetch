#include "transfer.h"
#include "curl_runtime.h"
#include "bandwidth_limiter.h"
#include "disk_space.h"
#include <QFile>
#include <QFileInfo>
#include <QElapsedTimer>
#include <QUuid>
#include <curl/curl.h>
#include <limits>
#include <memory>

namespace downloader {
namespace {
struct Context {
    QFile& output;
    const std::atomic_bool& cancelled;
    const std::function<void(TransferProgress)>& progress;
    qint64 received = 0;
    bool diskError = false;
    bool callbackError = false;
    BandwidthLimiter* limiter=nullptr;
    CURL* handle=nullptr;
    qint64 checkedTotal=-1;
    QString diskMessage;
    QElapsedTimer spaceClock;
    qint64 spaceCheckedAt=0;
    bool checkSpace(qint64 total) {
        if(total==checkedTotal&&spaceClock.isValid()&&received-spaceCheckedAt<4*1024*1024
            &&spaceClock.elapsed()<1000)return true;
        checkedTotal=total;
        spaceCheckedAt=received;spaceClock.start();
        diskMessage=insufficientDiskSpace(output.fileName(),total<0?64*1024:qMax<qint64>(0,total-received));
        if(diskMessage.isEmpty())return true;
        diskError=true;return false;
    }
};
size_t writeData(char* data, size_t size, size_t count, void* user) noexcept
{
    auto& state = *static_cast<Context*>(user);
    if (state.cancelled.load()) return CURL_WRITEFUNC_ERROR;
    if (count && size > static_cast<size_t>((std::numeric_limits<qint64>::max)()) / count)
        return CURL_WRITEFUNC_ERROR;
    const auto length = static_cast<qint64>(size * count);
    try{
        curl_off_t total=-1;
        if(curl_easy_getinfo(state.handle,CURLINFO_CONTENT_LENGTH_DOWNLOAD_T,&total)==CURLE_OK
            &&!state.checkSpace(total))return CURL_WRITEFUNC_ERROR;
        if(state.limiter&&!state.limiter->consume(length,state.cancelled))return CURL_WRITEFUNC_ERROR;
    }
    catch(...){state.callbackError=true;return CURL_WRITEFUNC_ERROR;}
    qint64 written = 0;
    while (written < length) {
        const auto bytes = state.output.write(data + written, length - written);
        if (bytes <= 0) { state.diskError = true; return CURL_WRITEFUNC_ERROR; }
        written += bytes;
    }
    state.received += length;
    return size * count;
}
int reportProgress(void* user, curl_off_t total, curl_off_t now, curl_off_t, curl_off_t) noexcept
{
    auto& state = *static_cast<Context*>(user);
    if (state.cancelled.load()) return 1;
    try {
        if (state.progress) state.progress({now, total});
    } catch (...) {
        state.callbackError = true;
        return 1;
    }
    return state.cancelled.load() ? 1 : 0;
}
TransferResult failure(TransferError error, QString message)
{
    return {TransferStatus::Failed, error, std::move(message)};
}
} // namespace

TransferResult transfer(const DownloadRequest& request, const QString& destination,
    const std::atomic_bool& cancelled, const std::function<void(TransferProgress)>& progress,BandwidthLimiter* limiter)
{
    if (request.protocol != Protocol::Http && request.protocol != Protocol::Ftp)
        return failure(TransferError::InvalidRequest, QStringLiteral("该任务需要对应的 P2P 下载核心。"));
    const auto parsed = parseRequest(request.source);
    const auto* validated = std::get_if<DownloadRequest>(&parsed);
    if (!validated || validated->protocol != request.protocol)
        return failure(TransferError::InvalidRequest, QStringLiteral("下载地址无效。"));
    if (cancelled.load()) return {TransferStatus::Cancelled};
    const QFileInfo target(destination);
    if (!target.isAbsolute() || target.fileName().isEmpty() || target.exists())
        return failure(TransferError::Destination, QStringLiteral("请选择尚不存在的目标文件。"));
    if (detail::initializeCurl() != CURLE_OK)
        return failure(TransferError::Internal, QStringLiteral("无法初始化网络组件。"));
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> handle(curl_easy_init(), curl_easy_cleanup);
    if (!handle) return failure(TransferError::Internal, QStringLiteral("无法创建下载连接。"));
    const auto temporary = destination + "." + QUuid::createUuid().toString(QUuid::Id128) + ".part";
    QFile output(temporary);
    if (!output.open(QIODevice::WriteOnly | QIODevice::NewOnly))
        return failure(TransferError::Destination, output.errorString());
    // All early exits after creating the staging file clean it up.
    struct Cleanup { QFile& file; ~Cleanup() { file.close(); file.remove(); } } cleanup{output};
    Context context{output, cancelled, progress};
    context.limiter=limiter;
    context.handle=handle.get();
    CURLcode optionResult = CURLE_OK;
    auto set = [&](CURLoption option, auto value) {
        if (optionResult == CURLE_OK) optionResult = curl_easy_setopt(handle.get(), option, value);
    };
    const auto source = validated->source.toUtf8();
    set(CURLOPT_URL, source.constData());
    set(CURLOPT_PROTOCOLS_STR, "http,https,ftp");
    set(CURLOPT_REDIR_PROTOCOLS_STR, QUrl(validated->source).scheme() == "https" ? "https" : "http,https");
    set(CURLOPT_FOLLOWLOCATION, 1L);
    set(CURLOPT_MAXREDIRS, 10L);
    set(CURLOPT_FAILONERROR, 1L);
    set(CURLOPT_NOSIGNAL, 1L);
    set(CURLOPT_CONNECTTIMEOUT, 30L);
    set(CURLOPT_LOW_SPEED_LIMIT, 1L);
    set(CURLOPT_LOW_SPEED_TIME, 60L);
    // Explicit proxy policy will be added by the task manager; ignore process env.
    set(CURLOPT_PROXY, "");
    set(CURLOPT_WRITEFUNCTION, &writeData);
    set(CURLOPT_WRITEDATA, &context);
    set(CURLOPT_NOPROGRESS, 0L);
    set(CURLOPT_XFERINFOFUNCTION, &reportProgress);
    set(CURLOPT_XFERINFODATA, &context);
    set(CURLOPT_USERAGENT, "NativeDownloader/0.1");
    if (optionResult != CURLE_OK)
        return failure(TransferError::Internal, QString::fromLatin1(curl_easy_strerror(optionResult)));
    const auto code = curl_easy_perform(handle.get());
    long response = 0;
    curl_easy_getinfo(handle.get(), CURLINFO_RESPONSE_CODE, &response);
    TransferResult result{TransferStatus::Failed, TransferError::Network, {}, response, context.received};
    if (cancelled.load()) { result.status = TransferStatus::Cancelled; result.error = TransferError::None; return result; }
    if (context.diskError) { result.error = TransferError::Disk; result.message = context.diskMessage.isEmpty()?output.errorString():context.diskMessage; return result; }
    if (context.callbackError) { result.error = TransferError::Internal; result.message = QStringLiteral("进度接收失败。"); return result; }
    if (code != CURLE_OK) {
        if (code == CURLE_HTTP_RETURNED_ERROR) result.error = TransferError::Http;
        if (code == CURLE_PEER_FAILED_VERIFICATION || code == CURLE_SSL_CONNECT_ERROR) result.error = TransferError::Tls;
        result.message = QString::fromLatin1(curl_easy_strerror(code));
        return result;
    }
    if (request.protocol == Protocol::Http && (response < 200 || response >= 300)) {
        result.error = TransferError::Http;
        result.message = QStringLiteral("服务器未返回文件内容（HTTP %1）。").arg(response);
        return result;
    }
    if (!output.flush()) { result.error = TransferError::Disk; result.message = output.errorString(); return result; }
    output.close();
    // QFile::rename refuses to replace an existing destination, including a race.
    if (!output.rename(destination)) {
        result.error = TransferError::Destination; result.message = output.errorString(); return result;
    }
    // Cleanup must never remove the successfully published destination.
    output.setFileName(temporary);
    result.status = TransferStatus::Completed;
    result.error = TransferError::None;
    return result;
}
}
