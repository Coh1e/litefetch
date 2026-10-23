#include "amule_runtime.h"
#include "amule_api.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QLockFile>
#include <QProcess>
#include <QProcessEnvironment>
#include <QFile>
#include <QSaveFile>
#include <QMap>
#include <QTcpServer>
#include <QUuid>
#include <future>
#include <chrono>

namespace downloader {
namespace {
void require(bool value,const char* message) { if(!value) throw std::runtime_error(message); }
void stopProcess(QProcess& process)
{
    if(process.state()==QProcess::NotRunning)return;
    process.kill();process.waitForFinished(3000);
}
void updateIni(const QString& path,const QMap<QString,QString>& values)
{
    QByteArray original;
    if(QFile::exists(path)){QFile file(path);require(file.open(QIODevice::ReadOnly),"Cannot read aMule configuration");original=file.readAll();}
    // wxWidgets permits values that Qt's INI parser rejects. Preserve every
    // unrelated line, editing only the adapter's explicitly owned keys.
    auto remaining=values;QString group;
    for(const auto& value:values)require(!value.contains('\n')&&!value.contains('\r'),"Invalid aMule configuration value");
    QStringList output;
    for(const auto& line:QString::fromUtf8(original).split('\n')){
        const auto trimmed=line.trimmed();
        if(trimmed.startsWith('[')&&trimmed.endsWith(']'))group=trimmed.mid(1,trimmed.size()-2);
        const auto equals=line.indexOf('=');
        const auto key=group+'/'+(equals<0?QString{}:line.left(equals).trimmed());
        if(equals>=0&&values.contains(key)){
            if(remaining.contains(key)){output.append(line.left(equals).trimmed()+'='+remaining.take(key));}
            continue;
        }
        output.append(line.endsWith('\r')?line.left(line.size()-1):line);
    }
    QString current;
    for(auto it=remaining.cbegin();it!=remaining.cend();++it){
        const auto slash=it.key().indexOf('/');const auto section=it.key().left(slash);
        require(!it.value().contains('\n')&&!it.value().contains('\r'),"Invalid aMule configuration value");
        if(section!=current){output.append('['+section+']');current=section;}
        output.append(it.key().mid(slash+1)+'='+it.value());
    }
    const auto bytes=output.join('\n').toUtf8();QSaveFile file(path);
    require(file.open(QIODevice::WriteOnly)&&file.write(bytes)==bytes.size()&&file.commit(),"Cannot save aMule configuration");
}
}
AmuleRuntime::AmuleRuntime(AmuleRuntimeOptions options)
{
    std::promise<AmuleConnection> ready;
    auto future=ready.get_future();
    supervisor_=std::jthread([this,options=std::move(options),ready=std::move(ready)](std::stop_token stop) mutable {
        QProcess daemon,api;
        std::unique_ptr<QLockFile> ownership;
        bool delivered=false;
        try {
            require(QFileInfo(options.stateDirectory).isAbsolute(),"aMule state directory must be absolute");
            require(QDir().mkpath(options.stateDirectory),"Cannot create aMule state directory");
            const auto state=QFileInfo(options.stateDirectory).canonicalFilePath();
            require(!state.isEmpty(),"Cannot resolve aMule state directory");
            ownership=std::make_unique<QLockFile>(QDir(state).filePath("native-owner.lock"));
            ownership->setStaleLockTime(0);
            require(ownership->tryLock(0),"aMule state directory already has an owner");
            for(const auto& name:{"amuled.exe","amuleapi.exe","amulecmd.exe"})
                require(QFileInfo(QDir(options.binaries).filePath(name)).isFile(),"aMule runtime binary missing");
            auto environment=QProcessEnvironment::systemEnvironment();
            for(const auto& key:{"HOME","USERPROFILE","APPDATA","LOCALAPPDATA","TEMP","TMP"}) {
                const auto directory=QDir(state).filePath(QStringLiteral("environment/")+key);
                require(QDir().mkpath(directory),"Cannot create isolated aMule environment");
                environment.insert(key,directory);
            }
            for(const auto& name:{"incoming","parts"})require(QDir().mkpath(QDir(state).filePath(name)),"Cannot create aMule data directory");
            auto configure=[&](QProcess& process,const QString& name) {
                process.setProgram(QDir(options.binaries).absoluteFilePath(name+".exe"));
                process.setWorkingDirectory(state);process.setProcessEnvironment(environment);
                process.setStandardOutputFile(QDir(state).filePath(name+".stdout.log"),QIODevice::Append);
                process.setStandardErrorFile(QDir(state).filePath(name+".stderr.log"),QIODevice::Append);
            };
            QTcpServer ecReservation,httpReservation;
            require(ecReservation.listen(QHostAddress::LocalHost,0)&&httpReservation.listen(QHostAddress::LocalHost,0),"Cannot reserve local aMule ports");
            const auto ecPort=ecReservation.serverPort(),httpPort=httpReservation.serverPort();
            const auto password=(QUuid::createUuid().toString(QUuid::Id128)+QUuid::createUuid().toString(QUuid::Id128)).toLatin1();
            const auto passwordHash=QCryptographicHash::hash(password,QCryptographicHash::Md5).toHex();
            {
                QMap<QString,QString> config{{"eMule/AppVersion","3.1.0"},
                    {"eMule/IncomingDir",QDir::fromNativeSeparators(QDir(state).filePath("incoming"))},
                    {"eMule/TempDir",QDir::fromNativeSeparators(QDir(state).filePath("parts"))},
                    {"ExternalConnect/AcceptExternalConnections","1"},{"ExternalConnect/ECAddress","127.0.0.1"},
                    {"ExternalConnect/ECPort",QString::number(ecPort)},
                    {"ExternalConnect/ECPassword",QString::fromLatin1(passwordHash)},{"AmuleApi/Enabled","0"}};
                for(const auto& key:{"Autoconnect","ConnectToKad","ConnectToED2K","NewVersionCheck","GeoIPEnabled","GeoIPAutoUpdate","UPnPEnabled"})config.insert(QStringLiteral("eMule/")+key,"0");
                updateIni(QDir(state).filePath("amule.conf"),config);
                updateIni(QDir(state).filePath("amuleapi.conf"),{{"EC/Password",QString::fromLatin1(password)}});
            }
            QProcess initializer;configure(initializer,"amuleapi");
            initializer.setArguments({"--config-dir="+state,"--set-admin-pass="+QString::fromLatin1(password)});
            initializer.start();
            if(!initializer.waitForStarted(3000)||!initializer.waitForFinished(10000)){
                stopProcess(initializer);throw std::runtime_error("Cannot initialize aMule API credentials");
            }
            require(initializer.exitStatus()==QProcess::NormalExit&&initializer.exitCode()==0,"aMule API credential setup failed");
            configure(daemon,"amuled");daemon.setArguments({"--config-dir="+state,"--log-stdout"});
            ecReservation.close();daemon.start();require(daemon.waitForStarted(3000),"Cannot start aMule daemon");
            configure(api,"amuleapi");api.setArguments({"--config-dir="+state,"--host=127.0.0.1","--port="+QString::number(ecPort),"--http-port="+QString::number(httpPort),"--bind=127.0.0.1"});
            httpReservation.close();api.start();require(api.waitForStarted(3000),"Cannot start aMule API");
            bool connected=false;
            const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(15);
            while(!stop.stop_requested()&&std::chrono::steady_clock::now()<deadline) {
                daemon.waitForFinished(100);api.waitForFinished(10);
                require(daemon.state()!=QProcess::NotRunning&&api.state()!=QProcess::NotRunning,"aMule exited during startup");
                try { AmuleApi client(httpPort,password);connected=client.connected(); }catch(const AmuleApiError&){}
                if(connected)break;
            }
            require(connected,"aMule API did not become ready");
            running_=true;ready.set_value({httpPort,password});delivered=true;
            while(!stop.stop_requested()&&daemon.state()!=QProcess::NotRunning&&api.state()!=QProcess::NotRunning){
                daemon.waitForFinished(100);api.waitForFinished(10);
            }
            running_=false;
            if(daemon.state()!=QProcess::NotRunning){
                QProcess shutdown;configure(shutdown,"amulecmd");
                shutdown.setArguments({"--host=127.0.0.1","--port="+QString::number(ecPort),"--password="+QString::fromLatin1(password),"--command=shutdown","--no-log-file"});
                shutdown.start();if(!shutdown.waitForStarted(3000)||!shutdown.waitForFinished(5000))stopProcess(shutdown);
                daemon.waitForFinished(5000);
            }
            stopProcess(api);stopProcess(daemon);
        }catch(...){
            running_=false;stopProcess(api);stopProcess(daemon);
            if(!delivered)ready.set_exception(std::current_exception());
        }
    });
    connection_=future.get();
}
AmuleRuntime::~AmuleRuntime(){supervisor_.request_stop();if(supervisor_.joinable())supervisor_.join();}
}
