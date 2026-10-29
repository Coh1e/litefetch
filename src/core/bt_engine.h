#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace litefetch {

// One file inside a torrent.
struct BtFile {
    int index = 0;
    std::string path; // relative path as published by the torrent
    std::int64_t size = 0;
};

// What the metadata says about a torrent. No content has been written yet.
struct BtMetadata {
    std::string name;
    std::string infoHash;
    std::int64_t totalBytes = 0;
    std::vector<BtFile> files;
    std::vector<std::string> trackers;
    // A private torrent forbids adding trackers outside its own list.
    bool isPrivate = false;
};

// Per-file intent. 0 skips the file, 1 downloads it normally, 2 downloads it
// ahead of the others.
struct BtFileChoice {
    int index = 0;
    int priority = 1;
};

struct BtMetadataResult {
    bool ok = false;
    BtMetadata metadata;
    std::string error;
};

// A magnet link or a .torrent file path. Fetches metadata only; the caller
// shows the file list and confirms the selection before any content moves.
BtMetadataResult btFetchMetadata(const std::string& source, const std::string& workingDirectory,
    int timeoutMillis, const std::atomic_bool& cancelled);

// What to do once every wanted piece is on disk. 0 stops, 1 seeds until the
// upload/download ratio is reached, 2 seeds for a number of minutes.
struct BtSeedPolicy {
    int mode = 0;
    double ratioLimit = 1.0;
    int minutesLimit = 0;
};

struct BtDownloadRequest {
    std::string source;      // magnet link or .torrent path
    std::string savePath;    // directory that receives the torrent's files
    std::vector<BtFileChoice> choices; // empty means every file at normal priority
    std::vector<std::string> trackers; // extra trackers from the user; refused when private
    std::string listenInterface = "127.0.0.1:0";
    // Per-session byte limits handed to libtorrent. The shared HTTP/ed2k budget
    // cannot be enforced inside libtorrent, so the same number is applied here
    // as its own cap; the service documents that difference.
    std::int64_t downloadRateLimit = 0;
    std::int64_t uploadRateLimit = 0;
    // Extra peers ("host:port") to connect immediately, used by the local swarm
    // tests and by the explicit peer entry a later ticket will surface.
    std::vector<std::string> peers;
    // Where fast-resume data lives. It lets a restart continue without a full
    // recheck, and it is rewritten when the transfer stops.
    std::string resumePath;
    // Recheck every piece before trusting what is on disk. This is how damaged
    // data is found and repaired; a plain restart trusts the resume data.
    bool forceVerify = false;
    BtSeedPolicy seedPolicy;
};

struct BtProgressInfo {
    std::int64_t received = 0;
    std::int64_t total = -1;
    std::int32_t peers = 0;
    std::int32_t seeds = 0;
    double rateBytesPerSecond = 0.0;
    std::int64_t uploaded = 0;
    double uploadBytesPerSecond = 0.0;
    bool seeding = false;
    // Loopback port the session listens on; 0 until the socket is open.
    std::int32_t listenPort = 0;
};

struct BtDownloadResult {
    bool ok = false;
    bool cancelled = false;
    std::string error;
    std::int64_t received = 0;
    std::int64_t uploaded = 0;
    bool seeding = false;
};

// Blocking transfer on the calling thread, like the HTTP engine. Only the
// selected files are downloaded; the rest get zero priority.
BtDownloadResult btDownload(const BtDownloadRequest& request, const std::atomic_bool& cancelled,
    const std::function<void(const BtProgressInfo&)>& progress = {});

} // namespace litefetch
