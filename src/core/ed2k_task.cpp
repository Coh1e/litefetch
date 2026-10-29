#include "ed2k_task.h"

#include "ed2k_serve.h"
#include "ed2k_transfer.h"

#include <chrono>
#include <thread>

#include <filesystem>

namespace litefetch {
namespace {
std::string partPathFor(const std::string& destination)
{
    return destination + ".part";
}
} // namespace

Ed2kTaskResult downloadEd2kTask(const Ed2kTaskRequest& request,
    const std::atomic_bool& cancelled,
    const std::function<void(std::int64_t received, std::int64_t total)>& progress)
{
    Ed2kTaskResult result;
    const auto link = parseEd2kLink(request.link);
    if (!link.ok) {
        result.error = "the link is not a usable ed2k file link: " + link.error;
        return result;
    }

    // An existing destination is never overwritten: it is either already the
    // file we want, or it is something else and the user must decide.
    if (std::filesystem::exists(request.destination)) {
        if (ed2kFileHash(request.destination) == link.link.hash) {
            result.ok = true;
            result.received = link.link.size;
            return result;
        }
        result.error = "the destination already exists with different content";
        return result;
    }

    const auto partPath = partPathFor(request.destination);
    {
        // A finished part file from an interrupted run needs no peer at all:
        // verify it and publish.
        const auto resume = inspectEd2kParts(partPath, link.link.size, link.link.hash);
        if (resume.missingParts == 0 && resume.verifiedParts > 0
            && ed2kFileHash(partPath) == link.link.hash) {
            std::error_code publishError;
            std::filesystem::rename(partPath, request.destination, publishError);
            if (publishError) {
                result.error = "cannot move the finished file into place: " + publishError.message();
                return result;
            }
            std::filesystem::remove(partPath + ".ed2kmeta", publishError);
            result.ok = true;
            result.received = link.link.size;
            return result;
        }
    }

    std::vector<Ed2kServerEndpoint> candidates = request.peers;
    // Peers named in the link itself need no server at all.
    for (const auto& source : link.sources)
        candidates.push_back({source.address, source.port});
    if (candidates.empty()) {
        if (request.servers.empty() && !request.lookup) {
            result.error = "no ed2k server is configured";
            return result;
        }
        bool anyServerAnswered = false;
        for (const auto& server : request.servers) {
            if (cancelled.load()) {
                result.error = "cancelled";
                return result;
            }
            const auto query = queryEd2kSources(server, link.link, request.timeoutMillis);
            if (!query.ok) continue;
            anyServerAnswered = true;
            if (query.sources.empty()) continue;
            for (const auto& source : query.sources)
                candidates.push_back({source.address, source.port});
            break;
        }
        if (candidates.empty() && request.lookup) {
            for (const auto& source : request.lookup(link.link.hash))
                candidates.push_back({source.address, source.port});
        }
        if (candidates.empty()) {
            result.noSources = anyServerAnswered;
            result.error = anyServerAnswered ? "the servers know no sources for this file"
                                             : "no ed2k server answered";
            return result;
        }
    }

    const auto directory = std::filesystem::path(request.destination).parent_path();
    std::error_code error;
    if (!directory.empty()) std::filesystem::create_directories(directory, error);
    if (error) {
        result.error = "cannot create the destination directory: " + error.message();
        return result;
    }

    Ed2kPeerRequest peerRequest;
    peerRequest.fileHash = link.link.hash;
    peerRequest.fileSize = link.link.size;
    peerRequest.partPath = partPath;
    peerRequest.timeoutMillis = request.timeoutMillis;
    if (progress) {
        try {
            progress(0, link.link.size);
        } catch (...) {
        }
    }

    // Try the candidates in order. Parts a failed attempt finished stay in the
    // part file and its records, so the next peer continues instead of starting
    // over.
    Ed2kPeerResult transfer;
    std::string lastError = "no peer was tried";
    for (const auto& candidate : candidates) {
        if (cancelled.load()) {
            result.error = "cancelled";
            return result;
        }
        peerRequest.host = candidate.host;
        peerRequest.port = candidate.port;
        transfer = downloadFromEd2kPeer(peerRequest, cancelled);
        if (transfer.ok) break;
        lastError = transfer.error;
        if (lastError == "cancelled") {
            result.error = lastError;
            result.received = transfer.received;
            return result;
        }
    }
    if (!transfer.ok) {
        result.error = lastError;
        result.received = transfer.received;
        return result;
    }

    // The part file is byte-identical to the finished file; the rename is what
    // makes a download visible.
    std::filesystem::rename(partPath, request.destination, error);
    if (error) {
        result.error = "cannot move the finished file into place: " + error.message();
        return result;
    }
    std::filesystem::remove(partPath + ".ed2kmeta", error);
    if (progress) {
        try {
            progress(link.link.size, link.link.size);
        } catch (...) {
        }
    }
    result.ok = true;
    result.received = link.link.size;

    // Sharing keeps the task busy after the bytes are on disk: the same file is
    // served to other peers until the budget ends or the task is cancelled.
    if (request.shareMinutes > 0) {
        Ed2kPeerServer server(request.destination, link.link.hash, request.sharePort);
        std::string shareError;
        if (server.start(shareError)) {
            if (request.onSharing) request.onSharing(server.port());
            if (request.publish)
                request.publish(link.link.hash, "127.0.0.1", server.port());
            const auto deadline = std::chrono::steady_clock::now()
                + std::chrono::minutes(request.shareMinutes);
            while (!cancelled.load() && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            server.stop();
            result.uploaded = server.uploadedBytes();
        } else {
            result.error = "cannot share the finished file: " + shareError;
            result.ok = false;
        }
    }
    return result;
}

} // namespace litefetch
