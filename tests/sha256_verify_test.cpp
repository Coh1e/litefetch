#include "sha256_verify.h"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QFile>
#include <QDir>
#include <iostream>
using namespace downloader;
static void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
int main(int argc,char** argv){QCoreApplication app(argc,argv);try{
    QTemporaryDir directory;check(directory.isValid(),"No temp directory");const auto path=directory.filePath("file.bin");
    const auto write=[&](const QByteArray& bytes){QFile file(path);check(file.open(QIODevice::WriteOnly|QIODevice::Truncate)&&file.write(bytes)==bytes.size(),"Fixture write failed");};
    const QString expected="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    write("abc");const auto good=verifySha256(path,expected.toUpper());check(good.status==Sha256Status::Matched&&good.actual==expected,"Known SHA-256 vector differs");
    check(verifySha256(path,QString(64,'0')).status==Sha256Status::Mismatch,"Wrong expected value matched");
    check(verifySha256(path,QString(64,'g')).status==Sha256Status::Failed,"Invalid expected accepted");
    check(verifySha256(directory.filePath("missing"),expected).status==Sha256Status::Failed,"Missing file accepted");
    check(verifySha256(path,expected,[]{return true;}).status==Sha256Status::Cancelled,"Initial cancellation ignored");
    write({});check(verifySha256(path,"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855").status==Sha256Status::Matched,"Empty file hash failed");
    write(QByteArray(2*1024*1024,'x'));bool cancelled=false; qint64 received=0;
    const auto stopped=verifySha256(path,expected,[&]{return cancelled;},[&](qint64 read,qint64 total){received=read;check(total==2*1024*1024,"Wrong total");cancelled=true;});
    check(stopped.status==Sha256Status::Cancelled&&received==65536,"Streaming cancellation read entire file");
#ifdef Q_OS_WIN
    bool writeDenied=false;
    verifySha256(path,expected,{},[&](qint64,qint64){if(writeDenied)return;QFile other(path);writeDenied=!other.open(QIODevice::WriteOnly);});
    check(writeDenied,"Concurrent Windows write was not denied");
#endif
    QFile original(path);check(original.open(QIODevice::ReadOnly)&&original.readAll()==QByteArray(2*1024*1024,'x'),"Verification changed file contents");
    const auto root=directory.filePath("bt");check(QDir().mkpath(root),"BT directory failed");const auto child=root+"/data.bin";
    {QFile selected(child);check(selected.open(QIODevice::WriteOnly)&&selected.write("abc",3)==3,"BT file fixture failed");}
    check(verificationFileInDirectory(root,child)==QFileInfo(child).canonicalFilePath(),"Task file rejected");
    check(verifySha256(verificationFileInDirectory(root,child),expected).status==Sha256Status::Matched,"Selected BT file verification failed");
    check(verificationFileInDirectory(root,path).isEmpty(),"Outside task file accepted");
    check(verificationFileInDirectory(root,root).isEmpty(),"Directory accepted as hash target");
    check(verificationFileInDirectory(root,root+"/../bt/data.bin")==QFileInfo(child).canonicalFilePath(),"Normalized inside file rejected");
    check(verificationFileInDirectory(root,root+"/../file.bin").isEmpty(),"Parent traversal accepted");
    std::cout<<"SHA-256 match, mismatch, cancellation and read-only integrity passed\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
