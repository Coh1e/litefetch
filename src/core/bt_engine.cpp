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

lt::settings_pack localSettings(const std::string& listenInterface,
    std::int64_t downloadRateLimit, std::int64_t uploadRateLimit)
{
    lt::settings_pack pack;
    if (downloadRateLimit > 0)
        pack.set_int(lt::settings_pack::download_rate_limit,
            static_cast<int>(downloadRateLimit));
    if (uploadRateLimit > 0)
        pack.set_int(lt::settings_pack::upload_rate_limit, static_cast<int>(uploadRateLimit));
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

template <typename Digest>
std::string hexOf(const Digest& digest)
{
    static const char* digits = "0123456789abcdef";
    std::string out;
    out.reserve(static_cast<std::size_t>(digest.size()) * 2);
    for (int index = 0; index < digest.size(); ++index) {
        const auto byte = static_cast<unsigned char>(digest[index]);
        out.push_back(digits[byte >> 4]);
        out.push_back(digits[byte & 0x0f]);
    }
    return out;
}

std::string infoHashHex(const lt::info_hash_t& hash)
{
    // The digest's own string form is the raw 20/32 bytes; a magnet link and the
    // property window both need hex, which is also what the torrent file holds.
    if (hash.has_v1()) return hexOf(hash.v1);
    return hexOf(hash.v2);
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

// Waits until libtorrent reports metadata or the deadline passes. The bytes the
// session pulled in getting there are handed back: they were spent before any
// piece existed, and they belong to the metadata bucket.
bool waitForMetadata(lt::session& session, lt::torrent_handle& handle, int timeoutMillis,
    const std::atomic_bool& cancelled, std::int64_t* bytesSpent = nullptr)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMillis);
    while (std::chrono::steady_clock::now() < deadline) {
        if (cancelled.load()) return false;
        if (handle.is_valid() && handle.status().has_metadata) break;
        session.wait_for_alert(std::chrono::milliseconds(100));
        std::vector<lt::alert*> alerts;
        session.pop_alerts(&alerts);
        for (const auto* alert : alerts) {
            if (alert->type() == lt::torrent_error_alert::alert_type) return false;
        }
    }
    const bool ready = handle.is_valid() && handle.status().has_metadata;
    if (ready && bytesSpent) *bytesSpent = handle.status().total_download;
    return ready;
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
        lt::session session(lt::session_params(localSettings("127.0.0.1:0", 0, 0)));
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
        lt::session session(lt::session_params(localSettings(request.listenInterface,
        request.downloadRateLimit, request.uploadRateLimit)));
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
        // Explicit peers are dialled before anything else: a magnet link has no
        // file list, and the peer that has it is the only way to get one. Waiting
        // for metadata first would sit until the timeout with a peer list unused.
        const auto connectPeers = [&](lt::torrent_handle& target) {
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
                target.connect_peer(lt::tcp::endpoint(address, static_cast<unsigned short>(port)));
            }
        };
        connectPeers(handle);
        std::int64_t metadataSoFar = 0;
        if (!waitForMetadata(session, handle, 60000, cancelled, &metadataSoFar)) {
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
        // A magnet link carries no description, so the info dictionary arrived
        // over the wire. The session's counters update on their own thread, so
        // they are polled briefly while no piece exists yet; if the first piece
        // beats the poll, the metadata stays uncounted rather than guessed.
        if (isMagnet(request.source) && metadataSoFar == 0) {
            for (int attempt = 0; attempt < 40; ++attempt) {
                const auto sample = handle.status();
                if (sample.total_payload_download > 0) break;
                if (sample.total_download > 0) {
                    metadataSoFar = sample.total_download;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(25));
            }
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
                // While no piece has completed, everything downloaded belongs to
                // the torrent's description; after that the count freezes so the
                // two buckets never overlap. These counters update asynchronously,
                // so they are sampled here rather than trusted once.
                if (status.total_wanted_done == 0 && status.total_download > metadataSoFar)
                    metadataSoFar = status.total_download;
                update.metadataBytes = metadataSoFar;
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