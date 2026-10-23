#include "torrent_engine.h"
#include "disk_space.h"
#include "torrent_space.h"
#include "torrent_error.h"
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <libtorrent/add_torrent_params.hpp>
#include <libtorrent/alert_types.hpp>
#include <libtorrent/file_storage.hpp>
#include <libtorrent/ip_filter.hpp>
#include <libtorrent/load_torrent.hpp>
#include <libtorrent/magnet_uri.hpp>
#include <libtorrent/read_resume_data.hpp>
#include <libtorrent/session.hpp>
#include <libtorrent/settings_pack.hpp>
#include <libtorrent/torrent_status.hpp>
#include <libtorrent/write_resume_data.hpp>
#include <chrono>
#include <future>
#include <map>
#include <mutex>
#include <thread>

namespace downloader {
namespace lt = libtorrent;
namespace {
std::string utf8(const QString& value) { return value.toUtf8().toStdString(); }
QStringList identities(const lt::info_hash_t& hashes)
{
    QStringList result;
    if (hashes.has_v1()) result.append("btih:" + QString::fromLatin1(QByteArray(hashes.v1.data(), 20).toHex()));
    if (hashes.has_v2()) result.append("btmh:1220" + QString::fromLatin1(QByteArray(hashes.v2.data(), 32).toHex()));
    return result;
}
bool compatible(const lt::info_hash_t& expected, const lt::info_hash_t& actual)
{
    return (expected.has_v1() || expected.has_v2()) && (!expected.has_v1() || (actual.has_v1() && expected.v1 == actual.v1))
        && (!expected.has_v2() || (actual.has_v2() && expected.v2 == actual.v2));
}
lt::add_torrent_params loadInput(const DownloadRequest& request)
{
    if (request.protocol != Protocol::BitTorrent) throw std::runtime_error("Not a BitTorrent request");
    if (request.source.startsWith("magnet:", Qt::CaseInsensitive)) return lt::parse_magnet_uri(utf8(request.source));
    QFile file(request.source);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 16 * 1024 * 1024) throw std::runtime_error("Cannot read metainfo, or metainfo exceeds 16 MiB");
    const auto bytes = file.readAll();
    return lt::load_torrent_buffer(lt::span<char const>(bytes.constData(), static_cast<int>(bytes.size())));
}
TorrentMetadata describe(const lt::torrent_info& info)
{
    TorrentMetadata result;
    result.name = QString::fromStdString(info.name()); result.identities = identities(info.info_hashes());
    lt::add_torrent_params magnet;
    magnet.info_hashes = info.info_hashes(); magnet.name = info.name();
    result.magnet = QString::fromStdString(lt::make_magnet_uri(magnet));
    const auto& files = info.layout();
    if (files.num_files() <= 0 || files.num_files() > 100000) throw std::runtime_error("Invalid or excessive torrent file count");
    QSet<QString> paths;
    const QRegularExpression reserved(QStringLiteral("\\A(?:con|prn|aux|nul|com[1-9]|lpt[1-9])(?:\\.|\\z)"), QRegularExpression::CaseInsensitiveOption);
    const QRegularExpression forbidden(QStringLiteral("[<>:\"|?*\\x00-\\x1f]"));
    for (const auto index : files.file_range()) {
        if (files.file_flags(index) & lt::file_storage::flag_symlink) throw std::runtime_error("Torrent symlinks are not supported");
        const bool padding = bool(files.file_flags(index) & lt::file_storage::flag_pad_file);
        auto path = QDir::fromNativeSeparators(QString::fromStdString(files.file_path(index)));
        if (!padding) {
            if (QDir::isAbsolutePath(path)) throw std::runtime_error("Absolute torrent path rejected");
            for (const auto& segment : path.split('/')) {
                if (segment.isEmpty() || segment == "." || segment == ".." || segment.endsWith('.') || segment.endsWith(' ')
                    || forbidden.match(segment).hasMatch() || reserved.match(segment).hasMatch()) throw std::runtime_error("Unsafe torrent path rejected");
            }
            const auto key = path.toCaseFolded();
            if (paths.contains(key)) throw std::runtime_error("Torrent paths collide on Windows");
            paths.insert(key);
        }
        result.files.append({path, files.file_size(index), padding});
        if (!padding) result.total += files.file_size(index);
    }
    for (const auto& path : paths) {
        for (auto separator = path.indexOf('/'); separator >= 0; separator = path.indexOf('/', separator + 1))
            if (paths.contains(path.first(separator))) throw std::runtime_error("Torrent file/directory collision");
    }
    return result;
}
bool safeExistingPaths(const QString& directory, const TorrentMetadata& info)
{
    const QFileInfo root(directory);
    if (root.isSymLink() || root.isJunction()) return false;
    for (const auto& file : info.files) {
        if (file.padding) continue;
        QString path = directory;
        for (const auto& segment : file.path.split('/')) {
            path = QDir(path).filePath(segment);
            const QFileInfo node(path);
            if (node.isSymLink() || node.isJunction()) return false;
        }
    }
    return true;
}
bool saveBytes(const QString& path, const QByteArray& bytes)
{
    QSaveFile output(path);
    return output.open(QIODevice::WriteOnly) && output.write(bytes) == bytes.size() && output.commit();
}
QByteArray readBytes(const QString& path)
{
    QFile file(path); if (!file.open(QIODevice::ReadOnly) || file.size() > 64 * 1024 * 1024) return {}; return file.readAll();
}
TransferResult error(TransferError type, const QString& message, qint64 received = 0)
{
    return {TransferStatus::Failed, type, message, 0, received};
}
}
TorrentInspection inspectTorrent(const QString& path)
{
    QFile input(path);
    if (!input.open(QIODevice::ReadOnly) || input.size() > 16 * 1024 * 1024)
        return InvalidRequest{QStringLiteral("无法读取种子，或种子超过16MiB。")};
    return inspectTorrentData(input.readAll());
}
TorrentInspection inspectTorrentData(QByteArrayView bytes)
{
    try {
        if (bytes.size() > 16 * 1024 * 1024) return InvalidRequest{QStringLiteral("种子超过16MiB。")};
        const auto params = lt::load_torrent_buffer(lt::span<char const>(bytes.data(), static_cast<int>(bytes.size())));
        if (!params.ti) return InvalidRequest{QStringLiteral("需要本地种子文件才能预览文件列表。")};
        auto result = describe(*params.ti);
        result.magnet = QString::fromStdString(lt::make_magnet_uri(params));
        return result;
    } catch (const std::exception& exception) { return InvalidRequest{QString::fromUtf8(exception.what())}; }
}
QString validateTorrentSelection(const TorrentMetadata& metadata, const QVector<int>& priorities)
{
    if (priorities.size() != metadata.files.size()) return QStringLiteral("BT 文件优先级数量不匹配。");
    bool wanted = false;
    for (qsizetype i = 0; i < priorities.size(); ++i) {
        if (priorities[i] < 0 || priorities[i] > 7) return QStringLiteral("BT 文件优先级无效。");
        if (!metadata.files[i].padding && priorities[i] > 0) wanted = true;
    }
    return wanted ? QString{} : QStringLiteral("请至少选择一个文件。");
}
class TorrentEngine::Impl {
public:
    struct ResumeReply { QByteArray bytes; QString error; };
    struct Pending { std::promise<ResumeReply> promise; bool pause = false; };
    struct PriorityPending { std::promise<QString> promise; std::vector<lt::download_priority_t> expected; };
    TorrentEngineOptions options;
    std::unique_ptr<lt::session> session;
    std::atomic_bool stopping{false};
    std::mutex mutex;
    std::map<lt::torrent_handle, std::shared_ptr<Pending>> pending;
    std::map<lt::torrent_handle, std::shared_ptr<PriorityPending>> priorityPending;
    std::jthread pump;
    explicit Impl(TorrentEngineOptions settings) : options(settings) {
        lt::settings_pack pack;
        pack.set_int(lt::settings_pack::alert_mask, static_cast<int>(static_cast<std::uint32_t>(lt::alert_category::error | lt::alert_category::storage | lt::alert_category::status)));
        pack.set_str(lt::settings_pack::listen_interfaces, options.localOnly ? "127.0.0.1:0" : "0.0.0.0:0,[::]:0");
        pack.set_str(lt::settings_pack::user_agent, "NativeDownloader/0.1");
        pack.set_bool(lt::settings_pack::enable_dht, !options.localOnly);
        pack.set_bool(lt::settings_pack::enable_lsd, false);
        pack.set_bool(lt::settings_pack::enable_upnp, false);
        pack.set_bool(lt::settings_pack::enable_natpmp, false);
        pack.set_int(lt::settings_pack::connections_limit, 64);
        pack.set_int(lt::settings_pack::upload_rate_limit, qMax(0, options.uploadBytesPerSecond));
        pack.set_int(lt::settings_pack::download_rate_limit, qMax(0, options.downloadBytesPerSecond));
        if (options.localOnly) {
            pack.set_int(lt::settings_pack::unchoke_interval,1);
            pack.set_int(lt::settings_pack::optimistic_unchoke_interval,1);
            pack.set_str(lt::settings_pack::outgoing_interfaces, "127.0.0.1");
            pack.set_str(lt::settings_pack::dht_bootstrap_nodes, "");
            pack.set_bool(lt::settings_pack::enable_outgoing_utp, false);
            pack.set_bool(lt::settings_pack::enable_incoming_utp, false);
            pack.set_bool(lt::settings_pack::allow_multiple_connections_per_ip, true);
            pack.set_int(lt::settings_pack::min_reconnect_time, 1);
        }
        session = std::make_unique<lt::session>(pack);
        // Include local peers in the same quota as Internet peers.
        lt::ip_filter classes;
        const auto globalClass = std::uint32_t{1} << static_cast<std::uint32_t>(lt::session::global_peer_class_id);
        classes.add_rule(lt::make_address("0.0.0.0"), lt::make_address("255.255.255.255"), globalClass);
        classes.add_rule(lt::make_address("::"), lt::make_address("ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff"), globalClass);
        session->set_peer_class_filter(classes);
        if (options.localOnly) {
            lt::ip_filter filter;
            filter.add_rule(lt::make_address("0.0.0.0"), lt::make_address("255.255.255.255"), lt::ip_filter::blocked);
            filter.add_rule(lt::make_address("::"), lt::make_address("ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff"), lt::ip_filter::blocked);
            filter.add_rule(lt::make_address("127.0.0.0"), lt::make_address("127.255.255.255"), 0);
            filter.add_rule(lt::make_address("::1"), lt::make_address("::1"), 0);
            session->set_ip_filter(filter);
        }
        pump = std::jthread([this] {
            while (!stopping.load()) {
                session->wait_for_alert(std::chrono::milliseconds(100));
                std::vector<lt::alert*> alerts; session->pop_alerts(&alerts);
                for (const auto* alert : alerts) {
                    const auto* priorities = lt::alert_cast<lt::file_prio_alert>(alert);
                    const auto* fileError = lt::alert_cast<lt::file_error_alert>(alert);
                    if (priorities || fileError) {
                        const auto handle = priorities ? priorities->handle : fileError->handle;
                        std::shared_ptr<PriorityPending> response;
                        { std::lock_guard guard(mutex); const auto found = priorityPending.find(handle);
                          if (found != priorityPending.end()) response = found->second; }
                        if (response) {
                            bool ready = false; QString message;
                            try {
                                if (fileError) { ready = true; message = QString::fromStdString(fileError->error.message()); }
                                else ready = handle.get_file_priorities() == response->expected;
                            } catch (const std::exception& exception) { ready = true; message = QString::fromUtf8(exception.what()); }
                            bool deliver = false;
                            if (ready) {
                                std::lock_guard guard(mutex); const auto found = priorityPending.find(handle);
                                if (found != priorityPending.end() && found->second == response) { priorityPending.erase(found); deliver = true; }
                            }
                            if (deliver) response->promise.set_value(message);
                        }
                    }
                    const auto* saved = lt::alert_cast<lt::save_resume_data_alert>(alert);
                    const auto* failed = lt::alert_cast<lt::save_resume_data_failed_alert>(alert);
                    const auto* paused = lt::alert_cast<lt::torrent_paused_alert>(alert);
                    if (!saved && !failed && !paused) continue;
                    const auto handle = saved ? saved->handle : failed ? failed->handle : paused->handle;
                    std::shared_ptr<Pending> response;
                    { std::lock_guard guard(mutex); const auto found = pending.find(handle);
                      if (found == pending.end() || found->second->pause != (paused != nullptr)) continue;
                      response = found->second; pending.erase(found); }
                    try {
                        if (paused) response->promise.set_value({{}, {}});
                        else if (saved) {
                            const auto bytes = lt::write_resume_data_buf(saved->params);
                            response->promise.set_value({QByteArray(bytes.data(), static_cast<qsizetype>(bytes.size())), {}});
                        } else response->promise.set_value({{}, QString::fromStdString(failed->error.message())});
                    } catch (const std::exception& exception) { response->promise.set_value({{}, QString::fromUtf8(exception.what())}); }
                }
            }
        });
    }
    ~Impl() { stopping = true; pump.join(); }
    QString prioritize(const lt::torrent_handle& handle, std::vector<lt::download_priority_t> priorities) {
        auto response = std::make_shared<PriorityPending>(); response->expected = std::move(priorities);
        auto future = response->promise.get_future();
        { std::lock_guard guard(mutex); priorityPending[handle] = response; }
        handle.prioritize_files(response->expected);
        if (future.wait_for(std::chrono::seconds(10)) != std::future_status::ready) {
            std::lock_guard guard(mutex); priorityPending.erase(handle); return QStringLiteral("应用 BT 文件选择超时。");
        }
        return future.get();
    }
    QString pause(const lt::torrent_handle& handle) {
        auto response = std::make_shared<Pending>(); response->pause = true;
        auto future = response->promise.get_future();
        { std::lock_guard guard(mutex); pending[handle] = response; }
        handle.pause();
        if (future.wait_for(std::chrono::seconds(10)) != std::future_status::ready) {
            std::lock_guard guard(mutex); pending.erase(handle); return QStringLiteral("等待 BT 文件写入结束超时。");
        }
        return future.get().error;
    }
    QString checkpoint(const lt::torrent_handle& handle, const QString& path) {
        auto response = std::make_shared<Pending>(); auto future = response->promise.get_future();
        { std::lock_guard guard(mutex); pending[handle] = response; }
        handle.save_resume_data(lt::torrent_handle::flush_disk_cache | lt::torrent_handle::save_info_dict);
        if (future.wait_for(std::chrono::seconds(10)) != std::future_status::ready) {
            std::lock_guard guard(mutex); pending.erase(handle); return QStringLiteral("保存 BT 恢复信息超时。");
        }
        const auto result = future.get();
        if (!result.error.isEmpty()) return result.error;
        return saveBytes(path, result.bytes) ? QString{} : QStringLiteral("无法保存 BT 恢复文件。");
    }
};
TorrentEngine::TorrentEngine(TorrentEngineOptions options) : impl_(std::make_unique<Impl>(options)) {}
TorrentEngine::~TorrentEngine() = default;
void TorrentEngine::setRateLimits(TorrentRateLimits limits){
    if(limits.download<0||limits.upload<0)throw std::invalid_argument("Invalid BT rate limits");
    lt::settings_pack pack;
    pack.set_int(lt::settings_pack::download_rate_limit,limits.download);
    pack.set_int(lt::settings_pack::upload_rate_limit,limits.upload);
    impl_->session->apply_settings(std::move(pack));
}
TorrentRateLimits TorrentEngine::rateLimits() const{
    const auto pack=impl_->session->get_settings();
    return {pack.get_int(lt::settings_pack::download_rate_limit),pack.get_int(lt::settings_pack::upload_rate_limit)};
}
quint16 TorrentEngine::listeningPort() const{return impl_->session->listen_port();}
TransferResult TorrentEngine::run(const DownloadRequest& request, const QString& directory,
    const QString& taskId, const std::atomic_bool& stopped, const std::function<void(TransferProgress)>& progress, TorrentRunOptions options)
{
    if(!validSeedingPolicy(options.seeding))return error(TransferError::InvalidRequest,QStringLiteral("BT 做种时间或比例无效。"));
    if (!QRegularExpression(QStringLiteral("\\A[0-9a-f]{32}\\z")).match(taskId).hasMatch() || !QFileInfo(directory).isAbsolute()
        || QFileInfo(directory).fileName().isEmpty() || directory != QDir::cleanPath(directory))
        return error(TransferError::InvalidRequest, QStringLiteral("BT 保存目录或任务标识无效。"));
    if (stopped.load()) return {TransferStatus::Cancelled};
    try {
        lt::add_torrent_params params;
        try { params = loadInput(request); if (params.ti) describe(*params.ti); }
        catch (const std::exception& exception) { return error(TransferError::InvalidRequest, QString::fromUtf8(exception.what())); }
        const auto expected = params.ti ? params.ti->info_hashes() : params.info_hashes;
        if (!options.metainfo.isEmpty()) {
            auto cached = lt::load_torrent_buffer(lt::span<char const>(options.metainfo.constData(), static_cast<int>(options.metainfo.size())));
            if (!cached.ti || !compatible(expected, cached.ti->info_hashes()))
                return error(TransferError::ResourceChanged, QStringLiteral("缓存种子与任务哈希不匹配。"));
            describe(*cached.ti); params = std::move(cached);
        }
        const auto key = identities(expected).join('|');
        if (key.isEmpty()) return error(TransferError::InvalidRequest, QStringLiteral("种子或磁力链没有有效哈希。"));
        const auto sidecar = directory + '.' + taskId + ".bt";
        const auto owner = QJsonDocument::fromJson(readBytes(sidecar + ".json")).object();
        const bool existingDirectory = QFileInfo::exists(directory);
        if (existingDirectory) {
            if (!QFileInfo(directory).isDir() || owner.value("id").toString() != taskId || owner.value("hashes").toString() != key
                || owner.value("directory").toString() != directory)
                return error(TransferError::Destination, QStringLiteral("BT 目标目录已存在，未合并或覆盖。"));
        } else {
            if (!QDir().mkdir(directory)) return error(TransferError::Destination, QStringLiteral("无法创建 BT 任务目录。"));
            const auto bytes = QJsonDocument(QJsonObject{{"id", taskId}, {"hashes", key}, {"directory", directory}}).toJson();
            if (!saveBytes(sidecar + ".json", bytes)) {
                QDir().rmdir(directory);
                return error(TransferError::Disk, QStringLiteral("无法保存 BT 目录归属记录。"));
            }
        }
        const auto resume = readBytes(sidecar + ".resume");
        if (!resume.isEmpty()) {
            lt::error_code ec;
            auto restored = lt::read_resume_data(lt::span<char const>(resume.constData(), static_cast<int>(resume.size())), ec);
            if (!ec && compatible(expected, restored.ti ? restored.ti->info_hashes() : restored.info_hashes)) {
                if (!restored.ti) restored.ti = params.ti;
                params = std::move(restored);
            }
        }
        if (params.ti && !safeExistingPaths(directory, describe(*params.ti)))
            return error(TransferError::Destination, QStringLiteral("BT 目录含有链接或目录联接，已停止读取和写入。"));
        const auto restoredPriorities = params.file_priorities;
        params.file_priorities.clear();
        params.save_path = utf8(directory);
        params.renamed_files.clear();
        params.flags &= ~(lt::torrent_flags::paused | lt::torrent_flags::auto_managed | lt::torrent_flags::seed_mode);
        params.flags |= lt::torrent_flags::upload_mode | lt::torrent_flags::duplicate_is_error | lt::torrent_flags::default_dont_download;
        if (impl_->options.localOnly) {
            params.trackers.clear(); params.tracker_tiers.clear(); params.url_seeds.clear(); params.peers.clear(); params.dht_nodes.clear();
            params.flags |= lt::torrent_flags::disable_dht | lt::torrent_flags::disable_lsd | lt::torrent_flags::disable_pex;
        }
        lt::error_code ec;
        const auto handle = impl_->session->add_torrent(std::move(params), ec);
        if (ec) return error(classifyTorrentError(ec), QString::fromStdString(ec.message()));
        struct Remove { lt::session& session; lt::torrent_handle handle; ~Remove() {
            try { handle.pause(); session.remove_torrent(handle); } catch (...) {}
        } } remove{*impl_->session, handle};
        std::vector<lt::tcp::endpoint> peers;
        for (const auto& peer : options.peers) {
            const auto address = lt::make_address(utf8(peer.first), ec);
            if (ec || (impl_->options.localOnly && !address.is_loopback())) return error(TransferError::InvalidRequest, QStringLiteral("BT 测试来源地址无效。"));
            peers.emplace_back(address, peer.second);
        }
        bool metadataAccepted = false;
        QElapsedTimer seedingClock;
        QElapsedTimer checkpointClock; checkpointClock.start();
        QElapsedTimer peerClock; peerClock.start();
        bool connected = false;
        qint64 received = 0;
        for (;;) {
            const auto status = handle.status(lt::status_flags_t{});
            received = status.total_wanted_done;
            if (status.errc) return error(classifyTorrentError(status.errc,status.error_file), QString::fromStdString(status.errc.message()), received);
            if (!metadataAccepted && status.has_metadata) {
                const auto info = handle.torrent_file();
                if (!info || !compatible(expected, info->info_hashes())) return error(TransferError::ResourceChanged, QStringLiteral("BT 元数据哈希不匹配。"));
                const auto description = describe(*info);
                if (!safeExistingPaths(directory, description)) return error(TransferError::Destination, QStringLiteral("BT 目录含有链接或目录联接，已停止写入。"));
                // Persist metadata before waiting for the human file choice.
                const auto metadataError = impl_->checkpoint(handle, sidecar + ".resume");
                if (!metadataError.isEmpty()) return error(TransferError::Disk, metadataError, received);
                if (options.filePriorities.isEmpty() && options.selectFiles) {
                    options.filePriorities = options.selectFiles(description, stopped);
                    if (stopped.load() || options.filePriorities.isEmpty()) {
                        const auto pauseError = impl_->pause(handle);
                        if (!pauseError.isEmpty()) return error(TransferError::Disk, pauseError, received);
                        const auto saveError = impl_->checkpoint(handle, sidecar + ".resume");
                        if (!saveError.isEmpty()) return error(TransferError::Disk, saveError, received);
                        return {TransferStatus::Cancelled, TransferError::None, {}, 0, received};
                    }
                }
                {
                    if (!options.filePriorities.isEmpty()) {
                        const auto validation = validateTorrentSelection(description, options.filePriorities);
                        if (!validation.isEmpty()) return error(TransferError::InvalidRequest, validation);
                    }
                    std::vector<lt::download_priority_t> priorities;
                    bool wanted = false;
                    for (qsizetype i = 0; i < description.files.size(); ++i) {
                        const int priority = !options.filePriorities.isEmpty() ? options.filePriorities[i]
                            : static_cast<size_t>(i) < restoredPriorities.size() ? static_cast<int>(static_cast<std::uint8_t>(restoredPriorities[static_cast<size_t>(i)])) : 4;
                        if (priority < 0 || priority > 7) return error(TransferError::InvalidRequest, QStringLiteral("BT 文件优先级无效。"));
                        priorities.emplace_back(static_cast<std::uint8_t>(description.files[i].padding ? 0 : priority));
                        if (!description.files[i].padding && priority > 0) wanted = true;
                    }
                    if (!wanted) return error(TransferError::InvalidRequest, QStringLiteral("请至少选择一个 BT 文件。"));
                    QVector<QPair<qint64,qint64>> selectedIntervals;
                    const auto& layout=info->layout();
                    for(const auto index:layout.file_range()){
                        if(priorities[static_cast<std::size_t>(static_cast<int>(index))]==lt::dont_download
                            ||(layout.file_flags(index)&lt::file_storage::flag_pad_file))continue;
                        const auto start=layout.file_offset(index);
                        selectedIntervals.append({start,start+layout.file_size(index)});
                    }
                    const auto required=torrentSpaceUpperBound(std::move(selectedIntervals),info->piece_length(),layout.total_size());
                    const auto shortage=insufficientDiskSpace(directory,required);
                    if(!shortage.isEmpty())return error(TransferError::Disk,shortage,received);
                    const auto priorityError = impl_->prioritize(handle, std::move(priorities));
                    if (!priorityError.isEmpty()) return error(TransferError::Disk, priorityError, received);
                }
                const auto saveError = impl_->checkpoint(handle, sidecar + ".resume");
                if (!saveError.isEmpty()) return error(TransferError::Disk, saveError, received);
                if (existingDirectory) handle.force_recheck();
                handle.unset_flags(lt::torrent_flags::upload_mode);
                metadataAccepted = true;
                connected = false;
                // Never use the pre-recheck/pre-priority status as completion proof.
                continue;
            }
            const bool checking = status.state == lt::torrent_status::checking_files || status.state == lt::torrent_status::checking_resume_data;
            if (!stopped.load() && !checking && (!connected || peerClock.elapsed() >= 1000)) {
                for (const auto& peer : peers) handle.connect_peer(peer);
                connected = true; peerClock.restart();
            }
            const auto phase = !status.has_metadata ? TransferPhase::FetchingMetadata
                : status.state == lt::torrent_status::checking_files || status.state == lt::torrent_status::checking_resume_data
                ? TransferPhase::CheckingFiles : status.is_finished ? TransferPhase::Seeding : TransferPhase::Receiving;
            const bool finished=metadataAccepted&&status.is_finished&&!checking;
            if(finished&&!seedingClock.isValid())seedingClock.start();
            if(!finished)seedingClock.invalidate();
            const bool seedComplete=finished&&seedingLimitReached(options.seeding,seedingClock.elapsed(),status.all_time_upload,status.total_wanted);
            if (progress) progress({received, status.total_wanted, phase,status.all_time_upload,status.upload_payload_rate});
            if (stopped.load() || seedComplete) {
                const auto pauseError = impl_->pause(handle);
                if (!pauseError.isEmpty()) return error(TransferError::Disk, pauseError, received);
                if (status.has_metadata) {
                    const auto saveError = impl_->checkpoint(handle, sidecar + ".resume");
                    if (!saveError.isEmpty()) return error(TransferError::Disk, saveError, received);
                }
                return {stopped.load() ? TransferStatus::Cancelled : TransferStatus::Completed, TransferError::None, {}, 0, received,status.all_time_upload};
            }
            if (metadataAccepted && checkpointClock.elapsed() >= 30000) {
                const auto saveError = impl_->checkpoint(handle, sidecar + ".resume");
                if (!saveError.isEmpty()) return error(TransferError::Disk, saveError, received);
                checkpointClock.restart();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    } catch (const std::exception& exception) { return error(TransferError::Internal, QString::fromUtf8(exception.what())); }
}
}
