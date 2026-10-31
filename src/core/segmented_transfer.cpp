#include "segmented_transfer.h"

#include "text_utf8.h"

#include <curl/curl.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <system_error>
#include <thread>
#include <vector>

namespace litefetch {
namespace {
std::mutex gateMutex;
std::condition_variable gateCondition;
std::map<std::string, int> gateActive;

// One connection budget per site, shared by every transfer in the process. A
// segmented download takes as many slots as it has live ranges, so two tasks
// on the same host cannot exceed the cap together.
struct GateLease {
    std::string host;
    bool held = false;
    ~GateLease()
    {
        if (!held) return;
        {
            std::lock_guard lock(gateMutex);
            const auto found = gateActive.find(host);
            if (found != gateActive.end() && --found->second <= 0) gateActive.erase(found);
        }
        gateCondition.notify_one();
    }
};

bool acquireGate(const std::string& host, int limit, const std::atomic_bool& cancelled, GateLease& lease)
{
    std::unique_lock lock(gateMutex);
    while (true) {
        const auto found = gateActive.find(host);
        const int active = found == gateActive.end() ? 0 : found->second;
        if (active < limit) break;
        if (cancelled.load()) return false;
        gateCondition.wait_for(lock, std::chrono::milliseconds(50));
    }
    ++gateActive[host];
    lease.host = host;
    lease.held = true;
    return true;
}

bool iequals(std::string_view left, std::string_view right)
{
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        const auto a = static_cast<unsigned char>(left[index]);
        const auto b = static_cast<unsigned char>(right[index]);
        if (std::tolower(a) != std::tolower(b)) return false;
    }
    return true;
}

std::string trimValue(std::string text)
{
    while (!text.empty() && (text.back() == '\r' || text.back() == '\n' || text.back() == ' ')) text.pop_back();
    std::size_t start = 0;
    while (start < text.size() && text[start] == ' ') ++start;
    return text.substr(start);
}

bool parseNumber(std::string_view text, std::int64_t& value)
{
    if (text.empty()) return false;
    std::int64_t result = 0;
    for (const char character : text) {
        if (character < '0' || character > '9') return false;
        result = result * 10 + (character - '0');
    }
    value = result;
    return true;
}

struct ProbeHeaders {
    std::string etag;
    std::string lastModified;
    std::string acceptRanges;
    std::int64_t total = -1;
    long status = 0;
};

std::size_t probeHeaderCallback(char* data, std::size_t size, std::size_t count, void* user) noexcept
{
    auto& headers = *static_cast<ProbeHeaders*>(user);
    const std::size_t length = size * count;
    const std::string line(data, std::min<std::size_t>(length, 4096));
    const auto trimmed = trimValue(line);
    if (trimmed.empty()) return length;
    if (trimmed.rfind("HTTP/", 0) == 0) {
        const auto space = trimmed.find(' ');
        if (space != std::string::npos) {
            std::int64_t code = 0;
            if (parseNumber(trimmed.substr(space + 1, 3), code)) headers.status = static_cast<long>(code);
        }
        return length;
    }
    const auto colon = trimmed.find(':');
    if (colon == std::string::npos) return length;
    const auto name = trimmed.substr(0, colon);
    const auto value = trimValue(trimmed.substr(colon + 1));
    if (iequals(name, "etag")) headers.etag = value;
    else if (iequals(name, "last-modified")) headers.lastModified = value;
    else if (iequals(name, "accept-ranges")) headers.acceptRanges = value;
    else if (iequals(name, "content-length")) parseNumber(value, headers.total);
    return length;
}

std::string hostOf(const std::string& url)
{
    CURLU* handle = curl_url();
    if (!handle) return {};
    std::string host;
    if (curl_url_set(handle, CURLUPART_URL, url.c_str(), 0) == CURLUE_OK) {
        char* value = nullptr;
        if (curl_url_get(handle, CURLUPART_HOST, &value, 0) == CURLUE_OK && value) {
            host = value;
            curl_free(value);
        }
    }
    curl_url_cleanup(handle);
    return host;
}

bool publishNoReplace(const std::filesystem::path& source, const std::filesystem::path& destination,
    std::string& message)
{
    if (MoveFileExW(source.c_str(), destination.c_str(), 0)) return true;
    const DWORD error = GetLastError();
    message = (error == ERROR_ALREADY_EXISTS || error == ERROR_FILE_EXISTS)
        ? "destination already exists"
        : "cannot publish destination: " + std::system_category().message(static_cast<int>(error));
    return false;
}

void writeMetaFile(const std::filesystem::path& path, const SegmentProbe& probe, const std::string& source)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) return;
    output << "version=1\n" << "source=" << source << '\n';
    if (!probe.etag.empty()) output << "etag=" << probe.etag << '\n';
    if (!probe.lastModified.empty()) output << "last_modified=" << probe.lastModified << '\n';
    output << "total=" << probe.total << '\n';
}

struct Segment {
    std::int64_t start = 0;
    std::int64_t end = 0; // inclusive
    std::filesystem::path path;
    std::atomic<std::int64_t> written{0};
    std::atomic<bool> complete{false};
    std::atomic<bool> failed{false};
    std::string message;
    std::mutex messageMutex;
};

struct SegmentWriter {
    std::ofstream output;
    Segment* segment = nullptr;
    const std::atomic_bool& cancelled;
    std::atomic_bool& stopAll;
    std::int64_t expectedStart = 0;
    long status = 0;
    std::int64_t rangeStart = -1;
    bool wrongRange = false;
};

std::size_t segmentHeaderCallback(char* data, std::size_t size, std::size_t count, void* user) noexcept
{
    auto& writer = *static_cast<SegmentWriter*>(user);
    const std::size_t length = size * count;
    const std::string line(data, std::min<std::size_t>(length, 4096));
    const auto trimmed = trimValue(line);
    if (trimmed.empty()) return length;
    if (trimmed.rfind("HTTP/", 0) == 0) {
        const auto space = trimmed.find(' ');
        if (space != std::string::npos) {
            std::int64_t code = 0;
            if (parseNumber(trimmed.substr(space + 1, 3), code)) writer.status = static_cast<long>(code);
        }
        return length;
    }
    const auto colon = trimmed.find(':');
    if (colon == std::string::npos) return length;
    if (!iequals(trimmed.substr(0, colon), "content-range")) return length;
    const auto value = trimValue(trimmed.substr(colon + 1));
    const auto space = value.find(' ');
    const auto dash = value.find('-', space == std::string::npos ? 0 : space);
    if (space != std::string::npos && dash != std::string::npos)
        parseNumber(value.substr(space + 1, dash - space - 1), writer.rangeStart);
    return length;
}

std::size_t segmentWrite(char* data, std::size_t size, std::size_t count, void* user) noexcept
{
    auto& writer = *static_cast<SegmentWriter*>(user);
    if (writer.cancelled.load() || writer.stopAll.load()) return CURL_WRITEFUNC_ERROR;
    if (writer.status != 206 || (writer.rangeStart >= 0 && writer.rangeStart != writer.expectedStart)) {
        writer.wrongRange = true;
        return CURL_WRITEFUNC_ERROR;
    }
    const std::size_t length = size * count;
    try {
        writer.output.write(data, static_cast<std::streamsize>(length));
        if (!writer.output) return CURL_WRITEFUNC_ERROR;
    } catch (...) {
        return CURL_WRITEFUNC_ERROR;
    }
    writer.segment->written.fetch_add(static_cast<std::int64_t>(length));
    return length;
}

void runSegment(const std::string& url, const std::string& username, const std::string& password,
    const std::string& host, Segment& segment, const std::atomic_bool& cancelled,
    std::atomic_bool& stopAll)
{
    GateLease lease;
    if (!acquireGate(host, 4, cancelled, lease)) {
        segment.failed.store(true);
        std::lock_guard lock(segment.messageMutex);
        segment.message = "cancelled while waiting for a connection";
        stopAll.store(true);
        return;
    }
    std::ofstream output(segment.path, std::ios::binary | std::ios::trunc);
    if (!output) {
        segment.failed.store(true);
        {
            std::lock_guard lock(segment.messageMutex);
            segment.message = "cannot create the segment file";
        }
        stopAll.store(true);
        return;
    }
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> handle(curl_easy_init(), curl_easy_cleanup);
    if (!handle) {
        segment.failed.store(true);
        {
            std::lock_guard lock(segment.messageMutex);
            segment.message = "cannot create an HTTP handle";
        }
        stopAll.store(true);
        return;
    }
    SegmentWriter writer{std::move(output), &segment, cancelled, stopAll, segment.start};
    const std::string range = std::to_string(segment.start) + "-" + std::to_string(segment.end);
    std::array<char, CURL_ERROR_SIZE> errorBuffer{};
    auto set = [&](CURLoption option, auto value) {
        curl_easy_setopt(handle.get(), option, value);
    };
    set(CURLOPT_URL, url.c_str());
    set(CURLOPT_ERRORBUFFER, errorBuffer.data());
    set(CURLOPT_PROTOCOLS_STR, "http,https");
    set(CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
    set(CURLOPT_FOLLOWLOCATION, 1L);
    set(CURLOPT_MAXREDIRS, 10L);
    set(CURLOPT_FAILONERROR, 0L);
    set(CURLOPT_NOSIGNAL, 1L);
    set(CURLOPT_CONNECTTIMEOUT, 30L);
    set(CURLOPT_LOW_SPEED_LIMIT, 1L);
    set(CURLOPT_LOW_SPEED_TIME, 60L);
    set(CURLOPT_PROXY, "");
    set(CURLOPT_USERAGENT, "LiteFetch/0.1 (core)");
    set(CURLOPT_RANGE, range.c_str());
    set(CURLOPT_WRITEFUNCTION, &segmentWrite);
    set(CURLOPT_WRITEDATA, &writer);
    set(CURLOPT_HEADERFUNCTION, &segmentHeaderCallback);
    set(CURLOPT_HEADERDATA, &writer);
    set(CURLOPT_NOPROGRESS, 1L);
    if (!username.empty()) {
        set(CURLOPT_HTTPAUTH, static_cast<long>(CURLAUTH_BASIC));
        set(CURLOPT_USERNAME, username.c_str());
        set(CURLOPT_PASSWORD, password.c_str());
    }
    const CURLcode code = curl_easy_perform(handle.get());
    writer.output.flush();
    const bool wrote = static_cast<bool>(writer.output);
    writer.output.close();
    const auto written = segment.written.load();
    if (writer.wrongRange) {
        segment.failed.store(true);
        std::lock_guard lock(segment.messageMutex);
        segment.message = "server returned a different range";
        stopAll.store(true);
        return;
    }
    if (cancelled.load() || stopAll.load()) return;
    if (code != CURLE_OK || !wrote) {
        segment.failed.store(true);
        {
            std::lock_guard lock(segment.messageMutex);
            segment.message = errorBuffer[0] != '\0' ? std::string(errorBuffer.data())
                                                     : std::string(curl_easy_strerror(code));
        }
        stopAll.store(true);
        return;
    }
    if (written != segment.end - segment.start + 1) {
        segment.failed.store(true);
        {
            std::lock_guard lock(segment.messageMutex);
            segment.message = "segment ended before the announced range";
        }
        stopAll.store(true);
        return;
    }
    segment.complete.store(true);
}

// Merges the segments that really arrived, in order, into the partial file. A
// prefix is all single-stream resume can use; later gaps are discarded.
std::int64_t mergeSegments(const std::vector<Segment>& segments, const std::filesystem::path& partial)
{
    std::ofstream output(partial, std::ios::binary | std::ios::trunc);
    if (!output) return 0;
    std::int64_t merged = 0;
    for (const auto& segment : segments) {
        const auto expected = segment.end - segment.start + 1;
        const auto written = segment.written.load();
        const auto copy = (std::min)(expected, written);
        if (copy > 0) {
            std::ifstream input(segment.path, std::ios::binary);
            if (!input) break;
            std::vector<char> buffer(64 * 1024);
            std::int64_t remaining = copy;
            while (remaining > 0) {
                const auto chunk = static_cast<std::streamsize>((std::min)(remaining,
                    static_cast<std::int64_t>(buffer.size())));
                input.read(buffer.data(), chunk);
                if (input.gcount() <= 0) break;
                output.write(buffer.data(), input.gcount());
                remaining -= input.gcount();
            }
            merged += copy - remaining;
        }
        if (!segment.complete.load()) break;
    }
    output.flush();
    return merged;
}
} // namespace

SegmentProbe probeForSegments(const std::string& url, const std::string& username,
    const std::string& password, const std::atomic_bool& cancelled)
{
    SegmentProbe probe;
    probe.host = hostOf(url);
    if (cancelled.load()) return probe;
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> handle(curl_easy_init(), curl_easy_cleanup);
    if (!handle) return probe;
    ProbeHeaders headers;
    std::array<char, CURL_ERROR_SIZE> errorBuffer{};
    auto set = [&](CURLoption option, auto value) { curl_easy_setopt(handle.get(), option, value); };
    set(CURLOPT_URL, url.c_str());
    set(CURLOPT_NOBODY, 1L);
    set(CURLOPT_ERRORBUFFER, errorBuffer.data());
    set(CURLOPT_PROTOCOLS_STR, "http,https");
    set(CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
    set(CURLOPT_FOLLOWLOCATION, 1L);
    set(CURLOPT_MAXREDIRS, 10L);
    set(CURLOPT_FAILONERROR, 0L);
    set(CURLOPT_NOSIGNAL, 1L);
    set(CURLOPT_CONNECTTIMEOUT, 15L);
    set(CURLOPT_TIMEOUT, 30L);
    set(CURLOPT_PROXY, "");
    set(CURLOPT_USERAGENT, "LiteFetch/0.1 (core)");
    set(CURLOPT_HEADERFUNCTION, &probeHeaderCallback);
    set(CURLOPT_HEADERDATA, &headers);
    if (!username.empty()) {
        set(CURLOPT_HTTPAUTH, static_cast<long>(CURLAUTH_BASIC));
        set(CURLOPT_USERNAME, username.c_str());
        set(CURLOPT_PASSWORD, password.c_str());
    }
    const CURLcode code = curl_easy_perform(handle.get());
    if (code != CURLE_OK) return probe;
    if (headers.status < 200 || headers.status >= 300) return probe;
    probe.ok = true;
    probe.total = headers.total;
    probe.etag = headers.etag;
    probe.lastModified = headers.lastModified;
    probe.ranges = iequals(headers.acceptRanges, "bytes");
    return probe;
}

TransferResult transferSegmented(const std::string& url, const std::string& username,
    const std::string& password, const std::filesystem::path& destination,
    const std::filesystem::path& partial, const std::filesystem::path& metaPath,
    const SegmentProbe& probe, int segments, const std::atomic_bool& cancelled,
    const std::function<void(const TransferProgressInfo&)>& progress)
{
    TransferResult result;
    result.status = TransferStatus::Failed;
    result.error = TransferError::Network;
    result.total = probe.total;
    const int count = (std::max)(1, segments);
    const std::int64_t chunk = probe.total / count;
    if (chunk <= 0) {
        result.message = "resource is too small to segment";
        return result;
    }

    writeMetaFile(metaPath, probe, url);
    std::vector<Segment> parts(static_cast<std::size_t>(count));
    for (int index = 0; index < count; ++index) {
        auto& segment = parts[static_cast<std::size_t>(index)];
        segment.start = chunk * index;
        segment.end = index == count - 1 ? probe.total - 1 : chunk * (index + 1) - 1;
        segment.path = partial;
        segment.path += pathFromUtf8(".seg" + std::to_string(index));
    }

    std::atomic_bool stopAll{false};
    // The ranges arrive in parallel and finish together, so a report tied to a
    // finished range can be minutes late on a large file: the UI would show
    // "connecting" with no size for the whole transfer. One reporter samples
    // the per-range counters instead, and the probe's total is known from the
    // start, so the row gets a size and live bytes while data arrives.
    auto bytesWritten = [&parts] {
        std::int64_t written = 0;
        for (const auto& segment : parts) written += segment.written.load();
        return written;
    };
    auto report = [&](std::int64_t current) {
        if (progress) progress(TransferProgressInfo{current, probe.total});
    };
    std::atomic_bool stopReporting{false};
    std::thread reporter([&] {
        while (!stopReporting.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            report(bytesWritten());
        }
    });

    std::vector<std::thread> workers;
    workers.reserve(static_cast<std::size_t>(count));
    for (auto& segment : parts) {
        workers.emplace_back([&, thisSegment = &segment] {
            runSegment(url, username, password, probe.host, *thisSegment, cancelled, stopAll);
        });
    }
    for (auto& worker : workers) worker.join();
    stopReporting.store(true);
    reporter.join();
    // A fast transfer can finish inside the reporter's first interval; the
    // final total must still reach the caller before the merge starts.
    report(bytesWritten());

    if (cancelled.load()) {
        stopAll.store(true);
        const auto merged = mergeSegments(parts, partial);
        for (const auto& segment : parts) {
            std::error_code ignored;
            std::filesystem::remove(segment.path, ignored);
        }
        result.status = TransferStatus::Cancelled;
        result.error = TransferError::None;
        result.received = merged;
        result.partialBytes = merged;
        return result;
    }

    bool anyFailed = false;
    std::string message;
    for (auto& segment : parts) {
        if (segment.failed.load() || !segment.complete.load()) {
            anyFailed = true;
            std::lock_guard lock(segment.messageMutex);
            if (message.empty() && !segment.message.empty()) message = segment.message;
        }
    }
    if (anyFailed) {
        const auto merged = mergeSegments(parts, partial);
        for (const auto& segment : parts) {
            std::error_code ignored;
            std::filesystem::remove(segment.path, ignored);
        }
        result.error = TransferError::Network;
        result.message = message.empty() ? "a segment did not finish" : message;
        result.received = merged;
        result.partialBytes = merged;
        if (merged == 0) {
            std::error_code ignored;
            std::filesystem::remove(partial, ignored);
            std::filesystem::remove(metaPath, ignored);
        }
        return result;
    }

    const auto merged = mergeSegments(parts, partial);
    for (const auto& segment : parts) {
        std::error_code ignored;
        std::filesystem::remove(segment.path, ignored);
    }
    if (merged != probe.total) {
        result.message = "merged length differs from the announced size";
        result.received = merged;
        result.partialBytes = merged;
        return result;
    }
    std::string publishMessage;
    if (!publishNoReplace(partial, destination, publishMessage)) {
        result.error = TransferError::Destination;
        result.message = std::move(publishMessage);
        result.received = merged;
        result.partialBytes = merged;
        return result;
    }
    std::error_code ignored;
    std::filesystem::remove(metaPath, ignored);
    result.status = TransferStatus::Completed;
    result.error = TransferError::None;
    result.message.clear();
    result.received = merged;
    result.partialBytes = 0;
    return result;
}

} // namespace litefetch
