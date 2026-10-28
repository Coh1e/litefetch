#include "http_engine.h"

#include "segmented_transfer.h"
#include "text_utf8.h"

#include <curl/curl.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <utility>

namespace litefetch {
namespace {
// Below this size the probe round trip costs more than the concurrency saves.
constexpr std::int64_t segmentMinimumSize = 4 * 1024 * 1024;
CURLcode initializeCurl()
{
    static const CURLcode result = curl_global_init(CURL_GLOBAL_DEFAULT);
    return result;
}

bool isSupportedUrl(const std::string& source)
{
    CURLU* url = curl_url();
    if (!url) return false;
    const bool parsed = curl_url_set(url, CURLUPART_URL, source.c_str(), 0) == CURLUE_OK;
    char* scheme = nullptr;
    const bool supported = parsed
        && curl_url_get(url, CURLUPART_SCHEME, &scheme, 0) == CURLUE_OK
        && scheme
        && (std::string(scheme) == "http" || std::string(scheme) == "https"
            || std::string(scheme) == "ftp");
    if (scheme) curl_free(scheme);
    curl_url_cleanup(url);
    return supported;
}

// Validators and the total length belong to the resource, not to the task:
// they live beside the partial file so a crashed or cancelled transfer can
// decide on its own whether the bytes on disk still belong together.
struct PartialMeta {
    std::string source;
    std::string etag;
    std::string lastModified;
    std::int64_t total = -1;
};

std::string trimValue(std::string text)
{
    while (!text.empty() && (text.back() == '\r' || text.back() == '\n' || text.back() == ' ')) text.pop_back();
    std::size_t start = 0;
    while (start < text.size() && text[start] == ' ') ++start;
    return text.substr(start);
}

bool parseNumber(std::string_view text, std::int64_t& value)
{
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

bool readMeta(const std::filesystem::path& path, PartialMeta& meta)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) return false;
    std::string line;
    bool versionOk = false;
    while (std::getline(input, line)) {
        const auto separator = line.find('=');
        if (separator == std::string::npos) continue;
        const auto key = line.substr(0, separator);
        const auto value = trimValue(line.substr(separator + 1));
        if (key == "version" && value == "1") versionOk = true;
        else if (key == "source") meta.source = value;
        else if (key == "etag") meta.etag = value;
        else if (key == "last_modified") meta.lastModified = value;
        else if (key == "total") parseNumber(value, meta.total);
    }
    return versionOk && !meta.source.empty();
}

bool writeMeta(const std::filesystem::path& path, const PartialMeta& meta)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) return false;
    output << "version=1\n" << "source=" << meta.source << '\n';
    if (!meta.etag.empty()) output << "etag=" << meta.etag << '\n';
    if (!meta.lastModified.empty()) output << "last_modified=" << meta.lastModified << '\n';
    if (meta.total >= 0) output << "total=" << meta.total << '\n';
    output.flush();
    return static_cast<bool>(output);
}

std::int64_t fileSize(const std::filesystem::path& path)
{
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    return error ? 0 : static_cast<std::int64_t>(size);
}

struct Context {
    std::ofstream& output;
    const std::atomic_bool& cancelled;
    const std::function<void(const TransferProgressInfo&)>& progress;
    PartialMeta meta;
    std::filesystem::path metaPath;
    std::int64_t offset = 0; // resume start, 0 for a fresh transfer
    std::int64_t received = 0;
    bool resumeExpected = false;
    bool metaWritten = false;
    long statusCode = 0;
    std::int64_t contentRangeStart = -1;
    std::int64_t total = -1;
    bool restartRequired = false;
    bool diskError = false;
    bool callbackError = false;
    std::string diskMessage;
    std::string restartReason;
};

std::size_t writeData(char* data, std::size_t size, std::size_t count, void* user) noexcept
{
    auto& context = *static_cast<Context*>(user);
    if (context.cancelled.load()) return CURL_WRITEFUNC_ERROR;
    if (context.resumeExpected && context.statusCode > 0) {
        if (context.statusCode != 206) {
            // The server sent the whole resource: appending would corrupt the
            // partial data, so stop before a single byte is written.
            context.restartRequired = true;
            context.restartReason = "server did not honor the resume range";
            return CURL_WRITEFUNC_ERROR;
        }
        if (context.contentRangeStart >= 0 && context.contentRangeStart != context.offset) {
            context.restartRequired = true;
            context.restartReason = "server resumed at a different offset";
            return CURL_WRITEFUNC_ERROR;
        }
    }
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

// curl delivers each header line separately; the empty line ends the block and
// is the moment the validator and total are complete enough to persist.
std::size_t readHeader(char* data, std::size_t size, std::size_t count, void* user) noexcept
{
    auto& context = *static_cast<Context*>(user);
    const std::size_t length = size * count;
    const std::string line(data, std::min<std::size_t>(length, 4096));
    const auto trimmed = trimValue(line);
    if (trimmed.empty()) {
        if (!context.metaWritten && (context.statusCode == 200 || context.statusCode == 206)) {
            context.metaWritten = writeMeta(context.metaPath, context.meta);
            if (!context.metaWritten) {
                context.diskError = true;
                context.diskMessage = "cannot write resume metadata";
                return 0;
            }
        }
        return length;
    }
    const auto colon = trimmed.find(':');
    if (colon == std::string::npos) {
        if (trimmed.rfind("HTTP/", 0) == 0) {
            const auto space = trimmed.find(' ');
            if (space != std::string::npos) {
                std::int64_t parsed = 0;
                if (parseNumber(trimmed.substr(space + 1, 3), parsed))
                    context.statusCode = static_cast<long>(parsed);
            }
            context.contentRangeStart = -1;
        }
        return length;
    }
    const auto name = trimmed.substr(0, colon);
    const auto value = trimValue(trimmed.substr(colon + 1));
    auto iequals = [](std::string_view left, std::string_view right) {
        if (left.size() != right.size()) return false;
        for (std::size_t index = 0; index < left.size(); ++index) {
            const auto a = static_cast<unsigned char>(left[index]);
            const auto b = static_cast<unsigned char>(right[index]);
            if (std::tolower(a) != std::tolower(b)) return false;
        }
        return true;
    };
    if (iequals(name, "etag")) {
        context.meta.etag = value;
    } else if (iequals(name, "last-modified")) {
        context.meta.lastModified = value;
    } else if (iequals(name, "content-range")) {
        // bytes <start>-<end>/<total>
        const auto space = value.find(' ');
        const auto dash = value.find('-', space == std::string::npos ? 0 : space);
        const auto slash = value.find('/', dash == std::string::npos ? 0 : dash);
        if (space != std::string::npos && dash != std::string::npos) {
            parseNumber(value.substr(space + 1, dash - space - 1), context.contentRangeStart);
            if (slash != std::string::npos) {
                const auto totalText = value.substr(slash + 1);
                std::int64_t total = -1;
                if (parseNumber(totalText, total)) {
                    context.total = total;
                    context.meta.total = total;
                }
            }
        }
    } else if (iequals(name, "content-length")) {
        std::int64_t lengthValue = -1;
        if (parseNumber(value, lengthValue) && context.statusCode != 206) {
            context.total = lengthValue;
            context.meta.total = lengthValue;
        }
    }
    return length;
}

int reportProgress(void* user, curl_off_t total, curl_off_t now, curl_off_t, curl_off_t) noexcept
{
    auto& context = *static_cast<Context*>(user);
    if (context.cancelled.load()) return 1;
    try {
        if (context.progress) {
            const auto reported = context.offset + static_cast<std::int64_t>(now);
            const auto expected = context.total >= 0 ? context.total
                : (total >= 0 ? context.offset + static_cast<std::int64_t>(total) : -1);
            context.progress(TransferProgressInfo{reported, expected});
        }
    } catch (...) {
        context.callbackError = true;
        return 1;
    }
    return 0;
}

TransferResult failure(TransferError error, std::string message)
{
    TransferResult result;
    result.status = TransferStatus::Failed;
    result.error = error;
    result.message = std::move(message);
    return result;
}

bool publishNoReplace(const std::filesystem::path& staging,
    const std::filesystem::path& destination, std::string& message)
{
#ifdef _WIN32
    if (MoveFileExW(staging.c_str(), destination.c_str(), 0)) return true;
    const DWORD error = GetLastError();
    if (error == ERROR_ALREADY_EXISTS || error == ERROR_FILE_EXISTS)
        message = "destination already exists";
    else
        message = "cannot publish destination: "
            + std::system_category().message(static_cast<int>(error));
    return false;
#else
    std::error_code error;
    if (std::filesystem::exists(destination, error)) {
        message = "destination already exists";
        return false;
    }
    if (error) {
        message = "cannot inspect destination: " + error.message();
        return false;
    }
    std::filesystem::rename(staging, destination, error);
    if (error) {
        message = "cannot publish destination: " + error.message();
        return false;
    }
    return true;
#endif
}

std::string curlMessage(CURLcode code, const std::array<char, CURL_ERROR_SIZE>& buffer)
{
    if (buffer[0] != '\0') return buffer.data();
    return curl_easy_strerror(code);
}

// Credentials embedded in a URL are split off before curl sees the URL, so no
// error message, redirect or sidecar file can echo them back.
struct ParsedUrl {
    std::string url;
    std::string user;
    std::string password;
};

ParsedUrl splitCredentials(const std::string& url)
{
    ParsedUrl parsed;
    parsed.url = url;
    CURLU* handle = curl_url();
    if (!handle) return parsed;
    if (curl_url_set(handle, CURLUPART_URL, url.c_str(), 0) == CURLUE_OK) {
        char* user = nullptr;
        char* password = nullptr;
        if (curl_url_get(handle, CURLUPART_USER, &user, 0) == CURLUE_OK && user) {
            parsed.user = user;
            curl_free(user);
        }
        if (curl_url_get(handle, CURLUPART_PASSWORD, &password, 0) == CURLUE_OK && password) {
            parsed.password = password;
            curl_free(password);
        }
        if (curl_url_set(handle, CURLUPART_USER, nullptr, 0) == CURLUE_OK
            && curl_url_set(handle, CURLUPART_PASSWORD, nullptr, 0) == CURLUE_OK) {
            char* without = nullptr;
            if (curl_url_get(handle, CURLUPART_URL, &without, 0) == CURLUE_OK && without) {
                parsed.url = without;
                curl_free(without);
            }
        }
    }
    curl_url_cleanup(handle);
    return parsed;
}

std::string schemeOf(const std::string& url)
{
    CURLU* handle = curl_url();
    if (!handle) return {};
    std::string scheme;
    if (curl_url_set(handle, CURLUPART_URL, url.c_str(), 0) == CURLUE_OK) {
        char* value = nullptr;
        if (curl_url_get(handle, CURLUPART_SCHEME, &value, 0) == CURLUE_OK && value) {
            scheme = value;
            curl_free(value);
        }
    }
    curl_url_cleanup(handle);
    return scheme;
}

TransferError ftpErrorClass(long response)
{
    if (response == 530) return TransferError::Http;
    if (response == 550) return TransferError::Http;
    return TransferError::Network;
}

std::string ftpMessage(CURLcode code, long response, const std::array<char, CURL_ERROR_SIZE>& buffer)
{
    switch (response) {
    case 421: return "FTP 421: service not available, closing control connection";
    case 425: return "FTP 425: cannot open the data connection";
    case 426: return "FTP 426: data connection closed, transfer aborted";
    case 530: return "FTP 530: login failed";
    case 550: return "FTP 550: file unavailable";
    default: break;
    }
    if (code == CURLE_REMOTE_ACCESS_DENIED) return "FTP 530: login failed";
    if (code == CURLE_FTP_COULDNT_RETR_FILE) return "FTP 550: file unavailable";
    if (code == CURLE_FTP_COULDNT_USE_REST) return "resume is not supported by this server";
    if (buffer[0] != '\0') return buffer.data();
    return curl_easy_strerror(code);
}

// FTP has no HEAD: a body-less request makes curl issue SIZE instead.
std::int64_t ftpRemoteSize(const std::string& url, const std::string& username,
    const std::string& password, const std::atomic_bool& cancelled, long& code, std::string& message)
{
    if (cancelled.load()) {
        message = "cancelled";
        return -1;
    }
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> handle(curl_easy_init(), curl_easy_cleanup);
    if (!handle) {
        message = "cannot create an FTP handle";
        return -1;
    }
    std::array<char, CURL_ERROR_SIZE> errorBuffer{};
    curl_easy_setopt(handle.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(handle.get(), CURLOPT_NOBODY, 1L);
    curl_easy_setopt(handle.get(), CURLOPT_PROTOCOLS_STR, "ftp,ftps");
    curl_easy_setopt(handle.get(), CURLOPT_ERRORBUFFER, errorBuffer.data());
    curl_easy_setopt(handle.get(), CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(handle.get(), CURLOPT_CONNECTTIMEOUT, 30L);
    curl_easy_setopt(handle.get(), CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(handle.get(), CURLOPT_PROXY, "");
    curl_easy_setopt(handle.get(), CURLOPT_USERAGENT, "LiteFetch/0.1 (core)");
    if (!username.empty()) {
        curl_easy_setopt(handle.get(), CURLOPT_HTTPAUTH, static_cast<long>(CURLAUTH_BASIC));
        curl_easy_setopt(handle.get(), CURLOPT_USERNAME, username.c_str());
        curl_easy_setopt(handle.get(), CURLOPT_PASSWORD, password.c_str());
    }
    const CURLcode result = curl_easy_perform(handle.get());
    curl_easy_getinfo(handle.get(), CURLINFO_RESPONSE_CODE, &code);
    if (result != CURLE_OK) {
        message = ftpMessage(result, code, errorBuffer);
        return -1;
    }
    curl_off_t length = -1;
    curl_easy_getinfo(handle.get(), CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &length);
    return length >= 0 ? static_cast<std::int64_t>(length) : -1;
}

// One destination has one partial file, so two transfers must never write it at// the same time. They are serialised per destination instead: the loser then
// fails the no-replace publish instead of corrupting the winner's bytes.
std::shared_ptr<std::mutex> mutexForDestination(const std::string& destination)
{
    static std::mutex guard;
    static std::map<std::string, std::weak_ptr<std::mutex>> locks;
    std::lock_guard lock(guard);
    auto& slot = locks[destination];
    if (auto existing = slot.lock()) return existing;
    auto created = std::make_shared<std::mutex>();
    slot = created;
    return created;
}
} // namespace

std::filesystem::path partialPathFor(const std::filesystem::path& destination)
{
    auto path = destination;
    path += pathFromUtf8(".lfpart");
    return path;
}

std::filesystem::path partialMetaPathFor(const std::filesystem::path& destination)
{
    auto path = destination;
    path += pathFromUtf8(".lfpart.meta");
    return path;
}

bool discardPartial(const std::filesystem::path& destination)
{
    std::error_code error;
    std::filesystem::remove(partialPathFor(destination), error);
    std::error_code metaError;
    std::filesystem::remove(partialMetaPathFor(destination), metaError);
    return !error;
}

TransferResult transferHttp(const TransferRequest& request, const std::atomic_bool& cancelled,
    const std::function<void(const TransferProgressInfo&)>& progress)
{
    if (!isSupportedUrl(request.source))
        return failure(TransferError::InvalidRequest, "source is not an HTTP(S) or FTP URL");
    const std::filesystem::path destination = pathFromUtf8(request.destination);
    if (!destination.is_absolute() || destination.filename().empty())
        return failure(TransferError::Destination, "destination must be an absolute file path");

    // Serialise per destination *before* every filesystem decision: the second
    // transfer must observe the winner's published file, not race past the
    // preflight and then try to reuse the same partial path.
    const auto destinationLock = mutexForDestination(request.destination);
    std::unique_lock<std::mutex> serialize(*destinationLock);

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

    const auto partial = partialPathFor(destination);
    const auto metaPath = partialMetaPathFor(destination);
    if (request.restartFromZero) discardPartial(destination);

    // Credentials are in-memory only: the sidecar keeps the source without
    // userinfo, so a shared partial file never stores a password.
    const auto parsedSource = splitCredentials(request.source);
    const std::string metaSource = parsedSource.url;
    const std::string username = request.username.empty() ? parsedSource.user : request.username;
    const std::string password = request.username.empty() ? parsedSource.password : request.password;
    PartialMeta meta;
    const bool haveMeta = readMeta(metaPath, meta) && meta.source == metaSource;
    std::int64_t offset = haveMeta ? fileSize(partial) : 0;
    if (!haveMeta) {
        std::error_code ignored;
        std::filesystem::remove(partial, ignored);
        offset = 0;
    }
    meta.source = metaSource;

    const std::string scheme = schemeOf(parsedSource.url);
    const bool isFtp = scheme == "ftp" || scheme == "ftps";
    if (isFtp) {
        // FTP has no HEAD: a body-less request issues SIZE, which is what the
        // resume decision needs. The remote size is also the consistency check:
        // a changed size means the partial data belongs to an older file.
        long code = 0;
        std::string message;
        const auto remote = ftpRemoteSize(parsedSource.url, username, password, cancelled, code, message);
        if (remote < 0) {
            TransferResult result;
            result.status = TransferStatus::Failed;
            result.error = ftpErrorClass(code);
            result.message = message.empty() ? "cannot read the remote file size" : message;
            result.responseCode = code;
            return result;
        }
        if (offset > 0 && meta.total >= 0 && (remote != meta.total || offset > remote)) {
            TransferResult result;
            result.status = TransferStatus::Failed;
            result.error = TransferError::Http;
            result.restartRequired = true;
            result.message = "remote file changed since the partial data was saved";
            result.received = offset;
            result.partialBytes = offset;
            return result;
        }
        meta.total = remote;
        writeMeta(metaPath, meta);
        if (offset > 0 && offset == remote) {
            std::string publishMessage;
            if (!publishNoReplace(partial, destination, publishMessage)) {
                return failure(TransferError::Destination, std::move(publishMessage));
            }
            std::error_code ignored;
            std::filesystem::remove(metaPath, ignored);
            TransferResult result;
            result.status = TransferStatus::Completed;
            result.error = TransferError::None;
            result.received = offset;
            return result;
        }
    }

    // A fresh transfer may use several ranges when the server advertises them
    // and the resource is big enough. Anything unusual in the probe falls back
    // to the single-stream path below, which is also the resume path.
    if (!isFtp && offset == 0 && !cancelled.load()) {
        const auto probe = probeForSegments(parsedSource.url, username, password, cancelled);
        if (probe.ok && probe.ranges && probe.total >= segmentMinimumSize) {
            const int segments = static_cast<int>((std::min)(std::int64_t{4},
                probe.total / (512 * 1024)));
            if (segments > 1)
                return transferSegmented(parsedSource.url, username, password, destination, partial,
                    metaPath, probe, segments, cancelled, progress);
        }
    }

    std::ofstream output(partial, std::ios::binary | (offset > 0 ? std::ios::app : std::ios::trunc));
    if (!output) return failure(TransferError::Destination, "cannot create the partial file");

    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> handle(curl_easy_init(), curl_easy_cleanup);
    if (!handle) return failure(TransferError::Internal, "cannot create an HTTP handle");

    Context context{output, cancelled, progress};
    context.meta = meta;
    context.metaPath = metaPath;
    context.offset = offset;
    context.resumeExpected = offset > 0;
    context.total = meta.total;

    std::array<char, CURL_ERROR_SIZE> errorBuffer{};
    CURLcode optionResult = CURLE_OK;
    auto set = [&](CURLoption option, auto value) {
        if (optionResult == CURLE_OK) optionResult = curl_easy_setopt(handle.get(), option, value);
    };
    set(CURLOPT_URL, parsedSource.url.c_str());
    set(CURLOPT_ERRORBUFFER, errorBuffer.data());
    set(CURLOPT_PROTOCOLS_STR, "http,https,ftp");
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
    // Basic auth for this transfer only. Redirects keep curl's default: the
    // Authorization header is dropped when the host changes.
    if (!username.empty()) {
        set(CURLOPT_HTTPAUTH, static_cast<long>(CURLAUTH_BASIC));
        set(CURLOPT_USERNAME, username.c_str());
        set(CURLOPT_PASSWORD, password.c_str());
    }
    set(CURLOPT_WRITEFUNCTION, &writeData);
    set(CURLOPT_WRITEDATA, &context);
    set(CURLOPT_HEADERFUNCTION, &readHeader);
    set(CURLOPT_HEADERDATA, &context);
    set(CURLOPT_NOPROGRESS, 0L);
    set(CURLOPT_XFERINFOFUNCTION, &reportProgress);
    set(CURLOPT_XFERINFODATA, &context);

    curl_slist* headers = nullptr;
    if (offset > 0) {
        if (isFtp) {
            set(CURLOPT_RESUME_FROM_LARGE, static_cast<curl_off_t>(offset));
        } else {
            const std::string range = std::to_string(offset) + "-";
            set(CURLOPT_RANGE, range.c_str());
            std::string validator;
            if (!meta.etag.empty()) validator = "If-Range: " + meta.etag;
            else if (!meta.lastModified.empty()) validator = "If-Range: " + meta.lastModified;
            if (!validator.empty()) {
                headers = curl_slist_append(headers, validator.c_str());
                set(CURLOPT_HTTPHEADER, headers);
            }
        }
    }
    if (optionResult != CURLE_OK) {
        if (headers) curl_slist_free_all(headers);
        return failure(TransferError::Internal, curl_easy_strerror(optionResult));
    }

    const CURLcode code = curl_easy_perform(handle.get());
    if (headers) curl_slist_free_all(headers);
    long response = 0;
    curl_easy_getinfo(handle.get(), CURLINFO_RESPONSE_CODE, &response);

    TransferResult result;
    result.status = TransferStatus::Failed;
    result.error = TransferError::Network;
    result.responseCode = response;
    result.total = context.total >= 0 ? context.total : -1;

    if (context.restartRequired) {
        output.close();
        result.received = offset;
        result.partialBytes = offset;
        result.restartRequired = true;
        result.error = TransferError::Http;
        result.message = context.restartReason;
        return result;
    }
    if (cancelled.load() || context.callbackError) {
        output.close();
        result.status = TransferStatus::Cancelled;
        result.error = TransferError::None;
        result.received = offset + context.received;
        result.partialBytes = result.received;
        return result;
    }
    if (response == 416 && context.resumeExpected) {
        // The server says the range is unsatisfiable: either the partial file
        // is already the whole resource, or the resource shrank.
        output.close();
        if (context.total >= 0 && offset == context.total) {
            std::string publishMessage;
            if (!publishNoReplace(partial, destination, publishMessage)) {
                result.error = TransferError::Destination;
                result.message = std::move(publishMessage);
                return result;
            }
            std::error_code ignored;
            std::filesystem::remove(metaPath, ignored);
            result.status = TransferStatus::Completed;
            result.error = TransferError::None;
            result.message.clear();
            result.received = offset;
            result.partialBytes = 0;
            return result;
        }
        result.restartRequired = true;
        result.error = TransferError::Http;
        result.message = "resources changed; the stored range is unsatisfiable";
        result.received = offset;
        result.partialBytes = offset;
        return result;
    }
    // A failure that wrote nothing has nothing to resume; leaving an empty
    // partial file behind would only confuse the next attempt.
    auto dropEmptyPartial = [&] {
        if (offset + context.received != 0) return;
        std::error_code ignored;
        std::filesystem::remove(partial, ignored);
        std::filesystem::remove(metaPath, ignored);
    };
    if (context.diskError) {
        output.close();
        result.error = TransferError::Disk;
        result.message = context.diskMessage.empty() ? "cannot write destination" : context.diskMessage;
        result.received = offset + context.received;
        result.partialBytes = result.received;
        dropEmptyPartial();
        return result;
    }
    if (code != CURLE_OK) {
        output.close();
        if (isFtp && context.resumeExpected && code == CURLE_FTP_COULDNT_USE_REST) {
            // The server refuses REST: the partial bytes are still valid for this
            // resource, so keep them and ask before starting over.
            result.restartRequired = true;
            result.error = TransferError::Http;
            result.message = "resume is not supported by this server";
            result.received = offset;
            result.partialBytes = offset;
            return result;
        }
        if (code == CURLE_HTTP_RETURNED_ERROR) result.error = TransferError::Http;
        if (isFtp) result.error = ftpErrorClass(response);
        if (code == CURLE_PEER_FAILED_VERIFICATION || code == CURLE_SSL_CONNECT_ERROR)
            result.error = TransferError::Network;
        result.message = isFtp ? ftpMessage(code, response, errorBuffer) : curlMessage(code, errorBuffer);
        if (!isFtp && response >= 400)
            result.message = "HTTP " + std::to_string(response) + ": " + result.message;
        result.received = offset + context.received;
        result.partialBytes = result.received;
        dropEmptyPartial();
        return result;
    }
    if (response < 200 || response >= 300) {
        output.close();
        result.error = TransferError::Http;
        result.message = "HTTP " + std::to_string(response);
        result.received = offset + context.received;
        result.partialBytes = result.received;
        dropEmptyPartial();
        return result;
    }
    if (context.resumeExpected && response == 206 && context.total >= 0
        && offset + context.received != context.total) {
        // A Complete answer that is shorter than the announced length is a
        // truncated transfer, not a finished file.
        output.close();
        result.error = TransferError::Network;
        result.message = "transfer ended before the announced length";
        result.received = offset + context.received;
        result.partialBytes = result.received;
        return result;
    }
    if (context.total >= 0 && offset + context.received != context.total && !context.resumeExpected) {
        output.close();
        result.error = TransferError::Network;
        result.message = "transfer ended before the announced length";
        result.received = context.received;
        result.partialBytes = context.received;
        dropEmptyPartial();
        return result;
    }
    output.flush();
    if (!output) {
        output.close();
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
    std::string publishMessage;
    if (!publishNoReplace(partial, destination, publishMessage)) {
        result.error = TransferError::Destination;
        result.message = std::move(publishMessage);
        result.received = offset + context.received;
        result.partialBytes = result.received;
        return result;
    }
    std::error_code ignored;
    std::filesystem::remove(metaPath, ignored);
    result.status = TransferStatus::Completed;
    result.error = TransferError::None;
    result.message.clear();
    result.received = offset + context.received;
    result.partialBytes = 0;
    return result;
}

} // namespace litefetch
