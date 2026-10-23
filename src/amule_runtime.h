#pragma once
#include <QString>
#include <QByteArray>
#include <atomic>
#include <thread>
namespace downloader {
struct AmuleRuntimeOptions { QString binaries, stateDirectory; };
struct AmuleConnection { quint16 port = 0; QByteArray password; };
// Owns one isolated daemon/API pair. Startup blocks: construct off the GUI
// thread. QProcess objects stay entirely on the supervisor's dedicated thread.
class AmuleRuntime {
public:
    explicit AmuleRuntime(AmuleRuntimeOptions options);
    ~AmuleRuntime();
    AmuleRuntime(const AmuleRuntime&) = delete;
    AmuleRuntime& operator=(const AmuleRuntime&) = delete;
    const AmuleConnection& connection() const { return connection_; }
    bool running() const { return running_.load(); }
private:
    AmuleConnection connection_;
    std::atomic_bool running_{false};
    std::jthread supervisor_;
};
}
