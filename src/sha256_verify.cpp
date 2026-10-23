#include "sha256_verify.h"
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QRegularExpression>
#ifdef Q_OS_WIN
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#endif
namespace downloader {
bool validSha256(const QString& expected){return QRegularExpression(QStringLiteral("\\A[0-9a-fA-F]{64}\\z")).match(expected).hasMatch();}
QString verificationFileInDirectory(const QString& directory,const QString& selected){
    const QFileInfo root(directory),file(selected);
    if(!root.isDir()||!file.isFile())return {};
    const auto canonicalRoot=root.canonicalFilePath(),canonicalFile=file.canonicalFilePath();
    if(canonicalRoot.isEmpty()||canonicalFile.isEmpty())return {};
    const auto relative=QDir(canonicalRoot).relativeFilePath(canonicalFile);
    if(QDir::isAbsolutePath(relative)||relative==".."||relative.startsWith("../")||relative.startsWith("..\\"))return {};
    return canonicalFile;
}
Sha256Result verifySha256(const QString& path,const QString& expected,const std::function<bool()>& cancelled,
    const std::function<void(qint64,qint64)>& progress){
    try{
        if(!validSha256(expected))return {Sha256Status::Failed,{},QStringLiteral("请输入64位十六进制SHA-256。")};
        const auto stopped=[&]{return cancelled&&cancelled();};
        if(stopped())return {Sha256Status::Cancelled};
        if(!QFileInfo(path).isFile())return {Sha256Status::Failed,{},QStringLiteral("文件不存在或不是普通文件。")};
        QFile file(path);
#ifdef Q_OS_WIN
        const auto handle=CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_SEQUENTIAL_SCAN,nullptr);
        if(handle==INVALID_HANDLE_VALUE)return {Sha256Status::Failed,{},QStringLiteral("无法只读打开文件，可能正在被其他程序修改（%1）。").arg(GetLastError())};
        const auto descriptor=_open_osfhandle(reinterpret_cast<intptr_t>(handle),_O_RDONLY|_O_BINARY);
        if(descriptor<0){CloseHandle(handle);return {Sha256Status::Failed,{},QStringLiteral("无法创建文件读取句柄。")};}
        if(!file.open(descriptor,QIODevice::ReadOnly,QFileDevice::AutoCloseHandle)){_close(descriptor);return {Sha256Status::Failed,{},file.errorString()};}
#else
        if(!file.open(QIODevice::ReadOnly))return {Sha256Status::Failed,{},file.errorString()};
#endif
        const auto total=file.size();qint64 read=0;QCryptographicHash hash(QCryptographicHash::Sha256);
        while(!file.atEnd()){
            if(stopped())return {Sha256Status::Cancelled};
            const auto bytes=file.read(64*1024);
            if(bytes.isEmpty())return {Sha256Status::Failed,{},file.errorString()};
            hash.addData(bytes);read+=bytes.size();if(progress)progress(read,total);
        }
        if(stopped())return {Sha256Status::Cancelled};
        if(read!=total||file.size()!=total)return {Sha256Status::Failed,{},QStringLiteral("校验期间文件大小发生变化。")};
        const auto actual=QString::fromLatin1(hash.result().toHex());
        return {actual.compare(expected,Qt::CaseInsensitive)==0?Sha256Status::Matched:Sha256Status::Mismatch,actual,{}};
    }catch(...){return {Sha256Status::Failed,{},QStringLiteral("文件校验未能完成。")};}
}
}
