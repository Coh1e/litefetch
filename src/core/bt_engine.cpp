#include "bt_engine.h"

#include "text_utf8.h"

#include <libtorrent/alert_types.hpp>
#include <libtorrent/download_priority.hpp>
#include <libtorrent/error_code.hpp>
#include <libtorrent/load_torrent.hpp>
#include <libtorrent/magnet_uri.hpp>
#include <libtorrent/read_resume_data.hpp>
#include <libtorrent/session.hpp>
#include <libtorrent/session_params.hpp>
#include <libtorrent/settings_pack.hpp>
#include <libtorrent/address.hpp>
#include <libtorrent/socket.hpp>
#include <libtorrent/torrent_info.hpp>
#include <libtorrent/write_resume_data.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
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

// Asks libtorrent for fast-resume data and writes it, so the next start can
// continue instead of rechecking every piece.
void saveResumeData(lt::session& session, lt::torrent_handle& handle, const std::string& path)
{
    if (path.empty()) return;
    handle.save_resume_data(lt::torrent_handle::save_info_dict);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        session.wait_for_alert(std::chrono::milliseconds(100));
        std::vector<lt::alert*> alerts;
        session.pop_alerts(&alerts);
        for (auto* alert : alerts) {
            if (alert->type() == lt::save_resume_data_alert::alert_type) {
                auto* resume = static_cast<lt::save_resume_data_alert*>(alert);
                const auto buffer = lt::write_resume_data_buf(resume->params);
                std::ofstream output(path, std::ios::binary | std::ios::trunc);
                output.write(buffer.data(), static_cast<std::streamsize>(buffer.size()));
                return;
            }
            if (alert->type() == lt::save_resume_data_failed_alert::alert_type) return;
        }
    }
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
        std::vector<char> resumeData;
        if (!request.resumePath.empty()) {
            std::ifstream input(request.resumePath, std::ios::binary);
            if (input) {
                resumeData.assign(std::istreambuf_iterator<char>(input),
                    std::istreambuf_iterator<char>());
            }
        }
        if (!resumeData.empty()) {
            // Later libtorrent versions hand resume state back through
            // read_resume_data instead of a params field.
            auto resumed = lt::read_resume_data(
                {resumeData.data(), static_cast<int>(resumeData.size())});
            if (!resumed.ti) resumed.ti = params.ti;
            resumed.save_path = params.save_path;
            params = std::move(resumed);
        }
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
        if (request.forceVerify) handle.force_recheck();

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

        const auto startedAt = std::chrono::steady_clock::now();
        bool completed = false;
        while (true) {
            if (cancelled.load()) {
                // Report the counters from the moment of cancellation: the loop
                // may not have sampled them after the last uploaded piece.
                result.uploaded = handle.status().all_time_upload;
                result.received = handle.status().total_wanted_done;
                saveResumeData(session, handle, request.resumePath);
                session.pause();
                result.cancelled = true;
                return result;
            }
            const auto status = handle.status();
            result.received = status.total_wanted_done;
            result.uploaded = status.all_time_upload;

            // Everything wanted is on disk: either stop here or keep seeding
            // until the configured ratio or time budget is met.
            const bool done = (status.total_wanted > 0
                                  && status.total_wanted_done >= status.total_wanted)
                || status.state == lt::torrent_status::finished
                || status.state == lt::torrent_status::seeding;
            if (done && !completed) {
                completed = true;
                if (request.seedPolicy.mode == 0) {
                    result.uploaded = handle.status().all_time_upload;
                    result.ok = true;
                    result.uploaded = handle.status().all_time_upload;
                    saveResumeData(session, handle, request.resumePath);
                    return result;
                }
            }
            if (completed) {
                result.seeding = true;
                const double ratio = status.all_time_download > 0
                    ? static_cast<double>(status.all_time_upload) / status.all_time_download
                    : 0.0;
                const auto minutes = std::chrono::duration_cast<std::chrono::minutes>(
                    std::chrono::steady_clock::now() - startedAt).count();
                if ((request.seedPolicy.mode == 1 && ratio >= request.seedPolicy.ratioLimit)
                    || (request.seedPolicy.mode == 2
                        && minutes >= request.seedPolicy.minutesLimit)) {
                    result.ok = true;
                    result.uploaded = handle.status().all_time_upload;
                    saveResumeData(session, handle, request.resumePath);
                    return result;
                }
            }

            if (progress) {
                BtProgressInfo update;
                update.received = status.total_wanted_done;
                update.total = status.total_wanted;
                update.peers = status.num_peers;
                update.seeds = status.num_seeds;
                update.rateBytesPerSecond = static_cast<double>(status.download_rate);
                update.uploaded = status.all_time_upload;
                update.uploadBytesPerSecond = static_cast<double>(status.upload_rate);
                update.seeding = completed;
                update.listenPort = static_cast<std::int32_t>(session.listen_port());
                try {
                    progress(update);
                } catch (...) {
                }
            }
            session.wait_for_alert(std::chrono::milliseconds(200));
        }
    } catch (const std::exception& exception) {
        result.error = exception.what();
        return result;
    }
}

} // namespace litefetch