#include "ed2k_task.h"

#include "ed2k_transfer.h"

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

    Ed2kServerEndpoint peer = request.peer;
    if (peer.port == 0) {
        if (request.servers.empty()) {
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
            peer.host = query.sources.front().address;
            peer.port = query.sources.front().port;
            break;
        }
        if (peer.port == 0) {
            result.noSources = anyServerAnswered;
            result.error = anyServerAnswered ? "the servers know no sources for this file"
                                             : "no ed2k server answered";
            return result;
        }
    }

    const auto partPath = partPathFor(request.destination);
    const auto directory = std::filesystem::path(request.destination).parent_path();
    std::error_code error;
    if (!directory.empty()) std::filesystem::create_directories(directory, error);
    if (error) {
        result.error = "cannot create the destination directory: " + error.message();
        return result;
    }

    Ed2kPeerRequest peerRequest;
    peerRequest.host = peer.host;
    peerRequest.port = peer.port;
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
    const auto transfer = downloadFromEd2kPeer(peerRequest, cancelled);
    if (!transfer.ok) {
        result.error = transfer.error;
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
    return result;
}

} // namespace litefetch
