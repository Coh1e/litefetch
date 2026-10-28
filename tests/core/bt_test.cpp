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

std::vector<char> buildTorrent(const std::filesystem::path& seedDirectory)
{
    std::vector<lt::create_file_entry> entries;
    entries.push_back(lt::create_file_entry{"data/keep.bin", 64 * 1024});
    entries.push_back(lt::create_file_entry{"data/skip.bin", 32 * 1024});
    lt::create_torrent creator(std::move(entries), 16 * 1024);
    lt::set_piece_hashes(creator, seedDirectory.string());
    // generate_buf() returns the complete bencoded .torrent document.
    const auto buffer = creator.generate_buf();
    return std::vector<char>(buffer.begin(), buffer.end());
}

void bitTorrentRoundTrip()
{
    TempDir root;
    const auto seedDirectory = root.path() / "seed";
    std::filesystem::create_directories(seedDirectory / "data");
    const auto keep = payload(64 * 1024);
    auto skip = payload(32 * 1024);
    for (auto& byte : skip) byte = static_cast<unsigned char>(byte ^ 0x5a);
    writeFile(seedDirectory / "data" / "keep.bin", keep);
    writeFile(seedDirectory / "data" / "skip.bin", skip);

    const auto torrentBytes = buildTorrent(seedDirectory);
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
    const auto seedPort = seeder.listen_port();
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


}
} // namespace

int main()
{
    return run("bt", [] {
        bitTorrentRoundTrip();
        return 0;
    });
}