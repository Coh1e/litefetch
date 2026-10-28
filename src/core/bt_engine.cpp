#include "bt_engine.h"

#include "text_utf8.h"

#include <libtorrent/alert_types.hpp>
#include <libtorrent/download_priority.hpp>
#include <libtorrent/error_code.hpp>
#include <libtorrent/load_torrent.hpp>
#include <libtorrent/magnet_uri.hpp>
#include <libtorrent/session.hpp>
#include <libtorrent/session_params.hpp>
#include <libtorrent/settings_pack.hpp>
#include <libtorrent/address.hpp>
#include <libtorrent/socket.hpp>
#include <libtorrent/torrent_info.hpp>

#include <chrono>
#include <filesystem>
#include <memory>
#include <thread>

namespace litefetch {
namespace {
bool isTorrentFile(const std::string& source)
{
    return source.size() > 8 && (source.ends_with(".torrent") || source.ends_with(".TORRENT"));
}

bool isMagnet(const std::string& source)
{
    return source.rfind("magnet:", 0) == 0;
}

lt::settings_pack localSettings(const std::string& listenInterface)
{
    lt::settings_pack pack;
    // A first local-only profile: no DHT/LSD/UPnP traffic until the ticket that
    // owns discovery. Peers come from trackers, the torrent or an explicit link.
    pack.set_bool(lt::settings_pack::enable_dht, false);
    pack.set_bool(lt::settings_pack::enable_lsd, false);
    pack.set_bool(lt::settings_pack::enable_upnp, false);
    pack.set_bool(lt::settings_pack::enable_natpmp, false);
    pack.set_str(lt::settings_pack::listen_interfaces, listenInterface);
    pack.set_bool(lt::settings_pack::allow_multiple_connections_per_ip, true);
    return pack;
}

std::string infoHashHex(const lt::info_hash_t& hash)
{
    return hash.has_v1() ? hash.v1.to_string() : hash.v2.to_string();
}

std::vector<BtFile> filesOf(const lt::torrent_info& info)
{
    std::vector<BtFile> files;
    const auto& storage = info.layout();
    files.reserve(static_cast<std::size_t>(storage.num_files()));
    for (int index = 0; index < storage.num_files(); ++index) {
        BtFile file;
        file.index = index;
        file.path = storage.file_path(lt::file_index_t{index});
        file.size = static_cast<std::int64_t>(storage.file_size(lt::file_index_t{index}));
        files.push_back(std::move(file));
    }
    return files;
}

BtMetadata metadataOf(const lt::torrent_info& info)
{
    BtMetadata metadata;
    metadata.name = info.name();
    metadata.infoHash = infoHashHex(info.info_hashes());
    metadata.totalBytes = info.total_size();
    metadata.files = filesOf(info);
    metadata.isPrivate = info.priv();
    return metadata;
}

// Waits until libtorrent reports metadata or the deadline passes.
bool waitForMetadata(lt::session& session, lt::torrent_handle& handle, int timeoutMillis,
    const std::atomic_bool& cancelled)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMillis);
    while (std::chrono::steady_clock::now() < deadline) {
        if (cancelled.load()) return false;
        if (handle.is_valid() && handle.status().has_metadata) return true;
        session.wait_for_alert(std::chrono::milliseconds(100));
        std::vector<lt::alert*> alerts;
        session.pop_alerts(&alerts);
        for (const auto* alert : alerts) {
            if (alert->type() == lt::torrent_error_alert::alert_type) return false;
        }
    }
    return handle.is_valid() && handle.status().has_metadata;
}
} // namespace

BtMetadataResult btFetchMetadata(const std::string& source, const std::string& workingDirectory,
    int timeoutMillis, const std::atomic_bool& cancelled)
{
    BtMetadataResult result;
    if (!isMagnet(source) && !isTorrentFile(source)) {
        result.error = "source is not a magnet link or a .torrent file";
        return result;
    }
    if (cancelled.load()) {
        result.error = "cancelled";
        return result;
    }
    try {
        lt::session session(lt::session_params(localSettings("127.0.0.1:0")));
        lt::add_torrent_params params;
        if (isMagnet(source)) {
            params = lt::parse_magnet_uri(source);
        } else {
            params = lt::load_torrent_file(source);
        }
        params.save_path = workingDirectory;
        auto handle = session.add_torrent(std::move(params));
        if (!waitForMetadata(session, handle, timeoutMillis, cancelled)) {
            result.error = "timed out waiting for torrent metadata";
            return result;
        }
        const auto info = handle.torrent_file();
        if (!info) {
            result.error = "torrent metadata is unavailable";
            return result;
        }
        result.ok = true;
        result.metadata = metadataOf(*info);
        return result;
    } catch (const std::exception& error) {
        result.error = error.what();
        return result;
    }
}

BtDownloadResult btDownload(const BtDownloadRequest& request, const std::atomic_bool& cancelled,
    const std::function<void(const BtProgressInfo&)>& progress)
{
    BtDownloadResult result;
    if (!isMagnet(request.source) && !isTorrentFile(request.source)) {
        result.error = "source is not a magnet link or a .torrent file";
        return result;
    }
    std::error_code error;
    std::filesystem::create_directories(request.savePath, error);
    if (error) {
        result.error = "cannot create the save directory: " + error.message();
        return result;
    }
    try {
        lt::session session(lt::session_params(localSettings(request.listenInterface)));
        lt::add_torrent_params params;
        if (isMagnet(request.source)) {
            params = lt::parse_magnet_uri(request.source);
        } else {
            params = lt::load_torrent_file(request.source);
        }
        params.save_path = request.savePath;
        auto handle = session.add_torrent(std::move(params));
        if (!waitForMetadata(session, handle, 60000, cancelled)) {
            if (cancelled.load()) {
                result.cancelled = true;
                return result;
            }
            result.error = "timed out waiting for torrent metadata";
            return result;
        }

        // Only the confirmed files get a download priority; everything else is
        // left out instead of downloading first and deleting later.
        const auto info = handle.torrent_file();
        if (!info) {
            result.error = "torrent metadata is unavailable";
            return result;
        }
        const int fileCount = info->num_files();
        std::vector<lt::download_priority_t> priorities(static_cast<std::size_t>(fileCount),
            lt::download_priority_t{0});
        if (request.choices.empty()) {
            std::fill(priorities.begin(), priorities.end(), lt::default_priority);
        } else {
            for (const auto& choice : request.choices) {
                if (choice.index < 0 || choice.index >= fileCount) continue;
                // 0 skips, 1 downloads normally, 2 downloads ahead of the rest.
                priorities[static_cast<std::size_t>(choice.index)] = choice.priority >= 2
                    ? lt::top_priority
                    : (choice.priority <= 0 ? lt::download_priority_t{0} : lt::default_priority);
            }
        }
        handle.prioritize_files(priorities);

        if (!request.trackers.empty()) {
            if (info->priv()) {
                result.error = "a private torrent does not accept additional trackers";
                return result;
            }
            for (const auto& tracker : request.trackers)
                handle.add_tracker(lt::announce_entry{tracker});
        }

        for (const auto& peer : request.peers) {
            const auto colon = peer.rfind(':');
            if (colon == std::string::npos) continue;
            const auto portText = peer.substr(colon + 1);
            if (portText.empty()) continue;
            int port = 0;
            try {
                port = std::stoi(portText);
            } catch (...) {
                continue;
            }
            lt::error_code endpointError;
            const auto address = lt::make_address(peer.substr(0, colon), endpointError);
            if (endpointError) continue;
            handle.connect_peer(lt::tcp::endpoint(address, static_cast<unsigned short>(port)));
        }

        auto lastReport = std::chrono::steady_clock::now();
        while (true) {
            if (cancelled.load()) {
                session.pause();
                result.cancelled = true;
                return result;
            }
            const auto status = handle.status();
            result.received = status.total_wanted_done;
            if (progress) {
                BtProgressInfo update;
                update.received = status.total_wanted_done;
                update.total = status.total_wanted;
                update.peers = status.num_peers;
                update.rateBytesPerSecond = static_cast<double>(status.download_rate);
                try {
                    progress(update);
                } catch (...) {
                }
            }
            if (status.total_wanted > 0 && status.total_wanted_done >= status.total_wanted) {
                result.ok = true;
                return result;
            }
            if (status.state == lt::torrent_status::finished
                || status.state == lt::torrent_status::seeding) {
                result.ok = true;
                return result;
            }
            session.wait_for_alert(std::chrono::milliseconds(200));
            (void)lastReport;
        }
    } catch (const std::exception& exception) {
        result.error = exception.what();
        return result;
    }
}

} // namespace litefetch