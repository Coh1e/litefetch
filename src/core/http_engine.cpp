#include "http_engine.h"

#include <curl/curl.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <array>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <utility>

namespace litefetch {
namespace {
CURLcode initializeCurl()
{
    static const CURLcode result = curl_global_init(CURL_GLOBAL_DEFAULT);
    return result;
}

bool isHttpUrl(const std::string& source)
{
    CURLU* url = curl_url();
    if (!url) return false;
    const bool parsed = curl_url_set(url, CURLUPART_URL, source.c_str(), 0) == CURLUE_OK;
    char* scheme = nullptr;
    const bool supported = parsed
        && curl_url_get(url, CURLUPART_SCHEME, &scheme, 0) == CURLUE_OK
        && scheme
        && (std::string(scheme) == "http" || std::string(scheme) == "https");
    if (scheme) curl_free(scheme);
    curl_url_cleanup(url);
    return supported;
}

struct Context {
    std::ofstream& output;
    const std::atomic_bool& cancelled;
    const std::function<void(const TransferProgressInfo&)>& progress;
    std::int64_t received = 0;
    bool diskError = false;
    bool callbackError = false;
    std::string diskMessage;
};

std::size_t writeData(char* data, std::size_t size, std::size_t count, void* user) noexcept
{
    auto& context = *static_cast<Context*>(user);
    if (context.cancelled.load()) return CURL_WRITEFUNC_ERROR;
    if (count != 0 && size > (std::numeric_limits<std::size_t>::max)() / count)
        return CURL_WRITEFUNC_ERROR;
    const std::size_t length = size * count;
    if (length > static_cast<std::size_t>((std::numeric_limits<std::streamsize>::max)()))
        return CURL_WRITEFUNC_ERROR;
    try {
        context.output.write(data, static_cast<std::streamsize>(length));
        if (!context.output) {
            context.diskError = true;
            context.diskMessage = "cannot write destination";
            return CURL_WRITEFUNC_ERROR;
        }
    } catch (const std::exception& error) {
        context.diskError = true;
        context.diskMessage = error.what();
        return CURL_WRITEFUNC_ERROR;
    } catch (...) {
        context.diskError = true;
        context.diskMessage = "cannot write destination";
        return CURL_WRITEFUNC_ERROR;
    }
    context.received += static_cast<std::int64_t>(length);
    return length;
}

int reportProgress(void* user, curl_off_t total, curl_off_t now, curl_off_t, curl_off_t) noexcept
{
    auto& context = *static_cast<Context*>(user);
    if (context.cancelled.load()) return 1;
    try {
        if (context.progress)
            context.progress(TransferProgressInfo{static_cast<std::int64_t>(now),
                total >= 0 ? static_cast<std::int64_t>(total) : -1});
    } catch (...) {
        context.callbackError = true;
        return 1;
    }
    return context.cancelled.load() ? 1 : 0;
}

TransferResult failure(TransferError error, std::string message)
{
    TransferResult result;
    result.status = TransferStatus::Failed;
    result.error = error;
    result.message = std::move(message);
    return result;
}

std::string curlMessage(CURLcode code, const std::array<char, CURL_ERROR_SIZE>& buffer)
{
    if (buffer[0] != '\0') return buffer.data();
    return curl_easy_strerror(code);
}
} // namespace

TransferResult transferHttp(const TransferRequest& request, const std::atomic_bool& cancelled,
    const std::function<void(const TransferProgressInfo&)>& progress)
{
    if (!isHttpUrl(request.source))
        return failure(TransferError::InvalidRequest, "source is not an HTTP(S) URL");
    const std::filesystem::path destination(request.destination);
    if (!destination.is_absolute() || destination.filename().empty())
        return failure(TransferError::Destination, "destination must be an absolute file path");
    std::error_code existsError;
    if (std::filesystem::exists(destination, existsError))
        return failure(TransferError::Destination, "destination already exists");
    if (existsError) return failure(TransferError::Destination, "cannot inspect destination");
    const auto parent = destination.parent_path();
    if (!std::filesystem::is_directory(parent, existsError))
        return failure(TransferError::Destination, "destination directory does not exist");
    if (cancelled.load()) {
        TransferResult result;
        result.status = TransferStatus::Cancelled;
        return result;
    }
    if (initializeCurl() != CURLE_OK)
        return failure(TransferError::Internal, "cannot initialize the network runtime");

    static std::atomic<unsigned long long> sequence{0};
    const auto staging = destination.string() + ".part-"
        + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(sequence.fetch_add(1));
    std::ofstream output(staging, std::ios::binary | std::ios::trunc);
    if (!output) return failure(TransferError::Destination, "cannot create the staging file");
    struct Cleanup {
        std::ofstream& output;
        std::string staging;
        bool armed = true;
        ~Cleanup()
        {
            if (!armed) return;
            output.close();
            std::error_code error;
            std::filesystem::remove(staging, error);
        }
    } cleanup{output, staging};

    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> handle(curl_easy_init(), curl_easy_cleanup);
    if (!handle) return failure(TransferError::Internal, "cannot create an HTTP handle");

    Context context{output, cancelled, progress};
    std::array<char, CURL_ERROR_SIZE> errorBuffer{};
    CURLcode optionResult = CURLE_OK;
    auto set = [&](CURLoption option, auto value) {
        if (optionResult == CURLE_OK) optionResult = curl_easy_setopt(handle.get(), option, value);
    };
    set(CURLOPT_URL, request.source.c_str());
    set(CURLOPT_ERRORBUFFER, errorBuffer.data());
    set(CURLOPT_PROTOCOLS_STR, "http,https");
    set(CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
    set(CURLOPT_FOLLOWLOCATION, 1L);
    set(CURLOPT_MAXREDIRS, 10L);
    set(CURLOPT_FAILONERROR, 1L);
    set(CURLOPT_NOSIGNAL, 1L);
    set(CURLOPT_CONNECTTIMEOUT, 30L);
    set(CURLOPT_LOW_SPEED_LIMIT, 1L);
    set(CURLOPT_LOW_SPEED_TIME, 60L);
    set(CURLOPT_PROXY, "");
    set(CURLOPT_USERAGENT, "LiteFetch/0.1 (core)");
    set(CURLOPT_WRITEFUNCTION, &writeData);
    set(CURLOPT_WRITEDATA, &context);
    set(CURLOPT_NOPROGRESS, 0L);
    set(CURLOPT_XFERINFOFUNCTION, &reportProgress);
    set(CURLOPT_XFERINFODATA, &context);
    if (optionResult != CURLE_OK)
        return failure(TransferError::Internal, curl_easy_strerror(optionResult));

    const CURLcode code = curl_easy_perform(handle.get());
    long response = 0;
    curl_easy_getinfo(handle.get(), CURLINFO_RESPONSE_CODE, &response);
    curl_off_t total = -1;
    curl_easy_getinfo(handle.get(), CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &total);

    TransferResult result;
    result.status = TransferStatus::Failed;
    result.error = TransferError::Network;
    result.responseCode = response;
    result.received = context.received;
    result.total = total >= 0 ? static_cast<std::int64_t>(total) : -1;

    if (cancelled.load()) {
        result.status = TransferStatus::Cancelled;
        result.error = TransferError::None;
        return result;
    }
    if (context.diskError) {
        result.error = TransferError::Disk;
        result.message = context.diskMessage.empty() ? "cannot write destination" : context.diskMessage;
        return result;
    }
    if (context.callbackError) {
        result.error = TransferError::Internal;
        result.message = "progress callback failed";
        return result;
    }
    if (code != CURLE_OK) {
        if (code == CURLE_HTTP_RETURNED_ERROR) result.error = TransferError::Http;
        if (code == CURLE_PEER_FAILED_VERIFICATION || code == CURLE_SSL_CONNECT_ERROR)
            result.error = TransferError::Network;
        result.message = curlMessage(code, errorBuffer);
        if (response >= 400) result.message = "HTTP " + std::to_string(response) + ": " + result.message;
        return result;
    }
    if (response < 200 || response >= 300) {
        result.error = TransferError::Http;
        result.message = "HTTP " + std::to_string(response);
        return result;
    }
    output.flush();
    if (!output) {
        result.error = TransferError::Disk;
        result.message = "cannot flush the destination";
        return result;
    }
    output.close();
    if (output.fail()) {
        result.error = TransferError::Disk;
        result.message = "cannot close the destination";
        return result;
    }
    std::error_code renameError;
    std::filesystem::rename(staging, destination, renameError);
    if (renameError) {
        result.error = TransferError::Destination;
        result.message = "cannot publish destination: " + renameError.message();
        return result;
    }
    cleanup.armed = false;
    result.status = TransferStatus::Completed;
    result.error = TransferError::None;
    result.message.clear();
    return result;
}

} // namespace litefetch
