#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace litefetch {

// User settings that change behaviour of the network engines. They live in the
// database's key/value table so a portable run keeps everything together.
struct Settings {
    // Bytes per second, 0 = unlimited.
    std::int64_t speedLimitBytesPerSecond = 0;
    // HTTP proxy for HTTP/FTP transfers, empty = direct. BT and ed2k have their
    // own transports and are not covered by this value yet.
    std::string httpProxy;
    // Seconds before a stalled transfer is given up.
    int connectTimeoutSeconds = 30;
    // What to do when the destination already exists: false refuses (default),
    // true picks "name (1).ext" instead of failing.
    bool renameOnConflict = false;
    // Save single-file downloads into a per-category subfolder (视频/音频/...).
    bool saveByCategory = false;
    // How many transfers may run at once; 0 means no limit.
    int maxConcurrentDownloads = 0;
    // Back off to one transfer when nothing happened for this long; 0 disables.
    int lowUsageIdleSeconds = 0;
    // ed2k servers, "host:port" each.
    std::vector<std::string> ed2kServers;
};

// Reads settings from the task database; missing keys keep the defaults.
class TaskRepository;
Settings loadSettings(const TaskRepository& repository);

// Writes settings into the same database.
void saveSettings(TaskRepository& repository, const Settings& settings);

// Folder name a file belongs to when categorized saving is on.
std::string categoryForFile(const std::string& fileName);

// "host:port" parsing shared by the settings file and the server list.
bool parseServerEndpoint(const std::string& text, std::string& host, std::uint16_t& port);

} // namespace litefetch
