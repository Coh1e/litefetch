#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace litefetch {

enum class TransferStatus { Completed, Cancelled, Failed };
enum class TransferError { None, InvalidRequest, Destination, Network, Http, Disk, Internal };

struct TransferRequest {
    std::string source;
    std::string destination;
    // The user confirmed starting over after the server refused to resume or
    // the resource changed. Without it a refused resume keeps the bytes on disk.
    bool restartFromZero = false;
    // Site credentials for this transfer only. They are never persisted, never
    // written to the resume sidecar and never sent to another host.
    std::string username;
    std::string password;
};

struct TransferProgressInfo {
    std::int64_t received = 0;
    std::int64_t total = -1;
};

struct TransferResult {
    TransferStatus status = TransferStatus::Failed;
    TransferError error = TransferError::None;
    std::string message;
    long responseCode = 0;
    std::int64_t received = 0;
    std::int64_t total = -1;
    // The transfer stopped because the partial data could not be resumed: the
    // server answered with the whole resource (200) or the validator changed.
    // Partial bytes are kept; the caller must ask the user before restarting.
    bool restartRequired = false;
    // Bytes already on disk for this destination (partial data), valid after
    // any outcome so the caller can persist real progress.
    std::int64_t partialBytes = 0;
};

// Partial data for a destination lives in a stable sibling file plus a small
// metadata sidecar, so a crashed or cancelled transfer can resume later.
std::filesystem::path partialPathFor(const std::filesystem::path& destination);
std::filesystem::path partialMetaPathFor(const std::filesystem::path& destination);
// Removes the partial data and its metadata (used after a confirmed restart).
bool discardPartial(const std::filesystem::path& destination);

// Blocking HTTP(S) engine boundary. Progress runs on the calling thread. A
// failed or cancelled transfer never publishes a destination file, and keeps
// its partial data for a later resume.
using TransferFunction = std::function<TransferResult(const TransferRequest&,
    const std::atomic_bool&, const std::function<void(const TransferProgressInfo&)>&)>;

TransferResult transferHttp(const TransferRequest& request, const std::atomic_bool& cancelled,
    const std::function<void(const TransferProgressInfo&)>& progress = {});

} // namespace litefetch
