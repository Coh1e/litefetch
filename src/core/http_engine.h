#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

namespace litefetch {

enum class TransferStatus { Completed, Cancelled, Failed };
enum class TransferError { None, InvalidRequest, Destination, Network, Http, Disk, Internal };

struct TransferRequest {
    std::string source;
    std::string destination;
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
};

// Blocking HTTP(S) engine boundary. The destination must not exist. Progress
// runs on the calling thread. A failed or cancelled transfer never publishes a
// destination file.
using TransferFunction = std::function<TransferResult(const TransferRequest&,
    const std::atomic_bool&, const std::function<void(const TransferProgressInfo&)>&)>;

TransferResult transferHttp(const TransferRequest& request, const std::atomic_bool& cancelled,
    const std::function<void(const TransferProgressInfo&)>& progress = {});

} // namespace litefetch
