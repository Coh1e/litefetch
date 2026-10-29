#include "settings.h"

#include "task_store.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdio>

namespace litefetch {
namespace {
constexpr const char* speedLimitKey = "network.speed_limit";
constexpr const char* proxyKey = "network.http_proxy";
constexpr const char* connectTimeoutKey = "network.connect_timeout";
constexpr const char* ed2kServersKey = "ed2k.servers";
constexpr const char* renameOnConflictKey = "download.rename_on_conflict";
constexpr const char* saveByCategoryKey = "download.save_by_category";

std::int64_t parseInt64(const std::string& text, std::int64_t fallback)
{
    if (text.empty()) return fallback;
    std::int64_t value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} ? value : fallback;
}

std::vector<std::string> splitLines(const std::string& text)
{
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start < text.size()) {
        const auto end = text.find('\n', start);
        auto line = text.substr(start,
            end == std::string::npos ? std::string::npos : end - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) lines.push_back(line);
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return lines;
}
} // namespace

// The folder a file belongs to when categorized saving is on.
std::string categoryForFile(const std::string& fileName)
{
    const auto dot = fileName.find_last_of('.');
    std::string extension;
    if (dot != std::string::npos)
        for (std::size_t index = dot + 1; index < fileName.size(); ++index)
            extension.push_back(static_cast<char>(
                std::tolower(static_cast<unsigned char>(fileName[index]))));
    static const std::vector<std::string> video{"mp4", "mkv", "avi", "mov", "wmv", "flv", "webm", "ts"};
    static const std::vector<std::string> audio{"mp3", "flac", "wav", "aac", "ogg", "m4a"};
    static const std::vector<std::string> image{"jpg", "jpeg", "png", "gif", "bmp", "webp"};
    static const std::vector<std::string> document{"pdf", "doc", "docx", "xls", "xlsx", "ppt", "pptx",
        "txt", "md", "epub"};
    static const std::vector<std::string> archive{"zip", "rar", "7z", "tar", "gz", "xz"};
    const auto has = [&](const std::vector<std::string>& list) {
        return std::find(list.begin(), list.end(), extension) != list.end();
    };
    if (has(video)) return "视频";
    if (has(audio)) return "音频";
    if (has(image)) return "图片";
    if (has(document)) return "文档";
    if (has(archive)) return "压缩包";
    return "其他";
}

bool parseServerEndpoint(const std::string& text, std::string& host, std::uint16_t& port)
{
    const auto colon = text.rfind(':');
    if (colon == std::string::npos || colon == 0) return false;
    const auto portText = text.substr(colon + 1);
    int value = 0;
    const auto result = std::from_chars(portText.data(), portText.data() + portText.size(), value);
    if (result.ec != std::errc{} || value <= 0 || value > 65535) return false;
    host = text.substr(0, colon);
    port = static_cast<std::uint16_t>(value);
    return true;
}

Settings loadSettings(const TaskRepository& repository)
{
    Settings settings;
    settings.speedLimitBytesPerSecond = parseInt64(repository.metaValue(speedLimitKey), 0);
    if (settings.speedLimitBytesPerSecond < 0) settings.speedLimitBytesPerSecond = 0;
    settings.httpProxy = repository.metaValue(proxyKey);
    settings.connectTimeoutSeconds =
        static_cast<int>(parseInt64(repository.metaValue(connectTimeoutKey), 30));
    if (settings.connectTimeoutSeconds < 5) settings.connectTimeoutSeconds = 5;
    settings.ed2kServers = splitLines(repository.metaValue(ed2kServersKey));
    settings.renameOnConflict = repository.metaValue(renameOnConflictKey) == "1";
    settings.saveByCategory = repository.metaValue(saveByCategoryKey) == "1";
    return settings;
}

void saveSettings(TaskRepository& repository, const Settings& settings)
{
    repository.setMetaValue(speedLimitKey, std::to_string(settings.speedLimitBytesPerSecond));
    repository.setMetaValue(proxyKey, settings.httpProxy);
    repository.setMetaValue(connectTimeoutKey, std::to_string(settings.connectTimeoutSeconds));
    std::string servers;
    for (const auto& server : settings.ed2kServers) {
        if (!servers.empty()) servers += '\n';
        servers += server;
    }
    repository.setMetaValue(ed2kServersKey, servers);
    repository.setMetaValue(renameOnConflictKey, settings.renameOnConflict ? "1" : "0");
    repository.setMetaValue(saveByCategoryKey, settings.saveByCategory ? "1" : "0");
}

} // namespace litefetch
