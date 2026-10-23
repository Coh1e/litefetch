#include "ed2k_settings.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRandomGenerator>
#include <QSaveFile>
#include <QSpinBox>
#include <QVBoxLayout>
#include <stdexcept>
namespace downloader {
namespace {
bool valid(const Ed2kPreferences& p){
    auto options=p.options;
    if(!p.enabled){options.useKad=false;options.server=QHostAddress::LocalHost;}
    if((!options.server.isNull()&&options.server.protocol()!=QAbstractSocket::IPv4Protocol)||!validEd2kOptions(options))return false;
    for(const auto c:options.nickname)if(c.unicode()<32||c.unicode()>126)return false;
    return true;
}
int integer(const QJsonObject& o,const char* key){const auto value=o.value(QLatin1String(key));
    if(!value.isDouble()||value.toDouble()!=value.toInt(-1))throw std::runtime_error("Invalid ed2k numeric setting");return value.toInt();}
}
Ed2kPreferences defaultEd2kPreferences(){
    Ed2kPreferences p;p.options.userHash.resize(16);
    for(auto& byte:p.options.userHash)byte=char(QRandomGenerator::global()->generate()&0xff);
    return p;
}
Ed2kPreferences loadEd2kPreferences(const QString& path){
    auto p=defaultEd2kPreferences();
    if(!QFileInfo::exists(path))return p;
    QFile file(path);if(!file.open(QIODevice::ReadOnly)||file.size()>16384)throw std::runtime_error("Cannot read ed2k settings");
    QJsonParseError error;const auto document=QJsonDocument::fromJson(file.readAll(),&error);
    if(error.error!=QJsonParseError::NoError||!document.isObject())throw std::runtime_error("Invalid ed2k settings JSON");
    const auto o=document.object();
    const auto version=integer(o,"version");
    if((version!=1&&version!=2)||!o["enabled"].isBool()||!o["server"].isString()||!o["userHash"].isString()||!o["nickname"].isString())
        throw std::runtime_error("Invalid ed2k settings fields");
    p.enabled=o["enabled"].toBool();const auto server=o["server"].toString();
    if(!server.isEmpty()&&!p.options.server.setAddress(server))throw std::runtime_error("Invalid ed2k server address");
    const auto serverPort=integer(o,"serverPort"),incomingPort=integer(o,"incomingPort");
    if(serverPort<1||serverPort>65535||incomingPort<1||incomingPort>65535)throw std::runtime_error("Invalid ed2k ports");
    p.options.serverPort=quint16(serverPort);p.options.incomingPort=quint16(incomingPort);
    const auto raw=o["userHash"].toString().toLatin1();p.options.userHash=QByteArray::fromHex(raw);
    if(raw.size()!=32||p.options.userHash.toHex()!=raw.toLower())throw std::runtime_error("Invalid ed2k user hash");
    p.options.nickname=o["nickname"].toString();p.options.operationTimeoutMs=integer(o,"timeoutMs");
    if(version==2){
        const auto mode=o["mode"].toString();if(mode!="server"&&mode!="kad")throw std::runtime_error("Invalid ed2k discovery mode");
        p.options.useKad=mode=="kad";p.options.kadDiscoveryTimeoutMs=integer(o,"kadTimeoutMs");
        if(!o["kadSeeds"].isArray()||o["kadSeeds"].toArray().size()>8)throw std::runtime_error("Invalid Kad seed list");
        for(const auto& entry:o["kadSeeds"].toArray()){
            if(!entry.isObject())throw std::runtime_error("Invalid Kad seed");const auto seed=entry.toObject();QHostAddress address;
            if(!seed["address"].isString()||!address.setAddress(seed["address"].toString()))throw std::runtime_error("Invalid Kad seed address");
            const auto port=integer(seed,"port");if(port<1||port>65535)throw std::runtime_error("Invalid Kad seed port");
            p.options.kadSeeds.append({address,quint16(port)});
        }
    }
    if(!valid(p))throw std::runtime_error("Invalid ed2k configuration");return p;
}
void saveEd2kPreferences(const QString& path,const Ed2kPreferences& p){
    if(!valid(p))throw std::invalid_argument("Invalid ed2k settings");
    const auto& o=p.options;QJsonArray seeds;for(const auto& seed:o.kadSeeds)seeds.append(QJsonObject{{"address",seed.first.toString()},{"port",seed.second}});
    const QJsonObject object{{"version",2},{"enabled",p.enabled},{"server",o.server.isNull()?QString{}:o.server.toString()},
        {"serverPort",o.serverPort},{"incomingPort",o.incomingPort},{"userHash",QString::fromLatin1(o.userHash.toHex())},
        {"nickname",o.nickname},{"timeoutMs",o.operationTimeoutMs},{"mode",o.useKad?"kad":"server"},{"kadSeeds",seeds},{"kadTimeoutMs",o.kadDiscoveryTimeoutMs}};
    const auto bytes=QJsonDocument(object).toJson();QSaveFile output(path);output.setDirectWriteFallback(false);
    if(!output.open(QIODevice::WriteOnly)||output.write(bytes)!=bytes.size()||!output.commit())throw std::runtime_error("Cannot save ed2k settings");
}
Ed2kRuntimeOptions ed2kRuntimeOptions(const Ed2kPreferences& p){auto options=p.options;if(!p.enabled){options.server={};options.useKad=false;}return options;}
std::optional<Ed2kPreferences> chooseEd2kPreferences(QWidget* parent,const Ed2kPreferences& current){
    QDialog dialog(parent);dialog.setObjectName("ed2kPreferencesDialog");dialog.setWindowTitle(QStringLiteral("ed2k 设置"));dialog.resize(520,300);
    auto* layout=new QVBoxLayout(&dialog);auto* enabled=new QCheckBox(QStringLiteral("启用实验性 ed2k 下载"),&dialog);
    enabled->setObjectName("ed2kEnabled");enabled->setChecked(current.enabled);layout->addWidget(enabled);
    auto* note=new QLabel(QStringLiteral("当前支持直连源、4 GiB 以下文件和完整分块续传。Kad 找源为实验性功能；回调、加密和入站连接尚未实现，真实网络兼容仍在验证。"),&dialog);
    note->setWordWrap(true);layout->addWidget(note);
    auto* form=new QFormLayout;auto* mode=new QComboBox(&dialog);mode->setObjectName("ed2kSourceMode");mode->addItems({QStringLiteral("服务器找源"),QStringLiteral("Kad 找源（实验性）")});
    mode->setCurrentIndex(current.options.useKad?1:0);form->addRow(QStringLiteral("找源方式"),mode);
    auto* address=new QLineEdit(current.options.server.isNull()?QString{}:current.options.server.toString(),&dialog);
    address->setObjectName("ed2kServerAddress");address->setPlaceholderText(QStringLiteral("服务器 IPv4 地址"));form->addRow(QStringLiteral("服务器地址"),address);
    auto* port=new QSpinBox(&dialog);port->setObjectName("ed2kServerPort");port->setRange(1,65535);port->setValue(current.options.serverPort);form->addRow(QStringLiteral("服务器端口"),port);
    auto* kadAddress=new QLineEdit(current.options.kadSeeds.isEmpty()?QString{}:current.options.kadSeeds.first().first.toString(),&dialog);kadAddress->setObjectName("kadSeedAddress");
    kadAddress->setPlaceholderText(QStringLiteral("明确的引导节点 IPv4 地址"));form->addRow(QStringLiteral("Kad 引导地址"),kadAddress);
    auto* kadPort=new QSpinBox(&dialog);kadPort->setObjectName("kadSeedPort");kadPort->setRange(1,65535);kadPort->setValue(current.options.kadSeeds.isEmpty()?4672:current.options.kadSeeds.first().second);form->addRow(QStringLiteral("Kad UDP 端口"),kadPort);
    auto* nickname=new QLineEdit(current.options.nickname,&dialog);nickname->setObjectName("ed2kNickname");nickname->setMaxLength(128);form->addRow(QStringLiteral("昵称（英文或数字）"),nickname);layout->addLayout(form);
    auto* validation=new QLabel(&dialog);validation->setObjectName("ed2kValidation");layout->addWidget(validation);
    auto* buttons=new QDialogButtonBox(QDialogButtonBox::Save|QDialogButtonBox::Cancel,&dialog);buttons->setObjectName("ed2kButtons");layout->addWidget(buttons);
    buttons->button(QDialogButtonBox::Save)->setText(QStringLiteral("保存"));buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    const auto choice=[&]{auto p=current;p.enabled=enabled->isChecked();p.options.server={};
        if(!address->text().trimmed().isEmpty())p.options.server.setAddress(address->text().trimmed());p.options.serverPort=quint16(port->value());p.options.nickname=nickname->text().trimmed();
        p.options.useKad=mode->currentIndex()==1;
        if(kadAddress->text().trimmed().isEmpty())p.options.kadSeeds.clear();
        else{QHostAddress seed;seed.setAddress(kadAddress->text().trimmed());const QPair<QHostAddress,quint16> endpoint{seed,quint16(kadPort->value())};
            if(p.options.kadSeeds.isEmpty())p.options.kadSeeds.append(endpoint);else p.options.kadSeeds[0]=endpoint;}
        return p;};
    const auto refresh=[&]{address->setEnabled(enabled->isChecked());port->setEnabled(enabled->isChecked());nickname->setEnabled(enabled->isChecked());
        mode->setEnabled(enabled->isChecked());kadAddress->setEnabled(enabled->isChecked());kadPort->setEnabled(enabled->isChecked());
        const bool kad=mode->currentIndex()==1;form->setRowVisible(address,!kad);form->setRowVisible(port,!kad);form->setRowVisible(kadAddress,kad);form->setRowVisible(kadPort,kad);
        const auto p=choice();const bool ok=valid(p);
        buttons->button(QDialogButtonBox::Save)->setEnabled(ok);validation->setText(ok?QString{}:QStringLiteral("请输入有效的 IPv4 地址和英文昵称。"));};
    QObject::connect(enabled,&QCheckBox::toggled,&dialog,refresh);QObject::connect(address,&QLineEdit::textChanged,&dialog,refresh);QObject::connect(nickname,&QLineEdit::textChanged,&dialog,refresh);
    QObject::connect(mode,&QComboBox::currentIndexChanged,&dialog,refresh);QObject::connect(kadAddress,&QLineEdit::textChanged,&dialog,refresh);
    QObject::connect(buttons,&QDialogButtonBox::accepted,&dialog,[&]{if(valid(choice()))dialog.accept();});QObject::connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
    refresh();if(dialog.exec()!=QDialog::Accepted)return {};return choice();
}
}
