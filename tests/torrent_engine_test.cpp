#include "torrent_engine.h"
#include "task_manager.h"
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTextStream>
#include <QTimer>
#include <libtorrent/add_torrent_params.hpp>
#include <libtorrent/create_torrent.hpp>
#include <libtorrent/entry.hpp>
#include <libtorrent/ip_filter.hpp>
#include <libtorrent/load_torrent.hpp>
#include <libtorrent/magnet_uri.hpp>
#include <libtorrent/session.hpp>
#include <libtorrent/settings_pack.hpp>
#include <libtorrent/torrent_handle.hpp>
#include <libtorrent/torrent_status.hpp>
#include <future>
#include <thread>

namespace lt = libtorrent;
using namespace downloader;
namespace {
std::string utf8(const QString& value) { return value.toUtf8().toStdString(); }
QByteArray read(const QString& path) { QFile file(path); if (!file.open(QIODevice::ReadOnly)) return {}; return file.readAll(); }
bool write(const QString& path, const QByteArray& bytes) {
    QFile file(path); return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
bool waitUntil(const std::function<bool()>& predicate, int timeout = 10000) {
    QElapsedTimer timer; timer.start();
    while (!predicate() && timer.elapsed() < timeout) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return predicate();
}
struct Fixture {
    QString torrent, magnet, first, second;
    lt::torrent_handle seed;
};
}
int main(int argc, char** argv) try
{
    QCoreApplication app(argc, argv);
    QTemporaryDir directory(QCoreApplication::applicationDirPath() + "/torrent-tests-XXXXXX");
    if (!directory.isValid()) return 2;
    int failures = 0;
    auto check = [&](bool ok, const char* message) { if (!ok) { ++failures; QTextStream(stderr) << "FAIL: " << message << '\n'; } };
    QByteArray first(2 * 1024 * 1024, Qt::Uninitialized), second(first.size(), Qt::Uninitialized);
    for (qsizetype i = 0; i < first.size(); ++i) { first[i] = static_cast<char>(i * 31 % 251); second[i] = static_cast<char>(i * 17 % 239); }
    const auto seedRoot = directory.filePath("seed");
    if (!QDir().mkdir(seedRoot)) return 3;
    lt::settings_pack settings;
    settings.set_str(lt::settings_pack::listen_interfaces, "127.0.0.1:0");
    settings.set_str(lt::settings_pack::outgoing_interfaces, "127.0.0.1");
    settings.set_str(lt::settings_pack::dht_bootstrap_nodes, "");
    settings.set_bool(lt::settings_pack::enable_dht, false);
    settings.set_bool(lt::settings_pack::enable_lsd, false);
    settings.set_bool(lt::settings_pack::enable_upnp, false);
    settings.set_bool(lt::settings_pack::enable_natpmp, false);
    settings.set_bool(lt::settings_pack::enable_outgoing_utp, false);
    settings.set_bool(lt::settings_pack::enable_incoming_utp, false);
    settings.set_bool(lt::settings_pack::allow_multiple_connections_per_ip, true);
    settings.set_int(lt::settings_pack::upload_rate_limit, 2 * 1024 * 1024);
    settings.set_int(lt::settings_pack::unchoke_interval, 1);
    settings.set_int(lt::settings_pack::optimistic_unchoke_interval, 1);
    lt::session seeder(settings);
    lt::ip_filter peerClasses;
    peerClasses.add_rule(lt::make_address("0.0.0.0"), lt::make_address("255.255.255.255"), 1);
    peerClasses.add_rule(lt::make_address("::"), lt::make_address("ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff"), 1);
    seeder.set_peer_class_filter(peerClasses);
    lt::ip_filter allowed;
    allowed.add_rule(lt::make_address("0.0.0.0"), lt::make_address("255.255.255.255"), lt::ip_filter::blocked);
    allowed.add_rule(lt::make_address("::"), lt::make_address("ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff"), lt::ip_filter::blocked);
    allowed.add_rule(lt::make_address("127.0.0.0"), lt::make_address("127.255.255.255"), 0);
    seeder.set_ip_filter(allowed);
    if (!waitUntil([&] { return seeder.listen_port() != 0; })) return 4;
    QVector<Fixture> fixtures;
    int mode = 0;
    for (const auto flags : {lt::create_torrent::v1_only, lt::create_torrent::v2_only, lt::create_flags_t{}}) {
        const auto name = QString("payload-%1").arg(++mode);
        QDir().mkpath(QDir(seedRoot).filePath(name + "/sub"));
        const auto fileA = name + "/a.bin", fileB = name + "/sub/b.bin";
        if (!write(QDir(seedRoot).filePath(fileA), first) || !write(QDir(seedRoot).filePath(fileB), second)) return 5;
        std::vector<lt::create_file_entry> entries{{utf8(fileA), first.size()}, {utf8(fileB), second.size()}};
        lt::create_torrent generator(std::move(entries), 16 * 1024, flags);
        lt::set_piece_hashes(generator, utf8(seedRoot));
        const auto encoded = generator.generate_buf();
        const auto torrent = directory.filePath(name + ".torrent");
        if (!write(torrent, QByteArray(encoded.data(), static_cast<qsizetype>(encoded.size())))) return 6;
        auto params = lt::load_torrent_buffer(lt::span<char const>(encoded.data(), static_cast<int>(encoded.size())));
        const auto magnet = QString::fromStdString(lt::make_magnet_uri(params));
        params.save_path = utf8(seedRoot);
        params.flags &= ~(lt::torrent_flags::paused | lt::torrent_flags::auto_managed);
        auto handle = seeder.add_torrent(std::move(params));
        fixtures.append({torrent, magnet, fileA, fileB, handle});
        if (!waitUntil([&] { return handle.status().is_seeding; })) return 7;
        const auto preview = inspectTorrent(torrent);
        const auto* metadata = std::get_if<TorrentMetadata>(&preview);
        check(metadata && metadata->total == first.size() + second.size()
            && metadata->identities.size() == (mode == 3 ? 2 : 1), "metainfo preview has full v1/v2 identities and real sizes");
    }
    int sequence = 0;
    auto id = [&] { return QString::number(++sequence, 16).rightJustified(32, '0'); };
    auto request = [](const QString& source) { return std::get<DownloadRequest>(parseRequest(source)); };
    auto options = [&] { TorrentRunOptions value; value.peers.append({"127.0.0.1", seeder.listen_port()}); return value; };
    std::atomic_bool stopped{false};
    auto run = [&](TorrentEngine& engine, const DownloadRequest& input, const QString& output, const QString& taskId,
                   TorrentRunOptions opts, qint64 pauseAt = 0, std::atomic_bool* sawMetadata = nullptr) {
        stopped = false; QElapsedTimer timeout; timeout.start();
        const auto result = engine.run(input, output, taskId, stopped, [&](auto progress) {
            if (sawMetadata && progress.phase == TransferPhase::FetchingMetadata) sawMetadata->store(true);
            if ((pauseAt && progress.received >= pauseAt) || timeout.elapsed() > 20000) stopped = true;
        }, std::move(opts));
        QTextStream(stdout) << "BT result=" << static_cast<int>(result.status) << " bytes=" << result.received << " error=" << result.message << '\n';
        return result;
    };
    const bool selectionOnly = app.arguments().contains("--only-selection");
    if (!selectionOnly) {
    {
        TorrentEngine engine({true,512*1024,0});lt::session leecher(settings);
        leecher.set_ip_filter(allowed);leecher.set_peer_class_filter(peerClasses);
        const auto output=directory.filePath("ratio-seeding"),leecherRoot=directory.filePath("ratio-leecher");
        check(QDir().mkdir(leecherRoot),"create independent leecher output");
        auto seedOptions=options();seedOptions.seeding={0,100};
        lt::torrent_handle incoming;bool connected=false;qint64 observedUpload=-1,peakUploadRate=0;
        struct ResumeSource {lt::torrent_handle handle;~ResumeSource(){handle.resume();}} restore{fixtures[0].seed};
        stopped=false;QElapsedTimer overall;overall.start();
        const auto result=engine.run(request(fixtures[0].torrent),output,id(),stopped,[&](auto progress){
            observedUpload=qMax(observedUpload,progress.uploaded);peakUploadRate=qMax(peakUploadRate,progress.uploadBytesPerSecond);
            if(progress.phase==TransferPhase::Seeding&&!connected){
                fixtures[0].seed.pause();
                auto params=lt::load_torrent_file(utf8(fixtures[0].torrent));params.save_path=utf8(leecherRoot);
                params.flags&=~(lt::torrent_flags::paused|lt::torrent_flags::auto_managed);
                params.flags|=lt::torrent_flags::disable_pex;
                incoming=leecher.add_torrent(std::move(params));
                check(engine.listeningPort()!=0,"downloader exposes active listening port");
                incoming.connect_peer({lt::make_address("127.0.0.1"),engine.listeningPort()});connected=true;
            }
            if(overall.elapsed()>40000)stopped=true;
        },seedOptions);
        check(result.status==TransferStatus::Completed&&connected,"real upload reaches configured seeding ratio");
        check(observedUpload>=first.size()+second.size()&&result.uploaded>=first.size()+second.size()&&peakUploadRate>0,
            "actual upload counters and nonzero payload rate reach progress and final result");
        check(waitUntil([&]{return incoming.is_valid()&&incoming.status().is_finished;},5000)
            &&incoming.status().total_payload_download>=first.size()+second.size()
            &&read(QDir(leecherRoot).filePath(fixtures[0].first))==first
            &&read(QDir(leecherRoot).filePath(fixtures[0].second))==second,
            "independent leecher receives all exact payload from downloader with original source paused");
    }
    {
        TorrentEngine engine({true,0,0});auto settings=options();settings.seeding={1,100000};
        stopped=false;QElapsedTimer overall,seeding;overall.start();qint64 observedSeedingMs=0;
        const auto output=directory.filePath("timed-seeding");
        const auto result=engine.run(request(fixtures[0].torrent),output,id(),stopped,[&](auto progress){
            if(progress.phase==TransferPhase::Seeding){if(!seeding.isValid())seeding.start();observedSeedingMs=seeding.elapsed();}
            if(overall.elapsed()>15000)stopped=true;
        },settings);
        check(result.status==TransferStatus::Completed&&seeding.isValid()&&observedSeedingMs>=1000
            &&read(QDir(output).filePath(fixtures[0].first))==first,
            "completed BT remains active in seeding phase until configured time limit");
    }
    {
        TorrentEngine engine({true,0,0});engine.setRateLimits({64*1024,32*1024});
        const auto configured=engine.rateLimits();
        check(configured.download==64*1024&&configured.upload==32*1024,"live BT limits reach actual session settings");
        bool invalidRejected=false;try{engine.setRateLimits({-1,0});}catch(const std::invalid_argument&){invalidRejected=true;}
        check(invalidRejected&&engine.rateLimits().download==64*1024,"negative BT limit leaves session unchanged");
        stopped=false;QElapsedTimer overall,limited;overall.start();qint64 baseline=0;bool changed=false;
        const auto output=directory.filePath("live-rate-change");
        const auto result=engine.run(request(fixtures[0].torrent),output,id(),stopped,[&](auto progress){
            if(!limited.isValid()&&progress.received>0){limited.start();baseline=progress.received;}
            if(!changed&&limited.isValid()&&limited.elapsed()>=1500){
                check(progress.received-baseline<512*1024,"BT loopback peer bypassed download quota");
                engine.setRateLimits({0,0});changed=true;
            }
            if(overall.elapsed()>15000)stopped=true;
        },options());
        check(changed&&result.status==TransferStatus::Completed&&read(QDir(output).filePath(fixtures[0].first))==first,
            "active BT rate change releases limited real transfer with exact bytes");
        check(engine.rateLimits().upload==0&&engine.rateLimits().download==0,"live BT limits can be disabled");
    }
    for (int i = 0; i < fixtures.size(); ++i) {
        TorrentEngine engine({true, 0, 0});
        const auto output = directory.filePath(QString("received-%1").arg(i));
        std::atomic_bool sawMetadata{false};
        const auto result = run(engine, request(i == 0 ? fixtures[i].torrent : fixtures[i].magnet), output, id(), options(), 0, &sawMetadata);
        check(result.status == TransferStatus::Completed && read(QDir(output).filePath(fixtures[i].first)) == first
            && read(QDir(output).filePath(fixtures[i].second)) == second, "real v1/v2/hybrid peer transfer matches both binary files");
        if (i > 0) check(sawMetadata.load(), "magnet actually enters metadata-fetching phase");
    }
    {
        const auto original = directory.filePath("imported-copy.torrent");
        check(QFile::copy(fixtures[1].torrent, original), "create movable metainfo input");
        const auto metadata = std::get<TorrentMetadata>(inspectTorrent(original));
        QVector<int> priorities;
        for (const auto& file : metadata.files) priorities.append(file.path == fixtures[1].first ? 7 : 0);
        const auto database = directory.filePath("managed.sqlite"), output = directory.filePath(QStringLiteral("恢复下载"));
        const TorrentRuntimeOptions runtime{{true, 0, 0}, options().peers,{1,0}};
        QString taskId;
        {
            TaskManager manager(database, 1, {}, nullptr, runtime);
            taskId = manager.add(request(original), output, 0, priorities).id;
            check(!taskId.isEmpty() && manager.tasks()[0].request.source.startsWith("magnet:"), "metainfo accepted as stable magnet identity");
            { TaskStore independent(database); const auto accepted = independent.load();
              check(accepted.size() == 1 && !accepted[0].torrentMetainfo.isEmpty() && accepted[0].filePriorities == priorities,
                  "metainfo and selection are durable before dispatch"); }
            check(QFile::remove(original), "move original metainfo away after acceptance");
            QObject::connect(&manager, &TaskManager::changed, &app, [&](const auto&) {
                const auto& task = manager.tasks()[0];
                if (task.state == TaskState::Running && task.received >= 256 * 1024) manager.pause(taskId);
            });
            check(waitUntil([&] { return manager.tasks()[0].state == TaskState::Paused; }, 15000), "managed v2 transfer pauses after actual bytes");
        }
        {
            TaskManager manager(database, 1, {}, nullptr, runtime);
            check(manager.tasks()[0].state == TaskState::Paused && manager.tasks()[0].filePriorities == priorities,
                "v2 selection and paused state survive restart without original metainfo");
            check(manager.resume(taskId), "resume cached metainfo task");
            check(waitUntil([&] { return manager.tasks()[0].state == TaskState::Completed; }, 15000)
                && read(QDir(output).filePath(fixtures[1].first)) == first && !QFileInfo::exists(QDir(output).filePath(fixtures[1].second)),
                "v2 resumes through durable queue and only selected file exists");
            manager.setSeedingPolicy({});
            check(!manager.canReseed(taskId)&&!manager.resume(taskId),"completed torrent does not restart with immediate-stop policy");
            manager.setSeedingPolicy({1,0});
            check(manager.canReseed(taskId),"completed torrent can reseed with explicit policy");
            bool reseedObserved=false;
            QObject::connect(&manager,&TaskManager::changed,&app,[&](const QString& changed){
                if(changed==taskId&&manager.tasks()[0].state==TaskState::Running&&manager.tasks()[0].phase==TransferPhase::Seeding)reseedObserved=true;
            });
            const auto originalId=manager.tasks()[0].id;
            check(manager.resume(taskId)&&waitUntil([&]{return manager.tasks()[0].state==TaskState::Completed;},15000)
                &&reseedObserved&&manager.tasks().size()==1&&manager.tasks()[0].id==originalId
                &&read(QDir(output).filePath(fixtures[1].first))==first&&!QFileInfo::exists(QDir(output).filePath(fixtures[1].second)),
                "completed selected-file task rechecks and seeds without duplicate record or changing output selection");
        }
    }
    {
        const auto database = directory.filePath("magnet-choices.sqlite"), output = directory.filePath("magnet-choices");
        TaskManager manager(database, 1, {}, nullptr, {{true, 0, 0}, options().peers});
        const auto uploadedBefore = fixtures[2].seed.status().total_payload_upload;
        bool asked = false, answered = false;
        QObject::connect(&manager, &TaskManager::torrentFilesReady, &app, [&](const QString& taskId, const TorrentMetadata& metadata) {
            asked = true;
            QTimer::singleShot(100, &manager, [&, taskId, metadata] {
                check(fixtures[2].seed.status().total_payload_upload == uploadedBefore, "magnet transfers no payload before human file choice");
                QVector<int> priorities;
                for (const auto& file : metadata.files) priorities.append(file.path == fixtures[2].first ? 4 : 0);
                answered = manager.selectTorrentFiles(taskId, priorities);
            });
        });
        const auto added = manager.add(request(fixtures[2].magnet), output);
        check(added.added && waitUntil([&] { return manager.tasks()[0].state == TaskState::Completed; }, 15000)
            && asked && answered && read(QDir(output).filePath(fixtures[2].first)) == first
            && !QFileInfo::exists(QDir(output).filePath(fixtures[2].second)), "magnet metadata to persisted human choice to actual transfer");
        const auto aliases = std::get<TorrentMetadata>(inspectTorrent(fixtures[2].torrent)).identities;
        for (const auto& alias : aliases) {
            const auto duplicate = manager.add(request("magnet:?xt=urn:" + alias), directory.filePath("duplicate-" + QString::number(sequence++)));
            check(!duplicate.added && duplicate.id == added.id, "hybrid aliases deduplicate v1-only and v2-only links");
        }
        check(manager.remove(added.id) && read(QDir(output).filePath(fixtures[2].first)) == first, "removing BT record preserves downloaded file");
    }
    {
        TaskManager manager(directory.filePath("declined.sqlite"), 1, {}, nullptr, {{true, 0, 0}, options().peers});
        QObject::connect(&manager, &TaskManager::torrentFilesReady, &app, [&](const QString& taskId, const auto&) { manager.pause(taskId); });
        check(manager.add(request(fixtures[0].magnet), directory.filePath("declined")).added
            && waitUntil([&] { return manager.tasks()[0].state == TaskState::Paused; }, 10000)
            && manager.tasks()[0].received == 0, "declining magnetic file choice pauses without deadlock or payload");
    }
    const auto pausedRoot = directory.filePath("paused"), pausedId = id();
    {
        TorrentEngine engine({true, 0, 0});
        const auto result = run(engine, request(fixtures[0].magnet), pausedRoot, pausedId, options(), 256 * 1024);
        check(result.status == TransferStatus::Cancelled && result.received > 0 && result.received < first.size() + second.size()
            && QFileInfo::exists(pausedRoot + '.' + pausedId + ".bt.resume"), "stop real partial transfer and save resume data");
    }
    {
        TorrentEngine engine({true, 0, 0});
        const auto result = run(engine, request(fixtures[0].magnet), pausedRoot, pausedId, options());
        check(result.status == TransferStatus::Completed && read(QDir(pausedRoot).filePath(fixtures[0].first)) == first
            && read(QDir(pausedRoot).filePath(fixtures[0].second)) == second, "new session resumes partial data and produces exact files");
    }
    {
        QFile file(QDir(pausedRoot).filePath(fixtures[0].first));
        check(file.open(QIODevice::ReadWrite) && file.write("corrupt", 7) == 7, "tamper completed piece fixture"); file.close();
        TorrentEngine engine({true, 0, 0});
        stopped = false; QElapsedTimer clock; clock.start();
        const auto result = engine.run(request(fixtures[0].magnet), pausedRoot, pausedId, stopped,
            [&](auto) { if (clock.elapsed() > 1500) stopped = true; });
        check(result.status == TransferStatus::Cancelled && result.received < first.size() + second.size(),
            "recheck refuses old have-pieces claims after file modification without a source");
    }
    {
        TorrentEngine engine({true, 0, 0});
        const auto result = run(engine, request(fixtures[0].magnet), pausedRoot, pausedId, options());
        check(result.status == TransferStatus::Completed && read(QDir(pausedRoot).filePath(fixtures[0].first)) == first,
            "corrupted piece is repaired from controlled seed");
    }
    }
    {
        TorrentEngine engine({true, 0, 0});
        auto selection = options(); selection.filePriorities = {7, 0};
        const auto output = directory.filePath("selected");
        const auto result = run(engine, request(fixtures[0].torrent), output, id(), selection);
        check(result.status == TransferStatus::Completed, "selected file reaches completed status");
        check(read(QDir(output).filePath(fixtures[0].first)) == first, "selected file contains exact payload");
        check(!QFileInfo::exists(QDir(output).filePath(fixtures[0].second)), "skipped file is not created");
    }
    if (!selectionOnly) {
    {
        TorrentEngine engine({true, 0, 0});
        const auto outputA = directory.filePath("concurrent-a"), outputB = directory.filePath("concurrent-b");
        const auto idA = id(), idB = id();
        std::atomic_bool stopA{false}, stopB{false};
        auto job = [&](int index, const QString& output, const QString& taskId, std::atomic_bool& signal) {
            QElapsedTimer timer; timer.start();
            return engine.run(request(fixtures[index].magnet), output, taskId, signal, [&](auto) { if (timer.elapsed() > 20000) signal = true; }, options());
        };
        auto a = std::async(std::launch::async, [&] { return job(0, outputA, idA, stopA); });
        auto b = std::async(std::launch::async, [&] { return job(1, outputB, idB, stopB); });
        check(a.get().status == TransferStatus::Completed && b.get().status == TransferStatus::Completed
            && read(QDir(outputA).filePath(fixtures[0].first)) == first && read(QDir(outputB).filePath(fixtures[1].second)) == second,
            "one shared session dispatches resume alerts to two simultaneous jobs");
    }
    {
        TorrentEngine engine({true, 0, 0});
        const auto existing = directory.filePath("existing"); QDir().mkdir(existing); write(QDir(existing).filePath("keep.txt"), "keep");
        stopped = false;
        check(engine.run(request(fixtures[0].torrent), existing, id(), stopped).error == TransferError::Destination
            && read(QDir(existing).filePath("keep.txt")) == "keep", "existing unrelated directory is never merged");
        const auto broken = directory.filePath("broken.torrent"); write(broken, "invalid bencode");
        check(std::holds_alternative<InvalidRequest>(inspectTorrent(broken)), "invalid metainfo rejected");
        lt::entry metainfo;
        metainfo["info"]["name"] = "root";
        metainfo["info"]["piece length"] = 16384;
        metainfo["info"]["pieces"] = std::string(20, 'h');
        lt::entry::list_type files;
        for (const auto& path : {QString("a"), QString("a-b"), QString("a/c")}) {
            lt::entry file; file["length"] = 1;
            lt::entry::list_type segments;
            for (const auto& segment : path.split('/')) segments.emplace_back(utf8(segment));
            file["path"] = lt::entry(std::move(segments)); files.push_back(std::move(file));
        }
        metainfo["info"]["files"] = lt::entry(std::move(files));
        std::vector<char> encoded; lt::bencode(std::back_inserter(encoded), metainfo);
        const auto collision = directory.filePath("ancestor-collision.torrent");
        write(collision, QByteArray(encoded.data(), static_cast<qsizetype>(encoded.size())));
        check(std::holds_alternative<InvalidRequest>(inspectTorrent(collision)), "file ancestor collision rejected even with intervening sort key");
    }
    }
    QTextStream(stdout) << "BT integration checks: " << failures << " failures\n";
    return failures ? 1 : 0;
} catch (const std::exception& exception) {
    QTextStream(stderr) << "BT test setup exception: " << exception.what() << '\n'; return 20;
}
