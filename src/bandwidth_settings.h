#pragma once
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <cmath>
#include <stdexcept>
namespace downloader {
struct BandwidthPreferences {int directDownload=0,btDownload=0,btUpload=128*1024;bool lowUsage=false;};
inline bool validBandwidthPreferences(const BandwidthPreferences& p){
    const auto valid=[](int rate){return rate>=0&&rate<=1073741824;};
    return valid(p.directDownload)&&valid(p.btDownload)&&valid(p.btUpload);
}
inline BandwidthPreferences effectiveBandwidthPreferences(const BandwidthPreferences& preferences){
    if(!validBandwidthPreferences(preferences))throw std::invalid_argument("Invalid bandwidth settings");
    auto effective=preferences;
    if(preferences.lowUsage){
        const auto capped=[](int rate,int cap){return rate==0?cap:qMin(rate,cap);};
        effective.directDownload=capped(effective.directDownload,128*1024);
        effective.btDownload=capped(effective.btDownload,128*1024);
        effective.btUpload=capped(effective.btUpload,32*1024);
    }
    return effective;
}
inline BandwidthPreferences loadBandwidthPreferences(const QString& path){
    QFile file(path);if(!file.exists())return {};
    if(!file.open(QIODevice::ReadOnly)||file.size()>4096)throw std::runtime_error("Cannot read bandwidth settings");
    QJsonParseError error;const auto document=QJsonDocument::fromJson(file.readAll(),&error);
    if(error.error!=QJsonParseError::NoError||!document.isObject())throw std::runtime_error("Invalid bandwidth settings document");
    const auto object=document.object();
    if(object.value("version")!=QJsonValue(1))throw std::runtime_error("Unsupported bandwidth settings version");
    const auto rate=[&](const char* key){const auto value=object.value(QLatin1String(key));
        const auto number=value.toDouble(-1);
        if(!value.isDouble()||number<0||number>1073741824||std::floor(number)!=number)throw std::runtime_error("Invalid bandwidth setting value");
        return static_cast<int>(number);
    };
    const auto lowUsage=object.value("lowUsage");
    if(!lowUsage.isUndefined()&&!lowUsage.isBool())throw std::runtime_error("Invalid low usage setting");
    return {rate("directDownload"),rate("btDownload"),rate("btUpload"),lowUsage.toBool(false)};
}
inline void saveBandwidthPreferences(const QString& path,const BandwidthPreferences& preferences){
    if(!validBandwidthPreferences(preferences))throw std::invalid_argument("Invalid bandwidth settings");
    const auto bytes=QJsonDocument(QJsonObject{{"version",1},{"directDownload",preferences.directDownload},
        {"btDownload",preferences.btDownload},{"btUpload",preferences.btUpload},{"lowUsage",preferences.lowUsage}}).toJson();
    QSaveFile file(path);file.setDirectWriteFallback(false);
    if(!file.open(QIODevice::WriteOnly)||file.write(bytes)!=bytes.size()||!file.commit())throw std::runtime_error("Cannot save bandwidth settings");
}
}
