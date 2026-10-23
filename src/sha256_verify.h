#pragma once
#include <QString>
#include <functional>
namespace downloader {
enum class Sha256Status {Matched,Mismatch,Cancelled,Failed};
struct Sha256Result {Sha256Status status=Sha256Status::Failed;QString actual,message;};
bool validSha256(const QString& expected);
// Preflight a chosen BT file against the canonical task directory.
// Returns a canonical regular-file path, or empty on invalid/outside selection.
QString verificationFileInDirectory(const QString& directory,const QString& selected);
// Blocking, read-only worker operation; callbacks run on the worker thread.
Sha256Result verifySha256(const QString& path,const QString& expected,
    const std::function<bool()>& cancelled={},const std::function<void(qint64,qint64)>& progress={});
}
