// BitTorrent boundary tests: create a torrent, seed it from a second local
// session, then fetch metadata and download only the confirmed file.
//
// winsock2 must precede windows.h (pulled in by test_support.h), otherwise the
// winsock 1 headers win and asio fails to compile.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include "bt_engine.h"
#include "test_support.h"

#include <libtorrent/add_torrent_params.hpp>
#include <libtorrent/bencode.hpp>
#include <libtorrent/entry.hpp>
#include <libtorrent/address.hpp>
#include <libtorrent/create_torrent.hpp>
#include <libtorrent/file_storage.hpp>
#include <libtorrent/load_torrent.hpp>
#include <libtorrent/session.hpp>
#include <libtorrent/settings_pack.hpp>
#include <libtorrent/socket.hpp>
#include <libtorrent/torrent_info.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdio>
#include <thread>
#include <thread>
#include <cstdio>
#include <cstdio>
#include <atomic>
#include <chrono>
#include <thread>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

using namespace litefetch;
using namespace litefetch::test;

namespace {

lt::settings_pack localSettings()
{
    lt::settings_pack pack;
    pack.set_bool(lt::settings_pack::enable_dht, false);
    pack.set_bool(lt::settings_pack::enable_lsd, false);
    pack.set_bool(lt::settings_pack::enable_upnp, false);
    pack.set_bool(lt::settings_pack::enable_natpmp, false);
    pack.set_str(lt::settings_pack::listen_interfaces, "127.0.0.1:0");
    pack.set_bool(lt::settings_pack::allow_multiple_connections_per_ip, true);
    return pack;
}

enum class TorrentFlavour { v1, hybrid, v2 };

std::vector<char> buildTorrent(const std::filesystem::path& seedDirectory,
    TorrentFlavour flavour = TorrentFlavour::v1)
{
    std::vector<lt::create_file_entry> entries;
    entries.push_back(lt::create_file_entry{"data/keep.bin", 64 * 1024});
    entries.push_back(lt::create_file_entry{"data/skip.bin", 32 * 1024});
    const auto flags = flavour == TorrentFlavour::v2 ? lt::create_torrent::v2_only
        : (flavour == TorrentFlavour::v1 ? lt::create_torrent::v1_only
                                         : lt::create_flags_t{});
    lt::create_torrent creator(std::move(entries), 16 * 1024, flags);
    lt::set_piece_hashes(creator, seedDirectory.string());
    // generate_buf() returns the complete bencoded .torrent document.
    const auto buffer = creator.generate_buf();
    return std::vector<char>(buffer.begin(), buffer.end());
}

// Upload: our completed download keeps seeding while a second session fetches
// the same torrent from us, so bytes really leave the process to a peer.
void uploadToPeer(const std::vector<char>& torrentBytes, const std::filesystem::path& torrentPath,
    const std::filesystem::path& downloadDirectory, const TempDir& root, std::uint16_t seedPort)
{
    const auto secondDirectory = root.path() / "second";
    std::atomic_bool seedingCancelled{false};
    std::atomic_bool seeding{false};
    std::atomic<long long> seedUploaded{0};
    std::int32_t listenPort = 0;
    BtDownloadRequest seedRequest;
    seedRequest.source = torrentPath.string();
    seedRequest.savePath = downloadDirectory.string();
    seedRequest.choices = {BtFileChoice{0, 1}};
    seedRequest.peers = {"127.0.0.1:" + std::to_string(seedPort)};
    // Keep seeding well past the leecher window so a slow machine cannot end
    // the share before the second peer finishes.
    seedRequest.seedPolicy = BtSeedPolicy{2, 1.0, 2};
    BtDownloadResult seedResult;
    std::thread seederThread([&] {
        seedResult = btDownload(seedRequest, seedingCancelled, [&](const BtProgressInfo& update) {
            if (update.listenPort != 0) listenPort = update.listenPort;
            if (update.seeding) seeding = true;
            if (update.uploaded > seedUploaded.load()) seedUploaded.store(update.uploaded);
        });
    });

    // Wait for the seeding session, then fetch from it. Everything that could
    // throw happens after the thread is joined.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!seeding.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const bool seedingUp = seeding.load();

    bool leeched = false;
    if (seedingUp && listenPort != 0) {
        lt::settings_pack probeSettings;
        probeSettings.set_str(lt::settings_pack::listen_interfaces, "127.0.0.1:0");
        probeSettings.set_bool(lt::settings_pack::enable_dht, false);
        probeSettings.set_bool(lt::settings_pack::enable_lsd, false);
        lt::session leecher{lt::session_params(probeSettings)};
        lt::add_torrent_params leechParams = lt::load_torrent_buffer(
            {torrentBytes.data(), static_cast<int>(torrentBytes.size())});
        leechParams.save_path = secondDirectory.string();
        const auto leechHandle = leecher.add_torrent(std::move(leechParams));
        // The seeder only holds the first file, so the leecher wants only that
        // file too; asking for the skipped one would wait for pieces nobody has.
        leechHandle.prioritize_files({lt::default_priority, lt::download_priority_t{0}});
        const auto leechDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (std::chrono::steady_clock::now() < leechDeadline) {
            leechHandle.connect_peer(lt::tcp::endpoint(lt::make_address("127.0.0.1"),
                static_cast<unsigned short>(listenPort)));
            leecher.wait_for_alert(std::chrono::milliseconds(500));
            const auto status = leechHandle.status();
            if (status.total_wanted > 0 && status.total_wanted_done >= status.total_wanted) {
                leeched = true;
                break;
            }
        }
    }

    // The seeder accounts uploaded bytes asynchronously; wait for the counter
    // before stopping instead of racing it.
    const auto uploadDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (seedUploaded.load() == 0 && std::chrono::steady_clock::now() < uploadDeadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

    seedingCancelled.store(true);
    seederThread.join();

    check(seedingUp, "the completed task kept seeding instead of stopping");
    check(listenPort != 0, "the seeding session reported its listen port");
    check(leeched, "a second peer downloaded the torrent from us");
    check(seedUploaded.load() > 0 || seedResult.uploaded > 0,
        "the seeding task counted uploaded bytes");
    check(readFile(secondDirectory / "data" / "keep.bin") == payload(64 * 1024),
        "the uploaded bytes match the original file");
}
void bitTorrentRoundTrip(TorrentFlavour flavour)
{
    TempDir root;
    const auto seedDirectory = root.path() / "seed";
    std::filesystem::create_directories(seedDirectory / "data");
    const auto keep = payload(64 * 1024);
    auto skip = payload(32 * 1024);
    for (auto& byte : skip) byte = static_cast<unsigned char>(byte ^ 0x5a);
    writeFile(seedDirectory / "data" / "keep.bin", keep);
    writeFile(seedDirectory / "data" / "skip.bin", skip);

    const auto torrentBytes = buildTorrent(seedDirectory, flavour);
    {
        // The fixture must carry the hashes the flavour promises, otherwise the
        // rest of the test would silently exercise the wrong thing.
        const auto probe = lt::load_torrent_buffer(
            {torrentBytes.data(), static_cast<int>(torrentBytes.size())});
        check(probe.ti->info_hashes().has_v2() == (flavour != TorrentFlavour::v1),
            "fixture v2 hash presence matches the flavour");
        check(probe.ti->info_hashes().has_v1() == (flavour != TorrentFlavour::v2),
            "fixture v1 hash presence matches the flavour");
    }
    const auto torrentPath = root.file("fixture.torrent");
    {
        std::ofstream output(torrentPath, std::ios::binary | std::ios::trunc);
        output.write(torrentBytes.data(), static_cast<std::streamsize>(torrentBytes.size()));
    }

    // Seed the fixture from a second session on loopback.
    lt::session seeder{lt::session_params(localSettings())};
    lt::add_torrent_params seedParams = lt::load_torrent_buffer(torrentBytes);
    seedParams.save_path = seedDirectory.string();
    seedParams.flags |= lt::torrent_flags::seed_mode;
    const auto seedHandle = seeder.add_torrent(std::move(seedParams));
    // The session binds asynchronously, so asking once right after construction
    // is a race that shows up on a loaded machine.
    std::uint16_t seedPort = 0;
    for (int attempt = 0; attempt < 100 && seedPort == 0; ++attempt) {
        seedPort = seeder.listen_port();
        if (seedPort == 0) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    check(seedPort != 0, "seeder listens on a loopback port");
    seeder.wait_for_alert(std::chrono::milliseconds(500));

    // Metadata only: the file list is what the selection dialog shows.
    std::atomic_bool cancelled{false};
    const auto metadata = btFetchMetadata(torrentPath.string(), (root.path() / "meta").string(),
        10000, cancelled);
    check(metadata.ok, ("metadata fetch failed: " + metadata.error).c_str());
    check(metadata.metadata.files.size() == 2, "metadata lists both files");
    check(metadata.metadata.totalBytes == 96 * 1024, "metadata reports the total size");
    check(metadata.metadata.files[0].path.find("keep.bin") != std::string::npos, "first file path");
    check(metadata.metadata.files[1].size == 32 * 1024, "second file size");
    check(!metadata.metadata.infoHash.empty(), "info hash is reported");

    // Download only the first file, with the seeder as the explicit peer.
    const auto downloadDirectory = root.path() / "download";
    BtDownloadRequest request;
    request.source = torrentPath.string();
    request.savePath = downloadDirectory.string();
    request.choices = {BtFileChoice{0, 1}};
    request.peers = {"127.0.0.1:" + std::to_string(seedPort)};
    std::int64_t lastReceived = 0;
    int maxPeers = 0;
    const auto result = btDownload(request, cancelled, [&](const BtProgressInfo& progress) {
        lastReceived = progress.received;
        maxPeers = (std::max)(maxPeers, static_cast<int>(progress.peers));
    });
    check(result.ok, ("torrent download failed: " + result.error).c_str());
    check(lastReceived > 0, "progress reported received bytes");
    check(maxPeers > 0, "progress reported the connected peer");
    check(readFile(downloadDirectory / "data" / "keep.bin") == keep,
        "downloaded file matches the seeded content");
    std::error_code error;
    const auto skipped = std::filesystem::file_size(downloadDirectory / "data" / "skip.bin", error);
    check(error || skipped == 0, "the unselected file was not downloaded");

    // Damage repair: corrupt a byte, then verify. The recheck must notice the
    // bad piece and fetch it again instead of reporting success.
    {
        auto damaged = readFile(downloadDirectory / "data" / "keep.bin");
        damaged[1000] = static_cast<char>(damaged[1000] ^ 0x7f);
        writeFile(downloadDirectory / "data" / "keep.bin", damaged);
    }
    BtDownloadRequest verifyRequest = request;
    verifyRequest.forceVerify = true;
    verifyRequest.resumePath = (root.path() / "repair.resume").string();
    const auto repaired = btDownload(verifyRequest, cancelled);
    check(repaired.ok, ("verify run failed: " + repaired.error).c_str());
    check(readFile(downloadDirectory / "data" / "keep.bin") == keep,
        "the corrupted piece was repaired from the peer");
    check(std::filesystem::exists(verifyRequest.resumePath),
        "the finished transfer left resume data behind");
    uploadToPeer(torrentBytes, torrentPath, downloadDirectory, root, seedPort);


}
} // namespace

int main()
{
    return run("bt", [] {
        bitTorrentRoundTrip(TorrentFlavour::v1);
        bitTorrentRoundTrip(TorrentFlavour::hybrid);
        bitTorrentRoundTrip(TorrentFlavour::v2);
        return 0;
    });
}