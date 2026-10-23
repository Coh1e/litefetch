#include "task_store.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QUuid>
#include <limits>
#include <sqlite3.h>
#include <stdexcept>

namespace downloader {
namespace {
[[noreturn]] void fail(sqlite3* db) { throw std::runtime_error(sqlite3_errmsg(db)); }
void execute(sqlite3* db, const char* sql)
{
    if (sqlite3_exec(db, sql, nullptr, nullptr, nullptr) != SQLITE_OK) fail(db);
}
struct Statement {
    sqlite3* db;
    sqlite3_stmt* value = nullptr;
    Statement(sqlite3* database, const char* sql) : db(database) {
        if (sqlite3_prepare_v2(db, sql, -1, &value, nullptr) != SQLITE_OK) fail(db);
    }
    ~Statement() { sqlite3_finalize(value); }
    void text(int index, const QString& text) {
        const auto bytes = text.toUtf8();
        if (sqlite3_bind_text(value, index, bytes.constData(), static_cast<int>(bytes.size()), SQLITE_TRANSIENT) != SQLITE_OK) fail(db);
    }
    void integer(int index, qint64 number) {
        if (sqlite3_bind_int64(value, index, number) != SQLITE_OK) fail(db);
    }
    void blob(int index, const QByteArray& bytes) {
        if (sqlite3_bind_blob(value, index, bytes.constData(), static_cast<int>(bytes.size()), SQLITE_TRANSIENT) != SQLITE_OK) fail(db);
    }
    void done() { if (sqlite3_step(value) != SQLITE_DONE) fail(db); }
    QString text(int index) const {
        return QString::fromUtf8(reinterpret_cast<const char*>(sqlite3_column_text(value, index)), sqlite3_column_bytes(value, index));
    }
};
struct Transaction {
    sqlite3* db;
    explicit Transaction(sqlite3* value) : db(value) { execute(db, "BEGIN IMMEDIATE"); }
    ~Transaction() { if (db) sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr); }
    void commit() { execute(db, "COMMIT"); db = nullptr; }
};
void expireHandoffs(sqlite3* db, qint64 now)
{
    if (now < 0) throw std::runtime_error("Invalid handoff clock");
    Statement query(db, "UPDATE browser_handoffs SET state=3 WHERE state=0 AND expires<=?");
    query.integer(1, now); query.done();
}
std::optional<BrowserHandoff> handoff(sqlite3* db, const QString& origin, const QString& id)
{
    Statement query(db, "SELECT origin,request_id,task_id,source,destination,state,created,expires "
        "FROM browser_handoffs WHERE origin=? AND request_id=?");
    query.text(1, origin); query.text(2, id);
    const auto code = sqlite3_step(query.value);
    if (code == SQLITE_DONE) return {};
    if (code != SQLITE_ROW) fail(db);
    return BrowserHandoff{query.text(0),query.text(1),query.text(2),query.text(3),query.text(4),
        static_cast<HandoffState>(sqlite3_column_int(query.value,5)),
        sqlite3_column_int64(query.value,6),sqlite3_column_int64(query.value,7)};
}
void setHandoffState(sqlite3* db, BrowserHandoff& record, HandoffState state)
{
    Statement update(db, "UPDATE browser_handoffs SET state=? WHERE origin=? AND request_id=?");
    update.integer(1, static_cast<int>(state)); update.text(2,record.origin); update.text(3,record.requestId); update.done();
    record.state = state;
}
bool taskConflict(sqlite3* db, const QString& identity, const QString& destination, const QString& id = {})
{
    Statement query(db, "SELECT 1 FROM tasks WHERE identity=? OR destination_key=? OR id=? LIMIT 1");
    query.text(1,identity); query.text(2,destination); query.text(3,id);
    const auto code = sqlite3_step(query.value);
    if (code == SQLITE_ROW) return true;
    if (code != SQLITE_DONE) fail(db);
    return false;
}
}
QString destinationKey(const QString& path)
{
    const QFileInfo file(path);
    // Resolve the existing parent to catch alternate paths through junctions.
    const auto parent = file.dir().canonicalPath();
    auto result = QDir::cleanPath(parent.isEmpty() ? file.absoluteFilePath() : QDir(parent).filePath(file.fileName()));
#ifdef Q_OS_WIN
    result = result.toCaseFolded();
#endif
    return result;
}
QString taskIdentity(const DownloadRequest& request)
{
    const auto input = request.identities.isEmpty() ? request.source : request.identities.first();
    return QString::fromLatin1(QCryptographicHash::hash(input.toUtf8(), QCryptographicHash::Sha256).toHex());
}
TaskStore::TaskStore(const QString& databasePath)
{
    const auto bytes = databasePath.toUtf8();
    const int code = sqlite3_open_v2(bytes.constData(), &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX, nullptr);
    if (code != SQLITE_OK) {
        const std::string message = db_ ? sqlite3_errmsg(db_) : "Cannot allocate database";
        sqlite3_close(db_); db_ = nullptr;
        throw std::runtime_error(message);
    }
    try {
        sqlite3_busy_timeout(db_, 2000);
        execute(db_, "PRAGMA temp_store=MEMORY; PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL;");
        // Serialize schema inspection with migration: a second opener must read
        // the version after the first opener has committed its ALTER statements.
        execute(db_, "BEGIN IMMEDIATE;");
        int version = 0;
        { Statement statement(db_, "PRAGMA user_version");
          if (sqlite3_step(statement.value) != SQLITE_ROW) fail(db_);
          version = sqlite3_column_int(statement.value, 0); }
        if (version > 5) throw std::runtime_error("Database was created by a newer application");
        if (version == 1) execute(db_, "ALTER TABLE tasks RENAME TO tasks_v1;");
        execute(db_, "CREATE TABLE IF NOT EXISTS tasks("
            "id TEXT PRIMARY KEY NOT NULL, source TEXT NOT NULL, destination TEXT NOT NULL,"
            "destination_key TEXT NOT NULL UNIQUE, identity TEXT NOT NULL UNIQUE,"
            "state INTEGER NOT NULL CHECK(state BETWEEN 0 AND 8), priority INTEGER NOT NULL,"
            "created INTEGER NOT NULL, received INTEGER NOT NULL, total INTEGER NOT NULL, error TEXT NOT NULL);");
        if (version == 1) execute(db_, "INSERT INTO tasks SELECT * FROM tasks_v1; DROP TABLE tasks_v1;");
        if (version < 3) execute(db_, "ALTER TABLE tasks ADD COLUMN file_priorities TEXT NOT NULL DEFAULT '[]';"
            "ALTER TABLE tasks ADD COLUMN torrent_metainfo BLOB NOT NULL DEFAULT X'';");
        if(version<5)execute(db_,"ALTER TABLE tasks ADD COLUMN error_type INTEGER NOT NULL DEFAULT 0 CHECK(error_type BETWEEN 0 AND 8);");
        execute(db_, "CREATE TABLE IF NOT EXISTS browser_handoffs("
            "origin TEXT NOT NULL,request_id TEXT NOT NULL,task_id TEXT NOT NULL UNIQUE,"
            "source TEXT NOT NULL,destination TEXT NOT NULL,destination_key TEXT NOT NULL,identity TEXT NOT NULL,"
            "state INTEGER NOT NULL CHECK(state BETWEEN 0 AND 3),created INTEGER NOT NULL,expires INTEGER NOT NULL,"
            "PRIMARY KEY(origin,request_id));"
            "CREATE UNIQUE INDEX IF NOT EXISTS handoff_destination ON browser_handoffs(destination_key) WHERE state=0;"
            "CREATE UNIQUE INDEX IF NOT EXISTS handoff_identity ON browser_handoffs(identity) WHERE state=0;"
            "PRAGMA user_version=5; COMMIT;");
    } catch (...) { sqlite3_close(db_); db_ = nullptr; throw; }
}
TaskStore::~TaskStore() { sqlite3_close(db_); }
QVector<Task> TaskStore::load()
{
    QVector<Task> result;
    Statement statement(db_, "SELECT id,source,destination,identity,state,priority,created,received,total,error,file_priorities,torrent_metainfo,error_type FROM tasks ORDER BY created,id");
    int code;
    while ((code = sqlite3_step(statement.value)) == SQLITE_ROW) {
        auto parsed = parseRequest(statement.text(1));
        const auto* request = std::get_if<DownloadRequest>(&parsed);
        if (!request) throw std::runtime_error("Stored download source is invalid; original database retained");
        Task task{statement.text(0), *request, statement.text(2), statement.text(3),
            static_cast<TaskState>(sqlite3_column_int(statement.value, 4)), sqlite3_column_int(statement.value, 5),
            sqlite3_column_int64(statement.value, 6), sqlite3_column_int64(statement.value, 7),
            sqlite3_column_int64(statement.value, 8), statement.text(9)};
        const auto choices = QJsonDocument::fromJson(statement.text(10).toUtf8());
        if (!choices.isArray() || choices.array().size() > 100000) throw std::runtime_error("Invalid stored torrent selection");
        for (const auto& item : choices.array()) {
            if (!item.isDouble() || item.toDouble() != item.toInt(-1) || item.toInt(-1) < 0 || item.toInt(-1) > 7)
                throw std::runtime_error("Invalid stored torrent priority");
            task.filePriorities.append(item.toInt());
        }
        const int size = sqlite3_column_bytes(statement.value, 11);
        if (size > 16 * 1024 * 1024) throw std::runtime_error("Stored metainfo exceeds limit");
        task.torrentMetainfo = QByteArray(static_cast<const char*>(sqlite3_column_blob(statement.value, 11)), size);
        const auto errorType=sqlite3_column_int64(statement.value,12);
        if(sqlite3_column_type(statement.value,12)!=SQLITE_INTEGER||errorType<0||errorType>8)
            throw std::runtime_error("Invalid stored transfer error classification");
        task.errorType=static_cast<TransferError>(errorType);
        result.append(std::move(task));
    }
    if (code != SQLITE_DONE) fail(db_);
    return result;
}
void TaskStore::save(const Task& task)
{
    std::optional<Transaction> transaction;
    if (sqlite3_get_autocommit(db_)) transaction.emplace(db_);
    {
        Statement reservation(db_, "SELECT 1 FROM browser_handoffs WHERE state=0 AND task_id<>? "
            "AND (identity=? OR destination_key=?) LIMIT 1");
        reservation.text(1,task.id); reservation.text(2,task.identity); reservation.text(3,destinationKey(task.destination));
        const auto code=sqlite3_step(reservation.value);
        if (code==SQLITE_ROW) throw std::runtime_error("Task conflicts with a prepared browser handoff");
        if (code!=SQLITE_DONE) fail(db_);
    }
    Statement statement(db_, "INSERT INTO tasks(id,source,destination,destination_key,identity,state,priority,created,received,total,error,file_priorities,torrent_metainfo,error_type) "
        "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?) ON CONFLICT(id) DO UPDATE SET source=excluded.source,destination=excluded.destination,"
        "destination_key=excluded.destination_key,identity=excluded.identity,state=excluded.state,priority=excluded.priority,"
        "created=excluded.created,received=excluded.received,total=excluded.total,error=excluded.error,"
        "file_priorities=excluded.file_priorities,torrent_metainfo=excluded.torrent_metainfo,error_type=excluded.error_type");
    statement.text(1, task.id); statement.text(2, task.request.source); statement.text(3, task.destination);
    statement.text(4, destinationKey(task.destination)); statement.text(5, task.identity);
    statement.integer(6, static_cast<int>(task.state)); statement.integer(7, task.priority);
    statement.integer(8, task.created); statement.integer(9, task.received); statement.integer(10, task.total);
    QJsonArray choices; for (const auto value : task.filePriorities) choices.append(value);
    statement.text(11, task.error); statement.text(12, QString::fromUtf8(QJsonDocument(choices).toJson(QJsonDocument::Compact)));
    statement.blob(13, task.torrentMetainfo);statement.integer(14,static_cast<int>(task.errorType)); statement.done();
    if (transaction) transaction->commit();
}
void TaskStore::erase(const QString& id)
{
    Statement statement(db_, "DELETE FROM tasks WHERE id=?"); statement.text(1, id); statement.done();
}
BrowserHandoff TaskStore::prepareHandoff(const QString& origin, const QString& requestId,
    const DownloadRequest& request, const QString& destination, qint64 now, qint64 leaseMs)
{
    static const QRegularExpression allowedOrigin(QStringLiteral("^chrome-extension://[a-p]{32}/$"));
    static const QRegularExpression allowedId(QStringLiteral("^[A-Za-z0-9_-]{1,128}$"));
    const auto parsed = parseRequest(request.source);
    const auto* valid = std::get_if<DownloadRequest>(&parsed);
    if (!allowedOrigin.match(origin).hasMatch() || !allowedId.match(requestId).hasMatch()
        || !valid || valid->protocol != Protocol::Http || request.protocol != Protocol::Http || valid->source.size()>8192
        || !QUrl(valid->source).userInfo().isEmpty() || !QFileInfo(destination).isAbsolute()
        || now < 0 || leaseMs < 1000 || leaseMs > 300000 || now > std::numeric_limits<qint64>::max()-leaseMs)
        throw std::runtime_error("Invalid browser handoff");
    const auto target = destinationKey(destination), identity = taskIdentity(*valid);
    Transaction transaction(db_); expireHandoffs(db_,now);
    if (auto existing = handoff(db_,origin,requestId)) {
        if (existing->source != valid->source || destinationKey(existing->destination) != target)
            throw std::runtime_error("Handoff request ID is already bound to another request");
        transaction.commit(); return *existing;
    }
    if (QFileInfo::exists(destination) || !QFileInfo(QFileInfo(destination).absolutePath()).isDir()
        || taskConflict(db_,identity,target)) throw std::runtime_error("Handoff destination or source is unavailable");
    BrowserHandoff record{origin,requestId,QUuid::createUuid().toString(QUuid::Id128),valid->source,
        QDir::cleanPath(destination),HandoffState::Prepared,now,now+leaseMs};
    Statement insert(db_, "INSERT INTO browser_handoffs(origin,request_id,task_id,source,destination,destination_key,identity,state,created,expires) "
        "VALUES(?,?,?,?,?,?,?,0,?,?)");
    insert.text(1,origin); insert.text(2,requestId); insert.text(3,record.taskId); insert.text(4,record.source);
    insert.text(5,record.destination); insert.text(6,target); insert.text(7,identity);
    insert.integer(8,now); insert.integer(9,record.expires); insert.done();
    transaction.commit(); return record;
}
std::optional<BrowserHandoff> TaskStore::queryHandoff(const QString& origin, const QString& requestId, qint64 now)
{
    expireHandoffs(db_,now); return handoff(db_,origin,requestId);
}
BrowserHandoff TaskStore::commitHandoff(const QString& origin, const QString& requestId, qint64 now)
{
    Transaction transaction(db_); expireHandoffs(db_,now);
    auto record = handoff(db_,origin,requestId);
    if (!record) throw std::runtime_error("Unknown handoff");
    if (record->state != HandoffState::Prepared) { transaction.commit(); return *record; }
    const auto parsed = parseRequest(record->source);
    const auto* request = std::get_if<DownloadRequest>(&parsed);
    if (!request || request->protocol != Protocol::Http || QFileInfo::exists(record->destination)
        || taskConflict(db_,taskIdentity(*request),destinationKey(record->destination),record->taskId))
        throw std::runtime_error("Handoff cannot enter the task queue");
    qint64 created = now;
    {
        Statement maximum(db_, "SELECT MAX(created) FROM tasks");
        if (sqlite3_step(maximum.value) != SQLITE_ROW) fail(db_);
        if (sqlite3_column_type(maximum.value,0) != SQLITE_NULL) {
            const auto last = sqlite3_column_int64(maximum.value,0);
            if (last == std::numeric_limits<qint64>::max()) throw std::runtime_error("Task order exhausted");
            created = qMax(created,last+1);
        }
    }
    Task task{record->taskId,*request,record->destination,taskIdentity(*request),TaskState::Queued,0,created};
    save(task);
    setHandoffState(db_,*record,HandoffState::Committed);
    transaction.commit(); return *record;
}
std::optional<BrowserHandoff> TaskStore::abortHandoff(const QString& origin, const QString& requestId, qint64 now)
{
    Transaction transaction(db_); expireHandoffs(db_,now);
    auto record = handoff(db_,origin,requestId);
    if (record && record->state == HandoffState::Prepared) setHandoffState(db_,*record,HandoffState::Aborted);
    transaction.commit(); return record;
}
bool TaskStore::reserved(const DownloadRequest& request, const QString& destination, qint64 now)
{
    expireHandoffs(db_,now);
    Statement query(db_, "SELECT 1 FROM browser_handoffs WHERE state=0 AND (identity=? OR destination_key=?) LIMIT 1");
    query.text(1,taskIdentity(request)); query.text(2,destinationKey(destination));
    const auto code = sqlite3_step(query.value);
    if (code == SQLITE_ROW) return true;
    if (code != SQLITE_DONE) fail(db_);
    return false;
}
}
