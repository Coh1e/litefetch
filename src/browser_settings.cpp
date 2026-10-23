#include "browser_settings.h"
#include "browser_identity.h"
#include "native_protocol.h"
#include <QCheckBox>
#include <QCryptographicHash>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSaveFile>
#include <QVBoxLayout>
#include <stdexcept>
namespace downloader {
std::optional<BrowserPreferences> chooseBrowserPreferences(QWidget* parent,const BrowserPreferences& current)
{
    QDialog dialog(parent); dialog.setObjectName("browserPreferencesDialog");
    dialog.setWindowTitle(QStringLiteral("浏览器接管")); dialog.resize(560,230);
    auto* layout=new QVBoxLayout(&dialog);
    auto* enabled=new QCheckBox(QStringLiteral("允许浏览器将下载交给本机下载器"),&dialog);
    enabled->setObjectName("browserCaptureEnabled"); enabled->setChecked(current.enabled); layout->addWidget(enabled);
    auto* note=new QLabel(QStringLiteral("仅接管浏览器扩展中允许的网站。关闭后不再接收新任务，已确认的任务继续处理。"),&dialog);
    note->setWordWrap(true); layout->addWidget(note);
    auto* label=new QLabel(QStringLiteral("保存目录"),&dialog); layout->addWidget(label);
    auto* row=new QHBoxLayout;
    auto* directory=new QLineEdit(current.directory,&dialog); directory->setObjectName("browserSaveDirectory"); label->setBuddy(directory);
    auto* browse=new QPushButton(QStringLiteral("选择…"),&dialog); browse->setAutoDefault(false);
    row->addWidget(directory); row->addWidget(browse); layout->addLayout(row);
    auto* connection=new QLabel(QStringLiteral("开发版：浏览器连接组件尚未注册。保存设置不会安装扩展或修改浏览器。"),&dialog);
    connection->setWordWrap(true); layout->addWidget(connection);
    auto* validation=new QLabel(&dialog); validation->setObjectName("browserPreferencesValidation"); layout->addWidget(validation);
    layout->addStretch();
    auto* buttons=new QDialogButtonBox(QDialogButtonBox::Save|QDialogButtonBox::Cancel,&dialog);
    buttons->setObjectName("browserPreferencesButtons"); layout->addWidget(buttons);
    buttons->button(QDialogButtonBox::Save)->setText(QStringLiteral("保存"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    bool prepareConnection=false;
    auto* prepare=buttons->addButton(QStringLiteral("准备连接文件"),QDialogButtonBox::ActionRole);
    prepare->setObjectName("browserPrepareConnection"); prepare->setAutoDefault(false);
    const auto refresh=[&] {
        directory->setEnabled(enabled->isChecked()); browse->setEnabled(enabled->isChecked());
        directory->setToolTip(directory->text());
        const QFileInfo target(directory->text());
        const bool valid=!enabled->isChecked() || (target.isAbsolute()
            && (target.isDir() || (!target.exists() && QFileInfo(target.absolutePath()).isDir())));
        validation->setText(valid?QString{}:QStringLiteral("请选择有效的保存目录。"));
        buttons->button(QDialogButtonBox::Save)->setEnabled(valid);
        prepare->setEnabled(valid && enabled->isChecked());
    };
    QObject::connect(enabled,&QCheckBox::toggled,&dialog,refresh);
    QObject::connect(directory,&QLineEdit::textChanged,&dialog,refresh);
    QObject::connect(browse,&QPushButton::clicked,&dialog,[&] {
        const auto folder=QFileDialog::getExistingDirectory(&dialog,QStringLiteral("保存浏览器下载"),directory->text());
        if (!folder.isEmpty()) directory->setText(folder);
    });
    QObject::connect(buttons,&QDialogButtonBox::accepted,&dialog,&QDialog::accept);
    QObject::connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
    QObject::connect(prepare,&QPushButton::clicked,&dialog,[&] { prepareConnection=true; dialog.accept(); });
    refresh();
    if (dialog.exec()!=QDialog::Accepted) return {};
    return BrowserPreferences{enabled->isChecked(),QDir::cleanPath(directory->text()),prepareConnection};
}
BrowserBridgeConfig makeBrowserSetup(const QString& dataDirectory,const BrowserPreferences& preferences)
{
    const auto root=QFileInfo(dataDirectory).canonicalFilePath();
    if (root.isEmpty()) throw std::runtime_error("Task directory is unavailable");
    if (!QFileInfo(preferences.directory).isAbsolute()) {
        if (!preferences.enabled) return {};
        throw std::runtime_error("Browser download directory must be absolute");
    }
    if (!preferences.enabled && !QFileInfo(preferences.directory).isDir()) {
        BrowserBridgeConfig off; off.downloadDirectory=preferences.directory; return off;
    }
    if (!QDir().mkpath(preferences.directory)) throw std::runtime_error("Cannot create browser download directory");
    const auto folder=QFileInfo(preferences.directory).canonicalFilePath();
    if (folder.isEmpty() || !QFileInfo(folder).isDir()) throw std::runtime_error("Browser download directory is unavailable");
    const auto hash=QCryptographicHash::hash(QByteArray::fromBase64(browserDevelopmentPublicKey),QCryptographicHash::Sha256).toHex().left(32);
    QString extension;
    for (const auto digit : hash) extension.append(QChar('a'+(digit<='9'?digit-'0':digit-'a'+10)));
    BrowserBridgeConfig config;
    config.enabled=preferences.enabled; config.downloadDirectory=folder;
    config.ipcName="NativeDownloader-"+QString::fromLatin1(QCryptographicHash::hash(root.toLower().toUtf8(),QCryptographicHash::Sha256).toHex().left(32));
    config.allowedOrigins={"chrome-extension://"+extension+"/"};
    return config;
}
void saveBrowserSetup(const QString& path,const BrowserBridgeConfig& config)
{
    QJsonArray origins; for (const auto& origin : config.allowedOrigins) origins.append(origin);
    const QJsonObject object{{"enabled",config.enabled},{"ipcName",config.ipcName},
        {"downloadDirectory",config.downloadDirectory},{"allowedOrigins",origins}};
    QSaveFile file(path);
    const auto bytes=QJsonDocument(object).toJson();
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes)!=bytes.size() || !file.commit())
        throw std::runtime_error("Browser settings could not be saved");
}
QString prepareBrowserConnection(const QString& applicationDirectory,const BrowserBridgeConfig& config)
{
    const auto root=QFileInfo(applicationDirectory).canonicalFilePath();
    const auto executable=QDir(root).filePath("native-downloader-host.exe");
    if (!QFileInfo(applicationDirectory).isAbsolute() || root.isEmpty() || !QFileInfo(executable).isFile()
        || !QRegularExpression(QStringLiteral("^[A-Za-z0-9_.-]{1,100}$")).match(config.ipcName).hasMatch()
        || config.allowedOrigins.isEmpty() || config.allowedOrigins.size()>10)
        throw std::runtime_error("Browser host or settings unavailable");
    for (const auto& value : config.allowedOrigins)
        if (!QRegularExpression(QStringLiteral("^chrome-extension://[a-p]{32}/$")).match(value).hasMatch())
            throw std::runtime_error("Invalid browser origin");
    QJsonArray origins; for (const auto& value : config.allowedOrigins) origins.append(value);
    const QJsonObject runtime{{"ipcName",config.ipcName},{"allowedOrigins",origins}};
    const auto runtimePath=QDir(root).filePath("native-host.json");
    const auto manifestPath=QDir(root).filePath("org.native_downloader.bridge.json");
    const QJsonObject manifest{{"name","org.native_downloader.bridge"},{"description","Native Downloader browser bridge"},
        {"path",executable},{"type","stdio"},{"allowed_origins",origins}};
    const auto previous=[&](const QString& path)->std::optional<QJsonObject> {
        if (!QFileInfo::exists(path)) return {};
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly) || file.size()>16384) throw std::runtime_error("Existing browser connection is unreadable");
        return native::parseObject(file.readAll());
    };
    if (const auto old=previous(runtimePath); old && *old!=runtime)
        throw std::runtime_error("Host configuration belongs to another setup");
    if (auto old=previous(manifestPath)) {
        old->insert("path",QFileInfo(old->value("path").toString()).canonicalFilePath());
        if (*old!=manifest) throw std::runtime_error("Host manifest belongs to another setup");
    }
    const auto write=[](const QString& path,const QJsonObject& object) {
        QSaveFile file(path); const auto bytes=QJsonDocument(object).toJson();
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes)!=bytes.size() || !file.commit())
            throw std::runtime_error("Browser connection file could not be saved");
    };
    write(runtimePath,runtime); write(manifestPath,manifest);
    return manifestPath;
}
}
